/*
 * save_states.hpp - The Save States window: an autosave and five slots
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "platform.hpp"
#include "state_store.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "machine/machine_profile.hpp"

namespace a2e::native {

class Emulation;

// The browser build's Save States window (save-states-window.js,
// state-manager.js) and its rules:
//
// - Five slots shared by every machine, each naming the machine that filled
//   it, and an autosave per machine; the autosave is written every five
//   seconds while the machine runs, when it is turned on (it starts off).
// - A state restores only into the machine that wrote it, so loading one
//   from another machine asks first, switches to that machine, then loads:
//   a save is of a whole machine, and loading it asks for that machine back.
// - A machine that is off is switched on before a state goes in.
// - The disks in a state come back with it, and the drive windows are told.
class SaveStates {
public:
  struct Hooks {
    std::function<const MachineProfile *()> machine;
    // Rebuild as another machine, without asking: the state already asked.
    std::function<bool(MachineId)> switchTo;
    // A state went in: the drives hold what it held.
    std::function<void()> loaded;
  };

  SaveStates(Emulation &emulation, Platform &platform, std::string directory, Hooks hooks);
  ~SaveStates();

  void draw(bool *open);
  // Every frame: the autosave's timer.
  void update(double now);
  // Before quitting or replacing the machine.
  void autosaveNow();

  bool autosave = false;

private:
  struct Row {
    std::optional<StateRecord> record;
    ImTextureID thumbnail = ImTextureID_Invalid;
  };

  void refresh();
  void releaseThumbnails();
  void saveTo(const std::string &id);
  void loadBytes(const std::vector<uint8_t> &data, const std::string &what);
  void importNow(const std::vector<uint8_t> &data, const std::string &what);
  void exportRecord(const std::string &id, const std::string &suggestedName);
  void drawRow(const char *label, const std::string &id, bool isAutosave);
  void notice(const std::string &message);
  void error(const std::string &message);

  Emulation &emulation_;
  Platform &platform_;
  StateStore store_;
  Hooks hooks_;
  std::map<std::string, Row> rows_;
  bool stale_ = true;
  bool wasOpen_ = false;
  double lastAutosave_ = 0;

  // A load waiting for the user to agree to switch machines.
  std::vector<uint8_t> pendingLoad_;
  std::string pendingWhat_;
  std::string pendingMachineName_;
  int pendingMachine_ = -1;
  bool openSwitch_ = false;

  std::string notice_;
  double noticeUntil_ = 0;
  std::string error_;
  bool openError_ = false;
};

} // namespace a2e::native
