/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE in the xf86 modes layer (xf86XScale.c), and what it uses of
 * xf86Rotate.c.
 */
#ifndef _XSERVER_XF86XSCALE_PRIV_H_
#define _XSERVER_XF86XSCALE_PRIV_H_

#include "xf86Crtc.h"

/* xf86Rotate.c: magnify the whole root into the CRTC's shadow again. */
void xf86CrtcDamageShadow(xf86CrtcPtr crtc);

#ifdef CONFIG_XSCALE
/* Lets X-SCALE scanout sources feed this screen's transformed CRTCs. */
void xf86RotateScanoutInit(ScreenPtr pScreen);

/* Copies the CRTC's X-SCALE source into its shadow, from
 * xf86RotateRedisplay(). */
void xf86RotateScanoutRedisplay(ScreenPtr pScreen, xf86CrtcPtr crtc);

/* xf86CrtcTransformRedisplay() for a CRTC with an X-SCALE source; FALSE
 * when it has none. */
Bool xf86CrtcCopyScanoutSource(xf86CrtcPtr crtc, PixmapPtr dst_pixmap,
                               RegionPtr region);
#endif

#endif /* _XSERVER_XF86XSCALE_PRIV_H_ */
