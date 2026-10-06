/*
 * disk35_drives.cpp - A IIgs's two 3.5" drives, and their window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "disk35_drives.hpp"

#include "emulation.hpp"
#include "drive_ui.hpp"
#include "ui_controls.hpp"

#include "cards/iwm/iwm.hpp"
#include "cards/iwm/sony_drive.hpp"
#include "disk-image/gcr35.hpp"

#include "imgui.h"
#include "ui_theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace a2e::native {

using namespace drive_ui;

namespace {

constexpr const char *ERROR_POPUP = "3.5\" Drive Error";
constexpr const char *EJECT_POPUP = "Eject 3.5\" Disk";
constexpr const char *NOT_IIGS = "Only a IIgs has 3.5\" drives. Switch machine to use one.";
const std::vector<std::string> EXTENSIONS = {"po", "2mg", "dsk", "hdv", "img", "woz"};

// Arcs per ring, as the 5.25" drives read them.
constexpr int OVERVIEW_BUCKETS = 720;
// A side is read again this soon at most while the machine writes to it:
// eighty tracks are a moment's work under the machine's lock.
constexpr double REREAD_SECONDS = 2.0;

uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// The block data inside an image, for the volume's name; a WOZ has none.
std::pair<const uint8_t *, size_t> blocksOf(const std::vector<uint8_t> &data) {
  if (data.size() >= 64 && std::memcmp(data.data(), "2IMG", 4) == 0) {
    const uint32_t offset = le32(data.data() + 0x18);
    const uint32_t length = le32(data.data() + 0x1C);
    if (static_cast<size_t>(offset) + length <= data.size()) return {data.data() + offset, length};
  }
  if (data.size() == GCR35::SIZE_800K || data.size() == GCR35::SIZE_400K) return {data.data(), data.size()};
  return {nullptr, 0};
}

} // namespace

bool Disk35Drives::isDisk35Image(const std::string &path) {
  // A 3.5" disk is under a megabyte in any of its formats, so the core is
  // asked about the whole file rather than a guess from its name.
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > 2 * 1024 * 1024) return false;
  const auto data = readFile(path);
  return data && SonyDrive::isDiskImage(data->data(), data->size());
}

Disk35Drives::Disk35Drives(Emulation &emulation, Platform &platform, std::string mediaDirectory)
    : emulation_(emulation), platform_(platform), store_(std::move(mediaDirectory), "disk35"),
      inspector_(platform) {}

Disk35Drives::~Disk35Drives() {
  for (Drive &d : drives_) releaseThumbnail(d);
}

void Disk35Drives::releaseThumbnail(Drive &drive) {
  if (platform_.releaseTexture) platform_.releaseTexture(drive.thumbnail);
  drive.thumbnail = ImTextureID_Invalid;
}

// A drive emptied or given a new disk. Its picture may already be in this
// frame's draw lists, so the texture is kept until the card is next painted,
// which releases it before drawing anything.
void Disk35Drives::clearDrive(Drive &drive) {
  const ImTextureID texture = drive.thumbnail;
  const uint32_t serial = drive.serial;
  drive = Drive{};
  drive.thumbnail = texture;
  drive.paintStale = true;
  drive.serial = serial + 1;
}

void Disk35Drives::paintThumbnail(Drive &drive) {
  if (!drive.paintStale || !platform_.makeTexture) return;
  releaseThumbnail(drive);
  drive.paintStale = false;
  if (!drive.filename) return;
  std::vector<uint8_t> rgba;
  paintPlatter(rgba, THUMBNAIL_PIXELS, drive.overview ? &*drive.overview : nullptr, PlatterMode::Structure);
  drive.thumbnail = platform_.makeTexture(rgba.data(), THUMBNAIL_PIXELS, THUMBNAIL_PIXELS);
}

void Disk35Drives::notice(const std::string &message) {
  notice_ = message;
  noticeUntil_ = ImGui::GetTime() + 6.0;
}

void Disk35Drives::reportError(const std::string &message) {
  error_ = message;
  openError_ = true;
}

void Disk35Drives::insertImage(int drive, const std::string &filename, const std::vector<uint8_t> &data,
                               bool remember, const std::string &path) {
  bool iigs = false;
  const bool ok = emulation_.withMachine([&](host::MachineHost &host) {
    iigs = host.has35Drives();
    return iigs && host.insert35Disk(drive, data.data(), data.size(), filename.c_str());
  });
  if (!iigs) {
    reportError(NOT_IIGS);
    return;
  }
  if (!ok) {
    reportError(filename + " is not a 3.5\" disk: an 800K or 400K image, a 2MG holding one, or a 3.5\" WOZ.");
    return;
  }
  Drive &d = drives_[drive];
  clearDrive(d);
  d.filename = filename;
  if (!path.empty()) d.path = path;
  d.size = data.size();
  const auto [blocks, size] = blocksOf(data);
  if (blocks) d.volume = summariseVolume(blocks, size, 1);
  if (remember) {
    store_.saveInserted(drive, filename, data, path);
    store_.addRecent(drive, filename, data, path);
  }
}

void Disk35Drives::chooseDisk(int drive) {
  if (!platform_.openFile) return;
  platform_.openFile("Insert a disk into 3.5\" drive " + std::to_string(drive + 1), EXTENSIONS,
                     [this, drive](const std::string &path) {
                       if (!path.empty()) insertFile(drive, path);
                     });
}

void Disk35Drives::insertFile(int drive, const std::string &path) {
  auto data = readFile(path);
  if (!data) {
    reportError("Could not read " + path + ".");
    return;
  }
  replace(drive, [this, drive, path, data = std::move(*data)] { insertImage(drive, baseName(path), data, true, path); });
}

void Disk35Drives::insertRecentEntry(int drive, const RecentEntry &entry) {
  auto image = store_.loadRecent(drive, entry);
  if (!image) {
    reportError(entry.path.empty() ? "Could not read the recent disk " + entry.filename + "."
                                   : entry.filename + " is no longer at " + entry.path + ".");
    return;
  }
  replace(drive, [this, drive, image = std::move(*image)] {
    insertImage(drive, image.filename, image.data, true, image.path);
  });
}

void Disk35Drives::restore() {
  const bool iigs = emulation_.withMachine([](host::MachineHost &host) { return host.has35Drives(); });
  if (!iigs) return;
  for (int drive = 0; drive < DRIVES; drive++) {
    const bool inserted = emulation_.withMachine([&](host::MachineHost &host) { return host.is35DiskInserted(drive); });
    if (inserted) continue;
    auto image = store_.loadInserted(drive);
    if (!image) continue;
    if (image->missing) {
      reportError(image->filename + " is no longer at " + image->path + ", so 3.5\" drive " +
                  std::to_string(drive + 1) + " is empty.");
      store_.clearInserted(drive);
      continue;
    }
    insertImage(drive, image->filename, image->data, false, image->path);
  }
}

void Disk35Drives::machineChanged() {
  // The remembered disks stay remembered: they are the IIgs's, and come back
  // with it.
  for (Drive &d : drives_) {
    clearDrive(d);
  }
  restore();
}

void Disk35Drives::syncWithMachine() {
  emulation_.withMachine([&](host::MachineHost &host) {
    for (int drive = 0; drive < DRIVES; drive++) {
      if (host.is35DiskInserted(drive)) {
        const std::string name = host.disk35Filename(drive);
        if (drives_[drive].filename != name) {
          clearDrive(drives_[drive]);
          drives_[drive].filename = name.empty() ? std::string("Restored disk") : name;
        }
      } else {
        clearDrive(drives_[drive]);
      }
    }
  });
}

int Disk35Drives::driveAt(ImVec2 point) const {
  if (cardFrame_ < 0 || ImGui::GetFrameCount() - cardFrame_ > 2) return -1;
  for (int drive = 0; drive < DRIVES; drive++) {
    const ImVec2 min = cardRects_[drive * 2];
    const ImVec2 max = cardRects_[drive * 2 + 1];
    if (point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y) return drive;
  }
  return -1;
}

int Disk35Drives::dropTarget() const {
  if (!drives_[0].filename) return 0;
  if (!drives_[1].filename) return 1;
  return 0;
}

// Exported and marked saved under one hold of the machine, so a write made in
// between is never counted as kept; the file is written after, outside it.
bool Disk35Drives::writeBack(int drive) {
  Drive &d = drives_[drive];
  if (!d.filename) return true;
  bool written = false;
  std::vector<uint8_t> data;
  emulation_.withMachine([&](host::MachineHost &host) {
    written = host.is35DiskModified(drive);
    if (!written || !d.path) return;
    size_t size = 0;
    const uint8_t *bytes = host.export35Disk(drive, &size);
    if (bytes && size) data.assign(bytes, bytes + size);
    host.mark35DiskSaved(drive);
  });
  d.modified = written && !d.path;
  if (!written) return !d.writeBackFailed;
  if (!d.path) return false;
  if (data.empty() || !writeFile(*d.path, data.data(), data.size())) {
    if (!d.writeBackFailed) reportError("Could not write " + *d.filename + " back to " + *d.path + ".");
    d.writeBackFailed = true;
    return false;
  }
  d.writeBackFailed = false;
  return true;
}

void Disk35Drives::writeBackAll() {
  for (int drive = 0; drive < DRIVES; drive++) writeBack(drive);
}

std::vector<std::string> Disk35Drives::unsavedDisks() {
  std::vector<std::string> names;
  for (int drive = 0; drive < DRIVES; drive++) {
    if (!writeBack(drive) && drives_[drive].filename) names.push_back(*drives_[drive].filename);
  }
  return names;
}

void Disk35Drives::replace(int drive, std::function<void()> insert) {
  if (!drives_[drive].filename || writeBack(drive)) {
    insert();
    return;
  }
  askEject_ = drive;
  askThen_ = std::move(insert);
  openAskEject_ = true;
}

void Disk35Drives::requestEject(int drive) {
  if (writeBack(drive)) {
    eject(drive);
    return;
  }
  askEject_ = drive;
  askThen_ = nullptr;
  openAskEject_ = true;
}

void Disk35Drives::saveThenEject(int drive, std::function<void()> then) {
  std::vector<uint8_t> data;
  emulation_.withMachine([&](host::MachineHost &host) {
    size_t size = 0;
    const uint8_t *bytes = host.export35Disk(drive, &size);
    if (bytes && size) data.assign(bytes, bytes + size);
  });
  if (data.empty()) {
    reportError("The disk in 3.5\" drive " + std::to_string(drive + 1) + " could not be read back.");
    return;
  }
  const std::string name = drives_[drive].filename.value_or("disk" + std::to_string(drive + 1) + ".po");
  platform_.saveFile("Save the disk in 3.5\" drive " + std::to_string(drive + 1), name, EXTENSIONS,
                     [this, drive, data, serial = drives_[drive].serial, then](const std::string &path) {
                       if (path.empty()) return; // kept in, as the other drives do
                       if (!writeFile(path, data.data(), data.size())) {
                         reportError("Could not write " + path + ".");
                         return;
                       }
                       // A disk put in while the panel was open is not this one.
                       if (drives_[drive].serial != serial) return;
                       eject(drive);
                       if (then) then();
                     });
}

void Disk35Drives::eject(int drive) {
  emulation_.withMachine([&](host::MachineHost &host) { host.eject35Disk(drive); });
  clearDrive(drives_[drive]);
  store_.clearInserted(drive);
}

// The machine ejected a disk: into Recent as it now is, changes and all.
void Disk35Drives::takeEjected(int drive) {
  std::vector<uint8_t> data;
  emulation_.withMachine([&](host::MachineHost &host) {
    size_t size = 0;
    const uint8_t *bytes = host.export35Ejected(drive, &size);
    if (bytes && size) data.assign(bytes, bytes + size);
    host.clear35Ejected(drive);
  });
  const std::string name = drives_[drive].filename.value_or("disk" + std::to_string(drive + 1) + ".po");
  const std::optional<std::string> path = drives_[drive].path;
  // Back to its file when it has one, and in Recent either way.
  if (!data.empty() && path && !writeFile(*path, data.data(), data.size())) {
    reportError("Could not write " + name + " back to " + *path + ".");
    store_.addRecent(drive, name, data);
  } else if (!data.empty()) {
    store_.addRecent(drive, name, data, path.value_or(""));
  }
  clearDrive(drives_[drive]);
  store_.clearInserted(drive);
  notice(name + " was ejected from drive " + std::to_string(drive + 1) + ". It is in Recent.");
}

void Disk35Drives::update() {
  const double now = ImGui::GetTime();
  std::array<bool, DRIVES> ejected{};
  emulation_.poll(poll_, [&](host::MachineHost &host) {
    available_ = host.has35Drives();
    if (!available_) return;
    for (int i = 0; i < DRIVES; i++) {
      Drive &d = drives_[i];
      SonyDrive &sony = host.iigs()->iwm().sonyDrive(i);
      ejected[i] = host.has35Ejected(i);
      d.spinning = host.is35MotorOn(i);
      d.modified = host.is35DiskModified(i);
      d.track = host.disk35Track(i);
      d.side = host.disk35Side(i);
      d.writeProtected = sony.isWriteProtected();
      d.rotation = sony.rotation();
      d.coreRevision = sony.revision();
      // The side under the head, read again when the head changes sides,
      // and a while after anything is written.
      if (d.filename && sony.hasDisk()) {
        const bool changed = sony.revision() != d.revision;
        if (!d.overview || d.overviewSide != d.side ||
            (changed && now - d.overviewAt >= REREAD_SECONDS)) {
          d.overview = parseOverview(sony.overview(d.side, OVERVIEW_BUCKETS));
          d.overviewSide = d.side;
          d.revision = sony.revision();
          d.overviewAt = now;
          d.paintStale = true;
          d.overviewSerial++;
        }
      }
    }
    // The inspector reads the drive it is on: a ring is half a track, on
    // the side under the head.
    SonyDrive &sony = host.iigs()->iwm().sonyDrive(inspected_);
    const int side = drives_[inspected_].side;
    inspector_.update(inspected(), [&](int ring) {
      DiskImage *image = sony.image();
      return image ? readTrackDetail(*image, (ring / 2) * 2 + side, inspect::Recording::ThreeAndAHalf)
                   : TrackDetail{};
    }, now);
  });
  for (int i = 0; i < DRIVES; i++) {
    if (ejected[i]) takeEjected(i);
  }
  // A written disk goes back to its file once its spindle has been stopped
  // for a second.
  for (int i = 0; i < DRIVES; i++) {
    Drive &d = drives_[i];
    if (!d.path || !d.modified || d.spinning || d.writeBackFailed) {
      d.idleSince = -1;
      continue;
    }
    if (d.idleSince < 0) d.idleSince = now;
    if (now - d.idleSince >= 1.0) {
      writeBack(i);
      d.idleSince = -1;
    }
  }
  // The pictures turn at the speed of the zone the head is in, 394rpm at the
  // outside to 590 at the inside, and coast down when the spindle stops.
  static constexpr double RPM[GCR35::ZONES] = {394, 429, 472, 525, 590};
  for (Drive &d : drives_) {
    const double turns = RPM[std::clamp(d.track / GCR35::TRACKS_PER_ZONE, 0, GCR35::ZONES - 1)] / 60.0;
    d.spin.update(now, d.rotation, d.filename.has_value(), d.spinning, turns);
  }
}

void Disk35Drives::drawRecentPopup(int index) {
  const std::string id = "##d35recent" + std::to_string(index);
  if (!ImGui::BeginPopup(id.c_str())) return;
  const std::vector<RecentEntry> recent = store_.recent(index);
  if (recent.empty()) {
    ImGui::TextDisabled("No recent disks");
  } else {
    for (const RecentEntry &entry : recent) {
      if (ImGui::Selectable(entry.filename.c_str())) {
        insertRecentEntry(index, entry);
      }
    }
    ImGui::Separator();
    if (ImGui::Selectable("Clear Recent")) store_.clearRecent(index);
  }
  ImGui::EndPopup();
}

void Disk35Drives::drawDrive(int index) {
  Drive &d = drives_[index];
  const bool present = d.filename.has_value();
  ImGui::PushID(index);
  ImGui::BeginGroup();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 card = ImGui::GetCursorScreenPos();
  // Takes the SameLine that put this card beside the other.
  ImGui::Dummy(ImVec2(0, 0));
  const ImVec2 end(card.x + CARD_WIDTH, card.y + CARD_HEIGHT);
  cardRects_[index * 2] = card;
  cardRects_[index * 2 + 1] = end;
  cardFrame_ = ImGui::GetFrameCount();
  const bool inspected = inspectorShown && index == inspected_;
  cardFrame(draw, card, end, index, inspected);

  // The side under the head, turning with the disk.
  thumbnail(draw, ImVec2(card.x + CARD_PADDING + THUMBNAIL_SIZE * 0.5f, card.y + CARD_HEIGHT * 0.5f), d.thumbnail,
            present, d.spin.turn);

  // Its label and the light, what is on it, and where the head is: the
  // track along a bar of the 80, the speed zones marked.
  const float x0 = card.x + CARD_PADDING + THUMBNAIL_SIZE + 14;
  const float right = end.x - CARD_PADDING;
  const float top = card.y + CARD_PADDING;
  light(draw, ImVec2(right - 4, top + 11), 3.5f, headColour(true, false), d.spinning);
  label(draw, ImVec2(x0, top), ImVec2(right - 16, top + 22), present ? &*d.filename : nullptr, "No disk");

  float chipX = x0;
  const float chipY = top + 22 + 7;
  if (present) {
    std::string extension = std::filesystem::path(*d.filename).extension().string();
    if (!extension.empty()) extension.erase(0, 1);
    for (char &c : extension) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!extension.empty()) chipX += chip(draw, ImVec2(chipX, chipY), extension.c_str(), text(0.75f)) + 5;
    if (d.volume.prodos) chipX += chip(draw, ImVec2(chipX, chipY), d.volume.name.c_str(), IM_COL32(0, 157, 220, 255)) + 5;
    if (d.writeProtected) chipX += chip(draw, ImVec2(chipX, chipY), "Locked", secondary()) + 5;
    if (d.modified) chipX += chip(draw, ImVec2(chipX, chipY), "Edited", accent()) + 5;
  } else {
    draw->AddText(ImVec2(x0, chipY), secondary(), "Drop an 800K disk on this window");
  }

  const std::string where = present ? "Track " + std::to_string(d.track) + ", side " + std::to_string(d.side)
                                    : "Track --";
  trackBar(draw, ImVec2(x0, chipY + 28), right - x0, present, (d.track + 0.5f) / GCR35::TRACKS,
           {0.2f, 0.4f, 0.6f, 0.8f}, where, present ? (d.spinning ? "Turning" : "Stopped") : "", d.spinning, false);

  // The controls.
  ImGui::SetCursorScreenPos(ImVec2(x0, end.y - CARD_PADDING - ImGui::GetFrameHeight()));
  if (ui::Button("Insert", ImVec2(0, 0), present ? ui::ButtonKind::Normal : ui::ButtonKind::Primary)) {
    chooseDisk(index);
  }
  ImGui::SameLine(0, 5);
  const std::string recentId = "##d35recent" + std::to_string(index);
  if (ui::Button("Recent")) ImGui::OpenPopup(recentId.c_str());
  drawRecentPopup(index);
  ImGui::SameLine(0, 5);
  ImGui::BeginDisabled(!present);
  if (ui::Button("Eject")) requestEject(index);
  ImGui::EndDisabled();

  // With the inspector shown, a click anywhere else on the card inspects
  // this drive.
  if (inspectorShown && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(card, end) &&
      !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    inspected_ = index;
  }

  if (dragOver && dragOver->x >= card.x && dragOver->x < end.x && dragOver->y >= card.y && dragOver->y < end.y) {
    dropHighlight(card, end, (present ? "Drop to replace the disk in Drive " : "Drop to insert into Drive ") +
                                 std::to_string(index + 1));
  }

  ImGui::SetCursorScreenPos(card);
  ImGui::Dummy(ImVec2(CARD_WIDTH, CARD_HEIGHT));
  ImGui::EndGroup();
  ImGui::PopID();
}

// What the inspector shows: the drive it is on and the side under its head.
InspectedDisk Disk35Drives::inspected() const {
  const Drive &d = drives_[inspected_];
  InspectedDisk disk;
  disk.drive = inspected_;
  disk.side = d.side;
  disk.threeAndAHalf = true;
  disk.hasDisk = d.filename.has_value() && d.overview.has_value();
  disk.title = "Drive " + std::to_string(inspected_ + 1);
  disk.about = d.filename ? *d.filename + "  ·  side " + std::to_string(d.side) : "Empty";
  if (d.overview) {
    const DiskSummary summary = summariseDisk(*d.overview);
    if (summary.sectors > 0) {
      disk.about += "  ·  " + std::to_string(summary.good) + " of " + std::to_string(summary.sectors) + " good";
    }
  }
  if (d.filename) disk.status = d.spinning ? "SPINDLE ON" : "SPINDLE OFF";
  disk.headRing = d.track * 2 + 1;
  disk.spin = d.spin.turn;
  disk.spinSpeed = d.spin.speed;
  disk.active = d.spinning;
  disk.revision = d.coreRevision;
  disk.overviewSerial = d.overviewSerial;
  disk.overview = d.overview ? &*d.overview : nullptr;
  return disk;
}

void Disk35Drives::draw(bool *open) {
  if (open && *open) {
    for (Drive &d : drives_) paintThumbnail(d);
    ui::BeforeWindow("3.5\" Drives");
    ui::KeepOnMonitor("3.5\" Drives");
    if (ui::BeginWindow("3.5\" Drives", open, ImGuiWindowFlags_AlwaysAutoResize)) {
      dialogs_.note();
      ui::Switch("Inspector", &inspectorShown);
      if (ImGui::GetTime() < noticeUntil_) {
        ImGui::SameLine(0, 16);
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", notice_.c_str());
      }
      ImGui::Spacing();
      drawDrive(0);
      ImGui::SameLine(0, CARD_GAP);
      drawDrive(1);
      if (inspectorShown) {
        ImGui::Dummy(ImVec2(0, 2));
        inspector_.draw(inspected(), WINDOW_WIDTH);
      }
    }
    ImGui::End();
  }

  if (openAskEject_) {
    ImGui::OpenPopup(EJECT_POPUP);
    openAskEject_ = false;
  }
  dialogs_.placeNext();
  if (ImGui::BeginPopupModal(EJECT_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const int drive = askEject_;
    const std::string name = drives_[drive].filename.value_or("the disk");
    ImGui::Text("The disk in 3.5\" drive %d has changes that are not saved.", drive + 1);
    ImGui::TextDisabled(askThen_ ? "Save %s before it is replaced?" : "Save %s before ejecting it?", name.c_str());
    ImGui::Spacing();
    if (ui::Button("Save…", ImVec2(110, 0), ui::ButtonKind::Primary)) {
      ImGui::CloseCurrentPopup();
      saveThenEject(drive, std::move(askThen_));
      askThen_ = nullptr;
    }
    ImGui::SameLine();
    if (ui::Button("Don't Save", ImVec2(110, 0))) {
      ImGui::CloseCurrentPopup();
      eject(drive);
      if (askThen_) askThen_();
      askThen_ = nullptr;
    }
    ImGui::SameLine();
    if (ui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
      askThen_ = nullptr;
    }
    ImGui::EndPopup();
  }

  if (openError_) {
    ImGui::OpenPopup(ERROR_POPUP);
    openError_ = false;
  }
  dialogs_.placeNext();
  if (ImGui::BeginPopupModal(ERROR_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
    ImGui::TextUnformatted(error_.c_str());
    ImGui::PopTextWrapPos();
    if (ui::Button("OK", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

} // namespace a2e::native
