#include "video_frames.h"
#include "native_media.h"
#include "render_workers.h"
#include "video_timing.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

qint64 layerFootageTime(const Layer &l, double layerFrame, int fps) {
  double seconds = videoSourceSeconds(l, layerFrame, fps);
  if (l.timeRemap) {
    // Held inside the footage: before it, its first picture; after it, its last.
    const double last = l.sourceDurationMs > 0 ? (l.sourceDurationMs - 1) / 1000. : seconds;
    seconds = std::clamp(seconds, 0., std::max(0., last));
  }
  return qint64(std::llround(std::max(0., seconds) * 1e7));
}

QImage mixFrames(const QImage &first, const QImage &second, double amount) {
  if (second.isNull() || amount <= 0) return first;
  if (first.isNull() || amount >= 1) return second;
  const QImage a = first.convertToFormat(QImage::Format_RGB32);
  QImage b = second.convertToFormat(QImage::Format_RGB32);
  if (b.size() != a.size()) b = b.scaled(a.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  QImage out(a.size(), QImage::Format_RGB32);
  const uint w = uint(std::lround(amount * 256)), keep = 256 - w;
  const int bytes = a.width() * 4;
  renderParallel(a.height(), [&](int from, int to) {
    for (int y = from; y < to; ++y) {
      const uchar *pa = a.constScanLine(y), *pb = b.constScanLine(y);
      uchar *po = out.scanLine(y);
      for (int x = 0; x < bytes; ++x) po[x] = uchar((pa[x] * keep + pb[x] * w + 128) >> 8);
    }
  });
  return out;
}

void addRemappedSound(const int16_t *pcm, qint64 pcmFrames, const Layer &l, int fps, qint64 first, qint64 count, qint64 *sum) {
  if (!pcm || pcmFrames < 2 || count <= 0 || !sum) return;
  const double perSample = double(std::max(1, fps)) / 48000.; // composition frames a sample lasts
  for (qint64 n = 0; n < count; ++n) {
    const double layerFrame = double(first + n) * perSample - l.start;
    if (layerFrame < 0 || layerFrame >= l.duration) continue;
    // Where in the footage's sound this sample falls, and where the next one
    // does: how fast the footage is running here.
    const double at = double(layerFootageTime(l, layerFrame, fps)) * 48000. / 1e7;
    const double next = double(layerFootageTime(l, layerFrame + perSample, fps)) * 48000. / 1e7;
    // Held (or run past the footage's end): the picture stands still, and so
    // does the sound -- silence, not one sample repeated into a buzz.
    if (std::abs(next - at) < .02) continue;
    const qint64 i = qint64(std::floor(at));
    if (i < 0 || i + 1 >= pcmFrames) continue;
    const double f = at - double(i);
    for (int c = 0; c < 2; ++c)
      sum[size_t(n) * 2 + size_t(c)] += qint64(std::lround(pcm[i * 2 + c] * (1 - f) + pcm[(i + 1) * 2 + c] * f));
  }
}

int footageShrink(double onScreen) {
  if (!(onScreen > 0)) return 1;
  int shrink = 1;
  while (shrink < 4 && onScreen * shrink * 2 <= 1.0001) shrink *= 2;
  return shrink;
}
QImage footageFrame(NativeMedia &media, const QString &path, qint64 time, bool blend, qint64 *shownFrom, qint64 *shownUntil) {
  qint64 start = 0, length = 0;
  const QImage picture = media.videoFrame(path, time, &start, &length);
  if (shownFrom) *shownFrom = time;
  if (shownUntil) *shownUntil = time + 1;
  if (!blend && length > 0 && start <= time && time < start + length) {
    if (shownFrom) *shownFrom = start;
    if (shownUntil) *shownUntil = start + length;
  }
  if (!blend || picture.isNull() || length <= 0) return picture;
  const double amount = double(time - start) / double(length);
  // On a picture's own start there is nothing to mix.
  if (amount < .002) return picture;
  qint64 nextStart = 0, nextLength = 0;
  QImage next;
  // A little into the next picture, not on its edge: timestamps are whole
  // 100 ns steps, so at 30 fps every third picture starts one step after the
  // last one's start plus its length, and asking exactly there got the last
  // picture back -- no mix at all on one blended frame in three.
  const qint64 into = start + length + std::max<qint64>(1, std::min<qint64>(length / 4, 10000));
  try {
    next = media.videoFrame(path, into, &nextStart, &nextLength);
  } catch (...) {
    return picture;
  }
  // The last picture of the footage has no next one to go towards.
  if (next.isNull() || nextStart <= start) return picture;
  return mixFrames(picture, next, std::clamp(amount, 0., 1.));
}

// ---------------------------------------------------------------------------
// Several decoders, so the footage of one layer does not wait for another's:
// one thread decoding every video layer in turn took the sum of their times
// for each frame of playback.
static int videoDecodeWorkers() { return std::clamp(int(std::thread::hardware_concurrency()) / 6, 2, 4); }
VideoFrameServer::VideoFrameServer(std::function<void()> ready)
    : core(std::make_shared<VideoFrameCore>()), readyCall(std::make_shared<std::function<void()>>(std::move(ready))) {
  core->ready = readyCall;
  const int n = videoDecodeWorkers();
  core->workerCount = n;
  core->running = n;
  for (int i = 0; i < n; ++i)
    workers.emplace_back([core = core, i] {
      core->run(i);
      std::lock_guard<std::mutex> lock(core->doneGuard);
      --core->running;
      core->doneWake.notify_all();
    });
}
VideoFrameServer::~VideoFrameServer() {
  {
    std::lock_guard<std::mutex> lock(core->guard);
    core->stopping = true;
  }
  core->wake.notify_all();
  // Calls already queued for the editor find nothing to call.
  readyCall.reset();
  std::unique_lock<std::mutex> lock(core->doneGuard);
  const bool ended = core->doneWake.wait_for(lock, std::chrono::milliseconds(stopWaitMs), [&] { return core->running == 0; });
  lock.unlock();
  for (auto &worker : workers) {
    if (ended) worker.join();
    else worker.detach(); // holds the core itself, and ends with the program
  }
}
static std::atomic<int> g_decodeHold{0};
void VideoFrameServer::holdDecodingForTest(int ms) { g_decodeHold = std::max(0, ms); }
void VideoFrameCore::postReady() {
  if (!qApp || ready.expired()) return;
  QMetaObject::invokeMethod(qApp, [weak = ready] { if (const auto call = weak.lock(); call && *call) (*call)(); }, Qt::QueuedConnection);
}
void VideoFrameCore::setBudget(qint64 bytes) {
  std::lock_guard<std::mutex> lock(guard);
  budget = std::max<qint64>(64LL * 1024 * 1024, bytes);
}
int VideoFrameCore::ownerOf(const Want &want) {
  const QString stream = streamOf(want);
  const auto found = owners.find(stream);
  if (found != owners.end()) return found->second;
  const int owner = nextOwner++ % std::max(1, workerCount);
  owners.emplace(stream, owner);
  return owner;
}
static bool shows(const VideoFrameWant &want, qint64 time, bool blend, const QString &path, qint64 from, qint64 until,
                  int shrink) {
  if (blend != want.blend || path != want.path || shrink != want.shrink) return false;
  return want.time == time || (!blend && from <= want.time && want.time < until);
}
const VideoFrameCore::Made *VideoFrameCore::find(const Want &want) const {
  for (const auto &m : made_)
    if (shows(want, m.time, m.blend, m.path, m.from, m.until, m.shrink)) return &m;
  return nullptr;
}
bool VideoFrameCore::cannotDecode(const QString &path) const {
  std::lock_guard<std::mutex> lock(guard);
  return undecodable.count(path) > 0;
}
QImage VideoFrameCore::frame(const Want &want, bool *exact, QSize *footage) {
  std::lock_guard<std::mutex> lock(guard);
  if (undecodable.count(want.path)) {
    if (exact) *exact = false;
    return {};
  }
  for (auto &m : made_)
    if (shows(want, m.time, m.blend, m.path, m.from, m.until, m.shrink)) {
      m.used = ++clock;
      ++rememberedCount;
      if (exact) *exact = true;
      if (footage) *footage = m.footage;
      return m.image;
    }
  if (exact) *exact = false;
  const bool asked = std::any_of(urgent.begin(), urgent.end(), [&](const Want &w) {
    return w.time == want.time && w.blend == want.blend && w.path == want.path;
  });
  if (!asked) {
    ownerOf(want);
    urgent.insert(urgent.begin(), want);
    // Only the newest few for each footage: a scrub asks for dozens, and only
    // where it stopped matters.
    int same = 0;
    for (auto it = urgent.begin(); it != urgent.end();)
      if (streamOf(*it) == streamOf(want) && ++same > 4) it = urgent.erase(it);
      else ++it;
    wake.notify_all();
  }
  // Meanwhile the nearest picture of the same footage, so a layer does not blink empty.
  const Made *nearest = nullptr;
  for (const auto &m : made_)
    if (m.path == want.path && (!nearest || std::llabs(m.time - want.time) < std::llabs(nearest->time - want.time))) nearest = &m;
  if (nearest && footage) *footage = nearest->footage;
  return nearest ? nearest->image : QImage();
}
void VideoFrameCore::ahead(const std::vector<Want> &wants) {
  std::lock_guard<std::mutex> lock(guard);
  later.clear();
  wanted = wants;
  for (const auto &w : wants)
    if (!find(w) && !undecodable.count(w.path)) {
      ownerOf(w);
      later.push_back(w);
    }
  if (!later.empty()) wake.notify_all();
}
void VideoFrameCore::run(int index) {
  struct Reader { QString stream; std::unique_ptr<NativeMedia> media; quint64 used = 0; };
  std::vector<Reader> readers;
  quint64 turn = 0;
  // The reader for a stream, opened the first time it is needed; the least
  // used goes when there are more than a few.
  auto readerFor = [&](const QString &stream) -> NativeMedia * {
    for (auto &r : readers)
      if (r.stream == stream) { r.used = ++turn; return r.media.get(); }
    std::unique_ptr<NativeMedia> media;
    try {
      media = std::make_unique<NativeMedia>();
    } catch (...) {
      noDecoder = true;
      return nullptr; // no decoder on this system: layers keep what they have
    }
    if (readers.size() >= 8)
      readers.erase(std::min_element(readers.begin(), readers.end(), [](const Reader &a, const Reader &b) { return a.used < b.used; }));
    readers.push_back({stream, std::move(media), ++turn});
    return readers.back().media.get();
  };
  for (;;) {
    Want want;
    {
      std::unique_lock<std::mutex> lock(guard);
      // The first wish of a footage this worker decodes, urgent ones first.
      auto mine = [&](std::vector<Want> &from) {
        return std::find_if(from.begin(), from.end(), [&](const Want &w) {
          const auto found = owners.find(streamOf(w));
          return found != owners.end() && found->second == index;
        });
      };
      wake.wait(lock, [&] { return stopping || mine(urgent) != urgent.end() || mine(later) != later.end(); });
      if (stopping) return;
      if (auto it = mine(urgent); it != urgent.end()) { want = *it; urgent.erase(it); }
      else { auto next = mine(later); want = *next; later.erase(next); }
      if (find(want)) continue;
    }
    if (const int hold = g_decodeHold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(hold));
    QImage image;
    qint64 from = want.time, until = want.time + 1;
    QSize footage;
    QElapsedTimer spent;
    spent.start();
    NativeMedia *media = nullptr;
    try {
      media = readerFor(want.stream.isEmpty() ? want.path : want.stream);
      if (media) {
        media->setShrink(want.shrink);
        image = footageFrame(*media, want.path, want.time, want.blend, &from, &until);
        footage = media->footageSize();
      }
    } catch (...) {
      image = QImage();
      // Footage the decoders cannot open at all, as against a time past its
      // end: it is not asked for again, and the editor is told.
      if (media && !media->opensVideo(want.path)) {
        {
          std::lock_guard<std::mutex> lock(guard);
          undecodable.insert(want.path);
        }
        postReady();
      }
    }
    const double ms = spent.nsecsElapsed() / 1e6;
    if (image.isNull()) {
      // Told, so the editor can show the footage some other way.
      if (noDecoder) postReady();
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(guard);
      if (footage.isEmpty()) footage = image.size();
      made_.push_back({want.path, want.time, want.blend, image, ++clock, from, until, want.shrink, footage});
      ++madeCount;
      decodingTime += ms;
      // Until it fits, always keeping a few: first what is not wanted ahead,
      // least recently used first; then what is wanted latest. Least recently
      // used alone threw out the pictures made ahead -- not used yet, so the
      // oldest -- as soon as a few 4K ones filled the memory, and playback
      // found them gone when it got there.
      qint64 bytes = 0;
      for (const auto &m : made_) bytes += m.image.sizeInBytes();
      auto rank = [&](const Made &m) {
        for (size_t k = 0; k < wanted.size(); ++k)
          if (shows(wanted[k], m.time, m.blend, m.path, m.from, m.until, m.shrink)) return int(k);
        return -1;
      };
      while (bytes > budget && made_.size() > 3) {
        auto out = made_.end();
        int outRank = -1;
        for (auto it = made_.begin(); it != made_.end(); ++it) {
          const int r = rank(*it);
          const bool better = out == made_.end() ||
                              (r < 0 && (outRank >= 0 || it->used < out->used)) ||
                              (r >= 0 && outRank >= 0 && r > outRank);
          if (better) { out = it; outRank = r; }
        }
        bytes -= out->image.sizeInBytes();
        made_.erase(out);
      }
    }
    postReady();
  }
}
