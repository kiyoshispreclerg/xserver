/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE extension - DIX/DDX interface.
 *
 * A CRTC scaled by a RandR transform (xrandr --scale-from) scans out a
 * shadow buffer the DDX normally fills by magnifying the root. X-SCALE lets
 * a compositor give the CRTC a *scanout source* instead: a drawable already
 * at the CRTC's physical resolution, which the DDX copies into that shadow
 * in place of the magnified root (Xext/xscale/xscale.c, doc/x-scale.md).
 *
 * Every helper here is a strict no-op for a CRTC without a source, so they
 * are safe to call unconditionally from code that is always built. Guard
 * the *call site* with #ifdef CONFIG_XSCALE, since this header - and the
 * extension itself - may not be compiled in at all.
 */
#ifndef _XSERVER_XSCALE_H
#define _XSERVER_XSCALE_H

#include <X11/Xdefs.h>          /* Bool */
#include "screenint.h"          /* ScreenPtr */
#include "regionstr.h"          /* RegionPtr */
#include "randrstr.h"           /* RRCrtcPtr */

/*
 * What a DDX that can take scanout sources provides, per screen. The xf86
 * modes layer registers these for every xf86 DDX that lets it handle CRTC
 * transforms (hw/xfree86/modes/xf86XScale.c). A DDX that registers nothing
 * never has a capable CRTC, and compositors leave it to magnify the root.
 */
typedef struct _XScaleScanoutFuncs {
    /*
     * capable - can this CRTC be fed from a source right now?
     * @crtc:   the CRTC, which has a mode and a scale-only transform.
     * @width:  out, the size of the buffer the CRTC scans out (its
     * @height: physical resolution), only written when returning TRUE.
     *
     * TRUE iff the CRTC currently scans out of a buffer of its own that
     * the DDX can fill from a source.
     */
    Bool (*capable)(RRCrtcPtr crtc, int *width, int *height);

    /*
     * source_changed - the CRTC's source was just set, replaced or dropped.
     * @crtc: the CRTC; XScaleCrtcHasSource() says which.
     *
     * When the source is dropped, the DDX goes back to filling the buffer
     * itself, from scratch.
     */
    void (*source_changed)(RRCrtcPtr crtc);
} XScaleScanoutFuncs;

/*
 * XScaleSetScanoutFuncs - register what a screen's DDX offers.
 * @pScreen: the screen; GPU screens (PRIME secondaries) are ignored.
 * @funcs:   the DDX's functions, which must outlive the screen, or NULL.
 *
 * Called from the DDX's screen init.
 */
extern _X_EXPORT void XScaleSetScanoutFuncs(ScreenPtr pScreen,
                                            const XScaleScanoutFuncs *funcs);

/*
 * XScaleCrtcHasSource - is a client's source feeding this CRTC?
 * @crtc: the CRTC, or NULL (FALSE).
 */
extern _X_EXPORT Bool XScaleCrtcHasSource(RRCrtcPtr crtc);

/*
 * XScaleTakeScanoutDamage - what the DDX's redisplay should copy.
 * @crtc:   the CRTC.
 * @source: out, the source drawable.
 * @src_x:  out, where in the source the scanout starts.
 * @src_y:
 * @damage: an initialized region, overwritten with the part of the
 *          scanout to refresh, in scanout coordinates: everything drawn
 *          into the source since the last call, or the whole scanout right
 *          after the source was set or the CRTC reconfigured.
 *
 * TRUE iff the CRTC has a source. The accumulated damage is consumed.
 */
extern _X_EXPORT Bool XScaleTakeScanoutDamage(RRCrtcPtr crtc,
                                              DrawablePtr *source,
                                              int *src_x, int *src_y,
                                              RegionPtr damage);

/*
 * XScaleCrtcChanged - a CRTC's configuration was just updated.
 * @crtc: the CRTC.
 *
 * Called by RandR (RRCrtcNotify). Drops a source the CRTC can't take any
 * more; keeps one it still can, to be copied again whole.
 */
extern _X_EXPORT void XScaleCrtcChanged(RRCrtcPtr crtc);

#endif /* _XSERVER_XSCALE_H */
