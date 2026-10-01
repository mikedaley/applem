# Native macOS front end

`native/` is a second front end for the same emulator: a Cocoa app drawn
entirely with Dear ImGui (the docking branch) over Metal, with docking and
multi-viewport on, so any window can be dragged out of the main one into a
window of its own. It shares the C++ core and the host layer with the browser
build and shares no JavaScript.

## Building

```bash
git submodule update --init native/third_party/imgui
npm run native:build     # build-macos/native/ApplEm.app
npm run native:run       # build and open it
build-macos/native/test_native_input
```

`native:build` configures the top-level CMake with `-DA2E_BUILD_NATIVE=ON`.
The ROMs are embedded exactly as for the browser build.

## Layers

```
native/src/main.mm         Cocoa window, MTKView, ImGui's Cocoa + Metal backends
native/src/app.*           every menu and window: plain C++ over ImGui
native/src/emulation.*     the machine on its own thread, paced by Core Audio
native/src/audio_output.*  Core Audio's default output unit
native/src/key_mapper.*    ImGui keys as browser keycodes
src/host/machine_host.*    which machine is running, and how to reach its parts
src/core/                  the emulator
```

**`MachineHost` is shared with the browser.** Which of `Emulator` and
`IIgsMachine` is alive, and how a question about a drive, the speaker, the
debugger or the picture reaches it, used to be decided in
`wasm_interface.cpp`. It is decided in `src/host/` now, which the core purity
check covers, and both front ends sit on it. Anything a second front end
would otherwise decide again belongs there.

**App knows nothing about Cocoa or Metal.** What it needs from the platform
(the screen texture, Caps Lock, the window title) comes through `Platform` in
`platform.hpp`.

## Timing

The browser build's rules, unchanged. The audio device's render callback
reads a lock-free ring and wakes the emulation thread when fewer than two
frames (1600 samples) are waiting; the thread refills a frame (800 samples)
at a time, and the machine runs for exactly the time those samples
represent. A frame is published when `consumeFrameSamples()` says one is
complete, into a port of `frame-queue.js` (`frame_queue.hpp`). With no audio
device a free-running clock stands in, capped at 100ms a tick.

Everything else that touches the machine goes through
`Emulation::withMachine`, which takes the lock each refill holds. A refill
is about a millisecond.

## Keyboard

The core takes browser keycodes and does the Apple II's translation itself,
so the native keyboard is only `key_mapper`: ImGui key to browser keycode and
DOM location, and the browser's choice of which host key is an Apple key.
Two Mac details matter:

- **ImGui swaps Cmd and Ctrl on a Mac.** With `ConfigMacOSXBehaviors`,
  `ImGuiKey_LeftCtrl` is the physical Command key. `browserKeyFor` and
  `heldModifiers` undo it.
- **Modifiers come from ImGui's modifier flags**, not the modifier keys'
  state, because a key event carries its flags whether or not a modifier key
  event preceded it.

Option is Open and Closed Apple on the 8-bit machines; on a IIgs Cmd is Open
Apple (View > Cmd as Open Apple, remembered per machine). Ctrl+F12 is
Ctrl+Reset. ImGui's keyboard navigation is off, because with it Option alone
moves focus to the menu bar. While the screen has the keyboard the app tells
ImGui it wants it (`SetNextFrameWantCaptureKeyboard`): the Cocoa backend
passes every key ImGui says it did not use back to macOS, which finds no
text field and beeps.

## Display

The picture goes through the browser's CRT chain, ported to Metal:
`native/shaders/crt.metal` holds the CRT pass, the phosphor persistence pass
and the glass edge pass from `public/shaders/crt.glsl`, `burnin.glsl` and
`edge.glsl`. It is compiled when the app starts, from the bundle's copy, so
the build needs no Metal toolchain. `screen_renderer_metal.mm` renders it
into an offscreen texture at exactly the pixels the Screen window covers, at
the density of the display that window is on, and ImGui draws that one to
one.

**The port is checked against the original, pixel by pixel.**
`crt_render` boots a //e, draws lo-res, hi-res and the no-signal screen
through each preset's own decoder and through Metal, and writes the frames
and parameters; `scripts/compare-crt.mjs` draws the same frames through the
browser's own `WebGLRenderer` in headless Chrome and compares:

```bash
cmake --build build-macos --target crt_render
build-macos/native/crt_render native/shaders/crt.metal /tmp/crt
npm run dev -- --port 3011 --strictPort &
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new \
    --remote-debugging-port=9233 --user-data-dir=/tmp/crt-chrome \
    --use-gl=angle --use-angle=metal --disable-component-update &
node scripts/compare-crt.mjs /tmp/crt
```

Every preset matches to within a level or two. What remains is a texel
edge landing exactly on a pixel centre under nearest sampling, and the
bezel's grain hash, both a matter of float precision. The comparison is
what found that the shadow mask must count rows from the bottom, as
`gl_FragCoord` does: from the top, every staggered row and gap moved.
Only effects that do not move are compared; flicker, jitter, noise, sync
and the glowing line depend on when each side drew.

**The settings are the browser's** (`display_settings.*`, unit-tested in
`test_native_display`): the same presets with the same values, the same
0-100 scales, presets that never touch calibration or the bezel, Custom on
editing anything a preset claims, saved profiles that keep everything and
are marked modified rather than dropped, settings kept per machine with a
IIgs's screen border at zero, and the decoder sent to the core with
monochrome reached through the phosphor choice. The Display Settings window
(`display.cpp`) is the browser's window: Monitor, Image, then Advanced.

Powered off, the no-signal picture (`no_signal_frame.cpp`, a port of the
browser's) is uploaded as the source and goes through the same chain. View
> Full Page fills the main window with the picture (Ctrl+Esc, or the
right-click menu, to leave); View > Full Screen is macOS's.

## Disk drives

View > Disk Drives is the browser's window and its rules
(`disk_drives.*`, `media_store.*`, `drive_sounds.*`):

- **A drive remembers the image as it was inserted**, in
  `Media/floppy/` under Application Support, and puts it back at startup;
  what the machine writes afterwards is not kept. Each drive keeps ten
  recent images, newest first, one per name, with the bytes copied in. A
  blank disk (an unformatted WOZ) is neither remembered nor recent.
- **Ejecting asks to save only when the disk really changed.** The core
  saying it was written to is not enough, since software rewrites sectors
  with the same bytes; the image is fingerprinted at insert and again at
  eject. The save offers DOS order, ProDOS order and WOZ, defaulting to the
  disk's own format, with the impossible ones shown but disabled. Unlike the
  browser, Cancel (in the question or the save panel) keeps the disk in the
  drive rather than ejecting it unsaved.
- **A machine switch empties the drives and forgets them**, as the switch
  confirmation says; the browser kept them in storage, so they reappeared
  on the next reload.
- **The seek click** is the browser's synthesis, rendered once with its
  6kHz low pass and mixed into the output by the audio callback through a
  counter, without locking. It plays when an active drive crosses a whole
  track.
- The surface is the browser's canvas drawing in ImGui: the platter spins
  while the drive is active and coasts down with a 600ms half life, tracks
  warm with use and cool every 100ms, and the label colour comes from the
  same hash of the filename (pinned against the browser's own in
  `test_native_media`).
- Files dropped on the window go to the first empty drive. The browser's
  `public/disks` library is bundled and offered under Recent.

## Typeface

The UI is set in SF Mono at 13pt (`uiFontPath()` in `platform_paths.mm`),
read from the copy Terminal ships with, which has the static cuts on every
Mac, else from the system's variable SFNSMono. Neither is copied into the
app. ImGui 1.92's dynamic atlas rasterises it at each viewport's density, so
it is sharp on Retina and on a second monitor that is not, and glyphs beyond
ASCII (an ellipsis, curly quotes) load as they are needed. With neither file
present ImGui keeps its own font.

## Settings

`~/Library/Application Support/ApplEm Native/layout.ini` holds ImGui's layout
and, under `[ApplEm][Settings]`, the app's own settings: the machine, the IIgs
memory size, volume, the open windows and the Cmd key choice per machine;
each machine's display settings are under `[ApplEmDisplay][<machine key>]`.
Saved display profiles are in `display-profiles.ini` beside it, so Reset to
Defaults never takes them. Not the Tauri build's folder; the two keep
different things.

## Plan

1. Core as a library and the shared host layer. Done.
2. Scaffolding: window, docking, multi-viewport, menu bar. Done.
3. A machine running on screen, machine selection, keyboard, audio. Done.
4. Media and configuration: disk drives (done), SmartPort, expansion slots,
   save states, IIgs battery RAM, game port.
5. Display fidelity: the CRT shaders in Metal, display settings and profiles. Done.
6. Debugger: CPU, memory, stack, zero page, soft switches, trace.
7. The remaining debug views, including the Disk Inspector.
8. The tools that are JavaScript today: printers, editors, file explorer.
9. Signing, notarisation and CI.
