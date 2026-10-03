/*
 * disk_drives.cpp - The two 5.25" drives: what is in them, and the window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "disk_drives.hpp"
#include "drive_ui.hpp"
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

using namespace drive_ui;

namespace {

constexpr const char *SAVE_POPUP = "Save Disk";
constexpr const char *ERROR_POPUP = "Disk Error";
constexpr const char *BLANK_NAME = "Blank Disk.woz";

// Arcs per ring in the overview: half a degree each, as the browser reads it.
constexpr int OVERVIEW_BUCKETS = 720;
// A drive turns at 300 RPM, and when the motor stops the disk coasts down,
// losing half its speed in this long.
constexpr double TURNS_PER_SECOND = 5.0;
constexpr double SPIN_DOWN_HALF_LIFE = 0.35;
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
    : emulation_(emulation), platform_(platform), store_(std::move(mediaDirectory), "floppy"),
      inspector_(platform) {
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

int DiskDrives::driveAt(ImVec2 point) const {
  // Only a card drawn in the last frame or so: a closed window has none.
  if (deckFrame_ < 0 || ImGui::GetFrameCount() - deckFrame_ > 2) return -1;
  for (int drive = 0; drive < DRIVES; drive++) {
    const ImVec2 min = deckRects_[drive * 2];
    const ImVec2 max = deckRects_[drive * 2 + 1];
    if (point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y) return drive;
  }
  return -1;
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
  drive.spin.reset();
  drive.overviewRead = false;
  drive.overviewAt = -1;
  drive.overview.reset();
  drive.summary = DiskSummary{};
  drive.paintStale = true;
  drive.overviewSerial++; // whatever the inspector read of it is gone
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
  emulation_.poll(poll_, [&](host::MachineHost &host) {
    DiskController *disk = host.diskController();
    if (!disk) return;
    selectedDrive_ = disk->getSelectedDrive();
    motorOn_ = disk->isFiveInchMotorOn();
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
          d.overviewSerial++;
        }
      } else if (d.overviewRead) {
        d.overviewRead = false;
        d.overview.reset();
        d.summary = DiskSummary{};
        d.paintStale = true;
        d.overviewSerial++;
      }
    }
    // The inspector reads the drive it is on, by quarter track.
    inspector_.update(inspected(), [&](int qt) {
      const DiskImage *image = disk->getDiskImage(inspected_);
      return image ? readTrackDetail(*const_cast<DiskImage *>(image), qt) : TrackDetail{};
    }, now);
  });

  for (Drive &d : drives_) {
    // The picture's turn: the core's while it moves, coasting down after.
    // The motor runs a second after the program switches it off, as a real
    // Disk II's does; `active` already covers that second.
    d.spin.update(now, d.rotation, d.hasDisk, d.active, TURNS_PER_SECOND, SPIN_DOWN_HALF_LIFE);
    if (!d.hasDisk) {
      d.lastTrack = -1;
      continue;
    }
    // The stepper's click, on a whole track only.
    if (d.active && d.lastTrack >= 0 && d.track != d.lastTrack) emulation_.driveSounds().playSeek();
    d.lastTrack = d.track;
  }
}

// ---------------------------------------------------------------------------
// The platter
// ---------------------------------------------------------------------------

void DiskDrives::releasePlatters(Drive &drive) {
  if (platform_.releaseTexture) platform_.releaseTexture(drive.thumbnail);
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
  paintPlatter(rgba, THUMBNAIL_PIXELS, overview, PlatterMode::Structure);
  drive.thumbnail = platform_.makeTexture(rgba.data(), THUMBNAIL_PIXELS, THUMBNAIL_PIXELS);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

std::vector<std::string> DiskDrives::recentNames(int drive) const {
  std::vector<std::string> names;
  for (const RecentEntry &entry : store_.recent(drive)) names.push_back(entry.filename);
  return names;
}

void DiskDrives::insertRecent(int drive, size_t index) {
  const std::vector<RecentEntry> recent = store_.recent(drive);
  if (index >= recent.size()) return;
  if (auto image = store_.loadRecent(drive, recent[index])) {
    insertImage(drive, image->filename, image->data, true);
  } else {
    reportError("Could not read the recent disk " + recent[index].filename + ".");
  }
}

void DiskDrives::clearRecent(int drive) { store_.clearRecent(drive); }

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
  const ImVec2 end(card.x + CARD_WIDTH, card.y + CARD_HEIGHT);
  deckRects_[index * 2] = card;
  deckRects_[index * 2 + 1] = end;
  deckFrame_ = ImGui::GetFrameCount();
  const bool inspected = inspectorShown && index == inspected_;
  cardFrame(draw, card, end, index, inspected);

  // The disk.
  thumbnail(draw, ImVec2(card.x + CARD_PADDING + THUMBNAIL_SIZE * 0.5f, card.y + CARD_HEIGHT * 0.5f), d.thumbnail,
            d.hasDisk, d.spin.turn);

  // Its label and the light, what the inspector found on it, and where the
  // head is: the quarter track along a bar of the 35 tracks, a mark every
  // five.
  const float x0 = card.x + CARD_PADDING + THUMBNAIL_SIZE + 14;
  const float right = end.x - CARD_PADDING;
  const float top = card.y + CARD_PADDING;
  light(draw, ImVec2(right - 4, top + 11), 3.5f, headColour(true, d.writing), d.active);
  label(draw, ImVec2(x0, top), ImVec2(right - 16, top + 22), d.filename ? &*d.filename : nullptr, "No disk");

  float chipX = x0;
  const float chipY = top + 22 + 7;
  if (d.filename) {
    std::string extension = std::filesystem::path(*d.filename).extension().string();
    if (!extension.empty()) extension.erase(0, 1);
    for (char &c : extension) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!extension.empty()) chipX += chip(draw, ImVec2(chipX, chipY), extension.c_str(), text(0.75f)) + 5;
    if (d.summary.tracks > 0) {
      chipX += chip(draw, ImVec2(chipX, chipY), d.summary.format.c_str(), IM_COL32(0, 157, 220, 255)) + 5;
    }
    if (d.summary.fluxTracks > 0) chipX += chip(draw, ImVec2(chipX, chipY), "Flux", IM_COL32(150, 61, 151, 255)) + 5;
    if (d.summary.nonStandardTracks > 0 && d.summary.sectors > 0) {
      const std::string unknown = std::to_string(d.summary.nonStandardTracks) + " unknown";
      chipX += chip(draw, ImVec2(chipX, chipY), unknown.c_str(), IM_COL32(150, 61, 151, 255)) + 5;
    }
    if (d.summary.bad > 0) {
      const std::string bad = std::to_string(d.summary.bad) + " bad";
      chipX += chip(draw, ImVec2(chipX, chipY), bad.c_str(), IM_COL32(224, 58, 62, 255)) + 5;
    }
  } else {
    draw->AddText(ImVec2(x0, chipY), secondary(), "Drop a disk on this window");
  }

  const std::string where = d.hasDisk ? "Track " + trackLabel(d.quarterTrack) : "Track --";
  const std::string status = !d.hasDisk                   ? ""
                             : index != selectedDrive_    ? "Not selected"
                             : d.active && d.writing      ? "Writing"
                             : d.active                   ? "Reading"
                                                          : "Motor off";
  trackBar(draw, ImVec2(x0, chipY + 28), right - x0, d.hasDisk, (d.quarterTrack + 0.5f) / 140.0f,
           {5 / 35.0f, 10 / 35.0f, 15 / 35.0f, 20 / 35.0f, 25 / 35.0f, 30 / 35.0f}, where, status, d.active,
           d.writing);

  // The controls.
  ImGui::SetCursorScreenPos(ImVec2(x0, end.y - CARD_PADDING - ImGui::GetFrameHeight()));
  if (ui::Button("Insert", ImVec2(0, 0), d.filename ? ui::ButtonKind::Normal : ui::ButtonKind::Primary)) chooseDisk(index);
  ImGui::SameLine(0, 5);
  const std::string recentId = "##recent" + std::to_string(index);
  if (ui::Button("Recent")) ImGui::OpenPopup(recentId.c_str());
  drawRecentPopup(index);
  ImGui::SameLine(0, 5);
  if (ui::Button("Blank")) insertBlank(index);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Insert a blank disk");
  ImGui::SameLine(0, 5);
  ImGui::BeginDisabled(!d.filename);
  if (ui::Button("Eject")) requestEject(index);
  ImGui::EndDisabled();

  // With the inspector shown, a click anywhere else on the card inspects
  // this drive. Only the Inspector switch shows it.
  if (inspectorShown && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(card, end) &&
      !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    inspected_ = index;
  }

  if (dragOver && dragOver->x >= card.x && dragOver->x < end.x && dragOver->y >= card.y && dragOver->y < end.y) {
    dropHighlight(card, end, (d.filename ? "Drop to replace the disk in Drive " : "Drop to insert into Drive ") +
                                 std::to_string(index + 1));
  }

  ImGui::SetCursorScreenPos(card);
  ImGui::Dummy(ImVec2(CARD_WIDTH, CARD_HEIGHT));
  ImGui::EndGroup();
  ImGui::PopID();
}

// What the inspector shows: the drive it is on, the disk in it, and the
// controller as that drive sees it.
InspectedDisk DiskDrives::inspected() const {
  const Drive &d = drives_[inspected_];
  InspectedDisk disk;
  disk.drive = inspected_;
  disk.hasDisk = d.hasDisk;
  disk.title = "Drive " + std::to_string(inspected_ + 1);
  disk.about = d.filename ? *d.filename : "Empty";
  if (d.summary.tracks > 0) {
    disk.about += "  ·  " + std::to_string(d.summary.tracks) + " tracks  ·  " + d.summary.format;
    if (d.summary.sectors > 0) {
      disk.about += "  ·  " + std::to_string(d.summary.good) + " of " + std::to_string(d.summary.sectors) + " good";
    }
    if (d.summary.fluxTracks > 0) disk.about += "  ·  " + std::to_string(d.summary.fluxTracks) + " flux";
    if (d.summary.nonStandardTracks > 0 && d.summary.sectors > 0) {
      disk.about += "  ·  " + std::to_string(d.summary.nonStandardTracks) + " unknown";
    }
  }
  if (d.hasDisk && inspected_ == selectedDrive_) {
    char status[96];
    std::snprintf(status, sizeof(status), "PH %d%d%d%d  LATCH %02X  %s", phase_ & 1, (phase_ >> 1) & 1,
                  (phase_ >> 2) & 1, (phase_ >> 3) & 1, lastByte_,
                  d.active ? (d.writing ? "WRITING" : "READING") : "MOTOR OFF");
    disk.status = status;
  } else if (d.hasDisk) {
    disk.status = "NOT SELECTED";
  }
  disk.headRing = d.quarterTrack;
  disk.spin = d.spin.turn;
  disk.spinSpeed = d.spin.speed;
  disk.active = d.active;
  disk.writing = d.writing;
  disk.revision = d.revision;
  disk.overviewSerial = d.overviewSerial;
  disk.overview = d.overview ? &*d.overview : nullptr;
  return disk;
}

void DiskDrives::drawSavePopup() {
  if (save_.open) {
    ImGui::OpenPopup(SAVE_POPUP);
    save_.open = false;
  }
  dialogs_.placeNext();
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
  dialogs_.placeNext();
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
    // Named for the 3.5" drives beside it, with the id it always had, so a
    // saved layout still finds it.
    ui::BeforeWindow(WINDOW_NAME);
    if (ui::BeginWindow(WINDOW_NAME, open, ImGuiWindowFlags_AlwaysAutoResize)) {
      dialogs_.note();
      const ImVec2 top = ImGui::GetCursorScreenPos();
      ui::Switch("Inspector", &inspectorShown);
      bool sounds = emulation_.driveSounds().enabled();
      ImGui::SameLine();
      ImGui::SetCursorScreenPos(ImVec2(top.x + WINDOW_WIDTH - ui::SwitchWidth("Drive Sounds"), top.y));
      if (ui::Switch("Drive Sounds", &sounds)) emulation_.driveSounds().setEnabled(sounds);
      ImGui::Spacing();
      drawDeck(0);
      ImGui::SameLine(0, CARD_GAP);
      drawDeck(1);
      if (inspectorShown) {
        ImGui::Dummy(ImVec2(0, 2));
        inspector_.draw(inspected(), WINDOW_WIDTH);
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
