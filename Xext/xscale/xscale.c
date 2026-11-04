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
#include "Xext/xfixes/xfixes.h"             /* VERIFY_REGION_OR_NONE */
#include "Xext/xnotify/xnotify.h"

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

/* One XScalePresentCrtcScanout request: its pixmap, held (one pixmap
 * reference) until the server is done with it, which the client learns
 * from XScaleScanoutIdle. Referenced from every slot of the source that
 * holds it below. */
typedef struct {
    struct xorg_list entry;     /* in xscale_source_rec.copied */
    PixmapPtr pixmap;
    XID pixmap_id;              /* as the client named it */
    CARD32 serial;
    uint64_t token;             /* copy token, while in `copied` */
    int refs;
    Bool completed;             /* XScaleScanoutComplete sent */
} xscale_frame_rec, *xscale_frame_ptr;

/* What RRCrtcRec.xscale_source points to while a client's source feeds
 * the CRTC. */
typedef struct {
    RRCrtcPtr crtc;
    ClientPtr client;
    DrawablePtr drawable;
    DamagePtr damage;           /* NULL for presented pixmaps */
    int src_x, src_y;
    int width, height;          /* scanout size the source was set for */
    Bool full;                  /* the next redisplay copies everything */

    /* A source made of presented pixmaps (XScalePresentCrtcScanout)
     * rather than a drawable watched with Damage. `drawable` is then the
     * latest presented pixmap. */
    Bool presented;
    RegionRec pending;          /* presented updates the DDX hasn't taken */
    xscale_frame_ptr current;   /* the latest presented: the source */
    xscale_frame_ptr queued;    /* waits for a flip or unflip to land */
    xscale_frame_ptr flipping;  /* the DDX's flip() of it is pending */
    xscale_frame_ptr shown;     /* scanned out by a flip that landed */
    xscale_frame_ptr unflipping;/* copied; completes when unflip() lands */
    struct xorg_list copied;    /* copied, waiting for the vblank showing it */
    uint64_t copy_token;        /* bumped for every copied frame */
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

const XScaleScanoutFuncs *
XScaleGetScanoutFuncs(ScreenPtr pScreen)
{
    return xscale_funcs(pScreen);
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

/* The size of what the CRTC scans out, as XScaleGetCrtcScanout reports and
 * XScaleCopyCrtcScanout copies: the mode for an untransformed or plainly
 * scaled CRTC; for a rotated, reflected or otherwise transformed one, which
 * has the root's density, the root area it covers. */
static void
xscale_scanout_size(RRCrtcPtr crtc, int *width, int *height)
{
    double sx, sy;

    *width = *height = 0;
    if (!crtc->mode)
        return;
    if (xscale_crtc_scale(crtc, &sx, &sy)) {
        *width = crtc->mode->mode.width;
        *height = crtc->mode->mode.height;
    } else
        RRCrtcGetScanoutSize(crtc, width, height);
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

static void xscale_presented_release(xscale_source_ptr src);

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
    if (src->damage)
        DamageDestroy(src->damage);
    if (src->presented)
        xscale_presented_release(src);
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
    } else if (src->presented) {
        /* What the presented pixmaps changed, already clipped */
        RegionCopy(damage, &src->pending);
    } else {
        /* Damage is kept relative to the drawable's origin; the scanout
         * starts (src_x, src_y) into it. */
        RegionCopy(damage, DamageRegion(src->damage));
        RegionTranslate(damage, -src->src_x, -src->src_y);
        RegionInit(&clip, &scanout, 1);
        RegionIntersect(damage, damage, &clip);
        RegionUninit(&clip);
    }
    if (src->presented)
        RegionEmpty(&src->pending);
    else
        DamageEmpty(src->damage);

    *source = src->drawable;
    *src_x = src->src_x;
    *src_y = src->src_y;
    return TRUE;
}

/* The Render format to read or write `d` with: its own for a window, the
 * root's for anything of the root's depth, ARGB for a 32-bit pixmap. */
static PictFormatPtr
xscale_drawable_format(ScreenPtr screen, DrawablePtr d)
{
    if (d->type == DRAWABLE_WINDOW)
        return PictureWindowFormat((WindowPtr) d);
    if (d->depth == screen->rootDepth)
        return PictureWindowFormat(screen->root);
    return PictureMatchFormat(screen, d->depth, PICT_a8r8g8b8);
}

Bool
XScaleCopyScanoutSource(RRCrtcPtr crtc, DrawablePtr dst, RegionPtr region)
{
    xscale_source_ptr src = crtc ? crtc->xscale_source : NULL;
    ScreenPtr screen = dst->pScreen;
    XID include_inferiors = IncludeInferiors;
    PictFormatPtr src_format, dst_format;
    PicturePtr srcp, dstp;
    BoxPtr b;
    int n, error;
    Bool done = FALSE;

    if (!src)
        return FALSE;

    src_format = xscale_drawable_format(screen, src->drawable);
    dst_format = xscale_drawable_format(screen, dst);
    if (!src_format || !dst_format)
        return FALSE;

    srcp = CreatePicture(None, src->drawable, src_format, CPSubwindowMode,
                         &include_inferiors, serverClient, &error);
    dstp = CreatePicture(None, dst, dst_format, 0L, NULL, serverClient, &error);
    if (srcp && dstp) {
        n = RegionNumRects(region);
        b = RegionRects(region);
        for (; n--; b++)
            CompositePicture(PictOpSrc, srcp, NULL, dstp,
                             b->x1 + src->src_x, b->y1 + src->src_y, 0, 0,
                             b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1);
        done = TRUE;
    }
    if (srcp)
        FreePicture(srcp, None);
    if (dstp)
        FreePicture(dstp, None);
    return done;
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
 *  Presented pixmaps
 *
 *  A compositor with buffers of its own presents each finished frame
 *  (XScalePresentCrtcScanout). The latest one is the CRTC's source, as if
 *  set with XScaleSetCrtcScanoutSource, so captures, the cursor and the
 *  drop rules don't change. What changes is how it gets on screen: where
 *  the DDX can (XScaleScanoutFuncs.flip) the pixmap is scanned out as it
 *  is, with no copy at all; otherwise its update is copied by the DDX's
 *  usual source path, XScaleTakeScanoutDamage() handing it out.
 *
 *  Each frame gets one XScaleScanoutComplete, when it is shown or
 *  replaced, and one XScaleScanoutIdle, when nothing here holds it any
 *  more: not the source (a copy may read it again), not on screen, not
 *  waiting. Only one flip or unflip is in flight per CRTC; a frame
 *  presented meanwhile waits in `queued`, where a later one replaces it.
 * ------------------------------------------------------------------ */

static Bool
xscale_client_alive(ClientPtr client)
{
    return client && !client->clientGone;
}

static void
xscale_frame_complete(xscale_source_ptr src, xscale_frame_ptr f, CARD8 mode,
                      uint64_t msc, uint64_t ust)
{
    if (f->completed)
        return;
    f->completed = TRUE;
    if (!xscale_client_alive(src->client))
        return;

    xXScaleScanoutCompleteEvent ev = {
        .type = xscale_event_base + XScaleScanoutComplete,
        .mode = mode,
        .crtc = src->crtc->id,
        .serial = f->serial,
        .ustLo = (CARD32) ust,
        .ustHi = (CARD32) (ust >> 32),
        .mscLo = (CARD32) msc,
        .mscHi = (CARD32) (msc >> 32),
    };
    WriteEventsToClient(src->client, 1, (xEvent *) &ev);
}

static xscale_frame_ptr
xscale_frame_ref(xscale_frame_ptr f)
{
    if (f)
        f->refs++;
    return f;
}

/* Drops one slot's hold on `f`; the last one tells the client it may
 * have the pixmap back. A frame never shown completes as skipped. */
static void
xscale_frame_unref(xscale_source_ptr src, xscale_frame_ptr f)
{
    if (!f || --f->refs > 0)
        return;

    xscale_frame_complete(src, f, XScaleCompleteModeSkip, 0, 0);
    if (xscale_client_alive(src->client)) {
        xXScaleScanoutIdleEvent ev = {
            .type = xscale_event_base + XScaleScanoutIdle,
            .crtc = src->crtc->id,
            .serial = f->serial,
            .pixmap = f->pixmap_id,
        };
        WriteEventsToClient(src->client, 1, (xEvent *) &ev);
    }
    dixDestroyPixmap(f->pixmap, 0);
    free(f);
}

/* Lets go of every frame, as the source goes away. The DDX was told (or
 * is about to be, source_changed) and puts its own buffer back. */
static void
xscale_presented_release(xscale_source_ptr src)
{
    xscale_frame_ptr f, next;

    xorg_list_for_each_entry_safe(f, next, &src->copied, entry) {
        xorg_list_del(&f->entry);
        xscale_frame_unref(src, f);
    }
    xscale_frame_unref(src, src->queued);
    xscale_frame_unref(src, src->flipping);
    xscale_frame_unref(src, src->unflipping);
    xscale_frame_unref(src, src->shown);
    xscale_frame_unref(src, src->current);
    src->queued = src->flipping = src->unflipping = NULL;
    src->shown = src->current = NULL;
    RegionUninit(&src->pending);
}

/* The CRTC's presented source, or NULL. */
static xscale_source_ptr
xscale_presented(RRCrtcPtr crtc)
{
    xscale_source_ptr src = crtc ? crtc->xscale_source : NULL;

    return src && src->presented ? src : NULL;
}

/* A frame that goes out as a copy: it completes at the vblank showing
 * the copy (XScaleScanoutCopied), or now when there is nothing to copy. */
static void
xscale_frame_copied(xscale_source_ptr src, xscale_frame_ptr f)
{
    if (!src->full && !RegionNotEmpty(&src->pending)) {
        xscale_frame_complete(src, f, XScaleCompleteModeCopy, 0, 0);
        return;
    }
    f->token = ++src->copy_token;
    xorg_list_append(&f->entry, &src->copied);
    xscale_frame_ref(f);
}

/* Puts `f`, the current frame, on screen: flipped when the DDX can,
 * copied otherwise - after taking the CRTC back from a flipped one. */
static void
xscale_frame_show(RRCrtcPtr crtc, xscale_source_ptr src, xscale_frame_ptr f)
{
    const XScaleScanoutFuncs *funcs = xscale_funcs(crtc->pScreen);

    if (funcs && funcs->flip && funcs->flip(crtc, f->pixmap)) {
        src->flipping = xscale_frame_ref(f);
        return;
    }

    if (src->shown) {
        /* The DDX's own buffer has to come back first; refilled from
         * the source, that is this frame's copy. */
        if (funcs && funcs->unflip && funcs->unflip(crtc)) {
            src->unflipping = xscale_frame_ref(f);
            return;
        }
        /* Stuck on the flipped pixmap for now (VT switched away): */
        xscale_frame_complete(src, f, XScaleCompleteModeSkip, 0, 0);
        return;
    }

    xscale_frame_copied(src, f);
}

/* After a flip or unflip landed: the frame that waited for it. */
static void
xscale_frame_run_queued(RRCrtcPtr crtc, xscale_source_ptr src)
{
    xscale_frame_ptr f = src->queued;

    if (!f || src->flipping || src->unflipping)
        return;
    src->queued = NULL;
    xscale_frame_show(crtc, src, f);
    xscale_frame_unref(src, f);
}

void
XScaleFlipDone(RRCrtcPtr crtc, Bool landed, uint64_t msc, uint64_t ust)
{
    xscale_source_ptr src = xscale_presented(crtc);
    xscale_frame_ptr f;

    if (!src || !src->flipping)
        return;

    f = src->flipping;
    src->flipping = NULL;
    if (landed) {
        xscale_frame_ptr old = src->shown;

        src->shown = f;         /* keeps the flipping slot's hold */
        xscale_frame_complete(src, f, XScaleCompleteModeFlip, msc, ust);
        xscale_frame_unref(src, old);
    } else
        xscale_frame_unref(src, f);

    xscale_frame_run_queued(crtc, src);
}

void
XScaleScanoutLeft(RRCrtcPtr crtc, uint64_t msc, uint64_t ust)
{
    xscale_source_ptr src = xscale_presented(crtc);

    if (!src || (!src->shown && !src->unflipping))
        return;

    xscale_frame_unref(src, src->shown);
    src->shown = NULL;
    if (src->unflipping) {
        xscale_frame_complete(src, src->unflipping, XScaleCompleteModeCopy,
                              msc, ust);
        xscale_frame_unref(src, src->unflipping);
        src->unflipping = NULL;
    }
    xscale_frame_run_queued(crtc, src);
}

uint64_t
XScaleScanoutCopyToken(RRCrtcPtr crtc)
{
    xscale_source_ptr src = xscale_presented(crtc);

    return src ? src->copy_token : 0;
}

void
XScaleScanoutCopied(RRCrtcPtr crtc, uint64_t token, uint64_t msc,
                    uint64_t ust)
{
    xscale_source_ptr src = xscale_presented(crtc);
    xscale_frame_ptr f, next;

    if (!src || !token)
        return;

    xorg_list_for_each_entry_safe(f, next, &src->copied, entry) {
        if (f->token > token)
            break;
        xorg_list_del(&f->entry);
        xscale_frame_complete(src, f, XScaleCompleteModeCopy, msc, ust);
        xscale_frame_unref(src, f);
    }
}

/* Makes the CRTC's source a presented one, replacing a source the same
 * client set the other way. TRUE when it is new. */
static Bool
xscale_presented_source(ClientPtr client, RRCrtcPtr crtc, int width,
                        int height, xscale_source_ptr *out)
{
    xscale_source_ptr src = crtc->xscale_source;

    *out = NULL;
    if (src && src->presented) {
        *out = src;
        return FALSE;
    }

    xscale_source_free(crtc);
    src = calloc(1, sizeof(*src));
    if (!src)
        return FALSE;

    src->crtc = crtc;
    src->client = client;
    src->width = width;
    src->height = height;
    src->presented = TRUE;
    src->full = TRUE;
    RegionNull(&src->pending);
    xorg_list_init(&src->copied);
    crtc->xscale_source = src;
    xscale_source_count++;
    *out = src;
    return TRUE;
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

static void _X_COLD
SXScaleScanoutCompleteEvent(xXScaleScanoutCompleteEvent *from,
                            xXScaleScanoutCompleteEvent *to)
{
    to->type = from->type;
    to->mode = from->mode;
    cpswaps(from->sequenceNumber, to->sequenceNumber);
    cpswapl(from->crtc, to->crtc);
    cpswapl(from->serial, to->serial);
    cpswapl(from->ustLo, to->ustLo);
    cpswapl(from->ustHi, to->ustHi);
    cpswapl(from->mscLo, to->mscLo);
    cpswapl(from->mscHi, to->mscHi);
}

static void _X_COLD
SXScaleScanoutIdleEvent(xXScaleScanoutIdleEvent *from,
                        xXScaleScanoutIdleEvent *to)
{
    to->type = from->type;
    cpswaps(from->sequenceNumber, to->sequenceNumber);
    cpswapl(from->crtc, to->crtc);
    cpswapl(from->serial, to->serial);
    cpswapl(from->pixmap, to->pixmap);
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

    src_format = xscale_drawable_format(screen, source);
    dst_format = xscale_drawable_format(screen, dst);
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

    xscale_scanout_size(crtc, width, height);

    if (xscale_crtc_capable(crtc, width, height)) {
        const XScaleScanoutFuncs *funcs = xscale_funcs(crtc->pScreen);

        flags |= XScaleScanoutCapable;
        if (funcs && funcs->can_flip && funcs->flip && funcs->can_flip(crtc))
            flags |= XScaleScanoutFlippable;
    }
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
        reply.rotation = crtc->rotation;
    }

    X_REPLY_FIELD_CARD16(scanoutWidth);
    X_REPLY_FIELD_CARD16(scanoutHeight);
    X_REPLY_FIELD_CARD16(x);
    X_REPLY_FIELD_CARD16(y);
    X_REPLY_FIELD_CARD16(width);
    X_REPLY_FIELD_CARD16(height);
    X_REPLY_FIELD_CARD16(rotation);
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

/* Copies the part (sx, sy, w x h) of the root area the CRTC covers, 1:1,
 * into dst at (dx, dy): through a CopyArea from the root with
 * IncludeInferiors, so the capture wraps (ours, Present's per-CRTC flips)
 * see it like any other root copy. */
static void
xscale_copy_root_area(RRCrtcPtr crtc, DrawablePtr dst, int sx, int sy,
                      int w, int h, int dx, int dy)
{
    ScreenPtr screen = dst->pScreen;
    ChangeGCVal val = { .val = IncludeInferiors };
    GCPtr gc = GetScratchGC(dst->depth, screen);

    if (!gc)
        return;
    ChangeGC(NULL, gc, GCSubwindowMode, &val);
    ValidateGC(dst, gc);
    (void) (*gc->ops->CopyArea)(&screen->root->drawable, dst, gc,
                                crtc->x + sx, crtc->y + sy, w, h, dx, dy);
    FreeScratchGC(gc);
}

/* Renders the part (sx, sy, w x h) of a scaled CRTC's scanout from the
 * root through the CRTC transform (bilinear) into dst at (dx, dy): what the
 * DDX does itself to make that scanout, for a DDX that doesn't let us read
 * its buffer. */
static void
xscale_render_root_scanout(RRCrtcPtr crtc, DrawablePtr dst, int sx, int sy,
                           int w, int h, int dx, int dy)
{
    ScreenPtr screen = dst->pScreen;
    XID include_inferiors = IncludeInferiors;
    PictFormatPtr dst_format;
    PictFilterPtr filter;
    PicturePtr srcp, dstp;
    int error;

    dst_format = dst->type == DRAWABLE_WINDOW
                     ? PictureWindowFormat((WindowPtr) dst)
                     : PictureWindowFormat(screen->root);
    srcp = CreatePicture(None, &screen->root->drawable,
                         PictureWindowFormat(screen->root), CPSubwindowMode,
                         &include_inferiors, serverClient, &error);
    dstp = CreatePicture(None, dst, dst_format, 0L, NULL, serverClient, &error);
    if (srcp && dstp) {
        /* crtc->transform maps scanout pixels to root pixels, the CRTC's
         * position included. */
        SetPictureTransform(srcp, &crtc->transform);
        filter = PictureFindFilter(screen, (char *) FilterBilinear,
                                   strlen(FilterBilinear));
        if (filter)
            SetPicturePictFilter(srcp, filter, NULL, 0);
        CompositePicture(PictOpSrc, srcp, NULL, dstp, sx, sy, 0, 0, dx, dy,
                         w, h);
    }
    if (srcp)
        FreePicture(srcp, None);
    if (dstp)
        FreePicture(dstp, None);
}

/* XScaleCopyCrtcScanout: what the CRTC scans out, into the client's
 * drawable. See xscaleproto.h. */
static int
ProcXScaleCopyCrtcScanout(ClientPtr client)
{
    const XScaleScanoutFuncs *funcs;
    RRCrtcPtr crtc;
    DrawablePtr dst;
    BoxRec part;
    PixmapPtr scanout;
    double sx, sy;
    int width, height, err;

    X_REQUEST_HEAD_STRUCT(xXScaleCopyCrtcScanoutReq);
    X_REQUEST_FIELD_CARD32(crtc);
    X_REQUEST_FIELD_CARD32(drawable);
    X_REQUEST_FIELD_CARD16(srcX);
    X_REQUEST_FIELD_CARD16(srcY);
    X_REQUEST_FIELD_CARD16(width);
    X_REQUEST_FIELD_CARD16(height);
    X_REQUEST_FIELD_CARD16(dstX);
    X_REQUEST_FIELD_CARD16(dstY);

    VERIFY_RR_CRTC(stuff->crtc, crtc, DixReadAccess);

    err = dixLookupDrawable(&dst, stuff->drawable, client, M_ANY,
                            DixWriteAccess);
    if (err != Success)
        return err;

    /* The same permission as reading the screen any other way. */
    if (!XnotifyIsAllowed(client, XNOTIFY_SCREEN))
        return BadAccess;

    if (!crtc->mode || dst->pScreen != crtc->pScreen ||
        dst->depth != crtc->pScreen->rootDepth)
        return BadMatch;

    /* The requested part, clipped to the scanout. */
    xscale_scanout_size(crtc, &width, &height);
    part.x1 = stuff->srcX;
    part.y1 = stuff->srcY;
    part.x2 = stuff->width ? stuff->srcX + stuff->width : width;
    part.y2 = stuff->height ? stuff->srcY + stuff->height : height;
    if (!stuff->width || !stuff->height)
        part.x1 = part.y1 = 0;
    part.x1 = max(part.x1, 0);
    part.y1 = max(part.y1, 0);
    part.x2 = min(part.x2, width);
    part.y2 = min(part.y2, height);
    if (part.x1 >= part.x2 || part.y1 >= part.y2)
        return Success;

    width = part.x2 - part.x1;
    height = part.y2 - part.y1;

    /* Only a plainly scaled CRTC has a resolution the root doesn't: every
     * other one is its root area, 1:1. */
    if (!xscale_crtc_scale(crtc, &sx, &sy) ||
        (fabs(sx - 1.0) < 1e-6 && fabs(sy - 1.0) < 1e-6)) {
        xscale_copy_root_area(crtc, dst, part.x1, part.y1, width, height,
                              stuff->dstX, stuff->dstY);
        return Success;
    }

    /* The buffer going to the monitor, when the DDX lets us read it: one
     * plain copy, and exactly the picture shown, sharp or not. */
    funcs = xscale_funcs(crtc->pScreen);
    if (xscale_presented(crtc) && xscale_presented(crtc)->shown)
        scanout = xscale_presented(crtc)->shown->pixmap;    /* flipped */
    else
        scanout = funcs && funcs->scanout_pixmap ? funcs->scanout_pixmap(crtc)
                                                 : NULL;
    if (scanout) {
        GCPtr gc = GetScratchGC(dst->depth, dst->pScreen);

        if (gc) {
            ValidateGC(dst, gc);
            (void) (*gc->ops->CopyArea)(&scanout->drawable, dst, gc,
                                        part.x1, part.y1, width, height,
                                        stuff->dstX, stuff->dstY);
            FreeScratchGC(gc);
        }
        return Success;
    }

    xscale_render_root_scanout(crtc, dst, part.x1, part.y1, width, height,
                               stuff->dstX, stuff->dstY);
    return Success;
}

/* XScalePresentCrtcScanout: shows a whole frame on the CRTC, flipped or
 * copied. See xscaleproto.h, and "Presented pixmaps" above. */
static int
ProcXScalePresentCrtcScanout(ClientPtr client)
{
    RRCrtcPtr crtc;
    PixmapPtr pixmap;
    RegionPtr update;
    xscale_source_ptr src;
    xscale_frame_ptr f, old;
    int width, height, ret;
    Bool fresh;

    X_REQUEST_HEAD_STRUCT(xXScalePresentCrtcScanoutReq);
    X_REQUEST_FIELD_CARD32(crtc);
    X_REQUEST_FIELD_CARD32(pixmap);
    X_REQUEST_FIELD_CARD32(serial);
    X_REQUEST_FIELD_CARD32(update);

    VERIFY_RR_CRTC(stuff->crtc, crtc, DixSetAttrAccess);

    ret = dixLookupResourceByType((void **) &pixmap, stuff->pixmap,
                                 X11_RESTYPE_PIXMAP, client, DixReadAccess);
    if (ret != Success) {
        client->errorValue = stuff->pixmap;
        return ret == BadValue ? BadPixmap : ret;
    }
    VERIFY_REGION_OR_NONE(update, stuff->update, client, DixReadAccess);

    src = crtc->xscale_source;
    if (src && src->client != client)
        return BadAccess;

    if (pixmap->drawable.pScreen != crtc->pScreen ||
        pixmap->drawable.depth != crtc->pScreen->rootDepth ||
        !xscale_crtc_capable(crtc, &width, &height) ||
        pixmap->drawable.width != width || pixmap->drawable.height != height)
        return BadMatch;

    f = calloc(1, sizeof(*f));
    if (!f)
        return BadAlloc;
    fresh = xscale_presented_source(client, crtc, width, height, &src);
    if (!src) {
        free(f);
        return BadAlloc;
    }

    f->pixmap = pixmap;
    pixmap->refcnt++;
    f->pixmap_id = stuff->pixmap;
    f->serial = stuff->serial;
    f->refs = 1;                /* the `current` slot's */
    xorg_list_init(&f->entry);

    /* What the DDX's copy path takes next (XScaleTakeScanoutDamage) */
    if (update) {
        BoxRec all = { 0, 0, width, height };
        RegionRec clip;

        RegionInit(&clip, &all, 1);
        RegionIntersect(&clip, &clip, update);
        RegionUnion(&src->pending, &src->pending, &clip);
        RegionUninit(&clip);
    } else
        src->full = TRUE;

    old = src->current;
    src->current = f;
    src->drawable = &pixmap->drawable;
    src->src_x = src->src_y = 0;
    xscale_frame_unref(src, old);

    if (fresh)
        xscale_source_changed(crtc);

    if (src->flipping || src->unflipping) {
        old = src->queued;
        src->queued = xscale_frame_ref(f);
        xscale_frame_unref(src, old);
    } else
        xscale_frame_show(crtc, src, f);
    return Success;
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
    case X_XScaleCopyCrtcScanout:       return ProcXScaleCopyCrtcScanout(client);
    case X_XScalePresentCrtcScanout:    return ProcXScalePresentCrtcScanout(client);
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
    EventSwapVector[xscale_event_base + XScaleScanoutComplete] =
        (EventSwapPtr) SXScaleScanoutCompleteEvent;
    EventSwapVector[xscale_event_base + XScaleScanoutIdle] =
        (EventSwapPtr) SXScaleScanoutIdleEvent;
}
