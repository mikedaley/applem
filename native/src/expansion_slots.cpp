/*
 * expansion_slots.cpp - The Expansion Slots window, and applying a layout
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "expansion_slots.hpp"
#include "ui_controls.hpp"

#include "emulation.hpp"
#include "ui_theme.hpp"

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

void dashedRect(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour) {
  auto line = [&](ImVec2 p, ImVec2 q) {
    const float length = std::hypot(q.x - p.x, q.y - p.y);
    const ImVec2 d((q.x - p.x) / length, (q.y - p.y) / length);
    for (float t = 0; t < length; t += 8) {
      const float u = std::min(t + 4.5f, length);
      draw->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * u, p.y + d.y * u), colour, 1.2f);
    }
  };
  line(a, ImVec2(b.x, a.y));
  line(ImVec2(b.x, a.y), b);
  line(b, ImVec2(a.x, b.y));
  line(ImVec2(a.x, b.y), a);
}

// A slot's edge connector, from the side: a black body along the bottom of
// the row, under the right-hand end of the card where its tab goes, and its
// fifty pins, twenty-five a side, coming out under it as two staggered rows,
// at the same pitch as a card's fingers.
void connector(ImDrawList *draw, float tabLeft, float tabRight, float y) {
  const float pitch = (tabRight - tabLeft) / PINS_PER_SIDE;
  draw->AddRectFilled(ImVec2(tabLeft - 6, y + 2), ImVec2(tabRight + 6, y + 12), IM_COL32(0x10, 0x10, 0x10, 255), 2.0f);
  draw->AddRectFilled(ImVec2(tabLeft - 9, y + 8), ImVec2(tabRight + 9, y + 12), IM_COL32(0x08, 0x08, 0x08, 255), 1.0f);
  for (int pin = 0; pin < PINS_PER_SIDE; pin++) {
    const float x = tabLeft + pitch * (pin + 0.5f);
    // Component side, then solder side, a quarter pitch either way.
    draw->AddLine(ImVec2(x - pitch * 0.25f, y + 12), ImVec2(x - pitch * 0.25f, y + 15), IM_COL32(0xd0, 0xd0, 0xd0, 170), 1.0f);
    draw->AddLine(ImVec2(x + pitch * 0.25f, y + 12), ImVec2(x + pitch * 0.25f, y + 17), IM_COL32(0xa8, 0xa8, 0xa8, 150), 1.0f);
  }
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

// A card in its slot, as an Apple II card looks from its component side in
// the machine: a green board standing on the gold tab at the right-hand end
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
  ImVec2 shadow[5];
  for (int i = 0; i < 5; i++) shadow[i] = ImVec2(outline[i].x + 2, outline[i].y + 3);
  draw->AddConvexPolyFilled(shadow, 5, IM_COL32(0, 0, 0, 80));
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
      for (float px = c.x + 2; px < c.x + w - 1; px += 3.2f) {
        draw->AddLine(ImVec2(px, c.y - 2), ImVec2(px, c.y), IM_COL32(210, 210, 210, 200));
        draw->AddLine(ImVec2(px, c.y + h), ImVec2(px, c.y + h + 2), IM_COL32(210, 210, 210, 200));
      }
      draw->AddRectFilled(c, ImVec2(c.x + w, c.y + h), IM_COL32(0x16, 0x16, 0x18, 255), 1.0f);
      // The notch at pin 1's end, and the legend.
      draw->PathArcTo(ImVec2(c.x, c.y + h * 0.5f), 2.2f, -IM_PI * 0.5f, IM_PI * 0.5f, 8);
      draw->PathFillConvex(IM_COL32(0x40, 0x40, 0x44, 255));
      const ImVec2 legend = ImGui::CalcTextSize(part.legend);
      draw->PushClipRect(c, ImVec2(c.x + w, c.y + h), true);
      draw->AddText(ImVec2(c.x + (w - legend.x) * 0.5f + 1, c.y + (h - legend.y) * 0.5f), IM_COL32(255, 255, 255, 170), part.legend);
      draw->PopClipRect();
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
    const float cardHeight = ROW_HEIGHT - 24;
    const ImVec2 cardAt(board.x + CARD_X, rowTop + 2);

    // The slot's number in silkscreen, and its connector.
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.25f);
    const std::string number = std::to_string(slot);
    draw->AddText(ImVec2(board.x + 36 - ImGui::CalcTextSize(number.c_str()).x * 0.5f,
                         rowTop + (cardHeight - ImGui::GetTextLineHeight()) * 0.5f + 2),
                  SILK, number.c_str());
    ImGui::PopFont();
    // The slot's connector along the bottom of the row, under where a card's
    // tab goes, whether or not there is a card in it.
    connector(draw, cardAt.x + CARD_WIDTH * (1 - TAB_SHARE), cardAt.x + CARD_WIDTH - TAB_INSET, cardAt.y + cardHeight + 2);

    // Beside the card: what the slot is for, and where it answers.
    const float infoX = cardAt.x + CARD_WIDTH + 14;
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
      card(draw, cardAt, ImVec2(CARD_WIDTH, cardHeight), fixedId, 0x8b949e, fixedCardLabel(*machine_, slot).c_str(), true);
      if (ui::IsHoveringRect(cardAt, ImVec2(cardAt.x + CARD_WIDTH, cardAt.y + cardHeight)) &&
          ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("Part of the machine: it cannot be taken out.");
      }
    } else {
      std::string &current = working_[slot];
      const CardInfo *info = findCard(current);
      // The card, or the shape of one: click it to choose.
      ImGui::SetCursorScreenPos(cardAt);
      const bool clicked = ImGui::InvisibleButton("##slot", ImVec2(CARD_WIDTH, cardHeight));
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
        card(draw, cardAt, ImVec2(CARD_WIDTH, cardHeight), builtInParts(*builtIn), 0x8b949e,
             (*builtIn + " (Built-in)").c_str(), true);
        if (hovered) {
          draw->AddRect(ImVec2(cardAt.x - 2, cardAt.y - 2), ImVec2(cardAt.x + CARD_WIDTH + 2, cardAt.y + cardHeight + 2),
                        IM_COL32(255, 255, 255, 200), 5.0f, 0, 1.5f);
          ImGui::SetTooltip("Part of the machine, and answering for this slot. Click to choose a card for its "
                            "socket; it answers once the slot is set to Your Card.");
        }
        // A card in the socket that the built-in device is answering over.
        if (info) {
          ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
          const std::string idle = std::string("IDLE: ") + info->name;
          const ImVec2 size = ImGui::CalcTextSize(idle.c_str());
          const ImVec2 chip(cardAt.x + CARD_WIDTH - size.x - 24, cardAt.y - 6);
          draw->AddRectFilled(chip, ImVec2(chip.x + size.x + 10, chip.y + size.y + 4), rgb(info->color), 6.0f);
          draw->AddText(ImVec2(chip.x + 5, chip.y + 2), ui::textOn(rgb(info->color)), idle.c_str());
          ImGui::PopFont();
        }
      } else if (info) {
        card(draw, cardAt, ImVec2(CARD_WIDTH, cardHeight), info->id, info->color, info->name, false);
        if (hovered) draw->AddRect(ImVec2(cardAt.x - 2, cardAt.y - 2), ImVec2(cardAt.x + CARD_WIDTH + 2, cardAt.y + cardHeight + 2),
                                   IM_COL32(255, 255, 255, 200), 5.0f, 0, 1.5f);
      } else {
        dashedRect(draw, cardAt, ImVec2(cardAt.x + CARD_WIDTH, cardAt.y + cardHeight),
                   hovered ? IM_COL32(255, 255, 255, 220) : withAlpha(SILK, 0.35f));
        const char *empty = hovered ? "+  Add a card" : "Empty";
        draw->AddText(ImVec2(cardAt.x + 12, cardAt.y + (cardHeight - ImGui::GetTextLineHeight()) * 0.5f),
                      hovered ? IM_COL32(255, 255, 255, 230) : withAlpha(SILK, 0.5f), empty);
      }
      // Changed and not yet fitted.
      auto was = applied_.find(slot);
      if (was != applied_.end() && was->second != current) {
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
        const char *pending = "ON RESET";
        const ImVec2 size = ImGui::CalcTextSize(pending);
        const ImVec2 chip(cardAt.x + CARD_WIDTH - size.x - 14, cardAt.y - 6);
        draw->AddRectFilled(chip, ImVec2(chip.x + size.x + 10, chip.y + size.y + 4), IM_COL32(0xfd, 0xb8, 0x27, 255), 6.0f);
        draw->AddText(ImVec2(chip.x + 5, chip.y + 2), IM_COL32(30, 24, 10, 255), pending);
        ImGui::PopFont();
      }

      if (clicked) ImGui::OpenPopup("##choose");
      ImGui::SetNextWindowPos(ImVec2(cardAt.x, cardAt.y + cardHeight + 4));
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
