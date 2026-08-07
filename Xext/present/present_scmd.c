/*
 * Copyright © 2013 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */
#include <dix-config.h>

#include <string.h>
#include <stdbool.h>

#include "dix/screenint_priv.h"
#include "Xext/randr/randrstr_priv.h"
#include "Xext/present/present_priv.h"

#include <servermd.h>
#include <misync.h>
#include <misyncstr.h>

/*
 * Screen flip mode
 *
 * Provides the default mode for drivers, that do not
 * support flips and the full screen flip mode.
 *
 */

static uint64_t present_scmd_event_id;

static struct xorg_list present_exec_queue;
static struct xorg_list present_flip_queue;

static void
present_execute(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc);

/*
 * Runtime-gated flip-decision tracing (XiS).
 *
 * present_check_flip() is the single gate that decides whether a Present
 * request becomes a zero-copy page flip or a (more expensive) copy to
 * scanout. Its decisions are otherwise invisible on a shipped binary:
 * DebugPresent is compiled out (#if 0) by default. This tracer lets us
 * observe, without recompiling, *why* a given window did or did not flip --
 * in particular the whole-root region test at present_check_flip() that
 * forces every per-monitor (single-CRTC) window on a multi-CRTC screen to
 * fall back to a copy, which is the cost this per-CRTC-flip work targets.
 *
 * Enable with XLIBRE_PRESENT_FLIP_DEBUG=1 in the server's environment;
 * silent and effectively free otherwise.
 */
static Bool
present_flip_debug_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("XLIBRE_PRESENT_FLIP_DEBUG") ? 1 : 0;
    return enabled;
}

#define PresentFlipDebug(...) do {                                      \
        if (present_flip_debug_enabled())                               \
            ErrorF("[present/flip] " __VA_ARGS__);                      \
    } while (0)

/*
 * Look up (or, if 'create', allocate) the flip state tracking one CRTC on this
 * screen. Nodes live until the screen's flip machinery is torn down. Returns
 * NULL if there is no such node (and !create) or on allocation failure.
 */
static present_flip_state_ptr
present_flip_state(present_screen_priv_ptr screen_priv, RRCrtcPtr crtc, Bool create)
{
    present_flip_state_ptr fs;

    if (!screen_priv)
        return NULL;

    xorg_list_for_each_entry(fs, &screen_priv->flip_states, link) {
        if (fs->crtc == crtc)
            return fs;
    }

    if (!create)
        return NULL;

    fs = calloc(1, sizeof(*fs));
    if (!fs)
        return NULL;
    fs->crtc = crtc;
    xorg_list_add(&fs->link, &screen_priv->flip_states);
    return fs;
}

/*
 * Is 'pixmap' the current or pending flip pixmap of any CRTC on this screen?
 * Used to tell a legitimately-flipping window apart from a Composite-redirected
 * one, whose pixmap is neither the screen pixmap nor any flip pixmap.
 */
static Bool
present_pixmap_is_flip(present_screen_priv_ptr screen_priv, PixmapPtr pixmap)
{
    present_flip_state_ptr fs;

    if (!screen_priv || !pixmap)
        return FALSE;

    xorg_list_for_each_entry(fs, &screen_priv->flip_states, link) {
        if (fs->flip_pixmap == pixmap)
            return TRUE;
        if (fs->flip_pending && fs->flip_pending->pixmap == pixmap)
            return TRUE;
    }
    return FALSE;
}

/* Does the driver's flip path support flipping a single-CRTC-sized pixmap to
 * one CRTC (as opposed to only a whole-screen buffer flipped to all CRTCs)? */
static Bool
present_flip_can_crtc(present_screen_priv_ptr screen_priv)
{
    return screen_priv->info &&
           screen_priv->info->version >= 2 &&
           screen_priv->info->capable_flip_crtc;
}

/* Fill 'box' with the desktop-space scanout rectangle of 'crtc', honouring
 * rotation. Returns FALSE if the CRTC has no mode (disabled). */
static Bool
present_crtc_box(RRCrtcPtr crtc, BoxPtr box)
{
    if (!crtc || !crtc->mode)
        return FALSE;

    box->x1 = crtc->x;
    box->y1 = crtc->y;
    switch (crtc->rotation) {
    case RR_Rotate_90:
    case RR_Rotate_270:
        box->x2 = crtc->x + crtc->mode->mode.height;
        box->y2 = crtc->y + crtc->mode->mode.width;
        break;
    default:
        box->x2 = crtc->x + crtc->mode->mode.width;
        box->y2 = crtc->y + crtc->mode->mode.height;
        break;
    }
    return TRUE;
}

static inline Bool
present_box_equal(BoxPtr a, BoxPtr b)
{
    return a->x1 == b->x1 && a->y1 == b->y1 &&
           a->x2 == b->x2 && a->y2 == b->y2;
}

/* Is 'region' exactly the single rectangle 'box'? */
static Bool
present_region_is_box(RegionPtr region, BoxPtr box)
{
    return RegionNumRects(region) == 1 &&
           present_box_equal(RegionExtents(region), box);
}

/*
 * A per-CRTC flip pixmap covers a single CRTC, so it is smaller than the whole
 * screen; a whole-screen flip pixmap matches the screen pixmap. Unlike a
 * whole-screen flip, a per-CRTC flip only redirects one CRTC's scanout -- it
 * does not replace the screen pixmap or the window-tree pixmaps -- so the code
 * that swaps/restores those must be skipped for it.
 */
static Bool
present_flip_is_per_crtc(ScreenPtr screen, PixmapPtr pixmap)
{
    PixmapPtr screen_pixmap = (*screen->GetScreenPixmap)(screen);

    return pixmap &&
           (pixmap->drawable.width != screen_pixmap->drawable.width ||
            pixmap->drawable.height != screen_pixmap->drawable.height);
}

/*
 * Read the 'sw'x'sh' rectangle at (src_sx,src_sy) of drawable 'src' via the
 * (already-unwrapped) real GetImage and place it into the ZPixmap result buffer
 * 'pdstLine' at column 'dx', row 'dy' (offsets within a 'w'-wide, 'dst_stride'-
 * strided result). When the rectangle spans the full result width the rows line
 * up and it reads straight in; otherwise it stages through a temp and copies
 * each row into its sub-rectangle.
 */
static void
present_read_into_result(ScreenPtr screen, DrawablePtr src,
                         int src_sx, int src_sy, int sw, int sh,
                         int dx, int dy, int w,
                         int depth, int bpp8, int dst_stride,
                         unsigned int format, unsigned long planeMask,
                         char *pdstLine)
{
    int     tmp_stride, r;
    char   *tmp;

    if (dx == 0 && sw == w) {
        (*screen->GetImage)(src, src_sx, src_sy, sw, sh, format, planeMask,
                            pdstLine + (size_t) dy * dst_stride);
        return;
    }

    tmp_stride = PixmapBytePad(sw, depth);
    tmp = malloc((size_t) tmp_stride * sh);
    if (!tmp)
        return;

    (*screen->GetImage)(src, src_sx, src_sy, sw, sh, format, planeMask, tmp);

    for (r = 0; r < sh; r++)
        memcpy(pdstLine + (size_t)(dy + r) * dst_stride + (size_t) dx * bpp8,
               tmp + (size_t) r * tmp_stride,
               (size_t) sw * bpp8);

    free(tmp);
}

/*
 * Is 'fs' a per-CRTC flip whose displayed content present_flip_overlay_image
 * will substitute into a root capture of the given pixel format? If so, fill
 * 'box' with its (desktop-space) CRTC rectangle. Used both to punch flipped
 * regions out of the screen-pixmap read and to drive the substitution, so the
 * two stay exactly in sync (every punched pixel is refilled, and vice versa).
 */
static Bool
present_flip_overlay_valid(present_flip_state_ptr fs, int depth, int bpp,
                           BoxPtr box)
{
    PixmapPtr fp = fs->flip_pixmap;

    return fp && fs->crtc && present_crtc_box(fs->crtc, box) &&
           fp->drawable.depth == depth &&
           fp->drawable.bitsPerPixel == bpp;
}

/*
 * When a per-CRTC flip is active, that CRTC scans out the flip pixmap, not the
 * screen pixmap -- so a root GetImage (as used by XGetImage/XShmGetImage screen
 * recorders) reads stale content for that region. Substitute the flipped
 * content back in: for each active per-CRTC flip whose CRTC rectangle overlaps
 * the requested region, read the flip pixmap for the overlap and copy it into
 * the caller's result buffer. Reads only; the screen pixmap isn't touched, so
 * this generates no damage (and thus no feedback loop with XDamage-driven
 * recorders). Cost is paid only when something captures.
 */
void
present_flip_overlay_image(DrawablePtr pDrawable, int sx, int sy, int w, int h,
                           unsigned int format, unsigned long planeMask,
                           char *pdstLine)
{
    ScreenPtr                   screen = pDrawable->pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_flip_state_ptr      fs;
    int                         depth, bpp8, dst_stride;
    int                         req_x0, req_y0, req_x1, req_y1;

    if (!screen_priv)
        return;

    /* Only the root ("the screen") capture path needs the displayed (flipped)
     * content substituted in; a specific window's GetImage wants that window. */
    if (pDrawable->type != DRAWABLE_WINDOW ||
        (WindowPtr) pDrawable != screen->root)
        return;

    /* Only packed (ZPixmap), byte-aligned pixels are handled; anything else
     * (rare XYPixmap/bitplane reads) passes through unchanged. */
    if (format != ZPixmap)
        return;
    depth = pDrawable->depth;
    if (pDrawable->bitsPerPixel % 8)
        return;
    bpp8 = pDrawable->bitsPerPixel / 8;

    req_x0 = pDrawable->x + sx;
    req_y0 = pDrawable->y + sy;
    req_x1 = req_x0 + w;
    req_y1 = req_y0 + h;
    dst_stride = PixmapBytePad(w, depth);

    xorg_list_for_each_entry(fs, &screen_priv->flip_states, link) {
        BoxRec      box;
        int         ix0, iy0, ix1, iy1, sw, sh;

        if (!present_flip_overlay_valid(fs, depth, pDrawable->bitsPerPixel, &box))
            continue;

        /* Intersect the requested region with this CRTC's rectangle. */
        ix0 = max(req_x0, box.x1);
        iy0 = max(req_y0, box.y1);
        ix1 = min(req_x1, box.x2);
        iy1 = min(req_y1, box.y2);
        if (ix0 >= ix1 || iy0 >= iy1)
            continue;

        sw = ix1 - ix0;
        sh = iy1 - iy0;

        /* The flip pixmap's origin is the CRTC origin, so shift the read by the
         * CRTC's top-left; place it at the overlap's offset in the result. */
        present_read_into_result(screen, &fs->flip_pixmap->drawable,
                                 ix0 - box.x1, iy0 - box.y1, sw, sh,
                                 ix0 - req_x0, iy0 - req_y0, w,
                                 depth, bpp8, dst_stride,
                                 format, planeMask, pdstLine);
    }
}

/*
 * Root-capture GetImage that avoids reading the screen pixmap for regions that
 * are page-flipped per CRTC (which present_flip_overlay_image would only
 * overwrite anyway -- a redundant, ~2x GPU readback while recording). Reads the
 * screen pixmap for just the non-flipped remainder, then substitutes the
 * flipped regions from the flip buffers.
 *
 * Returns TRUE if it handled the read (a root ZPixmap capture with at least one
 * overlapping per-CRTC flip). Returns FALSE -- unchanged behaviour -- for any
 * other read, so the caller does a plain full GetImage.
 */
Bool
present_flip_getimage(DrawablePtr pDrawable, int sx, int sy, int w, int h,
                      unsigned int format, unsigned long planeMask,
                      char *pdstLine)
{
    ScreenPtr                   screen = pDrawable->pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_flip_state_ptr      fs;
    int                         depth, bpp8, dst_stride;
    int                         req_x0, req_y0, req_x1, req_y1;
    RegionRec                   remaining;
    BoxRec                      req_box;
    BoxPtr                      rects;
    Bool                        any = FALSE;
    int                         nrects, i;

    if (!screen_priv)
        return FALSE;
    if (pDrawable->type != DRAWABLE_WINDOW ||
        (WindowPtr) pDrawable != screen->root)
        return FALSE;
    if (format != ZPixmap)
        return FALSE;
    depth = pDrawable->depth;
    if (pDrawable->bitsPerPixel % 8)
        return FALSE;
    bpp8 = pDrawable->bitsPerPixel / 8;

    req_x0 = pDrawable->x + sx;
    req_y0 = pDrawable->y + sy;
    req_x1 = req_x0 + w;
    req_y1 = req_y0 + h;
    dst_stride = PixmapBytePad(w, depth);

    req_box.x1 = req_x0;
    req_box.y1 = req_y0;
    req_box.x2 = req_x1;
    req_box.y2 = req_y1;
    RegionInit(&remaining, &req_box, 1);

    /* Punch each flipped CRTC rectangle out of what we'll read from the screen
     * pixmap. Only flips present_flip_overlay_image will refill are punched
     * (same predicate), so every punched pixel is guaranteed to be restored. */
    xorg_list_for_each_entry(fs, &screen_priv->flip_states, link) {
        BoxRec      box;
        RegionRec   r;

        if (!present_flip_overlay_valid(fs, depth, pDrawable->bitsPerPixel, &box))
            continue;

        /* Clip to the requested rect before subtracting. */
        if (box.x1 < req_x0) box.x1 = req_x0;
        if (box.y1 < req_y0) box.y1 = req_y0;
        if (box.x2 > req_x1) box.x2 = req_x1;
        if (box.y2 > req_y1) box.y2 = req_y1;
        if (box.x1 >= box.x2 || box.y1 >= box.y2)
            continue;

        RegionInit(&r, &box, 1);
        RegionSubtract(&remaining, &remaining, &r);
        RegionUninit(&r);
        any = TRUE;
    }

    if (!any) {
        /* No per-CRTC flip overlaps this read: leave it to the caller's plain
         * full GetImage (identical to the pre-optimization behaviour). */
        RegionUninit(&remaining);
        return FALSE;
    }

    /* Read the non-flipped remainder straight from the screen pixmap. */
    nrects = RegionNumRects(&remaining);
    rects = RegionRects(&remaining);
    for (i = 0; i < nrects; i++) {
        BoxPtr b = &rects[i];

        present_read_into_result(screen, pDrawable,
                                 b->x1 - pDrawable->x, b->y1 - pDrawable->y,
                                 b->x2 - b->x1, b->y2 - b->y1,
                                 b->x1 - req_x0, b->y1 - req_y0, w,
                                 depth, bpp8, dst_stride,
                                 format, planeMask, pdstLine);
    }
    RegionUninit(&remaining);

    /* Fill the flipped regions from the flip buffers. */
    present_flip_overlay_image(pDrawable, sx, sy, w, h, format, planeMask,
                               pdstLine);
    return TRUE;
}

static Bool
present_check_flip(RRCrtcPtr            crtc,
                   WindowPtr            window,
                   PixmapPtr            pixmap,
                   Bool                 sync_flip,
                   RegionPtr            valid,
                   int16_t              x_off,
                   int16_t              y_off,
                   PresentFlipReason   *reason)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    PixmapPtr                   window_pixmap;
    WindowPtr                   root = screen->root;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    PresentFlipReason           tmp_reason = PRESENT_FLIP_REASON_UNKNOWN;
    BoxRec                      crtc_box;
    Bool                        have_crtc_box;
    Bool                        per_crtc = FALSE;
    int16_t                     target_x, target_y;

    if (crtc) {
       screen_priv = present_screen_priv(crtc->pScreen);
    }

    if (reason)
        *reason = PRESENT_FLIP_REASON_UNKNOWN;

    if (!screen_priv)
        return FALSE;

    if (!screen_priv->info)
        return FALSE;

    if (!crtc)
        return FALSE;

    /* Check to see if the driver supports flips at all */
    if (!screen_priv->info->flip) {
        PresentFlipDebug("reject win 0x%08lx: driver has no flip hook\n",
                         (unsigned long) window->drawable.id);
        return FALSE;
    }

    /* Ask the driver for permission. Do this now to see if there's TearFree. */
    if (screen_priv->info->version >= 1 && screen_priv->info->check_flip2) {
        if (!(*screen_priv->info->check_flip2) (crtc, window, pixmap, sync_flip, &tmp_reason)) {
            DebugPresent(("\td %08" PRIx32 " -> %08" PRIx32 "\n", window->drawable.id, pixmap ? pixmap->drawable.id : 0));
            /* It's fine to return now unless the page flip failure reason is
             * PRESENT_FLIP_REASON_BUFFER_FORMAT; we must only output that
             * reason if all the other checks pass.
             */
            if (!reason || tmp_reason != PRESENT_FLIP_REASON_BUFFER_FORMAT) {
                if (reason)
                    *reason = tmp_reason;
                PresentFlipDebug("reject win 0x%08lx: driver check_flip2 said no "
                                 "(reason %d)\n",
                                 (unsigned long) window->drawable.id, tmp_reason);
                return FALSE;
            }
        }
    } else if (screen_priv->info->check_flip) {
        if (!(*screen_priv->info->check_flip) (crtc, window, pixmap, sync_flip)) {
            DebugPresent(("\td %08" PRIx32 " -> %08" PRIx32 "\n", window->drawable.id, pixmap ? pixmap->drawable.id : 0));
            return FALSE;
        }
    }

    /* Make sure the window hasn't been redirected with Composite */
    window_pixmap = screen->GetWindowPixmap(window);
    if (window_pixmap != screen->GetScreenPixmap(screen) &&
        !present_pixmap_is_flip(screen_priv, window_pixmap)) {
        PresentFlipDebug("reject win 0x%08lx: Composite-redirected\n",
                         (unsigned long) window->drawable.id);
        return FALSE;
    }

    /*
     * A flip must cover either the whole screen (one buffer flipped to every
     * CRTC, the long-standing path) or -- on drivers that advertise per-CRTC
     * flip support -- exactly the target CRTC's scanout rectangle (that one
     * buffer flipped to that one CRTC). Anything else falls back to a copy.
     */
    have_crtc_box = present_crtc_box(crtc, &crtc_box);

    if (RegionEqual(&window->clipList, &root->winSize)) {
        /* Whole-screen flip: window/pixmap must cover the root from (0,0). */
        target_x = 0;
        target_y = 0;
    } else if (present_flip_can_crtc(screen_priv) && have_crtc_box &&
               present_region_is_box(&window->clipList, &crtc_box)) {
        /* Per-CRTC flip: window/pixmap must cover exactly this CRTC. */
        per_crtc = TRUE;
        target_x = crtc_box.x1;
        target_y = crtc_box.y1;
    } else {
        if (present_flip_debug_enabled()) {
            BoxPtr wb = RegionExtents(&window->clipList);
            BoxPtr rb = RegionExtents(&root->winSize);
            Bool matches_crtc = have_crtc_box &&
                present_region_is_box(&window->clipList, &crtc_box);
            ErrorF("[present/flip] reject win 0x%08lx: not full-screen; "
                   "clipList (%d,%d)-(%d,%d) != root winSize (%d,%d)-(%d,%d)%s\n",
                   (unsigned long) window->drawable.id,
                   wb->x1, wb->y1, wb->x2, wb->y2,
                   rb->x1, rb->y1, rb->x2, rb->y2,
                   matches_crtc
                     ? " [covers target CRTC, but driver lacks per-CRTC flip capability]"
                     : "");
        }
        return FALSE;
    }

    /* Source pixmap must align with window exactly */
    if (x_off || y_off) {
        PresentFlipDebug("reject win 0x%08lx: nonzero source offset (%d,%d)\n",
                         (unsigned long) window->drawable.id, x_off, y_off);
        return FALSE;
    }

    /* Make sure the area marked as valid fills the flip target */
    if (valid) {
        Bool valid_ok = per_crtc ? present_region_is_box(valid, &crtc_box)
                                 : RegionEqual(valid, &root->winSize);
        if (!valid_ok) {
            PresentFlipDebug("reject win 0x%08lx: valid region != flip target\n",
                             (unsigned long) window->drawable.id);
            return FALSE;
        }
    }

    /*
     * The window and its source pixmap must exactly cover the flip target. For
     * a whole-screen flip the pixmap represents the screen, so its screen-space
     * origin (pixmap->screen_x/y) must match the window at (0,0). A per-CRTC
     * flip reads a standalone CRTC-sized buffer from its own origin (0,0), so
     * pixmap->screen_x/y do not track the window position and are not compared.
     */
    if (window->drawable.x != target_x || window->drawable.y != target_y ||
        (!per_crtc && (window->drawable.x != pixmap->screen_x ||
                       window->drawable.y != pixmap->screen_y)) ||
        window->drawable.width != pixmap->drawable.width ||
        window->drawable.height != pixmap->drawable.height) {
        PresentFlipDebug("reject win 0x%08lx: window/pixmap geometry mismatch "
                         "(win @%d,%d %dx%d vs pixmap @%d,%d %dx%d, target origin %d,%d, %s)\n",
                         (unsigned long) window->drawable.id,
                         window->drawable.x, window->drawable.y,
                         window->drawable.width, window->drawable.height,
                         pixmap->screen_x, pixmap->screen_y,
                         pixmap->drawable.width, pixmap->drawable.height,
                         target_x, target_y, per_crtc ? "per-CRTC" : "whole-screen");
        return FALSE;
    }

    if (tmp_reason == PRESENT_FLIP_REASON_BUFFER_FORMAT) {
        if (reason)
            *reason = tmp_reason;
        PresentFlipDebug("reject win 0x%08lx: buffer format not flippable\n",
                         (unsigned long) window->drawable.id);
        return FALSE;
    }

    PresentFlipDebug("accept win 0x%08lx: page-flip eligible (%s, crtc %p)\n",
                     (unsigned long) window->drawable.id,
                     per_crtc ? "per-CRTC" : "whole-screen", (void *) crtc);
    return TRUE;
}

static Bool
present_flip(RRCrtcPtr crtc,
             uint64_t event_id,
             uint64_t target_msc,
             PixmapPtr pixmap,
             Bool sync_flip)
{
    ScreenPtr                   screen = crtc->pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    return (*screen_priv->info->flip) (crtc, event_id, target_msc, pixmap, sync_flip);
}

static RRCrtcPtr
present_scmd_get_crtc(present_screen_priv_ptr screen_priv, WindowPtr window)
{
    if (!screen_priv->info)
        return NULL;

    if (!screen_priv->info->get_crtc)
        return NULL;

    return (*screen_priv->info->get_crtc)(window);
}

static uint32_t
present_scmd_query_capabilities(present_screen_priv_ptr screen_priv)
{
    if (!screen_priv->info)
        return 0;

    return screen_priv->info->capabilities;
}

static int
present_get_ust_msc(ScreenPtr screen, RRCrtcPtr crtc, uint64_t *ust, uint64_t *msc)
{
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_screen_priv_ptr     crtc_screen_priv = screen_priv;
    if (crtc)
        crtc_screen_priv = present_screen_priv(crtc->pScreen);

    if (crtc == NULL)
        return present_fake_get_ust_msc(screen, ust, msc);
    else
        return (*crtc_screen_priv->info->get_ust_msc)(crtc, ust, msc);
}

static void
present_flush(WindowPtr window)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    if (!screen_priv)
        return;

    if (!screen_priv->info)
        return;

    if (!screen_priv->info->flush)
        return;

    (*screen_priv->info->flush) (window);
}

static int
present_queue_vblank(ScreenPtr screen,
                     WindowPtr window,
                     RRCrtcPtr crtc,
                     uint64_t event_id,
                     uint64_t msc)
{
    bool                        ret;

    if (crtc == NULL)
        ret = present_fake_queue_vblank(screen, event_id, msc);
    else
    {
        present_screen_priv_ptr     screen_priv = present_screen_priv(crtc->pScreen);
        ret = (*screen_priv->info->queue_vblank) (crtc, event_id, msc);
    }
    return ret;
}

/*
 * When the wait fence or previous flip is completed, it's time
 * to re-try the request
 */
static void
present_re_execute(present_vblank_ptr vblank)
{
    uint64_t            ust = 0, crtc_msc = 0;

    if (vblank->crtc)
        (void) present_get_ust_msc(vblank->screen, vblank->crtc, &ust, &crtc_msc);

    present_execute(vblank, ust, crtc_msc);
}

static void
present_flip_try_ready(ScreenPtr screen)
{
    present_vblank_ptr  vblank;

    xorg_list_for_each_entry(vblank, &present_flip_queue, event_queue) {
        if (vblank->queued) {
            present_re_execute(vblank);
            return;
        }
    }
}

static void
present_flip_idle(present_flip_state_ptr fs)
{
    if (fs->flip_pixmap) {
        present_pixmap_idle(fs->flip_pixmap, fs->flip_window,
                            fs->flip_serial, fs->flip_idle_fence);
        if (fs->flip_idle_fence)
            present_fence_destroy(fs->flip_idle_fence);
        dixDestroyPixmap(fs->flip_pixmap, fs->flip_pixmap->drawable.id);
        fs->flip_crtc = NULL;
        fs->flip_window = NULL;
        fs->flip_serial = 0;
        fs->flip_pixmap = NULL;
        fs->flip_idle_fence = NULL;
    }
}

void
present_restore_screen_pixmap(ScreenPtr screen, present_flip_state_ptr fs)
{
    PixmapPtr screen_pixmap = (*screen->GetScreenPixmap)(screen);
    PixmapPtr flip_pixmap;
    WindowPtr flip_window;
    RRCrtcPtr flip_crtc;

    if (fs->flip_pending) {
        flip_window = fs->flip_pending->window;
        flip_pixmap = fs->flip_pending->pixmap;
        flip_crtc = fs->flip_pending->crtc;
    } else {
        flip_window = fs->flip_window;
        flip_pixmap = fs->flip_pixmap;
        flip_crtc = fs->flip_crtc;
    }

    assert (flip_pixmap);

    /* A per-CRTC flip never replaced the screen pixmap or the window-tree
     * pixmaps (it only redirected one CRTC's scanout), so the pixmap/tree swaps
     * below don't apply to it. But that CRTC's region of the screen pixmap was
     * never written while the CRTC scanned out the flip buffer, so on unflip it
     * would keep showing stale content -- a frozen "ghost" frame -- until
     * something else repaints it. Seed the region with the last flipped frame
     * at the CRTC's position: the CopyArea damages that region, so the driver
     * (including TearFree, which only refreshes damaged areas) repaints it
     * cleanly on its next vblank instead of scanning out stale pixels.
     *
     * Only seed when the flipping window still covers the whole CRTC (its
     * geometry still matches the CRTC box), i.e. a "clean" unflip like a VT
     * switch or DPMS blank where that same content should reappear. If the
     * window has since shrunk or moved -- e.g. a video leaving fullscreen --
     * its last full-CRTC frame no longer belongs across the areas it has
     * vacated, and with no compositor to repaint, seeding it there would leave
     * that frame lingering as a fake background. In that case leave the screen
     * pixmap untouched.
     *
     * Skip rotated/reflected CRTCs too: the flip buffer holds scanout-oriented
     * pixels that wouldn't match the screen pixmap's logical space (and rotated
     * per-CRTC flips are an untested edge case anyway). */
    if (present_flip_is_per_crtc(screen, flip_pixmap)) {
        BoxRec box;

        if (flip_crtc && flip_window &&
            flip_crtc->rotation == RR_Rotate_0 &&
            present_crtc_box(flip_crtc, &box) &&
            flip_window->drawable.x == box.x1 &&
            flip_window->drawable.y == box.y1 &&
            flip_window->drawable.width == box.x2 - box.x1 &&
            flip_window->drawable.height == box.y2 - box.y1)
            present_copy_region(&screen_pixmap->drawable, flip_pixmap, NULL,
                                box.x1, box.y1);
        return;
    }

    /* Update the screen pixmap with the current flip pixmap contents
     * Only do this the first time for a particular unflip operation, or
     * we'll probably scribble over other windows
     */
    if (screen->root && screen->GetWindowPixmap(screen->root) == flip_pixmap)
        present_copy_region(&screen_pixmap->drawable, flip_pixmap, NULL, 0, 0);

    /* Switch back to using the screen pixmap now to avoid
     * 2D applications drawing to the wrong pixmap.
     */
    if (flip_window)
        present_set_tree_pixmap(flip_window, flip_pixmap, screen_pixmap);
    if (screen->root)
        present_set_tree_pixmap(screen->root, NULL, screen_pixmap);
}

void
present_set_abort_flip(ScreenPtr screen, present_flip_state_ptr fs)
{
    if (!fs->flip_pending->abort_flip) {
        present_restore_screen_pixmap(screen, fs);
        fs->flip_pending->abort_flip = TRUE;
    }
}

static void
present_unflip(ScreenPtr screen, present_flip_state_ptr fs)
{
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    assert (!fs->unflip_event_id);
    assert (!fs->flip_pending);

    present_restore_screen_pixmap(screen, fs);

    fs->unflip_event_id = ++present_scmd_event_id;
    DebugPresent(("u %" PRIu64 "\n", fs->unflip_event_id));

    /* End a per-CRTC flip by restoring only its CRTC, when the driver supports
     * it. The whole-screen unflip flips EVERY CRTC back to the shared fb, which
     * for a per-CRTC flip would collide with another CRTC's still-pending flip
     * (EBUSY -> re-entrant flush -> crash) or push a rotated CRTC to the wrong
     * scanout. Restoring just this CRTC leaves the others untouched. */
    if (fs->flip_crtc &&
        screen_priv->info->version >= 2 &&
        screen_priv->info->unflip_crtc &&
        present_flip_is_per_crtc(screen, fs->flip_pixmap))
        (*screen_priv->info->unflip_crtc) (screen, fs->flip_crtc,
                                           fs->unflip_event_id);
    else
        (*screen_priv->info->unflip) (screen, fs->unflip_event_id);
}

static void
present_flip_notify(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    ScreenPtr                   screen = vblank->screen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_flip_state_ptr      fs = present_flip_state(screen_priv, vblank->crtc, TRUE);

    DebugPresent(("\tn %" PRIu64 " %p %" PRIu64 " %" PRIu64 ": %08" PRIx32 " -> %08" PRIx32 "\n",
                  vblank->event_id, vblank, vblank->exec_msc, vblank->target_msc,
                  vblank->pixmap ? vblank->pixmap->drawable.id : 0,
                  vblank->window ? vblank->window->drawable.id : 0));

    assert (vblank == fs->flip_pending);

    present_flip_idle(fs);

    xorg_list_del(&vblank->event_queue);

    /* Transfer reference for pixmap and fence from vblank to the flip state */
    fs->flip_crtc = vblank->crtc;
    fs->flip_window = vblank->window;
    fs->flip_serial = vblank->serial;
    fs->flip_pixmap = vblank->pixmap;
    fs->flip_sync = vblank->sync_flip;
    fs->flip_idle_fence = vblank->idle_fence;

    vblank->pixmap = NULL;
    vblank->idle_fence = NULL;

    fs->flip_pending = NULL;

    if (vblank->abort_flip)
        present_unflip(screen, fs);

    present_vblank_notify(vblank, PresentCompleteKindPixmap, PresentCompleteModeFlip, ust, crtc_msc);
    present_vblank_destroy(vblank);

    present_flip_try_ready(screen);
}

void
present_event_notify(uint64_t event_id, uint64_t ust, uint64_t msc)
{
    present_vblank_ptr  vblank;

    if (!event_id)
        return;
    DebugPresent(("\te %" PRIu64 " ust %" PRIu64 " msc %" PRIu64 "\n", event_id, ust, msc));
    xorg_list_for_each_entry(vblank, &present_exec_queue, event_queue) {
        int64_t match = event_id - vblank->event_id;
        if (match == 0) {
            present_execute(vblank, ust, msc);
            return;
        }
    }
    xorg_list_for_each_entry(vblank, &present_flip_queue, event_queue) {
        if (vblank->event_id == event_id) {
            if (vblank->queued)
                present_execute(vblank, ust, msc);
            else
                present_flip_notify(vblank, ust, msc);
            return;
        }
    }

    DIX_FOR_EACH_SCREEN({
        present_screen_priv_ptr screen_priv = present_screen_priv(walkScreen);
        present_flip_state_ptr  fs;

        xorg_list_for_each_entry(fs, &screen_priv->flip_states, link) {
            if (event_id == fs->unflip_event_id) {
                DebugPresent(("\tun %" PRIu64 "\n", event_id));
                fs->unflip_event_id = 0;
                present_flip_idle(fs);
                present_flip_try_ready(walkScreen);
                return;
            }
        }
    });
}

/*
 * 'window' is being reconfigured. Check to see if it is involved
 * in flipping and clean up as necessary
 */
static void
present_check_flip_window (WindowPtr window)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_window_priv_ptr     window_priv = present_window_priv(window);
    present_flip_state_ptr      fs;
    present_vblank_ptr          vblank;
    PresentFlipReason           reason;

    /* If this window hasn't ever been used with Present, it can't be
     * flipping
     */
    if (!window_priv)
        return;

    /* Re-validate the pending/current flip of every CRTC this window may be
     * flipping on; a CRTC busy unflipping is skipped and re-checked later.
     */
    xorg_list_for_each_entry(fs, &screen_priv->flip_states, link) {
        if (fs->unflip_event_id)
            continue;

        if (fs->flip_pending) {
            /* Check pending flip */
            if (fs->flip_pending->window == window &&
                !present_check_flip(fs->flip_pending->crtc, window, fs->flip_pending->pixmap,
                                    fs->flip_pending->sync_flip, NULL, 0, 0, NULL))
                present_set_abort_flip(screen, fs);
        } else {
            /* Check current flip */
            if (window == fs->flip_window &&
                !present_check_flip(fs->flip_crtc, window, fs->flip_pixmap, fs->flip_sync, NULL, 0, 0, NULL))
                present_unflip(screen, fs);
        }
    }

    /* Now check any queued vblanks */
    xorg_list_for_each_entry(vblank, &window_priv->vblank, window_list) {
        if (vblank->queued && vblank->flip && !present_check_flip(vblank->crtc, window, vblank->pixmap, vblank->sync_flip, NULL, 0, 0, &reason)) {
            vblank->flip = FALSE;
            /* Don't spuriously flag this as a TearFree presentation */
            if (reason < PRESENT_FLIP_REASON_DRIVER_TEARFREE)
                vblank->reason = reason;
            if (vblank->sync_flip)
                vblank->exec_msc = vblank->target_msc;
        }
    }
}

static Bool
present_scmd_can_window_flip(WindowPtr window)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    PixmapPtr                   window_pixmap;
    WindowPtr                   root = screen->root;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    if (!screen_priv)
        return FALSE;

    if (!screen_priv->info)
        return FALSE;

    /* Check to see if the driver supports flips at all */
    if (!screen_priv->info->flip)
        return FALSE;

    /* Make sure the window hasn't been redirected with Composite */
    window_pixmap = screen->GetWindowPixmap(window);
    if (window_pixmap != screen->GetScreenPixmap(screen) &&
        !present_pixmap_is_flip(screen_priv, window_pixmap))
        return FALSE;

    /* Check for full-screen window */
    if (!RegionEqual(&window->clipList, &root->winSize)) {
        return FALSE;
    }

    /* Does the window match the pixmap exactly? */
    if (window->drawable.x != 0 || window->drawable.y != 0) {
        return FALSE;
    }

    return TRUE;
}

/*
 * Clean up any pending or current flips for this window
 */
static void
present_scmd_clear_window_flip(WindowPtr window)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_flip_state_ptr      fs;

    xorg_list_for_each_entry(fs, &screen_priv->flip_states, link) {
        if (fs->flip_pending && fs->flip_pending->window == window) {
            present_set_abort_flip(screen, fs);
            fs->flip_pending->window = NULL;
        }
        if (fs->flip_window == window) {
            present_restore_screen_pixmap(screen, fs);
            fs->flip_window = NULL;
        }
    }
}

/*
 * Once the required MSC has been reached, execute the pending request.
 *
 * For requests to actually present something, either blt contents to
 * the screen or queue a frame buffer swap.
 *
 * For requests to just get the current MSC/UST combo, skip that part and
 * go straight to event delivery
 */

static void
present_execute(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_flip_state_ptr      fs;
    if (vblank && vblank->crtc) {
        screen_priv=present_screen_priv(vblank->crtc->pScreen);
    }
    fs = present_flip_state(screen_priv, vblank->crtc, TRUE);

    /* Without per-CRTC flip state (allocation failure) we cannot track a flip;
     * fall back to a copy for this frame. */
    if (!fs)
        vblank->flip = FALSE;

    if (present_execute_wait(vblank, crtc_msc))
        return;

    if (vblank->flip && vblank->pixmap && vblank->window) {
        if (fs->flip_pending || fs->unflip_event_id) {
            DebugPresent(("\tr %" PRIu64 " %p (pending %p unflip %" PRIu64 ")\n",
                          vblank->event_id, vblank,
                          fs->flip_pending, fs->unflip_event_id));
            xorg_list_del(&vblank->event_queue);
            xorg_list_append(&vblank->event_queue, &present_flip_queue);
            vblank->flip_ready = TRUE;
            return;
        }
    }

    xorg_list_del(&vblank->event_queue);
    xorg_list_del(&vblank->window_list);
    vblank->queued = FALSE;

    if (vblank->pixmap && vblank->window &&
        (vblank->reason < PRESENT_FLIP_REASON_DRIVER_TEARFREE ||
         vblank->exec_msc != vblank->target_msc)) {

        if (vblank->flip) {

            DebugPresent(("\tf %" PRIu64 " %p %" PRIu64 ": %08" PRIx32 " -> %08" PRIx32 "\n",
                          vblank->event_id, vblank, crtc_msc,
                          vblank->pixmap->drawable.id, vblank->window->drawable.id));

            /* Prepare to flip by placing it in the flip queue and
             * and sticking it into the flip_pending field
             */
            fs->flip_pending = vblank;

            xorg_list_add(&vblank->event_queue, &present_flip_queue);
            /* Try to flip
             */
            if (present_flip(vblank->crtc, vblank->event_id, vblank->target_msc, vblank->pixmap, vblank->sync_flip)) {
                RegionPtr damage;

                /* Fix window pixmaps for a whole-screen flip:
                 *  1) Restore previous flip window pixmap
                 *  2) Set current flip window pixmap to the new pixmap
                 *
                 * A per-CRTC flip only redirects one CRTC's scanout; it must
                 * not touch the screen or window-tree pixmaps (which are
                 * whole-screen), or it would misdirect 2D rendering for the
                 * rest of the screen. Its content lives entirely in the flipped
                 * buffer and is refreshed by the client re-presenting.
                 */
                if (!present_flip_is_per_crtc(screen, vblank->pixmap)) {
                    if (fs->flip_window && fs->flip_window != window)
                        present_set_tree_pixmap(fs->flip_window,
                                                fs->flip_pixmap,
                                                (*screen->GetScreenPixmap)(screen));
                    present_set_tree_pixmap(vblank->window, NULL, vblank->pixmap);
                    present_set_tree_pixmap(screen->root, NULL, vblank->pixmap);
                }

                /* Report update region as damaged
                 */
                if (vblank->update) {
                    damage = vblank->update;
                    RegionIntersect(damage, damage, &window->clipList);
                } else
                    damage = &window->clipList;

                DamageDamageRegion(&vblank->window->drawable, damage);
                return;
            }

            xorg_list_del(&vblank->event_queue);
            /* Oops, flip failed. Clear the flip_pending field
              */
            fs->flip_pending = NULL;
            vblank->flip = FALSE;
            vblank->exec_msc = vblank->target_msc;
        }
        DebugPresent(("\tc %p %" PRIu64 ": %08" PRIx32 " -> %08" PRIx32 "\n",
                      vblank, crtc_msc, vblank->pixmap->drawable.id, vblank->window->drawable.id));
        if (fs) {
            if (fs->flip_pending) {

                /* Check pending flip
                 */
                if (window == fs->flip_pending->window)
                    present_set_abort_flip(screen, fs);
            } else if (!fs->unflip_event_id) {

                /* Check current flip
                 */
                if (window == fs->flip_window)
                    present_unflip(screen, fs);
            }
        }

        present_execute_copy(vblank, crtc_msc);

        /* With TearFree, there's no way to tell exactly when the presentation
         * will be visible except by waiting for a notification from the kernel
         * driver indicating that the page flip is complete. This is because the
         * CRTC's MSC can change while the target MSC is calculated and even
         * while the page flip IOCTL is sent to the kernel due to scheduling
         * delays and/or unfortunate timing. Even worse, a page flip isn't
         * actually guaranteed to be finished after one vblank; it may be
         * several MSCs until a flip actually finishes depending on delays and
         * load in hardware.
         *
         * So, to get a notification from the driver with TearFree active, the
         * driver expects a present_flip() call with a NULL pixmap to indicate
         * that this is a fake flip for a pixmap that's already been copied to
         * the primary scanout, which will then be flipped by TearFree. TearFree
         * will then send a notification once the flip containing this pixmap is
         * complete.
         *
         * If the fake flip attempt fails, then fall back to just enqueuing a
         * vblank event targeting the next MSC.
         */
        if (!vblank->queued &&
            vblank->reason >= PRESENT_FLIP_REASON_DRIVER_TEARFREE) {
            uint64_t completion_msc = crtc_msc + 1;

            /* If TearFree is already flipping then the presentation will be
             * visible at the *next* next vblank. This calculation only matters
             * for the vblank event fallback.
             */
            if (vblank->reason == PRESENT_FLIP_REASON_DRIVER_TEARFREE_FLIPPING &&
                vblank->exec_msc < crtc_msc)
                    completion_msc++;

            /* Try the fake flip first and then fall back to a vblank event */
            if (present_flip(vblank->crtc, vblank->event_id, 0, NULL, TRUE) ||
                Success == screen_priv->queue_vblank(screen,
                                                     window,
                                                     vblank->crtc,
                                                     vblank->event_id,
                                                     completion_msc)) {
                /* Ensure present_execute_post() runs at the next execution */
                vblank->exec_msc = vblank->target_msc;
                vblank->queued = TRUE;
            }
        }

        if (vblank->queued) {
            xorg_list_add(&vblank->event_queue, &present_exec_queue);
            xorg_list_append(&vblank->window_list,
                             &present_get_window_priv(window, TRUE)->vblank);
            return;
        }
    }

    present_execute_post(vblank, ust, crtc_msc);
}

static void
present_scmd_update_window_crtc(WindowPtr window, RRCrtcPtr crtc, uint64_t new_msc)
{
    present_window_priv_ptr window_priv = present_get_window_priv(window, TRUE);
    uint64_t                old_ust, old_msc;

    /* Crtc unchanged, no offset. */
    if (crtc == window_priv->crtc)
        return;

    /* No crtc earlier to offset against, just set the crtc. */
    if (window_priv->crtc == PresentCrtcNeverSet) {
        window_priv->crtc = crtc;
        return;
    }

    /* Crtc may have been turned off or be destroyed, just use whatever previous MSC we'd seen from this CRTC. */
    if (!RRCrtcExists(window->drawable.pScreen, window_priv->crtc) ||
        present_get_ust_msc(window->drawable.pScreen, window_priv->crtc, &old_ust, &old_msc) != Success)
        old_msc = window_priv->msc;

    window_priv->msc_offset += new_msc - old_msc;
    window_priv->crtc = crtc;
}

static int
present_scmd_pixmap(WindowPtr window,
                    PixmapPtr pixmap,
                    CARD32 serial,
                    RegionPtr valid,
                    RegionPtr update,
                    int16_t x_off,
                    int16_t y_off,
                    RRCrtcPtr target_crtc,
                    SyncFence *wait_fence,
                    SyncFence *idle_fence,
#ifdef DRI3
                    struct dri3_syncobj *acquire_syncobj,
                    struct dri3_syncobj *release_syncobj,
                    uint64_t acquire_point,
                    uint64_t release_point,
#endif /* DRI3 */
                    uint32_t options,
                    uint64_t target_window_msc,
                    uint64_t divisor,
                    uint64_t remainder,
                    present_notify_ptr notifies,
                    int num_notifies)
{
    uint64_t                    ust = 0;
    uint64_t                    target_msc;
    uint64_t                    crtc_msc = 0;
    int                         ret;
    present_vblank_ptr          vblank, tmp;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_window_priv_ptr     window_priv = present_get_window_priv(window, TRUE);
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

#ifdef DRI3
    if (acquire_syncobj || release_syncobj)
        return BadValue;
#endif /* DRI3 */

    if (!window_priv)
        return BadAlloc;

    if (!screen_priv || !screen_priv->info)
        target_crtc = NULL;
    else if (!target_crtc) {
        /* Update the CRTC if we have a pixmap or we don't have a CRTC
         */
        if (!pixmap)
            target_crtc = window_priv->crtc;

        if (!target_crtc || target_crtc == PresentCrtcNeverSet)
            target_crtc = present_get_crtc(window);
    }

    ret = present_get_ust_msc(screen, target_crtc, &ust, &crtc_msc);

    present_scmd_update_window_crtc(window, target_crtc, crtc_msc);

    if (ret == Success) {
        /* Stash the current MSC away in case we need it later
         */
        window_priv->msc = crtc_msc;
    }

    target_msc = present_get_target_msc(target_window_msc + window_priv->msc_offset,
                                        crtc_msc,
                                        divisor,
                                        remainder,
                                        options);

    /*
     * Look for a matching presentation already on the list and
     * don't bother doing the previous one if this one will overwrite it
     * in the same frame
     */

    if (!update && pixmap) {
        xorg_list_for_each_entry_safe(vblank, tmp, &window_priv->vblank, window_list) {

            if (!vblank->pixmap)
                continue;

            if (!vblank->queued)
                continue;

            if (vblank->crtc != target_crtc || vblank->target_msc != target_msc)
                continue;

            /* Too late to abort now if TearFree execution already happened */
            if (vblank->reason >= PRESENT_FLIP_REASON_DRIVER_TEARFREE &&
                vblank->exec_msc == vblank->target_msc)
                continue;

            present_vblank_scrap(vblank);
            if (vblank->flip_ready)
                present_re_execute(vblank);
        }
    }

    vblank = present_vblank_create(window,
                                   pixmap,
                                   serial,
                                   valid,
                                   update,
                                   x_off,
                                   y_off,
                                   target_crtc,
                                   wait_fence,
                                   idle_fence,
#ifdef DRI3
                                   acquire_syncobj,
                                   release_syncobj,
                                   acquire_point,
                                   release_point,
#endif /* DRI3 */
                                   options,
                                   screen_priv->info ? screen_priv->info->capabilities : 0,
                                   notifies,
                                   num_notifies,
                                   target_msc,
                                   crtc_msc);

    if (!vblank)
        return BadAlloc;

    vblank->event_id = ++present_scmd_event_id;

    /* The soonest presentation is crtc_msc+2 if TearFree is already flipping */
    if (vblank->reason == PRESENT_FLIP_REASON_DRIVER_TEARFREE_FLIPPING &&
        !msc_is_after(vblank->exec_msc, crtc_msc + 1))
        vblank->exec_msc -= 2;
    else if (vblank->reason >= PRESENT_FLIP_REASON_DRIVER_TEARFREE ||
             (vblank->flip && vblank->sync_flip))
        vblank->exec_msc--;

    xorg_list_append(&vblank->event_queue, &present_exec_queue);
    vblank->queued = TRUE;
    if (msc_is_after(vblank->exec_msc, crtc_msc)) {
        ret = present_queue_vblank(screen, window, target_crtc, vblank->event_id, vblank->exec_msc);
        if (ret == Success)
            return Success;

        DebugPresent(("present_queue_vblank failed\n"));
    }

    present_execute(vblank, ust, crtc_msc);

    return Success;
}

static void
present_scmd_abort_vblank(ScreenPtr screen, WindowPtr window, RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    present_vblank_ptr  vblank;

    if (crtc == NULL)
        present_fake_abort_vblank(screen, event_id, msc);
    else
    {
        present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

        (*screen_priv->info->abort_vblank) (crtc, event_id, msc);
    }

    xorg_list_for_each_entry(vblank, &present_exec_queue, event_queue) {
        int64_t match = event_id - vblank->event_id;
        if (match == 0) {
            xorg_list_del(&vblank->event_queue);
            vblank->queued = FALSE;
            return;
        }
    }
    xorg_list_for_each_entry(vblank, &present_flip_queue, event_queue) {
        if (vblank->event_id == event_id) {
            xorg_list_del(&vblank->event_queue);
            vblank->queued = FALSE;
            return;
        }
    }
}

static void
present_scmd_flip_destroy(ScreenPtr screen)
{
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_flip_state_ptr      fs, tmp;

    xorg_list_for_each_entry_safe(fs, tmp, &screen_priv->flip_states, link) {
        /* Reset window pixmaps back to the screen pixmap */
        if (fs->flip_pending)
            present_set_abort_flip(screen, fs);

        /* Drop reference to any pending flip or unflip pixmaps. */
        present_flip_idle(fs);

        xorg_list_del(&fs->link);
        free(fs);
    }
}

void
present_scmd_init_mode_hooks(present_screen_priv_ptr screen_priv)
{
    screen_priv->query_capabilities =   &present_scmd_query_capabilities;
    screen_priv->get_crtc           =   &present_scmd_get_crtc;

    screen_priv->check_flip         =   &present_check_flip;
    screen_priv->check_flip_window  =   &present_check_flip_window;
    screen_priv->can_window_flip    =   &present_scmd_can_window_flip;
    screen_priv->clear_window_flip  =   &present_scmd_clear_window_flip;

    screen_priv->present_pixmap     =   &present_scmd_pixmap;

    screen_priv->queue_vblank       =   &present_queue_vblank;
    screen_priv->flush              =   &present_flush;
    screen_priv->re_execute         =   &present_re_execute;

    screen_priv->abort_vblank       =   &present_scmd_abort_vblank;
    screen_priv->flip_destroy       =   &present_scmd_flip_destroy;
}

Bool
present_init(void)
{
    xorg_list_init(&present_exec_queue);
    xorg_list_init(&present_flip_queue);
    present_fake_queue_init();
    return TRUE;
}
