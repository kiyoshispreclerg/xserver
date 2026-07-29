/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-INPUT-SCALE extension.
 *
 * Lets a compositor confine the pointer, per CRTC, to a sub-rectangle of
 * that CRTC's physical scanout area - the region it is actually drawing
 * "logical", higher-density content into when doing sharp per-monitor
 * HiDPI scaling.
 *
 * Deliberately does *not* remap any reported coordinate: window geometry,
 * click/hit-testing, and every RandR geometry query (XRRGetMonitors, etc.)
 * stay exactly as they are. A compositor's physical/logical duality is
 * entirely internal to it and never needs to leak into the X11 protocol for
 * anything except this one thing: stopping the cursor from wandering into
 * scanout pixels the compositor isn't currently drawing into for that
 * output.
 *
 * This replaces an earlier, much larger X-INPUT-TRANSFORM extension that
 * also remapped absolute pointer coordinates physical<->logical before
 * hit-testing. That turned out to solve a problem this project's compositor
 * doesn't actually have here: window geometry and click coordinates already
 * agree without any remapping, so the coordinate-transform half was pure
 * liability (it was the actual cause of popups ending up misaligned with
 * the click that opened them) with no upside. Confinement is the one piece
 * that genuinely can't be done compositor-side without server help: cursor
 * motion runs through the core input pipeline on every event, and a client
 * reacting after the fact with XWarpPointer would be visibly laggy/jittery
 * compared to a synchronous clamp in that pipeline.
 *
 * See doc/x11-per-output-scaling-extension.md.
 */
#include <dix-config.h>

#include <X11/Xmd.h>
#include <X11/Xproto.h>

#include "dix/dix_priv.h"
#include "dix/request_priv.h"
#include "include/dixstruct.h"
#include "include/os.h"
#include "include/scrnintstr.h"
#include "include/randrstr.h"
#include "miext/extinit_priv.h"
#include "Xext/randr/randrstr_priv.h"       /* VERIFY_RR_CRTC, rrGetScrPriv */

#include "inputscaleproto.h"
#include "include/inputscale.h"

Bool noXInputScaleExtension = FALSE;

/* Number of CRTCs that currently carry an active confinement box. The
 * ConstrainCursorHarder wrap below checks this first, so the common
 * no-compositor case costs a single comparison. */
static int xis_active_count = 0;

Bool
XInputScaleActive(void)
{
    return !noXInputScaleExtension && xis_active_count > 0;
}

/* ------------------------------------------------------------------ *
 *  Per-CRTC state helpers
 * ------------------------------------------------------------------ */

static void
xis_crtc_reset(RRCrtcPtr crtc)
{
    if (!crtc->confine_active)
        return;
    crtc->confine_active = FALSE;
    crtc->confine_client = NULL;
    if (xis_active_count > 0)
        xis_active_count--;
}

/* Physical (scanout) bounding box of a CRTC, in desktop coordinates. */
static Bool
xis_crtc_box(BoxPtr box, RRCrtcPtr crtc)
{
    if (!crtc->mode)
        return FALSE;

    box->x1 = crtc->x;
    box->y1 = crtc->y;
    switch (crtc->rotation) {
    case RR_Rotate_90:
    case RR_Rotate_270:
        box->x2 = crtc->x + crtc->mode->mode.height;
        box->y2 = crtc->y + crtc->mode->mode.width;
        break;
    default:
        box->x2 = crtc->x + crtc->mode->mode.width;
        box->y2 = crtc->y + crtc->mode->mode.height;
        break;
    }
    return TRUE;
}

static inline Bool
xis_box_contains(const BoxRec *box, int x, int y)
{
    return x >= box->x1 && x < box->x2 && y >= box->y1 && y < box->y2;
}

static inline Bool
xis_box_contains_box(const BoxRec *outer, const BoxRec *inner)
{
    return inner->x1 >= outer->x1 && inner->x2 <= outer->x2 &&
           inner->y1 >= outer->y1 && inner->y2 <= outer->y2;
}

/* ------------------------------------------------------------------ *
 *  Hardware cursor scale factor
 *
 *  Consumed by hw/xfree86/modes/xf86Cursors.c for both bitmap scaling
 *  (xf86_crtc_load_cursor_argb()) and, applied locally per CRTC to that
 *  CRTC's own offset, position (xf86_crtc_set_cursor_position()). See the
 *  declaration in include/inputscale.h for why position scaling happens
 *  there instead of once upstream on the shared pointer position.
 * ------------------------------------------------------------------ */

Bool
XInputScaleGetCrtcScale(RRCrtcPtr crtc, double *sx, double *sy)
{
    BoxRec physical;
    int confine_w, confine_h;

    if (!crtc->confine_active)
        return FALSE;
    if (!xis_crtc_box(&physical, crtc))
        return FALSE;

    confine_w = crtc->confine_box.x2 - crtc->confine_box.x1;
    confine_h = crtc->confine_box.y2 - crtc->confine_box.y1;
    if (confine_w <= 0 || confine_h <= 0)
        return FALSE;

    *sx = (double) (physical.x2 - physical.x1) / confine_w;
    *sy = (double) (physical.y2 - physical.y1) / confine_h;
    return TRUE;
}

/* ------------------------------------------------------------------ *
 *  Cursor confinement to the compositor-declared boxes
 *
 *  Always chains to the previously-installed hook (normally RandR's own
 *  RRConstrainCursorHarder) *first*, unconditionally - that is what gives a
 *  multi-monitor layout its ordinary "screens are closed boxes, open only
 *  where they touch" behavior (respecting actual per-CRTC alignment/height
 *  differences, corners, gaps between non-adjacent edges, etc.), and it must
 *  keep doing so for every CRTC exactly as on a stock setup, regardless of
 *  whether some *other* CRTC on the screen has an active confine box. Only
 *  once that has produced an already screen-valid (x,y) do we look at
 *  whether it landed on a CRTC that has its own confine box active, and if
 *  so, tighten further into *that CRTC's own box* - never into a different
 *  CRTC's, so confining one output can never trap the cursor away from an
 *  adjacent, unconfined (or independently confined) one.
 * ------------------------------------------------------------------ */

static ConstrainCursorHarderProcPtr xis_orig_constrain[MAXSCREENS];

static void
xis_constrain_cursor_harder(DeviceIntPtr pDev, ScreenPtr pScreen, int mode,
                            int *x, int *y)
{
    rrScrPrivPtr pScrPriv;

    /* Always run the previously-installed hook (normally RandR's own
     * RRConstrainCursorHarder) first, unconditionally - see the comment
     * above this function. */
    if (xis_orig_constrain[pScreen->myNum])
        xis_orig_constrain[pScreen->myNum](pDev, pScreen, mode, x, y);
    /* Some chained hooks (e.g. xf86's RandR compat shim,
     * xf86RandR13ConstrainCursorHarder in hw/xfree86/modes/xf86RandR12.c)
     * save/restore pScreen->ConstrainCursorHarder around their own call and
     * point it back at *themselves* afterward, assuming they're the only
     * wrapper. Reassert ourselves as the owner so we don't get silently
     * evicted from the chain. */
    pScreen->ConstrainCursorHarder = xis_constrain_cursor_harder;

    if (!XInputScaleActive())
        return;

    pScrPriv = rrGetScrPriv(pScreen);
    if (!pScrPriv)
        return;

    /* On top of the already screen-valid (x,y) above, tighten further if it
     * landed on a CRTC that has its own confine box active - and only into
     * that CRTC's own box, never into a different CRTC's. */
    for (int i = 0; i < pScrPriv->numCrtcs; i++) {
        RRCrtcPtr crtc = pScrPriv->crtcs[i];
        BoxRec physical;

        if (!crtc->confine_active)
            continue;
        if (!xis_crtc_box(&physical, crtc))
            continue;
        if (!xis_box_contains(&physical, *x, *y))
            continue;

        if (xis_box_contains(&crtc->confine_box, *x, *y))
            return; /* inside this CRTC's own confine box: fine as-is */

        if (*x < crtc->confine_box.x1)
            *x = crtc->confine_box.x1;
        if (*x >= crtc->confine_box.x2)
            *x = crtc->confine_box.x2 - 1;
        if (*y < crtc->confine_box.y1)
            *y = crtc->confine_box.y1;
        if (*y >= crtc->confine_box.y2)
            *y = crtc->confine_box.y2 - 1;
        return;
    }
}

/* ------------------------------------------------------------------ *
 *  Auto-revert on client disconnect (compositor crash/exit)
 * ------------------------------------------------------------------ */

static void
xis_client_state(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    NewClientInfoRec *info = calldata;
    ClientPtr client = info->client;

    if (client->clientState != ClientStateGone &&
        client->clientState != ClientStateRetained)
        return;

    if (xis_active_count == 0)
        return;

    for (int s = 0; s < screenInfo.numScreens; s++) {
        ScreenPtr pScreen = screenInfo.screens[s];
        rrScrPrivPtr pScrPriv = rrGetScrPriv(pScreen);

        if (!pScrPriv)
            continue;
        for (int i = 0; i < pScrPriv->numCrtcs; i++) {
            RRCrtcPtr crtc = pScrPriv->crtcs[i];

            if (crtc->confine_active && crtc->confine_client == client)
                xis_crtc_reset(crtc);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Requests
 * ------------------------------------------------------------------ */

static int
ProcXISQueryVersion(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXISQueryVersionReq);
    X_REQUEST_FIELD_CARD32(clientMajorVersion);
    X_REQUEST_FIELD_CARD32(clientMinorVersion);

    xXISQueryVersionReply reply = {
        .majorVersion = XIS_MAJOR_VERSION,
        .minorVersion = (stuff->clientMajorVersion < XIS_MAJOR_VERSION)
                            ? stuff->clientMinorVersion : XIS_MINOR_VERSION,
    };
    X_REPLY_FIELD_CARD32(majorVersion);
    X_REPLY_FIELD_CARD32(minorVersion);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

static int
ProcXISSetCrtcConfine(ClientPtr client)
{
    RRCrtcPtr crtc;
    BoxRec box, physical;

    X_REQUEST_HEAD_STRUCT(xXISSetCrtcConfineReq);
    X_REQUEST_FIELD_CARD32(crtc);
    X_REQUEST_FIELD_CARD16(x);
    X_REQUEST_FIELD_CARD16(y);
    X_REQUEST_FIELD_CARD16(width);
    X_REQUEST_FIELD_CARD16(height);

    VERIFY_RR_CRTC(stuff->crtc, crtc, DixSetAttrAccess);

    if (stuff->width == 0 || stuff->height == 0)
        return BadValue;

    box.x1 = stuff->x;
    box.y1 = stuff->y;
    box.x2 = stuff->x + stuff->width;
    box.y2 = stuff->y + stuff->height;

    /* Only ever shrinks where the cursor can go - never grows the desktop.
     * A CRTC that needs *more* logical space than it has physical pixels
     * (low-DPI/upscale) is out of scope; use RandR's own CRTC transform
     * proportionally (xrandr --scale) for that instead. */
    if (!xis_crtc_box(&physical, crtc) || !xis_box_contains_box(&physical, &box))
        return BadMatch;

    if (!crtc->confine_active)
        xis_active_count++;
    crtc->confine_active = TRUE;
    crtc->confine_client = client;
    crtc->confine_box = box;
    return Success;
}

static int
ProcXISGetCrtcConfine(ClientPtr client)
{
    RRCrtcPtr crtc;

    X_REQUEST_HEAD_STRUCT(xXISGetCrtcConfineReq);
    X_REQUEST_FIELD_CARD32(crtc);

    VERIFY_RR_CRTC(stuff->crtc, crtc, DixReadAccess);

    xXISGetCrtcConfineReply reply = {
        .active = crtc->confine_active ? 1 : 0,
    };

    if (crtc->confine_active) {
        reply.x = crtc->confine_box.x1;
        reply.y = crtc->confine_box.y1;
        reply.width = crtc->confine_box.x2 - crtc->confine_box.x1;
        reply.height = crtc->confine_box.y2 - crtc->confine_box.y1;
    }

    X_REPLY_FIELD_CARD16(x);
    X_REPLY_FIELD_CARD16(y);
    X_REPLY_FIELD_CARD16(width);
    X_REPLY_FIELD_CARD16(height);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

static int
ProcXISResetCrtcConfine(ClientPtr client)
{
    RRCrtcPtr crtc;

    X_REQUEST_HEAD_STRUCT(xXISResetCrtcConfineReq);
    X_REQUEST_FIELD_CARD32(crtc);

    VERIFY_RR_CRTC(stuff->crtc, crtc, DixSetAttrAccess);

    xis_crtc_reset(crtc);
    return Success;
}

static int
ProcXISDispatch(ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data) {
    case X_XISQueryVersion:     return ProcXISQueryVersion(client);
    case X_XISSetCrtcConfine:   return ProcXISSetCrtcConfine(client);
    case X_XISGetCrtcConfine:   return ProcXISGetCrtcConfine(client);
    case X_XISResetCrtcConfine: return ProcXISResetCrtcConfine(client);
    default:                    return BadRequest;
    }
}

/* ------------------------------------------------------------------ *
 *  Init
 * ------------------------------------------------------------------ */

void
XInputScaleExtensionInit(void)
{
    ExtensionEntry *ext;

    if (noXInputScaleExtension)
        return;

    if (!AddCallback(&ClientStateCallback, xis_client_state, NULL)) {
        noXInputScaleExtension = TRUE;
        return;
    }

    ext = AddExtension(XIS_EXTENSION_NAME, 0, 0,
                       ProcXISDispatch, ProcXISDispatch,
                       NULL, StandardMinorOpcode);
    if (!ext) {
        DeleteCallback(&ClientStateCallback, xis_client_state, NULL);
        noXInputScaleExtension = TRUE;
        return;
    }

    /* Wrap ConstrainCursorHarder on every screen so the pointer is confined
     * to the compositor-declared boxes while any are active (see
     * xis_constrain_cursor_harder() above). Saved per-screen so we can chain
     * to whatever RandR/the driver already installed there (usually
     * RRConstrainCursorHarder) when we're not the ones doing the clamping. */
    for (int s = 0; s < screenInfo.numScreens; s++) {
        ScreenPtr pScreen = screenInfo.screens[s];

        xis_orig_constrain[pScreen->myNum] = pScreen->ConstrainCursorHarder;
        pScreen->ConstrainCursorHarder = xis_constrain_cursor_harder;
    }
}
