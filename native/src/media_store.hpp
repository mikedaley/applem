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

// A disk image with the name it was inserted under.
struct StoredImage {
  std::string filename;
  std::vector<uint8_t> data;
};

struct RecentEntry {
  std::string filename;
  std::string file; // where its bytes are kept, inside the store
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
// Each unit (a floppy drive, or a SmartPort device) remembers the image that
// was inserted into it, as it was inserted: what the machine writes to a disk
// afterwards is not kept, so starting again brings the pristine image back.
// Each unit also keeps its ten most recent images, newest first, one entry
// per filename; the bytes are copied in, so a recent disk still loads after
// the original file has moved.
//
// `kind` names the set of units ("floppy", "hard-drive"), so the floppies'
// and the hard drives' records never mix.
class MediaStore {
public:
  static constexpr size_t MAX_RECENT = 10;

  MediaStore(std::string directory, std::string kind);

  void saveInserted(int unit, const std::string &filename, const std::vector<uint8_t> &data);
  std::optional<StoredImage> loadInserted(int unit) const;
  void clearInserted(int unit);

  // Put an image at the front of a unit's recent list.
  void addRecent(int unit, const std::string &filename, const std::vector<uint8_t> &data);
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
