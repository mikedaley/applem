/*
 * app.hpp - The native front end's user interface
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "disk_drives.hpp"
#include "display.hpp"
#include "hard_drives.hpp"
#include "joystick.hpp"
#include "save_states.hpp"
#include "emulation.hpp"
#include "expansion_slots.hpp"
#include "key_mapper.hpp"
#include "menu_model.hpp"
#include "mockingboard_window.hpp"
#include "platform.hpp"

#include <functional>
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
  bool showDiskDrives = true;
  bool diskInspector = false; // hidden until first shown
  bool driveSounds = true;
  bool showHardDrives = false;
  bool showExpansionSlots = false;
  bool showSaveStates = false;
  bool autosave = false;
  int speed = 1; // 1, 2, 4 or 8 times the machine's clock
  bool showJoystick = false;
  bool showMockingboard = false;
  int mockingboardMutes = 0; // a bit a channel, PSG 1's A B C then PSG 2's
  int gamePort = 0; // 0 the Apple joystick, 1 the Joyport
  bool cursorKeys = false;
  bool gamepads = true;
  float deadzone = DEFAULT_DEADZONE;
  int appearance = 0; // 0 the system's, 1 light, 2 dark
  bool windowDocking = false; // windows stay windows unless this is on
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

  // Files dropped on one of the app's windows, at a point in ImGui's
  // coordinates: a disk dropped on a drive's card goes into that drive, and
  // anywhere else into the first empty one.
  void filesDropped(const std::vector<std::string> &paths, std::optional<ImVec2> at = std::nullopt);
  // Where a drag of files is over one of the app's windows, or nothing once
  // it has left, so the card it would land on can light up.
  void dragHover(std::optional<ImVec2> at);

  // The menu bar as of the last frame, and an item chosen from it. The
  // action runs at the start of the next frame, where ImGui may be used.
  const MenuBar &menuBar() const { return menuBar_; }
  const ToolbarState &toolbarState() const { return toolbar_; }
  void menuChosen(const std::string &action);

  // Whether a key with Command held should skip the menu bar and go to the
  // window: the machine takes Command as Open Apple and has the keyboard,
  // or an ImGui text field is being typed into.
  bool commandKeysToWindow() const;

  // The picture keeps the machine's shape when the main window is resized.
  // Given the content size the user is dragging to and the one it has now,
  // the size that keeps the picture's area at that shape; false when the
  // picture is not filling the main window, which is then free to take any
  // size. The edge that moved most leads.
  bool mainContentSizeFor(float proposedWidth, float proposedHeight, float currentWidth,
                          float currentHeight, float &width, float &height) const;
  // The largest content that keeps that shape within `maxWidth` by
  // `maxHeight`, for the zoom button.
  bool mainContentSizeWithin(float maxWidth, float maxHeight, float &width, float &height) const;
  // The user is dragging the main window's frame, by a side edge (the
  // width leads) or by the top or bottom (the height does). Nothing refits
  // the window while they do, and floating windows keep their own.
  void beginLiveResize(bool widthLeads);
  void endLiveResize();

private:
  void registerSettingsHandler();
  void registerDisplayHandler();
  void registerSlotsHandler();
  void startEmulation();

  void buildMenus();
  void runMenuActions();
  MenuItem machineMenu();
  MenuItem viewMenu();
  // Absent when the machine has nothing to debug through it yet.
  std::optional<MenuItem> debugMenu();
  void drawDockSpace();
  void drawScreenWindow();
  void drawFullPage();
  // The picture, fitted to the space left in the current window at the
  // machine's aspect, through the CRT chain at the display's own density.
  void drawScreen();
  // Resizes the main window to the picture's shape when the shape, or what
  // shares the window with the picture, has changed.
  void fitMainWindow();
  void updateScreenSource();
  void drawStatusBar();
  void drawDiskDrives();
  void drawJoystick();
  void drawMockingboard();
  void drawSwitchConfirmation();

  void routeKeyboard();
  void handleAppShortcuts();
  void paste();
  bool switchMachine(MachineId id);
  bool commandIsOpenApple() const;
  void updateWindowTitle();
  void applyMachineDisplay();
  void applySpeed();
  // A IIgs's battery-backed settings, kept as a real battery keeps them.
  void restoreBatteryRam();
  void saveBatteryRamIfChanged(double now);

  std::string settingsDirectory_;
  std::string iniPath_;
  Platform platform_;
  Settings settings_;
  Display display_;
  Emulation emulation_;
  std::unique_ptr<DiskDrives> drives_;
  std::unique_ptr<HardDrives> hardDrives_;
  ExpansionSlots slots_{emulation_};
  std::unique_ptr<SaveStates> states_;
  Joystick joystick_{emulation_};
  MockingboardWindow mockingboard_{emulation_};
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
  // Where the picture was last drawn: the machine's shape, whether it filled
  // the main window (docked there, or Full Page), and what of that window it
  // did not cover, which a resize carries along unchanged.
  float screenAspect_ = 0;
  bool screenFillsMain_ = false;
  ImVec2 screenExtra_{0, 0};
  // Whether the Screen window was docked, and its size less its picture
  // (the title bar and border), for its own size constraint when floating.
  bool screenDocked_ = true;
  ImVec2 screenChrome_{0, 0};
  // What the main window was last fitted to, so it is fitted once a change.
  bool liveResize_ = false;
  bool widthLeads_ = true;
  float fittedAspect_ = 0;
  ImVec2 fittedExtra_{-1, -1};
  // The powered-off picture needs drawing again: switched off, or another
  // machine chosen while off.
  bool noSignalStale_ = true;
  bool wasPowered_ = false;
  double batteryCheckedAt_ = 0;
  bool quitRequested_ = false;
  bool layoutChecked_ = false;

  // Whether the screen had the keyboard last frame, and the keys it sent down
  // that have not come up, so losing the keyboard can release them.
  bool screenHadKeyboard_ = false;
  std::set<int> keysDown_; // ImGuiKey values

  MenuBar menuBar_;
  ToolbarState toolbar_;
  std::map<std::string, std::function<void()>> menuActions_;
  std::vector<std::string> pendingActions_;
  bool textInputActive_ = false;

  // A machine switch waiting for the user to confirm it.
  std::optional<MachineId> pendingMachine_;
};

} // namespace a2e::native
