#ifndef _XSERV_GLOBAL_H_
#define _XSERV_GLOBAL_H_

#include <X11/Xdefs.h>
#include <X11/Xfuncproto.h>

/* Global X server variables that are visible to mi, dix, os, and ddx */

extern _X_EXPORT const char *defaultFontPath;
extern _X_EXPORT int monitorResolution;
extern _X_EXPORT int defaultColorVisualClass;
/* Opt-in: recompute the per-output RandR "DPI" property from EDID physical
 * size + active mode. Off by default: outputs keep monitorResolution/96.
 * Enabled via "Option "AutoDPI" "true"" in xorg.conf's ServerFlags section. */
extern _X_EXPORT Bool rrAutoComputeDPI;

#endif                          /* !_XSERV_GLOBAL_H_ */
