#pragma once
// Inside the OpenFX host: what ofx_host.cpp (plug-ins run in this process) and
// ofx_worker.cpp (plug-ins run in the plug-in host process, and the public
// functions that choose between the two) share. Not for the rest of Wavy-Flo.
#include "ofx_host.h"
#include <QSet>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// A plug-in as the rest of Wavy-Flo holds it (OfxEffectEntry::plugin). Either
// loaded in this process -- `local` is ofx_host.cpp's own record of it -- or
// in the plug-in host process, where it is number `remote` in the list the
// worker keeps. The handle outlives a restart of the worker: `key` finds the
// same plug-in in the new one.
struct OfxPluginEffect {
  void *local = nullptr;
  int remote = -1;
  QString key; // identifier, then the binary it came from
  // Remote only: what is known of it here.
  std::atomic<int> varies{-1};      // see ofxVariesWithTime
  std::atomic<bool> broken{false};  // it stopped or hung the worker, or crashed in it
  std::mutex instancesGuard;
  QSet<QString> instances;          // the instance names asked for, for ofxDropInstances
  QHash<QString, QString> messages; // ofxEffectMessage by instance, as the plug-in host last said
};

// The plug-ins run in this process (ofx_host.cpp): the public functions'
// work, without the choice of where.
namespace ofx_local {
// A plug-in found twice among folders read for the first time together is
// loaded from the first place only. `taken` carries the bundles (by name)
// taken so far from one call into the next, for the plug-in host, which
// loads the folders it is asked for one at a time.
std::vector<OfxEffectEntry> loadEffects(const QStringList &folders, QStringList *problems, QSet<QString> *taken = nullptr);
void rememberRefusals(const QString &file, bool askAgain);
bool render(const OfxEffectEntry &, const QString &instance, QImage &image, double time, double fps,
            const QVector<float> &values, double renderScale, QString *error, const OfxRenderOptions &options);
double regionReach(const OfxEffectEntry &, const QString &instance, QSizeF content, double time, double fps,
                   const QVector<float> &values);
bool pressButton(const OfxEffectEntry &, const QString &instance, const QString &button, double time, double fps,
                 QVector<float> &values, QMap<QString, QString> &texts, const QImage &picture, QString *error,
                 const std::function<bool(const QString &, double)> &progress);
QString effectMessage(const OfxEffectEntry &, const QString &instance);
// What plug-ins said in passing in this process, taken; and added to (for
// what the plug-in host said).
QStringList takeNotices();
void addNotices(const QStringList &);
bool variesWithTime(const OfxEffectEntry &);
// Switched off after it crashed (or never loaded here).
bool isBroken(const OfxEffectEntry &);
void dropInstances(const std::function<bool(const QString &)> &keep);
int instanceCount();
void unloadAll();
QStringList hostGaps();
// A bundle written down as turned away as a whole (the worker cannot write it
// itself when it is the bundle that stopped it).
void rememberBundleRefused(const QString &file, const QString &binary, const QString &why);
// The quick way frames go in and out of a render, against Qt's own: where they
// differ, for --ofx-self-test. Empty when they agree.
QStringList pictureConversionProblems();
}

// OpenGL for plug-ins that draw on the card (ofx_gl.cpp). All but run are
// called inside a task run hands the card's thread, where the context is
// current.
namespace ofx_gl {
// `task` on the card's thread; false, and the task not run, when this machine
// gives no context.
bool run(const std::function<void()> &task);
// A texture of the picture at `bits` (bottom row first, RGBA at `depth`); 0
// when it could not be made.
unsigned upload(const void *bits, int width, int height, int rowBytes, const std::string &depth);
void drop(unsigned texture);
// What a render on the card draws into: a texture behind a framebuffer, bound,
// cleared and the viewport set to it.
struct Target {
  unsigned framebuffer = 0, texture = 0;
  int width = 0, height = 0;
  std::string depth;
  explicit operator bool() const { return framebuffer != 0; }
};
Target target(int width, int height, const std::string &depth);
bool read(const Target &, void *bits, int rowBytes);
void release(Target &);
}

// Told the binary of each bundle as it is about to be loaded (the worker
// passes it on, so a bundle that stops the worker while loading is known).
extern std::function<void(const QString &binary)> ofxBundleHook;
// Bundles not to be loaded at all this time (the worker is told them).
extern QStringList ofxSkippedBundles;
