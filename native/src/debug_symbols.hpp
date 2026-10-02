/*
 * debug_symbols.hpp - Names for addresses: the Apple II's own, imported ones, and the user's
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

// The browser's symbols.js and label-manager.js, together.
//
// Three layers, looked up in order: the user's own labels (with an optional
// comment each), symbols imported from an assembler's output, and the Apple
// II's built-in names, which are generated from symbols.js
// (apple2_symbols.inc) so the two front ends cannot disagree about them.
//
// The built-in names describe the 8-bit machines' address space, so they
// answer only for an address with no bank.
class DebugSymbols {
public:
  enum class Category { ZeroPage, SoftSwitch, Disk, Rom, Basic, Vector, Io, Imported, User };

  struct Symbol {
    std::string name;
    std::string description;
    Category category = Category::User;
  };

  struct Label {
    std::string name;
    std::string comment;
  };

  // The name for an address, if anything has one.
  std::optional<Symbol> lookup(uint32_t address) const;
  // A label or comment the user put at an address.
  const Label *label(uint32_t address) const;

  // An address the user typed: "$2000", "2000", "0x2000", "E1/2000" (a bank
  // and an offset, as a IIgs's monitor writes one), or any symbol's name,
  // case blind. Addresses wider than `addressMask` are refused.
  std::optional<uint32_t> resolve(const std::string &text, uint32_t addressMask) const;

  // Labels. An empty name and comment removes the entry.
  void setLabel(uint32_t address, const std::string &name);
  void setComment(uint32_t address, const std::string &comment);
  const std::map<uint32_t, Label> &labels() const { return labels_; }

  // An assembler's symbol file: ca65's .dbg, Merlin's EQU, ACME's `=`, or a
  // plain "$ADDR NAME" list. Returns how many symbols it found; they replace
  // whatever was imported before.
  int importSymbols(const std::string &text);
  size_t importedCount() const { return imported_.size(); }
  void clearImported() { imported_.clear(); }

  // Settings lines: "Label=<hex>\t<name>\t<comment>" and "Import=<hex>\t<name>".
  void writeSettings(std::string &out) const;
  bool readSetting(const char *line);

private:
  std::map<uint32_t, Label> labels_;
  std::map<uint32_t, std::string> imported_;
};

// How a symbol category is named in symbols.js.
DebugSymbols::Category symbolCategory(const char *name);

} // namespace a2e::native
