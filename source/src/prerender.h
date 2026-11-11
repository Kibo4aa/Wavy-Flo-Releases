#pragma once
#include "engine.h"
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QPair>
#include <QVector>
#include <QCache>
#include <QSet>
#include <vector>
#include <future>
#include <memory>

// What a scene looks like, taken apart far enough to say which frames an edit
// can possibly have changed.
//
// The whole cache rests on that question being answered conservatively. A
// frame that is wrongly dropped costs a render nobody notices. A frame that is
// wrongly kept is a picture of something that no longer exists, shown to
// someone who has no way to tell. So every field that reaches a pixel is in
// here, and anything whose reach is not obvious -- the camera, the 3D flags,
// the composition itself -- is global: touch it and nothing survives.
struct PreRenderSignature {
  // Decides every frame at once.
  QByteArray global;
  // Per layer: everything that decides its pixels, folded together with its
  // parents' keys and with what is stacked behind it, so a parent move or a
  // reorder invalidates the layers it actually affects and no others.
  QHash<QString, QByteArray> layers;
  // The frames each layer can reach, inclusive.
  QHash<QString, QPair<int, int>> spans;
  // Frames whose content does not come from the scene: video, which the player
  // decodes as the playhead moves. Never stored, because a stored video frame
  // would be the same picture forever.
  QVector<QPair<int, int>> unstable;
  bool stableAt(int frame) const;
  // Everything that decides this one frame, as a single value. A render started
  // on a worker can be asked, when it comes back, whether it is still a picture
  // of the frame it was started for.
  QByteArray frameKey(int frame) const;
};

// context folds in everything true of the render but not of the scene: the
// preview resolution, the compositing backend.
PreRenderSignature preRenderSignature(const Scene &, const QByteArray &context);

// Run-length coding of an image, over whole pixels, row by row. Motion graphics
// are mostly flat: a transparent or single-colour background, solid fills, and
// antialiased edges only where something actually is. A 1080p frame of one
// title over nothing is eight megabytes of which nearly all is the same pixel
// repeated, and storing it as runs costs a fraction of that with every pixel
// still exactly itself. Lossless, so nothing about the preview changes.
QByteArray packImage(const QImage &);
QImage unpackImage(const QByteArray &, QSize, QImage::Format);

// The other half of the picture, literally: runs see nothing in a gradient,
// because no two neighbouring pixels are quite the same. A glow is gradient
// almost everywhere, which is why a busy frame barely packs at all.
//
// So the frame is taken apart instead -- this row's reds, then its greens, then
// blues, then alphas, a row at a time so the work stays inside cache -- each
// byte is replaced by its difference from the one to its left, which turns a
// smooth ramp into a near-constant, and the result is deflated. Measured on
// forty glowing layers at 1080p: runs manage 1.8x, this manages 29x, and it
// decodes in eleven milliseconds against the three hundred it would cost to
// render the frame again. Still lossless.
QByteArray packPlanes(const QImage &);
QImage unpackPlanes(const QByteArray &, QSize, QImage::Format);

// Light frames: the colour as a JPEG, the coverage beside it without loss (and
// nothing at all when the frame is opaque, which a finished frame nearly always
// is). A busy 1080p frame that packs to three or four megabytes exactly is a
// few hundred kilobytes this way -- a fraction of the memory, of the disk and of
// the writing -- and it is made and read back faster than the exact codings.
// Only for looking at: what is exported is always rendered, never read from here.
QByteArray packLight(const QImage &, int quality = 90);
QImage unpackLight(const QByteArray &, QSize, QImage::Format);

// A frame's packed bytes kept in a file instead of in memory: the pre-render
// cache on disk. Several cached frames that look the same share one, and the
// file goes when the last of them lets go.
struct DiskBlob {
  QString path;
  qint64 size = 0;
  ~DiskBlob();
  QByteArray read() const;
};

// Frames rendered ahead of time, so playing back is not rendering again.
class PreRenderCache {
public:
  struct Prepared {
    QByteArray data;
    QSize size;
    int format = 0;
    int coding = 0;
    // Set instead of data when the frame lives in a file.
    std::shared_ptr<const DiskBlob> disk;
    // Set instead of either while the frame is kept exactly as it was rendered:
    // not packed yet (see keep()).
    QImage image;
  };
  // CPU work on an immutable image; safe on the render coordinator.
  static Prepared prepare(const QImage &);
  // The frame as it is, for now. Packing a busy 1080p frame costs twice what
  // rendering it does, so a pre-render that packs as it goes falls behind the
  // playhead it is meant to be ahead of. Kept frames are shown without any
  // unpacking, and are packed later, when there is time (repack()).
  static Prepared keep(const QImage &);
  // Where prepare() keeps frames: a folder makes it write each packed frame to a
  // file there (from whichever thread is preparing it) and hand back a reference;
  // empty keeps them in memory, as before. Frames already held are not moved.
  static void setDiskDirectory(const QString &folder);
  // Light frames (packLight) instead of exact ones, for every frame prepared
  // from now on. The editor names the choice in the render context, so frames
  // of the other kind are never mixed in: switching empties the cache.
  static void setLight(bool);
  static bool light();
  static QString diskDirectory();
  // Folders of earlier sessions under `base` ("session-<process>") whose program
  // is no longer running -- left by a crash, or by a cache moved elsewhere --
  // taken away with what is in them. `keep` (this session's) never is. Returns
  // the bytes they held.
  static qint64 clearStaleSessionFolders(const QString &base, const QString &keep);
  void storePrepared(int frame, const Prepared &);
  void prefetch(int from, int total, int count=8);
  quint64 decodeCount() const { return decodedCount; }
  // Frames decoded by at() itself, on the caller's thread, because decode-ahead
  // had not got to them: each is a frame the interface spent its own time on.
  quint64 foregroundDecodes() const { return foregroundCount; }
  // Bytes of stored frames to keep. Older frames are dropped first.
  void setBudget(qint64 bytes);
  qint64 budget() const { return limit; }
  qint64 bytes() const { return held; }
  // Of those, the bytes kept in files.
  qint64 diskBytes() const { return diskHeld; }
  // How much of a cache on disk is kept in memory first, packed. Only what does
  // not fit there is written out: every frame straight to a file wrote the
  // drive for every second of timeline, again after every edit, again every
  // session, while most of it would have fitted in memory all along.
  static qint64 memoryFirstBytes();
  // Whether frames may go to files at all just now. Not while playing: a
  // playhead running through a four-minute song would write every frame of it,
  // gigabytes a pass, for pictures it has just shown. Then the memory part is
  // reused instead (dropBehind()).
  static void setSpillAllowed(bool);
  // Bytes written into cache files since the program started, to count what the
  // cache costs the drive.
  static qint64 diskBytesWritten();
  // How many more bytes may go to files; a frame that does not fit stays in
  // memory instead. The editor sets it from the day's limit.
  static void setDiskWriteRoom(qint64 bytes);
  // Drops frames held in memory -- not in files -- outside the `window` frames
  // from `from` onward, the ones longest gone past first, until `bytes` are
  // given back. Returns the bytes given back.
  qint64 dropBehind(int from, int total, int window, qint64 bytes);
  int count() const { return frames.size(); }
  // Bytes held by frames kept as rendered, and how many of those to allow: a
  // share of the machine's memory, whatever the budget -- a budget on disk can
  // be far larger than memory, and kept frames are in memory.
  qint64 hotBytes() const { return hotHeld; }
  qint64 hotBudget() const;
  // Up to `count` kept pictures, the ones needed last first: counting forward
  // from `from` and wrapping at `total`, the furthest away. What to pack next.
  std::vector<Prepared> hotPictures(int from, int total, int count) const;
  // Puts `packed` in place of the kept picture `hot` in every frame showing it.
  // Does nothing when no frame shows it any more.
  void repack(const Prepared &hot, const Prepared &packed);

  // Declares what the stored frames are pictures of, in one opaque piece. If it
  // differs from what is already held, everything held is discarded before
  // anything is returned. Returns true if that happened, so a caller can say so.
  bool expect(const QByteArray &fingerprint);
  QByteArray fingerprint() const { return sig.global; }

  // The same question asked per layer: keeps every frame the edit cannot have
  // touched, drops the rest. Returns how many were dropped.
  int reconcile(const PreRenderSignature &);
  const PreRenderSignature &signature() const { return sig; }
  // Drops the stored frames in [from, to]. The signature is left alone.
  void invalidate(int from, int to);

  bool has(int frame) const { return frames.contains(frame); }
  // The stored frame, or a null image. Never a frame from another signature.
  QImage at(int frame) const;
  void store(int frame, const QImage &);
  // Records that `frame` looks exactly like `like`, which is already held. The
  // two then share one copy and the budget is charged once between them: a
  // title standing still for three seconds is ninety frames of timeline and
  // one picture.
  bool shareFrom(int like, int frame);
  void clear();

  // Contiguous runs of cached frames within [from, to], for drawing the bar
  // that tells someone which parts of their timeline are ready.
  QVector<QPair<int, int>> ranges(int from, int to) const;
  // The cached frame furthest ahead of `from`, counting forward and wrapping at
  // `total`, or -1 when nothing is held. What to give up when the budget is
  // full and the playhead has moved somewhere the cache does not reach.
  int furthestAhead(int from, int total) const;

private:
  // Which of the three a frame is kept as. Whichever came out smallest: runs
  // win on flat frames, planes win on busy ones, and the plain rows are there
  // for the frames that defeat both.
  enum Coding { Rows = 0, Runs = 1, Planes = 2, Kept = 3, Light = 4 };
  using Held = Prepared;
  struct Decoded { Held source; QImage image; };
  static QImage decode(const Held &);
  void collectDecoded() const;
  void keepDecoded(const Held &, const QImage &) const;
  void place(int frame, Held entry);
public:
  // What makes two prepared frames the same picture: the buffer, or the file.
  static const void *keyOf(const Held &);
private:
  static qint64 sizeOf(const Held &);
  void charge(const Held &);
  void refund(const Held &);
  void evict();
  void dropAll();
  QMap<int, Held> frames;
  QVector<int> order;  // least recently stored first
  // Buffers in use and how many frames point at each, so frames that share one
  // picture are charged to the budget once between them.
  QHash<const void *, int> shares;
  PreRenderSignature sig;
  qint64 limit = 512LL * 1024 * 1024;
  qint64 held = 0;
  qint64 hotHeld = 0;
  qint64 diskHeld = 0;
  void updateSpill() const;
  // Paints arrive in bursts for one frame -- an overlay change, a resize, a
  // selection -- so the last frame unpacked is worth keeping whole.
  mutable int recentFrame = -1;
  mutable QImage recentImage;
  mutable Held recentHeld;
  // Bounded playback working set, keyed by picture rather than frame number.
  // Keeping source bytes here also prevents pointer reuse after eviction.
  mutable std::unique_ptr<QCache<const void *, Decoded>> decoded=
      std::make_unique<QCache<const void *, Decoded>>(65536); // KiB, <=64 MiB
  mutable std::vector<std::future<Decoded>> decodeJobs;
  mutable QSet<const void *> decoding; // pictures being decoded right now
  mutable quint64 decodedCount = 0, foregroundCount = 0;
private:
};

// Physical memory on this machine, or 0 when it cannot be asked. The cache
// budget is offered against this: a budget larger than the machine holds is not
// a bigger cache, it is swapping, and swapping a preview cache is slower than
// rendering the frame again.
qint64 physicalMemoryBytes();
// The largest budget worth offering here. Half of physical memory: a preview
// cache that leaves the machine nothing for the composition it is caching has
// defeated itself.
qint64 maximumCacheBudget();

int checkPreRender(QStringList &failures);
