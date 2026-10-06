/*
 * disk35_drives.hpp - A IIgs's two 3.5" drives, and their window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "drives/disk_inspector.hpp"
#include "drives/drive_ui.hpp"
#include "drives/disk_inspector_data.hpp"
#include "app/machine_poll.hpp"
#include "drives/media_store.hpp"
#include "app/platform.hpp"
#include "ui/ui_controls.hpp"
#include "sound/volume_map.hpp"

#include "imgui.h"

#include <array>
#include <functional>
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
// - A disk from a file writes back to it, as the 5.25" drives' do: once the
//   drive has stopped for a moment, on eject, before another disk replaces
//   it, and on quit. One with no file of its own is offered for saving.
// - A drive remembers its disk and keeps ten recent ones, as the other
//   drives do.
// - The machine ejects disks itself (GS/OS's Eject, an installer asking for
//   the next disk). Such a disk goes back to its file, or into Recent as it
//   then is, so nothing written to it is lost however it left the drive.
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
  // As the 5.25" drives': write back every disk with a file, and name those
  // whose changes would be lost.
  void writeBackAll();
  std::vector<std::string> unsavedDisks();
  int dropTarget() const;
  int driveAt(ImVec2 point) const;
  std::optional<ImVec2> dragOver;
  // Whether the inspector is open below the drives, as the 5.25" window's is.
  bool inspectorShown = false;

private:
  struct Drive {
    std::optional<std::string> filename;
    std::optional<std::string> path; // the user's file, written back to
    bool writeBackFailed = false;
    uint32_t serial = 0; // which insertion, for a Save panel answered late
    double idleSince = -1;
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

  void insertImage(int drive, const std::string &filename, const std::vector<uint8_t> &data, bool remember,
                   const std::string &path = "");
  void insertRecentEntry(int drive, const RecentEntry &entry);
  void replace(int drive, std::function<void()> insert);
  bool writeBack(int drive);
  void requestEject(int drive);
  void saveThenEject(int drive, std::function<void()> then);
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
  // What waits on the question: another disk going in.
  std::function<void()> askThen_;
  ui::DialogAnchor dialogs_;
  int inspected_ = 0;
  DiskInspector inspector_;
};

} // namespace a2e::native
