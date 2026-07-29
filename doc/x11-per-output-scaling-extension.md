# X-INPUT-SCALE — per-CRTC cursor confinement for HiDPI compositors

Server-side extension that lets a compositor confine the pointer, per CRTC,
to a sub-rectangle of that CRTC's **physical** scanout area — the "logical"
region it is actually drawing sharp, higher-density content into for that
output. The core mechanism touches exactly one thing: where the cursor is
allowed to be. It does **not** remap window geometry or click/hit-testing —
those stay exactly as they already are. Two additional pieces build on the
same confinement state: scaling the *hardware cursor itself* (position and
bitmap, §Hardware cursor) so it stays sharp and correctly placed without
falling back to a laggy software-composited cursor, and an experimental
RandR geometry override to help popup placement (§Experimental). Both are
opt-in by construction — they only ever affect a CRTC that has confinement
active — but the RandR override is newer and less battle-tested than
everything else here.

## Why this exists, and why it's this narrow

An earlier version of this extension (`X-INPUT-TRANSFORM`) also remapped
absolute pointer coordinates physical↔logical before hit-testing, on the
theory that a compositor drawing windows at a different size than their real
X11 geometry needs the server to translate clicks into that geometry. In
practice, that wasn't true for this project's compositor: window geometry and
click coordinates already agree without any remapping (the compositor's
logical/physical duality is entirely internal to it and never surfaces at the
X11 protocol level). Adding a second, server-side notion of "logical space"
on top of that just created a second source of truth that could disagree with
the first — which is exactly what caused popups to open misaligned with the
click that triggered them, since toolkits compute popup placement by mixing
the pointer position (remapped) with RandR geometry queries (not remapped).

Removing the coordinate remap fixed that, for free — it was solving a problem
that didn't exist. What's left, and what this extension is scoped to, is
**cursor confinement**: preventing the pointer from wandering into scanout
pixels the compositor isn't currently drawing "logical" content into. That
piece genuinely cannot be done compositor-side: cursor motion runs through
the core input pipeline (`mi/mipointer.c`) on every single event, and a
client reacting after the fact with `XWarpPointer` would be visibly
laggy/jittery compared to a synchronous clamp inside that pipeline. Anything
else — like keeping popups on screen — the compositor/WM can and should
already do on its own by clamping `ConfigureWindow` for override-redirect
windows against the same logical bounds it already tracks internally; no
server involvement needed for that.

## Scope: shrink-only, HiDPI (downscale) only

A confinement box must fit entirely within its CRTC's current physical
scanout box (`XISSetCrtcConfine` returns `BadMatch` otherwise). This
extension only ever *shrinks* where the cursor can go — it never grows the
desktop. A CRTC that wants *more* logical space than it has physical pixels
(a low-DPI/upscale output) is out of scope here; use RandR's own CRTC
transform proportionally instead (`xrandr --output <o> --scale
<96/dpi>x<96/dpi>`, i.e. `RRSetCrtcTransform`), which already owns growing
the screen size and already confines the cursor correctly for that case via
`RRConstrainCursorHarder` (`Xext/randr/rrcrtc.c`).

## Protocol

Extension name: `X-INPUT-SCALE`, version 1.0. Wire definitions in
[`Xext/inputscale/inputscaleproto.h`](../Xext/inputscale/inputscaleproto.h).

| Opcode | Request | Effect |
|--------|---------|--------|
| 0 | `XISQueryVersion(major, minor)` | standard version negotiation |
| 1 | `XISSetCrtcConfine(crtc, x, y, width, height)` | confine the cursor to this desktop-space box while over `crtc`; must fit inside the CRTC's physical scanout box or `BadMatch` |
| 2 | `XISGetCrtcConfine(crtc)` → `(active, x, y, width, height)` | read the current confinement box |
| 3 | `XISResetCrtcConfine(crtc)` | disable confinement for this CRTC |

Coordinates are absolute desktop-space pixels, the same convention RandR
itself uses for CRTC position/size — no matrix, no transform, just a
rectangle.

## Server implementation

- **State** lives directly on the RandR CRTC
  ([`include/randrstr.h`](../include/randrstr.h), `struct _rrCrtc`):
  `confine_active`, `confine_client`, `confine_box`. Independent of any
  RandR transform field.

- **Cursor confinement:** `pScreen->ConstrainCursorHarder` is wrapped and
  *always* chains to the previously-installed hook (normally RandR's own
  `RRConstrainCursorHarder`) **first, unconditionally** — that is what gives
  a multi-monitor layout its ordinary "screens are closed boxes, open only
  where they touch" behavior (respecting actual per-CRTC alignment/height
  differences, corners, gaps between non-adjacent edges), and it has to keep
  working exactly like a stock setup regardless of whether some *other* CRTC
  on the screen has an active confine box. Only once that produces an
  already screen-valid `(x, y)` does this extension look at whether it
  landed on a CRTC that has its own confine box active, and if so, tightens
  further into *that CRTC's own box* — never into a different CRTC's. So
  confining one output can never trap the cursor away from an adjacent,
  unconfined (or independently confined) one, and normal cross-monitor edge
  behavior for every *other* CRTC is completely unaffected by this
  extension being active anywhere on the screen.

  Chaining through to the saved original **re-asserts ourselves as the hook
  afterward**. This matters: some chained hooks (e.g. xf86's RandR-compat
  shim, `xf86RandR13ConstrainCursorHarder` in
  `hw/xfree86/modes/xf86RandR12.c`) do their own save/call/restore around
  `pScreen->ConstrainCursorHarder` and point it back at *themselves*
  unconditionally, assuming they're the only wrapper. Without the
  re-assert, the very first pointer motion before any compositor registers
  a confinement box would silently evict this extension's hook from the
  chain for good.

- **Auto-revert:** confinement boxes are owned by the setting client. A
  `ClientStateCallback` resets every CRTC owned by a client that disconnects
  (`ClientStateGone`/`Retained`), so a compositor crash can never leave the
  cursor stuck confined to a box nothing is drawing into anymore.

- **No click/hit-test remap.** Unlike the coordinate-transform design this
  replaced, nothing in `dix/getevents.c` is touched — the pointer position
  used for hit-testing, `QueryPointer`, and event reporting is never
  remapped. The two pieces below are the only places this project's
  logical/physical duality is allowed to leak past pure confinement, and both
  are scoped to a single, narrow consumer each.

## Hardware cursor: position and bitmap scaling

**Status: implemented and tested (modesetting, amdgpu). Nearest-neighbor
bitmap scaling only.**

A hardware cursor plane/sprite is positioned by the driver in *physical*
scanout coordinates, and its bitmap is uploaded at a fixed hardware size.
The pointer's logical position (`pPointer->x/y`) is correct for everything
else (hit-testing, `QueryPointer`, event reporting — never touched by any of
this), but handed straight to the driver it points the HW cursor at the
wrong physical location on a confined CRTC, and the bitmap looks undersized
relative to the panel's physical pixel density. Both are fixed entirely
inside `hw/xfree86/modes/xf86Cursors.c` — the shared `xf86Crtc` framework
used by `modesetting`, `amdgpu`, `nouveau`, and legacy `intel`, so this is
**not** modesetting-specific except for one small piece noted below.

`XInputScaleGetCrtcScale(RRCrtcPtr, double *sx, double *sy)`
(`Xext/inputscale/inputscale.c`, declared in `include/inputscale.h`) is the
single `physical / confine` ratio both position and bitmap scaling below
consume, so they can never disagree with each other. It takes an
`xf86CrtcPtr`'s `->randr_crtc` field to find the matching `RRCrtcPtr`.

Both `XInputScaleGetCrtcScale`/`XInputScaleActive` are declared `_X_EXPORT`
(`include/inputscale.h`) — `modesetting_drv.so` (and any other DDX driver
module) is a separately `dlopen()`ed shared object that only resolves
against the server's *exported* dynamic symbols, not its static-library
internals; missing this caused an "undefined symbol" at driver load time
the first time a driver-side caller was added.

### Position: per-CRTC, not upstream

**First attempt (reverted):** map the pointer's logical position to physical
once, upstream in `mi/mipointer.c`, based on which CRTC's confine box
contained it, and hand the single transformed value down through the normal
sprite-position plumbing. This worked for the simple case but broke down at
a monitor boundary: X.Org's own `xf86_crtc_set_cursor_position()`
(`hw/xfree86/modes/xf86Cursors.c`) already shows the hardware cursor
simultaneously on *every* CRTC whose physical area the (single, shared)
cursor position is within `cursor_info->MaxWidth/MaxHeight` of — this is
what makes cursor motion look continuous crossing between two ordinary
monitors. Since the upstream transform only activated once the point was
*inside* the confine box, the confined CRTC's cursor plane would show an
unscaled fragment of the bitmap, at the unscaled position, for however long
the pointer was straddling the boundary but hadn't yet crossed in — then pop
to the correct scaled position/size the instant it did.

**Fix:** scale locally, per CRTC, inside `xf86_crtc_set_cursor_position()`,
operating on that CRTC's own already-computed local offset (`crtc_x = x -
crtc->x`, already including the unscaled hotspot subtraction `xf86SetCursor()`
applies upstream in `hw/xfree86/ramdac/xf86HWCurs.c`) rather than the shared
desktop-space value every CRTC receives:

```c
crtc_x -= crtc->x;
crtc_y -= crtc->y;
if (XInputScaleGetCrtcScale(crtc->randr_crtc, &sx, &sy)) {
    crtc_x = lround(crtc_x * sx);
    crtc_y = lround(crtc_y * sy);
}
```

Each CRTC now decides independently, continuously, using only its own
factor — the confined CRTC's cursor plane is already correctly scaled and
positioned *before* the hotspot crosses in, so the straddle looks like a
normal multi-monitor cursor transition instead of a pop. The unscaled
hotspot subtraction upstream needs no separate correction here: scaling the
whole already-hotspot-adjusted offset uniformly is algebraically the same as
subtracting the *scaled* hotspot from the *scaled* tip position — exactly
what's visually correct once the bitmap itself is scaled by the same factor.
(A short-lived first implementation of this fix folded a hotspot correction
term into a since-removed `XInputScaleLogicalToPhysicalCursor()` upstream
helper; it's gone now that the math falls out for free at the right layer.)

Only the `RR_Rotate_0` path is scaled; a rotated *and* confined CRTC is an
unhandled combination for now.

### Bitmap: nearest-neighbor upsampling, two layers had to agree

Two things independently determine what a hardware cursor plane displays,
and both needed to agree on the glyph's *effective* size:

- `xf86_crtc_load_cursor_argb()` (`hw/xfree86/modes/xf86Cursors.c`) composes
  a cursor glyph into the fixed-size hardware buffer, **once per enabled
  CRTC** (every CRTC already gets its own independent upload here, which is
  what makes a per-CRTC scale factor possible with no cross-CRTC
  interference). It normally maps each destination pixel back to a source
  pixel 1:1 (after accounting for rotation); when `XInputScaleGetCrtcScale()`
  reports an active scale for that CRTC, the reverse-mapped coordinate is
  additionally divided by the scale factor before the source lookup —
  nearest-neighbor upsampling of the glyph into the same fixed buffer, so it
  now occupies proportionally more of it.

- `drmmode_load_cursor_argb_check()` — the **one `modesetting`-DDX-specific
  piece** (`hw/xfree86/drivers/video/modesetting/drmmode_display.c`) —
  independently decides how much of that buffer is actually "the cursor":
  which hardware cursor plane size to use, and what region to crop/upload,
  by reading `cursor->bits->width/height` straight off the `CursorPtr`, with
  no idea the layer above already painted a larger glyph into the buffer.
  Left alone, it cropped back down to the *original*, unscaled dimensions,
  visibly clipping the correctly-scaled content. Fixed by applying the same
  `XInputScaleGetCrtcScale()` factor to `glyph_width/height` before they
  drive hardware-size selection and cropping. Other DDX drivers not present
  in this tree (`amdgpu`, `nouveau`, ...) likely have their own, separate
  implementation of this same kind of function and may need the equivalent
  fix in their own source trees.

  This turned out to also close a gap for free: hardware-size selection
  already picks the smallest hardware-supported cursor plane size `>=` the
  glyph, and already returns `FALSE` (graceful software-cursor fallback) if
  none fits. Since it now sees the *scaled* size, a scale factor large
  enough that no supported plane size fits correctly falls back instead of
  silently clipping.

**Known rough edge, not yet handled:** nearest-neighbor only — blockier than
a "real" higher-density hardware cursor would look. An accepted tradeoff for
latency, per the original motivation, but bilinear would look better if the
cost is acceptable.

## Experimental: lying in RandR geometry replies

**Status: experimental, not part of the core extension above, opt-in by
nature of only affecting a CRTC that has confinement active.**

The core extension only stops the cursor from *moving* into unused scanout
pixels. It does nothing about a toolkit computing where to place a popup:
that math typically starts from the pointer position (already correct,
untouched) but also consults monitor/output geometry via RandR
(`XRRGetCrtcInfo`, `XRRGetMonitors`) or `_NET_WORKAREA` to decide where the
screen "ends". `_NET_WORKAREA` is compositor-owned (KWin can already set it
freely, no server change needed) but is a single box per virtual desktop in
this project's KWin fork (`Workspace::updateClientArea()`), not per monitor —
forcing it to the confined CRTC's box would break popup placement on any
*other*, unconfined monitor via `QScreen::availableGeometry()`'s
geometry ∩ workarea intersection. RandR's per-CRTC/per-monitor replies don't
have that problem: each output already gets its own independent rectangle.

So, while a CRTC has an active confine box, two reply paths report that box
instead of the true physical scanout box:

- `ProcRRGetCrtcInfo()` (`Xext/randr/rrcrtc.c`) — backs `XRRGetCrtcInfo`.
- `RRMonitorGetCrtcGeometry()` (`Xext/randr/rrmonitor.c`) — backs
  `XRRGetMonitors` for server-generated (non client-defined) monitors, and is
  recomputed live on every query, so it always reflects current confinement
  state.

Both are narrowly scoped: only the confined CRTC's own reply is affected,
every other output's geometry is untouched, and the lie disappears the
instant confinement is reset or the owning client disconnects (same
auto-revert as everything else here) — `pScreen->width/height`, RandR screen
resources, and every other reply path are untouched.

**Known tradeoff, accepted deliberately for now:** anything else that reads
these same replies while confinement is active — `xrandr`, KDE Display
Settings/`kscreen`, screen capture/remote-desktop tools — sees the confined
box too, not the true physical geometry. There's no per-client filtering;
the lie is visible to every client querying that CRTC while it's active.
This is considered acceptable for now because it's exactly reversible and
scoped to only while a compositor has actively chosen to scale that output,
but it's the reason this piece is called out as experimental rather than
folded into the "done" list above.

## Build & enable

Meson option `input_scale` (default `true`) → `CONFIG_INPUT_SCALE`. The
extension is present but **inert until a confinement box is registered**, so
leaving it enabled is safe. Disable at runtime with `-extension X-INPUT-SCALE`.

```sh
meson setup build -Dinput_scale=true
ninja -C build
```

## Testing in isolation

```sh
cc Xext/inputscale/test/xis-smoke-test.c -o xis-smoke-test \
   $(pkg-config --cflags --libs xcb xcb-randr)
build/hw/vfb/Xvfb :99 +extension RANDR -screen 0 1920x1080x24 &
DISPLAY=:99 ./xis-smoke-test
```

Checks: version negotiation, set/get/reset, a box exceeding the physical CRTC
being rejected, absolute XTEST motion into the unconfined area being clamped
back into the box, and auto-revert when a second client disconnects.
