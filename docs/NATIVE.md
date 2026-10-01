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
moves focus to the menu bar.

## Settings

`~/Library/Application Support/ApplEm Native/layout.ini` holds ImGui's layout
and, under `[ApplEm][Settings]`, the app's own settings: the machine, the IIgs
memory size, volume, Sharp Pixels and the Cmd key choice per machine. Not
the Tauri build's folder; the two keep different things.

## Plan

1. Core as a library and the shared host layer. Done.
2. Scaffolding: window, docking, multi-viewport, menu bar. Done.
3. A machine running on screen, machine selection, keyboard, audio. Done.
4. Media and configuration: disk drives, SmartPort, expansion slots, save
   states, IIgs battery RAM, game port.
5. Display fidelity: the CRT shaders in Metal, display settings and profiles.
6. Debugger: CPU, memory, stack, zero page, soft switches, trace.
7. The remaining debug views, including the Disk Inspector.
8. The tools that are JavaScript today: printers, editors, file explorer.
9. Signing, notarisation and CI.
