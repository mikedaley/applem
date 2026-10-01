/*
 * app.cpp - The native front end's user interface
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "app.hpp"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder, the status bar's viewport side bar

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace a2e::native {

namespace {

constexpr const char *SCREEN_WINDOW = "Screen";
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
      platform_(std::move(platform)) {
  registerSettingsHandler();
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
    else if (std::sscanf(line, "SharpPixels=%d", &value) == 1) s.sharpPixels = value;
    else if (std::sscanf(line, "ShowScreen=%d", &value) == 1) s.showScreen = value;
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
    out->appendf("SharpPixels=%d\n", s.sharpPixels ? 1 : 0);
    out->appendf("ShowScreen=%d\n", s.showScreen ? 1 : 0);
    out->appendf("ShowStatusBar=%d\n", s.showStatusBar ? 1 : 0);
    for (const auto &[key, on] : s.commandIsOpenApple) {
      out->appendf("CommandIsOpenApple.%s=%d\n", key.c_str(), on ? 1 : 0);
    }
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
  profile_ = wanted;

  emulation_.setVolume(settings_.volume);
  emulation_.setMuted(settings_.muted);
  emulation_.start(wanted->id, static_cast<size_t>(settings_.iigsMemoryKB) * 1024);
  emulation_.setPowered(true);
  started_ = true;
  updateWindowTitle();
}

void App::frame() {
  if (!started_) startEmulation();

  // Drain the queue every frame, shown or not, so the emulation thread never
  // finds it full.
  if (const FrameQueue::Frame *frame = emulation_.takeFrame()) {
    platform_.screen->upload(frame->pixels.data(), frame->width, frame->height);
  }

  drawMenuBar();
  if (settings_.showStatusBar) drawStatusBar();
  drawDockSpace();
  if (settings_.showScreen) drawScreenWindow();
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
    ImGui::MenuItem("Status Bar", nullptr, &settings_.showStatusBar);
    ImGui::Separator();
    if (ImGui::MenuItem("Sharp Pixels", nullptr, settings_.sharpPixels)) {
      settings_.sharpPixels = !settings_.sharpPixels;
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

// The picture at the shape the machine's monitor shows it (the profile's
// aspect), as large as the window allows, centred on black.
void App::drawScreenWindow() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const bool visible = ImGui::Begin(
      SCREEN_WINDOW, &settings_.showScreen,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (!visible) {
    ImGui::End();
    return;
  }

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
  const ImVec2 p0(origin.x + (avail.x - width) * 0.5f,
                  origin.y + (avail.y - height) * 0.5f);
  const ImVec2 p1(p0.x + width, p0.y + height);

  const ImTextureID texture = platform_.screen->texture();
  if (emulation_.powered() && texture != ImTextureID_Invalid) {
    const ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
    const bool sharp = settings_.sharpPixels && pio.DrawCallback_SetSamplerNearest;
    if (sharp) draw->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
    draw->AddImage(ImTextureRef(texture), p0, p1);
    if (sharp) draw->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
  } else {
    const char *text = "No signal";
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(origin.x + (avail.x - size.x) * 0.5f,
                         origin.y + (avail.y - size.y) * 0.5f),
                  IM_COL32(110, 110, 110, 255), text);
  }

  // Takes the clicks, so a click on the picture focuses the window rather
  // than starting to drag it.
  ImGui::InvisibleButton("##screen", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 1.0f)));
  ImGui::End();
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
  ImGuiWindow *screen = ImGui::FindWindowByName(SCREEN_WINDOW);
  const ImGuiContext &g = *ImGui::GetCurrentContext();
  const bool focused = screen && g.NavWindow == screen;
  const bool keyboard = started_ && emulation_.powered() && settings_.showScreen &&
                        focused && !ImGui::GetIO().WantTextInput;

  if (!keyboard) {
    if (screenHadKeyboard_) releaseKeys();
    screenHadKeyboard_ = false;
    return;
  }
  screenHadKeyboard_ = true;

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
