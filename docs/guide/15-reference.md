# 15. Menus, shortcuts and files

## Every menu

### ApplEm

| Item | Keys | |
| --- | --- | --- |
| About ApplEm | | The version, and when it was built |
| Settings… | ⌘, | Opens Display Settings |
| Hide ApplEm / Hide Others / Show All | ⌘H / ⌥⌘H / | |
| Quit ApplEm | ⌘Q | Writes disks back and saves the machine for next time. Works even while ⌘ is Open Apple |

### File

| Item | Keys | |
| --- | --- | --- |
| Insert in Drive 1… | ⌘O | Puts a 5.25" disk in drive 1 |
| Insert in Drive 2… | ⇧⌘O | Puts a 5.25" disk in drive 2 |
| Open Recent | | The recent disks of both 5.25" drives, and **Clear Menu** |
| Eject Drive 1 / Eject Drive 2 | ⌘E / ⇧⌘E | |
| Insert 3.5" Disk ›, Eject 3.5" Disk › | | IIgs only |
| Insert Hard Disk Image ›, Eject Hard Disk Image › | | When the machine has a SmartPort |
| Save Screenshot… | ⌥⌘S | Saves the picture as a PNG |
| Close Window | ⌘W | Closes the window in front (not the Screen) |

### Edit

| Item | Keys | |
| --- | --- | --- |
| Copy Screen Text | ⇧⌘C | Copies the text on the screen |
| Paste to Machine | ⌘V | Types the clipboard into the machine |

### Machine

| Item | Keys | |
| --- | --- | --- |
| Power | | |
| Control-Reset | ⌃F12 | Warm reset |
| Reboot | ⌃⌘R | Cold restart |
| Apple II Plus, Apple IIe Enhanced, Apple IIc, Apple IIgs | | Switches machine (asks first) |
| CPU Speed › 1x, 2x, 4x, 8x | | Not on the IIgs |
| Video Standard › NTSC, PAL | | II Plus, //e, //c |
| IIgs Memory › 256K to 8M | | IIgs |
| Reset Battery RAM… | | IIgs |
| Keyboard › Command as Open Apple | | Per machine |
| Keyboard › Cursor Keys as Joystick | | |
| Keyboard › UK Character Set | | //e |
| Capture Mouse / Release Mouse (⌃⌥) | | Machines with a mouse |
| Sound › Mute, Volume › | | |
| Sound › Mockingboard Phase Lock, Mockingboard Mono | | |
| Sound › Equalizer | | |

### View

| Item | Keys | |
| --- | --- | --- |
| Status Bar | ⌘/ | |
| Appearance › System, Light, Dark | | ApplEm's own light or dark look |
| Full Page / Leave Full Page | ⌃Esc | |
| Enter Full Screen / Exit Full Screen | ⌃⌘F | |

### Debug

| Item | Keys | |
| --- | --- | --- |
| CPU Debugger | ⇧⌘D | |
| Memory Viewer | ⇧⌘M | |
| Applesoft BASIC | ⇧⌘B | II Plus, //e, //c |
| Console | ⇧⌘K | |
| Soft Switches | | |
| Profiler | ⇧⌘P | |
| Start Profiling / Stop Profiling | ⌥⇧⌘R | |
| Mockingboard | | When a Mockingboard is fitted |
| Ensoniq | | IIgs |
| Continue / Pause | F5, or ⌃⌘Y | |
| Step Over | F10, or F6 | |
| Step Into | F11, or F7 | |
| Step Out | ⇧F11, or F8 | |
| Back / Forward | ⌘[ / ⌘] | In the CPU Debugger's listing |

### Develop

| Item | Keys | |
| --- | --- | --- |
| Open Project… | ⌥⌘O | |
| Build and Run | ⌘B | |
| Run Again | ⌥⌘B | |
| Watch for Changes | | |
| Build Window | | |
| Close Project | | |

### Window

| Item | Keys | |
| --- | --- | --- |
| Minimize / Zoom | ⌘M / | |
| Screen | ⌘1 | |
| 5.25" Drives | ⌘2 | |
| 3.5" Drives | | IIgs |
| SmartPort Drives | ⌘3 | When the machine has a SmartPort |
| Joystick | ⌘4 | |
| Expansion Slots | | Not on the //c |
| Save States | ⇧⌘S | |
| Display Settings | | |
| Equalizer | | |
| Window Docking | | Lets ApplEm's windows dock together inside the main window |

### Help

| Item | Keys | |
| --- | --- | --- |
| ApplEm Help | ⌘? | Opens this guide in the Mac's help viewer |

The search field at the top of the Help menu finds menu items and pages of
this guide.

### Shortcuts inside windows

| Window | Keys | |
| --- | --- | --- |
| CPU Debugger | Home | Go to the PC and follow it |
| | F9 | Breakpoint on the selected line |
| | ↑ ↓ Page Up Page Down | Scroll |
| Applesoft BASIC | ⌘R | Run or continue |
| | ⌘. | Stop |
| | ⌘\\ | Breakpoint on the cursor's line |
| Console | ↑ ↓ | Earlier commands |
| | Tab | Complete |
| Profiler | esc | Zoom the flame graph out |
| Disk Inspector | ⌘-scroll | Text size |

## Windows

ApplEm's tool windows (everything but the main window) have the Mac's three
buttons, drawn by ApplEm. The red one closes the window, and the green one
fills the screen it is on. The yellow one rolls the window up into its title
bar; click it again to unroll it. Windows that size themselves to their
contents have no green button.

The first click on a window behind another only brings it to the front, so
you don't press a button by accident.

Windows stay separate unless you turn on **Window › Window Docking**. With
it on, you can drag one window into another to combine them. Each window
remembers whether it was open, and where.

## Where ApplEm keeps things

Everything ApplEm remembers is in one folder:

```
~/Library/Application Support/ApplEm Native/
```

To open it, choose **Go › Go to Folder…** in the Finder (⇧⌘G) and paste the
path.

| File or folder | What it holds |
| --- | --- |
| `layout.ini` | Your settings and window layout, the debugger's breakpoints and labels, the BASIC editor's program, each machine's slots and display settings |
| `display-profiles.ini` | Your saved display profiles |
| `Media/` | What was in each drive, and the recent disk lists. A disk from a file is remembered by its path; a disk with no file is kept here as a copy |
| `States/` | The save state slots, autosaves, and the machine saved at quit |
| `iigs-battery-ram.bin` | The IIgs's Control Panel settings |

To start ApplEm completely afresh, quit it and move this folder to the Bin.

**For the experienced:** set the environment variable
`APPLEM_SETTINGS_DIR` to another folder to run a second copy of ApplEm with
settings of its own, for example from Terminal:

```bash
APPLEM_SETTINGS_DIR=~/applem-test /Applications/ApplEm.app/Contents/MacOS/ApplEm
```

A project's ProDOS boot disk is kept in the project's own folder, in
`.applem/`.

## File types

| Extension | What it is | Opened by ApplEm |
| --- | --- | --- |
| `.dsk`, `.do` | 5.25" disk, DOS 3.3 sector order | Yes |
| `.po` | ProDOS-order disk: 5.25", or a hard drive image if bigger than 140K | Yes |
| `.woz` | A disk recorded bit for bit, or flux by flux (5.25" or 3.5") | Yes |
| `.hdv`, `.2mg` | Hard drive image (or an 800K disk on a IIgs) | Yes |
| `.a2state` | A save state, from this app or the browser version | Yes |
| `.applem` | A Build project | Yes |
| `.bas` | An Applesoft program as text, for the BASIC window | Through the BASIC window's Open… |

## Known quirks

Things that behave in a way you might not expect:

- **Condition arithmetic has no precedence.** `2+3*4` is 20, not 14. Use
  brackets, or put the multiplication first: `PEEK($07)*256+PEEK($06)`.
- **There are no bit operators in conditions**, such as `&` to mask a value.
  Compare instead: `PEEK($C000) >= $80` is true while a key is waiting.
- **The Console's `find "text"` matches capitals and small letters
  exactly.** The Memory Viewer's text search ignores the difference.
- **Clicking a build error goes to its line only in some editors**: Xcode,
  Visual Studio Code, Sublime Text, BBEdit, TextMate and Zed. Others open the
  file at the top.
