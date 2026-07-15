# X-INPUT-SCALE — per-CRTC cursor confinement for HiDPI compositors

Server-side extension that lets a compositor confine the pointer, per CRTC,
to a sub-rectangle of that CRTC's **physical** scanout area — the "logical"
region it is actually drawing sharp, higher-density content into for that
output. It touches exactly one thing: where the cursor is allowed to be. It
does **not** remap window geometry, click/hit-testing, or any RandR geometry
query — those stay exactly as they already are.

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

- **No input/geometry hooks anywhere else.** Unlike the coordinate-transform
  design this replaced, nothing in `dix/getevents.c` or any RandR query path
  is touched — that's the whole point.

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
