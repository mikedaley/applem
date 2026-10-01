/*
 * app.hpp - The native front end's user interface
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "display.hpp"
#include "emulation.hpp"
#include "key_mapper.hpp"
#include "platform.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace a2e::native {

// What the app remembers between runs. It is written into the same file as
// ImGui's layout, so the windows, where they are and what they show come back
// together.
struct Settings {
  std::string machine = "apple2e";
  int iigsMemoryKB = 1024;
  float volume = 0.5f;
  bool muted = false;
  bool showScreen = true;
  bool showDisplaySettings = false;
  bool ukCharacterSet = false;
  bool showStatusBar = true;
  // Per machine, keyed by profile key. Absent means the machine's default:
  // on for a IIgs, whose keyboard is a Mac's, and off for the rest.
  std::map<std::string, bool> commandIsOpenApple;
};

// Everything drawn each frame: the menu bar, the dock space and the windows.
//
// This is plain C++ over Dear ImGui and knows nothing about Cocoa or Metal;
// main.mm owns those and calls frame() between NewFrame and Render. What the
// UI needs from the platform comes through `Platform`.
class App {
public:
  App(std::string settingsDirectory, Platform platform);
  ~App();

  App(const App &) = delete;
  App &operator=(const App &) = delete;

  // Build this frame's UI. Call between ImGui::NewFrame() and ImGui::Render().
  void frame();

  // Whether the user asked to quit from inside the UI.
  bool quitRequested() const { return quitRequested_; }

  // The path ImGui keeps its layout in. It must outlive the ImGui context,
  // which is why the App owns the string.
  const char *iniPath() const { return iniPath_.c_str(); }

  // The app lost the keyboard to another one: let go of every key the
  // machine thinks is held, because their key-ups will never arrive.
  void releaseKeys();

  // Stop the machine before the platform goes away.
  void shutdown();

private:
  void registerSettingsHandler();
  void registerDisplayHandler();
  void startEmulation();

  void drawMenuBar();
  void drawMachineMenu();
  void drawDockSpace();
  void drawScreenWindow();
  void drawFullPage();
  // The picture, fitted to the space left in the current window at the
  // machine's aspect, through the CRT chain at the display's own density.
  void drawScreen();
  void updateScreenSource();
  void drawStatusBar();
  void drawSwitchConfirmation();

  void routeKeyboard();
  void handleAppShortcuts();
  void paste();
  void switchMachine(MachineId id);
  bool commandIsOpenApple() const;
  void updateWindowTitle();
  void applyMachineDisplay();

  std::string settingsDirectory_;
  std::string iniPath_;
  Platform platform_;
  Settings settings_;
  Display display_;
  Emulation emulation_;
  bool started_ = false;

  // The machine profile currently built, for drawing; refreshed on a switch.
  const MachineProfile *profile_ = nullptr;

  bool showDemo_ = false;
  // Full Page: the picture fills the main window and everything else goes,
  // as the browser's full page mode does. Ctrl+Escape leaves it.
  bool fullPage_ = false;
  bool enterFullPage_ = false;
  // Which window shows the picture this frame, so the keyboard follows it.
  const char *screenWindowName_ = nullptr;
  // The powered-off picture needs drawing again: switched off, or another
  // machine chosen while off.
  bool noSignalStale_ = true;
  bool wasPowered_ = false;
  bool quitRequested_ = false;
  bool layoutChecked_ = false;

  // Whether the screen had the keyboard last frame, and the keys it sent down
  // that have not come up, so losing the keyboard can release them.
  bool screenHadKeyboard_ = false;
  std::set<int> keysDown_; // ImGuiKey values

  // A machine switch waiting for the user to confirm it.
  std::optional<MachineId> pendingMachine_;
};

} // namespace a2e::native
