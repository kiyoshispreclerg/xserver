/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-SCALE smoke test (see doc/x-scale.md).
 *
 * Exercises the wire protocol against a running server. Xvfb is enough for
 * everything here: it never has a CRTC that can take a scanout source (no
 * xf86 shadow), which is exactly the "not capable" side a compositor has to
 * handle on any other DDX too.
 *
 *   1. QueryVersion answers 1.x.
 *   2. SelectInput accepts XScaleScanoutNotifyMask and 0, and refuses any
 *      other bit with BadValue.
 *   3. GetCrtcScanout reports the CRTC's mode size and logical box, and no
 *      capability on an untransformed CRTC.
 *   4. SetCrtcScanoutSource with a pixmap is refused (BadMatch) there.
 *   5. SetCrtcScanoutSource with None (give back) is always accepted.
 *   6. CopyCrtcScanout of an untransformed CRTC reproduces its root area
 *      (painted a known colour), and refuses a destination of another
 *      depth with BadMatch. An Xnotify policy denying screen reads makes
 *      it answer BadAccess, which is reported as a skip.
 *   7. PresentCrtcScanout is refused with BadMatch on that CRTC, and
 *      with BadPixmap for a pixmap that doesn't exist.
 *
 * The capable side - a sharp source actually shown on a transformed CRTC,
 * the hardware cursor taking over - needs an xf86 DDX on real hardware with
 * `xrandr --scale-from`; see the doc's testing section.
 *
 * Build:
 *   cc xscale-smoke-test.c -o xscale-smoke-test \
 *      $(pkg-config --cflags --libs xcb xcb-randr)
 * Run:
 *   DISPLAY=:99 ./xscale-smoke-test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/randr.h>

#define XSCALE_NAME "X-SCALE"

/* mirror of xscaleproto.h, kept local so the test needs no server headers */
#define X_XScaleQueryVersion            0
#define X_XScaleSelectInput             1
#define X_XScaleGetCrtcScanout          2
#define X_XScaleSetCrtcScanoutSource    3
#define X_XScaleCopyCrtcScanout         4
#define X_XScalePresentCrtcScanout      5
#define XScaleScanoutNotifyMask         (1 << 0)
#define XScaleScanoutCapable            (1 << 0)
#define XScaleScanoutActive             (1 << 1)

static uint8_t xscale_op;
static int failures = 0;

#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok   - " __VA_ARGS__); putchar('\n'); } \
    else      { printf("  FAIL - " __VA_ARGS__); putchar('\n'); failures++; } \
} while (0)

/* Sends an extension request whose header is (major, minor, length, ...).
 * With ext = NULL, xcb writes byte 0 = opcode (major) and the length; the
 * payload carries byte 1 = minor opcode. */
static unsigned
send_ext(xcb_connection_t *c, const void *req, size_t len, int isvoid)
{
    struct iovec parts[4];
    xcb_protocol_request_t r = {
        .count = 2, .ext = NULL, .opcode = xscale_op, .isvoid = isvoid
    };
    parts[2].iov_base = (void *) req;
    parts[2].iov_len = len;
    parts[3].iov_base = NULL;
    parts[3].iov_len = -len & 3;
    /* Void requests are sent checked, so xcb_request_check() gets their
     * errors instead of the event queue. */
    return xcb_send_request(c, isvoid ? XCB_REQUEST_CHECKED : 0, parts + 2, &r);
}

/* The reply to a request, or NULL on error. */
static void *
reply_ext(xcb_connection_t *c, const void *req, size_t len)
{
    unsigned seq = send_ext(c, req, len, 0);
    xcb_generic_error_t *err = NULL;
    void *rep = xcb_wait_for_reply(c, seq, &err);
    if (err) { free(err); free(rep); return NULL; }
    return rep;
}

/* The X error code of a void request, or 0 on success. */
static int
void_ext(xcb_connection_t *c, const void *req, size_t len)
{
    unsigned seq = send_ext(c, req, len, 1);
    xcb_generic_error_t *e = xcb_request_check(c, (xcb_void_cookie_t){ seq });
    int code = e ? e->error_code : 0;
    free(e);
    return code;
}

static int
select_input(xcb_connection_t *c, uint32_t window, uint32_t mask)
{
    struct { uint8_t reqType, xscaleReqType; uint16_t length;
             uint32_t window, mask; } req = { 0 };
    req.xscaleReqType = X_XScaleSelectInput;
    req.window = window;
    req.mask = mask;
    return void_ext(c, &req, sizeof(req));
}

static int
set_source(xcb_connection_t *c, uint32_t crtc, uint32_t drawable)
{
    struct { uint8_t reqType, xscaleReqType; uint16_t length;
             uint32_t crtc, drawable; int16_t x, y; } req = { 0 };
    req.xscaleReqType = X_XScaleSetCrtcScanoutSource;
    req.crtc = crtc;
    req.drawable = drawable;
    return void_ext(c, &req, sizeof(req));
}

static int
copy_scanout(xcb_connection_t *c, uint32_t crtc, uint32_t drawable)
{
    struct { uint8_t reqType, xscaleReqType; uint16_t length;
             uint32_t crtc, drawable; int16_t sx, sy; uint16_t w, h;
             int16_t dx, dy; } req = { 0 };
    req.xscaleReqType = X_XScaleCopyCrtcScanout;
    req.crtc = crtc;
    req.drawable = drawable;      /* whole scanout to (0, 0) */
    return void_ext(c, &req, sizeof(req));
}

static int
present_scanout(xcb_connection_t *c, uint32_t crtc, uint32_t pixmap)
{
    struct { uint8_t reqType, xscaleReqType; uint16_t length;
             uint32_t crtc, pixmap, serial, update; } req = { 0 };
    req.xscaleReqType = X_XScalePresentCrtcScanout;
    req.crtc = crtc;
    req.pixmap = pixmap;
    req.serial = 1;
    return void_ext(c, &req, sizeof(req));
}

static uint8_t
query_op(xcb_connection_t *c, const char *name)
{
    xcb_query_extension_reply_t *r =
        xcb_query_extension_reply(c, xcb_query_extension(c, strlen(name), name), NULL);
    uint8_t op = (r && r->present) ? r->major_opcode : 0;
    free(r);
    return op;
}

int
main(void)
{
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) { fprintf(stderr, "cannot connect\n"); return 2; }
    xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t root = scr->root;

    xscale_op = query_op(c, XSCALE_NAME);
    CHECK(xscale_op != 0, "%s present (opcode %d)", XSCALE_NAME, xscale_op);
    if (!xscale_op) { xcb_disconnect(c); return 1; }

    /* QueryVersion */
    struct { uint8_t reqType, xscaleReqType; uint16_t length;
             uint32_t maj, min; } qv = { 0 };
    qv.xscaleReqType = X_XScaleQueryVersion; qv.maj = 1; qv.min = 0;
    uint32_t *qr = reply_ext(c, &qv, sizeof(qv));
    CHECK(qr != NULL && qr[2] == 1, "QueryVersion answers 1.x (%u.%u)",
          qr ? qr[2] : 0, qr ? qr[3] : 0);
    free(qr);

    /* SelectInput */
    CHECK(select_input(c, root, XScaleScanoutNotifyMask) == 0,
          "SelectInput(ScanoutNotify) accepted");
    int err = select_input(c, root, 0x80);
    CHECK(err == XCB_VALUE, "SelectInput with an unknown bit refused with "
          "BadValue (%d)", err);
    CHECK(select_input(c, root, 0) == 0, "SelectInput(0) cancels");

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(c,
            xcb_randr_get_screen_resources_current(c, root), NULL);
    if (!res || xcb_randr_get_screen_resources_current_crtcs_length(res) == 0) {
        printf("  SKIP - no RandR CRTC available; protocol-only checks done\n");
        xcb_disconnect(c);
        return failures ? 1 : 0;
    }
    xcb_randr_crtc_t crtc = xcb_randr_get_screen_resources_current_crtcs(res)[0];
    xcb_randr_get_crtc_info_reply_t *ci =
        xcb_randr_get_crtc_info_reply(c, xcb_randr_get_crtc_info(c, crtc, 0), NULL);
    int cx = ci ? ci->x : 0, cy = ci ? ci->y : 0;
    int cw = ci ? ci->width : 0, ch = ci ? ci->height : 0;
    printf("         using CRTC 0x%x at (%d,%d) %ux%u\n", crtc, cx, cy, cw, ch);

    /* GetCrtcScanout: reply [1]=flags [8..9]=scanoutW [10..11]=scanoutH
     * [12..13]=x [14..15]=y [16..17]=width [18..19]=height */
    struct { uint8_t reqType, xscaleReqType; uint16_t length;
             uint32_t crtc; } gs = { 0 };
    gs.xscaleReqType = X_XScaleGetCrtcScanout;
    gs.crtc = crtc;
    uint8_t *gr = reply_ext(c, &gs, sizeof(gs));
    CHECK(gr != NULL, "GetCrtcScanout replied");
    if (gr) {
        uint16_t *u = (uint16_t *) (gr + 8);
        int16_t *s = (int16_t *) (gr + 8);
        printf("         flags 0x%x, scanout %ux%u, logical %ux%u+%d+%d\n",
               gr[1], u[0], u[1], u[4], u[5], s[2], s[3]);
        CHECK(!(gr[1] & XScaleScanoutCapable),
              "an untransformed CRTC is not capable");
        CHECK(!(gr[1] & XScaleScanoutActive), "no source active");
        CHECK(u[0] == cw && u[1] == ch, "scanout size is the mode size");
        CHECK(s[2] == cx && s[3] == cy && u[4] == cw && u[5] == ch,
              "logical box is the CRTC's box without a transform");
        free(gr);
    }

    /* setting a source on a CRTC that can't take one */
    xcb_pixmap_t pix = xcb_generate_id(c);
    xcb_create_pixmap(c, scr->root_depth, pix, root,
                      cw > 0 ? cw : 1, ch > 0 ? ch : 1);
    err = set_source(c, crtc, pix);
    CHECK(err == XCB_MATCH, "SetCrtcScanoutSource refused on a non-capable "
          "CRTC with BadMatch (%d)", err);
    xcb_free_pixmap(c, pix);

    err = set_source(c, crtc, XCB_NONE);
    CHECK(err == 0, "SetCrtcScanoutSource None accepted (%d)", err);

    /* presenting on a CRTC that can't take a source */
    pix = xcb_generate_id(c);
    xcb_create_pixmap(c, scr->root_depth, pix, root,
                      cw > 0 ? cw : 1, ch > 0 ? ch : 1);
    err = present_scanout(c, crtc, pix);
    CHECK(err == XCB_MATCH, "PresentCrtcScanout refused on a non-capable "
          "CRTC with BadMatch (%d)", err);
    xcb_free_pixmap(c, pix);
    err = present_scanout(c, crtc, pix);
    CHECK(err == XCB_PIXMAP, "PresentCrtcScanout of a freed pixmap refused "
          "with BadPixmap (%d)", err);

    /* CopyCrtcScanout: paint the root one colour, copy the scanout out */
    if (cw > 0 && ch > 0) {
        const uint32_t colour = 0x00123456;
        uint32_t bg = colour;
        xcb_change_window_attributes(c, root, XCB_CW_BACK_PIXEL, &bg);
        xcb_clear_area(c, 0, root, 0, 0, 0, 0);

        pix = xcb_generate_id(c);
        xcb_create_pixmap(c, scr->root_depth, pix, root, cw, ch);
        err = copy_scanout(c, crtc, pix);
        if (err == XCB_ACCESS) {
            printf("  SKIP - CopyCrtcScanout denied by the Xnotify policy\n");
        } else {
            CHECK(err == 0, "CopyCrtcScanout into a root-depth pixmap (%d)", err);
            xcb_get_image_reply_t *img = xcb_get_image_reply(c,
                xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, pix,
                              cw / 2, ch / 2, 1, 1, ~0u), NULL);
            uint32_t got = 0;
            if (img && xcb_get_image_data_length(img) >= 4)
                got = *(uint32_t *) xcb_get_image_data(img) & 0x00ffffff;
            CHECK(got == colour, "copied scanout shows the root (0x%06x, "
                  "want 0x%06x)", got, colour);
            free(img);

            xcb_pixmap_t bitmap = xcb_generate_id(c);
            xcb_create_pixmap(c, 1, bitmap, root, 8, 8);
            err = copy_scanout(c, crtc, bitmap);
            CHECK(err == XCB_MATCH, "CopyCrtcScanout into a depth-1 pixmap "
                  "refused with BadMatch (%d)", err);
            xcb_free_pixmap(c, bitmap);
        }
        xcb_free_pixmap(c, pix);
    }

    xcb_disconnect(c);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
