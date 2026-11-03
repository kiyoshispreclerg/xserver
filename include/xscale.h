/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE extension - DIX/DDX interface.
 *
 * A CRTC scaled by a RandR transform (xrandr --scale-from) scans out a
 * shadow buffer the DDX normally fills by magnifying the root. X-SCALE lets
 * a compositor give the CRTC a *scanout source* instead: a drawable already
 * at the CRTC's physical resolution, which the DDX copies into that shadow
 * (or straight into its TearFree buffers, when it does the transform
 * itself) in place of the magnified root (Xext/xscale/xscale.c,
 * doc/x-scale.md).
 *
 * Every helper here is a strict no-op for a CRTC without a source, so they
 * are safe to call unconditionally from code that is always built. Guard
 * the *call site* with #ifdef CONFIG_XSCALE, since this header - and the
 * extension itself - may not be compiled in at all.
 */
#ifndef _XSERVER_XSCALE_H
#define _XSERVER_XSCALE_H

#include <stdint.h>

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

    /*
     * scanout_pixmap - the buffer the CRTC scans out right now.
     * @crtc: the CRTC, which has a mode and a scale-only transform.
     *
     * The pixmap holding exactly what goes to the monitor, at its
     * physical resolution and in root orientation, or NULL when the CRTC
     * has no such buffer (the DDX scans out of the root, or keeps its
     * buffers to itself). XScaleCopyCrtcScanout copies from it, and
     * renders the root through the CRTC transform when it is NULL. A DDX
     * that later flips sources straight onto the CRTC, or frees idle
     * shadows, only changes what this returns. Optional.
     */
    PixmapPtr (*scanout_pixmap)(RRCrtcPtr crtc);

    /*
     * Presented pixmaps (XScalePresentCrtcScanout). All optional:
     * without flip, every presented pixmap is copied like any source.
     */

    /*
     * can_flip - would flip() be tried on this CRTC now?
     * @crtc: a capable CRTC.
     *
     * Only a hint for clients (XScaleScanoutFlippable).
     */
    Bool (*can_flip)(RRCrtcPtr crtc);

    /*
     * flip - scan `pixmap` out directly from the next vblank on.
     * @crtc:   a capable CRTC, with no flip of this DDX pending on it.
     * @pixmap: the CRTC's source, of the scanout's size and the root's
     *          depth.
     *
     * TRUE when the flip is queued: the DDX then calls XScaleFlipDone()
     * exactly once. FALSE when it can't be flipped now, and nothing
     * changed: the server copies the pixmap instead.
     */
    Bool (*flip)(RRCrtcPtr crtc, PixmapPtr pixmap);

    /*
     * unflip - go back from a flipped pixmap to the DDX's own buffer.
     * @crtc: a CRTC showing a pixmap flip() put there, with no flip
     *        pending.
     *
     * The DDX fills its buffer from the source in full (or from the root,
     * when the source is gone) and shows it from the next vblank on. TRUE
     * when that is queued: the DDX then calls XScaleScanoutLeft() once
     * it is shown. FALSE when it can't be done now; the CRTC keeps
     * showing the flipped pixmap.
     */
    Bool (*unflip)(RRCrtcPtr crtc);
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
 * XScaleGetScanoutFuncs - what a screen's DDX registered so far.
 * @pScreen: the screen.
 *
 * NULL when nothing is. A driver that handles some CRTCs itself and leaves
 * the rest to a lower layer (the xf86 modes layer) registers its own funcs
 * and calls these for the CRTCs it doesn't handle.
 */
extern _X_EXPORT const XScaleScanoutFuncs *
XScaleGetScanoutFuncs(ScreenPtr pScreen);

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
 * XScaleCopyScanoutSource - copy part of a CRTC's source into a buffer.
 * @crtc:   the CRTC.
 * @dst:    a drawable of the scanout's size, on the CRTC's screen.
 * @region: the part to copy, in scanout coordinates.
 *
 * Copies 1:1, on the GPU when the screen is accelerated. Doesn't touch
 * the accumulated damage. TRUE iff the CRTC has a source and the copy was
 * issued.
 */
extern _X_EXPORT Bool XScaleCopyScanoutSource(RRCrtcPtr crtc, DrawablePtr dst,
                                              RegionPtr region);

/*
 * XScaleFlipDone - a flip() landed, or was aborted.
 * @crtc:   the CRTC.
 * @landed: TRUE when the pixmap is now scanned out; FALSE when the flip
 *          was aborted and the CRTC still shows what it showed.
 * @msc:    the vblank it landed on, and when that started (microseconds,
 * @ust:    CLOCK_MONOTONIC); 0 when unknown.
 */
extern _X_EXPORT void XScaleFlipDone(RRCrtcPtr crtc, Bool landed,
                                     uint64_t msc, uint64_t ust);

/*
 * XScaleScanoutLeft - the CRTC no longer shows a pixmap flip() put there.
 * @crtc: the CRTC.
 * @msc:  as for XScaleFlipDone; 0 when unknown.
 * @ust:
 *
 * Called when an unflip() is shown, and whenever the DDX left the
 * flipped pixmap by itself (a modeset, a VT switch).
 */
extern _X_EXPORT void XScaleScanoutLeft(RRCrtcPtr crtc, uint64_t msc,
                                        uint64_t ust);

/*
 * XScaleScanoutCopyToken / XScaleScanoutCopied - completing copied
 * presentations at the vblank that shows them.
 * @crtc:  the CRTC.
 * @token: what XScaleScanoutCopyToken() returned when the DDX copied the
 *         source into the buffer it is about to show.
 * @msc:   when that buffer is shown; 0 when unknown.
 * @ust:
 *
 * Every presentation copied before the token was taken completes.
 * Tokens are never 0; 0 means nothing to complete.
 */
extern _X_EXPORT uint64_t XScaleScanoutCopyToken(RRCrtcPtr crtc);
extern _X_EXPORT void XScaleScanoutCopied(RRCrtcPtr crtc, uint64_t token,
                                          uint64_t msc, uint64_t ust);

/*
 * XScaleGetCrtcScale - the hardware cursor's scale on a CRTC with a source.
 * @crtc: the CRTC.
 * @sx:   out, the CRTC's physical/logical ratio (> 1 when the output is
 * @sy:   magnified), only written when returning TRUE.
 *
 * TRUE iff the CRTC has a source and a scale-only transform. On such a
 * CRTC the hardware cursor is what shows the pointer - the software
 * cursor is drawn into the root, which the CRTC no longer shows - so the
 * cursor code allows it there and scales its position and bitmap by this
 * (hw/xfree86/modes/xf86Cursors.c, and the modesetting DDX's crop).
 */
extern _X_EXPORT Bool XScaleGetCrtcScale(RRCrtcPtr crtc, double *sx, double *sy);

/*
 * XScaleCrtcChanged - a CRTC's configuration was just updated.
 * @crtc: the CRTC.
 *
 * Called by RandR (RRCrtcNotify). Drops a source the CRTC can't take any
 * more; keeps one it still can, to be copied again whole.
 */
extern _X_EXPORT void XScaleCrtcChanged(RRCrtcPtr crtc);

#endif /* _XSERVER_XSCALE_H */
