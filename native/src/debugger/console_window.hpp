/*
 * console_window.hpp - The debugging console
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "debugger/console_command.hpp"
#include "debugger/debug_breakpoints.hpp"

#include "../../../src/host/machine_host.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

class CpuDebugger;
class Emulation;

// A command line onto the machine: the Apple II monitor's syntax and a
// debugger's words (console_command.hpp), run against whichever machine is
// built. What it prints scrolls above the prompt, and an address in it is a
// link to that line in the CPU debugger.
//
// It shares everything it can rather than keeping its own: the breakpoint
// list is the App's, so `bp` adds to what the CPU debugger and the Soft
// Switches window show; names come from the debugger's symbols; stepping goes
// through the debugger, so its listing follows; and a stop the debugger
// judges (a breakpoint's condition, its hit count) is reported here in the
// debugger's words.
class ConsoleWindow {
public:
  ConsoleWindow(Emulation &emulation, CpuDebugger &debugger, Breakpoints &breakpoints);

  // The machine changed: its processor, its timing and its switches.
  void setMachine(const MachineProfile &profile);

  // Every frame, open or not, so a stop is not missed while it is closed.
  void update();
  void draw(bool *open);

  // Run a line as if typed, for a script or a test.
  void run(const std::string &line);

  // An address in the output was clicked.
  std::function<void(uint32_t)> showAddress;

  // The history, kept between runs.
  void writeSettings(std::string &out) const;
  void readSetting(const char *line);

  // What has been printed, oldest first, for a test to read.
  struct Line {
    enum class Style { Input, Output, Error, Note, Stop };
    std::string text;
    Style style = Style::Output;
    std::optional<uint32_t> address; // a link to the listing
  };
  const std::deque<Line> &output() const { return output_; }

private:
  void print(const std::string &text, Line::Style style = Line::Style::Output,
             std::optional<uint32_t> address = std::nullopt);
  void error(const std::string &text) { print(text, Line::Style::Error); }
  void execute(const ConsoleCommand &command);
  // The shared breakpoints into the core now, not at the next frame.
  void applyBreakpoints();

  // An address as the user wrote it (a name, $2000, E1/2000), or nothing,
  // having said why.
  std::optional<uint32_t> address(const std::string &text, uint32_t mask = 0);
  // A value: a number or a name, or else an expression the conditions
  // understand (PEEK($24)+1).
  std::optional<int64_t> value(const std::string &text);
  std::string formatAddress(uint32_t address) const;
  uint32_t addressMask() const { return wide_ ? 0xFFFFFF : 0xFFFF; }
  std::string describe(const Breakpoint &b) const;
  void printInstruction(const host::Instruction &in, bool atPC);

  void dump(uint32_t from, uint32_t to);
  void list(std::optional<uint32_t> from, int count);
  void registers();
  void breakpoint(const ConsoleCommand &command);
  void listBreakpoints();
  void printBreakpoint(size_t index);
  void find(const ConsoleCommand &command);
  void help(const std::string &topic);

  Emulation &emulation_;
  CpuDebugger &debugger_;
  Breakpoints &breakpoints_;
  const MachineProfile *profile_ = nullptr;
  bool wide_ = false;
  std::vector<SoftSwitchInfo> switches_;

  std::deque<Line> output_;
  bool scrollToEnd_ = false;
  std::array<char, 512> input_{};
  bool focusInput_ = true;
  bool followedLink_ = false; // the click in progress followed an address
  std::vector<std::string> history_;
  int historyAt_ = -1; // -1: the line being typed, not one from history
  std::string typed_;  // what was being typed before going back in history

  // The debugger's stop count when this last looked, to report each stop once.
  // A stop this console made by stepping it reports itself, and marks seen.
  uint32_t seenStops_ = 0;

  // A run from an address, as the monitor's G: a return address was pushed
  // for the routine's RTS, and the temporary breakpoint waits there. When
  // it is reached with the stack back where it was, the registers the
  // routine left are shown and the ones it found are put back, and a machine
  // that was running carries on.
  struct GoReturn {
    uint32_t address = 0; // where the RTS comes back to
    uint32_t from = 0;    // where the routine began
    host::CpuState before;
    bool wasRunning = false;
  };
  std::optional<GoReturn> goReturn_;
  bool go(uint32_t from);
  void returnedFromGo();

  static constexpr size_t MAX_OUTPUT = 4000;
  static constexpr size_t MAX_HISTORY = 200;
};

} // namespace a2e::native
