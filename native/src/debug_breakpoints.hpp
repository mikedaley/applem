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
struct SoftSwitchInfo;
}

namespace a2e::native {

class DebugSymbols;

// The browser's BreakpointManager (breakpoint-manager.js): the list the user
// sees, with what the core does not hold, a condition and a hit count, and
// the work of keeping the core's MachineDebug in step with it.
//
// **There is one list, and every window shares it.** The App owns it and
// hands it to the CPU debugger, the Soft Switches window and the console, so
// a breakpoint made in one is in the others, and the App applies it to the
// core once a frame when it has changed (needsApply). Every kind the core
// can stop on is here: an address or a range entered, an access, the stack
// pointer, a soft switch and the beam.
//
// The core knows nothing of conditions. So a stop is checked here
// afterwards, as the browser checks it: a breakpoint whose condition is false
// sends the machine straight back to running.
struct Breakpoint {
  enum class Kind { Exec, Read, Write, ReadWrite, Stack, Switch, Beam };
  // How a beam breakpoint was asked for, which is how it is described.
  enum BeamMode { BeamVbl, BeamHbl, BeamLine, BeamColumn, BeamLineColumn };

  Kind kind = Kind::Exec;
  // Given by the list when the breakpoint joins it and never reused, so a
  // number the console printed, a stop being shown or a condition being
  // built still names the same breakpoint after another one is deleted. 0
  // until it joins.
  uint32_t id = 0;
  uint32_t start = 0;
  uint32_t end = 0; // == start for a single address
  bool enabled = true;
  std::string condition; // empty: always
  uint32_t hits = 0;     // stops it has caused, this session

  // A soft switch (Kind::Switch): the catalog's key (soft_switch_catalog),
  // which is the same on every machine that has the switch, and either any
  // change or a value under a mask. A one-bit switch's value is 1 for on.
  std::string key;
  bool equals = false;
  uint8_t value = 0;
  uint8_t mask = 0xFF;

  // The beam (Kind::Beam): the line and the horizontal position the core
  // matches, -1 for any, and how the user asked for them.
  int scanline = -1;
  int hPos = -1;
  int beamMode = BeamLine;

  // The core's id for a switch or beam breakpoint while it holds it, or -1:
  // those are reported by id rather than by address.
  int32_t coreId = -1;

  bool isRange() const { return end != start; }
  bool contains(uint32_t value) const { return value >= start && value <= end; }
  // An address in memory: what a memory view marks.
  bool isAddress() const { return kind != Kind::Stack && kind != Kind::Switch && kind != Kind::Beam; }
  // The same breakpoint, whatever its state: what add() refuses twice.
  bool same(const Breakpoint &other) const;
};

class Breakpoints {
public:
  // "$2000", "$2000-$20FF" or a symbol, with either end a symbol.
  static std::optional<std::pair<uint32_t, uint32_t>> parseRange(const std::string &text, const DebugSymbols &symbols,
                                                                  uint32_t addressMask);

  const std::vector<Breakpoint> &all() const { return list_; }
  std::vector<Breakpoint> &all() { return list_; }

  // False if the same one is already there.
  bool add(const Breakpoint &breakpoint);
  void remove(size_t index);
  // By the id the list gave it. -1, nullptr and false when it has gone.
  int indexOf(uint32_t id) const;
  Breakpoint *find(uint32_t id);
  bool removeId(uint32_t id);
  // A switch breakpoint taken away if it is there, added if it is not.
  void toggle(const Breakpoint &breakpoint);
  // The plain execution breakpoint at an address, as a gutter click makes.
  void toggleExec(uint32_t address);
  bool hasExecAt(uint32_t address) const;
  void clear() { list_.clear(); }

  // The entry behind a stop, or -1.
  int execFor(uint32_t pc) const;
  int accessFor(uint32_t address, bool write) const;
  int stackFor(uint32_t low) const;
  // A switch or beam breakpoint, by the id the core reported.
  int switchFor(int32_t coreId) const;
  int beamFor(int32_t coreId) const;

  // Make the core hold exactly the enabled entries. A switch breakpoint is
  // found in the machine's catalog by its key, and one the machine does not
  // have is kept and not applied. Only what changed is touched: taking every
  // breakpoint out and putting it back forgot which one the machine was
  // stopped on, so the next Continue stopped on it again.
  void apply(MachineDebug &debug, const std::vector<SoftSwitchInfo> &switches);
  // Whether the list differs from what the core was last given, or the
  // machine was rebuilt (invalidate) and holds none of it.
  bool needsApply() const;
  void invalidate() { stale_ = true; }

  // "PAGE2 changes", "TEXT on", "NEWVIDEO & $80 = $80".
  static std::string describeSwitch(const Breakpoint &breakpoint, const std::vector<SoftSwitchInfo> &switches);

  // Settings lines: "Breakpoint=<kind>\t<start>\t<end>\t<enabled>\t<condition>",
  // then for a switch "\t<key>\t<change|equals>\t<value>\t<mask>" and for
  // the beam "\t<mode>\t<scanline>\t<hPos>". The lines the beam list and
  // the Soft Switches window used to keep for themselves are read too.
  void writeSettings(std::string &out) const;
  bool readSetting(const char *line);

private:
  std::vector<Breakpoint> list_;
  // What was handed to the core last time, so it can be taken back.
  std::vector<Breakpoint> applied_;
  bool stale_ = true;
  uint32_t nextId_ = 1;
};

const char *kindName(Breakpoint::Kind kind);

} // namespace a2e::native
