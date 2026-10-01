/*
 * hard_drives.hpp - The SmartPort's two block devices, and their window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "media_store.hpp"
#include "platform.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

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

private:
  struct Device {
    std::optional<std::string> filename;
    size_t size = 0;
    int activityFrames = 0;
    bool lastWrite = false;
  };

  void insertImage(int device, const std::string &filename, const std::vector<uint8_t> &data,
                   bool remember);
  void requestEject(int device);
  void eject(int device);
  void notice(const std::string &message);
  void reportError(const std::string &message);
  void drawDevice(int index);
  void drawRecentPopup(int index);

  Emulation &emulation_;
  Platform &platform_;
  MediaStore store_;
  std::vector<LibraryEntry> library_;
  std::string libraryDirectory_;
  std::array<Device, DEVICES> devices_;
  bool available_ = false;
  std::string notice_;
  double noticeUntil_ = 0;
  std::string error_;
  bool openError_ = false;
};

} // namespace a2e::native
