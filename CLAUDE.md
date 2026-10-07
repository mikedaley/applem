# CLAUDE.md

Guidance for Claude Code in this repository: ApplEm, a cycle-accurate Apple
II emulator (II Plus, //e, //c and IIgs) for macOS, drawn with Dear ImGui
over Metal. `docs/NATIVE.md` holds the detail and the plan; read the relevant
part before changing a subsystem.

**The emulation is the `core/` submodule**
([applem-core](https://github.com/mikedaley/applem-core)), shared with the
browser build ([web-a2e](https://github.com/mikedaley/web-a2e)). Read
`core/CLAUDE.md` before changing anything in it. A core change is made and
committed in the core repository, pushed there, and then this repository's
submodule pointer is moved to it in a commit of its own here. Never leave
this repository pointing at a core commit that is not pushed.

## Build commands

```bash
git submodule update --init   # the core and Dear ImGui
make                          # build-macos/native/ApplEm.app
make run                      # build and open it
make test                     # build everything and run the app's tests
make release                  # scripts/build-native-mac.sh: signed, notarised
```

Keep build parallelism at `-j 4`. The core's own tests are built and run in
the core (`cd core && mkdir -p build && cd build && cmake .. && make -j 4 &&
ctest`).

## Testing

`native/tests/` holds the app's tests (`test_native_*`, Catch2 from
`core/tests/catch2`): input mapping, the debugger and profiler model, the
console, media, display, the BASIC editor, the equaliser, ca65 projects and
the window chrome. `make test` runs them.

## Load-bearing rules

- **`core/src/host/machine_host.*` is shared with the browser build.** It
  decides which of `Emulator` and `IIgsMachine` is alive and routes what
  every host asks of a machine. Logic both front ends need goes there,
  never into this app alone.
- **Timing is the browser's**: Core Audio's callback wakes the emulation
  thread below two frames of samples, a refill is one frame, and frames go
  through a port of the browser's `frame-queue.js`. Everything else touches
  the machine through `Emulation::withMachine`.
- **ImGui swaps Cmd and Ctrl on a Mac** (`ImGuiKey_LeftCtrl` is physical ⌘);
  modifiers come from ImGui's modifier flags. `key_mapper` turns ImGui keys
  into the browser keycodes the core expects (`test_native_input`).
- **The slot layout is applied before the power comes on.** The `Emulator`
  constructor fits only the drives, the Mockingboard and a //c's ports; the
  SmartPort and Thunderclock come from the layout. Order: cards, floppies,
  hard drive images, then power.
- **A disk from a file writes back to that file** on idle, eject, replace,
  machine change and quit (`MachineHost::markDiskSaved` and twins), under one
  hold of the machine. A disk with no file asks instead.
- **`native/shaders/crt.metal` ports the browser's `crt.glsl`.** Change one,
  change the other in web-a2e, and compare them (`docs/NATIVE.md`, Display).
- **Animated shader effects must stay within photosensitive-epilepsy
  limits** (no more than three flashes a second or a 10% luminance change).
- **Windows**: every window uses `ui::BeginWindow` (a Mac title bar) and
  `ui::ClickToFocus` decides what a click on a window behind does.
- **Hardware behaviour is settled by the hardware's documentation**, not by
  another emulator (see `core/CLAUDE.md`).

## Release process

When the user says "release":

1. Review the git log since the last release
2. Bump `VERSION`
3. Update `README.md` for new features
4. Update this file and `docs/NATIVE.md` for architectural changes
5. `make release`
