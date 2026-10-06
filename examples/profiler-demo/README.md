# Profiler demo

A small 65C02 program for the native app's Profiler (Debug > Profiler). It
scrolls colour bars across the lo-res screen, and its routines cost very
different amounts, so there is something to find.

## Running it

You need cc65 (`brew install cc65`).

1. In ApplEm, Develop > Open Project and choose `profiler-demo.applem`.
2. Build and Run (Command-B). The bars start scrolling.
3. Debug > Profiler, then Record. Give it a few seconds, then Stop.

Control-Reset stops the program.

## What you should see

Measured on a //e over ten seconds:

| Routine        | Self  | Total | Calls a pass | Cycles a call |
| -------------- | ----- | ----- | ------------ | ------------- |
| `plot`         | ~49%  | ~49%  | 1,920        | 61            |
| `draw_bars`    | ~32%  | ~84%  | 1            | ~199,000      |
| `fib`          | ~6%   | ~6%   | 465          | 31            |
| `wait_vbl`     | ~5%   | ~5%   | 1            | ~13,000       |
| `clear_screen` | ~5%   | ~5%   | 1            | ~11,000       |
| `multiply`     | ~2.5% | ~2.5% | 40           | 150           |

Things to look at:

- **Routines**: `plot` is cheap per call and the heaviest overall, because of
  how often it runs. `draw_bars` is the reverse: called once a pass, and its
  total includes everything it calls.
- **Call Tree** and **Flame Graph**: `main` calls `draw_bars`, which calls
  `plot` and `multiply`. Click `draw_bars` in the flame graph to zoom in.
- **`fib`** calls itself 465 times a pass, but its total is not counted
  more than once, so it stays at about 6%.
- **Hot Lines**: the top lines are inside `plot`. The `bit $C019` loop in
  `wait_vbl` also ranks high, even though it does no useful work.
- **The detail pane**: select `plot` to see how the time divides between
  its even-line and odd-line paths.

## Try it out

`clear_screen` is wasted work, because `draw_bars` redraws every pixel
anyway. Delete the `jsr clear_screen` in `main`, build again, and record:
its share disappears and the bars move faster. Then try making `plot`
cheaper (for example, look the colour up in a table of shifted nibbles
instead of shifting four times) and see how much its total drops.
