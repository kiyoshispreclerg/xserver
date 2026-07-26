/*
 * xis-present-flip-test - a black-box harness for the per-CRTC Present flip work
 *
 * Drives the Present flip-eligibility gate deterministically and reports,
 * per frame, whether the server *flipped* (zero-copy scanout) or *copied*,
 * by reading the `mode` field of each PresentCompleteNotify -- no server-log
 * grepping required.
 *
 * Two modes:
 *   (default / --whole-root) present a root-sized buffer to a window covering
 *       the entire X screen. Expected: FLIP today (baseline, same path a
 *       fullscreen game or the compiz cube takes).
 *   (--crtc N)               present a CRTC-sized buffer to a window covering
 *       only active CRTC N, passing that CRTC as target_crtc. Expected: COPY
 *       today (blocked by present_check_flip()'s whole-root region gate);
 *       should become FLIP once that gate is relaxed per-CRTC.
 *
 * Buffers are GBM scanout BOs imported via DRI3, so the driver's buffer-format
 * flip check passes and the region/geometry gate is the only variable.
 *
 * Run WITHOUT a compositor so the override-redirect test window is not
 * Composite-redirected (otherwise the flip is refused before the region gate).
 *
 * Build:
 *   cc xis-present-flip-test.c -o xis-present-flip-test \
 *      $(pkg-config --cflags --libs xcb xcb-present xcb-dri3 xcb-randr gbm)
 *
 * Usage:
 *   ./xis-present-flip-test --list            # list active CRTCs and exit
 *   ./xis-present-flip-test                    # whole-root baseline
 *   ./xis-present-flip-test --crtc 0           # per-CRTC test on CRTC 0
 *   ./xis-present-flip-test --crtc 0 --frames 600
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/mman.h>

#include <xcb/xcb.h>
#include <xcb/present.h>
#include <xcb/dri3.h>
#include <xcb/randr.h>
#include <gbm.h>

static void die(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

struct crtc_rect {
    xcb_randr_crtc_t xid;
    int16_t x, y;
    uint16_t w, h;
};

struct fb {
    struct gbm_bo *bo;
    xcb_pixmap_t pixmap;
};

/* Allocate a linear scanout GBM buffer, fill it with a solid colour, and
 * import it as an X pixmap through DRI3. */
static struct fb
create_fb(xcb_connection_t *conn, struct gbm_device *gbm, xcb_window_t root,
          uint16_t w, uint16_t h, uint8_t depth, uint8_t bpp, uint32_t color)
{
    struct fb fb = { 0 };

    fb.bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_XRGB8888,
                          GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    if (!fb.bo)
        die("gbm_bo_create failed (no scanout-capable linear buffer)");

    /* Fill so a real flip is visibly distinct from the previous frame. */
    uint32_t map_stride = 0;
    void *map_data = NULL;
    void *ptr = gbm_bo_map(fb.bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE,
                           &map_stride, &map_data);
    if (ptr != MAP_FAILED && ptr != NULL) {
        for (uint16_t y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)((char *)ptr + (size_t)y * map_stride);
            for (uint16_t x = 0; x < w; x++)
                row[x] = color;
        }
        gbm_bo_unmap(fb.bo, map_data);
    }

    uint32_t stride = gbm_bo_get_stride(fb.bo);
    int fd = gbm_bo_get_fd(fb.bo);
    if (fd < 0)
        die("gbm_bo_get_fd failed");

    fb.pixmap = xcb_generate_id(conn);
    xcb_dri3_pixmap_from_buffer(conn, fb.pixmap, root,
                                stride * h, w, h, stride, depth, bpp, fd);
    return fb;
}

static const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case XCB_PRESENT_COMPLETE_MODE_COPY:            return "COPY";
    case XCB_PRESENT_COMPLETE_MODE_FLIP:            return "FLIP";
    case XCB_PRESENT_COMPLETE_MODE_SKIP:            return "SKIP";
    case XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY: return "SUBOPTIMAL_COPY";
    default:                                        return "UNKNOWN";
    }
}

int main(int argc, char **argv)
{
    int want_crtc = -1;        /* -1 == whole-root mode */
    int frames = 240;
    int do_list = 0;

    static struct option opts[] = {
        { "crtc",       required_argument, 0, 'c' },
        { "frames",     required_argument, 0, 'f' },
        { "whole-root", no_argument,       0, 'w' },
        { "list",       no_argument,       0, 'l' },
        { "help",       no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:f:wlh", opts, NULL)) != -1) {
        switch (opt) {
        case 'c': want_crtc = atoi(optarg); break;
        case 'f': frames = atoi(optarg); break;
        case 'w': want_crtc = -1; break;
        case 'l': do_list = 1; break;
        case 'h':
        default:
            printf("usage: %s [--whole-root | --crtc N] [--frames N] [--list]\n",
                   argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    if (!conn || xcb_connection_has_error(conn))
        die("cannot connect to X server (is DISPLAY set?)");

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    xcb_window_t root = screen->root;
    uint8_t depth = screen->root_depth;   /* pixmap/window depth must match */
    uint8_t bpp = 32;
    if (depth != 24)
        fprintf(stderr, "warning: root depth is %u, not 24; "
                        "XRGB8888 buffers assume depth 24\n", depth);

    /* Extension version negotiation. */
    xcb_present_query_version_reply_t *pv =
        xcb_present_query_version_reply(conn,
            xcb_present_query_version(conn, 1, 0), NULL);
    if (!pv)
        die("Present extension not available");
    free(pv);

    xcb_dri3_query_version_reply_t *dv =
        xcb_dri3_query_version_reply(conn,
            xcb_dri3_query_version(conn, 1, 0), NULL);
    if (!dv)
        die("DRI3 extension not available");
    free(dv);

    /* Open the DRM device DRI3 hands us and wrap it in GBM. */
    xcb_dri3_open_reply_t *open =
        xcb_dri3_open_reply(conn, xcb_dri3_open(conn, root, 0), NULL);
    if (!open || xcb_dri3_open_reply_fds(conn, open) == NULL)
        die("xcb_dri3_open failed");
    int drm_fd = xcb_dri3_open_reply_fds(conn, open)[0];
    free(open);

    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm)
        die("gbm_create_device failed");

    /* Enumerate active CRTCs. */
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res)
        die("RandR GetScreenResourcesCurrent failed");

    int ncrtc = xcb_randr_get_screen_resources_current_crtcs_length(res);
    xcb_randr_crtc_t *crtcs =
        xcb_randr_get_screen_resources_current_crtcs(res);

    struct crtc_rect active[64];
    int nactive = 0;
    for (int i = 0; i < ncrtc && nactive < 64; i++) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, crtcs[i], res->config_timestamp),
                NULL);
        if (!ci)
            continue;
        if (ci->mode != 0 && ci->width > 0 && ci->height > 0) {
            active[nactive].xid = crtcs[i];
            active[nactive].x = ci->x;
            active[nactive].y = ci->y;
            active[nactive].w = ci->width;
            active[nactive].h = ci->height;
            nactive++;
        }
        free(ci);
    }
    free(res);

    printf("root: %ux%u depth %u; %d active CRTC(s):\n",
           screen->width_in_pixels, screen->height_in_pixels, depth, nactive);
    for (int i = 0; i < nactive; i++)
        printf("  crtc[%d] xid 0x%08x  %ux%u+%d+%d\n", i,
               active[i].xid, active[i].w, active[i].h, active[i].x, active[i].y);

    if (do_list) {
        xcb_disconnect(conn);
        return 0;
    }

    /* Decide the target rectangle and target_crtc. */
    int16_t rx, ry;
    uint16_t rw, rh;
    xcb_randr_crtc_t target_crtc;
    if (want_crtc < 0) {
        rx = 0; ry = 0;
        rw = screen->width_in_pixels;
        rh = screen->height_in_pixels;
        target_crtc = XCB_NONE;     /* server picks the CRTC for the window */
        printf("mode: WHOLE-ROOT  rect %ux%u+0+0  (expect FLIP today)\n", rw, rh);
    } else {
        if (want_crtc >= nactive)
            die("--crtc index out of range (see --list)");
        rx = active[want_crtc].x;
        ry = active[want_crtc].y;
        rw = active[want_crtc].w;
        rh = active[want_crtc].h;
        target_crtc = active[want_crtc].xid;
        printf("mode: PER-CRTC %d  rect %ux%u+%d+%d  target_crtc 0x%08x  "
               "(expect COPY today, FLIP after the gate is relaxed)\n",
               want_crtc, rw, rh, rx, ry, target_crtc);
    }

    /* Override-redirect window exactly covering the target rectangle. */
    xcb_window_t win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
    uint32_t values[2] = { screen->black_pixel, 1 };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, root,
                      rx, ry, rw, rh, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_COPY_FROM_PARENT, mask, values);
    xcb_map_window(conn, win);
    uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    xcb_flush(conn);

    /* Subscribe to Present completion/idle events for this window. */
    uint32_t eid = xcb_generate_id(conn);
    xcb_present_select_input(conn, eid, win,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                             XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
    xcb_special_event_t *se =
        xcb_register_for_special_xge(conn, &xcb_present_id, eid, NULL);
    if (!se)
        die("xcb_register_for_special_xge failed");

    /* Two alternating buffers so consecutive frames differ (no SKIP) and a
     * real flip is visible as the screen alternating colour. */
    struct fb fbs[2];
    fbs[0] = create_fb(conn, gbm, root, rw, rh, depth, bpp, 0x00203a8c); /* blue */
    fbs[1] = create_fb(conn, gbm, root, rw, rh, depth, bpp, 0x00238c3a); /* green */
    xcb_flush(conn);

    unsigned counts[4] = { 0, 0, 0, 0 };
    for (int f = 0; f < frames; f++) {
        struct fb *cur = &fbs[f & 1];
        xcb_present_pixmap(conn, win, cur->pixmap,
                           (uint32_t) f,       /* serial */
                           XCB_NONE,           /* valid */
                           XCB_NONE,           /* update */
                           0, 0,               /* x_off, y_off */
                           target_crtc,
                           XCB_NONE, XCB_NONE, /* wait_fence, idle_fence */
                           0,                  /* options: allow flip */
                           0, 0, 0,            /* target_msc, divisor, remainder */
                           0, NULL);           /* notifies */
        xcb_flush(conn);

        /* Wait for this frame's completion; report its mode. */
        for (;;) {
            xcb_generic_event_t *ev = xcb_wait_for_special_event(conn, se);
            if (!ev)
                die("connection dropped while waiting for Present events");
            xcb_present_generic_event_t *ge = (void *) ev;
            if (ge->evtype == XCB_PRESENT_COMPLETE_NOTIFY) {
                xcb_present_complete_notify_event_t *ce = (void *) ev;
                if (ce->serial == (uint32_t) f) {
                    if (ce->mode < 4)
                        counts[ce->mode]++;
                    if (f < 5 || f == frames - 1)
                        printf("  frame %4d: %s (msc %llu)\n", f,
                               mode_name(ce->mode),
                               (unsigned long long) ce->msc);
                    free(ev);
                    break;
                }
            }
            free(ev);   /* ignore idle notifies and stale completions */
        }
    }

    printf("\nsummary over %d frames: FLIP=%u COPY=%u SKIP=%u SUBOPTIMAL=%u\n",
           frames, counts[XCB_PRESENT_COMPLETE_MODE_FLIP],
           counts[XCB_PRESENT_COMPLETE_MODE_COPY],
           counts[XCB_PRESENT_COMPLETE_MODE_SKIP],
           counts[XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY]);

    if (counts[XCB_PRESENT_COMPLETE_MODE_FLIP] > 0)
        printf("verdict: server took the FLIP path.\n");
    else
        printf("verdict: no flips -- server copied every frame.\n");

    xcb_disconnect(conn);
    return 0;
}
