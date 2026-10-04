/*
 * soft_switch_window.hpp - The soft switches, live, with breakpoints on them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "machine_poll.hpp"

#include "../../src/core/debug/soft_switch_catalog.hpp"

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
// and one on NEWVIDEO waits, unarmed, on a machine without it.
class SoftSwitchWindow {
public:
  explicit SoftSwitchWindow(Emulation &emulation);

  // The machine changed, or was rebuilt: its switches may differ, and its
  // debugger holds no breakpoints, so they are handed over again.
  void setMachine();

  // Every frame, open or not, so a stop is counted whether or not anyone is
  // looking.
  void update();
  void draw(bool *open);

  // Settings, under their own section of the ini file.
  void writeSettings(std::string &out) const;
  void readSetting(const char *line);

private:
  enum class Condition { Changes, Equals };

  struct SwitchBreak {
    std::string key;
    Condition condition = Condition::Changes;
    uint8_t value = 0; // 1 for on, for a switch
    uint8_t mask = 0xFF;
    bool enabled = true;
    uint32_t hits = 0;
    int32_t coreId = -1;

    bool same(const SwitchBreak &other) const;
  };

  const SoftSwitchInfo *find(const std::string &key) const;
  std::string describe(const SwitchBreak &bp) const;
  void apply();
  void add(SwitchBreak bp);
  void toggle(const SwitchBreak &bp);
  void take();
  void drawBreakpoints();
  void drawSwitch(const SoftSwitchInfo &sw, float width);
  void drawMenu(const SoftSwitchInfo &sw);

  Emulation &emulation_;
  MachinePoll updatePoll_;
  MachinePoll takePoll_;

  std::vector<SoftSwitchInfo> catalog_;
  std::vector<SwitchBreak> breaks_;

  // What the window draws, read once a frame.
  uint64_t flags_ = 0;
  std::vector<uint8_t> registers_; // one per catalog entry; unused for a flag
  bool paused_ = false;
  bool hit_ = false;
  int32_t hitId_ = -1;
  std::string hitText_;
  // The stop already counted, so it is counted once.
  int32_t countedId_ = -1;

  // The register breakpoint being typed in the menu.
  std::array<char, 4> valueText_{};
  std::array<char, 4> maskText_{};
};

} // namespace a2e::native
