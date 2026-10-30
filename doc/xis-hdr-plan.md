# XiS HDR — implementation plan

Status: **plan, nothing implemented yet** (written 2026-10-30).

This is the working plan for HDR10 output in the XiS fork. It is split into
phases that can each be implemented in a separate session. To start one, point
the session at this file and the phase number ("implement phase 2 of
`doc/xis-hdr-plan.md`"). Every phase lists what to port, where it goes in our
tree, how to test it and when it counts as done. When a phase lands, update its
**Status** line and anything the implementation proved wrong here.

Conventions for every phase:

- Branch: `xis-hdr`, cut from `experiments` at the start of phase 0. One commit
  per logical step, bisectable, in the style of the per-CRTC flip series.
- Build dir: same as `experiments` (`build-test-debug`). The user builds,
  installs and tests; the implementing session stops after code + a read-only
  sanity check and hands over what to build and how to test.
- Branding: fork features are "XiS" in docs; upstream naming stays "XLibre".
- When code is ported from dec05eba's fork (MIT, `Copyright © 2026 dec05eba`),
  keep the copyright notice in the files that carry it (e.g. `hdr.c`).


## 1. Goals and non-goals

Goals, in priority order:

1. **Zero cost when nothing uses HDR.** With HDR off, or on with no HDR client,
   no shader runs and no buffer grows. The desktop stays an 8-bit
   framebuffer; SDR→PQ conversion is done by the display engine through the
   CRTC colour pipeline (`DEGAMMA_LUT` → `CTM` → `GAMMA_LUT`).
2. **Client compatibility with dec05eba's ecosystem**, which is the only X11
   HDR client stack that exists today: [VK_hdr_layer (X11 fork)][vkhdr] and the
   [mpv fork][mpv]. Same window property, same RandR output property names and
   types, same 30-bit visual on depth-24 screens. A client that works on
   dec05eba's server must work on XiS with no change.
3. **External compositors stay first-class.** The server must never require
   being the compositor. kicomp or a future HDR-aware KWin must be able to do
   windowed HDR on top of what the server exposes.
4. **Runtime control per output** through RandR properties, like our existing
   `TearFree` / `PerCRTCFlip` properties.
5. Fit the per-CRTC flip work: HDR on one output while the others stay SDR.

Non-goals (for now):

- A linear/FP16 desktop (cepelinas' approach). Correct in theory, but it costs
  2× framebuffer memory and bandwidth plus a shader on every draw even with no
  HDR client, and it touches all of glamor.
- The HDRProto extension from cepelinas' branch. It is a "draft of draft" with
  no known clients. The internal state is designed so a translation layer can
  be added later (§9).
- An in-server compositor. Last phase, optional, and only if it can yield to an
  external compositor (phase 5).
- amdgpu DDX support. modesetting only, as with per-CRTC flips.


## 2. Prior art and what we take from each

### 2.1 dec05eba's Xorg fork — primary source

- Repo: <https://github.com/dec05eba/xserver> (branch `main`). It is based on
  **freedesktop Xorg master, not XLibre**, so nothing cherry-picks. Port by hand.
  The modesetting paths differ: theirs is `hw/xfree86/drivers/modesetting/`,
  ours is `hw/xfree86/drivers/video/modesetting/`.
- User docs: `DIVERGENCE.md` and `modesetting.man` in that repo.
- Local clone for reading, if still present:
  `git clone --bare https://github.com/dec05eba/xserver` (any scratch dir).

Relevant commits (hashes as of 2026-10-30, they may get rebased):

| Commit | What | Use |
|---|---|---|
| `f92f06d2ed` Apply hdr support | first fullscreen passthrough, connector props | read for history only |
| `dcc9cfdd2c`, `74322f9a7a` | `HDR` option → RandR `HDR` + `SDR white level` props | phase 1 |
| `37e071eab4` | `HDR capable` RandR prop | phase 1 |
| `10e23933ea` | don't disable HDR for override-redirect fullscreen windows | phase 2 (tree-walk rule) |
| `b9a602dea4` Support depth 30 fullscreen window in depth 24 mode | 30-bit alt visual, GLX fbconfig filter, AddFB2 with the real format, `scanout_format` tracking | phase 0 |
| `9ac7005e2a` modesetting: fix atomic modesetting | drop ASYNC when src coords change under atomic, `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` | phase 0 (we already have the cap=2 part) |
| `e24a439f75` (tree state) | **cleanest HDR snapshot before the compositor**: `hdr.c` with desktop LUT/CTM/metadata, EDID PQ check, tree walk | phase 1–2 main reference |
| `a8e880a85f`, `e8fc71d8e7` | compositor + windowed HDR + tone mapping; `hdr.c` grows `TONEMAP` mode and `ms_hdr_tonemap_color` | phase 2b (LUT tonemap only), phase 4/5 reference |
| `470c2e6cf8` | fix temporary glitch when an HDR window appears | phase 2 (ordering of colour commit vs flip) |
| `8412a92133` | reset VRR and colour state left by the previous DRM master (start + EnterVT) | phase 1 (lifecycle) |
| `6f822773c2` | skip gamma commits that don't change the ramp (each colour commit blocks up to a frame) | phase 1 (gamma) |
| `68f1991e95` (`cursor.c`, `ms_cursor_pixel_to_pq`) | convert the SDR cursor image to PQ while the CRTC expects PQ | phase 2 (cursor) |
| `0503207147` (`colorop.c`) | SDR→PQ on the primary **plane** colour pipeline (KMS colorop API, `DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE`, Linux 6.19+), CRTC stays passthrough: no amdgpu flicker on mode switches | §8 (future) |
| `db67ea184e`, `71c2a531f3` | nvidia-drm: write identity tables instead of clearing them with a flip; force plane input-LUT reload (driver 615) | phase 1/2, nvidia only |
| `82bc76f8e2` | amdgpu can drop the primary plane colour pipeline when an overlay plane toggles | only if we use overlays/colorops |
| `d99b133b63`, `f1b662d2df` (`overlay.c`) | windows above a directly scanned-out fullscreen window go on an overlay plane | phase 2 item 7 (option b) |
| `55d631950d`, `941aa3b3ee` | with HDR, amdgpu re-commits the plane under the cursor on cursor updates, stalling behind GPU work on the scanout buffer (e.g. a screen recorder) | phase 2 (risk to watch) |

To read the pre-compositor HDR code:

```sh
git show e24a439f75:hw/xfree86/drivers/modesetting/hdr.c
git show e24a439f75:hw/xfree86/drivers/modesetting/drmmode_display.c   # drmmode_crtc_commit_hdr & co, ~line 4860+
git show main:hw/xfree86/drivers/modesetting/hdr.c                     # adds TONEMAP colour math
```

What we take: the client contract (§3), the desktop colour pipeline math
(`ms_hdr_desktop_color`), the PQ/tonemap LUT math (`ms_hdr_tonemap_color`,
`ms_hdr_tone_map`, `ms_hdr_content_peak`), the tree walk that finds a
fullscreen HDR window, the depth-30 visual work, the atomic/async fixes.

What we don't take: the in-server compositor, the per-monitor framebuffer that
replaces the screen pixmap with 0×0, their Present per-CRTC refactor (we have
our own, see `doc/present-per-crtc-flip.md`), `Compositing` replacing
`TearFree`.

### 2.2 cepelinas9000 (Tautvis) XLibre branch — secondary source

- Branch: <https://github.com/cepelinas9000/xserver/tree/wip/x11hdrV2>, one
  commit `159a6f484a` "Reworked X11 HDR" (2026-03-12) on XLibre master of
  2026-02. Self-described as rough ("expect a lot of dark windows").
- Design: FP16 BT.2020-linear screen pixmap, `HDRColor` visual class, colour
  conversion inside glamor ops, HDRProto extension with Vulkan structs
  (`glamor/hdrext/HDRProto.txt`), per-window SDR brightness through the
  `__X11HDR_SDR_PARAMS` property.
- **Already in our tree from the same author:** `2d72f9e192` "xfree86: ddc:
  parse CTA-861-G HDR static metadata + colorimetry data blocks", i.e.
  `xf86DoInterpretHDRMetadata()` (`hw/xfree86/ddc/interpret_edid.c`) filling
  `struct monitor_hdr_data` (`include/edid.h`, field `xf86Monitor::hdr`), with
  tests in `test/ddc-tests.c`. Phase 1 uses this instead of dec05eba's ad-hoc
  `ms_hdr_edid_supports_pq`.
- What we take beyond that: the idea of a per-window SDR brightness hint, only
  as an optional compositor hint (§9). Its connector code
  (`drmmode_crtc_set_colorimetry`, `drmmode_crtc_set_hdr_static_metadata_v1`)
  is a second reading of the same kernel API; don't port it, it uses legacy
  per-property sets and leaks on error paths.
- Discussion with a screenshot of windowed HDR:
  <https://github.com/orgs/X11Libre/discussions/251#discussioncomment-16089204>

### 2.3 Client side

- [VK_hdr_layer, dec05eba's X11 fork][vkhdr] (upstream by Xaver Hugl for
  Wayland). Read its README "How it works"; it is the de facto client spec.
  Source: `src/VkLayer_hdr_wsi.cpp`. Enabled with `ENABLE_HDR_WSI=1`.
- [mpv fork][mpv], commits `7604e85` "Add hdr support on x11" and `ad0e943`
  "Allow hdr on x11 even in windowed mode". Run with
  `--vo=gpu-next --target-colorspace-hint=yes`. It sets the window property
  itself (don't combine with the layer).
- Proton/DXVK: `DXVK_HDR=1`. vkd3d-proton needs a build that falls back to
  `VK_FORMAT_A2R10G10B10_UNORM_PACK32` (see the layer README).

### 2.4 Kernel references

- KMS colour management properties (`DEGAMMA_LUT`, `CTM`, `GAMMA_LUT`, the
  `*_SIZE` props):
  <https://docs.kernel.org/gpu/drm-kms.html#color-management-properties>
- Connector `HDR_OUTPUT_METADATA` and `Colorspace`:
  <https://docs.kernel.org/gpu/drm-kms.html#standard-connector-properties>
- `struct hdr_output_metadata` / `hdr_metadata_infoframe`: `include/uapi/drm/drm_mode.h`.
- Future (not used here): the KMS per-plane colour pipeline API, which would
  allow HDR planes/overlays without these CRTC-wide LUTs.


## 3. Client-visible contract (compatibility surface)

This must match dec05eba's server bit for bit. Phase 1/2 must not invent
names or change types.

### 3.1 RandR output properties (per output, modesetting)

| Name | Type | Range | Meaning |
|---|---|---|---|
| `HDR` | INTEGER, 32 | 0..1 (range prop) | 1 = output runs in HDR10 (PQ, BT.2020). Settable. |
| `HDR capable` | INTEGER, 32 | 0..1, immutable/read-only | 1 = EDID advertises SMPTE ST.2084 and the connector has `HDR_OUTPUT_METADATA`. |
| `SDR white level` | INTEGER, 32 | 1..10000 | Nits that SDR white (the desktop) is shown at in HDR mode. Default 203. |

The VK layer reads `HDR` and `HDR capable` with `XCB_ATOM_INTEGER` and checks
`== 1`, on **every** output the window overlaps, and listens for RandR output
property notifies to return `VK_SUBOPTIMAL_KHR`. So:

- Do **not** reuse our `drmmode_output_create_toggle_property()` (on/off/auto
  atoms). `HDR` is a plain INTEGER 0..1 range property.
- Property changes must generate the normal RandR `OutputPropertyNotify`
  (they do when going through `RRChangeOutputProperty` with `sendevent`).

xorg.conf (Device section), initial values only:

```
Option "HDR"           "true"   # default false
Option "SDRWhiteLevel" "203"
```

### 3.2 Window property

- Name `hdr_output_metadata`, type CARDINAL, format 8, size exactly
  `sizeof(struct hdr_output_metadata)` (kernel `drm_mode.h`), else ignored.
- Content: PQ (ST.2084) BT.2020 pixels; the struct carries mastering/CLL/FALL.
- Set by the client on its window (the layer may set it on a depth-30 child
  window it creates; see §3.3). Deleted when the swapchain is no longer HDR10.

### 3.3 30-bit visual on depth-24 screens

Vulkan drivers only expose 10-bit swapchain formats on depth-30 windows. The
layer looks for a depth-30 visual in the screen's allowed depths and, on a
depth-24 desktop, renders into a **depth-30 child window** of the app window.
So the server must offer a `x2r10g10b10` visual at depth 30 on a depth-24
screen, and such windows must be page-flippable (not implicitly redirected).
Don't set `DefaultDepth 30`: it is not needed and breaks many apps.

### 3.4 Behaviour per output (what the user sees)

| Output `HDR` | Fullscreen HDR window on it? | CRTC colour mode | Cost |
|---|---|---|---|
| 0 | no | OFF (normal SDR, user gamma/CTM) | none |
| 0 | yes | TONEMAP (PQ→SDR in the LUTs) | none (display engine) |
| 1 | no | DESKTOP (SDR→PQ in the LUTs) | none |
| 1 | yes | PASSTHROUGH (identity LUTs, window metadata) | none |
| any | HDR window, not fullscreen, no compositor | OFF/DESKTOP; window looks washed out until phase 4 | none |
| any | HDR window, external HDR compositor | compositor's job (phase 3) | compositor's |


## 4. Architecture

### 4.1 Per-CRTC HDR colour state

New file `hw/xfree86/drivers/video/modesetting/hdr.c` (+ declarations in
`driver.h` / `drmmode_display.h`), following dec05eba's split:

```c
enum drmmode_hdr_mode {
    DRMMODE_HDR_OFF,          /* SDR, user gamma ramp + user CTM */
    DRMMODE_HDR_DESKTOP,      /* SDR desktop -> PQ via DEGAMMA/CTM/GAMMA */
    DRMMODE_HDR_PASSTHROUGH,  /* PQ window scanned out unchanged */
    DRMMODE_HDR_TONEMAP,      /* PQ window -> SDR via the LUTs */
};
```

Per CRTC: current mode, current window metadata, sticky `hdr_failed` (like
`per_crtc_flip_failed`: set when the kernel rejects a colour commit, cleared
when the user touches the `HDR` property or the CRTC is re-enabled).

Per output: `hdr_enabled` (property), `hdr_capable` (EDID + kernel prop),
`sdr_white_level`, atoms, and the connector prop ids for
`HDR_OUTPUT_METADATA` and `Colorspace` (the latter may be missing, §8).

### 4.2 Decision function (per enabled CRTC)

```
hdr_on   = every output on the CRTC has HDR=1 && HDR capable=1
           && the CRTC has DEGAMMA/CTM/GAMMA props && !hdr_failed
win      = fullscreen HDR window covering crtc->bounds (tree walk, §4.3)
mode     = hdr_on ? (win ? PASSTHROUGH : DESKTOP)
                  : (win && tonemap_enabled ? TONEMAP : OFF)
```

Recomputed lazily from the block handler when `ms->hdr_dirty` is set. Things
that set it: `PostValidateTree` (window tree/geometry changed),
`PropertyStateCallback` for the `hdr_output_metadata` atom, RandR property
changes (`HDR`, `SDR white level`), gamma ramp / CTM changes, CRTC set_mode,
EDID update (hotplug), EnterVT, and per-CRTC flip start/end on that CRTC.

### 4.3 Finding the fullscreen HDR window

Port `ms_fullscreen_hdr_output_metadata()` from dec05eba (`e24a439f75`): walk
root's children top-down; skip unviewable/InputOnly and anything whose
`borderSize` does not fully contain the CRTC box; return the first window (or
descendant, recursively) carrying a valid `hdr_output_metadata`; stop at the
first non-override-redirect window that covers the box (it hides everything
below), but keep going past covering override-redirect windows only if they
don't carry metadata (`10e23933ea`).

This is geometry-based, not flip-based, on purpose: a fullscreen window that
is copied instead of flipped (TearFree, rotation, transform) still shows the
right colours in PASSTHROUGH, just quantised to 8 bits. The flip is the
quality/perf path, not a correctness requirement. Log once (debug) when
PASSTHROUGH runs on a non-flipped CRTC.

### 4.4 Colour pipeline math (port from dec05eba `hdr.c`)

- DESKTOP: `DEGAMMA_LUT` = user gamma ramp (`crtc->gamma_red/green/blue`)
  linearised with γ 2.2; `CTM` = BT.709→BT.2020 × user CTM;
  `GAMMA_LUT` = inverse PQ EOTF of (linear × SDR white level).
  Connector: `HDR_OUTPUT_METADATA` = desktop metadata (EOTF PQ, BT.2020
  primaries; max luminance from the EDID's `monitor_hdr_data` desired max
  luminance, falling back to the SDR white level); `Colorspace` = BT2020_RGB.
- PASSTHROUGH: no DEGAMMA/CTM/GAMMA (NULL blobs = bypass), metadata = the
  window's struct (clamp luminance to 10000), Colorspace = BT2020_RGB.
  On nvidia-drm write identity tables instead of NULL: it ignores clearing
  them when that comes with a flip (`db67ea184e`).
  The user gamma ramp / night light is **not** applied here (same as
  dec05eba; document it).
- TONEMAP: `DEGAMMA_LUT` = PQ EOTF / content peak; `CTM` = user CTM ×
  BT.2020→BT.709; `GAMMA_LUT` = `ms_hdr_tone_map` (knee 0.75) → γ 1/2.2 → user
  ramp. Content peak = MaxCLL, else mastering max, else 1000 nits, clamped to
  [white level, 10000]. No connector changes.
- OFF: restore our existing behaviour exactly (legacy gamma or `GAMMA_LUT`
  from the ramp, user CTM via `drmmode_set_ctm`, metadata blob 0, Colorspace
  Default).

Our tree already has `use_gamma_lut`, `drmmode_set_ctm()` and the per-output
`CTM` RandR property (`drmmode_display.c`, `ctm_atom`); the HDR code must
compose with them, not replace them.

### 4.5 Committing

- Switching DESKTOP ↔ PASSTHROUGH must not modeset: in HDR mode the connector
  always carries **some** PQ metadata, so only blob contents and LUTs change.
  That is what removed the blank on switch in dec05eba's later version.
- Turning `HDR` on/off for an output may need a full modeset (metadata
  appears/disappears, Colorspace changes); commit with
  `DRM_MODE_ATOMIC_ALLOW_MODESET` only in that case.
- Always `TEST_ONLY` first, then commit; on failure set `hdr_failed`, log once
  with the errno, fall back to OFF.
- **Requires `Option "Atomic" "True"`.** Without atomic, `HDR capable` stays 0
  and an info line says why. (Per-CRTC flips with real compositor buffers
  already need it, see `doc/present-per-crtc-flip.md`.) Legacy fallback can
  be added later if anyone needs it.
- Glitch avoidance (phase 2, `470c2e6cf8`): when a window enters PASSTHROUGH,
  the LUT change and the first HDR frame should land in the same vblank.
  First version: commit colour state from the block handler and accept a
  one-frame glitch. Refinement: fold the colour props into the flip's atomic
  request in `drmmode_crtc_flip()` when the flip belongs to the HDR window.


## 5. Phases

### Phase 0 — groundwork (no HDR yet, fully testable on SDR monitors)

**Status:** not started.

0a. **Async flips vs. src/format change under atomic.** Our
`drmmode_crtc_flip()` (`drmmode_display.c` ~line 986) keeps
`DRM_MODE_PAGE_FLIP_ASYNC` in the atomic path even when the src coordinates
change (shared fb ↔ per-CRTC fb), which the kernel rejects. Port from
`9ac7005e2a` + `b9a602dea4`:
- track `drmmode_crtc->scanout_format` (via `drmModeGetFB2`) on set_mode and
  on every successful flip, and keep `src_x/src_y` updated in the atomic path
  too (today only the legacy path updates them);
- clear `DRM_MODE_PAGE_FLIP_ASYNC` when format or src changed;
- legacy path: also go through `drmModeSetPlane` when the format changes.
Check first that `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` handling is already
equivalent (we have `5460be02c9`).

0b. **Scanout FBs with the real buffer format.** Imported client buffers must
be added with `drmModeAddFB2(get_opaque_format(gbm_bo_get_format()))`, not
`drmModeAddFB(scrn->depth, kbpp)`, or a 10-bit buffer gets labelled XRGB8888
(`b9a602dea4`, `drmmode_bo_import`). Add `ARGB1555` to `get_opaque_format`.
Audit both the whole-screen and per-CRTC flip paths (`pageflip.c`).

0c. **30-bit visual on depth-24 screens.** Port `b9a602dea4`:
- `Xext/composite/compinit.c`: add `{30, PIXMAN_x2r10g10b10, implicitRedirect
  = FALSE}` to `altVisuals[]` (our tree uses `PIXMAN_*` names), allow depth 30
  even when ≥ root depth, register an implicit-redirection exception for it,
  set `bitsPerRGBValue`.
- `glx/glxscreens.c`: an fbconfig only counts a Composite alternate visual if
  it was duplicated for it (`duplicatedForComp`), so depth-30 fbconfigs don't
  wrongly claim window support.
- modesetting `PreInit`: `xf86AddPixFormat(pScrn, 30, 32, 0)` when glamor is
  on, depth is 24 and not a GPU screen.
- Check glamor can create depth-30 pixmaps on a depth-24 screen (format table
  in `glamor/glamor.c` / `glamor_egl.c`).
- Check `present_check_flip` / `ms_present_check_flip` accept a depth-30
  window/pixmap on a depth-24 screen (format/depth gates, modifiers query in
  `get_drawable_modifiers`).

Tests:
- `xdpyinfo | grep -A3 'depth:.*30'` shows a TrueColor depth-30 visual.
- Extend `Xext/present/test/xis-present-flip-test.c` with `--depth30` (create
  a depth-30 window, `GBM_FORMAT_XRGB2101010` buffer): reports FLIP whole-screen
  and with `--crtc N`; also with `--async` style presents if the harness has it.
- `vkcube` with `ENABLE_HDR_WSI=1` is not useful yet (no HDR props), but
  `vkcube --present_mode 0` on an `A2R10G10B10` swapchain in a depth-30
  window should flip.
- Regression: normal fullscreen game/video still flips; KWin per-CRTC still
  flips; GLX apps still pick depth-24 configs by default.

Done when: depth-30 windows exist on a depth-24 desktop, flip whole-screen and
per-CRTC with Atomic on, and nothing else changed.

### Phase 1 — HDR desktop mode + RandR properties (camada 0)

**Status:** not started. Depends on phase 0 only for testing with real clients.

Steps:
1. Connector props: add `HDR_OUTPUT_METADATA` and `Colorspace` (enum,
   find `BT2020_RGB` and `Default` by name) to the connector prop tables in
   `drmmode_display.c`; CRTC props `DEGAMMA_LUT(_SIZE)` next to the existing
   `GAMMA_LUT`/`CTM` entries.
2. `HDR capable` = connector has `HDR_OUTPUT_METADATA` && EDID PQ support from
   `xf86DoInterpretHDRMetadata()` / `monitor_hdr_data` (make sure the
   modesetting EDID path calls it; check `drmmode_output_attach_tile` /
   `xf86OutputSetEDID` flow) && `ms->atomic_modeset`. Update on hotplug.
3. Create `HDR`, `HDR capable`, `SDR white level` in
   `drmmode_output_create_resources()`, handle sets in
   `drmmode_output_set_property()` (validate range, set dirty, clear
   `hdr_failed`). Options `HDR` / `SDRWhiteLevel` in the option table and
   `modesetting.man`.
4. `hdr.c`: colour math for DESKTOP and OFF (§4.4), `ms_hdr_update()` called
   from the block handler, `ms_hdr_screen_init/close_screen`.
5. Commit path (§4.5) with TEST_ONLY and `hdr_failed`.
6. Lifecycle: `LeaveVT` and `CloseScreen` restore OFF on every CRTC **while
   still DRM master** (same ordering rule as the per-CRTC flip flush: before
   vtSema/master drop), so the console/next client never inherits PQ.
   `EnterVT` marks dirty. DPMS off/on re-applies. RandR modeset re-applies
   after `set_mode_major`.
   Also the reverse direction (`8412a92133`): at server start and on every
   `EnterVT`, commit our current colour state unconditionally, because the
   previous DRM master (e.g. KWin Wayland with HDR on) may have left
   `HDR_OUTPUT_METADATA`, `Colorspace`, colour tables or VRR set, and we
   otherwise only commit on change.
7. Gamma interaction: `xrandr --gamma` / night light change the ramp →
   DESKTOP degamma is rebuilt (they keep working in HDR desktop mode). The
   legacy-gamma path (`!use_gamma_lut`) can't do HDR → `HDR capable` 0.
   Remember the ramp of the last colour commit and skip identical ones
   (`6f822773c2`): colormap installs, EnterVT and night-light tools re-set
   the same ramp often, and each atomic colour commit blocks for up to a
   frame per monitor. Forget it on EnterVT / lease changes.
8. Docs: section in README "XiS — This Fork", man page, a user doc
   `doc/xis-hdr.md` (user facing; this plan stays the dev doc).

Tests (see §7 for the hardware caveat):
- `xrandr --prop` shows the three props with the right types; `--set HDR 1`
  on a non-capable output is rejected or ignored with a log line.
- Debug override env `XIS_HDR_FORCE_CAPABLE=1` (debug builds only, document
  it) lets `HDR 1` run on an SDR monitor: the picture becomes visibly wrong
  (PQ-encoded) and `drm_info` shows non-zero `DEGAMMA_LUT`, `CTM`,
  `GAMMA_LUT` and `HDR_OUTPUT_METADATA` blobs; `HDR 0` restores the exact
  previous look, including a custom `xrandr --gamma`.
- VT switch, DPMS, resolution change, hotplug, server exit with HDR on: the
  console and the next session come back SDR.
- On a real HDR monitor: the monitor enters HDR mode; the desktop looks the
  same as SDR at the chosen `SDR white level`.

Done when: an HDR monitor runs the desktop in HDR10 with no visible change,
toggled at runtime, and every teardown path returns to SDR.

### Phase 2 — fullscreen HDR passthrough + LUT tone mapping (camada 1)

**Status:** not started. Needs phases 0 and 1.

Steps:
1. `hdr_output_metadata` atom + `PropertyStateCallback` + `PostValidateTree`
   wrap → `hdr_dirty` (port `ms_hdr_property_state`,
   `ms_hdr_post_validate_tree`).
2. Tree walk (§4.3) per enabled CRTC using `crtc->bounds`. With per-output
   scaling/transform (Tier 1 SCALE, XInputTransform), use the same box that
   `present_crtc_box` uses; note in a comment which coordinate space it is.
3. PASSTHROUGH (§4.4) when `hdr_on`.
4. TONEMAP (§4.4) when the output is not in HDR, behind a per-output toggle
   (decide: an `HDR tonemap` INTEGER 0..1 property defaulting to 1, or always
   on; dec05eba always tone maps). This one is testable on SDR monitors.
5. Hook per-CRTC flip start/end (our `present_flip_window` per CRTC,
   `ms_do_pageflip_crtc`, `ms_do_unflip_crtc`) to set `hdr_dirty`, so the mode
   follows the flip without waiting for a tree change.
6. Refinement (separate commit): colour state in the same atomic commit as the
   first flip of the HDR window (§4.5).
7. **SDR content above a PASSTHROUGH window** (found in dec05eba's later
   work, not in the first draft of this plan). The tree walk only skips
   windows that don't cover the whole CRTC, so a menu, OSD or notification
   over a fullscreen HDR window leaves the CRTC in PASSTHROUGH; the window
   stops flipping, the popup is copied into the screen pixmap as SDR, and
   its SDR values are shown as PQ (white = 10000 nits). Options:
   a. v1: PASSTHROUGH only while the HDR window's `clipList` equals the CRTC
      box (nothing above it); otherwise the CRTC goes back to DESKTOP (HDR
      content washed out while the popup is up). Simple, zero cost.
   b. later: put the windows above on an overlay plane, as dec05eba does
      (`overlay.c`), with their pixels converted to PQ (glamor shader into the
      overlay buffer, or the plane colour pipeline on 6.19+), so the HDR
      window keeps flipping.
   Start with (a).
8. **Cursor.** The hardware cursor blends after the CRTC's DEGAMMA/CTM, so in
   PASSTHROUGH (and TONEMAP) an SDR cursor image is read as PQ. Convert the
   cursor image to PQ at `SDR white level` when it is uploaded while the CRTC
   is in such a mode, and re-upload it on mode switches (`ms_cursor_pixel_to_pq`
   + `ms_hdr_sdr_to_pq_table`). CPU cost only on cursor image changes. Our
   cursor upload is `drmmode_load_cursor_argb_check()`, which also carries the
   X-SCALE cursor scaling (`xf86CrtcCursorScale()`); convert after scaling.

Tests:
- New tiny client `Xext/present/test/xis-hdr-test.c`: fullscreen depth-30
  window, sets `hdr_output_metadata` (MaxCLL 1000), presents a PQ test pattern
  (grey ramp 0–10000 nits in bands + BT.2020 primaries) via DRI3/Present,
  `--crtc N`, `--no-metadata`, `--metadata-maxcll N`. Reports FLIP/COPY like
  the flip harness. Lets every mode be tested without games.
- SDR monitor, `HDR 0`: with metadata the ramp is tone mapped (highlights roll
  off, nothing clips hard before the top band); without metadata it shows the
  raw PQ values (dark, washed): proves the switch.
- mpv fork `--vo=gpu-next --target-colorspace-hint=yes --fs` on an HDR clip:
  TONEMAP on SDR, PASSTHROUGH on HDR.
- `ENABLE_HDR_WSI=1 vkcube` / a Proton game with `DXVK_HDR=1` on an HDR
  monitor.
- Two monitors: HDR fullscreen on one while the other keeps playing SDR
  video; per-CRTC flip stays on both.
- Alt-tab out of the fullscreen HDR window, unmap it, resize it: the CRTC
  goes back to DESKTOP/OFF within a frame or two, no stuck LUTs.
- Expect a brief flicker on amdgpu when a CRTC switches between DESKTOP and
  PASSTHROUGH: amdgpu flickers whenever the CRTC colour tables change, on any
  kernel before the plane colour pipeline API (6.19). dec05eba documents the
  same. Only the switch flickers, not steady state.
- Cursor over the fullscreen HDR window has normal SDR brightness; a menu or
  notification over it follows item 7.
- Moving the cursor while a screen recorder reads the HDR scanout must not
  stall the game's flips (`55d631950d`; our cursor path is the legacy cursor
  ioctl, which amdgpu turns into an atomic commit too).

Done when: fullscreen HDR works on HDR monitors and tone maps on SDR ones,
with zero GPU cost, alongside per-CRTC flips.

### Phase 3 — external compositor contract (camada 2)

**Status:** not started. Server work is small; most work is in the
compositor (kicomp, KWin fork `kiyoshi-forks/kwin5`).

The contract: **a compositor does windowed HDR by presenting a PQ buffer per
output and tagging the window it presents with `hdr_output_metadata`.** For
the server that is just a fullscreen HDR window on that CRTC → PASSTHROUGH
(phase 2). No new protocol.

Server side:
1. Make sure the tree walk finds what a compositor actually presents: the
   Composite Overlay Window (COW) or a child of it, a fullscreen per-output
   window, or a per-CRTC Present target. Check how the COW sits in root's
   child list in our tree (`Xext/composite/compoverlay.c`) and whether the
   walk needs a special case for it.
2. Write the contract down in `doc/xis-hdr.md` (compositor section): which
   properties to read (`HDR`, `HDR capable`, `SDR white level` per output,
   `hdr_output_metadata` per client window), what to set, what the server
   does in return, and that the compositor must tone map for non-HDR outputs
   itself (TONEMAP only applies to a tagged window covering the CRTC, which
   for a compositor would be its own already-composited output; it must
   therefore only tag outputs that are in HDR).
3. **Open problem — getting the client's 10-bit pixels.** The layer renders
   into a depth-30 **child** window. Under a compositing manager the top-level
   is redirected and the child's content is composited into the parent's
   8-bit pixmap by the server's automatic redirection, losing precision (PQ
   in 8 bits bands badly). Options to evaluate in this phase:
   - the compositor finds subwindows carrying `hdr_output_metadata` and
     manually redirects them (`XCompositeRedirectWindow` on the child) to get
     their own 10-bit pixmap and composite it itself;
   - the server skips automatic composition of such a child into the parent
     and exposes the child pixmap via `NameWindowPixmap`.
   Pick one and specify it; the first needs no server change.
4. Optional hint: honour cepelinas' `__X11HDR_SDR_PARAMS` (STRING,
   `"nits,contrast,saturation,hue,brightness,gamma"` or `"default"`) as a
   per-window SDR brightness hint for compositors only. Document; the server
   doesn't act on it.

Compositor side (separate repos/sessions): composite in PQ/BT.2020 into the
per-output buffer (our per-CRTC flip), SDR windows scaled to `SDR white
level`, HDR windows passed through or tone mapped, set the property on the
output window. When no HDR window is visible on an output, the compositor
should delete the property and composite in plain SDR again: DESKTOP mode
already converts SDR to PQ in hardware, so SDR-only frames cost nothing extra.

Done when: kicomp or KWin shows a windowed HDR mpv/vkcube correctly on an HDR
output and tone mapped on an SDR one, and the doc describes the contract.

### Phase 4 — no-compositor windowed fallback (camada 3, optional)

**Status:** not started.

Without a compositor, a non-fullscreen HDR window is copied by Present into
the 8-bit screen pixmap and shown as if it were SDR (washed out in DESKTOP,
wrong in OFF). Fix only where it's needed: when Present copies a pixmap whose
window (or ancestor) carries `hdr_output_metadata`, use a glamor shader that
converts PQ BT.2020 → SDR BT.709 (tone mapped to the SDR range, scaled to
`SDR white level` when the output is in DESKTOP). Costs one shader pass per
copy of that window only.

Reference: dec05eba `glamor/glamor_color.c` (`e8fc71d8e7`) and
`ms_hdr_pq_to_sdr_conversion`. Hook point: Present's copy path
(`present_copy_region` / the scmd copy in `Xext/present/present_scmd.c`), or a
DDX hook so it stays modesetting-only. Highlights above SDR white are rolled
off, not shown as HDR: this is "correct-looking", not "HDR".

Also covers the depth-30 child-window case of §5 phase 3 item 3 when no
compositor runs.

Done when: windowed mpv HDR without a compositor looks like SDR mpv of the
same clip, and SDR-only sessions show no measurable GPU change.

### Phase 5 — in-server compositor (camada 4, optional, deferred)

**Status:** deferred; revisit only after phases 0–3 and if a lightweight-WM
use case shows up.

Requirements if it ever happens:
- Never take `_NET_WM_CM_S<n>` while another client owns it; when an external
  compositor requests it, hand it over and stop compositing (unredirect).
- Off by default, separate option (not a replacement for `TearFree`).
- Build on our per-CRTC flips, not on a second Present per-CRTC design.
- Reference: dec05eba `composite/compserver.c`, `compdirect.c`,
  `compsync.c`/`comphold.c` (resize sync, `_NET_WM_SYNC_REQUEST`,
  `_NET_WM_FRAME_TIMINGS`), modesetting `compositor.c`. The resize-sync piece
  may be worth evaluating on its own.


## 6. Interactions checklist (every phase must keep these working)

| Feature | Rule |
|---|---|
| Per-CRTC flips (`doc/present-per-crtc-flip.md`) | HDR state is per CRTC; flip start/end marks dirty; drain-on-reconfig and LeaveVT flush ordering are unchanged; HDR restore goes in the same "while still master" window. |
| TearFree per-CRTC shadows | LUTs apply to whatever buffer scans out; no change needed. Check that TearFree's flip on a PASSTHROUGH CRTC doesn't reset colour props (atomic plane props only). |
| Rotation / reflection | DESKTOP and TONEMAP are fine. PASSTHROUGH works but via copy (rotation shadow is 8-bit): banding, accepted. |
| Per-output SCALE / XInputTransform / AutoDPI | Tree walk must use the CRTC box in screen coordinates like `present_crtc_box`. |
| `xrandr --gamma`, night light, `CTM` RandR prop | Folded into DESKTOP degamma/CTM and TONEMAP gamma/CTM; not applied in PASSTHROUGH. |
| PRIME / reverse PRIME outputs on another GPU | Out of scope; `HDR capable` 0 there (`is_gpu` screens). |
| Xnotify screen capture gating | Unchanged. Captures of a PASSTHROUGH output show PQ values; document. |
| RandR leases | A leased connector is not ours; skip it. |
| Server crash | The kernel keeps the last colour state until the next master sets its own. Document; nothing to do. |


## 7. Test environment (this machine, as of 2026-10-30)

- Kernel 5.15.0-101, amdgpu (display) + nvidia-drm (render offload).
- amdgpu exposes per CRTC `DEGAMMA_LUT`, `CTM`, `GAMMA_LUT`, per connector
  `HDR_OUTPUT_METADATA` and `max bpc` (8..16, currently 8), but **no
  `Colorspace` connector property** on 5.15. amdgpu gained it in a later
  kernel (around 6.6–6.7, to verify). Without it the server can't signal
  BT.2020 colorimetry explicitly. Whether amdgpu DC on 5.15 switches
  colorimetry by itself when PQ metadata is set is **unverified**; phase 1
  must work without the property and log that it's missing.
- Connected monitors: DP-1 "Mi Monitor" and HDMI-A-1 "SyncMaster". **Neither
  EDID has an HDR static metadata block** → both report `HDR capable 0`.
  So on this machine: phase 0 fully testable; phase 1 only with
  `XIS_HDR_FORCE_CAPABLE=1` (pipeline/teardown checks, not image quality);
  phase 2 TONEMAP fully testable; PASSTHROUGH and real HDR need an HDR
  monitor/TV and ideally a newer kernel.
- Inspect state with `drm_info` (installed). `max bpc` may need raising to 10
  for HDR on some monitors; decide in phase 1 whether the server sets it when
  `HDR` is 1.


## 8. Open questions

1. Colorimetry without the `Colorspace` property (5.15 amdgpu): test on real
   HDR hardware; maybe require a newer kernel for `HDR capable`.
2. Should the server raise `max bpc` to 10 when `HDR` is 1?
3. TONEMAP default: always on (dec05eba) or per-output property?
4. Phase 3 item 3: how a compositor gets the depth-30 child pixels.
5. Should PASSTHROUGH require an actual flip (refuse on copy paths) or stay
   geometry-based (§4.3)? Plan says geometry-based; revisit after testing.
6. Legacy (non-atomic) colour commit fallback: only if someone needs HDR
   without `Option "Atomic"`.
7. Plane colour pipeline (KMS colorop API, Linux 6.19+, dec05eba
   `colorop.c`): convert SDR→PQ per plane instead of per CRTC. Removes the
   amdgpu flicker on DESKTOP↔PASSTHROUGH switches (the CRTC stays
   passthrough), keeps the cursor and overlay planes correct without CPU/shader
   conversion, and is the clean base for phase 2 item 7b. Optional path when
   the kernel has `DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE`; the CRTC-LUT path
   stays for older kernels (this machine runs 5.15).


## 9. Forward compatibility with cepelinas' HDRProto

Keep the per-window HDR state behind one internal accessor
(`ms_hdr_window_metadata(WindowPtr)`), so a future extension can feed it from
`ApplyVulkanProperties` (`VkHdrMetadataEXT` → `struct hdr_output_metadata`,
`VkSurfaceFormat2KHR` colour space → PQ/sRGB) without touching the
decision/commit code. Not planned unless clients appear.


## 10. References

- dec05eba Xorg fork: <https://github.com/dec05eba/xserver>
  (`DIVERGENCE.md`, `hw/xfree86/drivers/modesetting/hdr.c`)
- dec05eba VK_hdr_layer (X11): <https://github.com/dec05eba/VK_hdr_layer> [vkhdr]
- dec05eba mpv fork: <https://github.com/dec05eba/mpv> [mpv]
- cepelinas9000 (Tautvis) XLibre HDR branch:
  <https://github.com/cepelinas9000/xserver/tree/wip/x11hdrV2>
- XLibre discussion #251 (X11 HDR):
  <https://github.com/orgs/X11Libre/discussions/251#discussioncomment-16089204>
- Upstream VK_hdr_layer (Wayland, Xaver Hugl / Zamundaaa):
  <https://github.com/Zamundaaa/VK_hdr_layer>
- Linux KMS colour management and connector properties:
  <https://docs.kernel.org/gpu/drm-kms.html>
- In-tree: `doc/present-per-crtc-flip.md`, `include/edid.h`
  (`struct monitor_hdr_data`), `hw/xfree86/ddc/interpret_edid.c`
  (`xf86DoInterpretHDRMetadata`), `test/ddc-tests.c`.

[vkhdr]: https://github.com/dec05eba/VK_hdr_layer
[mpv]: https://github.com/dec05eba/mpv
