/*
 * expansion_slots.hpp - The Expansion Slots window, and applying a layout
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "slot_layout.hpp"

#include <functional>
#include <map>
#include <string>

struct ImGuiTextBuffer;

namespace a2e::native {

class Emulation;

// The browser build's Expansion Slots window (slot-configuration-window.js,
// slot-storage.js) and its rules:
//
// - A layout is remembered per machine, because the machines do not agree
//   about what a slot is; a machine with nothing saved gets its profile's
//   defaults, and "never configured" stays different from "emptied".
// - The machine's own constructor fits only the drives, the Mockingboard and
//   a //c's ports, so the host applies the layout at startup and after every
//   switch; without it a //e has no SmartPort and no clock.
// - Each card can be fitted once. A fixed slot is shown locked.
// - Apply & Reset fits every changeable slot, empty ones included, and
//   resets the machine. A IIgs's built-in-or-card switch takes effect at
//   once, as the Control Panel's does.
// - The No-Slot Clock is one setting for every machine.
class ExpansionSlots {
public:
  explicit ExpansionSlots(Emulation &emulation);

  void setMachine(const MachineProfile &machine) { machine_ = &machine; dirty_ = false; }
  // Put the remembered layout (or the defaults) into the machine.
  void apply();

  void draw(bool *open);
  // After Apply & Reset: the cards may have been rebuilt.
  void setAppliedCallback(std::function<void()> callback) { onApplied_ = std::move(callback); }

  // Settings file: [ApplEmSlots][<machine key>] with SlotN=id lines.
  SlotLayout *openSection(const char *machine);
  void readLine(SlotLayout *layout, const char *line);
  void writeAll(ImGuiTextBuffer *out, const char *typeName) const;
  bool noSlotClock = false;

private:
  void refreshFromMachine();
  void applyAndReset();

  Emulation &emulation_;
  const MachineProfile *machine_ = nullptr;
  std::map<std::string, SlotLayout> saved_; // by machine key
  SlotLayout working_;
  bool dirty_ = false;
  bool wasOpen_ = false;
  std::function<void()> onApplied_;
};

} // namespace a2e::native
