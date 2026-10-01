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

#include <cctype>
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

const ImVec4 ACTIVE_TEXT(0.38f, 0.73f, 0.27f, 1.0f);

std::string lower(std::string text) {
  for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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

// The SmartPort's activity is the card's, not a device's, so a transfer
// lights every device with an image in it, held for three frames.
void HardDrives::update() {
  bool activity = false;
  bool write = false;
  emulation_.withMachine([&](host::MachineHost &host) {
    SmartPortCard *card = host.smartPort();
    available_ = card != nullptr;
    if (!card) return;
    activity = card->hasActivity();
    write = card->isActivityWrite();
    if (activity) card->clearActivity();
  });
  for (Device &device : devices_) {
    if (activity && device.filename) {
      device.activityFrames = 3;
      device.lastWrite = write;
    } else if (device.activityFrames > 0) {
      device.activityFrames--;
    }
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

void HardDrives::drawDevice(int index) {
  Device &d = devices_[index];
  ImGui::PushID(index);
  ImGui::BeginGroup();

  // The activity light and the device's name.
  const ImVec2 led = ImGui::GetCursorScreenPos();
  const float radius = ImGui::GetTextLineHeight() * 0.3f;
  const ImVec2 center(led.x + radius + 1, led.y + ImGui::GetTextLineHeight() * 0.5f);
  const bool lit = d.activityFrames > 0;
  ImGui::GetWindowDrawList()->AddCircleFilled(
      center, radius,
      lit ? (d.lastWrite ? IM_COL32(224, 58, 62, 255) : IM_COL32(97, 187, 70, 255)) : IM_COL32(60, 60, 60, 255));
  ImGui::Dummy(ImVec2(radius * 2 + 4, ImGui::GetTextLineHeight()));
  ImGui::SameLine();
  ImGui::Text("Device %d", index + 1);

  const float width = 270.0f;
  const std::string name = d.filename.value_or("No Image");
  const ImVec2 start = ImGui::GetCursorScreenPos();
  ImGui::PushClipRect(start, ImVec2(start.x + width - 70, start.y + ImGui::GetTextLineHeight()), true);
  if (d.filename) ImGui::TextUnformatted(name.c_str());
  else ImGui::TextDisabled("%s", name.c_str());
  ImGui::PopClipRect();
  if (d.filename && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", name.c_str());
  if (d.filename) {
    ImGui::SameLine(0, 0);
    const std::string size = sizeText(d.size);
    ImGui::SetCursorScreenPos(ImVec2(start.x + width - ImGui::CalcTextSize(size.c_str()).x, start.y));
    ImGui::TextDisabled("%s", size.c_str());
  }

  if (ui::Button("Insert")) {
    if (!available_) {
      reportError(NOT_INSTALLED);
    } else {
      platform_.openFile("Insert an image into SmartPort device " + std::to_string(index + 1),
                         {"hdv", "po", "2mg"}, [this, index](const std::string &path) {
                           if (!path.empty()) insertFile(index, path);
                         });
    }
  }
  ImGui::SameLine();
  const std::string recentId = "##hdrecent" + std::to_string(index);
  if (ui::Button("Recent")) {
    if (!available_) reportError(NOT_INSTALLED);
    else ImGui::OpenPopup(recentId.c_str());
  }
  drawRecentPopup(index);
  ImGui::SameLine();
  ImGui::BeginDisabled(!d.filename);
  if (ui::Button("Eject")) requestEject(index);
  ImGui::EndDisabled();
  ImGui::Dummy(ImVec2(width, 0));

  ImGui::EndGroup();
  ImGui::PopID();
}

void HardDrives::draw(bool *open) {
  if (open && *open) {
    if (ImGui::Begin("SmartPort Drives", open, ImGuiWindowFlags_AlwaysAutoResize)) {
      drawDevice(0);
      ImGui::SameLine(0, 24);
      drawDevice(1);
      if (ImGui::GetTime() < noticeUntil_) {
        ImGui::Separator();
        ImGui::TextColored(ACTIVE_TEXT, "%s", notice_.c_str());
      }
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
