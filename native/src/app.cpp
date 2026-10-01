/*
 * app.cpp - The native front end's user interface
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "app.hpp"

#include "no_signal_frame.hpp"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder, the status bar's viewport side bar

#include "video/video.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

bool isModifier(int keyCode) {
  return keyCode == KEY_SHIFT || keyCode == KEY_CONTROL || keyCode == KEY_ALT ||
         keyCode == KEY_META_LEFT || keyCode == KEY_META_RIGHT;
}

} // namespace

App::App(std::string settingsDirectory, Platform platform)
    : settingsDirectory_(std::move(settingsDirectory)),
      iniPath_(settingsDirectory_ + "/layout.ini"),
      platform_(std::move(platform)),
      display_(settingsDirectory_ + "/display-profiles.ini") {
  registerSettingsHandler();
  registerDisplayHandler();
}

App::~App() { shutdown(); }

void App::shutdown() {
  if (!started_) return;
  releaseKeys();
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
    Settings &s = static_cast<App *>(h->UserData)->settings_;
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
    else if (std::sscanf(line, "ShowStatusBar=%d", &value) == 1) s.showStatusBar = value;
    else if (std::sscanf(line, "CommandIsOpenApple.%63[^=]=%d", text, &value) == 2) {
      s.commandIsOpenApple[text] = value;
    }
  };
  handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h,
                          ImGuiTextBuffer *out) {
    const Settings &s = static_cast<const App *>(h->UserData)->settings_;
    out->appendf("[%s][Settings]\n", h->TypeName);
    out->appendf("Machine=%s\n", s.machine.c_str());
    out->appendf("IIgsMemoryKB=%d\n", s.iigsMemoryKB);
    out->appendf("Volume=%.3f\n", s.volume);
    out->appendf("Muted=%d\n", s.muted ? 1 : 0);
    out->appendf("ShowScreen=%d\n", s.showScreen ? 1 : 0);
    out->appendf("ShowDisplaySettings=%d\n", s.showDisplaySettings ? 1 : 0);
    out->appendf("UKCharacterSet=%d\n", s.ukCharacterSet ? 1 : 0);
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
  emulation_.setPowered(true);
  started_ = true;
  updateWindowTitle();
}

void App::frame() {
  if (!started_) startEmulation();

  updateScreenSource();

  // The decoder and character set live in the machine's video, which a
  // rebuild replaces, so they are told again whenever they may have gone.
  if (display_.takeMachineChange()) applyMachineDisplay();
  display_.applyToRenderer(*platform_.screen);

  // Ctrl+Escape leaves Full Page, as in the browser.
  if (fullPage_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
      heldModifiers(ImGui::GetIO().ConfigMacOSXBehaviors).control) {
    fullPage_ = false;
  }

  screenWindowName_ = nullptr;
  if (fullPage_) {
    drawFullPage();
    if (settings_.showDisplaySettings) display_.drawWindow(&settings_.showDisplaySettings);
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
    drawSwitchConfirmation();
    handleAppShortcuts();
    routeKeyboard();
    return;
  }

  drawMenuBar();
  if (settings_.showStatusBar) drawStatusBar();
  drawDockSpace();
  if (settings_.showScreen) drawScreenWindow();
  if (settings_.showDisplaySettings) display_.drawWindow(&settings_.showDisplaySettings);
  if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
  drawSwitchConfirmation();

  handleAppShortcuts();
  routeKeyboard();
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

void App::drawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Quit", "Cmd+Q")) quitRequested_ = true;
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Paste to Machine", commandIsOpenApple() ? nullptr : "Cmd+V")) {
      paste();
    }
    ImGui::EndMenu();
  }

  drawMachineMenu();

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem(SCREEN_WINDOW, nullptr, &settings_.showScreen);
    ImGui::MenuItem("Display Settings", nullptr, &settings_.showDisplaySettings);
    ImGui::MenuItem("Status Bar", nullptr, &settings_.showStatusBar);
    ImGui::Separator();
    if (ImGui::MenuItem("Full Page", "Ctrl+Esc to leave")) {
      fullPage_ = true;
      enterFullPage_ = true;
    }
    if (ImGui::MenuItem("Full Screen") && platform_.toggleFullScreen) platform_.toggleFullScreen();
    ImGui::Separator();
    if (profile_ && profile_->caps.hasUkCharSet &&
        ImGui::MenuItem("UK Character Set", nullptr, &settings_.ukCharacterSet)) {
      applyMachineDisplay();
      ImGui::MarkIniSettingsDirty();
    }
    bool command = commandIsOpenApple();
    if (ImGui::MenuItem("Cmd as Open Apple", nullptr, &command)) {
      releaseKeys();
      settings_.commandIsOpenApple[profile_->key] = command;
      ImGui::MarkIniSettingsDirty();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Mute", nullptr, &settings_.muted)) {
      emulation_.setMuted(settings_.muted);
      ImGui::MarkIniSettingsDirty();
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Volume", &settings_.volume, 0.0f, 1.0f, "%.2f")) {
      emulation_.setVolume(settings_.volume);
      ImGui::MarkIniSettingsDirty();
    }
    ImGui::Separator();
    ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}

void App::drawMachineMenu() {
  if (!ImGui::BeginMenu("Machine")) return;

  bool powered = emulation_.powered();
  if (ImGui::MenuItem("Power", nullptr, &powered)) {
    releaseKeys();
    emulation_.setPowered(powered);
  }
  if (ImGui::MenuItem("Ctrl+Reset", "Ctrl+F12", false, powered)) {
    emulation_.withMachine([](host::MachineHost &host) { host.warmReset(); });
  }
  if (ImGui::MenuItem("Reboot", nullptr, false, powered)) {
    emulation_.withMachine([](host::MachineHost &host) { host.reset(); });
  }

  ImGui::Separator();
  for (int i = 0; i < MACHINE_COUNT; i++) {
    const MachineProfile &machine = machineProfileAt(i);
    const bool runnable = Emulator::isMachineRunnable(machine.id);
    const bool current = profile_ && machine.id == profile_->id;
    std::string label = machine.name;
    if (!runnable) label += " (ROM missing)";
    if (ImGui::MenuItem(label.c_str(), nullptr, current, runnable) && !current) {
      pendingMachine_ = machine.id;
    }
  }

  ImGui::Separator();
  if (ImGui::BeginMenu("IIgs Memory")) {
    for (const MemorySize &size : IIGS_MEMORY_SIZES) {
      const bool current = settings_.iigsMemoryKB == size.kb;
      if (ImGui::MenuItem(size.label, nullptr, current) && !current) {
        // Changing it rebuilds a running IIgs, as switching machines does,
        // and is only remembered by any other machine.
        settings_.iigsMemoryKB = size.kb;
        ImGui::MarkIniSettingsDirty();
        releaseKeys();
        emulation_.setIIgsFastRam(static_cast<size_t>(size.kb) * 1024);
        display_.machineRebuilt();
      }
    }
    ImGui::EndMenu();
  }

  ImGui::EndMenu();
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
  if (ImGui::Button("Switch", ImVec2(120, 0))) {
    switchMachine(*pendingMachine_);
    pendingMachine_.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    pendingMachine_.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::switchMachine(MachineId id) {
  releaseKeys();
  if (!emulation_.setMachine(id)) return;
  profile_ = &machineProfile(id);
  settings_.machine = profile_->key;
  display_.setMachine(*profile_);
  noSignalStale_ = true;
  ImGui::MarkIniSettingsDirty();
  // The new machine starts as if switched on, as the old one was.
  if (emulation_.powered()) {
    emulation_.withMachine([](host::MachineHost &host) { host.reset(); });
  }
  updateWindowTitle();
}

void App::updateWindowTitle() {
  if (platform_.setWindowTitle && profile_) {
    platform_.setWindowTitle(std::string("ApplEm · ") + profile_->name);
  }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void App::drawDockSpace() {
  const ImGuiID dockspace = ImGui::GetID(DOCKSPACE_ID);
  ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(),
                               ImGuiDockNodeFlags_PassthruCentralNode);

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

void App::drawStatusBar() {
  const float height = ImGui::GetFrameHeight();
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_MenuBar;
  if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(),
                                  ImGuiDir_Down, height, flags)) {
    if (ImGui::BeginMenuBar()) {
      ImGui::TextUnformatted(profile_ ? profile_->name : "");
      ImGui::Separator();
      if (emulation_.powered()) {
        ImGui::Text("%.3f MHz", emulation_.measuredMHz());
      } else {
        ImGui::TextDisabled("Off");
      }
      ImGui::Separator();
      if (emulation_.audioRunning()) {
        ImGui::TextUnformatted(settings_.muted ? "Muted" : "Audio");
      } else {
        ImGui::TextDisabled("No audio device: free-running");
      }
      if (screenHadKeyboard_) {
        ImGui::Separator();
        ImGui::TextUnformatted(commandIsOpenApple() ? "Keyboard: Cmd is Open Apple"
                                                    : "Keyboard: Option is Open Apple");
      }
      ImGui::EndMenuBar();
    }
  }
  ImGui::End();
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

  // Takes the clicks, so a click on the picture focuses the window rather
  // than starting to drag it.
  ImGui::InvisibleButton("##screen", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 1.0f)));
}

void App::drawScreenWindow() {
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

// The app's own keys. These are checked before the machine is given the
// keyboard and are never passed on to it.
void App::handleAppShortcuts() {
  if (!started_ || !emulation_.powered()) return;
  const bool swap = ImGui::GetIO().ConfigMacOSXBehaviors;
  const HeldModifiers held = heldModifiers(swap);

  // A Mac has no Reset key; Ctrl+F12 is Ctrl+Reset.
  if (held.control && ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
    emulation_.withMachine([](host::MachineHost &host) { host.warmReset(); });
  }
  // Cmd+V pastes, unless Cmd is the machine's Open Apple key.
  if (screenHadKeyboard_ && held.command && !commandIsOpenApple() &&
      ImGui::IsKeyPressed(ImGuiKey_V, false)) {
    paste();
  }
}

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
