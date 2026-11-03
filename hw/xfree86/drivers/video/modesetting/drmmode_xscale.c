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

        if (crtc->rotation != RR_Rotate_0 || !drmmode_crtc->tearfree.buf[0].px)
            return FALSE;
        *width = crtc->mode.HDisplay;
        *height = crtc->mode.VDisplay;
        return TRUE;
    }
    return lower->capable && lower->capable(randr_crtc, width, height);
}

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
