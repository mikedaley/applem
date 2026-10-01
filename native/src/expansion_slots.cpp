/*
 * expansion_slots.cpp - The Expansion Slots window, and applying a layout
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "expansion_slots.hpp"
#include "ui_controls.hpp"

#include "emulation.hpp"

#include "machine/machine_profile.hpp"

#include "imgui.h"
#include "imgui_internal.h" // MarkIniSettingsDirty

#include <algorithm>

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
  dirty_ = false;
}

void ExpansionSlots::applyAndReset() {
  saved_[machine_->key] = working_;
  ImGui::MarkIniSettingsDirty();
  emulation_.withMachine([&](host::MachineHost &host) {
    for (const auto &[slot, card] : working_) host.setSlotCard(slot, card);
    host.reset();
  });
  dirty_ = false;
  if (onApplied_) onApplied_();
}

void ExpansionSlots::draw(bool *open) {
  if (!open || !*open || !machine_) {
    wasOpen_ = false;
    return;
  }
  // Each time the window opens it starts from what the machine has.
  if (!wasOpen_ && !dirty_) refreshFromMachine();
  wasOpen_ = true;

  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Expansion Slots", open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  const bool iigs = machine_->family == MachineFamily::AppleIIgs;
  if (!machine_->caps.hasExpansionSlots) {
    ImGui::TextWrapped("The %s has no expansion sockets: everything in its slots is part of the machine.",
                       machine_->name);
    ImGui::Spacing();
  }

  if (ImGui::BeginTable("slots", iigs ? 4 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("Use");
    ImGui::TableSetupColumn("Card", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    if (iigs) ImGui::TableSetupColumn("Answers", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableHeadersRow();

    for (int slot = machine_->firstSlot; slot <= machine_->lastSlot; slot++) {
      ImGui::PushID(slot);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%d", slot);
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", slotNote(*machine_, slot).c_str());
      ImGui::TableNextColumn();

      if (isFixedSlot(*machine_, slot)) {
        ImGui::TextUnformatted(fixedCardLabel(*machine_, slot).c_str());
      } else {
        std::string &current = working_[slot];
        const CardInfo *info = findCard(current);
        const char *label = info ? info->name : "Empty";
        ImGui::SetNextItemWidth(-1);
        if (ui::BeginPopUpButton("##card", label)) {
          if (ImGui::Selectable("Empty", current == "empty")) {
            dirty_ |= current != "empty";
            current = "empty";
          }
          const std::vector<std::string> used = cardsInUse(working_, slot);
          for (const std::string &id : slotOffers(*machine_, slot)) {
            // Each card once: one already in another slot is not offered.
            if (std::find(used.begin(), used.end(), id) != used.end()) continue;
            const CardInfo *card = findCard(id);
            if (!card) continue;
            const ImVec4 accent(((card->color >> 16) & 0xFF) / 255.0f, ((card->color >> 8) & 0xFF) / 255.0f,
                                (card->color & 0xFF) / 255.0f, 1.0f);
            ImGui::ColorButton("##accent", accent, ImGuiColorEditFlags_NoTooltip, ImVec2(8, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            if (ImGui::Selectable(card->name, current == id)) {
              dirty_ |= current != id;
              current = id;
            }
          }
          ui::EndPopUpButton();
        }
      }

      if (iigs) {
        ImGui::TableNextColumn();
        if (const auto builtIn = builtInDevice(*machine_, slot)) {
          // The Control Panel's choice, in $C02D: at once, no reset.
          const bool internal = emulation_.withMachine([&](host::MachineHost &host) { return host.isSlotInternal(slot); });
          const char *choices[] = {builtIn->c_str(), "Your Card"};
          int choice = internal ? 0 : 1;
          ImGui::SetNextItemWidth(-1);
          if (ui::PopUpButton("##answers", &choice, choices, 2)) {
            emulation_.withMachine([&](host::MachineHost &host) { host.setSlotInternal(slot, choice == 0); });
          }
        } else {
          ImGui::TextDisabled("Always the slot");
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (!iigs && machine_->caps.hasExpansionSlots) {
    ImGui::Spacing();
    if (ui::Switch("No-Slot Clock (DS1215)", &noSlotClock)) {
      emulation_.withMachine([&](host::MachineHost &host) { host.setNoSlotClock(noSlotClock); });
      ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("A ProDOS clock under the $C300 ROM, needing no slot. Takes effect at once.");
    }
  }

  if (machine_->caps.hasExpansionSlots) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::BeginDisabled(!dirty_);
    if (ui::Button("Apply & Reset", ImVec2(0, 0), ui::ButtonKind::Primary)) applyAndReset();
    ImGui::SameLine();
    if (ui::Button("Revert")) refreshFromMachine();
    ImGui::EndDisabled();
    if (dirty_) {
      ImGui::SameLine();
      ImGui::TextDisabled("The machine resets to fit the cards.");
    }
  }
  ImGui::End();
}

} // namespace a2e::native
