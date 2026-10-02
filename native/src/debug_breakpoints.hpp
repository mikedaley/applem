/*
 * debug_breakpoints.hpp - The debugger's breakpoints, as the user keeps them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace a2e {
class MachineDebug;
}

namespace a2e::native {

class DebugSymbols;

// The browser's BreakpointManager (breakpoint-manager.js): the list the user
// sees, with what the core does not hold, a condition and a hit count, and
// the work of keeping the core's MachineDebug in step with it.
//
// The core stops on an address, a range entered, an access or the stack
// pointer; it knows nothing of conditions. So a stop is checked here
// afterwards, as the browser checks it: a breakpoint whose condition is false
// sends the machine straight back to running.
struct Breakpoint {
  enum class Kind { Exec, Read, Write, ReadWrite, Stack };

  Kind kind = Kind::Exec;
  uint32_t start = 0;
  uint32_t end = 0; // == start for a single address
  bool enabled = true;
  std::string condition; // empty: always
  uint32_t hits = 0;     // stops it has caused, this session

  bool isRange() const { return end != start; }
  bool contains(uint32_t value) const { return value >= start && value <= end; }
};

class Breakpoints {
public:
  // "$2000", "$2000-$20FF" or a symbol, with either end a symbol.
  static std::optional<std::pair<uint32_t, uint32_t>> parseRange(const std::string &text, const DebugSymbols &symbols,
                                                                  uint32_t addressMask);

  const std::vector<Breakpoint> &all() const { return list_; }
  std::vector<Breakpoint> &all() { return list_; }

  // False if the same kind already starts there.
  bool add(const Breakpoint &breakpoint);
  void remove(size_t index);
  // The plain execution breakpoint at an address, as a gutter click makes.
  void toggleExec(uint32_t address);
  bool hasExecAt(uint32_t address) const;
  void clear() { list_.clear(); }

  // The entry behind a stop, or -1.
  int execFor(uint32_t pc) const;
  int accessFor(uint32_t address, bool write) const;
  int stackFor(uint32_t low) const;

  // Make the core hold exactly the enabled entries. Call after any change,
  // and with `fresh` after the machine was rebuilt, when the core holds none.
  void apply(MachineDebug &debug, bool fresh = false);

  // Settings lines: "Breakpoint=<kind>\t<start>\t<end>\t<enabled>\t<condition>".
  void writeSettings(std::string &out) const;
  bool readSetting(const char *line);

private:
  std::vector<Breakpoint> list_;
  // What was handed to the core last time, so it can be taken back.
  std::vector<Breakpoint> applied_;
};

const char *kindName(Breakpoint::Kind kind);

} // namespace a2e::native
