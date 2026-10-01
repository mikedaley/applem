/*
 * display.hpp - Display settings: kept per machine, applied, and edited
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "display_settings.hpp"

#include <map>
#include <string>
#include <vector>

struct ImGuiTextBuffer;

namespace a2e::host {
class MachineHost;
}

namespace a2e::native {

class ScreenRenderer;

// The display settings for every machine, the user's saved profiles, and the
// Display Settings window.
//
// Settings are kept per machine, as in the browser (display-storage.js): a
// //e's composite look for games has no business on a IIgs's RGB desktop.
// They are written into ImGui's ini under [ApplEmDisplay][<machine key>].
// Profiles are global, named snapshots any machine may pick, and live in
// their own file so Reset to Defaults never takes them.
class Display {
public:
  explicit Display(std::string profilesPath);

  // The running machine changed (or was first chosen): show its settings,
  // or its defaults if it has none.
  void setMachine(const MachineProfile &machine);

  // ImGui settings handler hooks, for sections [ApplEmDisplay][<machine key>].
  // openSection returns that machine's state, or nullptr for an unknown key.
  DisplayState *openSection(const char *name);
  void readLine(DisplayState *state, const char *line);
  void writeAll(ImGuiTextBuffer *out, const char *typeName) const;
  // After the ini has been read: settle what was loaded against the presets
  // and profiles as they are now.
  void finishLoading();

  // Push the picture to the renderer, and the decoder to the machine's video.
  // The machine half is needed again whenever the machine is rebuilt.
  void applyToRenderer(ScreenRenderer &renderer) const;
  void applyToMachine(host::MachineHost &host) const;

  const DisplaySettings &settings() const { return current().settings; }

  void drawWindow(bool *open);

  // Whether the machine's video must be told the settings again: the
  // decoder or the monochrome switch changed, or the machine was rebuilt
  // and has forgotten them. Answering clears it.
  bool takeMachineChange();
  void machineRebuilt() { machineDirty_ = true; }

private:
  DisplayState &current();
  const DisplayState &current() const;
  void changed(const std::string &key);
  void loadProfiles();
  void saveProfiles();
  void flashStatus(const std::string &message);

  bool sliderRow(const char *label, const char *key, const char *tooltip = nullptr);
  void drawPresetControls();
  void drawSaveAsPopup();
  void drawDeletePopup();

  std::string profilesPath_;
  std::vector<DisplayProfile> profiles_;
  std::map<std::string, DisplayState> machines_; // by machine key
  // Machines whose settings came from the ini, so a machine never touched
  // is not written out and keeps following the defaults.
  std::map<std::string, bool> saved_;
  std::string machineKey_ = "apple2e";
  const MachineProfile *machine_ = nullptr;

  // The core needs telling when the decoder or monochrome switch changes.
  bool machineDirty_ = false;

  // The Save As and Delete dialogs.
  bool openSaveAs_ = false;
  bool openDelete_ = false;
  char nameBuffer_[64] = {};
  std::string nameError_;
  std::string pendingReplace_;

  std::string status_;
  double statusUntil_ = 0;
};

} // namespace a2e::native
