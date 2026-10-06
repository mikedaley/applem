/*
 * test_native_media.cpp - Disk images kept between runs, and how they are named
 *
 * The browser's rules: a drive remembers the image as it went in, the recent
 * list is ten per drive, newest first, one entry per name, and a saved disk
 * takes its format's extension unless it already has one that fits.
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "../src/disk_drives.hpp"
#include "../src/drive_sounds.hpp"
#include "../src/hard_drives.hpp"
#include "../src/media_store.hpp"
#include "../src/slot_layout.hpp"
#include "../src/state_store.hpp"
#include "../src/volume_map.hpp"
#include "../src/disk_inspector_data.hpp"
#include "../src/disk_platter.hpp"
#include "disk-image/dsk_disk_image.hpp"
#include "../../src/host/machine_host.hpp"
#include "machine/machine_profile.hpp"

#include <cstring>
#include <filesystem>
#include <random>

using namespace a2e::native;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() / ("a2e-media-" + std::to_string(std::random_device{}()));
    fs::create_directories(path);
  }
  ~TempDir() { fs::remove_all(path); }
};

std::vector<uint8_t> bytes(uint8_t fill, size_t size = 64) { return std::vector<uint8_t>(size, fill); }

} // namespace

TEST_CASE("A drive remembers the image it was given", "[media]") {
  TempDir dir;
  MediaStore store(dir.path.string(), "floppy");
  REQUIRE_FALSE(store.loadInserted(0));
  store.saveInserted(0, "Lode Runner.dsk", bytes(7));
  auto image = store.loadInserted(0);
  REQUIRE(image);
  REQUIRE(image->filename == "Lode Runner.dsk");
  REQUIRE(image->data == bytes(7));
  REQUIRE_FALSE(store.loadInserted(1));
  store.clearInserted(0);
  REQUIRE_FALSE(store.loadInserted(0));

  // Hard drives are kept apart from floppies.
  MediaStore drives(dir.path.string(), "hard-drive");
  store.saveInserted(0, "a.dsk", bytes(1));
  REQUIRE_FALSE(drives.loadInserted(0));
}

TEST_CASE("Recent disks are newest first, one per name, ten at most", "[media]") {
  TempDir dir;
  MediaStore store(dir.path.string(), "floppy");
  for (int i = 0; i < 12; i++) {
    store.addRecent(0, "disk" + std::to_string(i) + ".dsk", bytes(static_cast<uint8_t>(i)));
  }
  auto recent = store.recent(0);
  REQUIRE(recent.size() == MediaStore::MAX_RECENT);
  REQUIRE(recent.front().filename == "disk11.dsk");
  REQUIRE(recent.back().filename == "disk2.dsk");

  // The same name again moves to the front with its new bytes.
  store.addRecent(0, "disk5.dsk", bytes(99));
  recent = store.recent(0);
  REQUIRE(recent.size() == MediaStore::MAX_RECENT);
  REQUIRE(recent.front().filename == "disk5.dsk");
  REQUIRE(store.loadRecent(0, recent.front())->data == bytes(99));

  // Each drive has its own list.
  REQUIRE(store.recent(1).empty());
  store.clearRecent(0);
  REQUIRE(store.recent(0).empty());
}

TEST_CASE("A disk from a file is remembered by its path, and read from it again", "[media]") {
  // What the machine writes goes back to the user's file, so starting again
  // reads that file rather than a copy taken when it went in.
  TempDir dir;
  MediaStore store(dir.path.string(), "floppy");
  const std::string file = (dir.path / "Work.dsk").string();
  REQUIRE(writeFile(file, bytes(1).data(), 64));
  store.saveInserted(0, "Work.dsk", bytes(1), file);
  REQUIRE(writeFile(file, bytes(2).data(), 64)); // written back since
  auto image = store.loadInserted(0);
  REQUIRE(image);
  REQUIRE(image->path == file);
  REQUIRE(image->data == bytes(2));
  REQUIRE_FALSE(image->missing);

  // Moved away: the drive says so rather than silently coming up empty.
  fs::remove(file);
  image = store.loadInserted(0);
  REQUIRE(image);
  REQUIRE(image->missing);
  REQUIRE(image->data.empty());

  // A disk with no file is kept as bytes, and replaces the path.
  store.saveInserted(0, "Blank.woz", bytes(3));
  image = store.loadInserted(0);
  REQUIRE(image->path.empty());
  REQUIRE(image->data == bytes(3));
}

TEST_CASE("A recent disk with a path is the file, not a copy", "[media]") {
  TempDir dir;
  MediaStore store(dir.path.string(), "floppy");
  fs::create_directories(dir.path / "a");
  fs::create_directories(dir.path / "b");
  const std::string first = (dir.path / "a" / "DISK1.DSK").string();
  const std::string second = (dir.path / "b" / "DISK1.DSK").string();
  REQUIRE(writeFile(first, bytes(1).data(), 64));
  REQUIRE(writeFile(second, bytes(2).data(), 64));
  store.addRecent(0, "DISK1.DSK", bytes(1), first);
  store.addRecent(0, "DISK1.DSK", bytes(2), second);

  // Two files of the same name are two entries, and nothing was copied.
  auto recent = store.recent(0);
  REQUIRE(recent.size() == 2);
  REQUIRE(recent.front().path == second);
  for (const auto &entry : fs::directory_iterator(dir.path / "floppy")) {
    REQUIRE(entry.path().extension() != ".img");
  }
  // Chosen, it is read from the file as it is now.
  REQUIRE(writeFile(first, bytes(9).data(), 64));
  auto image = store.loadRecent(0, recent.back());
  REQUIRE(image);
  REQUIRE(image->data == bytes(9));
  REQUIRE(image->path == first);
  // The same file again moves to the front rather than adding another.
  store.addRecent(0, "DISK1.DSK", bytes(9), first);
  REQUIRE(store.recent(0).size() == 2);
  REQUIRE(store.recent(0).front().path == first);
}

TEST_CASE("A failed write leaves nothing behind", "[media]") {
  TempDir dir;
  const std::string into = (dir.path / "no-such-folder" / "x.dsk").string();
  REQUIRE_FALSE(writeFile(into, bytes(1).data(), 64));
  REQUIRE_FALSE(fs::exists(into + ".tmp"));
}

TEST_CASE("The disk library is read from the browser's library.json", "[media]") {
  const std::string json = R"([
    {"id": "prodos", "name": "ProDOS 2.4.3", "file": "ProDOS 2.4.3.po", "type": "floppy",
     "size": "140 KB", "description": "ProDOS with \"BASIC.SYSTEM\""},
    {"id": "hd", "name": "HD", "file": "h32mb.2mg", "type": "hard-drive"},
    {"name": "no file", "type": "floppy"}
  ])";
  const auto library = parseLibrary(json);
  REQUIRE(library.size() == 2);
  REQUIRE(library[0].file == "ProDOS 2.4.3.po");
  REQUIRE(library[1].type == "hard-drive");
}

TEST_CASE("A saved disk takes its format's extension", "[media]") {
  REQUIRE(nameForFormat("Game.woz", 0) == "Game.dsk");
  REQUIRE(nameForFormat("Game.do", 0) == "Game.do"); // already fits
  REQUIRE(nameForFormat("Game.dsk", 1) == "Game.po");
  REQUIRE(nameForFormat("Game", 2) == "Game.woz");
  REQUIRE(nameForFormat("My.Disk.nib", 2) == "My.Disk.woz");
}

TEST_CASE("Floppy images are known by their extension", "[media]") {
  for (const char *name : {"a.dsk", "a.DO", "a.po", "a.woz"}) REQUIRE(DiskDrives::isFloppyImage(name));
  // A .nib is not offered: the core reads sector and WOZ images, and every
  // NIB used to fail with "Could not load the disk image".
  for (const char *name : {"a.nib", "a.2mg", "a.hdv", "a.txt", "dsk"}) REQUIRE_FALSE(DiskDrives::isFloppyImage(name));
}

TEST_CASE("A dropped image goes to the drive it belongs in", "[media]") {
  REQUIRE(HardDrives::isBlockImage("Total Replay.hdv", 32 * 1024 * 1024));
  REQUIRE(HardDrives::isBlockImage("System 6.2MG", 800 * 1024));
  // A 140K ProDOS-order image is a floppy; a bigger one is a hard drive.
  REQUIRE_FALSE(HardDrives::isBlockImage("ProDOS.po", 143360));
  REQUIRE(HardDrives::isBlockImage("Big.po", 800 * 1024));
  REQUIRE_FALSE(HardDrives::isBlockImage("Game.dsk", 143360));
}

namespace {

// An 800K ProDOS volume named HARD1, its bitmap in block 6, with blocks 0
// to 99 in use and three entries in its directory.
std::vector<uint8_t> prodosVolume() {
  std::vector<uint8_t> image(1600 * 512, 0);
  uint8_t *header = image.data() + 2 * 512;
  header[0x04] = 0xF5;
  std::memcpy(header + 0x05, "HARD1", 5);
  header[0x23] = 0x27;
  header[0x24] = 0x0D;
  header[0x25] = 3;
  header[0x27] = 6;
  header[0x29] = 1600 & 0xFF;
  header[0x2A] = 1600 >> 8;
  uint8_t *bitmap = image.data() + 6 * 512;
  for (int block = 100; block < 1600; block++) bitmap[block / 8] |= 0x80 >> (block % 8);
  return image;
}

} // namespace

TEST_CASE("A SmartPort volume is read off its own blocks", "[media][volume]") {
  const std::vector<uint8_t> image = prodosVolume();
  const VolumeSummary volume = summariseVolume(image.data(), image.size(), 16);
  CHECK(volume.prodos);
  CHECK(volume.name == "/HARD1");
  CHECK(volume.totalBlocks == 1600);
  CHECK(volume.freeBlocks == 1500);
  CHECK(volume.entries == 3);
  REQUIRE(volume.used.size() == 16);
  CHECK(volume.used[0] == 1.0f); // blocks 0 to 99, all in use
  CHECK(volume.used[1] == 0.0f);
  CHECK(volume.used[15] == 0.0f);
}

TEST_CASE("A volume that is not ProDOS has only its size", "[media][volume]") {
  std::vector<uint8_t> image(800 * 512, 0);
  const VolumeSummary volume = summariseVolume(image.data(), image.size(), 16);
  CHECK_FALSE(volume.prodos);
  CHECK(volume.totalBlocks == 800);
  CHECK(volume.used.empty());

  // A volume claiming a bitmap past the end of the image is not trusted.
  std::vector<uint8_t> broken = prodosVolume();
  broken[2 * 512 + 0x27] = 0xFF;
  broken[2 * 512 + 0x28] = 0x7F;
  CHECK_FALSE(summariseVolume(broken.data(), broken.size(), 16).prodos);
}

TEST_CASE("Every block has a cell, and the cells cover the volume", "[media][volume]") {
  CHECK(cellForBlock(0, 65535, 384) == 0);
  CHECK(cellForBlock(65534, 65535, 384) == 383);
  CHECK(cellForBlock(70000, 65535, 384) == 383); // past the end, clamped
  for (int cell = 0; cell < 384; cell++) {
    const uint32_t first = cellFirstBlock(cell, 65535, 384);
    CHECK(cellForBlock(first, 65535, 384) == cell);
    if (first > 0) CHECK(cellForBlock(first - 1, 65535, 384) == cell - 1);
  }
  CHECK(cellFirstBlock(384, 65535, 384) == 65535);
}

TEST_CASE("The inspector reads a whole disk through the core's overview", "[media][inspector]") {
  auto data = readFile("public/disks/ProDOS 2.4.3.po");
  REQUIRE(data);
  a2e::DskDiskImage image;
  REQUIRE(image.load(data->data(), data->size(), "ProDOS 2.4.3.po"));

  const auto overview = parseOverview(a2e::inspect::buildOverview(image, 720));
  REQUIRE(overview);
  CHECK(overview->buckets == 720);
  REQUIRE(overview->tracks.size() == 160);
  CHECK(overview->tracks[0].present);
  CHECK(overview->tracks[0].sectorsFound == 16);
  CHECK(overview->tracks[0].kinds.size() == 720);

  const DiskSummary summary = summariseDisk(*overview);
  CHECK(summary.tracks == 35);
  CHECK(summary.sectors == 560);
  CHECK(summary.good == 560);
  CHECK(summary.bad == 0);
  CHECK(summary.format == "16 sector");

  // A buffer it does not know is refused rather than drawn.
  std::vector<uint8_t> wrong = a2e::inspect::buildOverview(image, 720);
  wrong[4] = 99;
  CHECK_FALSE(parseOverview(wrong));
  CHECK_FALSE(parseOverview({}));
}

TEST_CASE("The inspector reads one track in full", "[media][inspector]") {
  auto data = readFile("public/disks/ProDOS 2.4.3.po");
  REQUIRE(data);
  a2e::DskDiskImage image;
  REQUIRE(image.load(data->data(), data->size(), "ProDOS 2.4.3.po"));

  const TrackDetail track = readTrackDetail(image, 0);
  CHECK(track.present);
  CHECK_FALSE(track.flux);
  CHECK(track.analysis.sectors.size() == 16);
  REQUIRE_FALSE(track.analysis.nibbles.empty());

  // Every nibble is found again from any of its cells, and the cells before
  // the first belong to the last, round the end of the track.
  const auto &nibbles = track.analysis.nibbles;
  for (size_t i = 0; i < nibbles.size(); i += 97) {
    CHECK(nibbleAtCell(nibbles, nibbles[i].start_bit) == static_cast<int>(i));
    CHECK(nibbleAtCell(nibbles, nibbles[i].start_bit + nibbles[i].cells - 1) == static_cast<int>(i));
  }
  if (nibbles.front().start_bit > 0) {
    CHECK(nibbleAtCell(nibbles, 0) == static_cast<int>(nibbles.size()) - 1);
  }

  CHECK_FALSE(readTrackDetail(image, 159).present); // past the last track
  CHECK(trackLabel(0) == "0");
  CHECK(trackLabel(69) == "17.25");
  CHECK(trackLabel(70) == "17.5");
}

TEST_CASE("The platter puts each quarter track on its own ring", "[media][inspector]") {
  for (int qt = 0; qt < platter::QUARTER_TRACKS; qt++) {
    CHECK(platter::quarterTrackAt(platter::radiusOf(qt)) == qt);
  }
  CHECK(platter::quarterTrackAt(platter::BAND_OUTER + 0.01f) == -1);
  CHECK(platter::quarterTrackAt(platter::BAND_INNER - 0.01f) == -1);

  // Outside the disk and in the hub hole is transparent; the medium is not.
  std::vector<uint8_t> rgba;
  paintPlatter(rgba, 64, nullptr, PlatterMode::Structure);
  REQUIRE(rgba.size() == 64 * 64 * 4);
  CHECK(rgba[3] == 0);                      // a corner
  CHECK(rgba[(32 * 64 + 32) * 4 + 3] == 0); // the middle of the hub hole
  CHECK(rgba[(32 * 64 + 2) * 4 + 3] > 200); // on the medium, near the rim
}

TEST_CASE("The label colour is the browser's", "[media]") {
  // Expected values from the browser's own _getStickerColor, run in Node.
  REQUIRE(stickerColor("ProDOS 2.4.3.po") == 0xb8d4e8);
  REQUIRE(stickerColor("Apple DOS 3.3 January 1983.dsk") == 0xb8d4e8);
  REQUIRE(stickerColor("Blank Disk.woz") == 0xf0e8a0);
  REQUIRE(stickerColor("Lode Runner.dsk") == 0xd0c8e8);
}

TEST_CASE("A seek click plays once, at the main volume", "[media][sound]") {
  DriveSounds sounds(48000);
  std::vector<float> buffer(4096 * 2, 0.0f);
  sounds.mix(buffer.data(), 4096, 1.0f);
  REQUIRE(std::all_of(buffer.begin(), buffer.end(), [](float s) { return s == 0.0f; }));

  sounds.playSeek();
  sounds.mix(buffer.data(), 4096, 1.0f);
  float peak = 0;
  size_t last = 0;
  for (size_t i = 0; i < buffer.size(); i++) {
    if (buffer[i] != 0.0f) last = i;
    peak = std::max(peak, std::abs(buffer[i]));
  }
  REQUIRE(peak > 0.05f);
  REQUIRE(peak < 0.3f);                   // 0.3 x 0.8 of full scale at most
  REQUIRE(last / 2 < 48000 * 26 / 1000);  // 25ms and no more

  std::fill(buffer.begin(), buffer.end(), 0.0f);
  sounds.mix(buffer.data(), 4096, 1.0f);
  REQUIRE(std::all_of(buffer.begin(), buffer.end(), [](float s) { return s == 0.0f; }));

  sounds.setEnabled(false);
  sounds.playSeek();
  sounds.mix(buffer.data(), 4096, 1.0f);
  REQUIRE(std::all_of(buffer.begin(), buffer.end(), [](float s) { return s == 0.0f; }));
}

TEST_CASE("A fresh machine's slots are its profile's defaults", "[slots]") {
  using a2e::MachineId;
  const SlotLayout iie = defaultLayout(a2e::machineProfile(MachineId::AppleIIe));
  REQUIRE(iie == SlotLayout{{4, "mockingboard"}, {5, "thunderclock"}, {6, "disk2"}, {7, "smartport"}});
  // Fixed slots are not the user's, so they are not in the layout.
  REQUIRE(iie.count(3) == 0);
  REQUIRE(isFixedSlot(a2e::machineProfile(MachineId::AppleIIe), 3));
  REQUIRE(fixedCardLabel(a2e::machineProfile(MachineId::AppleIIe), 3) == "80-Column (Built-in)");
  // A //c's slots are all soldered down.
  REQUIRE(defaultLayout(a2e::machineProfile(MachineId::AppleIIc)).empty());
}

TEST_CASE("Each slot offers what it conventionally takes", "[slots]") {
  const auto &iie = a2e::machineProfile(a2e::MachineId::AppleIIe);
  REQUIRE(slotOffers(iie, 6) == std::vector<std::string>{"disk2"});
  REQUIRE(slotOffers(iie, 4) ==
          std::vector<std::string>{"mockingboard", "mouse", "smartport", "softcard"});
  const auto &gs = a2e::machineProfile(a2e::MachineId::AppleIIgs);
  // No printer or serial card until there is a printer, and no second
  // SmartPort on a IIgs, which has one built in.
  REQUIRE(slotOffers(gs, 4) == std::vector<std::string>{"mockingboard", "mouse", "thunderclock"});
  REQUIRE(slotOffers(iie, 1) == std::vector<std::string>{"softcard"});
  REQUIRE(builtInDevice(gs, 5) == std::optional<std::string>("SmartPort"));
  REQUIRE_FALSE(builtInDevice(gs, 3)); // slot 3 has no switch
  REQUIRE_FALSE(builtInDevice(iie, 5));
}

TEST_CASE("Slot lines are read back and nonsense is refused", "[slots]") {
  int slot = 0;
  std::string card;
  REQUIRE(parseSlotLine(formatSlotLine(4, "mouse").c_str(), slot, card));
  REQUIRE(slot == 4);
  REQUIRE(card == "mouse");
  REQUIRE(parseSlotLine("Slot2=empty", slot, card));
  REQUIRE_FALSE(parseSlotLine("Slot9=mouse", slot, card));
  REQUIRE_FALSE(parseSlotLine("Slot4=toaster", slot, card));
  REQUIRE(cardsInUse({{4, "mouse"}, {5, "empty"}, {7, "smartport"}}, 7) == std::vector<std::string>{"mouse"});
}

TEST_CASE("A state names the machine that wrote it", "[states]") {
  a2e::host::MachineHost host;
  host.build();
  size_t size = 0;
  const uint8_t *state = host.exportState(&size);
  const auto header = readStateHeader(state, size);
  REQUIRE(header);
  REQUIRE(header->machineId == static_cast<uint32_t>(a2e::MachineId::AppleIIe));
  // Anything else is refused.
  const uint8_t junk[16] = {'n', 'o', 'p', 'e'};
  REQUIRE_FALSE(readStateHeader(junk, sizeof(junk)));
  REQUIRE_FALSE(readStateHeader(state, 8));
}

TEST_CASE("A thumbnail is the whole frame, shrunk", "[states]") {
  // Left half white, right half black, at a IIgs's size.
  const int width = 736;
  const int height = 448;
  std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4, 0);
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width / 2; x++) {
      uint8_t *p = &frame[(static_cast<size_t>(y) * width + x) * 4];
      p[0] = p[1] = p[2] = 255;
    }
  }
  const auto thumb = makeThumbnail(frame.data(), width, height);
  REQUIRE(thumb.size() == static_cast<size_t>(THUMB_WIDTH) * THUMB_HEIGHT * 4);
  REQUIRE(thumb[0] == 255);                                   // top left white
  REQUIRE(thumb[(THUMB_WIDTH - 1) * 4] == 0);                 // top right black
  REQUIRE(thumb[3] == 255);                                   // opaque
}

TEST_CASE("Save states are kept by id and say who wrote them", "[states]") {
  TempDir dir;
  StateStore store(dir.path.string());
  REQUIRE_FALSE(store.info(StateStore::slotId(1)));
  const std::vector<uint8_t> state = bytes(42, 1000);
  const std::vector<uint8_t> thumb(static_cast<size_t>(THUMB_WIDTH) * THUMB_HEIGHT * 4, 9);
  REQUIRE(store.save(StateStore::slotId(1), "apple2gs", state, thumb));
  auto info = store.info(StateStore::slotId(1));
  REQUIRE(info);
  REQUIRE(info->machine == "apple2gs");
  REQUIRE(info->savedAt > 0);
  REQUIRE(info->thumbnail == thumb);
  REQUIRE(*store.load(StateStore::slotId(1)) == state);
  // Each machine's autosave is its own.
  REQUIRE(StateStore::autosaveId("apple2e") != StateStore::autosaveId("apple2c"));
  store.clear(StateStore::slotId(1));
  REQUIRE_FALSE(store.info(StateStore::slotId(1)));
}
