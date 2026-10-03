/*
 * disk35_drives.hpp - A IIgs's two 3.5" drives, and their window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "disk_inspector.hpp"
#include "drive_ui.hpp"
#include "disk_inspector_data.hpp"
#include "machine_poll.hpp"
#include "media_store.hpp"
#include "platform.hpp"
#include "ui_controls.hpp"
#include "volume_map.hpp"

#include "imgui.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

class Emulation;

// The browser build's 3.5" Drives window (disk35-manager.js), with its rules:
//
// - Only a IIgs has 3.5" drives, on its IWM; the window is offered only on
//   one. An 800K or 400K block image, a 2MG holding one, or a 3.5" WOZ goes
//   in, and saving gives back the format that came in.
// - A drive remembers its disk and keeps ten recent ones, as the other
//   drives do, and a changed disk is offered for saving before it is ejected.
// - The machine ejects disks itself (GS/OS's Eject, an installer asking for
//   the next disk). Such a disk goes into Recent as it then is, so nothing
//   written to it is lost however it left the drive.
// - The remembered disks belong to the IIgs: switching to another machine
//   leaves them, and they come back with it.
class Disk35Drives {
public:
  static constexpr int DRIVES = 2;

  Disk35Drives(Emulation &emulation, Platform &platform, std::string mediaDirectory);
  ~Disk35Drives();

  // Put the remembered disks back into a IIgs's empty drives.
  void restore();
  void machineChanged();
  // After a save state: what each drive holds, from the machine.
  void syncWithMachine();
  // Every frame: whether there are drives, the lights and the head, and a
  // disk the machine ejected.
  void update();
  void draw(bool *open);

  bool available() const { return available_; }
  bool hasDisk(int drive) const { return drives_[drive].filename.has_value(); }
  bool isBusy(int drive) const { return drives_[drive].spinning; }
  const std::optional<std::string> &diskName(int drive) const { return drives_[drive].filename; }

  // A 3.5" disk, by its size and its first bytes, as the core decides.
  static bool isDisk35Image(const std::string &path);
  void insertFile(int drive, const std::string &path);
  void chooseDisk(int drive);
  void ejectDrive(int drive) { requestEject(drive); }
  int dropTarget() const;
  int driveAt(ImVec2 point) const;
  std::optional<ImVec2> dragOver;
  // Whether the inspector is open below the drives, as the 5.25" window's is.
  bool inspectorShown = false;

private:
  struct Drive {
    std::optional<std::string> filename;
    size_t size = 0;
    VolumeSummary volume;  // as inserted
    bool spinning = false;
    bool modified = false;
    bool writeProtected = false;
    int track = 0;
    int side = 0;

    // The side under the head as the Disk Inspector reads it, two rings to
    // a track, painted once into a texture and drawn turning with the disk.
    std::optional<Overview> overview;
    int overviewSide = -1;
    uint32_t revision = 0;
    double overviewAt = -1;
    bool paintStale = true;
    ImTextureID thumbnail = ImTextureID_Invalid;
    double rotation = 0; // the core's, 0 to 1
    drive_ui::Spin spin; // the turn drawn, coasting down when the spindle stops
    uint32_t coreRevision = 0;   // the drive's, moving with every write
    uint32_t overviewSerial = 0; // moves each time the overview is read
  };

  void insertImage(int drive, const std::string &filename, const std::vector<uint8_t> &data, bool remember);
  void requestEject(int drive);
  void saveThenEject(int drive);
  void eject(int drive);
  void takeEjected(int drive);
  void notice(const std::string &message);
  void reportError(const std::string &message);
  void drawDrive(int index);
  void drawRecentPopup(int index);
  void paintThumbnail(Drive &drive);
  void releaseThumbnail(Drive &drive);
  void clearDrive(Drive &drive);
  InspectedDisk inspected() const;

  Emulation &emulation_;
  MachinePoll poll_;
  Platform &platform_;
  MediaStore store_;
  std::array<Drive, DRIVES> drives_;
  bool available_ = false;
  std::array<ImVec2, DRIVES * 2> cardRects_{};
  int cardFrame_ = -1;
  std::string notice_;
  double noticeUntil_ = 0;
  std::string error_;
  bool openError_ = false;
  int askEject_ = 0;
  bool openAskEject_ = false;
  ui::DialogAnchor dialogs_;
  int inspected_ = 0;
  DiskInspector inspector_;
};

} // namespace a2e::native
