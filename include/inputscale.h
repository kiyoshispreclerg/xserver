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

/*
 * Fast-path predicate: TRUE iff at least one CRTC currently has an active
 * confinement box.
 */
extern Bool XInputScaleActive(void);

/*
 * Map a *logical* desktop-space point (the pointer's normal, unremapped
 * position - the same one used for hit-testing/reporting) to the
 * corresponding *physical* scanout-space point, using whichever CRTC's
 * confinement box currently contains it. For hardware cursor placement only:
 * a HW cursor plane/sprite is positioned by the driver in physical scanout
 * coordinates, which differ from the logical coordinate range while a CRTC
 * is confined (see Xext/inputscale/inputscale.c). Leaves x/y untouched if
 * inactive or if the point falls outside every confined CRTC's box.
 */
extern void XInputScaleLogicalToPhysicalCursor(ScreenPtr pScreen,
                                               int *x, int *y);

#endif /* _XSERVER_INPUTSCALE_H */
