# 2. The machines

ApplEm emulates four Apple II models. Each one is a different machine, with
its own processor, memory, slots and quirks, and software written for one may
not run on another, just as on the real hardware.

| Machine | Year | Processor | What it is |
| --- | --- | --- | --- |
| **Apple II Plus** | 1979 | 6502 | The classic Apple II with Applesoft BASIC in ROM. 48K of RAM, plus a 16K language card in slot 0 to make 64K. Capital letters only, no Open Apple or Closed Apple keys, no 80 columns |
| **Apple IIe Enhanced** | 1983 | 65C02 | The most widely used model. 128K of RAM with 80 columns and double hi-res. Seven expansion slots. **ApplEm starts with this one** |
| **Apple IIc** | 1984 | 65C02 | A //e folded into a portable case. The same 128K, but no expansion slots: its disk drive, serial ports and mouse port are built in |
| **Apple IIgs** | 1986 | 65C816 | A 16-bit machine that also runs almost all //e software. Super Hi-Res graphics, the Ensoniq sound chip, 3.5" drives, a built-in SmartPort and mouse |

## Choosing a machine

Choose a machine from the **Machine** menu, or from the machine pop-up in the
middle of the toolbar.

Switching machine rebuilds the computer from scratch, so ApplEm asks first:

> **Switch to the Apple IIgs?**
> The machine is rebuilt. Disks and anything in memory are lost.

Click **Switch** to go ahead or **Cancel** to stay. Before it switches, ApplEm
writes any changed disks back to their files, so what was written to them is
kept. Then:

- **The 5.25" drives and SmartPort hard drives start empty** on the new
  machine. Insert the disks you want again.
- **A IIgs's 3.5" disks are the exception.** They belong to the IIgs and come
  back when you return to it.
- **Each machine has its own slot layout and display settings**, as you last
  left them on that machine.

ApplEm remembers which machine you were using and opens with it next time.

## Power, reset and reboot

| Action | How | What happens |
| --- | --- | --- |
| **Power** | **Machine › Power**, or the Power button | Switches the machine off or on. Switching on is a cold start, like flipping the real power switch. While the machine is off, the screen shows a "no signal" picture and the status bar's clock reads **Off** |
| **Control-Reset** | **Machine › Control-Reset** (⌃F12), or the Reset button | The Apple's Control-Reset: a warm reset. Most programs stop and you return to BASIC, but memory is kept |
| **Reboot** | **Machine › Reboot** (⌃⌘R), or the Reboot button | A cold restart. The machine starts again from its disk drives, as when it is switched on |

ApplEm switches the machine on when it starts.

## CPU speed

**Machine › CPU Speed** runs the 8-bit machines faster than the real ones:
**1x** (1.023 MHz, the real speed), **2x**, **4x** or **8x**. The status bar
shows the speed; above 1x it adds the multiple, for example **2x 2.046
MHz**. The setting is kept when you reset or
switch machine.

The menu isn't offered on a IIgs, which has its own speed control: its
Control Panel chooses between Normal (1 MHz) and Fast (2.8 MHz), as on the
real machine.

## NTSC and PAL

Apple sold the II Plus, //e and //c in Europe with PAL video, which runs at
50 frames a second instead of the American 60. Some European software was
timed for that.

**Machine › Video Standard** chooses **NTSC (60Hz, 262 lines)** or **PAL
(50Hz, 312 lines)** for the machine you are using. The change is immediate,
and a note on the screen suggests rebooting so a program starts afresh at the
new speed. Each machine remembers its own choice. The IIgs was only made
with NTSC timing, so the menu isn't offered for it.

## IIgs memory

**Machine › IIgs Memory** sets how much RAM the IIgs has: **256K (as
shipped)**, **512K**, **1M**, **2M**, **4M** or **8M**. ApplEm starts with
1M.

A memory change restarts the machine, so ApplEm asks first. Disks stay in
their drives; anything in memory is lost.

## IIgs battery RAM

A real IIgs keeps its Control Panel settings (the display, the startup slot,
the clock and so on) in memory backed by a battery. ApplEm keeps the same
settings in a file and puts them back each time the IIgs starts, so whatever
you set in the Control Panel stays set.

To put every Control Panel setting back to its default, choose **Machine ›
Reset Battery RAM…**. The IIgs restarts. Disks stay in their drives; anything
in memory is lost.

## The UK character set

A //e sold in Britain had a character ROM with a pound sign (£) in place of
the hash (#). **Machine › Keyboard › UK Character Set** switches the //e to
that set. Only the //e offers it.

**For the experienced:** On the II Plus, text on the screen shows green and
violet fringes in every mode, because a II Plus never switches off its colour
burst. A //e switches the burst off on text lines and so shows clean white
text. ApplEm decodes the video signal the way a television does, so it shows
the same difference. See [The display](04-display.md).
