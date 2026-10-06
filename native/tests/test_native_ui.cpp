/*
 * test_native_ui.cpp - The app's own window chrome, driven frame by frame
 *
 * Two windows side by side, drawn with BeginWindow and ClickToFocus as the
 * app draws them, and the mouse moved and clicked through ImGui's input
 * queue. No renderer: the frames are built and thrown away.
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "ui/ui_controls.hpp"

using namespace a2e::native;

namespace {

// Where the two windows sit, and the centre of a traffic light: AppKit's
// measures, which BeginWindow draws to (the first light 18 points in, then
// every 20, in a title bar 28 tall).
constexpr ImVec2 LEFT(20, 20), RIGHT(300, 20), SIZE(220, 160);
ImVec2 light(ImVec2 window, int which) { return ImVec2(window.x + 18 + 20 * which, window.y + 14); }
constexpr int CLOSE = 0, MINIMISE = 1;

struct Fixture {
  bool leftOpen = true, rightOpen = true;
  int rightButtonPresses = 0;

  Fixture() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    unsigned char *pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    away();
    frame();
    frame();
  }
  ~Fixture() { ImGui::DestroyContext(); }

  void frame() {
    ImGuiIO &io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ui::ClickToFocus();
    ImGui::SetNextWindowPos(LEFT, ImGuiCond_Always);
    ImGui::SetNextWindowSize(SIZE, ImGuiCond_Always);
    if (leftOpen) {
      ui::BeginWindow("Left", &leftOpen);
      ImGui::End();
    }
    ImGui::SetNextWindowPos(RIGHT, ImGuiCond_Always);
    ImGui::SetNextWindowSize(SIZE, ImGuiCond_Always);
    if (rightOpen) {
      ui::BeginWindow("Right", &rightOpen);
      if (ImGui::Button("Press", ImVec2(120, 40))) rightButtonPresses++;
      ImGui::End();
    }
    ImGui::Render();
  }

  void away() { ImGui::GetIO().AddMousePosEvent(700, 500); }
  // Over a point for a frame, then a click there: down for a frame, up for
  // another, and a frame after.
  void click(ImVec2 at) {
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    frame();
  }
  ImGuiWindow *focused() const {
    ImGuiContext &g = *GImGui;
    return g.NavWindow ? g.NavWindow->RootWindow : nullptr;
  }
  ImGuiWindow *window(const char *name) const { return ImGui::FindWindowByName(name); }
  // With the left window in front and focused, the right one behind it.
  void focusLeft() {
    click(ImVec2(LEFT.x + 100, LEFT.y + 120));
    away();
    frame();
    REQUIRE(focused() == window("Left"));
  }
};

} // namespace

TEST_CASE("A window behind closes from its close button in one click", "[ui]") {
  Fixture f;
  f.focusLeft();
  f.click(light(RIGHT, CLOSE));
  REQUIRE_FALSE(f.rightOpen);
  REQUIRE(f.leftOpen);
}

TEST_CASE("A window behind minimises from its minimise button in one click", "[ui]") {
  Fixture f;
  f.focusLeft();
  f.click(light(RIGHT, MINIMISE));
  REQUIRE(f.window("Right")->Collapsed);
}

TEST_CASE("A click on a control in a window behind still only focuses it", "[ui]") {
  Fixture f;
  f.focusLeft();
  // The button sits at the top left of the right window's contents.
  const ImGuiWindow *right = f.window("Right");
  const ImVec2 button(right->DC.CursorStartPos.x + 20, right->DC.CursorStartPos.y + 10);
  f.click(button);
  REQUIRE(f.rightButtonPresses == 0);
  REQUIRE(f.focused() == right);
  f.click(button);
  REQUIRE(f.rightButtonPresses == 1);
}
