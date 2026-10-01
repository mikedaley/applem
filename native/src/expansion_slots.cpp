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

namespace a2e::native {

ExpansionSlots::ExpansionSlots(Emulation &emulation) : emulation_(emulation) {}

SlotLayout *ExpansionSlots::openSection(const char *machine) {
  if (!findMachineProfile(machine)) return nullptr;
  SlotLayout &layout = saved_[machine];
  layout.clear();
  return &layout;
}

void ExpansionSlots::readLine(SlotLayout *layout, const char *line) {
  int slot = 0;
  std::string card;
  if (layout && parseSlotLine(line, slot, card)) (*layout)[slot] = card;
}

void ExpansionSlots::writeAll(ImGuiTextBuffer *out, const char *typeName) const {
  for (const auto &[machine, layout] : saved_) {
    out->appendf("[%s][%s]\n", typeName, machine.c_str());
    for (const auto &[slot, card] : layout) out->appendf("%s\n", formatSlotLine(slot, card).c_str());
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
  emulation_.withMachine([&](host::MachineHost &host) {
    for (const auto &[slot, card] : layout) {
      if (!isFixedSlot(*machine_, slot)) host.setSlotCard(slot, card);
    }
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

void ExpansionSlots::applyAndReset() {
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
constexpr float BOARD_WIDTH = 560;
constexpr float ROW_HEIGHT = 50;
constexpr float CONNECTOR_X = 54;   // from the board's left
constexpr float CARD_WIDTH = 230;
constexpr ImU32 PCB = IM_COL32(0x1f, 0x5a, 0x3a, 255);
constexpr ImU32 PCB_DARK = IM_COL32(0x16, 0x45, 0x2c, 255);
constexpr ImU32 SILK = IM_COL32(0xe8, 0xf0, 0xe0, 210);
constexpr ImU32 GOLD = IM_COL32(0xd8, 0xb0, 0x4a, 255);

ImU32 rgb(unsigned c, float alpha = 1.0f) {
  return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, static_cast<int>(255 * alpha));
}
ImU32 shade(unsigned c, float k) {
  auto ch = [&](int shift) { return std::clamp(static_cast<int>(((c >> shift) & 0xFF) * k), 0, 255); };
  return IM_COL32(ch(16), ch(8), ch(0), 255);
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

// A slot's edge connector, seen from above: a black block with its contacts.
void connector(ImDrawList *draw, ImVec2 at, float height) {
  draw->AddRectFilled(at, ImVec2(at.x + 16, at.y + height), IM_COL32(0x12, 0x12, 0x12, 255), 2.0f);
  draw->AddRectFilled(ImVec2(at.x + 6, at.y + 3), ImVec2(at.x + 10, at.y + height - 3), IM_COL32(0x05, 0x05, 0x05, 255));
  for (float y = at.y + 5; y < at.y + height - 4; y += 4) {
    draw->AddLine(ImVec2(at.x + 3, y), ImVec2(at.x + 5, y), GOLD, 1.0f);
    draw->AddLine(ImVec2(at.x + 11, y), ImVec2(at.x + 13, y), GOLD, 1.0f);
  }
}

// A card standing in its slot: its board in the card's colour, gold fingers
// in the connector, a few chips, and its name printed on it.
void card(ImDrawList *draw, ImVec2 at, ImVec2 size, unsigned colour, const char *name, int chips, bool locked) {
  const ImVec2 end(at.x + size.x, at.y + size.y);
  draw->AddRectFilled(ImVec2(at.x + 2, at.y + 3), ImVec2(end.x + 2, end.y + 3), IM_COL32(0, 0, 0, 70), 4.0f);
  draw->AddRectFilled(at, end, shade(colour, locked ? 0.45f : 0.62f), 4.0f);
  draw->AddRectFilledMultiColor(at, ImVec2(end.x, at.y + size.y * 0.5f), IM_COL32(255, 255, 255, 26),
                                IM_COL32(255, 255, 255, 26), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
  draw->AddRect(at, end, shade(colour, 0.95f), 4.0f, 0, 1.0f);
  // The fingers, at the end that goes into the connector.
  for (float y = at.y + 5; y < end.y - 4; y += 4) draw->AddRectFilled(ImVec2(at.x - 3, y), ImVec2(at.x + 4, y + 2), GOLD);
  // Chips along the far end, each with its legs.
  float x = end.x - 10;
  for (int i = 0; i < chips; i++) {
    const float w = i == 0 ? 26.0f : 18.0f;
    const ImVec2 c(x - w, at.y + size.y * 0.5f - 7);
    for (float px = c.x + 2; px < c.x + w - 1; px += 3.5f) {
      draw->AddLine(ImVec2(px, c.y - 2), ImVec2(px, c.y), IM_COL32(200, 200, 200, 180));
      draw->AddLine(ImVec2(px, c.y + 14), ImVec2(px, c.y + 16), IM_COL32(200, 200, 200, 180));
    }
    draw->AddRectFilled(c, ImVec2(c.x + w, c.y + 14), IM_COL32(0x1a, 0x1a, 0x1c, 255), 1.5f);
    draw->AddCircleFilled(ImVec2(c.x + 3, c.y + 3), 1.0f, IM_COL32(255, 255, 255, 60));
    x -= w + 8;
  }
  // The name, as silkscreen.
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.92f);
  const ImVec2 textSize = ImGui::CalcTextSize(name);
  draw->PushClipRect(ImVec2(at.x + 10, at.y), ImVec2(x + 4, end.y), true);
  draw->AddText(ImVec2(at.x + 12, at.y + (size.y - textSize.y) * 0.5f), IM_COL32(255, 255, 255, 235), name);
  draw->PopClipRect();
  ImGui::PopFont();
  if (locked) {
    // A padlock after the name: it is part of the machine.
    const ImVec2 lock(std::min(at.x + 12 + textSize.x + 14, x - 6), at.y + size.y * 0.5f - 6);
    draw->AddRectFilled(ImVec2(lock.x - 4, lock.y + 3), ImVec2(lock.x + 4, lock.y + 9), IM_COL32(255, 255, 255, 200), 1.5f);
    draw->PathArcTo(ImVec2(lock.x, lock.y + 3), 3.0f, IM_PI, IM_PI * 2, 10);
    draw->PathStroke(IM_COL32(255, 255, 255, 200), 0, 1.5f);
  }
}

int chipsFor(const std::string &id) {
  unsigned h = 0;
  for (char c : id) h = h * 31 + static_cast<unsigned char>(c);
  return 2 + static_cast<int>(h % 3);
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

  if (!ImGui::Begin("Expansion Slots", open, ImGuiWindowFlags_AlwaysAutoResize)) {
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
    const float cardHeight = ROW_HEIGHT - 16;
    const ImVec2 cardAt(board.x + CONNECTOR_X + 10, rowTop + 2);

    // The slot's number in silkscreen, and its connector.
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.25f);
    const std::string number = std::to_string(slot);
    draw->AddText(ImVec2(board.x + 36 - ImGui::CalcTextSize(number.c_str()).x * 0.5f,
                         rowTop + (cardHeight - ImGui::GetTextLineHeight()) * 0.5f + 2),
                  SILK, number.c_str());
    ImGui::PopFont();
    connector(draw, ImVec2(board.x + CONNECTOR_X, rowTop), cardHeight + 4);

    // Beside the card: what the slot is for, and where it answers.
    const float infoX = cardAt.x + CARD_WIDTH + 14;
    draw->AddText(ImVec2(infoX, rowTop + 2), SILK, slotNote(*machine_, slot).c_str());
    char where[32];
    std::snprintf(where, sizeof(where), "$C0%X0  $C%X00", 8 + slot, slot);
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
    draw->AddText(ImVec2(infoX, rowTop + ImGui::GetTextLineHeight() + 6), withAlpha(SILK, 0.55f), where);
    ImGui::PopFont();

    if (isFixedSlot(*machine_, slot)) {
      card(draw, cardAt, ImVec2(CARD_WIDTH, cardHeight), 0x8b949e, fixedCardLabel(*machine_, slot).c_str(), 2, true);
      if (ImGui::IsMouseHoveringRect(cardAt, ImVec2(cardAt.x + CARD_WIDTH, cardAt.y + cardHeight)) &&
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
      if (info) {
        card(draw, cardAt, ImVec2(CARD_WIDTH, cardHeight), info->color, info->name, chipsFor(info->id), false);
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
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.75f);
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
        }
      }
    }
    ImGui::PopID();
  }

  // The No-Slot Clock: a chip under the ROM, needing no slot.
  if (clockRow) {
    const float y = board.y + 28 + slots * ROW_HEIGHT + 4;
    const ImVec2 chip(board.x + CONNECTOR_X + 10, y + 4);
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
