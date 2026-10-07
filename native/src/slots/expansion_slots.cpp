/*
 * expansion_slots.cpp - The Expansion Slots window, and applying a layout
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "slots/expansion_slots.hpp"
#include "ui/ui_controls.hpp"

#include "app/emulation.hpp"
#include "ui/ui_theme.hpp"

#include "machine/machine_profile.hpp"

#include "imgui.h"
#include "imgui_internal.h" // MarkIniSettingsDirty

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <vector>

namespace a2e::native {

ExpansionSlots::ExpansionSlots(Emulation &emulation) : emulation_(emulation) {}

SlotLayout *ExpansionSlots::openSection(const char *machine) {
  if (!findMachineProfile(machine)) return nullptr;
  SlotLayout &layout = saved_[machine];
  layout.clear();
  sockets_[machine].clear();
  section_ = machine;
  return &layout;
}

void ExpansionSlots::readLine(SlotLayout *layout, const char *line) {
  int slot = 0;
  int socket = 0;
  std::string card;
  if (!layout) return;
  if (std::sscanf(line, "Socket%d=%d", &slot, &socket) == 2) {
    if (slot >= 1 && slot <= 7) sockets_[section_][slot] = socket != 0;
    return;
  }
  if (parseSlotLine(line, slot, card)) (*layout)[slot] = card;
}

void ExpansionSlots::writeAll(ImGuiTextBuffer *out, const char *typeName) const {
  for (const auto &[machine, layout] : saved_) {
    out->appendf("[%s][%s]\n", typeName, machine.c_str());
    for (const auto &[slot, card] : layout) out->appendf("%s\n", formatSlotLine(slot, card).c_str());
    if (const auto it = sockets_.find(machine); it != sockets_.end()) {
      for (const auto &[slot, socket] : it->second) out->appendf("Socket%d=%d\n", slot, socket ? 1 : 0);
    }
    out->append("\n");
  }
}

// What this machine was last left with, or what it ships with. Only the
// slots the layout names are fitted, as the browser does at startup; the
// rest keep what the machine was built with.
void ExpansionSlots::apply() {
  if (!machine_) return;
  auto it = saved_.find(machine_->key);
  const SlotLayout layout = it != saved_.end() ? it->second : defaultLayout(*machine_);
  const bool clock = noSlotClock;
  const std::map<int, bool> sockets = sockets_[machine_->key];
  emulation_.withMachine([&](host::MachineHost &host) {
    for (const auto &[slot, card] : layout) {
      if (!isFixedSlot(*machine_, slot)) host.setSlotCard(slot, card);
    }
    // Only the slots the user chose: one never touched is the firmware's.
    for (const auto &[slot, socket] : sockets) host.setSlotInternal(slot, !socket);
    host.setNoSlotClock(clock);
  });
  dirty_ = false;
}

// The machine is the authority on what is fitted.
void ExpansionSlots::refreshFromMachine() {
  working_.clear();
  if (!machine_) return;
  emulation_.withMachine([&](host::MachineHost &host) {
    for (int slot = machine_->firstSlot; slot <= machine_->lastSlot; slot++) {
      if (!isFixedSlot(*machine_, slot)) working_[slot] = host.slotCard(slot);
    }
  });
  applied_ = working_;
  dirty_ = false;
}

void ExpansionSlots::adoptMachineLayout() {
  if (!machine_) return;
  refreshFromMachine();
  saved_[machine_->key] = working_;
  ImGui::MarkIniSettingsDirty();
}

void ExpansionSlots::applyAndReset() {
  if (onApplying_) onApplying_();
  saved_[machine_->key] = working_;
  ImGui::MarkIniSettingsDirty();
  emulation_.withMachine([&](host::MachineHost &host) {
    for (const auto &[slot, card] : working_) host.setSlotCard(slot, card);
    host.reset();
  });
  applied_ = working_;
  dirty_ = false;
  if (onApplied_) onApplied_();
}

namespace {

// The window, in points.
constexpr float BOARD_WIDTH = 600;
constexpr float ROW_HEIGHT = 74;
constexpr float CARD_X = 54;        // where a card's left edge sits, from the board's left
// The tab with the fingers is at the right-hand end of a card's bottom edge,
// as on Apple's cards seen from the component side: it ends this far short
// of the right edge and takes this share of the edge's length.
constexpr float TAB_INSET = 22;
constexpr float TAB_SHARE = 0.62f;
// An Apple II slot is a 50-contact connector, 25 on each side of the card.
constexpr int PINS_PER_SIDE = 25;
constexpr float CARD_WIDTH = 270;
constexpr ImU32 PCB = IM_COL32(0x1f, 0x5a, 0x3a, 255);
constexpr ImU32 PCB_DARK = IM_COL32(0x16, 0x45, 0x2c, 255);
constexpr ImU32 SILK = IM_COL32(0xe8, 0xf0, 0xe0, 210);
constexpr ImU32 GOLD = IM_COL32(0xd8, 0xb0, 0x4a, 255);

ImU32 rgb(unsigned c, float alpha = 1.0f) {
  return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, static_cast<int>(255 * alpha));
}
ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

void dashedOutline(ImDrawList *draw, const ImVec2 *points, int count, ImU32 colour) {
  auto line = [&](ImVec2 p, ImVec2 q) {
    const float length = std::hypot(q.x - p.x, q.y - p.y);
    const ImVec2 d((q.x - p.x) / length, (q.y - p.y) / length);
    for (float t = 0; t < length; t += 8) {
      const float u = std::min(t + 4.5f, length);
      draw->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * u, p.y + d.y * u), colour, 1.2f);
    }
  };
  for (int i = 0; i < count; i++) line(points[i], points[(i + 1) % count]);
}

void dashedRect(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour) {
  const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
  dashedOutline(draw, corners, 4, colour);
}

// The cards are seen from in front of the machine and above it, at this
// elevation and from this far away, in points: standing in their slots,
// their faces are foreshortened, and their tops, nearer the eye, are a
// little larger than their bottoms. The board itself is drawn as it is seen.
constexpr float VIEW_ELEVATION = IM_PI / 4;
constexpr float EYE_DISTANCE = 700;
constexpr float CARD_HEIGHT = 66;   // a card's face, before it is foreshortened
constexpr float SLOT_MOUTH = 56;    // from a row's top, where a card goes into its connector
constexpr float CARD_RISE = 4;      // how far a card's bottom edge stands above the mouth
constexpr float PCB_THICKNESS = 3;
// The light is behind the eye's left shoulder: a card's shadow falls on the
// board behind it and to its right.
constexpr float SHADOW_REACH = 1.15f;
constexpr float SHADOW_SLANT = 0.22f;

// Stands up what is drawn flat in front of a slot: a point a height above
// the slot's mouth goes to where that point of an upright card is seen,
// spreading from the card's middle as it comes nearer the eye.
struct Stand {
  float centre;
  float mouth;
  ImVec2 operator()(ImVec2 p) const {
    const float h = mouth - p.y;
    const float k = EYE_DISTANCE / (EYE_DISTANCE - h * std::sin(VIEW_ELEVATION));
    return ImVec2(centre + (p.x - centre) * k, mouth - h * std::cos(VIEW_ELEVATION) * k);
  }
  // Where a point of the card throws its shadow on the board.
  ImVec2 shadow(ImVec2 p) const {
    const float h = mouth - p.y;
    return ImVec2(p.x + h * SHADOW_SLANT, mouth - h * SHADOW_REACH * std::sin(VIEW_ELEVATION));
  }
};

// Draws what `paint` draws flat, as the component side of a card facing the
// eye, and then stands it up in its slot.
template <typename Paint> void standUp(ImDrawList *draw, const Stand &stand, Paint &&paint) {
  const int first = draw->VtxBuffer.Size;
  paint();
  for (int i = first; i < draw->VtxBuffer.Size; i++) draw->VtxBuffer[i].pos = stand(draw->VtxBuffer[i].pos);
}

// A slot's edge connector, seen from in front and above, as the moulded
// black housing of a 50-contact connector is: its top a lip either side of
// the channel a card stands in, a window over each contact along both lips,
// the far wall of the channel and its spring contacts lit from above, and
// the housing's front face falling away to the board. Its pins go through
// the board and are not seen. The back is drawn before a card and the front
// after it, so the card's fingers go down into the channel.
constexpr ImU32 HOUSING_TOP = IM_COL32(0x30, 0x2e, 0x2d, 255);
constexpr float HOUSING_END = 8;   // past the end contacts
constexpr float CHANNEL_END = 3;

void connectorBack(ImDrawList *draw, float tabLeft, float tabRight, float mouth) {
  const float pitch = (tabRight - tabLeft) / PINS_PER_SIDE;
  const float left = tabLeft - HOUSING_END;
  const float right = tabRight + HOUSING_END;
  // Its shadow on the board, soft, and deepest in front of it.
  for (int i = 0; i < 4; i++) {
    const float spread = 1.0f + i * 1.6f;
    draw->AddRectFilled(ImVec2(left - spread, mouth - 5 - spread * 0.5f), ImVec2(right + spread, mouth + 11 + spread * 2),
                        IM_COL32(0, 0, 0, 22), 3.0f + spread);
  }
  // The top, its far edge catching the light.
  draw->AddRectFilled(ImVec2(left, mouth - 5), ImVec2(right, mouth + 4.5f), HOUSING_TOP, 2.5f, ImDrawFlags_RoundCornersTop);
  draw->AddLine(ImVec2(left + 2, mouth - 4.6f), ImVec2(right - 2, mouth - 4.6f), IM_COL32(255, 255, 255, 30), 1.0f);
  // The far lip's contact windows.
  for (int pin = 0; pin < PINS_PER_SIDE; pin++) {
    const float x = tabLeft + pitch * (pin + 0.5f) - pitch * 0.25f;
    draw->AddRectFilled(ImVec2(x - pitch * 0.2f, mouth - 3.8f), ImVec2(x + pitch * 0.2f, mouth - 2.8f), IM_COL32(0x12, 0x11, 0x11, 255));
  }
  // The channel: its floor in the dark, and its far wall, with the
  // component side's contacts standing on it, lit at their tops where they
  // bend over to meet a card.
  draw->AddRectFilled(ImVec2(tabLeft - CHANNEL_END, mouth - 2.2f), ImVec2(tabRight + CHANNEL_END, mouth + 1.6f),
                      IM_COL32(0x03, 0x03, 0x03, 255), 1.0f);
  draw->AddRectFilled(ImVec2(tabLeft - CHANNEL_END, mouth - 2.2f), ImVec2(tabRight + CHANNEL_END, mouth - 0.3f),
                      IM_COL32(0x1a, 0x19, 0x18, 255));
  for (int pin = 0; pin < PINS_PER_SIDE; pin++) {
    const float x = tabLeft + pitch * (pin + 0.5f) - pitch * 0.25f;
    draw->AddRectFilledMultiColor(ImVec2(x - pitch * 0.17f, mouth - 2.1f), ImVec2(x + pitch * 0.17f, mouth + 0.4f),
                                  IM_COL32(0xf4, 0xd6, 0x80, 255), IM_COL32(0xf4, 0xd6, 0x80, 255),
                                  IM_COL32(0x6a, 0x52, 0x1c, 255), IM_COL32(0x6a, 0x52, 0x1c, 255));
  }
}

void connectorFront(ImDrawList *draw, float tabLeft, float tabRight, float mouth) {
  const float pitch = (tabRight - tabLeft) / PINS_PER_SIDE;
  const float left = tabLeft - HOUSING_END;
  const float right = tabRight + HOUSING_END;
  const float face = mouth + 4.5f;
  const float foot = mouth + 11;
  // The near lip, over a card's tab, and its windows over the solder side's
  // contacts, a half pitch along from the far lip's.
  draw->AddRectFilled(ImVec2(tabLeft - CHANNEL_END, mouth + 1.6f), ImVec2(tabRight + CHANNEL_END, face), HOUSING_TOP);
  for (int pin = 0; pin < PINS_PER_SIDE; pin++) {
    const float x = tabLeft + pitch * (pin + 0.5f) + pitch * 0.25f;
    draw->AddRectFilled(ImVec2(x - pitch * 0.2f, mouth + 2.5f), ImVec2(x + pitch * 0.2f, mouth + 3.6f), IM_COL32(0x12, 0x11, 0x11, 255));
  }
  // The edge between the top and the face, catching the light, and the face
  // darkening down to the board.
  draw->AddRectFilledMultiColor(ImVec2(left, face), ImVec2(right, foot), IM_COL32(0x1c, 0x1b, 0x1a, 255),
                                IM_COL32(0x1c, 0x1b, 0x1a, 255), IM_COL32(0x08, 0x08, 0x08, 255), IM_COL32(0x08, 0x08, 0x08, 255));
  draw->AddLine(ImVec2(left + 1, face), ImVec2(right - 1, face), IM_COL32(255, 255, 255, 55), 1.0f);
  // Its ends, a little lighter on the left, where the light comes from.
  draw->AddLine(ImVec2(left + 0.5f, face), ImVec2(left + 0.5f, foot), IM_COL32(255, 255, 255, 18), 1.0f);
  draw->AddLine(ImVec2(right - 0.5f, face), ImVec2(right - 0.5f, foot), IM_COL32(0, 0, 0, 90), 1.0f);
  draw->AddLine(ImVec2(left, foot), ImVec2(right, foot), IM_COL32(0, 0, 0, 140), 1.0f);
}

// A card's shadow on the board, and the top edge of its board, which the eye
// sees from above: drawn round a card standing in its slot.
void cardDepth(ImDrawList *draw, const Stand &stand, ImVec2 at, ImVec2 size, bool locked) {
  const ImVec2 end(at.x + size.x, at.y + size.y);
  const float cut = 12;
  const ImVec2 outline[5] = {ImVec2(at.x + cut, at.y), ImVec2(end.x, at.y), ImVec2(end.x, end.y),
                             ImVec2(at.x, end.y), ImVec2(at.x, at.y + cut)};
  ImVec2 shadow[5];
  for (int i = 0; i < 5; i++) shadow[i] = stand.shadow(outline[i]);
  draw->AddConvexPolyFilled(shadow, 5, IM_COL32(0, 0, 0, 70));

  // The top edge and the chamfer, going back from the face.
  const float back = PCB_THICKNESS * std::sin(VIEW_ELEVATION);
  const ImU32 edge = locked ? IM_COL32(0x6a, 0x84, 0x72, 255) : IM_COL32(0x5e, 0xa4, 0x70, 255);
  const ImVec2 corner = stand(ImVec2(at.x, at.y + cut));
  const ImVec2 left = stand(ImVec2(at.x + cut, at.y));
  const ImVec2 right = stand(ImVec2(end.x, at.y));
  const ImVec2 strip[4] = {corner, left, ImVec2(left.x, left.y - back), ImVec2(corner.x, corner.y - back)};
  draw->AddConvexPolyFilled(strip, 4, edge);
  draw->AddRectFilled(ImVec2(left.x, left.y - back), ImVec2(right.x, right.y + 0.5f), edge);
}

// What is on each card, as the real one has it, left to right under the
// label: chips with their part numbers, and the odd crystal, battery, DIP
// switch or ribbon header.
enum class Part { Chip, BigChip, Crystal, Battery, Header, DipSwitch };
struct PartSpec {
  Part kind;
  const char *legend;
};

const std::vector<PartSpec> &partsFor(const std::string &id) {
  static const std::map<std::string, std::vector<PartSpec>> parts = {
      {"disk2", {{Part::Chip, "P5 PROM"}, {Part::Chip, "P6 PROM"}, {Part::Chip, "74LS259"}, {Part::Header, "DRIVE 1"}, {Part::Header, "DRIVE 2"}}},
      {"mockingboard", {{Part::BigChip, "AY-3-8910"}, {Part::BigChip, "AY-3-8910"}, {Part::BigChip, "R6522"}}},
      {"thunderclock", {{Part::Chip, "uPD1990"}, {Part::Crystal, ""}, {Part::Chip, "2716"}, {Part::Battery, ""}}},
      {"mouse", {{Part::BigChip, "MC6821"}, {Part::BigChip, "MC68705"}, {Part::Chip, "2716"}}},
      {"smartport", {{Part::Chip, "27C64"}, {Part::Chip, "74LS245"}, {Part::Chip, "74LS138"}, {Part::Header, "BUS"}}},
      {"softcard", {{Part::BigChip, "Z80A"}, {Part::Chip, "74LS74"}, {Part::Chip, "74LS367"}}},
      {"ssc", {{Part::BigChip, "SY6551"}, {Part::Chip, "2716"}, {Part::DipSwitch, "SW1"}, {Part::DipSwitch, "SW2"}, {Part::Crystal, ""}}},
      {"parallel", {{Part::Chip, "P1 PROM"}, {Part::Chip, "74LS374"}, {Part::Header, "PRINTER"}}},
      {"80col", {{Part::Chip, "4164"}, {Part::Chip, "4164"}, {Part::Chip, "4164"}, {Part::Chip, "4164"}}},
      {"languagecard", {{Part::Chip, "4116"}, {Part::Chip, "4116"}, {Part::Chip, "4116"}, {Part::Chip, "74LS175"}}},
      // A //c's ports are the SSC's ACIA with no card round it, and its disk an IWM.
      {"serial1", {{Part::BigChip, "6551 ACIA"}}},
      {"serial2", {{Part::BigChip, "6551 ACIA"}}},
      {"iwm", {{Part::BigChip, "IWM"}}},
      // A IIgs's own devices: the SCC behind both ports and AppleTalk, the
      // GLU for the ADB mouse, the IWM, and the Mega II's 80 columns.
      {"gs-printer", {{Part::BigChip, "Z8530 SCC"}}},
      {"gs-modem", {{Part::BigChip, "Z8530 SCC"}}},
      {"gs-mouse", {{Part::BigChip, "ADB GLU"}}},
      {"gs-smartport", {{Part::BigChip, "IWM"}, {Part::Chip, "ROM 01"}}},
      {"gs-drives", {{Part::BigChip, "IWM"}, {Part::Header, "DISK"}}},
      {"gs-appletalk", {{Part::BigChip, "Z8530 SCC"}, {Part::Header, "LOCALTALK"}}},
      {"gs-80col", {{Part::BigChip, "MEGA II"}}},
  };
  static const std::vector<PartSpec> generic = {{Part::Chip, "ROM"}, {Part::Chip, "74LS"}};
  auto it = parts.find(id);
  return it != parts.end() ? it->second : generic;
}

// Apple's own board numbers, and the makers' for theirs, as silkscreen.
const char *boardNumber(const std::string &id) {
  static const std::map<std::string, const char *> numbers = {
      {"disk2", "820-0026"},      {"mockingboard", "SWEET MICRO"}, {"thunderclock", "THUNDERWARE"},
      {"mouse", "820-0104"},      {"softcard", "MICROSOFT"},       {"ssc", "820-0118"},
      {"parallel", "820-0004"},   {"80col", "820-0067"},           {"languagecard", "820-0057"},
  };
  if (id.rfind("gs-", 0) == 0) return "APPLE IIGS";
  auto it = numbers.find(id);
  return it != numbers.end() ? it->second : "APPLE II";
}

ImU32 mixWithWhite(unsigned c, float t) {
  auto ch = [&](int shift) { return static_cast<int>(((c >> shift) & 0xFF) + (255 - ((c >> shift) & 0xFF)) * t); };
  return IM_COL32(ch(16), ch(8), ch(0), 255);
}

// A card, drawn flat, as an Apple II card looks from its component side in
// the machine, for standUp to stand in its slot: a green board standing on the gold tab at the right-hand end
// of its bottom edge, a chamfered corner at the other end of the top, the
// chips and parts it really carries running from the left, a paper label in
// the card's colour with its name, and its board number in silkscreen.
void card(ImDrawList *draw, ImVec2 at, ImVec2 size, const std::string &id, unsigned colour, const char *name,
          bool locked) {
  const ImVec2 end(at.x + size.x, at.y + size.y);
  const float tab = 8;   // how far the tab with the fingers reaches down
  const float cut = 12;  // the corner chamfer, at the end away from the tab
  const ImU32 board = locked ? IM_COL32(0x3a, 0x55, 0x44, 255) : IM_COL32(0x2a, 0x6a, 0x3c, 255);
  const ImU32 edge = locked ? IM_COL32(0x55, 0x70, 0x5e, 255) : IM_COL32(0x4c, 0x8e, 0x5c, 255);

  // The tab at the right-hand end of the bottom edge, with its gold fingers,
  // which goes into the slot's connector.
  const float tabLeft = end.x - size.x * TAB_SHARE;
  const float tabRight = end.x - TAB_INSET;
  draw->AddRectFilled(ImVec2(tabLeft, end.y - 1), ImVec2(tabRight, end.y + tab), board);
  // Twenty-five fingers, one for each contact on this side of the slot.
  const float pitch = (tabRight - tabLeft) / PINS_PER_SIDE;
  for (int finger = 0; finger < PINS_PER_SIDE; finger++) {
    const float x = tabLeft + pitch * (finger + 0.2f);
    draw->AddRectFilled(ImVec2(x, end.y + 1), ImVec2(x + pitch * 0.6f, end.y + tab), GOLD, 0.5f);
  }

  // The board: square over the connector, the top corner away from it cut.
  const ImVec2 outline[5] = {ImVec2(at.x + cut, at.y), ImVec2(end.x, at.y), ImVec2(end.x, end.y),
                             ImVec2(at.x, end.y), ImVec2(at.x, at.y + cut)};
  draw->AddConvexPolyFilled(outline, 5, board);
  // Traces under the solder mask, and its gloss.
  for (float y = at.y + 7; y < end.y - 4; y += 6) {
    draw->AddLine(ImVec2(at.x + 4, y), ImVec2(end.x - 6, y), IM_COL32(255, 255, 255, 10), 1.0f);
  }
  draw->AddRectFilledMultiColor(ImVec2(at.x + cut, at.y + 1), ImVec2(end.x - 1, at.y + size.y * 0.45f),
                                IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 0),
                                IM_COL32(255, 255, 255, 0));
  draw->AddPolyline(outline, 5, edge, ImDrawFlags_Closed, 1.0f);

  // The label: a paper sticker in the card's colour, its name on it.
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 nameSize = ImGui::CalcTextSize(name);
  const ImVec2 label(at.x + cut + 2, at.y + 5);
  const ImVec2 labelEnd(label.x + nameSize.x + 14, label.y + nameSize.y + 6);
  draw->AddRectFilled(ImVec2(label.x + 1, label.y + 1.5f), ImVec2(labelEnd.x + 1, labelEnd.y + 1.5f), IM_COL32(0, 0, 0, 70), 2.5f);
  draw->AddRectFilled(label, labelEnd, locked ? IM_COL32(0xc8, 0xcc, 0xc4, 255) : mixWithWhite(colour, 0.35f), 2.5f);
  draw->AddText(ImVec2(label.x + 7, label.y + 3), IM_COL32(24, 24, 24, 255), name);
  ImGui::PopFont();
  float x = labelEnd.x + 8;
  float topRowEnd = x; // where the label, and its padlock, end
  if (locked) {
    topRowEnd = x + 10;
    // A padlock after the label: it is part of the machine.
    const ImVec2 lock(x + 4, label.y + 2);
    draw->AddRectFilled(ImVec2(lock.x - 4, lock.y + 4), ImVec2(lock.x + 4, lock.y + 10), IM_COL32(255, 255, 255, 210), 1.5f);
    draw->PathArcTo(ImVec2(lock.x, lock.y + 4), 3.0f, IM_PI, IM_PI * 2, 10);
    draw->PathStroke(IM_COL32(255, 255, 255, 210), 0, 1.5f);
  }
  // How wide each part is drawn.
  auto partWidth = [](Part kind) {
    switch (kind) {
    case Part::BigChip: return 50.0f;
    case Part::Chip: return 34.0f;
    case Part::Crystal: return 16.0f;
    case Part::Battery: return 20.0f;
    case Part::Header: return 30.0f;
    case Part::DipSwitch: return 24.0f;
    }
    return 0.0f;
  };
  const std::vector<PartSpec> &parts = partsFor(id);
  // Ribbon headers go along the top edge at the right-hand end.
  float headerLeft = end.x - 2;
  for (const PartSpec &part : parts) {
    if (part.kind == Part::Header) headerLeft -= partWidth(Part::Header) + 2;
  }

  // The board number, in silkscreen along the top, between the label and
  // the headers, where there is room for it.
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.6f);
  const char *number = boardNumber(id);
  const float numberWidth = ImGui::CalcTextSize(number).x;
  const float numberX = std::min(headerLeft, end.x - 2) - numberWidth - 6;
  if (numberX > topRowEnd + 4) {
    draw->AddText(ImVec2(numberX, label.y + (labelEnd.y - label.y - ImGui::GetTextLineHeight()) * 0.5f),
                  IM_COL32(255, 255, 255, 120), number);
  }
  ImGui::PopFont();

  // The rest of the parts under the label, spread evenly across the whole
  // board, as many as fit.
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.55f);
  const float rowLeft = at.x + 8;
  const float rowRight = end.x - 6;
  float used = 0;
  int fitted = 0;
  for (const PartSpec &part : parts) {
    if (part.kind == Part::Header) continue;
    const float w = partWidth(part.kind);
    if (used + w + fitted * 6 > rowRight - rowLeft) break;
    used += w;
    fitted++;
  }
  const float gap = fitted ? (rowRight - rowLeft - used) / (fitted + 1) : 0;
  float left = rowLeft + gap;
  int placed = 0;
  const float middle = at.y + size.y * 0.69f; // under the label
  for (const PartSpec &part : parts) {
    const float w = partWidth(part.kind);
    if (part.kind != Part::Header && placed++ >= fitted) break;
    switch (part.kind) {
    case Part::Chip:
    case Part::BigChip: {
      const float h = part.kind == Part::BigChip ? 17.0f : 13.0f;
      const ImVec2 c(left, middle - h * 0.5f);
      // Its shadow on the card under it, and its top, which the eye sees
      // from above, with the pins along it.
      draw->AddRectFilled(ImVec2(c.x + 1, c.y + h), ImVec2(c.x + w + 1, c.y + h + 3), IM_COL32(0, 0, 0, 70), 1.0f);
      draw->AddRectFilled(ImVec2(c.x, c.y - 3), ImVec2(c.x + w, c.y + 1), IM_COL32(0x34, 0x34, 0x38, 255), 1.0f);
      for (float px = c.x + 2; px < c.x + w - 1; px += 3.2f) {
        draw->AddLine(ImVec2(px, c.y - 3), ImVec2(px, c.y - 1), IM_COL32(210, 210, 210, 200));
        draw->AddLine(ImVec2(px, c.y + h), ImVec2(px, c.y + h + 2), IM_COL32(210, 210, 210, 200));
      }
      draw->AddRectFilled(c, ImVec2(c.x + w, c.y + h), IM_COL32(0x16, 0x16, 0x18, 255), 1.0f);
      // The notch at pin 1's end, and the legend.
      draw->PathArcTo(ImVec2(c.x, c.y + h * 0.5f), 2.2f, -IM_PI * 0.5f, IM_PI * 0.5f, 8);
      draw->PathFillConvex(IM_COL32(0x40, 0x40, 0x44, 255));
      const ImVec2 legend = ImGui::CalcTextSize(part.legend);
      // Clipped as it is drawn: a clip rectangle would not stand up with it.
      const ImVec4 clip(c.x, c.y, c.x + w, c.y + h);
      draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(c.x + (w - legend.x) * 0.5f + 1, c.y + (h - legend.y) * 0.5f),
                    IM_COL32(255, 255, 255, 170), part.legend, nullptr, 0, &clip);
      break;
    }
    case Part::Crystal: {
      // A silver can.
      const ImVec2 c(left + 2, middle - 7);
      draw->AddRectFilled(c, ImVec2(c.x + 12, c.y + 14), IM_COL32(0xc8, 0xc8, 0xcc, 255), 6.0f);
      draw->AddRectFilled(ImVec2(c.x + 3, c.y + 2), ImVec2(c.x + 6, c.y + 12), IM_COL32(255, 255, 255, 120), 2.0f);
      break;
    }
    case Part::Battery: {
      // A coin cell.
      const ImVec2 c(left + w * 0.5f, middle);
      draw->AddCircleFilled(c, 9.0f, IM_COL32(0xb8, 0xb8, 0xbe, 255));
      draw->AddCircle(c, 9.0f, IM_COL32(0x80, 0x80, 0x86, 255), 0, 1.0f);
      draw->AddText(ImVec2(c.x - 3, c.y - 4), IM_COL32(60, 60, 64, 255), "+");
      break;
    }
    case Part::Header: {
      // A ribbon header along the board's top edge, its pins in two rows.
      const ImVec2 c(headerLeft, at.y + 3);
      draw->AddRectFilled(c, ImVec2(c.x + w - 4, c.y + 9), IM_COL32(0x10, 0x10, 0x10, 255), 1.0f);
      for (float px = c.x + 3; px < c.x + w - 6; px += 3.0f) {
        draw->AddRectFilled(ImVec2(px, c.y + 2), ImVec2(px + 1.2f, c.y + 3.2f), GOLD);
        draw->AddRectFilled(ImVec2(px, c.y + 5.5f), ImVec2(px + 1.2f, c.y + 6.7f), GOLD);
      }
      draw->AddText(ImVec2(c.x, c.y + 11), IM_COL32(255, 255, 255, 140), part.legend);
      headerLeft += w + 2; // a header takes no room from the chips below it
      continue;
    }
    case Part::DipSwitch: {
      // A red DIP switch with its white levers.
      const ImVec2 c(left, middle - 7);
      draw->AddRectFilled(c, ImVec2(c.x + w - 2, c.y + 14), IM_COL32(0xc0, 0x2a, 0x2a, 255), 1.5f);
      for (int s = 0; s < 4; s++) {
        const float sx = c.x + 3 + s * 5.0f;
        const bool on = (s * 7 + static_cast<int>(left)) % 3 == 0;
        draw->AddRectFilled(ImVec2(sx, c.y + (on ? 2 : 7)), ImVec2(sx + 3, c.y + (on ? 7 : 12)), IM_COL32(245, 245, 245, 255), 0.5f);
      }
      break;
    }
    }
    left += w + gap;
  }
  ImGui::PopFont();
}

// The parts a IIgs's built-in device is drawn with, by its name.
std::string builtInParts(const std::string &name) {
  static const std::map<std::string, std::string> ids = {
      {"Printer Port", "gs-printer"}, {"Modem Port", "gs-modem"},     {"Mouse", "gs-mouse"},
      {"SmartPort", "gs-smartport"},   {"5.25\" Drives", "gs-drives"}, {"AppleTalk", "gs-appletalk"},
      {"80-Column", "gs-80col"},
  };
  auto it = ids.find(name);
  return it != ids.end() ? it->second : "";
}

} // namespace

void ExpansionSlots::draw(bool *open) {
  if (!open || !*open || !machine_) {
    wasOpen_ = false;
    return;
  }
  // Each time the window opens it starts from what the machine has.
  if (!wasOpen_ && !dirty_) refreshFromMachine();
  wasOpen_ = true;

  ui::BeforeWindow("Expansion Slots");

  if (!ui::BeginWindow("Expansion Slots", open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  const bool iigs = machine_->family == MachineFamily::AppleIIgs;
  const bool sockets = machine_->caps.hasExpansionSlots;
  ImDrawList *draw = ImGui::GetWindowDrawList();
  if (!sockets) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + BOARD_WIDTH);
    ImGui::TextDisabled("The %s has no expansion sockets: everything in its slots is part of the machine.",
                        machine_->name);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  }

  // The motherboard.
  const int slots = machine_->lastSlot - machine_->firstSlot + 1;
  const bool clockRow = !iigs && sockets;
  const ImVec2 board = ImGui::GetCursorScreenPos();
  const float boardHeight = 28 + slots * ROW_HEIGHT + (clockRow ? 46 : 0) + 10;
  const ImVec2 boardEnd(board.x + BOARD_WIDTH, board.y + boardHeight);
  draw->AddRectFilled(board, boardEnd, PCB, 12.0f);
  // A ground plane round the edge, as a board has.
  draw->AddRect(ImVec2(board.x + 5, board.y + 5), ImVec2(boardEnd.x - 5, boardEnd.y - 5), PCB_DARK, 9.0f, 0, 3.0f);
  draw->AddRect(board, boardEnd, IM_COL32(0, 0, 0, 90), 12.0f, 0, 1.5f);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.72f);
  draw->AddText(ImVec2(board.x + 14, board.y + 8), SILK, (std::string(machine_->name) + "  MAIN LOGIC BOARD").c_str());
  ImGui::PopFont();

  for (int slot = machine_->firstSlot; slot <= machine_->lastSlot; slot++) {
    ImGui::PushID(slot);
    const float rowTop = board.y + 28 + (slot - machine_->firstSlot) * ROW_HEIGHT;
    // A card is drawn flat in front of its slot and stood up in it: where
    // it is drawn, and where it is seen, from its top corners to the mouth.
    const float mouth = rowTop + SLOT_MOUTH;
    const ImVec2 cardAt(board.x + CARD_X, mouth - CARD_RISE - CARD_HEIGHT);
    const ImVec2 cardSize(CARD_WIDTH, CARD_HEIGHT);
    const ImVec2 cardEnd(cardAt.x + CARD_WIDTH, cardAt.y + CARD_HEIGHT);
    const Stand stand{cardAt.x + CARD_WIDTH * 0.5f, mouth};
    const ImVec2 seenAt = stand(cardAt);
    const ImVec2 seenEnd(stand(ImVec2(cardEnd.x, cardAt.y)).x, mouth);
    const float tabLeft = cardAt.x + CARD_WIDTH * (1 - TAB_SHARE);
    const float tabRight = cardAt.x + CARD_WIDTH - TAB_INSET;
    // A card's shadow and the top of its board, the back of the slot's
    // connector, the card standing in it, and the connector's front.
    auto standCard = [&](bool locked, auto &&paint) {
      cardDepth(draw, stand, cardAt, cardSize, locked);
      connectorBack(draw, tabLeft, tabRight, mouth);
      standUp(draw, stand, paint);
      connectorFront(draw, tabLeft, tabRight, mouth);
    };
    auto outline = [&] { draw->AddRect(ImVec2(cardAt.x - 2, cardAt.y - 2), ImVec2(cardEnd.x + 2, cardEnd.y + 2),
                                       IM_COL32(255, 255, 255, 200), 5.0f, 0, 1.5f); };

    // The slot's number in silkscreen.
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.25f);
    const std::string number = std::to_string(slot);
    draw->AddText(ImVec2(board.x + 36 - ImGui::CalcTextSize(number.c_str()).x * 0.5f,
                         (seenAt.y + mouth - ImGui::GetTextLineHeight()) * 0.5f),
                  SILK, number.c_str());
    ImGui::PopFont();

    // Beside the card, clear of its shadow: what the slot is for, and where
    // it answers.
    const float infoX = seenEnd.x + 26;
    draw->AddText(ImVec2(infoX, rowTop + 2), SILK, slotNote(*machine_, slot).c_str());
    char where[32];
    std::snprintf(where, sizeof(where), "$C0%X0  $C%X00", 8 + slot, slot);
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
    draw->AddText(ImVec2(infoX, rowTop + ImGui::GetTextLineHeight() + 6), withAlpha(SILK, 0.55f), where);
    ImGui::PopFont();

    if (isFixedSlot(*machine_, slot)) {
      // A IIgs's fixed slots are its own devices, with its own chips: its
      // ports are the SCC, where a //c's are the ACIA.
      const char *fixed = machine_->slots[slot].fixedCard;
      std::string fixedId = fixed ? fixed : "";
      if (iigs) {
        if (const auto device = builtInDevice(*machine_, slot)) fixedId = builtInParts(*device);
      }
      standCard(true, [&] { card(draw, cardAt, cardSize, fixedId, 0x8b949e, fixedCardLabel(*machine_, slot).c_str(), true); });
      if (ui::IsHoveringRect(seenAt, seenEnd) &&
          ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("Part of the machine: it cannot be taken out.");
      }
    } else {
      std::string &current = working_[slot];
      const CardInfo *info = findCard(current);
      // The card, or the shape of one: click it to choose.
      ImGui::SetCursorScreenPos(seenAt);
      const bool clicked = ImGui::InvisibleButton("##slot", ImVec2(seenEnd.x - seenAt.x, seenEnd.y - seenAt.y));
      const bool hovered = ImGui::IsItemHovered();
      if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      // On a IIgs a slot the machine's own device answers holds that device,
      // drawn as a built-in card, as slot 3's 80 columns do with no card in it.
      std::optional<std::string> builtIn;
      if (iigs) {
        builtIn = builtInDevice(*machine_, slot);
        if (!builtIn && slot == 3) builtIn = "80-Column";
      }
      const bool answering = builtIn && (slot == 3 ? !info : emulation_.withMachine([&](host::MachineHost &host) {
                                           return host.isSlotInternal(slot);
                                         }));
      if (answering) {
        standCard(true, [&] {
          card(draw, cardAt, cardSize, builtInParts(*builtIn), 0x8b949e, (*builtIn + " (Built-in)").c_str(), true);
          if (hovered) outline();
        });
        if (hovered) {
          ImGui::SetTooltip("Part of the machine, and answering for this slot. Click to choose a card for its "
                            "socket; it answers once the slot is set to Your Card.");
        }
        // A card in the socket that the built-in device is answering over.
        if (info) {
          ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
          const std::string idle = std::string("IDLE: ") + info->name;
          const ImVec2 size = ImGui::CalcTextSize(idle.c_str());
          const ImVec2 chip(seenEnd.x - size.x - 24, seenAt.y - 6);
          draw->AddRectFilled(chip, ImVec2(chip.x + size.x + 10, chip.y + size.y + 4), rgb(info->color), 6.0f);
          draw->AddText(ImVec2(chip.x + 5, chip.y + 2), ui::textOn(rgb(info->color)), idle.c_str());
          ImGui::PopFont();
        }
      } else if (info) {
        standCard(false, [&] {
          card(draw, cardAt, cardSize, info->id, info->color, info->name, false);
          if (hovered) outline();
        });
      } else {
        // The outline of a card where one would stand, clear so the board
        // shows through it, and the whole of the empty connector over it,
        // its channel open and its contacts showing.
        standUp(draw, stand, [&] {
          const float cut = 12;
          const ImVec2 shape[5] = {ImVec2(cardAt.x + cut, cardAt.y), ImVec2(cardEnd.x, cardAt.y), cardEnd,
                                   ImVec2(cardAt.x, cardEnd.y), ImVec2(cardAt.x, cardAt.y + cut)};
          dashedOutline(draw, shape, 5, hovered ? IM_COL32(255, 255, 255, 220) : withAlpha(SILK, 0.35f));
          const char *empty = hovered ? "+  Add a card" : "Empty";
          draw->AddText(ImVec2(cardAt.x + 12, cardAt.y + (CARD_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f),
                        hovered ? IM_COL32(255, 255, 255, 230) : withAlpha(SILK, 0.5f), empty);
        });
        connectorBack(draw, tabLeft, tabRight, mouth);
        connectorFront(draw, tabLeft, tabRight, mouth);
      }
      // Changed and not yet fitted.
      auto was = applied_.find(slot);
      if (was != applied_.end() && was->second != current) {
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
        const char *pending = "ON RESET";
        const ImVec2 size = ImGui::CalcTextSize(pending);
        const ImVec2 chip(seenEnd.x - size.x - 14, seenAt.y - 6);
        draw->AddRectFilled(chip, ImVec2(chip.x + size.x + 10, chip.y + size.y + 4), IM_COL32(0xfd, 0xb8, 0x27, 255), 6.0f);
        draw->AddText(ImVec2(chip.x + 5, chip.y + 2), IM_COL32(30, 24, 10, 255), pending);
        ImGui::PopFont();
      }

      if (clicked) ImGui::OpenPopup("##choose");
      ImGui::SetNextWindowPos(ImVec2(seenAt.x, mouth + 14));
      if (ImGui::BeginPopup("##choose")) {
        ImGui::TextDisabled("Slot %d  ·  %s", slot, slotNote(*machine_, slot).c_str());
        ImGui::Separator();
        auto option = [&](const char *id, const char *name, unsigned colour) {
          ImGui::PushID(id);
          const bool chosen = current == id;
          const ImVec2 at = ImGui::GetCursorScreenPos();
          if (ImGui::Selectable("##option", chosen, 0, ImVec2(220, ImGui::GetFrameHeight()))) {
            dirty_ |= current != id;
            current = id;
          }
          ImDrawList *popup = ImGui::GetWindowDrawList();
          const float mid = at.y + ImGui::GetFrameHeight() * 0.5f;
          if (colour) popup->AddRectFilled(ImVec2(at.x + 4, mid - 7), ImVec2(at.x + 18, mid + 7), rgb(colour), 3.0f);
          else dashedRect(popup, ImVec2(at.x + 4, mid - 7), ImVec2(at.x + 18, mid + 7), ImGui::GetColorU32(ImGuiCol_TextDisabled));
          popup->AddText(ImVec2(at.x + 28, mid - ImGui::GetTextLineHeight() * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), name);
          ImGui::PopID();
        };
        option("empty", "Empty", 0);
        const std::vector<std::string> used = cardsInUse(working_, slot);
        for (const std::string &id : slotOffers(*machine_, slot)) {
          // Each card once: one already in another slot is not offered.
          if (std::find(used.begin(), used.end(), id) != used.end()) continue;
          if (const CardInfo *offer = findCard(id)) option(offer->id, offer->name, offer->color);
        }
        ImGui::EndPopup();
      }
    }

    // A IIgs slot is its built-in device or the socket; the Control Panel's
    // choice, in $C02D, at once and with no reset.
    if (iigs) {
      if (const auto builtIn = builtInDevice(*machine_, slot)) {
        const bool internal = emulation_.withMachine([&](host::MachineHost &host) { return host.isSlotInternal(slot); });
        const char *choices[] = {builtIn->c_str(), "Your Card"};
        int choice = internal ? 0 : 1;
        ImGui::SetCursorScreenPos(ImVec2(boardEnd.x - 14 - 150, rowTop + 4));
        ImGui::SetNextItemWidth(150);
        if (ui::PopUpButton("##answers", &choice, choices, 2)) {
          emulation_.withMachine([&](host::MachineHost &host) { host.setSlotInternal(slot, choice == 0); });
          // Remembered, or a machine started again (or rebuilt) put the
          // firmware's choice back and the user's card stopped answering.
          sockets_[machine_->key][slot] = choice == 1;
          // Written under the machine's section, which a machine whose cards
          // were never changed does not have yet: it has what it has now.
          if (!saved_.count(machine_->key)) saved_[machine_->key] = applied_;
          ImGui::MarkIniSettingsDirty();
        }
      }
    }
    ImGui::PopID();
  }

  // The No-Slot Clock: a chip under the ROM, needing no slot.
  if (clockRow) {
    const float y = board.y + 28 + slots * ROW_HEIGHT + 4;
    const ImVec2 chip(board.x + CARD_X + 10, y + 4);
    for (float px = chip.x + 3; px < chip.x + 40; px += 4) {
      draw->AddLine(ImVec2(px, chip.y - 2), ImVec2(px, chip.y), IM_COL32(200, 200, 200, 180));
      draw->AddLine(ImVec2(px, chip.y + 22), ImVec2(px, chip.y + 24), IM_COL32(200, 200, 200, 180));
    }
    draw->AddRectFilled(chip, ImVec2(chip.x + 44, chip.y + 22), noSlotClock ? IM_COL32(0x1a, 0x1a, 0x1c, 255)
                                                                            : IM_COL32(0x1a, 0x1a, 0x1c, 90), 2.0f);
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.62f);
    draw->AddText(ImVec2(chip.x + 4, chip.y + 6), IM_COL32(255, 255, 255, noSlotClock ? 170 : 60), "DS1215");
    ImGui::PopFont();
    draw->AddText(ImVec2(chip.x + 58, chip.y + 3), SILK, "No-Slot Clock");
    ImGui::SetCursorScreenPos(ImVec2(boardEnd.x - 14 - ui::SwitchWidth("##nsc"), chip.y));
    if (ui::Switch("##nsc", &noSlotClock)) {
      emulation_.withMachine([&](host::MachineHost &host) { host.setNoSlotClock(noSlotClock); });
      ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("A ProDOS clock under the $C300 ROM, needing no slot. Takes effect at once.");
    }
  }
  ImGui::SetCursorScreenPos(board);
  ImGui::Dummy(ImVec2(BOARD_WIDTH, boardHeight));

  if (sockets) {
    ImGui::Spacing();
    ImGui::BeginDisabled(!dirty_);
    if (ui::Button("Apply & Reset", ImVec2(0, 0), ui::ButtonKind::Primary)) applyAndReset();
    ImGui::SameLine();
    if (ui::Button("Revert")) refreshFromMachine();
    ImGui::EndDisabled();
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(dirty_ ? "The machine resets to fit the cards." : "Click a slot to choose its card.");
  }
  ImGui::End();
}

} // namespace a2e::native
