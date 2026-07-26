/*
 * xis-present-flip-test - a black-box harness for the per-CRTC Present flip work
 *
 * Drives Present's flip-eligibility gate deterministically and reports, per
 * frame, whether the server *flipped* (zero-copy scanout) or *copied*, by
 * reading the `mode` field of each PresentCompleteNotify -- no server-log
 * grepping required.
 *
 * Modes:
 *   (default / --whole-root) present a root-sized buffer to a window covering
 *       the entire X screen. Expect FLIP (same path a fullscreen game takes).
 *   (--crtc N)   present a CRTC-sized buffer to a window covering only active
 *       CRTC N, passing that CRTC as target_crtc. Expect FLIP once per-CRTC
 *       flips are supported by the server + driver.
 *   (--all)      one per-CRTC window on every active CRTC, all presenting each
 *       round so multiple CRTCs have flips in flight *simultaneously*. This is
 *       the concurrent per-CRTC case a per-output compositor (kwin) exercises;
 *       each CRTC should independently report FLIP.
 *
 * Buffers are GBM scanout BOs imported via DRI3, so the driver's buffer-format
 * flip check passes and the region/geometry gate is the only variable.
 *
 * Run WITHOUT a compositor so the override-redirect test windows are not
 * Composite-redirected (otherwise the flip is refused before the region gate).
 *
 * Build:
 *   cc xis-present-flip-test.c -o xis-present-flip-test \
 *      $(pkg-config --cflags --libs xcb xcb-present xcb-dri3 xcb-randr gbm)
 *
 * Usage:
 *   ./xis-present-flip-test --list             # list active CRTCs and exit
 *   ./xis-present-flip-test                     # whole-root baseline
 *   ./xis-present-flip-test --crtc 0            # per-CRTC test on CRTC 0
 *   ./xis-present-flip-test --all               # concurrent per-CRTC, all CRTCs
 *   ./xis-present-flip-test --all --frames 600
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>

#include <xcb/xcb.h>
#include <xcb/present.h>
#include <xcb/dri3.h>
#include <xcb/randr.h>
#include <gbm.h>

#define MAX_OUTPUTS 64

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

/* One test window on one CRTC (or the whole root), with its own swapchain and
 * Present event stream. */
struct output {
    const char *label;
    int16_t x, y;
    uint16_t w, h;
    xcb_randr_crtc_t target_crtc;   /* XCB_NONE for whole-root */
    xcb_window_t win;
    struct fb fbs[2];
    uint32_t eid;
    xcb_special_event_t *se;
    unsigned counts[4];             /* indexed by PresentCompleteMode */

    /* Independent (self-paced) driving state. */
    uint32_t inflight;              /* serial of the frame currently in flight */
    unsigned presented;            /* frames presented (also next serial) */
    unsigned completed;            /* frames whose completion we've counted */
    double t_start;                 /* time of first present */
    double t_done;                  /* time the frames-th completion arrived */
};

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

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

/* Two alternating colours per output so consecutive frames differ (no SKIP)
 * and a real flip is visible as the screen alternating colour. */
static const uint32_t colours[2][2] = {
    { 0x00203a8c, 0x00238c3a },   /* blue / green   */
    { 0x008c2030, 0x008c8020 },   /* red  / yellow  */
};

static void
setup_output(xcb_connection_t *conn, struct gbm_device *gbm, xcb_window_t root,
             xcb_screen_t *screen, uint8_t depth, uint8_t bpp,
             struct output *out, int idx)
{
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
    uint32_t values[2] = { screen->black_pixel, 1 };

    out->win = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, out->win, root,
                      out->x, out->y, out->w, out->h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_COPY_FROM_PARENT, mask, values);
    xcb_map_window(conn, out->win);
    uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, out->win, XCB_CONFIG_WINDOW_STACK_MODE, &above);

    out->eid = xcb_generate_id(conn);
    xcb_present_select_input(conn, out->eid, out->win,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                             XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
    out->se = xcb_register_for_special_xge(conn, &xcb_present_id, out->eid, NULL);
    if (!out->se)
        die("xcb_register_for_special_xge failed");

    out->fbs[0] = create_fb(conn, gbm, root, out->w, out->h, depth, bpp,
                            colours[idx % 2][0]);
    out->fbs[1] = create_fb(conn, gbm, root, out->w, out->h, depth, bpp,
                            colours[idx % 2][1]);
}

/* Present this output's next frame (asap: target_msc 0), flipping between its
 * two buffers. Records the in-flight serial so its completion can be matched. */
static void
present_next(xcb_connection_t *conn, struct output *out)
{
    struct fb *cur = &out->fbs[out->presented & 1];

    out->inflight = out->presented;
    xcb_present_pixmap(conn, out->win, cur->pixmap,
                       out->presented,
                       XCB_NONE, XCB_NONE,   /* valid, update */
                       0, 0,                 /* x_off, y_off */
                       out->target_crtc,
                       XCB_NONE, XCB_NONE,   /* wait_fence, idle_fence */
                       0,                    /* options: allow flip */
                       0, 0, 0,              /* target_msc, divisor, remainder */
                       0, NULL);             /* notifies */
    out->presented++;
}

int main(int argc, char **argv)
{
    int want_crtc = -1;        /* -1 == whole-root mode */
    int do_all = 0;
    int frames = 240;
    int do_list = 0;

    static struct option opts[] = {
        { "crtc",       required_argument, 0, 'c' },
        { "all",        no_argument,       0, 'a' },
        { "frames",     required_argument, 0, 'f' },
        { "whole-root", no_argument,       0, 'w' },
        { "list",       no_argument,       0, 'l' },
        { "help",       no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:af:wlh", opts, NULL)) != -1) {
        switch (opt) {
        case 'c': want_crtc = atoi(optarg); break;
        case 'a': do_all = 1; break;
        case 'f': frames = atoi(optarg); break;
        case 'w': want_crtc = -1; break;
        case 'l': do_list = 1; break;
        case 'h':
        default:
            printf("usage: %s [--whole-root | --crtc N | --all] "
                   "[--frames N] [--list]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    if (!conn || xcb_connection_has_error(conn))
        die("cannot connect to X server (is DISPLAY set?)");

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    xcb_window_t root = screen->root;
    uint8_t depth = screen->root_depth;
    uint8_t bpp = 32;
    if (depth != 24)
        fprintf(stderr, "warning: root depth is %u, not 24; "
                        "XRGB8888 buffers assume depth 24\n", depth);

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

    struct crtc_rect active[MAX_OUTPUTS];
    int nactive = 0;
    for (int i = 0; i < ncrtc && nactive < MAX_OUTPUTS; i++) {
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

    /* Build the set of outputs to drive. */
    struct output outs[MAX_OUTPUTS] = { 0 };
    int nout = 0;

    if (do_all) {
        if (nactive == 0)
            die("--all: no active CRTCs");
        for (int i = 0; i < nactive; i++) {
            outs[i].label = "per-CRTC";
            outs[i].x = active[i].x;
            outs[i].y = active[i].y;
            outs[i].w = active[i].w;
            outs[i].h = active[i].h;
            outs[i].target_crtc = active[i].xid;
        }
        nout = nactive;
        printf("mode: ALL (%d CRTCs presenting concurrently, expect per-CRTC FLIP)\n",
               nout);
    } else if (want_crtc < 0) {
        outs[0].label = "whole-root";
        outs[0].x = 0;
        outs[0].y = 0;
        outs[0].w = screen->width_in_pixels;
        outs[0].h = screen->height_in_pixels;
        outs[0].target_crtc = XCB_NONE;
        nout = 1;
        printf("mode: WHOLE-ROOT %ux%u+0+0 (expect FLIP)\n", outs[0].w, outs[0].h);
    } else {
        if (want_crtc >= nactive)
            die("--crtc index out of range (see --list)");
        outs[0].label = "per-CRTC";
        outs[0].x = active[want_crtc].x;
        outs[0].y = active[want_crtc].y;
        outs[0].w = active[want_crtc].w;
        outs[0].h = active[want_crtc].h;
        outs[0].target_crtc = active[want_crtc].xid;
        nout = 1;
        printf("mode: PER-CRTC %d  %ux%u+%d+%d  target_crtc 0x%08x (expect FLIP)\n",
               want_crtc, outs[0].w, outs[0].h, outs[0].x, outs[0].y,
               outs[0].target_crtc);
    }

    for (int i = 0; i < nout; i++)
        setup_output(conn, gbm, root, screen, depth, bpp, &outs[i], i);

    /* Kick off the first frame on every output, then let each output pace
     * itself: as soon as an output's CompleteNotify arrives, present its next
     * frame. Outputs are never made to wait for each other, so each flips at
     * its own CRTC's vblank rate -- the independent per-output pacing that
     * per-CRTC flips exist to provide. Every output keeps animating until the
     * slowest has reached 'frames', so both monitors stay live throughout. */
    double t0 = now_sec();
    for (int i = 0; i < nout; i++) {
        outs[i].t_start = t0;
        present_next(conn, &outs[i]);
    }
    xcb_flush(conn);

    int fd = xcb_get_file_descriptor(conn);
    for (;;) {
        int all_done = 1;
        for (int i = 0; i < nout; i++)
            if (outs[i].completed < (unsigned) frames)
                all_done = 0;
        if (all_done)
            break;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 1000) < 0)
            die("poll failed");

        int need_flush = 0;
        for (int i = 0; i < nout; i++) {
            struct output *o = &outs[i];
            xcb_generic_event_t *ev;

            while ((ev = xcb_poll_for_special_event(conn, o->se)) != NULL) {
                xcb_present_generic_event_t *ge = (void *) ev;
                if (ge->evtype == XCB_PRESENT_COMPLETE_NOTIFY) {
                    xcb_present_complete_notify_event_t *ce = (void *) ev;
                    if (ce->serial == o->inflight) {
                        if (o->completed < (unsigned) frames) {
                            if (ce->mode < 4)
                                o->counts[ce->mode]++;
                            if (++o->completed == (unsigned) frames)
                                o->t_done = now_sec();
                        }
                        /* Keep animating until every output is done. */
                        present_next(conn, o);
                        need_flush = 1;
                    }
                }
                free(ev);
            }
        }
        if (need_flush)
            xcb_flush(conn);
        if (xcb_connection_has_error(conn))
            die("connection dropped");
    }

    printf("\nsummary over %d frames per output:\n", frames);
    int all_flipped = 1;
    for (int i = 0; i < nout; i++) {
        struct output *o = &outs[i];
        double secs = o->t_done - o->t_start;
        double fps = secs > 0 ? frames / secs : 0;
        printf("  out[%d] %-10s (crtc 0x%08x %ux%u+%d+%d): "
               "FLIP=%u COPY=%u SKIP=%u SUBOPTIMAL=%u  |  %.1f fps (%.2fs)\n",
               i, o->label, o->target_crtc, o->w, o->h, o->x, o->y,
               o->counts[XCB_PRESENT_COMPLETE_MODE_FLIP],
               o->counts[XCB_PRESENT_COMPLETE_MODE_COPY],
               o->counts[XCB_PRESENT_COMPLETE_MODE_SKIP],
               o->counts[XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY],
               fps, secs);
        if (o->counts[XCB_PRESENT_COMPLETE_MODE_FLIP] == 0)
            all_flipped = 0;
    }

    if (all_flipped)
        printf("verdict: every output took the FLIP path; per-output fps above "
               "should track each CRTC's own refresh rate.\n");
    else
        printf("verdict: at least one output never flipped (see COPY counts).\n");

    xcb_disconnect(conn);
    return 0;
}
