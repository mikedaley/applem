/*
 * disk_inspector.cpp - What is recorded on a disk, shown in a drive window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "disk_inspector.hpp"
#include "drive_ui.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace a2e::native {

using namespace drive_ui;

namespace {

// The panel, in points.
constexpr float PANEL_PADDING = 16;
constexpr float PLATTER_SIZE = 260;
constexpr float STRIP_HEIGHT = 76;
// The bytes and the nibbles, in a smaller face, so a sector's 256 bytes fit
// beside the platter.
constexpr float PANE_FONT_SCALE = 0.78f;
// How far the panes zoom, as a scale of the window's font, and by how much a
// button press or a notch of the wheel moves it.
constexpr float PANE_SCALE_MIN = PANE_FONT_SCALE;
constexpr float PANE_SCALE_MAX = 2.0f;
constexpr float PANE_SCALE_STEP = 1.15f;
// The fewest cells the strip shows, and how far the platter zooms.
constexpr double STRIP_MIN_SPAN = 16;
constexpr double MAX_PLATTER_ZOOM = 400;
// The zoomed platter reads rings in full once no more than this many show,
// and a few a frame, so the machine's lock is not held for long.
constexpr int MAX_RINGS_IN_FULL = 48;
constexpr int RINGS_PER_FRAME = 8;
// The platter's texture, in pixels: enough for a Retina display.
constexpr int PLATTER_PIXELS = 800;
// How long the head must stay on a ring before Follow head shows it. A
// program reading a disk steps across tracks, and a copy-protected one darts
// between quarter tracks for a frame or two at a time; following every step
// swapped the whole track view several times a second.
constexpr double HEAD_SETTLE_SECONDS = 0.3;
// How soon a disk being written is read again.
constexpr double REREAD_SECONDS = 0.5;

// The panes that show what is on a track are the disk's colour whatever
// the appearance, as the platter is, so the kinds' colours read the same.
constexpr ImU32 PANE_BACKGROUND = IM_COL32(0x14, 0x10, 0x0a, 255);
constexpr ImU32 PANE_TEXT = IM_COL32(0xe0, 0xdc, 0xd0, 255);
constexpr ImU32 PANE_DIM = IM_COL32(0xe0, 0xdc, 0xd0, 90);

// A nibble's colour on the dark panes: sync is the medium's own tint on the
// platter, too dark to read as text, so it is lifted here.
ImU32 paneKindColour(uint8_t kind) {
  if ((kind & inspect::KIND_MASK) == inspect::SYNC && !(kind & inspect::BAD)) return IM_COL32(0x8b, 0x94, 0x9e, 200);
  return rgbU32(kindColour(kind));
}

} // namespace

DiskInspector::~DiskInspector() {
  if (platform_.releaseTexture) {
    platform_.releaseTexture(platter_);
    platform_.releaseTexture(viewTexture_);
  }
}

// A 5.25" disk's rings are its quarter tracks; a 3.5" disk's are one side's
// tracks, two rings to each.
std::string DiskInspector::ringName(int ring) const {
  if (!disk_.threeAndAHalf) return trackLabel(ring);
  return std::to_string(ring / 2) + ", side " + std::to_string(disk_.side);
}

// ---------------------------------------------------------------------------
// Following the disk
// ---------------------------------------------------------------------------

void DiskInspector::update(const InspectedDisk &disk, const RingReader &read, double now) {
  disk_ = disk;
  if (disk.drive != lastDrive_) {
    // Another drive: its own view, starting where its head is.
    lastDrive_ = disk.drive;
    followHead_ = true;
    stripRing_ = -1;
    headSeen_ = -1;
    selectedRing_ = disk.headRing;
    fitPlatter();
  }
  if (disk.headRing != headSeen_) {
    headSeen_ = disk.headRing;
    headSeenAt_ = now;
  }
  if (followHead_ && disk.hasDisk && now - headSeenAt_ >= HEAD_SETTLE_SECONDS) selectedRing_ = headSeen_;

  if (!disk.hasDisk) {
    detail_ = TrackDetail{};
    detailDrive_ = -1;
    if (!rings_.empty()) viewStale_ = true;
    rings_.clear();
    ringsDrive_ = -1;
    return;
  }

  // The picked track, read again if it is a different one or the disk has
  // changed under it.
  const bool moved = detailDrive_ != disk.drive || detailSide_ != disk.side || detail_.quarterTrack != selectedRing_;
  const bool changed = disk.revision != detailRevision_ && now - detailAt_ >= REREAD_SECONDS;
  if (moved || changed) {
    detail_ = read(selectedRing_);
    detail_.quarterTrack = selectedRing_;
    detailDrive_ = disk.drive;
    detailSide_ = disk.side;
    detailRevision_ = disk.revision;
    detailAt_ = now;
    if (selectedSector_ >= static_cast<int>(detail_.analysis.sectors.size())) selectedSector_ = 0;
  }

  // The rings the zoomed platter shows, a few a frame.
  if (ringsDrive_ != disk.drive || ringsSide_ != disk.side ||
      (disk.revision != ringsRevision_ && now - ringsAt_ >= REREAD_SECONDS)) {
    rings_.clear();
    ringsDrive_ = disk.drive;
    ringsSide_ = disk.side;
    ringsRevision_ = disk.revision;
    ringsAt_ = now;
    viewStale_ = true;
  }
  if (rings_.size() > 2 * MAX_RINGS_IN_FULL) {
    for (auto it = rings_.begin(); it != rings_.end();) {
      if (std::find(wantedRings_.begin(), wantedRings_.end(), it->first) == wantedRings_.end()) it = rings_.erase(it);
      else ++it;
    }
  }
  int count = 0;
  for (int ring : wantedRings_) {
    if (rings_.count(ring)) continue;
    if (count++ == RINGS_PER_FRAME) break;
    TrackDetail track = read(ring);
    track.quarterTrack = ring;
    rings_.emplace(ring, makeRing(std::move(track)));
    viewStale_ = true;
  }
}

// The platter, painted once for each new overview or mode and drawn turned.
void DiskInspector::paintPlatterTexture() {
  const bool stale = !platterPainted_ || platterDrive_ != disk_.drive || platterSide_ != disk_.side ||
                     platterSerial_ != disk_.overviewSerial || platterMode_ != mode_;
  if (!stale || !platform_.makeTexture) return;
  if (platform_.releaseTexture) platform_.releaseTexture(platter_);
  platter_ = ImTextureID_Invalid;
  platterPainted_ = true;
  platterDrive_ = disk_.drive;
  platterSide_ = disk_.side;
  platterSerial_ = disk_.overviewSerial;
  platterMode_ = mode_;
  viewStale_ = true;
  if (!disk_.hasDisk) return;
  std::vector<uint8_t> rgba;
  paintPlatter(rgba, PLATTER_PIXELS, disk_.overview, mode_);
  platter_ = platform_.makeTexture(rgba.data(), PLATTER_PIXELS, PLATTER_PIXELS);
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

// The disk, large. At full size it turns with the real one, with the head
// fixed at twelve o'clock. Zoomed in (scroll, about the pointer) it holds
// still and the head goes round it, since a view at fifty times turning
// five times a second shows nothing; a drag pans and a double click shows
// the whole disk again. Once few enough rings show, each is read in full
// and drawn cell by cell, with its flux transitions and, closer still, the
// nibbles' values along it. Hover for what is under the pointer; a click
// picks the track.
void DiskInspector::drawPlatter(ImVec2 origin, float size) {
  const InspectedDisk &d = disk_;
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float radius = size * 0.5f - 2;
  const ImVec2 centre(origin.x + size * 0.5f, origin.y + size * 0.5f);
  ImGui::SetCursorScreenPos(origin);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##platter", ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();

  if (!d.hasDisk || platter_ == ImTextureID_Invalid) {
    ghostDisk(draw, centre, radius);
    const std::string empty = "No disk in " + d.title;
    centredText(draw, centre, secondary(), empty.c_str());
    return;
  }

  const ImGuiIO &io = ImGui::GetIO();
  const ImVec2 viewMin(centre.x - radius, centre.y - radius);
  const ImVec2 viewMax(centre.x + radius, centre.y + radius);

  // Zoom about a point on the screen, keeping the disk under it still.
  auto zoomAt = [&](ImVec2 at, double factor) {
    const double z0 = view_.zoom;
    const double z1 = std::clamp(z0 * factor, 1.0, MAX_PLATTER_ZOOM);
    if (z0 <= 1.001 && z1 > 1.001) view_.turn = d.spin; // hold the disk where it is
    const double px = (at.x - centre.x) / radius;
    const double py = (at.y - centre.y) / radius;
    view_.cx += px / z0 - px / z1;
    view_.cy += py / z0 - py / z1;
    view_.zoom = z1;
    viewStale_ = true;
  };
  if (hovered) {
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if (io.MouseWheel != 0) zoomAt(io.MousePos, std::pow(1.2, io.MouseWheel));
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fitPlatter();
  }
  const bool dragging = active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f);
  if (dragging && view_.zoom > 1.001) {
    view_.cx -= io.MouseDelta.x / (radius * view_.zoom);
    view_.cy -= io.MouseDelta.y / (radius * view_.zoom);
    viewStale_ = true;
  }
  if (view_.zoom <= 1.001) {
    view_ = PlatterView{};
  } else {
    const double limit = 1.0 - 1.0 / view_.zoom;
    view_.cx = std::clamp(view_.cx, -limit, limit);
    view_.cy = std::clamp(view_.cy, -limit, limit);
  }
  const bool zoomed = view_.zoom > 1.001;
  const double zoom = view_.zoom;
  const double scale = radius * zoom; // screen points per disk unit
  auto toScreen = [&](double x, double y) {
    return ImVec2(static_cast<float>(centre.x + (x - view_.cx) * scale), static_cast<float>(centre.y + (y - view_.cy) * scale));
  };
  // The disk's angle, as a fraction of a turn, at twelve o'clock.
  const double turn = zoomed ? view_.turn : d.spin;

  const bool dark = ui::isDark();
  if (!zoomed) {
    draw->AddCircleFilled(ImVec2(centre.x, centre.y + 8), radius * 0.98f, IM_COL32(0, 0, 0, dark ? 70 : 26), 96);
    draw->AddCircleFilled(ImVec2(centre.x, centre.y + 3), radius * 0.99f, IM_COL32(0, 0, 0, dark ? 90 : 34), 96);
    drawTurned(draw, platter_, centre, radius, static_cast<float>(-turn * 2.0 * M_PI));
    sheen(draw, centre, radius * platter::BAND_OUTER, radius * platter::BAND_INNER);
  } else {
    // Which rings show, and read them in full once there are few enough.
    double nearest = std::hypot(std::max(0.0, std::abs(view_.cx) - 1.0 / zoom), std::max(0.0, std::abs(view_.cy) - 1.0 / zoom));
    double farthest = 0;
    for (int corner = 0; corner < 4; corner++) {
      const double x = view_.cx + (corner & 1 ? 1 : -1) / zoom;
      const double y = view_.cy + (corner & 2 ? 1 : -1) / zoom;
      farthest = std::max(farthest, std::hypot(x, y));
    }
    const int outer = std::max(0, static_cast<int>((platter::BAND_OUTER - farthest) / platter::RING_WIDTH));
    const int inner = std::min(platter::QUARTER_TRACKS - 1,
                               static_cast<int>((platter::BAND_OUTER - nearest) / platter::RING_WIDTH));
    wantedRings_.clear();
    if (nearest <= platter::BAND_OUTER && farthest >= platter::BAND_INNER && inner - outer + 1 <= MAX_RINGS_IN_FULL) {
      for (int qt = outer; qt <= inner; qt++) wantedRings_.push_back(qt);
    }
    // Painted afresh as the view moves: at half resolution while it is
    // being dragged, and in full once it settles.
    const int pixels = dragging || io.MouseWheel != 0 ? PLATTER_PIXELS / 2 : PLATTER_PIXELS;
    if (viewStale_ || viewPixels_ != pixels) paintView(pixels);
    draw->AddRectFilled(viewMin, viewMax, IM_COL32(0x0c, 0x0a, 0x06, 255), 12.0f);
    if (viewTexture_ != ImTextureID_Invalid) {
      draw->AddImageRounded(ImTextureRef(viewTexture_), viewMin, viewMax, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 12.0f);
    }
    border(draw, viewMin, viewMax, 12.0f);
  }
  draw->PushClipRect(viewMin, viewMax, true);

  // A ring marked, for the track picked and the one under the pointer, while
  // rings are too thin to tell apart. Once a ring is wide it is plain to see
  // and the tooltip names it, and lines along its edges only got in the way
  // of what is recorded there.
  const ImVec2 hub = toScreen(0, 0);
  const float ringPixels = static_cast<float>(platter::RING_WIDTH * scale);
  auto outline = [&](int qt, ImU32 colour) {
    if (ringPixels >= 6) return;
    const float r = static_cast<float>(platter::radiusOf(qt) * scale);
    draw->AddCircle(hub, r, colour, 0, std::max(1.5f, ringPixels + 0.5f));
  };
  if (detail_.quarterTrack >= 0) outline(detail_.quarterTrack, accent(0.95f));

  // The nibbles' values along each ring, once there is room to write them.
  if (zoomed && ringPixels >= 11) {
    ImGui::PushFont(ui::monoFont(), std::min(15.0f, ringPixels * 0.62f));
    const float hexWidth = ImGui::CalcTextSize("FF").x;
    for (int qt : wantedRings_) {
      auto it = rings_.find(qt);
      if (it == rings_.end() || !it->second.track.present) continue;
      const auto &a = it->second.track.analysis;
      const double rMid = platter::radiusOf(qt);
      const double arcPerCell = 2 * M_PI * rMid * scale / a.bit_count;
      if (arcPerCell * 8 < hexWidth + 4) continue;
      for (const inspect::Nibble &n : a.nibbles) {
        if (arcPerCell * n.cells < hexWidth + 4) continue;
        const double angle = ((n.start_bit + n.cells * 0.5) / a.bit_count - turn) * 2 * M_PI;
        const ImVec2 at = toScreen(std::sin(angle) * rMid, -std::cos(angle) * rMid);
        if (at.x < viewMin.x - 20 || at.x > viewMax.x + 20 || at.y < viewMin.y - 20 || at.y > viewMax.y + 20) continue;
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02X", n.value);
        const bool quiet = (n.kind & inspect::KIND_MASK) == inspect::SYNC || (n.kind & inspect::KIND_MASK) == inspect::INVALID;
        centredText(draw, at, quiet ? IM_COL32(255, 255, 255, 170) : IM_COL32(0, 0, 0, 200), hex);
      }
    }
    ImGui::PopFont();
  }

  // Under the pointer: which quarter track, and what is on it there.
  if (hovered && !dragging) {
    const double x = (io.MousePos.x - centre.x) / scale + view_.cx;
    const double y = (io.MousePos.y - centre.y) / scale + view_.cy;
    const int qt = platter::quarterTrackAt(static_cast<float>(std::hypot(x, y)));
    if (qt >= 0 && d.overview) {
      outline(qt, IM_COL32(255, 255, 255, 110));
      double a = std::atan2(x, -y) / (2 * M_PI) + turn;
      a -= std::floor(a);
      const OverviewTrack &t = d.overview->tracks[qt];
      std::string tip = "Track " + ringName(qt);
      auto ring = rings_.find(qt);
      if (ring != rings_.end() && ring->second.track.present) {
        const auto &analysis = ring->second.track.analysis;
        const uint32_t cell = std::min(analysis.bit_count - 1, static_cast<uint32_t>(a * analysis.bit_count));
        const int i = nibbleAtCell(analysis.nibbles, cell);
        if (i >= 0) {
          const inspect::Nibble &n = analysis.nibbles[i];
          char line[64];
          std::snprintf(line, sizeof(line), "\nNibble %d  $%02X", i, n.value);
          tip += line + std::string("\n") + kindName(n.kind);
          if (n.sector != inspect::NO_SECTOR && n.sector < analysis.sectors.size()) {
            tip += ", sector " + std::to_string(analysis.sectors[n.sector].sector);
          }
          tip += "\nCell " + std::to_string(cell) + " of " + std::to_string(analysis.bit_count);
        }
      } else if (t.present && d.overview->buckets > 0) {
        const int b = std::min(d.overview->buckets - 1, static_cast<int>(a * d.overview->buckets));
        tip += "\n" + kindName(t.kinds[b]);
        if (t.sectors[b] != inspect::NO_SECTOR) tip += ", sector " + std::to_string(t.sectors[b]);
        if (t.flux && t.times[b]) {
          char time[48];
          std::snprintf(time, sizeof(time), "\nCells %.2f\xC2\xB5s", t.times[b] / 32.0);
          tip += time;
        }
        tip += "\n" + std::to_string(t.sectorsFound) + " sectors, " + std::to_string(t.sectorsGood) + " good";
      } else {
        tip += "\nNothing recorded";
      }
      ImGui::SetTooltip("%s", tip.c_str());
      if (ImGui::IsItemDeactivated() && io.MouseDragMaxDistanceSqr[0] < 9.0f) {
        selectedRing_ = qt;
        followHead_ = false;
      }
    }
  }

  // The head. At full size it sits at twelve o'clock on its arm; zoomed, it
  // goes round the still disk at the angle under it now.
  const double headRadius = platter::radiusOf(d.headRing);
  if (!zoomed) {
    const ImVec2 head(centre.x, centre.y - static_cast<float>(headRadius * radius));
    draw->AddLine(ImVec2(centre.x, centre.y - radius - 6), head, dark ? IM_COL32(255, 255, 255, 60) : IM_COL32(0, 0, 0, 70), 4.0f);
    if (d.active) glow(draw, head, 4.0f, headColour(true, d.writing));
    draw->AddRectFilled(ImVec2(head.x - 7, head.y - 4), ImVec2(head.x + 7, head.y + 4), headColour(d.active, d.writing), 2.5f);
    draw->AddRect(ImVec2(head.x - 7, head.y - 4), ImVec2(head.x + 7, head.y + 4), IM_COL32(0, 0, 0, 120), 2.5f);
  } else {
    const double angle = (d.spin - turn) * 2 * M_PI;
    const ImVec2 head = toScreen(std::sin(angle) * headRadius, -std::cos(angle) * headRadius);
    const float size = std::clamp(ringPixels * 0.45f, 4.0f, 9.0f);
    if (d.active) glow(draw, head, size, headColour(true, d.writing));
    draw->AddCircleFilled(head, size, headColour(d.active, d.writing));
    draw->AddCircle(head, size, IM_COL32(0, 0, 0, 140), 0, 1.5f);
  }
  draw->PopClipRect();

  // Zoom controls, over the bottom right corner.
  const float button = ImGui::GetFrameHeight();
  const float controlsWidth = button * 2 + 52 + 44 + 12;
  ImGui::SetCursorScreenPos(ImVec2(viewMax.x - controlsWidth - 6, viewMax.y - button - 6));
  ImGui::PushID("platterzoom");
  if (ui::Button("-", ImVec2(button, 0))) zoomAt(centre, 1 / 1.5);
  ImGui::SameLine(0, 4);
  char level[16];
  std::snprintf(level, sizeof(level), zoom < 10 ? "%.1fx" : "%.0fx", zoom);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  draw->AddRectFilled(at, ImVec2(at.x + 52, at.y + button), withAlpha(ImGui::GetColorU32(ImGuiCol_WindowBg), 0.85f), button * 0.5f);
  centredText(draw, ImVec2(at.x + 26, at.y + button * 0.5f), text(), level);
  ImGui::Dummy(ImVec2(52, button));
  ImGui::SameLine(0, 4);
  if (ui::Button("+", ImVec2(button, 0))) zoomAt(centre, 1.5);
  ImGui::SameLine(0, 4);
  ImGui::BeginDisabled(!zoomed);
  if (ui::Button("Fit", ImVec2(44, 0))) fitPlatter();
  ImGui::EndDisabled();
  ImGui::PopID();
}

void DiskInspector::drawLegend(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 start = ImGui::GetCursorScreenPos();
  struct Entry {
    uint32_t rgb;
    const char *label;
  };
  std::vector<Entry> entries;
  if (mode_ == PlatterMode::Timing) {
    entries = {{timeColour(static_cast<uint8_t>(NOMINAL_CELL_TIME * 0.94)), "Fast cells"},
               {timeColour(static_cast<uint8_t>(NOMINAL_CELL_TIME + 0.5)), disk_.threeAndAHalf ? "Nominal 1.96\xC2\xB5s" : "Nominal 3.91\xC2\xB5s"},
               {timeColour(static_cast<uint8_t>(NOMINAL_CELL_TIME * 1.06)), "Slow cells"}};
  } else {
    entries = {{kindColour(inspect::SYNC), "Sync"},          {kindColour(inspect::ADDR_PROLOGUE), "Address marks"},
               {kindColour(inspect::ADDR), "Address"},       {kindColour(inspect::DATA_PROLOGUE), "Data marks"},
               {kindColour(inspect::DATA), "Data"},          {kindColour(inspect::DATA | inspect::BAD), "Bad checksum"},
               {kindColour(inspect::OTHER), "Unknown"}, {kindColour(inspect::INVALID), "Noise"}};
  }
  float x = start.x;
  float y = start.y;
  const float line = ImGui::GetTextLineHeight();
  for (const Entry &e : entries) {
    const float w = 14 + ImGui::CalcTextSize(e.label).x + 14;
    if (x + w > start.x + width) {
      x = start.x;
      y += line + 3;
    }
    draw->AddRectFilled(ImVec2(x, y + line * 0.5f - 4), ImVec2(x + 9, y + line * 0.5f + 5), rgbU32(e.rgb), 2.5f);
    draw->AddText(ImVec2(x + 14, y), secondary(), e.label);
    x += w;
  }
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(width, y - start.y + line));
}

// The quarter track unrolled, start of the track at the left: every nibble
// its kind's colour, the sectors named over their address fields, the bits
// themselves once there is room for them, a flux track's cell times along
// the bottom, and the head where it is now. Scroll zooms about the pointer,
// a drag pans, a double click shows the whole track, and a click on a
// sector's nibbles picks the sector.
void DiskInspector::drawStrip(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + width, p0.y + STRIP_HEIGHT);
  ImGui::InvisibleButton("##strip", ImVec2(width, STRIP_HEIGHT));
  const bool hovered = ImGui::IsItemHovered();
  draw->AddRectFilled(p0, p1, PANE_BACKGROUND, 8.0f);

  const inspect::TrackAnalysis &a = detail_.analysis;
  const auto &nibbles = a.nibbles;
  const double cells = a.bit_count;
  if (!detail_.present || cells == 0 || nibbles.empty()) {
    centredText(draw, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), PANE_DIM, "Nothing recorded on this quarter track");
    border(draw, p0, p1, 8.0f);
    return;
  }
  // The zoom carries from track to track, so following the head while a
  // disk loads keeps the view it was given; a new drive starts whole.
  if (stripRing_ < 0 || stripSpan_ <= 0 || stripWhole_) {
    stripStart_ = 0;
    stripSpan_ = cells;
  }
  stripSpan_ = std::min(stripSpan_, cells);
  stripRing_ = detail_.quarterTrack;

  const ImGuiIO &io = ImGui::GetIO();
  if (hovered) {
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if (io.MouseWheel != 0) {
      const double at = (io.MousePos.x - p0.x) / width;
      const double cell = stripStart_ + at * stripSpan_;
      stripSpan_ = std::clamp(stripSpan_ * std::pow(0.8, io.MouseWheel), std::min(STRIP_MIN_SPAN, cells), cells);
      stripStart_ = cell - at * stripSpan_;
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      stripStart_ = 0;
      stripSpan_ = cells;
    }
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
    stripStart_ -= io.MouseDelta.x / width * stripSpan_;
  }
  stripStart_ = std::clamp(stripStart_, 0.0, cells - stripSpan_);
  stripWhole_ = stripSpan_ >= cells;

  const double perCell = width / stripSpan_;
  auto X = [&](double cell) { return static_cast<float>(p0.x + (cell - stripStart_) * perCell); };
  const float labelTop = p0.y + 5;
  const float bandTop = p0.y + 22;
  const float bandBottom = p1.y - 16;
  const float timeTop = p1.y - 11;
  draw->PushClipRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), true);

  // The nibbles. Each is its kind's colour across the band; once there is
  // room its value is written in the top half, and when a cell is wide
  // enough the bottom half shows the cells themselves: a tick for each flux
  // transition, and closer still a 1 or a 0 in every cell.
  int first = nibbleAtCell(nibbles, static_cast<uint32_t>(stripStart_));
  if (nibbles[first].start_bit > stripStart_) first = 0;
  const bool showCells = perCell >= 2.5;
  const bool showDigits = perCell >= 11;
  const float split = showCells ? bandTop + (bandBottom - bandTop) * 0.5f : bandBottom;
  const float valueSize = std::clamp(static_cast<float>(perCell) * 1.1f, ImGui::GetFontSize() * ui::SMALL_TEXT, 18.0f);
  ImGui::PushFont(ui::monoFont(), valueSize);
  const float hexWidth = ImGui::CalcTextSize("FF").x;
  ImGui::PopFont();
  for (size_t i = first; i < nibbles.size(); i++) {
    const inspect::Nibble &n = nibbles[i];
    if (n.start_bit > stripStart_ + stripSpan_) break;
    const float x0 = X(n.start_bit);
    const float x1 = X(n.start_bit + n.cells);
    const uint8_t kind = n.kind;
    const uint8_t k = kind & inspect::KIND_MASK;
    const bool noise = k == inspect::INVALID && !(kind & inspect::BAD);
    const bool quiet = noise || (k == inspect::SYNC && !(kind & inspect::BAD));
    const ImU32 colour = noise ? IM_COL32(0x8b, 0x94, 0x9e, 90) : rgbU32(kindColour(kind));
    const float gap = x1 - x0 >= 6 ? 1.0f : 0.0f;
    const float right = std::max(x0 + 1, x1 - gap);
    draw->AddRectFilled(ImVec2(x0, bandTop), ImVec2(right, bandBottom), colour, x1 - x0 >= 6 ? 2.0f : 0.0f);
    const ImU32 ink = quiet ? IM_COL32(255, 255, 255, 190) : IM_COL32(0, 0, 0, 200);

    if (x1 - x0 >= hexWidth + 6) {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%02X", n.value);
      ImGui::PushFont(ui::monoFont(), valueSize);
      centredText(draw, ImVec2((x0 + x1) * 0.5f, (bandTop + split) * 0.5f), ink, hex);
      ImGui::PopFont();
      // And what it is, when the nibble is wide enough to say.
      if (x1 - x0 >= 230) {
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
        const std::string name = kindName(kind);
        draw->AddText(ImVec2(std::max(x0, p0.x) + 6, bandTop + 3), withAlpha(ink, 0.75f), name.c_str());
        ImGui::PopFont();
      }
    }
    if (showCells) {
      draw->AddRectFilled(ImVec2(x0, split), ImVec2(right, bandBottom), IM_COL32(0, 0, 0, 95));
      ImGui::PushFont(ui::monoFont(), std::min(15.0f, static_cast<float>(perCell) * 0.8f));
      for (uint32_t c = 0; c < n.cells; c++) {
        const uint32_t cell = (n.start_bit + c) % a.bit_count;
        const float xc = X(n.start_bit + c + 0.5);
        if (xc < p0.x - perCell || xc > p1.x + perCell) continue;
        const bool one = cellBit(detail_.bits, cell);
        if (showDigits) {
          if (c > 0) {
            const float xs = X(n.start_bit + c);
            draw->AddLine(ImVec2(xs, split + 3), ImVec2(xs, bandBottom - 3), IM_COL32(255, 255, 255, 30), 1.0f);
          }
          centredText(draw, ImVec2(xc, (split + bandBottom) * 0.5f),
                      one ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 80), one ? "1" : "0");
        } else if (one) {
          draw->AddLine(ImVec2(xc, split + 3), ImVec2(xc, bandBottom - 3), IM_COL32(255, 255, 255, 200), 1.0f);
        }
      }
      ImGui::PopFont();
    }
  }

  // The sectors, named over their address fields.
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  for (size_t s = 0; s < a.sectors.size(); s++) {
    const inspect::Sector &sector = a.sectors[s];
    if (sector.address_nibble >= nibbles.size()) continue;
    const float x = X(nibbles[sector.address_nibble].start_bit);
    if (x < p0.x - 40 || x > p1.x) continue;
    const bool picked = static_cast<int>(s) == selectedSector_;
    draw->AddLine(ImVec2(x, labelTop + 2), ImVec2(x, bandTop - 2), picked ? accent() : PANE_DIM, 1.0f);
    char name[8];
    std::snprintf(name, sizeof(name), "S%d", sector.sector);
    draw->AddText(ImVec2(x + 3, labelTop), picked ? accent() : PANE_TEXT, name);
  }
  ImGui::PopFont();

  // A flux track's cell times.
  if (!detail_.cellTime.empty()) {
    for (float x = p0.x; x < p1.x; x += 1.0f) {
      const size_t cell = static_cast<size_t>(stripStart_ + (x - p0.x + 0.5) / perCell);
      if (cell >= detail_.cellTime.size()) break;
      const uint32_t rgb = timeColour(detail_.cellTime[cell]);
      if (rgb) draw->AddRectFilled(ImVec2(x, timeTop), ImVec2(x + 1, p1.y - 5), rgbU32(rgb));
    }
  }

  // The head, if it is over this quarter track.
  const InspectedDisk &d = disk_;
  if (d.headRing == detail_.quarterTrack) {
    const float x = X(d.spin * cells);
    // While the disk turns, a fading trail behind the head, so a mark that
    // crosses the whole track five times a second reads as a sweep rather
    // than a line jumping about.
    if (d.spinSpeed > 0.5) {
      const float trail = static_cast<float>(cells * 0.06 * perCell);
      const float from = std::max(p0.x, x - trail);
      if (x > p0.x && from < x) {
        const ImU32 colour = d.active ? headColour(true, d.writing) : IM_COL32(255, 255, 255, 255);
        draw->AddRectFilledMultiColor(ImVec2(from, p0.y + 2), ImVec2(std::min(x, p1.x), p1.y - 2), withAlpha(colour, 0.0f),
                                      withAlpha(colour, 0.30f), withAlpha(colour, 0.30f), withAlpha(colour, 0.0f));
      }
    }
    if (x >= p0.x && x <= p1.x) {
      const ImU32 colour = d.active ? headColour(true, d.writing) : IM_COL32(255, 255, 255, 200);
      draw->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), colour, 1.5f);
      draw->AddTriangleFilled(ImVec2(x - 4, p0.y + 1), ImVec2(x + 4, p0.y + 1), ImVec2(x, p0.y + 6), colour);
    }
  }
  draw->PopClipRect();
  border(draw, p0, p1, 8.0f);

  // What is under the pointer, and a click to pick its sector.
  if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
    const double cell = stripStart_ + (io.MousePos.x - p0.x) / perCell;
    const int i = nibbleAtCell(nibbles, static_cast<uint32_t>(std::max(0.0, cell)));
    if (i >= 0) {
      const inspect::Nibble &n = nibbles[i];
      std::string tip = "Nibble " + std::to_string(i) + "  $";
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%02X", n.value);
      tip += hex;
      tip += "\n" + kindName(n.kind);
      if (n.sector != inspect::NO_SECTOR && n.sector < a.sectors.size()) {
        tip += ", sector " + std::to_string(a.sectors[n.sector].sector);
      }
      tip += "\nCell " + std::to_string(static_cast<uint32_t>(cell)) + " of " + std::to_string(a.bit_count);
      if (!detail_.cellTime.empty() && static_cast<size_t>(cell) < detail_.cellTime.size()) {
        char time[32];
        std::snprintf(time, sizeof(time), ", %.2f\xC2\xB5s", detail_.cellTime[static_cast<size_t>(cell)] / 32.0);
        tip += time;
      }
      ImGui::SetTooltip("%s", tip.c_str());
      if (ImGui::IsItemDeactivated() && io.MouseDragMaxDistanceSqr[0] < 9.0f && n.sector != inspect::NO_SECTOR) {
        selectedSector_ = n.sector;
      }
    }
  }

  // Zoom controls, and how much of the track shows.
  {
    const float button = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 6));
    ImGui::PushID("stripzoom");
    auto zoomStrip = [&](double factor) {
      const double middle = stripStart_ + stripSpan_ * 0.5;
      stripSpan_ = std::clamp(stripSpan_ * factor, std::min(STRIP_MIN_SPAN, cells), cells);
      stripStart_ = std::clamp(middle - stripSpan_ * 0.5, 0.0, cells - stripSpan_);
      stripWhole_ = stripSpan_ >= cells;
    };
    if (ui::Button("-", ImVec2(button, 0))) zoomStrip(2.0);
    ImGui::SameLine(0, 4);
    if (ui::Button("+", ImVec2(button, 0))) zoomStrip(0.5);
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(stripSpan_ >= cells);
    if (ui::Button("Whole Track")) {
      stripStart_ = 0;
      stripSpan_ = cells;
      stripWhole_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 12);
    char shown[64];
    std::snprintf(shown, sizeof(shown), "%.0fx  ·  cells %.0f–%.0f", cells / stripSpan_, stripStart_,
                  stripStart_ + stripSpan_);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", shown);
    // And the controller, as the inspected drive sees it, at the right.
    if (!disk_.status.empty()) {
      const float statusWidth = ImGui::CalcTextSize(disk_.status.c_str()).x;
      ImGui::SameLine(width - statusWidth);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(disk_.active ? headColour(true, disk_.writing) : secondary()),
                         "%s", disk_.status.c_str());
    }
    ImGui::PopFont();
    ImGui::PopID();
  }
}

// The sectors in the order they pass the head, each its logical number,
// coloured by how its fields read.
void DiskInspector::drawSectorChips(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const auto &sectors = detail_.analysis.sectors;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  if (sectors.empty()) {
    draw->AddText(start, secondary(), detail_.present ? "No standard sectors on this track" : "");
    ImGui::Dummy(ImVec2(width, ImGui::GetFrameHeight()));
    return;
  }
  const float w = 27.0f;
  const float h = ImGui::GetFrameHeight();
  float x = start.x;
  float y = start.y;
  ImGui::PushFont(ui::monoFont(), 0.0f);
  for (size_t i = 0; i < sectors.size(); i++) {
    const inspect::Sector &s = sectors[i];
    if (x + w > start.x + width) {
      x = start.x;
      y += h + 4;
    }
    ImU32 colour = IM_COL32(97, 187, 70, 255);
    const char *state = "Data good";
    switch (s.data) {
    case inspect::DataState::Good: break;
    case inspect::DataState::Bad: colour = IM_COL32(224, 58, 62, 255); state = "Data checksum failed"; break;
    case inspect::DataState::None: colour = IM_COL32(253, 184, 39, 255); state = "No data field"; break;
    case inspect::DataState::Unverified: colour = IM_COL32(139, 148, 158, 255); state = "Data not verified"; break;
    }
    if (!s.address_ok) colour = IM_COL32(224, 58, 62, 255);
    const ImVec2 a(x, y);
    const ImVec2 b(x + w, y + h);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::InvisibleButton("##sector", ImVec2(w, h))) selectedSector_ = static_cast<int>(i);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const bool picked = static_cast<int>(i) == selectedSector_;
    draw->AddRectFilled(a, b, withAlpha(colour, picked ? 0.85f : hovered ? 0.30f : 0.18f), 6.0f);
    if (picked) draw->AddRect(a, b, accent(), 6.0f, 0, 1.5f);
    char label[8];
    std::snprintf(label, sizeof(label), "%X", s.sector);
    centredText(draw, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), picked ? ui::textOn(colour) : colour, label);
    if (hovered) {
      ImGui::SetTooltip(disk_.threeAndAHalf ? "Sector %d, %d%s past the index\nSide %d, track %d\n%s%s"
                                             : "Sector %d, %d%s past the index\nVolume %d, track %d\n%s%s",
                        s.sector, static_cast<int>(i) + 1,
                        i == 0 ? "st" : i == 1 ? "nd" : i == 2 ? "rd" : "th", s.volume, s.track, state,
                        s.address_ok ? "" : "\nAddress checksum failed");
    }
    x += w + 4;
  }
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, y - start.y + h));
}

// The picked sector's 256 bytes, sixteen to a row with their characters (the
// first half of a 3.5" block, which is all the analyser keeps).
void DiskInspector::drawSectorBytes(float width, float height) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + width, p0.y + height);
  draw->AddRectFilled(p0, p1, PANE_BACKGROUND, 8.0f);
  border(draw, p0, p1, 8.0f);
  const auto &sectors = detail_.analysis.sectors;
  if (sectors.empty() || selectedSector_ < 0 || selectedSector_ >= static_cast<int>(sectors.size())) {
    centredText(draw, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), PANE_DIM, "No sector to show");
    ImGui::Dummy(ImVec2(width, height));
    return;
  }
  const inspect::Sector &s = sectors[selectedSector_];
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * paneScale_);
  const float cw = ImGui::CalcTextSize("0").x;
  const float lh = ImGui::GetTextLineHeight() + 1;

  // Which sector, held at the top.
  char head[96];
  const char *state = s.data == inspect::DataState::Good  ? "data good"
                      : s.data == inspect::DataState::Bad ? "data checksum failed"
                      : s.data == inspect::DataState::None ? "no data field"
                                                           : "data not verified";
  std::snprintf(head, sizeof(head),
                disk_.threeAndAHalf ? "SECTOR %d  SIDE %d  TRACK %d  %s  (FIRST 256 OF 512)" : "SECTOR %d  VOLUME %d  TRACK %d  %s",
                s.sector, s.volume, s.track, state);
  draw->PushClipRect(p0, p1, true);
  draw->AddText(ImVec2(p0.x + 12, p0.y + 8), s.data == inspect::DataState::Good ? IM_COL32(97, 187, 70, 255) : IM_COL32(253, 184, 39, 255), head);
  draw->PopClipRect();
  const float headHeight = 8 + lh + 4;

  // The bytes under it, scrolling within the pane when they do not fit.
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 1, p0.y + headHeight));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(11, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
  ImGui::BeginChild("##bytes", ImVec2(width - 2, height - headHeight - 6), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_HorizontalScrollbar);
  zoomPaneFromWheel();
  if (s.data != inspect::DataState::None) {
    ImDrawList *rows = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(cw * 70, lh * 16));
    for (int row = 0; row < 16; row++) {
      const float y = origin.y + row * lh;
      float x = origin.x;
      char offset[8];
      std::snprintf(offset, sizeof(offset), "%02X", row * 16);
      rows->AddText(ImVec2(x, y), PANE_DIM, offset);
      x += cw * 4;
      for (int col = 0; col < 16; col++) {
        const uint8_t byte = s.bytes[row * 16 + col];
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02X", byte);
        rows->AddText(ImVec2(x, y), byte == 0 ? PANE_DIM : PANE_TEXT, hex);
        x += cw * 3 + (col == 7 ? cw : 0);
      }
      x += cw;
      for (int col = 0; col < 16; col++) {
        // As the Apple II shows it: the high bit set is normal text.
        const uint8_t c = s.bytes[row * 16 + col] & 0x7F;
        const char glyph[2] = {c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.', 0};
        rows->AddText(ImVec2(x, y), c >= 0x20 && c < 0x7F ? IM_COL32(0, 157, 220, 255) : PANE_DIM, glyph);
        x += cw;
      }
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(width, height));
}

// Every nibble round the track, sixteen to a row in their kinds' colours,
// the picked sector's own marked.
void DiskInspector::drawNibbles(float width, float height) {
  const auto &nibbles = detail_.analysis.nibbles;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(PANE_BACKGROUND));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
  ImGui::BeginChild("##nibbles", ImVec2(width, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_HorizontalScrollbar);
  zoomPaneFromWheel();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * paneScale_);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float cw = ImGui::CalcTextSize("0").x;
  const float lh = ImGui::GetTextLineHeight() + 1;
  const int rows = static_cast<int>((nibbles.size() + 15) / 16);
  ImGuiListClipper clipper;
  clipper.Begin(rows, lh);
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
      ImGui::SetCursorPosY(10 + row * lh);
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(cw * 54, lh));
      char offset[8];
      std::snprintf(offset, sizeof(offset), "%04X", row * 16);
      draw->AddText(at, PANE_DIM, offset);
      float x = at.x + cw * 6;
      for (int col = 0; col < 16; col++) {
        const size_t i = static_cast<size_t>(row) * 16 + col;
        if (i >= nibbles.size()) break;
        const inspect::Nibble &n = nibbles[i];
        if (n.sector != inspect::NO_SECTOR && static_cast<int>(n.sector) == selectedSector_) {
          draw->AddRectFilled(ImVec2(x - 2, at.y), ImVec2(x + cw * 2 + 2, at.y + lh - 1), IM_COL32(255, 255, 255, 24), 3.0f);
        }
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02X", n.value);
        draw->AddText(ImVec2(x, at.y), paneKindColour(n.kind), hex);
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(x - 2, at.y), ImVec2(x + cw * 2 + 2, at.y + lh))) {
          ImGui::SetTooltip("Nibble %zu\n%s", i, kindName(n.kind).c_str());
        }
        x += cw * 3;
      }
    }
  }
  if (nibbles.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(PANE_DIM), "Nothing recorded on this quarter track");
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

void DiskInspector::fitPlatter() {
  view_ = PlatterView{};
  wantedRings_.clear();
  viewStale_ = true;
}

// The zoomed view, painted afresh, with the rings read in full in it.
void DiskInspector::paintView(int pixels) {
  if (!platform_.makeTexture) return;
  const InspectedDisk &d = disk_;
  PlatterRings rings{};
  for (const auto &[qt, ring] : rings_) {
    if (qt >= 0 && qt < platter::QUARTER_TRACKS) rings[qt] = &ring;
  }
  std::vector<uint8_t> rgba;
  paintPlatter(rgba, pixels, d.overview, mode_, view_, &rings);
  if (platform_.releaseTexture) platform_.releaseTexture(viewTexture_);
  viewTexture_ = platform_.makeTexture(rgba.data(), pixels, pixels);
  viewPixels_ = pixels;
  viewStale_ = false;
}


// The picked track: its name and what was found on it on one line, a way
// back to the head at its end, and its sectors in the order they pass it.
void DiskInspector::drawTrackSummary(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float lineHeight = ImGui::GetFrameHeight();
  ImGui::PushFont(ui::monoFont(), 16.0f);
  const std::string title = "Track " + ringName(selectedRing_);
  const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
  draw->AddText(ImVec2(start.x, start.y + (lineHeight - titleSize.y) * 0.5f), text(), title.c_str());
  ImGui::PopFont();
  const auto &a = detail_.analysis;
  std::string meta;
  if (!detail_.present) {
    meta = "Nothing recorded here";
  } else {
    int good = 0;
    for (const auto &s : a.sectors) good += s.data == inspect::DataState::Good;
    meta = std::to_string(a.sectors.size()) + " sectors  ·  " + std::to_string(good) + " good  ·  " +
           std::to_string(a.nibbles.size()) + " nibbles  ·  " + std::to_string(a.bit_count) + " cells";
    if (detail_.flux) meta += "  ·  flux";
  }
  const float goWidth = followHead_ ? 0.0f : 100.0f;
  const float metaX = start.x + titleSize.x + 12;
  draw->PushClipRect(ImVec2(metaX, start.y), ImVec2(start.x + width - goWidth - 8, start.y + lineHeight), true);
  draw->AddText(ImVec2(metaX, start.y + (lineHeight - ImGui::GetTextLineHeight()) * 0.5f), secondary(), meta.c_str());
  draw->PopClipRect();
  if (!followHead_) {
    // Back to the head, as Follow head would.
    ImGui::SetCursorScreenPos(ImVec2(start.x + width - goWidth, start.y));
    if (ui::Button("Go to Head", ImVec2(goWidth, 0))) followHead_ = true;
  }
  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + lineHeight + 6));
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "Sectors, in the order they pass the head");
  ImGui::PopFont();
  drawSectorChips(width);
}

void DiskInspector::zoomPane(float factor) {
  paneScale_ = std::clamp(paneScale_ * factor, PANE_SCALE_MIN, PANE_SCALE_MAX);
}

// ⌘ and the wheel over a pane zoom its text, as text zooms anywhere on a Mac.
// ImGui reads ⌘ as Ctrl there.
void DiskInspector::zoomPaneFromWheel() {
  const ImGuiIO &io = ImGui::GetIO();
  if (!io.KeyCtrl || io.MouseWheel == 0 || !ImGui::IsWindowHovered()) return;
  zoomPane(std::pow(PANE_SCALE_STEP, io.MouseWheel));
}

void DiskInspector::draw(const InspectedDisk &disk, float width) {
  disk_ = disk;
  paintPlatterTexture();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 panel = ImGui::GetCursorScreenPos();
  const float left = panel.x + PANEL_PADDING;
  const float right = panel.x + width - PANEL_PADDING;
  const float inner = right - left;
  // The frame goes behind everything, so it is drawn on a channel below.
  draw->ChannelsSplit(2);
  draw->ChannelsSetCurrent(1);

  // One line: which disk and what is on it, and how to look at it.
  float y = panel.y + 12;
  const float lineHeight = ImGui::GetFrameHeight();
  const float switchWidth = ui::SwitchWidth("Follow head");
  const float controls = 170 + 14 + switchWidth;
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.1f);
  const ImVec2 titleSize = ImGui::CalcTextSize(disk.title.c_str());
  draw->AddText(ImVec2(left, y + (lineHeight - titleSize.y) * 0.5f), text(), disk.title.c_str());
  ImGui::PopFont();
  const float aboutX = left + titleSize.x + 12;
  draw->PushClipRect(ImVec2(aboutX, y), ImVec2(right - controls - 12, y + lineHeight), true);
  draw->AddText(ImVec2(aboutX, y + (lineHeight - ImGui::GetTextLineHeight()) * 0.5f), secondary(), disk.about.c_str());
  draw->PopClipRect();
  ImGui::SetCursorScreenPos(ImVec2(right - controls, y));
  int mode = mode_ == PlatterMode::Timing ? 1 : 0;
  if (ui::SegmentedControl("##mode", &mode, {"Structure", "Timing"}, 170.0f)) {
    mode_ = mode == 1 ? PlatterMode::Timing : PlatterMode::Structure;
    viewStale_ = true;
  }
  ImGui::SameLine(0, 14);
  ui::Switch("Follow head", &followHead_);
  y += lineHeight + 10;

  // The platter and its key; beside them the picked track, its sectors and
  // what is in them, down to the key's foot.
  drawPlatter(ImVec2(left, y), PLATTER_SIZE);
  ImGui::SetCursorScreenPos(ImVec2(left, y + PLATTER_SIZE + 8));
  drawLegend(PLATTER_SIZE);
  const float platterBottom = ImGui::GetCursorScreenPos().y;
  const float columnX = left + PLATTER_SIZE + 16;
  const float columnWidth = right - columnX;
  ImGui::SetCursorScreenPos(ImVec2(columnX, y));
  ImGui::BeginGroup();
  if (disk.hasDisk) {
    drawTrackSummary(columnWidth);
    ImGui::Dummy(ImVec2(0, 2));
    ui::SegmentedControl("##pane", &pane_, {"Sector Data", "Nibbles"}, 200.0f);
    // The text's size: smaller, larger, and how large it is now.
    ImGui::SameLine(0, 12);
    ImGui::PushID("panezoom");
    const float button = ImGui::GetFrameHeight();
    ImGui::BeginDisabled(paneScale_ <= PANE_SCALE_MIN + 0.001f);
    if (ui::Button("-", ImVec2(button, 0))) zoomPane(1.0f / PANE_SCALE_STEP);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Smaller text (\xE2\x8C\x98-scroll over the table)");
    }
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(paneScale_ >= PANE_SCALE_MAX - 0.001f);
    if (ui::Button("+", ImVec2(button, 0))) zoomPane(PANE_SCALE_STEP);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Larger text (\xE2\x8C\x98-scroll over the table)");
    }
    ImGui::SameLine(0, 8);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%.0f%%", paneScale_ / PANE_FONT_SCALE * 100.0f);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 2));
    const float height = std::max(150.0f, platterBottom - ImGui::GetCursorScreenPos().y);
    if (pane_ == 0) drawSectorBytes(columnWidth, height);
    else drawNibbles(columnWidth, height);
  } else {
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "Insert a disk to see what is recorded on it.");
  }
  ImGui::EndGroup();
  y = std::max(platterBottom, ImGui::GetItemRectMax().y) + 10;

  // The track unrolled, the whole width.
  if (disk.hasDisk) {
    ImGui::SetCursorScreenPos(ImVec2(left, y));
    ImGui::BeginGroup();
    drawStrip(inner);
    ImGui::EndGroup();
    y = ImGui::GetItemRectMax().y;
  }
  const ImVec2 end(panel.x + width, y + 12);

  draw->ChannelsSetCurrent(0);
  draw->AddRectFilled(panel, end, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  border(draw, panel, end, CARD_ROUNDING);
  draw->ChannelsMerge();
  ImGui::SetCursorScreenPos(panel);
  ImGui::Dummy(ImVec2(width, end.y - panel.y));
}

} // namespace a2e::native
