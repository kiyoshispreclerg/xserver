# Per-CRTC page flips for Present

**Per-CRTC page flips** let a client (typically a compositing window manager) hand
the X server **one buffer per output** and have the server page-flip each buffer to
its own CRTC, at that CRTC's own vblank — instead of the pre-existing all-or-nothing
model where a page flip requires **one buffer that covers the whole X screen and is
flipped to every CRTC at once**.

This is a soft-fork (**XiS**) addition on top of XLibre. It extends the Present
extension (no new protocol) plus the `modesetting` DDX as the first driver to
implement it. Other drivers (amdgpu, nouveau, intel, …) can opt in with the driver
contract described below; until they do, they simply keep getting whole-screen flips
and per-CRTC requests fall back to a copy — so the change is **safe by construction**.

> ⚠️ This is young, evolving code. It is dormant unless a driver opts in *and* a
> client presents a single-CRTC-sized buffer with a `target_crtc`. Expect bugs on
> the edges (see [Known limitations](#known-limitations)). Use with care.


## Requirements

At runtime, per-CRTC flips need:

- An **atomic-capable KMS driver** — the kernel DRM driver must recognize
  `DRM_CLIENT_CAP_ATOMIC` (`ms->atomic_modeset_capable`). Atomic KMS has been in
  mainline Linux since the ~4.2 era, and every current amdgpu/i915/nouveau is
  atomic-capable, so in practice any contemporary kernel qualifies. This gates whether
  a per-CRTC flip is *offered* at all (`capable_flip_crtc`, §5.1).
- **`Option "Atomic" "True"`** in the modesetting `Device` section (i.e.
  `ms->atomic_modeset`). In practice this is required for a real compositor:
  with the option **off**, the flip runs via the legacy `drmModePageFlip` path, which
  only accepts a **linear** per-CRTC buffer — it returns `EINVAL` on the **tiled**
  scanout buffers Mesa hands a normal GL client, so a compositor's per-CRTC flips fail
  and fall back to a copy. The atomic commit path sets the framebuffer, source
  coordinates and plane props coherently in one go, which is what a per-CRTC flip to a
  different-stride, tiled buffer needs. (A standalone client that allocates
  `GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR` can flip on the legacy path too, but that is
  not how a compositor's buffers are allocated.) Note that modesetting's atomic support
  is experimental and off by default — see §8.
- **DRI3 + Present** on the client side, to hand the server scanout-capable pixmaps
  with a `target_crtc` (both long-standing: DRI3 1.0 / Present 1.0).
- **GBM** (`libgbm`/`libdrm`) for allocating the scanout buffers — the glamor path the
  server already uses.

These are baseline on any modern Mesa + libdrm stack; the feature does not depend on
an unusually new library version. This document does not assert exact minimum version
numbers — pin those against your specific driver's atomic conversion if you need them.


## 1. Why this exists

On X11 the **X server owns scanout**, not the compositor. The server can already
page-flip (zero-copy scanout), but the generic flip-eligibility gate only allows a
window that covers the **entire** X screen — which is how an unredirected fullscreen
game or video gets a zero-copy flip today.

A compositing WM that renders and presents **per output** (its own render loop per
monitor, the way a Wayland/DRM compositor does) cannot use that path: each per-output
window is smaller than the whole screen, fails the whole-root test, and silently
falls back to a **copy** (a GPU blit to scanout) once per output per frame. On a
2-monitor setup that roughly **doubles** the presentation cost versus a single
whole-screen swap; a single monitor shows no regression. The cost is the copy, not
the rendering.

Per-CRTC flips remove that copy: the compositor presents one scanout-capable pixmap
per output and the server flips it directly, at each output's own vblank, giving
**independent per-output pacing at low GPU cost** — the same thing that makes
multi-monitor cheap under Wayland/DRM.

The `modesetting` (and `amdgpu`) driver source already flagged this as the missing
piece — the all-CRTC flip loop carries the comment *"if/when we get per-CRTC buffers,
we'll have to update this."* This work is that update.


## 2. What existed before, and the difference

| | Whole-screen flip (pre-existing) | Per-CRTC flip (this work) |
| --- | --- | --- |
| Buffer | One pixmap the size of the **whole X screen** | One pixmap the size of **one CRTC** |
| Target | Flipped to **every** enabled CRTC at once | Flipped to **one** named CRTC |
| Eligibility | `window->clipList == root->winSize` | `window->clipList == that CRTC's rectangle` |
| Flip state | One global flip in flight | One flip **per CRTC**, independent |
| Screen pixmap | Root pixmap redirected to the flip buffer | **Not** touched (a CRTC-sized pixmap cannot back the whole root) |
| Pacing feedback | One `PresentCompleteNotify` | One `PresentCompleteNotify` **per CRTC**, with that CRTC's `ust`/`msc` |

The Present protocol already carries everything needed: `PresentPixmap` has a
`target_crtc` field and delivers per-request `PresentCompleteNotify` with per-CRTC
`ust`/`msc`. No new extension, no new events — per-output vblank feedback comes for
free.


## 3. The model

A per-CRTC flip is offered when **all** of these hold:

1. The driver advertises the capability (`capable_flip_crtc`, see §5).
2. The request names a `target_crtc`.
3. The window's `clipList` is exactly that CRTC's desktop-space rectangle, and the
   window sits at the CRTC origin.
4. The window is **not** Composite-redirected (`GetWindowPixmap == GetScreenPixmap`),
   and the presented pixmap is the CRTC's size (smaller than the screen pixmap).

Present detects "this is a per-CRTC flip" purely by size: the flip pixmap is smaller
than the screen pixmap (`present_flip_is_per_crtc()`). A single-monitor setup, where
one CRTC covers the whole screen, therefore keeps using the whole-screen path
unchanged.

Unlike a whole-screen flip, a per-CRTC flip **does not** redirect the root/window-tree
pixmaps — it only redirects one CRTC's scanout. The screen pixmap's region under that
CRTC is left untouched while the flip is live (see the ghost-seed note in §4).

A compositor uses it by giving each output an un-redirected, CRTC-covering,
top-most window, rendering into a small swapchain of scanout-capable pixmaps
(DRI3/GBM), and calling `xcb_present_pixmap(window, pixmap, target_crtc=thatCRTC)`
per output, then driving each output's render loop from that output's
`PresentCompleteNotify`.


## 4. What Present core already does (driver-agnostic)

A driver author does **not** need to reimplement any of this — it lives in
`Xext/present/present_scmd.c` and `present_screen.c` and works for any driver that
opts in:

- **Flip-eligibility gate** (`present_check_flip`): accepts a whole-screen flip
  (clip == root) **or** a per-CRTC flip (clip == CRTC box **and** the driver set
  `capable_flip_crtc`). For per-CRTC it skips the `pixmap->screen_x/y` geometry test
  (those are 0 for standalone DRI3 buffers, only set by Composite).
- **Per-CRTC flip state**: flip bookkeeping is a list keyed by `RRCrtcPtr`, so two
  CRTCs can be in independent flip cycles at once.
- **Per-CRTC unflip routing** (`present_unflip`): for a per-CRTC flip it calls the
  driver's `unflip_crtc(screen, crtc, event_id)` (§5) instead of the whole-screen
  `unflip`, so ending one CRTC's flip never disturbs another CRTC.
- **Ghost-frame seed** (`present_restore_screen_pixmap`): because the screen pixmap's
  CRTC region was never written while the CRTC scanned out the flip buffer, on unflip
  Present seeds that region with the last flipped frame (at the CRTC's box position)
  so the CRTC does not scan out stale pixels. It only does this when the flipping
  window still covers the whole CRTC (a clean unflip such as a VT switch), and skips
  rotated CRTCs. The `CopyArea` damages the region, so TearFree/compositor repaint it
  cleanly on the next vblank.
- **Screen capture** (`present_flip_overlay_image`, via a `GetImage` wrap in
  `present_screen.c`): a root `XGetImage`/`XShmGetImage` stitches in per-CRTC flipped
  content from the flip buffers, so classic recorders still capture a per-output
  flipping compositor instead of stale screen-pixmap content. Read-only, cost only
  while capturing.


## 5. Driver contract — what a DDX must implement

Everything below is the shape of the `modesetting` implementation
(`hw/xfree86/drivers/video/modesetting/`); a new driver mirrors it. Function names in
parentheses are the modesetting ones to copy.

### 5.1 Advertise the capability

Set `present_screen_info.capable_flip_crtc = TRUE`
(`ms_present_screen_init`), gated on the kernel driver being **atomic-capable**
(`ms->atomic_modeset_capable`, i.e. the DRM driver recognizes `DRM_CLIENT_CAP_ATOMIC`).
This is what makes Present *offer* per-CRTC flips at all.

Whether a per-CRTC flip actually *succeeds*, though, also depends on
`Option "Atomic" "True"` (`ms->atomic_modeset`) at run time. A per-CRTC flip switches a
CRTC to a framebuffer of the CRTC's own size — a different stride than the shared screen
framebuffer, and (for a real compositor) a **tiled** modifier:

- With the option **on**, `drmmode_crtc_flip()` submits an atomic commit
  (`drmModeAtomicCommit` via `plane_add_props`), setting the framebuffer, source
  coordinates and plane props together — which is what such a flip needs.
- With the option **off**, it falls back to legacy `drmModePageFlip` (plus a
  `drmModeSetPlane` for the source-coordinate change). That path works for a **linear**
  per-CRTC buffer, but returns `EINVAL` on the **tiled** scanout buffers Mesa hands a
  normal GL client — so a real compositor's per-CRTC flips fail there and fall back to a
  copy.

So capability-gating on `atomic_modeset_capable` is correct (it decides eligibility),
but in practice a compositor needs `Option "Atomic" "True"` for the flip to land. A new
driver whose legacy flip path *can* handle a different-stride/tiled fb could relax this,
but should not assume the legacy path suffices. See the Requirements section and §8.

The struct is at `PRESENT_SCREEN_INFO_VERSION` 2; `capable_flip_crtc` and
`unflip_crtc` are the version-2 fields.

### 5.2 Per-CRTC-aware flip check (`check_flip2` → `ms_present_check_flip`)

For a per-CRTC request, compare the window's geometry to the **target CRTC's box**
(its `x,y,w,h`), not to the whole root. A helper `ms_flip_is_per_crtc(screen, pixmap)`
(pixmap size != screen size) distinguishes the two. Skip the `pixmap->screen_x/y`
comparison for per-CRTC (those don't track a standalone DRI3 buffer). Keep all
stride/format/modifier checks, evaluated per pixmap.

### 5.3 Per-CRTC rotation check (`ms_present_check_unflip`)

The pre-existing check vetoed flipping if **any** CRTC was rotated (a whole-screen
flip shares one fb; a rotated CRTC scans a rotation shadow BO). For a per-CRTC flip
only the **target** CRTC's rotation matters — a rotated *other* CRTC is untouched and
must not veto the flip. Check only the target for per-CRTC; keep vetoing on any
rotated CRTC for a whole-screen flip (`crtc == NULL`) or when the target itself is
rotated.

### 5.4 Flip one CRTC (`ms_present_flip` → `ms_do_pageflip_crtc`)

Route a per-CRTC pixmap to a path that flips it to **only** the target CRTC (its own
CRTC-sized framebuffer, imported from the pixmap's BO), rather than looping over all
CRTCs with one shared fb. Track scanout ownership **per CRTC**:

- `drmmode_crtc->present_flip_fb_id` — the fb this CRTC currently scans out under a
  per-CRTC flip (0 = the shared screen fb).
- `drmmode_crtc->present_flip_window` — the window flipping this CRTC (so concurrent
  per-CRTC flips don't clobber a global).

**Do not** set the global whole-screen flip flag (`drmmode.present_flipping`) for a
per-CRTC flip. Per-CRTC ownership is per CRTC; the global would wrongly gate every
CRTC.

### 5.5 TearFree interaction

If the driver has a TearFree path, gate it **per CRTC** on `present_flip_fb_id`, not
on the global flip flag (`ms_tearfree_do_flips` skips a CRTC whose
`present_flip_fb_id` is set). Otherwise a per-CRTC flip on one output freezes TearFree
on the others. When the CRTC is unflipped and `present_flip_fb_id` returns to 0,
TearFree resumes on it automatically.

### 5.6 Per-CRTC unflip (`unflip_crtc` → `ms_present_unflip_crtc` → `ms_do_unflip_crtc`)

Implement the `unflip_crtc(screen, crtc, event_id)` callback: flip **only** that CRTC
back to the framebuffer it should normally scan out — whatever
`drmmode_crtc_get_fb_id()` reports (the shared fb, its TearFree buffer, or its
rotation shadow) — leaving every other CRTC untouched. Use `new_present_fb_id = 0`
in the queue bookkeeping so the CRTC's per-CRTC fb is released on completion and
TearFree resumes. Call `present_event_notify(event_id, …)` when it lands (or
immediately on failure, so Present doesn't wait forever).

This is essential: the **whole-screen** `unflip` flips *every* CRTC to the shared fb,
which for per-CRTC flips (a) collides (`EBUSY`) with another CRTC's still-pending flip
— and the retry re-enters the DRM event handler and corrupts the event queue → crash
— and (b) pushes a rotated CRTC to the wrong scanout. The target CRTC's own flip has
already completed by the time Present calls `unflip_crtc` (Present defers the unflip
until then), so it does not hit `EBUSY`.

### 5.7 Reconfiguration teardown (modeset / rotation / output on-off / VT switch)

A per-CRTC flip's scanout ownership is invisible to code paths that reconfigure a CRTC
from outside Present. Route them through the same unflip+drain the whole-screen path
uses **before** reprogramming any CRTC:

- Before a modeset (`drmmmode_prepare_modeset`): trigger the pre-modeset unflip+drain
  when **any** CRTC has `present_flip_fb_id` set (not only on the global flip flag).
  `present_check_flips` + `ms_present_check_flip` rejecting during `pending_modeset`
  unflips the per-CRTC flips; then drain DRM events. This covers resolution changes,
  rotation, and output enable (all go through `set_mode_major`).
- On `LeaveVT`: run that same unflip+drain **first**, while the server still holds DRM
  master (`vtSema` still TRUE), so no in-flight per-CRTC flip has its completion
  delivered against torn-down state after master is dropped.

Without this: the CRTC's `present_flip_fb_id` is left stale after the mode change →
TearFree never resumes on it (frozen output), or a pending flip completion races the
fb being torn down (crash).


## 6. Present core API (version 2)

`include/present.h`, `present_screen_info`:

```c
#define PRESENT_SCREEN_INFO_VERSION 2

/* Version >= 2. Driver can page-flip a single-CRTC-sized pixmap to one CRTC. */
Bool capable_flip_crtc;

/* Version >= 2. Restore a single CRTC to its normal scanout, ending a per-CRTC
 * flip without touching any other CRTC. Paired with capable_flip_crtc; NULL =>
 * fall back to the whole-screen unflip. */
typedef void (*present_unflip_crtc_ptr)(ScreenPtr screen, RRCrtcPtr crtc,
                                        uint64_t event_id);
present_unflip_crtc_ptr unflip_crtc;
```

Existing fields used unchanged: `check_flip2` (gets the `target_crtc`), `flip` (gets
`crtc`, `event_id`, `target_msc`, `pixmap`, `sync_flip`), `unflip` (whole-screen
fallback).

A debug tracer for the flip-decision gate is available at runtime with
`XLIBRE_PRESENT_FLIP_DEBUG=1` (env), which logs *why* a given window did or did not
flip — useful when bringing up a new driver or compositor.


## 7. Porting to another DDX (amdgpu / nouveau / intel)

The drivers map function-for-function. Using the `modesetting` ↔ `amdgpu` mapping as
the example:

| Concern | modesetting | amdgpu |
| --- | --- | --- |
| Flip entry | `ms_present_flip()` | `amdgpu_present_flip()` |
| Per-CRTC flip exec | `ms_do_pageflip_crtc()` | (add, mirroring `amdgpu_do_pageflip()`) |
| Per-CRTC unflip exec | `ms_do_unflip_crtc()` | (add) |
| Flip check | `ms_present_check_flip()` | `amdgpu_present_check_flip()` |
| Unflip/rotation check | `ms_present_check_unflip()` | `amdgpu_present_check_unflip()` |
| All-CRTC flip loop + TODO | `ms_do_pageflip()` | `amdgpu_do_pageflip()` |
| Per-CRTC scanout state | `drmmode_crtc->present_flip_fb_id` / `present_flip_window` | (add to amdgpu's crtc priv) |
| Pre-modeset drain | `drmmmode_prepare_modeset()` | (amdgpu's modeset prep) |

Suggested order:

1. Advertise `capable_flip_crtc` when the kernel driver is atomic-capable (§5.1).
2. Per-CRTC `check_flip2` / rotation check (§5.2–5.3).
3. Flip one CRTC + per-CRTC state + TearFree gating (§5.4–5.5).
4. `unflip_crtc` (§5.6) — do **not** skip this; the whole-screen unflip crashes with
   concurrent per-CRTC flips.
5. Reconfiguration drain (§5.7).
6. Test with a standalone client that Presents a CRTC-sized pixmap to a
   CRTC-covering override-redirect window (watch `XLIBRE_PRESENT_FLIP_DEBUG=1` for
   FLIP vs COPY), then two CRTCs concurrently, then VT switch / xrandr mode+rotation
   while flipping, then a screen recorder.


## 8. Known limitations

- **Needs `Option "Atomic" "True"` in practice.** Eligibility is gated on the kernel
  driver being atomic-capable (every modern driver is), but with the Atomic option off
  the flip runs via legacy `drmModePageFlip`, which `EINVAL`s on the tiled scanout
  buffers a real compositor uses (only linear buffers flip on the legacy path). So a
  compositor must set `Option "Atomic" "True"`. Note that modesetting's atomic support
  is experimental and off by default, with known rough edges (cursor handling, the
  initial-modeset flicker), so a glitch that also appears under Atomic with a plain
  single-drawable compositor or a fullscreen game is the atomic path itself, not the
  per-CRTC flip code. See §5.1 and Requirements.
- **Rotated target.** A per-CRTC flip whose *target* CRTC is rotated is not supported
  (falls to copy); a rotated *other* CRTC no longer blocks flips on unrotated ones.
- **No compositor + `background=None` root.** After a per-CRTC flip ends with no
  compositor to repaint, a region a shrunk window vacated shows static content (the
  ghost seed is skipped in that case). Cosmetic; the intended target is a compositor,
  which repaints.
- **Clone/mirror CRTCs** sharing a rectangle, and **two windows presenting to one
  CRTC**, are untested.
- Concurrent per-CRTC flip teardown is inherently re-entrancy-sensitive (DRM event
  queue + flipdata refcounts); a stray crash in the driver's DRM event handler
  (`ms_drm_sequence_handler` on modesetting) is the signal to investigate there.


## 9. References

- Present core: `Xext/present/present_scmd.c`, `present_screen.c`, `include/present.h`.
- modesetting DDX: `hw/xfree86/drivers/video/modesetting/{present,pageflip,driver,drmmode_display}.c`.
- Related independent XiS work (different axes, not this one): per-output input
  coordinate scaling, and per-output DPI/scale. This document is about
  **output/scanout** only.
