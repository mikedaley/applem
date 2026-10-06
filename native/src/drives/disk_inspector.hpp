/*
 * disk_inspector.hpp - What is recorded on a disk, shown in a drive window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "drives/disk_inspector_data.hpp"
#include "drives/disk_platter.hpp"
#include "app/platform.hpp"

#include "imgui.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace a2e::native {

/*
 * The disk being inspected, as its drive window has it this frame. The
 * inspector does not know what kind of drive it is looking at: a 5.25" disk's
 * rings are its 160 quarter tracks, a 3.5" disk's are one side's 80 tracks,
 * two rings to a track, and the window says which and reads a ring for it.
 */
struct InspectedDisk {
  int drive = -1;         // a different drive starts the view afresh
  int side = 0;           // a 3.5" disk's side: a different one reads again
  bool threeAndAHalf = false;
  bool hasDisk = false;
  std::string title;      // "Drive 1"
  std::string about;      // the disk and what is on it
  std::string status;     // the controller, as this drive sees it
  int headRing = 0;       // 0-159
  double spin = 0;        // the picture's turn, as the card draws it
  double spinSpeed = 0;
  bool active = false;
  bool writing = false;
  uint32_t revision = 0;  // moves whenever what is on the disk may have
  uint32_t overviewSerial = 0; // moves each time the overview is read again
  const Overview *overview = nullptr;
};

// Reads one ring in full, under the machine's lock.
using RingReader = std::function<TrackDetail(int ring)>;

/*
 * The browser's Disk Inspector (disk-inspector-window.js), in a drive
 * window: the platter large, turning with the disk, zoomable down to single
 * flux transitions; the picked track unrolled as a strip; its sectors in the
 * order they pass the head; and a sector's bytes or every nibble round it.
 */
class DiskInspector {
public:
  explicit DiskInspector(Platform &platform) : platform_(platform) {}
  ~DiskInspector();

  // Under the machine's lock: follow the head, and read the picked track and
  // the rings a zoomed view shows when they are wanted or have changed.
  void update(const InspectedDisk &disk, const RingReader &read, double now);
  // The panel, `width` wide, at the cursor.
  void draw(const InspectedDisk &disk, float width);

private:
  void drawPlatter(ImVec2 origin, float size);
  void drawLegend(float width);
  void drawTrackSummary(float width);
  void drawStrip(float width);
  void drawSectorChips(float width);
  void drawSectorBytes(float width, float height);
  void drawNibbles(float width, float height);
  void zoomPane(float factor);
  void zoomPaneFromWheel();
  void paintPlatterTexture();
  void paintView(int pixels);
  void fitPlatter();
  std::string ringName(int ring) const;

  Platform &platform_;
  InspectedDisk disk_; // as of the last draw

  int lastDrive_ = -1;
  int selectedRing_ = 0;
  bool followHead_ = true;
  // Where the head was last seen and since when: Follow head moves to a
  // track only once the head has stayed on it a moment.
  int headSeen_ = -1;
  double headSeenAt_ = 0;
  PlatterMode mode_ = PlatterMode::Structure;
  int pane_ = 0; // 0 the sector's bytes, 1 the nibbles
  float paneScale_ = 0.78f; // their text, as a scale of the window's font
  int selectedSector_ = 0;

  // The picked track, read in full.
  TrackDetail detail_;
  int detailDrive_ = -1;
  int detailSide_ = -1;
  uint32_t detailRevision_ = 0;
  double detailAt_ = -1;
  // The unrolled track's view, in cells: where it starts and how many show.
  double stripStart_ = 0;
  double stripSpan_ = 0;
  bool stripWhole_ = true;
  int stripRing_ = -1;

  // The platter, painted from the overview in the mode chosen.
  ImTextureID platter_ = ImTextureID_Invalid;
  int platterDrive_ = -1;
  int platterSide_ = -1;
  uint32_t platterSerial_ = 0;
  PlatterMode platterMode_ = PlatterMode::Structure;
  bool platterPainted_ = false;

  // Zoomed in: the part of the disk shown, painted afresh when it moves, and
  // the rings it shows read in full.
  PlatterView view_;
  ImTextureID viewTexture_ = ImTextureID_Invalid;
  int viewPixels_ = 0;
  bool viewStale_ = true;
  std::map<int, Ring> rings_;
  std::vector<int> wantedRings_;
  int ringsDrive_ = -1;
  int ringsSide_ = -1;
  uint32_t ringsRevision_ = 0;
  double ringsAt_ = -1;
};

} // namespace a2e::native
