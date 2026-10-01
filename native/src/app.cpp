/*
 * app.cpp - The native front end's user interface
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "app.hpp"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder, for the first-run layout

#include <cstdio>
#include <cstring>

namespace a2e::native {

namespace {

constexpr const char *SCREEN_WINDOW = "Screen";
constexpr const char *DOCKSPACE_ID = "ApplEmDockSpace";

} // namespace

App::App(std::string settingsDirectory)
    : settingsDirectory_(std::move(settingsDirectory)),
      iniPath_(settingsDirectory_ + "/layout.ini") {
  registerSettingsHandler();
}

App::~App() = default;

// Which windows are open is kept in the same file as where they are, so a
// layout and the windows in it come back together.
void App::registerSettingsHandler() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "ApplEm";
  handler.TypeHash = ImHashStr("ApplEm");
  handler.UserData = this;
  handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *,
                          const char *name) -> void * {
    return std::strcmp(name, "Windows") == 0 ? reinterpret_cast<void *>(1)
                                             : nullptr;
  };
  handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *h, void *,
                          const char *line) {
    App *app = static_cast<App *>(h->UserData);
    int value = 0;
    if (std::sscanf(line, "Screen=%d", &value) == 1) app->showScreen_ = value;
  };
  handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h,
                          ImGuiTextBuffer *out) {
    const App *app = static_cast<const App *>(h->UserData);
    out->appendf("[%s][Windows]\n", h->TypeName);
    out->appendf("Screen=%d\n", app->showScreen_ ? 1 : 0);
    out->append("\n");
  };
  ImGui::AddSettingsHandler(&handler);
}

void App::frame() {
  drawMenuBar();
  drawDockSpace();
  if (showScreen_) drawScreenWindow();
  if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
}

void App::drawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Quit", "Cmd+Q")) quitRequested_ = true;
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem(SCREEN_WINDOW, nullptr, &showScreen_);
    ImGui::Separator();
    ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}

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

void App::drawScreenWindow() {
  ImGui::Begin(SCREEN_WINDOW, &showScreen_);
  ImGui::TextDisabled("No machine yet.");
  ImGui::End();
}

} // namespace a2e::native
