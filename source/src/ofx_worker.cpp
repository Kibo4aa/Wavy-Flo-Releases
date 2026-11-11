#include "ofx_internal.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRandomGenerator>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <werapi.h>
#include <csignal>
#include <cstdlib>
#include <exception>
#endif

// ---------------------------------------------------------------------------
// The plug-in host process. OpenFX plug-ins are other people's native code; in
// Wavy-Flo's own process one that ends the process, corrupts memory or never
// returns takes the editor -- and the person's work -- with it. So they run in
// a process of their own: this same executable started with --ofx-worker.
// Wavy-Flo talks to it over a named pipe only it may connect to, one
// connection per thread that renders; pictures go through shared memory. When
// the worker stops or a render does not come back in time, the plug-in that
// did it is switched off, the person is told, and the worker is started again
// with everything it had loaded.
// ---------------------------------------------------------------------------

namespace {
// Ask: the worker, in the middle of a render, asks for the layer at another
// time -- its picture (AskPicture) or the effect's controls (AskValues);
// Answer is Wavy-Flo's, on the same connection.
// Reach: how far an effect draws beyond the layer (ofxRegionReach). Press: a
// button of it pressed (ofxPressButton).
enum Kind : quint32 { Load = 1, Render = 2, Drop = 3, Count = 4, Unload = 5, Reach = 6, Press = 7, Reply = 100, Progress = 101, Ask = 102, Answer = 103 };
// AskProgress: a plug-in answering a button says how far it has gone, and
// hears whether to go on.
enum Question : qint32 { AskPicture = 0, AskValues = 1, AskProgress = 2 };
constexpr quint32 kLargestMessage = 1u << 28;

QByteArray message(quint32 kind, const QByteArray &payload) {
  QByteArray m(8, 0);
  const quint32 size = quint32(payload.size());
  std::memcpy(m.data(), &size, 4);
  std::memcpy(m.data() + 4, &kind, 4);
  return m + payload;
}
template <class Fill> QByteArray pack(Fill fill) {
  QByteArray bytes;
  QDataStream out(&bytes, QIODevice::WriteOnly);
  out.setVersion(QDataStream::Qt_6_5);
  fill(out);
  return bytes;
}

// A picture as its bytes: QImage's own stream format is a PNG, far too slow
// for one that goes with every frame.
void writePictures(QDataStream &out, const QMap<QString, QImage> &pictures) {
  out << qint32(pictures.size());
  for (auto it = pictures.cbegin(); it != pictures.cend(); ++it) {
    const QImage &image = it.value();
    out << it.key() << qint32(image.width()) << qint32(image.height()) << qint32(image.format())
        << qint32(image.bytesPerLine())
        << QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()), int(image.sizeInBytes()));
  }
}
QMap<QString, QImage> readPictures(QDataStream &in) {
  QMap<QString, QImage> pictures;
  qint32 count = 0;
  in >> count;
  for (qint32 n = 0; n < count && in.status() == QDataStream::Ok; ++n) {
    QString name;
    qint32 width = 0, height = 0, format = 0, rowBytes = 0;
    QByteArray bytes;
    in >> name >> width >> height >> format >> rowBytes >> bytes;
    if (width <= 0 || height <= 0 || rowBytes <= 0 || bytes.size() < qint64(rowBytes) * height) continue;
    pictures.insert(name, QImage(reinterpret_cast<const uchar *>(bytes.constData()), width, height, rowBytes,
                                 QImage::Format(format)).copy());
  }
  return pictures;
}

void writeEntry(QDataStream &out, const OfxEffectEntry &e) {
  out << e.name << e.group << e.identifier << e.binary << e.otherFrames << e.kind << qint32(e.card) << qint32(e.controls.size());
  for (const OfxControl &c : e.controls)
    out << qint32(c.kind) << c.id << c.label << c.minimum << c.maximum << c.initial << c.step << c.choices
        << c.colour[0] << c.colour[1] << c.colour[2] << c.colour[3] << c.text << qint32(c.textMode) << c.open
        << c.optional << c.mask << c.point << qint32(c.axis) << c.normalised;
  out << e.hidden;
}
OfxEffectEntry readEntry(QDataStream &in) {
  OfxEffectEntry e;
  qint32 count = 0;
  qint32 card = 0;
  in >> e.name >> e.group >> e.identifier >> e.binary >> e.otherFrames >> e.kind >> card >> count;
  e.card = card;
  for (qint32 k = 0; k < count && in.status() == QDataStream::Ok; ++k) {
    OfxControl c;
    qint32 kind = 0, mode = 0, axis = -1;
    in >> kind >> c.id >> c.label >> c.minimum >> c.maximum >> c.initial >> c.step >> c.choices >> c.colour[0] >>
        c.colour[1] >> c.colour[2] >> c.colour[3] >> c.text >> mode >> c.open >> c.optional >> c.mask >> c.point >> axis >>
        c.normalised;
    c.axis = axis;
    c.kind = OfxControl::Kind(std::clamp(int(kind), 0, int(OfxControl::Input)));
    c.textMode = OfxControl::TextMode(std::clamp(int(mode), 0, int(OfxControl::Label)));
    e.controls.push_back(c);
  }
  in >> e.hidden;
  return e;
}

std::atomic<bool> wanted{false};
std::atomic<int> renderTimeout{30000}, loadTimeout{60000};
std::mutex programGuard;
QString workerProgram;
} // namespace

#ifdef _WIN32
namespace {
// --- Pipes -----------------------------------------------------------------
// Wavy-Flo's end: every read and write may give up after a time, so a worker
// that hangs is found out.
struct Pipe {
  HANDLE handle = INVALID_HANDLE_VALUE;
  HANDLE event = nullptr;
  bool open() const { return handle != INVALID_HANDLE_VALUE; }
  void close() {
    if (open()) {
      CancelIoEx(handle, nullptr);
      CloseHandle(handle);
    }
    handle = INVALID_HANDLE_VALUE;
  }
  ~Pipe() {
    close();
    if (event) CloseHandle(event);
  }
  // False when the other end is gone, or -- `timedOut` -- did not keep up.
  bool transfer(bool writing, char *at, DWORD left, int timeoutMs, bool &timedOut) {
    if (!event) event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (left) {
      OVERLAPPED o{};
      o.hEvent = event;
      ResetEvent(event);
      DWORD done = 0;
      const BOOL now = writing ? WriteFile(handle, at, left, nullptr, &o) : ReadFile(handle, at, left, nullptr, &o);
      if (!now && GetLastError() != ERROR_IO_PENDING) return false;
      if (WaitForSingleObject(event, timeoutMs < 0 ? INFINITE : DWORD(timeoutMs)) != WAIT_OBJECT_0) {
        CancelIoEx(handle, &o);
        GetOverlappedResult(handle, &o, &done, TRUE);
        timedOut = true;
        return false;
      }
      if (!GetOverlappedResult(handle, &o, &done, FALSE) || done == 0) return false;
      at += done;
      left -= done;
    }
    return true;
  }
};

// The worker's end: plain blocking reads and writes; Wavy-Flo keeps the time.
bool writeAll(HANDLE h, const QByteArray &bytes) {
  const char *at = bytes.constData();
  DWORD left = DWORD(bytes.size());
  while (left) {
    DWORD done = 0;
    if (!WriteFile(h, at, left, &done, nullptr) || !done) return false;
    at += done;
    left -= done;
  }
  return true;
}
bool readAll(HANDLE h, char *at, DWORD left) {
  while (left) {
    DWORD done = 0;
    if (!ReadFile(h, at, left, &done, nullptr) || !done) return false;
    at += done;
    left -= done;
  }
  return true;
}
bool readMessage(HANDLE h, quint32 &kind, QByteArray &payload) {
  char head[8];
  if (!readAll(h, head, 8)) return false;
  quint32 size = 0;
  std::memcpy(&size, head, 4);
  std::memcpy(&kind, head + 4, 4);
  if (size > kLargestMessage) return false;
  payload.resize(int(size));
  return size == 0 || readAll(h, payload.data(), size);
}

// --- Shared memory for pictures -----------------------------------------------
struct Shared {
  HANDLE map = nullptr;
  uchar *view = nullptr;
  size_t size = 0;
  QString name;
  void release() {
    if (view) UnmapViewOfFile(view);
    if (map) CloseHandle(map);
    view = nullptr;
    map = nullptr;
    size = 0;
  }
  ~Shared() { release(); }
  bool ensure(size_t need) {
    if (view && size >= need) return true;
    release();
    static std::atomic<int> serial{0};
    const size_t rounded = ((std::max<size_t>(need, 1) + (8u << 20) - 1) / (8u << 20)) * (8u << 20);
    name = QString("Local\\WavyFloOfxPicture-%1-%2").arg(GetCurrentProcessId()).arg(serial++);
    map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(quint64(rounded) >> 32),
                             DWORD(rounded & 0xffffffffu), reinterpret_cast<LPCWSTR>(name.utf16()));
    if (!map) return false;
    view = static_cast<uchar *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, rounded));
    if (!view) {
      release();
      return false;
    }
    size = rounded;
    return true;
  }
};

// --- Wavy-Flo's side ------------------------------------------------------------
struct Channel {
  Pipe pipe;
  Shared shared;
  Shared other; // a picture at another time, asked for in the middle of a render
  quint64 generation = 0;
};

struct Worker {
  std::mutex life; // starting, loading, restarting -- one at a time
  HANDLE process = nullptr;
  std::atomic<quint32> pid{0}; // the running worker's, read without `life` (ofxRaisePluginWindow)
  QString pipe;
  quint64 generation = 0;
  HANDLE noteMap = nullptr;
  char *note = nullptr; // the worker's last words: the plug-in it was in, and what happened
  Channel control;      // loading and stopping, under `life`
  int failedStarts = 0;
  bool givenUp = false, stopped = false;
  QStringList folders, skipped;
  QString refusals;
  bool askAgain = false;
  std::map<QString, std::shared_ptr<OfxPluginEffect>> handles; // by key, for the life of the program
  std::vector<std::shared_ptr<OfxPluginEffect>> numbered;      // by the running worker's numbers
  QStringList gaps, log;
  std::mutex flightGuard;
  std::map<OfxPluginEffect *, int> inFlight;
};
Worker &worker() {
  static Worker *w = new Worker; // never destroyed: render threads may outlive static destruction
  return *w;
}
thread_local Channel channel;
// A render asked for while this thread's channel waits on one already: making
// the picture at another time can draw an earlier plug-in on the same layer.
// Each depth has a connection of its own.
thread_local int channelDepth = 0;
thread_local std::vector<std::unique_ptr<Channel>> deeperChannels;
Channel &channelFor(int depth) {
  if (depth <= 0) return channel;
  while (int(deeperChannels.size()) < depth) deeperChannels.push_back(std::make_unique<Channel>());
  return *deeperChannels[size_t(depth - 1)];
}

bool aliveLocked(Worker &w) { return w.process && WaitForSingleObject(w.process, 0) == WAIT_TIMEOUT; }

void forgetProcessLocked(Worker &w) {
  if (w.process) CloseHandle(w.process);
  w.process = nullptr;
  w.pid = 0;
  w.control.pipe.close();
  for (auto &[key, handle] : w.handles) handle->remote = -1;
  w.numbered.clear();
}

bool startLocked(Worker &w, QString *why) {
  if (w.stopped || w.givenUp) {
    if (why) *why = "the plug-in host is not running";
    return false;
  }
  if (w.process) {
    TerminateProcess(w.process, 9);
    WaitForSingleObject(w.process, 3000);
  }
  forgetProcessLocked(w);
  const quint64 generation = w.generation + 1;
  const QString tag = QString("%1-%2-%3")
                          .arg(GetCurrentProcessId())
                          .arg(generation)
                          .arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
  w.pipe = "\\\\.\\pipe\\WavyFloOfx-" + tag;
  const QString noteName = "Local\\WavyFloOfxNote-" + tag;
  if (w.note) UnmapViewOfFile(w.note);
  if (w.noteMap) CloseHandle(w.noteMap);
  w.note = nullptr;
  w.noteMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 1024,
                                 reinterpret_cast<LPCWSTR>(noteName.utf16()));
  if (w.noteMap) w.note = static_cast<char *>(MapViewOfFile(w.noteMap, FILE_MAP_ALL_ACCESS, 0, 0, 1024));
  if (w.note) {
    std::memset(w.note, 0, 1024);
    *reinterpret_cast<qint32 *>(w.note) = -1;
  }
  QString program;
  {
    std::lock_guard<std::mutex> lock(programGuard);
    program = workerProgram.isEmpty() ? QCoreApplication::applicationFilePath() : workerProgram;
  }
  std::wstring command = QString("\"%1\" --ofx-worker %2 %3 %4")
                             .arg(QDir::toNativeSeparators(program), w.pipe)
                             .arg(GetCurrentProcessId())
                             .arg(noteName)
                             .toStdWString();
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &info)) {
    ++w.failedStarts;
    if (why) *why = "the plug-in host could not be started";
    w.log << "could not start " + program;
    if (w.failedStarts >= 3) w.givenUp = true;
    return false;
  }
  CloseHandle(info.hThread);
  w.process = info.hProcess;
  w.pid = info.dwProcessId;
  // Listening once its pipe is there.
  const std::wstring pipeName = w.pipe.toStdWString();
  QElapsedTimer clock;
  clock.start();
  while (!WaitNamedPipeW(pipeName.c_str(), 100)) {
    if (!aliveLocked(w) || clock.elapsed() > 20000) {
      ++w.failedStarts;
      w.log << "the plug-in host did not come up";
      if (w.failedStarts >= 3) w.givenUp = true;
      if (w.process) TerminateProcess(w.process, 9);
      forgetProcessLocked(w);
      if (why) *why = "the plug-in host did not start";
      return false;
    }
    Sleep(10);
  }
  w.generation = generation;
  w.log << QString("started the plug-in host (process %1)").arg(info.dwProcessId);
  return true;
}

bool connectLocked(Worker &w, Channel &c) {
  if (c.pipe.open() && c.generation == w.generation) return true;
  c.pipe.close();
  const std::wstring name = w.pipe.toStdWString();
  for (int tries = 0; tries < 100; ++tries) {
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      c.pipe.handle = h;
      c.generation = w.generation;
      return true;
    }
    if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 200);
    else Sleep(10);
    if (!aliveLocked(w)) return false;
  }
  return false;
}

// A message and its answer. Progress on the way is handed to `progress`, and a
// question the worker asks on the way (Ask) to `asked`, whose answer goes
// back as Answer.
bool exchange(Channel &c, quint32 kind, const QByteArray &payload, int timeoutMs, QByteArray &reply, bool &timedOut,
              const std::function<void(const QByteArray &)> &progress = {},
              const std::function<QByteArray(const QByteArray &)> &asked = {}) {
  timedOut = false;
  QByteArray out = message(kind, payload);
  if (!c.pipe.transfer(true, out.data(), DWORD(out.size()), timeoutMs, timedOut)) return false;
  for (;;) {
    char head[8];
    if (!c.pipe.transfer(false, head, 8, timeoutMs, timedOut)) return false;
    quint32 size = 0, answer = 0;
    std::memcpy(&size, head, 4);
    std::memcpy(&answer, head + 4, 4);
    if (size > kLargestMessage) return false;
    QByteArray body(int(size), Qt::Uninitialized);
    if (size && !c.pipe.transfer(false, body.data(), size, timeoutMs, timedOut)) return false;
    if (answer == Progress) {
      if (progress) progress(body);
      continue;
    }
    if (answer == Ask) {
      QByteArray given = message(Answer, asked ? asked(body) : QByteArray());
      if (!c.pipe.transfer(true, given.data(), DWORD(given.size()), timeoutMs, timedOut)) return false;
      continue;
    }
    reply = body;
    return answer == Reply;
  }
}

// Every folder loaded so far and these, loaded in the running worker (it
// loads each folder once); the entries of `folders` come back. A bundle that
// stops or hangs the worker as it loads is left out from then on -- and
// remembered as such when refusals are -- and the worker started again.
std::vector<OfxEffectEntry> loadLocked(Worker &w, const QStringList &folders, QStringList *problems, bool &ok) {
  ok = false;
  QStringList all = w.folders;
  for (const QString &f : folders)
    if (!all.contains(f)) all << f;
  QHash<QString, int> stops; // bundles that stopped the worker this time, and how often
  for (int round = 0; round < 64; ++round) {
    if (!aliveLocked(w) && !startLocked(w, nullptr)) return {};
    if (!connectLocked(w, w.control)) {
      if (w.process) TerminateProcess(w.process, 9);
      forgetProcessLocked(w);
      w.log << "could not reach the plug-in host";
      if (++w.failedStarts >= 3) {
        w.givenUp = true;
        return {};
      }
      continue;
    }
    QString current;
    QByteArray reply;
    bool timedOut = false;
    const QByteArray request = pack([&](QDataStream &out) { out << all << w.refusals << w.askAgain << w.skipped; });
    const bool answered = exchange(w.control, Load, request, loadTimeout, reply, timedOut, [&](const QByteArray &p) {
      current = QString::fromUtf8(p);
    });
    if (!answered) {
      // It stopped (or hung) while loading `current`. How it ended, for the
      // log: its exit code, or that it was still running when the pipe broke.
      QString how;
      if (w.process) {
        // A process ending closes its pipe before Windows counts it as ended:
        // given a moment, one on its way out is seen to have gone.
        if (!timedOut) WaitForSingleObject(w.process, 2000);
        const bool running = aliveLocked(w);
        DWORD code = 0;
        if (!running && GetExitCodeProcess(w.process, &code)) how = QString(" (exit code 0x%1)").arg(code, 8, 16, QChar('0'));
        else if (running && !timedOut) how = " (still running when it stopped answering)";
        // Its last words, when it had time for them: a crash, std::terminate or abort.
        if (w.note) {
          const QString what = QString::fromUtf8(w.note + 4, int(strnlen(w.note + 4, 1000)));
          if (!what.isEmpty()) how += " (" + what + ")";
        }
        if (timedOut || running) TerminateProcess(w.process, 9);
        WaitForSingleObject(w.process, 3000);
      }
      forgetProcessLocked(w);
      if (current.isEmpty()) {
        if (++w.failedStarts >= 3) w.givenUp = true;
        w.log << "the plug-in host stopped before loading anything";
        if (w.givenUp) return {};
        continue;
      }
      const QString why = timedOut ? "hung the plug-in host while loading" : "stopped the plug-in host while loading";
      // Stopped, once: asked again in a new worker before it is left out (and
      // remembered as left out for two weeks). One commercial suite's bundles now and then
      // end the process from inside their own Load -- an exception thrown
      // through their own code -- and load cleanly the next time; leaving the
      // bundle out for that took effects that work with it.
      if (!timedOut && ++stops[current] < 2) {
        w.log << current + ": " + why + how + "; trying it once more";
        continue;
      }
      w.skipped << current;
      w.log << current + ": " + why + how;
      ofx_local::rememberBundleRefused(w.refusals, current, why);
      continue;
    }
    w.askAgain = false;
    w.failedStarts = 0;
    QDataStream in(reply);
    in.setVersion(QDataStream::Qt_6_5);
    qint32 folderCount = 0;
    in >> folderCount;
    std::vector<OfxEffectEntry> wantedEntries;
    for (qint32 f = 0; f < folderCount && in.status() == QDataStream::Ok; ++f) {
      QString folder;
      qint32 count = 0;
      in >> folder >> count;
      for (qint32 k = 0; k < count && in.status() == QDataStream::Ok; ++k) {
        qint32 index = -1;
        in >> index;
        OfxEffectEntry entry = readEntry(in);
        const QString key = entry.identifier + "|" + entry.binary;
        auto &handle = w.handles[key];
        if (!handle) {
          handle = std::make_shared<OfxPluginEffect>();
          handle->key = key;
        }
        handle->remote = index;
        if (index >= 0) {
          if (size_t(index) >= w.numbered.size()) w.numbered.resize(size_t(index) + 1);
          w.numbered[size_t(index)] = handle;
        }
        entry.plugin = handle;
        if (folders.contains(folder)) wantedEntries.push_back(entry);
      }
    }
    QStringList said, gaps;
    in >> said >> gaps;
    if (problems) *problems << said;
    for (const QString &g : gaps)
      if (!w.gaps.contains(g)) w.gaps << g;
    w.folders = all;
    ok = true;
    return wantedEntries;
  }
  return {};
}

bool ensureLocked(Worker &w, QString *why) {
  if (aliveLocked(w)) return true;
  if (!startLocked(w, why)) return false;
  if (w.folders.isEmpty()) return true;
  bool ok = false;
  loadLocked(w, {}, nullptr, ok);
  if (!ok && why) *why = "the plug-in host could not load the plug-ins again";
  return ok;
}

// The worker went away under this channel, or did not answer in time. Who
// did it is switched off: the one that timed out; the one the worker's last
// words name; or, failing those, the only one that was rendering.
void lost(Channel &c, OfxPluginEffect *suspect, bool timedOut) {
  Worker &w = worker();
  std::lock_guard<std::mutex> lock(w.life);
  c.pipe.close();
  if (c.generation != w.generation || !w.process) return; // seen to already
  if (timedOut || aliveLocked(w)) TerminateProcess(w.process, 9);
  WaitForSingleObject(w.process, 3000);
  OfxPluginEffect *culprit = timedOut ? suspect : nullptr;
  QString what;
  if (w.note) {
    const qint32 index = *reinterpret_cast<const qint32 *>(w.note);
    what = QString::fromUtf8(w.note + 4, int(strnlen(w.note + 4, 1000)));
    if (!culprit && index >= 0 && size_t(index) < w.numbered.size()) culprit = w.numbered[size_t(index)].get();
  }
  if (!culprit) {
    std::lock_guard<std::mutex> flight(w.flightGuard);
    if (w.inFlight.size() == 1) culprit = w.inFlight.begin()->first;
  }
  if (culprit) culprit->broken = true;
  w.log << QString("the plug-in host %1%2%3")
               .arg(timedOut ? "did not answer and was ended" : "stopped")
               .arg(what.isEmpty() ? QString() : " (" + what + ")")
               .arg(culprit ? "; switched off " + culprit->key : QString());
  forgetProcessLocked(w);
}

bool connectChannel(Channel &c, QString *why) {
  Worker &w = worker();
  std::lock_guard<std::mutex> lock(w.life);
  if (!ensureLocked(w, why)) return false;
  if (connectLocked(w, c)) return true;
  if (why) *why = "could not reach the plug-in host";
  return false;
}

bool remoteRender(const OfxEffectEntry &entry, const QString &key, QImage &image, double time, double fps,
                  const QVector<float> &values, double renderScale, QString *error, const OfxRenderOptions &options) {
  const OfxOtherTimes *frames = options.times;
  auto fail = [&](const QString &why) {
    if (error) *error = entry.name + ": " + why;
    return false;
  };
  OfxPluginEffect *handle = entry.plugin.get();
  if (!handle) return fail("not loaded");
  if (handle->broken) return fail("switched off: it stopped, hung or crashed in the plug-in host");
  if (image.isNull()) return true;
  {
    std::lock_guard<std::mutex> lock(handle->instancesGuard);
    handle->instances.insert(key);
  }
  Worker &w = worker();
  struct Deeper {
    Deeper() { ++channelDepth; }
    ~Deeper() { --channelDepth; }
  };
  Channel &channel = channelFor(channelDepth);
  const Deeper deeper;
  // The layer at another time, asked for by the plug-in: the controls, or the
  // picture -- made here, handed over through shared memory of its own, in
  // the format of this one.
  const bool pictures = frames && frames->at, controls = frames && frames->values;
  auto asked = [&](const QByteArray &question) {
    QDataStream in(question);
    in.setVersion(QDataStream::Qt_6_5);
    qint32 what = AskPicture;
    in >> what;
    if (what == AskValues) {
      QVector<double> when;
      in >> when;
      const QVector<QVector<float>> given = controls ? frames->values(when) : QVector<QVector<float>>();
      return pack([&](QDataStream &out) { out << given; });
    }
    double when = time;
    in >> when;
    QImage other = pictures ? frames->at(when) : QImage();
    if (!other.isNull()) other = other.convertToFormat(image.format());
    const bool fits = !other.isNull() && other.size() == image.size() && other.bytesPerLine() == image.bytesPerLine();
    const size_t otherBytes = fits ? size_t(other.bytesPerLine()) * size_t(other.height()) : 0;
    const bool given = fits && channel.other.ensure(otherBytes);
    if (given) std::memcpy(channel.other.view, other.constBits(), otherBytes);
    return pack([&](QDataStream &out) { out << given << channel.other.name << quint64(channel.other.size); });
  };
  for (int attempt = 0; attempt < 2; ++attempt) {
    QString why;
    if (!connectChannel(channel, &why)) return fail(why);
    const int index = handle->remote;
    if (index < 0) return fail("not available in the plug-in host");
    const size_t bytes = size_t(image.bytesPerLine()) * size_t(image.height());
    if (!channel.shared.ensure(bytes)) return fail("out of memory");
    std::memcpy(channel.shared.view, image.constBits(), bytes);
    const QByteArray request = pack([&](QDataStream &out) {
      out << qint32(index) << key << time << fps << values << renderScale << qint32(image.width())
          << qint32(image.height()) << qint32(image.format()) << qint32(image.bytesPerLine()) << channel.shared.name
          << quint64(channel.shared.size) << pictures << controls << (frames ? frames->first : 0.)
          << (frames ? frames->last : 0.) << (frames ? frames->keys : QVector<QVector<double>>()) << options.draft
          << options.content << options.texts;
      writePictures(out, options.inputs);
    });
    {
      std::lock_guard<std::mutex> flight(w.flightGuard);
      ++w.inFlight[handle];
    }
    QByteArray reply;
    bool timedOut = false;
    const bool answered = exchange(channel, Render, request, renderTimeout, reply, timedOut, {}, asked);
    {
      std::lock_guard<std::mutex> flight(w.flightGuard);
      if (--w.inFlight[handle] <= 0) w.inFlight.erase(handle);
    }
    if (answered) {
      QDataStream in(reply);
      in.setVersion(QDataStream::Qt_6_5);
      bool drawn = false, changed = false, broken = false;
      qint32 varies = 1;
      QString said, kept;
      QStringList notices;
      in >> drawn >> changed >> broken >> varies >> said >> kept >> notices;
      handle->varies = varies;
      {
        std::lock_guard<std::mutex> lock(handle->instancesGuard);
        handle->messages[key] = kept;
      }
      ofx_local::addNotices(notices);
      if (broken) handle->broken = true;
      if (!drawn) return fail(said.section(": ", 1).isEmpty() ? said : said.section(": ", 1));
      if (changed) std::memcpy(image.bits(), channel.shared.view, bytes);
      return true;
    }
    lost(channel, handle, timedOut);
    if (handle->broken)
      return fail(timedOut ? QString("did not finish within %1 s and is switched off").arg(renderTimeout / 1000)
                           : QString("stopped the plug-in host and is switched off"));
    // Another plug-in's doing: once more, in the new worker.
  }
  return fail("the plug-in host stopped");
}

// How far the effect draws beyond the layer, asked of the plug-in host. 0 when
// that cannot be had: the effect then keeps to the layer's own picture.
double remoteReach(const OfxEffectEntry &entry, const QString &key, QSizeF content, double time, double fps,
                   const QVector<float> &values) {
  OfxPluginEffect *handle = entry.plugin.get();
  if (!handle || handle->broken) return 0;
  {
    std::lock_guard<std::mutex> lock(handle->instancesGuard);
    handle->instances.insert(key);
  }
  Worker &w = worker();
  struct Deeper {
    Deeper() { ++channelDepth; }
    ~Deeper() { --channelDepth; }
  };
  Channel &channel = channelFor(channelDepth);
  const Deeper deeper;
  // Its number in the worker as it is now: one started again numbers anew.
  if (!connectChannel(channel, nullptr) || handle->remote < 0) return 0;
  const QByteArray request = pack([&](QDataStream &out) {
    out << qint32(handle->remote) << key << time << fps << values << content.width() << content.height();
  });
  {
    std::lock_guard<std::mutex> flight(w.flightGuard);
    ++w.inFlight[handle];
  }
  QByteArray reply;
  bool timedOut = false;
  const bool answered = exchange(channel, Reach, request, renderTimeout, reply, timedOut);
  {
    std::lock_guard<std::mutex> flight(w.flightGuard);
    if (--w.inFlight[handle] <= 0) w.inFlight.erase(handle);
  }
  if (!answered) {
    lost(channel, handle, timedOut);
    return 0;
  }
  QDataStream in(reply);
  in.setVersion(QDataStream::Qt_6_5);
  double reach = 0;
  bool broken = false;
  in >> reach >> broken;
  if (broken) handle->broken = true;
  return reach;
}

// A button pressed, in the plug-in host. The picture goes as bytes in the
// message: pressed by hand, once, it is not worth shared memory of its own.
bool remotePress(const OfxEffectEntry &entry, const QString &key, const QString &button, double time, double fps,
                 QVector<float> &values, QMap<QString, QString> &texts, const QImage &picture, QString *error,
                 const std::function<bool(const QString &, double)> &progress) {
  auto fail = [&](const QString &why) {
    if (error) *error = entry.name + ": " + why;
    return false;
  };
  OfxPluginEffect *handle = entry.plugin.get();
  if (!handle) return fail("not loaded");
  if (handle->broken) return fail("switched off: it stopped, hung or crashed in the plug-in host");
  {
    std::lock_guard<std::mutex> lock(handle->instancesGuard);
    handle->instances.insert(key);
  }
  Worker &w = worker();
  struct Deeper {
    Deeper() { ++channelDepth; }
    ~Deeper() { --channelDepth; }
  };
  Channel &channel = channelFor(channelDepth);
  const Deeper deeper;
  QString why;
  if (!connectChannel(channel, &why)) return fail(why);
  if (handle->remote < 0) return fail("not available in the plug-in host");
  // The picture as it is, as its bytes. Widened to float and streamed as a
  // QImage -- a PNG -- a 2560-pixel photo took about a second to write and read
  // before the plug-in heard of the press, and its window (a look suite's Edit
  // Look) came up that much later. The plug-in host makes it what the effect takes.
  QMap<QString, QImage> sent;
  if (!picture.isNull()) sent.insert(QString(), picture);
  const QByteArray request = pack([&](QDataStream &out) {
    out << qint32(handle->remote) << key << button << time << fps << values << texts;
    writePictures(out, sent);
  });
  {
    std::lock_guard<std::mutex> flight(w.flightGuard);
    ++w.inFlight[handle];
  }
  QByteArray reply;
  bool timedOut = false;
  // A button may open a window of the plug-in's own (an "Edit Look") and not
  // come back until it is closed: it has half an hour, not a render's time.
  constexpr int pressTimeout = 30 * 60 * 1000;
  // How far it has gone, asked across while it works: whoever is waiting
  // hears it, and says whether it is to go on.
  auto asked = [&](const QByteArray &question) {
    QDataStream in(question);
    in.setVersion(QDataStream::Qt_6_5);
    qint32 what = -1;
    QString label;
    double fraction = 0;
    in >> what >> label >> fraction;
    const bool go = what != AskProgress || !progress || progress(label, fraction);
    return pack([&](QDataStream &out) { out << go; });
  };
  const bool answered = exchange(channel, Press, request, pressTimeout, reply, timedOut, {}, asked);
  {
    std::lock_guard<std::mutex> flight(w.flightGuard);
    if (--w.inFlight[handle] <= 0) w.inFlight.erase(handle);
  }
  if (!answered) {
    lost(channel, handle, timedOut);
    return fail(timedOut ? QString("did not answer within %1 minutes and is switched off").arg(pressTimeout / 60000)
                         : QString("stopped the plug-in host"));
  }
  QDataStream in(reply);
  in.setVersion(QDataStream::Qt_6_5);
  bool taken = false, broken = false;
  QString said, kept;
  QVector<float> after;
  QMap<QString, QString> textsAfter;
  QStringList notices;
  in >> taken >> broken >> said >> after >> textsAfter >> kept >> notices;
  if (broken) handle->broken = true;
  {
    std::lock_guard<std::mutex> lock(handle->instancesGuard);
    handle->messages[key] = kept;
  }
  ofx_local::addNotices(notices);
  if (!taken) return fail(said.section(": ", 1).isEmpty() ? said : said.section(": ", 1));
  if (after.size() == values.size()) values = after;
  texts = textsAfter;
  return true;
}

// --- The worker's side --------------------------------------------------------
char *workerNote = nullptr;
thread_local int renderingNow = -1;
void lastWords(const char *what) {
  if (!workerNote) return;
  *reinterpret_cast<qint32 *>(workerNote) = renderingNow;
  strncpy_s(workerNote + 4, 1000, what, _TRUNCATE);
}
LONG WINAPI onCrash(EXCEPTION_POINTERS *info) {
  char what[96];
  sprintf_s(what, "a crash (0x%08lX)", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul);
  lastWords(what);
  TerminateProcess(GetCurrentProcess(), 3);
  return EXCEPTION_EXECUTE_HANDLER;
}
void onTerminate() {
  lastWords("it ended the process (std::terminate)");
  TerminateProcess(GetCurrentProcess(), 3);
}
void onAbort(int) {
  lastWords("it ended the process (abort)");
  TerminateProcess(GetCurrentProcess(), 3);
}

std::mutex servedGuard;
std::vector<OfxEffectEntry> served; // numbered as Wavy-Flo knows them
std::mutex loadGuard;

int numberOf(const OfxEffectEntry &e) {
  std::lock_guard<std::mutex> lock(servedGuard);
  const QString key = e.identifier + "|" + e.binary;
  for (size_t i = 0; i < served.size(); ++i)
    if (served[i].identifier + "|" + served[i].binary == key) return int(i);
  served.push_back(e);
  return int(served.size() - 1);
}

void serve(HANDLE h) {
  std::set_terminate(onTerminate);
  struct View {
    HANDLE map = nullptr;
    uchar *at = nullptr;
    quint64 size = 0;
  };
  std::map<QString, View> views;
  quint32 kind = 0;
  QByteArray body;
  while (readMessage(h, kind, body)) {
    QDataStream in(body);
    in.setVersion(QDataStream::Qt_6_5);
    QByteArray answer;
    if (kind == Load) {
      QStringList folders, skipped;
      QString refusals;
      bool askAgain = false;
      in >> folders >> refusals >> askAgain >> skipped;
      std::lock_guard<std::mutex> lock(loadGuard);
      ofxSkippedBundles = skipped;
      ofxBundleHook = [h](const QString &binary) { writeAll(h, message(Progress, binary.toUtf8())); };
      ofx_local::rememberRefusals(refusals, askAgain);
      QStringList problems;
      std::vector<std::pair<QString, std::vector<OfxEffectEntry>>> perFolder;
      // One set of bundles taken for all of them, as in a load in this
      // process: a plug-in in two of the folders is loaded from the first.
      QSet<QString> taken;
      for (const QString &folder : folders)
        perFolder.push_back({folder, ofx_local::loadEffects({folder}, &problems, &taken)});
      ofxBundleHook = nullptr;
      answer = pack([&](QDataStream &out) {
        out << qint32(perFolder.size());
        for (const auto &[folder, entries] : perFolder) {
          out << folder << qint32(entries.size());
          for (const OfxEffectEntry &e : entries) {
            out << qint32(numberOf(e));
            writeEntry(out, e);
          }
        }
        out << problems << ofx_local::hostGaps();
      });
    } else if (kind == Render) {
      qint32 index = -1, width = 0, height = 0, format = 0, rowBytes = 0;
      QString key, mapName;
      double time = 0, fps = 30, renderScale = 1;
      QVector<float> values;
      quint64 mapSize = 0;
      bool pictures = false, controls = false;
      double first = 0, last = 0;
      QVector<QVector<double>> keys;
      OfxRenderOptions options;
      in >> index >> key >> time >> fps >> values >> renderScale >> width >> height >> format >> rowBytes >> mapName >>
          mapSize >> pictures >> controls >> first >> last >> keys >> options.draft >> options.content >> options.texts;
      options.inputs = readPictures(in);
      OfxEffectEntry entry;
      {
        std::lock_guard<std::mutex> lock(servedGuard);
        if (index >= 0 && size_t(index) < served.size()) entry = served[size_t(index)];
      }
      View &view = views[mapName];
      if (!view.at) {
        view.map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, reinterpret_cast<LPCWSTR>(mapName.utf16()));
        if (view.map) view.at = static_cast<uchar *>(MapViewOfFile(view.map, FILE_MAP_ALL_ACCESS, 0, 0, size_t(mapSize)));
        view.size = mapSize;
      }
      bool drawn = false, changed = false;
      QString error;
      if (!entry.plugin) error = "not known to the plug-in host";
      else if (!view.at || quint64(rowBytes) * quint64(height) > view.size || width <= 0 || height <= 0)
        error = "the picture did not arrive";
      else {
        QImage image(view.at, width, height, rowBytes, QImage::Format(format));
        // Another time: asked of Wavy-Flo on this connection, which answers
        // before it goes on waiting for the render. A plug-in may ask from
        // several threads; one question at a time goes down the pipe.
        std::mutex asking;
        auto ask = [&](const QByteArray &question, QByteArray &given) {
          std::lock_guard<std::mutex> lock(asking);
          quint32 kind = 0;
          return writeAll(h, message(Ask, question)) && readMessage(h, kind, given) && kind == Answer;
        };
        OfxOtherTimes frames;
        frames.first = first;
        frames.last = last;
        frames.keys = keys;
        if (controls)
          frames.values = [&](const QVector<double> &when) -> QVector<QVector<float>> {
            QByteArray given;
            if (!ask(pack([&](QDataStream &out) { out << qint32(AskValues) << when; }), given)) return {};
            QDataStream back(given);
            back.setVersion(QDataStream::Qt_6_5);
            QVector<QVector<float>> got;
            back >> got;
            return got;
          };
        if (pictures)
          frames.at = [&](double when) -> QImage {
            QByteArray given;
            if (!ask(pack([&](QDataStream &out) { out << qint32(AskPicture) << when; }), given)) return {};
            QDataStream back(given);
            back.setVersion(QDataStream::Qt_6_5);
            bool ok = false;
            QString otherName;
            quint64 otherSize = 0;
            back >> ok >> otherName >> otherSize;
            if (!ok) return {};
            View &other = views[otherName];
            if (!other.at) {
              other.map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, reinterpret_cast<LPCWSTR>(otherName.utf16()));
              if (other.map) other.at = static_cast<uchar *>(MapViewOfFile(other.map, FILE_MAP_ALL_ACCESS, 0, 0, size_t(otherSize)));
              other.size = otherSize;
            }
            if (!other.at || quint64(rowBytes) * quint64(height) > other.size) return {};
            // Copied out: the next frame asked for comes through the same memory.
            return QImage(other.at, width, height, rowBytes, QImage::Format(format)).copy();
          };
        renderingNow = index;
        options.times = pictures || controls ? &frames : nullptr;
        drawn = ofx_local::render(entry, key, image, time, fps, values, renderScale, &error, options);
        renderingNow = -1;
        // Drawn into the shared memory itself, as a render is; or into a
        // picture of its own (a transition at its far end), copied there.
        changed = drawn;
        if (drawn && image.constBits() != view.at) {
          const qsizetype row = std::min<qsizetype>(rowBytes, image.bytesPerLine());
          for (int y = 0; y < height; ++y) std::memcpy(view.at + qsizetype(y) * rowBytes, image.constScanLine(y), size_t(row));
        }
      }
      const bool broken = entry.plugin && ofx_local::isBroken(entry);
      const qint32 varies = entry.plugin && ofx_local::variesWithTime(entry) ? 1 : 0;
      const QString kept = entry.plugin ? ofx_local::effectMessage(entry, key) : QString();
      const QStringList notices = ofx_local::takeNotices();
      answer = pack([&](QDataStream &out) { out << drawn << changed << broken << varies << error << kept << notices; });
    } else if (kind == Press) {
      qint32 index = -1;
      QString key, button, error;
      double time = 0, fps = 30;
      QVector<float> values;
      QMap<QString, QString> texts;
      in >> index >> key >> button >> time >> fps >> values >> texts;
      const QImage picture = readPictures(in).value(QString());
      OfxEffectEntry entry;
      {
        std::lock_guard<std::mutex> lock(servedGuard);
        if (index >= 0 && size_t(index) < served.size()) entry = served[size_t(index)];
      }
      // Progress, asked of Wavy-Flo on this connection; from any of the
      // plug-in's threads, one question at a time.
      std::mutex asking;
      auto progress = [&](const QString &label, double fraction) {
        std::lock_guard<std::mutex> lock(asking);
        quint32 kind = 0;
        QByteArray given;
        if (!writeAll(h, message(Ask, pack([&](QDataStream &out) { out << qint32(AskProgress) << label << fraction; }))) ||
            !readMessage(h, kind, given) || kind != Answer)
          return true;
        QDataStream back(given);
        back.setVersion(QDataStream::Qt_6_5);
        bool go = true;
        back >> go;
        return go;
      };
      renderingNow = index;
      const bool taken = entry.plugin && ofx_local::pressButton(entry, key, button, time, fps, values, texts, picture,
                                                                &error, progress);
      renderingNow = -1;
      if (!entry.plugin) error = "not known to the plug-in host";
      const bool broken = entry.plugin && ofx_local::isBroken(entry);
      const QString kept = entry.plugin ? ofx_local::effectMessage(entry, key) : QString();
      const QStringList notices = ofx_local::takeNotices();
      answer = pack([&](QDataStream &out) { out << taken << broken << error << values << texts << kept << notices; });
    } else if (kind == Reach) {
      qint32 index = -1;
      QString key;
      double time = 0, fps = 30, width = 0, height = 0;
      QVector<float> values;
      in >> index >> key >> time >> fps >> values >> width >> height;
      OfxEffectEntry entry;
      {
        std::lock_guard<std::mutex> lock(servedGuard);
        if (index >= 0 && size_t(index) < served.size()) entry = served[size_t(index)];
      }
      renderingNow = index;
      const double reach = entry.plugin ? ofx_local::regionReach(entry, key, QSizeF(width, height), time, fps, values) : 0.;
      renderingNow = -1;
      const bool broken = entry.plugin && ofx_local::isBroken(entry);
      answer = pack([&](QDataStream &out) { out << reach << broken; });
    } else if (kind == Drop) {
      QStringList going;
      in >> going;
      const QSet<QString> drop(going.begin(), going.end());
      ofx_local::dropInstances([&drop](const QString &instance) { return !drop.contains(instance); });
    } else if (kind == Count) {
      answer = pack([](QDataStream &out) { out << qint32(ofx_local::instanceCount()); });
    } else if (kind == Unload) {
      ofx_local::unloadAll();
    }
    if (!writeAll(h, message(Reply, answer))) break;

    if (kind == Unload) {
      FlushFileBuffers(h);
      TerminateProcess(GetCurrentProcess(), 0);
    }
  }
  for (auto &[name, view] : views) {
    if (view.at) UnmapViewOfFile(view.at);
    if (view.map) CloseHandle(view.map);
  }
  DisconnectNamedPipe(h);
  CloseHandle(h);
}
} // namespace
#endif // _WIN32

// ---------------------------------------------------------------------------
// The public functions: each in this process, or in the worker.
// ---------------------------------------------------------------------------
void ofxUseWorker(bool on, const QString &program) {
  {
    std::lock_guard<std::mutex> lock(programGuard);
    if (!program.isEmpty()) workerProgram = program;
  }
#ifdef _WIN32
  wanted = on;
#else
  (void)on;
#endif
}
bool ofxWorkerInUse() { return wanted; }
void ofxSetWorkerTimeouts(int renderMs, int loadMs) {
  renderTimeout = std::max(100, renderMs);
  loadTimeout = std::max(100, loadMs);
}
QStringList ofxWorkerLog() {
#ifdef _WIN32
  Worker &w = worker();
  std::lock_guard<std::mutex> lock(w.life);
  return w.log;
#else
  return {};
#endif
}
// Windows lets only the program in front put a window in front. A plug-in's
// window -- a look suite's Edit Look -- belongs to the plug-in host, a process
// in the background, and opened behind Wavy-Flo.
void ofxLetPluginWindowsForward() {
#ifdef _WIN32
  AllowSetForegroundWindow(ASFW_ANY);
#endif
}
bool ofxRaisePluginWindow() {
#ifdef _WIN32
  const DWORD host = worker().pid.load();
  if (!host) return false;
  // The host and every program it started (a plug-in may open its editor as
  // a program of its own), and theirs.
  std::set<DWORD> family{host};
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    std::vector<std::pair<DWORD, DWORD>> parents; // a process, and the one that started it
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
      parents.push_back({entry.th32ProcessID, entry.th32ParentProcessID});
    CloseHandle(snapshot);
    for (bool grew = true; grew;) {
      grew = false;
      for (const auto &[process, parent] : parents)
        if (family.count(parent) && family.insert(process).second) grew = true;
    }
  }
  struct Search {
    const std::set<DWORD> *family;
    HWND found = nullptr;
  } search{&family};
  EnumWindows(
      [](HWND window, LPARAM data) -> BOOL {
        auto *s = reinterpret_cast<Search *>(data);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        RECT box{};
        if (!s->family->count(process) || !IsWindowVisible(window) || !GetWindowRect(window, &box) ||
            box.right - box.left < 50 || box.bottom - box.top < 50)
          return TRUE;
        s->found = window;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&search));
  if (!search.found) return false;
  if (IsIconic(search.found)) ShowWindow(search.found, SW_RESTORE);
  SetForegroundWindow(search.found);
  return true;
#else
  return false;
#endif
}

std::vector<OfxEffectEntry> ofxLoadEffects(const QStringList &folders, QStringList *problems) {
#ifdef _WIN32
  if (wanted) {
    static std::once_flag registered;
    std::call_once(registered, [] {
      if (QCoreApplication::instance()) qAddPostRoutine(ofxUnloadAll);
    });
    Worker &w = worker();
    std::lock_guard<std::mutex> lock(w.life);
    bool ok = false;
    auto entries = loadLocked(w, folders, problems, ok);
    if (!ok && problems) *problems << "the plug-in host could not be started; OpenFX plug-ins are off";
    return entries;
  }
#endif
  return ofx_local::loadEffects(folders, problems);
}

void ofxRememberRefusals(const QString &file, bool askAgain) {
  ofx_local::rememberRefusals(file, askAgain);
#ifdef _WIN32
  Worker &w = worker();
  std::lock_guard<std::mutex> lock(w.life);
  w.refusals = file;
  w.askAgain = askAgain;
#endif
}

static bool remote(const OfxEffectEntry &entry) { return entry.plugin && !entry.plugin->local; }

bool ofxRender(const OfxEffectEntry &entry, const QString &instance, QImage &image, double time, double fps,
               const QVector<float> &values, double renderScale, QString *error, const OfxRenderOptions &options) {
#ifdef _WIN32
  if (remote(entry)) return remoteRender(entry, instance, image, time, fps, values, renderScale, error, options);
#endif
  return ofx_local::render(entry, instance, image, time, fps, values, renderScale, error, options);
}

bool ofxPressButton(const OfxEffectEntry &entry, const QString &instance, const QString &button, double time, double fps,
                    QVector<float> &values, QMap<QString, QString> &texts, const QImage &picture, QString *error,
                    const std::function<bool(const QString &, double)> &progress) {
#ifdef _WIN32
  if (remote(entry)) return remotePress(entry, instance, button, time, fps, values, texts, picture, error, progress);
#endif
  return ofx_local::pressButton(entry, instance, button, time, fps, values, texts, picture, error, progress);
}

QString ofxEffectMessage(const OfxEffectEntry &entry, const QString &instance) {
  if (remote(entry)) {
    std::lock_guard<std::mutex> lock(entry.plugin->instancesGuard);
    return entry.plugin->messages.value(instance);
  }
  return ofx_local::effectMessage(entry, instance);
}

QStringList ofxTakeNotices() { return ofx_local::takeNotices(); }

double ofxRegionReach(const OfxEffectEntry &entry, const QString &instance, QSizeF content, double time, double fps,
                      const QVector<float> &values) {
  if (entry.kind == "generator") return 0;
#ifdef _WIN32
  if (remote(entry)) return remoteReach(entry, instance, content, time, fps, values);
#endif
  return ofx_local::regionReach(entry, instance, content, time, fps, values);
}

bool ofxVariesWithTime(const OfxEffectEntry &entry) {
  if (remote(entry)) return entry.plugin->varies != 0;
  return ofx_local::variesWithTime(entry);
}

void ofxDropInstances(const std::function<bool(const QString &instance)> &keep) {
  ofx_local::dropInstances(keep);
#ifdef _WIN32
  Worker &w = worker();
  QStringList going;
  {
    std::lock_guard<std::mutex> lock(w.life);
    for (auto &[key, handle] : w.handles) {
      std::lock_guard<std::mutex> held(handle->instancesGuard);
      for (auto it = handle->instances.begin(); it != handle->instances.end();) {
        if (keep(*it)) {
          ++it;
          continue;
        }
        going << *it;
        it = handle->instances.erase(it);
      }
    }
    if (going.isEmpty() || !aliveLocked(w)) return;
  }
  QString why;
  if (!connectChannel(channel, &why)) return;
  QByteArray reply;
  bool timedOut = false;
  if (!exchange(channel, Drop, pack([&](QDataStream &out) { out << going; }), renderTimeout, reply, timedOut))
    lost(channel, nullptr, timedOut);
#endif
}

int ofxInstanceCount() {
  int count = ofx_local::instanceCount();
#ifdef _WIN32
  Worker &w = worker();
  {
    std::lock_guard<std::mutex> lock(w.life);
    if (!aliveLocked(w)) return count;
  }
  QString why;
  if (!connectChannel(channel, &why)) return count;
  QByteArray reply;
  bool timedOut = false;
  if (exchange(channel, Count, {}, renderTimeout, reply, timedOut)) {
    QDataStream in(reply);
    in.setVersion(QDataStream::Qt_6_5);
    qint32 there = 0;
    in >> there;
    count += there;
  } else {
    lost(channel, nullptr, timedOut);
  }
#endif
  return count;
}

void ofxUnloadAll() {
#ifdef _WIN32
  Worker &w = worker();
  {
    std::lock_guard<std::mutex> lock(w.life);
    if (aliveLocked(w) && connectLocked(w, w.control)) {
      QByteArray reply;
      bool timedOut = false;
      exchange(w.control, Unload, {}, 5000, reply, timedOut);
      if (WaitForSingleObject(w.process, 3000) != WAIT_OBJECT_0) TerminateProcess(w.process, 0);
    } else if (w.process) {
      TerminateProcess(w.process, 0);
    }
    forgetProcessLocked(w);
    w.stopped = true;
  }
#endif
  ofx_local::unloadAll();
}

QStringList ofxHostGaps() {
  QStringList gaps = ofx_local::hostGaps();
#ifdef _WIN32
  Worker &w = worker();
  std::lock_guard<std::mutex> lock(w.life);
  for (const QString &g : w.gaps)
    if (!gaps.contains(g)) gaps << g;
#endif
  return gaps;
}

int ofxWorkerMain(int argc, char **argv) {
#ifdef _WIN32
  if (argc < 5) return 2;
  // No dialogs from this process, whatever a plug-in does: it has no window
  // for anyone to answer, and Wavy-Flo is waiting on it.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  WerSetFlags(WER_FAULT_REPORTING_NO_UI);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  QCoreApplication app(argc, argv);
  const std::wstring pipe = QString::fromLocal8Bit(argv[2]).toStdWString();
  const DWORD parent = DWORD(QByteArray(argv[3]).toULong());
  const QString noteName = QString::fromLocal8Bit(argv[4]);
  if (HANDLE noteMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, reinterpret_cast<LPCWSTR>(noteName.utf16())))
    workerNote = static_cast<char *>(MapViewOfFile(noteMap, FILE_MAP_ALL_ACCESS, 0, 0, 1024));
  SetUnhandledExceptionFilter(onCrash);
  std::set_terminate(onTerminate);
  std::signal(SIGABRT, onAbort);
  // Gone with Wavy-Flo, however Wavy-Flo goes.
  std::thread([parent] {
    if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, parent)) WaitForSingleObject(p, INFINITE);
    TerminateProcess(GetCurrentProcess(), 0);
  }).detach();
  for (;;) {
    HANDLE h = CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 2;
    if (!ConnectNamedPipe(h, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
      CloseHandle(h);
      continue;
    }
    // Only Wavy-Flo itself.
    ULONG client = 0;
    if (!GetNamedPipeClientProcessId(h, &client) || client != parent) {
      DisconnectNamedPipe(h);
      CloseHandle(h);
      continue;
    }
    std::thread([h] { serve(h); }).detach();
  }
#else
  (void)argc;
  (void)argv;
  return 2;
#endif
}
