/*
 * profiler_window.cpp - Where a program spends its time, drawn
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "debugger/profiler_window.hpp"

#include "app/emulation.hpp"
#include "debugger/cpu_debugger.hpp"
#include "ui/ui_controls.hpp"
#include "ui/ui_theme.hpp"

#include "imgui.h"
#include "imgui_internal.h" // RenderTextEllipsis

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace a2e::native {

namespace {

constexpr const char *TITLE = "Profiler";
// How often a recording is read while it runs, in seconds. The tree is
// copied whole, so this is the window's whole cost to the emulation thread.
constexpr double REFRESH_SECONDS = 0.25;
constexpr float DETAIL_WIDTH = 340;
constexpr float TIMELINE_HEIGHT = 92;
constexpr size_t HOT_LINES = 400;
constexpr int CODE_LINES = 160;

ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }

ImU32 alpha(ImU32 colour, float a) {
  ImVec4 v = ImGui::ColorConvertU32ToFloat4(colour);
  v.w *= a;
  return ImGui::ColorConvertFloat4ToU32(v);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
  const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
  const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

std::string percent(double share) {
  char text[16];
  if (share <= 0) return "0%";
  if (share < 0.001) return "<0.1%";
  std::snprintf(text, sizeof text, "%.1f%%", share * 100.0);
  return text;
}

// A count as a person reads one: 999, 12.3K, 4.56M.
std::string count(double n) {
  char text[24];
  if (n < 1000) std::snprintf(text, sizeof text, "%.0f", n);
  else if (n < 1e6) std::snprintf(text, sizeof text, "%.1fK", n / 1e3);
  else if (n < 1e9) std::snprintf(text, sizeof text, "%.2fM", n / 1e6);
  else std::snprintf(text, sizeof text, "%.2fG", n / 1e9);
  return text;
}

// A section's heading, as the other debug windows set theirs: body size,
// the accent colour, a rule under it.
void heading(const char *label) {
  ImGui::Dummy(ImVec2(0, 4));
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::accentText()), "%s", label);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y - 1),
                                      ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y - 1),
                                      ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
  ImGui::Dummy(ImVec2(0, 3));
}

// A share as a bar with the figure beside it, filling the cell it is in.
void shareBar(double share, ImU32 colour) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const float line = ImGui::GetTextLineHeight();
  ImGui::PushFont(ui::monoFont(), 0.0f);
  const std::string text = percent(share);
  const float textWidth = ImGui::CalcTextSize("100.0%").x;
  const float barWidth = std::max(8.0f, width - textWidth - 8);
  const float mid = at.y + line * 0.5f;
  const ImVec2 a(at.x, mid - 3), b(at.x + barWidth, mid + 3);
  draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_Text, 0.07f), 3.0f);
  const float fill = static_cast<float>(std::clamp(share, 0.0, 1.0)) * barWidth;
  if (fill > 0.5f) draw->AddRectFilled(a, ImVec2(a.x + std::max(fill, 3.0f), b.y), colour, 3.0f);
  const float w = ImGui::CalcTextSize(text.c_str()).x;
  draw->AddText(ImVec2(at.x + width - w, at.y), share > 0 ? ImGui::GetColorU32(ImGuiCol_Text) : secondary(),
                text.c_str());
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(width, line));
}

// Right-aligned figures in a table cell.
void figure(const std::string &text, ImU32 colour = 0) {
  ImGui::PushFont(ui::monoFont(), 0.0f);
  const float w = ImGui::CalcTextSize(text.c_str()).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
  if (colour) ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextUnformatted(text.c_str());
  if (colour) ImGui::PopStyleColor();
  ImGui::PopFont();
}

// A small capsule naming what kind of entry a routine has.
void chip(const char *label, ImU32 colour) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  ImGui::SameLine(0, 6);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 size = ImGui::CalcTextSize(label);
  ImGui::PopFont();
  const float h = size.y + 2;
  const float top = at.y + (ImGui::GetTextLineHeight() - h) * 0.5f + 1;
  draw->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + size.x + 10, top + h), alpha(colour, 0.22f), h * 0.5f);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(ImVec2(at.x + 5, top + 1), colour, label);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(size.x + 10, ImGui::GetTextLineHeight()));
}

// Record and Stop: a capsule with the record dot, and red while recording,
// the dot squared off and breathing slowly (well inside the photosensitivity
// limits CLAUDE.md sets: a small mark, under one cycle a second).
bool recordButton(bool recording) {
  const char *label = recording ? "Stop" : "Record";
  const float h = ImGui::GetFrameHeight();
  const ImVec2 text = ImGui::CalcTextSize(label);
  const ImVec2 size(h * 0.5f + 16 + text.x + 14, h);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##record", size);
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  if (hovered) ImGui::SetTooltip(recording ? "Stop recording" : "Start a new recording");

  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImU32 red = ui::palette().red;
  ImU32 bg = recording ? red : ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered
                                                                                          : ImGuiCol_Button);
  if (recording && (hovered || held)) bg = mix(red, IM_COL32(0, 0, 0, 255), held ? 0.25f : 0.12f);
  draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), bg, h * 0.5f);
  const ImVec2 dot(at.x + 8 + h * 0.25f, at.y + h * 0.5f);
  const ImU32 ink = recording ? ui::textOn(red) : ImGui::GetColorU32(ImGuiCol_Text);
  if (recording) {
    const float breath = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.0f * 3.14159265f * 0.7f);
    draw->AddCircle(dot, 7.0f + breath, alpha(ink, 0.25f + 0.35f * breath), 0, 1.5f);
    draw->AddRectFilled(ImVec2(dot.x - 3.5f, dot.y - 3.5f), ImVec2(dot.x + 3.5f, dot.y + 3.5f), ink, 1.5f);
  } else {
    draw->AddCircleFilled(dot, 5.5f, red);
  }
  draw->AddText(ImVec2(dot.x + h * 0.25f + 8, at.y + (h - text.y) * 0.5f), ink, label);
  return pressed;
}

} // namespace

ProfilerWindow::ProfilerWindow(Emulation &emulation, const CpuDebugger &debugger)
    : emulation_(emulation), debugger_(debugger) {}

// ---------------------------------------------------------------------------
// Reading the machine
// ---------------------------------------------------------------------------

void ProfilerWindow::toggleRecording() {
  const bool start = !recording_;
  recording_ = start;
  emulation_.post([start](host::MachineHost &host) {
    MachineDebug *debug = host.debug();
    if (!debug) return;
    if (start) debug->profiler().clear();
    debug->profiler().setEnabled(start);
  });
  if (start) {
    model_.clear();
    hot_.clear();
    code_.clear();
    codeFor_ = Profiler::TOP_LEVEL;
    totalTime_ = 0;
    instructions_ = 0;
    openHotPath_ = true;
    flameRoot_ = 0;
  }
  dirty_ = true;
}

void ProfilerWindow::refresh(bool force) {
  const double now = ImGui::GetTime();
  if (!force && !dirty_ && !(recording_ && now - refreshedAt_ >= REFRESH_SECONDS)) return;

  std::vector<Profiler::Node> nodes;
  std::vector<Profiler::Frame> frames;
  uint64_t framesCompleted = 0;
  bool fresh = false;
  const uint64_t nextFrame = model_.nextFrame();
  const uint32_t wantCode = hasSelection_ ? selected_ : Profiler::TOP_LEVEL;

  const bool ran = emulation_.poll(poll_, [&](host::MachineHost &host) {
    MachineDebug *debug = host.debug();
    if (host.generation() != generation_) {
      // Another machine, or the same one rebuilt: its profiler is new.
      generation_ = host.generation();
      fresh = true;
      const MachineProfile &profile = host.profile();
      wide_ = profile.family == MachineFamily::AppleIIgs;
      // A IIgs is measured on its slow clock (profiler.hpp).
      clockHz_ = wide_ ? 1023000.0 : profile.timing.cpuClockHz;
      model_.setFrameTime(profile.timing.cyclesPerFrame());
    }
    if (!debug) return;
    const Profiler &p = debug->profiler();
    recording_ = p.enabled();
    nodes = p.nodes();
    framesCompleted = p.framesCompleted();
    p.framesFrom(fresh ? 0 : nextFrame, frames);
    totalTime_ = p.totalTime();
    instructions_ = p.instructions();
    depth_ = p.depth();
    truncated_ = p.truncated();

    hot_.clear();
    for (const Profiler::AddressCost &c : p.hottest(HOT_LINES)) {
      HotLine line;
      line.address = c.address;
      line.time = c.time;
      line.executions = c.executions;
      const host::Instruction in = host.disassemble(c.address);
      line.instruction = in.mnemonic + (in.operand.empty() ? "" : " " + in.operand);
      hot_.push_back(std::move(line));
    }

    // The selected routine's code, from its entry to where it stops being
    // code that ran: past a return or a jump, it goes on only while lines
    // that ran follow closely.
    code_.clear();
    codeFor_ = wantCode;
    if (wantCode != Profiler::TOP_LEVEL) {
      uint32_t at = wantCode;
      const uint32_t bank = wantCode & 0xFF0000;
      for (int i = 0; i < CODE_LINES; i++) {
        const host::Instruction in = host.disassemble(at);
        CodeLine line;
        line.address = at;
        line.text = in.mnemonic + (in.operand.empty() ? "" : " " + in.operand);
        line.time = p.timeAt(at);
        line.executions = p.executionsAt(at);
        code_.push_back(std::move(line));
        const uint32_t next = bank | ((at + std::max<uint32_t>(1, in.length)) & 0xFFFF);
        if (next < at) break; // the end of the bank
        at = next;
        if (in.flow == FlowType::RETURN || in.flow == FlowType::UNCONDITIONAL || in.flow == FlowType::INDIRECT ||
            in.flow == FlowType::HALT) {
          bool more = false;
          for (uint32_t ahead = 0; ahead < 24 && !more; ahead++) more = p.executionsAt(bank | ((at + ahead) & 0xFFFF)) > 0;
          if (!more) break;
        }
      }
    }
  });
  if (!ran) return;
  refreshedAt_ = now;
  dirty_ = false;

  if (fresh) {
    model_.clear();
    flameRoot_ = 0;
    openHotPath_ = true;
  }
  for (HotLine &line : hot_) {
    if (const auto near = debugger_.symbols().nearestCode(line.address)) {
      char where[96];
      if (near->offset) std::snprintf(where, sizeof where, "%s+$%X", near->symbol.name.c_str(), near->offset);
      else std::snprintf(where, sizeof where, "%s", near->symbol.name.c_str());
      line.where = where;
    }
  }
  model_.setTree(std::move(nodes), framesCompleted);
  model_.addFrames(std::move(frames));
  model_.setSelected(hasSelection_ ? selected_ : Profiler::TOP_LEVEL);
  model_.rebuild();
  if (flameRoot_ >= model_.nodes().size()) flameRoot_ = 0;
}

// ---------------------------------------------------------------------------
// Names, colours and figures
// ---------------------------------------------------------------------------

std::string ProfilerWindow::formatAddress(uint32_t address) const {
  char text[16];
  if (wide_) std::snprintf(text, sizeof text, "$%02X/%04X", (address >> 16) & 0xFF, address & 0xFFFF);
  else std::snprintf(text, sizeof text, "$%04X", address & 0xFFFF);
  return text;
}

std::string ProfilerWindow::name(uint32_t function) const {
  if (function == Profiler::TOP_LEVEL) return "Top level";
  if (const auto symbol = debugger_.symbols().lookup(function)) return symbol->name;
  return formatAddress(function);
}

// A time as the machine's cycles, and in real time beside it once that
// reads better.
std::string ProfilerWindow::formatTime(double time) const {
  char text[48];
  const double seconds = time / clockHz_;
  if (seconds >= 1.0) std::snprintf(text, sizeof text, "%s cyc  %.2f s", count(time).c_str(), seconds);
  else if (seconds >= 0.001) std::snprintf(text, sizeof text, "%s cyc  %.1f ms", count(time).c_str(), seconds * 1e3);
  else std::snprintf(text, sizeof text, "%s cyc", count(time).c_str());
  return text;
}

// The timeline's colours: the logo's stripes, in an order that keeps
// neighbouring bands apart, and a seventh between two of them.
ImU32 ProfilerWindow::bandColour(int band) const {
  const ui::Palette &p = ui::palette();
  switch (band) {
  case 0: return p.blue;
  case 1: return p.orange;
  case 2: return p.green;
  case 3: return p.purple;
  case 4: return p.yellow;
  case 5: return p.red;
  case 6: return mix(p.blue, p.green, 0.5f);
  default: return ImGui::GetColorU32(ImGuiCol_Text, 0.22f);
  }
}

// A routine's colour: its band's, or one of its own from its address, quieter
// than the bands so the routines that matter stand out.
ImU32 ProfilerWindow::colourFor(uint32_t function) const {
  if (const ProfileModel::Function *f = model_.function(function); f && f->band >= 0) return bandColour(f->band);
  uint32_t h = function * 2654435761u;
  h ^= h >> 15;
  const float hue = static_cast<float>(h % 360) / 360.0f;
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(hue, ui::isDark() ? 0.35f : 0.30f, ui::isDark() ? 0.55f : 0.86f, r, g, b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

bool ProfilerWindow::matchesFilter(const std::string &text) const {
  if (!filter_[0]) return true;
  const std::string needle = filter_;
  auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
  return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                     [&](char a, char b) { return lower(static_cast<unsigned char>(a)) == lower(static_cast<unsigned char>(b)); }) != text.end();
}

void ProfilerWindow::select(uint32_t function) {
  if (hasSelection_ && selected_ == function) return;
  selected_ = function;
  hasSelection_ = true;
  dirty_ = true;
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void ProfilerWindow::draw(bool *open) {
  if (!open || !*open) return;
  refresh(false);

  ui::BeforeWindow(TITLE);
  ImGui::SetNextWindowSizeConstraints(ImVec2(760, 460), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::SetNextWindowSize(ImVec2(1180, 760), ImGuiCond_FirstUseEver);
  if (ui::BeginWindow(TITLE, open)) {
    drawHeader();
    if (model_.time() <= 0 && model_.frames().empty() && !recording_) {
      drawEmpty();
    } else {
      drawTimeline();
      drawLegend();
      ImGui::Dummy(ImVec2(0, 2));
      const float detail = DETAIL_WIDTH;
      const float gap = ImGui::GetStyle().ItemSpacing.x;
      const float mainWidth = std::max(200.0f, ImGui::GetContentRegionAvail().x - detail - gap);
      if (ImGui::BeginChild("##main", ImVec2(mainWidth, 0), ImGuiChildFlags_None,
                            view == Flame ? ImGuiWindowFlags_NoScrollWithMouse : 0)) {
        switch (view) {
        case Functions: drawFunctions(); break;
        case CallTree: drawCallTree(); break;
        case Flame: drawFlame(); break;
        default: drawHot(); break;
        }
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.55f));
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
      if (ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding)) drawDetail();
      ImGui::EndChild();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();
    }
  }
  ImGui::End();
}

void ProfilerWindow::drawHeader() {
  if (recordButton(recording_)) toggleRecording();
  ImGui::SameLine();
  ImGui::BeginDisabled(recording_ || (model_.time() <= 0 && model_.frames().empty()));
  if (ui::Button("Clear")) {
    emulation_.post([](host::MachineHost &host) {
      if (MachineDebug *debug = host.debug()) debug->profiler().clear();
    });
    model_.clear();
    hot_.clear();
    code_.clear();
    totalTime_ = 0;
    instructions_ = 0;
    hasSelection_ = false;
    dirty_ = true;
  }
  ImGui::EndDisabled();

  // What has been recorded, in the machine's time.
  ImGui::SameLine(0, 18);
  ImGui::AlignTextToFramePadding();
  ImGui::PushFont(ui::monoFont(), 0.0f);
  const double seconds = totalTime_ / clockHz_;
  if (recording_) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::palette().red), "REC");
    ImGui::SameLine(0, 8);
  }
  ImGui::Text("%d:%05.2f", static_cast<int>(seconds / 60), std::fmod(seconds, 60.0));
  ImGui::PopFont();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Machine time recorded");
  ImGui::SameLine(0, 14);
  ImGui::TextDisabled("%s frames  %s instructions", count(static_cast<double>(model_.frameCount())).c_str(),
                      count(static_cast<double>(instructions_)).c_str());
  if (truncated_) {
    ImGui::SameLine(0, 10);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::palette().yellow), "Tree full");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("The call tree reached %zu paths; newer paths are charged to their deepest known caller",
                        Profiler::MAX_NODES);
    }
  }
  if (const auto &range = model_.range()) {
    ImGui::SameLine(0, 14);
    char label[64];
    std::snprintf(label, sizeof label, "Frames %llu to %llu  \xC3\x97", static_cast<unsigned long long>(range->first),
                  static_cast<unsigned long long>(range->second));
    ImGui::PushStyleColor(ImGuiCol_Button, alpha(ui::accentText(), 0.18f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, alpha(ui::accentText(), 0.30f));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::accentText());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
    if (ImGui::Button(label)) {
      model_.setRange(std::nullopt);
      model_.rebuild();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Looking at these frames alone; click to see the whole recording");
  }

  // The views and the filter, at the right.
  static const std::vector<std::string> views = {"Routines", "Call Tree", "Flame Graph", "Hot Lines"};
  const float segment = 380;
  const float field = 170;
  const float right = ImGui::GetWindowContentRegionMax().x;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12, right - segment - field - 12));
  ui::SegmentedControl("##view", &view, views, segment);
  ImGui::SameLine(0, 12);
  ImGui::SetNextItemWidth(field);
  ImGui::InputTextWithHint("##filter", "Filter", filter_, sizeof filter_);
  ImGui::Dummy(ImVec2(0, 2));
}

void ProfilerWindow::drawEmpty() {
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 centre(origin.x + avail.x * 0.5f, origin.y + avail.y * 0.42f);
  ImDrawList *draw = ImGui::GetWindowDrawList();

  // A dial of the six stripes, a share each, for want of a recording.
  const float radius = 46;
  const ui::Palette &p = ui::palette();
  const ImU32 stripes[] = {p.green, p.yellow, p.orange, p.red, p.purple, p.blue};
  const float shares[] = {0.34f, 0.22f, 0.16f, 0.12f, 0.09f, 0.07f};
  float angle = -3.14159265f * 0.5f;
  for (int i = 0; i < 6; i++) {
    const float sweep = shares[i] * 2 * 3.14159265f;
    draw->PathArcTo(centre, radius, angle + 0.03f, angle + sweep - 0.03f, 32);
    draw->PathStroke(stripes[i], ImDrawFlags_None, 12.0f);
    angle += sweep;
  }

  auto centred = [&](const char *text, float y, ImU32 colour, float scale) {
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * scale);
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(centre.x - size.x * 0.5f, y), colour, text);
    ImGui::PopFont();
  };
  float y = centre.y + radius + 28;
  centred("See where the time goes", y, ImGui::GetColorU32(ImGuiCol_Text), 1.45f);
  y += ImGui::GetFontSize() * 1.45f + 10;
  centred("Record while your program runs. Every call and interrupt is followed,", y, secondary(), 1.0f);
  y += ImGui::GetTextLineHeightWithSpacing();
  centred("and the time each routine and each line takes is counted in machine cycles.", y, secondary(), 1.0f);
  y += ImGui::GetTextLineHeightWithSpacing() + 18;

  const char *label = "Start Recording";
  const float width = ImGui::CalcTextSize(label).x + 40;
  ImGui::SetCursorScreenPos(ImVec2(centre.x - width * 0.5f, y));
  if (ui::Button(label, ImVec2(width, 0), ui::ButtonKind::Primary)) toggleRecording();
  y += ImGui::GetFrameHeight() + 18;
  if (debugger_.symbols().importedCount() == 0) {
    centred("Routines are named from the CPU debugger's symbols: import your", y, secondary(), ui::SMALL_TEXT);
    y += ImGui::GetTextLineHeightWithSpacing();
    centred("assembler's symbol file there, or build with ca65 from the Build window.", y, secondary(),
            ui::SMALL_TEXT);
  }
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(avail);
}

// ---------------------------------------------------------------------------
// The timeline
// ---------------------------------------------------------------------------

void ProfilerWindow::drawTimeline() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 a = at, b(at.x + width, at.y + TIMELINE_HEIGHT);
  ImGui::InvisibleButton("##timeline", ImVec2(width, TIMELINE_HEIGHT));
  const bool hovered = ImGui::IsItemHovered();

  draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 8.0f);
  const auto &frames = model_.frames();
  const auto &bands = model_.frameBands();
  const size_t n = std::min(frames.size(), bands.size());
  if (n == 0) {
    const char *text = recording_ ? "Waiting for the first frame\xE2\x80\xA6" : "No frames recorded";
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(a.x + (width - size.x) * 0.5f, a.y + (TIMELINE_HEIGHT - size.y) * 0.5f), secondary(), text);
    return;
  }

  // The plot sits inside the rounded frame, a column a pixel (or wider when
  // there are fewer frames than pixels).
  const float pad = 6;
  const ImVec2 pa(a.x + pad, a.y + pad), pb(b.x - pad, b.y - pad);
  const float plotW = pb.x - pa.x, plotH = pb.y - pa.y;
  const int columns = std::max(1, static_cast<int>(std::min<float>(plotW, static_cast<float>(n))));
  const float columnW = plotW / static_cast<float>(columns);
  const double perColumn = static_cast<double>(n) / columns;

  draw->PushClipRect(pa, pb, true);
  std::vector<ImVec2> trace;
  trace.reserve(static_cast<size_t>(columns) * 2);
  const bool tracing = hasSelection_ && selected_ != Profiler::TOP_LEVEL;
  for (int c = 0; c < columns; c++) {
    const size_t f0 = static_cast<size_t>(c * perColumn);
    const size_t f1 = std::max(f0 + 1, std::min(n, static_cast<size_t>((c + 1) * perColumn)));
    std::array<double, ProfileModel::BANDS + 1> sum{};
    double total = 0, selected = 0;
    for (size_t f = f0; f < f1; f++) {
      for (size_t k = 0; k <= ProfileModel::BANDS; k++) sum[k] += bands[f].time[k];
      total += bands[f].total;
      selected += bands[f].selected;
    }
    if (total <= 0) continue;
    const float x0 = pa.x + c * columnW;
    const float x1 = std::max(x0 + 1.0f, pa.x + (c + 1) * columnW - (columnW > 3 ? 1.0f : 0.0f));
    float y = pb.y;
    // The bands stack from the bottom, the biggest first; everything else
    // goes on top, quietly.
    for (size_t k = 0; k <= ProfileModel::BANDS; k++) {
      if (sum[k] <= 0) continue;
      const float h = static_cast<float>(sum[k] / total) * plotH;
      ImU32 colour = bandColour(k == ProfileModel::BANDS ? -1 : static_cast<int>(k));
      if (tracing) colour = alpha(colour, 0.55f);
      draw->AddRectFilled(ImVec2(x0, y - h), ImVec2(x1, y), colour);
      y -= h;
    }
    if (tracing) {
      const float ty = pb.y - static_cast<float>(selected / total) * plotH;
      trace.push_back(ImVec2(x0, ty));
      trace.push_back(ImVec2(x1, ty));
    }
  }
  if (trace.size() >= 2) {
    // The selected routine's whole share of each frame, as a line over the
    // bands, with a soft fill under it so it reads against any colour.
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    for (size_t i = 0; i + 1 < trace.size(); i += 2) {
      draw->AddRectFilled(ImVec2(trace[i].x, trace[i].y), ImVec2(trace[i + 1].x, pb.y), alpha(ink, 0.10f));
    }
    draw->AddPolyline(trace.data(), static_cast<int>(trace.size()), ink, ImDrawFlags_None, 1.75f);
  }

  // Gridlines at a quarter, a half and three quarters of a frame.
  for (int q = 1; q < 4; q++) {
    const float gy = pb.y - plotH * q / 4.0f;
    draw->AddLine(ImVec2(pa.x, gy), ImVec2(pb.x, gy), ImGui::GetColorU32(ImGuiCol_WindowBg, q == 2 ? 0.55f : 0.30f));
  }

  // Frame index under the pointer.
  auto frameAt = [&](float x) {
    const float t = std::clamp((x - pa.x) / plotW, 0.0f, 0.9999f);
    return frames[static_cast<size_t>(t * static_cast<float>(n))].index;
  };
  auto xOf = [&](uint64_t index) {
    const uint64_t first = frames.front().index;
    const double pos = static_cast<double>(index - std::min(index, first)) / static_cast<double>(n);
    return pa.x + static_cast<float>(pos) * plotW;
  };

  // A range: dim what is outside it.
  std::optional<std::pair<uint64_t, uint64_t>> shown = model_.range();
  if (dragging_) {
    const uint64_t to = frameAt(ImGui::GetIO().MousePos.x);
    shown = std::make_pair(std::min(dragFrom_, to), std::max(dragFrom_, to));
  }
  if (shown) {
    const float x0 = xOf(shown->first), x1 = std::max(x0 + 2, xOf(shown->second + 1));
    const ImU32 veil = ImGui::GetColorU32(ImGuiCol_WindowBg, 0.62f);
    draw->AddRectFilled(pa, ImVec2(x0, pb.y), veil);
    draw->AddRectFilled(ImVec2(x1, pa.y), pb, veil);
    draw->AddRect(ImVec2(x0, pa.y), ImVec2(x1, pb.y), ui::accentText(), 2.0f, 0, 1.5f);
  }
  draw->PopClipRect();

  // Drag to choose frames; a click without a drag goes back to them all.
  if (ImGui::IsItemActivated()) {
    dragging_ = true;
    dragFrom_ = frameAt(ImGui::GetIO().MousePos.x);
  }
  if (dragging_ && !ImGui::IsItemActive()) {
    dragging_ = false;
    const uint64_t to = frameAt(ImGui::GetIO().MousePos.x);
    const float moved = std::fabs(ImGui::GetIO().MouseDragMaxDistanceSqr[0]);
    if (moved < 16.0f) model_.setRange(std::nullopt);
    else model_.setRange(std::make_pair(std::min(dragFrom_, to), std::max(dragFrom_, to)));
    model_.rebuild();
  }

  if (hovered && !dragging_) {
    const float x = ImGui::GetIO().MousePos.x;
    if (x >= pa.x && x <= pb.x) {
      draw->AddLine(ImVec2(x, pa.y), ImVec2(x, pb.y), ImGui::GetColorU32(ImGuiCol_Text, 0.6f), 1.0f);
      const size_t f = static_cast<size_t>(std::clamp((x - pa.x) / plotW, 0.0f, 0.9999f) * static_cast<float>(n));
      const ProfileModel::FrameBands &fb = bands[f];
      ImGui::BeginTooltip();
      ImGui::Text("Frame %llu", static_cast<unsigned long long>(frames[f].index));
      ImGui::Separator();
      const auto &names = model_.bandFunctions();
      for (size_t k = 0; k <= ProfileModel::BANDS; k++) {
        if (fb.time[k] <= 0 || fb.total <= 0) continue;
        if (k < ProfileModel::BANDS && k >= names.size()) continue;
        const ImVec2 dot = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(dot.x + 5, dot.y + ImGui::GetTextLineHeight() * 0.5f), 4.5f,
            bandColour(k == ProfileModel::BANDS ? -1 : static_cast<int>(k)));
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();
        ImGui::TextUnformatted(k == ProfileModel::BANDS ? "Everything else" : name(names[k]).c_str());
        ImGui::SameLine(240);
        ImGui::PushFont(ui::monoFont(), 0.0f);
        ImGui::TextUnformatted(percent(fb.time[k] / fb.total).c_str());
        ImGui::PopFont();
      }
      if (tracing && fb.total > 0) {
        ImGui::Separator();
        ImGui::Text("%s, calls included", name(selected_).c_str());
        ImGui::SameLine(240);
        ImGui::PushFont(ui::monoFont(), 0.0f);
        ImGui::TextUnformatted(percent(fb.selected / fb.total).c_str());
        ImGui::PopFont();
      }
      ImGui::TextDisabled("Drag to look at a range of frames");
      ImGui::EndTooltip();
    }
  }
}

void ProfilerWindow::drawLegend() {
  const auto &names = model_.bandFunctions();
  ImGui::Dummy(ImVec2(0, 1));
  for (size_t k = 0; k < names.size(); k++) {
    if (k) ImGui::SameLine(0, 14);
    ImGui::PushID(static_cast<int>(k));
    const std::string label = name(names[k]);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 size(14 + ImGui::CalcTextSize(label.c_str()).x, line);
    if (ImGui::InvisibleButton("##band", size)) select(names[k]);
    const bool on = hasSelection_ && selected_ == names[k];
    const bool hover = ImGui::IsItemHovered();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(at.x, at.y + line * 0.5f - 4), ImVec2(at.x + 8, at.y + line * 0.5f + 4),
                        bandColour(static_cast<int>(k)), 2.0f);
    draw->AddText(ImVec2(at.x + 14, at.y), on || hover ? ImGui::GetColorU32(ImGuiCol_Text) : secondary(), label.c_str());
    if (on) draw->AddLine(ImVec2(at.x + 14, at.y + line), ImVec2(at.x + size.x, at.y + line), ui::accentText(), 1.5f);
    ImGui::PopID();
  }
}

// ---------------------------------------------------------------------------
// Routines
// ---------------------------------------------------------------------------

void ProfilerWindow::drawFunctions() {
  const double whole = model_.time();
  const auto &all = model_.functions();
  std::vector<size_t> rows;
  rows.reserve(all.size());
  for (size_t i = 0; i < all.size(); i++) {
    if (matchesFilter(name(all[i].address)) || matchesFilter(formatAddress(all[i].address))) rows.push_back(i);
  }

  const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (!ImGui::BeginTable("##routines", 6, flags)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_NoHide, 2.4f);
  ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 1.3f);
  ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_PreferSortDescending, 1.3f);
  ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_PreferSortDescending, 0.7f);
  ImGui::TableSetupColumn("Per Call", ImGuiTableColumnFlags_PreferSortDescending, 0.75f);
  ImGui::TableSetupColumn("Per Frame", ImGuiTableColumnFlags_PreferSortDescending, 0.75f);
  ImGui::TableHeadersRow();

  if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
    const ImGuiTableColumnSortSpecs s = specs->Specs[0];
    const bool up = s.SortDirection == ImGuiSortDirection_Ascending;
    auto key = [&](const ProfileModel::Function &f) -> double {
      switch (s.ColumnIndex) {
      case 2: return f.total;
      case 3: return static_cast<double>(f.calls);
      case 4: return f.calls ? f.total / static_cast<double>(f.calls) : 0;
      case 5: return f.total;
      default: return f.self;
      }
    };
    if (s.ColumnIndex == 0) {
      std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
        const int c = name(all[a].address).compare(name(all[b].address));
        return up ? c < 0 : c > 0;
      });
    } else {
      std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
        return up ? key(all[a]) < key(all[b]) : key(all[a]) > key(all[b]);
      });
    }
  }

  const float rowHeight = ImGui::GetTextLineHeight() + 10;
  const double frames = std::max<double>(1.0, static_cast<double>(model_.frameCount()));
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(rows.size()), rowHeight);
  while (clipper.Step()) {
    for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
      const ProfileModel::Function &f = all[rows[static_cast<size_t>(r)]];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(f.address));
      const bool on = hasSelection_ && selected_ == f.address;
      const float top = ImGui::GetCursorPosY();
      if (ImGui::Selectable("##row", on, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                             ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0, rowHeight - 4))) {
        select(f.address);
        if (ImGui::IsMouseDoubleClicked(0) && f.address != Profiler::TOP_LEVEL && showAddress) showAddress(f.address);
      }
      ImGui::SetCursorPosY(top + 2);
      const ImVec2 dot = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + 5, dot.y + ImGui::GetTextLineHeight() * 0.5f), 4.5f,
                                                  colourFor(f.address));
      ImGui::Dummy(ImVec2(12, 0));
      ImGui::SameLine();
      ImGui::TextUnformatted(name(f.address).c_str());
      if (f.address != Profiler::TOP_LEVEL && debugger_.symbols().lookup(f.address)) {
        ImGui::SameLine(0, 8);
        ImGui::PushFont(ui::monoFont(), 0.0f);
        ImGui::TextDisabled("%s", formatAddress(f.address).c_str());
        ImGui::PopFont();
      }
      if (f.entry == Profiler::Entry::Interrupt) chip("INTERRUPT", ui::palette().orange);

      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(top + 2);
      shareBar(whole > 0 ? f.self / whole : 0, colourFor(f.address));
      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(top + 2);
      shareBar(whole > 0 ? f.total / whole : 0, alpha(colourFor(f.address), 0.55f));
      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(top + 2);
      figure(f.address == Profiler::TOP_LEVEL ? "" : count(static_cast<double>(f.calls)));
      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(top + 2);
      figure(f.calls ? count(f.total / static_cast<double>(f.calls)) : "");
      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(top + 2);
      figure(count(f.total / frames));
      if (ImGui::IsItemHovered() && model_.frameTime() > 0) {
        ImGui::SetTooltip("%s of a frame's %s cycles", percent(f.total / frames / model_.frameTime()).c_str(),
                          count(model_.frameTime()).c_str());
      }
      ImGui::PopID();
    }
  }
  ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// The call tree
// ---------------------------------------------------------------------------

void ProfilerWindow::drawCallTree() {
  if (ui::Button("Expand Hot Path")) openHotPath_ = true;
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Each path a routine was reached by, the heaviest first");

  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (!ImGui::BeginTable("##tree", 4, flags)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_NoHide, 2.8f);
  ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_None, 1.3f);
  ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_None, 1.3f);
  ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_None, 0.6f);
  ImGui::TableHeadersRow();
  if (!model_.nodes().empty()) drawTreeNode(0, 0);
  ImGui::EndTable();
  openHotPath_ = false;
}

void ProfilerWindow::drawTreeNode(uint32_t node, int depth) {
  const double whole = model_.time();
  const double total = model_.nodeTotal(node);
  // Paths that took next to nothing are left out, or the tree drowns.
  if (node != 0 && whole > 0 && total / whole < 0.0005 && !filter_[0]) return;
  const Profiler::Node &n = model_.nodes()[node];
  const auto &children = model_.children(node);
  const std::string label = name(n.function);

  // Open down the heaviest path while it still carries a real share.
  if (openHotPath_) {
    const uint32_t parent = n.parent < 0 ? 0 : static_cast<uint32_t>(n.parent);
    const bool heaviest = node == 0 || (!model_.children(parent).empty() && model_.children(parent).front() == node);
    ImGui::SetNextItemOpen(heaviest && (whole <= 0 || total / whole >= 0.03));
  }

  ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeight() + 8);
  ImGui::TableNextColumn();
  ImGui::PushID(static_cast<int>(node));
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_FramePadding;
  if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  if (hasSelection_ && selected_ == n.function) flags |= ImGuiTreeNodeFlags_Selected;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
  const bool open = ImGui::TreeNodeEx("##node", flags, "%s", "");
  ImGui::PopStyleVar();
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) select(n.function);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && n.function != Profiler::TOP_LEVEL && showAddress) {
    showAddress(n.function);
  }
  ImGui::SameLine(0, 2);
  const ImVec2 dot = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + 5, dot.y + ImGui::GetTextLineHeight() * 0.5f + 3), 4.5f,
                                              colourFor(n.function));
  ImGui::Dummy(ImVec2(12, 0));
  ImGui::SameLine();
  const bool matches = matchesFilter(label);
  if (filter_[0] && matches) ImGui::PushStyleColor(ImGuiCol_Text, ui::accentText());
  ImGui::TextUnformatted(label.c_str());
  if (filter_[0] && matches) ImGui::PopStyleColor();
  if (n.entry == Profiler::Entry::Interrupt) chip("INTERRUPT", ui::palette().orange);

  ImGui::TableNextColumn();
  shareBar(whole > 0 ? total / whole : 0, colourFor(n.function));
  ImGui::TableNextColumn();
  shareBar(whole > 0 ? model_.nodeSelf(node) / whole : 0, alpha(colourFor(n.function), 0.55f));
  ImGui::TableNextColumn();
  figure(node == 0 ? "" : count(static_cast<double>(model_.nodeCalls(node))));

  if (open && !children.empty()) {
    for (uint32_t child : children) drawTreeNode(child, depth + 1);
    ImGui::TreePop();
  }
  ImGui::PopID();
}

// ---------------------------------------------------------------------------
// The flame graph
// ---------------------------------------------------------------------------

void ProfilerWindow::drawFlame() {
  const auto &nodes = model_.nodes();
  const double whole = model_.nodeTotal(0);
  if (nodes.empty() || whole <= 0) {
    ImGui::TextDisabled("Nothing recorded yet");
    return;
  }

  // Breadcrumbs back up from the root in view.
  {
    std::vector<uint32_t> trail;
    for (int32_t at = static_cast<int32_t>(flameRoot_); at >= 0; at = nodes[static_cast<size_t>(at)].parent) {
      trail.push_back(static_cast<uint32_t>(at));
    }
    std::reverse(trail.begin(), trail.end());
    for (size_t i = 0; i < trail.size(); i++) {
      if (i) {
        ImGui::SameLine(0, 6);
        ImGui::TextDisabled("\xE2\x80\xBA");
        ImGui::SameLine(0, 6);
      }
      ImGui::PushID(static_cast<int>(i));
      const bool last = i + 1 == trail.size();
      const std::string label = name(nodes[trail[i]].function);
      if (last) {
        ImGui::TextUnformatted(label.c_str());
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::accentText());
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) flameRoot_ = trail[i];
      }
      ImGui::PopID();
    }
    if (flameRoot_ != 0) {
      ImGui::SameLine(0, 16);
      ImGui::TextDisabled("Click a bar to zoom in; Escape zooms out");
    }
  }

  // Where every node starts, along its parent, children heaviest first.
  std::vector<double> start(nodes.size(), 0.0);
  std::vector<uint32_t> stack{0};
  while (!stack.empty()) {
    const uint32_t at = stack.back();
    stack.pop_back();
    double offset = start[at];
    for (uint32_t child : model_.children(at)) {
      start[child] = offset;
      offset += model_.nodeTotal(child);
      stack.push_back(child);
    }
  }

  // Ease toward the root in view.
  const double targetFrom = start[flameRoot_] / whole;
  const double targetTo = (start[flameRoot_] + std::max(model_.nodeTotal(flameRoot_), whole * 1e-9)) / whole;
  const double now = ImGui::GetTime();
  const double dt = flameAt_ < 0 ? 1.0 : std::min(0.1, now - flameAt_);
  flameAt_ = now;
  const double k = 1.0 - std::exp(-dt * 14.0);
  flameFrom_ += (targetFrom - flameFrom_) * k;
  flameTo_ += (targetTo - flameTo_) * k;
  if (std::fabs(flameFrom_ - targetFrom) < 1e-6) flameFrom_ = targetFrom;
  if (std::fabs(flameTo_ - targetTo) < 1e-6) flameTo_ = targetTo;

  if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape) && flameRoot_ != 0) {
    const int32_t up = nodes[flameRoot_].parent;
    flameRoot_ = up < 0 ? 0 : static_cast<uint32_t>(up);
  }

  ImGui::Dummy(ImVec2(0, 4));
  if (!ImGui::BeginChild("##flame", ImVec2(0, 0), ImGuiChildFlags_None)) {
    ImGui::EndChild();
    return;
  }
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const float rowH = ImGui::GetTextLineHeight() + 9;
  const double span = std::max(1e-12, flameTo_ - flameFrom_);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool windowHovered = ImGui::IsWindowHovered();

  int deepest = 0;
  int32_t hoveredNode = -1;
  struct Item {
    uint32_t node;
    int depth;
  };
  std::vector<Item> work{{0, 0}};
  while (!work.empty()) {
    const Item item = work.back();
    work.pop_back();
    const double s = start[item.node] / whole;
    const double e = (start[item.node] + model_.nodeTotal(item.node)) / whole;
    float x0 = origin.x + static_cast<float>((s - flameFrom_) / span) * width;
    float x1 = origin.x + static_cast<float>((e - flameFrom_) / span) * width;
    if (x1 < origin.x || x0 > origin.x + width || x1 - x0 < 0.6f) continue;
    x0 = std::max(x0, origin.x - 2);
    x1 = std::min(x1, origin.x + width + 2);
    const float y0 = origin.y + item.depth * rowH;
    const float y1 = y0 + rowH - 2;
    deepest = std::max(deepest, item.depth);

    const uint32_t function = model_.nodes()[item.node].function;
    const bool inside = windowHovered && mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 && mouse.y < y1;
    if (inside) hoveredNode = static_cast<int32_t>(item.node);
    const bool chosen = hasSelection_ && selected_ == function;
    ImU32 fill = colourFor(function);
    // Above the root in view, the path into it, dimmed.
    const bool above = item.node != flameRoot_ && start[item.node] <= start[flameRoot_] &&
                       start[item.node] + model_.nodeTotal(item.node) >= start[flameRoot_] + model_.nodeTotal(flameRoot_) &&
                       item.depth < 64 && [&] {
                         for (int32_t at = static_cast<int32_t>(flameRoot_); at >= 0; at = nodes[static_cast<size_t>(at)].parent) {
                           if (static_cast<uint32_t>(at) == item.node) return true;
                         }
                         return false;
                       }();
    if (above) fill = alpha(fill, 0.45f);
    if (inside) fill = mix(fill, ImGui::GetColorU32(ImGuiCol_Text), 0.18f);
    draw->AddRectFilled(ImVec2(x0 + 0.5f, y0), ImVec2(x1 - 0.5f, y1), fill, 3.0f);
    if (chosen) draw->AddRect(ImVec2(x0 + 0.5f, y0), ImVec2(x1 - 0.5f, y1), ImGui::GetColorU32(ImGuiCol_Text), 3.0f, 0, 2.0f);
    if (x1 - x0 > 28) {
      const std::string label = name(function);
      const float tx0 = std::max(x0, origin.x) + 6;
      const float tx1 = std::min(x1, origin.x + width) - 4;
      if (tx1 > tx0 + 8) {
        const ImU32 ink = ui::textOn(fill | IM_COL32_A_MASK);
        ImGui::PushStyleColor(ImGuiCol_Text, ink);
        ImGui::RenderTextEllipsis(draw, ImVec2(tx0, y0 + 4), ImVec2(tx1, y1), tx1, label.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
      }
    }
    const auto &children = model_.children(item.node);
    for (size_t i = children.size(); i-- > 0;) work.push_back(Item{children[i], item.depth + 1});
  }
  ImGui::Dummy(ImVec2(width, (deepest + 1) * rowH + 8));

  if (hoveredNode >= 0) {
    const auto node = static_cast<uint32_t>(hoveredNode);
    const uint32_t function = nodes[node].function;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(name(function).c_str());
    if (function != Profiler::TOP_LEVEL) {
      ImGui::SameLine();
      ImGui::PushFont(ui::monoFont(), 0.0f);
      ImGui::TextDisabled("%s", formatAddress(function).c_str());
      ImGui::PopFont();
    }
    ImGui::Separator();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    ImGui::Text("Total %-7s %s", percent(model_.nodeTotal(node) / model_.time()).c_str(),
                formatTime(model_.nodeTotal(node)).c_str());
    ImGui::Text("Self  %-7s %s", percent(model_.nodeSelf(node) / model_.time()).c_str(),
                formatTime(model_.nodeSelf(node)).c_str());
    if (node) ImGui::Text("Calls %s", count(static_cast<double>(model_.nodeCalls(node))).c_str());
    ImGui::PopFont();
    ImGui::EndTooltip();
    if (ImGui::IsMouseClicked(0)) {
      select(function);
      flameRoot_ = node == flameRoot_ && nodes[node].parent >= 0 ? static_cast<uint32_t>(nodes[node].parent) : node;
    }
    if (ImGui::IsMouseDoubleClicked(0) && function != Profiler::TOP_LEVEL && showAddress) showAddress(function);
  }
  ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Hot lines
// ---------------------------------------------------------------------------

void ProfilerWindow::drawHot() {
  const double whole = totalTime_;
  if (model_.range()) {
    ImGui::TextDisabled("Lines are counted over the whole recording, not the frames chosen");
  }
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (!ImGui::BeginTable("##hot", 6, flags)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_None, 0.7f);
  ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_None, 1.3f);
  ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_None, 1.3f);
  ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_None, 1.3f);
  ImGui::TableSetupColumn("Runs", ImGuiTableColumnFlags_None, 0.6f);
  ImGui::TableSetupColumn("Cycles", ImGuiTableColumnFlags_None, 0.55f);
  ImGui::TableHeadersRow();
  const float rowHeight = ImGui::GetTextLineHeight() + 8;
  const ImU32 heat = ui::palette().orange;
  for (const HotLine &line : hot_) {
    const std::string address = formatAddress(line.address);
    if (!matchesFilter(line.where) && !matchesFilter(line.instruction) && !matchesFilter(address)) continue;
    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(line.address));
    if (ImGui::Selectable("##line", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                               ImGuiSelectableFlags_AllowDoubleClick)) {
      if (ImGui::IsMouseDoubleClicked(0) && showAddress) showAddress(line.address);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Double-click to open in the CPU debugger");
    ImGui::SameLine(0, 0);
    ImGui::PushFont(ui::monoFont(), 0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::palette().orange), "%s", address.c_str());
    ImGui::TableNextColumn();
    if (line.where.empty()) ImGui::TextDisabled("\xE2\x80\x94");
    else ImGui::TextUnformatted(line.where.c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(line.instruction.c_str());
    ImGui::PopFont();
    ImGui::TableNextColumn();
    shareBar(whole > 0 ? line.time / whole : 0, heat);
    ImGui::TableNextColumn();
    figure(count(line.executions));
    ImGui::TableNextColumn();
    char each[16];
    std::snprintf(each, sizeof each, "%.1f", line.executions ? line.time / line.executions : line.time);
    figure(each);
    ImGui::PopID();
  }
  ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// The selected routine
// ---------------------------------------------------------------------------

void ProfilerWindow::drawDetail() {
  const ProfileModel::Function *f = hasSelection_ ? model_.function(selected_) : nullptr;
  if (!f) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(0, avail.y * 0.35f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail.x);
    ImGui::TextDisabled("Choose a routine to see what it costs, who calls it, what it calls, and the time each of "
                        "its lines takes.");
    ImGui::PopTextWrapPos();
    return;
  }
  const double whole = model_.time();
  ImDrawList *draw = ImGui::GetWindowDrawList();

  // Its name, large, and where it is.
  {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(ImVec2(at.x, at.y + 3), ImVec2(at.x + 4, at.y + ImGui::GetFontSize() * 1.4f + 3),
                        colourFor(f->address), 2.0f);
    ImGui::Indent(12);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.4f);
    ImGui::TextUnformatted(name(f->address).c_str());
    ImGui::PopFont();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    if (f->address == Profiler::TOP_LEVEL) ImGui::TextDisabled("outside any call");
    else ImGui::TextDisabled("%s", formatAddress(f->address).c_str());
    ImGui::PopFont();
    if (f->entry == Profiler::Entry::Interrupt) chip("INTERRUPT", ui::palette().orange);
    ImGui::Unindent(12);
  }
  ImGui::Dummy(ImVec2(0, 6));

  // Four tiles: self, total, calls, and the cost of one call.
  // Three lines stacked in each tile: a caption, the figure, and a line of
  // detail, each as tall as its own font says, so none can overlap another.
  const float body = ImGui::GetFontSize();
  ImGui::PushFont(nullptr, body * ui::SMALL_TEXT);
  const float smallLine = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  ImGui::PushFont(ui::monoFont(), body * 1.35f);
  const float bigLine = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  const float pad = 9;
  const float gap = 8;
  const float tileW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
  const float tileH = pad + smallLine + 3 + bigLine + 3 + smallLine + pad;
  const double frames = std::max<double>(1.0, static_cast<double>(model_.frameCount()));
  auto tile = [&](int index, const char *label, const std::string &big, const std::string &small, ImU32 accent) {
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const ImVec2 a(base.x + (index % 2) * (tileW + gap), base.y + (index / 2) * (tileH + gap));
    const ImVec2 b(a.x + tileW, a.y + tileH);
    draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_WindowBg, 0.85f), 7.0f);
    draw->AddRectFilled(ImVec2(a.x, a.y + 8), ImVec2(a.x + 3, b.y - 8), accent, 1.5f);
    const float x = a.x + 13;
    const float right = b.x - 8;
    float y = a.y + pad;
    ImGui::PushFont(nullptr, body * ui::SMALL_TEXT);
    draw->AddText(ImVec2(x, y), secondary(), label);
    ImGui::PopFont();
    y += smallLine + 3;
    ImGui::PushFont(ui::monoFont(), body * 1.35f);
    ImGui::RenderTextEllipsis(draw, ImVec2(x, y), ImVec2(right, y + bigLine), right, big.c_str(), nullptr, nullptr);
    ImGui::PopFont();
    y += bigLine + 3;
    ImGui::PushFont(nullptr, body * ui::SMALL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_Text, secondary());
    ImGui::RenderTextEllipsis(draw, ImVec2(x, y), ImVec2(right, y + smallLine), right, small.c_str(), nullptr,
                              nullptr);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  };
  const ui::Palette &p = ui::palette();
  tile(0, "SELF", percent(whole > 0 ? f->self / whole : 0), formatTime(f->self), colourFor(f->address));
  tile(1, "TOTAL", percent(whole > 0 ? f->total / whole : 0), formatTime(f->total), alpha(colourFor(f->address), 0.6f));
  tile(2, "CALLS", f->address == Profiler::TOP_LEVEL ? "\xE2\x80\x94" : count(static_cast<double>(f->calls)),
       count(static_cast<double>(f->calls) / frames) + " a frame", p.green);
  {
    char per[32];
    std::snprintf(per, sizeof per, "%s cyc", f->calls ? count(f->total / static_cast<double>(f->calls)).c_str() : "\xE2\x80\x94");
    const std::string ofFrame =
        model_.frameTime() > 0 ? percent(f->total / frames / model_.frameTime()) + " of each frame" : "";
    tile(3, "PER CALL", per, ofFrame, p.purple);
  }
  ImGui::Dummy(ImVec2(0, tileH * 2 + gap + 4));

  if (ImGui::BeginChild("##detailScroll", ImVec2(0, 0))) {
    drawEdges("Called From", model_.callers(f->address), f->total);
    drawEdges("Calls", model_.callees(f->address), f->total);
    drawCode();
  }
  ImGui::EndChild();
}

void ProfilerWindow::drawEdges(const char *title, const std::vector<ProfileModel::Edge> &edges, double whole) {
  if (edges.empty()) return;
  heading(title);
  const size_t shown = std::min<size_t>(edges.size(), 8);
  for (size_t i = 0; i < shown; i++) {
    const ProfileModel::Edge &e = edges[i];
    ImGui::PushID(title);
    ImGui::PushID(static_cast<int>(i));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float line = ImGui::GetTextLineHeight();
    if (ImGui::InvisibleButton("##edge", ImVec2(width, line + 10))) select(e.address);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    if (hovered) draw->AddRectFilled(at, ImVec2(at.x + width, at.y + line + 10), ImGui::GetColorU32(ImGuiCol_HeaderHovered), 5.0f);
    const double share = whole > 0 ? std::min(1.0, e.time / whole) : 0;
    draw->AddRectFilled(ImVec2(at.x, at.y + line + 6), ImVec2(at.x + static_cast<float>(share) * width, at.y + line + 8),
                        colourFor(e.address), 1.0f);
    draw->AddCircleFilled(ImVec2(at.x + 6, at.y + 2 + line * 0.5f), 4.0f, colourFor(e.address));
    const std::string label = name(e.address);
    ImGui::PushFont(ui::monoFont(), 0.0f);
    const std::string figures = percent(share);
    const float fw = ImGui::CalcTextSize(figures.c_str()).x;
    draw->AddText(ImVec2(at.x + width - fw, at.y + 2), secondary(), figures.c_str());
    ImGui::PopFont();
    ImGui::RenderTextEllipsis(draw, ImVec2(at.x + 16, at.y + 2), ImVec2(at.x + width - fw - 8, at.y + line + 2),
                              at.x + width - fw - 8, label.c_str(), nullptr, nullptr);
    if (hovered) {
      ImGui::SetTooltip("%s\n%s calls, %s", label.c_str(), count(static_cast<double>(e.calls)).c_str(),
                        formatTime(e.time).c_str());
    }
    ImGui::PopID();
    ImGui::PopID();
  }
  if (edges.size() > shown) ImGui::TextDisabled("and %zu more", edges.size() - shown);
}

void ProfilerWindow::drawCode() {
  if (!hasSelection_ || selected_ == Profiler::TOP_LEVEL) return;
  if (codeFor_ != selected_) {
    dirty_ = true; // read on the next refresh
    return;
  }
  if (code_.empty()) return;
  heading("Code");
  double hottest = 0, sum = 0;
  for (const CodeLine &l : code_) {
    hottest = std::max(hottest, l.time);
    sum += l.time;
  }
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float line = ImGui::GetTextLineHeight();
  const ImU32 heat = ui::palette().orange;
  ImGui::PushFont(ui::monoFont(), 0.0f);
  const float addressW = ImGui::CalcTextSize(wide_ ? "$00/0000" : "$0000").x;
  for (const CodeLine &l : code_) {
    ImGui::PushID(static_cast<int>(l.address));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##code", ImVec2(width, line + 3)) && showAddress) showAddress(l.address);
    const bool hovered = ImGui::IsItemHovered();
    const float t = hottest > 0 ? static_cast<float>(l.time / hottest) : 0;
    if (t > 0) draw->AddRectFilled(at, ImVec2(at.x + width, at.y + line + 2), alpha(heat, 0.05f + 0.30f * t), 3.0f);
    if (hovered) draw->AddRect(at, ImVec2(at.x + width, at.y + line + 2), alpha(heat, 0.8f), 3.0f);
    const ImU32 ink = l.executions ? ImGui::GetColorU32(ImGuiCol_Text) : ui::faintText();
    draw->AddText(ImVec2(at.x + 4, at.y + 1), l.executions ? secondary() : ui::faintText(),
                  formatAddress(l.address).c_str());
    draw->AddText(ImVec2(at.x + 4 + addressW + 10, at.y + 1), ink, l.text.c_str());
    if (l.time > 0) {
      const std::string share = percent(sum > 0 ? l.time / sum : 0);
      const float w = ImGui::CalcTextSize(share.c_str()).x;
      draw->AddText(ImVec2(at.x + width - w - 4, at.y + 1), ink, share.c_str());
    }
    if (hovered) {
      ImGui::SetTooltip("%s runs, %s\nClick to open in the CPU debugger", count(l.executions).c_str(),
                        formatTime(l.time).c_str());
    }
    ImGui::PopID();
  }
  ImGui::PopFont();
}

} // namespace a2e::native
