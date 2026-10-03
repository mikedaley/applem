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
#include "cards/mockingboard/mockingboard_card.hpp"

#include <algorithm>
#include <cassert>
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
  registerDebuggerHandler();
  registerMemoryHandler();
  registerBasicHandler();
  memory_.showDebugger = [this] {
    settings_.showCpuDebugger = true;
    ImGui::SetWindowFocus("CPU Debugger");
    ImGui::MarkIniSettingsDirty();
  };
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
    else if (std::sscanf(line, "ShowMockingboard=%d", &value) == 1) s.showMockingboard = value;
    else if (std::sscanf(line, "ShowEnsoniq=%d", &value) == 1) s.showEnsoniq = value;
    else if (std::sscanf(line, "ShowBasic=%d", &value) == 1) s.showBasic = value;
    else if (unsigned mutes = 0; std::sscanf(line, "EnsoniqMutes=%u", &mutes) == 1) s.ensoniqMutes = mutes;
    else if (std::sscanf(line, "ShowCpuDebugger=%d", &value) == 1) s.showCpuDebugger = value;
    else if (std::sscanf(line, "ShowMemoryViewer=%d", &value) == 1) s.showMemoryViewer = value;
    else if (std::sscanf(line, "MockingboardMutes=%d", &value) == 1) s.mockingboardMutes = value & 0x3F;
    else if (std::sscanf(line, "MockingboardPhaseLock=%d", &value) == 1) s.mockingboardPhaseLock = value;
    else if (std::sscanf(line, "ShowEqualizer=%d", &value) == 1) s.showEqualizer = value;
    else if (std::sscanf(line, "Equalizer=%d", &value) == 1) s.equalizer.enabled = value;
    else if (std::sscanf(line, "EqualizerPreamp=%f", &number) == 1) s.equalizer.preampDb = number;
    else if (std::sscanf(line, "EqualizerBand.%d=%f", &value, &number) == 2) {
      if (value >= 0 && value < Equalizer::BANDS) s.equalizer.gainDb[value] = number;
    }
    else if (std::sscanf(line, "GamePort=%d", &value) == 1) s.gamePort = value == 1 ? 1 : 0;
    else if (std::sscanf(line, "CursorKeys=%d", &value) == 1) s.cursorKeys = value;
    else if (std::sscanf(line, "Gamepads=%d", &value) == 1) s.gamepads = value;
    // A stored 0 is a deadzone of 0, which the browser read back as 0.1.
    else if (std::sscanf(line, "Deadzone=%f", &number) == 1) s.deadzone = std::clamp(number, 0.0f, MAX_DEADZONE);
    else if (std::sscanf(line, "NoSlotClock=%d", &value) == 1) app->slots_.noSlotClock = value;
    else if (std::sscanf(line, "ShowStatusBar=%d", &value) == 1) s.showStatusBar = value;
    else if (std::sscanf(line, "CommandIsOpenApple.%63[^=]=%d", text, &value) == 2) {
      s.commandIsOpenApple[text] = value;
    } else if (std::sscanf(line, "PAL.%63[^=]=%d", text, &value) == 2) {
      s.pal[text] = value;
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
    out->appendf("ShowMockingboard=%d\n", s.showMockingboard ? 1 : 0);
    out->appendf("ShowEnsoniq=%d\n", s.showEnsoniq ? 1 : 0);
    out->appendf("ShowBasic=%d\n", s.showBasic ? 1 : 0);
    out->appendf("EnsoniqMutes=%u\n", static_cast<unsigned>(s.ensoniqMutes));
    out->appendf("ShowCpuDebugger=%d\n", s.showCpuDebugger ? 1 : 0);
    out->appendf("ShowMemoryViewer=%d\n", s.showMemoryViewer ? 1 : 0);
    out->appendf("MockingboardMutes=%d\n", s.mockingboardMutes);
    out->appendf("MockingboardPhaseLock=%d\n", s.mockingboardPhaseLock ? 1 : 0);
    out->appendf("ShowEqualizer=%d\n", s.showEqualizer ? 1 : 0);
    out->appendf("Equalizer=%d\n", s.equalizer.enabled ? 1 : 0);
    out->appendf("EqualizerPreamp=%.1f\n", s.equalizer.preampDb);
    for (int i = 0; i < Equalizer::BANDS; i++) out->appendf("EqualizerBand.%d=%.1f\n", i, s.equalizer.gainDb[i]);
    out->appendf("GamePort=%d\n", s.gamePort);
    out->appendf("CursorKeys=%d\n", s.cursorKeys ? 1 : 0);
    out->appendf("Gamepads=%d\n", s.gamepads ? 1 : 0);
    out->appendf("Deadzone=%.2f\n", s.deadzone);
    out->appendf("NoSlotClock=%d\n", app->slots_.noSlotClock ? 1 : 0);
    out->appendf("ShowStatusBar=%d\n", s.showStatusBar ? 1 : 0);
    for (const auto &[key, on] : s.commandIsOpenApple) {
      out->appendf("CommandIsOpenApple.%s=%d\n", key.c_str(), on ? 1 : 0);
    }
    for (const auto &[key, on] : s.pal) {
      out->appendf("PAL.%s=%d\n", key.c_str(), on ? 1 : 0);
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

// The debugger's breakpoints, watches, labels and layout, under
// [ApplEmDebugger][State].
void App::registerDebuggerHandler() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "ApplEmDebugger";
  handler.TypeHash = ImHashStr("ApplEmDebugger");
  handler.UserData = &debugger_;
  handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *, const char *name) -> void * {
    return std::strcmp(name, "State") == 0 ? reinterpret_cast<void *>(1) : nullptr;
  };
  handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *h, void *, const char *line) {
    static_cast<CpuDebugger *>(h->UserData)->readSetting(line);
  };
  handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out) {
    std::string text;
    static_cast<const CpuDebugger *>(h->UserData)->writeSettings(text);
    out->appendf("[%s][State]\n", h->TypeName);
    out->append(text.c_str());
    out->append("\n");
  };
  ImGui::AddSettingsHandler(&handler);
}

// The memory viewer's view and bookmarks, under [ApplEmMemory][State].
void App::registerMemoryHandler() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "ApplEmMemory";
  handler.TypeHash = ImHashStr("ApplEmMemory");
  handler.UserData = &memory_;
  handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *, const char *name) -> void * {
    return std::strcmp(name, "State") == 0 ? reinterpret_cast<void *>(1) : nullptr;
  };
  handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *h, void *, const char *line) {
    static_cast<MemoryViewer *>(h->UserData)->readSetting(line);
  };
  handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out) {
    std::string text;
    static_cast<const MemoryViewer *>(h->UserData)->writeSettings(text);
    out->appendf("[%s][State]\n", h->TypeName);
    out->append(text.c_str());
    out->append("\n");
  };
  ImGui::AddSettingsHandler(&handler);
}

// The BASIC window's program, breakpoints and layout, under
// [ApplEmBasic][State].
void App::registerBasicHandler() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "ApplEmBasic";
  handler.TypeHash = ImHashStr("ApplEmBasic");
  handler.UserData = &basic_;
  handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *, const char *name) -> void * {
    return std::strcmp(name, "State") == 0 ? reinterpret_cast<void *>(1) : nullptr;
  };
  handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *h, void *, const char *line) {
    static_cast<BasicWindow *>(h->UserData)->readSetting(line);
  };
  handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out) {
    std::string text;
    static_cast<const BasicWindow *>(h->UserData)->writeSettings(text);
    out->appendf("[%s][State]\n", h->TypeName);
    out->append(text.c_str());
    out->append("\n");
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
  profile_ = &machineProfile(wanted->id, standardFor(*wanted));
  display_.setMachine(*wanted);

  emulation_.setVolume(settings_.volume);
  emulation_.setMuted(settings_.muted);
  equalizer_.settings = settings_.equalizer;
  emulation_.equalizer().set(settings_.equalizer);
  emulation_.start(wanted->id, static_cast<size_t>(settings_.iigsMemoryKB) * 1024);
  const VideoStandard standard = profile_->timing.standard;
  emulation_.withMachine([&](host::MachineHost &host) { host.setVideoStandard(standard); });
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
  mockingboard_.mutes = settings_.mockingboardMutes;
  applyMockingboardPhaseLock();
  ensoniq_.mutes = settings_.ensoniqMutes;
  debugger_.setMachine(*wanted);
  memory_.setMachine(*wanted);
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
  mockingboard_.update();
  ensoniq_.update();
  basic_.update(settings_.showBasic);
  debugger_.update();
  memory_.update(settings_.showMemoryViewer);
  updateMouse();

  // The decoder and character set live in the machine's video, which a
  // rebuild replaces, so they are told again whenever they may have gone.
  if (display_.takeMachineChange()) applyMachineDisplay();
  float beamX = -1.0f, beamY = -1.0f;
  debugger_.beamOnScreen(settings_.showCpuDebugger, beamX, beamY);
  display_.applyToRenderer(*platform_.screen, beamX, beamY);

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
    // An action nothing answers is a menu item or a toolbar button whose
    // id was renamed on one side only, which otherwise fails in silence.
    else assert(!"a menu or toolbar action with nothing registered under it");
  }
}

// The menus, rebuilt in full each frame so they always say what is true;
// the platform only rebuilds the real ones when this changes. The
// application and Window menus are the platform's own.
void App::buildMenus() {
  menuActions_.clear();
  auto &a = menuActions_;

  // ApplEm: Settings, which are the display's for now, where every Mac app
  // keeps them.
  MenuItem application = submenu(APPLICATION_MENU, {
      item(a, "app.settings", "Settings…", [this] {
             settings_.showDisplaySettings = true;
             ImGui::SetWindowFocus("Display Settings");
             ImGui::MarkIniSettingsDirty();
           }, ",", MOD_COMMAND),
  });

  // File: media in and out, and closing a window.
  std::vector<MenuItem> fileItems = {
      item(a, "disk.insert.1", "Insert in Drive 1…", [this] { drives_->chooseDisk(0); }, "o", MOD_COMMAND),
      item(a, "disk.insert.2", "Insert in Drive 2…", [this] { drives_->chooseDisk(1); }, "o",
           MOD_COMMAND | MOD_SHIFT),
  };
  std::vector<MenuItem> recent;
  for (int drive = 0; drive < 2; drive++) {
    const std::vector<std::string> names = drives_->recentNames(drive);
    if (names.empty()) continue;
    if (!recent.empty()) recent.push_back(MenuItem::separatorItem());
    MenuItem heading = item(a, "disk.recent.heading." + std::to_string(drive),
                            "Drive " + std::to_string(drive + 1), [] {}, "", 0, false, false);
    recent.push_back(heading);
    for (size_t i = 0; i < names.size(); i++) {
      recent.push_back(item(a, "disk.recent." + std::to_string(drive) + "." + std::to_string(i), names[i],
                            [this, drive, i] { drives_->insertRecent(drive, i); }));
    }
  }
  if (!recent.empty()) {
    recent.push_back(MenuItem::separatorItem());
    recent.push_back(item(a, "disk.recent.clear", "Clear Menu", [this] {
                            drives_->clearRecent(0);
                            drives_->clearRecent(1);
                          }));
  }
  fileItems.push_back(submenu("Open Recent", recent, !recent.empty()));
  fileItems.push_back(MenuItem::separatorItem());
  fileItems.push_back(item(a, "disk.eject.1", "Eject Drive 1", [this] { drives_->ejectDrive(0); }, "e", MOD_COMMAND,
                           false, drives_->hasDisk(0)));
  fileItems.push_back(item(a, "disk.eject.2", "Eject Drive 2", [this] { drives_->ejectDrive(1); }, "e",
                           MOD_COMMAND | MOD_SHIFT, false, drives_->hasDisk(1)));
  // The SmartPort's, when there is one: a IIgs's, or a card.
  if (hardDrives_->available()) {
    fileItems.push_back(MenuItem::separatorItem());
    std::vector<MenuItem> inserts, ejects;
    for (int device = 0; device < 2; device++) {
      const std::string unit = "Device " + std::to_string(device + 1);
      inserts.push_back(item(a, "hd.insert." + std::to_string(device), unit + "…",
                             [this, device] { hardDrives_->chooseImage(device); }));
      ejects.push_back(item(a, "hd.eject." + std::to_string(device), unit,
                            [this, device] { hardDrives_->ejectDevice(device); }, "", 0, false,
                            hardDrives_->hasImage(device)));
    }
    fileItems.push_back(submenu("Insert Hard Disk Image", inserts));
    fileItems.push_back(submenu("Eject Hard Disk Image", ejects, hardDrives_->hasImage(0) || hardDrives_->hasImage(1)));
  }
  fileItems.push_back(MenuItem::separatorItem());
  fileItems.push_back(item(a, "window.close", "Close Window", [this] { closeFocusedWindow(); }, "w", MOD_COMMAND,
                           false, focusedToolWindow() != nullptr));
  MenuItem file = submenu("File", fileItems);

  // Edit: text in and out of the machine. The usual Undo, Cut, Copy and
  // Select All are left out because there is nothing for them to act on: a
  // text field that is being typed into takes those keys itself.
  MenuItem edit = submenu("Edit", {
      item(a, "edit.copyscreen", "Copy Screen Text", [this] { copyScreenText(); }, "c", MOD_COMMAND | MOD_SHIFT,
           false, emulation_.powered()),
      item(a, "edit.paste", "Paste to Machine", [this] { paste(); }, "v", MOD_COMMAND),
  });

  menuBar_ = {application, file, edit, machineMenu(), viewMenu()};
  if (std::optional<MenuItem> debug = debugMenu()) menuBar_.push_back(*debug);
  menuBar_.push_back(windowMenu());
  menuBar_.push_back(helpMenu());

  // The toolbar: the same actions, and the machine choices from the menu.
  toolbar_.powered = emulation_.powered();
  toolbar_.machineName = profile_ ? profile_->name : "";
  toolbar_.hardDrives = hardDrives_->available();
  toolbar_.expansionSlots = profile_ && profile_->caps.hasExpansionSlots;
  toolbar_.machines.clear();
  for (const MenuItem &menu : menuBar_) {
    if (menu.title != "Machine") continue;
    for (const MenuItem &entry : menu.children) {
      if (entry.action.rfind("machine.select.", 0) == 0) toolbar_.machines.push_back(entry);
    }
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
      item(a, "machine.ctrlreset", "Control-Reset", [this] {
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

  // Only for a machine Apple also made for PAL countries.
  if (profile_ && machineHasStandard(profile_->id, VideoStandard::PAL)) {
    const bool pal = profile_->timing.standard == VideoStandard::PAL;
    items.push_back(submenu("Video Standard", {
        item(a, "machine.standard.ntsc", "NTSC  (60Hz, 262 lines)",
             [this] { setVideoStandard(VideoStandard::NTSC); }, "", 0, !pal),
        item(a, "machine.standard.pal", "PAL  (50Hz, 312 lines)",
             [this] { setVideoStandard(VideoStandard::PAL); }, "", 0, pal),
    }));
  }

  // Only while a IIgs is running: on any other machine the choice would do
  // nothing that could be seen.
  if (profile_ && profile_->family == MachineFamily::AppleIIgs) {
    std::vector<MenuItem> memory;
    for (const MemorySize &size : IIGS_MEMORY_SIZES) {
      const int kb = size.kb;
      memory.push_back(item(a, "machine.iigsmemory." + std::to_string(kb), size.label, [this, kb] {
                              if (settings_.iigsMemoryKB == kb) return;
                              // Changing it rebuilds the IIgs, as switching
                              // machines does.
                              settings_.iigsMemoryKB = kb;
                              ImGui::MarkIniSettingsDirty();
                              releaseKeys();
                              saveBatteryRamIfChanged(-1);
                              emulation_.setIIgsFastRam(static_cast<size_t>(kb) * 1024);
                              display_.machineRebuilt();
                              slots_.apply();
                              restoreBatteryRam();
                              joystick_.machineRebuilt();
                              if (profile_) {
                                debugger_.setMachine(*profile_);
                                memory_.setMachine(*profile_);
                              }
                            }, "", 0, settings_.iigsMemoryKB == kb));
    }
    items.push_back(submenu("IIgs Memory", memory));
  }

  // How the host's keyboard reaches the machine.
  std::vector<MenuItem> keyboard = {
      item(a, "machine.commandapple", "Command as Open Apple", [this] {
             releaseKeys();
             settings_.commandIsOpenApple[profile_->key] = !commandIsOpenApple();
             ImGui::MarkIniSettingsDirty();
           }, "", 0, commandIsOpenApple()),
      item(a, "machine.cursorkeys", "Cursor Keys as Joystick", [this] {
             joystick_.cursorKeys = !joystick_.cursorKeys;
           }, "", 0, joystick_.cursorKeys),
  };
  // The //e's generator alone holds a second set.
  if (profile_ && profile_->caps.hasUkCharSet) {
    keyboard.push_back(item(a, "machine.ukcharset", "UK Character Set", [this] {
                              settings_.ukCharacterSet = !settings_.ukCharacterSet;
                              applyMachineDisplay();
                              ImGui::MarkIniSettingsDirty();
                            }, "", 0, settings_.ukCharacterSet));
  }
  items.push_back(MenuItem::separatorItem());
  items.push_back(submenu("Keyboard", keyboard));
  // Taking the mouse from the menu, for anyone who has not found the click.
  // Giving it back is Control-Option, since the pointer cannot reach a menu
  // while the machine has it.
  items.push_back(item(a, "machine.mouse", mouseCaptured_ ? "Release Mouse (\u2303\u2325)" : "Capture Mouse",
                       [this] { setMouseCaptured(!mouseCaptured_); }, "", 0, false,
                       mouseCaptured_ || (machineHasMouse_ && powered)));

  std::vector<MenuItem> volumes;
  for (int percent : {25, 50, 75, 100}) {
    volumes.push_back(item(a, "machine.volume." + std::to_string(percent), std::to_string(percent) + "%",
                           [this, percent] {
                             settings_.volume = percent / 100.0f;
                             emulation_.setVolume(settings_.volume);
                             ImGui::MarkIniSettingsDirty();
                           }, "", 0, std::lround(settings_.volume * 100) == percent));
  }
  items.push_back(submenu("Sound", {
      item(a, "machine.mute", "Mute", [this] {
             settings_.muted = !settings_.muted;
             emulation_.setMuted(settings_.muted);
             ImGui::MarkIniSettingsDirty();
           }, "", 0, settings_.muted),
      submenu("Volume", volumes),
      MenuItem::separatorItem(),
      item(a, "machine.mockingboardPhaseLock", "Mockingboard Phase Lock", [this] {
             settings_.mockingboardPhaseLock = !settings_.mockingboardPhaseLock;
             applyMockingboardPhaseLock();
             ImGui::MarkIniSettingsDirty();
           }, "", 0, settings_.mockingboardPhaseLock),
      item(a, "machine.equalizer", "Equalizer", [this] {
             settings_.showEqualizer = !settings_.showEqualizer;
             ImGui::MarkIniSettingsDirty();
           }, "", 0, settings_.showEqualizer),
  }));
  return submenu("Machine", items);
}

// How the picture and the main window look.
MenuItem App::viewMenu() {
  auto &a = menuActions_;
  std::vector<MenuItem> items = {
      item(a, "view.statusbar", "Status Bar", [this] {
             settings_.showStatusBar = !settings_.showStatusBar;
             ImGui::MarkIniSettingsDirty();
           }, "/", MOD_COMMAND, settings_.showStatusBar),
  };
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
  items.push_back(MenuItem::separatorItem());
  items.push_back(item(a, "view.fullpage", fullPage_ ? "Leave Full Page" : "Full Page", [this] {
                         fullPage_ = !fullPage_;
                         enterFullPage_ = fullPage_;
                       }, "Escape", MOD_CONTROL, fullPage_));
  items.push_back(item(a, "toggleFullScreen", "Enter Full Screen", [] {}, "f", MOD_CONTROL | MOD_COMMAND));
  return submenu("View", items);
}

// The debug views, each offered only when the machine has what it shows, as
// the browser's menus follow the machine (machine-availability.js).
std::optional<MenuItem> App::debugMenu() {
  auto &a = menuActions_;
  const bool on = emulation_.powered();
  std::vector<MenuItem> items = {
      item(a, "debug.cpu", "CPU Debugger", [this] {
             settings_.showCpuDebugger = !settings_.showCpuDebugger;
             ImGui::MarkIniSettingsDirty();
           }, "d", MOD_COMMAND | MOD_SHIFT, settings_.showCpuDebugger),
      item(a, "debug.memory", "Memory Viewer", [this] {
             settings_.showMemoryViewer = !settings_.showMemoryViewer;
             ImGui::MarkIniSettingsDirty();
           }, "m", MOD_COMMAND | MOD_SHIFT, settings_.showMemoryViewer),
  };
  if (mockingboard_.available()) {
    items.push_back(item(a, "debug.mockingboard", "Mockingboard", [this] {
                           settings_.showMockingboard = !settings_.showMockingboard;
                           ImGui::MarkIniSettingsDirty();
                         }, "", 0, settings_.showMockingboard));
  }
  // Applesoft, on the 8-bit machines.
  if (basic_.available()) {
    items.insert(items.begin() + 2, item(a, "debug.basic", "Applesoft BASIC", [this] {
                                           settings_.showBasic = !settings_.showBasic;
                                           ImGui::MarkIniSettingsDirty();
                                         }, "b", MOD_COMMAND | MOD_SHIFT, settings_.showBasic));
  }
  // A IIgs's sound chip.
  if (ensoniq_.available()) {
    items.push_back(item(a, "debug.ensoniq", "Ensoniq", [this] {
                           settings_.showEnsoniq = !settings_.showEnsoniq;
                           ImGui::MarkIniSettingsDirty();
                         }, "", 0, settings_.showEnsoniq));
  }
  // The browser's keys, which are Visual Studio's, and Xcode's beside them
  // as hidden items: macOS takes F11 for Show Desktop, so on most Macs Step
  // Into's F11 never arrives and F7 does.
  auto alternate = [&a](const std::string &id, const std::string &key, unsigned modifiers, bool enabled,
                        std::function<void()> run) {
    MenuItem hidden = item(a, id, id, std::move(run), key, modifiers, false, enabled);
    hidden.hidden = true;
    return hidden;
  };
  const std::vector<MenuItem> run = {
      MenuItem::separatorItem(),
      item(a, "debug.continue", debugger_.paused() ? "Continue" : "Pause", [this] { debugger_.continueOrPause(); },
           "F5", 0, false, on),
      alternate("debug.continue.xcode", "y", MOD_CONTROL | MOD_COMMAND, on, [this] { debugger_.continueOrPause(); }),
      item(a, "debug.stepover", "Step Over", [this] { debugger_.stepOver(); }, "F10", 0, false, on),
      alternate("debug.stepover.xcode", "F6", 0, on, [this] { debugger_.stepOver(); }),
      item(a, "debug.stepinto", "Step Into", [this] { debugger_.stepInto(); }, "F11", 0, false, on),
      alternate("debug.stepinto.xcode", "F7", 0, on, [this] { debugger_.stepInto(); }),
      item(a, "debug.stepout", "Step Out", [this] { debugger_.stepOut(); }, "F11", MOD_SHIFT, false, on),
      alternate("debug.stepout.xcode", "F8", 0, on, [this] { debugger_.stepOut(); }),
      MenuItem::separatorItem(),
      item(a, "debug.back", "Back", [this] { debugger_.back(); }, "[", MOD_COMMAND, false,
           settings_.showCpuDebugger && debugger_.canGoBack()),
      item(a, "debug.forward", "Forward", [this] { debugger_.forward(); }, "]", MOD_COMMAND, false,
           settings_.showCpuDebugger && debugger_.canGoForward()),
  };
  items.insert(items.end(), run.begin(), run.end());
#ifndef NDEBUG
  items.push_back(MenuItem::separatorItem());
  items.push_back(item(a, "debug.imguidemo", "Dear ImGui Demo", [this] { showDemo_ = !showDemo_; }, "", 0, showDemo_));
#endif
  return submenu("Debug", items);
}

// The app's windows, added to the system's Window menu: each shows or hides
// its window, and is ticked while it is open.
MenuItem App::windowMenu() {
  auto &a = menuActions_;
  auto window = [&a](const std::string &id, const std::string &title, bool &flag, const std::string &key = "",
                     unsigned modifiers = 0) {
    return item(a, id, title, [&flag] {
      flag = !flag;
      ImGui::MarkIniSettingsDirty();
    }, key, modifiers, flag);
  };
  std::vector<MenuItem> items = {
      window("window.screen", "Screen", settings_.showScreen, "1", MOD_COMMAND),
      window("window.drives", "Disk Drives", settings_.showDiskDrives, "2", MOD_COMMAND),
  };
  if (hardDrives_->available()) {
    items.push_back(window("window.harddrives", "SmartPort Drives", settings_.showHardDrives, "3", MOD_COMMAND));
  }
  items.push_back(window("window.joystick", "Joystick", settings_.showJoystick, "4", MOD_COMMAND));
  // Not on a //c, whose every slot is soldered down.
  if (profile_ && profile_->caps.hasExpansionSlots) {
    items.push_back(window("window.slots", "Expansion Slots", settings_.showExpansionSlots));
  }
  items.push_back(window("window.states", "Save States", settings_.showSaveStates, "s", MOD_COMMAND | MOD_SHIFT));
  items.push_back(window("window.display", "Display Settings", settings_.showDisplaySettings));
  items.push_back(window("window.equalizer", "Equalizer", settings_.showEqualizer));
  items.push_back(MenuItem::separatorItem());
  // Off, every window stays a window: none docks into another or into the
  // main window, and one already docked comes back out.
  items.push_back(window("window.docking", "Window Docking", settings_.windowDocking));
  return submenu(WINDOW_MENU, items);
}

MenuItem App::helpMenu() {
  auto &a = menuActions_;
  return submenu(HELP_MENU, {
      item(a, "help.wiki", "ApplEm Help", [this] {
             if (platform_.openURL) platform_.openURL("https://github.com/mikedaley/web-a2e/wiki");
           }),
  });
}

// The window ⌘W closes: the app window that has the keyboard, if it is one
// that can be shut. The screen cannot, and the main window is not one.
bool *App::focusedToolWindow() {
  const ImGuiWindow *focused = GImGui ? GImGui->NavWindow : nullptr;
  if (!focused || !focused->RootWindow) return nullptr;
  const std::string name = focused->RootWindow->Name;
  const std::pair<const char *, bool *> windows[] = {
      {"Disk Drives", &settings_.showDiskDrives},
      {"SmartPort Drives", &settings_.showHardDrives},
      {"Joystick", &settings_.showJoystick},
      {"Expansion Slots", &settings_.showExpansionSlots},
      {"Save States", &settings_.showSaveStates},
      {"Display Settings", &settings_.showDisplaySettings},
      {"Equalizer", &settings_.showEqualizer},
      {"Mockingboard", &settings_.showMockingboard},
      {"Ensoniq", &settings_.showEnsoniq},
      {"Applesoft BASIC", &settings_.showBasic},
      {"CPU Debugger", &settings_.showCpuDebugger},
      {"Memory Viewer", &settings_.showMemoryViewer},
  };
  for (const auto &[title, flag] : windows) {
    if (name == title) return *flag ? flag : nullptr;
  }
  return nullptr;
}

void App::closeFocusedWindow() {
  if (bool *flag = focusedToolWindow()) {
    *flag = false;
    ImGui::MarkIniSettingsDirty();
  }
}

// The text screen onto the clipboard, as the browser's text selection copies
// it, forty or eighty columns.
void App::copyScreenText() {
  const std::string text = emulation_.withMachine([](host::MachineHost &host) { return host.screenText(); });
  if (!text.empty()) ImGui::SetClipboardText(text.c_str());
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
  // A machine is built NTSC, and retimed if it was left PAL.
  profile_ = &machineProfile(id, standardFor(machineProfile(id)));
  const VideoStandard standard = profile_->timing.standard;
  emulation_.withMachine([&](host::MachineHost &host) { host.setVideoStandard(standard); });
  settings_.machine = profile_->key;
  display_.setMachine(*profile_);
  slots_.setMachine(*profile_);
  slots_.apply();
  restoreBatteryRam();
  applySpeed();
  joystick_.machineRebuilt();
  debugger_.setMachine(*profile_);
  memory_.setMachine(*profile_);
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
      // The mouse: how to take it, or how to give it back.
      if (mouseCaptured_) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::accentText()), "Mouse captured: \u2303\u2325 releases");
        ImGui::SameLine(0, 14);
      } else if (machineHasMouse_ && emulation_.powered()) {
        ImGui::TextColored(secondary, "Click the screen to use the mouse");
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

// Its mutes are the user's, and are kept whenever they change.
void App::applyMockingboardPhaseLock() {
  const bool on = settings_.mockingboardPhaseLock;
  emulation_.withMachine([on](host::MachineHost &) { a2e::MockingboardCard::setPhaseLock(on); });
}

void App::drawMockingboard() {
  firstPosition(420, 80);
  mockingboard_.draw(&settings_.showMockingboard);
  if (mockingboard_.mutes != settings_.mockingboardMutes) {
    settings_.mockingboardMutes = mockingboard_.mutes;
    ImGui::MarkIniSettingsDirty();
  }
}

// Its settings are the user's, kept whenever they change.
void App::drawEqualizer() {
  firstPosition(300, 140);
  equalizer_.draw(&settings_.showEqualizer);
  if (equalizer_.changed()) {
    settings_.equalizer = equalizer_.settings;
    ImGui::MarkIniSettingsDirty();
  }
}

// Its mutes are the user's too.
void App::drawEnsoniq() {
  firstPosition(440, 60);
  ensoniq_.draw(&settings_.showEnsoniq);
  if (ensoniq_.mutes != settings_.ensoniqMutes) {
    settings_.ensoniqMutes = ensoniq_.mutes;
    ImGui::MarkIniSettingsDirty();
  }
}

void App::drawDiskDrives() {
  drawJoystick();
  drawMockingboard();
  drawEnsoniq();
  firstPosition(120, 50);
  basic_.draw(&settings_.showBasic);
  drawEqualizer();
  firstPosition(60, 40);
  debugger_.draw(&settings_.showCpuDebugger);
  firstPosition(100, 70);
  memory_.draw(&settings_.showMemoryViewer);
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

App::DropPlan App::planDrop(const std::vector<std::string> &paths, std::optional<ImVec2> at) const {
  DropPlan plan;
  for (const std::string &path : paths) {
    std::error_code error;
    const size_t size = static_cast<size_t>(std::filesystem::file_size(path, error));
    if (error) continue;
    if (HardDrives::isBlockImage(path, size)) {
      const int device = at ? hardDrives_->deviceAt(*at) : -1;
      return {DropPlan::Kind::SmartPort, device >= 0 ? device : hardDrives_->dropTarget(), path};
    }
    if (DiskDrives::isFloppyImage(path)) {
      const int drive = at ? drives_->driveAt(*at) : -1;
      return {DropPlan::Kind::Floppy, drive >= 0 ? drive : drives_->dropTarget(), path};
    }
  }
  if (!paths.empty()) plan.path = paths.front();
  return plan;
}

void App::dropNotice(const std::string &text, bool error) {
  dropNotice_ = text;
  dropNoticeError_ = error;
  dropNoticeUntil_ = ImGui::GetTime() + 3.0;
}

void App::filesDropped(const std::vector<std::string> &paths, std::optional<ImVec2> at) {
  if (!started_) return;
  const DropPlan plan = planDrop(paths, at);
  const std::string name = std::filesystem::path(plan.path).filename().string();
  const std::string unit = std::to_string(plan.unit + 1);
  switch (plan.kind) {
  case DropPlan::Kind::None:
    if (!name.empty()) dropNotice(name + " is not a disk image", true);
    return;
  case DropPlan::Kind::Floppy:
    drives_->insertFile(plan.unit, plan.path);
    if (drives_->diskName(plan.unit) == name) dropNotice(name + " is in Drive " + unit, false);
    return;
  case DropPlan::Kind::SmartPort:
    // With no SmartPort fitted this says so itself, in a dialog.
    hardDrives_->insertFile(plan.unit, plan.path);
    if (hardDrives_->imageName(plan.unit) == name) dropNotice(name + " is in SmartPort " + unit, false);
    return;
  }
}

bool App::dragHover(std::optional<ImVec2> at, const std::vector<std::string> &paths) {
  drives_->dragOver = at;
  hardDrives_->dragOver = at;
  dragOver_ = at;
  dragPlan_ = at ? planDrop(paths, at) : DropPlan{};
  return dragPlan_.kind != DropPlan::Kind::None;
}

// While files are dragged over the picture: an outline round it and a pill
// saying where the disk would go, as a drive's card shows when dragged over;
// and after a drop, what it did.
void App::drawScreenDrop(ImVec2 min, ImVec2 max) {
  ImDrawList *top = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
  const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
  const ImU32 red = IM_COL32(0xe0, 0x3a, 0x3e, 255);
  auto pill = [&](const std::string &text, ImU32 colour) {
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 middle((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    top->AddRectFilled(ImVec2(middle.x - size.x * 0.5f - 16, middle.y - size.y * 0.5f - 9),
                       ImVec2(middle.x + size.x * 0.5f + 16, middle.y + size.y * 0.5f + 9), colour, 20.0f);
    top->AddText(ImVec2(middle.x - size.x * 0.5f, middle.y - size.y * 0.5f), IM_COL32_WHITE, text.c_str());
    ImGui::PopFont();
  };

  // Under the drag and not covered by another window there.
  const bool over = dragOver_ && dragOver_->x >= min.x && dragOver_->x < max.x && dragOver_->y >= min.y &&
                    dragOver_->y < max.y &&
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                           ImGuiHoveredFlags_AllowWhenBlockedByPopup);
  if (over) {
    const DropPlan &plan = dragPlan_;
    const bool none = plan.kind == DropPlan::Kind::None;
    const ImU32 colour = none ? red : accent;
    const ImVec2 a(min.x + 6, min.y + 6), b(max.x - 6, max.y - 6);
    top->AddRectFilled(a, b, (colour & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 40), 8.0f);
    top->AddRect(a, b, colour, 8.0f, 0, 2.5f);
    const std::string name = std::filesystem::path(plan.path).filename().string();
    const std::string unit = std::to_string(plan.unit + 1);
    std::string prompt;
    switch (plan.kind) {
    case DropPlan::Kind::None: prompt = name + " is not a disk image"; break;
    case DropPlan::Kind::Floppy:
      prompt = (drives_->hasDisk(plan.unit) ? "Drop to replace the disk in Drive " : "Drop to insert into Drive ") + unit;
      break;
    case DropPlan::Kind::SmartPort:
      prompt = !hardDrives_->available() ? "There is no SmartPort to take this image"
               : hardDrives_->hasImage(plan.unit) ? "Drop to replace the image in SmartPort " + unit
                                                  : "Drop to insert into SmartPort " + unit;
      break;
    }
    pill(prompt, colour);
    return;
  }
  if (!dropNotice_.empty() && ImGui::GetTime() < dropNoticeUntil_) {
    pill(dropNotice_, dropNoticeError_ ? red : accent);
  }
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

VideoStandard App::standardFor(const MachineProfile &machine) const {
  auto it = settings_.pal.find(machine.key);
  const bool pal = it != settings_.pal.end() && it->second;
  return pal && machineHasStandard(machine.id, VideoStandard::PAL) ? VideoStandard::PAL : VideoStandard::NTSC;
}

void App::setVideoStandard(VideoStandard standard) {
  if (!profile_ || profile_->timing.standard == standard) return;
  if (!emulation_.withMachine([&](host::MachineHost &host) { return host.setVideoStandard(standard); })) return;
  settings_.pal[profile_->key] = standard == VideoStandard::PAL;
  ImGui::MarkIniSettingsDirty();
  profile_ = &machineProfile(profile_->id, standard);
  debugger_.setMachine(*profile_);
  memory_.setMachine(*profile_);
  emulation_.resetMeasurement();
  dropNotice(standard == VideoStandard::PAL ? "PAL, 50Hz: reboot to start a program afresh"
                                            : "NTSC, 60Hz: reboot to start a program afresh",
             false);
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
  // than starting to drag it, and on a machine with a mouse gives it the
  // mouse.
  ImGui::InvisibleButton("##screen", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 1.0f)));
  const ImVec2 pointer = ImGui::GetIO().MousePos;
  const bool overPicture = ImGui::IsItemHovered() && pointer.x >= p0.x && pointer.x < p1.x && pointer.y >= p0.y &&
                           pointer.y < p1.y;
  // The first click on a window that is not the one in use only selects it,
  // as a click on any Mac window does; the click after that takes the
  // mouse. Whether it was in use is last frame's answer, because by the time
  // the click is seen ImGui may already have focused the window for it.
  if (overPicture && ImGui::IsItemClicked(ImGuiMouseButton_Left) && screenWasFocused_ && machineHasMouse_ &&
      emulation_.powered()) {
    setMouseCaptured(true);
  }
  const bool activeWindow = ImGui::GetWindowViewport()->Flags & ImGuiViewportFlags_IsFocused;
  screenWasFocused_ = activeWindow && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  drawMouseHints(p0, p1, overPicture, screenWasFocused_);
  drawScreenDrop(origin, ImVec2(origin.x + avail.x, origin.y + avail.y));
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
  // A captured mouse is the machine's, resize or not.
  if (!mouseCaptured_) ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
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
  const bool visible = ui::BeginWindow(
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

  // This frame's key changes, posted together and only when there are any:
  // the machine is not waited for, and not touched at all on a frame with
  // nothing to say.
  struct Change {
    bool down;
    CoreKeyEvent event;
  };
  std::vector<Change> changes;
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
        changes.push_back({true, *event});
        keysDown_.insert(k);
      }
    }
    if (ImGui::IsKeyReleased(key) && keysDown_.erase(k)) {
      if (auto event = coreKeyEvent(*hostKey, held, command, true)) {
        changes.push_back({false, *event});
      }
    }
  }
  if (changes.empty()) return;
  emulation_.post([changes = std::move(changes), capsLock](host::MachineHost &host) {
    for (const Change &c : changes) {
      const CoreKeyEvent &e = c.event;
      if (c.down) {
        host.handleRawKeyDown(e.keyCode, e.shift, e.ctrl, e.alt, e.meta, capsLock, e.location);
      } else {
        host.handleRawKeyUp(e.keyCode, e.shift, e.ctrl, e.alt, e.meta, e.location);
      }
    }
  });
}

void App::setMouseCaptured(bool captured) {
  if (captured == mouseCaptured_) return;
  if (captured && !(machineHasMouse_ && emulation_.powered() && platform_.captureMouse)) return;
  mouseCaptured_ = captured;
  if (platform_.captureMouse) platform_.captureMouse(captured);
  ImGuiIO &io = ImGui::GetIO();
  if (captured) {
    // ImGui sees no mouse while the machine has it, and leaves the pointer
    // hidden; the keyboard goes to the screen, where the mouse is.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    if (screenWindowName_) ImGui::SetWindowFocus(screenWindowName_);
    mouseCapturedAt_ = ImGui::GetTime();
    mouseCarryX_ = mouseCarryY_ = 0;
    if (platform_.takeMouseInput) platform_.takeMouseInput(); // anything from before
  } else {
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    if (!liveResize_) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    // A button held as the mouse went back is let go, or the machine would
    // go on dragging.
    if (mouseButtonDown_) {
      mouseButtonDown_ = false;
      emulation_.post([](host::MachineHost &host) { host.mouseButton(false); });
    }
  }
}

// Whether the machine has a mouse, every frame, and while it has the host's,
// the movement and the button handed over: a whole unit at a time, with the
// rest carried, so slow movement is not lost.
void App::updateMouse() {
  emulation_.poll(mousePoll_, [this](host::MachineHost &host) {
    machineHasMouse_ = host.isBuilt() && host.hasMouse();
  });
  if (!mouseCaptured_) return;
  Platform::MouseInput in = platform_.takeMouseInput ? platform_.takeMouseInput() : Platform::MouseInput{};
  if (in.released || !machineHasMouse_ || !emulation_.powered()) {
    setMouseCaptured(false);
    return;
  }
  mouseCarryX_ += in.dx;
  mouseCarryY_ += in.dy;
  const int dx = static_cast<int>(mouseCarryX_);
  const int dy = static_cast<int>(mouseCarryY_);
  mouseCarryX_ -= static_cast<float>(dx);
  mouseCarryY_ -= static_cast<float>(dy);
  if (!in.buttons.empty()) mouseButtonDown_ = in.buttons.back();
  if (dx == 0 && dy == 0 && in.buttons.empty()) return;
  emulation_.post([dx, dy, buttons = std::move(in.buttons)](host::MachineHost &host) {
    if (dx || dy) host.mouseMove(dx, dy);
    for (bool down : buttons) host.mouseButton(down);
  });
}

// How to use the mouse, said where the user is looking: on the picture, when
// the pointer rests on a machine that has a mouse, and how to give it back
// for a few seconds after it is taken. Drawn over the picture, which is dark
// whatever the appearance, so the capsules are dark with white text.
void App::drawMouseHints(ImVec2 p0, ImVec2 p1, bool overPicture, bool focused) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const double now = ImGui::GetTime();
  auto capsule = [&](const char *text, float y, float alpha) {
    if (alpha <= 0.01f) return;
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float icon = size.y;
    const float width = icon + 8 + size.x + 28;
    const float height = size.y + 14;
    const ImVec2 a(std::floor((p0.x + p1.x - width) * 0.5f), y);
    const ImVec2 b(a.x + width, a.y + height);
    draw->AddRectFilled(a, b, IM_COL32(20, 20, 24, static_cast<int>(200 * alpha)), height * 0.5f);
    draw->AddRect(a, b, IM_COL32(255, 255, 255, static_cast<int>(40 * alpha)), height * 0.5f);
    // A mouse, drawn: a rounded body, a button line and the cable's notch.
    const ImU32 ink = IM_COL32(255, 255, 255, static_cast<int>(235 * alpha));
    const ImVec2 m(a.x + 14, a.y + 7);
    const float mw = icon * 0.62f, mh = icon;
    draw->AddRect(m, ImVec2(m.x + mw, m.y + mh), ink, mw * 0.5f, 0, 1.4f);
    draw->AddLine(ImVec2(m.x + mw * 0.5f, m.y + 1.5f), ImVec2(m.x + mw * 0.5f, m.y + mh * 0.38f), ink, 1.4f);
    draw->AddText(ImVec2(m.x + mw + 8, a.y + 7), ink, text);
  };

  if (mouseCaptured_) {
    // Shown for four seconds, then faded over one.
    const float age = static_cast<float>(now - mouseCapturedAt_);
    const float alpha = std::clamp(5.0f - age, 0.0f, 1.0f);
    capsule("Mouse captured. Press \u2303\u2325 (Control-Option) to release it", p0.y + 14, alpha);
    pictureHoveredSince_ = -1.0;
    return;
  }
  if (!overPicture || !machineHasMouse_ || !emulation_.powered()) {
    pictureHoveredSince_ = -1.0;
    return;
  }
  if (pictureHoveredSince_ < 0) pictureHoveredSince_ = now;
  // Fades in after the pointer has rested a moment, so it does not flicker
  // across the picture as the pointer passes over.
  const float alpha = std::clamp(static_cast<float>(now - pictureHoveredSince_ - 0.4) / 0.25f, 0.0f, 1.0f);
  const float height = ImGui::GetTextLineHeight() + 14;
  capsule(focused ? "Click to use the mouse" : "Click to select the screen, then again to use the mouse",
          p1.y - height - 14, alpha);
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
