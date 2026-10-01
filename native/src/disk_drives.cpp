/*
 * disk_drives.cpp - The two 5.25" drives: what is in them, and the window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "disk_drives.hpp"

#include "emulation.hpp"

#include "cards/disk_controller.hpp"
#include "disk-image/disk_image.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace a2e::native {

namespace {

constexpr const char *SAVE_POPUP = "Save Disk";
constexpr const char *ERROR_POPUP = "Disk Error";
constexpr const char *BLANK_NAME = "Blank Disk.woz";

// The surface, in the browser's canvas units (disk-surface-renderer.js).
constexpr float CANVAS_W = 280;
constexpr float CANVAS_H = 240;
constexpr float CENTER_X = 140;
constexpr float CENTER_Y = 115;
constexpr float OUTER_RADIUS = 105;
constexpr float HUB_HOLE_RADIUS = 28;
constexpr float HUB_RING_INNER = 28;
constexpr float HUB_RING_OUTER = 34;
constexpr float TRACK_OUTER = OUTER_RADIUS - 3;
constexpr float TRACK_INNER = HUB_RING_OUTER + 4;
constexpr float TRACK_RANGE = TRACK_OUTER - TRACK_INNER;
constexpr int NUM_TRACKS = 35;
constexpr int NUM_SECTORS = 16;
constexpr double RAD_PER_MS = M_PI / 100.0; // 300 RPM

// The dark theme's colours from the browser's CSS fallbacks.
constexpr ImU32 COLOR_BG = IM_COL32(0x16, 0x1b, 0x22, 255);
constexpr ImU32 COLOR_MEDIUM = IM_COL32(0x1a, 0x13, 0x08, 255);
constexpr ImU32 COLOR_SECTOR = IM_COL32(255, 255, 255, 20);
constexpr ImU32 COLOR_GHOST_OUTLINE = IM_COL32(255, 255, 255, 15);
constexpr ImU32 COLOR_GHOST_SECTOR = IM_COL32(255, 255, 255, 8);
constexpr ImU32 COLOR_GHOST_HUB = IM_COL32(210, 208, 200, 20);
constexpr ImU32 COLOR_HUB_RING = IM_COL32(210, 208, 200, 217);
constexpr ImU32 COLOR_HUB_EDGE = IM_COL32(255, 255, 255, 31);
constexpr ImU32 COLOR_HUB_EDGE_INNER = IM_COL32(255, 255, 255, 20);
constexpr ImU32 COLOR_HOLE_EDGE = IM_COL32(0, 0, 0, 77);
constexpr ImU32 COLOR_DISK_EDGE = IM_COL32(255, 255, 255, 15);

const ImVec4 ACTIVE_TEXT(0.38f, 0.73f, 0.27f, 1.0f);   // the logo's green
const ImVec4 WRITE_TEXT(0.88f, 0.23f, 0.24f, 1.0f);    // and its red

std::string lower(std::string text) {
  for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string extensionOf(const std::string &path) {
  return lower(std::filesystem::path(path).extension().string());
}

} // namespace

const std::array<SaveFormat, 3> &saveFormats() {
  static const std::array<SaveFormat, 3> formats = {{
      {"DOS 3.3 order", ".dsk", {"dsk", "do"}},
      {"ProDOS order", ".po", {"po"}},
      {"WOZ", ".woz", {"woz"}},
  }};
  return formats;
}

std::string nameForFormat(const std::string &filename, int format) {
  if (format < 0 || format > 2) return filename;
  const SaveFormat &spec = saveFormats()[format];
  const size_t dot = filename.find_last_of('.');
  const std::string base = dot != std::string::npos && dot > 0 ? filename.substr(0, dot) : filename;
  const std::string extension =
      dot != std::string::npos && dot > 0 ? lower(filename.substr(dot + 1)) : "";
  if (std::find(spec.extensions.begin(), spec.extensions.end(), extension) != spec.extensions.end()) {
    return filename;
  }
  return base + "." + spec.extensions.front();
}

uint32_t stickerColor(const std::string &filename) {
  static const uint32_t colors[] = {0xf5f0d0, 0xe8d8a0, 0xc8e6c0, 0xb8d4e8,
                                    0xf0c0c8, 0xf0e8a0, 0xd0c8e8, 0xf0ece8};
  int32_t hash = 0;
  for (unsigned char c : filename) hash = static_cast<int32_t>((static_cast<uint32_t>(hash) << 5) - static_cast<uint32_t>(hash) + c);
  return colors[std::abs(static_cast<int64_t>(hash)) % 8];
}

bool DiskDrives::isFloppyImage(const std::string &path) {
  const std::string extension = extensionOf(path);
  return extension == ".dsk" || extension == ".do" || extension == ".po" ||
         extension == ".woz" || extension == ".nib";
}

DiskDrives::DiskDrives(Emulation &emulation, Platform &platform, std::string mediaDirectory)
    : emulation_(emulation), platform_(platform), store_(std::move(mediaDirectory), "floppy") {
  libraryDirectory_ = platform_.resourceDirectory + "/disks";
  std::ifstream in(libraryDirectory_ + "/library.json");
  if (in) {
    std::stringstream json;
    json << in.rdbuf();
    for (const LibraryEntry &entry : parseLibrary(json.str())) {
      if (entry.type == "floppy") library_.push_back(entry);
    }
  }
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

std::optional<uint32_t> DiskDrives::currentFingerprint(int drive) {
  return emulation_.withMachine([&](host::MachineHost &host) -> std::optional<uint32_t> {
    size_t size = 0;
    const uint8_t *data = host.exportDiskAs(drive, host.diskNativeFormat(drive), &size);
    if (!data || size == 0) return std::nullopt;
    return fingerprint(data, size);
  });
}

void DiskDrives::insertImage(int drive, const std::string &filename,
                             const std::vector<uint8_t> &data, bool remember) {
  const bool ok = emulation_.withMachine([&](host::MachineHost &host) {
    return host.insertDisk(drive, data.data(), data.size(), filename.c_str());
  });
  if (!ok) {
    reportError("Could not load the disk image " + filename + ".");
    return;
  }
  Drive &d = drives_[drive];
  resetVisuals(d);
  d.filename = filename;
  d.baseline = currentFingerprint(drive);
  if (remember) {
    store_.saveInserted(drive, filename, data);
    store_.addRecent(drive, filename, data);
  }
}

void DiskDrives::insertFile(int drive, const std::string &path) {
  auto data = readFile(path);
  if (!data) {
    reportError("Could not read " + path + ".");
    return;
  }
  insertImage(drive, baseName(path), *data, true);
}

// An unformatted WOZ for the machine to format. Not remembered: there is
// nothing on it yet.
void DiskDrives::insertBlank(int drive) {
  const bool ok = emulation_.withMachine([&](host::MachineHost &host) { return host.insertBlankDisk(drive); });
  if (!ok) {
    reportError("Could not insert a blank disk.");
    return;
  }
  Drive &d = drives_[drive];
  resetVisuals(d);
  d.filename = BLANK_NAME;
  d.baseline = currentFingerprint(drive);
  store_.clearInserted(drive);
}

void DiskDrives::restore() {
  for (int drive = 0; drive < DRIVES; drive++) {
    if (auto image = store_.loadInserted(drive)) insertImage(drive, image->filename, image->data, false);
  }
}

void DiskDrives::machineChanged() {
  for (int drive = 0; drive < DRIVES; drive++) {
    resetVisuals(drives_[drive]);
    drives_[drive].filename.reset();
    drives_[drive].baseline.reset();
    store_.clearInserted(drive);
  }
}

// The baseline is taken afresh, so a disk is asked about on eject only if it
// changes after the state was loaded.
void DiskDrives::syncWithMachine() {
  for (int drive = 0; drive < DRIVES; drive++) {
    std::optional<std::string> name;
    emulation_.withMachine([&](host::MachineHost &host) {
      if (!host.isDiskInserted(drive)) return;
      const char *filename = host.diskFilename(drive);
      name = filename && *filename ? filename : "Restored Disk";
    });
    Drive &d = drives_[drive];
    resetVisuals(d);
    d.filename = name;
    d.baseline = name ? currentFingerprint(drive) : std::nullopt;
  }
}

void DiskDrives::chooseDisk(int drive) {
  platform_.openFile("Insert a disk into drive " + std::to_string(drive + 1),
                     {"dsk", "do", "po", "woz", "nib"}, [this, drive](const std::string &path) {
                       if (!path.empty()) insertFile(drive, path);
                     });
}

int DiskDrives::dropTarget() const {
  if (!drives_[0].filename) return 0;
  if (!drives_[1].filename) return 1;
  return 0;
}

// Two gates, the cheap one first: the core says whether anything was ever
// written, and only then is the image fingerprinted to see whether it is
// actually different from what went in.
void DiskDrives::requestEject(int drive) {
  const bool written = emulation_.withMachine([&](host::MachineHost &host) { return host.isDiskModified(drive); });
  if (written) {
    const auto now = currentFingerprint(drive);
    if (!now || !drives_[drive].baseline || *now != *drives_[drive].baseline) {
      beginSave(drive);
      return;
    }
  }
  eject(drive);
}

void DiskDrives::eject(int drive) {
  emulation_.withMachine([&](host::MachineHost &host) { host.ejectDisk(drive); });
  resetVisuals(drives_[drive]);
  drives_[drive].filename.reset();
  drives_[drive].baseline.reset();
  store_.clearInserted(drive);
}

void DiskDrives::beginSave(int drive) {
  PendingSave save;
  save.drive = drive;
  int native = 0;
  emulation_.withMachine([&](host::MachineHost &host) {
    native = static_cast<int>(host.diskNativeFormat(drive));
    for (int format = 0; format < 3; format++) {
      save.available[format] = host.canExportDiskAs(drive, static_cast<DiskSaveFormat>(format));
    }
  });
  // A disk that cannot be written in any format is not saved; it is ejected
  // as the browser ejects it.
  if (std::none_of(save.available.begin(), save.available.end(), [](bool b) { return b; })) {
    eject(drive);
    return;
  }
  if (native < 0 || native > 2 || !save.available[native]) {
    native = static_cast<int>(std::find(save.available.begin(), save.available.end(), true) -
                              save.available.begin());
  }
  save.format = native;
  const std::string suggested =
      nameForFormat(drives_[drive].filename.value_or("disk" + std::to_string(drive + 1)), native);
  std::snprintf(save.name, sizeof(save.name), "%s", suggested.c_str());
  save.open = true;
  save_ = save;
}

void DiskDrives::finishSave(const PendingSave &save, const std::vector<uint8_t> &data) {
  const std::vector<std::string> &extensions = saveFormats()[save.format].extensions;
  const int drive = save.drive;
  platform_.saveFile("Save the disk in drive " + std::to_string(drive + 1), save.name, extensions,
                     [this, drive, data](const std::string &path) {
                       // Cancelling the panel keeps the disk in the drive,
                       // so nothing the machine wrote is lost by accident.
                       if (path.empty()) return;
                       if (!writeFile(path, data.data(), data.size())) {
                         reportError("Could not write " + path + ".");
                         return;
                       }
                       eject(drive);
                     });
}

void DiskDrives::resetVisuals(Drive &drive) {
  drive.trackAccess.fill(0);
  drive.maxAccess = 0;
  drive.lastTrack = -1;
  drive.angle = 0;
  drive.velocity = 0;
  drive.spinning = false;
  drive.lastTime = 0;
}

void DiskDrives::reportError(const std::string &message) {
  error_ = message;
  openError_ = true;
}

// ---------------------------------------------------------------------------
// Following the drives
// ---------------------------------------------------------------------------

void DiskDrives::update(double now) {
  emulation_.withMachine([&](host::MachineHost &host) {
    DiskController *disk = host.diskController();
    if (!disk) return;
    selectedDrive_ = disk->getSelectedDrive();
    motorOn_ = disk->isMotorOn();
    phase_ = disk->getPhaseStates();
    lastByte_ = disk->getDataLatch();
    const bool writing = disk->getQ7();
    for (int i = 0; i < DRIVES; i++) {
      Drive &d = drives_[i];
      d.hasDisk = disk->hasDisk(i);
      d.active = d.hasDisk && motorOn_ && i == selectedDrive_;
      d.writing = writing;
      if (const DiskImage *image = disk->getDiskImage(i)) {
        d.track = image->getTrack();
        d.quarterTrack = image->getQuarterTrack();
        d.nibble = image->getCurrentNibblePosition();
      }
    }
  });

  for (Drive &d : drives_) {
    if (!d.hasDisk) {
      d.lastTrack = -1;
      continue;
    }
    // The stepper's click, on a whole track only.
    if (d.active && d.lastTrack >= 0 && d.track != d.lastTrack) emulation_.driveSounds().playSeek();
    d.lastTrack = d.track;

    if (d.active) {
      const int track = std::min(d.track, NUM_TRACKS - 1);
      d.trackAccess[track]++;
      d.maxAccess = std::max(d.maxAccess, d.trackAccess[track]);
    }
    if (now - d.lastDecay > 0.1) {
      d.lastDecay = now;
      uint32_t max = 0;
      for (uint32_t &count : d.trackAccess) {
        count = static_cast<uint32_t>(std::floor(count * 0.8));
        max = std::max(max, count);
      }
      d.maxAccess = max;
    }

    const double dtMs = d.lastTime > 0 ? (now - d.lastTime) * 1000.0 : 0.0;
    d.lastTime = now;
    if (d.active) {
      d.spinning = true;
      d.velocity = RAD_PER_MS;
    } else if (d.spinning && dtMs > 0) {
      d.velocity *= std::pow(0.5, dtMs / 600.0);
      if (d.velocity < RAD_PER_MS * 0.005) {
        d.velocity = 0;
        d.spinning = false;
      }
    }
    if (d.spinning) d.angle += d.velocity * dtMs;
  }
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void DiskDrives::drawSurface(int index, ImVec2 origin) {
  const Drive &d = drives_[index];
  ImDrawList *draw = ImGui::GetWindowDrawList();
  auto at = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };
  const ImVec2 center = at(CENTER_X, CENTER_Y);
  const float TWO_PI = static_cast<float>(2 * M_PI);

  draw->AddRectFilled(origin, at(CANVAS_W, CANVAS_H), COLOR_BG);

  if (!d.hasDisk) {
    // A ghost of the platter.
    draw->AddCircle(center, OUTER_RADIUS, COLOR_GHOST_OUTLINE, 96, 1.0f);
    for (int s = 0; s < NUM_SECTORS; s++) {
      const float a = s * TWO_PI / NUM_SECTORS;
      draw->AddLine(ImVec2(center.x + std::cos(a) * TRACK_INNER, center.y + std::sin(a) * TRACK_INNER),
                    ImVec2(center.x + std::cos(a) * TRACK_OUTER, center.y + std::sin(a) * TRACK_OUTER),
                    COLOR_GHOST_SECTOR, 0.5f);
    }
    draw->AddCircle(center, (HUB_RING_INNER + HUB_RING_OUTER) / 2, COLOR_GHOST_HUB, 64,
                    HUB_RING_OUTER - HUB_RING_INNER);
    draw->AddCircle(center, HUB_HOLE_RADIUS, COLOR_GHOST_OUTLINE, 64, 0.5f);
    return;
  }

  // The medium.
  draw->AddCircleFilled(center, OUTER_RADIUS, COLOR_MEDIUM, 128);
  draw->AddCircle(center, OUTER_RADIUS, COLOR_DISK_EDGE, 128, 0.5f);

  // Tracks the head has been over lately, outermost first, warmer the more.
  if (d.maxAccess > 0) {
    const double logMax = std::log(d.maxAccess + 1.0);
    for (int t = 0; t < NUM_TRACKS; t++) {
      if (d.trackAccess[t] == 0) continue;
      const float outer = TRACK_OUTER - t * TRACK_RANGE / NUM_TRACKS - 0.5f;
      const float inner = TRACK_OUTER - (t + 1) * TRACK_RANGE / NUM_TRACKS + 0.5f;
      const double intensity = std::log(d.trackAccess[t] + 1.0) / logMax;
      const int r = static_cast<int>(std::lround(40 + 215 * intensity));
      const int g = static_cast<int>(std::lround(60 + 80 * intensity - 40 * intensity * intensity));
      const int b = static_cast<int>(std::lround(100 - 80 * intensity));
      const int a = static_cast<int>(std::lround((0.3 + 0.55 * intensity) * 255));
      draw->AddCircle(center, (outer + inner) / 2, IM_COL32(r, g, b, a), 96,
                      std::max(outer - inner, 0.5f));
    }
  }

  // Sector lines, turning with the disk.
  const float angle = static_cast<float>(d.angle);
  for (int s = 0; s < NUM_SECTORS; s++) {
    const float a = s * TWO_PI / NUM_SECTORS + angle;
    draw->AddLine(ImVec2(center.x + std::cos(a) * TRACK_INNER, center.y + std::sin(a) * TRACK_INNER),
                  ImVec2(center.x + std::cos(a) * TRACK_OUTER, center.y + std::sin(a) * TRACK_OUTER),
                  COLOR_SECTOR, 0.5f);
  }

  // The head: green reading, red writing, grey idle, with a glow while busy.
  const float headR = TRACK_OUTER - ((d.quarterTrack / 4.0f) + 0.5f) * TRACK_RANGE / NUM_TRACKS;
  if (d.active) {
    draw->AddRectFilled(ImVec2(center.x - 4, center.y - headR - 3), ImVec2(center.x + 4, center.y - headR + 3),
                        d.writing ? IM_COL32(220, 40, 40, 102) : IM_COL32(40, 200, 60, 102));
  }
  const ImU32 head = !d.active ? IM_COL32(160, 155, 150, 179)
                     : d.writing ? IM_COL32(220, 40, 40, 217)
                                 : IM_COL32(40, 200, 60, 217);
  draw->AddRectFilled(ImVec2(center.x - 3, center.y - headR - 2), ImVec2(center.x + 3, center.y - headR + 2), head);

  // The hub's reinforcement ring and the hole through it.
  draw->AddCircle(center, (HUB_RING_INNER + HUB_RING_OUTER) / 2, COLOR_HUB_RING, 64,
                  HUB_RING_OUTER - HUB_RING_INNER);
  draw->AddCircle(center, HUB_RING_OUTER, COLOR_HUB_EDGE, 64, 0.5f);
  draw->AddCircle(center, HUB_RING_INNER, COLOR_HUB_EDGE_INNER, 64, 0.5f);
  draw->AddCircleFilled(center, HUB_HOLE_RADIUS, COLOR_BG, 64);
  draw->AddCircle(center, HUB_HOLE_RADIUS, COLOR_HOLE_EDGE, 64, 0.5f);

  // The index hole, punched through the ring, turning with the disk.
  auto rotated = [&](float x, float y) {
    return ImVec2(center.x + x * std::cos(angle) - y * std::sin(angle),
                  center.y + x * std::sin(angle) + y * std::cos(angle));
  };
  const ImVec2 hole[4] = {rotated(HUB_RING_OUTER, -1.5f), rotated(HUB_RING_OUTER, 1.5f),
                          rotated(HUB_RING_INNER, 2.5f), rotated(HUB_RING_INNER, -2.5f)};
  draw->AddConvexPolyFilled(hole, 4, IM_COL32(200, 30, 30, 230));
  draw->AddPolyline(hole, 4, IM_COL32(120, 10, 10, 153), ImDrawFlags_Closed, 0.5f);

  // The drive's label, as the browser puts it on the canvas.
  const std::string label = "D" + std::to_string(index + 1);
  draw->AddText(at(8, CANVAS_H - 20), IM_COL32(255, 255, 255, 110), label.c_str());
}

void DiskDrives::drawRecentPopup(int index) {
  const std::string id = "##recent" + std::to_string(index);
  if (!ImGui::BeginPopup(id.c_str())) return;

  const std::vector<RecentEntry> recent = store_.recent(index);
  if (recent.empty()) {
    ImGui::TextDisabled("No recent disks");
  } else {
    for (const RecentEntry &entry : recent) {
      if (ImGui::Selectable(entry.filename.c_str())) {
        if (auto image = store_.loadRecent(index, entry)) {
          insertImage(index, image->filename, image->data, true);
        } else {
          reportError("Could not read the recent disk " + entry.filename + ".");
        }
      }
    }
    ImGui::Separator();
    if (ImGui::Selectable("Clear Recent")) store_.clearRecent(index);
  }

  if (!library_.empty()) {
    ImGui::SeparatorText("Library");
    for (const LibraryEntry &entry : library_) {
      if (ImGui::Selectable(entry.name.c_str())) {
        if (auto data = readFile(libraryDirectory_ + "/" + entry.file)) {
          insertImage(index, entry.file, *data, true);
        } else {
          reportError("Could not read " + entry.name + " from the app.");
        }
      }
      if (!entry.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", entry.description.c_str());
      }
    }
  }
  ImGui::EndPopup();
}

void DiskDrives::drawDrive(int index) {
  Drive &d = drives_[index];
  ImGui::PushID(index);
  ImGui::BeginGroup();

  if (surfaceShown) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    drawSurface(index, origin);
    ImGui::Dummy(ImVec2(CANVAS_W, CANVAS_H));
  }

  // The name, and the track the head is on.
  const std::string name = d.filename.value_or("No Disk");
  const char *track = nullptr;
  char trackText[8];
  if (d.filename) {
    std::snprintf(trackText, sizeof(trackText), "T%02d", d.track);
    track = trackText;
  }
  const float width = CANVAS_W;
  const float trackWidth = ImGui::CalcTextSize("T00").x;
  ImGui::PushStyleColor(ImGuiCol_Text, d.filename ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                                  : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  const ImVec2 nameStart = ImGui::GetCursorScreenPos();
  ImGui::PushClipRect(nameStart, ImVec2(nameStart.x + width - trackWidth - 8, nameStart.y + ImGui::GetTextLineHeight()), true);
  if (d.filename) {
    const uint32_t sticker = stickerColor(name);
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(nameStart.x, nameStart.y + 2), ImVec2(nameStart.x + 4, nameStart.y + ImGui::GetTextLineHeight() - 2),
        IM_COL32((sticker >> 16) & 0xFF, (sticker >> 8) & 0xFF, sticker & 0xFF, 255));
    ImGui::SetCursorScreenPos(ImVec2(nameStart.x + 8, nameStart.y));
  }
  ImGui::TextUnformatted(name.c_str());
  ImGui::PopClipRect();
  ImGui::PopStyleColor();
  if (d.filename && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", name.c_str());
  ImGui::SameLine(0, 0);
  ImGui::SetCursorScreenPos(ImVec2(nameStart.x + width - trackWidth, nameStart.y));
  if (track) {
    ImGui::TextColored(d.active ? ACTIVE_TEXT : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", track);
  } else {
    ImGui::TextDisabled("T--");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Current track");

  // Controls.
  if (ImGui::Button("Insert")) chooseDisk(index);
  ImGui::SameLine();
  const std::string recentId = "##recent" + std::to_string(index);
  if (ImGui::Button("Recent")) ImGui::OpenPopup(recentId.c_str());
  drawRecentPopup(index);
  ImGui::SameLine();
  if (ImGui::Button("Blank")) insertBlank(index);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Insert a blank disk");
  ImGui::SameLine();
  ImGui::BeginDisabled(!d.filename);
  if (ImGui::Button("Eject")) requestEject(index);
  ImGui::EndDisabled();

  if (detailsShown) {
    ImGui::Spacing();
    if (ImGui::BeginTable("details", 4, ImGuiTableFlags_SizingFixedFit)) {
      auto row = [](const char *a, const std::string &av, const ImVec4 *ac, const char *b,
                    const std::string &bv, const ImVec4 *bc) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", a);
        ImGui::TableNextColumn();
        if (ac) ImGui::TextColored(*ac, "%s", av.c_str());
        else ImGui::TextUnformatted(av.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn();
        if (bc) ImGui::TextColored(*bc, "%s", bv.c_str());
        else ImGui::TextUnformatted(bv.c_str());
      };
      char byte[4];
      std::snprintf(byte, sizeof(byte), "%02X", lastByte_);
      const bool motor = motorOn_ && index == selectedDrive_;
      row("QTrack", std::to_string(d.hasDisk ? d.quarterTrack : 0), nullptr, "Phase",
          std::to_string(phase_), nullptr);
      row("Nibble", std::to_string(d.hasDisk ? d.nibble : 0), nullptr, "Motor", motor ? "ON" : "OFF",
          motor ? &ACTIVE_TEXT : nullptr);
      row("Mode", d.writing ? "Write" : "Read", d.writing ? &WRITE_TEXT : nullptr, "Byte",
          index == selectedDrive_ ? byte : "--", nullptr);
      ImGui::EndTable();
    }
  }

  ImGui::EndGroup();
  ImGui::PopID();
}

// Which format, under what name. Formats the disk cannot be written as are
// shown, disabled, rather than left out: a copy-protected WOZ has no sector
// form, and leaving the option out would look like the app had forgotten
// how to write a .dsk.
void DiskDrives::drawSavePopup() {
  if (save_.open) {
    ImGui::OpenPopup(SAVE_POPUP);
    save_.open = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(SAVE_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::Text("The disk in drive %d has changed. Save it as:", save_.drive + 1);
  ImGui::Spacing();
  for (int format = 0; format < 3; format++) {
    const SaveFormat &spec = saveFormats()[format];
    ImGui::BeginDisabled(!save_.available[format]);
    std::string label = std::string(spec.label) + "  " +
                        (save_.available[format] ? spec.hint : "not possible for this disk");
    if (ImGui::RadioButton(label.c_str(), save_.format == format)) {
      save_.format = format;
      const std::string renamed = nameForFormat(save_.name, format);
      std::snprintf(save_.name, sizeof(save_.name), "%s", renamed.c_str());
    }
    ImGui::EndDisabled();
  }
  ImGui::Spacing();
  ImGui::SetNextItemWidth(320.0f);
  ImGui::InputText("##name", save_.name, sizeof(save_.name));
  ImGui::Spacing();

  if (ImGui::Button("Save…", ImVec2(110, 0))) {
    std::vector<uint8_t> data;
    emulation_.withMachine([&](host::MachineHost &host) {
      size_t size = 0;
      const uint8_t *bytes = host.exportDiskAs(save_.drive, static_cast<DiskSaveFormat>(save_.format), &size);
      if (bytes && size) data.assign(bytes, bytes + size);
    });
    ImGui::CloseCurrentPopup();
    if (data.empty()) {
      reportError(std::string("This disk cannot be saved as ") + saveFormats()[save_.format].label + ".");
    } else {
      finishSave(save_, data);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Don't Save", ImVec2(110, 0))) {
    eject(save_.drive);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DiskDrives::drawErrorPopup() {
  if (openError_) {
    ImGui::OpenPopup(ERROR_POPUP);
    openError_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(ERROR_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextUnformatted(error_.c_str());
  if (ImGui::Button("OK", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DiskDrives::draw(bool *open) {
  if (open && *open) {
    if (ImGui::Begin("Disk Drives", open, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Checkbox("Surface", &surfaceShown);
      ImGui::SameLine();
      ImGui::Checkbox("Details", &detailsShown);
      ImGui::SameLine();
      bool sounds = emulation_.driveSounds().enabled();
      if (ImGui::Checkbox("Drive Sounds", &sounds)) emulation_.driveSounds().setEnabled(sounds);
      ImGui::Separator();
      drawDrive(0);
      ImGui::SameLine(0, 16);
      drawDrive(1);
    }
    ImGui::End();
  }
  // Outside the window, so an eject from a menu or a closed window still
  // gets its question answered.
  drawSavePopup();
  drawErrorPopup();
}

} // namespace a2e::native
