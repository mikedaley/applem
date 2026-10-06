/*
 * media_store.hpp - Disk images the app keeps: in the drives, and recent
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

// A disk image with the name it was inserted under, and the file it came
// from when it came from one: what the machine writes goes back there.
struct StoredImage {
  std::string filename;
  std::vector<uint8_t> data;
  std::string path;
  // The file it came from is not where it was: there are no bytes.
  bool missing = false;
};

struct RecentEntry {
  std::string filename;
  std::string file; // where its bytes are kept, inside the store, if it has no path
  std::string path; // the user's file, read again when it is chosen
  int64_t accessed = 0;
};

struct LibraryEntry {
  std::string id;
  std::string name;
  std::string file;
  std::string type; // "floppy" or "hard-drive"
  std::string description;
};

// The browser build's disk persistence (disk-persistence.js), in files.
//
// Each unit (a floppy drive, or a SmartPort device) remembers the disk that
// was inserted into it. A disk that came from a file is remembered by its
// path, because what the machine writes goes back to that file and starting
// again should bring back the disk as it was left, as a Mac app reopens a
// document. A disk with no file of its own (a blank one, one from the app's
// library) is remembered by a copy of its bytes.
//
// Each unit also keeps its ten most recent disks, newest first, one entry
// per file: a path for a disk that has one, which is read again when chosen,
// and otherwise a copy. Copying every recent disk kept up to ten 32MB hard
// drive images per device in the app's own folder, and named them by bare
// filename, so two DISK1.DSKs from different folders overwrote each other.
//
// `kind` names the set of units ("floppy", "hard-drive"), so the floppies'
// and the hard drives' records never mix.
class MediaStore {
public:
  static constexpr size_t MAX_RECENT = 10;

  MediaStore(std::string directory, std::string kind);

  // With a path, only the path is kept; without one, the bytes.
  void saveInserted(int unit, const std::string &filename, const std::vector<uint8_t> &data,
                    const std::string &path = "");
  std::optional<StoredImage> loadInserted(int unit) const;
  void clearInserted(int unit);

  // Put an image at the front of a unit's recent list.
  void addRecent(int unit, const std::string &filename, const std::vector<uint8_t> &data,
                 const std::string &path = "");
  std::vector<RecentEntry> recent(int unit) const;
  std::optional<StoredImage> loadRecent(int unit, const RecentEntry &entry) const;
  void clearRecent(int unit);

private:
  std::string unitPath(int unit, const std::string &suffix) const;
  std::string indexPath(int unit) const;
  void writeIndex(int unit, const std::vector<RecentEntry> &entries) const;

  std::string directory_;
  std::string kind_;
};

// The disks that ship with the app (public/disks/library.json, copied into
// the bundle). Malformed entries are skipped.
std::vector<LibraryEntry> parseLibrary(const std::string &json);

// FNV-1a over a buffer. Tells whether a disk changed between insert and
// eject without keeping a copy; not cryptographic and does not need to be.
uint32_t fingerprint(const uint8_t *data, size_t size);

std::optional<std::vector<uint8_t>> readFile(const std::string &path);
bool writeFile(const std::string &path, const uint8_t *data, size_t size);
std::string baseName(const std::string &path);

} // namespace a2e::native
