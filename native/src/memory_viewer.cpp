/*
 * memory_viewer.cpp - The memory viewer: every byte of the machine, live
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "memory_viewer.hpp"

#include "cpu_debugger.hpp"
#include "emulation.hpp"
#include "media_store.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "basic/applesoft_vars.hpp"
#include "video/ntsc.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>

namespace a2e::native {

namespace {

constexpr float CARD_ROUNDING = 10.0f;
constexpr float PAD = 12.0f;
constexpr float MAP_WIDTH = 88.0f;
constexpr float SIDEBAR_MIN = 290.0f;
// A changed byte is lit for this long, fading.
constexpr double CHANGE_FADE = 1.6;
// Rows read beyond each edge of the view, so a scroll has bytes to show
// before the next snapshot.
constexpr int MARGIN_ROWS = 8;
// The longest selection that is summed, and the most matches a search keeps.
constexpr size_t STATS_MAX = 0x10000;
constexpr size_t MATCHES_MAX = 20000;
constexpr size_t UNDO_MAX = 500;
// How long an access stays lit, in seconds (an exponential's time constant).
constexpr float GLOW_TAU = 0.35f;

// The Apple logo's six stripes, which every colour in the app comes from,
// made readable for the current appearance by the theme.
using ui::Palette;
using ui::palette;

ImU32 hueColour(int hue, const Palette &p) {
  switch (hue) {
  case 0: return p.green;
  case 1: return p.yellow;
  case 2: return p.orange;
  case 3: return p.red;
  case 4: return p.purple;
  case 5: return p.blue;
  }
  return 0;
}

enum Hue { GREEN, YELLOW, ORANGE, RED, PURPLE, BLUE };

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 cardFill() { return ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 7); }
ImU32 well() { return ui::isDark() ? IM_COL32(0, 0, 0, 80) : IM_COL32(255, 255, 255, 200); }

void card(ImDrawList *draw, ImVec2 a, ImVec2 b) {
  draw->AddRectFilled(a, b, cardFill(), CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

// Small capitals, as the other debug windows label things.
void caption(const char *label) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

// A capsule with a word in it, lit or not. Returns its width.
float pill(ImDrawList *draw, ImVec2 at, const char *label, bool lit, ImU32 colour, float scale = 0.78f) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * scale);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const ImVec2 end(at.x + size.x + 12, at.y + size.y + 4);
  draw->AddRectFilled(at, end, lit ? colour : text(0.07f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 6, at.y + 2), lit ? ui::textOn(colour) : secondary(), label);
  ImGui::PopFont();
  return end.x - at.x;
}

float pillHeight(float scale = 0.78f) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * scale);
  const float h = ImGui::GetTextLineHeight() + 4;
  ImGui::PopFont();
  return h;
}

// A card around whatever `body` draws: a caption, then the body, inside a
// rounded panel as wide as asked and as tall as the body turned out.
template <typename F> void cardAround(const char *title, float width, F &&body) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  draw->ChannelsSplit(2);
  draw->ChannelsSetCurrent(1);
  ImGui::SetCursorScreenPos(ImVec2(at.x + PAD, at.y + PAD * 0.75f));
  ImGui::BeginGroup();
  caption(title);
  ImGui::Dummy(ImVec2(width - PAD * 2, 2));
  body(width - PAD * 2);
  ImGui::EndGroup();
  const float bottom = ImGui::GetItemRectMax().y + PAD * 0.75f;
  draw->ChannelsSetCurrent(0);
  card(draw, at, ImVec2(at.x + width, bottom));
  draw->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(at.x, bottom + 10));
  ImGui::Dummy(ImVec2(width, 0));
}

enum class Icon { Back, Forward, Previous, Next, Undo, Redo };

void drawIcon(ImDrawList *draw, Icon icon, ImVec2 c, float s, ImU32 colour) {
  const float t = std::max(1.5f, s * 0.13f);
  switch (icon) {
  case Icon::Back:
  case Icon::Forward: {
    const float d = icon == Icon::Back ? -1.0f : 1.0f;
    draw->AddLine(ImVec2(c.x - d * s * 0.15f, c.y - s * 0.35f), ImVec2(c.x + d * s * 0.2f, c.y), colour, t);
    draw->AddLine(ImVec2(c.x + d * s * 0.2f, c.y), ImVec2(c.x - d * s * 0.15f, c.y + s * 0.35f), colour, t);
    break;
  }
  case Icon::Previous:
  case Icon::Next: {
    const float d = icon == Icon::Previous ? -1.0f : 1.0f;
    draw->AddLine(ImVec2(c.x - s * 0.35f, c.y - d * s * 0.15f), ImVec2(c.x, c.y + d * s * 0.2f), colour, t);
    draw->AddLine(ImVec2(c.x, c.y + d * s * 0.2f), ImVec2(c.x + s * 0.35f, c.y - d * s * 0.15f), colour, t);
    break;
  }
  case Icon::Undo:
  case Icon::Redo: {
    const float d = icon == Icon::Undo ? 1.0f : -1.0f;
    const float a0 = icon == Icon::Undo ? IM_PI * 1.15f : -IM_PI * 0.15f;
    const float a1 = icon == Icon::Undo ? IM_PI * 2.1f : -IM_PI * 1.1f;
    draw->PathArcTo(ImVec2(c.x, c.y + s * 0.1f), s * 0.32f, a0, a1, 12);
    draw->PathStroke(colour, 0, t);
    const ImVec2 tip(c.x - d * s * 0.32f, c.y + s * 0.02f);
    draw->AddTriangleFilled(ImVec2(tip.x - d * s * 0.18f, tip.y - s * 0.12f), ImVec2(tip.x + d * s * 0.18f, tip.y - s * 0.12f),
                            ImVec2(tip.x, tip.y + s * 0.2f), colour);
    break;
  }
  }
}

// A button a frame high with an icon on it, highlighted when hovered.
bool iconButton(const char *id, Icon icon, const char *tip, bool enabled = true) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float height = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!enabled);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(height, height));
  const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  const bool held = ImGui::IsItemActive();
  ImGui::EndDisabled();
  const ImVec2 end(at.x + height, at.y + height);
  if (enabled && (hovered || held)) draw->AddRectFilled(at, end, text(held ? 0.14f : 0.08f), ImGui::GetStyle().FrameRounding);
  drawIcon(draw, icon, ImVec2(at.x + height * 0.5f, at.y + height * 0.5f), height * 0.42f,
           enabled ? text(0.85f) : text(0.25f));
  if (hovered && tip) ImGui::SetTooltip("%s", tip);
  return pressed && enabled;
}

// A small round button with an x, for taking a row away.
bool removeButton(const char *id) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float size = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
  if (hovered) draw->AddCircleFilled(c, size * 0.5f, withAlpha(palette().red, 0.2f));
  const float r = size * 0.2f;
  const ImU32 colour = hovered ? palette().red : secondary();
  draw->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), colour, 1.5f);
  draw->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), colour, 1.5f);
  return pressed;
}

ImU32 kindColour(Breakpoint::Kind kind, const Palette &p) {
  switch (kind) {
  case Breakpoint::Kind::Exec: return p.red;
  case Breakpoint::Kind::Read: return p.blue;
  case Breakpoint::Kind::Write: return p.orange;
  case Breakpoint::Kind::ReadWrite: return p.purple;
  case Breakpoint::Kind::Stack: return p.yellow;
  }
  return p.red;
}

ImU32 symbolColour(DebugSymbols::Category category, const Palette &p) {
  switch (category) {
  case DebugSymbols::Category::ZeroPage: return p.green;
  case DebugSymbols::Category::SoftSwitch: return p.orange;
  case DebugSymbols::Category::Disk: return p.purple;
  case DebugSymbols::Category::Rom: return p.blue;
  case DebugSymbols::Category::Basic: return p.yellow;
  case DebugSymbols::Category::Vector: return p.red;
  case DebugSymbols::Category::Io: return p.orange;
  case DebugSymbols::Category::Imported:
  case DebugSymbols::Category::User: return ui::accentText();
  }
  return text();
}

// A byte as text. In Apple's screen codes the top two bits choose the
// style: $01-$3F inverse, $40-$7F flashing, $80 up normal, with $80-$9F the
// control characters, which the screen shows as capitals; they are dimmed so
// a carriage return does not read as an M. As ASCII, only the printable
// seven-bit characters are shown.
struct Glyph {
  char c = '.';
  enum Style { Normal, Inverse, Flash, Dim } style = Normal;
};

Glyph glyphFor(uint8_t v, bool apple) {
  // Zero is nearly always nothing rather than an inverse @, and a page of
  // them drawn as inverse cells hides whatever text is among them.
  if (v == 0) return {'.', Glyph::Dim};
  if (!apple) {
    if (v >= 0x20 && v < 0x7F) return {static_cast<char>(v), Glyph::Normal};
    return {'.', Glyph::Dim};
  }
  if (v >= 0x80) {
    const uint8_t c = v & 0x7F;
    if (c < 0x20) return {static_cast<char>(c + 0x40), Glyph::Dim};
    if (c == 0x7F) return {'.', Glyph::Dim};
    return {static_cast<char>(c), Glyph::Normal};
  }
  const uint8_t c = v & 0x3F;
  const char ch = static_cast<char>(c < 0x20 ? c + 0x40 : c);
  return {ch, v < 0x40 ? Glyph::Inverse : Glyph::Flash};
}

// A byte's colour on the map: nothing for zero, grey for $FF, and the logo's
// stripes in order round the values between, brighter as they climb, so code,
// text, pictures and empty space each have a look of their own.
void valueColour(uint8_t v, bool dark, uint8_t out[3]) {
  if (v == 0) {
    const uint8_t c[3] = {static_cast<uint8_t>(dark ? 20 : 238), static_cast<uint8_t>(dark ? 22 : 239),
                          static_cast<uint8_t>(dark ? 28 : 243)};
    std::memcpy(out, c, 3);
    return;
  }
  if (v == 0xFF) {
    const uint8_t c[3] = {static_cast<uint8_t>(dark ? 104 : 168), static_cast<uint8_t>(dark ? 108 : 172),
                          static_cast<uint8_t>(dark ? 120 : 182)};
    std::memcpy(out, c, 3);
    return;
  }
  static const float stripes[6][3] = {{97, 187, 70},  {253, 184, 39}, {245, 130, 31},
                                      {224, 58, 62},  {150, 61, 151}, {0, 157, 220}};
  const float t = (v - 1) / 253.0f * 5.0f;
  const int i = std::min(4, static_cast<int>(t));
  const float f = t - static_cast<float>(i);
  const float level = v / 255.0f;
  for (int k = 0; k < 3; k++) {
    const float c = stripes[i][k] + (stripes[i + 1][k] - stripes[i][k]) * f;
    const float shown = dark ? c * (0.42f + 0.5f * level) : 255.0f + (c - 255.0f) * (0.45f + 0.45f * level);
    out[k] = static_cast<uint8_t>(std::clamp(shown, 0.0f, 255.0f));
  }
}

// Hex bytes from text, however they were written: "A9 00", "$A9,$00",
// "0xA9, 0x00", "A900" or a line of Merlin's HEX. A token that is not whole
// bytes of hex is skipped, so a mnemonic or a directive passes by. "??" is a
// wildcard, -1, where the caller allows one.
std::vector<int> parseHex(const std::string &input, bool wildcards) {
  std::vector<int> out;
  std::string token;
  auto flush = [&] {
    std::string t = token;
    token.clear();
    if (t.empty()) return;
    if (t[0] == '$') t.erase(0, 1);
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t.erase(0, 2);
    if (t.empty() || t.size() % 2) return;
    std::vector<int> bytes;
    for (size_t i = 0; i < t.size(); i += 2) {
      if (wildcards && t[i] == '?' && t[i + 1] == '?') {
        bytes.push_back(-1);
        continue;
      }
      if (!std::isxdigit(static_cast<unsigned char>(t[i])) || !std::isxdigit(static_cast<unsigned char>(t[i + 1]))) {
        return;
      }
      bytes.push_back(static_cast<int>(std::strtoul(t.substr(i, 2).c_str(), nullptr, 16)));
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
  };
  for (char c : input) {
    if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ';') flush();
    else token += c;
  }
  flush();
  return out;
}

uint16_t crc16(const std::vector<uint8_t> &bytes) {
  // CRC-16/XMODEM, the one an Apple II's communications software checks.
  uint16_t crc = 0;
  for (uint8_t b : bytes) {
    crc ^= static_cast<uint16_t>(b << 8);
    for (int i = 0; i < 8; i++) crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
  }
  return crc;
}

// The places worth jumping to, by machine: the 8-bit machines keep them all
// in one bank, a IIgs spreads them over its own banks and the Mega II's.
struct Place {
  const char *label;
  uint32_t address;
};

const Place PLACES_8BIT[] = {
    {"Zero page", 0x0000}, {"Stack", 0x0100},   {"Text 1", 0x0400}, {"Text 2", 0x0800},
    {"Hi-res 1", 0x2000},  {"Hi-res 2", 0x4000}, {"DOS 3.3", 0x9600}, {"I/O", 0xC000},
    {"Slot ROM", 0xC100},  {"ROM", 0xD000},      {"Monitor", 0xF800}, {"Vectors", 0xFFFA},
};

const Place PLACES_IIGS[] = {
    {"Zero page", 0x000000}, {"Stack", 0x000100},  {"Text", 0xE00400},   {"Text aux", 0xE10400},
    {"Hi-res", 0xE02000},    {"SHR", 0xE12000},    {"SCB", 0xE19D00},    {"Palettes", 0xE19E00},
    {"I/O", 0xE0C000},       {"ROM", 0xFF0000},    {"Vectors", 0x00FFE0},
};

// The grid's columns, measured in the monospaced face from the grid's left.
struct Layout {
  float charW = 7, lineH = 18;
  float addrX = 0, hexX0 = 0, cellW = 0, groupGap = 0, textX0 = 0, textCellW = 0, mapX = 0, width = 0;
  int columns = 16;
  float hexX(int i) const { return hexX0 + i * cellW + static_cast<float>(i / 8) * groupGap; }
};

Layout layoutFor(int columns, bool wide) {
  Layout l;
  ImGui::PushFont(ui::monoFont(), 0.0f);
  l.charW = ImGui::CalcTextSize("0").x;
  l.lineH = ImGui::GetTextLineHeight() + 5;
  ImGui::PopFont();
  l.columns = columns;
  l.addrX = 16;
  l.hexX0 = l.addrX + l.charW * (wide ? 7.0f : 4.0f) + l.charW * 1.8f;
  l.cellW = l.charW * 3.0f;
  l.groupGap = l.charW * 0.9f;
  const float hexEnd = l.hexX(columns - 1) + l.charW * 2;
  l.textX0 = hexEnd + l.charW * 2.4f;
  l.textCellW = l.charW * 1.1f;
  l.mapX = l.textX0 + columns * l.textCellW + 18;
  l.width = l.mapX + MAP_WIDTH + 10;
  return l;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and the machine
// ---------------------------------------------------------------------------

MemoryViewer::MemoryViewer(Emulation &emulation, Platform &platform, CpuDebugger &debugger)
    : emulation_(emulation), platform_(platform), debugger_(debugger) {}

MemoryViewer::~MemoryViewer() {
  if (!platform_.releaseTexture) return;
  for (ImTextureID texture : {mapTexture_, bitmapTexture_, screenTexture_}) {
    if (texture != ImTextureID_Invalid) platform_.releaseTexture(texture);
  }
}

void MemoryViewer::setMachine(const MachineProfile &profile) {
  const bool changed = profile_ && profile_->id != profile.id;
  profile_ = &profile;
  wide_ = profile.family == MachineFamily::AppleIIgs;
  if (changed) {
    // Nothing of the old machine's places means anything on this one.
    spaces_.clear();
    space_ = 0;
    wantedSpace_.clear();
    wantedTop_.reset();
    caret_ = anchor_ = 0;
    topRow_ = 0;
    backStack_.clear();
    forwardStack_.clear();
    undo_.clear();
    redo_.clear();
    matches_.clear();
    searchStale_ = true;
    changed_.clear();
    mapSpace_ = SIZE_MAX;
    activityApplied_ = false;
  }
}

const host::MemorySpace *MemoryViewer::space() const {
  return space_ < spaces_.size() ? &spaces_[space_] : nullptr;
}

uint32_t MemoryViewer::spaceEnd() const {
  const host::MemorySpace *sp = space();
  return sp ? sp->base + sp->size : 0;
}

int MemoryViewer::rowCount() const {
  const host::MemorySpace *sp = space();
  return sp ? static_cast<int>(sp->size / static_cast<uint32_t>(columns_)) : 0;
}

uint32_t MemoryViewer::rowAddress(int row) const {
  const host::MemorySpace *sp = space();
  return (sp ? sp->base : 0) + static_cast<uint32_t>(row * columns_);
}

int MemoryViewer::rowOf(uint32_t address) const {
  const host::MemorySpace *sp = space();
  if (!sp || address < sp->base) return 0;
  return static_cast<int>((address - sp->base) / static_cast<uint32_t>(columns_));
}

bool MemoryViewer::inSpace(uint32_t address) const {
  const host::MemorySpace *sp = space();
  return sp && address >= sp->base && address < sp->base + sp->size;
}

std::optional<uint8_t> MemoryViewer::byteAt(uint32_t address) const {
  if (snapshot_.space != space_ || address < snapshot_.first) return std::nullopt;
  const size_t i = address - snapshot_.first;
  if (i >= snapshot_.bytes.size()) return std::nullopt;
  return snapshot_.bytes[i];
}

std::string MemoryViewer::formatAddress(uint32_t address) const {
  char out[16];
  if (wide_) std::snprintf(out, sizeof out, "%02X/%04X", (address >> 16) & 0xFF, address & 0xFFFF);
  else std::snprintf(out, sizeof out, "%04X", address & 0xFFFF);
  return out;
}

uint32_t MemoryViewer::stackAddress() const {
  const host::CpuState &cpu = snapshot_.cpu;
  if (wide_ && !cpu.emulation()) return cpu.sp;
  return 0x100 | (cpu.sp & 0xFF);
}

// What is where in the space being browsed.
std::vector<MemoryViewer::Region> MemoryViewer::regions() const {
  const host::MemorySpace *sp = space();
  if (!sp) return {};
  std::vector<Region> low = {
      {0x0000, 0x00FF, "Zero page", GREEN},     {0x0100, 0x01FF, "Stack", YELLOW},
      {0x0200, 0x02FF, "Input buffer", -1},     {0x0300, 0x03FF, "Vectors and free space", -1},
      {0x0400, 0x07FF, "Text page 1", BLUE},    {0x0800, 0x0BFF, "Text page 2", BLUE},
      {0x0C00, 0x1FFF, "Free", -1},             {0x2000, 0x3FFF, "Hi-res page 1", PURPLE},
      {0x4000, 0x5FFF, "Hi-res page 2", PURPLE}, {0x6000, 0xBFFF, "Free", -1},
  };
  auto with = [&](std::initializer_list<Region> high) {
    std::vector<Region> all = low;
    all.insert(all.end(), high);
    return all;
  };
  const Region io = {0xC000, 0xC0FF, "I/O and soft switches", ORANGE};
  const Region card = {0xD000, 0xFFFF, "ROM or language card", RED};

  if (wide_) {
    const uint32_t bank = sp->base >> 16;
    if (bank == 0xE1) {
      return {{0x0000, 0x00FF, "Zero page", GREEN},
              {0x0100, 0x01FF, "Stack", YELLOW},
              {0x0200, 0x03FF, "Free", -1},
              {0x0400, 0x07FF, "80-column text", BLUE},
              {0x0800, 0x1FFF, "Free", -1},
              {0x2000, 0x9CFF, "Super Hi-Res pixels", PURPLE},
              {0x9D00, 0x9DFF, "Scan-line control bytes", GREEN},
              {0x9E00, 0x9FFF, "Super Hi-Res palettes", YELLOW},
              {0xA000, 0xBFFF, "Free", -1},
              io,
              {0xC100, 0xCFFF, "Slot firmware", RED},
              {0xD000, 0xFFFF, "Language card", ORANGE}};
    }
    if (bank == 0x00 || bank == 0x01 || bank == 0xE0) return with({io, {0xC100, 0xCFFF, "Slot firmware", RED}, card});
    if (!sp->writable) return {{0x0000, 0xFFFF, "ROM", RED}};
    return {{0x0000, 0xFFFF, "Fast RAM", -1}};
  }

  using K = host::MemorySpace::Kind;
  switch (sp->kind) {
  case K::Processor: {
    const bool slots = profile_ && profile_->caps.hasExpansionSlots;
    return with({io, {0xC100, 0xCFFF, slots ? "Slot firmware" : "Internal firmware", RED}, card});
  }
  case K::MainRAM:
  case K::AuxRAM: {
    if (sp->kind == K::AuxRAM) {
      low[4].name = "80-column text, even columns";
      low[5].name = "Text page 2, auxiliary";
      low[7].name = "Double hi-res page 1";
      low[8].name = "Double hi-res page 2";
    }
    return with({{0xC000, 0xCFFF, "Language card bank 1 ($D000)", ORANGE},
                 {0xD000, 0xDFFF, "Language card bank 2", ORANGE},
                 {0xE000, 0xFFFF, "Language card", ORANGE}});
  }
  case K::ROM: {
    std::vector<Region> rom;
    const uint32_t base = profile_ ? profile_->memory.romBaseAddress : 0xC000;
    if (base > 0xC000) {
      rom.push_back({0xC000, base - 1, "No ROM here", -1});
    } else {
      rom.push_back({0xC000, 0xC0FF, "I/O, not ROM", -1});
      rom.push_back({0xC100, 0xCFFF, "Internal firmware", RED});
    }
    rom.push_back({0xD000, 0xF7FF, "Applesoft", YELLOW});
    rom.push_back({0xF800, 0xFFFF, "Monitor", RED});
    return rom;
  }
  }
  return low;
}

const MemoryViewer::Region *MemoryViewer::regionFor(uint32_t address, const std::vector<Region> &list) const {
  const uint32_t offset = address & 0xFFFF;
  for (const Region &r : list) {
    if (offset >= r.start && offset <= r.end) return &r;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Reading the machine
// ---------------------------------------------------------------------------

void MemoryViewer::update(bool open) {
  open_ = open;
  if (!profile_) return;
  if (!open) {
    if (activityApplied_) {
      emulation_.post([](host::MachineHost &host) { host.setMemoryActivity(false); });
      activityApplied_ = false;
    }
    return;
  }
  take();
}

void MemoryViewer::take() {
  Snapshot s;
  const host::MemorySpace *current = space();
  const int firstRow = std::max(0, topRow_ - MARGIN_ROWS);
  const double now = ImGui::GetTime();
  const bool wantActivity = activityOn_ && open_;
  const bool wantStats = hasSelection() &&
                         (stats_.space != space_ || stats_.low != selectionLow() || stats_.high != selectionHigh() ||
                          now - stats_.at > 0.5);
  std::vector<uint8_t> statBytes;
  bool readMap = false;

  const bool took = emulation_.poll(poll_, [&](host::MachineHost &host) {
    if (!host.isBuilt()) return;
    // The spaces follow the machine: a IIgs given more memory has more banks.
    std::vector<host::MemorySpace> spaces = host.memorySpaces();
    if (spaces.size() != spaces_.size() || (current && (space_ >= spaces.size() || spaces[space_].base != current->base ||
                                                        spaces[space_].kind != current->kind))) {
      size_t keep = 0;
      for (size_t i = 0; i < spaces.size(); i++) {
        const bool same = current ? spaces[i].base == current->base && spaces[i].kind == current->kind
                                  : !wantedSpace_.empty() && spaces[i].name == wantedSpace_;
        if (same) keep = i;
      }
      spaces_ = std::move(spaces);
      space_ = keep;
      current = space();
      if (wantedTop_ && current) {
        caret_ = anchor_ = std::clamp(*wantedTop_, current->base, current->base + current->size - 1);
        topRow_ = rowOf(caret_);
      }
      wantedTop_.reset();
      wantedSpace_.clear();
    }
    if (!current) return;

    s.valid = true;
    s.space = space_;
    s.paused = host.isPaused();
    s.cpu = host.cpuState();
    s.pcLength = host.disassemble(s.cpu.pc).length;

    const int rows = std::max(0, std::min(rowCount(), topRow_ + visibleRows_ + MARGIN_ROWS) - firstRow);
    s.first = rowAddress(firstRow);
    s.bytes.resize(static_cast<size_t>(rows * columns_));
    host.readSpace(*current, s.first, s.bytes.data(), s.bytes.size());
    host.readSpace(*current, caret_, s.atCaret.data(), s.atCaret.size());

    if (Emulator *e = host.emulator()) {
      s.switches = e->getMMU().getSoftSwitches();
      s.haveSwitches = true;
    } else if (iigs::IIgsMachine *gs = host.iigs()) {
      s.switches = gs->memory().megaII().getSoftSwitches();
      s.haveSwitches = true;
    }

    // Accesses since the last frame, and then forgotten by the machine: how
    // long they stay lit is the viewer's business.
    const bool activity = wantActivity && host.hasMemoryActivity() && current->activity;
    if (activity != activityApplied_) {
      host.setMemoryActivity(activity);
      activityApplied_ = activity;
    }
    s.activity = activity;
    if (activity) {
      rawReads_.resize(current->size);
      rawWrites_.resize(current->size);
      host.memoryActivity(current->base, current->size, rawReads_.data(), rawWrites_.data());
      host.decayMemoryActivity(255);
      haveRaw_ = true;
    }

    switch (follow_) {
    case Follow::Off: break;
    case Follow::PC: s.follow = s.cpu.pc; break;
    case Follow::Stack: s.follow = wide_ && !s.cpu.emulation() ? s.cpu.sp : 0x100u | (s.cpu.sp & 0xFF); break;
    case Follow::Expression:
      if (followText_[0]) {
        const int32_t value = host.evaluateExpression(followText_);
        if (host.conditionError().empty()) s.follow = static_cast<uint32_t>(value) & addressMask();
      }
      break;
    }

    if (wantStats) {
      const size_t length = std::min<size_t>(selectionHigh() - selectionLow() + 1, STATS_MAX);
      statBytes.resize(length);
      host.readSpace(*current, selectionLow(), statBytes.data(), length);
    }

    // The pages this machine has, for the screen view to offer.
    availablePages_ = 0;
    for (int page = 0; page < 7; page++) {
      if (host.hasDisplayPage(static_cast<host::MachineHost::DisplayPage>(page))) availablePages_ |= 1u << page;
    }
    // The bytes the bitmap shows, from its top: as many rows as fit, of
    // either layout.
    if (view_ == View::Bitmap) {
      const size_t rowBytes = static_cast<size_t>(bitmapWidth_) * (bitmapTiles_ ? 8 : 1);
      const size_t rows = static_cast<size_t>(bitmapTiles_ ? (bitmapRows_ + 7) / 8 : bitmapRows_);
      bitmapBytes_.resize(std::min<size_t>(rowBytes * rows, current->size));
      host.readSpace(*current, bitmapTop_, bitmapBytes_.data(), bitmapBytes_.size());
      bitmapBytesFrom_ = bitmapTop_;
    }
    // The page, decoded by the machine's own renderer, thirty times a second.
    if (view_ == View::Screen && now - screenTakenAt_ > 1.0 / 30.0) {
      if (!(availablePages_ & (1u << screenPage_))) {
        for (int page = 0; page < 7; page++) {
          if (availablePages_ & (1u << page)) {
            screenPage_ = page;
            break;
          }
        }
      }
      static constexpr VideoColorMode DECODES[] = {VideoColorMode::SOLID, VideoColorMode::PIXEL_EXACT,
                                                   VideoColorMode::MONOCHROME};
      if (host.renderDisplayPage(static_cast<host::MachineHost::DisplayPage>(screenPage_), screenPage2_,
                                 DECODES[std::clamp(screenColours_, 0, 2)], screenRgba_, screenWidth_,
                                 screenHeight_)) {
        screenTakenAt_ = now;
      }
    }

    // The map, a few times a second, or more often while it is showing
    // accesses.
    const double interval = activity ? 0.1 : 0.3;
    if (mapSpace_ != space_ || now - mapTakenAt_ > interval) {
      mapBytes_.resize(current->size);
      host.readSpace(*current, current->base, mapBytes_.data(), mapBytes_.size());
      mapSpace_ = space_;
      mapTakenAt_ = now;
      readMap = true;
    }
  });
  if (!took || !s.valid) return;

  // Every byte that differs from what was shown at the same address lights.
  if (changedSpace_ != s.space) {
    changed_.clear();
    changedSpace_ = s.space;
  } else if (snapshot_.valid && snapshot_.space == s.space) {
    const uint32_t lo = std::max(s.first, snapshot_.first);
    const uint32_t hi = std::min<uint32_t>(s.first + static_cast<uint32_t>(s.bytes.size()),
                                           snapshot_.first + static_cast<uint32_t>(snapshot_.bytes.size()));
    for (uint32_t a = lo; a < hi; a++) {
      if (s.bytes[a - s.first] != snapshot_.bytes[a - snapshot_.first]) changed_[a] = now;
    }
  }
  for (auto it = changed_.begin(); it != changed_.end();) {
    it = now - it->second > CHANGE_FADE ? changed_.erase(it) : std::next(it);
  }

  // The accesses fade as they age and are lit again by new ones, brighter for
  // more of them.
  const host::MemorySpace *sp = space();
  if (s.activity && sp && haveRaw_) {
    readGlow_.resize(sp->size, 0.0f);
    writeGlow_.resize(sp->size, 0.0f);
    const float fade = std::exp(-static_cast<float>(std::max(0.0, now - glowAt_)) / GLOW_TAU);
    static const float LOG_MAX = std::log1p(255.0f);
    for (size_t i = 0; i < sp->size; i++) {
      readGlow_[i] *= fade;
      writeGlow_[i] *= fade;
      if (rawReads_[i]) readGlow_[i] = std::max(readGlow_[i], 0.35f + 0.65f * std::log1p(static_cast<float>(rawReads_[i])) / LOG_MAX);
      if (rawWrites_[i]) {
        writeGlow_[i] = std::max(writeGlow_[i], 0.45f + 0.55f * std::log1p(static_cast<float>(rawWrites_[i])) / LOG_MAX);
      }
    }
    haveRaw_ = false;
  } else if (!s.activity) {
    readGlow_.clear();
    writeGlow_.clear();
  }
  glowAt_ = now;

  if (!statBytes.empty()) {
    SelectionStats st;
    st.space = space_;
    st.low = selectionLow();
    st.high = selectionHigh();
    st.at = now;
    st.length = statBytes.size();
    for (uint8_t b : statBytes) {
      st.sum8 = static_cast<uint8_t>(st.sum8 + b);
      st.sum16 = static_cast<uint16_t>(st.sum16 + b);
      st.xor8 ^= b;
      if (!b) st.zeros++;
    }
    st.crc16 = crc16(statBytes);
    stats_ = st;
  }

  // Following: the view moves only when what it follows leaves it.
  const std::optional<uint32_t> follow = s.follow;
  snapshot_ = std::move(s);
  if (follow) {
    if (!inSpace(*follow)) {
      for (size_t i = 0; i < spaces_.size(); i++) {
        if (spaces_[i].processor && *follow >= spaces_[i].base && *follow < spaces_[i].base + spaces_[i].size) {
          selectSpace(i);
          break;
        }
      }
    }
    if (inSpace(*follow)) reveal(*follow);
  }
  if (readMap) mapDirty_ = true;
  if (view_ == View::Bitmap && now - bitmapPaintedAt_ > 1.0 / 30.0) {
    paintBitmap();
    bitmapPaintedAt_ = now;
  }
  if (view_ == View::Screen && !screenRgba_.empty()) {
    showTexture(screenTexture_, screenTextureWidth_, screenTextureHeight_, screenRgba_, screenWidth_, screenHeight_);
  }
  if (mapDirty_) paintMap();
}

// The whole space as a picture, a pixel a byte and a row a page.
void MemoryViewer::paintMap() {
  mapDirty_ = false;
  if (!platform_.makeTexture || mapBytes_.empty()) return;
  const int width = 256;
  const int height = static_cast<int>((mapBytes_.size() + 255) / 256);
  std::vector<uint8_t> rgba(static_cast<size_t>(width * height) * 4, 0);
  const bool dark = ui::isDark();
  const bool glow = !readGlow_.empty() && readGlow_.size() == mapBytes_.size();
  for (size_t i = 0; i < mapBytes_.size(); i++) {
    uint8_t c[3];
    valueColour(mapBytes_[i], dark, c);
    float r = c[0], g = c[1], b = c[2];
    if (glow) {
      const float rd = readGlow_[i] * 0.85f, wr = writeGlow_[i];
      r += (60 - r) * rd, g += (190 - g) * rd, b += (255 - b) * rd;
      r += (255 - r) * wr, g += (150 - g) * wr, b += (40 - b) * wr;
    }
    uint8_t *px = &rgba[i * 4];
    px[0] = static_cast<uint8_t>(r);
    px[1] = static_cast<uint8_t>(g);
    px[2] = static_cast<uint8_t>(b);
    px[3] = 255;
  }
  if (platform_.releaseTexture && mapTexture_ != ImTextureID_Invalid) platform_.releaseTexture(mapTexture_);
  mapTexture_ = platform_.makeTexture(rgba.data(), width, height);
  mapWidth_ = width;
  mapHeight_ = height;
}

// ---------------------------------------------------------------------------
// Moving about
// ---------------------------------------------------------------------------

void MemoryViewer::selectSpace(size_t index) {
  if (index >= spaces_.size() || index == space_) return;
  // The same offset in the new space, where it has one.
  const uint32_t offset = caret_ - (space() ? space()->base : 0);
  space_ = index;
  const host::MemorySpace &sp = spaces_[index];
  caret_ = anchor_ = sp.base + std::min(offset, sp.size - 1);
  topRow_ = std::max(0, rowOf(caret_) - visibleRows_ / 4);
  scrollPixels_ = 0;
  highNibbleTyped_ = false;
  searchStale_ = true;
  matches_.clear();
  readGlow_.clear();
  writeGlow_.clear();
  ImGui::MarkIniSettingsDirty();
}

void MemoryViewer::goTo(uint32_t address, bool remember) {
  address &= addressMask();
  if (!inSpace(address)) {
    // Another space has it: the processor's first, then any.
    std::optional<size_t> found;
    for (size_t pass = 0; pass < 2 && !found; pass++) {
      for (size_t i = 0; i < spaces_.size(); i++) {
        const auto &sp = spaces_[i];
        if ((pass == 1 || sp.processor) && address >= sp.base && address < sp.base + sp.size) {
          found = i;
          break;
        }
      }
    }
    if (!found) {
      message_ = "Nothing answers at $" + formatAddress(address);
      messageAt_ = ImGui::GetTime();
      return;
    }
    if (remember) backStack_.push_back(caret_);
    selectSpace(*found);
  } else if (remember && address != caret_) {
    backStack_.push_back(caret_);
  }
  if (remember) forwardStack_.clear();
  caret_ = anchor_ = address;
  highNibbleTyped_ = false;
  // A destination is put a quarter of the way down, with what leads up to it
  // above, unless it is already comfortably in view.
  const int row = rowOf(address);
  if (row < topRow_ + 1 || row > topRow_ + visibleRows_ - 3) {
    topRow_ = std::clamp(row - visibleRows_ / 4, 0, std::max(0, rowCount() - visibleRows_ + 2));
    scrollPixels_ = 0;
  }
}

void MemoryViewer::reveal(uint32_t address) {
  const int row = rowOf(address);
  if (row >= topRow_ + 1 && row <= topRow_ + visibleRows_ - 3) return;
  if (row == topRow_ && scrollPixels_ == 0) return;
  if (row < topRow_ + 1) topRow_ = std::max(0, row - 1);
  else topRow_ = std::clamp(row - visibleRows_ + 3, 0, std::max(0, rowCount() - visibleRows_ + 2));
  scrollPixels_ = 0;
}

void MemoryViewer::back() {
  if (backStack_.empty()) return;
  forwardStack_.push_back(caret_);
  const uint32_t to = backStack_.back();
  backStack_.pop_back();
  goTo(to, false);
}

void MemoryViewer::forward() {
  if (forwardStack_.empty()) return;
  backStack_.push_back(caret_);
  const uint32_t to = forwardStack_.back();
  forwardStack_.pop_back();
  goTo(to, false);
}

void MemoryViewer::setCaret(uint32_t address, bool extend) {
  const host::MemorySpace *sp = space();
  if (!sp) return;
  caret_ = std::clamp(address, sp->base, sp->base + sp->size - 1);
  if (!extend) anchor_ = caret_;
  highNibbleTyped_ = false;
}

// ---------------------------------------------------------------------------
// Changing memory
// ---------------------------------------------------------------------------

std::vector<uint8_t> MemoryViewer::read(uint32_t address, size_t count) {
  std::vector<uint8_t> bytes(count);
  const host::MemorySpace *sp = space();
  if (!sp || !count) return bytes;
  const host::MemorySpace copy = *sp;
  emulation_.withMachine([&](host::MachineHost &host) { host.readSpace(copy, address, bytes.data(), count); });
  return bytes;
}

bool MemoryViewer::write(uint32_t address, const std::vector<uint8_t> &bytes, bool record) {
  const host::MemorySpace *sp = space();
  if (!sp || bytes.empty()) return false;
  if (!sp->writable) {
    message_ = sp->name + " cannot be written";
    messageAt_ = ImGui::GetTime();
    return false;
  }
  const host::MemorySpace copy = *sp;
  Edit edit;
  edit.space = space_;
  edit.address = address;
  edit.after = bytes;
  size_t landed = 0;
  emulation_.withMachine([&](host::MachineHost &host) {
    edit.before.resize(bytes.size());
    host.readSpace(copy, address, edit.before.data(), bytes.size());
    for (size_t i = 0; i < bytes.size(); i++) {
      if (host.pokeSpace(copy, address + static_cast<uint32_t>(i), bytes[i])) landed++;
    }
  });
  if (!landed) {
    message_ = "That is I/O or ROM, and cannot be written";
    messageAt_ = ImGui::GetTime();
    return false;
  }
  if (record) {
    undo_.push_back(std::move(edit));
    if (undo_.size() > UNDO_MAX) undo_.erase(undo_.begin());
    redo_.clear();
  }
  // Shown at once rather than at the next snapshot, and not lit as a change
  // the machine made.
  for (size_t i = 0; i < bytes.size(); i++) {
    const uint32_t a = sp->base + ((address + static_cast<uint32_t>(i) - sp->base) % sp->size);
    if (snapshot_.space == space_ && a >= snapshot_.first && a - snapshot_.first < snapshot_.bytes.size()) {
      snapshot_.bytes[a - snapshot_.first] = bytes[i];
    }
    if (a - sp->base < mapBytes_.size() && mapSpace_ == space_) mapBytes_[a - sp->base] = bytes[i];
  }
  mapDirty_ = true;
  stats_.at = -10.0;
  return true;
}

void MemoryViewer::undo() {
  if (undo_.empty()) return;
  Edit edit = undo_.back();
  undo_.pop_back();
  if (edit.space != space_) selectSpace(edit.space);
  write(edit.address, edit.before, false);
  setCaret(edit.address, false);
  reveal(edit.address);
  redo_.push_back(std::move(edit));
}

void MemoryViewer::redo() {
  if (redo_.empty()) return;
  Edit edit = redo_.back();
  redo_.pop_back();
  if (edit.space != space_) selectSpace(edit.space);
  write(edit.address, edit.after, false);
  setCaret(edit.address, false);
  reveal(edit.address);
  undo_.push_back(std::move(edit));
}

// A hex digit typed at the caret: the first sets the high nibble, the second
// the low one and moves on. The two are one edit to undo.
void MemoryViewer::typeNibble(int value) {
  std::optional<uint8_t> shown = byteAt(caret_);
  const uint8_t current = shown ? *shown : read(caret_, 1)[0];
  if (!highNibbleTyped_) {
    const uint8_t next = static_cast<uint8_t>((value << 4) | (current & 0x0F));
    if (write(caret_, {next})) highNibbleTyped_ = true;
    return;
  }
  const uint8_t next = static_cast<uint8_t>((current & 0xF0) | value);
  if (write(caret_, {next}, false) && !undo_.empty() && undo_.back().address == caret_) undo_.back().after = {next};
  highNibbleTyped_ = false;
  if (caret_ + 1 < spaceEnd()) setCaret(caret_ + 1, false);
  reveal(caret_);
}

// A character typed in the text column, as the screen would store it: with
// the top bit set in Apple's codes.
void MemoryViewer::typeCharacter(unsigned c) {
  if (c < 0x20 || c > 0x7E) return;
  if (textApple_ && profile_ && !profile_->caps.hasLowercase) c = static_cast<unsigned>(std::toupper(static_cast<int>(c)));
  const uint8_t value = static_cast<uint8_t>(textApple_ ? (c | 0x80) : c);
  if (!write(caret_, {value})) return;
  if (caret_ + 1 < spaceEnd()) setCaret(caret_ + 1, false);
  reveal(caret_);
}

void MemoryViewer::pasteHex(const std::string &text) {
  const std::vector<int> parsed = parseHex(text, false);
  if (parsed.empty()) {
    message_ = "Nothing on the clipboard reads as hex";
    messageAt_ = ImGui::GetTime();
    return;
  }
  std::vector<uint8_t> bytes(parsed.begin(), parsed.end());
  const uint32_t room = spaceEnd() - caret_;
  if (bytes.size() > room) bytes.resize(room);
  if (write(caret_, bytes)) {
    anchor_ = caret_;
    caret_ = caret_ + static_cast<uint32_t>(bytes.size()) - 1;
    char text2[64];
    std::snprintf(text2, sizeof text2, "Pasted %zu bytes", bytes.size());
    message_ = text2;
    messageAt_ = ImGui::GetTime();
  }
}

// The selection on the clipboard, as hex or as source to paste into an
// assembler or a C file.
void MemoryViewer::copySelection(int format) {
  const uint32_t lo = selectionLow();
  const size_t length = std::min<size_t>(selectionHigh() - lo + 1, STATS_MAX);
  const std::vector<uint8_t> bytes = read(lo, length);
  std::string out;
  char item[16];
  for (size_t i = 0; i < bytes.size(); i++) {
    const uint8_t b = bytes[i];
    const bool lineStart = i % 16 == 0;
    switch (format) {
    case 0: // hex
      std::snprintf(item, sizeof item, "%s%02X", i == 0 ? "" : lineStart ? "\n" : " ", b);
      break;
    case 1: // Merlin HEX
      std::snprintf(item, sizeof item, "%s%02X", lineStart ? (i ? "\n         HEX   " : "         HEX   ") : "", b);
      break;
    case 2: // DFB
      std::snprintf(item, sizeof item, "%s$%02X", lineStart ? (i ? "\n         DFB   " : "         DFB   ") : ",", b);
      break;
    case 3: // C
      std::snprintf(item, sizeof item, "%s0x%02X%s", lineStart ? "    " : " ", b,
                    i + 1 == bytes.size() ? "\n" : (i % 16 == 15 ? ",\n" : ","));
      break;
    default: { // text, a return as a new line
      const Glyph g = glyphFor(b, textApple_);
      item[0] = (b & 0x7F) == 0x0D ? '\n' : g.style == Glyph::Dim ? '.' : g.c;
      item[1] = 0;
      break;
    }
    }
    out += item;
  }
  if (format == 3) out = "const unsigned char data[" + std::to_string(bytes.size()) + "] = {\n" + out + "};\n";
  ImGui::SetClipboardText(out.c_str());
  std::snprintf(item, sizeof item, "%zu", bytes.size());
  message_ = std::string("Copied ") + item + (bytes.size() == 1 ? " byte" : " bytes");
  messageAt_ = ImGui::GetTime();
}

void MemoryViewer::saveSelection() {
  if (!platform_.saveFile) return;
  const uint32_t lo = selectionLow();
  const size_t length = hasSelection() ? selectionHigh() - lo + 1 : 1;
  auto bytes = std::make_shared<std::vector<uint8_t>>(read(lo, length));
  char name[64];
  std::snprintf(name, sizeof name, "memory-%s.bin", formatAddress(lo).c_str());
  for (char *c = name; *c; c++) {
    if (*c == '/') *c = '-';
  }
  platform_.saveFile("Save Memory", name, {"bin"}, [this, bytes](const std::string &path) {
    if (path.empty()) return;
    message_ = writeFile(path, bytes->data(), bytes->size()) ? "Saved " + std::to_string(bytes->size()) + " bytes"
                                                              : "Could not write the file";
    messageAt_ = ImGui::GetTime();
  });
}

// A file put into memory, at an address the user confirms: the caret, or
// the load address in a CiderPress name ("PROG#062000").
void MemoryViewer::loadFile() {
  if (!platform_.openFile) return;
  platform_.openFile("Load Into Memory", {}, [this](const std::string &path) {
    if (path.empty()) return;
    auto bytes = readFile(path);
    if (!bytes || bytes->empty()) {
      message_ = "Could not read the file";
      messageAt_ = ImGui::GetTime();
      return;
    }
    loadBytes_ = std::move(*bytes);
    const size_t slash = path.find_last_of('/');
    loadName_ = slash == std::string::npos ? path : path.substr(slash + 1);
    std::snprintf(loadAt_, sizeof loadAt_, "%s", formatAddress(caret_).c_str());
    const size_t hash = loadName_.rfind('#');
    if (hash != std::string::npos && hash + 7 <= loadName_.size()) {
      const std::string aux = loadName_.substr(hash + 3, 4);
      if (std::all_of(aux.begin(), aux.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
        std::snprintf(loadAt_, sizeof loadAt_, "%s", aux.c_str());
      }
    }
    openLoad_ = true;
  });
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

void MemoryViewer::runSearch() {
  matches_.clear();
  matchIndex_ = -1;
  searchStale_ = false;
  const host::MemorySpace *sp = space();
  if (!sp || !searchText_[0]) return;
  std::vector<int> pattern;
  if (searchKind_ == SearchKind::Hex) {
    pattern = parseHex(searchText_, true);
  } else {
    for (const char *c = searchText_; *c; c++) pattern.push_back(static_cast<unsigned char>(*c));
  }
  searchBad_ = pattern.empty();
  if (pattern.empty()) return;
  const std::vector<uint8_t> all = read(sp->base, sp->size);
  const bool text = searchKind_ == SearchKind::Text;
  for (size_t i = 0; i + pattern.size() <= all.size() && matches_.size() < MATCHES_MAX; i++) {
    bool hit = true;
    for (size_t k = 0; k < pattern.size() && hit; k++) {
      const int want = pattern[k];
      if (want < 0) continue;
      const uint8_t have = all[i + k];
      // Text matches with the top bit either way and capitals either way, so
      // a word finds itself on the screen and in a program's data alike.
      hit = text ? std::toupper(have & 0x7F) == std::toupper(want) : have == want;
    }
    if (hit) matches_.push_back(sp->base + static_cast<uint32_t>(i));
  }
  matchLength_ = pattern.size();
  searchBad_ = matches_.empty();
}

void MemoryViewer::search(int direction) {
  if (searchStale_) runSearch();
  if (matches_.empty()) return;
  int index;
  if (direction > 0) {
    auto it = std::upper_bound(matches_.begin(), matches_.end(), std::min(caret_, anchor_));
    index = it == matches_.end() ? 0 : static_cast<int>(it - matches_.begin());
  } else {
    auto it = std::lower_bound(matches_.begin(), matches_.end(), std::min(caret_, anchor_));
    index = it == matches_.begin() ? static_cast<int>(matches_.size()) - 1 : static_cast<int>(it - matches_.begin()) - 1;
  }
  matchIndex_ = index;
  const uint32_t at = matches_[static_cast<size_t>(index)];
  if (at != std::min(caret_, anchor_)) backStack_.push_back(caret_);
  goTo(at, false);
  anchor_ = at + static_cast<uint32_t>(matchLength_) - 1;
  caret_ = at;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void MemoryViewer::draw(bool *open) {
  if (!open || !*open || !profile_) return;
  ui::BeforeWindow("Memory Viewer");
  const Layout layout = layoutFor(columns_, wide_);
  ImGui::SetNextWindowSize(ImVec2(layout.width + SIDEBAR_MIN + 60, 780), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(layout.width + SIDEBAR_MIN + 36, 560), ImVec2(FLT_MAX, FLT_MAX));
  if (!ui::BeginWindow("Memory Viewer", open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::End();
    return;
  }
  if (!snapshot_.valid || !space()) {
    ImGui::TextDisabled("The machine is not running.");
    ImGui::End();
    return;
  }

  drawToolbar();
  ImGui::Dummy(ImVec2(0, 2));
  if (view_ == View::Hex) drawSearchBar();
  else drawViewOptions();
  ImGui::Dummy(ImVec2(0, 4));

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float statusHeight = ImGui::GetFrameHeight() + 4;
  ImGui::BeginGroup();
  const ImVec2 viewSize(layout.width, avail.y - statusHeight);
  switch (view_) {
  case View::Hex: drawGrid(viewSize); break;
  case View::Bitmap: drawBitmap(viewSize); break;
  case View::Screen: drawScreenView(viewSize); break;
  }
  drawStatus();
  ImGui::EndGroup();
  ImGui::SameLine(0, 10);
  drawSidebar(std::max(SIDEBAR_MIN, avail.x - layout.width - 10), avail.y);

  drawContextMenu();
  drawFillPopup();
  drawLoadPopup();
  ImGui::End();
}

void MemoryViewer::drawToolbar() {
  ImGui::PushID("toolbar");
  constexpr float GROUP_GAP = 14.0f;
  constexpr float ITEM_GAP = 6.0f;

  // How the bytes are shown.
  int viewChoice = static_cast<int>(view_);
  if (ui::SegmentedControl("##view", &viewChoice, {"Hex", "Bitmap", "Screen"}, 200)) {
    const View was = view_;
    view_ = static_cast<View>(viewChoice);
    // A bitmap starts where the hex view was looking.
    if (view_ == View::Bitmap && was != View::Bitmap) bitmapTop_ = caret_;
    ImGui::MarkIniSettingsDirty();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Hex: the bytes\nBitmap: the bytes as pixels, to find sprites, fonts and shapes\n"
                      "Screen: a display page as the machine decodes it, whatever is on screen");
  }
  ImGui::SameLine(0, GROUP_GAP);

  // Which memory: the processor's view and the banks themselves, or a IIgs's
  // banks.
  const host::MemorySpace *sp = space();
  if (ui::BeginPopUpButton("##space", sp ? sp->name.c_str() : "", wide_ ? 210.0f : 160.0f)) {
    for (size_t i = 0; i < spaces_.size(); i++) {
      if (ImGui::Selectable(spaces_[i].name.c_str(), i == space_)) selectSpace(i);
    }
    ui::EndPopUpButton();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(wide_ ? "Which bank to show"
                            : "Processor: what a program sees, as the switches stand\n"
                              "Main and Auxiliary RAM: each bank itself, the language card's bank 1 at $C000\n"
                              "ROM: the firmware, whatever is switched over it");
  }

  ImGui::SameLine(0, GROUP_GAP);
  if (iconButton("##back", Icon::Back, "Back", !backStack_.empty())) back();
  ImGui::SameLine(0, 0);
  if (iconButton("##forward", Icon::Forward, "Forward", !forwardStack_.empty())) forward();
  ImGui::SameLine(0, ITEM_GAP);
  ImGui::SetNextItemWidth(150);
  if (gotoBad_) ImGui::PushStyleColor(ImGuiCol_FrameBg, withAlpha(palette().red, 0.18f));
  const bool go = ImGui::InputTextWithHint("##goto", "Go to address or name", gotoText_, sizeof gotoText_,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
  if (gotoBad_) ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("$0400, 9600, HIMEM, COUT%s", wide_ ? ", E1/2000" : "");
  if (go) {
    if (auto at = debugger_.symbols().resolve(gotoText_, addressMask())) {
      // A name or a 16-bit address on a IIgs means the bank being shown.
      uint32_t target = *at;
      if (wide_ && target <= 0xFFFF && !std::strchr(gotoText_, '/') && space()) target |= space()->base & 0xFF0000;
      goTo(target);
      follow_ = Follow::Off;
      gotoBad_ = false;
    } else {
      gotoBad_ = true;
    }
  }
  if (ImGui::IsItemEdited()) gotoBad_ = false;

  if (view_ != View::Hex) {
    ImGui::PopID();
    return;
  }

  // Following something that moves.
  ImGui::SameLine(0, GROUP_GAP);
  static const char *const FOLLOWS[] = {"Follow: Off", "Follow: PC", "Follow: Stack", "Follow: Expression"};
  if (ui::BeginPopUpButton("##follow", FOLLOWS[static_cast<int>(follow_)], 150.0f)) {
    static const char *const NAMES[] = {"Off", "PC", "Stack", "Expression"};
    for (int i = 0; i < 4; i++) {
      if (ImGui::Selectable(NAMES[i], static_cast<int>(follow_) == i)) {
        follow_ = static_cast<Follow>(i);
        ImGui::MarkIniSettingsDirty();
      }
    }
    ui::EndPopUpButton();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep something in view as it moves: the PC, the stack pointer, or a pointer");
  if (follow_ == Follow::Expression) {
    ImGui::SameLine(0, ITEM_GAP);
    ImGui::SetNextItemWidth(170);
    if (ImGui::InputTextWithHint("##followexpr", "PEEK($06)+PEEK($07)*256", followText_, sizeof followText_)) {
      ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Any watch expression: registers, PEEK, arithmetic");
  }

  // How it is shown.
  ImGui::SameLine(0, GROUP_GAP);
  int columnChoice = columns_ == 8 ? 0 : columns_ == 32 ? 2 : 1;
  if (ui::SegmentedControl("##columns", &columnChoice, {"8", "16", "32"}, 108)) {
    const uint32_t keep = rowAddress(topRow_);
    columns_ = columnChoice == 0 ? 8 : columnChoice == 2 ? 32 : 16;
    topRow_ = rowOf(keep);
    ImGui::MarkIniSettingsDirty();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bytes to a row");
  ImGui::SameLine(0, ITEM_GAP);
  int textChoice = textApple_ ? 0 : 1;
  if (ui::SegmentedControl("##text", &textChoice, {"Apple", "ASCII"}, 120)) {
    textApple_ = textChoice == 0;
    ImGui::MarkIniSettingsDirty();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Apple: the screen's codes, inverse and flashing included, with the top bit set for normal text\n"
                      "ASCII: seven-bit characters as they are");
  }
  if (sp && sp->activity) {
    ImGui::SameLine(0, GROUP_GAP);
    if (ui::Switch("Activity", &activityOn_)) ImGui::MarkIniSettingsDirty();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Light every byte the processor reads (blue) and writes (orange) as it happens");
    }
  }
  ImGui::PopID();
}

void MemoryViewer::drawSearchBar() {
  ImGui::PushID("search");
  constexpr float ITEM_GAP = 6.0f;
  int kind = static_cast<int>(searchKind_);
  if (ui::SegmentedControl("##kind", &kind, {"Hex", "Text"}, 104)) {
    searchKind_ = static_cast<SearchKind>(kind);
    searchStale_ = true;
    matches_.clear();
  }
  ImGui::SameLine(0, ITEM_GAP);
  ImGui::SetNextItemWidth(230);
  if (searchBad_ && searchText_[0]) ImGui::PushStyleColor(ImGuiCol_FrameBg, withAlpha(palette().red, 0.18f));
  const bool enter = ImGui::InputTextWithHint("##find", searchKind_ == SearchKind::Hex ? "Find bytes: A9 ?? 8D" : "Find text",
                                              searchText_, sizeof searchText_, ImGuiInputTextFlags_EnterReturnsTrue);
  if (searchBad_ && searchText_[0]) ImGui::PopStyleColor();
  if (ImGui::IsItemEdited()) {
    searchStale_ = true;
    searchBad_ = false;
    matches_.clear();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(searchKind_ == SearchKind::Hex ? "Bytes in hex, ?? for any byte; Return finds the next"
                                                     : "Text, with the top bit set or clear and in either case");
  }
  if (enter) {
    searchStale_ = true;
    search(1);
    ImGui::SetKeyboardFocusHere(-1);
  }
  ImGui::SameLine(0, ITEM_GAP);
  if (iconButton("##prev", Icon::Previous, "Previous match", searchText_[0] != 0)) search(-1);
  ImGui::SameLine(0, 0);
  if (iconButton("##next", Icon::Next, "Next match", searchText_[0] != 0)) search(1);
  if (!searchStale_ && searchText_[0]) {
    ImGui::SameLine(0, ITEM_GAP);
    ImGui::AlignTextToFramePadding();
    if (matches_.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette().red), "Not found");
    else if (matchIndex_ >= 0) ImGui::TextDisabled("%d of %zu%s", matchIndex_ + 1, matches_.size(), matches_.size() >= MATCHES_MAX ? "+" : "");
    else ImGui::TextDisabled("%zu found%s", matches_.size(), matches_.size() >= MATCHES_MAX ? "+" : "");
  }

  // Editing, on the right.
  const float right = ImGui::GetFrameHeight() * 2 + 8 + ImGui::CalcTextSize("Load File\xE2\x80\xA6").x + 40;
  ImGui::SameLine(0, 12);
  if (ImGui::GetContentRegionAvail().x > right) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - right);
  }
  if (iconButton("##undo", Icon::Undo, "Undo the last edit", !undo_.empty())) undo();
  ImGui::SameLine(0, 0);
  if (iconButton("##redo", Icon::Redo, "Redo", !redo_.empty())) redo();
  ImGui::SameLine(0, ITEM_GAP);
  if (ui::Button("Load File\xE2\x80\xA6")) loadFile();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put a file's bytes into memory, at the caret or the address its name gives");
  ImGui::PopID();
}

void MemoryViewer::drawGrid(ImVec2 size) {
  ImGui::BeginChild("##grid", size, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 end(origin.x + avail.x, origin.y + avail.y);
  draw->AddRectFilled(origin, end, well(), 8.0f);
  draw->AddRect(origin, end, ImGui::GetColorU32(ImGuiCol_Border), 8.0f);

  const Layout l = layoutFor(columns_, wide_);
  const host::MemorySpace &sp = *space();
  const ImGuiIO &io = ImGui::GetIO();
  const double now = ImGui::GetTime();
  const std::vector<Region> regionList = regions();
  const DebugSymbols &symbols = debugger_.symbols();

  ImGui::PushFont(ui::monoFont(), 0.0f);
  const float lineH = l.lineH;
  const float charW = l.charW;
  const float textH = ImGui::GetTextLineHeight();
  const float headerH = lineH + 4;
  const float rowsTop = origin.y + headerH + 2;
  const float rowsBottom = end.y - 6;
  visibleRows_ = std::max(4, static_cast<int>(std::ceil((rowsBottom - rowsTop) / lineH)) + 1);
  const int maxTop = std::max(0, rowCount() - (visibleRows_ - 2));
  const float x0 = origin.x;
  const float mapX = x0 + l.mapX;

  // Scrolling, in pixels as the CPU debugger's listing scrolls.
  const bool hovered = ImGui::IsWindowHovered();
  if (hovered && io.MouseWheel != 0 && !draggingMap_) {
    scrollPixels_ -= io.MouseWheel * lineH * 3;
    follow_ = Follow::Off;
  }
  if (hovered && ImGui::IsMouseClicked(3)) back();
  if (hovered && ImGui::IsMouseClicked(4)) forward();
  while (scrollPixels_ < 0 && topRow_ > 0) {
    topRow_--;
    scrollPixels_ += lineH;
  }
  while (scrollPixels_ >= lineH && topRow_ < maxTop) {
    topRow_++;
    scrollPixels_ -= lineH;
  }
  topRow_ = std::clamp(topRow_, 0, maxTop);
  if (topRow_ == 0 && scrollPixels_ < 0) scrollPixels_ = 0;
  if (topRow_ >= maxTop && scrollPixels_ > 0) scrollPixels_ = 0;

  // The keyboard, while the grid has it.
  gridFocused_ = ImGui::IsWindowFocused();
  // The grid takes typed hex itself, so it claims the keyboard: unclaimed,
  // ImGui's Cocoa backend passes every key on to macOS, which beeps.
  if (gridFocused_) ImGui::SetNextFrameWantCaptureKeyboard(true);
  if (gridFocused_ && !io.WantTextInput) {
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    const bool shift = io.KeyShift;
    int64_t move = 0;
    if (key(ImGuiKey_LeftArrow) || key(ImGuiKey_Backspace)) move = -1;
    if (key(ImGuiKey_RightArrow)) move = 1;
    if (key(ImGuiKey_UpArrow)) move = -columns_;
    if (key(ImGuiKey_DownArrow)) move = columns_;
    if (key(ImGuiKey_PageUp)) move = -static_cast<int64_t>(columns_) * std::max(1, visibleRows_ - 3);
    if (key(ImGuiKey_PageDown)) move = static_cast<int64_t>(columns_) * std::max(1, visibleRows_ - 3);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) move = -static_cast<int64_t>((caret_ - sp.base) % columns_);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) move = columns_ - 1 - static_cast<int64_t>((caret_ - sp.base) % columns_);
    if (move) {
      const int64_t to = std::clamp<int64_t>(static_cast<int64_t>(caret_) + move, sp.base, sp.base + sp.size - 1);
      setCaret(static_cast<uint32_t>(to), shift);
      reveal(caret_);
      follow_ = Follow::Off;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
      textColumn_ = !textColumn_;
      highNibbleTyped_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      anchor_ = caret_;
      highNibbleTyped_ = false;
    }
    if (!io.KeyCtrl && !io.KeySuper) {
      for (ImWchar c : io.InputQueueCharacters) {
        if (textColumn_) {
          typeCharacter(c);
        } else if (c < 128 && std::isxdigit(static_cast<int>(c))) {
          typeNibble(static_cast<int>(std::strtol(std::string(1, static_cast<char>(c)).c_str(), nullptr, 16)));
        }
      }
    }
    ImGui::GetIO().InputQueueCharacters.resize(0);
  }

  // The column heads, the one under the pointer lit.
  int hoverColumn = -1;
  hovered_.reset();
  const ImVec2 mouse = io.MousePos;
  const bool mouseInRows = mouse.y >= rowsTop && mouse.y < rowsBottom && mouse.x >= x0 && mouse.x < mapX - 8;
  bool mouseInText = false;
  if (mouseInRows) {
    const int row = topRow_ + static_cast<int>((mouse.y - rowsTop + scrollPixels_) / lineH);
    int column = -1;
    if (mouse.x >= x0 + l.textX0 - charW * 0.5f) {
      column = static_cast<int>((mouse.x - x0 - l.textX0) / l.textCellW);
      mouseInText = true;
    } else if (mouse.x >= x0 + l.hexX0 - charW * 0.5f) {
      for (int i = columns_ - 1; i >= 0; i--) {
        if (mouse.x >= x0 + l.hexX(i) - charW * 0.5f) {
          column = i;
          break;
        }
      }
    } else {
      column = 0;
    }
    column = std::clamp(column, 0, columns_ - 1);
    if (row >= 0 && row < rowCount()) {
      hovered_ = rowAddress(row) + static_cast<uint32_t>(column);
      hoverColumn = column;
    }
  }

  const float headY = origin.y + 5;
  draw->AddText(ImVec2(x0 + l.addrX, headY), secondary(), wide_ ? "BANK/ADDR" : "ADDR");
  for (int i = 0; i < columns_; i++) {
    char head[4];
    std::snprintf(head, sizeof head, "%02X", i);
    const bool lit = i == hoverColumn || (caret_ - sp.base) % columns_ == static_cast<uint32_t>(i);
    draw->AddText(ImVec2(x0 + l.hexX(i), headY), lit ? ui::accentText() : secondary(), head);
    char one[2] = {"0123456789ABCDEF"[i & 15], 0};
    draw->AddText(ImVec2(x0 + l.textX0 + i * l.textCellW, headY), lit ? ui::accentText() : ui::faintText(), one);
  }
  draw->AddLine(ImVec2(x0 + 8, origin.y + headerH), ImVec2(mapX - 10, origin.y + headerH),
                ImGui::GetColorU32(ImGuiCol_Border));

  // The grid itself: an invisible button over the rows takes the clicks.
  ImGui::SetCursorScreenPos(ImVec2(x0, rowsTop));
  ImGui::InvisibleButton("##rows", ImVec2(mapX - 8 - x0, std::max(1.0f, rowsBottom - rowsTop)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  if (ImGui::IsItemActivated() && hovered_ && ImGui::IsMouseDown(0)) {
    setCaret(*hovered_, io.KeyShift);
    textColumn_ = mouseInText;
    selecting_ = true;
    follow_ = Follow::Off;
  }
  if (selecting_ && ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f)) {
    // Dragging past the top or bottom scrolls.
    if (mouse.y < rowsTop) scrollPixels_ -= lineH * 0.5f;
    if (mouse.y > rowsBottom) scrollPixels_ += lineH * 0.5f;
    const int row = std::clamp(topRow_ + static_cast<int>(std::floor((mouse.y - rowsTop + scrollPixels_) / lineH)), 0,
                               rowCount() - 1);
    const uint32_t column = hoverColumn >= 0 ? static_cast<uint32_t>(hoverColumn) : (caret_ - sp.base) % columns_;
    caret_ = rowAddress(row) + column;
    highNibbleTyped_ = false;
  }
  if (!ImGui::IsItemActive()) selecting_ = false;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hovered_) {
    menuAddress_ = *hovered_;
    if (!(hasSelection() && menuAddress_ >= selectionLow() && menuAddress_ <= selectionHigh())) setCaret(menuAddress_, false);
    ImGui::OpenPopup("##memmenu");
  }

  // What is marked where, worked out once for the rows in view.
  const bool processor = sp.processor;
  const uint32_t pc = snapshot_.cpu.pc;
  const uint32_t stack = stackAddress();
  const bool page1Stack = !wide_ || snapshot_.cpu.emulation();
  const bool selection = hasSelection();
  const uint32_t selLo = selectionLow(), selHi = selectionHigh();
  const auto &bps = debugger_.breakpoints().all();
  const bool glowing = !readGlow_.empty() && readGlow_.size() == sp.size;
  auto matchAt = [&](uint32_t a) -> int {
    if (matches_.empty()) return 0;
    auto it = std::upper_bound(matches_.begin(), matches_.end(), a);
    if (it == matches_.begin()) return 0;
    --it;
    if (a >= *it + matchLength_) return 0;
    return matchIndex_ >= 0 && *it == matches_[static_cast<size_t>(matchIndex_)] ? 2 : 1;
  };

  const ImVec2 clipMin(x0 + 2, rowsTop), clipMax(mapX - 6, rowsBottom);
  draw->PushClipRect(clipMin, clipMax, true);
  for (int r = 0; r <= visibleRows_; r++) {
    const int row = topRow_ + r;
    if (row >= rowCount()) break;
    const float y = rowsTop + r * lineH - scrollPixels_;
    if (y > rowsBottom) break;
    const uint32_t rowAddr = rowAddress(row);
    const float ty = y + (lineH - textH) * 0.5f;

    // The region down the left edge, and the row under the pointer.
    if (const Region *region = regionFor(rowAddr, regionList); region && region->hue >= 0) {
      draw->AddRectFilled(ImVec2(x0 + 5, y), ImVec2(x0 + 8, y + lineH), withAlpha(hueColour(region->hue, p), 0.85f));
    }
    if (hovered_ && rowOf(*hovered_) == row) {
      draw->AddRectFilled(ImVec2(x0 + 10, y), ImVec2(mapX - 10, y + lineH), text(0.035f), 4.0f);
    }
    const bool caretRow = rowOf(caret_) == row;
    const std::string address = formatAddress(rowAddr);
    draw->AddText(ImVec2(x0 + l.addrX, ty), caretRow ? ui::accentText() : secondary(), address.c_str());

    for (int i = 0; i < columns_; i++) {
      const uint32_t a = rowAddr + static_cast<uint32_t>(i);
      const float cx = x0 + l.hexX(i);
      const ImVec2 cellMin(cx - charW * 0.45f, y + 1), cellMax(cx + charW * 2.45f, y + lineH - 1);
      const float tx = x0 + l.textX0 + i * l.textCellW;
      const ImVec2 tMin(tx - 0.5f, y + 1), tMax(tx + l.textCellW - 0.5f, y + lineH - 1);
      const std::optional<uint8_t> value = byteAt(a);

      // Backgrounds, quietest first.
      auto both = [&](ImU32 colour, float rounding = 3.0f) {
        draw->AddRectFilled(cellMin, cellMax, colour, rounding);
        draw->AddRectFilled(tMin, tMax, colour, 2.0f);
      };
      if (processor && page1Stack && (a & 0xFFFF00) == 0x000100 && (a & 0xFF) > (stack & 0xFF)) {
        both(withAlpha(p.yellow, 0.09f));
      }
      if (glowing) {
        const float rd = readGlow_[a - sp.base], wr = writeGlow_[a - sp.base];
        if (rd > 0.02f) both(withAlpha(p.blue, rd * 0.42f));
        if (wr > 0.02f) both(withAlpha(p.orange, wr * 0.55f));
      }
      if (auto it = changed_.find(a); it != changed_.end()) {
        const float t = 1.0f - static_cast<float>((now - it->second) / CHANGE_FADE);
        if (t > 0) both(accent(0.5f * t * t));
      }
      if (processor) {
        for (const Breakpoint &b : bps) {
          if (!b.enabled || !b.contains(a)) continue;
          if (b.kind == Breakpoint::Kind::Read || b.kind == Breakpoint::Kind::Write || b.kind == Breakpoint::Kind::ReadWrite) {
            const ImU32 colour = kindColour(b.kind, p);
            draw->AddRectFilled(cellMin, cellMax, withAlpha(colour, 0.16f), 3.0f);
            draw->AddTriangleFilled(ImVec2(cellMax.x - 6, cellMin.y), ImVec2(cellMax.x, cellMin.y),
                                    ImVec2(cellMax.x, cellMin.y + 6), colour);
          } else if (b.kind == Breakpoint::Kind::Exec && b.start == a) {
            draw->AddCircleFilled(ImVec2(cellMin.x + 1, (cellMin.y + cellMax.y) * 0.5f), 3.0f, p.red);
          }
        }
      }
      if (const int m = matchAt(a)) both(withAlpha(p.yellow, m == 2 ? 0.55f : 0.28f));
      if (selection && a >= selLo && a <= selHi) both(accent(0.26f), 0.0f);

      // The PC's instruction and the stack pointer, outlined.
      if (processor && a >= pc && a < pc + static_cast<uint32_t>(snapshot_.pcLength)) {
        draw->AddRect(cellMin, cellMax, p.green, 3.0f, 0, 1.5f);
        draw->AddRect(tMin, tMax, withAlpha(p.green, 0.7f), 2.0f);
      }
      if (processor && a == stack) draw->AddRect(cellMin, cellMax, p.yellow, 3.0f, 0, 1.5f);

      // The caret: solid in the column being typed into, an outline in the
      // other.
      if (a == caret_) {
        const bool hexActive = !textColumn_;
        const float focus = gridFocused_ ? 1.0f : 0.55f;
        draw->AddRect(cellMin, cellMax, accent(hexActive ? focus : 0.45f * focus), 3.0f, 0, hexActive ? 2.0f : 1.0f);
        draw->AddRect(tMin, tMax, accent(!hexActive ? focus : 0.45f * focus), 2.0f, 0, !hexActive ? 2.0f : 1.0f);
        if (highNibbleTyped_ && hexActive) {
          draw->AddLine(ImVec2(cx + charW, y + lineH - 3), ImVec2(cx + charW * 2, y + lineH - 3), accent(), 2.0f);
        }
      }

      if (!value) {
        draw->AddText(ImVec2(cx, ty), ui::faintText(), "--");
        continue;
      }
      // The byte: zero quietly, a recent change at full strength over its
      // highlight.
      char hex[3];
      std::snprintf(hex, sizeof hex, "%02X", *value);
      const bool fresh = changed_.count(a) && now - changed_.at(a) < CHANGE_FADE * 0.5;
      ImU32 colour = *value == 0 ? ui::faintText() : text();
      if (fresh) colour = text();
      draw->AddText(ImVec2(cx, ty), colour, hex);
      // A name is marked under its byte, except the built-in names for the
      // zero page and the soft switches, which would mark nearly all of both.
      if (auto sym = symbols.lookup(a); sym && sym->category != DebugSymbols::Category::ZeroPage &&
                                        sym->category != DebugSymbols::Category::SoftSwitch) {
        const ImU32 sc = withAlpha(symbolColour(sym->category, p), 0.8f);
        for (float dx = 0; dx < charW * 2; dx += 3) {
          draw->AddLine(ImVec2(cx + dx, ty + textH + 0.5f), ImVec2(cx + std::min(dx + 1.5f, charW * 2), ty + textH + 0.5f), sc, 1.0f);
        }
      }

      // The same byte as text.
      const Glyph g = glyphFor(*value, textApple_);
      char ch[2] = {g.c, 0};
      switch (g.style) {
      case Glyph::Normal:
        draw->AddText(ImVec2(tx + (l.textCellW - charW) * 0.5f, ty), text(), ch);
        break;
      case Glyph::Dim:
        draw->AddText(ImVec2(tx + (l.textCellW - charW) * 0.5f, ty), ui::faintText(), ch);
        break;
      case Glyph::Inverse:
      case Glyph::Flash:
        // Drawn still, never flashing: a flashing cell is tinted instead.
        draw->AddRectFilled(ImVec2(tMin.x + 0.5f, tMin.y + 1), ImVec2(tMax.x - 0.5f, tMax.y - 1),
                            g.style == Glyph::Inverse ? text(0.78f) : withAlpha(p.purple, 0.75f), 1.5f);
        draw->AddText(ImVec2(tx + (l.textCellW - charW) * 0.5f, ty), ImGui::GetColorU32(ImGuiCol_WindowBg), ch);
        break;
      }
    }
  }
  draw->PopClipRect();
  ImGui::PopFont();

  drawMap(ImVec2(mapX, origin.y + 6), ImVec2(MAP_WIDTH, avail.y - 12));
  ImGui::EndChild();
}

void MemoryViewer::drawMap(ImVec2 origin, ImVec2 size) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const host::MemorySpace &sp = *space();
  const std::vector<Region> regionList = regions();
  const float x0 = origin.x + 6, x1 = origin.x + size.x;
  const float y0 = origin.y, y1 = origin.y + size.y;
  const float h = y1 - y0;
  const float span = static_cast<float>(sp.size);
  auto yFor = [&](uint32_t address) { return y0 + (address - sp.base) / span * h; };

  // Interaction first, so the drawing shows this frame's position.
  ImGui::SetCursorScreenPos(ImVec2(origin.x, y0));
  ImGui::InvisibleButton("##map", ImVec2(size.x, std::max(1.0f, h)));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  auto addressAt = [&](float y) {
    const float f = std::clamp((y - y0) / h, 0.0f, 0.9999f);
    return sp.base + (static_cast<uint32_t>(f * span) & ~0xFFu);
  };
  if (ImGui::IsItemActivated()) draggingMap_ = true;
  if (ImGui::IsItemActive() && draggingMap_) {
    const uint32_t at = addressAt(mouse.y);
    if (view_ == View::Bitmap) {
      const uint32_t rowBytes = static_cast<uint32_t>(bitmapWidth_) * (bitmapTiles_ ? 8 : 1);
      bitmapTop_ = std::max(sp.base, at - std::min(at - sp.base, rowBytes * 8));
    } else {
      topRow_ = std::clamp(rowOf(at) - visibleRows_ / 2, 0, std::max(0, rowCount() - visibleRows_ + 2));
    }
    scrollPixels_ = 0;
    follow_ = Follow::Off;
  }
  if (ImGui::IsItemDeactivated()) draggingMap_ = false;

  // The regions down its left edge, then the bytes.
  const uint32_t bank = sp.base & 0xFF0000;
  const uint32_t lowest = sp.base & 0xFFFF, highest = lowest + sp.size - 1;
  for (const Region &r : regionList) {
    if (r.hue < 0) continue;
    const uint32_t lo = std::max(r.start, lowest), hi = std::min(r.end, highest);
    if (hi < lo) continue;
    const float ya = yFor(bank | lo), yb = yFor((bank | hi) + 1);
    draw->AddRectFilled(ImVec2(origin.x, ya), ImVec2(origin.x + 3, std::max(ya + 1, yb)), hueColour(r.hue, p), 1.0f);
  }
  if (mapTexture_ != ImTextureID_Invalid && mapSpace_ == space_) {
    draw->AddImageRounded(ImTextureRef(mapTexture_), ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32_WHITE, 4.0f);
  }
  draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), ImGui::GetColorU32(ImGuiCol_Border), 4.0f);

  // Marks: matches and bookmarks at the right edge, breakpoints, the
  // selection, the stack pointer and the PC across it.
  for (size_t i = 0; i < matches_.size(); i += std::max<size_t>(1, matches_.size() / 2000)) {
    const float y = yFor(matches_[i]);
    draw->AddLine(ImVec2(x1 - 6, y), ImVec2(x1, y), p.yellow, 2.0f);
  }
  for (uint32_t b : bookmarks_) {
    if (b < sp.base || b >= sp.base + sp.size) continue;
    const float y = yFor(b);
    draw->AddTriangleFilled(ImVec2(x1 + 1, y - 4), ImVec2(x1 + 1, y + 4), ImVec2(x1 - 5, y), p.blue);
  }
  if (sp.processor) {
    for (const Breakpoint &b : debugger_.breakpoints().all()) {
      if (!b.enabled || b.kind == Breakpoint::Kind::Stack || b.end < sp.base || b.start >= sp.base + sp.size) continue;
      const float ya = yFor(std::max(b.start, sp.base)), yb = yFor(std::min(b.end + 1, sp.base + sp.size));
      draw->AddRectFilled(ImVec2(x0 - 4, ya), ImVec2(x0 - 1, std::max(yb, ya + 2)), kindColour(b.kind, p), 1.0f);
    }
  }
  if (hasSelection()) {
    draw->AddRectFilled(ImVec2(x0, yFor(selectionLow())), ImVec2(x1, std::max(yFor(selectionHigh() + 1), yFor(selectionLow()) + 2)),
                        accent(0.35f));
  }
  if (sp.processor) {
    const uint32_t stack = stackAddress();
    if (stack >= sp.base && stack < sp.base + sp.size) {
      const float y = yFor(stack);
      draw->AddLine(ImVec2(x0, y), ImVec2(x1, y), p.yellow, 1.5f);
    }
    const uint32_t pc = snapshot_.cpu.pc;
    if (pc >= sp.base && pc < sp.base + sp.size) {
      const float y = yFor(pc);
      draw->AddLine(ImVec2(x0, y), ImVec2(x1, y), p.green, 1.5f);
      draw->AddTriangleFilled(ImVec2(x0 - 6, y - 4), ImVec2(x0 - 6, y + 4), ImVec2(x0, y), p.green);
    }
  }

  // The part in view.
  uint32_t first = rowAddress(topRow_);
  uint32_t last = std::min(rowAddress(topRow_ + visibleRows_), spaceEnd());
  if (view_ == View::Bitmap) {
    first = bitmapTop_;
    last = std::min<uint32_t>(bitmapTop_ + static_cast<uint32_t>(bitmapBytes_.size()), spaceEnd());
  }
  const float va = yFor(first), vb = std::max(yFor(last), va + 3);
  draw->AddRectFilled(ImVec2(x0 - 1, va), ImVec2(x1 + 1, vb), accent(draggingMap_ ? 0.22f : 0.14f), 2.0f);
  draw->AddRect(ImVec2(x0 - 1, va), ImVec2(x1 + 1, vb), accent(0.9f), 2.0f, 0, 1.5f);

  if (hovered || draggingMap_) {
    const uint32_t at = addressAt(mouse.y);
    const Region *region = regionFor(at, regionList);
    ImGui::SetTooltip("%s-%s  %s", formatAddress(at).c_str(), formatAddress(at + 0xFF).c_str(),
                      region ? region->name : "");
  }
}

void MemoryViewer::drawStatus() {
  const Palette p = palette();
  const uint32_t at = hovered_.value_or(caret_);
  const std::vector<Region> regionList = regions();
  const Region *region = regionFor(at, regionList);
  ImGui::Dummy(ImVec2(0, 2));
  const float startX = ImGui::GetCursorPosX();
  ImGui::AlignTextToFramePadding();
  ImGui::PushFont(ui::monoFont(), 0.0f);
  ImGui::TextUnformatted(formatAddress(at).c_str());
  ImGui::PopFont();
  if (auto v = byteAt(at)) {
    ImGui::SameLine(0, 10);
    ImGui::PushFont(ui::monoFont(), 0.0f);
    ImGui::TextDisabled("$%02X  %3d", *v, *v);
    ImGui::PopFont();
  }
  if (region) {
    ImGui::SameLine(0, 12);
    ImGui::TextDisabled("%s", region->name);
  }
  if (auto sym = debugger_.symbols().lookup(at)) {
    ImGui::SameLine(0, 12);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(symbolColour(sym->category, p)), "%s", sym->name.c_str());
    if (!sym->description.empty()) {
      ImGui::SameLine(0, 6);
      ImGui::TextDisabled("%s", sym->description.c_str());
    }
  }
  const double age = ImGui::GetTime() - messageAt_;
  if (!message_.empty() && age < 3.5) {
    const float width = ImGui::CalcTextSize(message_.c_str()).x;
    ImGui::SameLine(0, 12);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), startX + layoutFor(columns_, wide_).width - width - 6));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::accentText(std::min(1.0f, static_cast<float>(3.5 - age)))), "%s",
                       message_.c_str());
  }
}

void MemoryViewer::drawSidebar(float width, float height) {
  ImGui::BeginChild("##side", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  const float inner = ImGui::GetContentRegionAvail().x;
  drawInspector(inner);
  drawSelectionCard(inner);
  if (snapshot_.haveSwitches) drawBanking(inner);
  drawPlaces(inner);
  ImGui::EndChild();
}

void MemoryViewer::drawInspector(float width) {
  cardAround("AT THE CARET", width, [&](float w) {
    const Palette p = palette();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const auto &b = snapshot_.atCaret;
    const std::vector<Region> regionList = regions();
    const Region *region = regionFor(caret_, regionList);

    // The address, large, and what is there.
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.45f);
    ImGui::TextUnformatted(("$" + formatAddress(caret_)).c_str());
    ImGui::PopFont();
    if (region) {
      ImGui::SameLine(0, 10);
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float ph = pillHeight();
      const float lineH = ImGui::GetFontSize() * 1.45f;
      pill(draw, ImVec2(at.x, at.y + (lineH - ph) * 0.5f + 2), region->name, region->hue >= 0,
           region->hue >= 0 ? hueColour(region->hue, p) : 0);
      ImGui::NewLine();
    }
    if (auto sym = debugger_.symbols().lookup(caret_)) {
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(symbolColour(sym->category, p)), "%s", sym->name.c_str());
      if (!sym->description.empty()) {
        ImGui::SameLine(0, 6);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextDisabled("%s", sym->description.c_str());
        ImGui::PopTextWrapPos();
      }
    }
    ImGui::Dummy(ImVec2(0, 4));

    // A row: a name on the left, the value in figures on the right.
    const float valueX = 88;
    auto row = [&](const char *name, const std::string &value, ImU32 colour = 0) {
      ImGui::TextDisabled("%s", name);
      ImGui::SameLine(valueX);
      ImGui::PushFont(ui::monoFont(), 0.0f);
      if (colour) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colour), "%s", value.c_str());
      else ImGui::TextUnformatted(value.c_str());
      ImGui::PopFont();
    };
    char s[96];
    const uint8_t v = b[0];
    // The signed reading only when it differs from the unsigned one.
    if (v & 0x80) std::snprintf(s, sizeof s, "$%02X  %u  %d", v, v, static_cast<int8_t>(v));
    else std::snprintf(s, sizeof s, "$%02X  %u", v, v);
    row("Byte", s);
    const Glyph g = glyphFor(v, textApple_);
    std::snprintf(s, sizeof s, "'%c'  %s", g.c,
                  g.style == Glyph::Inverse ? "inverse" : g.style == Glyph::Flash ? "flashing" : g.style == Glyph::Dim ? "control" : "normal");
    row("Character", s);

    // The bits, each one a switch.
    ImGui::TextDisabled("Bits");
    ImGui::SameLine(valueX);
    {
      const float box = std::min(22.0f, (w - valueX - 7 * 3) / 8);
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.7f);
      for (int bit = 7; bit >= 0; bit--) {
        const int i = 7 - bit;
        const ImVec2 a(at.x + i * (box + 3) + (i >= 4 ? 4 : 0), at.y);
        const ImVec2 e(a.x + box, a.y + box);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(bit);
        const bool clicked = ImGui::InvisibleButton("##bit", ImVec2(box, box));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool lit = (v >> bit) & 1;
        draw->AddRectFilled(a, e, lit ? accent(hov ? 0.85f : 1.0f) : text(hov ? 0.12f : 0.06f), 4.0f);
        char digit[2] = {static_cast<char>('0' + bit), 0};
        const ImVec2 ds = ImGui::CalcTextSize(digit);
        draw->AddText(ImVec2(a.x + (box - ds.x) * 0.5f, a.y + (box - ds.y) * 0.5f), lit ? IM_COL32_WHITE : secondary(), digit);
        if (hov) ImGui::SetTooltip("Bit %d%s: click to flip", bit, bit == 7 && region && region->hue == PURPLE ? " (the hi-res palette bit)" : "");
        if (clicked) write(caret_, {static_cast<uint8_t>(v ^ (1u << bit))});
      }
      ImGui::PopFont();
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + box + 4));
      ImGui::Dummy(ImVec2(0, 0));
    }

    const uint16_t le = static_cast<uint16_t>(b[0] | (b[1] << 8));
    const uint16_t be = static_cast<uint16_t>((b[0] << 8) | b[1]);
    if (le & 0x8000) std::snprintf(s, sizeof s, "$%04X  %u  %d", le, le, static_cast<int16_t>(le));
    else std::snprintf(s, sizeof s, "$%04X  %u", le, le);
    row("Word", s);
    std::snprintf(s, sizeof s, "$%04X  %u", be, be);
    row("Word (BE)", s);
    if (wide_) {
      const uint32_t lng = static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16));
      std::snprintf(s, sizeof s, "$%06X", lng);
      row("Long", s);
    }
    const uint32_t dword = static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24));
    std::snprintf(s, sizeof s, "$%08X", dword);
    row("Dword", s);
    const double real = ApplesoftVars::decodeFloat(b.data());
    std::snprintf(s, sizeof s, "%.9g", real);
    row("Applesoft", s);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The five bytes here as an Applesoft real (MBF)");

    // A pointer, which is a link.
    ImGui::TextDisabled("Pointer");
    ImGui::SameLine(valueX);
    const uint32_t target = wide_ ? ((space()->base & 0xFF0000) | le) : le;
    const std::string label = "\xE2\x86\x92 $" + formatAddress(target);
    ImGui::PushFont(ui::monoFont(), 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::accentText());
    const bool go = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(0, 0));
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) {
      auto sym = debugger_.symbols().lookup(target);
      ImGui::SetTooltip("Go to the word here as an address%s%s", sym ? ": " : "", sym ? sym->name.c_str() : "");
    }
    if (go) {
      goTo(target);
      follow_ = Follow::Off;
    }
  });
}

void MemoryViewer::drawSelectionCard(float width) {
  cardAround("SELECTION", width, [&](float w) {
    if (!hasSelection()) {
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
      ImGui::TextDisabled("Drag across bytes, or Shift-click or Shift-arrow, to select a range.");
      ImGui::PopTextWrapPos();
      return;
    }
    const float valueX = 88;
    char s[96];
    auto row = [&](const char *name, const char *value) {
      ImGui::TextDisabled("%s", name);
      ImGui::SameLine(valueX);
      ImGui::PushFont(ui::monoFont(), 0.0f);
      ImGui::TextUnformatted(value);
      ImGui::PopFont();
    };
    std::snprintf(s, sizeof s, "$%s-$%s", formatAddress(selectionLow()).c_str(), formatAddress(selectionHigh()).c_str());
    row("Range", s);
    const uint32_t length = selectionHigh() - selectionLow() + 1;
    std::snprintf(s, sizeof s, "%u  ($%X)", length, length);
    row("Length", s);
    if (stats_.space == space_ && stats_.low == selectionLow() && stats_.high == selectionHigh()) {
      std::snprintf(s, sizeof s, "$%02X  /  $%04X", stats_.sum8, stats_.sum16);
      row("Sum", s);
      std::snprintf(s, sizeof s, "$%02X", stats_.xor8);
      row("XOR", s);
      std::snprintf(s, sizeof s, "$%04X", stats_.crc16);
      row("CRC-16", s);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("CRC-16/XMODEM%s", stats_.length < length ? ", over the first 64K" : "");
      std::snprintf(s, sizeof s, "%zu", stats_.zeros);
      row("Zeros", s);
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (ui::Button("Copy")) copySelection(0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("As hex; the context menu copies it as source");
    ImGui::SameLine(0, 6);
    ImGui::BeginDisabled(!space()->writable);
    if (ui::Button("Fill\xE2\x80\xA6")) openFill_ = true;
    ImGui::EndDisabled();
    ImGui::SameLine(0, 6);
    if (ui::Button("Save\xE2\x80\xA6")) saveSelection();
    if (space()->processor) {
      ImGui::SameLine(0, 6);
      if (ui::Button("Watch")) {
        const bool added = debugger_.addBreakpoint(Breakpoint::Kind::Write, selectionLow(), selectionHigh());
        message_ = added ? "The machine will stop on a write here" : "There is already a breakpoint there";
        messageAt_ = ImGui::GetTime();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop when anything writes into the range");
    }
  });
}

// Which bank each part of the map is read from and written to, as the
// switches stand, and the switches themselves.
void MemoryViewer::drawBanking(float width) {
  const SoftSwitches &sw = snapshot_.switches;
  const bool aux = profile_ && profile_->caps.hasAuxRam;
  cardAround(wide_ ? "BANK $00 MAP" : "MEMORY MAP", width, [&](float w) {
    const Palette p = palette();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float rangeW = ImGui::CalcTextSize("$0000").x * 2.2f;
    auto chip = [&](float x, float y, const char *label, ImU32 colour) {
      return pill(draw, ImVec2(x, y), label, colour != 0, colour, 0.72f);
    };
    const ImU32 MAIN = p.blue, AUX = p.purple, ROM = p.red, CARD = p.orange;
    const float rowH = pillHeight(0.72f) + 4;
    const float readX = rangeW + 14, writeX = readX + (w - readX) * 0.5f;
    {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
      draw->AddText(ImVec2(at.x + readX, at.y), secondary(), "READS");
      draw->AddText(ImVec2(at.x + writeX, at.y), secondary(), "WRITES");
      ImGui::PopFont();
      ImGui::Dummy(ImVec2(w, ImGui::GetFontSize() * 0.8f));
    }
    auto line = [&](const char *range, const char *read, ImU32 readColour, const char *written, ImU32 writeColour,
                    const char *tip) {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
      draw->AddText(ImVec2(at.x, at.y + 2), text(0.8f), range);
      ImGui::PopFont();
      chip(at.x + readX, at.y, read, readColour);
      chip(at.x + writeX, at.y, written, writeColour);
      ImGui::Dummy(ImVec2(w, rowH));
      if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    auto bank = [&](bool isAux) { return isAux ? "AUX" : "MAIN"; };
    auto bankColour = [&](bool isAux) { return isAux ? AUX : MAIN; };
    if (aux) {
      line("0000-01FF", bank(sw.altzp), bankColour(sw.altzp), bank(sw.altzp), bankColour(sw.altzp),
           "Zero page and stack: ALTZP");
      line("0200-BFFF", bank(sw.ramrd), bankColour(sw.ramrd), bank(sw.ramwrt), bankColour(sw.ramwrt),
           "RAMRD reads, RAMWRT writes");
      if (sw.store80) {
        line("0400-07FF", bank(sw.page2), bankColour(sw.page2), bank(sw.page2), bankColour(sw.page2),
             "80STORE on: PAGE2 chooses the text page's bank");
        if (sw.hires) {
          line("2000-3FFF", bank(sw.page2), bankColour(sw.page2), bank(sw.page2), bankColour(sw.page2),
               "80STORE and HIRES on: PAGE2 chooses the hi-res page's bank");
        }
      }
    } else {
      line("0000-BFFF", "MAIN", MAIN, "MAIN", MAIN, nullptr);
    }
    if (!wide_ && profile_ && profile_->caps.hasExpansionSlots && aux) {
      line("C100-CFFF", sw.intcxrom ? "ROM" : "SLOTS", sw.intcxrom ? ROM : CARD, "-", 0,
           "INTCXROM: the internal firmware or the cards'");
    }
    char lc[24];
    std::snprintf(lc, sizeof lc, "LC%d %s", sw.lcram2 ? 2 : 1, aux && sw.altzp ? "AUX" : "");
    char lcHigh[24];
    std::snprintf(lcHigh, sizeof lcHigh, "LC %s", aux && sw.altzp ? "AUX" : "");
    line("D000-DFFF", sw.lcram ? lc : "ROM", sw.lcram ? CARD : ROM, sw.lcwrite ? lc : "LOCKED", sw.lcwrite ? CARD : 0,
         "The language card: which $D000 bank, and whether it is read and written");
    line("E000-FFFF", sw.lcram ? lcHigh : "ROM", sw.lcram ? CARD : ROM, sw.lcwrite ? lcHigh : "LOCKED",
         sw.lcwrite ? CARD : 0, nullptr);

    // The switches themselves.
    ImGui::Dummy(ImVec2(0, 4));
    struct Flag {
      const char *name;
      bool on;
      bool needsAux;
    };
    const Flag flags[] = {{"80STORE", sw.store80, true}, {"RAMRD", sw.ramrd, true},   {"RAMWRT", sw.ramwrt, true},
                          {"ALTZP", sw.altzp, true},     {"INTCXROM", sw.intcxrom, true}, {"SLOTC3ROM", sw.slotc3rom, true},
                          {"TEXT", sw.text, false},      {"MIXED", sw.mixed, false},  {"PAGE2", sw.page2, false},
                          {"HIRES", sw.hires, false},    {"80COL", sw.col80, true},   {"ALTCHAR", sw.altCharSet, true}};
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float x = start.x, y = start.y;
    const float ph = pillHeight(0.72f);
    for (const Flag &f : flags) {
      if (f.needsAux && !aux) continue;
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.72f);
      const float pw = ImGui::CalcTextSize(f.name).x + 12;
      ImGui::PopFont();
      if (x + pw > start.x + w) {
        x = start.x;
        y += ph + 4;
      }
      x += pill(draw, ImVec2(x, y), f.name, f.on, p.green, 0.72f) + 4;
    }
    ImGui::Dummy(ImVec2(w, y - start.y + ph));
  });
}

void MemoryViewer::drawPlaces(float width) {
  cardAround("PLACES", width, [&](float w) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const Palette p = palette();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float x = start.x, y = start.y;
    const float ph = pillHeight(0.8f);
    auto places = wide_ ? std::vector<Place>(std::begin(PLACES_IIGS), std::end(PLACES_IIGS))
                        : std::vector<Place>(std::begin(PLACES_8BIT), std::end(PLACES_8BIT));
    for (const Place &place : places) {
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
      const float pw = ImGui::CalcTextSize(place.label).x + 12;
      ImGui::PopFont();
      if (x + pw > start.x + w) {
        x = start.x;
        y += ph + 5;
      }
      ImGui::SetCursorScreenPos(ImVec2(x, y));
      ImGui::PushID(place.label);
      const bool clicked = ImGui::InvisibleButton("##place", ImVec2(pw, ph));
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      // A tinted chip in the accent, the way a macOS token reads as
      // something to press; filled solid under the pointer.
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
      const ImVec2 end(x + pw, y + ph);
      draw->AddRectFilled(ImVec2(x, y), end, hov ? accent() : accent(ui::isDark() ? 0.18f : 0.11f), ph * 0.5f);
      draw->AddText(ImVec2(x + 6, y + 2), hov ? ui::textOn(accent()) : ui::accentText(), place.label);
      ImGui::PopFont();
      if (hov) ImGui::SetTooltip("$%s", formatAddress(place.address).c_str());
      if (clicked) {
        goTo(place.address);
        follow_ = Follow::Off;
      }
      x += pw + 5;
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, y + ph + 8));

    // The user's own.
    for (size_t i = 0; i < bookmarks_.size(); i++) {
      const uint32_t b = bookmarks_[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::PushFont(ui::monoFont(), 0.0f);
      const auto sym = debugger_.symbols().lookup(b);
      const std::string label = "$" + formatAddress(b) + (sym ? "  " + sym->name : "");
      ImGui::PushStyleColor(ImGuiCol_Text, p.blue);
      const bool go = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(w - 24, 0));
      ImGui::PopStyleColor();
      ImGui::PopFont();
      ImGui::SameLine(w - ImGui::GetTextLineHeight());
      if (removeButton("##remove")) {
        bookmarks_.erase(bookmarks_.begin() + static_cast<long>(i));
        ImGui::MarkIniSettingsDirty();
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
      if (go) {
        goTo(b);
        follow_ = Follow::Off;
      }
    }
    const bool marked = std::find(bookmarks_.begin(), bookmarks_.end(), caret_) != bookmarks_.end();
    ImGui::BeginDisabled(marked);
    if (ui::Button("Bookmark the Caret")) {
      bookmarks_.insert(std::lower_bound(bookmarks_.begin(), bookmarks_.end(), caret_), caret_);
      ImGui::MarkIniSettingsDirty();
    }
    ImGui::EndDisabled();
  });
}

void MemoryViewer::drawContextMenu() {
  if (!ImGui::BeginPopup("##memmenu")) return;
  const host::MemorySpace &sp = *space();
  const bool selection = hasSelection();
  const uint32_t lo = selection ? selectionLow() : menuAddress_;
  const uint32_t hi = selection ? selectionHigh() : menuAddress_;
  ImGui::TextDisabled("%s", selection ? ("$" + formatAddress(lo) + "-$" + formatAddress(hi)).c_str()
                                      : ("$" + formatAddress(menuAddress_)).c_str());
  ImGui::Separator();
  if (ImGui::MenuItem("Copy")) {
    if (!selection) anchor_ = caret_ = menuAddress_;
    copySelection(0);
  }
  if (ImGui::BeginMenu("Copy As")) {
    static const char *const FORMATS[] = {"Hex Bytes", "Merlin HEX", "DFB Directives", "C Array", "Text"};
    for (int i = 0; i < 5; i++) {
      if (ImGui::MenuItem(FORMATS[i])) copySelection(i);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Copy Address")) {
    ImGui::SetClipboardText(("$" + formatAddress(menuAddress_)).c_str());
  }
  const char *clip = ImGui::GetClipboardText();
  if (ImGui::MenuItem("Paste Hex", nullptr, false, sp.writable && clip && *clip)) {
    setCaret(lo, false);
    pasteHex(clip);
  }
  if (ImGui::MenuItem("Fill\xE2\x80\xA6", nullptr, false, sp.writable)) {
    if (!selection) anchor_ = caret_ = menuAddress_;
    openFill_ = true;
  }
  ImGui::Separator();
  const std::vector<uint8_t> word = read(menuAddress_, 3);
  const uint32_t pointer = (wide_ ? (sp.base & 0xFF0000) : 0) | static_cast<uint32_t>(word[0] | (word[1] << 8));
  if (ImGui::MenuItem(("Go to Pointer $" + formatAddress(pointer)).c_str())) goTo(pointer);
  if (wide_) {
    const uint32_t lng = static_cast<uint32_t>(word[0] | (word[1] << 8) | (word[2] << 16));
    if (ImGui::MenuItem(("Go to Long Pointer $" + formatAddress(lng)).c_str())) goTo(lng);
  }
  if (sp.processor) {
    if (ImGui::MenuItem("Disassemble Here")) {
      debugger_.showInListing(menuAddress_);
      if (showDebugger) showDebugger();
    }
    ImGui::Separator();
    auto addBreak = [&](Breakpoint::Kind kind) {
      const bool added = debugger_.addBreakpoint(kind, lo, hi);
      message_ = added ? "Breakpoint added" : "There is already one there";
      messageAt_ = ImGui::GetTime();
    };
    if (ImGui::MenuItem("Break on Read")) addBreak(Breakpoint::Kind::Read);
    if (ImGui::MenuItem("Break on Write")) addBreak(Breakpoint::Kind::Write);
    if (ImGui::MenuItem("Break on Access")) addBreak(Breakpoint::Kind::ReadWrite);
    if (ImGui::MenuItem("Break on Execute", nullptr, false, !selection)) addBreak(Breakpoint::Kind::Exec);
  }
  ImGui::Separator();
  const auto marked = std::find(bookmarks_.begin(), bookmarks_.end(), menuAddress_);
  if (ImGui::MenuItem(marked == bookmarks_.end() ? "Add Bookmark" : "Remove Bookmark")) {
    if (marked == bookmarks_.end()) {
      bookmarks_.insert(std::lower_bound(bookmarks_.begin(), bookmarks_.end(), menuAddress_), menuAddress_);
    } else {
      bookmarks_.erase(marked);
    }
    ImGui::MarkIniSettingsDirty();
  }
  if (ImGui::MenuItem("Save to File\xE2\x80\xA6")) saveSelection();
  if (ImGui::MenuItem("Load File Here\xE2\x80\xA6", nullptr, false, sp.writable)) {
    setCaret(menuAddress_, false);
    loadFile();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Undo", nullptr, false, !undo_.empty())) undo();
  if (ImGui::MenuItem("Redo", nullptr, false, !redo_.empty())) redo();
  ImGui::EndPopup();
}

void MemoryViewer::drawFillPopup() {
  if (openFill_) {
    ImGui::OpenPopup("##fill");
    openFill_ = false;
  }
  if (!ImGui::BeginPopup("##fill")) return;
  const uint32_t lo = selectionLow(), hi = selectionHigh();
  ImGui::Text("Fill $%s-$%s", formatAddress(lo).c_str(), formatAddress(hi).c_str());
  ImGui::TextDisabled("A byte, or a pattern to repeat: 00, A0, AA 55");
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(220);
  const bool enter = ImGui::InputText("##fillvalue", fillText_, sizeof fillText_, ImGuiInputTextFlags_EnterReturnsTrue);
  const std::vector<int> pattern = parseHex(fillText_, false);
  ImGui::BeginDisabled(pattern.empty());
  const bool ok = ui::Button("Fill", ImVec2(90, 0), ui::ButtonKind::Primary) || (enter && !pattern.empty());
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  if (ok) {
    std::vector<uint8_t> bytes(hi - lo + 1);
    for (size_t i = 0; i < bytes.size(); i++) bytes[i] = static_cast<uint8_t>(pattern[i % pattern.size()]);
    if (write(lo, bytes)) {
      message_ = "Filled " + std::to_string(bytes.size()) + (bytes.size() == 1 ? " byte" : " bytes");
      messageAt_ = ImGui::GetTime();
    }
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void MemoryViewer::drawLoadPopup() {
  if (openLoad_) {
    ImGui::OpenPopup("##load");
    openLoad_ = false;
  }
  if (!ImGui::BeginPopup("##load")) return;
  ImGui::Text("Load %s", loadName_.c_str());
  ImGui::TextDisabled("%zu bytes, into %s", loadBytes_.size(), space()->name.c_str());
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(220);
  const bool enter = ImGui::InputTextWithHint("##loadat", "Address", loadAt_, sizeof loadAt_, ImGuiInputTextFlags_EnterReturnsTrue);
  auto at = debugger_.symbols().resolve(loadAt_, addressMask());
  ImGui::BeginDisabled(!at);
  const bool ok = ui::Button("Load", ImVec2(90, 0), ui::ButtonKind::Primary) || (enter && at);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    loadBytes_.clear();
    ImGui::CloseCurrentPopup();
  }
  if (ok && at) {
    uint32_t address = *at;
    if (wide_ && address <= 0xFFFF && !std::strchr(loadAt_, '/')) address |= space()->base & 0xFF0000;
    if (!inSpace(address)) goTo(address);
    if (inSpace(address)) {
      std::vector<uint8_t> bytes = loadBytes_;
      const uint32_t room = spaceEnd() - address;
      if (bytes.size() > room) bytes.resize(room);
      if (write(address, bytes)) {
        goTo(address);
        anchor_ = address;
        caret_ = address + static_cast<uint32_t>(bytes.size()) - 1;
        message_ = "Loaded " + std::to_string(bytes.size()) + " bytes at $" + formatAddress(address);
        messageAt_ = ImGui::GetTime();
      }
    }
    loadBytes_.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// The bitmap and screen views
// ---------------------------------------------------------------------------

namespace {

constexpr const char *BITMAP_FORMATS[] = {"Hi-res bytes", "1 bpp", "2 bpp", "4 bpp", "8 bpp"};
constexpr const char *BITMAP_FORMAT_TIPS[] = {
    "Seven pixels a byte, bit 0 on the left, as hi-res draws them; bit 7 shifts the colour",
    "Eight pixels a byte, bit 7 on the left",
    "Four pixels a byte, two bits each, high bits first",
    "Two pixels a byte, a nibble each, high nibble first, in the lo-res colours",
    "A pixel a byte, its value as its colour",
};
constexpr const char *PAGE_NAMES[] = {"Text, 40 columns", "Text, 80 columns", "Lo-res", "Double lo-res",
                                      "Hi-res",           "Double hi-res",    "Super Hi-Res"};

// An Apple II colour from the palette the core's decoders share, as RGB.
void idealColour(int index, uint8_t *out) {
  const uint32_t c = ntsc::idealPalette()[static_cast<size_t>(index & 15)];
  out[0] = static_cast<uint8_t>(c >> 16);
  out[1] = static_cast<uint8_t>(c >> 8);
  out[2] = static_cast<uint8_t>(c);
}

} // namespace

int MemoryViewer::bitmapPixelsPerByte() const {
  switch (bitmapFormat_) {
  case BitmapFormat::AppleHiRes: return 7;
  case BitmapFormat::OneBit: return 8;
  case BitmapFormat::TwoBit: return 4;
  case BitmapFormat::FourBit: return 2;
  case BitmapFormat::EightBit: return 1;
  }
  return 8;
}

void MemoryViewer::showTexture(ImTextureID &texture, int &width, int &height, const std::vector<uint8_t> &rgba,
                               int w, int h) {
  if (!platform_.makeTexture || w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return;
  if (platform_.releaseTexture && texture != ImTextureID_Invalid) platform_.releaseTexture(texture);
  texture = platform_.makeTexture(rgba.data(), w, h);
  width = w;
  height = h;
}

// The bytes as pixels, in the chosen format and layout. A byte the view has
// not read yet is drawn dark rather than as zero.
void MemoryViewer::paintBitmap() {
  const int ppb = bitmapPixelsPerByte();
  const int w = bitmapWidth_ * ppb;
  const int h = bitmapTiles_ ? ((bitmapRows_ + 7) / 8) * 8 : bitmapRows_;
  if (w <= 0 || h <= 0) return;
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
  const bool dark = ui::isDark();
  const uint8_t missing[3] = {static_cast<uint8_t>(dark ? 30 : 210), static_cast<uint8_t>(dark ? 30 : 210),
                              static_cast<uint8_t>(dark ? 34 : 214)};
  auto byteAtPixel = [&](int x, int y, int &pixel) -> std::optional<uint8_t> {
    const int column = x / ppb;
    pixel = x % ppb;
    size_t index;
    if (bitmapTiles_) index = (static_cast<size_t>(y / 8) * bitmapWidth_ + column) * 8 + static_cast<size_t>(y % 8);
    else index = static_cast<size_t>(y) * bitmapWidth_ + column;
    if (index >= bitmapBytes_.size()) return std::nullopt;
    return bitmapBytes_[index];
  };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t *px = &rgba[(static_cast<size_t>(y) * w + x) * 4];
      px[3] = 255;
      int pixel = 0;
      const std::optional<uint8_t> byte = byteAtPixel(x, y, pixel);
      if (!byte) {
        std::memcpy(px, missing, 3);
        continue;
      }
      const uint8_t b = *byte;
      switch (bitmapFormat_) {
      case BitmapFormat::AppleHiRes: {
        const bool on = (b >> pixel) & 1;
        if (!on) break;
        if (!bitmapColour_) {
          px[0] = px[1] = px[2] = 255;
          break;
        }
        // The same rule the Solid decoder applies: a lit pixel beside a lit
        // pixel is white, one on its own takes its column's colour, violet or
        // green, or blue or orange when its byte's high bit is set.
        int left = 0, right = 0;
        const std::optional<uint8_t> lb = x > 0 ? byteAtPixel(x - 1, y, left) : std::nullopt;
        const std::optional<uint8_t> rb = x + 1 < w ? byteAtPixel(x + 1, y, right) : std::nullopt;
        const bool neighbour = (lb && ((*lb >> left) & 1)) || (rb && ((*rb >> right) & 1));
        if (neighbour) {
          px[0] = px[1] = px[2] = 255;
          break;
        }
        const bool odd = x & 1;
        idealColour((b & 0x80) ? (odd ? 9 : 6) : (odd ? 12 : 3), px);
        break;
      }
      case BitmapFormat::OneBit:
        if ((b >> (7 - pixel)) & 1) px[0] = px[1] = px[2] = 255;
        break;
      case BitmapFormat::TwoBit: {
        const int v = (b >> (6 - pixel * 2)) & 3;
        if (bitmapColour_) {
          static const int TWO[4] = {0, 12, 3, 15};
          idealColour(TWO[v], px);
        } else {
          px[0] = px[1] = px[2] = static_cast<uint8_t>(v * 85);
        }
        break;
      }
      case BitmapFormat::FourBit: {
        const int v = pixel == 0 ? b >> 4 : b & 0x0F;
        if (bitmapColour_) idealColour(v, px);
        else px[0] = px[1] = px[2] = static_cast<uint8_t>(v * 17);
        break;
      }
      case BitmapFormat::EightBit:
        if (bitmapColour_) valueColour(b, true, px);
        else px[0] = px[1] = px[2] = b;
        break;
      }
    }
  }
  showTexture(bitmapTexture_, bitmapTextureWidth_, bitmapTextureHeight_, rgba, w, h);
}

void MemoryViewer::drawViewOptions() {
  ImGui::PushID("viewoptions");
  constexpr float GROUP_GAP = 14.0f;
  constexpr float ITEM_GAP = 6.0f;
  if (view_ == View::Bitmap) {
    if (ui::BeginPopUpButton("##format", BITMAP_FORMATS[static_cast<int>(bitmapFormat_)], 140.0f)) {
      for (int i = 0; i < 5; i++) {
        if (ImGui::Selectable(BITMAP_FORMATS[i], static_cast<int>(bitmapFormat_) == i)) {
          bitmapFormat_ = static_cast<BitmapFormat>(i);
          ImGui::MarkIniSettingsDirty();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", BITMAP_FORMAT_TIPS[i]);
      }
      ui::EndPopUpButton();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", BITMAP_FORMAT_TIPS[static_cast<int>(bitmapFormat_)]);

    // The width, in bytes, stepped or typed.
    ImGui::SameLine(0, GROUP_GAP);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Width");
    ImGui::SameLine(0, ITEM_GAP);
    if (iconButton("##narrower", Icon::Back, "Narrower", bitmapWidth_ > 1)) bitmapWidth_--, ImGui::MarkIniSettingsDirty();
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(44);
    if (ImGui::InputInt("##width", &bitmapWidth_, 0, 0)) {
      bitmapWidth_ = std::clamp(bitmapWidth_, 1, 80);
      ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bytes to a row (tiles to a row, laid out as tiles): 40 is a hi-res line");
    ImGui::SameLine(0, 0);
    if (iconButton("##wider", Icon::Forward, "Wider", bitmapWidth_ < 80)) bitmapWidth_++, ImGui::MarkIniSettingsDirty();

    ImGui::SameLine(0, GROUP_GAP);
    int layout = bitmapTiles_ ? 1 : 0;
    if (ui::SegmentedControl("##layout", &layout, {"Rows", "Tiles"}, 110)) {
      bitmapTiles_ = layout == 1;
      ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Rows: each row the next run of bytes\nTiles: eight bytes down a column, as a font's glyphs");
    }
    ImGui::SameLine(0, GROUP_GAP);
    static const int ZOOMS[] = {1, 2, 3, 4, 6, 8};
    int zoom = 0;
    for (int i = 0; i < 6; i++) {
      if (ZOOMS[i] == bitmapZoom_) zoom = i;
    }
    if (ui::SegmentedControl("##zoom", &zoom, {"1x", "2x", "3x", "4x", "6x", "8x"}, 220)) {
      bitmapZoom_ = ZOOMS[zoom];
      ImGui::MarkIniSettingsDirty();
    }
    if (bitmapFormat_ != BitmapFormat::OneBit) {
      ImGui::SameLine(0, GROUP_GAP);
      if (ui::Switch("Colour", &bitmapColour_)) ImGui::MarkIniSettingsDirty();
    }
  } else {
    // Only the pages this machine has.
    if (ui::BeginPopUpButton("##page", PAGE_NAMES[screenPage_], 170.0f)) {
      for (int i = 0; i < 7; i++) {
        if (!(availablePages_ & (1u << i))) continue;
        if (ImGui::Selectable(PAGE_NAMES[i], screenPage_ == i)) {
          screenPage_ = i;
          screenTakenAt_ = -10.0;
          ImGui::MarkIniSettingsDirty();
        }
      }
      ui::EndPopUpButton();
    }
    if (screenPage_ != static_cast<int>(host::MachineHost::DisplayPage::SuperHiRes)) {
      ImGui::SameLine(0, ITEM_GAP);
      int page = screenPage2_ ? 1 : 0;
      if (ui::SegmentedControl("##page2", &page, {"Page 1", "Page 2"}, 150)) {
        screenPage2_ = page == 1;
        screenTakenAt_ = -10.0;
        ImGui::MarkIniSettingsDirty();
      }
    }
    ImGui::SameLine(0, GROUP_GAP);
    if (ui::SegmentedControl("##decode", &screenColours_, {"Solid", "Exact", "Mono"}, 190)) {
      screenTakenAt_ = -10.0;
      ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Decoded as the Display Settings' Solid Colour, Pixel Exact and Monochrome are");
    }
    ImGui::SameLine(0, GROUP_GAP);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Click a byte to inspect it, double-click to see it in hex");
  }
  ImGui::PopID();
}

void MemoryViewer::drawBitmap(ImVec2 size) {
  ImGui::BeginChild("##bitmap", size, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 end(origin.x + avail.x, origin.y + avail.y);
  draw->AddRectFilled(origin, end, well(), 8.0f);
  draw->AddRect(origin, end, ImGui::GetColorU32(ImGuiCol_Border), 8.0f);
  const host::MemorySpace &sp = *space();
  const float mapX = end.x - MAP_WIDTH - 10;
  const ImVec2 c0(origin.x + 10, origin.y + 10), c1(mapX - 10, end.y - 10);
  const float zoom = static_cast<float>(bitmapZoom_);
  const int ppb = bitmapPixelsPerByte();
  bitmapRows_ = std::max(8, static_cast<int>((c1.y - c0.y) / zoom));
  if (!inSpace(bitmapTop_)) bitmapTop_ = inSpace(caret_) ? caret_ : sp.base;
  const uint32_t rowBytes = static_cast<uint32_t>(bitmapWidth_) * (bitmapTiles_ ? 8 : 1);

  // The pixels, and a hairline grid between bytes once they are big enough
  // to tell apart.
  draw->PushClipRect(c0, c1, true);
  if (bitmapTexture_ != ImTextureID_Invalid) {
    const ImVec2 b(c0.x + bitmapTextureWidth_ * zoom, c0.y + bitmapTextureHeight_ * zoom);
    const ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
    if (pio.DrawCallback_SetSamplerNearest) draw->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
    draw->AddImage(ImTextureRef(bitmapTexture_), c0, b);
    if (pio.DrawCallback_SetSamplerLinear) draw->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
    if (zoom >= 4) {
      const ImU32 grid = text(0.10f);
      for (int c = 1; c < bitmapWidth_; c++) {
        const float x = c0.x + c * ppb * zoom;
        draw->AddLine(ImVec2(x, c0.y), ImVec2(x, b.y), grid);
      }
      if (bitmapTiles_) {
        for (int r = 8; r < bitmapTextureHeight_; r += 8) {
          const float y = c0.y + r * zoom;
          draw->AddLine(ImVec2(c0.x, y), ImVec2(b.x, y), grid);
        }
      }
    }
  }

  // Which byte a pixel is, and where that byte's cell is drawn.
  auto addressAt = [&](int x, int y) -> std::optional<uint32_t> {
    if (x < 0 || y < 0 || x >= bitmapWidth_ * ppb) return std::nullopt;
    const uint32_t column = static_cast<uint32_t>(x / ppb);
    uint32_t index;
    if (bitmapTiles_) index = (static_cast<uint32_t>(y / 8) * bitmapWidth_ + column) * 8 + static_cast<uint32_t>(y % 8);
    else index = static_cast<uint32_t>(y) * bitmapWidth_ + column;
    const uint32_t address = bitmapTop_ + index;
    if (index >= bitmapBytes_.size() || !inSpace(address)) return std::nullopt;
    return address;
  };
  auto cellOf = [&](uint32_t address, ImVec2 &a, ImVec2 &b) {
    if (address < bitmapTop_ || address >= bitmapTop_ + bitmapBytes_.size()) return false;
    const uint32_t index = address - bitmapTop_;
    int cx, cy;
    if (bitmapTiles_) {
      const uint32_t tile = index / 8;
      cx = static_cast<int>(tile % bitmapWidth_) * ppb;
      cy = static_cast<int>(tile / bitmapWidth_) * 8 + static_cast<int>(index % 8);
    } else {
      cx = static_cast<int>(index % bitmapWidth_) * ppb;
      cy = static_cast<int>(index / bitmapWidth_);
    }
    a = ImVec2(c0.x + cx * zoom, c0.y + cy * zoom);
    b = ImVec2(a.x + ppb * zoom, a.y + zoom);
    return true;
  };
  ImVec2 a, b;
  if (cellOf(caret_, a, b)) draw->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), accent(), 2.0f, 0, 2.0f);
  draw->PopClipRect();

  // Scrolling by rows, and the pointer.
  ImGui::SetCursorScreenPos(c0);
  ImGui::InvisibleButton("##pixels", ImVec2(std::max(1.0f, c1.x - c0.x), std::max(1.0f, c1.y - c0.y)));
  const ImGuiIO &io = ImGui::GetIO();
  if (ImGui::IsWindowHovered() && io.MouseWheel != 0 && !draggingMap_) {
    const int64_t step = static_cast<int64_t>(rowBytes) * (bitmapTiles_ ? 1 : 4) * static_cast<int64_t>(-io.MouseWheel);
    const int64_t last = static_cast<int64_t>(sp.base + sp.size) - static_cast<int64_t>(rowBytes);
    bitmapTop_ = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(bitmapTop_) + step, sp.base,
                                                           std::max<int64_t>(sp.base, last)));
  }
  hovered_.reset();
  if (ImGui::IsItemHovered()) {
    const int x = static_cast<int>((io.MousePos.x - c0.x) / zoom);
    const int y = static_cast<int>((io.MousePos.y - c0.y) / zoom);
    if (const std::optional<uint32_t> at = addressAt(x, y)) {
      hovered_ = at;
      if (cellOf(*at, a, b)) draw->AddRect(a, b, text(0.6f), 1.0f);
      const std::optional<uint8_t> v = byteAt(*at);
      const uint8_t value = v ? *v : bitmapBytes_[*at - bitmapTop_];
      ImGui::SetTooltip("$%s  $%02X\nPixel %d, %d", formatAddress(*at).c_str(), value, x, y);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) setCaret(*at, io.KeyShift);
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        view_ = View::Hex;
        goTo(*at);
      }
    }
  }

  drawMap(ImVec2(mapX, origin.y + 6), ImVec2(MAP_WIDTH, avail.y - 12));
  ImGui::EndChild();
}

// Which byte a pixel of a display page came from: the page's own address
// arithmetic, the interleaved rows of text and hi-res included.
std::optional<MemoryViewer::ScreenCell> MemoryViewer::screenCellAt(int x, int y) const {
  using Page = host::MachineHost::DisplayPage;
  const Page page = static_cast<Page>(screenPage_);
  if (x < 0 || y < 0 || x >= screenWidth_ || y >= screenHeight_) return std::nullopt;
  auto textAddress = [](int row, int column) {
    return static_cast<uint32_t>(((row & 7) << 7) + (row >> 3) * 40 + column);
  };
  auto hiResAddress = [](int line, int column) {
    return static_cast<uint32_t>(((line & 7) << 10) + (((line >> 3) & 7) << 7) + (line >> 6) * 40 + column);
  };
  ScreenCell cell;
  switch (page) {
  case Page::Text40:
  case Page::LoRes: {
    const int row = y / 8, column = x / 14;
    cell.address = (screenPage2_ ? 0x0800u : 0x0400u) + textAddress(row, column);
    cell.x0 = column * 14.0f, cell.x1 = cell.x0 + 14, cell.y0 = row * 8.0f, cell.y1 = cell.y0 + 8;
    break;
  }
  case Page::Text80:
  case Page::DoubleLoRes: {
    const int row = y / 8, column = x / 7;
    cell.address = (screenPage2_ ? 0x0800u : 0x0400u) + textAddress(row, column / 2);
    cell.aux = (column & 1) == 0;
    cell.x0 = column * 7.0f, cell.x1 = cell.x0 + 7, cell.y0 = row * 8.0f, cell.y1 = cell.y0 + 8;
    break;
  }
  case Page::HiRes: {
    const int column = x / 14;
    cell.address = (screenPage2_ ? 0x4000u : 0x2000u) + hiResAddress(y, column);
    cell.x0 = column * 14.0f, cell.x1 = cell.x0 + 14, cell.y0 = static_cast<float>(y), cell.y1 = cell.y0 + 1;
    break;
  }
  case Page::DoubleHiRes: {
    const int column = x / 7;
    cell.address = (screenPage2_ ? 0x4000u : 0x2000u) + hiResAddress(y, column / 2);
    cell.aux = (column & 1) == 0;
    cell.x0 = column * 7.0f, cell.x1 = cell.x0 + 7, cell.y0 = static_cast<float>(y), cell.y1 = cell.y0 + 1;
    break;
  }
  case Page::SuperHiRes: {
    // 160 bytes a line, four of the 640 pixels a byte either way: two
    // pixels of 320 mode or four of 640.
    const int column = x / 4;
    cell.address = 0x2000u + static_cast<uint32_t>(y) * 160 + static_cast<uint32_t>(column);
    cell.aux = true;
    cell.x0 = column * 4.0f, cell.x1 = cell.x0 + 4, cell.y0 = static_cast<float>(y), cell.y1 = cell.y0 + 1;
    break;
  }
  }
  return cell;
}

// To a byte of a page in the bank it is really in: the Mega II's $E0 or $E1
// on a IIgs, main or auxiliary RAM itself on the others.
void MemoryViewer::goToScreenCell(const ScreenCell &cell) {
  if (wide_) {
    goTo((cell.aux ? 0xE10000u : 0xE00000u) | cell.address);
    return;
  }
  const host::MemorySpace::Kind kind = cell.aux ? host::MemorySpace::Kind::AuxRAM : host::MemorySpace::Kind::MainRAM;
  for (size_t i = 0; i < spaces_.size(); i++) {
    if (spaces_[i].kind == kind) {
      selectSpace(i);
      break;
    }
  }
  goTo(cell.address);
}

void MemoryViewer::drawScreenView(ImVec2 size) {
  ImGui::BeginChild("##screen", size, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 end(origin.x + avail.x, origin.y + avail.y);
  draw->AddRectFilled(origin, end, well(), 8.0f);
  draw->AddRect(origin, end, ImGui::GetColorU32(ImGuiCol_Border), 8.0f);
  if (screenTexture_ == ImTextureID_Invalid || screenWidth_ <= 0) {
    ImGui::EndChild();
    return;
  }

  // The page at a monitor's 4:3, as large as fits.
  const float roomW = avail.x - 20, roomH = avail.y - 20;
  float w = roomW, h = roomW * 0.75f;
  if (h > roomH) h = roomH, w = roomH / 0.75f;
  const ImVec2 p0(std::floor(origin.x + (avail.x - w) * 0.5f), std::floor(origin.y + (avail.y - h) * 0.5f));
  const ImVec2 p1(p0.x + w, p0.y + h);
  const ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
  if (pio.DrawCallback_SetSamplerNearest) draw->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
  draw->AddImage(ImTextureRef(screenTexture_), p0, p1);
  if (pio.DrawCallback_SetSamplerLinear) draw->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
  const float sx = w / screenTextureWidth_, sy = h / screenTextureHeight_;

  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##page", ImVec2(std::max(1.0f, w), std::max(1.0f, h)));
  hovered_.reset();
  if (ImGui::IsItemHovered()) {
    const ImGuiIO &io = ImGui::GetIO();
    const int x = static_cast<int>((io.MousePos.x - p0.x) / sx);
    const int y = static_cast<int>((io.MousePos.y - p0.y) / sy);
    if (const std::optional<ScreenCell> cell = screenCellAt(x, y)) {
      draw->AddRect(ImVec2(p0.x + cell->x0 * sx, p0.y + cell->y0 * sy),
                    ImVec2(p0.x + cell->x1 * sx, p0.y + cell->y1 * sy), accent(), 1.0f, 0, 1.5f);
      const char *bank = wide_ ? (cell->aux ? "$E1" : "$E0") : (cell->aux ? "auxiliary" : "main");
      ImGui::SetTooltip("$%04X, %s\nLine %d", cell->address, bank, y);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) goToScreenCell(*cell);
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) view_ = View::Hex;
    }
  }
  ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void MemoryViewer::writeSettings(std::string &out) const {
  char line[160];
  std::snprintf(line, sizeof line, "Columns=%d\nApple=%d\nActivity=%d\nFollow=%d\nCaret=%06X\n", columns_,
                textApple_ ? 1 : 0, activityOn_ ? 1 : 0, static_cast<int>(follow_), caret_);
  out += line;
  if (const host::MemorySpace *sp = space()) out += "Space=" + sp->name + "\n";
  std::snprintf(line, sizeof line, "View=%d\nBitmapFormat=%d\nBitmapWidth=%d\nBitmapTiles=%d\nBitmapColour=%d\n"
                "BitmapZoom=%d\nScreenPage=%d\nScreenPage2=%d\nScreenColours=%d\n",
                static_cast<int>(view_), static_cast<int>(bitmapFormat_), bitmapWidth_, bitmapTiles_ ? 1 : 0,
                bitmapColour_ ? 1 : 0, bitmapZoom_, screenPage_, screenPage2_ ? 1 : 0, screenColours_);
  out += line;
  if (followText_[0]) out += std::string("FollowExpression=") + followText_ + "\n";
  for (uint32_t b : bookmarks_) {
    std::snprintf(line, sizeof line, "Bookmark=%06X\n", b);
    out += line;
  }
}

void MemoryViewer::readSetting(const char *line) {
  int value = 0;
  unsigned address = 0;
  if (std::sscanf(line, "Columns=%d", &value) == 1) columns_ = value == 8 || value == 32 ? value : 16;
  else if (std::sscanf(line, "Apple=%d", &value) == 1) textApple_ = value;
  else if (std::sscanf(line, "Activity=%d", &value) == 1) activityOn_ = value;
  else if (std::sscanf(line, "View=%d", &value) == 1) view_ = static_cast<View>(std::clamp(value, 0, 2));
  else if (std::sscanf(line, "BitmapFormat=%d", &value) == 1) bitmapFormat_ = static_cast<BitmapFormat>(std::clamp(value, 0, 4));
  else if (std::sscanf(line, "BitmapWidth=%d", &value) == 1) bitmapWidth_ = std::clamp(value, 1, 80);
  else if (std::sscanf(line, "BitmapTiles=%d", &value) == 1) bitmapTiles_ = value;
  else if (std::sscanf(line, "BitmapColour=%d", &value) == 1) bitmapColour_ = value;
  else if (std::sscanf(line, "BitmapZoom=%d", &value) == 1) bitmapZoom_ = std::clamp(value, 1, 8);
  else if (std::sscanf(line, "ScreenPage=%d", &value) == 1) screenPage_ = std::clamp(value, 0, 6);
  else if (std::sscanf(line, "ScreenPage2=%d", &value) == 1) screenPage2_ = value;
  else if (std::sscanf(line, "ScreenColours=%d", &value) == 1) screenColours_ = std::clamp(value, 0, 2);
  else if (std::sscanf(line, "Follow=%d", &value) == 1) follow_ = static_cast<Follow>(std::clamp(value, 0, 3));
  else if (std::sscanf(line, "Caret=%x", &address) == 1) wantedTop_ = address & 0xFFFFFF;
  else if (!std::strncmp(line, "Space=", 6)) wantedSpace_ = line + 6;
  else if (!std::strncmp(line, "FollowExpression=", 17)) std::snprintf(followText_, sizeof followText_, "%s", line + 17);
  else if (std::sscanf(line, "Bookmark=%x", &address) == 1) {
    const uint32_t b = address & 0xFFFFFF;
    if (std::find(bookmarks_.begin(), bookmarks_.end(), b) == bookmarks_.end()) {
      bookmarks_.insert(std::lower_bound(bookmarks_.begin(), bookmarks_.end(), b), b);
    }
  }
}

} // namespace a2e::native
