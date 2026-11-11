#pragma once
// Two ways of doing one kind of work -- on the processor and on the graphics
// card -- and which of them is the quicker here, for work of about this size,
// learned by doing it. Nothing is assumed about the machine: a desktop card, a
// laptop's integrated chip sharing the processor's memory, an old processor
// with two cores, all answer differently, and each answers for itself.
//
// Each size (by powers of two, in pixels) starts on the processor, which is
// timed twice, then the card, timed twice; from then on the quicker of the two
// does the work, and the other is tried again now and then -- a laptop that
// slows as it warms, a card busy with something else, is followed. A card that
// could not do it at all is not asked again for that size for a good while.
//
// Any new piece of work that has both forms gets all of this by asking card()
// before it starts and saying what it took with took(): neither form has to be
// tuned for one machine or the other.
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QtGlobal>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

class RouteChoice {
public:
  // `recheckEvery`: how many uses of the quicker way before the other is tried
  // again. Where the two ways give a picture a little different (two ways of
  // sampling a motion blur), seldom, so the change does not show.
  explicit RouteChoice(const QByteArray &name, int recheckEvery = 48) : name(name), recheck(recheckEvery) {}
  // Whether to do work of `size` pixels on the card now (when it can).
  bool card(qint64 size) {
    if (forced() >= 0) return forced() == 1;
    std::lock_guard<std::mutex> lock(guard);
    Bucket &b = bucket(size);
    if (b.refusedFor > 0) { --b.refusedFor; return false; }
    if (b.cpuCount < kTrials) return false;
    if (b.gpuCount < kTrials) return true;
    const bool cardQuicker = b.gpuCost < b.cpuCost * kMargin;
    // Now and then the other one, to see whether that is still so.
    if (++b.sinceOther >= recheck) { b.sinceOther = 0; return !cardQuicker; }
    return cardQuicker;
  }
  // What a piece of `size` took, in milliseconds, on the card or not.
  void took(qint64 size, bool onCard, double ms) {
    if (forced() >= 0 || size <= 0) return;
    std::lock_guard<std::mutex> lock(guard);
    Bucket &b = bucket(size);
    const double cost = ms / (double(size) / 1e6); // per megapixel, so sizes in a bucket compare
    double &kept = onCard ? b.gpuCost : b.cpuCost;
    int &count = onCard ? b.gpuCount : b.cpuCount;
    // The first time is often cold (a program compiled, a buffer made), so it
    // only counts until the second replaces it; later ones move it a quarter.
    kept = count < 2 ? cost : kept * .75 + cost * .25;
    ++count;
  }
  // The card was asked and could not: not again for this size for a while.
  void refused(qint64 size) {
    if (forced() >= 0) return;
    std::lock_guard<std::mutex> lock(guard);
    bucket(size).refusedFor = kRefusal;
  }
  // What has been learned, for the person and the checks.
  QString report() {
    std::lock_guard<std::mutex> lock(guard);
    QString text = QString::fromUtf8(name) + ":";
    for (size_t i = 0; i < buckets.size(); ++i) {
      const Bucket &b = buckets[i];
      if (!b.cpuCount && !b.gpuCount) continue;
      text += QString(" [~%1 MP: CPU %2 ms/MP x%3, card %4 ms/MP x%5]")
                  .arg(std::ldexp(1., int(i) + kSmallest) / 1e6, 0, 'g', 2)
                  .arg(b.cpuCost, 0, 'f', 2).arg(b.cpuCount).arg(b.gpuCost, 0, 'f', 2).arg(b.gpuCount);
    }
    return text;
  }
  // Within one, on this thread, every RouteChoice takes the way it is given
  // and learns nothing: for the checks, which compare the two ways.
  struct Force {
    explicit Force(bool onCard) : previous(forced()) { forced() = onCard ? 1 : 0; }
    ~Force() { forced() = previous; }
    int previous;
  };

private:
  static constexpr int kTrials = 2, kRefusal = 200, kSmallest = 10;
  static constexpr double kMargin = .9; // the card has to be clearly quicker
  struct Bucket {
    double cpuCost = 0, gpuCost = 0;
    int cpuCount = 0, gpuCount = 0, sinceOther = 0, refusedFor = 0;
  };
  Bucket &bucket(qint64 size) {
    int index = 0;
    while (index + 1 < int(buckets.size()) && (qint64(1) << (index + kSmallest + 1)) <= size) ++index;
    return buckets[size_t(index)];
  }
  static int &forced() {
    thread_local int value = -1;
    return value;
  }
  QByteArray name;
  int recheck = 48;
  std::mutex guard;
  std::array<Bucket, 22> buckets{};
};
// One RouteChoice for each kind of work, made the first time the kind is asked
// for: the same chain of effects learns for itself, apart from other chains
// that cost the two ways differently.
inline RouteChoice &routeFor(const QByteArray &kind) {
  static std::mutex guard;
  static std::map<QByteArray, std::unique_ptr<RouteChoice>> routes;
  std::lock_guard<std::mutex> lock(guard);
  auto &route = routes[kind];
  if (!route) route = std::make_unique<RouteChoice>(kind);
  return *route;
}
// Timing a piece of work done the processor's way, for `route`, from when it is
// made to when it goes -- whichever way the work then returns. Null: not timed.
struct ProcessorTiming {
  RouteChoice *route;
  qint64 size;
  QElapsedTimer clock;
  ProcessorTiming(RouteChoice *route, qint64 size) : route(route), size(size) { clock.start(); }
  ~ProcessorTiming() { if (route) route->took(size, false, clock.nsecsElapsed() / 1e6); }
};
// A piece of work done the quicker way: `onCard()` does it on the card and
// says whether it could, `onProcessor()` does it on the processor, which always
// can. `cardCan` false (no card here, or not for this) goes straight to the
// processor and learns nothing. This is all a new piece of work with both forms
// needs to be done on whichever is the quicker on the machine it runs on.
template <class Card, class Processor>
void quickerWay(RouteChoice &route, qint64 size, bool cardCan, Card &&onCard, Processor &&onProcessor) {
  if (!cardCan) { onProcessor(); return; }
  if (route.card(size)) {
    QElapsedTimer clock;
    clock.start();
    if (onCard()) { route.took(size, true, clock.nsecsElapsed() / 1e6); return; }
    route.refused(size);
  }
  const ProcessorTiming timing(&route, size);
  onProcessor();
}
