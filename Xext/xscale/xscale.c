/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE extension.
 *
 * Sharp per-output scaling on X11 without leaving one coordinate space.
 * The output is scaled the ordinary RandR way: a CRTC transform (xrandr
 * --scale-from) gives it a logical footprint in the root smaller than its
 * physical resolution, so the root, window geometry, input, the pointer's
 * travel between outputs and every RandR query stay contiguous and agree
 * with each other with no help from anyone. What RandR alone can't do is
 * make it sharp: such a CRTC scans out a shadow the DDX fills by magnifying
 * the logical root.
 *
 * This extension lets a compositor that rendered the output at physical
 * density hand that picture over instead - a *scanout source*, any drawable
 * of the CRTC's physical size. The server watches it with Damage and the
 * DDX copies what changed into the CRTC's shadow in place of the magnified
 * root (hw/xfree86/modes/xf86XScale.c). The client never sends a request
 * per frame: it keeps rendering into the drawable. A CRTC is "capable"
 * when it has a scale-only transform and its DDX can take a source
 * (XScaleScanoutFuncs, include/xscale.h); anything else - another DDX, a
 * rotation, no compositor - is just the stock blurry-but-correct
 * magnification.
 *
 * See doc/x-scale.md.
 */
#include <dix-config.h>

#include <math.h>

#include <X11/Xmd.h>
#include <X11/Xproto.h>

#include "dix/dix_priv.h"
#include "dix/request_priv.h"
#include "include/damage.h"
#include "include/dix.h"
#include "include/dixstruct.h"
#include "include/extnsionst.h"
#include "include/list.h"
#include "include/randrstr.h"
#include "include/resource.h"
#include "include/scrnintstr.h"
#include "include/windowstr.h"
#include "miext/extinit_priv.h"
#include "Xext/randr/randrstr_priv.h"       /* VERIFY_RR_CRTC, rrGetScrPriv */

#include "xscaleproto.h"
#include "include/xscale.h"

Bool noXScaleExtension = FALSE;

/* Per screen, what its DDX offers (NULL: nothing - no CRTC is capable).
 * Set from the DDX's screen init, which runs before extension init, so it
 * lives in a plain array rather than a screen private. Protocol screens
 * only: GPU screens (PRIME secondaries) are numbered from
 * GPU_SCREEN_OFFSET, past the end of this array, and their CRTCs never
 * report capable. */
static const XScaleScanoutFuncs *xscale_scanout_funcs[MAXSCREENS];

/* What RRCrtcRec.xscale_source points to while a client's source feeds
 * the CRTC. */
typedef struct {
    RRCrtcPtr crtc;
    ClientPtr client;
    DrawablePtr drawable;
    DamagePtr damage;
    int src_x, src_y;
    int width, height;          /* scanout size the source was set for */
    Bool full;                  /* the next redisplay copies everything */
} xscale_source_rec, *xscale_source_ptr;

static int xscale_event_base;

static void xscale_notify_scanout(RRCrtcPtr crtc);

/* ------------------------------------------------------------------ *
 *  DDX registration
 * ------------------------------------------------------------------ */

static Bool
xscale_protocol_screen(ScreenPtr pScreen)
{
    return !pScreen->isGPU && pScreen->myNum >= 0 &&
           pScreen->myNum < MAXSCREENS;
}

static const XScaleScanoutFuncs *
xscale_funcs(ScreenPtr pScreen)
{
    if (!xscale_protocol_screen(pScreen))
        return NULL;
    return xscale_scanout_funcs[pScreen->myNum];
}

void
XScaleSetScanoutFuncs(ScreenPtr pScreen, const XScaleScanoutFuncs *funcs)
{
    if (!xscale_protocol_screen(pScreen))
        return;
    xscale_scanout_funcs[pScreen->myNum] = funcs;
}

Bool
XScaleCrtcHasSource(RRCrtcPtr crtc)
{
    return crtc && crtc->xscale_source != NULL;
}

/* ------------------------------------------------------------------ *
 *  Capability
 * ------------------------------------------------------------------ */

/* The CRTC's physical/logical ratio iff its transform is a plain scale (no
 * rotation, reflection, shear or projection) - the only kind a compositor
 * renders for and the hardware cursor code knows how to follow. */
static Bool
xscale_crtc_scale(RRCrtcPtr crtc, double *sx, double *sy)
{
    const struct pixman_f_transform *t = &crtc->f_transform;

    if (!crtc->mode || crtc->rotation != RR_Rotate_0)
        return FALSE;
    if (fabs(t->m[0][1]) > 1e-6 || fabs(t->m[1][0]) > 1e-6 ||
        fabs(t->m[2][0]) > 1e-6 || fabs(t->m[2][1]) > 1e-6 ||
        fabs(t->m[2][2] - 1.0) > 1e-6 ||
        t->m[0][0] <= 0.0 || t->m[1][1] <= 0.0)
        return FALSE;

    /* f_transform maps CRTC (physical) pixels to framebuffer (logical) ones */
    *sx = 1.0 / t->m[0][0];
    *sy = 1.0 / t->m[1][1];
    return TRUE;
}

/* Can the CRTC take a source now, and of what size? */
static Bool
xscale_crtc_capable(RRCrtcPtr crtc, int *width, int *height)
{
    const XScaleScanoutFuncs *funcs;
    double sx, sy;

    if (noXScaleExtension || !crtc->mode)
        return FALSE;
    funcs = xscale_funcs(crtc->pScreen);
    if (!funcs || !funcs->capable)
        return FALSE;
    if (!xscale_crtc_scale(crtc, &sx, &sy))
        return FALSE;
    return funcs->capable(crtc, width, height);
}

/* ------------------------------------------------------------------ *
 *  Scanout source lifetime
 * ------------------------------------------------------------------ */

/* Tells the DDX, and every client listening, that the CRTC's source
 * changed. */
static void
xscale_source_changed(RRCrtcPtr crtc)
{
    const XScaleScanoutFuncs *funcs = xscale_funcs(crtc->pScreen);

    if (funcs && funcs->source_changed)
        funcs->source_changed(crtc);
    xscale_notify_scanout(crtc);
}

/* Forgets the CRTC's source record, without telling anyone. Clearing
 * crtc->xscale_source first makes the Damage destroy callback that
 * DamageDestroy() runs a no-op. */
static void
xscale_source_free(RRCrtcPtr crtc)
{
    xscale_source_ptr src = crtc->xscale_source;

    if (!src)
        return;
    crtc->xscale_source = NULL;
    DamageDestroy(src->damage);
    free(src);
}

/* Drops the CRTC's source and gives the CRTC back to the DDX. */
static void
xscale_source_drop(RRCrtcPtr crtc)
{
    if (!crtc->xscale_source)
        return;
    xscale_source_free(crtc);
    xscale_source_changed(crtc);
}

/* The source drawable is being destroyed: the Damage layer frees the
 * Damage itself right after this. */
static void
xscale_source_damage_destroy(DamagePtr damage, void *closure)
{
    xscale_source_ptr src = closure;
    RRCrtcPtr crtc = src->crtc;

    if (crtc->xscale_source != src)
        return;             /* already being freed by xscale_source_free() */
    crtc->xscale_source = NULL;
    free(src);
    xscale_source_changed(crtc);
}

/* Makes `drawable` the CRTC's source, replacing one the same client set. */
static int
xscale_source_set(ClientPtr client, RRCrtcPtr crtc, DrawablePtr drawable,
                  int src_x, int src_y, int width, int height)
{
    xscale_source_ptr src;

    /* Replacing: let go of the old drawable without telling the DDX it
     * may refill the scanout from the root in between. */
    xscale_source_free(crtc);

    src = calloc(1, sizeof(*src));
    if (!src)
        return BadAlloc;

    src->damage = DamageCreate(NULL, xscale_source_damage_destroy,
                               DamageReportNone, TRUE, crtc->pScreen, src);
    if (!src->damage) {
        free(src);
        return BadAlloc;
    }
    DamageRegister(drawable, src->damage);

    src->crtc = crtc;
    src->client = client;
    src->drawable = drawable;
    src->src_x = src_x;
    src->src_y = src_y;
    src->width = width;
    src->height = height;
    src->full = TRUE;
    crtc->xscale_source = src;

    xscale_source_changed(crtc);
    return Success;
}

Bool
XScaleTakeScanoutDamage(RRCrtcPtr crtc, DrawablePtr *source,
                        int *src_x, int *src_y, RegionPtr damage)
{
    xscale_source_ptr src = crtc ? crtc->xscale_source : NULL;
    BoxRec scanout;
    RegionRec clip;

    if (!src)
        return FALSE;

    scanout.x1 = 0;
    scanout.y1 = 0;
    scanout.x2 = src->width;
    scanout.y2 = src->height;

    if (src->full) {
        RegionReset(damage, &scanout);
        src->full = FALSE;
    } else {
        /* Damage is kept relative to the drawable's origin; the scanout
         * starts (src_x, src_y) into it. */
        RegionCopy(damage, DamageRegion(src->damage));
        RegionTranslate(damage, -src->src_x, -src->src_y);
        RegionInit(&clip, &scanout, 1);
        RegionIntersect(damage, damage, &clip);
        RegionUninit(&clip);
    }
    DamageEmpty(src->damage);

    *source = src->drawable;
    *src_x = src->src_x;
    *src_y = src->src_y;
    return TRUE;
}

void
XScaleCrtcChanged(RRCrtcPtr crtc)
{
    xscale_source_ptr src = crtc->xscale_source;
    int width, height;

    if (!src)
        return;

    /* Still the same kind of scanout: keep the source, but lay its picture
     * down again (the DDX may have a fresh buffer). */
    if (xscale_crtc_capable(crtc, &width, &height) &&
        width == src->width && height == src->height) {
        src->full = TRUE;
        return;
    }
    xscale_source_drop(crtc);
}

/* ------------------------------------------------------------------ *
 *  XScaleScanoutNotify event delivery
 *
 *  A flat list of (client, window, eventMask) listeners, mirroring
 *  XFixes's cursor-notify listener list (Xext/xfixes/cursor.c) rather than
 *  RandR's per-window event selection machinery: scanout sources are a
 *  screen-wide concept, not scoped to a window's hierarchy. The window only
 *  bounds the selection's lifetime.
 * ------------------------------------------------------------------ */

typedef struct _xscale_listener {
    struct xorg_list entry;
    ClientPtr client;
    WindowPtr window;
    XID clientResource;
    CARD32 eventMask;
} xscale_listener_rec, *xscale_listener_ptr;

static struct xorg_list xscale_listeners;
static RESTYPE xscale_client_type;
static RESTYPE xscale_window_type;

/* Resource destructor of a listener (client gone, selection cancelled,
 * or its window destroyed). */
static int
xscale_free_client(void *data, XID id)
{
    xscale_listener_ptr old = data;
    xscale_listener_ptr e;

    xorg_list_for_each_entry(e, &xscale_listeners, entry) {
        if (e == old) {
            xorg_list_del(&e->entry);
            free(e);
            break;
        }
    }
    return Success;
}

/* Resource destructor hanging off a selected window: drops every listener
 * selected on it. */
static int
xscale_free_window(void *data, XID id)
{
    WindowPtr window = data;
    xscale_listener_ptr e, next;

    xorg_list_for_each_entry_safe(e, next, &xscale_listeners, entry) {
        if (e->window == window)
            FreeResource(e->clientResource, 0);
    }
    return Success;
}

/* Adds, updates or (eventMask 0) removes the client's selection on window. */
static int
xscale_select_input(ClientPtr client, WindowPtr window, CARD32 eventMask)
{
    xscale_listener_ptr e;
    void *val;

    xorg_list_for_each_entry(e, &xscale_listeners, entry) {
        if (e->client == client && e->window == window) {
            if (eventMask)
                e->eventMask = eventMask;
            else
                FreeResource(e->clientResource, 0);
            return Success;
        }
    }
    if (!eventMask)
        return Success;

    e = calloc(1, sizeof(*e));
    if (!e)
        return BadAlloc;

    e->client = client;
    e->window = window;
    e->clientResource = FakeClientID(client->index);
    e->eventMask = eventMask;

    /* A resource on the window so we notice it being destroyed; one is
     * enough however many listeners select on it. */
    if (dixLookupResourceByType(&val, window->drawable.id, xscale_window_type,
                                serverClient, DixGetAttrAccess) != Success) {
        if (!AddResource(window->drawable.id, xscale_window_type, window)) {
            free(e);
            return BadAlloc;
        }
    }

    /* Listed first: on failure AddResource() runs xscale_free_client(),
     * which unlinks and frees it. */
    xorg_list_append(&e->entry, &xscale_listeners);
    if (!AddResource(e->clientResource, xscale_client_type, e))
        return BadAlloc;
    return Success;
}

static CARD8 xscale_scanout_flags(RRCrtcPtr crtc, ClientPtr asking,
                                  int *width, int *height);

/* Sends XScaleScanoutNotify for `crtc` to every listener. */
static void
xscale_notify_scanout(RRCrtcPtr crtc)
{
    xscale_listener_ptr e;
    int width, height;
    CARD8 flags;

    if (noXScaleExtension || xorg_list_is_empty(&xscale_listeners))
        return;

    flags = xscale_scanout_flags(crtc, NULL, &width, &height);

    xorg_list_for_each_entry(e, &xscale_listeners, entry) {
        if (!(e->eventMask & XScaleScanoutNotifyMask))
            continue;

        xXScaleScanoutNotifyEvent ev = {
            .type = xscale_event_base + XScaleScanoutNotify,
            .flags = flags,
            .window = e->window->drawable.id,
            .crtc = crtc->id,
            .scanoutWidth = width,
            .scanoutHeight = height,
            .timestamp = currentTime.milliseconds,
        };

        WriteEventsToClient(e->client, 1, (xEvent *) &ev);
    }
}

/* Byte-swaps an XScaleScanoutNotify for a client of the other byte order. */
static void _X_COLD
SXScaleScanoutNotifyEvent(xXScaleScanoutNotifyEvent *from,
                          xXScaleScanoutNotifyEvent *to)
{
    to->type = from->type;
    to->flags = from->flags;
    cpswaps(from->sequenceNumber, to->sequenceNumber);
    cpswapl(from->window, to->window);
    cpswapl(from->crtc, to->crtc);
    cpswaps(from->scanoutWidth, to->scanoutWidth);
    cpswaps(from->scanoutHeight, to->scanoutHeight);
    cpswapl(from->timestamp, to->timestamp);
}

/* ------------------------------------------------------------------ *
 *  Auto-revert on client disconnect (compositor crash/exit)
 * ------------------------------------------------------------------ */

/* Drops every source the departing client set, so a compositor that
 * crashes leaves the stock magnified picture rather than a frozen one. */
static void
xscale_client_state(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    NewClientInfoRec *info = calldata;
    ClientPtr client = info->client;

    if (client->clientState != ClientStateGone &&
        client->clientState != ClientStateRetained)
        return;

    for (int s = 0; s < screenInfo.numScreens; s++) {
        rrScrPrivPtr pScrPriv = rrGetScrPriv(screenInfo.screens[s]);

        if (!pScrPriv)
            continue;
        for (int i = 0; i < pScrPriv->numCrtcs; i++) {
            RRCrtcPtr crtc = pScrPriv->crtcs[i];
            xscale_source_ptr src = crtc->xscale_source;

            if (src && src->client == client)
                xscale_source_drop(crtc);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Requests
 * ------------------------------------------------------------------ */

/* The XScaleScanout* flags for `crtc`, as seen by `asking` (NULL: by
 * nobody in particular), and the size a source must have: the CRTC's mode
 * size, or what the DDX says when the CRTC is capable. */
static CARD8
xscale_scanout_flags(RRCrtcPtr crtc, ClientPtr asking, int *width, int *height)
{
    xscale_source_ptr src = crtc->xscale_source;
    CARD8 flags = 0;

    *width = crtc->mode ? crtc->mode->mode.width : 0;
    *height = crtc->mode ? crtc->mode->mode.height : 0;

    if (xscale_crtc_capable(crtc, width, height))
        flags |= XScaleScanoutCapable;
    if (src) {
        flags |= XScaleScanoutActive;
        if (asking && src->client == asking)
            flags |= XScaleScanoutMine;
    }
    return flags;
}

/* XScaleQueryVersion: the server's version, or the client's minor version
 * when the client asked for an older major. */
static int
ProcXScaleQueryVersion(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXScaleQueryVersionReq);
    X_REQUEST_FIELD_CARD32(clientMajorVersion);
    X_REQUEST_FIELD_CARD32(clientMinorVersion);

    xXScaleQueryVersionReply reply = {
        .majorVersion = XSCALE_MAJOR_VERSION,
        .minorVersion = (stuff->clientMajorVersion < XSCALE_MAJOR_VERSION)
                            ? stuff->clientMinorVersion : XSCALE_MINOR_VERSION,
    };
    X_REPLY_FIELD_CARD32(majorVersion);
    X_REPLY_FIELD_CARD32(minorVersion);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/* XScaleSelectInput: (un)subscribes the client to XScaleScanoutNotify. */
static int
ProcXScaleSelectInput(ClientPtr client)
{
    WindowPtr window;
    int err;

    X_REQUEST_HEAD_STRUCT(xXScaleSelectInputReq);
    X_REQUEST_FIELD_CARD32(window);
    X_REQUEST_FIELD_CARD32(eventMask);

    err = dixLookupWindow(&window, stuff->window, client, DixGetAttrAccess);
    if (err != Success)
        return err;

    if (stuff->eventMask & ~(CARD32) XScaleScanoutNotifyMask) {
        client->errorValue = stuff->eventMask;
        return BadValue;
    }

    return xscale_select_input(client, window, stuff->eventMask);
}

/* XScaleGetCrtcScanout: the CRTC's flags, physical size and logical box. */
static int
ProcXScaleGetCrtcScanout(ClientPtr client)
{
    RRCrtcPtr crtc;
    int width, height, lw = 0, lh = 0;

    X_REQUEST_HEAD_STRUCT(xXScaleGetCrtcScanoutReq);
    X_REQUEST_FIELD_CARD32(crtc);

    VERIFY_RR_CRTC(stuff->crtc, crtc, DixReadAccess);

    xXScaleGetCrtcScanoutReply reply = {
        .flags = xscale_scanout_flags(crtc, client, &width, &height),
        .scanoutWidth = width,
        .scanoutHeight = height,
    };

    if (crtc->mode) {
        RRCrtcGetScanoutSize(crtc, &lw, &lh);
        reply.x = crtc->x;
        reply.y = crtc->y;
        reply.width = lw;
        reply.height = lh;
    }

    X_REPLY_FIELD_CARD16(scanoutWidth);
    X_REPLY_FIELD_CARD16(scanoutHeight);
    X_REPLY_FIELD_CARD16(x);
    X_REPLY_FIELD_CARD16(y);
    X_REPLY_FIELD_CARD16(width);
    X_REPLY_FIELD_CARD16(height);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/* XScaleSetCrtcScanoutSource: sets, replaces or (drawable None) drops the
 * CRTC's source. See xscaleproto.h for the errors. */
static int
ProcXScaleSetCrtcScanoutSource(ClientPtr client)
{
    RRCrtcPtr crtc;
    DrawablePtr drawable;
    xscale_source_ptr src;
    int width, height, err;

    X_REQUEST_HEAD_STRUCT(xXScaleSetCrtcScanoutSourceReq);
    X_REQUEST_FIELD_CARD32(crtc);
    X_REQUEST_FIELD_CARD32(drawable);
    X_REQUEST_FIELD_CARD16(srcX);
    X_REQUEST_FIELD_CARD16(srcY);

    VERIFY_RR_CRTC(stuff->crtc, crtc, DixSetAttrAccess);
    src = crtc->xscale_source;

    if (src && src->client != client)
        return BadAccess;

    if (stuff->drawable == None) {
        xscale_source_drop(crtc);
        return Success;
    }

    err = dixLookupDrawable(&drawable, stuff->drawable, client, M_ANY,
                            DixReadAccess);
    if (err != Success)
        return err;

    if (drawable->pScreen != crtc->pScreen ||
        (drawable->depth != 24 && drawable->depth != 32) ||
        !xscale_crtc_capable(crtc, &width, &height) ||
        stuff->srcX < 0 || stuff->srcY < 0 ||
        stuff->srcX + width > drawable->width ||
        stuff->srcY + height > drawable->height)
        return BadMatch;

    return xscale_source_set(client, crtc, drawable, stuff->srcX, stuff->srcY,
                             width, height);
}

/* Dispatches by minor opcode. Shared by both byte orders: every request
 * handler swaps its own fields (X_REQUEST_FIELD_*). */
static int
ProcXScaleDispatch(ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data) {
    case X_XScaleQueryVersion:          return ProcXScaleQueryVersion(client);
    case X_XScaleSelectInput:           return ProcXScaleSelectInput(client);
    case X_XScaleGetCrtcScanout:        return ProcXScaleGetCrtcScanout(client);
    case X_XScaleSetCrtcScanoutSource:  return ProcXScaleSetCrtcScanoutSource(client);
    default:                            return BadRequest;
    }
}

/* ------------------------------------------------------------------ *
 *  Init
 * ------------------------------------------------------------------ */

/* Registers the extension. Called once from the extension init table
 * (mi/miinitext.c); `-extension X-SCALE` skips it, and then no CRTC is
 * ever reported capable. */
void
XScaleExtensionInit(void)
{
    ExtensionEntry *ext;

    if (noXScaleExtension)
        return;

    xorg_list_init(&xscale_listeners);
    xscale_client_type = CreateNewResourceType(xscale_free_client,
                                               "XScaleClient");
    xscale_window_type = CreateNewResourceType(xscale_free_window,
                                               "XScaleWindow");
    if (!xscale_client_type || !xscale_window_type) {
        noXScaleExtension = TRUE;
        return;
    }

    if (!AddCallback(&ClientStateCallback, xscale_client_state, NULL)) {
        noXScaleExtension = TRUE;
        return;
    }

    ext = AddExtension(XSCALE_EXTENSION_NAME, XScaleNumberEvents, 0,
                       ProcXScaleDispatch, ProcXScaleDispatch,
                       NULL, StandardMinorOpcode);
    if (!ext) {
        DeleteCallback(&ClientStateCallback, xscale_client_state, NULL);
        noXScaleExtension = TRUE;
        return;
    }

    xscale_event_base = ext->eventBase;
    EventSwapVector[xscale_event_base + XScaleScanoutNotify] =
        (EventSwapPtr) SXScaleScanoutNotifyEvent;
}
