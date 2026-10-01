/*
 * app.cpp - The native front end's user interface
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "app.hpp"
#include "ui_controls.hpp"

#include "no_signal_frame.hpp"
#include "ui_theme.hpp"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder, the status bar's viewport side bar

#include "video/video.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace a2e::native {

namespace {

constexpr const char *SCREEN_WINDOW = "Screen";
constexpr const char *FULL_PAGE_WINDOW = "##FullPage";
constexpr const char *DOCKSPACE_ID = "ApplEmDockSpace";
constexpr const char *SWITCH_POPUP = "Switch machine?";

// What the IIgs may be fitted with, as the browser build offers it
// (src/js/machine/iigs-memory.js).
struct MemorySize {
  int kb;
  const char *label;
};
constexpr MemorySize IIGS_MEMORY_SIZES[] = {
    {256, "256K (as shipped)"}, {512, "512K"}, {1024, "1M"},
    {2048, "2M"},               {4096, "4M"},  {8192, "8M"},
};

// Where a window first appears, until it has been moved: staggered across
// the main window rather than all on top of one another.
void firstPosition(float x, float y) {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + x, viewport->WorkPos.y + y), ImGuiCond_FirstUseEver);
}

bool isModifier(int keyCode) {
  return keyCode == KEY_SHIFT || keyCode == KEY_CONTROL || keyCode == KEY_ALT ||
         keyCode == KEY_META_LEFT || keyCode == KEY_META_RIGHT;
}

} // namespace

App::App(std::string settingsDirectory, Platform platform)
    : settingsDirectory_(std::move(settingsDirectory)),
      iniPath_(settingsDirectory_ + "/layout.ini"),
      platform_(std::move(platform)),
      display_(settingsDirectory_ + "/display-profiles.ini"),
      drives_(std::make_unique<DiskDrives>(emulation_, platform_, settingsDirectory_ + "/Media")),
      hardDrives_(std::make_unique<HardDrives>(emulation_, platform_, settingsDirectory_ + "/Media")) {
  registerSettingsHandler();
  registerDisplayHandler();
  registerSlotsHandler();
  joystick_.setGamepadSource(platform_.gamepads);
  // Refitting can rebuild the SmartPort, and its images with it.
  slots_.setAppliedCallback([this] { hardDrives_->syncWithMachine(); });

  SaveStates::Hooks hooks;
  hooks.machine = [this] { return profile_; };
  hooks.switchTo = [this](MachineId id) { return switchMachine(id); };
  hooks.loaded = [this] {
    drives_->syncWithMachine();
    hardDrives_->syncWithMachine();
  };
  states_ = std::make_unique<SaveStates>(emulation_, platform_, settingsDirectory_ + "/States", std::move(hooks));
}

App::~App() { shutdown(); }

void App::shutdown() {
  if (!started_) return;
  releaseKeys();
  states_->autosaveNow();
  saveBatteryRamIfChanged(-1);
  emulation_.stop();
  started_ = false;
}

// The settings live in ImGui's ini file, under [ApplEm][Settings], so they
// are read and written with the layout.
void App::registerSettingsHandler() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "ApplEm";
  handler.TypeHash = ImHashStr("ApplEm");
  handler.UserData = this;
  handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *,
                          const char *name) -> void * {
    return std::strcmp(name, "Settings") == 0 ? reinterpret_cast<void *>(1)
                                              : nullptr;
  };
  handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *h, void *,
                          const char *line) {
    App *app = static_cast<App *>(h->UserData);
    Settings &s = app->settings_;
    char text[64] = {};
    int value = 0;
    float number = 0.0f;
    if (std::sscanf(line, "Machine=%63s", text) == 1) s.machine = text;
    else if (std::sscanf(line, "IIgsMemoryKB=%d", &value) == 1) s.iigsMemoryKB = value;
    else if (std::sscanf(line, "Volume=%f", &number) == 1) s.volume = number;
    else if (std::sscanf(line, "Muted=%d", &value) == 1) s.muted = value;
    else if (std::sscanf(line, "ShowScreen=%d", &value) == 1) s.showScreen = value;
    else if (std::sscanf(line, "ShowDisplaySettings=%d", &value) == 1) s.showDisplaySettings = value;
    else if (std::sscanf(line, "UKCharacterSet=%d", &value) == 1) s.ukCharacterSet = value;
    else if (std::sscanf(line, "ShowDiskDrives=%d", &value) == 1) s.showDiskDrives = value;
    else if (std::sscanf(line, "DiskInspector=%d", &value) == 1) s.diskInspector = value;
    else if (std::sscanf(line, "DriveSounds=%d", &value) == 1) s.driveSounds = value;
    else if (std::sscanf(line, "ShowHardDrives=%d", &value) == 1) s.showHardDrives = value;
    else if (std::sscanf(line, "ShowExpansionSlots=%d", &value) == 1) s.showExpansionSlots = value;
    else if (std::sscanf(line, "ShowSaveStates=%d", &value) == 1) s.showSaveStates = value;
    else if (std::sscanf(line, "Autosave=%d", &value) == 1) s.autosave = value;
    else if (std::sscanf(line, "Speed=%d", &value) == 1) s.speed = value;
    else if (std::sscanf(line, "Appearance=%d", &value) == 1) s.appearance = std::clamp(value, 0, 2);
    else if (std::sscanf(line, "WindowDocking=%d", &value) == 1) s.windowDocking = value;
    else if (std::sscanf(line, "ShowJoystick=%d", &value) == 1) s.showJoystick = value;
    else if (std::sscanf(line, "GamePort=%d", &value) == 1) s.gamePort = value == 1 ? 1 : 0;
    else if (std::sscanf(line, "CursorKeys=%d", &value) == 1) s.cursorKeys = value;
    else if (std::sscanf(line, "Gamepads=%d", &value) == 1) s.gamepads = value;
    // A stored 0 is a deadzone of 0, which the browser read back as 0.1.
    else if (std::sscanf(line, "Deadzone=%f", &number) == 1) s.deadzone = std::clamp(number, 0.0f, MAX_DEADZONE);
    else if (std::sscanf(line, "NoSlotClock=%d", &value) == 1) app->slots_.noSlotClock = value;
    else if (std::sscanf(line, "ShowStatusBar=%d", &value) == 1) s.showStatusBar = value;
    else if (std::sscanf(line, "CommandIsOpenApple.%63[^=]=%d", text, &value) == 2) {
      s.commandIsOpenApple[text] = value;
    }
  };
  handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h,
                          ImGuiTextBuffer *out) {
    const App *app = static_cast<const App *>(h->UserData);
    const Settings &s = app->settings_;
    out->appendf("[%s][Settings]\n", h->TypeName);
    out->appendf("Machine=%s\n", s.machine.c_str());
    out->appendf("IIgsMemoryKB=%d\n", s.iigsMemoryKB);
    out->appendf("Volume=%.3f\n", s.volume);
    out->appendf("Muted=%d\n", s.muted ? 1 : 0);
    out->appendf("ShowScreen=%d\n", s.showScreen ? 1 : 0);
    out->appendf("ShowDisplaySettings=%d\n", s.showDisplaySettings ? 1 : 0);
    out->appendf("UKCharacterSet=%d\n", s.ukCharacterSet ? 1 : 0);
    out->appendf("ShowDiskDrives=%d\n", s.showDiskDrives ? 1 : 0);
    out->appendf("DiskInspector=%d\n", s.diskInspector ? 1 : 0);
    out->appendf("DriveSounds=%d\n", s.driveSounds ? 1 : 0);
    out->appendf("ShowHardDrives=%d\n", s.showHardDrives ? 1 : 0);
    out->appendf("ShowExpansionSlots=%d\n", s.showExpansionSlots ? 1 : 0);
    out->appendf("ShowSaveStates=%d\n", s.showSaveStates ? 1 : 0);
    out->appendf("Autosave=%d\n", app->states_ && app->states_->autosave ? 1 : 0);
    out->appendf("Speed=%d\n", s.speed);
    out->appendf("Appearance=%d\n", s.appearance);
    out->appendf("WindowDocking=%d\n", s.windowDocking ? 1 : 0);
    out->appendf("ShowJoystick=%d\n", s.showJoystick ? 1 : 0);
    out->appendf("GamePort=%d\n", s.gamePort);
    out->appendf("CursorKeys=%d\n", s.cursorKeys ? 1 : 0);
    out->appendf("Gamepads=%d\n", s.gamepads ? 1 : 0);
    out->appendf("Deadzone=%.2f\n", s.deadzone);
    out->appendf("NoSlotClock=%d\n", app->slots_.noSlotClock ? 1 : 0);
    out->appendf("ShowStatusBar=%d\n", s.showStatusBar ? 1 : 0);
    for (const auto &[key, on] : s.commandIsOpenApple) {
      out->appendf("CommandIsOpenApple.%s=%d\n", key.c_str(), on ? 1 : 0);
    }
    out->append("\n");
  };
  ImGui::AddSettingsHandler(&handler);
}

// Each machine's display settings, under [ApplEmDisplay][<machine key>].
void App::registerDisplayHandler() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "ApplEmDisplay";
  handler.TypeHash = ImHashStr("ApplEmDisplay");
  handler.UserData = &display_;
  handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *h, const char *name) -> void * {
    return static_cast<Display *>(h->UserData)->openSection(name);
  };
  handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *h, void *entry, const char *line) {
    static_cast<Display *>(h->UserData)->readLine(static_cast<DisplayState *>(entry), line);
  };
  handler.ApplyAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h) {
    static_cast<Display *>(h->UserData)->finishLoading();
  };
  handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out) {
    static_cast<const Display *>(h->UserData)->writeAll(out, h->TypeName);
  };
  ImGui::AddSettingsHandler(&handler);
}

// Each machine's slot layout, under [ApplEmSlots][<machine key>].
void App::registerSlotsHandler() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "ApplEmSlots";
  handler.TypeHash = ImHashStr("ApplEmSlots");
  handler.UserData = &slots_;
  handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *h, const char *name) -> void * {
    return static_cast<ExpansionSlots *>(h->UserData)->openSection(name);
  };
  handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *h, void *entry, const char *line) {
    static_cast<ExpansionSlots *>(h->UserData)->readLine(static_cast<SlotLayout *>(entry), line);
  };
  handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out) {
    static_cast<const ExpansionSlots *>(h->UserData)->writeAll(out, h->TypeName);
  };
  ImGui::AddSettingsHandler(&handler);
}

// Started on the first frame rather than in the constructor, because ImGui
// reads the ini, and so the settings, inside the first NewFrame.
void App::startEmulation() {
  // A remembered machine the build cannot run is ignored rather than
  // honoured, as in the browser build.
  const MachineProfile *wanted = findMachineProfile(settings_.machine.c_str());
  if (!wanted || !Emulator::isMachineRunnable(wanted->id)) {
    wanted = &machineProfile(MachineId::AppleIIe);
  }
  settings_.machine = wanted->key;
  profile_ = wanted;
  display_.setMachine(*wanted);

  emulation_.setVolume(settings_.volume);
  emulation_.setMuted(settings_.muted);
  emulation_.start(wanted->id, static_cast<size_t>(settings_.iigsMemoryKB) * 1024);
  // The cards, then the media, then the power: the machine starts with the
  // layout and the disks it was left with, as a real one would, and its
  // boot scan finds them. Media restored after the power came on was missed
  // by the scan, so a //e left with a hard drive started without it.
  slots_.setMachine(*wanted);
  slots_.apply();
  restoreBatteryRam();
  drives_->inspectorShown = settings_.diskInspector;
  emulation_.driveSounds().setEnabled(settings_.driveSounds);
  drives_->restore();
  // After the slot layout: fitting it can rebuild the SmartPort, taking an
  // image with it.
  hardDrives_->update();
  hardDrives_->restore();
  states_->autosave = settings_.autosave;
  if (platform_.setAppearance) platform_.setAppearance(settings_.appearance);
  joystick_.device = settings_.gamePort;
  joystick_.cursorKeys = settings_.cursorKeys;
  joystick_.gamepadEnabled = settings_.gamepads;
  joystick_.deadzone = settings_.deadzone;
  joystick_.machineRebuilt();
  applySpeed();
  emulation_.setPowered(true);
  started_ = true;
  updateWindowTitle();
}

void App::frame() {
  if (!started_) startEmulation();
  runMenuActions();
  textInputActive_ = ImGui::GetIO().WantTextInput;

  updateScreenSource();
  drives_->update(ImGui::GetTime());
  saveBatteryRamIfChanged(ImGui::GetTime());
  states_->update(ImGui::GetTime());
  joystick_.update(screenHadKeyboard_);
  hardDrives_->update();

  // The decoder and character set live in the machine's video, which a
  // rebuild replaces, so they are told again whenever they may have gone.
  if (display_.takeMachineChange()) applyMachineDisplay();
  display_.applyToRenderer(*platform_.screen);

  // Ctrl+Escape leaves Full Page, as in the browser.
  if (fullPage_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
      heldModifiers(ImGui::GetIO().ConfigMacOSXBehaviors).control) {
    fullPage_ = false;
  }

  ui::SetWindowDocking(settings_.windowDocking);
  screenWindowName_ = nullptr;
  screenFillsMain_ = liveResize_ && screenFillsMain_;
  if (fullPage_) {
    drawFullPage();
    if (settings_.showDisplaySettings) display_.drawWindow(&settings_.showDisplaySettings);
    drawDiskDrives();
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
    drawSwitchConfirmation();
    handleAppShortcuts();
    routeKeyboard();
    buildMenus();
    return;
  }

  if (settings_.showStatusBar) drawStatusBar();
  drawDockSpace();
  if (settings_.showScreen) drawScreenWindow();
  if (settings_.showDisplaySettings) {
    firstPosition(320, 40);
    display_.drawWindow(&settings_.showDisplaySettings);
  }
  drawDiskDrives();
  if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
  drawSwitchConfirmation();

  handleAppShortcuts();
  routeKeyboard();
  buildMenus();
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

namespace {

// An item that runs `run` when chosen, registered under `id`.
MenuItem item(std::map<std::string, std::function<void()>> &actions, const std::string &id,
              const std::string &title, std::function<void()> run, const std::string &key = "",
              unsigned modifiers = 0, bool checked = false, bool enabled = true) {
  MenuItem result;
  result.title = title;
  result.action = id;
  result.key = key;
  result.modifiers = modifiers;
  result.checked = checked;
  result.enabled = enabled;
  actions[id] = std::move(run);
  return result;
}

MenuItem submenu(const std::string &title, std::vector<MenuItem> children, bool enabled = true) {
  MenuItem result;
  result.title = title;
  result.children = std::move(children);
  result.enabled = enabled;
  return result;
}

} // namespace

bool App::commandKeysToWindow() const {
  return textInputActive_ || (screenHadKeyboard_ && commandIsOpenApple());
}

// A choice from the menu bar or the toolbar. ImGui sees every mouse event
// the app gets, the press that opened a toolbar pull-down included, but the
// menu's own tracking takes the release, so ImGui would go on thinking the
// button held and spend the next click letting go of it: a dialog the choice
// opened then needed clicking twice. The button is let go here instead.
void App::menuChosen(const std::string &action) {
  pendingActions_.push_back(action);
  ImGuiIO &io = ImGui::GetIO();
  for (int button = 0; button < ImGuiMouseButton_COUNT; button++) io.AddMouseButtonEvent(button, false);
}

void App::runMenuActions() {
  std::vector<std::string> actions;
  actions.swap(pendingActions_);
  for (const std::string &action : actions) {
    auto it = menuActions_.find(action);
    if (it != menuActions_.end()) it->second();
  }
}

// The menus, rebuilt in full each frame so they always say what is true;
// the platform only rebuilds the real ones when this changes. The
// application and Window menus are the platform's own.
void App::buildMenus() {
  menuActions_.clear();
  auto &a = menuActions_;
  auto toggle = [](bool &flag) {
    return [&flag] {
      flag = !flag;
      ImGui::MarkIniSettingsDirty();
    };
  };

  MenuItem file = submenu("File", {
      item(a, "disk.insert.1", "Insert Disk…", [this] { drives_->chooseDisk(0); }, "o", MOD_COMMAND),
      item(a, "disk.insert.2", "Insert Disk in Drive 2…", [this] { drives_->chooseDisk(1); }, "o",
           MOD_COMMAND | MOD_SHIFT),
      item(a, "disk.eject.1", "Eject Drive 1", [this] { drives_->ejectDrive(0); }, "e", MOD_COMMAND, false,
           drives_->hasDisk(0)),
      item(a, "disk.eject.2", "Eject Drive 2", [this] { drives_->ejectDrive(1); }, "e", MOD_COMMAND | MOD_SHIFT,
           false, drives_->hasDisk(1)),
      MenuItem::separatorItem(),
      item(a, "states.show", "Save States…", toggle(settings_.showSaveStates), "s", MOD_COMMAND | MOD_SHIFT,
           settings_.showSaveStates),
  });

  MenuItem edit = submenu("Edit", {
      item(a, "edit.paste", "Paste to Machine", [this] { paste(); }, "v", MOD_COMMAND),
  });

  menuBar_ = {file, edit, machineMenu(), viewMenu()};

  // The toolbar: the same actions, and the machine choices from the menu.
  toolbar_.powered = emulation_.powered();
  toolbar_.machineName = profile_ ? profile_->name : "";
  toolbar_.hardDrives = hardDrives_->available();
  toolbar_.expansionSlots = profile_ && profile_->caps.hasExpansionSlots;
  toolbar_.machines.clear();
  for (const MenuItem &entry : menuBar_[2].children) {
    if (entry.action.rfind("machine.select.", 0) == 0) toolbar_.machines.push_back(entry);
  }
}

MenuItem App::machineMenu() {
  auto &a = menuActions_;
  const bool powered = emulation_.powered();
  std::vector<MenuItem> items = {
      item(a, "machine.power", "Power", [this] {
             releaseKeys();
             emulation_.setPowered(!emulation_.powered());
           }, "", 0, powered),
      item(a, "machine.ctrlreset", "Ctrl+Reset", [this] {
             emulation_.withMachine([](host::MachineHost &host) { host.warmReset(); });
           }, "F12", MOD_CONTROL, false, powered),
      item(a, "machine.reboot", "Reboot", [this] {
             emulation_.withMachine([](host::MachineHost &host) { host.reset(); });
           }, "r", MOD_CONTROL | MOD_COMMAND, false, powered),
      MenuItem::separatorItem(),
  };

  for (int i = 0; i < MACHINE_COUNT; i++) {
    const MachineProfile &machine = machineProfileAt(i);
    const bool runnable = Emulator::isMachineRunnable(machine.id);
    const bool current = profile_ && machine.id == profile_->id;
    std::string title = machine.name;
    if (!runnable) title += " (ROM missing)";
    const MachineId id = machine.id;
    items.push_back(item(a, std::string("machine.select.") + machine.key, title, [this, id, current] {
                           if (!current) pendingMachine_ = id;
                         }, "", 0, current, runnable));
  }
  items.push_back(MenuItem::separatorItem());

  // A IIgs keeps its own speed register, so it is not offered one here.
  if (profile_ && profile_->family != MachineFamily::AppleIIgs) {
    std::vector<MenuItem> speeds;
    for (int multiple : {1, 2, 4, 8}) {
      char title[32];
      std::snprintf(title, sizeof(title), "%dx  (%.3f MHz)", multiple,
                    profile_->timing.cpuClockHz * multiple / 1.0e6);
      speeds.push_back(item(a, "machine.speed." + std::to_string(multiple), title, [this, multiple] {
                              settings_.speed = multiple;
                              applySpeed();
                              ImGui::MarkIniSettingsDirty();
                            }, "", 0, settings_.speed == multiple));
    }
    items.push_back(submenu("CPU Speed", speeds));
  }

  std::vector<MenuItem> memory;
  for (const MemorySize &size : IIGS_MEMORY_SIZES) {
    const int kb = size.kb;
    memory.push_back(item(a, "machine.iigsmemory." + std::to_string(kb), size.label, [this, kb] {
                            if (settings_.iigsMemoryKB == kb) return;
                            // Changing it rebuilds a running IIgs, as
                            // switching machines does, and is only
                            // remembered by any other machine.
                            settings_.iigsMemoryKB = kb;
                            ImGui::MarkIniSettingsDirty();
                            releaseKeys();
                            saveBatteryRamIfChanged(-1);
                            emulation_.setIIgsFastRam(static_cast<size_t>(kb) * 1024);
                            display_.machineRebuilt();
                            slots_.apply();
                            restoreBatteryRam();
                            joystick_.machineRebuilt();
                          }, "", 0, settings_.iigsMemoryKB == kb));
  }
  items.push_back(submenu("IIgs Memory", memory));

  // Not on a //c, whose every slot is soldered down.
  if (profile_ && profile_->caps.hasExpansionSlots) {
    items.push_back(MenuItem::separatorItem());
    items.push_back(item(a, "slots.show", "Expansion Slots…", [this] {
                           settings_.showExpansionSlots = !settings_.showExpansionSlots;
                           ImGui::MarkIniSettingsDirty();
                         }, "", 0, settings_.showExpansionSlots));
  }
  return submenu("Machine", items);
}

MenuItem App::viewMenu() {
  auto &a = menuActions_;
  auto window = [&a](const std::string &id, const std::string &title, bool &flag, const std::string &key,
                     unsigned modifiers = MOD_COMMAND) {
    return item(a, id, title, [&flag] {
      flag = !flag;
      ImGui::MarkIniSettingsDirty();
    }, key, modifiers, flag);
  };

  std::vector<MenuItem> items = {
      window("view.screen", "Screen", settings_.showScreen, "1"),
      window("view.drives", "Disk Drives", settings_.showDiskDrives, "2"),
  };
  // Offered only when there is a SmartPort: a IIgs's, or a card.
  if (hardDrives_->available()) {
    items.push_back(window("view.harddrives", "SmartPort Drives", settings_.showHardDrives, "3"));
  }
  items.push_back(window("view.joystick", "Joystick", settings_.showJoystick, "4"));
  items.push_back(window("view.display", "Display Settings…", settings_.showDisplaySettings, ","));
  items.push_back(window("view.statusbar", "Status Bar", settings_.showStatusBar, "/"));
  // Light, dark, or whatever the system is, as the browser's theme offers.
  std::vector<MenuItem> appearances;
  const char *names[] = {"System", "Light", "Dark"};
  for (int choice = 0; choice < 3; choice++) {
    appearances.push_back(item(a, "view.appearance." + std::to_string(choice), names[choice], [this, choice] {
                                 settings_.appearance = choice;
                                 if (platform_.setAppearance) platform_.setAppearance(choice);
                                 ImGui::MarkIniSettingsDirty();
                               }, "", 0, settings_.appearance == choice));
  }
  items.push_back(submenu("Appearance", appearances));
  // Off, every window stays a window: none docks into another or into the
  // main window, and one already docked comes back out.
  items.push_back(item(a, "view.docking", "Window Docking", [this] {
                         settings_.windowDocking = !settings_.windowDocking;
                         ImGui::MarkIniSettingsDirty();
                       }, "", 0, settings_.windowDocking));
  items.push_back(MenuItem::separatorItem());
  items.push_back(item(a, "view.fullpage", fullPage_ ? "Leave Full Page" : "Full Page", [this] {
                         fullPage_ = !fullPage_;
                         enterFullPage_ = fullPage_;
                       }, "Escape", MOD_CONTROL, fullPage_));
  items.push_back(item(a, "toggleFullScreen", "Enter Full Screen", [] {}, "f", MOD_CONTROL | MOD_COMMAND));
  items.push_back(MenuItem::separatorItem());

  if (profile_ && profile_->caps.hasUkCharSet) {
    items.push_back(item(a, "view.ukcharset", "UK Character Set", [this] {
                           settings_.ukCharacterSet = !settings_.ukCharacterSet;
                           applyMachineDisplay();
                           ImGui::MarkIniSettingsDirty();
                         }, "", 0, settings_.ukCharacterSet));
  }
  items.push_back(item(a, "view.commandapple", "Command as Open Apple", [this] {
                         releaseKeys();
                         settings_.commandIsOpenApple[profile_->key] = !commandIsOpenApple();
                         ImGui::MarkIniSettingsDirty();
                       }, "", 0, commandIsOpenApple()));
  items.push_back(item(a, "view.cursorkeys", "Cursor Keys as Joystick", [this] {
                         joystick_.cursorKeys = !joystick_.cursorKeys;
                       }, "", 0, joystick_.cursorKeys));
  items.push_back(MenuItem::separatorItem());

  items.push_back(item(a, "view.mute", "Mute", [this] {
                         settings_.muted = !settings_.muted;
                         emulation_.setMuted(settings_.muted);
                         ImGui::MarkIniSettingsDirty();
                       }, "", 0, settings_.muted));
  std::vector<MenuItem> volumes;
  for (int percent : {25, 50, 75, 100}) {
    volumes.push_back(item(a, "view.volume." + std::to_string(percent), std::to_string(percent) + "%",
                           [this, percent] {
                             settings_.volume = percent / 100.0f;
                             emulation_.setVolume(settings_.volume);
                             ImGui::MarkIniSettingsDirty();
                           }, "", 0, std::lround(settings_.volume * 100) == percent));
  }
  items.push_back(submenu("Volume", volumes));
  items.push_back(MenuItem::separatorItem());
  items.push_back(item(a, "view.imguidemo", "Dear ImGui Demo", [this] { showDemo_ = !showDemo_; }, "", 0, showDemo_));
  return submenu("View", items);
}

// Switching is destructive, so it asks first, as the browser build's machine
// menu does: the machine is rebuilt and inserted media and memory are lost.
void App::drawSwitchConfirmation() {
  if (pendingMachine_ && !ImGui::IsPopupOpen(SWITCH_POPUP)) {
    ImGui::OpenPopup(SWITCH_POPUP);
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(SWITCH_POPUP, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  const MachineProfile &target = machineProfile(*pendingMachine_);
  ImGui::Text("Switch to the %s?", target.name);
  ImGui::TextDisabled("The machine is rebuilt. Disks and anything in memory are lost.");
  ImGui::Spacing();
  if (ui::Button("Switch", ImVec2(120, 0), ui::ButtonKind::Primary)) {
    switchMachine(*pendingMachine_);
    pendingMachine_.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    pendingMachine_.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

bool App::switchMachine(MachineId id) {
  releaseKeys();
  saveBatteryRamIfChanged(-1);
  states_->autosaveNow();
  if (!emulation_.setMachine(id)) return false;
  profile_ = &machineProfile(id);
  settings_.machine = profile_->key;
  display_.setMachine(*profile_);
  slots_.setMachine(*profile_);
  slots_.apply();
  restoreBatteryRam();
  applySpeed();
  joystick_.machineRebuilt();
  drives_->machineChanged();
  hardDrives_->machineChanged();
  noSignalStale_ = true;
  ImGui::MarkIniSettingsDirty();
  // The new machine starts as if switched on, as the old one was.
  if (emulation_.powered()) {
    emulation_.withMachine([](host::MachineHost &host) { host.reset(); });
  }
  updateWindowTitle();
  return true;
}

void App::updateWindowTitle() {
  if (platform_.setWindowTitle && profile_) {
    // The machine is the window's subtitle, under the app's name.
    platform_.setWindowTitle("ApplEm");
  }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void App::drawDockSpace() {
  const ImGuiID dockspace = ImGui::GetID(DOCKSPACE_ID);
  ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(),
                               ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar);

  // A first run, or an ini with nothing docked: put the screen in the middle.
  // Anything the user has arranged since is left exactly as it is.
  if (layoutChecked_) return;
  layoutChecked_ = true;
  ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockspace);
  if (node && (node->IsSplitNode() || node->Windows.Size > 0)) return;
  if (ImGui::FindWindowSettingsByID(ImHashStr(SCREEN_WINDOW))) return;

  ImGui::DockBuilderRemoveNode(dockspace);
  ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
  ImGui::DockBuilderDockWindow(SCREEN_WINDOW, dockspace);
  ImGui::DockBuilderFinish(dockspace);
}

// Small indicators along the bottom, as a macOS window's status bar has
// them: the drives' lights on the left, what the keyboard and sound are
// doing in the middle, and the clock on the right. The machine's name is in
// the window's subtitle, so it is not repeated here.
void App::drawStatusBar() {
  const float height = ImGui::GetFrameHeight() + 2;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_MenuBar;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
  if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags)) {
    if (ImGui::BeginMenuBar()) {
      ImDrawList *draw = ImGui::GetWindowDrawList();
      const ImVec4 secondary = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
      const float lineHeight = ImGui::GetTextLineHeight();

      // A light: green while reading, red while writing, a dim ring empty.
      // It is centred on its label as the label is actually placed: in a
      // bar ImGui pushes text down by the frame padding, so the top of the
      // line is not where the text is.
      auto light = [&](const char *label, bool present, bool busy, bool writing) {
        const float x = ImGui::GetCursorScreenPos().x;
        ImGui::Dummy(ImVec2(12, lineHeight));
        ImGui::SameLine(0, 4);
        ImGui::TextColored(present ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : secondary, "%s", label);
        const float y = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
        const ImVec2 centre(x + 5, y);
        const ImU32 colour = !present ? IM_COL32(128, 128, 128, 70)
                             : !busy ? IM_COL32(128, 128, 128, 140)
                             : writing ? IM_COL32(224, 58, 62, 255)
                                       : IM_COL32(97, 187, 70, 255);
        if (present) draw->AddCircleFilled(centre, 4.0f, colour);
        else draw->AddCircle(centre, 3.5f, colour, 0, 1.2f);
        ImGui::SameLine(0, 14);
      };
      light("Disk 1", drives_->hasDisk(0), drives_->isActive(0), drives_->isWriting(0));
      light("Disk 2", drives_->hasDisk(1), drives_->isActive(1), drives_->isWriting(1));
      if (hardDrives_->available()) {
        for (int device = 0; device < HardDrives::DEVICES; device++) {
          const std::string label = "HD " + std::to_string(device + 1);
          light(label.c_str(), hardDrives_->hasImage(device), hardDrives_->isBusy(device),
                hardDrives_->isWriting(device));
        }
      }

      // What the keys are doing.
      if (screenHadKeyboard_) {
        ImGui::TextColored(secondary, "%s", commandIsOpenApple() ? "⌘ is Open Apple" : "⌥ is Open Apple");
        ImGui::SameLine(0, 14);
      }
      if (joystick_.cursorKeys) {
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "Cursor Keys as Joystick");
        ImGui::SameLine(0, 14);
      }
      // Sound only when it is not simply working.
      if (!emulation_.audioRunning()) {
        ImGui::TextColored(ImVec4(0.96f, 0.51f, 0.12f, 1.0f), "No audio device");
        ImGui::SameLine(0, 14);
      } else if (settings_.muted) {
        ImGui::TextColored(secondary, "Muted");
        ImGui::SameLine(0, 14);
      }

      // The clock, against the right-hand edge.
      char clock[48];
      if (!emulation_.powered()) {
        std::snprintf(clock, sizeof(clock), "Off");
      } else if (settings_.speed > 1 && profile_ && profile_->family != MachineFamily::AppleIIgs) {
        std::snprintf(clock, sizeof(clock), "%dx  %.3f MHz", settings_.speed, emulation_.measuredMHz());
      } else {
        std::snprintf(clock, sizeof(clock), "%.3f MHz", emulation_.measuredMHz());
      }
      ImGui::PushFont(ui::monoFont(), 0.0f);
      const float width = ImGui::CalcTextSize(clock).x;
      const float right = ImGui::GetWindowContentRegionMax().x - ImGui::GetStyle().FramePadding.x;
      if (ImGui::GetCursorPosX() < right - width) ImGui::SetCursorPosX(right - width);
      ImGui::TextColored(emulation_.powered() ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : secondary, "%s", clock);
      ImGui::PopFont();
      ImGui::EndMenuBar();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// The window draws every frame it is open; its save and error questions are
// drawn whether it is or not. The options it toggles are remembered.
// The game port's settings are the Joystick's to change; they are kept
// whenever they do.
void App::drawJoystick() {
  firstPosition(380, 120);
  joystick_.draw(&settings_.showJoystick);
  if (joystick_.device != settings_.gamePort || joystick_.cursorKeys != settings_.cursorKeys ||
      joystick_.gamepadEnabled != settings_.gamepads || joystick_.deadzone != settings_.deadzone) {
    settings_.gamePort = joystick_.device;
    settings_.cursorKeys = joystick_.cursorKeys;
    settings_.gamepads = joystick_.gamepadEnabled;
    settings_.deadzone = joystick_.deadzone;
    ImGui::MarkIniSettingsDirty();
  }
}

void App::drawDiskDrives() {
  drawJoystick();
  firstPosition(80, 60);
  states_->draw(&settings_.showSaveStates);
  if (states_->autosave != settings_.autosave) {
    settings_.autosave = states_->autosave;
    ImGui::MarkIniSettingsDirty();
  }
  bool showSlots = settings_.showExpansionSlots && profile_ && profile_->caps.hasExpansionSlots;
  firstPosition(140, 100);
  slots_.draw(&showSlots);
  if (profile_ && profile_->caps.hasExpansionSlots) settings_.showExpansionSlots = showSlots;
  firstPosition(200, 380);
  drives_->draw(&settings_.showDiskDrives);
  firstPosition(260, 160);
  bool showHard = settings_.showHardDrives && hardDrives_->available();
  hardDrives_->draw(&showHard);
  if (hardDrives_->available()) settings_.showHardDrives = showHard;
  const bool sounds = emulation_.driveSounds().enabled();
  if (drives_->inspectorShown != settings_.diskInspector || sounds != settings_.driveSounds) {
    settings_.diskInspector = drives_->inspectorShown;
    settings_.driveSounds = sounds;
    ImGui::MarkIniSettingsDirty();
  }
}

void App::filesDropped(const std::vector<std::string> &paths, std::optional<ImVec2> at) {
  if (!started_) return;
  for (const std::string &path : paths) {
    std::error_code error;
    const size_t size = static_cast<size_t>(std::filesystem::file_size(path, error));
    if (error) continue;
    if (HardDrives::isBlockImage(path, size)) {
      const int device = at ? hardDrives_->deviceAt(*at) : -1;
      hardDrives_->insertFile(device >= 0 ? device : hardDrives_->dropTarget(), path);
      return;
    }
    if (DiskDrives::isFloppyImage(path)) {
      const int drive = at ? drives_->driveAt(*at) : -1;
      drives_->insertFile(drive >= 0 ? drive : drives_->dropTarget(), path);
      return;
    }
  }
}

void App::dragHover(std::optional<ImVec2> at) {
  drives_->dragOver = at;
  hardDrives_->dragOver = at;
}

// The machine's frames, or while it is switched off the no-signal picture,
// which goes through the same CRT chain as its video does.
void App::updateScreenSource() {
  const bool powered = emulation_.powered();
  if (powered != wasPowered_) {
    wasPowered_ = powered;
    noSignalStale_ = true;
  }

  // Drain the queue every frame, shown or not, so the emulation thread never
  // finds it full. A frame that arrives just after switching off must not
  // land on top of the no-signal picture.
  const FrameQueue::Frame *frame = emulation_.takeFrame();
  if (powered) {
    if (frame) platform_.screen->upload(frame->pixels.data(), frame->width, frame->height);
    return;
  }
  if (!noSignalStale_ || !profile_) return;
  noSignalStale_ = false;
  const auto &display = profile_->display;
  const std::vector<uint8_t> picture = buildNoSignalFrame(
      display.pixelWidth, display.pixelHeight, noSignalMachineName(profile_->name));
  platform_.screen->upload(picture.data(), display.pixelWidth, display.pixelHeight);
}

// The 256 bytes go out and come back exactly as the firmware wrote them,
// checksum included: the firmware checks it before trusting the contents,
// and alters nothing as long as nothing else does. They are put back after
// the IIgs is built and before it is powered on, because its firmware reads
// them as it starts; restored any later, it has already written its own
// defaults, which the next save then keeps.
void App::restoreBatteryRam() {
  auto bytes = readFile(settingsDirectory_ + "/iigs-battery-ram.bin");
  if (!bytes || bytes->size() != 256) return;
  emulation_.withMachine([&](host::MachineHost &host) { host.setBatteryRam(*bytes); });
}

// Checked every two seconds, as the browser does, and whenever the machine
// is about to be replaced (now < 0).
void App::saveBatteryRamIfChanged(double now) {
  if (now >= 0 && now - batteryCheckedAt_ < 2.0) return;
  batteryCheckedAt_ = std::max(now, 0.0);
  std::vector<uint8_t> bytes;
  emulation_.withMachine([&](host::MachineHost &host) {
    if (host.takeBatteryRamChanged()) bytes = host.batteryRam();
  });
  if (bytes.size() == 256) {
    writeFile(settingsDirectory_ + "/iigs-battery-ram.bin", bytes.data(), bytes.size());
  }
}

// A host preference, not machine state: a reset keeps it, a rebuilt machine
// is told again, and a value from an older settings file snaps to the
// nearest the menu offers.
void App::applySpeed() {
  int multiple = 1;
  for (int option : {1, 2, 4, 8}) {
    if (std::abs(option - settings_.speed) < std::abs(multiple - settings_.speed)) multiple = option;
  }
  settings_.speed = multiple;
  emulation_.withMachine([&](host::MachineHost &host) { host.setSpeedMultiplier(multiple); });
  emulation_.resetMeasurement();
}

// The video settings that live in the machine rather than in the shader.
void App::applyMachineDisplay() {
  const bool uk = settings_.ukCharacterSet && profile_ && profile_->caps.hasUkCharSet;
  emulation_.withMachine([&](host::MachineHost &host) {
    display_.applyToMachine(host);
    if (Video *video = host.video()) video->setUKCharacterSet(uk);
  });
}

// The picture at the shape the machine's monitor shows it (the profile's
// aspect), as large as the space allows, on black. The CRT chain renders it
// at exactly the pixels it will cover, at the density of the display the
// window is on, and it is drawn one to one.
void App::drawScreen() {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                      IM_COL32(0, 0, 0, 255));

  const auto &display = profile_->display;
  const float aspect =
      static_cast<float>(display.aspectWidth) / static_cast<float>(display.aspectHeight);
  float width = avail.x;
  float height = width / aspect;
  if (height > avail.y) {
    height = avail.y;
    width = height * aspect;
  }

  // Whole device pixels, and the rectangle in points that covers exactly
  // those, so nothing is resampled on the way to the glass.
  const float scale = ImGui::GetWindowViewport()->FramebufferScale.x > 0
                          ? ImGui::GetWindowViewport()->FramebufferScale.x
                          : 1.0f;
  const int pixelWidth = static_cast<int>(width * scale);
  const int pixelHeight = static_cast<int>(height * scale);
  width = pixelWidth / scale;
  height = pixelHeight / scale;
  const ImVec2 p0(std::floor(origin.x + (avail.x - width) * 0.5f),
                  std::floor(origin.y + (avail.y - height) * 0.5f));
  const ImVec2 p1(p0.x + width, p0.y + height);

  const ImTextureID texture = platform_.screen->render(pixelWidth, pixelHeight, scale);
  if (texture != ImTextureID_Invalid) {
    const ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
    if (pio.DrawCallback_SetSamplerNearest) draw->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
    draw->AddImage(ImTextureRef(texture), p0, p1);
    if (pio.DrawCallback_SetSamplerLinear) draw->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
  }
  const std::string error = platform_.screen->error();
  if (!error.empty()) {
    draw->AddText(ImVec2(origin.x + 8, origin.y + 8), IM_COL32(224, 58, 62, 255), error.c_str());
  }

  screenAspect_ = aspect;
  const ImGuiViewport *mainViewport = ImGui::GetMainViewport();
  screenDocked_ = ImGui::IsWindowDocked();
  screenChrome_ = ImVec2(ImGui::GetWindowWidth() - avail.x, ImGui::GetWindowHeight() - avail.y);
  // What surrounds the picture is held through a drag: it does not change,
  // and measured mid-drag it can be a frame behind the window.
  if (ImGui::GetWindowViewport() == mainViewport && (screenDocked_ || fullPage_) && !liveResize_) {
    screenFillsMain_ = true;
    screenExtra_ = ImVec2(mainViewport->Size.x - avail.x, mainViewport->Size.y - avail.y);
    fitMainWindow();
  }

  // Takes the clicks, so a click on the picture focuses the window rather
  // than starting to drag it.
  ImGui::InvisibleButton("##screen", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 1.0f)));
}

namespace {

// The smallest picture a resize may leave, in points: a //e at one to one.
constexpr float MIN_PICTURE_WIDTH = 280;

// The Screen window's size, floating, with its picture at the machine's
// shape and its title bar and border on top. The edge that moved most leads.
struct ScreenShape {
  float aspect;
  ImVec2 chrome;
};

void keepScreenShape(ImGuiSizeCallbackData *data) {
  const auto *shape = static_cast<const ScreenShape *>(data->UserData);
  const ImVec2 current = data->CurrentSize;
  const ImVec2 desired = data->DesiredSize;
  if (std::fabs(desired.x - current.x) >= std::fabs(desired.y - current.y)) {
    const float picture = std::max(desired.x - shape->chrome.x, MIN_PICTURE_WIDTH);
    data->DesiredSize = ImVec2(picture + shape->chrome.x, picture / shape->aspect + shape->chrome.y);
  } else {
    const float picture = std::max((desired.y - shape->chrome.y) * shape->aspect, MIN_PICTURE_WIDTH);
    data->DesiredSize = ImVec2(picture + shape->chrome.x, picture / shape->aspect + shape->chrome.y);
  }
}

} // namespace

bool App::mainContentSizeFor(float proposedWidth, float proposedHeight, float currentWidth,
                             float currentHeight, float &width, float &height) const {
  if (!screenFillsMain_ || screenAspect_ <= 0) return false;
  const ImVec2 extra = screenExtra_;
  float picture;
  const bool widthLeads = liveResize_ ? widthLeads_
                                      : std::fabs(proposedWidth - currentWidth) >=
                                            std::fabs(proposedHeight - currentHeight);
  if (widthLeads) {
    picture = proposedWidth - extra.x;
  } else {
    picture = (proposedHeight - extra.y) * screenAspect_;
  }
  picture = std::max(picture, MIN_PICTURE_WIDTH);
  width = std::round(picture + extra.x);
  height = std::round(picture / screenAspect_ + extra.y);
  return true;
}

bool App::mainContentSizeWithin(float maxWidth, float maxHeight, float &width, float &height) const {
  if (!screenFillsMain_ || screenAspect_ <= 0) return false;
  const ImVec2 extra = screenExtra_;
  float picture = std::min(maxWidth - extra.x, (maxHeight - extra.y) * screenAspect_);
  picture = std::max(picture, MIN_PICTURE_WIDTH);
  width = std::floor(picture + extra.x);
  height = std::floor(picture / screenAspect_ + extra.y);
  return true;
}

void App::beginLiveResize(bool widthLeads) {
  liveResize_ = true;
  widthLeads_ = widthLeads;
  // A floating window the main window grows over would be merged into it,
  // and then carried along when the window's top or left edge moves. Held
  // apart, each keeps its place on the screen; they merge again after.
  ImGui::GetIO().ConfigViewportsNoAutoMerge = true;
  // ImGui sees every mouse event the app gets, the press on the window's
  // frame included, and a floating window near that point takes it for a
  // drag of itself and follows the pointer round the resize. The mouse is
  // the frame's until the resize ends.
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  GImGui->MovingWindow = nullptr;
  ImGui::ClearActiveID();
}

void App::endLiveResize() {
  liveResize_ = false;
  ImGui::GetIO().ConfigViewportsNoAutoMerge = false;
  ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
  // The drag kept the shape; what it left is the new fit.
  fittedAspect_ = screenAspect_;
  fittedExtra_ = screenExtra_;
}

void App::fitMainWindow() {
  if (!platform_.setMainContentSize) return;
  // The dock space settles over the first frames, and a splitter or a
  // window being dragged changes what is left for the picture as it goes:
  // fit when it has settled, not during.
  if (liveResize_ || ImGui::GetFrameCount() < 4 || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
  if (std::fabs(screenAspect_ - fittedAspect_) < 1e-4f && std::fabs(screenExtra_.x - fittedExtra_.x) < 0.5f &&
      std::fabs(screenExtra_.y - fittedExtra_.y) < 0.5f) {
    return;
  }
  fittedAspect_ = screenAspect_;
  fittedExtra_ = screenExtra_;

  // Keep the width and fix the height, unless that will not fit the screen.
  const ImVec2 size = ImGui::GetMainViewport()->Size;
  float width = 0;
  float height = 0;
  mainContentSizeFor(size.x, size.y, size.x, size.y, width, height);
  if (platform_.mainContentLimit) {
    const ImVec2 limit = platform_.mainContentLimit();
    if (height > limit.y || width > limit.x) mainContentSizeWithin(limit.x, limit.y, width, height);
  }
  if (std::fabs(width - size.x) >= 1 || std::fabs(height - size.y) >= 1) {
    platform_.setMainContentSize(width, height);
  }
}

void App::drawScreenWindow() {
  // Floating, the window keeps the picture's shape itself; docked, the main
  // window does it (see mainContentSizeFor).
  static ScreenShape shape;
  if (!screenDocked_ && screenAspect_ > 0) {
    shape = ScreenShape{screenAspect_, screenChrome_};
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, FLT_MAX), keepScreenShape, &shape);
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const bool visible = ImGui::Begin(
      SCREEN_WINDOW, &settings_.showScreen,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (visible) {
    screenWindowName_ = SCREEN_WINDOW;
    drawScreen();
  }
  ImGui::End();
}

// The picture over the whole of the main window, with no menu bar, status
// bar or dock space. Windows that have been dragged out stay where they are.
void App::drawFullPage() {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::SetNextWindowViewport(viewport->ID);
  if (enterFullPage_) {
    ImGui::SetNextWindowFocus();
    enterFullPage_ = false;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoScrollWithMouse;
  if (ImGui::Begin(FULL_PAGE_WINDOW, nullptr, flags)) {
    screenWindowName_ = FULL_PAGE_WINDOW;
    drawScreen();
    // Right-click for the way out, since the menu bar has gone.
    if (ImGui::BeginPopupContextWindow("##FullPageMenu")) {
      if (ImGui::MenuItem("Leave Full Page", "Ctrl+Esc")) fullPage_ = false;
      ImGui::MenuItem("Display Settings", nullptr, &settings_.showDisplaySettings);
      ImGui::EndPopup();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

bool App::commandIsOpenApple() const {
  if (!profile_) return false;
  auto it = settings_.commandIsOpenApple.find(profile_->key);
  if (it != settings_.commandIsOpenApple.end()) return it->second;
  return profile_->family == MachineFamily::AppleIIgs;
}

void App::paste() {
  const char *text = ImGui::GetClipboardText();
  if (!text || !*text) return;
  emulation_.withMachine([text](host::MachineHost &host) { host.pasteText(text); });
}

// The app's own keys are the menu bar's key equivalents now (Ctrl+F12 is
// Ctrl+Reset, Command-V pastes); this is what is left that a menu cannot be.
void App::handleAppShortcuts() {}

// While the screen has the keyboard, every key goes to the machine as a
// browser keycode, so the core's own translation does the rest exactly as it
// does in the browser. Key repeat is ImGui's, at the system's rate.
void App::routeKeyboard() {
  ImGuiWindow *screen = screenWindowName_ ? ImGui::FindWindowByName(screenWindowName_) : nullptr;
  const ImGuiContext &g = *ImGui::GetCurrentContext();
  const bool focused = screen && g.NavWindow == screen;
  const bool keyboard = started_ && emulation_.powered() && focused &&
                        !ImGui::GetIO().WantTextInput;

  if (!keyboard) {
    if (screenHadKeyboard_) releaseKeys();
    screenHadKeyboard_ = false;
    return;
  }
  screenHadKeyboard_ = true;
  // Claim the keyboard, or ImGui's Cocoa backend hands every key it did not
  // use back to macOS, which finds no text field to type into and beeps.
  ImGui::SetNextFrameWantCaptureKeyboard(true);

  const bool swap = ImGui::GetIO().ConfigMacOSXBehaviors;
  const HeldModifiers held = heldModifiers(swap);
  const bool command = commandIsOpenApple();
  const bool capsLock = platform_.capsLockOn && platform_.capsLockOn();

  emulation_.withMachine([&](host::MachineHost &host) {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
      const ImGuiKey key = static_cast<ImGuiKey>(k);
      const std::optional<HostKey> hostKey = browserKeyFor(key, swap);
      if (!hostKey) continue;
      // Caps Lock is a state the core is told with every key, not a key.
      if (hostKey->keyCode == 20) continue;
      // Ctrl+F12 is the app's Ctrl+Reset.
      if (key == ImGuiKey_F12 && held.control) continue;

      // Modifier keys do not repeat on a Mac, so they are never sent twice.
      const bool repeat = !isModifier(hostKey->keyCode);
      if (ImGui::IsKeyPressed(key, repeat)) {
        if (auto event = coreKeyEvent(*hostKey, held, command, false)) {
          host.handleRawKeyDown(event->keyCode, event->shift, event->ctrl,
                                event->alt, event->meta, capsLock, event->location);
          keysDown_.insert(k);
        }
      }
      if (ImGui::IsKeyReleased(key) && keysDown_.erase(k)) {
        if (auto event = coreKeyEvent(*hostKey, held, command, true)) {
          host.handleRawKeyUp(event->keyCode, event->shift, event->ctrl,
                              event->alt, event->meta, event->location);
        }
      }
    }
  });
}

void App::releaseKeys() {
  if (!started_) return;
  const bool command = commandIsOpenApple();
  const bool swap = ImGui::GetIO().ConfigMacOSXBehaviors;
  emulation_.withMachine([&](host::MachineHost &host) {
    for (int k : keysDown_) {
      const std::optional<HostKey> hostKey = browserKeyFor(static_cast<ImGuiKey>(k), swap);
      if (!hostKey) continue;
      if (auto event = coreKeyEvent(*hostKey, HeldModifiers{}, command, true)) {
        host.handleRawKeyUp(event->keyCode, false, false, false, false,
                            event->location);
      }
    }
    host.releaseModifiers();
  });
  keysDown_.clear();
}

} // namespace a2e::native
