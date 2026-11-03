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

#include <X11/extensions/render.h>

#include "dix/dix_priv.h"
#include "dix/request_priv.h"
#include "dix/screen_hooks_priv.h"
#include "include/damage.h"
#include "include/dix.h"
#include "include/dixstruct.h"
#include "include/extnsionst.h"
#include "include/gcstruct.h"
#include "include/list.h"
#include "include/picturestr.h"
#include "include/pixmapstr.h"
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

static Bool xscale_install_capture_wraps(ScreenPtr screen);

/* How many CRTCs have a source, on every screen: what lets the GetImage
 * wrap below cost nothing until a compositor sets one. */
static int xscale_source_count;

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
    if (!xscale_protocol_screen(pScreen) || noXScaleExtension)
        return;

    /* A screen that can have sources needs its root captures stitched;
     * without the wraps its CRTCs are not offered as capable at all, since
     * screen recorders would then record them frozen. */
    if (funcs && !xscale_install_capture_wraps(pScreen))
        funcs = NULL;
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
    xscale_source_count--;
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
    xscale_source_count--;
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
    xscale_source_count++;

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

Bool
XScaleGetCrtcScale(RRCrtcPtr crtc, double *sx, double *sy)
{
    if (!crtc || !crtc->xscale_source)
        return FALSE;
    return xscale_crtc_scale(crtc, sx, sy);
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
 *  Root captures: GetImage stitching
 *
 *  A CRTC with a source shows the source, not the part of the root it
 *  covers: the compositor draws only into the source, so that part of the
 *  screen pixmap holds whatever it last left there. Screen recorders read
 *  "the screen" with a root GetImage (XGetImage, XShmGetImage), so without
 *  this they would record a frozen picture of every output with a source.
 *
 *  So a root capture that overlaps such a CRTC reads the rest of the root
 *  as usual and fills the CRTC's logical box with its source, scaled down
 *  to logical size (bilinear) - the same picture the CRTC shows, at the
 *  resolution of the root the recorder asked about. Reads only: nothing on
 *  screen is touched, so no damage and no feedback with XDamage-driven
 *  recorders, and the cost is paid only while something captures. Same
 *  approach as Present's for per-CRTC page flips (present_flip_getimage()
 *  in Xext/present/present_scmd.c), and it chains with it: the rest of the
 *  root is read through whatever GetImage was below this one.
 * ------------------------------------------------------------------ */

static GetImageProcPtr xscale_orig_get_image[MAXSCREENS];

/* Reads sw x sh at (src_sx, src_sy) of `src` with the screen's current
 * (unwrapped) GetImage into the ZPixmap result at column dx, row dy of a
 * `w`-wide result with `dst_stride` bytes per row. */
static void
xscale_read_into_result(ScreenPtr screen, DrawablePtr src,
                        int src_sx, int src_sy, int sw, int sh,
                        int dx, int dy, int w, int depth, int bpp8,
                        int dst_stride, unsigned long planeMask,
                        char *pdstLine)
{
    int tmp_stride;
    char *tmp;

    if (dx == 0 && sw == w) {
        (*screen->GetImage)(src, src_sx, src_sy, sw, sh, ZPixmap, planeMask,
                            pdstLine + (size_t) dy * dst_stride);
        return;
    }

    tmp_stride = PixmapBytePad(sw, depth);
    tmp = malloc((size_t) tmp_stride * sh);
    if (!tmp)
        return;

    (*screen->GetImage)(src, src_sx, src_sy, sw, sh, ZPixmap, planeMask, tmp);
    for (int r = 0; r < sh; r++)
        memcpy(pdstLine + (size_t) (dy + r) * dst_stride + (size_t) dx * bpp8,
               tmp + (size_t) r * tmp_stride, (size_t) sw * bpp8);
    free(tmp);
}

/* The logical box (root coordinates) of a CRTC with a source and a
 * scale-only transform, which is what can be stitched in. */
static Bool
xscale_stitch_box(RRCrtcPtr crtc, BoxPtr box)
{
    double sx, sy;
    int width, height;

    if (!crtc->xscale_source || !xscale_crtc_scale(crtc, &sx, &sy))
        return FALSE;
    RRCrtcGetScanoutSize(crtc, &width, &height);
    box->x1 = crtc->x;
    box->y1 = crtc->y;
    box->x2 = crtc->x + width;
    box->y2 = crtc->y + height;
    return TRUE;
}

/* Renders the part `part` (root coordinates) of the CRTC's logical box
 * `box` from its source, scaled down to logical size (bilinear), into
 * `dst` at (dst_x, dst_y). The one place a source is turned back into what
 * the root would hold, for both GetImage and CopyArea. */
static Bool
xscale_render_source(RRCrtcPtr crtc, const BoxRec *box, const BoxRec *part,
                     DrawablePtr dst, int dst_x, int dst_y)
{
    ScreenPtr screen = dst->pScreen;
    xscale_source_ptr src = crtc->xscale_source;
    DrawablePtr source = src->drawable;
    XID include_inferiors = IncludeInferiors;
    PictFormatPtr src_format, dst_format;
    PictFilterPtr filter;
    PicturePtr srcp, dstp;
    PictTransform t;
    double sx, sy;
    int error;
    Bool done = FALSE;

    if (!xscale_crtc_scale(crtc, &sx, &sy))
        return FALSE;

    if (source->type == DRAWABLE_WINDOW)
        src_format = PictureWindowFormat((WindowPtr) source);
    else if (source->depth == screen->rootDepth)
        src_format = PictureWindowFormat(screen->root);
    else
        src_format = PictureMatchFormat(screen, source->depth, PICT_a8r8g8b8);

    if (dst->type == DRAWABLE_WINDOW)
        dst_format = PictureWindowFormat((WindowPtr) dst);
    else if (dst->depth == screen->rootDepth)
        dst_format = PictureWindowFormat(screen->root);
    else
        dst_format = PictureMatchFormat(screen, dst->depth, PICT_a8r8g8b8);

    if (!src_format || !dst_format)
        return FALSE;

    srcp = CreatePicture(None, source, src_format, CPSubwindowMode,
                         &include_inferiors, serverClient, &error);
    dstp = CreatePicture(None, dst, dst_format, 0L, NULL, serverClient, &error);
    if (srcp && dstp) {
        /* Destination (logical) pixels -> source (physical) pixels, the
         * source starting (src_x, src_y) into its drawable. */
        memset(&t, 0, sizeof(t));
        t.matrix[0][0] = pixman_double_to_fixed(sx);
        t.matrix[0][2] = pixman_int_to_fixed(src->src_x);
        t.matrix[1][1] = pixman_double_to_fixed(sy);
        t.matrix[1][2] = pixman_int_to_fixed(src->src_y);
        t.matrix[2][2] = pixman_fixed_1;
        SetPictureTransform(srcp, &t);

        filter = PictureFindFilter(screen, (char *) FilterBilinear,
                                   strlen(FilterBilinear));
        if (filter)
            SetPicturePictFilter(srcp, filter, NULL, 0);

        CompositePicture(PictOpSrc, srcp, NULL, dstp,
                         part->x1 - box->x1, part->y1 - box->y1, 0, 0,
                         dst_x, dst_y,
                         part->x2 - part->x1, part->y2 - part->y1);
        done = TRUE;
    }
    if (srcp)
        FreePicture(srcp, None);
    if (dstp)
        FreePicture(dstp, None);
    return done;
}

/* Renders `part` of the CRTC's logical box `box` from its source into the
 * GetImage result at the part's offset from (req_x0, req_y0), through a
 * scratch pixmap. */
static void
xscale_stitch_source(ScreenPtr screen, RRCrtcPtr crtc, const BoxRec *box,
                     const BoxRec *part, int req_x0, int req_y0, int w,
                     int depth, int bpp8, int dst_stride,
                     unsigned long planeMask, char *pdstLine)
{
    int pw = part->x2 - part->x1, ph = part->y2 - part->y1;
    PixmapPtr scratch;

    scratch = (*screen->CreatePixmap)(screen, pw, ph, screen->rootDepth,
                                      CREATE_PIXMAP_USAGE_SCRATCH);
    if (!scratch)
        return;

    if (xscale_render_source(crtc, box, part, &scratch->drawable, 0, 0))
        xscale_read_into_result(screen, &scratch->drawable, 0, 0, pw, ph,
                                part->x1 - req_x0, part->y1 - req_y0, w,
                                depth, bpp8, dst_stride, planeMask, pdstLine);
    dixDestroyPixmap(scratch, 0);
}

/* A root ZPixmap capture overlapping CRTCs with a source: reads everything
 * else of the request through the screen's (unwrapped) GetImage, then the
 * CRTCs' parts from their sources. FALSE - nothing done, the caller does a
 * plain read - for any other read. */
static Bool
xscale_get_image_stitched(DrawablePtr pDrawable, int sx, int sy, int w, int h,
                          unsigned int format, unsigned long planeMask,
                          char *pdstLine)
{
    ScreenPtr screen = pDrawable->pScreen;
    rrScrPrivPtr pScrPriv;
    int depth, bpp8, dst_stride, nrects;
    BoxRec req, box;
    RegionRec remaining;
    BoxPtr rects;
    Bool any = FALSE;

    if (xscale_source_count == 0 || format != ZPixmap ||
        pDrawable->type != DRAWABLE_WINDOW ||
        (WindowPtr) pDrawable != screen->root ||
        pDrawable->bitsPerPixel % 8)
        return FALSE;
    pScrPriv = rrGetScrPriv(screen);
    if (!pScrPriv)
        return FALSE;

    depth = pDrawable->depth;
    bpp8 = pDrawable->bitsPerPixel / 8;
    dst_stride = PixmapBytePad(w, depth);
    req.x1 = pDrawable->x + sx;
    req.y1 = pDrawable->y + sy;
    req.x2 = req.x1 + w;
    req.y2 = req.y1 + h;

    /* What is left to read from the root once every stitchable CRTC box is
     * taken out: the same predicate decides both, so every pixel taken out
     * is filled in again below. */
    RegionInit(&remaining, &req, 1);
    for (int i = 0; i < pScrPriv->numCrtcs; i++) {
        RegionRec r;

        if (!xscale_stitch_box(pScrPriv->crtcs[i], &box))
            continue;
        RegionInit(&r, &box, 1);
        if (RegionContainsRect(&remaining, &box) != rgnOUT) {
            RegionSubtract(&remaining, &remaining, &r);
            any = TRUE;
        }
        RegionUninit(&r);
    }
    if (!any) {
        RegionUninit(&remaining);
        return FALSE;
    }

    nrects = RegionNumRects(&remaining);
    rects = RegionRects(&remaining);
    for (int i = 0; i < nrects; i++) {
        BoxPtr b = &rects[i];

        xscale_read_into_result(screen, pDrawable,
                                b->x1 - pDrawable->x, b->y1 - pDrawable->y,
                                b->x2 - b->x1, b->y2 - b->y1,
                                b->x1 - req.x1, b->y1 - req.y1, w,
                                depth, bpp8, dst_stride, planeMask, pdstLine);
    }
    RegionUninit(&remaining);

    for (int i = 0; i < pScrPriv->numCrtcs; i++) {
        RRCrtcPtr crtc = pScrPriv->crtcs[i];
        BoxRec part;

        if (!xscale_stitch_box(crtc, &box))
            continue;
        part.x1 = max(box.x1, req.x1);
        part.y1 = max(box.y1, req.y1);
        part.x2 = min(box.x2, req.x2);
        part.y2 = min(box.y2, req.y2);
        if (part.x1 >= part.x2 || part.y1 >= part.y2)
            continue;
        xscale_stitch_source(screen, crtc, &box, &part, req.x1, req.y1, w,
                             depth, bpp8, dst_stride, planeMask, pdstLine);
    }
    return TRUE;
}

/* ScreenRec.GetImage wrap; see the section comment above. */
static void
xscale_get_image(DrawablePtr pDrawable, int sx, int sy, int w, int h,
                 unsigned int format, unsigned long planeMask, char *pdstLine)
{
    ScreenPtr screen = pDrawable->pScreen;
    int n = screen->myNum;

    screen->GetImage = xscale_orig_get_image[n];
    if (!xscale_get_image_stitched(pDrawable, sx, sy, w, h, format, planeMask,
                                   pdstLine))
        (*screen->GetImage)(pDrawable, sx, sy, w, h, format, planeMask,
                            pdstLine);
    xscale_orig_get_image[n] = screen->GetImage;
    screen->GetImage = xscale_get_image;
}

/* ------------------------------------------------------------------ *
 *  Root captures: CopyArea stitching
 *
 *  A CopyArea from the root with IncludeInferiors is the other core way to
 *  grab "the screen", into a pixmap, and the cheaper one: it stays on the
 *  GPU, with no readback (OBS's XiS capture does this, gated on Present's
 *  XiSRootCopy capability, which promises exactly that such a copy shows
 *  what is on screen). Like GetImage it reads the stale root under a CRTC
 *  with a source, so such a copy is split the same way: the rest from the
 *  root by the real CopyArea, each CRTC's part rendered from its source
 *  scaled down, straight into the destination - still all GPU work. Only
 *  the destination is written, so the root gets no damage.
 *
 *  The hook is a GC wrap, the way Present does it for per-CRTC flips
 *  (Xext/present/present_screen.c): our ops are installed only on GCs whose
 *  subwindow mode is IncludeInferiors (the only ones that can read through
 *  to the root's contents), so ordinary rendering keeps its own ops and
 *  only pays the GCFuncs indirection on state changes.
 * ------------------------------------------------------------------ */

typedef struct {
    const GCFuncs *funcs;
    const GCOps *ops;           /* lower ops while ours are installed, else NULL */
    GCOps wrapped_ops;          /* lower ops with CopyArea replaced */
} xscale_gc_priv_rec, *xscale_gc_priv_ptr;

static DevPrivateKeyRec xscale_gc_private_key;
static CreateGCProcPtr xscale_orig_create_gc[MAXSCREENS];

static inline xscale_gc_priv_ptr
xscale_gc_priv(GCPtr gc)
{
    return dixLookupPrivate(&gc->devPrivates, &xscale_gc_private_key);
}

static void xscale_validate_gc(GCPtr gc, unsigned long changes, DrawablePtr d);
static void xscale_change_gc(GCPtr gc, unsigned long mask);
static void xscale_copy_gc(GCPtr src, unsigned long mask, GCPtr dst);
static void xscale_destroy_gc(GCPtr gc);
static void xscale_change_clip(GCPtr gc, int type, void *pvalue, int nrects);
static void xscale_destroy_clip(GCPtr gc);
static void xscale_copy_clip(GCPtr dst, GCPtr src);

static const GCFuncs xscale_gc_funcs = {
    xscale_validate_gc, xscale_change_gc, xscale_copy_gc, xscale_destroy_gc,
    xscale_change_clip, xscale_destroy_clip, xscale_copy_clip
};

static RegionPtr xscale_gc_copy_area(DrawablePtr pSrc, DrawablePtr pDst,
                                     GCPtr gc, int srcx, int srcy, int w, int h,
                                     int dstx, int dsty);

/* Restores the lower layer's funcs (and ops, if ours are installed). */
static void
xscale_gc_unwrap(xscale_gc_priv_ptr priv, GCPtr gc)
{
    gc->funcs = priv->funcs;
    if (priv->ops)
        gc->ops = priv->ops;
}

/* Re-installs our funcs, and our ops only on an IncludeInferiors GC. The
 * lower layer may have switched its ops table, so the copy is refreshed. */
static void
xscale_gc_wrap(xscale_gc_priv_ptr priv, GCPtr gc)
{
    priv->funcs = gc->funcs;
    gc->funcs = &xscale_gc_funcs;

    if (gc->subWindowMode == IncludeInferiors) {
        priv->ops = gc->ops;
        priv->wrapped_ops = *gc->ops;
        priv->wrapped_ops.CopyArea = xscale_gc_copy_area;
        gc->ops = &priv->wrapped_ops;
    } else
        priv->ops = NULL;
}

static void
xscale_validate_gc(GCPtr gc, unsigned long changes, DrawablePtr d)
{
    xscale_gc_priv_ptr priv = xscale_gc_priv(gc);

    xscale_gc_unwrap(priv, gc);
    (*gc->funcs->ValidateGC) (gc, changes, d);
    xscale_gc_wrap(priv, gc);
}

static void
xscale_change_gc(GCPtr gc, unsigned long mask)
{
    xscale_gc_priv_ptr priv = xscale_gc_priv(gc);

    xscale_gc_unwrap(priv, gc);
    (*gc->funcs->ChangeGC) (gc, mask);
    xscale_gc_wrap(priv, gc);
}

static void
xscale_copy_gc(GCPtr src, unsigned long mask, GCPtr dst)
{
    xscale_gc_priv_ptr priv = xscale_gc_priv(dst);

    xscale_gc_unwrap(priv, dst);
    (*dst->funcs->CopyGC) (src, mask, dst);
    xscale_gc_wrap(priv, dst);
}

static void
xscale_destroy_gc(GCPtr gc)
{
    xscale_gc_priv_ptr priv = xscale_gc_priv(gc);

    /* The GC is freed right after; nothing to re-install. */
    xscale_gc_unwrap(priv, gc);
    (*gc->funcs->DestroyGC) (gc);
}

static void
xscale_change_clip(GCPtr gc, int type, void *pvalue, int nrects)
{
    xscale_gc_priv_ptr priv = xscale_gc_priv(gc);

    xscale_gc_unwrap(priv, gc);
    (*gc->funcs->ChangeClip) (gc, type, pvalue, nrects);
    xscale_gc_wrap(priv, gc);
}

static void
xscale_destroy_clip(GCPtr gc)
{
    xscale_gc_priv_ptr priv = xscale_gc_priv(gc);

    xscale_gc_unwrap(priv, gc);
    (*gc->funcs->DestroyClip) (gc);
    xscale_gc_wrap(priv, gc);
}

static void
xscale_copy_clip(GCPtr dst, GCPtr src)
{
    xscale_gc_priv_ptr priv = xscale_gc_priv(dst);

    xscale_gc_unwrap(priv, dst);
    (*dst->funcs->CopyClip) (dst, src);
    xscale_gc_wrap(priv, dst);
}

/* Merges one CopyArea's GraphicsExpose region into the running result. */
static void
xscale_accumulate_exposed(RegionPtr *exposed, RegionPtr r)
{
    if (!r)
        return;
    if (!*exposed) {
        *exposed = r;
        return;
    }
    RegionUnion(*exposed, *exposed, r);
    RegionDestroy(r);
}

static PixmapPtr
xscale_drawable_pixmap(DrawablePtr d)
{
    if (d->type == DRAWABLE_WINDOW)
        return (*d->pScreen->GetWindowPixmap)((WindowPtr) d);
    return (PixmapPtr) d;
}

/* A root IncludeInferiors copy overlapping CRTCs with a source, split as
 * described above. Called with the GC's ops unwrapped. FALSE - nothing
 * done, the caller does a plain CopyArea - for any other copy, and for
 * ones the split can't reproduce exactly: a clipped GC, a raster op or
 * plane mask other than a plain copy (the source parts are composited,
 * which honours neither), or a destination sharing storage with the root
 * or a source (splitting would then read what an earlier piece wrote). */
static Bool
xscale_copy_area_stitched(DrawablePtr pSrc, DrawablePtr pDst, GCPtr gc,
                          int srcx, int srcy, int w, int h, int dstx, int dsty,
                          RegionPtr *exposed)
{
    ScreenPtr screen = pSrc->pScreen;
    rrScrPrivPtr pScrPriv;
    xscale_source_ptr src;
    PixmapPtr dst_pixmap;
    BoxRec req, box;
    RegionRec remaining;
    BoxPtr rects;
    Bool any = FALSE;
    int nrects;
    unsigned long full = gc->depth >= 32 ? ~0UL : (1UL << gc->depth) - 1;

    if (xscale_source_count == 0 || w <= 0 || h <= 0 ||
        pSrc->type != DRAWABLE_WINDOW || (WindowPtr) pSrc != screen->root ||
        gc->subWindowMode != IncludeInferiors || gc->alu != GXcopy ||
        gc->clientClip || (gc->planemask & full) != full)
        return FALSE;
    pScrPriv = rrGetScrPriv(screen);
    if (!pScrPriv)
        return FALSE;

    dst_pixmap = xscale_drawable_pixmap(pDst);
    if (dst_pixmap == (*screen->GetScreenPixmap)(screen) ||
        dst_pixmap == (*screen->GetWindowPixmap)(screen->root))
        return FALSE;

    req.x1 = pSrc->x + srcx;
    req.y1 = pSrc->y + srcy;
    req.x2 = req.x1 + w;
    req.y2 = req.y1 + h;

    RegionInit(&remaining, &req, 1);
    for (int i = 0; i < pScrPriv->numCrtcs; i++) {
        RRCrtcPtr crtc = pScrPriv->crtcs[i];
        RegionRec r;

        if (!xscale_stitch_box(crtc, &box))
            continue;
        src = crtc->xscale_source;
        if (xscale_drawable_pixmap(src->drawable) == dst_pixmap) {
            RegionUninit(&remaining);
            return FALSE;
        }
        RegionInit(&r, &box, 1);
        if (RegionContainsRect(&remaining, &box) != rgnOUT) {
            RegionSubtract(&remaining, &remaining, &r);
            any = TRUE;
        }
        RegionUninit(&r);
    }
    if (!any) {
        RegionUninit(&remaining);
        return FALSE;
    }

    *exposed = NULL;

    /* The rest from the root. Out-of-bounds source parts stay in it, so
     * their GraphicsExpose regions come from here, exactly as from a single
     * full copy. */
    nrects = RegionNumRects(&remaining);
    rects = RegionRects(&remaining);
    for (int i = 0; i < nrects; i++) {
        BoxPtr b = &rects[i];

        xscale_accumulate_exposed(exposed,
            (*gc->ops->CopyArea)(pSrc, pDst, gc,
                                 b->x1 - pSrc->x, b->y1 - pSrc->y,
                                 b->x2 - b->x1, b->y2 - b->y1,
                                 dstx + (b->x1 - req.x1),
                                 dsty + (b->y1 - req.y1)));
    }
    RegionUninit(&remaining);

    /* Each CRTC's part from its source. */
    for (int i = 0; i < pScrPriv->numCrtcs; i++) {
        RRCrtcPtr crtc = pScrPriv->crtcs[i];
        BoxRec part;

        if (!xscale_stitch_box(crtc, &box))
            continue;
        part.x1 = max(box.x1, req.x1);
        part.y1 = max(box.y1, req.y1);
        part.x2 = min(box.x2, req.x2);
        part.y2 = min(box.y2, req.y2);
        if (part.x1 >= part.x2 || part.y1 >= part.y2)
            continue;
        /* Picture coordinates are drawable-relative, like dstx/dsty. */
        xscale_render_source(crtc, &box, &part, pDst,
                             dstx + (part.x1 - req.x1),
                             dsty + (part.y1 - req.y1));
    }
    return TRUE;
}

static RegionPtr
xscale_gc_copy_area(DrawablePtr pSrc, DrawablePtr pDst, GCPtr gc,
                    int srcx, int srcy, int w, int h, int dstx, int dsty)
{
    xscale_gc_priv_ptr priv = xscale_gc_priv(gc);
    RegionPtr exposed;

    xscale_gc_unwrap(priv, gc);
    if (!xscale_copy_area_stitched(pSrc, pDst, gc, srcx, srcy, w, h,
                                   dstx, dsty, &exposed))
        exposed = (*gc->ops->CopyArea) (pSrc, pDst, gc, srcx, srcy, w, h,
                                        dstx, dsty);
    xscale_gc_wrap(priv, gc);
    return exposed;
}

/* ScreenRec.CreateGC wrap: puts our GCFuncs on every new GC. */
static Bool
xscale_create_gc(GCPtr gc)
{
    ScreenPtr screen = gc->pScreen;
    int n = screen->myNum;
    Bool ret;

    screen->CreateGC = xscale_orig_create_gc[n];
    ret = (*screen->CreateGC) (gc);
    if (ret) {
        xscale_gc_priv_ptr priv = xscale_gc_priv(gc);

        priv->ops = NULL;
        priv->funcs = gc->funcs;
        gc->funcs = &xscale_gc_funcs;
    }
    xscale_orig_create_gc[n] = screen->CreateGC;
    screen->CreateGC = xscale_create_gc;
    return ret;
}

/* ------------------------------------------------------------------ *
 *  Capture wraps: install and remove
 * ------------------------------------------------------------------ */

/* Screen close hook: takes both wraps off again, where still on top. */
static void
xscale_close_screen(CallbackListPtr *pcbl, ScreenPtr screen, void *unused)
{
    int n = screen->myNum;

    dixScreenUnhookClose(screen, xscale_close_screen);
    if (screen->GetImage == xscale_get_image)
        screen->GetImage = xscale_orig_get_image[n];
    if (screen->CreateGC == xscale_create_gc)
        screen->CreateGC = xscale_orig_create_gc[n];
    xscale_orig_get_image[n] = NULL;
    xscale_orig_create_gc[n] = NULL;
}

/* Wraps GetImage and CreateGC on a screen that can have sources. Runs from
 * the DDX's screen init (XScaleSetScanoutFuncs), the last moment the GC
 * private can still be registered - before any GC exists, exactly where
 * Present registers its own. */
static Bool
xscale_install_capture_wraps(ScreenPtr screen)
{
    int n = screen->myNum;

    if (xscale_orig_get_image[n])
        return TRUE;        /* already installed */

    if (!dixPrivateKeyRegistered(&xscale_gc_private_key) &&
        !dixRegisterPrivateKey(&xscale_gc_private_key, PRIVATE_GC,
                               sizeof(xscale_gc_priv_rec)))
        return FALSE;

    xscale_orig_get_image[n] = screen->GetImage;
    screen->GetImage = xscale_get_image;
    xscale_orig_create_gc[n] = screen->CreateGC;
    screen->CreateGC = xscale_create_gc;
    dixScreenHookClose(screen, xscale_close_screen);
    return TRUE;
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
