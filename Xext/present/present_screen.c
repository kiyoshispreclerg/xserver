/*
 * Copyright © 2013 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */
#include <dix-config.h>

#include "dix/screen_hooks_priv.h"
#include "dix/screenint_priv.h"
#include "miext/extinit_priv.h"
#include "Xext/present/present_priv.h"

#include "gcstruct.h"

#define PRESENT_WRAP_HOOK(priv,real,mem,func) {\
    (priv)->mem = (real)->mem; \
    (real)->mem = (func); \
}

#define PRESENT_UNWRAP_HOOK(priv,real,mem) {\
    (real)->mem = (priv)->mem; \
}

int present_request;
DevPrivateKeyRec present_screen_private_key;
DevPrivateKeyRec present_window_private_key;

/*
 * Get a pointer to a present window private, creating if necessary
 */
present_window_priv_ptr
present_get_window_priv(WindowPtr window, Bool create)
{
    present_window_priv_ptr window_priv = present_window_priv(window);

    if (!create || window_priv != NULL)
        return window_priv;
    window_priv = calloc (1, sizeof (present_window_priv_rec));
    if (!window_priv)
        return NULL;
    xorg_list_init(&window_priv->vblank);
    xorg_list_init(&window_priv->notifies);

    window_priv->window = window;
    window_priv->crtc = PresentCrtcNeverSet;
    dixSetPrivate(&window->devPrivates, &present_window_private_key, window_priv);
    return window_priv;
}

/*
 * Hook the close screen function to clean up our screen private
 */
static void present_close_screen(CallbackListPtr *pcbl, ScreenPtr screen, void *unused)
{
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);
    if (!screen_priv)
        return;

    if (screen_priv->flip_destroy)
        screen_priv->flip_destroy(screen);

    PRESENT_UNWRAP_HOOK(screen_priv, screen, GetImage);
    if (screen_priv->CreateGC)
        PRESENT_UNWRAP_HOOK(screen_priv, screen, CreateGC);

    dixScreenUnhookClose(screen, present_close_screen);
    dixSetPrivate(&screen->devPrivates, &present_screen_private_key, NULL);
    free(screen_priv->info);
    free(screen_priv);
}

/*
 * Free any queued presentations for this window
 */
static void
present_free_window_vblank(WindowPtr window)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_window_priv_ptr     window_priv = present_window_priv(window);
    present_vblank_ptr          vblank, tmp;

    xorg_list_for_each_entry_safe(vblank, tmp, &window_priv->vblank, window_list) {
        screen_priv->abort_vblank(window->drawable.pScreen, window, vblank->crtc, vblank->event_id, vblank->target_msc);
        present_vblank_destroy(vblank);
    }
}

/*
 * Hook the close window function to clean up our window private
 */
static void
present_destroy_window(CallbackListPtr *pcbl, ScreenPtr pScreen, WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);
    present_window_priv_ptr window_priv = present_window_priv(window);

    present_send_config_notify(window,
                               window->drawable.x,
                               window->drawable.y,
                               window->drawable.width,
                               window->drawable.height,
                               window->borderWidth,
                               window->nextSib,
                               PresentWindowDestroyed);

    if (window_priv) {
        present_clear_window_notifies(window);
        present_free_events(window);
        present_free_window_vblank(window);

        screen_priv->clear_window_flip(window);

        free(window_priv);
    }
}

/*
 * Hook the config notify screen function to deliver present config notify events
 */
static int
present_config_notify(WindowPtr window,
                   int x, int y, int w, int h, int bw,
                   WindowPtr sibling)
{
    ScreenPtr screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    present_send_config_notify(window, x, y, w, h, bw, sibling, 0);

    PRESENT_UNWRAP_HOOK(screen_priv, screen, ConfigNotify);

    int ret;
    if (screen->ConfigNotify)
        ret = screen->ConfigNotify (window, x, y, w, h, bw, sibling);
    else
        ret = 0;

    PRESENT_WRAP_HOOK(screen_priv, screen, ConfigNotify, present_config_notify);
    return ret;
}

/*
 * Hook the clip notify screen function to un-flip as necessary
 */

static void
present_clip_notify(WindowPtr window, int dx, int dy)
{
    ScreenPtr screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    screen_priv->check_flip_window(window);
    PRESENT_UNWRAP_HOOK(screen_priv, screen, ClipNotify)
    if (screen->ClipNotify)
        screen->ClipNotify (window, dx, dy);
    PRESENT_WRAP_HOOK(screen_priv, screen, ClipNotify, present_clip_notify);
}

/*
 * Hook GetImage so a root screen capture (XGetImage/XShmGetImage) sees content
 * that is currently page-flipped per CRTC -- which lives in the flip buffers,
 * not the screen pixmap the default GetImage reads.
 */
static void
present_get_image(DrawablePtr pDrawable, int sx, int sy, int w, int h,
                  unsigned int format, unsigned long planeMask, char *pdstLine)
{
    ScreenPtr screen = pDrawable->pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    PRESENT_UNWRAP_HOOK(screen_priv, screen, GetImage);
    /* If a per-CRTC flip overlaps this root capture, read the non-flipped
     * remainder from the screen pixmap and stitch the flipped regions in from
     * the flip buffers -- without reading the flipped area twice. Otherwise
     * (the common case) fall back to a plain full read. Done while unwrapped so
     * the underlying reads use the original GetImage. */
    if (!present_flip_getimage(pDrawable, sx, sy, w, h, format, planeMask,
                               pdstLine))
        (*screen->GetImage)(pDrawable, sx, sy, w, h, format, planeMask, pdstLine);
    PRESENT_WRAP_HOOK(screen_priv, screen, GetImage, present_get_image);
}

/*
 * GC wrap so a CopyArea from the root with IncludeInferiors (the core-protocol
 * way to grab "the screen" into a pixmap) sees content that is page-flipped
 * per CRTC, like present_get_image does for GetImage. Only installed on screens
 * whose driver can flip per CRTC.
 *
 * To keep the per-operation cost off ordinary rendering, Present's ops are
 * installed only on GCs whose subwindow-mode is IncludeInferiors (the only
 * ones that can read through to the root's contents); every other GC keeps
 * its own ops untouched and only pays the GCFuncs indirection on state
 * changes. The wrapped ops are the lower ops with just CopyArea replaced.
 */
typedef struct {
    const GCFuncs   *funcs;
    const GCOps     *ops;           /* lower ops while ours are installed, else NULL */
    GCOps           wrapped_ops;    /* lower ops with CopyArea replaced */
} present_gc_priv_rec, *present_gc_priv_ptr;

static DevPrivateKeyRec present_gc_private_key;

static inline present_gc_priv_ptr
present_gc_priv(GCPtr gc)
{
    return dixLookupPrivate(&gc->devPrivates, &present_gc_private_key);
}

static void present_validate_gc(GCPtr gc, unsigned long changes, DrawablePtr pDrawable);
static void present_change_gc(GCPtr gc, unsigned long mask);
static void present_copy_gc(GCPtr src, unsigned long mask, GCPtr dst);
static void present_destroy_gc(GCPtr gc);
static void present_change_clip(GCPtr gc, int type, void *pvalue, int nrects);
static void present_destroy_clip(GCPtr gc);
static void present_copy_clip(GCPtr dst, GCPtr src);

static const GCFuncs present_gc_funcs = {
    present_validate_gc, present_change_gc, present_copy_gc, present_destroy_gc,
    present_change_clip, present_destroy_clip, present_copy_clip
};

static RegionPtr present_gc_copy_area(DrawablePtr pSrc, DrawablePtr pDst, GCPtr gc,
                                      int srcx, int srcy, int w, int h,
                                      int dstx, int dsty);

/* Restore the lower layer's funcs (and ops, if ours are installed). */
static void
present_gc_unwrap(present_gc_priv_ptr priv, GCPtr gc)
{
    gc->funcs = priv->funcs;
    if (priv->ops)
        gc->ops = priv->ops;
}

/* Re-install our funcs, and our ops only if the GC is IncludeInferiors. The
 * lower layer may have switched its ops table, so refresh the copy. */
static void
present_gc_wrap(present_gc_priv_ptr priv, GCPtr gc)
{
    priv->funcs = gc->funcs;
    gc->funcs = &present_gc_funcs;

    if (gc->subWindowMode == IncludeInferiors) {
        priv->ops = gc->ops;
        priv->wrapped_ops = *gc->ops;
        priv->wrapped_ops.CopyArea = present_gc_copy_area;
        gc->ops = &priv->wrapped_ops;
    } else
        priv->ops = NULL;
}

static void
present_validate_gc(GCPtr gc, unsigned long changes, DrawablePtr pDrawable)
{
    present_gc_priv_ptr priv = present_gc_priv(gc);

    present_gc_unwrap(priv, gc);
    (*gc->funcs->ValidateGC) (gc, changes, pDrawable);
    present_gc_wrap(priv, gc);
}

static void
present_change_gc(GCPtr gc, unsigned long mask)
{
    present_gc_priv_ptr priv = present_gc_priv(gc);

    present_gc_unwrap(priv, gc);
    (*gc->funcs->ChangeGC) (gc, mask);
    present_gc_wrap(priv, gc);
}

static void
present_copy_gc(GCPtr src, unsigned long mask, GCPtr dst)
{
    present_gc_priv_ptr priv = present_gc_priv(dst);

    present_gc_unwrap(priv, dst);
    (*dst->funcs->CopyGC) (src, mask, dst);
    present_gc_wrap(priv, dst);
}

static void
present_destroy_gc(GCPtr gc)
{
    present_gc_priv_ptr priv = present_gc_priv(gc);

    /* The GC is freed right after; no need to re-install anything. */
    present_gc_unwrap(priv, gc);
    (*gc->funcs->DestroyGC) (gc);
}

static void
present_change_clip(GCPtr gc, int type, void *pvalue, int nrects)
{
    present_gc_priv_ptr priv = present_gc_priv(gc);

    present_gc_unwrap(priv, gc);
    (*gc->funcs->ChangeClip) (gc, type, pvalue, nrects);
    present_gc_wrap(priv, gc);
}

static void
present_destroy_clip(GCPtr gc)
{
    present_gc_priv_ptr priv = present_gc_priv(gc);

    present_gc_unwrap(priv, gc);
    (*gc->funcs->DestroyClip) (gc);
    present_gc_wrap(priv, gc);
}

static void
present_copy_clip(GCPtr dst, GCPtr src)
{
    present_gc_priv_ptr priv = present_gc_priv(dst);

    present_gc_unwrap(priv, dst);
    (*dst->funcs->CopyClip) (dst, src);
    present_gc_wrap(priv, dst);
}

static RegionPtr
present_gc_copy_area(DrawablePtr pSrc, DrawablePtr pDst, GCPtr gc,
                     int srcx, int srcy, int w, int h, int dstx, int dsty)
{
    present_gc_priv_ptr priv = present_gc_priv(gc);
    RegionPtr exposed;

    present_gc_unwrap(priv, gc);
    /* A root copy overlapping a per-CRTC flip is split between the root and
     * the flip buffers; anything else is a plain CopyArea. */
    if (!present_flip_copy_area(pSrc, pDst, gc, srcx, srcy, w, h, dstx, dsty,
                                &exposed))
        exposed = (*gc->ops->CopyArea) (pSrc, pDst, gc, srcx, srcy, w, h,
                                        dstx, dsty);
    present_gc_wrap(priv, gc);
    return exposed;
}

static Bool
present_create_gc(GCPtr gc)
{
    ScreenPtr screen = gc->pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);
    Bool ret;

    PRESENT_UNWRAP_HOOK(screen_priv, screen, CreateGC);
    ret = (*screen->CreateGC) (gc);
    if (ret) {
        present_gc_priv_ptr priv = present_gc_priv(gc);

        priv->ops = NULL;
        priv->funcs = gc->funcs;
        gc->funcs = &present_gc_funcs;
    }
    PRESENT_WRAP_HOOK(screen_priv, screen, CreateGC, present_create_gc);
    return ret;
}

Bool
present_screen_register_priv_keys(void)
{
    if (!dixRegisterPrivateKey(&present_screen_private_key, PRIVATE_SCREEN, 0))
        return FALSE;

    if (!dixRegisterPrivateKey(&present_window_private_key, PRIVATE_WINDOW, 0))
        return FALSE;

    return TRUE;
}

present_screen_priv_ptr
present_screen_priv_init(ScreenPtr screen)
{
    present_screen_priv_ptr screen_priv = calloc(1, sizeof (present_screen_priv_rec));
    if (!screen_priv)
        return NULL;

    xorg_list_init(&screen_priv->flip_states);

    dixScreenHookWindowDestroy(screen, present_destroy_window);
    dixScreenHookClose(screen, present_close_screen);

    PRESENT_WRAP_HOOK(screen_priv, screen, ConfigNotify, present_config_notify);
    PRESENT_WRAP_HOOK(screen_priv, screen, ClipNotify, present_clip_notify);
    PRESENT_WRAP_HOOK(screen_priv, screen, GetImage, present_get_image);

    dixSetPrivate(&screen->devPrivates, &present_screen_private_key, screen_priv);
    screen_priv->pScreen = screen;

    return screen_priv;
}

static int
check_flip_visit(WindowPtr window, void *data)
{
    ScreenPtr screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    if (!screen_priv)
        return WT_DONTWALKCHILDREN;

    screen_priv->check_flip_window(window);

    return WT_WALKCHILDREN;
}

void
present_check_flips(WindowPtr window)
{
    TraverseTree(window, check_flip_visit, NULL);
}

/*
 * Initialize a screen for use with present in default screen flip mode (scmd)
 */
int
present_screen_init(ScreenPtr screen, present_screen_info_ptr info)
{
    if (!present_screen_register_priv_keys())
        return FALSE;

    if (!present_screen_priv(screen)) {
        present_screen_priv_ptr screen_priv = present_screen_priv_init(screen);
        if (!screen_priv)
            return FALSE;

        if (info) {
            screen_priv->info = malloc(sizeof(*info));
            if (!screen_priv->info) {
                return FALSE;
            }
            *screen_priv->info = *info;
        } else {
            screen_priv->info = NULL;
        }
        present_scmd_init_mode_hooks(screen_priv);

        /* Root CopyArea substitution is only needed where per-CRTC flips can
         * happen. Called from the DDX's ScreenInit, before any GC exists, so
         * the GC private can still be registered here. */
        if (info && info->version >= 2 && info->capable_flip_crtc) {
            if (!dixRegisterPrivateKey(&present_gc_private_key, PRIVATE_GC,
                                       sizeof(present_gc_priv_rec)))
                return FALSE;
            PRESENT_WRAP_HOOK(screen_priv, screen, CreateGC, present_create_gc);
        }

        present_fake_screen_init(screen);
    }

    return TRUE;
}

/*
 * Initialize the present extension
 */
void
present_extension_init(void)
{
    if (PanoramiXIsEnabled()) {
        return;
    }

    ExtensionEntry *extension = AddExtension(
                             PRESENT_NAME, PresentNumberEvents, PresentNumberErrors,
                             proc_present_dispatch, sproc_present_dispatch,
                             NULL, StandardMinorOpcode);
    if (!extension)
        goto bail;

    present_request = extension->base;

    if (!present_init())
        goto bail;

    if (!present_event_init())
        goto bail;

    DIX_FOR_EACH_SCREEN({
        if (!present_screen_init(walkScreen, NULL))
            goto bail;
    });

    return;

bail:
    FatalError("Cannot initialize Present extension");
}
