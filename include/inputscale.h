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
#include "randrstr.h"           /* RRCrtcPtr */

/*
 * Fast-path predicate: TRUE iff at least one CRTC currently has an active
 * confinement box.
 */
extern _X_EXPORT Bool XInputScaleActive(void);

/*
 * TRUE and fills sx/sy (via the out-pointers) with the physical-scanout-size
 * to confine-box-size ratio for this CRTC iff it currently has an active
 * confinement box; FALSE (leaving sx/sy untouched) otherwise.
 *
 * For hardware cursor bitmap scaling: a driver can multiply a cursor
 * glyph's dimensions by this factor before uploading it to a confined
 * CRTC's cursor plane, so the on-screen cursor size matches the panel's
 * physical pixel density instead of looking undersized relative to it (see
 * xf86_crtc_load_cursor_argb() in hw/xfree86/modes/xf86Cursors.c).
 *
 * For hardware cursor *position*: unlike bitmap scaling, this is applied
 * per-CRTC to that CRTC's own *local* offset (already relative to its
 * origin, already including the unscaled hotspot subtraction upstream in
 * xf86HWCurs.c's xf86SetCursor()) rather than to the shared desktop-space
 * position every CRTC receives - see xf86_crtc_set_cursor_position() in
 * hw/xfree86/modes/xf86Cursors.c for why: scaling locally, independently
 * per CRTC, is what makes the cursor already look correctly scaled and
 * positioned on a confined CRTC while still straddling its boundary with
 * an unconfined (or differently confined) neighbor, instead of jumping to
 * the right place only once the pointer's hotspot itself crosses in.
 */
extern _X_EXPORT Bool XInputScaleGetCrtcScale(RRCrtcPtr crtc, double *sx, double *sy);

#endif /* _XSERVER_INPUTSCALE_H */
