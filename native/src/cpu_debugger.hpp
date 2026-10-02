/*
 * cpu_debugger.hpp - The CPU debugger window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "machine_poll.hpp"
#include "condition_rules.hpp"
#include "debug_breakpoints.hpp"
#include "debug_symbols.hpp"
#include "platform.hpp"

#include "../../src/host/machine_host.hpp"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

class Emulation;

// The browser's CPU debugger (cpu-debugger-window.js), for either processor.
//
// The registers, the flags, the clock, the beam and the stack down the left;
// the code, as the machine's own disassembler reads it, in the middle; and
// the breakpoints, watches, beam breakpoints and the trace in a panel below.
// Everything is shaped by the machine's processor: a 65816 has banks, a
// direct page, sixteen-bit registers and a second set of flag names, and a
// 6502 shows none of that.
//
// The machine is read once a frame, under one lock, into a snapshot that the
// window draws from, so an open debugger costs the emulation thread a few
// microseconds a frame and nothing while it draws.
//
// Stops are examined every frame whether the window is open or not, because
// the core knows nothing of conditions: a breakpoint whose condition is false
// has to be sent straight back to running by someone, and a closed window is
// no excuse to leave the machine paused on it.
class CpuDebugger {
public:
  CpuDebugger(Emulation &emulation, Platform &platform);

  // The machine changed, or was rebuilt with the same profile: its debugger
  // starts with no breakpoints, so they are handed over again.
  void setMachine(const MachineProfile &profile);

  // Every frame, before drawing anything.
  void update();
  void draw(bool *open);

  // The Debug menu's actions.
  void continueOrPause();
  void stepInto();
  void stepOver();
  void stepOut();
  bool paused() const { return snapshot_.paused; }

  // Following a jump or call in the listing, and coming back: the Debug
  // menu's Back and Forward.
  void back();
  void forward();
  bool canGoBack() const { return !backStack_.empty(); }
  bool canGoForward() const { return !forwardStack_.empty(); }

  // Where the beam crosshair goes on the screen, as the renderer's beamX and
  // beamY (0 to 1 across the frame, -1 for none): drawn while the machine is
  // paused and the window is open, as the browser draws it. A line in
  // blanking has no column, so only the horizontal line is drawn.
  void beamOnScreen(bool windowOpen, float &x, float &y) const;

  // Settings, under their own section of the ini file.
  void writeSettings(std::string &out) const;
  void readSetting(const char *line);

private:
  // What the window draws, read from the machine in one go.
  struct StackEntry {
    uint32_t address = 0;
    uint8_t value = 0;
    // The low byte of a return address a JSR (or JSL) left here: where the
    // RTS will go, which is one past what was pushed.
    std::optional<uint32_t> returnsTo;
    bool longReturn = false;
  };
  struct Snapshot {
    bool valid = false;
    bool paused = false;
    host::CpuState cpu;
    // The listing, with a few lines beyond each edge; lines[topIndex] is
    // the one at the top of the window.
    std::vector<host::Instruction> lines;
    size_t topIndex = 0;
    std::vector<host::CycleCost> costs;
    std::vector<uint32_t> heat;    // cycles spent at each line (a //e)
    std::vector<uint8_t> executed; // whether each line has run since heat went on
    bool profiling = false;
    bool cycleProfile = false;
    bool coverage = false;
    struct Selection {
      int count = 0;
      uint32_t min = 0, max = 0;
      bool perByte = false;
      uint64_t heat = 0;
    } selection;
    host::Instruction current;
    std::optional<host::EffectiveAddress> effective;
    std::optional<bool> taken;
    std::vector<StackEntry> stack;
    std::vector<int32_t> watchValues;
    size_t traceCount = 0;
    bool traceEnabled = false;
  };

  struct Watch {
    std::string expression;
    int32_t previous = 0; // its value at the last stop, to show a change
    bool changed = false;
  };

  struct BeamBreak {
    int mode = 0; // 0 VBL, 1 HBL, 2 line, 3 column, 4 line and column
    int scanline = -1;
    int hPos = -1;
    bool enabled = true;
    int32_t id = -1;
  };

  void take();
  void handleStop();
  void applyBreakpoints(bool fresh);
  void applyBeams();
  void stopped(const std::string &reason);
  void runTo(uint32_t address);
  void setPC(uint32_t address);
  void goTo(uint32_t address);

  void drawToolbar();
  void drawSidebar(float width);
  void drawRegisters();
  void drawFlags();
  void drawClock();
  void drawBeam();
  void drawStack(float height);
  void drawCode(ImVec2 size);
  void drawCodeHeader();
  void drawLineMenu();
  void drawPanel(float height);
  void drawBreakpoints();
  void drawWatches();
  void drawBeams();
  void drawTrace();
  void drawEditPopup();
  void openRuleBuilder(size_t index);
  void drawRuleBuilder();
  bool drawRuleGroup(ConditionNode &group, int depth);
  bool drawRuleRow(ConditionNode &rule);
  void drawScrollbar(ImVec2 origin, ImVec2 size);
  void navigate(uint32_t target);
  void drawBookmarks();
  void toggleBookmark(uint32_t address);
  bool isBookmarked(uint32_t address) const;

  std::string formatAddress(uint32_t address) const;
  std::string symbolised(const host::Instruction &in) const;
  uint32_t addressMask() const { return wide_ ? 0xFFFFFF : 0xFFFF; }

  Emulation &emulation_;
  MachinePoll poll_;
  Platform &platform_;
  const MachineProfile *profile_ = nullptr;
  bool wide_ = false; // a 65816

  Snapshot snapshot_;
  DebugSymbols symbols_;
  Breakpoints breakpoints_;
  std::vector<Watch> watches_;
  std::vector<BeamBreak> beams_;

  // The listing: following the PC, or held at an address the user chose.
  bool followPC_ = true;
  uint32_t viewTop_ = 0;
  std::optional<uint32_t> centreOn_;
  int scrollBy_ = 0;
  int visibleRows_ = 40;
  // The inside width of the sidebar card being drawn: in a group, ImGui's
  // available width still runs to the window's edge.
  float cardInner_ = 0;
  // The inside height left for the card being drawn, to the column's foot.
  float cardHeight_ = 0;
  // How far the listing is drawn above its top line, in pixels: a scroll
  // moves this, and whole lines of it move the top line.
  float scrollPixels_ = 0;
  std::optional<uint32_t> jumpTo_; // a top line to show as it is, unaligned
  bool draggingBar_ = false;
  float barGrab_ = 0;
  // Where the user went, to come back from.
  std::vector<uint32_t> backStack_;
  std::vector<uint32_t> forwardStack_;
  // Heat and coverage, and the hottest line and the total to scale them by.
  bool heatOn_ = false;
  bool clearHeat_ = false;
  int totalsAge_ = 0;
  uint32_t heatMax_ = 0;
  uint64_t heatTotal_ = 0;
  // A run of lines, from selected_ to here (shift-click), for its total.
  std::optional<uint32_t> selectionEnd_;
  std::optional<uint32_t> selected_;
  uint32_t menuAddress_ = 0;
  // Addresses the user marked to come back to (Command-click in the gutter),
  // kept sorted.
  std::vector<uint32_t> bookmarks_;

  // Why the machine last stopped, and what it looked like then: registers
  // that differ from the stop before are lit, and the clock counts from it.
  std::string reason_ = "Running";
  bool stopHandled_ = false;
  int hitIndex_ = -1;
  int beamHitIndex_ = -1;
  uint64_t stoppedAtCycle_ = 0;
  host::CpuState previousStop_;
  host::CpuState lastStop_;
  bool havePrevious_ = false;
  double stoppedAt_ = -10.0; // ImGui time, for the status flash

  // The panel below the code.
  int tab_ = 0;
  float panelHeight_ = 230.0f;
  bool panelFolded_ = false;

  // Forms.
  int newKind_ = 0;
  char newAddress_[64] = "";
  bool newAddressBad_ = false;
  char newWatch_[96] = "";
  int newBeamMode_ = 0;
  int newBeamLine_ = 192;
  int newBeamColumn_ = 0;
  char gotoText_[64] = "";
  bool gotoBad_ = false;

  // A register being typed into, and the label or comment popup.
  int editingRegister_ = -1;
  bool focusRegister_ = false; // the field takes the keyboard once, as it opens
  char registerText_[16] = "";
  enum class EditKind { None, Label, Comment };
  EditKind editKind_ = EditKind::None;
  uint32_t editAddress_ = 0;
  char editText_[128] = "";
  bool openEdit_ = false;
  std::vector<std::array<char, 512>> conditionText_;
  // The rule builder: the breakpoint whose condition is being built, and the
  // tree, read from that condition as it opens. `rulesReplace_` says the
  // condition was not one the builder could read, so Apply replaces it.
  int ruleTarget_ = -1;
  bool openRules_ = false;
  bool rulesReplace_ = false;
  ConditionNode ruleTree_;
  std::string importMessage_;
};

} // namespace a2e::native
