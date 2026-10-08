# The ApplEm User Guide

ApplEm is an Apple II emulator for macOS. It runs four machines, the Apple II
Plus, the Apple IIe Enhanced, the Apple IIc and the Apple IIgs, closely
enough that software written for the real hardware runs unchanged, copy
protection and timing tricks included. Alongside the machines it has a set of
tools for people who write software for them: a debugger, a memory viewer, a
console that speaks the Monitor's language, a profiler, an Applesoft BASIC
editor and a way to build and run your own assembly or C programs with one
key.

This guide covers all of it. If you have never used an Apple II, start at the
beginning and work through the first few chapters. If you have, the chapters
stand on their own: skip to the one you need.

The guide is also inside ApplEm: choose **Help › ApplEm Help** (⌘?), or type
what you're looking for in the Help menu's search field.

![The ApplEm window, running a //e booted from the DOS 3.3 System Master](images/main-window.png)

## Contents

**Using the machines**

1. [Getting started](01-getting-started.md): installing, the first launch, the
   window, and a first program
2. [The machines](02-machines.md): the four models, power, reset, speed, PAL
   and NTSC, IIgs memory
3. [Keyboard, mouse and joysticks](03-keyboard-mouse-joystick.md): how a Mac
   keyboard becomes an Apple II one, pasting text, the mouse, game controllers
4. [The display](04-display.md): the screen, Full Page and Full Screen, and
   every display setting
5. [Disks and drives](05-disks-and-drives.md): 5.25" floppies, the Disk
   Inspector, 3.5" disks, SmartPort hard drives, and how changes are saved
6. [Expansion slots](06-expansion-slots.md): choosing the cards in each slot
7. [Save states](07-save-states.md): saving and restoring the whole machine,
   and carrying on where you left off
8. [Sound](08-sound.md): volume, the equaliser, the Mockingboard and the
   IIgs's Ensoniq

**Programming**

9. [Building and running your own programs](09-developing.md): ca65 and cc65
   projects, the Build window, step-by-step tutorials
10. [The CPU Debugger](10-cpu-debugger.md): stepping, breakpoints, conditions,
    symbols, watches, the beam, tracing
11. [The Memory Viewer and Soft Switches](11-memory-and-soft-switches.md):
    looking at and changing memory, the machine's switches
12. [The Console](12-console.md): a command line for the debugger, with the
    Monitor's own syntax
13. [The Profiler](13-profiler.md): finding where a program spends its time
14. [Applesoft BASIC](14-applesoft-basic.md): writing, running and debugging
    BASIC programs

**Reference**

15. [Menus, shortcuts and files](15-reference.md): every menu item and
    shortcut, where ApplEm keeps its files, and known quirks

## How to read this guide

- Keys are written as they appear on a Mac keyboard: ⌘ is Command, ⌥ is
  Option, ⌃ is Control and ⇧ is Shift. ⇧⌘D means hold Shift and Command and
  press D.
- Menu items are written as **Menu › Item**, for example **Debug › CPU
  Debugger**.
- Apple II addresses are in hexadecimal with a `$` in front, as Apple's own
  manuals write them: `$C000`. A number without a `$` is decimal, except in
  the [Console](12-console.md), where every number is hexadecimal.
- Text you type is shown `like this`.
- Notes for programmers who already know the Apple II well are marked
  **For the experienced**. You can skip them on a first read.

## The examples

The programs used in the tutorials are in the
[`examples`](../../examples) folder of ApplEm's repository on GitHub:
`hello-asm` and `hello-c` for the Build tutorials, `profiler-demo` for the
Profiler, and `gem.bas` for the BASIC editor. To get them all, open the
[repository's page](https://github.com/mikedaley/applem), click **Code** and
choose **Download ZIP**; the `examples` folder is inside. To get one file,
open it on GitHub and click the **Download raw file** button above it.
