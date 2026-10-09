# ApplEm

A cycle-accurate Apple II emulator for macOS: the Apple IIe Enhanced, the
Apple II Plus, the Apple IIc and the Apple IIgs, in a native app drawn with
Dear ImGui over Metal.

The emulation itself is [applem-core](https://github.com/mikedaley/applem-core),
included here as a submodule at `core/`. The same core runs in the browser as
[web-a2e](https://github.com/mikedaley/web-a2e).

## Building

You need Xcode's command line tools and CMake. The app runs on macOS 15 and
later.

```bash
git clone --recursive https://github.com/mikedaley/applem.git
cd applem
make            # build-macos/native/ApplEm.app
make run        # build it and open it
make test       # the app's own tests
make release    # signed with Developer ID, notarised and stapled
```

In a clone made without `--recursive`, run `git submodule update --init`
first: it fetches the core and Dear ImGui.

## What it does

- **Four machines**, each with its own slot layout, display settings and
  save states.
- **Windows of their own.** Any window docks into the main one or drags out
  into a Mac window, with a Mac title bar.
- **A CRT picture** from the signal: the core decodes the machine's dot
  stream, and a Metal shader draws the monitor.
- **Disk drives**, 5.25" and 3.5", and SmartPort hard drives. A disk opened
  from a file is written back to that file, and the Disk Inspector shows a
  track down to its nibbles and flux.
- **Expansion slots** drawn as cards standing in their connectors; click a
  slot to choose its card.
- **Sound**: the speaker, the Mockingboard and the IIgs's Ensoniq, mixed so
  that nothing clips. The Mockingboard can carry its original AY-3-8910s or
  Yamaha's YM2149Fs.
- **Debugging**: CPU debugger, memory viewer, soft switches, breakpoints of
  every kind, and a profiler that records where a program spends its time,
  with a timeline of frames to zoom and pan.
- **Develop with ca65.** A project is a small `.applem` file beside a
  Makefile. Build and Run (Command-B) builds it and starts what it made,
  with its symbols in the debugger. `examples/hello-asm`, `examples/hello-c`
  and `examples/profiler-demo` are ones to try.
- **Applesoft BASIC** editing and running.
- **Carries on where you left off**: quit, and the machine is there as you
  left it when ApplEm opens again.
- **Help built in**: Help > ApplEm Help opens the user guide.

The [ApplEm User Guide](docs/guide/README.md) explains everything the app
does, with tutorials for building and debugging your own programs. The app
carries it too, as its Help Book: Help > ApplEm Help.
`docs/NATIVE.md` covers how the app is built; the core's `docs/design/`
covers the machines.

## Layout

```
core/                 applem-core (submodule): the emulation and the host layer
native/
  src/                app, debugger, drives, display, develop, sound, ui...
  shaders/crt.metal   the CRT shader
  resources/          Info.plist, the icon, the bundled disk library
  tests/              the app's tests
  third_party/imgui/  Dear ImGui, docking branch (submodule)
examples/             programs to try: ca65 and cc65 projects, and BASIC
scripts/              the release script
docs/guide/           the user guide
docs/NATIVE.md        the app's design
VERSION               the version the release process bumps
```

## License

MIT License. See [LICENSE](LICENSE).
