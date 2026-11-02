/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE extension - protocol definitions (version 1.0)
 *
 * Sharp per-output scaling without leaving one coordinate space. An output
 * is scaled the ordinary RandR way, with a CRTC transform (for example
 * `xrandr --output DP-1 --scale-from 1280x720`), so the root window,
 * window geometry, input and every RandR query share one contiguous
 * logical space. Such a CRTC normally scans out a picture the server makes
 * by magnifying the logical root, which is blurry. X-SCALE lets a
 * compositor that drew the output at its physical resolution give the CRTC
 * that picture instead: a *scanout source*.
 *
 * See doc/x-scale.md.
 *
 * This header is the on-the-wire contract: it would normally live in
 * xorgproto and be shared with client bindings. It is kept here while the
 * design settles.
 */
#ifndef _XSERVER_XSCALEPROTO_H
#define _XSERVER_XSCALEPROTO_H

#include <X11/Xmd.h>
#include <X11/Xproto.h>

#define XSCALE_EXTENSION_NAME       "X-SCALE"
#define XSCALE_MAJOR_VERSION        1
#define XSCALE_MINOR_VERSION        0

/* request opcodes (minor) */
#define X_XScaleQueryVersion            0
#define X_XScaleSelectInput             1
#define X_XScaleGetCrtcScanout          2
#define X_XScaleSetCrtcScanoutSource    3
#define XScaleNumberRequests            4

/* events */
#define XScaleScanoutNotify             0
#define XScaleNumberEvents              1

/* XScaleSelectInput event mask */
#define XScaleScanoutNotifyMask         (1 << XScaleScanoutNotify)

/* XScaleGetCrtcScanout reply / XScaleScanoutNotify flags */
#define XScaleScanoutCapable    (1 << 0) /* a source can be set right now */
#define XScaleScanoutActive     (1 << 1) /* some client's source is in use */
#define XScaleScanoutMine       (1 << 2) /* ... and it is the asking client's */

/* ------------------------------------------------------------------ *
 *  Requests
 * ------------------------------------------------------------------ */

typedef struct {
    CARD8   reqType;            /* extension major opcode */
    CARD8   xscaleReqType;      /* X_XScaleQueryVersion */
    CARD16  length;
    CARD32  clientMajorVersion;
    CARD32  clientMinorVersion;
} xXScaleQueryVersionReq;
#define sz_xXScaleQueryVersionReq 12

/* Select (eventMask = XScaleScanoutNotifyMask) or cancel (0) delivery of
 * XScaleScanoutNotify events to this client, for as long as `window`
 * exists. */
typedef struct {
    CARD8   reqType;
    CARD8   xscaleReqType;      /* X_XScaleSelectInput */
    CARD16  length;
    CARD32  window;
    CARD32  eventMask;
} xXScaleSelectInputReq;
#define sz_xXScaleSelectInputReq 12

/* What a CRTC scans out, and whether it can take a source. */
typedef struct {
    CARD8   reqType;
    CARD8   xscaleReqType;      /* X_XScaleGetCrtcScanout */
    CARD16  length;
    CARD32  crtc;               /* RRCrtc */
} xXScaleGetCrtcScanoutReq;
#define sz_xXScaleGetCrtcScanoutReq 8

/* Make `drawable`, read from (srcX, srcY), what `crtc` scans out, at the
 * CRTC's physical resolution, in place of the server's magnified root.
 * From then on the server copies whatever is drawn into the drawable onto
 * the CRTC by itself, tracking it with Damage: the client sends nothing
 * per frame. `drawable` None gives the CRTC back to the server.
 *
 * The server copies whenever it goes idle, so a client should only ever
 * change the drawable a whole frame at a time (a buffer swap, or one copy
 * of a finished frame); otherwise a half-drawn frame may be shown.
 *
 * Errors:
 *   BadMatch   the CRTC is not capable (see XScaleGetCrtcScanout), or the
 *              drawable is on another screen, is not depth 24 or 32, or
 *              does not cover the scanout size from (srcX, srcY);
 *   BadAccess  another client's source is in use.
 *
 * The source is dropped, and an XScaleScanoutNotify sent, when its client
 * disconnects, the drawable is destroyed, or the CRTC is reconfigured so
 * that it stops being capable or changes scanout size. A reconfiguration
 * that keeps both (moving the output) keeps the source. */
typedef struct {
    CARD8   reqType;
    CARD8   xscaleReqType;      /* X_XScaleSetCrtcScanoutSource */
    CARD16  length;
    CARD32  crtc;               /* RRCrtc */
    CARD32  drawable;           /* Drawable, or None */
    INT16   srcX;
    INT16   srcY;
} xXScaleSetCrtcScanoutSourceReq;
#define sz_xXScaleSetCrtcScanoutSourceReq 16

/* ------------------------------------------------------------------ *
 *  Replies (32-byte reply header)
 * ------------------------------------------------------------------ */

typedef struct {
    BYTE    type;               /* X_Reply */
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;
    CARD32  majorVersion;
    CARD32  minorVersion;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
} xXScaleQueryVersionReply;
#define sz_xXScaleQueryVersionReply 32

/* scanoutWidth/Height: the CRTC's physical resolution, the size a source
 * has to cover. x/y/width/height: the logical box the CRTC shows, in root
 * coordinates (its transformed footprint). Both are filled whenever the
 * CRTC has a mode, capable or not; all zero for a disabled CRTC. */
typedef struct {
    BYTE    type;               /* X_Reply */
    CARD8   flags;              /* XScaleScanout* */
    CARD16  sequenceNumber;
    CARD32  length;
    CARD16  scanoutWidth;
    CARD16  scanoutHeight;
    INT16   x;
    INT16   y;
    CARD16  width;
    CARD16  height;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
} xXScaleGetCrtcScanoutReply;
#define sz_xXScaleGetCrtcScanoutReply 32

/* ------------------------------------------------------------------ *
 *  Events
 * ------------------------------------------------------------------ */

/* Sent to every client that selected XScaleScanoutNotifyMask whenever a
 * CRTC's source is set, replaced or dropped. `flags` as in the
 * XScaleGetCrtcScanout reply, except that XScaleScanoutMine is never set:
 * the same event goes to every listener. */
typedef struct {
    BYTE    type;               /* XScaleEventBase + XScaleScanoutNotify */
    CARD8   flags;
    CARD16  sequenceNumber;
    Window  window;             /* the window the selection was made on */
    CARD32  crtc;               /* RRCrtc */
    CARD16  scanoutWidth;
    CARD16  scanoutHeight;
    Time    timestamp;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
} xXScaleScanoutNotifyEvent;
#define sz_xXScaleScanoutNotifyEvent 32

#endif /* _XSERVER_XSCALEPROTO_H */
