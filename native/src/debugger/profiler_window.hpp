/*
 * profiler_window.hpp - Where a program spends its time, drawn
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "app/machine_poll.hpp"
#include "debugger/profile_model.hpp"

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

class CpuDebugger;
class Emulation;

// Debug > Profiler: the core's Profiler (profiler.hpp), recorded on demand
// and read a few times a second while it records.
//
// Across the top, the recording's controls and a timeline of every frame,
// each a column split between the routines that took the most time in it,
// with the selected routine's share, calls included, traced over it. A drag
// across the timeline looks at those frames alone. Below that, four views of
// the same recording: the routines as a table, the call tree, a flame graph
// and the hottest lines. Down the right, the selected routine: its numbers,
// who calls it and what it calls, and its code with the time each line took.
//
// Routines are named by the CPU debugger's symbols, so a program built with
// ca65 (or any assembler whose symbols the debugger imports) is profiled by
// its own names.
class ProfilerWindow {
public:
  ProfilerWindow(Emulation &emulation, const CpuDebugger &debugger);

  // The machine changed, or was rebuilt: its profiler is a new one, not
  // recording.
  void setMachine() {
    recording_ = false;
    dirty_ = true;
  }

  void draw(bool *open);

  // Start a new recording, or stop the one running.
  void toggleRecording();
  bool recording() const { return recording_; }

  // Open the CPU debugger's listing at an address; the App shows the window.
  std::function<void(uint32_t)> showAddress;

  // Which view is showing, kept with the App's settings.
  int view = 0;

private:
  enum View { Functions, CallTree, Flame, Hot };

  struct HotLine {
    uint32_t address = 0;
    double time = 0;
    uint32_t executions = 0;
    std::string where;
    std::string instruction;
  };
  struct CodeLine {
    uint32_t address = 0;
    std::string text;
    double time = 0;
    uint32_t executions = 0;
  };

  // Read the machine: the tree, new frames, the hottest lines and the
  // selected routine's code. Often while recording, once after.
  void refresh(bool force);

  void drawHeader();
  void drawEmpty();
  void drawTimeline();
  void drawLegend();
  void drawFunctions();
  void drawCallTree();
  void drawTreeNode(uint32_t node, int depth);
  void drawFlame();
  void drawHot();
  void drawDetail();
  void drawEdges(const char *title, const std::vector<ProfileModel::Edge> &edges, double whole);
  void drawCode();

  void select(uint32_t function);
  std::string name(uint32_t function) const;
  std::string formatAddress(uint32_t address) const;
  std::string formatTime(double time) const;
  ImU32 colourFor(uint32_t function) const;
  ImU32 bandColour(int band) const;
  bool matchesFilter(const std::string &text) const;

  Emulation &emulation_;
  const CpuDebugger &debugger_;
  MachinePoll poll_;
  double refreshedAt_ = -1;
  bool dirty_ = true;

  ProfileModel model_;
  uint64_t generation_ = 0;
  bool wide_ = false;       // a 65816's 24-bit addresses
  double clockHz_ = 1023000.0;
  bool recording_ = false;
  double totalTime_ = 0;
  uint64_t instructions_ = 0;
  size_t depth_ = 0;
  bool truncated_ = false;
  std::vector<HotLine> hot_;
  std::vector<CodeLine> code_;
  uint32_t codeFor_ = Profiler::TOP_LEVEL;

  uint32_t selected_ = Profiler::TOP_LEVEL;
  bool hasSelection_ = false;
  char filter_[64] = {};

  // The timeline: a drag in progress, in frame indices.
  bool dragging_ = false;
  uint64_t dragFrom_ = 0;
  // The call tree opens down the hottest path the first time it has data.
  bool openHotPath_ = true;
  // The flame graph's root, and the span of it on screen, eased toward.
  uint32_t flameRoot_ = 0;
  double flameFrom_ = 0, flameTo_ = 1;
  double flameAt_ = -1;
};

} // namespace a2e::native
