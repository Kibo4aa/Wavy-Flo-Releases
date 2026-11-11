#pragma once
// Pictures from footage at any time of it: for export, and for every video
// layer the preview's media player does not drive -- a layer whose time is
// remapped (slowed, sped up, held, run backwards), and every video layer after
// the first.
#include "engine.h"
#include <atomic>
#include <condition_variable>
#include <map>
#include <set>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class NativeMedia;

// A time in the footage, in 100 ns, for a layer at a layer frame: Time Remap or
// straight on from the in-point, held inside the footage when it is remapped.
qint64 layerFootageTime(const Layer &, double layerFrame, int fps);
// The picture on screen in `path` at `time` (100 ns); with `blend`, mixed with
// the next picture by how far between the two the time falls.
// `shownFrom`/`shownUntil` get the stretch of footage time the returned
// picture stands for: the decoded picture's own, or just `time` when mixed.
QImage footageFrame(NativeMedia &media, const QString &path, qint64 time, bool blend,
                    qint64 *shownFrom = nullptr, qint64 *shownUntil = nullptr);
// A video layer's pictures for a viewer: 1, 2 or 4, as small as they can be
// made and still be drawn no smaller than they show. `onScreen` is how many
// viewer pixels one pixel of footage covers.
int footageShrink(double onScreen);
// (1 - amount) of a with amount of b, pixel by pixel.
QImage mixFrames(const QImage &a, const QImage &b, double amount);
// A remapped layer's sound, as its picture goes: samples [first, first + count)
// of the composition at 48 kHz, added into `sum` (stereo, interleaved). `pcm`
// is the footage's whole sound, 16-bit stereo at 48 kHz, `pcmFrames` long.
// Slowed, it plays slower and lower; backwards, backwards; held, silent.
void addRemappedSound(const int16_t *pcm, qint64 pcmFrames, const Layer &, int fps, qint64 first, qint64 count, qint64 *sum);

struct VideoFrameWant {
  QString path;
  qint64 time = 0;
  bool blend = false;
  // Whose picture it is (a layer's id). Each gets a reader of its own: two
  // layers of the same footage at different times, or two footages, would
  // otherwise pull one reader back and forth and make it seek every time.
  QString stream;
  // Made at 1/shrink of the footage's size (NativeMedia::setShrink).
  int shrink = 1;
};

// What the frame server's threads share with it: everything they read and
// write lives here, held by each of them as well as by the server, so a thread
// left behind -- stuck inside Windows' decoder, which once waited for good --
// never reads a server that is gone.
class VideoFrameCore {
public:
  using Want = VideoFrameWant;
  // The picture for this time if it has been made; otherwise asks for it
  // (ahead of everything waiting) and returns the made picture of the same
  // footage nearest in time, or null if there is none yet.
  // `footage`, when given, gets the footage's own size, whatever size the
  // picture was made at.
  QImage frame(const Want &want, bool *exact = nullptr, QSize *footage = nullptr);
  // What will be wanted next, most urgent first. Replaces what was asked ahead
  // before: a jump makes the old read-ahead worthless.
  void ahead(const std::vector<Want> &wants);
  // How many pictures it has made, and how many requests it answered from memory.
  quint64 made() const { return madeCount; }
  quint64 remembered() const { return rememberedCount; }
  // Time spent decoding, all told.
  double decodingMs() const { return decodingTime; }
  void setBudget(qint64 bytes);
  qint64 budgetBytes() const { return budget; }
  // False once this system turned out to have no decoder at all.
  bool decodes() const { return !noDecoder; }
  // Footage this system's decoders cannot open: asked for, it gives nothing.
  bool cannotDecode(const QString &path) const;

private:
  friend class VideoFrameServer;
  // `from`/`until`: the footage time the picture stands for. Playback asks at
  // fractions of a frame, which almost never fell on a time asked before; a
  // picture made ahead is the one for any time inside its own stretch.
  struct Made { QString path; qint64 time; bool blend; QImage image; quint64 used; qint64 from, until; int shrink; QSize footage; };
  void run(int index);
  const Made *find(const Want &) const;
  // The worker that decodes a stream: each footage stays with one, which keeps
  // its reader, and different footages decode side by side.
  int ownerOf(const Want &);
  static QString streamOf(const Want &w) { return w.stream.isEmpty() ? w.path : w.stream; }
  std::map<QString, int> owners;
  int nextOwner = 0;
  std::atomic<bool> noDecoder{false};
  std::set<QString> undecodable;
  // Called on the application's thread when a picture is made; gone with the
  // server, so a call queued after it is a call to nothing.
  std::weak_ptr<std::function<void()>> ready;
  void postReady();
  int workerCount = 1;
  // Threads still running, and the signal each gives as it ends.
  std::mutex doneGuard;
  std::condition_variable doneWake;
  int running = 0;
  mutable std::mutex guard;
  std::condition_variable wake;
  std::vector<Want> urgent, later;
  // Everything the last ahead() asked for, made or not: kept when memory is
  // short, ahead of what has already been shown.
  std::vector<Want> wanted;
  std::vector<Made> made_;
  qint64 budget = 768LL * 1024 * 1024;
  quint64 clock = 0, madeCount = 0, rememberedCount = 0;
  double decodingTime = 0;
  bool stopping = false;
};

// Decodes on threads of its own and remembers what it made. The editor asks
// for the picture it needs now and the ones it will need next; what is not
// ready yet arrives later through `ready`, called on the application's thread.
class VideoFrameServer {
public:
  using Want = VideoFrameWant;
  explicit VideoFrameServer(std::function<void()> ready = {});
  // Waits a few seconds for its threads; one still stuck in a decoder then is
  // let go rather than held for, which had held the whole editor on closing.
  ~VideoFrameServer();
  QImage frame(const Want &want, bool *exact = nullptr, QSize *footage = nullptr) { return core->frame(want, exact, footage); }
  void ahead(const std::vector<Want> &wants) { core->ahead(wants); }
  quint64 made() const { return core->made(); }
  quint64 remembered() const { return core->remembered(); }
  double decodingMs() const { return core->decodingMs(); }
  void setBudget(qint64 bytes) { core->setBudget(bytes); }
  qint64 budgetBytes() const { return core->budgetBytes(); }
  bool decodes() const { return core->decodes(); }
  bool cannotDecode(const QString &path) const { return core->cannotDecode(path); }
  // How long the server waits for its threads when it goes.
  static constexpr int stopWaitMs = 3000;
  // For a test: every thread waits this long before each picture it makes,
  // as one stuck in a decoder would.
  static void holdDecodingForTest(int ms);

private:
  std::shared_ptr<VideoFrameCore> core;
  std::shared_ptr<std::function<void()>> readyCall;
  std::vector<std::thread> workers;
};

int checkVideoFrames(QStringList &report);
