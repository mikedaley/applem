/*
 * console_command.hpp - What a line typed into the debugging console asks for
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "debug_breakpoints.hpp"

#include <string>
#include <utility>
#include <vector>

namespace a2e::native {

// A line typed into the console, read but not yet acted on.
//
// The console speaks two languages. The Apple II monitor's, because every
// Apple II programmer's fingers already know it: an address alone dumps a
// line of bytes, 300.3FF a range, 300: A9 00 writes, 300L lists and 300G
// runs, with a bank and a slash on a IIgs (E1/2000.20FF). Numbers there are
// hex without a dollar sign, as the monitor reads them. And a debugger's,
// with words for commands, in the manner of VICE and MAME: step, bp, r and
// the rest, whose arguments are anything DebugSymbols resolves ($2000, a
// name like COUT) or the condition language evaluates.
//
// Nothing here touches a machine, the symbols or the evaluator: addresses
// and expressions are kept as the user wrote them for whatever runs the
// command to resolve, so the reading of a line can be tested on its own.
struct ConsoleCommand {
  enum class Kind {
    Empty,   // nothing typed
    Error,   // could not be read; `error` says why
    Help,    // help [command]
    Clear,   // cls
    Dump,    // memory from `from`, to `to` or a line of it
    Write,   // `values` into memory from `from`
    Fill,    // `from` to `to` with the one value in `values`
    List,    // disassemble from `from` (the PC if empty), `count` lines
    Go,      // run, from `from` if given
    Step,    // `count` instructions (1 if 0)
    StepOver,
    StepOut,
    Pause,
    Until,   // run until the PC reaches `from`
    Registers, // show them, or set `assignments`
    Evaluate,  // print the value of `text`
    BreakAdd,  // `breakpoint`, with its addresses in `from`/`to`
    BreakList,
    BreakDelete,  // number `count`, or `all`
    BreakEnable,  // number `count`, or `all`
    BreakDisable, // number `count`, or `all`
    Symbol,   // what `text` is: a name's address, or an address's name
    Stack,
    Trace,    // the last `count` instructions
    Find,     // the bytes in `values` (?? matches any), or the string in `text`
    Reset,    // Control-Reset
    Reboot,   // the power switch
  };

  Kind kind = Kind::Empty;
  std::string error;

  std::string from, to;             // address expressions, unresolved
  std::vector<std::string> values;  // bytes, each an expression; "??" in a find
  std::string text;                 // an expression, a name, a help topic, a string
  int count = 0;                    // steps, lines, a breakpoint's number; 0 for the default
  bool all = false;                 // bd all, be all, bx all
  std::vector<std::pair<std::string, std::string>> assignments; // r a=$42 pc=COUT

  // For BreakAdd: its kind, and for a switch or the beam what it waits for.
  // The addresses are in `from` and `to`, the condition in `breakpoint.condition`.
  Breakpoint breakpoint;
};

ConsoleCommand parseConsoleCommand(const std::string &line);

// The commands, for help and for completing a word as it is typed.
struct ConsoleCommandHelp {
  const char *name;
  const char *aliases; // space separated, or ""
  const char *usage;
  const char *summary;
};
const std::vector<ConsoleCommandHelp> &consoleCommands();

} // namespace a2e::native
