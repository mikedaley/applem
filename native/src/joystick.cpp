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

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace a2e::native {

namespace {

// The window, in points.
constexpr float WIDTH = 580;
constexpr float STICK_SIZE = 250;
constexpr float CARD_ROUNDING = 10;
constexpr double TRAIL_SECONDS = 0.45;

// A paddle is read by timing a one-shot: the firmware's PREAD loop counts in
// steps of eleven cycles of the 1.023MHz clock until the timer runs out.
constexpr double PREAD_CYCLES_PER_COUNT = 11.0;
constexpr double CLOCK_MHZ = 1.023;

constexpr ImU32 GREEN = IM_COL32(97, 187, 70, 255);
constexpr ImU32 RED = IM_COL32(224, 58, 62, 255);

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

void card(ImDrawList *draw, ImVec2 a, ImVec2 b) {
  draw->AddRectFilled(a, b, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

void glow(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour) {
  for (int i = 5; i >= 1; i--) draw->AddCircleFilled(centre, radius + i * 2.5f, withAlpha(colour, 0.08f));
}

// A ball top: a shadow under it, the ball, and a highlight where the light is.
void ball(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour, ImU32 shine) {
  draw->AddCircleFilled(ImVec2(centre.x + radius * 0.18f, centre.y + radius * 0.3f), radius * 1.05f, IM_COL32(0, 0, 0, 70));
  draw->AddCircleFilled(centre, radius, colour);
  draw->AddCircleFilled(ImVec2(centre.x - radius * 0.12f, centre.y - radius * 0.12f), radius * 0.78f,
                        withAlpha(shine, 0.18f));
  draw->AddCircleFilled(ImVec2(centre.x - radius * 0.35f, centre.y - radius * 0.38f), radius * 0.28f,
                        withAlpha(shine, 0.55f));
}

void centredText(ImDrawList *draw, ImVec2 centre, ImU32 colour, const char *line) {
  const ImVec2 size = ImGui::CalcTextSize(line);
  draw->AddText(ImVec2(std::floor(centre.x - size.x * 0.5f), std::floor(centre.y - size.y * 0.5f)), colour, line);
}

// Small capitals over a reading, as a front panel labels its dials.
void caption(ImDrawList *draw, ImVec2 at, const char *label, ImU32 colour) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
  draw->AddText(at, colour, label);
  ImGui::PopFont();
}

const char *sourceName(int source) {
  static const char *const names[] = {"Centred", "Mouse", "Cursor keys", "Gamepad"};
  return names[source];
}

// A row of capsules naming what can drive the stick, the one that is lit.
void sources(ImDrawList *draw, ImVec2 at, int active) {
  float x = at.x;
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.85f);
  for (int s = 1; s <= 3; s++) {
    const ImVec2 size = ImGui::CalcTextSize(sourceName(s));
    const ImVec2 end(x + size.x + 16, at.y + size.y + 6);
    const bool lit = s == active;
    draw->AddRectFilled(ImVec2(x, at.y), end, lit ? accent() : text(0.07f), (end.y - at.y) * 0.5f);
    draw->AddText(ImVec2(x + 8, at.y + 3), lit ? IM_COL32_WHITE : secondary(), sourceName(s));
    x = end.x + 6;
  }
  ImGui::PopFont();
}

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
  const bool arrows = up || down || left || right;

  if (device == 0) {
    // The stick: the on-screen one while it is held, then the arrows while
    // one is down, then the first gamepad, else centred.
    if (dragging_) {
      paddles_ = {static_cast<int>(std::lround(knobX_ * 255)), static_cast<int>(std::lround(knobY_ * 255))};
      source_ = Source::Mouse;
    } else if (arrows) {
      paddles_ = {paddleFromKeys(left, right), paddleFromKeys(up, down)};
      source_ = Source::Keys;
    } else if (!pads_.empty()) {
      paddles_ = {paddleFromAxis(pads_[0].axes[0], deadzone), paddleFromAxis(pads_[0].axes[1], deadzone)};
      source_ = Source::Gamepad;
    } else {
      paddles_ = {128, 128};
      source_ = Source::None;
    }
    if (!dragging_) {
      knobX_ = paddles_[0] / 255.0f;
      knobY_ = paddles_[1] / 255.0f;
    }
    for (int i = 0; i < 3; i++) {
      buttons_[i] = screenButtons_[i] || (i < 2 && !pads_.empty() && pads_[0].buttons[i]);
    }

    // Only what changed, and posted: the UI does not wait for the machine.
    bool changed = false;
    for (int i = 0; i < 2; i++) changed |= paddles_[i] != sentPaddles_[i];
    for (int i = 0; i < 3; i++) changed |= static_cast<int>(buttons_[i]) != sentButtons_[i];
    if (changed) {
      emulation_.post([paddles = paddles_, sentPaddles = sentPaddles_, buttons = buttons_,
                       sentButtons = sentButtons_](host::MachineHost &host) {
        for (int i = 0; i < 2; i++) {
          if (paddles[i] != sentPaddles[i]) host.setPaddleValue(i, paddles[i]);
        }
        for (int i = 0; i < 3; i++) {
          if (static_cast<int>(buttons[i]) != sentButtons[i]) host.setButton(i, buttons[i]);
        }
      });
    }
    sentPaddles_ = paddles_;
    for (int i = 0; i < 3; i++) sentButtons_[i] = buttons_[i];
  } else {
    // Each stick its own pad, or one pad driving both: a one-player game
    // that reads stick 2 then still plays. The arrows are stick 1, and a
    // stick pushed with the mouse overrides both, each keeping whatever fire
    // the others hold.
    for (int stick = 0; stick < 2; stick++) {
      const Pad *pad = pads_.size() == 1 ? &pads_[0] : (stick < static_cast<int>(pads_.size()) ? &pads_[stick] : nullptr);
      int mask = pad ? joyportMask(*pad, deadzone) : 0;
      stickSources_[stick] = pad && mask ? Source::Gamepad : Source::None;
      if (stick == 0 && arrows) {
        mask = (mask & SWITCH_FIRE) | joyportMaskFromKeys(up, down, left, right);
        stickSources_[stick] = Source::Keys;
      }
      if (screenSticks_[stick]) {
        mask = (mask & SWITCH_FIRE) | screenSticks_[stick];
        stickSources_[stick] = Source::Mouse;
      }
      if (screenButtons_[stick]) mask |= SWITCH_FIRE;
      sticks_[stick] = mask;
    }
    if (sticks_ != sentSticks_) {
      emulation_.post([sticks = sticks_, sent = sentSticks_](host::MachineHost &host) {
        for (int stick = 0; stick < 2; stick++) {
          if (sticks[stick] != sent[stick]) host.setJoyportStick(stick, sticks[stick]);
        }
      });
    }
    sentSticks_ = sticks_;
  }
}

// An Apple joystick, from above: a beige case, a recessed gate as square as
// the paddles' 0 to 255, and a ball-top stick on its shaft. Drag it; it
// springs back to the middle when let go, and leaves a trail where it went.
void Joystick::drawAppleJoystick() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 end(origin.x + STICK_SIZE, origin.y + STICK_SIZE);
  const double now = ImGui::GetTime();

  // The case.
  draw->AddRectFilled(ImVec2(origin.x, origin.y + 4), ImVec2(end.x, end.y + 4), IM_COL32(0, 0, 0, 50), 22.0f);
  draw->AddRectFilled(origin, end, IM_COL32(0xd8, 0xd0, 0xbe, 255), 22.0f);
  draw->AddRectFilledMultiColor(ImVec2(origin.x + 6, origin.y + 6), ImVec2(end.x - 6, origin.y + STICK_SIZE * 0.5f),
                                IM_COL32(255, 255, 255, 36), IM_COL32(255, 255, 255, 36), IM_COL32(255, 255, 255, 0),
                                IM_COL32(255, 255, 255, 0));
  draw->AddRect(origin, end, IM_COL32(0x8c, 0x84, 0x72, 255), 22.0f, 0, 1.2f);
  // A screw in each corner.
  for (int corner = 0; corner < 4; corner++) {
    const ImVec2 screw(corner & 1 ? end.x - 16 : origin.x + 16, corner & 2 ? end.y - 16 : origin.y + 16);
    draw->AddCircleFilled(screw, 4.0f, IM_COL32(0xb4, 0xab, 0x96, 255));
    draw->AddLine(ImVec2(screw.x - 2.5f, screw.y - 2.5f), ImVec2(screw.x + 2.5f, screw.y + 2.5f), IM_COL32(0x7e, 0x76, 0x64, 255), 1.2f);
  }

  // The gate: the square the stick can reach, recessed.
  const float inset = 34;
  const ImVec2 gate(origin.x + inset, origin.y + inset);
  const ImVec2 gateEnd(end.x - inset, end.y - inset);
  const float span = gateEnd.x - gate.x;
  draw->AddRectFilled(ImVec2(gate.x - 3, gate.y - 3), ImVec2(gateEnd.x + 3, gateEnd.y + 3), IM_COL32(0x9e, 0x95, 0x80, 255), 16.0f);
  draw->AddRectFilled(gate, gateEnd, IM_COL32(0x22, 0x20, 0x1c, 255), 14.0f);
  for (int i = 1; i < 4; i++) {
    const float t = i / 4.0f;
    const ImU32 line = i == 2 ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 14);
    draw->AddLine(ImVec2(gate.x + span * t, gate.y + 6), ImVec2(gate.x + span * t, gateEnd.y - 6), line);
    draw->AddLine(ImVec2(gate.x + 6, gate.y + span * t), ImVec2(gateEnd.x - 6, gate.y + span * t), line);
  }
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.72f);
  draw->AddText(ImVec2(gate.x + 6, gateEnd.y - 16), IM_COL32(255, 255, 255, 70), "0");
  const ImVec2 maxSize = ImGui::CalcTextSize("255");
  draw->AddText(ImVec2(gateEnd.x - maxSize.x - 6, gateEnd.y - 16), IM_COL32(255, 255, 255, 70), "255");
  ImGui::PopFont();

  // Taking the mouse, over the whole case.
  ImGui::InvisibleButton("##stick", ImVec2(STICK_SIZE, STICK_SIZE));
  if (ImGui::IsItemActive()) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    knobX_ = std::clamp((mouse.x - gate.x) / span, 0.0f, 1.0f);
    knobY_ = std::clamp((mouse.y - gate.y) / span, 0.0f, 1.0f);
    dragging_ = true;
  } else if (dragging_) {
    dragging_ = false;
    knobX_ = knobY_ = 0.5f;
  }
  if (ImGui::IsItemHovered() && !dragging_) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

  // The trail, fading behind the knob.
  const ImVec2 knob(gate.x + knobX_ * span, gate.y + knobY_ * span);
  if (trail_.empty() || std::hypot(trail_.back().first.x - knob.x, trail_.back().first.y - knob.y) > 1.0f) {
    trail_.emplace_back(knob, now);
  }
  while (!trail_.empty() && now - trail_.front().second > TRAIL_SECONDS) trail_.pop_front();
  for (size_t i = 1; i < trail_.size(); i++) {
    const float age = static_cast<float>((now - trail_[i].second) / TRAIL_SECONDS);
    draw->AddLine(trail_[i - 1].first, trail_[i].first, withAlpha(accent(), (1.0f - age) * 0.8f), 3.0f * (1.0f - age) + 1.0f);
  }

  // The stick: its boot round the base, the shaft, and the ball on top.
  const ImVec2 centre(gate.x + span * 0.5f, gate.y + span * 0.5f);
  draw->AddCircleFilled(centre, 26.0f, IM_COL32(0x15, 0x14, 0x12, 255));
  draw->AddCircle(centre, 26.0f, IM_COL32(255, 255, 255, 25), 0, 1.0f);
  draw->AddLine(centre, knob, IM_COL32(0x10, 0x10, 0x10, 255), 9.0f);
  draw->AddLine(centre, knob, IM_COL32(0x5a, 0x5a, 0x5e, 255), 3.0f);
  ball(draw, knob, 17.0f, IM_COL32(0x1c, 0x1c, 0x1e, 255), IM_COL32_WHITE);
}

// A CX40, from above: a black case, the red button at its top left, and the
// stick leaning toward whichever switches are closed. Push the stick with
// the mouse to close them; press the button to fire.
void Joystick::drawCX40(int stick, ImVec2 origin, float size) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 end(origin.x + size, origin.y + size);
  const int mask = sticks_[stick];

  draw->AddRectFilled(ImVec2(origin.x, origin.y + 4), ImVec2(end.x, end.y + 4), IM_COL32(0, 0, 0, 60), 18.0f);
  draw->AddRectFilled(origin, end, IM_COL32(0x1d, 0x1c, 0x1b, 255), 18.0f);
  draw->AddRectFilledMultiColor(ImVec2(origin.x + 5, origin.y + 5), ImVec2(end.x - 5, origin.y + size * 0.45f),
                                IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 0),
                                IM_COL32(255, 255, 255, 0));
  draw->AddRect(origin, end, IM_COL32(255, 255, 255, 30), 18.0f, 0, 1.0f);
  // The ribbing round its edge.
  for (int i = 0; i < 9; i++) {
    const float t = 0.2f + i * 0.075f;
    draw->AddLine(ImVec2(origin.x + size * t, end.y - 9), ImVec2(origin.x + size * t, end.y - 5), IM_COL32(255, 255, 255, 22), 1.5f);
  }

  // The fire button, top left, and its glow while it is down.
  const ImVec2 button(origin.x + 34, origin.y + 34);
  const bool fire = mask & SWITCH_FIRE;
  ImGui::SetCursorScreenPos(ImVec2(button.x - 22, button.y - 22));
  ImGui::InvisibleButton("##fire", ImVec2(44, 44));
  screenButtons_[stick] = ImGui::IsItemActive();
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  draw->AddCircleFilled(button, 20.0f, IM_COL32(0x0c, 0x0b, 0x0a, 255));
  if (fire) glow(draw, button, 15.0f, RED);
  ball(draw, ImVec2(button.x, button.y + (fire ? 1.5f : 0.0f)), 15.0f,
       fire ? IM_COL32(0xf0, 0x3c, 0x34, 255) : IM_COL32(0xb2, 0x22, 0x1e, 255), IM_COL32_WHITE);

  // The gate, and a lamp at each switch.
  const ImVec2 centre(origin.x + size * 0.55f, origin.y + size * 0.55f);
  const float gate = size * 0.27f;
  draw->AddCircleFilled(centre, gate + 8, IM_COL32(0x2b, 0x2a, 0x28, 255));
  draw->AddCircleFilled(centre, gate, IM_COL32(0x0e, 0x0e, 0x0d, 255));
  struct Lamp {
    int bit;
    float dx, dy;
  };
  const Lamp lamps[] = {{SWITCH_UP, 0, -1}, {SWITCH_DOWN, 0, 1}, {SWITCH_LEFT, -1, 0}, {SWITCH_RIGHT, 1, 0}};
  for (const Lamp &lamp : lamps) {
    const ImVec2 at(centre.x + lamp.dx * (gate + 22), centre.y + lamp.dy * (gate + 22));
    const bool lit = mask & lamp.bit;
    if (lit) glow(draw, at, 3.5f, GREEN);
    // A small arrowhead pointing out from the stick.
    const ImVec2 tip(at.x + lamp.dx * 5, at.y + lamp.dy * 5);
    const ImVec2 a(at.x - lamp.dy * 5 - lamp.dx * 2, at.y + lamp.dx * 5 - lamp.dy * 2);
    const ImVec2 b(at.x + lamp.dy * 5 - lamp.dx * 2, at.y - lamp.dx * 5 - lamp.dy * 2);
    draw->AddTriangleFilled(tip, a, b, lit ? GREEN : IM_COL32(255, 255, 255, 45));
  }

  // Pushing the stick with the mouse: eight ways, from the angle, once it is
  // far enough out to close a switch.
  ImGui::SetCursorScreenPos(ImVec2(centre.x - gate - 24, centre.y - gate - 24));
  ImGui::InvisibleButton("##push", ImVec2((gate + 24) * 2, (gate + 24) * 2));
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (ImGui::IsItemActive()) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float dx = (mouse.x - centre.x) / gate;
    const float dy = (mouse.y - centre.y) / gate;
    int pushed = 0;
    if (std::hypot(dx, dy) > 0.35f) {
      const float angle = std::atan2(dy, dx);
      const int octant = static_cast<int>(std::lround(angle / (M_PI / 4))) & 7; // 0 right, 2 down…
      static const int ways[8] = {SWITCH_RIGHT, SWITCH_RIGHT | SWITCH_DOWN, SWITCH_DOWN, SWITCH_DOWN | SWITCH_LEFT,
                                  SWITCH_LEFT,  SWITCH_LEFT | SWITCH_UP,     SWITCH_UP,   SWITCH_UP | SWITCH_RIGHT};
      pushed = ways[octant];
    }
    screenSticks_[stick] = pushed;
  } else {
    screenSticks_[stick] = 0;
  }

  // The stick, leaning toward the closed switches.
  const float lx = (mask & SWITCH_RIGHT ? 1.0f : 0.0f) - (mask & SWITCH_LEFT ? 1.0f : 0.0f);
  const float ly = (mask & SWITCH_DOWN ? 1.0f : 0.0f) - (mask & SWITCH_UP ? 1.0f : 0.0f);
  const float norm = std::max(1.0f, std::hypot(lx, ly));
  const ImVec2 top(centre.x + lx / norm * gate * 0.7f, centre.y + ly / norm * gate * 0.7f);
  // The shaft, as a capsule: a thick line alone ends square.
  const float shaft = gate * 0.36f;
  draw->AddCircleFilled(centre, gate * 0.62f, IM_COL32(0x16, 0x16, 0x15, 255));
  draw->AddCircleFilled(centre, shaft, IM_COL32(0x08, 0x08, 0x08, 255));
  draw->AddLine(centre, top, IM_COL32(0x08, 0x08, 0x08, 255), shaft * 2);
  ball(draw, top, gate * 0.42f, IM_COL32(0x22, 0x22, 0x22, 255), IM_COL32_WHITE);

  // Which stick, and its switches as the machine reads them.
  char name[16];
  std::snprintf(name, sizeof(name), "STICK %d", stick + 1);
  caption(draw, ImVec2(end.x - 56, origin.y + 14), name, IM_COL32(255, 255, 255, 110));
}

void Joystick::drawJoyport() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float gap = 24;
  const float size = (WIDTH - gap) * 0.5f - 20;
  for (int stick = 0; stick < 2; stick++) {
    ImGui::PushID(stick);
    const ImVec2 origin(start.x + stick * (size + 20 + gap) + 10, start.y);
    drawCX40(stick, origin, size);

    // The switches, lit as they close, and what is closing them.
    const int mask = sticks_[stick];
    ImGui::PushFont(ui::monoFont(), 0.0f);
    float x = origin.x + 4;
    const float y = origin.y + size + 14;
    const struct {
      int bit;
      const char *label;
    } switches[] = {{SWITCH_UP, "UP"}, {SWITCH_DOWN, "DN"}, {SWITCH_LEFT, "LT"}, {SWITCH_RIGHT, "RT"}, {SWITCH_FIRE, "FIRE"}};
    for (const auto &s : switches) {
      const bool lit = mask & s.bit;
      const ImVec2 sz = ImGui::CalcTextSize(s.label);
      const ImU32 colour = s.bit == SWITCH_FIRE ? RED : GREEN;
      draw->AddRectFilled(ImVec2(x, y), ImVec2(x + sz.x + 12, y + sz.y + 6), lit ? colour : text(0.07f), 6.0f);
      draw->AddText(ImVec2(x + 6, y + 3), lit ? IM_COL32(20, 20, 20, 255) : secondary(), s.label);
      x += sz.x + 18;
    }
    ImGui::PopFont();
    char driven[48];
    std::snprintf(driven, sizeof(driven), "%s", sourceName(static_cast<int>(stickSources_[stick])));
    draw->AddText(ImVec2(origin.x + 4, y + 30), stickSources_[stick] == Source::None ? secondary() : accent(), driven);
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(WIDTH, size + 14 + 30 + ImGui::GetTextLineHeight() + 4));
}

void Joystick::drawInputs() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetFrameHeight();
  const int rows = gamepadEnabled ? std::max(1, static_cast<int>(pads_.size())) : 0;
  const float height = 16 + line * 2 + 10 + (gamepadEnabled ? line + 8 + rows * (line + 8) : 0) + 12;
  card(draw, start, ImVec2(start.x + WIDTH, start.y + height));

  const float left = start.x + 16;
  const float right = start.x + WIDTH - 16;
  float y = start.y + 14;
  auto switchRow = [&](const char *label, const char *hint, bool *value) {
    draw->AddText(ImVec2(left, y + 1), text(), label);
    if (hint) {
      ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.85f);
      draw->AddText(ImVec2(left, y + ImGui::GetTextLineHeight() + 4), secondary(), hint);
      ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(right - ui::SwitchWidth("##switch"), y));
    ImGui::PushID(label);
    ui::Switch("##switch", value);
    ImGui::PopID();
  };
  switchRow("Cursor keys", "The arrows still reach the machine's keyboard too.", &cursorKeys);
  y += line + 22;
  switchRow("Gamepads", nullptr, &gamepadEnabled);
  y += line + 6;
  if (gamepadEnabled) {
    draw->AddText(ImVec2(left, y + 3), secondary(), "Deadzone");
    ImGui::SetCursorScreenPos(ImVec2(left + 90, y));
    ImGui::SetNextItemWidth(right - left - 90 - 50);
    ui::SliderFloat("##deadzone", &deadzone, 0.0f, MAX_DEADZONE, "%.2f");
    y += line + 8;

    if (pads_.empty()) {
      draw->AddText(ImVec2(left, y + 3), secondary(), "No gamepad connected. Pair one in System Settings.");
    }
    for (size_t i = 0; i < pads_.size(); i++) {
      const Pad &pad = pads_[i];
      // Its left stick, live, then its two fire buttons, then what it drives.
      const ImVec2 dial(left + line * 0.5f, y + line * 0.5f);
      draw->AddCircleFilled(dial, line * 0.5f, text(0.08f));
      draw->AddCircle(dial, line * 0.5f * deadzone * 2, text(0.15f));
      draw->AddCircleFilled(ImVec2(dial.x + pad.axes[0] * line * 0.42f, dial.y + pad.axes[1] * line * 0.42f), 3.5f, accent());
      for (int b = 0; b < 2; b++) {
        const ImVec2 at(left + line + 14 + b * 14, dial.y);
        draw->AddCircleFilled(at, 5.0f, pad.buttons[b] ? RED : text(0.12f));
      }
      draw->AddText(ImVec2(left + line + 50, y + 3), text(), pad.name.c_str());
      const char *drives = device == 0 ? (i == 0 ? "The joystick" : "Not used")
                           : pads_.size() == 1 ? "Both sticks"
                           : i < 2            ? (i == 0 ? "Stick 1" : "Stick 2")
                                              : "Not used";
      const float w = ImGui::CalcTextSize(drives).x;
      draw->AddText(ImVec2(right - w, y + 3), secondary(), drives);
      y += line + 8;
    }
  }
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(WIDTH, height));
}

void Joystick::draw(bool *open) {
  if (!open || !*open) {
    screenButtons_ = {};
    screenSticks_ = {};
    dragging_ = false;
    return;
  }
  ui::BeforeWindow("Joystick");
  if (ImGui::Begin("Joystick", open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(top.x + (WIDTH - 340) * 0.5f, top.y));
    ui::SegmentedControl("##device", &device, {"Apple Joystick", "Sirius Joyport"}, 340.0f);
    ImGui::SetCursorScreenPos(ImVec2(top.x, ImGui::GetCursorScreenPos().y + 10));

    if (device == 0) {
      const ImVec2 start = ImGui::GetCursorScreenPos();
      drawAppleJoystick();

      // Beside it, what the machine reads.
      const float x = start.x + STICK_SIZE + 24;
      const float width = WIDTH - STICK_SIZE - 24;
      float y = start.y + 2;
      const char *const names[] = {"PADDLE 0  ·  X", "PADDLE 1  ·  Y"};
      const char *const addresses[] = {"$C064", "$C065"};
      for (int i = 0; i < 2; i++) {
        caption(draw, ImVec2(x, y), names[i], secondary());
        ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.72f);
        const float aw = ImGui::CalcTextSize(addresses[i]).x;
        draw->AddText(ImVec2(x + width - aw, y), secondary(), addresses[i]);
        ImGui::PopFont();
        y += 16;
        ImGui::PushFont(ui::monoFont(), 26.0f);
        char value[8];
        std::snprintf(value, sizeof(value), "%3d", paddles_[i]);
        draw->AddText(ImVec2(x, y), text(), value);
        const float valueWidth = ImGui::CalcTextSize("000").x;
        ImGui::PopFont();
        // How long the timer runs for it.
        char time[24];
        std::snprintf(time, sizeof(time), "%.2f ms", paddles_[i] * PREAD_CYCLES_PER_COUNT / CLOCK_MHZ / 1000.0);
        draw->AddText(ImVec2(x + valueWidth + 12, y + 9), secondary(), time);
        y += 34;
        const float t = paddles_[i] / 255.0f;
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + 6), text(0.10f), 3.0f);
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + std::max(6.0f, width * t), y + 6), accent(), 3.0f);
        draw->AddLine(ImVec2(x + width * 0.5f, y - 3), ImVec2(x + width * 0.5f, y + 9), text(0.25f), 1.0f);
        y += 24;
      }

      // The pushbuttons: hold one down with the mouse.
      caption(draw, ImVec2(x, y), "PUSHBUTTONS", secondary());
      y += 18;
      const char *const roles[] = {"Pushbutton 0: the Open Apple key on a //e", "Pushbutton 1: the Closed Apple key on a //e",
                                   "Pushbutton 2: a third button, on the connector only"};
      const char *const labels[] = {"PB0", "PB1", "PB2"};
      const char *const lines[] = {"$C061", "$C062", "$C063"};
      const float spacing = width / 3;
      for (int i = 0; i < 3; i++) {
        const ImVec2 centre(x + spacing * (i + 0.5f), y + 24);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(centre.x - 24, centre.y - 24));
        ImGui::InvisibleButton("##button", ImVec2(48, 48));
        screenButtons_[i] = ImGui::IsItemActive();
        if (ImGui::IsItemHovered()) {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", roles[i]);
        }
        ImGui::PopID();
        const bool down = buttons_[i];
        draw->AddCircleFilled(centre, 23.0f, ui::isDark() ? IM_COL32(0x10, 0x10, 0x10, 255) : IM_COL32(0x9a, 0x92, 0x80, 255));
        if (down) glow(draw, centre, 18.0f, RED);
        ball(draw, ImVec2(centre.x, centre.y + (down ? 1.5f : 0.0f)), 18.0f,
             down ? IM_COL32(0xf0, 0x3c, 0x34, 255) : IM_COL32(0x2a, 0x2a, 0x2c, 255), IM_COL32_WHITE);
        ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
        centredText(draw, ImVec2(centre.x, centre.y + 38), down ? RED : text(0.85f), labels[i]);
        centredText(draw, ImVec2(centre.x, centre.y + 52), secondary(), lines[i]);
        ImGui::PopFont();
      }
      y += 24 + 64;
      caption(draw, ImVec2(x, y), "DRIVEN BY", secondary());
      sources(draw, ImVec2(x, y + 16), static_cast<int>(source_));
      y += 16 + ImGui::GetFrameHeight();
      ImGui::SetCursorScreenPos(start);
      ImGui::Dummy(ImVec2(WIDTH, std::max(STICK_SIZE + 8, y - start.y + 8)));
    } else {
      drawJoyport();
    }

    ImGui::Dummy(ImVec2(0, 6));
    drawInputs();
  }
  ImGui::End();
}

} // namespace a2e::native
