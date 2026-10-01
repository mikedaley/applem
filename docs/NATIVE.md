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

**A first run starts on Solid Colour**, where the browser starts on Pixel
Exact; Reset to Defaults comes back to it. A machine's settings are written
only once they have been changed, so from the first change on, that choice
is what the machine starts with.

**The settings are the browser's** (`display_settings.*`, unit-tested in
`test_native_display`): the same presets with the same values except
that every one is flat (the browser's RGB Monitor and monochrome presets
curve the screen; here curve and bezel are for the user to add), the same
0-100 scales, presets that never touch calibration or the bezel, Custom on
editing anything a preset claims, saved profiles that keep everything and
are marked modified rather than dropped, settings kept per machine with a
IIgs's screen border at zero, and the decoder sent to the core with
monochrome reached through the phosphor choice. The Display Settings window
(`display.cpp`) has the browser's settings in another shape: the presets,
the user's profiles and Custom (once it is in use) as tiles, each a small
drawn monitor showing its look, with the description and Save, Save As and
Delete under them; then the settings a page at a time (Picture, CRT,
Signal, Frame), in rounded groups of rows as System Settings draws them.
A group's panel is drawn after its rows, on a channel underneath, since its
height is known only then.

Powered off, the no-signal picture (`no_signal_frame.cpp`, a port of the
browser's) is uploaded as the source and goes through the same chain. View
> Full Page fills the main window with the picture (Ctrl+Esc, or the
right-click menu, to leave); View > Full Screen is macOS's.

## Disk drives

View > Disk Drives is the browser's Disk Drives and Disk Inspector windows
in one (`disk_drives.*`, `disk_inspector_data.*`, `disk_platter.*`,
`media_store.*`, `drive_sounds.*`). A card per drive carries the disk,
turning as the real one does, with the head on it (when the motor stops,
after the drive's own second of run-on, it coasts down rather than
stopping dead); the label in the
filename's sticker colour; what the inspector found (format, 13 or 16
sector, flux, bad sectors); the head's track; the controller's phases,
latch and state; and Insert, Recent, Blank and Eject. Clicking a card
inspects that drive, below: the platter coloured by what is recorded on
every quarter track (or, in Timing, how long a flux track's cells took),
hover for what is under the pointer and click to pick a track, and scroll
to zoom (to 400x, about the pointer) and drag to pan, when the disk holds
still and the head goes round it; the track unrolled as a strip that zooms
down to sixteen cells, where each nibble's value and kind are written and
every cell shows its 1 or 0, with its sectors named and the head marked; the sectors in the
order they pass the head; and the picked sector's bytes or every nibble on
the track. Follow head keeps the inspector on the head's track. The
Inspector switch shows the inspector. It is hidden until it is first shown,
and the choice is remembered (`DiskInspector` in the settings); clicking a
card picks the drive it inspects but does not open it.

**One description of a disk, the core's.** The whole disk comes through
`inspect::buildOverview`, the buffer the browser parses, read here by
`parseOverview`, so both builds draw the same thing; a single track is read
straight from `inspect::analyzeTrack`, since nothing here needs it as bytes.
A disk is read again only when `DiskController::getRevision` moves, and at
most twice a second while it is being written, through the *const* image
accessor: the writable one counts as a change, and the disk would be read
for ever.

**The platter is painted once and turned.** `paintPlatter` lays the
overview out pixel by pixel from polar coordinates, as the browser's
`_paintDisk` does, into a texture that is repainted only when the overview
or the mode changes; a frame draws it as one quad turned by the core's own
`DiskImage::getRotation()`, so what passes under the drawn head is what is
passing under the real one. Zoomed in, the view is painted afresh as it
moves (at half resolution while dragged), and once no more than 48 rings
show, each is read in full (`makeRing`, a few a frame under the lock) and
drawn from its own cells: flux transitions once a cell is wide enough to
see, and the nibbles' values along the ring once there is room. `test_native_media` pins the parsing, the
summary, a whole track, the ring geometry and the painting's transparency.

The drive's rules are the browser's:

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
- The label colour comes from the same hash of the filename as the
  browser's (pinned against the browser's own in `test_native_media`).
- Files can be dropped on any of the app's windows, including one dragged
  out of the main window: `main.mm` registers the view ImGui's backend puts
  in each window it makes. A disk dropped on a drive's card (or an image on
  a SmartPort device's) goes into that drive, which lights up while the
  drag is over it; dropped anywhere else, it goes to the first empty drive. The browser's
  `public/disks` library is bundled and offered under Recent.

## SmartPort drives and expansion slots

View > SmartPort Drives (`hard_drives.*`) is offered when the machine has a
SmartPort: a IIgs's, or a card, and says which slot it is in. Each of the
two devices is a card: a drive whose light shows a transfer (green reading,
red writing), the ProDOS volume's name, the image's name and size, Edited
and Locked when they apply, how full the volume is, a map of its blocks,
the last four seconds' transfers as a graph with their rates, and Insert,
Recent with the library's hard drive images, and Eject, which offers a
changed image for saving as it is. An empty device is a place to drop one.
On a IIgs an image inserted while the machine runs is taken at the next
reset, and the window says so in a banner.

**The map is the volume, and the machine's reads and writes light it.**
`volume_map.*` reads the volume directory's header in block 2 and the
free-block bitmap it points at, and divides the volume into 384 equal runs
of blocks, each shaded by how much of it is in use (tested in
`test_native_media`). The card's transfer callback reports every block the
machine reads or writes, on the emulation thread under the machine's lock;
the window collects them under the same lock each frame and warms that
block's cell green or red, cooling over about two seconds. A write marks the
volume's figures for reading again, at most once a second. The callback is
set once per card, since a refit or a machine switch builds a new one.

View > Expansion Slots (`expansion_slots.*`, `slot_layout.*`) has the
browser's rules drawn as the machine's logic board: each slot its number
in silkscreen and its connector along the bottom of the row, the card in it
drawn as Apple II cards are, a green board standing on the gold-fingered tab
along its bottom edge, a chamfered back corner, the chips and parts the real
card carries (the Mockingboard's AY-3-8910s, the Disk II's PROMs and drive
headers, the Super Serial Card's DIP switches), a paper label in the card's
colour with its name and its board number in silkscreen; beside it the
slot's use and its I/O and ROM addresses. A click on a slot offers its cards from its
`SLOT_UI` table, each card fitted once; a fixed slot's card is grey and
padlocked; an empty slot is a dashed outline; a slot changed and not yet
fitted is marked ON RESET until Apply & Reset. The No-Slot Clock is a
DS1215 on the board with its switch, and on a IIgs each slot has its
built-in-or-card switch, which takes effect at once.
Layouts are kept per machine under `[ApplEmSlots][<machine key>]`.

**The host applies the slot layout, and before the power comes on.** The
`Emulator` constructor fits only the drives, the Mockingboard and a //c's
ports from the profile's defaults; the SmartPort and the Thunderclock come
from the layout, which the browser's slot window applies at startup.
Without it a //e has no SmartPort (`test_machine_host` pins this). The
startup order is the cards, then the floppies, then the hard drive images
(fitting a layout can rebuild the SmartPort and take its images), then the
power, so the boot scan finds what the machine was left with.

## Save states

File > Save States (`save_states.*`, `state_store.*`) holds the browser's
records in cards: the autosave across the top, with its switch and a bar
filling toward the next save, then the five slots and Load from File three
to a row. A filled slot shows its thumbnail (140x96, box filtered from the
frame, so a IIgs's comes out whole) as a small screen, the machine that
wrote it (orange when loading it would switch machines) and when ("4
minutes ago", the date on hover); hovering puts Load, Save, Export and
Clear over the picture, and a right click gives them as a menu. An empty
slot saves when clicked. A save or a load flashes over its card. A card's
hover is taken from its own rectangle: the buttons over the picture would
otherwise take it from the card, hide, and give it back.
States are kept in `States/` as `<id>.a2state` with `.meta` and `.thumb`
beside them. The five slots are shared by every machine; the autosave is
one per machine, written every five seconds while the machine runs when
turned on (off by default, as in the browser), and before quitting or
switching machine.

A state starts with the browser's twelve-byte header (magic `A2ES`,
version, machine id). One from another machine asks first, switches to
that machine, then loads, because a state is a whole machine and restores
only into its own kind. A machine that is off is switched on first. After
a load the drive windows take what the drives hold from the core, and each
floppy's fingerprint is taken afresh, so eject asks about changes made
after the load.

## CPU speed

Machine > CPU Speed offers 1x, 2x, 4x and 8x of the machine's clock, as the
browser does, with the same mechanism (the core runs more cycles per audio
sample, so audio keeps pacing it). It is a preference rather than machine
state: kept, applied again after a switch, snapped to the nearest offered
value, and not offered on a IIgs, which has its own speed register. Above
1x the status bar says so.

The status bar's clock is measured across a sliding ten-second window. A
refill runs a whole video frame at once, about 17,000 cycles, so a
one-second window caught one refill more or fewer and wandered by about
1.3% (1.019 to 1.026 MHz); ten seconds brings that to about 0.1%, and it
reads 1.023. It starts again on a power cycle, a machine change, a speed
change, or a clock that went backwards (a rebuilt or reloaded machine).

## Game port

View > Joystick (`joystick.*`, `game_port.*`) chooses the device on the
connector, the Apple joystick or Sirius's Joyport, and drives it from the
on-screen stick (it springs back when let go), the cursor keys when View >
Cursor Keys as Joystick is on (the arrows still reach the keyboard too; a
CURSOR KEYS mark shows in the status bar), and gamepads through the
GameController framework, mapped into the browser's standard layout. The
browser's numbers: a deadzone that rescales the rest of the range, a
switch closing past half travel, the D-pad on 12 to 15, fire on A or B,
an impossible pair dropped, and one pad driving both Joyport sticks.

The window draws the device rather than describing it. An Apple joystick
is its beige case, a recessed gate as square as the paddles' 0 to 255, and
a ball-top stick that leaves a fading trail; beside it each paddle's value,
its address and how long the PREAD timer runs for it (eleven cycles a
count), and the three pushbuttons as round buttons that light, held with
the mouse. A Joyport is two CX40s, each stick leaning toward the switches
that are closed and pushed with the mouse eight ways, its red button fire.
What is driving each (mouse, cursor keys, gamepad) is shown, and every
connected gamepad is listed by the system's name with its stick and
buttons live and what it drives.

Sources are merged each frame and only a change is sent, because the
pushbutton lines are also the Apple keys: sending "not pressed" every frame
would let go of an Apple key the keyboard holds. A rebuilt machine starts
on an Apple joystick and is told the device again. A stored deadzone of 0
stays 0 (the browser read it back as 0.1).

## IIgs battery RAM

The 256 bytes are kept in `iigs-battery-ram.bin`, written when the core
says they changed (checked every two seconds, and before the machine is
quit or replaced) and put back exactly as written, checksum included. They
go back after the IIgs is built and before it is powered on, because the
firmware reads them as it starts. Verified by starting twice: the second
start leaves the file byte for byte as the first wrote it. The browser
restores them before it rebuilds a remembered IIgs, so its firmware writes
defaults; the native order avoids that.

## Look and feel

The app is meant to feel like a Mac app rather than an ImGui tool:

- **The menus are the macOS menu bar** (`menu_model.*`, `native_menu.mm`).
  The App describes File, Edit, Machine and View as a C++ model each frame;
  the bar is rebuilt only when that changes and never while a menu is open,
  around the system's application and Window menus. A chosen item runs at
  the start of the next frame, where ImGui can be used. Command keys go past
  the menus to the window when the machine takes Command as Open Apple and
  has the keyboard, or an ImGui text field is being typed into; Command-Q
  always quits.
- **A unified toolbar** (`native_toolbar.mm`): Power (green while on),
  Ctrl+Reset, Reboot, a pull-down naming and choosing the machine, and the
  windows, with SF Symbols, sending the menu items' own actions. The window
  is titled ApplEm with the machine as its subtitle.
- **System colours** (`ui_theme.mm`): AppKit's named colours resolved under
  the current appearance, the user's accent colour for checks, sliders,
  selection and tabs, reapplied when either changes. View > Appearance
  offers System, Light or Dark.
- **AppKit's controls, drawn** (`ui_controls.*`): capsule push buttons
  (Primary in the accent colour for the action a window is for), rounded
  checkboxes with a tick, switches with a sliding knob, sliders with a round
  knob over a thin track, segmented controls, pop-up buttons with small up
  and down chevrons, and a turning chevron for a disclosure. Each is an
  ordinary ImGui item, so keyboard focus and IDs work as ImGui's own do.
  Window code calls `ui::` rather than `ImGui::` for any of these; a stock
  ImGui control in a window stands out at once.
- **SF Pro** for the interface and **SF Mono** for figures.
- A dock area holding one window hides its tab, so the screen has none.
- The status bar is indicators: the drives' lights, which key is Open Apple,
  cursor keys, sound only when muted or missing, and the clock on the right.
- **The window keeps the picture's shape.** While the picture fills the main
  window (docked there, or Full Page), a resize lands on the size that keeps
  the picture at the machine's aspect, with the status bar and anything
  docked beside it carried on top. The edge grabbed decides, for the whole
  drag, whether the width or the height leads (deciding at each step flips
  between them in a corner and the window jumps), and the zoom button picks
  the largest such size on the screen. During the drag ImGui is given no
  mouse and merges no floating window into the main one: it sees every event
  the app gets, the press on the frame included, and took it for a drag of
  whichever floating window was nearby, which then followed the pointer. A change of shape
  (another machine, or a panel docked beside it) refits the window once the
  mouse is up. Floating, the Screen window keeps the shape itself through an
  ImGui size constraint. Full screen is the system's to size, so there the
  picture is letterboxed instead.

## Typeface

Figures are set in SF Mono (`uiFontPath()` in `platform_paths.mm`, loaded
by `ui_theme.mm` beside SF Pro for everything else), read from the copy
Terminal ships with, which has the static cuts on every
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
4. Media and configuration: disk drives, SmartPort and expansion slots
   IIgs battery RAM, save states, CPU speed and the game port. Done.
5. Display fidelity: the CRT shaders in Metal, display settings and profiles. Done.
6. Debugger: CPU, memory, stack, zero page, soft switches, trace.
7. The remaining debug views, including the Disk Inspector.
8. The tools that are JavaScript today: printers, editors, file explorer.
9. Signing, notarisation and CI.
