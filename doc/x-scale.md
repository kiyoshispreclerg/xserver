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
| 2 | `XScaleGetCrtcScanout(crtc)` → `(flags, scanoutWidth, scanoutHeight, x, y, width, height, rotation)` | the CRTC's physical size, its logical box in the root, whether it can take a source, and its RandR rotation |
| 3 | `XScaleSetCrtcScanoutSource(crtc, drawable, srcX, srcY)` | feed the CRTC from `drawable`; `None` gives it back |
| 4 | `XScaleCopyCrtcScanout(crtc, drawable, srcX, srcY, width, height, dstX, dstY)` | copy what the CRTC scans out, at the monitor's resolution, into `drawable`; for recorders (see "Recording at the monitor's resolution") |
| 5 | `XScalePresentCrtcScanout(crtc, pixmap, serial, update)` | show a whole frame on the CRTC, flipped where the server can (see "Presenting frames") |

`flags`:

- `XScaleScanoutCapable` — a source can be set now;
- `XScaleScanoutActive` — some client's source is in use;
- `XScaleScanoutMine` — that source is the asking client's;
- `XScaleScanoutFlippable` — a presented frame would be flipped onto
  the CRTC right now rather than copied. A hint, not a promise.

### When a CRTC is capable

A CRTC is capable when it has a scale-only transform (no shear or
projection) *and* its DDX can take a source.

A **rotated or reflected** CRTC is capable only with the same scale on
both axes (the hardware cursor has to follow it). Its source is the
picture as the CRTC scans it out: the logical picture scaled, then
rotated and reflected the way RandR maps the CRTC to the root
(`RRTransformCompute()`), `scanoutWidth` x `scanoutHeight`, the mode's
size, with `rotation` saying how. Captures stitch any source back into
the root through the CRTC's whole transform, so they come out upright.

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

### Presenting frames

A compositor with a swapchain of its own can skip the source drawable and
hand over each finished frame instead:
`XScalePresentCrtcScanout(crtc, pixmap, serial, update)`, with `pixmap`
exactly the scanout's size and of the root's depth.

- The latest presented pixmap **is** the CRTC's source. Everything above
  still applies to it: who may set it (`BadAccess`), when it is dropped,
  screen captures, the hardware cursor. `XScaleSetCrtcScanoutSource(None)`
  gives the CRTC back.
- Where the DDX can, the pixmap is **page-flipped** onto the CRTC: it
  becomes the screen, with no copy at all. Otherwise its `update` (an
  XFixes region in scanout coordinates, `None` for all of it) is copied
  like any source. A buffer allocated for scanout through DRI3, with the
  modifiers DRI3 offers, is what can be flipped.
- Each request gets one `XScaleScanoutComplete(crtc, serial, mode, msc,
  ust)`, `mode` being Flip, Copy or Skip (replaced before it was shown),
  and later one `XScaleScanoutIdle(crtc, serial, pixmap)`. Both go to the
  requesting client only, with no selection needed.
- Until the Idle the pixmap is the server's: a flipped one is the screen
  until the next flip lands, and the latest one is what the server copies
  from again. Three buffers are enough for one on screen, one queued and
  one being drawn.
- A request sent while the previous flip hasn't landed waits for it; a
  later one replaces a waiting one (Skip).
- When the source is dropped, every pixmap is reported idle at once, and
  the DDX goes back to its own buffer at the next vblank.

## Server implementation

Presented frames live in `xscale.c` too ("Presented pixmaps"): the
frame bookkeeping, the events, and three optional `XScaleScanoutFuncs`
(`can_flip`, `flip`, `unflip`) the DDX answers with `XScaleFlipDone()`,
`XScaleScanoutLeft()` and, for copies, `XScaleScanoutCopyToken()` /
`XScaleScanoutCopied()` from the flip that shows them. modesetting
implements them on top of its per-CRTC flips (`ms_do_pageflip_crtc()`).


- **`Xext/xscale/xscale.c`** — the protocol, and the per-CRTC source state
  (`RRCrtcRec.xscale_source`, opaque to everyone else). It tracks the
  source with Damage and drops the source when its client goes away or the
  drawable is destroyed. `XScaleCrtcChanged()` is called from
  `RRCrtcNotify()` (`Xext/randr/rrcrtc.c`).
- **`include/xscale.h`** — the DIX/DDX interface:
  - `XScaleScanoutFuncs` (`capable`, `source_changed`), which a DDX
    registers per screen with `XScaleSetScanoutFuncs()`;
  - `XScaleGetScanoutFuncs()`, for a driver that takes over some CRTCs
    and leaves the rest to the funcs registered before it;
  - `XScaleTakeScanoutDamage()` and `XScaleCopyScanoutSource()` for the
    DDX's redisplay;
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
     shadow next (modesetting's dirty-fb notification) needs no change.

  Dropping the source damages the whole shadow, so it is filled from the
  root again.

  For a driver that renders a transformed CRTC into scanout buffers of its
  own instead (`XF86DriverTransformOutput`), the layer exports the same two
  steps: `xf86CrtcDamageFromScanoutSource()` (step 3) and
  `xf86CrtcTransformRedisplay()`, which renders a region of the CRTC into
  the driver's buffer, from the source 1:1 or from the root through the
  transform.
- **`hw/xfree86/drivers/video/modesetting/`** — with TearFree on a
  transformed CRTC, modesetting renders it straight into its TearFree back
  buffer (`drmmode_crtc_rotate()`), and the modes layer keeps no shadow for
  it. Its own `XScaleScanoutFuncs` (`drmmode_xscale.c`) handle those
  CRTCs: a source is copied straight into the TearFree back buffer, and
  `scanout_pixmap` is the TearFree front buffer. Every other CRTC goes to
  the modes layer's funcs.

### Screen capture

A CRTC with a source shows the source, not the part of the root it covers.
The compositor draws only into the source, so that part of the screen
pixmap holds whatever was last left there. Recorders that read "the
screen" with a root `GetImage` (`XGetImage`, `XShmGetImage`; OBS,
`ffmpeg -f x11grab`) would record it frozen.

To prevent that, `xscale.c` wraps `ScreenRec.GetImage`. A root `ZPixmap`
capture that overlaps such a CRTC reads the rest of the root as usual, and
fills the CRTC's logical box with its source, scaled down to logical size
(bilinear): the picture the CRTC shows, at the root's resolution.

- It only reads: nothing on screen changes, so there is no damage and no
  feedback with XDamage-driven recorders.
- It costs nothing while no CRTC has a source, and otherwise only while
  something captures.
- It chains with Present's equivalent for per-CRTC flips
  (`present_flip_getimage()`).

The other core way to grab the screen is a `CopyArea` from the root with
`IncludeInferiors` into a pixmap. It stays on the GPU with no readback, and
it is what OBS's XiS capture uses when Present advertises
`XiSRootCopy`. That copy is split the same way, through a GC wrap modelled
on Present's:

- the rest of the root comes from the real `CopyArea`;
- each CRTC's part is composited from its source, scaled down, straight
  into the destination, so everything stays GPU work.

The split only happens when it reproduces a plain copy exactly: an
unclipped GC, `GXcopy`, all planes, and a destination that doesn't share
storage with the root or a source. Any other copy is left alone.

Both wraps are installed from the DDX's screen init, only on screens that
can have sources, because the GC private has to be registered before any
GC exists. A screen where installing them fails never reports a CRTC
capable, so a source can never leave captures stale. That is also what
keeps Present's `XiSRootCopy` promise true with X-SCALE.

### DDX coverage

- **Capable:** modesetting (with or without TearFree), and any xf86 DDX
  that uses the modes layer's shadow.
- **Not capable:** a DDX that performs transforms on its own scanout
  buffers (`driverIsPerformingTransform & XF86DriverTransformOutput`, e.g.
  amdgpu with TearFree) and doesn't register its own funcs the way
  modesetting does, Xvfb, Xephyr and Xwayland. All of these keep the
  stock magnification: correct, but blurry.
- **GPU screens** (PRIME secondaries) are never capable.

## Hardware cursor

The software cursor is drawn into the root, which a CRTC with a source no
longer shows, so the hardware cursor has to take over there. It does on
every scaled CRTC, source or not:

- `xf86_use_hw_cursor()` (`hw/xfree86/modes/xf86Cursors.c`) normally
  refuses any transformed CRTC. It allows one whose transform is a plain
  scale, alone or with a rotation or reflection (then the same on both
  axes): `xf86CrtcCursorScale()`.
- `source_changed` calls `xf86CursorResetCursor()`, so that choice is made
  again the moment a source is set or dropped.

On such a CRTC, `xf86CrtcCursorScale()` gives the physical/logical ratio,
and the cursor code uses it for:

- **position** — `xf86_crtc_set_cursor_position()` scales the CRTC's own
  local offset (not the shared desktop position), so the cursor is placed
  right even while straddling an unscaled neighbour; on a rotated CRTC the
  offset and the hotspot are scaled before the rotation is applied;
- **bitmap** — `xf86_crtc_load_cursor_argb()` upsamples the glyph (nearest
  neighbour) into the hardware buffer, and the modesetting DDX's
  `drmmode_load_cursor_argb_check()` sizes its crop from the scaled glyph,
  so the cursor keeps its logical size.

Keeping the hardware cursor on scaled CRTCs without a source matters too:
a single software cursor anywhere blocks every page flip on the screen.

Two limits:

- a glyph that no longer fits the hardware buffer once scaled is clipped;
- if the hardware cursor can't be used at all (another CRTC with a
  transform that isn't a scale, or a scale that differs between the axes
  of a rotated CRTC, forces the software cursor for the whole screen), the
  cursor is not visible on a CRTC with a source.

## Cost

The table compares a frame on a scaled 1080p output with the same output
unscaled and page-flipped per CRTC by the compositor:

| | Per-CRTC flip, unscaled | X-SCALE source | X-SCALE presented, flipped |
|---|---|---|---|
| Copies per frame | 0 | 1 into the source (compositor) + 1 by the server: into the TearFree back buffer, or into the shadow without TearFree | 0 |
| Extra VRAM | — | the source drawable (≈ 8 MB at 1080p, ≈ 33 MB at 4K); without TearFree also the shadow, the same size again | none beyond the compositor's own swapchain |
| Idle desktop | nothing | nothing (every copy is damage-driven) | nothing |

A presented frame that can't be flipped costs one copy, by the server, of
its `update` region.

- Unscaled outputs keep per-CRTC flips; nothing changes for them.
- A transformed CRTC never page-flipped anyway, because it scans out its
  shadow or its TearFree buffers. Compared with the stock magnification,
  X-SCALE replaces a bilinear scaling blit with a 1:1 copy.
- With TearFree, modesetting renders a transformed CRTC, from the root or
  from a source, straight into its TearFree buffers, and keeps no shadow
  for it. That holds for any `xrandr --scale`, `--rotate` or
  `--transform`, with or without X-SCALE.
- Presented frames are flipped onto the CRTC by modesetting when
  TearFree renders that CRTC (the TearFree buffers then sit idle), when
  per-CRTC flips are allowed on its output (`PerCRTCFlip`, atomic KMS),
  and when the buffer's format and modifier can be scanned out. Leaving a
  flipped frame is a TearFree flip of a back buffer refilled in full, so
  no stale picture is shown. Without TearFree, or on any other DDX,
  presented frames are copied.

### Recording at the monitor's resolution

The stitched root captures above are what every existing recorder gets,
at logical resolution. A recorder that knows X-SCALE can offer the
monitor's real resolution too:

1. Check that the extension is there.
2. Call `XScaleGetCrtcScanout` per CRTC. `scanoutWidth`/`scanoutHeight` is
   the size of what the CRTC scans out, and `x`/`y`/`width`/`height` its
   logical box. The ratio between the two is also the factor for drawing
   the cursor, whose XFixes position stays logical.
3. Each frame, call `XScaleCopyCrtcScanout` into a pixmap of the scanout
   size.

What gets copied is the scanout itself, compositor or not:

- the sharp picture when a compositor set a source;
- otherwise the server's magnified root, which is what the monitor shows
  too;
- a CRTC that isn't scaled is its root area, 1:1. A rotated or reflected
  monitor also counts as unscaled here, because it has the root's density.

To record a region across monitors with different scales, compose at the
highest scale among them, scaling the others up. That is what KWin's
`ScreenShot2` does with `ScreenShotNativeResolution`.

The request goes through the same Xnotify permission as every other screen
read (`XNOTIFY_SCREEN`), and answers `BadAccess` when it's denied.

The cost is one GPU copy in the common case. The DDX's `scanout_pixmap`
hook gives the buffer the CRTC scans out (the xf86 shadow, or
modesetting's TearFree front buffer), and the server
copies from it, exactly like the `CopyArea` from the root OBS already does.
When the DDX doesn't expose that buffer (amdgpu with TearFree), the server
renders the root through the CRTC transform instead, still on the GPU.

The request promises the scanout, not the shadow, so later work changes
only what `scanout_pixmap` returns, and recorders don't notice:

- a presented frame flipped onto the CRTC is copied from directly;
- freeing idle shadows: the hook returns NULL.

## Known limitations

- **Screen capture:** root `GetImage` and `CopyArea` captures are stitched
  (above) and come at logical resolution; `XScaleCopyCrtcScanout` gives the
  monitor's resolution. A root `CopyArea` through a clipped GC, another
  raster op or a plane mask still reads the stale area.
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
