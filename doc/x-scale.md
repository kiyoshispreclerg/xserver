# X-SCALE — compositor-supplied scanout for scaled outputs

X-SCALE gives X11 **sharp per-output scaling** while keeping the whole
desktop in **one contiguous coordinate space**.

An output is scaled the ordinary RandR way, with a CRTC transform:

```sh
xrandr --output DP-1 --scale-from 1280x720 --pos 0x0 --output HDMI-1 --pos 1280x0
```

That alone already gets the geometry right everywhere. What it can't do is
make the scaled output sharp, and that one thing is what X-SCALE adds: a
compositor that drew the output at its physical resolution hands that
picture to the CRTC, instead of the server magnifying the logical root
into it.

Extension name `X-SCALE`, version **1.0**. Wire definitions are in
[`Xext/xscale/xscaleproto.h`](../Xext/xscale/xscaleproto.h); the DIX/DDX
interface is in [`include/xscale.h`](../include/xscale.h).

## Why it looks like this

A scaled output needs two things:

- a **logical** size, smaller than its physical resolution, which is where
  windows live and what input and the window manager see;
- pixels drawn at the **physical** resolution, so it isn't blurry.

A RandR transform gives the first for free and gets it right everywhere at
once. The CRTC's footprint in the root is its transformed size, so these
all agree with each other, with the outputs laid out edge to edge:

- the root and window geometry;
- the pointer's travel between outputs (`RRConstrainCursorHarder` uses the
  transformed bounds);
- `XRRGetMonitors`, `_NET_WORKAREA` and every toolkit.

What a transform doesn't give is sharpness. Such a CRTC scans out a shadow
buffer that the server fills by magnifying the logical root.

So the extension does exactly that one thing. A compositor gives a
transformed CRTC a **scanout source** — any drawable of the CRTC's physical
size — and the server copies what gets drawn into it onto the CRTC in
place of the magnified root.

An earlier experiment did it the other way round: it left the CRTC
untransformed, confined the pointer to the corner the compositor drew a
smaller desktop into, and magnified that. Scaling any output but the last
in a row then leaves a dead band in root coordinates between its logical
edge and its neighbour. The pointer can't cross it, and a window dragged
across loses its middle. Keeping the geometry in RandR avoids the problem
entirely.

## Protocol

| Opcode | Request | Effect |
|--------|---------|--------|
| 0 | `XScaleQueryVersion(major, minor)` | version negotiation; answers 1.0 |
| 1 | `XScaleSelectInput(window, eventMask)` | (un)subscribe to `XScaleScanoutNotify`; `eventMask` 0 cancels |
| 2 | `XScaleGetCrtcScanout(crtc)` → `(flags, scanoutWidth, scanoutHeight, x, y, width, height)` | the CRTC's physical size, its logical box in the root, and whether it can take a source |
| 3 | `XScaleSetCrtcScanoutSource(crtc, drawable, srcX, srcY)` | feed the CRTC from `drawable`; `None` gives it back |

`flags`:

- `XScaleScanoutCapable` — a source can be set now;
- `XScaleScanoutActive` — some client's source is in use;
- `XScaleScanoutMine` — that source is the asking client's.

### When a CRTC is capable

A CRTC is capable when it has a scale-only transform (no rotation,
reflection, shear or projection) *and* its DDX can take a source.

A DDX that can't never reports a capable CRTC. The compositor then renders
that output at logical size and lets the server magnify it, which is
exactly what any stock server does with `xrandr --scale`.

### Setting a source

`XScaleSetCrtcScanoutSource` never needs repeating per frame. The server
watches the drawable with Damage and, the next time it goes idle, copies
only what changed. The compositor just keeps rendering into the drawable.

Because the copy happens whenever the server goes idle, the drawable should
only ever change by whole frames: a buffer swap, or one copy of a finished
frame. Otherwise a half-drawn frame may be shown.

Errors:

- `BadMatch` — the CRTC isn't capable, or the drawable is on another
  screen, isn't depth 24/32, or doesn't cover the scanout size from
  `(srcX, srcY)`;
- `BadAccess` — another client's source is in use.

### When the server drops a source

The source is dropped, and an `XScaleScanoutNotify` sent, when:

- its client disconnects (so a crashed compositor leaves the stock
  magnified picture, never a frozen one);
- the drawable is destroyed;
- the CRTC is reconfigured so it stops being capable or changes scanout
  size.

A reconfiguration that keeps both, such as moving the output, keeps the
source and copies it again whole.

### Event

`XScaleScanoutNotify(window, crtc, flags, scanoutWidth, scanoutHeight,
timestamp)` is sent on every set, replace or drop, to every client that
selected it. The flags are the reply's, minus `XScaleScanoutMine`.

## Server implementation

- **`Xext/xscale/xscale.c`** — the protocol, and the per-CRTC source state
  (`RRCrtcRec.xscale_source`, opaque to everyone else). It tracks the
  source with Damage and drops the source when its client goes away or the
  drawable is destroyed. `XScaleCrtcChanged()` is called from
  `RRCrtcNotify()` (`Xext/randr/rrcrtc.c`).
- **`include/xscale.h`** — the DIX/DDX interface:
  - `XScaleScanoutFuncs` (`capable`, `source_changed`), which a DDX
    registers per screen with `XScaleSetScanoutFuncs()`;
  - `XScaleTakeScanoutDamage()` for the DDX's redisplay;
  - `XScaleGetCrtcScale()` for the cursor.

  Every function is documented in the header.
- **`hw/xfree86/modes/xf86XScale.c`**, called from `xf86Rotate.c` — the
  DDX side. It lives in the
  shared xf86 modes layer, so it covers every xf86 DDX that lets that layer
  handle transforms. For a CRTC with a source, `xf86RotateRedisplay()`:
  1. skips the root magnification;
  2. copies the source's damage 1:1 into the CRTC's shadow
     (`crtc->rotatedPixmap`);
  3. damages the screen pixmap over the CRTC's logical bounds. That is the
     same signal the magnification path gives, so whatever reads the
     shadow next (modesetting's TearFree buffers, its dirty-fb
     notification) needs no change.

  Dropping the source damages the whole shadow, so it is filled from the
  root again.

### DDX coverage

- **Capable:** modesetting (with or without TearFree), and any xf86 DDX
  that uses the modes layer's shadow.
- **Not capable:** a DDX that performs transforms on its own scanout
  buffers (`driverIsPerformingTransform & XF86DriverTransformOutput`, e.g.
  amdgpu with TearFree), Xvfb, Xephyr and Xwayland. All of these keep the
  stock magnification: correct, but blurry.
- **GPU screens** (PRIME secondaries) are never capable.

## Hardware cursor

The software cursor is drawn into the root, which a CRTC with a source no
longer shows, so the hardware cursor has to take over there:

- `xf86_use_hw_cursor()` (`hw/xfree86/modes/xf86Cursors.c`) normally
  refuses any transformed CRTC. It now allows one with a source.
- `source_changed` calls `xf86CursorResetCursor()`, so that choice is made
  again the moment a source is set or dropped.

On such a CRTC, `XScaleGetCrtcScale()` gives the physical/logical ratio,
and the cursor code uses it for:

- **position** — `xf86_crtc_set_cursor_position()` scales the CRTC's own
  local offset (not the shared desktop position), so the cursor is placed
  right even while straddling an unscaled neighbour;
- **bitmap** — `xf86_crtc_load_cursor_argb()` upsamples the glyph (nearest
  neighbour) into the hardware buffer, and the modesetting DDX's
  `drmmode_load_cursor_argb_check()` sizes its crop from the scaled glyph,
  so the cursor keeps its logical size.

Two limits:

- a glyph that no longer fits the hardware buffer once scaled is clipped;
- if the hardware cursor can't be used at all (another CRTC with a
  non-scale transform forces the software cursor for the whole screen),
  the cursor is not visible on a CRTC with a source.

## Cost

The table compares a frame on a scaled 1080p output with the same output
unscaled and page-flipped per CRTC by the compositor:

| | Per-CRTC flip, unscaled | X-SCALE source |
|---|---|---|
| Copies per frame | 0 | 1 into the source (compositor) + 1 into the shadow + 1 into the TearFree buffer, if TearFree is on |
| Extra VRAM | — | the source drawable + the shadow (≈ 16 MB at 1080p, ≈ 66 MB at 4K) |
| Idle desktop | nothing | nothing (every copy is damage-driven) |

- Unscaled outputs keep per-CRTC flips; nothing changes for them.
- A transformed CRTC never page-flipped anyway, because it scans out its
  shadow. Compared with the stock magnification, X-SCALE replaces a
  bilinear scaling blit with a 1:1 copy.
- A planned follow-up is to flip a scanout-capable source straight onto
  the CRTC instead of copying it into the shadow. That would remove the
  last two copies.

## Known limitations

- **Screen capture:** root `GetImage` captures read the logical root,
  which on a CRTC with a source holds whatever the compositor last left
  there. Recorders should use the compositor's own capture path.
- **Fullscreen unredirected windows:** a window unredirected on a CRTC with
  a source isn't shown, so the compositor must drop the source while it
  unredirects.

## Build & enable

The meson option `xscale` (default `true`) sets `CONFIG_XSCALE`. The
extension does nothing until a compositor sets a source. Disable it at
runtime with `-extension X-SCALE`.

## Testing

The protocol can be tested on Xvfb. Xvfb has no capable CRTC, so this
covers the refusal side:

```sh
cc Xext/xscale/test/xscale-smoke-test.c -o xscale-smoke-test \
   $(pkg-config --cflags --libs xcb xcb-randr)
build/hw/vfb/Xvfb :99 +extension RANDR -screen 0 1920x1080x24 &
DISPLAY=:99 ./xscale-smoke-test
```

The capable side needs an xf86 DDX on real hardware:

1. Scale an output with `xrandr --output X --scale-from WxH`, and move its
   neighbours to its new logical edge.
2. Run a compositor that sets a source (kicomp does).

Expected results:

- the output turns sharp;
- the cursor keeps its size;
- windows cross between outputs whole;
- killing the compositor drops back to the magnified picture.
