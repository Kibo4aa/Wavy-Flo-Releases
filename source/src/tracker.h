#pragma once
// Motion tracking: a patch of footage followed from frame to frame, so a title
// can ride on something moving in a shot, or a shaky shot can be held still.
//
// A point lives on the video layer it was tracked in, in that layer's own
// coordinates (the footage's pixels, centred), at layer frames: moving, scaling
// or sliding the layer in time never moves what was tracked. Tracking decodes
// the footage itself, frame by frame, on a thread of its own.
#include "engine.h"
#include <QJsonArray>
#include <atomic>
#include <mutex>

struct TrackSample {
  double frame = 0;      // layer frame
  QPointF at;            // layer coordinates
  double confidence = 1; // how well the patch matched, 0..1; 1 where placed by hand
};
struct TrackPoint {
  QString name;
  int feature = 40; // side of the patch that is followed, footage pixels
  int search = 120; // side of the area it is looked for in, around where it should be
  std::vector<TrackSample> samples; // one per frame, sorted by frame
  // Where it is at a layer frame: between samples in a straight line, and held
  // at the first or last one outside what has been tracked.
  QPointF at(double frame) const;
  // The sample at exactly this frame, if there is one.
  const TrackSample *sampleAt(double frame) const;
  // Puts a sample at its frame, replacing one already there.
  void set(const TrackSample &sample);
};
std::vector<TrackPoint> layerTracks(const Layer &);
void setLayerTracks(Layer &, const std::vector<TrackPoint> &);

// A frame in grey, which is all the matcher looks at.
struct Luma {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels;
  bool isNull() const { return width <= 0 || height <= 0; }
};
Luma lumaOf(const QImage &);

// Where the patch of `reference` centred on `from` (pixel coordinates, corner
// at 0,0) is found in `next`, looked for within `search` around `guess`.
// Sub-pixel. The score is the normalised correlation of the best place, 1 for
// a perfect match; `found` is false when the patch is flat or the area empty.
struct PatchMatch {
  QPointF at;
  double score = 0;
  bool found = false;
};
PatchMatch matchPatch(const Luma &reference, QPointF from, int feature, const Luma &next, QPointF guess, int search);

// One run of the tracker over a video layer, forward or back from a frame.
// Filled in by the editor, run on a thread of its own by runTrack, read back
// through `guard` while it runs.
struct TrackJob {
  QString path;
  double sourceOffsetMs = 0, sourceDurationMs = 0;
  int fps = 30;
  double fromFrame = 0, toFrame = 0; // layer frames; toFrame below fromFrame tracks backwards
  QPointF start;                     // layer coordinates at fromFrame
  int feature = 40, search = 120;
  // Below this the patch is taken as lost and the run stops there.
  double lostBelow = .6;

  std::atomic<bool> cancel{false}, done{false};
  std::mutex guard;
  std::vector<TrackSample> samples; // what has been found so far, in the order found
  QString error;                    // why it stopped early, when it did
  double lostAt = -1;               // the frame the patch was lost at, or -1
};
void runTrack(TrackJob &job);

// Position keys for `target` that carry it with the point, keeping where it is
// now relative to the point at `referenceFrame` (a composition frame).
// Returns how many keys were set.
int applyTrackToLayer(Scene &scene, int trackedLayer, const TrackPoint &point, int targetLayer, double referenceFrame);
// Anchor keys on the tracked layer itself that pin the point where it is at
// `referenceFrame`: the footage moves so the point does not.
int stabilizeWithTrack(Scene &scene, int trackedLayer, const TrackPoint &point, double referenceFrame);

int checkTracker(QStringList &report);
