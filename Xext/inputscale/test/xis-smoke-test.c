/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * X-INPUT-SCALE smoke test (see doc/x11-per-output-scaling-extension.md).
 *
 * Exercises the wire protocol and cursor confinement end to end against a
 * running server (Xvfb is enough):
 *
 *   1. QueryVersion.
 *   2. Register a confine box covering the left half of a CRTC.
 *   3. GetCrtcConfine reads it back.
 *   4. A box that doesn't fit inside the CRTC's physical scanout area is
 *      rejected (BadMatch) - this extension only ever shrinks, never grows.
 *   5. XTEST absolute motion into the right (unconfined) half must be
 *      clamped back into the box by QueryPointer.
 *   6. Reset -> motion into the same point is no longer clamped.
 *   7. A second client sets a confine box and disconnects; the CRTC must
 *      auto-revert (no confinement).
 *
 * Build:
 *   cc xis-smoke-test.c -o xis-smoke-test \
 *      $(pkg-config --cflags --libs xcb xcb-randr)
 * Run:
 *   DISPLAY=:99 ./xis-smoke-test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/randr.h>

#define XIS_NAME "X-INPUT-SCALE"

/* mirror of the protocol header (kept local so the test needs no server hdrs) */
#define X_XISQueryVersion       0
#define X_XISSetCrtcConfine     1
#define X_XISGetCrtcConfine     2
#define X_XISResetCrtcConfine   3

static uint8_t xis_op, xtest_op;
static int failures = 0;

#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok   - " __VA_ARGS__); putchar('\n'); } \
    else      { printf("  FAIL - " __VA_ARGS__); putchar('\n'); failures++; } \
} while (0)

/* Send an extension request whose header is (major, minor, length, ...).
 * With ext = NULL, xcb writes byte0 = opcode (major) and computes length; we
 * pre-set byte1 = minor opcode inside the payload. */
static unsigned
send_ext(xcb_connection_t *c, uint8_t major, const void *req, size_t len, int isvoid)
{
    struct iovec parts[4];
    xcb_protocol_request_t r = {
        .count = 2, .ext = NULL, .opcode = major, .isvoid = isvoid
    };
    parts[2].iov_base = (void *) req;
    parts[2].iov_len = len;
    parts[3].iov_base = NULL;
    parts[3].iov_len = -len & 3;
    return xcb_send_request(c, 0, parts + 2, &r);
}

static void *
reply_ext(xcb_connection_t *c, uint8_t major, const void *req, size_t len)
{
    unsigned seq = send_ext(c, major, req, len, 0);
    xcb_generic_error_t *err = NULL;
    void *rep = xcb_wait_for_reply(c, seq, &err);
    if (err) { free(err); free(rep); return NULL; }
    return rep;
}

struct confine_req {
    uint8_t reqType, xisReqType;
    uint16_t length;
    uint32_t crtc;
    int16_t x, y;
    uint16_t width, height;
};

/* returns the X error code, or 0 on success */
static int
set_confine(xcb_connection_t *c, uint32_t crtc, int16_t x, int16_t y,
            uint16_t width, uint16_t height)
{
    struct confine_req req = { 0 };
    req.xisReqType = X_XISSetCrtcConfine;
    req.crtc = crtc;
    req.x = x; req.y = y; req.width = width; req.height = height;
    unsigned seq = send_ext(c, xis_op, &req, sizeof(req), 1);
    xcb_generic_error_t *e = xcb_request_check(c, (xcb_void_cookie_t){ seq });
    int code = e ? e->error_code : 0;
    free(e);
    return code;
}

static void
reset_confine(xcb_connection_t *c, uint32_t crtc)
{
    struct { uint8_t reqType, xisReqType; uint16_t length; uint32_t crtc; } req = { 0 };
    req.xisReqType = X_XISResetCrtcConfine;
    req.crtc = crtc;
    unsigned seq = send_ext(c, xis_op, &req, sizeof(req), 1);
    free(xcb_request_check(c, (xcb_void_cookie_t){ seq }));
}

/* returns active flag (-1 on error) */
static int
get_confine(xcb_connection_t *c, uint32_t crtc, int *x, int *y, int *w, int *h)
{
    struct { uint8_t reqType, xisReqType; uint16_t length; uint32_t crtc; } req = { 0 };
    req.xisReqType = X_XISGetCrtcConfine;
    req.crtc = crtc;
    /* reply: [0]=type [1]=active [2..3]=seq [4..7]=len [8..9]=x [10..11]=y
     * [12..13]=width [14..15]=height */
    uint8_t *rep = reply_ext(c, xis_op, &req, sizeof(req));
    if (!rep) return -1;
    int active = rep[1];
    int16_t *xy = (int16_t *) (rep + 8);
    uint16_t *wh = (uint16_t *) (rep + 12);
    if (x) *x = xy[0];
    if (y) *y = xy[1];
    if (w) *w = wh[0];
    if (h) *h = wh[1];
    free(rep);
    return active;
}

static void
fake_motion(xcb_connection_t *c, xcb_window_t root, int16_t x, int16_t y)
{
    struct { uint8_t reqType, xtReqType; uint16_t length;
             uint8_t type, detail; uint16_t pad0; uint32_t time;
             uint32_t root; uint32_t pad1, pad2; int16_t rootX, rootY;
             uint32_t pad3; uint16_t pad4; uint8_t pad5, deviceid; } req = { 0 };
    req.xtReqType = 2;              /* X_XTestFakeInput */
    req.type = XCB_MOTION_NOTIFY;   /* 6 */
    req.detail = 0;                 /* absolute */
    req.root = root;
    req.rootX = x;
    req.rootY = y;
    unsigned seq = send_ext(c, xtest_op, &req, sizeof(req), 1);
    free(xcb_request_check(c, (xcb_void_cookie_t){ seq }));
}

static void
query_pointer(xcb_connection_t *c, xcb_window_t root, int *rx, int *ry)
{
    xcb_query_pointer_reply_t *r =
        xcb_query_pointer_reply(c, xcb_query_pointer(c, root), NULL);
    *rx = r ? r->root_x : -1;
    *ry = r ? r->root_y : -1;
    free(r);
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

    xis_op = query_op(c, XIS_NAME);
    xtest_op = query_op(c, "XTEST");
    CHECK(xis_op != 0, "%s present (opcode %d)", XIS_NAME, xis_op);
    if (!xis_op) { xcb_disconnect(c); return 1; }
    CHECK(xtest_op != 0, "XTEST present (opcode %d)", xtest_op);

    /* QueryVersion */
    struct { uint8_t reqType, xisReqType; uint16_t length; uint32_t maj, min; } qv = { 0 };
    qv.xisReqType = X_XISQueryVersion; qv.maj = 1; qv.min = 0;
    uint32_t *qr = reply_ext(c, xis_op, &qv, sizeof(qv));
    CHECK(qr != NULL, "QueryVersion replied");
    if (qr) { printf("         server version %u.%u\n", qr[2], qr[3]); free(qr); }

    /* first CRTC + its physical box */
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
    if (cw < 4 || ch < 4) {
        printf("  SKIP - CRTC too small to test meaningfully\n");
        xcb_disconnect(c);
        return failures ? 1 : 0;
    }

    /* a box exceeding the physical CRTC must be rejected */
    int err = set_confine(c, crtc, cx, cy, cw + 10, ch);
    CHECK(err != 0, "confine box larger than physical CRTC rejected (error %d)", err);

    /* confine to the left half */
    int halfw = cw / 2;
    err = set_confine(c, crtc, cx, cy, halfw, ch);
    CHECK(err == 0, "SetCrtcConfine to left half accepted");

    int gx, gy, gw, gh;
    int active = get_confine(c, crtc, &gx, &gy, &gw, &gh);
    CHECK(active == 1, "GetCrtcConfine active after set");
    CHECK(gx == cx && gy == cy && gw == halfw && gh == ch,
          "confine box read back as (%d,%d %dx%d) [want (%d,%d %dx%d)]",
          gx, gy, gw, gh, cx, cy, halfw, ch);

    /* motion into the right (unconfined) half must be clamped into the box */
    int px = cx + cw - 5, py = cy + ch / 2, rx, ry;
    fake_motion(c, root, px, py);
    xcb_flush(c);
    query_pointer(c, root, &rx, &ry);
    CHECK(rx < px && rx >= cx && rx < cx + halfw,
          "motion to (%d,%d) clamped into confine box, reported (%d,%d)",
          px, py, rx, ry);

    /* reset -> no confinement */
    reset_confine(c, crtc);
    active = get_confine(c, crtc, NULL, NULL, NULL, NULL);
    CHECK(active == 0, "GetCrtcConfine inactive after reset");
    fake_motion(c, root, px, py);
    xcb_flush(c);
    query_pointer(c, root, &rx, &ry);
    CHECK(rx == px && ry == py, "after reset motion is unclamped (%d,%d)", rx, ry);

    /* auto-revert: a second client sets a confine box then disconnects */
    xcb_connection_t *c2 = xcb_connect(NULL, NULL);
    set_confine(c2, crtc, cx, cy, halfw, ch);
    xcb_flush(c2);
    CHECK(get_confine(c, crtc, NULL, NULL, NULL, NULL) == 1,
          "confine active while 2nd client alive");
    xcb_disconnect(c2);
    /* give the server a round-trip to process the disconnect */
    free(xcb_query_pointer_reply(c, xcb_query_pointer(c, root), NULL));
    CHECK(get_confine(c, crtc, NULL, NULL, NULL, NULL) == 0,
          "CRTC auto-reverted to unconfined on client disconnect");

    xcb_disconnect(c);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
