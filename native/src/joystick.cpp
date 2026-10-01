/*
 * joystick.cpp - The game port: its device, what drives it, and the Joystick window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "joystick.hpp"
#include "ui_controls.hpp"

#include "emulation.hpp"
#include "ui_theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace a2e::native {

namespace {

const ImVec4 LIT(0.38f, 0.73f, 0.27f, 1.0f);
const ImVec4 FIRE(0.88f, 0.23f, 0.24f, 1.0f);

} // namespace

Joystick::Joystick(Emulation &emulation) : emulation_(emulation) {}

void Joystick::machineRebuilt() {
  sentDevice_ = -1;
  sentPaddles_ = {-1, -1};
  sentButtons_ = {-1, -1, -1};
  sentSticks_ = {-1, -1};
}

void Joystick::sendDevice() {
  if (device == sentDevice_) return;
  // Switching devices lets go of whatever the old one held.
  sentPaddles_ = {-1, -1};
  sentButtons_ = {-1, -1, -1};
  sentSticks_ = {-1, -1};
  const GamePortDevice chosen = device == 1 ? GamePortDevice::SiriusJoyport : GamePortDevice::AppleJoystick;
  emulation_.withMachine([&](host::MachineHost &host) { host.setGamePortDevice(chosen); });
  sentDevice_ = device;
}

void Joystick::update(bool screenHasKeyboard) {
  sendDevice();
  pads_ = gamepadEnabled && gamepads_ ? gamepads_() : std::vector<Pad>{};

  const bool keys = cursorKeys && screenHasKeyboard;
  const bool up = keys && ImGui::IsKeyDown(ImGuiKey_UpArrow);
  const bool down = keys && ImGui::IsKeyDown(ImGuiKey_DownArrow);
  const bool left = keys && ImGui::IsKeyDown(ImGuiKey_LeftArrow);
  const bool right = keys && ImGui::IsKeyDown(ImGuiKey_RightArrow);

  if (device == 0) {
    // The stick: the on-screen one while it is held, then the arrows while
    // one is down, then the first gamepad, else centred.
    if (dragging_) {
      paddles_ = {static_cast<int>(std::lround(knobX_ * 255)), static_cast<int>(std::lround(knobY_ * 255))};
    } else if (up || down || left || right) {
      paddles_ = {paddleFromKeys(left, right), paddleFromKeys(up, down)};
    } else if (!pads_.empty()) {
      paddles_ = {paddleFromAxis(pads_[0].axes[0], deadzone), paddleFromAxis(pads_[0].axes[1], deadzone)};
    } else {
      paddles_ = {128, 128};
    }
    if (!dragging_) {
      knobX_ = paddles_[0] / 255.0f;
      knobY_ = paddles_[1] / 255.0f;
    }
    for (int i = 0; i < 3; i++) {
      buttons_[i] = screenButtons_[i] || (i < 2 && !pads_.empty() && pads_[0].buttons[i]);
    }

    emulation_.withMachine([&](host::MachineHost &host) {
      for (int i = 0; i < 2; i++) {
        if (paddles_[i] != sentPaddles_[i]) host.setPaddleValue(i, paddles_[i]);
      }
      for (int i = 0; i < 3; i++) {
        if (static_cast<int>(buttons_[i]) != sentButtons_[i]) host.setButton(i, buttons_[i]);
      }
    });
    sentPaddles_ = paddles_;
    for (int i = 0; i < 3; i++) sentButtons_[i] = buttons_[i];
  } else {
    // Each stick its own pad, or one pad driving both: a one-player game
    // that reads stick 2 then still plays. The arrows are stick 1, keeping
    // whatever fire the pad or the window holds.
    for (int stick = 0; stick < 2; stick++) {
      const Pad *pad = pads_.size() == 1 ? &pads_[0] : (stick < static_cast<int>(pads_.size()) ? &pads_[stick] : nullptr);
      int mask = pad ? joyportMask(*pad, deadzone) : 0;
      if (stick == 0 && (up || down || left || right)) {
        mask = (mask & SWITCH_FIRE) | joyportMaskFromKeys(up, down, left, right);
      }
      if (screenButtons_[stick]) mask |= SWITCH_FIRE;
      sticks_[stick] = mask;
    }
    emulation_.withMachine([&](host::MachineHost &host) {
      for (int stick = 0; stick < 2; stick++) {
        if (sticks_[stick] != sentSticks_[stick]) host.setJoyportStick(stick, sticks_[stick]);
      }
    });
    sentSticks_ = sticks_;
  }
}

void Joystick::drawAppleJoystick() {
  // The stick: drag it; it springs back to the middle when let go.
  const float size = 150.0f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + size, origin.y + size), IM_COL32(22, 27, 34, 255), 6.0f);
  draw->AddLine(ImVec2(origin.x + size / 2, origin.y + 6), ImVec2(origin.x + size / 2, origin.y + size - 6),
                IM_COL32(255, 255, 255, 25));
  draw->AddLine(ImVec2(origin.x + 6, origin.y + size / 2), ImVec2(origin.x + size - 6, origin.y + size / 2),
                IM_COL32(255, 255, 255, 25));
  ImGui::InvisibleButton("##stick", ImVec2(size, size));
  if (ImGui::IsItemActive()) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    knobX_ = std::clamp((mouse.x - origin.x) / size, 0.0f, 1.0f);
    knobY_ = std::clamp((mouse.y - origin.y) / size, 0.0f, 1.0f);
    dragging_ = true;
  } else if (dragging_) {
    dragging_ = false;
    knobX_ = knobY_ = 0.5f;
  }
  const ImVec2 knob(origin.x + knobX_ * size, origin.y + knobY_ * size);
  draw->AddLine(ImVec2(origin.x + size / 2, origin.y + size / 2), knob, IM_COL32(255, 255, 255, 60), 3.0f);
  draw->AddCircleFilled(knob, 11.0f, IM_COL32(0, 157, 220, 255));
  draw->AddCircle(knob, 11.0f, IM_COL32(255, 255, 255, 90), 0, 1.5f);

  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted("Paddle 0");
  ImGui::SameLine(80);
  ImGui::PushFont(ui::monoFont(), 0.0f);
  ImGui::Text("%3d", paddles_[0]);
  ImGui::PopFont();
  ImGui::TextUnformatted("Paddle 1");
  ImGui::SameLine(80);
  ImGui::PushFont(ui::monoFont(), 0.0f);
  ImGui::Text("%3d", paddles_[1]);
  ImGui::PopFont();
  ImGui::Spacing();
  // Held while the mouse is down on them, as a real button is.
  for (int i = 0; i < 3; i++) {
    ImGui::PushID(i);
    if (buttons_[i]) ImGui::PushStyleColor(ImGuiCol_Button, LIT);
    char label[16];
    std::snprintf(label, sizeof(label), "Button %d", i);
    ui::Button(label, ImVec2(100, 0));
    screenButtons_[i] = ImGui::IsItemActive();
    if (buttons_[i]) ImGui::PopStyleColor();
    ImGui::PopID();
  }
  ImGui::EndGroup();
}

void Joystick::drawJoyport() {
  // Each stick's five switches, lit while closed.
  for (int stick = 0; stick < 2; stick++) {
    ImGui::PushID(stick);
    ImGui::BeginGroup();
    ImGui::Text("Stick %d", stick + 1);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const int mask = sticks_[stick];
    auto lamp = [&](float x, float y, bool lit, const ImVec4 &colour) {
      draw->AddCircleFilled(ImVec2(origin.x + x, origin.y + y), 9.0f,
                            lit ? ImGui::GetColorU32(colour) : IM_COL32(50, 50, 55, 255));
    };
    lamp(45, 12, mask & SWITCH_UP, LIT);
    lamp(45, 68, mask & SWITCH_DOWN, LIT);
    lamp(17, 40, mask & SWITCH_LEFT, LIT);
    lamp(73, 40, mask & SWITCH_RIGHT, LIT);
    lamp(110, 40, mask & SWITCH_FIRE, FIRE);
    ImGui::Dummy(ImVec2(125, 82));
    ui::Button("Fire", ImVec2(110, 0));
    screenButtons_[stick] = ImGui::IsItemActive();
    ImGui::EndGroup();
    ImGui::PopID();
    if (stick == 0) ImGui::SameLine(0, 30);
  }
  screenButtons_[2] = false;
}

void Joystick::draw(bool *open) {
  if (!open || !*open) {
    screenButtons_ = {};
    dragging_ = false;
    return;
  }
  if (ImGui::Begin("Joystick", open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Game port device");
    ui::SegmentedControl("##device", &device, {"Apple Joystick", "Sirius Joyport"}, 300.0f);
    ImGui::Separator();

    if (device == 0) drawAppleJoystick();
    else drawJoyport();

    ImGui::Separator();
    ui::Switch("Cursor keys drive the joystick", &cursorKeys);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The arrows still reach the machine's keyboard as well.");
    }
    ui::Switch("Use gamepads", &gamepadEnabled);
    ImGui::BeginDisabled(!gamepadEnabled);
    ImGui::SetNextItemWidth(160);
    ui::SliderFloat("Deadzone", &deadzone, 0.0f, MAX_DEADZONE, "%.2f");
    ImGui::EndDisabled();
    if (gamepadEnabled) {
      if (pads_.empty()) {
        ImGui::TextDisabled("No gamepad connected.");
      } else {
        ImGui::TextDisabled("%zu gamepad%s connected.", pads_.size(), pads_.size() == 1 ? "" : "s");
      }
    }
  }
  ImGui::End();
}

} // namespace a2e::native
