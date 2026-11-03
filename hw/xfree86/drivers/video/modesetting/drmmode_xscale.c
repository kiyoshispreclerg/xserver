/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE scanout functions of modesetting's own, for the CRTCs it
 * renders through their transform itself (doc/x-scale.md).
 */
#include <dix-config.h>

#include "xf86.h"
#include "xf86Crtc.h"
#include "damage.h"
#ifdef CONFIG_XSCALE
#include "include/xscale.h"
#endif

#include "driver.h"

#ifdef CONFIG_XSCALE
/*
 * X-SCALE (Xext/xscale/xscale.c, doc/x-scale.md) for the CRTCs this driver
 * renders through their transform itself (drmmode_crtc_rotate()): their
 * source is copied straight into the TearFree back buffer, which is then
 * flipped, instead of into a shadow first. The source's damage reaches the
 * TearFree buffers as screen damage (xf86CrtcDamageFromScanoutSource(),
 * from ms_tearfree_update_damages()), and drmmode_copy_damage() copies the
 * source for it. Every other CRTC is left to the xf86 modes layer.
 */

static const XScaleScanoutFuncs *
drmmode_xscale_lower(RRCrtcPtr randr_crtc)
{
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(randr_crtc->pScreen));

    return ms->drmmode.xscale_lower_funcs;
}

static Bool
drmmode_xscale_capable(RRCrtcPtr randr_crtc, int *width, int *height)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;
    const XScaleScanoutFuncs *lower = drmmode_xscale_lower(randr_crtc);

    if (crtc && drmmode_crtc_renders_transform(crtc)) {
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        if (crtc->rotation != RR_Rotate_0 ||
            (!drmmode_crtc->tearfree.buf[0].px && !drmmode_crtc->tearfree.parked))
            return FALSE;
        *width = crtc->mode.HDisplay;
        *height = crtc->mode.VDisplay;
        return TRUE;
    }
    return lower->capable && lower->capable(randr_crtc, width, height);
}

#ifdef GLAMOR
/*
 * Presented X-SCALE pixmaps are flipped straight onto a CRTC this
 * driver renders through its transform, as a per-CRTC flip of a buffer
 * of the CRTC's own size (ms_do_pageflip_crtc()): no copy at all, and the
 * TearFree buffers sit idle meanwhile (ms_tearfree_do_flips() skips a
 * CRTC whose scanout a per-CRTC flip owns). Going back is a TearFree flip
 * of a back buffer refilled in full (drmmode_xscale_unflip()), so the
 * CRTC never shows a stale TearFree buffer. The same conditions as for
 * Present's per-CRTC flips apply.
 */

typedef struct {
    xf86CrtcPtr crtc;
} drmmode_xscale_flip_rec;

static Bool
drmmode_xscale_can_flip_crtc(xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return drmmode_crtc_renders_transform(crtc) &&
           ms_tearfree_is_active_on_crtc(crtc) &&
           ms->drmmode.pageflip && ms->drmmode.glamor &&
           ms->atomic_modeset_capable &&
           !ms->drmmode.per_crtc_flip_failed &&
           drmmode_crtc_per_crtc_flip_wanted(crtc) &&
           !ms->drmmode.pending_modeset &&
           !ms->drmmode.dri2_flipping && !ms->drmmode.present_flipping &&
           !drmmode_crtc->xscale_flip_pending &&
           !drmmode_crtc->tearfree.flip_seq;
}

/* Back from a flipped presented pixmap to the TearFree buffers, refilled
 * in full: from the source, or from the root when it is gone. */
static Bool
drmmode_xscale_unflip_crtc(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_tearfree_ptr trf = &drmmode_crtc->tearfree;
    uint32_t old_fb = drmmode_crtc->present_flip_fb_id;
    RegionRec all;
    int i;

    if (!drmmode_crtc->xscale_flipped || drmmode_crtc->xscale_flip_pending ||
        !ms_tearfree_is_active_on_crtc(crtc) || trf->flip_seq ||
        !drmmode_tearfree_unpark(crtc))
        return FALSE;

    /* Nothing reached the TearFree buffers while the pixmap was shown */
    RegionInit(&all, &crtc->bounds, 1);
    for (i = 0; i < ARRAY_SIZE(trf->buf); i++)
        RegionUnion(&trf->buf[i].dmg, &trf->buf[i].dmg, &all);
    RegionUninit(&all);

    drmmode_crtc->present_flip_fb_id = 0;
    if (ms_do_tearfree_flip(xf86ScrnToScreen(crtc->scrn), crtc)) {
        /* Not queued: the CRTC still shows the pixmap */
        drmmode_crtc->present_flip_fb_id = old_fb;
        return FALSE;
    }
    trf->release_fb_id = old_fb;
    trf->xscale_leaving = TRUE;
    drmmode_crtc->xscale_flipped = FALSE;
    return TRUE;
}

static void
drmmode_xscale_flip_handler(modesettingPtr ms, uint64_t msc, uint64_t usec,
                            void *data)
{
    drmmode_xscale_flip_rec *flip = data;
    xf86CrtcPtr crtc = flip->crtc;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    free(flip);
    drmmode_crtc->xscale_flip_pending = FALSE;

    /* The source went away while the flip was pending */
    if (!XScaleCrtcHasSource(crtc->randr_crtc)) {
        drmmode_xscale_unflip_crtc(crtc);
        return;
    }
    XScaleFlipDone(crtc->randr_crtc, TRUE, msc, usec);
}

static void
drmmode_xscale_flip_abort(modesettingPtr ms, void *data)
{
    drmmode_xscale_flip_rec *flip = data;
    xf86CrtcPtr crtc = flip->crtc;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    free(flip);
    drmmode_crtc->xscale_flip_pending = FALSE;
    XScaleFlipDone(crtc->randr_crtc, FALSE, 0, 0);
}

static Bool
drmmode_xscale_can_flip(RRCrtcPtr randr_crtc)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;

    return crtc && drmmode_xscale_can_flip_crtc(crtc);
}

static Bool
drmmode_xscale_flip(RRCrtcPtr randr_crtc, PixmapPtr pixmap)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;
    ScreenPtr screen = randr_crtc->pScreen;
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));
    drmmode_crtc_private_ptr drmmode_crtc;
    drmmode_xscale_flip_rec *flip;
    struct gbm_bo *bo;
    uint32_t format;
    uint64_t modifier;

    if (!crtc || !drmmode_xscale_can_flip_crtc(crtc))
        return FALSE;
    drmmode_crtc = crtc->driver_private;

    /* A buffer the display engine can read, as for Present's flips */
    bo = ms->glamor.gbm_bo_from_pixmap(screen, pixmap);
    if (!bo)
        return FALSE;
    format = gbm_bo_get_format(bo);
    modifier = gbm_bo_get_modifier(bo);
    gbm_bo_destroy(bo);
    if (!drmmode_is_format_supported(crtc->scrn, format, modifier, FALSE))
        return FALSE;

    flip = calloc(1, sizeof(*flip));
    if (!flip)
        return FALSE;
    flip->crtc = crtc;

    /* Frees `flip` itself when it fails */
    if (!ms_do_pageflip_crtc(screen, pixmap, flip, crtc, FALSE,
                             drmmode_xscale_flip_handler,
                             drmmode_xscale_flip_abort, "X-SCALE-flip"))
        return FALSE;

    drmmode_crtc->xscale_flipped = TRUE;
    drmmode_crtc->xscale_flip_pending = TRUE;
    return TRUE;
}

static Bool
drmmode_xscale_unflip(RRCrtcPtr randr_crtc)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;

    return crtc && drmmode_xscale_unflip_crtc(crtc);
}
#endif /* GLAMOR */

/* A dropped source means rendering the whole root through the transform
 * into the TearFree buffers again. The modes layer still re-decides the
 * cursor, either way. */
static void
drmmode_xscale_source_changed(RRCrtcPtr randr_crtc)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;
    const XScaleScanoutFuncs *lower = drmmode_xscale_lower(randr_crtc);

    if (crtc && drmmode_crtc_renders_transform(crtc) &&
        !XScaleCrtcHasSource(randr_crtc)) {
        ScreenPtr pScreen = randr_crtc->pScreen;
        RegionRec region;

        RegionInit(&region, &crtc->bounds, 1);
        DamageDamageRegion(&pScreen->GetScreenPixmap(pScreen)->drawable,
                           &region);
        RegionUninit(&region);

#ifdef GLAMOR
        /* Off a presented pixmap still flipped there; a pending flip does
         * this when it lands (drmmode_xscale_flip_handler()). */
        drmmode_xscale_unflip_crtc(crtc);
#endif
    }

    if (lower->source_changed)
        lower->source_changed(randr_crtc);
}

/* What such a CRTC scans out is its TearFree front buffer. */
static PixmapPtr
drmmode_xscale_scanout_pixmap(RRCrtcPtr randr_crtc)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;
    const XScaleScanoutFuncs *lower = drmmode_xscale_lower(randr_crtc);

    if (crtc && drmmode_crtc_renders_transform(crtc)) {
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
        drmmode_tearfree_ptr trf = &drmmode_crtc->tearfree;

        if (crtc->rotation != RR_Rotate_0)
            return NULL;
        return trf->buf[trf->back_idx ^ 1].px;
    }
    return lower->scanout_pixmap ? lower->scanout_pixmap(randr_crtc) : NULL;
}

static const XScaleScanoutFuncs drmmode_xscale_funcs = {
    .capable = drmmode_xscale_capable,
    .source_changed = drmmode_xscale_source_changed,
    .scanout_pixmap = drmmode_xscale_scanout_pixmap,
#ifdef GLAMOR
    .can_flip = drmmode_xscale_can_flip,
    .flip = drmmode_xscale_flip,
    .unflip = drmmode_xscale_unflip,
#endif
};
#endif /* CONFIG_XSCALE */

/*
 * drmmode_xscale_init - take over X-SCALE for the CRTCs rendered here.
 * @pScreen: the screen, after xf86CrtcScreenInit().
 *
 * Only where the modes layer offers X-SCALE at all, and TearFree can run.
 */
void
drmmode_xscale_init(ScreenPtr pScreen)
{
#ifdef CONFIG_XSCALE
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(pScreen));
    const XScaleScanoutFuncs *lower = XScaleGetScanoutFuncs(pScreen);

    if (!lower || !ms->drmmode.tearfree_possible)
        return;
    ms->drmmode.xscale_lower_funcs = lower;
    XScaleSetScanoutFuncs(pScreen, &drmmode_xscale_funcs);
#endif
}
