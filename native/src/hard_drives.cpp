/*
 * hard_drives.cpp - The SmartPort's two block devices, and their window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "hard_drives.hpp"
#include "ui_controls.hpp"

#include "emulation.hpp"

#include "cards/smartport/smartport_card.hpp"

#include "imgui.h"
#include "ui_theme.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace a2e::native {

namespace {

constexpr const char *ERROR_POPUP = "SmartPort Error";
constexpr const char *NOT_INSTALLED =
    "There is no SmartPort to take an image. Fit a SmartPort card in the Expansion Slots "
    "window first.";

// A 140K image is a floppy whatever its extension; a ProDOS-order image
// bigger than one is a hard drive.
constexpr size_t FLOPPY_SIZE = 143360;

// The rainbow's green and red, as the drive lights in the status bar.
constexpr float PI = 3.14159265358979f;

constexpr ImU32 READ_COLOUR = IM_COL32(97, 187, 70, 255);
constexpr ImU32 WRITE_COLOUR = IM_COL32(224, 58, 62, 255);

// A device's card, and the parts in it, in points.
constexpr float CARD_WIDTH = 340.0f;
constexpr float CARD_PADDING = 14.0f;
constexpr float CARD_ROUNDING = 10.0f;
constexpr float CELL = 5.5f;
constexpr float CELL_GAP = 1.0f;
constexpr float GRAPH_HEIGHT = 26.0f;
// How long a cell glows after a transfer, and how soon the volume's
// figures are read again after a write.
constexpr float HEAT_SECONDS = 2.0f;
// How long the drive's light stays on after a transfer, so a single block shows.
constexpr double LIGHT_SECONDS = 0.15;
constexpr double VOLUME_REREAD_SECONDS = 1.0;

std::string lower(std::string text) {
  for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 blend(ImU32 a, ImU32 b, float t) {
  const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
  const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
  t = std::clamp(t, 0.0f, 1.0f);
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }

// A light with a soft glow round it while lit.
void light(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour, bool lit) {
  if (lit) {
    for (int i = 3; i >= 1; i--) {
      draw->AddCircleFilled(centre, radius + i * 2.0f, withAlpha(colour, 0.10f));
    }
    draw->AddCircleFilled(centre, radius, colour);
    draw->AddCircleFilled(ImVec2(centre.x - radius * 0.3f, centre.y - radius * 0.3f), radius * 0.35f,
                          IM_COL32(255, 255, 255, 110));
  } else {
    draw->AddCircleFilled(centre, radius, ui::isDark() ? IM_COL32(70, 70, 74, 255) : IM_COL32(190, 190, 194, 255));
  }
}

// A drive as Apple drew its external hard disks: a low case, a vent along
// the front and the light at one end.
void driveGlyph(ImDrawList *draw, ImVec2 at, ImVec2 size, bool present, bool lit, bool writing) {
  const bool dark = ui::isDark();
  const ImVec2 end(at.x + size.x, at.y + size.y);
  const ImU32 top = present ? (dark ? IM_COL32(92, 92, 98, 255) : IM_COL32(236, 234, 228, 255))
                            : (dark ? IM_COL32(58, 58, 62, 255) : IM_COL32(222, 222, 224, 255));
  const ImU32 bottom = present ? (dark ? IM_COL32(64, 64, 70, 255) : IM_COL32(206, 203, 195, 255))
                               : (dark ? IM_COL32(48, 48, 52, 255) : IM_COL32(206, 206, 208, 255));
  const float rounding = 6.0f;
  draw->AddRectFilled(ImVec2(at.x, at.y + 2), ImVec2(end.x, end.y + 2), IM_COL32(0, 0, 0, dark ? 70 : 28), rounding);
  draw->AddRectFilled(at, end, bottom, rounding);
  draw->AddRectFilled(at, ImVec2(end.x, at.y + size.y * 0.55f), top, rounding, ImDrawFlags_RoundCornersTop);
  draw->AddRect(at, end, dark ? IM_COL32(255, 255, 255, 30) : IM_COL32(0, 0, 0, 40), rounding, 0, 1.0f);
  // The vent: a row of short slots.
  const float ventY = at.y + size.y * 0.72f;
  for (int i = 0; i < 6; i++) {
    const float x = at.x + 8 + i * 5.0f;
    draw->AddLine(ImVec2(x, ventY - 2), ImVec2(x, ventY + 2), dark ? IM_COL32(0, 0, 0, 120) : IM_COL32(0, 0, 0, 55), 1.5f);
  }
  light(draw, ImVec2(end.x - 9, ventY), 2.6f, writing ? WRITE_COLOUR : READ_COLOUR, lit);
}

// A rectangle's outline in short dashes, for where an image would go.
void dashedRect(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour, float rounding) {
  const float dash = 5.0f;
  const float gap = 4.0f;
  auto line = [&](ImVec2 p, ImVec2 q) {
    const float length = std::hypot(q.x - p.x, q.y - p.y);
    const ImVec2 d((q.x - p.x) / length, (q.y - p.y) / length);
    for (float t = 0; t < length; t += dash + gap) {
      const float u = std::min(t + dash, length);
      draw->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * u, p.y + d.y * u), colour, 1.2f);
    }
  };
  const float r = rounding;
  line(ImVec2(a.x + r, a.y), ImVec2(b.x - r, a.y));
  line(ImVec2(a.x + r, b.y), ImVec2(b.x - r, b.y));
  line(ImVec2(a.x, a.y + r), ImVec2(a.x, b.y - r));
  line(ImVec2(b.x, a.y + r), ImVec2(b.x, b.y - r));
  draw->PathArcTo(ImVec2(a.x + r, a.y + r), r, PI, PI * 1.5f);
  draw->PathStroke(colour, 0, 1.2f);
  draw->PathArcTo(ImVec2(b.x - r, a.y + r), r, PI * 1.5f, PI * 2.0f);
  draw->PathStroke(colour, 0, 1.2f);
  draw->PathArcTo(ImVec2(b.x - r, b.y - r), r, 0, PI * 0.5f);
  draw->PathStroke(colour, 0, 1.2f);
  draw->PathArcTo(ImVec2(a.x + r, b.y - r), r, PI * 0.5f, PI);
  draw->PathStroke(colour, 0, 1.2f);
}

void centredText(ImDrawList *draw, ImVec2 centre, ImU32 colour, const char *line) {
  const ImVec2 size = ImGui::CalcTextSize(line);
  draw->AddText(ImVec2(std::floor(centre.x - size.x * 0.5f), std::floor(centre.y - size.y * 0.5f)), colour, line);
}

std::string rateText(double bytesPerSecond) {
  char text[32];
  if (bytesPerSecond >= 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f MB/s", bytesPerSecond / (1024.0 * 1024.0));
  else std::snprintf(text, sizeof(text), "%.0f KB/s", bytesPerSecond / 1024.0);
  return text;
}

std::string sizeText(size_t bytes) {
  char text[32];
  if (bytes >= 1024 * 1024) {
    std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
  } else {
    std::snprintf(text, sizeof(text), "%zu KB", bytes / 1024);
  }
  return text;
}

} // namespace

bool HardDrives::isBlockImage(const std::string &path, size_t size) {
  const std::string extension = lower(std::filesystem::path(path).extension().string());
  if (extension == ".hdv" || extension == ".2mg") return true;
  return extension == ".po" && size > FLOPPY_SIZE;
}

HardDrives::HardDrives(Emulation &emulation, Platform &platform, std::string mediaDirectory)
    : emulation_(emulation), platform_(platform), store_(std::move(mediaDirectory), "hard-drive") {
  libraryDirectory_ = platform_.resourceDirectory + "/disks";
  std::ifstream in(libraryDirectory_ + "/library.json");
  if (in) {
    std::stringstream json;
    json << in.rdbuf();
    for (const LibraryEntry &entry : parseLibrary(json.str())) {
      if (entry.type == "hard-drive") library_.push_back(entry);
    }
  }
}

void HardDrives::notice(const std::string &message) {
  notice_ = message;
  noticeUntil_ = ImGui::GetTime() + 6.0;
}

void HardDrives::reportError(const std::string &message) {
  error_ = message;
  openError_ = true;
}

void HardDrives::insertImage(int device, const std::string &filename,
                             const std::vector<uint8_t> &data, bool remember) {
  bool installed = false;
  bool pending = false;
  const bool ok = emulation_.withMachine([&](host::MachineHost &host) {
    installed = host.smartPort() != nullptr;
    if (!installed) return false;
    const bool inserted = host.insertBlockImage(device, data.data(), data.size(), filename.c_str());
    pending = inserted && host.isSmartPortROMPending();
    return inserted;
  });
  if (!installed) {
    reportError(NOT_INSTALLED);
    return;
  }
  if (!ok) {
    reportError("Could not load the SmartPort image " + filename + ".");
    return;
  }
  devices_[device] = Device{};
  devices_[device].filename = filename;
  devices_[device].size = data.size();
  if (remember) {
    store_.saveInserted(device, filename, data);
    store_.addRecent(device, filename, data);
  }
  if (pending) notice("Image inserted. Press Ctrl+Reset or Reboot to start from it.");
}

void HardDrives::insertFile(int device, const std::string &path) {
  auto data = readFile(path);
  if (!data) {
    reportError("Could not read " + path + ".");
    return;
  }
  insertImage(device, baseName(path), *data, true);
}

void HardDrives::restore() {
  for (int device = 0; device < DEVICES; device++) {
    if (auto image = store_.loadInserted(device)) {
      // Quietly: a machine without a SmartPort keeps the record for when it has one.
      const bool installed = emulation_.withMachine([](host::MachineHost &host) { return host.smartPort() != nullptr; });
      if (installed) insertImage(device, image->filename, image->data, false);
    }
  }
}

void HardDrives::machineChanged() {
  watched_ = nullptr; // the old machine's card is gone, and a new one may take its address
  transfers_.clear();
  for (int device = 0; device < DEVICES; device++) {
    devices_[device] = Device{};
    store_.clearInserted(device);
  }
}

void HardDrives::syncWithMachine() {
  emulation_.withMachine([&](host::MachineHost &host) {
    for (int device = 0; device < DEVICES; device++) {
      if (host.isBlockImageInserted(device)) {
        if (!devices_[device].filename) devices_[device].filename = host.blockImageFilename(device);
      } else {
        devices_[device] = Device{};
      }
    }
  });
}

int HardDrives::deviceAt(ImVec2 point) const {
  if (cardFrame_ < 0 || ImGui::GetFrameCount() - cardFrame_ > 2) return -1;
  for (int device = 0; device < DEVICES; device++) {
    const ImVec2 min = cardRects_[device * 2];
    const ImVec2 max = cardRects_[device * 2 + 1];
    if (point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y) return device;
  }
  return -1;
}

int HardDrives::dropTarget() const {
  if (!devices_[0].filename) return 0;
  if (!devices_[1].filename) return 1;
  return 0;
}

void HardDrives::requestEject(int device) {
  std::vector<uint8_t> data;
  emulation_.withMachine([&](host::MachineHost &host) {
    if (!host.isBlockImageModified(device)) return;
    size_t size = 0;
    const uint8_t *bytes = host.exportBlockImage(device, &size);
    if (bytes && size) data.assign(bytes, bytes + size);
  });
  if (data.empty()) {
    eject(device);
    return;
  }
  std::string name = devices_[device].filename.value_or("harddrive" + std::to_string(device + 1) + ".hdv");
  if (name.find('.') == std::string::npos) name += ".hdv";
  platform_.saveFile("Save the image in SmartPort device " + std::to_string(device + 1), name,
                     {"hdv", "po", "2mg"}, [this, device, data](const std::string &path) {
                       if (path.empty()) return; // kept in, as the floppies are
                       if (!writeFile(path, data.data(), data.size())) {
                         reportError("Could not write " + path + ".");
                         return;
                       }
                       eject(device);
                     });
}

void HardDrives::eject(int device) {
  emulation_.withMachine([&](host::MachineHost &host) { host.ejectBlockImage(device); });
  devices_[device] = Device{};
  store_.clearInserted(device);
}

HardDrives::~HardDrives() {
  emulation_.withMachine([&](host::MachineHost &host) {
    if (watched_ && host.smartPort() == watched_) watched_->setTransferCallback(nullptr);
  });
}

void HardDrives::watchTransfers(SmartPortCard *card) {
  if (card == watched_) return;
  watched_ = card;
  transfers_.clear();
  card->setTransferCallback([this](uint8_t op, int device, uint32_t block, uint32_t) {
    // 1 is a read and 2 a write; a status call moves no block.
    if ((op == 1 || op == 2) && device >= 0 && device < DEVICES && transfers_.size() < 65536) {
      transfers_.push_back(Transfer{device, block, op == 2});
    }
  });
}

// Advance the graph to now, then give each transfer since the last frame to
// its device's map and graph.
void HardDrives::applyTransfers(double now) {
  const long bin = static_cast<long>(std::floor(now / HISTORY_BIN_SECONDS));
  if (historyBin_ < 0) historyBin_ = bin;
  const long steps = std::min<long>(bin - historyBin_, HISTORY_BINS);
  for (long step = 0; step < steps; step++) {
    for (Device &d : devices_) {
      std::rotate(d.readHistory.begin(), d.readHistory.begin() + 1, d.readHistory.end());
      std::rotate(d.writeHistory.begin(), d.writeHistory.begin() + 1, d.writeHistory.end());
      d.readHistory.back() = 0;
      d.writeHistory.back() = 0;
    }
  }
  historyBin_ = bin;

  for (const Transfer &t : transfers_) {
    Device &d = devices_[t.device];
    if (!d.filename) continue;
    const uint32_t total = d.volume.totalBlocks ? d.volume.totalBlocks : static_cast<uint32_t>(d.size / 512);
    const int cell = cellForBlock(t.block, total, MAP_CELLS);
    uint16_t &count = t.write ? d.writeHistory.back() : d.readHistory.back();
    if (count < UINT16_MAX) count++;
    d.lightAt = now;
    d.lightWrite = t.write;
    if (t.write) {
      d.writeHeat[cell] = 1.0f;
      d.volumeStale = true;
    } else {
      d.readHeat[cell] = 1.0f;
    }
  }
  transfers_.clear();
}

// The volume's name and figures, read on insert and again a moment after
// the machine writes to it.
void HardDrives::readVolumes(double now) {
  for (int index = 0; index < DEVICES; index++) {
    Device &d = devices_[index];
    if (!d.filename || !watched_) continue;
    const bool due = d.volumeReadAt < 0 || (d.volumeStale && now - d.volumeReadAt >= VOLUME_REREAD_SECONDS);
    if (!due) continue;
    size_t size = 0;
    const uint8_t *blocks = watched_->getBlockData(index, &size);
    d.volume = summariseVolume(blocks, size, MAP_CELLS);
    d.volumeStale = false;
    d.volumeReadAt = now;
  }
}

void HardDrives::update() {
  const double now = ImGui::GetTime();
  bool activity = false;
  bool write = false;
  emulation_.withMachine([&](host::MachineHost &host) {
    SmartPortCard *card = host.smartPort();
    available_ = card != nullptr;
    if (!card) {
      watched_ = nullptr;
      transfers_.clear();
      return;
    }
    watchTransfers(card);
    // The SmartPort's light is the card's, not a device's, so a transfer
    // lights every device with an image in it.
    activity = card->hasActivity();
    write = card->isActivityWrite();
    if (activity) card->clearActivity();
    for (int index = 0; index < DEVICES; index++) {
      if (const BlockDevice *device = card->getDevice(index); device && device->isLoaded()) {
        devices_[index].modified = device->isModified();
        devices_[index].writeProtected = device->isWriteProtected();
      }
    }
    applyTransfers(now);
    readVolumes(now);

    location_.clear();
    if (host.machineId() == MachineId::AppleIIgs) {
      location_ = "Built in, slot 5";
    } else {
      for (int slot = 1; slot <= 7; slot++) {
        if (host.slotCard(slot) == "smartport") location_ = "Slot " + std::to_string(slot);
      }
    }
  });

  const float cool = std::exp(-static_cast<float>(now - lastUpdate_) * 3.0f / HEAT_SECONDS);
  lastUpdate_ = now;
  for (Device &device : devices_) {
    if (activity && device.filename) {
      device.activityFrames = 3;
      device.lastWrite = write;
    } else if (device.activityFrames > 0) {
      device.activityFrames--;
    }
    for (float &heat : device.readHeat) heat *= cool;
    for (float &heat : device.writeHeat) heat *= cool;
  }
}

void HardDrives::drawRecentPopup(int index) {
  const std::string id = "##hdrecent" + std::to_string(index);
  if (!ImGui::BeginPopup(id.c_str())) return;
  const std::vector<RecentEntry> recent = store_.recent(index);
  if (recent.empty()) {
    ImGui::TextDisabled("No recent images");
  } else {
    for (const RecentEntry &entry : recent) {
      if (ImGui::Selectable(entry.filename.c_str())) {
        if (auto image = store_.loadRecent(index, entry)) {
          insertImage(index, image->filename, image->data, true);
        } else {
          reportError("Could not read the recent image " + entry.filename + ".");
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

// Which SmartPort this is, and anything the last insert needs saying.
void HardDrives::drawHeader() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float width = CARD_WIDTH * DEVICES + ImGui::GetStyle().ItemSpacing.x * 2;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const std::string where = location_.empty() ? "SmartPort" : "SmartPort  ·  " + location_;
  draw->AddText(at, secondary(), where.c_str());
  ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));

  if (ImGui::GetTime() < noticeUntil_) {
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight() + 6;
    const ImVec2 max(min.x + width, min.y + height);
    draw->AddRectFilled(min, max, withAlpha(accent, 0.14f), 8.0f);
    draw->AddRect(min, max, withAlpha(accent, 0.35f), 8.0f);
    light(draw, ImVec2(min.x + 14, min.y + height * 0.5f), 3.0f, accent, true);
    draw->AddText(ImVec2(min.x + 26, min.y + (height - ImGui::GetTextLineHeight()) * 0.5f), text(), notice_.c_str());
    ImGui::Dummy(ImVec2(width, height));
  }
}

void HardDrives::drawDevice(int index) {
  Device &d = devices_[index];
  const VolumeSummary &volume = d.volume;
  const bool present = d.filename.has_value();
  const bool lit = ImGui::GetTime() - d.lightAt < LIGHT_SECONDS;
  ImGui::PushID(index);
  ImGui::BeginGroup();

  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float line = ImGui::GetTextLineHeight();
  const float inner = CARD_WIDTH - CARD_PADDING * 2;
  const float headerHeight = 38.0f;
  const float bodyHeight = 28.0f + 12.0f + (MAP_ROWS * (CELL + CELL_GAP) - CELL_GAP) + 12.0f + line + 5.0f + GRAPH_HEIGHT;
  const float cardHeight =
      CARD_PADDING + headerHeight + 14.0f + bodyHeight + 14.0f + ImGui::GetFrameHeight() + CARD_PADDING;
  const ImVec2 card = ImGui::GetCursorScreenPos();
  const ImVec2 cardEnd(card.x + CARD_WIDTH, card.y + cardHeight);
  cardRects_[index * 2] = card;
  cardRects_[index * 2 + 1] = cardEnd;
  cardFrame_ = ImGui::GetFrameCount();
  // Takes the SameLine that put this card beside the other. The card is
  // drawn rather than laid out, so its first item is the Insert button,
  // which would otherwise take it and set the row for the buttons after it
  // at the top of the card.
  ImGui::Dummy(ImVec2(0, 0));
  const bool dark = ui::isDark();
  draw->AddRectFilled(card, cardEnd, dark ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  draw->AddRect(card, cardEnd, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);

  // The drive, then the volume's name and what holds it.
  const ImVec2 glyph(card.x + CARD_PADDING, card.y + CARD_PADDING + 4);
  driveGlyph(draw, glyph, ImVec2(46, 28), present, lit, d.lightWrite);
  const float textX = glyph.x + 46 + 12;
  const float textWidth = card.x + CARD_WIDTH - CARD_PADDING - textX;
  const std::string title = !present          ? "Device " + std::to_string(index + 1)
                            : volume.prodos   ? volume.name
                                              : std::filesystem::path(*d.filename).stem().string();
  ImGui::PushFont(ui::monoFont(), 16.0f);
  draw->PushClipRect(ImVec2(textX, card.y), ImVec2(textX + textWidth, cardEnd.y), true);
  draw->AddText(ImVec2(textX, card.y + CARD_PADDING), present ? text() : secondary(), title.c_str());
  const float titleHeight = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  std::string sub = present ? *d.filename + "  ·  " + sizeText(d.size) : "No image";
  draw->AddText(ImVec2(textX, card.y + CARD_PADDING + titleHeight + 3), secondary(), sub.c_str());
  draw->PopClipRect();

  // Edited and Locked, as small capsules at the top right.
  float badgeRight = card.x + CARD_WIDTH - CARD_PADDING;
  auto badge = [&](const char *label, ImU32 colour) {
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.8f);
    const ImVec2 size = ImGui::CalcTextSize(label);
    const ImVec2 max(badgeRight, card.y + CARD_PADDING + size.y + 4);
    const ImVec2 min(max.x - size.x - 12, card.y + CARD_PADDING);
    draw->AddRectFilled(min, max, withAlpha(colour, 0.16f), (max.y - min.y) * 0.5f);
    draw->AddText(ImVec2(min.x + 6, min.y + 2), colour, label);
    ImGui::PopFont();
    badgeRight = min.x - 6;
  };
  if (present && d.writeProtected) badge("Locked", secondary());
  if (present && d.modified) badge("Edited", ImGui::GetColorU32(ImGuiCol_CheckMark));

  const float bodyTop = card.y + CARD_PADDING + headerHeight + 14.0f;
  const ImVec2 body(card.x + CARD_PADDING, bodyTop);
  const ImVec2 bodyEnd(body.x + inner, bodyTop + bodyHeight);
  if (!present) {
    // Where an image would go.
    dashedRect(draw, body, bodyEnd, text(dark ? 0.22f : 0.20f), 8.0f);
    const ImVec2 centre((body.x + bodyEnd.x) * 0.5f, (body.y + bodyEnd.y) * 0.5f);
    centredText(draw, ImVec2(centre.x, centre.y - line * 0.6f), text(0.75f), "No image");
    centredText(draw, ImVec2(centre.x, centre.y + line * 0.6f), secondary(), "Drop a .hdv, .po or .2mg on this window");
  } else {
    // How full it is.
    float y = bodyTop;
    const float fraction = volume.prodos && volume.totalBlocks
                               ? 1.0f - static_cast<float>(volume.freeBlocks) / volume.totalBlocks
                               : 0.0f;
    draw->AddRectFilled(ImVec2(body.x, y), ImVec2(bodyEnd.x, y + 6), text(0.10f), 3.0f);
    if (fraction > 0) {
      const ImU32 fill = fraction >= 0.9f ? IM_COL32(245, 130, 31, 255) : ImGui::GetColorU32(ImGuiCol_CheckMark);
      draw->AddRectFilled(ImVec2(body.x, y), ImVec2(body.x + std::max(6.0f, inner * fraction), y + 6), fill, 3.0f);
    }
    y += 12;
    char left[96];
    char right[32];
    if (volume.prodos) {
      std::snprintf(left, sizeof(left), "%d item%s  ·  %s free", volume.entries, volume.entries == 1 ? "" : "s",
                    sizeText(static_cast<size_t>(volume.freeBlocks) * 512).c_str());
      std::snprintf(right, sizeof(right), "%.0f%% used", fraction * 100.0f);
    } else {
      std::snprintf(left, sizeof(left), "Not a ProDOS volume");
      std::snprintf(right, sizeof(right), "%u blocks", volume.totalBlocks);
    }
    draw->AddText(ImVec2(body.x, y), secondary(), left);
    draw->AddText(ImVec2(bodyEnd.x - ImGui::CalcTextSize(right).x, y), secondary(), right);
    y += 16 + 12;

    // The volume, block 0 at the top left: how full each part is, and
    // where the machine has just read or written.
    const ImVec2 map(body.x, y);
    const ImU32 freeColour = text(dark ? 0.08f : 0.06f);
    const ImU32 fullColour = text(dark ? 0.45f : 0.38f);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hovered = -1;
    for (int cell = 0; cell < MAP_CELLS; cell++) {
      const int row = cell / MAP_COLUMNS;
      const int column = cell % MAP_COLUMNS;
      const ImVec2 a(map.x + column * (CELL + CELL_GAP), map.y + row * (CELL + CELL_GAP));
      const ImVec2 b(a.x + CELL, a.y + CELL);
      ImU32 colour = volume.prodos ? blend(freeColour, fullColour, volume.used[cell]) : text(0.14f);
      // The square root keeps a cooling cell visible for most of its time.
      colour = blend(colour, READ_COLOUR, std::sqrt(d.readHeat[cell]));
      colour = blend(colour, WRITE_COLOUR, std::sqrt(d.writeHeat[cell]));
      draw->AddRectFilled(a, b, colour, 1.5f);
      if (mouse.x >= a.x && mouse.x < b.x + CELL_GAP && mouse.y >= a.y && mouse.y < b.y + CELL_GAP) hovered = cell;
    }
    y += MAP_ROWS * (CELL + CELL_GAP) - CELL_GAP;
    if (hovered >= 0 && ImGui::IsWindowHovered()) {
      const uint32_t total = volume.totalBlocks ? volume.totalBlocks : static_cast<uint32_t>(d.size / 512);
      const uint32_t first = cellFirstBlock(hovered, total, MAP_CELLS);
      const uint32_t last = cellFirstBlock(hovered + 1, total, MAP_CELLS) - 1;
      const ImVec2 a(map.x + (hovered % MAP_COLUMNS) * (CELL + CELL_GAP), map.y + (hovered / MAP_COLUMNS) * (CELL + CELL_GAP));
      draw->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(a.x + CELL + 1, a.y + CELL + 1), text(0.9f), 2.0f, 0, 1.2f);
      if (volume.prodos) {
        ImGui::SetTooltip("Blocks %u–%u\n%.0f%% in use", first, last, volume.used[hovered] * 100.0f);
      } else {
        ImGui::SetTooltip("Blocks %u–%u", first, last);
      }
    }
    y += 12;

    // The last four seconds, blocks per bin: reads in green, writes on top
    // of them in red, and the last second's rates beside the title.
    uint32_t readSecond = 0;
    uint32_t writeSecond = 0;
    const int perSecond = static_cast<int>(std::lround(1.0 / HISTORY_BIN_SECONDS));
    for (int i = HISTORY_BINS - perSecond; i < HISTORY_BINS; i++) {
      readSecond += d.readHistory[i];
      writeSecond += d.writeHistory[i];
    }
    draw->AddText(ImVec2(body.x, y), secondary(), "Activity");
    ImGui::PushFont(ui::monoFont(), 0.0f);
    const std::string rates = "R " + rateText(readSecond * 512.0) + "   W " + rateText(writeSecond * 512.0);
    draw->AddText(ImVec2(bodyEnd.x - ImGui::CalcTextSize(rates.c_str()).x, y), secondary(), rates.c_str());
    ImGui::PopFont();
    y += line + 5;
    const ImVec2 graph(body.x, y);
    const ImVec2 graphEnd(bodyEnd.x, y + GRAPH_HEIGHT);
    draw->AddRectFilled(graph, graphEnd, text(dark ? 0.05f : 0.035f), 5.0f);
    int peak = 8;
    for (int i = 0; i < HISTORY_BINS; i++) peak = std::max(peak, d.readHistory[i] + d.writeHistory[i]);
    const float slot = inner / HISTORY_BINS;
    const float usable = GRAPH_HEIGHT - 4;
    for (int i = 0; i < HISTORY_BINS; i++) {
      const float x = graph.x + i * slot + slot * 0.15f;
      const float readHeight = usable * d.readHistory[i] / peak;
      const float writeHeight = usable * d.writeHistory[i] / peak;
      const float bottom = graphEnd.y - 2;
      if (readHeight > 0) {
        draw->AddRectFilled(ImVec2(x, bottom - std::max(readHeight, 1.5f)), ImVec2(x + slot * 0.7f, bottom),
                            withAlpha(READ_COLOUR, 0.85f), 1.0f);
      }
      if (writeHeight > 0) {
        draw->AddRectFilled(ImVec2(x, bottom - readHeight - std::max(writeHeight, 1.5f)),
                            ImVec2(x + slot * 0.7f, bottom - readHeight), withAlpha(WRITE_COLOUR, 0.9f), 1.0f);
      }
    }
  }

  // The controls, and which device this is.
  const float buttonsY = cardEnd.y - CARD_PADDING - ImGui::GetFrameHeight();
  ImGui::SetCursorScreenPos(ImVec2(card.x + CARD_PADDING, buttonsY));
  if (ui::Button("Insert", ImVec2(0, 0), present ? ui::ButtonKind::Normal : ui::ButtonKind::Primary)) {
    platform_.openFile("Insert an image into SmartPort device " + std::to_string(index + 1),
                       {"hdv", "po", "2mg"}, [this, index](const std::string &path) {
                         if (!path.empty()) insertFile(index, path);
                       });
  }
  ImGui::SameLine();
  const std::string recentId = "##hdrecent" + std::to_string(index);
  if (ui::Button("Recent")) ImGui::OpenPopup(recentId.c_str());
  drawRecentPopup(index);
  ImGui::SameLine();
  ImGui::BeginDisabled(!present);
  if (ui::Button("Eject")) requestEject(index);
  ImGui::EndDisabled();
  const std::string unit = "Device " + std::to_string(index + 1);
  const float unitWidth = ImGui::CalcTextSize(unit.c_str()).x;
  draw->AddText(ImVec2(cardEnd.x - CARD_PADDING - unitWidth, buttonsY + ImGui::GetStyle().FramePadding.y),
                secondary(), present ? unit.c_str() : "");

  // A drag of files over the card: where the image would go.
  if (dragOver && dragOver->x >= card.x && dragOver->x < cardEnd.x && dragOver->y >= card.y &&
      dragOver->y < cardEnd.y) {
    ImDrawList *top = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    top->AddRectFilled(card, cardEnd, withAlpha(accent, 0.16f), CARD_ROUNDING);
    top->AddRect(card, cardEnd, accent, CARD_ROUNDING, 0, 2.5f);
    const std::string prompt = (present ? "Drop to replace the image in Device " : "Drop to insert into Device ") +
                               std::to_string(index + 1);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
    const ImVec2 size = ImGui::CalcTextSize(prompt.c_str());
    const ImVec2 middle((card.x + cardEnd.x) * 0.5f, (card.y + cardEnd.y) * 0.5f);
    top->AddRectFilled(ImVec2(middle.x - size.x * 0.5f - 16, middle.y - size.y * 0.5f - 9),
                       ImVec2(middle.x + size.x * 0.5f + 16, middle.y + size.y * 0.5f + 9), accent, 20.0f);
    top->AddText(ImVec2(middle.x - size.x * 0.5f, middle.y - size.y * 0.5f), IM_COL32_WHITE, prompt.c_str());
    ImGui::PopFont();
  }

  ImGui::SetCursorScreenPos(card);
  ImGui::Dummy(ImVec2(CARD_WIDTH, cardHeight));
  ImGui::EndGroup();
  ImGui::PopID();
}

void HardDrives::draw(bool *open) {
  if (open && *open) {
    if (ImGui::Begin("SmartPort Drives", open, ImGuiWindowFlags_AlwaysAutoResize)) {
      drawHeader();
      drawDevice(0);
      ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
      drawDevice(1);
    }
    ImGui::End();
  }

  if (openError_) {
    ImGui::OpenPopup(ERROR_POPUP);
    openError_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
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
