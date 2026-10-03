/*
 * memory_viewer.hpp - The memory viewer: every byte of the machine, live
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "machine_poll.hpp"
#include "debug_breakpoints.hpp"
#include "platform.hpp"

#include "../../src/host/machine_host.hpp"

#include "imgui.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace a2e::native {

class CpuDebugger;
class Emulation;

// A hex view of the machine's memory for someone writing software for it.
//
// What it browses is a MemorySpace (machine_host.hpp): on the 8-bit machines
// the processor's view, switches and all, then main RAM, auxiliary RAM and
// the ROM as they are, whatever the switches say; on a IIgs, a bank at a
// time. Every byte is drawn as it changes and fades, and with Activity on
// the processor's reads and writes light the cells they touch. A map of the
// whole space, a pixel a byte and a row a page, runs down the right and is
// how the view is scrolled.
//
// Bytes are edited in place, in hex or as text, with undo; a range is
// selected, filled, copied as source or saved to a file, and a file is
// loaded at an address. The inspector reads the bytes at the caret every
// way a program might (a word, a pointer, an Applesoft float) and edits the
// bits of one; the banking card says which bank each part of the map reads
// and writes as the switches stand. Breakpoints on a byte or a range are
// the CPU debugger's own, marked on the bytes they watch.
//
// The machine is read once a frame under one lock, the rows in view and a
// few either side; the map is read a few times a second.
class MemoryViewer {
public:
  MemoryViewer(Emulation &emulation, Platform &platform, CpuDebugger &debugger);
  ~MemoryViewer();

  void setMachine(const MachineProfile &profile);

  // Every frame. A closed viewer reads nothing, and lets the machine stop
  // counting accesses for it.
  void update(bool open);
  void draw(bool *open);

  // Asked to show the CPU debugger (Disassemble Here).
  std::function<void()> showDebugger;

  // Settings, under their own section of the ini file.
  void writeSettings(std::string &out) const;
  void readSetting(const char *line);

private:
  struct Snapshot {
    bool valid = false;
    size_t space = SIZE_MAX;
    bool paused = false;
    host::CpuState cpu;
    int pcLength = 1;
    // The bytes from `first` on, the rows in view and a margin either side.
    uint32_t first = 0;
    std::vector<uint8_t> bytes;
    bool activity = false;
    // From the caret, for the inspector.
    std::array<uint8_t, 8> atCaret{};
    // The //e's switches: a IIgs's are its Mega II's, which bank $00 obeys.
    SoftSwitches switches{};
    bool haveSwitches = false;
    std::optional<uint32_t> follow;
  };

  struct Edit {
    size_t space = 0;
    uint32_t address = 0;
    std::vector<uint8_t> before, after;
  };

  // What is in the selection, read a couple of times a second.
  struct SelectionStats {
    size_t space = SIZE_MAX;
    uint32_t low = 0, high = 0;
    double at = -10.0;
    size_t length = 0;
    uint8_t sum8 = 0, xor8 = 0;
    uint16_t sum16 = 0, crc16 = 0;
    size_t zeros = 0;
  };

  struct Region {
    uint32_t start, end; // inclusive, offsets in the bank
    const char *name;
    int hue;             // a stripe of the logo, -1 for none
  };

  enum class Follow { Off, PC, Stack, Expression };
  // How the bytes are shown: as hex, as a bitmap at any address in any of
  // several pixel formats, or as one of the machine's display pages decoded
  // by its own renderer.
  enum class View { Hex, Bitmap, Screen };
  enum class BitmapFormat { AppleHiRes, OneBit, TwoBit, FourBit, EightBit };
  enum class SearchKind { Hex, Text };

  void take();
  void refreshMap(host::MachineHost &host);
  void paintMap();

  const host::MemorySpace *space() const;
  uint32_t spaceEnd() const;
  int rowCount() const;
  uint32_t rowAddress(int row) const;
  int rowOf(uint32_t address) const;
  bool inSpace(uint32_t address) const;
  std::optional<uint8_t> byteAt(uint32_t address) const;
  std::vector<Region> regions() const;
  const Region *regionFor(uint32_t address, const std::vector<Region> &list) const;
  std::string formatAddress(uint32_t address) const;
  uint32_t stackAddress() const;
  uint32_t addressMask() const { return wide_ ? 0xFFFFFF : 0xFFFF; }

  // Moving about.
  void goTo(uint32_t address, bool remember = true);
  void reveal(uint32_t address);
  void back();
  void forward();
  void selectSpace(size_t index);
  void setCaret(uint32_t address, bool extend);
  bool hasSelection() const { return anchor_ != caret_; }
  uint32_t selectionLow() const { return std::min(anchor_, caret_); }
  uint32_t selectionHigh() const { return std::max(anchor_, caret_); }

  // Changing memory: through the machine's lock, recorded for undo.
  bool write(uint32_t address, const std::vector<uint8_t> &bytes, bool record = true);
  std::vector<uint8_t> read(uint32_t address, size_t count);
  void undo();
  void redo();
  void typeNibble(int value);
  void typeCharacter(unsigned c);
  void pasteHex(const std::string &text);
  void copySelection(int format);
  void saveSelection();
  void loadFile();

  void search(int direction);
  void runSearch();

  // Drawing.
  void drawToolbar();
  void drawSearchBar();
  void drawGrid(ImVec2 size);
  void drawViewOptions();
  void drawBitmap(ImVec2 size);
  void drawScreenView(ImVec2 size);
  void paintBitmap();
  void showTexture(ImTextureID &texture, int &width, int &height, const std::vector<uint8_t> &rgba, int w, int h);
  // Where a pixel of the screen view came from: the address, whether it is
  // the auxiliary bank's, and the rectangle of the byte's cell in the
  // page's own pixels.
  struct ScreenCell {
    uint32_t address = 0;
    bool aux = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };
  std::optional<ScreenCell> screenCellAt(int x, int y) const;
  void goToScreenCell(const ScreenCell &cell);
  int bitmapPixelsPerByte() const;
  void drawMap(ImVec2 origin, ImVec2 size);
  void drawSidebar(float width, float height);
  void drawInspector(float width);
  void drawSelectionCard(float width);
  void drawBanking(float width);
  void drawPlaces(float width);
  void drawStatus();
  void drawContextMenu();
  void drawFillPopup();
  void drawLoadPopup();

  Emulation &emulation_;
  Platform &platform_;
  CpuDebugger &debugger_;
  MachinePoll poll_;
  const MachineProfile *profile_ = nullptr;
  bool wide_ = false;
  bool open_ = false;

  std::vector<host::MemorySpace> spaces_;
  size_t space_ = 0;
  std::string wantedSpace_; // from the settings, until the spaces are known
  std::optional<uint32_t> wantedTop_;

  Snapshot snapshot_;
  // When each byte in view last changed, by address, and what it was.
  std::unordered_map<uint32_t, double> changed_;
  size_t changedSpace_ = SIZE_MAX;

  // The view.
  int columns_ = 16;
  int topRow_ = 0;
  float scrollPixels_ = 0;
  int visibleRows_ = 32;
  bool textApple_ = true; // Apple's screen codes rather than ASCII
  bool activityOn_ = false;
  bool activityApplied_ = false;
  Follow follow_ = Follow::Off;
  char followText_[96] = "";
  std::optional<uint32_t> pendingReveal_;

  // The caret, the other end of the selection, and typing.
  uint32_t caret_ = 0;
  uint32_t anchor_ = 0;
  bool textColumn_ = false;
  bool highNibbleTyped_ = false;
  bool selecting_ = false;
  bool gridFocused_ = false;
  std::optional<uint32_t> hovered_;
  uint32_t menuAddress_ = 0;

  std::vector<uint32_t> backStack_, forwardStack_;
  std::vector<Edit> undo_, redo_;
  std::vector<uint32_t> bookmarks_;

  // Searching.
  SearchKind searchKind_ = SearchKind::Hex;
  char searchText_[128] = "";
  bool searchBad_ = false;
  std::vector<uint32_t> matches_;
  size_t matchLength_ = 0;
  int matchIndex_ = -1;
  bool searchStale_ = true;

  char gotoText_[64] = "";
  bool gotoBad_ = false;
  char fillText_[16] = "00";
  bool openFill_ = false;
  // A file waiting to be put into memory, and where.
  std::vector<uint8_t> loadBytes_;
  std::string loadName_;
  char loadAt_[32] = "";
  bool openLoad_ = false;
  std::string message_;
  double messageAt_ = -10.0;

  SelectionStats stats_;

  // How lately the processor read and wrote each byte of the space, 0 to 1,
  // fading: the machine counts accesses and the viewer remembers them.
  std::vector<float> readGlow_, writeGlow_;
  std::vector<uint8_t> rawReads_, rawWrites_;
  bool haveRaw_ = false;
  double glowAt_ = 0.0;

  // The map: the whole space a pixel a byte, repainted a few times a second.
  std::vector<uint8_t> mapBytes_, mapReads_, mapWrites_;
  size_t mapSpace_ = SIZE_MAX;
  double mapTakenAt_ = -10.0;
  bool mapDirty_ = false;
  ImTextureID mapTexture_ = ImTextureID_Invalid;
  int mapWidth_ = 0, mapHeight_ = 0;
  bool draggingMap_ = false;

  float sidebarWidth_ = 300.0f;

  View view_ = View::Hex;

  // The bitmap: where it starts, its format and shape, and what was read.
  uint32_t bitmapTop_ = 0;
  BitmapFormat bitmapFormat_ = BitmapFormat::AppleHiRes;
  int bitmapWidth_ = 8;    // bytes a row, or tiles a row
  bool bitmapTiles_ = false; // eight bytes down a column, as a font's glyphs
  bool bitmapColour_ = true;
  int bitmapZoom_ = 3;
  int bitmapRows_ = 64;    // visible, at the last draw
  std::vector<uint8_t> bitmapBytes_;
  uint32_t bitmapBytesFrom_ = 0;
  ImTextureID bitmapTexture_ = ImTextureID_Invalid;
  int bitmapTextureWidth_ = 0, bitmapTextureHeight_ = 0;
  double bitmapPaintedAt_ = -10.0;

  // The screen view: which page, page 2 or not, and how it is decoded.
  int screenPage_ = 4; // MachineHost::DisplayPage, hi-res
  bool screenPage2_ = false;
  int screenColours_ = 0; // Solid, Exact, Mono
  uint32_t availablePages_ = 0; // a bit a DisplayPage the machine has
  std::vector<uint8_t> screenRgba_;
  int screenWidth_ = 0, screenHeight_ = 0;
  ImTextureID screenTexture_ = ImTextureID_Invalid;
  int screenTextureWidth_ = 0, screenTextureHeight_ = 0;
  double screenTakenAt_ = -10.0;
};

} // namespace a2e::native
