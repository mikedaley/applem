/*
 * debug_symbols.cpp - Names for addresses: the Apple II's own, imported ones, and the user's
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "debug_symbols.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <sstream>

namespace a2e::native {

namespace {

struct BuiltIn {
  uint16_t address;
  const char *name;
  const char *description;
  const char *category;
};

constexpr BuiltIn BUILT_IN[] = {
#include "apple2_symbols.inc"
};

std::string upper(std::string text) {
  for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

std::string trim(const std::string &text) {
  const size_t a = text.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return text.substr(a, text.find_last_not_of(" \t\r\n") - a + 1);
}

bool isHex(const std::string &text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return std::isxdigit(static_cast<unsigned char>(c));
  });
}

} // namespace

DebugSymbols::Category symbolCategory(const char *name) {
  if (!std::strcmp(name, "zp")) return DebugSymbols::Category::ZeroPage;
  if (!std::strcmp(name, "sw")) return DebugSymbols::Category::SoftSwitch;
  if (!std::strcmp(name, "disk")) return DebugSymbols::Category::Disk;
  if (!std::strcmp(name, "rom")) return DebugSymbols::Category::Rom;
  if (!std::strcmp(name, "basic")) return DebugSymbols::Category::Basic;
  if (!std::strcmp(name, "vec")) return DebugSymbols::Category::Vector;
  return DebugSymbols::Category::Io;
}

std::optional<DebugSymbols::Symbol> DebugSymbols::lookup(uint32_t address) const {
  if (auto it = labels_.find(address); it != labels_.end() && !it->second.name.empty()) {
    return Symbol{it->second.name, it->second.comment.empty() ? it->second.name : it->second.comment,
                  Category::User};
  }
  if (auto it = imported_.find(address); it != imported_.end()) {
    return Symbol{it->second, "Imported symbol", Category::Imported};
  }
  if (address > 0xFFFF) return std::nullopt;
  const auto *end = std::end(BUILT_IN);
  const auto *found = std::lower_bound(std::begin(BUILT_IN), end, address,
                                       [](const BuiltIn &s, uint32_t a) { return s.address < a; });
  if (found == end || found->address != address) return std::nullopt;
  return Symbol{found->name, found->description, symbolCategory(found->category)};
}

const DebugSymbols::Label *DebugSymbols::label(uint32_t address) const {
  auto it = labels_.find(address);
  return it == labels_.end() ? nullptr : &it->second;
}

std::optional<uint32_t> DebugSymbols::resolve(const std::string &input, uint32_t addressMask) const {
  std::string text = trim(input);
  if (text.empty()) return std::nullopt;

  // A bank and an offset: "E1/2000".
  if (const size_t slash = text.find('/'); slash != std::string::npos) {
    std::string bank = text.substr(0, slash);
    if (!bank.empty() && bank[0] == '$') bank.erase(0, 1);
    const std::string offset = text.substr(slash + 1);
    if (bank.size() <= 2 && offset.size() <= 4 && isHex(bank) && isHex(offset)) {
      const uint32_t value = (std::strtoul(bank.c_str(), nullptr, 16) << 16) |
                             std::strtoul(offset.c_str(), nullptr, 16);
      return value <= addressMask ? std::optional<uint32_t>(value) : std::nullopt;
    }
    return std::nullopt;
  }

  auto number = [&](const std::string &digits) -> std::optional<uint32_t> {
    if (digits.empty() || digits.size() > 6 || !isHex(digits)) return std::nullopt;
    const uint32_t value = static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 16));
    return value <= addressMask ? std::optional<uint32_t>(value) : std::nullopt;
  };
  // "$" or "0x" says it is a number, whatever it spells.
  if (text[0] == '$') return number(text.substr(1));
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) return number(text.substr(2));

  // A name, the user's first, then imported, then the machine's; a bare
  // word that is also hex ("BEEF") is a name if anything has that name.
  const std::string wanted = upper(text);
  for (const auto &[address, l] : labels_) {
    if (!l.name.empty() && upper(l.name) == wanted) return address;
  }
  for (const auto &[address, name] : imported_) {
    if (upper(name) == wanted) return address;
  }
  for (const BuiltIn &s : BUILT_IN) {
    if (upper(s.name) == wanted) return s.address;
  }
  return number(text);
}

void DebugSymbols::setLabel(uint32_t address, const std::string &name) {
  Label &l = labels_[address];
  l.name = trim(name);
  if (l.name.empty() && l.comment.empty()) labels_.erase(address);
}

void DebugSymbols::setComment(uint32_t address, const std::string &comment) {
  Label &l = labels_[address];
  l.comment = trim(comment);
  if (l.name.empty() && l.comment.empty()) labels_.erase(address);
}

int DebugSymbols::importSymbols(const std::string &text) {
  // The same four formats, tried in the same order, as label-manager.js.
  static const std::regex ca65(R"re(^sym\s.*name="([^"]+)".*val=0x([0-9A-Fa-f]+))re");
  static const std::regex merlin(R"(^(\w+)\s+EQU\s+\$([0-9A-Fa-f]{1,4}))", std::regex::icase);
  static const std::regex acme(R"(^(?:!addr\s+)?(\w+)\s*=\s*\$([0-9A-Fa-f]{1,4}))", std::regex::icase);
  static const std::regex plain(R"(^\$?([0-9A-Fa-f]{2,4})\s+(\w+))");

  imported_.clear();
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '*') continue;
    std::smatch m;
    std::string name;
    unsigned long address = 0x10000;
    if (std::regex_search(line, m, ca65)) {
      name = m[1];
      address = std::strtoul(m[2].str().c_str(), nullptr, 16);
    } else if (std::regex_search(line, m, merlin) || std::regex_search(line, m, acme)) {
      name = m[1];
      address = std::strtoul(m[2].str().c_str(), nullptr, 16);
    } else if (std::regex_search(line, m, plain) && !isHex(m[2])) {
      name = m[2];
      address = std::strtoul(m[1].str().c_str(), nullptr, 16);
    }
    if (!name.empty() && address <= 0xFFFF) imported_[static_cast<uint32_t>(address)] = name;
  }
  return static_cast<int>(imported_.size());
}

void DebugSymbols::writeSettings(std::string &out) const {
  char line[64];
  for (const auto &[address, l] : labels_) {
    std::snprintf(line, sizeof line, "Label=%X\t", address);
    out += line + l.name + "\t" + l.comment + "\n";
  }
  for (const auto &[address, name] : imported_) {
    std::snprintf(line, sizeof line, "Import=%X\t", address);
    out += line + name + "\n";
  }
}

bool DebugSymbols::readSetting(const char *line) {
  auto fields = [](const char *rest) {
    std::vector<std::string> parts;
    std::string field;
    for (const char *c = rest; *c; c++) {
      if (*c == '\t') {
        parts.push_back(field);
        field.clear();
      } else {
        field.push_back(*c);
      }
    }
    parts.push_back(field);
    return parts;
  };
  if (!std::strncmp(line, "Label=", 6)) {
    const auto parts = fields(line + 6);
    if (parts.size() < 2 || !isHex(parts[0])) return true;
    Label &l = labels_[static_cast<uint32_t>(std::strtoul(parts[0].c_str(), nullptr, 16))];
    l.name = parts[1];
    l.comment = parts.size() > 2 ? parts[2] : "";
    return true;
  }
  if (!std::strncmp(line, "Import=", 7)) {
    const auto parts = fields(line + 7);
    if (parts.size() < 2 || !isHex(parts[0])) return true;
    imported_[static_cast<uint32_t>(std::strtoul(parts[0].c_str(), nullptr, 16))] = parts[1];
    return true;
  }
  return false;
}

} // namespace a2e::native
