/*
 * disk_drives.cpp - The two 5.25" drives: what is in them, and the window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "disk_drives.hpp"
#include "ui_controls.hpp"

#include "emulation.hpp"
#include "ui_theme.hpp"

#include "cards/disk_controller.hpp"
#include "disk-image/disk_image.hpp"
#include "disk-image/disk_inspection.hpp"

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

// The window, in points.
constexpr float TOTAL_WIDTH = 980;
constexpr float DECK_GAP = 12;
constexpr float DECK_WIDTH = (TOTAL_WIDTH - DECK_GAP) / 2;
constexpr float DECK_HEIGHT = 136;
constexpr float CARD_ROUNDING = 10;
constexpr float THUMBNAIL_SIZE = 108;
constexpr float PLATTER_SIZE = 400;
constexpr float DETAIL_HEIGHT = 520;
constexpr float STRIP_HEIGHT = 82;
// The platter's textures, in pixels: enough for a Retina display.
constexpr int PLATTER_PIXELS = 800;
constexpr int THUMBNAIL_PIXELS = 224;
// Arcs per ring in the overview: half a degree each, as the browser reads it.
constexpr int OVERVIEW_BUCKETS = 720;
// How soon a disk being written is read again.
constexpr double REREAD_SECONDS = 0.5;

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
  drive.lastTrack = -1;
  drive.rotation = 0;
  drive.overviewRead = false;
  drive.overviewAt = -1;
  drive.overview.reset();
  drive.summary = DiskSummary{};
  drive.paintStale = true;
  detailDrive_ = -1; // whatever was read of it is gone
}

void DiskDrives::reportError(const std::string &message) {
  error_ = message;
  openError_ = true;
}

DiskDrives::~DiskDrives() {
  for (Drive &drive : drives_) releasePlatters(drive);
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
      const DiskImage *image = disk->getDiskImage(i);
      if (image) {
        d.track = image->getTrack();
        d.quarterTrack = image->getQuarterTrack();
        d.nibble = image->getCurrentNibblePosition();
        d.rotation = image->getRotation();
      }

      // What is on the disk, read again when its revision moves. Through
      // the const accessor: the writable one counts as a change, and the
      // disk would be re-read for ever.
      if (d.hasDisk && image) {
        const uint32_t revision = disk->getRevision(i);
        if (!d.overviewRead || (revision != d.revision && now - d.overviewAt >= REREAD_SECONDS)) {
          d.overview = parseOverview(inspect::buildOverview(*const_cast<DiskImage *>(image), OVERVIEW_BUCKETS));
          d.summary = d.overview ? summariseDisk(*d.overview) : DiskSummary{};
          d.revision = revision;
          d.overviewRead = true;
          d.overviewAt = now;
          d.paintStale = true;
        }
      } else if (d.overviewRead) {
        d.overviewRead = false;
        d.overview.reset();
        d.summary = DiskSummary{};
        d.paintStale = true;
      }
    }
    if (followHead_ && drives_[inspected_].hasDisk) selectedQt_ = drives_[inspected_].quarterTrack;
    refreshDetail(*disk, now);
  });

  for (Drive &d : drives_) {
    if (!d.hasDisk) {
      d.lastTrack = -1;
      continue;
    }
    // The stepper's click, on a whole track only.
    if (d.active && d.lastTrack >= 0 && d.track != d.lastTrack) emulation_.driveSounds().playSeek();
    d.lastTrack = d.track;
  }
}

void DiskDrives::refreshDetail(DiskController &disk, double now) {
  const Drive &d = drives_[inspected_];
  if (!d.hasDisk) {
    detail_ = TrackDetail{};
    detailDrive_ = -1;
    return;
  }
  const uint32_t revision = disk.getRevision(inspected_);
  const bool moved = detailDrive_ != inspected_ || detail_.quarterTrack != selectedQt_;
  const bool changed = revision != detailRevision_ && now - detailAt_ >= REREAD_SECONDS;
  if (!moved && !changed) return;
  const DiskImage *image = disk.getDiskImage(inspected_);
  if (!image) return;
  detail_ = readTrackDetail(*const_cast<DiskImage *>(image), selectedQt_);
  detailDrive_ = inspected_;
  detailRevision_ = revision;
  detailAt_ = now;
  if (selectedSector_ >= static_cast<int>(detail_.analysis.sectors.size())) selectedSector_ = 0;
}

// ---------------------------------------------------------------------------
// The platter
// ---------------------------------------------------------------------------

void DiskDrives::releasePlatters(Drive &drive) {
  if (platform_.releaseTexture) {
    platform_.releaseTexture(drive.platter);
    platform_.releaseTexture(drive.thumbnail);
  }
  drive.platter = ImTextureID_Invalid;
  drive.thumbnail = ImTextureID_Invalid;
}

// Painted once per change and drawn turned, so a frame costs one textured
// quad however much is on the disk.
void DiskDrives::paintPlatters(Drive &drive) {
  if (!drive.paintStale || !platform_.makeTexture) return;
  releasePlatters(drive);
  drive.paintStale = false;
  if (!drive.hasDisk) return;
  const Overview *overview = drive.overview ? &*drive.overview : nullptr;
  std::vector<uint8_t> rgba;
  paintPlatter(rgba, PLATTER_PIXELS, overview, mode_);
  drive.platter = platform_.makeTexture(rgba.data(), PLATTER_PIXELS, PLATTER_PIXELS);
  paintPlatter(rgba, THUMBNAIL_PIXELS, overview, PlatterMode::Structure);
  drive.thumbnail = platform_.makeTexture(rgba.data(), THUMBNAIL_PIXELS, THUMBNAIL_PIXELS);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

namespace {

ImU32 rgbU32(uint32_t rgb, float alpha = 1.0f) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                  static_cast<int>(255 * std::clamp(alpha, 0.0f, 1.0f)));
}

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

// The colour of a drive's light: green reading, red writing.
ImU32 headColour(bool active, bool writing) {
  if (!active) return ui::isDark() ? IM_COL32(150, 150, 156, 255) : IM_COL32(120, 120, 126, 255);
  return writing ? IM_COL32(224, 58, 62, 255) : IM_COL32(97, 187, 70, 255);
}

void glow(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour) {
  for (int i = 4; i >= 1; i--) draw->AddCircleFilled(centre, radius + i * 2.2f, withAlpha(colour, 0.09f));
}

// A light, glowing while lit.
void light(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour, bool lit) {
  if (lit) {
    glow(draw, centre, radius, colour);
    draw->AddCircleFilled(centre, radius, colour);
    draw->AddCircleFilled(ImVec2(centre.x - radius * 0.3f, centre.y - radius * 0.3f), radius * 0.35f,
                          IM_COL32(255, 255, 255, 120));
  } else {
    draw->AddCircleFilled(centre, radius, ui::isDark() ? IM_COL32(70, 70, 74, 255) : IM_COL32(196, 196, 200, 255));
  }
}

// A square texture drawn turned about its centre by `angle` radians,
// clockwise on the screen.
void drawTurned(ImDrawList *draw, ImTextureID texture, ImVec2 centre, float radius, float angle) {
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  auto corner = [&](float x, float y) {
    return ImVec2(centre.x + (x * c - y * s) * radius, centre.y + (x * s + y * c) * radius);
  };
  draw->AddImageQuad(ImTextureRef(texture), corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1),
                     ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
}

// The light a spinning disk holds still: two soft bands across the medium,
// opposite each other, that do not turn with it.
void sheen(ImDrawList *draw, ImVec2 centre, float outer, float inner) {
  for (int side = 0; side < 2; side++) {
    const float middle = -0.75f * static_cast<float>(M_PI) + side * static_cast<float>(M_PI);
    for (int band = 0; band < 3; band++) {
      const float spread = 0.32f - band * 0.09f;
      draw->PathArcTo(centre, outer, middle - spread, middle + spread, 24);
      draw->PathArcTo(centre, inner, middle + spread, middle - spread, 24);
      draw->PathFillConcave(IM_COL32(255, 255, 255, 7));
    }
  }
}

// A disk's outline with nothing in the drive.
void ghostDisk(ImDrawList *draw, ImVec2 centre, float radius) {
  const ImU32 line = text(0.16f);
  draw->AddCircle(centre, radius * platter::DISK_EDGE, line, 0, 1.2f);
  draw->AddCircle(centre, radius * platter::HUB_RING_OUTER, line, 0, 1.0f);
  draw->AddCircle(centre, radius * platter::HUB_HOLE, line, 0, 1.0f);
  for (int s = 0; s < 16; s++) {
    const float a = s * 2.0f * static_cast<float>(M_PI) / 16;
    draw->AddLine(ImVec2(centre.x + std::sin(a) * radius * platter::BAND_INNER, centre.y - std::cos(a) * radius * platter::BAND_INNER),
                  ImVec2(centre.x + std::sin(a) * radius * platter::BAND_OUTER, centre.y - std::cos(a) * radius * platter::BAND_OUTER),
                  text(0.06f), 1.0f);
  }
}

// A small capsule with a label; returns its width.
float chip(ImDrawList *draw, ImVec2 at, const char *label, ImU32 colour) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.8f);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const ImVec2 end(at.x + size.x + 12, at.y + size.y + 4);
  draw->AddRectFilled(at, end, withAlpha(colour, 0.16f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 6, at.y + 2), colour, label);
  ImGui::PopFont();
  return end.x - at.x;
}

void centredText(ImDrawList *draw, ImVec2 centre, ImU32 colour, const char *line) {
  const ImVec2 size = ImGui::CalcTextSize(line);
  draw->AddText(ImVec2(std::floor(centre.x - size.x * 0.5f), std::floor(centre.y - size.y * 0.5f)), colour, line);
}

void dashedRect(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour) {
  const float dash = 4.0f;
  const float gap = 3.0f;
  auto line = [&](ImVec2 p, ImVec2 q) {
    const float length = std::hypot(q.x - p.x, q.y - p.y);
    const ImVec2 d((q.x - p.x) / length, (q.y - p.y) / length);
    for (float t = 0; t < length; t += dash + gap) {
      const float u = std::min(t + dash, length);
      draw->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * u, p.y + d.y * u), colour, 1.0f);
    }
  };
  line(a, ImVec2(b.x, a.y));
  line(ImVec2(b.x, a.y), b);
  line(b, ImVec2(a.x, b.y));
  line(ImVec2(a.x, b.y), a);
}

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

void border(ImDrawList *draw, ImVec2 a, ImVec2 b, float rounding) {
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), rounding);
}

} // namespace

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


// One drive: its disk turning as the real one does, the disk's label and
// what the inspector found on it, the head's track, the controller's state
// and the controls. Picking a card inspects that drive.
void DiskDrives::drawDeck(int index) {
  Drive &d = drives_[index];
  ImGui::PushID(index);
  ImGui::BeginGroup();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 card = ImGui::GetCursorScreenPos();
  // Takes the SameLine that put this card beside the other: everything in
  // the card is drawn, so its first item would otherwise take it.
  ImGui::Dummy(ImVec2(0, 0));
  const ImVec2 end(card.x + DECK_WIDTH, card.y + DECK_HEIGHT);
  const bool dark = ui::isDark();
  const bool inspected = inspectorShown && index == inspected_;
  draw->AddRectFilled(card, end, dark ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  if (inspected) {
    draw->AddRect(card, end, accent(0.9f), CARD_ROUNDING, 0, 1.5f);
  } else {
    border(draw, card, end, CARD_ROUNDING);
  }

  // The disk.
  const float radius = THUMBNAIL_SIZE * 0.5f;
  const ImVec2 centre(card.x + 14 + radius, card.y + DECK_HEIGHT * 0.5f);
  if (d.hasDisk && d.thumbnail != ImTextureID_Invalid) {
    draw->AddCircleFilled(ImVec2(centre.x, centre.y + 3), radius * 0.97f, IM_COL32(0, 0, 0, dark ? 90 : 40));
    drawTurned(draw, d.thumbnail, centre, radius, static_cast<float>(-d.rotation * 2 * M_PI));
    sheen(draw, centre, radius * platter::BAND_OUTER, radius * platter::BAND_INNER);
    const float headRadius = platter::radiusOf(d.quarterTrack) * radius;
    const ImVec2 head(centre.x, centre.y - headRadius);
    if (d.active) glow(draw, head, 2.5f, headColour(true, d.writing));
    draw->AddRectFilled(ImVec2(head.x - 4, head.y - 2.5f), ImVec2(head.x + 4, head.y + 2.5f),
                        headColour(d.active, d.writing), 1.5f);
  } else {
    ghostDisk(draw, centre, radius);
  }

  const float x0 = card.x + 14 + THUMBNAIL_SIZE + 16;
  const float right = end.x - 14;

  // The head's track, as a drive's front panel would show it.
  const ImVec2 box(right - 92, card.y + 14);
  const ImVec2 boxEnd(right, box.y + 58);
  draw->AddRectFilled(box, boxEnd, dark ? IM_COL32(0, 0, 0, 90) : IM_COL32(0, 0, 0, 14), 8.0f);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
  draw->AddText(ImVec2(box.x + 9, box.y + 7), secondary(), "TRACK");
  ImGui::PopFont();
  light(draw, ImVec2(boxEnd.x - 12, box.y + 12), 3.5f, headColour(true, d.writing), d.active);
  ImGui::PushFont(ui::monoFont(), 22.0f);
  const std::string trackText = d.hasDisk ? trackLabel(d.quarterTrack) : "--";
  const ImU32 trackColour = !d.hasDisk ? secondary() : d.active ? headColour(true, d.writing) : text();
  draw->AddText(ImVec2(box.x + 9, box.y + 24), trackColour, trackText.c_str());
  ImGui::PopFont();

  // The disk's label: the sticker colour the browser gives the name.
  const ImVec2 label(x0, card.y + 14);
  const ImVec2 labelEnd(box.x - 10, label.y + 26);
  if (d.filename) {
    const uint32_t sticker = stickerColor(*d.filename);
    draw->AddRectFilled(label, labelEnd, rgbU32(sticker), 5.0f);
    draw->AddRectFilled(label, ImVec2(label.x + 6, labelEnd.y), IM_COL32(0, 0, 0, 30), 5.0f, ImDrawFlags_RoundCornersLeft);
    draw->AddRect(label, labelEnd, IM_COL32(0, 0, 0, 40), 5.0f);
    draw->PushClipRect(label, ImVec2(labelEnd.x - 8, labelEnd.y), true);
    draw->AddText(ImVec2(label.x + 14, label.y + (26 - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(30, 28, 24, 255),
                  d.filename->c_str());
    draw->PopClipRect();
  } else {
    dashedRect(draw, label, labelEnd, text(0.22f));
    draw->AddText(ImVec2(label.x + 12, label.y + (26 - ImGui::GetTextLineHeight()) * 0.5f), secondary(),
                  "No disk. Drop one on this window.");
  }
  if (d.filename && ImGui::IsMouseHoveringRect(label, labelEnd)) ImGui::SetTooltip("%s", d.filename->c_str());

  // What the inspector found on it.
  float chipX = x0;
  const float chipY = label.y + 26 + 8;
  if (d.filename) {
    std::string extension = std::filesystem::path(*d.filename).extension().string();
    if (!extension.empty()) extension.erase(0, 1);
    for (char &c : extension) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!extension.empty()) chipX += chip(draw, ImVec2(chipX, chipY), extension.c_str(), text(0.75f)) + 5;
    if (d.summary.tracks > 0) {
      chipX += chip(draw, ImVec2(chipX, chipY), d.summary.format.c_str(), IM_COL32(0, 157, 220, 255)) + 5;
    }
    if (d.summary.fluxTracks > 0) chipX += chip(draw, ImVec2(chipX, chipY), "Flux", IM_COL32(150, 61, 151, 255)) + 5;
    if (d.summary.bad > 0) {
      const std::string bad = std::to_string(d.summary.bad) + " bad";
      chipX += chip(draw, ImVec2(chipX, chipY), bad.c_str(), IM_COL32(224, 58, 62, 255)) + 5;
    }
  }

  // The controller, which is one for both drives, as this drive sees it.
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.85f);
  char telemetry[96];
  if (d.hasDisk && index == selectedDrive_) {
    std::snprintf(telemetry, sizeof(telemetry), "PH %d%d%d%d  LATCH %02X  %s", phase_ & 1, (phase_ >> 1) & 1,
                  (phase_ >> 2) & 1, (phase_ >> 3) & 1, lastByte_,
                  d.active ? (d.writing ? "WRITING" : "READING") : "MOTOR OFF");
  } else {
    std::snprintf(telemetry, sizeof(telemetry), "%s", d.hasDisk ? "NOT SELECTED" : "");
  }
  draw->AddText(ImVec2(x0, chipY + 24), d.active ? headColour(true, d.writing) : secondary(), telemetry);
  ImGui::PopFont();

  // The controls.
  ImGui::SetCursorScreenPos(ImVec2(x0, end.y - 14 - ImGui::GetFrameHeight()));
  if (ui::Button("Insert", ImVec2(0, 0), d.filename ? ui::ButtonKind::Normal : ui::ButtonKind::Primary)) chooseDisk(index);
  ImGui::SameLine();
  const std::string recentId = "##recent" + std::to_string(index);
  if (ui::Button("Recent")) ImGui::OpenPopup(recentId.c_str());
  drawRecentPopup(index);
  ImGui::SameLine();
  if (ui::Button("Blank")) insertBlank(index);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Insert a blank disk");
  ImGui::SameLine();
  ImGui::BeginDisabled(!d.filename);
  if (ui::Button("Eject")) requestEject(index);
  ImGui::EndDisabled();
  const std::string driveName = "Drive " + std::to_string(index + 1);
  const float nameWidth = ImGui::CalcTextSize(driveName.c_str()).x;
  draw->AddText(ImVec2(right - nameWidth, end.y - 14 - ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y),
                inspected ? accent() : secondary(), driveName.c_str());

  // A click anywhere else on the card inspects this drive.
  if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(card, end) && !ImGui::IsAnyItemHovered() &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (inspected_ != index) {
      inspected_ = index;
      stripQt_ = -1;
      followHead_ = true;
    }
    inspectorShown = true;
  }

  ImGui::SetCursorScreenPos(card);
  ImGui::Dummy(ImVec2(DECK_WIDTH, DECK_HEIGHT));
  ImGui::EndGroup();
  ImGui::PopID();
}

void DiskDrives::drawPlatter(ImVec2 origin, float size) {
  Drive &d = drives_[inspected_];
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float radius = size * 0.5f - 2;
  const ImVec2 centre(origin.x + size * 0.5f, origin.y + size * 0.5f);
  ImGui::SetCursorScreenPos(origin);
  ImGui::InvisibleButton("##platter", ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();

  if (!d.hasDisk || d.platter == ImTextureID_Invalid) {
    ghostDisk(draw, centre, radius);
    const std::string empty = "No disk in drive " + std::to_string(inspected_ + 1);
    centredText(draw, centre, secondary(), empty.c_str());
    return;
  }

  const bool dark = ui::isDark();
  draw->AddCircleFilled(ImVec2(centre.x, centre.y + 8), radius * 0.98f, IM_COL32(0, 0, 0, dark ? 70 : 26), 96);
  draw->AddCircleFilled(ImVec2(centre.x, centre.y + 3), radius * 0.99f, IM_COL32(0, 0, 0, dark ? 90 : 34), 96);
  const float turn = static_cast<float>(d.rotation);
  drawTurned(draw, d.platter, centre, radius, -turn * 2.0f * static_cast<float>(M_PI));
  sheen(draw, centre, radius * platter::BAND_OUTER, radius * platter::BAND_INNER);

  // The quarter track being inspected.
  const float ringWidth = std::max(1.5f, platter::RING_WIDTH * radius);
  if (detail_.quarterTrack >= 0) {
    draw->AddCircle(centre, platter::radiusOf(detail_.quarterTrack) * radius, accent(0.95f), 0, ringWidth + 0.5f);
  }

  // Under the pointer: which quarter track, and what is on it there.
  if (hovered) {
    const ImVec2 m(ImGui::GetIO().MousePos.x - centre.x, ImGui::GetIO().MousePos.y - centre.y);
    const float r = std::hypot(m.x, m.y) / radius;
    const int qt = platter::quarterTrackAt(r);
    if (qt >= 0 && d.overview) {
      draw->AddCircle(centre, platter::radiusOf(qt) * radius, IM_COL32(255, 255, 255, 110), 0, ringWidth);
      double a = std::atan2(m.x, -m.y) / (2 * M_PI) + d.rotation;
      a -= std::floor(a);
      const OverviewTrack &t = d.overview->tracks[qt];
      std::string tip = "Track " + trackLabel(qt);
      if (t.present && d.overview->buckets > 0) {
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
      if (ImGui::IsItemClicked()) {
        selectedQt_ = qt;
        followHead_ = false;
      }
    }
  }

  // The head, at twelve o'clock, on its arm.
  const float headRadius = platter::radiusOf(d.quarterTrack) * radius;
  const ImVec2 head(centre.x, centre.y - headRadius);
  draw->AddLine(ImVec2(centre.x, centre.y - radius - 6), head, dark ? IM_COL32(255, 255, 255, 60) : IM_COL32(0, 0, 0, 70), 4.0f);
  if (d.active) glow(draw, head, 4.0f, headColour(true, d.writing));
  draw->AddRectFilled(ImVec2(head.x - 7, head.y - 4), ImVec2(head.x + 7, head.y + 4), headColour(d.active, d.writing), 2.5f);
  draw->AddRect(ImVec2(head.x - 7, head.y - 4), ImVec2(head.x + 7, head.y + 4), IM_COL32(0, 0, 0, 120), 2.5f);
}

void DiskDrives::drawLegend(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  struct Entry {
    uint32_t rgb;
    const char *label;
  };
  std::vector<Entry> entries;
  if (mode_ == PlatterMode::Timing) {
    entries = {{timeColour(static_cast<uint8_t>(NOMINAL_CELL_TIME * 0.94)), "Fast cells"},
               {timeColour(static_cast<uint8_t>(NOMINAL_CELL_TIME + 0.5)), "Nominal 3.91\xC2\xB5s"},
               {timeColour(static_cast<uint8_t>(NOMINAL_CELL_TIME * 1.06)), "Slow cells"}};
  } else {
    entries = {{kindColour(inspect::SYNC), "Sync"},          {kindColour(inspect::ADDR_PROLOGUE), "Address marks"},
               {kindColour(inspect::ADDR), "Address"},       {kindColour(inspect::DATA_PROLOGUE), "Data marks"},
               {kindColour(inspect::DATA), "Data"},          {kindColour(inspect::DATA | inspect::BAD), "Bad checksum"},
               {kindColour(inspect::OTHER), "Non-standard"}, {kindColour(inspect::INVALID), "Noise"}};
  }
  float x = start.x;
  float y = start.y;
  const float line = ImGui::GetTextLineHeight();
  for (const Entry &e : entries) {
    const float w = 14 + ImGui::CalcTextSize(e.label).x + 14;
    if (x + w > start.x + width) {
      x = start.x;
      y += line + 6;
    }
    draw->AddRectFilled(ImVec2(x, y + line * 0.5f - 4), ImVec2(x + 9, y + line * 0.5f + 5), rgbU32(e.rgb), 2.5f);
    draw->AddText(ImVec2(x + 14, y), secondary(), e.label);
    x += w;
  }
  ImGui::Dummy(ImVec2(width, y - start.y + line));
}

// The quarter track unrolled, start of the track at the left: every nibble
// its kind's colour, the sectors named over their address fields, the bits
// themselves once there is room for them, a flux track's cell times along
// the bottom, and the head where it is now. Scroll zooms about the pointer,
// a drag pans, a double click shows the whole track, and a click on a
// sector's nibbles picks the sector.
void DiskDrives::drawStrip(float width) {
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
  if (stripQt_ < 0 || stripSpan_ <= 0) {
    stripStart_ = 0;
    stripSpan_ = cells;
  }
  stripSpan_ = std::min(stripSpan_, cells);
  stripQt_ = detail_.quarterTrack;

  const ImGuiIO &io = ImGui::GetIO();
  if (hovered) {
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if (io.MouseWheel != 0) {
      const double at = (io.MousePos.x - p0.x) / width;
      const double cell = stripStart_ + at * stripSpan_;
      stripSpan_ = std::clamp(stripSpan_ * std::pow(0.8, io.MouseWheel), 24.0, cells);
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

  const double perCell = width / stripSpan_;
  auto X = [&](double cell) { return static_cast<float>(p0.x + (cell - stripStart_) * perCell); };
  const float labelTop = p0.y + 5;
  const float bandTop = p0.y + 22;
  const float bandBottom = p1.y - 16;
  const float timeTop = p1.y - 11;
  draw->PushClipRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), true);

  // The nibbles.
  int first = nibbleAtCell(nibbles, static_cast<uint32_t>(stripStart_));
  if (nibbles[first].start_bit > stripStart_) first = 0;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
  const float hexWidth = ImGui::CalcTextSize("FF").x;
  for (size_t i = first; i < nibbles.size(); i++) {
    const inspect::Nibble &n = nibbles[i];
    if (n.start_bit > stripStart_ + stripSpan_) break;
    const float x0 = X(n.start_bit);
    const float x1 = X(n.start_bit + n.cells);
    const uint8_t kind = n.kind;
    const bool noise = (kind & inspect::KIND_MASK) == inspect::INVALID && !(kind & inspect::BAD);
    const ImU32 colour = noise ? IM_COL32(0x8b, 0x94, 0x9e, 90) : rgbU32(kindColour(kind));
    const float gap = x1 - x0 >= 6 ? 1.0f : 0.0f;
    draw->AddRectFilled(ImVec2(x0, bandTop), ImVec2(std::max(x0 + 1, x1 - gap), bandBottom), colour,
                        x1 - x0 >= 6 ? 2.0f : 0.0f);
    if (perCell >= 2.5) {
      // The flux transitions: a nibble's ones, top bit first, and its zeros.
      for (int bit = 0; bit < 8; bit++) {
        if (!(n.value & (0x80 >> bit))) continue;
        const float x = X(n.start_bit + bit + 0.5);
        draw->AddLine(ImVec2(x, bandTop + 3), ImVec2(x, bandBottom - 3), IM_COL32(255, 255, 255, 170), 1.0f);
      }
    } else if (x1 - x0 >= hexWidth + 6) {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%02X", n.value);
      const ImU32 ink = (kind & inspect::KIND_MASK) == inspect::SYNC ? IM_COL32(255, 255, 255, 150) : IM_COL32(0, 0, 0, 190);
      centredText(draw, ImVec2((x0 + x1) * 0.5f, (bandTop + bandBottom) * 0.5f), ink, hex);
    }
  }
  ImGui::PopFont();

  // The sectors, named over their address fields.
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.75f);
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
  const Drive &d = drives_[inspected_];
  if (d.quarterTrack == detail_.quarterTrack) {
    const float x = X(d.rotation * cells);
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
}

// The sectors in the order they pass the head, each its logical number,
// coloured by how its fields read.
void DiskDrives::drawSectorChips(float width) {
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
    centredText(draw, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), picked ? IM_COL32(20, 20, 20, 255) : colour, label);
    if (hovered) {
      ImGui::SetTooltip("Sector %d, %d%s past the index\nVolume %d, track %d\n%s%s", s.sector, static_cast<int>(i) + 1,
                        i == 0 ? "st" : i == 1 ? "nd" : i == 2 ? "rd" : "th", s.volume, s.track, state,
                        s.address_ok ? "" : "\nAddress checksum failed");
    }
    x += w + 4;
  }
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, y - start.y + h));
}

// The picked sector's 256 bytes, sixteen to a row with their characters.
void DiskDrives::drawSectorBytes(float width, float height) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + width, p0.y + height);
  ImGui::Dummy(ImVec2(width, height));
  draw->AddRectFilled(p0, p1, PANE_BACKGROUND, 8.0f);
  border(draw, p0, p1, 8.0f);
  const auto &sectors = detail_.analysis.sectors;
  if (sectors.empty() || selectedSector_ < 0 || selectedSector_ >= static_cast<int>(sectors.size())) {
    centredText(draw, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), PANE_DIM, "No sector to show");
    return;
  }
  const inspect::Sector &s = sectors[selectedSector_];
  ImGui::PushFont(ui::monoFont(), 0.0f);
  const float cw = ImGui::CalcTextSize("0").x;
  const float lh = ImGui::GetTextLineHeight() + 1;
  float y = p0.y + 10;
  char head[96];
  const char *state = s.data == inspect::DataState::Good  ? "data good"
                      : s.data == inspect::DataState::Bad ? "data checksum failed"
                      : s.data == inspect::DataState::None ? "no data field"
                                                           : "data not verified";
  std::snprintf(head, sizeof(head), "SECTOR %d  VOLUME %d  TRACK %d  %s", s.sector, s.volume, s.track, state);
  draw->AddText(ImVec2(p0.x + 12, y), s.data == inspect::DataState::Good ? IM_COL32(97, 187, 70, 255) : IM_COL32(253, 184, 39, 255), head);
  y += lh + 6;
  if (s.data == inspect::DataState::None) {
    ImGui::PopFont();
    return;
  }
  for (int row = 0; row < 16; row++) {
    float x = p0.x + 12;
    char offset[8];
    std::snprintf(offset, sizeof(offset), "%02X", row * 16);
    draw->AddText(ImVec2(x, y), PANE_DIM, offset);
    x += cw * 4;
    for (int col = 0; col < 16; col++) {
      const uint8_t byte = s.bytes[row * 16 + col];
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%02X", byte);
      draw->AddText(ImVec2(x, y), byte == 0 ? PANE_DIM : PANE_TEXT, hex);
      x += cw * 3 + (col == 7 ? cw : 0);
    }
    x += cw;
    for (int col = 0; col < 16; col++) {
      // As the Apple II shows it: the high bit set is normal text.
      const uint8_t c = s.bytes[row * 16 + col] & 0x7F;
      const char glyph[2] = {c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.', 0};
      draw->AddText(ImVec2(x, y), c >= 0x20 && c < 0x7F ? IM_COL32(0, 157, 220, 255) : PANE_DIM, glyph);
      x += cw;
    }
    y += lh;
  }
  ImGui::PopFont();
}

// Every nibble round the track, sixteen to a row in their kinds' colours,
// the picked sector's own marked.
void DiskDrives::drawNibbles(float width, float height) {
  const auto &nibbles = detail_.analysis.nibbles;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(PANE_BACKGROUND));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
  ImGui::BeginChild("##nibbles", ImVec2(width, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushFont(ui::monoFont(), 0.0f);
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
      ImGui::Dummy(ImVec2(cw * 60, lh));
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

// The inspected track: its name and what was found on it, the strip, the
// sectors, and their bytes or the raw nibbles.
void DiskDrives::drawTrackDetail(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const Drive &d = drives_[inspected_];
  if (!d.hasDisk) {
    draw->AddText(ImVec2(start.x, start.y + 4), secondary(), "Insert a disk to see what is recorded on it.");
    ImGui::Dummy(ImVec2(width, DETAIL_HEIGHT));
    return;
  }

  ImGui::PushFont(ui::monoFont(), 18.0f);
  const std::string title = "Track " + trackLabel(selectedQt_);
  draw->AddText(start, text(), title.c_str());
  const float titleHeight = ImGui::GetTextLineHeight();
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
  draw->AddText(ImVec2(start.x, start.y + titleHeight + 2), secondary(), meta.c_str());
  if (!followHead_) {
    // Back to the head, as Follow head would.
    ImGui::SetCursorScreenPos(ImVec2(start.x + width - 110, start.y + 2));
    if (ui::Button("Go to Head", ImVec2(110, 0))) followHead_ = true;
  }
  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + titleHeight + ImGui::GetTextLineHeight() + 12));

  drawStrip(width);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.8f);
  ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                     "Scroll to zoom  ·  drag to pan  ·  double-click for the whole track");
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "Sectors, in the order they pass the head");
  drawSectorChips(width);
  ImGui::Spacing();
  ui::SegmentedControl("##pane", &pane_, {"Sector Data", "Nibbles"}, 240.0f);
  ImGui::Spacing();
  const float used = ImGui::GetCursorScreenPos().y - start.y;
  const float height = std::max(120.0f, DETAIL_HEIGHT - used);
  if (pane_ == 0) drawSectorBytes(width, height);
  else drawNibbles(width, height);
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, DETAIL_HEIGHT));
}

void DiskDrives::drawInspector() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 panel = ImGui::GetCursorScreenPos();
  const float height = 14 + 30 + 12 + DETAIL_HEIGHT + 14;
  const ImVec2 end(panel.x + TOTAL_WIDTH, panel.y + height);
  const bool dark = ui::isDark();
  draw->AddRectFilled(panel, end, dark ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  border(draw, panel, end, CARD_ROUNDING);

  // Which disk, what is on it, and how to look at it.
  const Drive &d = drives_[inspected_];
  const float left = panel.x + 16;
  const float right = end.x - 16;
  float y = panel.y + 14;
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
  const std::string title = "Drive " + std::to_string(inspected_ + 1);
  draw->AddText(ImVec2(left, y + 3), text(), title.c_str());
  const float titleWidth = ImGui::CalcTextSize(title.c_str()).x;
  ImGui::PopFont();
  std::string about = d.filename ? *d.filename : "Empty";
  if (d.summary.tracks > 0) {
    about += "  ·  " + std::to_string(d.summary.tracks) + " tracks  ·  " + d.summary.format;
    if (d.summary.sectors > 0) {
      about += "  ·  " + std::to_string(d.summary.good) + " of " + std::to_string(d.summary.sectors) + " sectors good";
    }
    if (d.summary.fluxTracks > 0) about += "  ·  " + std::to_string(d.summary.fluxTracks) + " flux";
  }
  draw->PushClipRect(ImVec2(left, y), ImVec2(right - 380, y + 30), true);
  draw->AddText(ImVec2(left + titleWidth + 12, y + 5), secondary(), about.c_str());
  draw->PopClipRect();

  const float switchWidth = ui::SwitchWidth("Follow head");
  ImGui::SetCursorScreenPos(ImVec2(right - switchWidth - 20 - 200, y));
  int mode = mode_ == PlatterMode::Timing ? 1 : 0;
  if (ui::SegmentedControl("##mode", &mode, {"Structure", "Timing"}, 200.0f)) {
    mode_ = mode == 1 ? PlatterMode::Timing : PlatterMode::Structure;
    for (Drive &drive : drives_) drive.paintStale = true;
  }
  ImGui::SameLine(0, 20);
  ui::Switch("Follow head", &followHead_);
  y += 30 + 12;

  // The platter and its key, then the track.
  ImGui::SetCursorScreenPos(ImVec2(left, y));
  drawPlatter(ImVec2(left, y), PLATTER_SIZE);
  ImGui::SetCursorScreenPos(ImVec2(left, y + PLATTER_SIZE + 10));
  drawLegend(PLATTER_SIZE);
  const float detailX = left + PLATTER_SIZE + 24;
  ImGui::SetCursorScreenPos(ImVec2(detailX, y));
  // A group, so each line in the column starts at its left edge rather than
  // the window's.
  ImGui::BeginGroup();
  drawTrackDetail(right - detailX);
  ImGui::EndGroup();

  ImGui::SetCursorScreenPos(panel);
  ImGui::Dummy(ImVec2(TOTAL_WIDTH, height));
}

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
    if (ui::RadioButton(label.c_str(), save_.format == format)) {
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

  if (ui::Button("Save…", ImVec2(110, 0), ui::ButtonKind::Primary)) {
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
  if (ui::Button("Don't Save", ImVec2(110, 0))) {
    eject(save_.drive);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
  if (ui::Button("OK", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}


void DiskDrives::draw(bool *open) {
  if (open && *open) {
    for (Drive &d : drives_) paintPlatters(d);
    if (ImGui::Begin("Disk Drives", open, ImGuiWindowFlags_AlwaysAutoResize)) {
      const ImVec2 top = ImGui::GetCursorScreenPos();
      ui::Disclosure("Inspector", &inspectorShown);
      bool sounds = emulation_.driveSounds().enabled();
      ImGui::SameLine();
      ImGui::SetCursorScreenPos(ImVec2(top.x + TOTAL_WIDTH - ui::SwitchWidth("Drive Sounds"), top.y));
      if (ui::Switch("Drive Sounds", &sounds)) emulation_.driveSounds().setEnabled(sounds);
      ImGui::Spacing();
      drawDeck(0);
      ImGui::SameLine(0, DECK_GAP);
      drawDeck(1);
      if (inspectorShown) {
        ImGui::Dummy(ImVec2(0, 2));
        drawInspector();
      }
    }
    ImGui::End();
  }
  // Outside the window, so an eject from a menu or a closed window still
  // gets its question answered.
  drawSavePopup();
  drawErrorPopup();
}

} // namespace a2e::native
