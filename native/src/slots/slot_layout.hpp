/*
 * slot_layout.hpp - Which cards are in which slots, and what each slot takes
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace a2e {
struct MachineProfile;
}

namespace a2e::native {

// A card the user can fit, as the browser names it.
struct CardInfo {
  const char *id;
  const char *name;
  unsigned color; // 0xRRGGBB, the tray's accent
};
const std::vector<CardInfo> &cards();
const CardInfo *findCard(const std::string &id);

// Slot number to card id, "empty" for a slot deliberately left empty. A slot
// that is absent has not been configured.
using SlotLayout = std::map<int, std::string>;

// What a fresh one of this machine ships with: the profile's default cards,
// fixed slots left out because they are not the user's to change.
SlotLayout defaultLayout(const MachineProfile &machine);

// Whether a slot holds a card nobody can remove (a //e's 80-column card, a
// //c's ports), and what to call it.
bool isFixedSlot(const MachineProfile &machine, int slot);
std::string fixedCardLabel(const MachineProfile &machine, int slot);

// The cards a slot is offered, and its conventional use. Host presentation
// rather than machine fact, as the browser's SLOT_UI is: the profile says
// which slots exist and which are fixed, this says what to offer the rest.
std::vector<std::string> slotOffers(const MachineProfile &machine, int slot);
std::string slotNote(const MachineProfile &machine, int slot);

// Whether a slot may hold this card now: "empty", or one it offers. A saved
// layout is held to it, so a card withdrawn from the offers (the printer and
// serial cards, until there is a printer) cannot come back from an old file.
bool slotTakes(const MachineProfile &machine, int slot, const std::string &card);

// A IIgs slot's built-in device, which the machine's $C02D switch chooses
// between and the socket; nothing for slot 3, which has no switch.
std::optional<std::string> builtInDevice(const MachineProfile &machine, int slot);

// Each card can be fitted once: the ids already in other slots.
std::vector<std::string> cardsInUse(const SlotLayout &layout, int exceptSlot);

// The text form kept in the settings file, one "SlotN=id" per line.
std::string formatSlotLine(int slot, const std::string &card);
bool parseSlotLine(const char *line, int &slot, std::string &card);

} // namespace a2e::native
