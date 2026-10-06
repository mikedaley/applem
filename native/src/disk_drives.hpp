/*
 * disk_drives.hpp - The two 5.25" drives: what is in them, and the window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "machine_poll.hpp"
#include "ui_controls.hpp"

#include "disk_inspector_data.hpp"
#include "disk_inspector.hpp"
#include "drive_ui.hpp"
#include "disk_platter.hpp"
#include "media_store.hpp"
#include "platform.hpp"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace a2e {
class DiskController;
}

namespace a2e::native {

class Emulation;

// The browser build's Disk Drives window and the disk operations behind it
// (disk-drives-window.js, disk-operations.js, disk-manager/index.js,
// disk-surface-renderer.js), with the same rules:
//
// - A disk from a file writes back to that file, in that file's format, as
//   every Mac emulator does: once the drive has been idle for a moment, on
//   eject, before another disk replaces it, and when the app quits or the
//   machine changes. Starting again reads the file, writes and all. The
//   recent list keeps ten per drive, by path.
// - A disk with no file of its own (blank, from the library, or one a save
//   state brought) asks to be saved when it is ejected or replaced, but only
//   when it really changed: the core saying it was written to is not enough,
//   because software rewrites sectors with the same bytes all the time, so
//   the image is fingerprinted at insert and again then. So does a disk
//   whose file could not take what was written (a WOZ-only change to a .dsk,
//   or a file that could not be written).
// - Saving offers DOS order, ProDOS order and WOZ, defaulting to the format
//   the disk came in as, with the ones the disk cannot be written as shown
//   but disabled.
// - A drive is active while the motor runs and it is the selected drive; the
//   seek click plays when an active drive's head crosses a whole track.
//
// The window is the browser's Disk Drives and Disk Inspector in one: a card
// per drive, the same card as the 3.5" window's, with its disk turning as the
// real one does and the head on a bar across the tracks, and below them the
// inspector (DiskInspector) for whichever card was picked. A disk is re-read
// only when the controller's revision for it moves, and at most twice a
// second while it is being written.
class DiskDrives {
public:
  static constexpr int DRIVES = 2;
  static constexpr const char *WINDOW_NAME = "5.25\" Drives###Disk Drives";

  DiskDrives(Emulation &emulation, Platform &platform, std::string mediaDirectory);
  ~DiskDrives();

  // Put back what was in the drives last time.
  void restore();
  // The machine was rebuilt: its drives are empty, and nothing is restored
  // into them again.
  void machineChanged();
  // A save state went in: the drives hold what it held.
  void syncWithMachine();

  // Every frame, shown or not: follows the drives for the seek click and
  // the surface's track heat.
  void update(double now);
  void draw(bool *open);

  // A file dropped on the app, or chosen from a panel.
  static bool isFloppyImage(const std::string &path);
  // A .nib is not offered: the core reads sector and WOZ images only.
  void insertFile(int drive, const std::string &path);
  // Where a dropped disk goes: the first empty drive, else drive 1.
  int dropTarget() const;
  // The drive whose card is at a point, as last drawn, or -1.
  int driveAt(ImVec2 point) const;
  // Where a drag of files is, for lighting the card it would land on.
  std::optional<ImVec2> dragOver;

  // Every disk with a file written back now, for a quit or a machine change.
  void writeBackAll();
  // The disks whose changes would be lost: no file to go back to, or a file
  // that could not take them. Named for a question before quitting.
  std::vector<std::string> unsavedDisks();

  // For the menus.
  void chooseDisk(int drive);
  void ejectDrive(int drive) { requestEject(drive); }
  // For File > Open Recent: a drive's recent disks, newest first, putting
  // one back in, and forgetting them.
  std::vector<std::string> recentNames(int drive) const;
  void insertRecent(int drive, size_t index);
  void clearRecent(int drive);
  bool hasDisk(int drive) const { return drives_[drive].filename.has_value(); }
  const std::optional<std::string> &diskName(int drive) const { return drives_[drive].filename; }
  // For the status bar's lights: turning under the head, and writing.
  bool isActive(int drive) const { return drives_[drive].active; }
  bool isWriting(int drive) const { return drives_[drive].active && drives_[drive].writing; }

  // Whether the inspector is open below the drives: hidden until the user
  // first shows it, and remembered after.
  bool inspectorShown = false;

private:
  struct Drive {
    std::optional<std::string> filename;
    std::optional<uint32_t> baseline; // fingerprint at insert, or at the last write back
    // The user's file it came from, which what the machine writes goes back to.
    std::optional<std::string> path;
    // A write back that could not be made, said once and asked about at eject.
    bool writeBackFailed = false;
    // Which insertion this is, so a Save panel answered late acts on the disk
    // it was opened for and not on one put in meanwhile.
    uint32_t serial = 0;
    // The core says it has been written since the last write back.
    bool modified = false;
    bool writeProtected = false;
    double idleSince = -1;

    // Polled each frame.
    bool hasDisk = false;
    bool active = false;
    bool writing = false;
    int track = 0;
    int quarterTrack = 0;
    size_t nibble = 0;
    int lastTrack = -1;

    // How far round the disk is under the head, as a fraction of a turn:
    // the core's own, so the platter turns as the disk does.
    double rotation = 0;
    // The turn drawn: the core's while the disk turns there, coasting down
    // as a drive's spindle does when the motor stops (drive_ui::Spin).
    drive_ui::Spin spin;

    // What is recorded on it, read again when the controller's revision
    // for the drive moves.
    bool overviewRead = false;
    uint32_t revision = 0;
    double overviewAt = -1;
    std::optional<Overview> overview;
    DiskSummary summary;

    // The card's picture of it, painted from the overview when it changes,
    // and how many times it has been read, for the inspector's own.
    ImTextureID thumbnail = ImTextureID_Invalid;
    bool paintStale = true;
    uint32_t overviewSerial = 0;
  };

  struct PendingSave {
    int drive = 0;
    uint32_t serial = 0;
    int format = 0;
    std::array<bool, 3> available{};
    char name[256] = {};
    bool open = false;
    // What was asked for that needed the drive empty: another disk going in.
    // Runs after the disk is saved or let go, not if the question is cancelled.
    std::function<void()> then;
  };

  void insertImage(int drive, const std::string &filename, const std::vector<uint8_t> &data,
                   bool remember, const std::string &path = "");
  void insertBlank(int drive);
  void insertRecentEntry(int drive, const RecentEntry &entry);
  // Another disk into a drive: what the machine wrote to the one there is
  // written back, or asked about, first.
  void replace(int drive, std::function<void()> insert);
  void requestEject(int drive);
  void eject(int drive);
  // Write the disk back to its file if the machine has changed it. True when
  // nothing the machine wrote is left unkept.
  bool writeBack(int drive);
  bool hasUnsavedChanges(int drive);
  void beginSave(int drive, std::function<void()> then = nullptr);
  void finishSave(const PendingSave &save, const std::vector<uint8_t> &data);
  std::optional<uint32_t> currentFingerprint(int drive);
  void resetVisuals(Drive &drive);
  void reportError(const std::string &message);

  void drawDeck(int index);
  void drawRecentPopup(int index);
  void paintPlatters(Drive &drive);
  void releasePlatters(Drive &drive);
  // What the inspector is to show, from the drive it is on.
  InspectedDisk inspected() const;
  void drawSavePopup();
  void drawErrorPopup();

  Emulation &emulation_;
  MachinePoll poll_;
  Platform &platform_;
  MediaStore store_;
  std::vector<LibraryEntry> library_;
  std::string libraryDirectory_;
  std::array<Drive, DRIVES> drives_;
  int selectedDrive_ = 0; // the controller's
  uint8_t lastByte_ = 0;
  int phase_ = 0;
  bool motorOn_ = false;
  // The inspector, and the drive it is on.
  int inspected_ = 0;
  DiskInspector inspector_;

  // Each drive's card as last drawn, and in which frame, for drops.
  std::array<ImVec2, DRIVES * 2> deckRects_{};
  int deckFrame_ = -1;

  PendingSave save_;
  std::string error_;
  bool openError_ = false;
  // Where this window's dialogs open: over it.
  ui::DialogAnchor dialogs_;
};

// The formats a disk can be saved as, in the order offered, as the core
// numbers them (a2e::DiskSaveFormat).
struct SaveFormat {
  const char *label;
  const char *hint;
  std::vector<std::string> extensions; // the first is the default
};
const std::array<SaveFormat, 3> &saveFormats();

// A filename with the extension the format uses, leaving one that already
// fits the format alone (.do stays .do).
std::string nameForFormat(const std::string &filename, int format);

// The label colour a filename gets, one of eight, as the browser picks it.
uint32_t stickerColor(const std::string &filename);

} // namespace a2e::native
