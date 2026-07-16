/* SPDX-License-Identifier: MIT OR X11
 *
 * X-INPUT-SCALE extension - DIX-facing helpers.
 *
 * These are strict no-ops unless a compositor has registered at least one
 * CRTC confinement box, so they are safe to call unconditionally from core
 * code that is always built (guard the *call site* with #ifdef
 * CONFIG_INPUT_SCALE instead, since this header - and the extension itself -
 * may not be compiled in at all).
 */
#ifndef _XSERVER_INPUTSCALE_H
#define _XSERVER_INPUTSCALE_H

#include <X11/Xdefs.h>          /* Bool */
#include "screenint.h"          /* ScreenPtr */
#include "randrstr.h"           /* RRCrtcPtr */

/*
 * Fast-path predicate: TRUE iff at least one CRTC currently has an active
 * confinement box.
 */
extern _X_EXPORT Bool XInputScaleActive(void);

/*
 * Map a *logical* desktop-space point (the pointer's normal, unremapped
 * position - the same one used for hit-testing/reporting) to the
 * corresponding *physical* scanout-space point, using whichever CRTC's
 * confinement box currently contains it. For hardware cursor placement only:
 * a HW cursor plane/sprite is positioned by the driver in physical scanout
 * coordinates, which differ from the logical coordinate range while a CRTC
 * is confined (see Xext/inputscale/inputscale.c). Leaves x/y untouched if
 * inactive or if the point falls outside every confined CRTC's box.
 *
 * hotx/hoty are the *unscaled* cursor hotspot (CursorPtr->bits->xhot/yhot).
 * They correct for the hardware cursor bitmap being scaled up by the same
 * factor for this CRTC (see XInputScaleGetCrtcScale() below): the hardware
 * ultimately positions the sprite's top-left corner at
 * `x_param - hotx_unscaled`, but the *visually correct* top-left corner is
 * `desired_tip - hotx_unscaled*scale`. Passing the real hotspot here lets
 * this function emit the one x_param value that makes both agree; pass
 * (0, 0) if the caller never scales the bitmap (the correction term is then
 * simply zero).
 */
extern _X_EXPORT void XInputScaleLogicalToPhysicalCursor(ScreenPtr pScreen,
                                                          int hotx, int hoty,
                                                          int *x, int *y);

/*
 * TRUE and fills sx/sy (via the out-pointers) with the physical-scanout-size
 * to confine-box-size ratio for this CRTC iff it currently has an active
 * confinement box; FALSE (leaving sx/sy untouched) otherwise. For hardware
 * cursor bitmap scaling: a driver can multiply a cursor glyph's dimensions
 * by this factor before uploading it to a confined CRTC's cursor plane, so
 * the on-screen cursor size matches the panel's physical pixel density
 * instead of looking undersized relative to it. Independent of, but meant
 * to be used with the same factor as, XInputScaleLogicalToPhysicalCursor()'s
 * position mapping.
 */
extern _X_EXPORT Bool XInputScaleGetCrtcScale(RRCrtcPtr crtc, double *sx, double *sy);

#endif /* _XSERVER_INPUTSCALE_H */
