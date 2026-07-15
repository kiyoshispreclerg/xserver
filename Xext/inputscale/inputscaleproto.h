/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-INPUT-SCALE extension - protocol definitions (DRAFT v1.0)
 *
 * Lets a compositor confine the pointer, per CRTC, to a sub-rectangle of that
 * CRTC's physical scanout area - the "logical" area it is actually drawing
 * into when doing sharp per-monitor HiDPI scaling. Unlike a coordinate
 * transform, this never remaps anything: window geometry, click/hit-testing,
 * and RandR's own geometry queries are all untouched. It only stops the
 * pointer from wandering into scanout pixels the compositor isn't currently
 * using for that output.
 *
 * See doc/x11-per-output-scaling-extension.md.
 *
 * This header is the on-the-wire contract: it would normally live in xorgproto
 * and be shared with client bindings. It is kept here while the design is in
 * flux.
 */
#ifndef _XSERVER_INPUTSCALEPROTO_H
#define _XSERVER_INPUTSCALEPROTO_H

#include <X11/Xmd.h>
#include <X11/Xproto.h>

#define XIS_EXTENSION_NAME    "X-INPUT-SCALE"
#define XIS_MAJOR_VERSION     1
#define XIS_MINOR_VERSION     0

/* request opcodes (minor) */
#define X_XISQueryVersion       0
#define X_XISSetCrtcConfine     1
#define X_XISGetCrtcConfine     2
#define X_XISResetCrtcConfine   3
#define XISNumberRequests       4

/* ------------------------------------------------------------------ *
 *  Requests
 * ------------------------------------------------------------------ */

typedef struct {
    CARD8   reqType;            /* extension major opcode */
    CARD8   xisReqType;         /* X_XISQueryVersion */
    CARD16  length;
    CARD32  clientMajorVersion;
    CARD32  clientMinorVersion;
} xXISQueryVersionReq;
#define sz_xXISQueryVersionReq 12

/* Confine the cursor to [x, y, x+width, y+height) (desktop-space coordinates)
 * while it is over this CRTC. Must lie within the CRTC's current physical
 * scanout box - rejected with BadMatch otherwise. This only ever shrinks
 * where the cursor can go; it never grows the desktop. A CRTC that needs
 * *more* logical space than it has physical pixels (a low-DPI/upscale
 * output) is out of scope here - use RandR's own CRTC transform
 * proportionally (e.g. `xrandr --scale`) for that instead. */
typedef struct {
    CARD8   reqType;
    CARD8   xisReqType;         /* X_XISSetCrtcConfine */
    CARD16  length;
    CARD32  crtc;                /* RRCrtc */
    INT16   x;
    INT16   y;
    CARD16  width;
    CARD16  height;
} xXISSetCrtcConfineReq;
#define sz_xXISSetCrtcConfineReq 16

typedef struct {
    CARD8   reqType;
    CARD8   xisReqType;         /* X_XISGetCrtcConfine */
    CARD16  length;
    CARD32  crtc;                /* RRCrtc */
} xXISGetCrtcConfineReq;
#define sz_xXISGetCrtcConfineReq 8

typedef struct {
    CARD8   reqType;
    CARD8   xisReqType;         /* X_XISResetCrtcConfine */
    CARD16  length;
    CARD32  crtc;                /* RRCrtc */
} xXISResetCrtcConfineReq;
#define sz_xXISResetCrtcConfineReq 8

/* ------------------------------------------------------------------ *
 *  Replies (32-byte reply header)
 * ------------------------------------------------------------------ */

typedef struct {
    BYTE    type;                /* X_Reply */
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;
    CARD32  majorVersion;
    CARD32  minorVersion;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
} xXISQueryVersionReply;
#define sz_xXISQueryVersionReply 32

typedef struct {
    BYTE    type;                /* X_Reply */
    CARD8   active;               /* 0 = no confinement, 1 = active */
    CARD16  sequenceNumber;
    CARD32  length;
    INT16   x;
    INT16   y;
    CARD16  width;
    CARD16  height;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
} xXISGetCrtcConfineReply;
#define sz_xXISGetCrtcConfineReply 32

#endif /* _XSERVER_INPUTSCALEPROTO_H */
