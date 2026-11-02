/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE extension.
 *
 * Sharp per-output scaling on X11 without leaving one coordinate space.
 * The output is scaled the ordinary RandR way: a CRTC transform (xrandr
 * --scale-from) gives it a logical footprint in the root smaller than its
 * physical resolution, so the root, window geometry, input, the pointer's
 * travel between outputs and every RandR query stay contiguous and agree
 * with each other with no help from anyone. What RandR alone can't do is
 * make it sharp: such a CRTC scans out a shadow the DDX fills by magnifying
 * the logical root.
 *
 * This extension lets a compositor that rendered the output at physical
 * density hand that picture over instead.
 *
 * See doc/x-scale.md.
 */
#include <dix-config.h>

#include <X11/Xmd.h>
#include <X11/Xproto.h>

#include "dix/dix_priv.h"
#include "dix/request_priv.h"
#include "include/dixstruct.h"
#include "include/extnsionst.h"
#include "miext/extinit_priv.h"

#include "xscaleproto.h"

Bool noXScaleExtension = FALSE;

/* ------------------------------------------------------------------ *
 *  Requests
 * ------------------------------------------------------------------ */

/* XScaleQueryVersion: the server's version, or the client's minor version
 * when the client asked for an older major. */
static int
ProcXScaleQueryVersion(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXScaleQueryVersionReq);
    X_REQUEST_FIELD_CARD32(clientMajorVersion);
    X_REQUEST_FIELD_CARD32(clientMinorVersion);

    xXScaleQueryVersionReply reply = {
        .majorVersion = XSCALE_MAJOR_VERSION,
        .minorVersion = (stuff->clientMajorVersion < XSCALE_MAJOR_VERSION)
                            ? stuff->clientMinorVersion : XSCALE_MINOR_VERSION,
    };
    X_REPLY_FIELD_CARD32(majorVersion);
    X_REPLY_FIELD_CARD32(minorVersion);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/* Dispatches by minor opcode. Shared by both byte orders: every request
 * handler swaps its own fields (X_REQUEST_FIELD_*). */
static int
ProcXScaleDispatch(ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data) {
    case X_XScaleQueryVersion:  return ProcXScaleQueryVersion(client);
    default:                    return BadRequest;
    }
}

/* ------------------------------------------------------------------ *
 *  Init
 * ------------------------------------------------------------------ */

/* Registers the extension. Called once from the extension init table
 * (mi/miinitext.c); `-extension X-SCALE` skips it. */
void
XScaleExtensionInit(void)
{
    ExtensionEntry *ext;

    if (noXScaleExtension)
        return;

    ext = AddExtension(XSCALE_EXTENSION_NAME, XScaleNumberEvents, 0,
                       ProcXScaleDispatch, ProcXScaleDispatch,
                       NULL, StandardMinorOpcode);
    if (!ext) {
        noXScaleExtension = TRUE;
        return;
    }
}
