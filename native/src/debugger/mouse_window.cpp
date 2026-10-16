/*
 * mouse_window.cpp - The Mouse window: an Apple Mouse Card, or a //c's own mouse
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "debugger/mouse_window.hpp"

#include "app/emulation.hpp"
#include "ui/ui_controls.hpp"
#include "ui/ui_theme.hpp"

#include "cards/mouse/mouse_card.hpp"
#include "emulator.hpp"
#include "iigs/iigs_machine.hpp"
#include "input/mouse_iou.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace a2e::native {

namespace {

// The window, in points.
constexpr float WIDTH = 500;
constexpr float CARD_ROUNDING = 10;
constexpr float PAD = 14;
constexpr float PAD_WIDTH = 150; // the clamp window's picture
constexpr float PAD_HEIGHT = 104;

// The colours the Mockingboard window uses.
constexpr ImU32 GREEN = IM_COL32(97, 187, 70, 255);
constexpr ImU32 RED = IM_COL32(224, 58, 62, 255);
constexpr ImU32 ORANGE = IM_COL32(245, 130, 31, 255);
constexpr ImU32 PURPLE = IM_COL32(0xB0, 0x5C, 0xB1, 255);
constexpr ImU32 BLUE = IM_COL32(0x18, 0xAB, 0xEA, 255);

// The commands the firmware sends the card's microcontroller through the
// PIA, by their high nibble (mouse-card-window.js's names).
constexpr const char *COMMANDS[8] = {"SET", "READ", "SERV", "CLEAR", "POS", "INIT", "CLAMP", "HOME"};

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

void card(ImDrawList *draw, ImVec2 a, ImVec2 b) {
  draw->AddRectFilled(a, b, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

// A well for a picture: darker than the card in either appearance.
ImU32 well() { return ui::isDark() ? IM_COL32(0, 0, 0, 90) : IM_COL32(0, 0, 0, 18); }

float smallLine() {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float height = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  return height;
}

// Small capitals over a reading, as the Mockingboard window labels its rows.
void caption(ImDrawList *draw, ImVec2 at, const char *label) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(at, secondary(), label);
  ImGui::PopFont();
}

// A capsule with a word in it, lit or not. Returns its width.
float pill(ImDrawList *draw, ImVec2 at, const char *label, bool lit, ImU32 colour) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const ImVec2 end(at.x + size.x + 10, at.y + size.y + 4);
  draw->AddRectFilled(at, end, lit ? colour : text(0.07f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 5, at.y + 2), lit ? ui::textOn(colour) : secondary(), label);
  ImGui::PopFont();
  return end.x - at.x;
}

float pillWidth(const char *label) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float width = ImGui::CalcTextSize(label).x + 10;
  ImGui::PopFont();
  return width;
}

// A label and a value in the mono face, the value coloured. Returns the width.
float field(ImDrawList *draw, ImVec2 at, const char *label, const char *value, ImU32 colour) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(at, secondary(), label);
  const float lw = *label ? ImGui::CalcTextSize(label).x + 4 : 0;
  draw->AddText(ImVec2(at.x + lw, at.y), colour, value);
  const float width = lw + ImGui::CalcTextSize(value).x;
  ImGui::PopFont();
  return width;
}

// A big reading in the mono face with its caption above, as the
// Mockingboard shows a note. Returns the width.
float reading(ImDrawList *draw, ImVec2 at, const char *label, int value) {
  caption(draw, at, label);
  char number[16];
  std::snprintf(number, sizeof(number), "%d", value);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.4f);
  draw->AddText(ImVec2(at.x, at.y + smallLine() + 2), text(), number);
  const float width = std::max(ImGui::CalcTextSize("00000").x, ImGui::CalcTextSize(number).x);
  ImGui::PopFont();
  return width;
}

float readingHeight() {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.4f);
  const float height = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  return smallLine() + 2 + height;
}

// A register's eight bits as cells, bit 7 first, lit where set.
void bits(ImDrawList *draw, ImVec2 at, uint8_t value, ImU32 colour) {
  const float cell = 9, gap = 3;
  const float height = smallLine();
  for (int bit = 7; bit >= 0; bit--) {
    const float x = at.x + (7 - bit) * (cell + gap) + (bit < 4 ? 4 : 0);
    const ImVec2 a(x, at.y + (height - cell) * 0.5f);
    draw->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), (value >> bit) & 1 ? colour : text(0.08f), 2.0f);
  }
}

void separator(ImDrawList *draw, float left, float right, float y) {
  draw->AddLine(ImVec2(left, y), ImVec2(right, y), ImGui::GetColorU32(ImGuiCol_Separator));
}

// A card's heading: a title, what it is in grey after it, and an IRQ pill on
// the right. Returns the y below it.
float heading(ImDrawList *draw, ImVec2 start, float width, const char *title, const char *detail, bool irq) {
  const float left = start.x + PAD;
  const float y = start.y + PAD;
  draw->AddText(ImVec2(left, y), text(), title);
  const float tw = ImGui::CalcTextSize(title).x;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(ImVec2(left + tw + 10, y + 2), secondary(), detail);
  ImGui::PopFont();
  pill(draw, ImVec2(start.x + width - PAD - pillWidth("IRQ"), y), "IRQ", irq, RED);
  return y + ImGui::GetTextLineHeight() + 12;
}

// The card's background, drawn behind what was drawn on it once its height
// is known.
struct CardScope {
  ImDrawList *draw;
  ImVec2 start;
  float width;
  CardScope(float w) : draw(ImGui::GetWindowDrawList()), start(ImGui::GetCursorScreenPos()), width(w) {
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
  }
  void end(float bottom) {
    draw->ChannelsSetCurrent(0);
    card(draw, start, ImVec2(start.x + width, bottom));
    draw->ChannelsMerge();
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, bottom - start.y));
  }
};

} // namespace

MouseWindow::MouseWindow(Emulation &emulation) : emulation_(emulation) {}

// Which mouse the machine has, if any. A IIgs's card can be in any socket;
// a //e's is wherever it was fitted, and a //c's is the IOU.
void MouseWindow::update() {
  emulation_.poll(updatePoll_, [&](host::MachineHost &host) {
    kind_ = Kind::None;
    if (iigs::IIgsMachine *gs = host.iigs()) {
      for (uint8_t slot = 1; slot <= 7; slot++) {
        if (dynamic_cast<MouseCard *>(gs->memory().slotCard(slot))) kind_ = Kind::Card;
      }
    } else if (Emulator *emu = host.emulator()) {
      if (emu->getMouseCard()) kind_ = Kind::Card;
      else if (emu->getMouseIOU()) kind_ = Kind::IOU;
    }
  });
}

void MouseWindow::take() {
  emulation_.poll(takePoll_, [&](host::MachineHost &host) {
    const MouseCard *mouse = nullptr;
    int slot = 4;
    bool iigs = false;
    if (iigs::IIgsMachine *gs = host.iigs()) {
      iigs = true;
      for (uint8_t s = 1; s <= 7 && !mouse; s++) {
        if ((mouse = dynamic_cast<MouseCard *>(gs->memory().slotCard(s)))) slot = s;
      }
    } else if (Emulator *emu = host.emulator()) {
      mouse = emu->getMouseCard();
      if (mouse) slot = mouse->getSlotNumber();
      if (const MouseIOU *iou = emu->getMouseIOU()) {
        iou_.button = iou->isButtonPressed();
        iou_.shift = iou->isShiftKeyHeld();
        iou_.movementEnabled = iou->movementInterruptsEnabled();
        iou_.vblEnabled = iou->vblInterruptsEnabled();
        iou_.xPending = iou->xInterruptPending();
        iou_.yPending = iou->yInterruptPending();
        iou_.vblPending = iou->vblInterruptPending();
        iou_.irq = iou->isIRQActive();
        iou_.bankedX = iou->pendingX();
        iou_.bankedY = iou->pendingY();
      }
    }

    if (mouse) {
      Card &c = card_;
      c.slot = slot;
      c.iigs = iigs;
      c.answering = !iigs || !host.isSlotInternal(static_cast<uint8_t>(slot));
      c.x = mouse->getMouseX();
      c.y = mouse->getMouseY();
      c.button = mouse->getMouseButton();
      c.moved = mouse->getMoved();
      c.buttonChanged = mouse->getButtonChanged();
      c.clampMinX = mouse->getClampMinX();
      c.clampMaxX = mouse->getClampMaxX();
      c.clampMinY = mouse->getClampMinY();
      c.clampMaxY = mouse->getClampMaxY();
      c.mode = mouse->getMode();
      c.irq = mouse->isIRQActive();
      c.vblPending = mouse->getVBLInterruptPending();
      c.movePending = mouse->getMoveInterruptPending();
      c.buttonPending = mouse->getButtonInterruptPending();
      c.inVbl = mouse->getWasInVBL();
      c.pia = {mouse->getDDRA(), mouse->getORA(), mouse->getIRA(), mouse->getCRA(),
               mouse->getDDRB(), mouse->getORB(), mouse->getIRB(), mouse->getCRB()};
      c.lastCommand = mouse->getLastCommand();
      c.responseState = mouse->getResponseState();
    }

    // The screen holes, in main memory whatever the banking: the AppleMouse
    // II manual's and the //c Technical Reference's $0478+n (X low), $04F8+n
    // (Y low), $0578+n (X high), $05F8+n (Y high), $0778+n (status) and
    // $07F8+n (mode). A IIgs's are in bank $00, as its firmware writes them.
    uint8_t hole[6] = {};
    const uint16_t offsets[6] = {0x0478, 0x04F8, 0x0578, 0x05F8, 0x0778, 0x07F8};
    if (iigs) {
      for (int i = 0; i < 6; i++) hole[i] = host.peek(offsets[i] + slot);
    } else {
      for (const host::MemorySpace &space : host.memorySpaces()) {
        if (space.kind != host::MemorySpace::Kind::MainRAM) continue;
        for (int i = 0; i < 6; i++) hole[i] = host.peekSpace(space, offsets[i] + slot);
      }
    }
    holes_.x = static_cast<int16_t>(hole[0] | (hole[2] << 8));
    holes_.y = static_cast<int16_t>(hole[1] | (hole[3] << 8));
    holes_.status = hole[4];
    holes_.mode = hole[5];
  });
}

void MouseWindow::drawCard(float width) {
  const Card &c = card_;
  CardScope scope(width);
  ImDrawList *draw = scope.draw;
  const ImVec2 start = scope.start;
  const float left = start.x + PAD;
  const float right = start.x + width - PAD;
  const float line = smallLine();
  char value[48];

  std::snprintf(value, sizeof(value), "Slot %d  ·  6821 PIA at $C0%X0", c.slot, 8 + c.slot);
  float y = heading(draw, start, width, "Mouse Card", value, c.irq);

  // Where the card has the mouse, inside the clamp window the firmware set:
  // the picture is that window, whatever its shape, with the mouse in it.
  const ImVec2 pa(left, y), pb(left + PAD_WIDTH, y + PAD_HEIGHT);
  draw->AddRectFilled(pa, pb, well(), 6.0f);
  const float spanX = static_cast<float>(std::max(1, c.clampMaxX - c.clampMinX));
  const float spanY = static_cast<float>(std::max(1, c.clampMaxY - c.clampMinY));
  const float fx = std::clamp((c.x - c.clampMinX) / spanX, 0.0f, 1.0f);
  const float fy = std::clamp((c.y - c.clampMinY) / spanY, 0.0f, 1.0f);
  const ImVec2 dot(pa.x + 6 + fx * (PAD_WIDTH - 12), pa.y + 6 + fy * (PAD_HEIGHT - 12));
  draw->AddLine(ImVec2(pa.x + 4, dot.y), ImVec2(pb.x - 4, dot.y), text(0.10f));
  draw->AddLine(ImVec2(dot.x, pa.y + 4), ImVec2(dot.x, pb.y - 4), text(0.10f));
  draw->AddCircleFilled(dot, 4.5f, c.button ? GREEN : accent());

  float x = pb.x + 18;
  float ry = y;
  x += reading(draw, ImVec2(x, ry), "X", c.x) + 18;
  reading(draw, ImVec2(x, ry), "Y", c.y);
  ry += readingHeight() + 10;
  x = pb.x + 18;
  x += pill(draw, ImVec2(x, ry), c.button ? "DOWN" : "UP", c.button, GREEN) + 4;
  x += pill(draw, ImVec2(x, ry), "MOVED", c.moved, ORANGE) + 4;
  pill(draw, ImVec2(x, ry), "BTN CHG", c.buttonChanged, ORANGE);
  ry += line + 12;
  x = pb.x + 18;
  std::snprintf(value, sizeof(value), "%d..%d", c.clampMinX, c.clampMaxX);
  field(draw, ImVec2(x, ry), "CLAMP X", value, GREEN);
  ry += line + 4;
  std::snprintf(value, sizeof(value), "%d..%d", c.clampMinY, c.clampMaxY);
  field(draw, ImVec2(x, ry), "CLAMP Y", value, GREEN);
  y = std::max(pb.y, ry + line) + 14;

  // The mode the firmware set (SetMouse), and the interrupts it asked for.
  separator(draw, left, right, y - 7);
  caption(draw, ImVec2(left, y + 1), "MODE");
  x = left + 78;
  std::snprintf(value, sizeof(value), "$%02X", c.mode);
  x += field(draw, ImVec2(x, y), "", value, PURPLE) + 12;
  x += pill(draw, ImVec2(x, y), "ON", c.mode & 0x01, BLUE) + 4;
  x += pill(draw, ImVec2(x, y), "MOV", c.mode & 0x02, BLUE) + 4;
  x += pill(draw, ImVec2(x, y), "BTN", c.mode & 0x04, BLUE) + 4;
  pill(draw, ImVec2(x, y), "VBL", c.mode & 0x08, BLUE);
  y += line + 10;
  caption(draw, ImVec2(left, y + 1), "PENDING");
  x = left + 78;
  x += pill(draw, ImVec2(x, y), "VBL", c.vblPending, RED) + 4;
  x += pill(draw, ImVec2(x, y), "MOV", c.movePending, RED) + 4;
  x += pill(draw, ImVec2(x, y), "BTN", c.buttonPending, RED) + 16;
  pill(draw, ImVec2(x, y), "IN VBL", c.inVbl, PURPLE);
  y += line + 18;

  // The PIA, port A beside port B, each register's bits as cells.
  separator(draw, left, right, y - 9);
  static const char *const names[8] = {"DDRA", "ORA", "IRA", "CRA", "DDRB", "ORB", "IRB", "CRB"};
  const float column = (right - left) * 0.5f;
  for (int port = 0; port < 2; port++) {
    float py = y;
    for (int r = 0; r < 4; r++) {
      const int i = port * 4 + r;
      const float px = left + port * column;
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
      draw->AddText(ImVec2(px, py), secondary(), names[i]);
      std::snprintf(value, sizeof(value), "$%02X", c.pia[i]);
      draw->AddText(ImVec2(px + 42, py), GREEN, value);
      ImGui::PopFont();
      bits(draw, ImVec2(px + 82, py), c.pia[i], withAlpha(BLUE, 0.85f));
      py += line + 5;
    }
  }
  y += (line + 5) * 4 + 9;

  // The last command the firmware sent the card's microcontroller, and
  // where the exchange is.
  separator(draw, left, right, y - 7);
  x = left;
  std::snprintf(value, sizeof(value), "$%02X", c.lastCommand);
  x += field(draw, ImVec2(x, y), "LAST COMMAND", value, GREEN) + 8;
  x += pill(draw, ImVec2(x, y), COMMANDS[(c.lastCommand >> 4) & 0x07], true, PURPLE) + 18;
  static const char *const states[3] = {"IDLE", "WRITING", "READING"};
  field(draw, ImVec2(x, y), "EXCHANGE", c.responseState < 3 ? states[c.responseState] : "?", text());
  y += line + PAD;

  scope.end(y);
}

void MouseWindow::drawIou(float width) {
  const Iou &m = iou_;
  CardScope scope(width);
  ImDrawList *draw = scope.draw;
  const ImVec2 start = scope.start;
  const float left = start.x + PAD;
  const float right = start.x + width - PAD;
  const float line = smallLine();
  char value[32];

  float y = heading(draw, start, width, "Mouse (Built-in)", "The IOU  ·  firmware in slot 4", m.irq);

  caption(draw, ImVec2(left, y + 1), "BUTTON");
  float x = left + 92;
  x += pill(draw, ImVec2(x, y), m.button ? "DOWN" : "UP", m.button, GREEN) + 4;
  pill(draw, ImVec2(x, y), "SHIFT", m.shift, GREEN);
  y += line + 10;

  // What the firmware allows ($C058-$C05B), and what has happened since the
  // handler last looked.
  caption(draw, ImVec2(left, y + 1), "ALLOWED");
  x = left + 92;
  x += pill(draw, ImVec2(x, y), "MOVEMENT", m.movementEnabled, BLUE) + 4;
  pill(draw, ImVec2(x, y), "VBL", m.vblEnabled, BLUE);
  y += line + 10;
  caption(draw, ImVec2(left, y + 1), "PENDING");
  x = left + 92;
  x += pill(draw, ImVec2(x, y), "X0", m.xPending, RED) + 4;
  x += pill(draw, ImVec2(x, y), "Y0", m.yPending, RED) + 4;
  pill(draw, ImVec2(x, y), "VBL", m.vblPending, RED);
  y += line + 18;

  // Travel the Mac's mouse has made that the IOU is still to deliver, a
  // step an interrupt.
  separator(draw, left, right, y - 9);
  caption(draw, ImVec2(left, y + 1), "TO DELIVER");
  x = left + 92;
  std::snprintf(value, sizeof(value), "%d", m.bankedX);
  x += field(draw, ImVec2(x, y), "X", value, GREEN) + 18;
  std::snprintf(value, sizeof(value), "%d", m.bankedY);
  field(draw, ImVec2(x, y), "Y", value, GREEN);
  y += line + PAD;

  scope.end(y);
}

void MouseWindow::drawHoles(float width) {
  const Holes &h = holes_;
  CardScope scope(width);
  ImDrawList *draw = scope.draw;
  const ImVec2 start = scope.start;
  const float left = start.x + PAD;
  const float line = smallLine();
  char value[48];

  const int slot = kind_ == Kind::Card ? card_.slot : 4;
  std::snprintf(value, sizeof(value), "$%04X and on  ·  what programs read", 0x0478 + slot);
  float y = start.y + PAD;
  draw->AddText(ImVec2(left, y), text(), "Screen Holes");
  const float tw = ImGui::CalcTextSize("Screen Holes").x;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(ImVec2(left + tw + 10, y + 2), secondary(), value);
  ImGui::PopFont();
  y += ImGui::GetTextLineHeight() + 12;

  float x = left;
  x += reading(draw, ImVec2(x, y), "X", h.x) + 18;
  x += reading(draw, ImVec2(x, y), "Y", h.y) + 28;
  // The status byte: the button now and at the last read, movement since,
  // and which of the three interrupts it was.
  const float sy = y + 2;
  caption(draw, ImVec2(x, sy), "STATUS");
  std::snprintf(value, sizeof(value), "$%02X", h.status);
  field(draw, ImVec2(x + 52, sy - 1), "", value, PURPLE);
  std::snprintf(value, sizeof(value), "$%02X", h.mode);
  caption(draw, ImVec2(x + 100, sy), "MODE");
  field(draw, ImVec2(x + 138, sy - 1), "", value, PURPLE);
  float px = x;
  const float py = sy + line + 8;
  px += pill(draw, ImVec2(px, py), "DOWN", h.status & 0x80, GREEN) + 4;
  px += pill(draw, ImVec2(px, py), "WAS DOWN", h.status & 0x40, GREEN) + 4;
  pill(draw, ImVec2(px, py), "MOVED", h.status & 0x20, ORANGE);
  px = x;
  const float qy = py + line + 8;
  px += pill(draw, ImVec2(px, qy), "VBL INT", h.status & 0x08, RED) + 4;
  px += pill(draw, ImVec2(px, qy), "BTN INT", h.status & 0x04, RED) + 4;
  pill(draw, ImVec2(px, qy), "MOV INT", h.status & 0x02, RED);
  y = std::max(y + readingHeight(), qy + line + 4) + PAD;

  scope.end(y);
}

void MouseWindow::draw(bool *open) {
  if (!open || !*open || kind_ == Kind::None) return;
  take();
  ui::BeforeWindow("Mouse");
  if (ui::BeginWindow("Mouse", open, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (kind_ == Kind::Card && !card_.answering) {
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + WIDTH);
      ImGui::TextDisabled("Slot %d is set to the IIgs's own mouse, so this card is idle until the slot is set to "
                          "Your Card in Expansion Slots.", card_.slot);
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
    }
    if (kind_ == Kind::Card) drawCard(WIDTH);
    else drawIou(WIDTH);
    ImGui::Dummy(ImVec2(0, 4));
    drawHoles(WIDTH);
  }
  ImGui::End();
}

} // namespace a2e::native
