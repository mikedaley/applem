/*
 * slot_layout.cpp - Which cards are in which slots, and what each slot takes
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "slot_layout.hpp"

#include "machine/machine_profile.hpp"

#include <cstdio>
#include <cstring>

namespace a2e::native {

namespace {

struct SlotUI {
  std::vector<std::string> available;
  const char *note;
};

// slot-configuration-window.js's SLOT_UI.
const std::map<int, SlotUI> &slotUI() {
  static const std::map<int, SlotUI> table = {
      {0, {{}, "16K RAM"}},
      {1, {{"parallel", "ssc", "softcard"}, "Printer"}},
      {2, {{"parallel", "ssc", "smartport", "softcard"}, "Modem / Serial"}},
      {3, {{"parallel", "ssc", "smartport", "softcard"}, "80-Column / Serial"}},
      {4, {{"mockingboard", "mouse", "smartport", "softcard"}, "Mouse / Sound"}},
      {5, {{"thunderclock", "smartport", "softcard"}, "3.5\" Drives / Clock"}},
      {6, {{"disk2"}, "5.25\" Drives"}},
      {7, {{"thunderclock", "smartport", "softcard"}, "Hard Disk / Clock"}},
  };
  return table;
}

// A IIgs's sockets each take any of these.
const std::vector<std::string> IIGS_CARDS = {"mockingboard", "mouse", "thunderclock",
                                             "ssc", "parallel", "smartport"};

// IIGS_SLOT_UI: the built-in device each slot has besides its socket.
const std::map<int, const char *> &iigsBuiltIn() {
  static const std::map<int, const char *> table = {
      {1, "Printer Port"}, {2, "Modem Port"}, {4, "Mouse"},
      {5, "SmartPort"},    {6, "5.25\" Drives"}, {7, "AppleTalk"},
  };
  return table;
}

// FIXED_CARD_LABELS.
const std::map<std::string, const char *> &fixedLabels() {
  static const std::map<std::string, const char *> table = {
      {"80col", "80-Column (Built-in)"},
      {"languagecard", "Language Card (16K)"},
      {"serial1", "Printer Port (Built-in)"},
      {"serial2", "Modem Port (Built-in)"},
      {"mouse", "Mouse (Built-in)"},
      {"iwm", "5.25\" Drives (Built-in)"},
  };
  return table;
}

} // namespace

const std::vector<CardInfo> &cards() {
  // An accent each, from the rainbow logo's palette, as the browser's tray
  // gives each card a colour.
  static const std::vector<CardInfo> list = {
      {"disk2", "Disk II", 0x61BB46},        {"mockingboard", "Mockingboard", 0x963D97},
      {"thunderclock", "Thunderclock", 0xF5821F}, {"mouse", "Mouse Card", 0x009DDC},
      {"smartport", "SmartPort", 0xE03A3E},  {"softcard", "Z-80 SoftCard", 0x2BB5B5},
      {"ssc", "Super Serial Card", 0xFDB827}, {"parallel", "Parallel Card", 0x3AA597},
  };
  return list;
}

const CardInfo *findCard(const std::string &id) {
  for (const CardInfo &card : cards()) {
    if (id == card.id) return &card;
  }
  return nullptr;
}

SlotLayout defaultLayout(const MachineProfile &machine) {
  SlotLayout layout;
  for (int slot = machine.firstSlot; slot <= machine.lastSlot; slot++) {
    const MachineSlot &s = machine.slots[slot];
    if (s.fixedCard) continue;
    if (s.defaultCard) layout[slot] = s.defaultCard;
  }
  return layout;
}

bool isFixedSlot(const MachineProfile &machine, int slot) {
  return slot >= machine.firstSlot && slot <= machine.lastSlot && machine.slots[slot].fixedCard;
}

std::string fixedCardLabel(const MachineProfile &machine, int slot) {
  if (!isFixedSlot(machine, slot)) return "";
  const std::string id = machine.slots[slot].fixedCard;
  auto it = fixedLabels().find(id);
  return it != fixedLabels().end() ? it->second : id;
}

std::vector<std::string> slotOffers(const MachineProfile &machine, int slot) {
  if (machine.family == MachineFamily::AppleIIgs) return IIGS_CARDS;
  auto it = slotUI().find(slot);
  return it != slotUI().end() ? it->second.available : std::vector<std::string>{};
}

std::string slotNote(const MachineProfile &machine, int slot) {
  if (machine.family == MachineFamily::AppleIIgs) {
    if (slot == 3) return "80-Column";
    auto it = iigsBuiltIn().find(slot);
    return it != iigsBuiltIn().end() ? it->second : "";
  }
  auto it = slotUI().find(slot);
  return it != slotUI().end() ? it->second.note : "";
}

std::optional<std::string> builtInDevice(const MachineProfile &machine, int slot) {
  if (machine.family != MachineFamily::AppleIIgs) return std::nullopt;
  auto it = iigsBuiltIn().find(slot);
  if (it == iigsBuiltIn().end()) return std::nullopt;
  return std::string(it->second);
}

std::vector<std::string> cardsInUse(const SlotLayout &layout, int exceptSlot) {
  std::vector<std::string> used;
  for (const auto &[slot, card] : layout) {
    if (slot != exceptSlot && card != "empty") used.push_back(card);
  }
  return used;
}

std::string formatSlotLine(int slot, const std::string &card) {
  return "Slot" + std::to_string(slot) + "=" + card;
}

bool parseSlotLine(const char *line, int &slot, std::string &card) {
  char id[32] = {};
  if (std::sscanf(line, "Slot%d=%31s", &slot, id) != 2) return false;
  if (slot < 0 || slot > 7) return false;
  card = id;
  return card == "empty" || findCard(card) != nullptr;
}

} // namespace a2e::native
