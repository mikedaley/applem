/*
 * hard_drives.hpp - The SmartPort's two block devices, and their window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "ui_controls.hpp"

#include "media_store.hpp"
#include "platform.hpp"
#include "volume_map.hpp"

#include "imgui.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace a2e {
class SmartPortCard;
}

namespace a2e::native {

class Emulation;

// The browser build's SmartPort Drives window (hard-drive-manager.js,
// hard-drive-window.js), with its rules:
//
// - An image goes in only when the machine has a SmartPort: a card in a
//   //e's slots, or the one a IIgs has in slot 5.
// - On a IIgs an image inserted while the machine runs takes over slot 5
//   only at the next reset, and the window says so; otherwise it looks as if
//   the image was ignored.
// - A device remembers its image as inserted and keeps ten recent ones; a
//   changed image is offered for saving as it is, with no choice of format.
//   Cancelling the save keeps it in, as the floppies do.
// - Images are put back only after the slot layout is in the machine:
//   fitting the layout can rebuild the SmartPort, and an image restored
//   before then would go with the old card.
class HardDrives {
public:
  static constexpr int DEVICES = 2;

  HardDrives(Emulation &emulation, Platform &platform, std::string mediaDirectory);
  ~HardDrives();

  void restore();
  void machineChanged();
  // The cards were refitted: take what each device holds from the machine,
  // since a SmartPort that moved or went took its images with it.
  void syncWithMachine();
  // Every frame: whether there is a SmartPort, and the activity light.
  void update();
  void draw(bool *open);

  // Whether the machine has a SmartPort to take an image, as of the last
  // update. The window is offered only when it does.
  bool available() const { return available_; }
  // For the status bar's light: a transfer within the last few frames.
  bool isBusy(int device) const { return devices_[device].activityFrames > 0; }
  bool isWriting(int device) const { return devices_[device].activityFrames > 0 && devices_[device].lastWrite; }
  bool hasImage(int device) const { return devices_[device].filename.has_value(); }

  static bool isBlockImage(const std::string &path, size_t size);
  void insertFile(int device, const std::string &path);
  int dropTarget() const;
  // The device whose card is at a point, as last drawn, or -1.
  int deviceAt(ImVec2 point) const;
  // Where a drag of files is, for lighting the card it would land on.
  std::optional<ImVec2> dragOver;

private:
  // The block map's grid, and the activity graph's bins.
  static constexpr int MAP_COLUMNS = 48;
  static constexpr int MAP_ROWS = 8;
  static constexpr int MAP_CELLS = MAP_COLUMNS * MAP_ROWS;
  static constexpr int HISTORY_BINS = 60;
  static constexpr double HISTORY_BIN_SECONDS = 1.0 / 15.0; // four seconds across

  struct Device {
    std::optional<std::string> filename;
    size_t size = 0;
    int activityFrames = 0;
    bool lastWrite = false;
    bool modified = false;
    double lightAt = -1; // the last transfer, for the drive's light
    bool lightWrite = false;
    bool writeProtected = false;

    // What the volume holds, read again a moment after anything writes.
    VolumeSummary volume;
    bool volumeStale = true;
    double volumeReadAt = -1;

    // Where on the volume the machine has been lately: each transfer warms
    // its cell, which cools over about a second.
    std::array<float, MAP_CELLS> readHeat{};
    std::array<float, MAP_CELLS> writeHeat{};
    // Blocks read and written in each of the last few seconds' bins.
    std::array<uint16_t, HISTORY_BINS> readHistory{};
    std::array<uint16_t, HISTORY_BINS> writeHistory{};
  };

  // A block transfer the card reported, kept until the next frame. The card
  // reports on the emulation thread, under the machine's lock, and the
  // frame takes them under the same lock.
  struct Transfer {
    int device;
    uint32_t block;
    bool write;
  };

  void insertImage(int device, const std::string &filename, const std::vector<uint8_t> &data,
                   bool remember);
  void requestEject(int device);
  void eject(int device);
  void notice(const std::string &message);
  void reportError(const std::string &message);
  void drawDevice(int index);
  void drawRecentPopup(int index);
  void drawHeader();
  // Watch the card's transfers, once per card: a refit or a machine switch
  // builds a new one.
  void watchTransfers(SmartPortCard *card);
  void applyTransfers(double now);
  void readVolumes(double now);

  Emulation &emulation_;
  Platform &platform_;
  MediaStore store_;
  std::vector<LibraryEntry> library_;
  std::string libraryDirectory_;
  std::array<Device, DEVICES> devices_;
  bool available_ = false;
  // Each device's card as last drawn, and in which frame, for drops.
  std::array<ImVec2, DEVICES * 2> cardRects_{};
  int cardFrame_ = -1;
  std::string location_; // "Slot 7", where the SmartPort is
  SmartPortCard *watched_ = nullptr;
  std::vector<Transfer> transfers_;
  long historyBin_ = -1; // the bin the newest history entry is for
  double lastUpdate_ = 0;
  std::string notice_;
  double noticeUntil_ = 0;
  std::string error_;
  bool openError_ = false;
  // Where this window's dialogs open: over it.
  ui::DialogAnchor dialogs_;
};

} // namespace a2e::native
