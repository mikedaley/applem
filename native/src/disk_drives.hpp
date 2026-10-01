/*
 * disk_drives.hpp - The two 5.25" drives: what is in them, and the window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "media_store.hpp"
#include "platform.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

class Emulation;

// The browser build's Disk Drives window and the disk operations behind it
// (disk-drives-window.js, disk-operations.js, disk-manager/index.js,
// disk-surface-renderer.js), with the same rules:
//
// - What is persisted is the image as it was inserted, and the recent list
//   keeps ten per drive. A blank disk is neither persisted nor recent.
// - Ejecting asks to save only when the disk really changed: the core
//   saying it was written to is not enough, because software rewrites
//   sectors with the same bytes all the time, so the image is fingerprinted
//   at insert and again at eject.
// - Saving offers DOS order, ProDOS order and WOZ, defaulting to the format
//   the disk came in as, with the ones the disk cannot be written as shown
//   but disabled.
// - A drive is active while the motor runs and it is the selected drive; the
//   seek click plays when an active drive's head crosses a whole track.
class DiskDrives {
public:
  static constexpr int DRIVES = 2;

  DiskDrives(Emulation &emulation, Platform &platform, std::string mediaDirectory);

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
  void insertFile(int drive, const std::string &path);
  // Where a dropped disk goes: the first empty drive, else drive 1.
  int dropTarget() const;

  bool surfaceShown = true;
  bool detailsShown = false;

private:
  struct Drive {
    std::optional<std::string> filename;
    std::optional<uint32_t> baseline; // fingerprint at insert

    // Polled each frame.
    bool hasDisk = false;
    bool active = false;
    bool writing = false;
    int track = 0;
    int quarterTrack = 0;
    size_t nibble = 0;
    int lastTrack = -1;

    // The surface's track heat, decayed every 100ms.
    std::array<uint32_t, 35> trackAccess{};
    uint32_t maxAccess = 0;
    double lastDecay = 0;

    // The platter's spin, run up while the drive is active and coasting to
    // a stop with a 600ms half-life after.
    double angle = 0;
    double velocity = 0;
    bool spinning = false;
    double lastTime = 0;
  };

  struct PendingSave {
    int drive = 0;
    int format = 0;
    std::array<bool, 3> available{};
    char name[256] = {};
    bool open = false;
  };

  void insertImage(int drive, const std::string &filename, const std::vector<uint8_t> &data,
                   bool remember);
  void insertBlank(int drive);
  void requestEject(int drive);
  void eject(int drive);
  void beginSave(int drive);
  void finishSave(const PendingSave &save, const std::vector<uint8_t> &data);
  std::optional<uint32_t> currentFingerprint(int drive);
  void resetVisuals(Drive &drive);
  void reportError(const std::string &message);

  void drawDrive(int index);
  void drawSurface(int index, ImVec2 origin);
  void drawRecentPopup(int index);
  void drawSavePopup();
  void drawErrorPopup();

  Emulation &emulation_;
  Platform &platform_;
  MediaStore store_;
  std::vector<LibraryEntry> library_;
  std::string libraryDirectory_;
  std::array<Drive, DRIVES> drives_;
  int selectedDrive_ = 0;
  uint8_t lastByte_ = 0;
  int phase_ = 0;
  bool motorOn_ = false;
  PendingSave save_;
  std::string error_;
  bool openError_ = false;
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
