/*
 * soft_switch_window.hpp - The soft switches, live, with breakpoints on them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "debugger/debug_breakpoints.hpp"
#include "app/machine_poll.hpp"

#include "../../../src/core/debug/soft_switch_catalog.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace a2e::native {

class Emulation;

// The browser's Soft Switch window (soft-switch-window.js): every switch and
// register the running machine has, from the core's catalog, as it stands,
// and a dot at the start of each row that sets a breakpoint on it. A switch
// can stop the machine when it changes or when it turns on or off; a
// register when it changes or when its bits under a mask hold a value. The
// core checks them after every instruction (MachineDebug), so a stop names
// the instruction that moved the switch.
//
// A breakpoint is kept by the switch's key, which is the same on every
// machine that has it, so one on PAGE2 follows the user from a //e to a IIgs
// and one on NEWVIDEO waits, unarmed, on a machine without it. They live in
// the App's shared breakpoint list, so the CPU debugger lists them, gives
// them conditions and counts their hits like any other.
class SoftSwitchWindow {
public:
  // The breakpoints are the App's shared list (debug_breakpoints.hpp): one
  // set here is in the CPU debugger's list too, and the reverse.
  SoftSwitchWindow(Emulation &emulation, Breakpoints &breakpoints);

  // The machine changed, or was rebuilt: its switches may differ.
  void setMachine();

  void draw(bool *open);

  // Settings, under their own section of the ini file. The breakpoints are
  // saved with the shared list; a line from before that is read into it.
  void writeSettings(std::string &out) const;
  void readSetting(const char *line);

private:
  const SoftSwitchInfo *find(const std::string &key) const { return findSoftSwitch(catalog_, key.c_str()); }
  static Breakpoint spec(const SoftSwitchInfo &sw, bool equals, uint8_t value, uint8_t mask);
  void take();
  void drawBreakpoints();
  void drawSwitch(const SoftSwitchInfo &sw, float width);
  void drawMenu(const SoftSwitchInfo &sw);

  Emulation &emulation_;
  Breakpoints &breakpoints_;
  MachinePoll takePoll_;

  std::vector<SoftSwitchInfo> catalog_;

  // What the window draws, read once a frame.
  uint64_t flags_ = 0;
  std::vector<uint8_t> registers_; // one per catalog entry; unused for a flag
  bool paused_ = false;
  bool hit_ = false;
  int32_t hitId_ = -1;
  std::string hitText_;

  // The register breakpoint being typed in the menu.
  std::array<char, 4> valueText_{};
  std::array<char, 4> maskText_{};
};

} // namespace a2e::native
