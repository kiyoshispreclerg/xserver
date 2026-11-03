/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE scanout sources on the CRTCs the xf86 modes layer transforms
 * (doc/x-scale.md): xf86Rotate.c calls in here.
 */
#include <dix-config.h>

#include "xf86.h"
#include "xf86Crtc.h"
#include "damage.h"
#ifdef CONFIG_XSCALE
#include "include/xscale.h"
#include "xf86Cursor.h"
#endif

#include "xf86XScale_priv.h"

#ifdef CONFIG_XSCALE
/*
 * X-SCALE scanout sources (Xext/xscale/xscale.c, doc/x-scale.md).
 *
 * A transformed CRTC scans out its shadow, which xf86RotateRedisplay()
 * normally fills by magnifying the root through the CRTC transform. When a
 * compositor gave the CRTC a source - a drawable already at the CRTC's
 * physical resolution - the shadow is filled from that instead, 1:1, with
 * only what was drawn into it since the last pass, and the magnification
 * is skipped for that CRTC.
 *
 * Whatever reads the shadow next (the modesetting DDX's TearFree buffers,
 * its dirty-fb notification) learns about the change the way it always
 * does: through damage on the screen pixmap over the CRTC's logical
 * bounds, the same signal the magnification path relies on. So no DDX
 * needs changes of its own.
 */

/* Damages the screen pixmap over the logical part of the root that the
 * scanout pixels in `scanout` show. */
static void
xf86CrtcDamageFromScanout(xf86CrtcPtr crtc, RegionPtr scanout)
{
    ScreenPtr pScreen = xf86ScrnToScreen(crtc->scrn);
    RegionRec fb_damage;
    int n = RegionNumRects(scanout);
    BoxPtr b = RegionRects(scanout);

    RegionNull(&fb_damage);
    for (; n--; b++) {
        BoxRec fb_box = *b;
        RegionRec r;

        pixman_f_transform_bounds(&crtc->f_crtc_to_framebuffer, &fb_box);
        RegionInit(&r, &fb_box, 1);
        RegionUnion(&fb_damage, &fb_damage, &r);
        RegionUninit(&r);
    }
    DamageDamageRegion(&(*pScreen->GetScreenPixmap) (pScreen)->drawable,
                       &fb_damage);
    RegionUninit(&fb_damage);
}

/*
 * xf86RotateScanoutRedisplay - copy a CRTC's X-SCALE source into its shadow.
 * @pScreen: the screen.
 * @crtc:    the CRTC; nothing happens unless it is enabled, has a shadow
 *           pixmap and an X-SCALE source.
 *
 * Copies the source's damage since the last pass (everything, for a shadow
 * that was just created), then damages the screen pixmap over the logical
 * area it shows.
 */
void
xf86RotateScanoutRedisplay(ScreenPtr pScreen, xf86CrtcPtr crtc)
{
    RRCrtcPtr randr_crtc = crtc->randr_crtc;
    DrawablePtr source;
    int src_x, src_y;
    RegionRec damage;

    if (!crtc->enabled || !crtc->rotatedPixmap ||
        !XScaleCrtcHasSource(randr_crtc))
        return;

    RegionNull(&damage);
    if (!XScaleTakeScanoutDamage(randr_crtc, &source, &src_x, &src_y, &damage))
        goto out;

    /* A shadow that was just (re)created wants all of it, which the root
     * magnification pass would have done had it not been skipped. */
    if (crtc->shadowClear) {
        BoxRec all = { 0, 0, crtc->mode.HDisplay, crtc->mode.VDisplay };

        RegionReset(&damage, &all);
        crtc->shadowClear = FALSE;
    }
    if (!RegionNotEmpty(&damage))
        goto out;

    if (XScaleCopyScanoutSource(randr_crtc, &crtc->rotatedPixmap->drawable,
                                &damage))
        xf86CrtcDamageFromScanout(crtc, &damage);
 out:
    RegionUninit(&damage);
}

/*
 * xf86ScanoutCapable - XScaleScanoutFuncs.capable for the xf86 modes layer.
 *
 * Only a transform this layer handles itself, through a shadow of its own:
 * a DDX doing the transform on its own scanout buffers (amdgpu with
 * TearFree, for one) has nothing this could fill.
 */
static Bool
xf86ScanoutCapable(RRCrtcPtr randr_crtc, int *width, int *height)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;

    if (!crtc || !crtc->enabled || !crtc->transform_in_use ||
        crtc->rotation != RR_Rotate_0 ||
        (crtc->driverIsPerformingTransform & XF86DriverTransformOutput) ||
        (!crtc->rotatedData && !crtc->rotatedPixmap))
        return FALSE;

    *width = crtc->mode.HDisplay;
    *height = crtc->mode.VDisplay;
    return TRUE;
}

/*
 * xf86ScanoutSourceChanged - XScaleScanoutFuncs.source_changed for the xf86
 * modes layer.
 *
 * A dropped source means magnifying the whole root into the shadow again.
 * Either way the cursor is re-decided: the software cursor, drawn into the
 * root, doesn't show on a CRTC with a source.
 */
static void
xf86ScanoutSourceChanged(RRCrtcPtr randr_crtc)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;

    if (!crtc)
        return;

    if (!XScaleCrtcHasSource(randr_crtc) && crtc->rotatedPixmap)
        xf86CrtcDamageShadow(crtc);

    xf86CursorResetCursor(randr_crtc->pScreen);
}

/*
 * xf86ScanoutPixmap - XScaleScanoutFuncs.scanout_pixmap for the xf86 modes
 * layer.
 *
 * The shadow is exactly what a transformed CRTC scans out, sharp when an
 * X-SCALE source fills it, magnified from the root otherwise, so screen
 * recorders can copy it as is. Only an unrotated CRTC's: a rotated shadow
 * holds scanout-oriented pixels. NULL when this layer doesn't own the
 * CRTC's scanout buffer.
 */
static PixmapPtr
xf86ScanoutPixmap(RRCrtcPtr randr_crtc)
{
    xf86CrtcPtr crtc = randr_crtc->devPrivate;

    if (!crtc || !crtc->enabled || !crtc->transform_in_use ||
        crtc->rotation != RR_Rotate_0 ||
        (crtc->driverIsPerformingTransform & XF86DriverTransformOutput))
        return NULL;
    return crtc->rotatedPixmap;
}

static const XScaleScanoutFuncs xf86_scanout_funcs = {
    .capable = xf86ScanoutCapable,
    .source_changed = xf86ScanoutSourceChanged,
    .scanout_pixmap = xf86ScanoutPixmap,
};

void
xf86RotateScanoutInit(ScreenPtr pScreen)
{
    XScaleSetScanoutFuncs(pScreen, &xf86_scanout_funcs);
}
#endif /* CONFIG_XSCALE */
