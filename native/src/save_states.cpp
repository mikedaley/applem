/*
 * save_states.cpp - The Save States window: an autosave and five slots
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "save_states.hpp"
#include "ui_controls.hpp"

#include "emulation.hpp"
#include "media_store.hpp"

#include "machine/machine_profile.hpp"

#include "imgui.h"

#include <ctime>

namespace a2e::native {

namespace {

constexpr const char *SWITCH_POPUP = "Load State";
constexpr const char *ERROR_POPUP = "Save State Error";
constexpr double AUTOSAVE_SECONDS = 5.0;

const ImVec4 GREEN(0.38f, 0.73f, 0.27f, 1.0f);

std::string formatTime(int64_t milliseconds) {
  const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
  std::tm local{};
  localtime_r(&seconds, &local);
  char text[64];
  std::strftime(text, sizeof(text), "%e %b %Y, %H:%M", &local);
  return text;
}

const MachineProfile *profileForId(uint32_t id) {
  for (int i = 0; i < MACHINE_COUNT; i++) {
    const MachineProfile &profile = machineProfileAt(i);
    if (static_cast<uint32_t>(profile.id) == id) return &profile;
  }
  return nullptr;
}

} // namespace

SaveStates::SaveStates(Emulation &emulation, Platform &platform, std::string directory, Hooks hooks)
    : emulation_(emulation), platform_(platform), store_(std::move(directory)), hooks_(std::move(hooks)) {}

SaveStates::~SaveStates() { releaseThumbnails(); }

void SaveStates::releaseThumbnails() {
  for (auto &[id, row] : rows_) {
    if (row.thumbnail != ImTextureID_Invalid && platform_.releaseTexture) platform_.releaseTexture(row.thumbnail);
  }
  rows_.clear();
}

// Read the records again: after a save or a clear, and when the window opens.
void SaveStates::refresh() {
  releaseThumbnails();
  const MachineProfile *machine = hooks_.machine();
  std::vector<std::string> ids;
  if (machine) ids.push_back(StateStore::autosaveId(machine->key));
  for (int slot = 1; slot <= StateStore::SLOTS; slot++) ids.push_back(StateStore::slotId(slot));
  for (const std::string &id : ids) {
    Row row;
    row.record = store_.info(id);
    if (row.record && !row.record->thumbnail.empty() && platform_.makeTexture) {
      row.thumbnail = platform_.makeTexture(row.record->thumbnail.data(), THUMB_WIDTH, THUMB_HEIGHT);
    }
    rows_[id] = std::move(row);
  }
  stale_ = false;
}

void SaveStates::notice(const std::string &message) {
  notice_ = message;
  noticeUntil_ = ImGui::GetTime() + 3.0;
}

void SaveStates::error(const std::string &message) {
  error_ = message;
  openError_ = true;
}

void SaveStates::saveTo(const std::string &id) {
  const MachineProfile *machine = hooks_.machine();
  if (!machine) return;
  std::vector<uint8_t> state;
  std::vector<uint8_t> thumbnail;
  emulation_.withMachine([&](host::MachineHost &host) {
    size_t size = 0;
    const uint8_t *bytes = host.exportState(&size);
    if (bytes && size) state.assign(bytes, bytes + size);
    const auto &display = machine->display;
    if (host.framebuffer() && host.framebufferSize() ==
                                  static_cast<size_t>(display.pixelWidth) * display.pixelHeight * 4) {
      thumbnail = makeThumbnail(host.framebuffer(), display.pixelWidth, display.pixelHeight);
    }
  });
  if (state.empty()) {
    error("The machine could not be saved.");
    return;
  }
  if (!store_.save(id, machine->key, state, thumbnail)) {
    error("The save state could not be written.");
    return;
  }
  stale_ = true;
}

void SaveStates::autosaveNow() {
  if (!autosave || !emulation_.powered() || !hooks_.machine()) return;
  saveTo(StateStore::autosaveId(hooks_.machine()->key));
}

void SaveStates::update(double now) {
  if (!autosave || !emulation_.powered()) {
    lastAutosave_ = now;
    return;
  }
  if (now - lastAutosave_ < AUTOSAVE_SECONDS) return;
  lastAutosave_ = now;
  autosaveNow();
}

// A state names the machine that wrote it. The same machine takes it at
// once; another is asked about first.
void SaveStates::loadBytes(const std::vector<uint8_t> &data, const std::string &what) {
  const auto header = readStateHeader(data.data(), data.size());
  if (!header) {
    error("That is not an ApplEm save state.");
    return;
  }
  const MachineProfile *target = profileForId(header->machineId);
  const MachineProfile *current = hooks_.machine();
  if (!target) {
    error("That state was saved on a machine this version does not know.");
    return;
  }
  if (current && target->id == current->id) {
    importNow(data, what);
    return;
  }
  if (!Emulator::isMachineRunnable(target->id)) {
    error(std::string("That state was saved on the ") + target->name +
          ", whose ROM is not built into this copy of ApplEm.");
    return;
  }
  pendingLoad_ = data;
  pendingWhat_ = what;
  pendingMachine_ = static_cast<int>(target->id);
  pendingMachineName_ = target->name;
  openSwitch_ = true;
}

void SaveStates::importNow(const std::vector<uint8_t> &data, const std::string &what) {
  if (!emulation_.powered()) emulation_.setPowered(true);
  const bool ok = emulation_.withMachine([&](host::MachineHost &host) { return host.importState(data.data(), data.size()); });
  if (!ok) {
    error("The state could not be loaded.");
    return;
  }
  if (hooks_.loaded) hooks_.loaded();
  notice("Loaded " + what + ".");
}

void SaveStates::exportRecord(const std::string &id, const std::string &suggestedName) {
  auto data = store_.load(id);
  if (!data) return;
  std::vector<uint8_t> bytes = std::move(*data);
  platform_.saveFile("Save the state to a file", suggestedName, {"a2state"},
                     [this, bytes](const std::string &path) {
                       if (path.empty()) return;
                       if (!writeFile(path, bytes.data(), bytes.size())) error("Could not write " + path + ".");
                     });
}

void SaveStates::drawRow(const char *label, const std::string &id, bool isAutosave) {
  const Row &row = rows_[id];
  ImGui::PushID(id.c_str());
  ImGui::TableNextRow();

  // The picture.
  ImGui::TableNextColumn();
  const ImVec2 size(THUMB_WIDTH * 0.6f, THUMB_HEIGHT * 0.6f);
  if (row.thumbnail != ImTextureID_Invalid) {
    ImGui::Image(ImTextureRef(row.thumbnail), size);
  } else {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(20, 20, 24, 255));
    ImGui::Dummy(size);
  }

  // What it is, which machine wrote it, and when.
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(label);
  const MachineProfile *current = hooks_.machine();
  if (row.record) {
    const MachineProfile *writer = findMachineProfile(row.record->machine.c_str());
    if (writer && current && writer->id != current->id) {
      ImGui::TextColored(ImVec4(0.96f, 0.51f, 0.12f, 1.0f), "%s", writer->name);
    } else {
      ImGui::TextDisabled("%s", writer ? writer->name : row.record->machine.c_str());
    }
    ImGui::TextDisabled("%s", formatTime(row.record->savedAt).c_str());
  } else {
    ImGui::TextDisabled(isAutosave ? "No autosave" : "Empty");
  }

  // What can be done with it.
  ImGui::TableNextColumn();
  if (!isAutosave) {
    if (ui::Button("Save")) {
      saveTo(id);
      notice(std::string("Saved to ") + label + ".");
    }
    ImGui::SameLine();
  }
  ImGui::BeginDisabled(!row.record);
  if (ui::Button("Load")) {
    if (auto data = store_.load(id)) loadBytes(*data, label);
  }
  if (!isAutosave) {
    ImGui::SameLine();
    if (ui::Button("Clear")) {
      store_.clear(id);
      stale_ = true;
    }
  }
  ImGui::SameLine();
  if (ui::Button("Export")) {
    const std::string machine = row.record ? row.record->machine : "state";
    exportRecord(id, machine + (isAutosave ? "-autosave" : "-" + id) + ".a2state");
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}

void SaveStates::draw(bool *open) {
  if (open && *open) {
    if (!wasOpen_) stale_ = true;
    wasOpen_ = true;
    if (stale_) refresh();

    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Save States", open, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (ImGui::BeginTable("states", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##thumb");
        ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableSetupColumn("##actions");
        if (const MachineProfile *machine = hooks_.machine()) {
          drawRow("Autosave", StateStore::autosaveId(machine->key), true);
        }
        for (int slot = 1; slot <= StateStore::SLOTS; slot++) {
          const std::string label = "Slot " + std::to_string(slot);
          drawRow(label.c_str(), StateStore::slotId(slot), false);
        }
        ImGui::EndTable();
      }
      ImGui::Spacing();
      if (ui::Button("Load from File…")) {
        platform_.openFile("Load a save state", {"a2state"}, [this](const std::string &path) {
          if (path.empty()) return;
          if (auto data = readFile(path)) loadBytes(*data, baseName(path));
          else error("Could not read " + path + ".");
        });
      }
      ImGui::SameLine();
      ui::Switch("Autosave every 5 seconds", &autosave);
      if (ImGui::GetTime() < noticeUntil_) ImGui::TextColored(GREEN, "%s", notice_.c_str());
    }
    ImGui::End();
  } else {
    wasOpen_ = false;
  }

  // A state from another machine: switch to it, then load.
  if (openSwitch_) {
    ImGui::OpenPopup(SWITCH_POPUP);
    openSwitch_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal(SWITCH_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("%s was saved on the %s.", pendingWhat_.c_str(), pendingMachineName_.c_str());
    ImGui::TextDisabled("Switching rebuilds the machine; disks and memory now in it are lost.");
    ImGui::Spacing();
    if (ui::Button("Switch and Load", ImVec2(150, 0), ui::ButtonKind::Primary)) {
      ImGui::CloseCurrentPopup();
      const std::vector<uint8_t> data = std::move(pendingLoad_);
      if (hooks_.switchTo(static_cast<MachineId>(pendingMachine_))) {
        importNow(data, pendingWhat_);
        stale_ = true;
      } else {
        error("Could not switch to the " + pendingMachineName_ + ".");
      }
      pendingLoad_.clear();
    }
    ImGui::SameLine();
    if (ui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      pendingLoad_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
