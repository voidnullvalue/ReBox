# Offline native UI core

This implements a small software menu and a presentation adapter for the
HR54's recovered native EGL/drawlist backend. DTVWM owns the rendering threads;
the client registers its own drawing surface and supplies a source texture.
There is no Druid, ITV, WebKit or replacement media player in this path.

The lifecycle and evidence are documented in
[NATIVE_UI_FINDINGS.md](../../docs/NATIVE_UI_FINDINGS.md). The adapter follows
stock BIST's software-image renderer. **Static recovery establishes the path;
host tests do not demonstrate receiver visibility.**

## Build and inspect on the host

From the repository root:

```sh
make -C tools/native-ui test preview
```

This compiles and runs only our host code. It also compiles
`receiver_bindings.c` to an **unlinked object** to check declarations; it does
not load receiver libraries, open devices, contact services, build a receiver
executable or deploy anything.

A C11 compiler and Python 3 are required; PNG conversion uses Pillow. Outputs:

* [samples/menu.png](samples/menu.png): 720×480 transparent menu overlay.
* [samples/menu-over-video.png](samples/menu-over-video.png): the same menu over
  a synthetic background for visual inspection, not a receiver capture.
* `build/menu.rgba`: top-row-first RGBA bytes, stride 2880.
* `build/test_native_ui`: raster, controller and fake native API tests.

The rasterizer has no image/font runtime dependencies. Its original 5×7 font
supports A–Z, digits and basic punctuation, with lowercase folded to capitals.
The example contains selected/unselected rows, a bitmap icon and progress bar.

## Files and boundaries

| File | Responsibility |
|---|---|
| `framebuffer.[ch]` | Explicit RGBA bytes; clipped source-over rectangles, image blits, text and dirty union. |
| `menu_model.[ch]` | Normalized key events, menu selection/visibility, incremental row/progress repaint and media callbacks. |
| `surface.[ch]` | Renderer-independent submit interface; retains damage on failure and skips clean frames. |
| `native_egl.[ch]` | Display/context/window/texture creation, uploads, retained quad publication and own-object teardown through an injected vendor API table. |
| `receiver_bindings.c` | Direct receiver symbol binding declarations; excluded from host executables. |
| `compositor_protocol.h` | Distinct ID namespaces, recovered UMP IDs and drawlist offset descriptors for analysis. Does not write shared memory. |
| `test_native_ui.c` | Byte-level raster checks, incremental/full-render comparison, input/media callback checks and fake EGL success/failure lifecycle. |
| `demo.c`, `png_preview.py` | Offline sample output. |

The input model consumes normalized events; it does not reimplement
keydispatcher. Media callbacks are explicit extension points for the existing
`playURL`, rate 0/1000 and stop adapters. Source entries are illustrative; this
core does not implement Jellyfin, IPTV or YouTube service clients.

## Native adapter contract

`hr54_egl_open(surface, api, width, height, depth)` returns zero on success.
Use a fresh/closed surface, a complete API table and a positive software raster
size no greater than 65535 in either dimension, matching the native image
wrapper's 16-bit fields. This prevents truncation; native allocation may impose
tighter limits. Depth is an explicit policy value; the example test's depth 10 is a fake
test argument, **not a recovered universal topmost stock layer**.

The vendor `eglCreateWindowSurface` has **six arguments**. Standard desktop EGL
headers/prototypes are incompatible with this receiver call. The adapter passes
the three middle arguments as zero and null sixth-argument attributes, exactly
as BIST does, then assigns depth through `eglDrawlistSetDepthDTV`.

```text
display 0 → initialize → config → context → vendor window → current → depth
→ allocate source texture
→ RGBA upload → complete textured quad → eglSwapBuffers
→ DTVWM retained frame → native drawing bridge → graphics output swap
```

`UiSurface` connects the framebuffer to this adapter:

```c
Hr54EglSurface native;
UiFramebuffer pixels;
/* Check return values and unwind failures in the eventual process. */
if (hr54_egl_open(&native, &api, 720, 480, chosen_depth)) return -1;
if (ui_fb_init(&pixels, 720, 480)) {
    hr54_egl_close(&native);
    return -1;
}
UiSurface output = { &native, hr54_egl_submit };
ui_fb_clear(&pixels, (UiColor){0, 0, 0, 0});
ui_fb_text(&pixels, 60, 50, "Media", 3, (UiColor){255,255,255,255});
int result = ui_present(&output, &pixels);
/* Later input/media events mutate state and repaint damaged regions. */
hr54_egl_close(&native);
ui_fb_free(&pixels);
return result;
```

`ui_present` returns zero after an accepted submission; this is not an HDMI
completion acknowledgement. First upload covers the complete texture. Later
uploads pack the damaged rectangle's rows, while each new retained drawing
frame still contains the **complete quad**. Publishing only changed-row drawing
commands would discard unchanged menu content when the retained frame changes.
The adapter queries actual output dimensions for its viewport.

Submission uses upload/draw/swap without a per-frame `glFinish`; texture upload
has its own vendor synchronization, and finish remains in teardown. CPU pixels
stay stable during upload. The native shell sets `replace_frame=1`: before its
complete quad, vendor `glClear(0x4000)` emits `dlSurfaceClear` for the own surface.
Without this, earlier blended drawing commands accumulate, and presentation
cost grows with frame count. The clear covers the output before any fitted
viewport is restored. Doom does not opt into this shell policy. No concurrent
presentation scheduler is implemented.

Hide by clearing the framebuffer to transparent and presenting it. Normal close
publishes a transparent frame, finishes own work, deletes the own texture,
unbinds, destroys the own window/context and terminates the client's display
use. Vendor destruction is deferred/reference-counted. No stock surface, server
thread, shared arena, decoder or plane mask is reset by this implementation.

## Verification and practical limits

Tests cover clipping, exact straight-alpha results, image stride, incremental
repaint equivalence, direct event state changes, playback callbacks, idle
submission suppression, packed dirty uploads, six-argument window creation,
swap failure retry, transparent hide and partial-open cleanup. The fake API
checks call ordering and arguments; it cannot validate vendor execution.

A receiver-linked process, actual output appearance, current stock depth
inventory, fractional alpha over video, latency and service-restart behavior
remain unvalidated. No hardware testing is requested or performed. Boot
integration is a concrete design in the findings, not an installed boot hook.

Evidence is reproducible entirely from extracted files:

```sh
python3 tools/re/native_presentation.py
```

That script emits disassembly, source hashes and linear call annotations under
`docs/native-ui-evidence/`. Branch-sensitive GOT calls must be checked against
the instructions; annotations are not a control-flow proof. Kernel module
addresses are section-relative offsets. See `tools/re/rel_mips.py` for relocation
annotations of the extracted ET_REL kernel module.

## Receiver smoke test

The default `make test` remains a host fake test. A separate real receiver build
is now available:

```sh
make receiver
tools/recv/hr54-push.sh tools/native-ui/build/hr54-native-smoke /tmp/hr54-native-smoke
HR54_DRAIN=16 tools/recv/hr54-shell.sh 'chmod 700 /tmp/hr54-native-smoke; ulimit -c 0; /tmp/hr54-native-smoke --depth 0 --seconds 12'
```

Run the last two commands from the repository root. `--depth` is mandatory;
depth 0 is BIST's stock reference, not a universal foreground priority. Exactly
one surface/texture is created per trial. Passing `(0,0)` as source geometry to
`hr54_egl_open` now selects the queried window size; explicit positive source
sizes still support scaling. SIGINT/SIGTERM/alarm request normal cleanup.

See [Live receiver validation](../../docs/NATIVE_UI_FINDINGS.md#live-receiver-validation)
for the exact link line, native runtime requirements, receiver logs and the
separate physical HDMI observation status. `receiver_runtime.h` deliberately
uses o32 uClibc declarations rather than importing musl time/FILE layouts.
