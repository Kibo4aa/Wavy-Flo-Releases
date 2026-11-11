#pragma once
#include "engine.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <QDataStream>
#include <QHash>
#include <QThread>
#include <QTransform>
#include <array>
#include <optional>
#include <vector>
struct CharacterPose {
  QPointF offset;
  double scale = 1, rotation = 0, opacity = 1;
  // Every animator's move, the first outermost: the ones below act in the
  // frame the ones above have moved, turned and scaled the letter into.
  QTransform matrix;
  // What it turns and scales about, from the letter's middle: the middle of
  // its word or line when an animation moves whole words or lines.
  QPointF pivot;
  double skew = 0;   // degrees, leaning the letter sideways
  double blur = 0;   // pixels, at the letter's own size
  QColor tint;       // a colour laid over the letter's fill...
  double tintAmount = 0; // ...this much of it
};
// Where one letter stands in its text, for the things that depend on it: the
// part of the text it covers (for a Range), the middle it turns about, and how
// far it sits from the middle of its line (for Tracking).
struct GlyphPlace {
  double from = 0, to = 1;  // the part of the text its character, word or line covers, 0..1
  QPointF pivot;            // from the letter's middle to its word's or line's
  double trackIndex = 0;    // letters from the middle of its line
};
// How far a letter has come: In or Out as its graph says, at the letter's own
// time. Nothing eases it again -- a Smooth Strength option once did, so a
// strong ease-out drawn in the graph lost its snap and long settle, and the
// letters moved unlike a layer keyed with the same graph.
inline double animatorProgress(const Property &curve, const QJsonObject &, double time, int rank, double delay) {
  return curve.at(time - rank * delay) / 100.;
}
// What Smooth Strength did, drawn into the keys instead: every stretch of In
// or Out that runs straight between two keys gets handles at a third and two
// thirds -- Smooth's own curve, exactly so from 0 to 100 -- and shows as eased
// in the graph. Stretches with an ease or handles of their own stay as drawn.
inline void easeStraightStretches(Property &curve) {
  std::sort(curve.keys.begin(), curve.keys.end(), [](const Key &a, const Key &b) { return a.frame < b.frame; });
  for (size_t i = 0; i + 1 < curve.keys.size(); ++i) {
    Key &k = curve.keys[i];
    if (k.ease != "linear" || k.custom) continue;
    k.custom = true;
    k.absolute = false;
    k.ease = "custom";
    k.curve = {1. / 3, 0, 2. / 3, 1};
  }
}
inline double characterProgress(const Layer &l, int property, double time,
                                int rank, double delay) {
  return animatorProgress(l.p[property], l.characterAnimation, time, rank, delay);
}
// ---------------------------------------------------------------------------
// Text Animations stacked. The one being edited keeps the places a single one
// always had: characterAnimation's own settings, and properties 6 and 7 (In
// and Out). Every other one waits in characterAnimation["stack"], its In and
// Out the extra curves "TextAnim/<uid>/In" and ".../Out". "stackOrder" lists
// them all by uid, top first; "uid" names the one in the places ("1" when
// never set). "enabled" switches the letters' animation as a whole.
// ---------------------------------------------------------------------------
struct TextAnimator {
  QJsonObject settings;
  const Property *in = nullptr, *out = nullptr;
  QString uid;
  // A Range's numbers when they are keyed: curves "TextAnim/<uid>/Range ...",
  // which stay with their animator whichever one is being edited.
  const Property *rangeStart = nullptr, *rangeEnd = nullptr, *rangeOffset = nullptr;
};
// The keyed Range numbers of an animator, by the name its curve has.
inline const QStringList &textRangeCurves() {
  static const QStringList names{"Range Start", "Range End", "Range Offset"};
  return names;
}
inline QString textAnimatorUid(const QJsonObject &a) { return a.value("uid").toString("1"); }
inline QJsonObject textAnimatorOwnSettings(QJsonObject a) {
  for (const char *meta : {"enabled", "stack", "stackOrder", "uid"}) a.remove(meta);
  return a;
}
inline QStringList textAnimatorOrder(const QJsonObject &a) {
  QStringList order;
  for (const auto &v : a.value("stackOrder").toArray())
    if (!v.toString().isEmpty() && !order.contains(v.toString())) order << v.toString();
  if (!order.contains(textAnimatorUid(a))) order.prepend(textAnimatorUid(a));
  return order;
}
// Every animator that is on, top first. With `all`, the ones switched off too.
inline std::vector<TextAnimator> textAnimators(const Layer &l, bool all = false) {
  std::vector<TextAnimator> list;
  const QJsonObject &a = l.characterAnimation;
  const QString here = textAnimatorUid(a);
  QHash<QString, QJsonObject> waiting;
  for (const auto &v : a.value("stack").toArray()) waiting.insert(v.toObject().value("uid").toString(), v.toObject());
  for (const QString &uid : textAnimatorOrder(a)) {
    TextAnimator animator;
    animator.uid = uid;
    if (uid == here) {
      animator.settings = a;
      animator.in = &l.p[6];
      animator.out = &l.p[7];
    } else {
      if (!waiting.contains(uid)) continue;
      const int in = layerFindExtra(l, "TextAnim/" + uid + "/In"), out = layerFindExtra(l, "TextAnim/" + uid + "/Out");
      if (in < 0 || out < 0) continue;
      animator.settings = waiting.value(uid);
      animator.in = &l.at(in);
      animator.out = &l.at(out);
    }
    if (!all && animator.settings.value("off").toBool()) continue;
    auto curve = [&](const QString &name) -> const Property * {
      const int at = layerFindExtra(l, "TextAnim/" + uid + "/" + name);
      return at >= 0 ? &l.at(at) : nullptr;
    };
    animator.rangeStart = curve("Range Start");
    animator.rangeEnd = curve("Range End");
    animator.rangeOffset = curve("Range Offset");
    list.push_back(animator);
  }
  return list;
}
// One animator's In and Out, wherever it is in the stack, eased where they run
// straight (easeStraightStretches).
inline void easeTextAnimatorStraight(Layer &l, const QString &uid) {
  if (uid == textAnimatorUid(l.characterAnimation)) {
    easeStraightStretches(l.p[6]);
    easeStraightStretches(l.p[7]);
    return;
  }
  for (const char *name : {"In", "Out"}) {
    const int at = layerFindExtra(l, "TextAnim/" + uid + "/" + name);
    if (at >= 0) easeStraightStretches(l.at(at));
  }
}
// A text from before Smooth Strength went -- a project, a pasted layer, a
// preset: each animator that had it on gets the same ease drawn into its keys,
// and the setting is dropped. It moves as it did, and its graph shows how.
inline void settleSmoothStrength(Layer &l) {
  QJsonObject a = l.characterAnimation;
  if (a.isEmpty()) return;
  bool changed = false;
  if (a.contains("smooth")) {
    if (a.value("smooth").toBool()) {
      easeStraightStretches(l.p[6]);
      easeStraightStretches(l.p[7]);
    }
    a.remove("smooth");
    changed = true;
  }
  QJsonArray stack = a.value("stack").toArray();
  for (int i = 0; i < stack.size(); ++i) {
    QJsonObject waiting = stack[i].toObject();
    if (!waiting.contains("smooth")) continue;
    if (waiting.value("smooth").toBool()) {
      const QString uid = waiting.value("uid").toString();
      for (const char *name : {"In", "Out"}) {
        const int at = layerFindExtra(l, "TextAnim/" + uid + "/" + name);
        if (at >= 0) easeStraightStretches(l.at(at));
      }
    }
    waiting.remove("smooth");
    stack[i] = waiting;
    a["stack"] = stack;
    changed = true;
  }
  if (changed) l.characterAnimation = a;
}
// The first numbers a QRandomGenerator seeded with `seed` gives, remembered.
// Seeding one runs the Mersenne Twister's whole set-up through a seed sequence,
// several microseconds, and the letters' poses asked for a dozen of them per
// letter per frame: a word of eight wiggling letters took 1.1 ms to pose, for
// every motion-blur sample. The numbers are the same ones, so is every pose.
inline std::array<double, 4> seededDoubles(quint32 seed) {
  static thread_local QHash<quint32, std::array<double, 4>> memo;
  auto found = memo.constFind(seed);
  if (found != memo.cend()) return *found;
  if (memo.size() >= 65536) memo.clear();
  QRandomGenerator random(seed);
  std::array<double, 4> values{};
  for (double &value : values) value = random.generateDouble();
  memo.insert(seed, values);
  return values;
}
inline quint32 seededWord(quint32 seed) {
  static thread_local QHash<quint32, quint32> memo;
  auto found = memo.constFind(seed);
  if (found != memo.cend()) return *found;
  if (memo.size() >= 65536) memo.clear();
  const quint32 word = QRandomGenerator(seed).generate();
  memo.insert(seed, word);
  return word;
}
// A value that drifts between random points, one point per wiggle, smoothed so
// the wandering has no corners in it. Posterize samples the clock in steps
// instead, which holds each value still until the next step: the same number of
// wiggles, arrived at in jumps rather than by drifting.
inline double wiggleAt(quint32 stream, double seconds, double speed,
                       double posterize) {
  if (speed <= 0) return 0;
  if (posterize > 0) seconds = std::floor(seconds * posterize) / posterize;
  const double position = seconds * speed;
  const double step = std::floor(position);
  auto pick = [stream](double at) {
    return seededDoubles(stream ^ quint32(qint64(at) * 2654435761LL))[0] * 2 - 1;
  };
  const double from = pick(step), to = pick(step + 1);
  double blend = position - step;
  blend = blend * blend * (3 - 2 * blend);
  return from + (to - from) * blend;
}
// How much of an animation reaches the part of the text from..to (0..1): all
// of it inside its Range, none outside, and in between by the Range's shape.
// The whole text (Start 0, End 100, no Offset, Square) is reached all the way.
inline double rangeWeight(const TextAnimator &animator, double time, double from, double to) {
  const QJsonObject &a = animator.settings;
  auto at = [&](const Property *curve, const char *key, double fallback) {
    return curve ? curve->at(time) : a.value(key).toDouble(fallback);
  };
  double start = at(animator.rangeStart, "rangeStart", 0) / 100., end = at(animator.rangeEnd, "rangeEnd", 100) / 100.;
  const double offset = at(animator.rangeOffset, "rangeOffset", 0) / 100.;
  if (start > end) std::swap(start, end);
  start += offset;
  end += offset;
  const int shape = a.value("rangeShape").toInt();
  if (shape == 0) {
    // Square: how much of the letter's own part lies inside.
    const double width = std::max(1e-9, to - from);
    return std::clamp((std::min(to, end) - std::max(from, start)) / width, 0., 1.);
  }
  const double middle = (from + to) / 2;
  if (end - start < 1e-9 || middle < start || middle > end) return 0;
  const double x = (middle - start) / (end - start); // 0 at the Range's start, 1 at its end
  switch (shape) {
  case 1: return x;                                          // ramp up
  case 2: return 1 - x;                                      // ramp down
  case 3: return 1 - std::abs(2 * x - 1);                    // triangle
  case 4: return std::sqrt(std::max(0., 1 - std::pow(2 * x - 1, 2))); // round
  default: return .5 - .5 * std::cos(2 * M_PI * x);         // smooth
  }
}
inline CharacterPose animatorPose(const TextAnimator &animator, double time, int rank, int count, int fps = 30,
                                  const GlyphPlace *place = nullptr) {
  const QJsonObject &a = animator.settings;
  auto value = [&](const char *key, double fallback) {
    return a[key].toDouble(fallback);
  };
  // Without a place (asked about a rank alone), the rank's share of the text.
  const double from = place ? place->from : double(rank) / std::max(1, count),
               to = place ? place->to : double(rank + 1) / std::max(1, count);
  const double reach = rangeWeight(animator, time, from, to);
  double t = animatorProgress(*animator.in, a, time, rank, value("delay", 2)),
         out = animatorProgress(*animator.out, a, time, rank, value("outDelay", 2));
  double dx = value("x", 0), dy = value("y", 80),
         scale = value("scale", 100) / 100., rotation = value("rotation", 0),
         opacity = value("opacity", 0) / 100.;
  int mode = a["mode"].toInt();
  // Random and ZigZag give each letter one move of its own -- Position or
  // Scale, never both: asked for both, every letter flew and grew at once.
  // Rotation is dealt apart and may land on any of them.
  // What is typed for Position and Rotation is how much a letter moves; which
  // way is the mode's: Y 100 sends some letters to 100 and others to -100. A
  // Scale keeps its way in Random (230% only grows); ZigZag has always shrunk
  // every other letter, now to 1/2.3 -- mirrored about 100% it went below
  // nothing and the letter came in turned inside out.
  const bool moves = std::abs(dx) > 1e-9 || std::abs(dy) > 1e-9, grows = std::abs(scale - 1) > 1e-9;
  auto otherWay = [](double s, double way) { return way > 0 ? s : s >= 1 ? 1 / s : 2 - s; };
  if (mode == 2) {
    // ZigZag, by turns. Asked for both, the letters take Position and Scale in
    // turn, and each of those changes its way in turn: up, grown, down, shrunk.
    // Asked for one, every letter changes its way, as it always has.
    const double sign = rank % 2 ? -1 : 1;
    if (moves && grows) {
      const double way = (rank / 2) % 2 ? -1 : 1;
      if (rank % 2 == 0) {
        dx *= way;
        dy *= way;
        scale = 1;
      } else {
        scale = otherWay(scale, way);
        dx = dy = 0;
      }
    } else {
      dx *= sign;
      dy *= sign;
      scale = otherWay(scale, sign);
    }
    rotation *= sign;
  } else if (mode == 1) {
    // Random: each letter is dealt one move -- up, down, to the side, or its
    // Scale -- from shuffled hands, so every few letters take
    // each move once (A up, B down, C scaled, D to the side) in an order of
    // their own. Up and down go as far as Position Y, the side as far as
    // Position X; the one not given takes the other's distance. Every letter
    // goes at least half as far as asked: with shares drawn from 0..1, Position
    // pulled far out moved a few letters and left most of the word in place.
    const int seed = a["seed"].toInt(1);
    const auto random = seededDoubles(quint32(seed + rank * 7193));
    auto signedShare = [](double r) {
      const double v = r * 2 - 1;
      return (v < 0 ? -1. : 1.) * (.5 + .5 * std::abs(v));
    };
    enum Move { Up, Down, Side, Grow };
    Move hand[4];
    int kinds = 0;
    if (moves) {
      hand[kinds++] = Up;
      hand[kinds++] = Down;
      hand[kinds++] = Side;
    }
    if (grows) hand[kinds++] = Grow;
    const double share = .5 + .5 * random[1];
    const double vertical = std::abs(dy) > 1e-9 ? std::abs(dy) : std::abs(dx);
    const double sideways = std::abs(dx) > 1e-9 ? std::abs(dx) : std::abs(dy);
    const double grown = scale;
    dx = dy = 0;
    scale = 1;
    if (kinds) {
      const auto shuffle = seededDoubles(quint32(seed) * 2654435761u + quint32(rank / kinds) * 40503u + 977u);
      for (int i = kinds - 1; i > 0; --i) std::swap(hand[i], hand[std::min(i, int(shuffle[size_t(i - 1)] * (i + 1)))]);
      switch (hand[rank % kinds]) {
      case Up: dy = -vertical * share; break;
      case Down: dy = vertical * share; break;
      case Side: dx = (random[0] < .5 ? -1 : 1) * sideways * share; break;
      case Grow: scale = 1 + (grown - 1) * share; break;
      }
    }
    // Rotation is a gacha: about half the letters turn, whatever their move,
    // each its own way, and one of them always does.
    const int always = int(seededWord(quint32(seed) ^ 0x5bd1e995u) % quint32(std::max(1, count)));
    rotation = random[2] < .5 || rank == always ? rotation * signedShare(random[3]) : 0;
  }
  bool separate = a["separateOut"].toBool();
  double ox = separate ? value("outX", 0) : -dx,
         oy = separate ? value("outY", -80) : -dy,
         os = separate ? value("outScale", 100) / 100. : scale,
         orr = separate ? value("outRotation", 0) : -rotation,
         oo = separate ? value("outOpacity", 0) / 100. : opacity;
  CharacterPose result;
  // How far out of place, in and out, where the Range lets the animation reach.
  const double away = (1 - t) * reach, gone = out * reach;
  const double tracking = value("tracking", 0), outTracking = separate ? value("outTracking", 0) : tracking;
  const double track = place ? place->trackIndex : 0;
  result.offset = {dx * away + ox * gone + track * (tracking * away + outTracking * gone), dy * away + oy * gone};
  result.scale = (1 + (scale - 1) * away) * (1 + (os - 1) * gone);
  result.rotation = rotation * away + orr * gone;
  result.skew = value("skew", 0) * away + (separate ? value("outSkew", 0) : -value("skew", 0)) * gone;
  result.blur = std::max(0., value("blur", 0) * away + (separate ? value("outBlur", 0) : value("blur", 0)) * gone);
  if (place && (a.value("basedOn").toInt() > 0)) result.pivot = place->pivot;
  if (value("colorAmount", 0) > 0) {
    result.tint = QColor(a.value("fillColor").toString("#FF4FA3"));
    result.tintAmount = std::clamp(value("colorAmount", 0) / 100. * std::min(1., (1 - t) + out) * reach, 0., 1.);
  }
  double inOpacity =
      a["pop"].toBool() ? (t <= .0001 && reach > .5 ? 0 : 1) : 1 + (opacity - 1) * away;
  double outOpacity =
      a["pop"].toBool() ? (out >= .9999 && reach > .5 ? 0 : 1) : 1 + (oo - 1) * gone;
  result.opacity = std::clamp(inOpacity * outOpacity, 0., 1.);
  // Wiggle rides on whatever the in and out transforms have settled on, so a
  // layer can wander while it arrives, after it has arrived, or with no in and
  // out animation at all -- strength rests at 100, which is the identity pose.
  // Every character and every channel draws from its own stream, or they would
  // all wander in step and read as one moving block.
  const double speed = value("wiggleSpeed", 0);
  if (speed > 0) {
    const double posterize = value("wigglePosterize", 0);
    const double seconds = time / std::max(1, fps);
    const quint32 stream =
        quint32(a["seed"].toInt(1)) * 2166136261u + quint32(rank) * 16777619u;
    auto noise = [&](int channel) {
      return wiggleAt(stream + quint32(channel) * 2749u, seconds, speed, posterize);
    };
    const double move = value("wigglePosition", 0) * reach;
    result.offset += QPointF(noise(0) * move, noise(1) * move);
    result.scale *= 1 + noise(2) * value("wiggleScale", 0) / 100. * reach;
    result.rotation += noise(3) * value("wiggleRotation", 0) * reach;
    result.opacity = std::clamp(
        result.opacity * (1 + noise(4) * value("wiggleOpacity", 0) / 100. * reach), 0., 1.);
  }
  return result;
}
// The animators' poses, one over the next: each moves, turns and scales the
// letter in the frame the ones above left it in, and the opacities multiply.
inline CharacterPose composePoses(const std::vector<CharacterPose> &steps) {
  CharacterPose result;
  QTransform m;
  double opacity = 1;
  for (const auto &step : steps) {
    // A blur counts at the size the animations above have scaled the letter to.
    const double above = std::sqrt(std::abs(m.determinant()));
    m.translate(step.offset.x(), step.offset.y());
    m.translate(step.pivot.x(), step.pivot.y());
    m.rotate(step.rotation);
    if (std::abs(step.skew) > 1e-6) m.shear(std::tan(std::clamp(step.skew, -85., 85.) * M_PI / 180.), 0);
    m.scale(step.scale, step.scale);
    m.translate(-step.pivot.x(), -step.pivot.y());
    opacity *= step.opacity;
    result.blur += step.blur * above;
    if (step.tintAmount > 0) {
      // Each colour over the ones above it.
      const QColor below = result.tintAmount > 0 ? result.tint : step.tint;
      const double k = step.tintAmount;
      result.tint = QColor::fromRgbF(below.redF() + (step.tint.redF() - below.redF()) * k,
                                     below.greenF() + (step.tint.greenF() - below.greenF()) * k,
                                     below.blueF() + (step.tint.blueF() - below.blueF()) * k);
      result.tintAmount = 1 - (1 - result.tintAmount) * (1 - k);
    }
  }
  result.matrix = m;
  result.offset = m.map(QPointF());
  result.scale = std::sqrt(std::abs(m.m11() * m.m22() - m.m12() * m.m21()));
  result.rotation = std::atan2(m.m12(), m.m11()) * 180. / M_PI;
  result.opacity = std::clamp(opacity, 0., 1.);
  return result;
}
// The stack, changed. Each keeps every animator's settings and curves whole.
inline void clearTextAnimatorCurves(Layer &l) {
  for (const QString &id : QStringList(l.extraCurveIds))
    if (id.startsWith("TextAnim/")) layerRemoveExtra(l, id);
}
// Brings animator `uid` into the places the panel edits; the one there waits.
inline void selectTextAnimator(Layer &l, const QString &uid) {
  const QJsonObject a = l.characterAnimation;
  const QString here = textAnimatorUid(a);
  if (uid == here) return;
  QJsonArray stack = a.value("stack").toArray();
  int found = -1;
  for (int i = 0; i < stack.size(); ++i)
    if (stack[i].toObject().value("uid").toString() == uid) found = i;
  if (found < 0) return;
  QJsonObject incoming = stack[found].toObject();
  stack.removeAt(found);
  QJsonObject parked = textAnimatorOwnSettings(a);
  parked["uid"] = here;
  stack.append(parked);
  const int inAt = layerFindExtra(l, "TextAnim/" + uid + "/In"), outAt = layerFindExtra(l, "TextAnim/" + uid + "/Out");
  Property in, out;
  in.base = 100;
  if (inAt >= 0) in = l.at(inAt);
  if (outAt >= 0) out = l.at(outAt);
  layerRemoveExtra(l, "TextAnim/" + uid + "/In");
  layerRemoveExtra(l, "TextAnim/" + uid + "/Out");
  l.at(layerAddExtra(l, "TextAnim/" + here + "/In", 0)) = l.p[6];
  l.at(layerAddExtra(l, "TextAnim/" + here + "/Out", 0)) = l.p[7];
  l.p[6] = in;
  l.p[7] = out;
  QJsonObject next = textAnimatorOwnSettings(incoming);
  next["uid"] = uid;
  next["stack"] = stack;
  next["stackOrder"] = QJsonArray::fromStringList(textAnimatorOrder(a));
  next["enabled"] = a.value("enabled").toBool(true);
  l.characterAnimation = next;
}
// A new animator at the bottom of the stack, brought in to be edited. It
// starts all the way in (In 100), so it changes nothing until it is keyed.
inline QString addTextAnimator(Layer &l) {
  QJsonObject a = l.characterAnimation;
  QStringList order = textAnimatorOrder(a);
  int next = 1;
  for (const QString &uid : order) next = std::max(next, uid.toInt() + 1);
  const QString uid = QString::number(next);
  QJsonArray stack = a.value("stack").toArray();
  stack.append(QJsonObject{{"uid", uid}});
  order << uid;
  a["stack"] = stack;
  a["stackOrder"] = QJsonArray::fromStringList(order);
  a["enabled"] = true;
  l.characterAnimation = a;
  layerAddExtra(l, "TextAnim/" + uid + "/In", 100);
  layerAddExtra(l, "TextAnim/" + uid + "/Out", 0);
  selectTextAnimator(l, uid);
  return uid;
}
// The stack in a new order (every uid of it, top first), as a drag leaves it.
inline bool setTextAnimatorOrder(Layer &l, const QStringList &order) {
  QJsonObject a = l.characterAnimation;
  QStringList now = textAnimatorOrder(a), sorted = order, wanted = now;
  std::sort(sorted.begin(), sorted.end());
  std::sort(wanted.begin(), wanted.end());
  if (order == now || sorted != wanted) return false; // the same, or not the same animators
  a["stackOrder"] = QJsonArray::fromStringList(order);
  l.characterAnimation = a;
  return true;
}
// One animator's own setting, wherever it is in the stack: in the editing
// places or waiting in the stack. An empty value removes the setting.
inline void setTextAnimatorSetting(Layer &l, const QString &uid, const QString &key, const QJsonValue &value) {
  // Smooth Strength is no setting any more: asked for (an old preset, a
  // script), it eases the keys there are now, and nothing is kept.
  if (key == "smooth") {
    if (value.toBool()) easeTextAnimatorStraight(l, uid);
    return;
  }
  QJsonObject a = l.characterAnimation;
  auto apply = [&](QJsonObject &o) {
    if (value.isNull() || value.isUndefined() || (value.isString() && value.toString().isEmpty())) o.remove(key);
    else o[key] = value;
  };
  if (uid == textAnimatorUid(a)) {
    apply(a);
  } else {
    QJsonArray stack = a.value("stack").toArray();
    for (int i = 0; i < stack.size(); ++i) {
      QJsonObject entry = stack[i].toObject();
      if (entry.value("uid").toString() != uid) continue;
      apply(entry);
      stack[i] = entry;
    }
    a["stack"] = stack;
  }
  l.characterAnimation = a;
}
// One animator switched off (or on again), wherever it is in the stack.
inline void setTextAnimatorOff(Layer &l, const QString &uid, bool off) {
  QJsonObject a = l.characterAnimation;
  if (uid == textAnimatorUid(a)) {
    if (off) a["off"] = true; else a.remove("off");
  } else {
    QJsonArray stack = a.value("stack").toArray();
    for (int i = 0; i < stack.size(); ++i) {
      QJsonObject entry = stack[i].toObject();
      if (entry.value("uid").toString() != uid) continue;
      if (off) entry["off"] = true; else entry.remove("off");
      stack[i] = entry;
    }
    a["stack"] = stack;
  }
  l.characterAnimation = a;
}
// A copy of the edited animator -- settings, In and Out -- right below it,
// brought in to be edited: a variation starts from what is there.
inline QString duplicateTextAnimator(Layer &l) {
  const QJsonObject before = l.characterAnimation;
  const QString source = textAnimatorUid(before);
  std::vector<std::pair<QString, Property>> ranges;
  for (const QString &name : textRangeCurves())
    if (const int at = layerFindExtra(l, "TextAnim/" + source + "/" + name); at >= 0) ranges.push_back({name, l.at(at)});
  const QJsonObject settings = textAnimatorOwnSettings(before);
  const Property in = l.p[6], out = l.p[7];
  const QString uid = addTextAnimator(l); // now the edited one, at the bottom
  QJsonObject a = l.characterAnimation;
  for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) a[it.key()] = it.value();
  if (!settings.value("name").toString().isEmpty()) a["name"] = settings.value("name").toString() + " copy";
  // Right below the one it came from rather than at the bottom.
  QStringList order = textAnimatorOrder(a);
  order.removeAll(uid);
  order.insert(order.indexOf(source) + 1, uid);
  a["stackOrder"] = QJsonArray::fromStringList(order);
  l.characterAnimation = a;
  l.p[6] = in;
  l.p[7] = out;
  for (const auto &[name, curve] : ranges) l.at(layerAddExtra(l, "TextAnim/" + uid + "/" + name, 0)) = curve;
  return uid;
}
// What an animation does, in a few words, from its settings: for its heading.
inline QString textAnimatorSummary(const QJsonObject &a) {
  QStringList parts;
  auto n = [&](const char *key, double fallback) { return a.value(key).toDouble(fallback); };
  const double x = n("x", 0), y = n("y", 80);
  if (x != 0 || y != 0) parts << QString("Move %1,%2").arg(x).arg(y);
  if (n("scale", 100) != 100) parts << QString("Scale %1%").arg(n("scale", 100));
  if (n("rotation", 0) != 0) parts << QString::fromUtf8("Turn %1\u00B0").arg(n("rotation", 0));
  if (n("opacity", 0) < 100) parts << (a.value("pop").toBool() ? "Pop" : "Fade");
  if (a.value("separateOut").toBool()) parts << "Own Out";
  if (n("blur", 0) > 0) parts << "Blur";
  if (n("tracking", 0) != 0) parts << "Tracking";
  if (n("skew", 0) != 0) parts << "Skew";
  if (n("colorAmount", 0) > 0) parts << "Color";
  if (n("wiggleSpeed", 0) > 0) parts << "Wiggle";
  if (a.value("write").toBool()) parts << "Write";
  if (a.value("basedOn").toInt() == 1) parts.prepend("Words");
  else if (a.value("basedOn").toInt() == 2) parts.prepend("Lines");
  if (n("rangeStart", 0) != 0 || n("rangeEnd", 100) != 100 || n("rangeOffset", 0) != 0) parts << "Range";
  return parts.isEmpty() ? QString("No change") : parts.join(QString::fromUtf8(" \u00B7 "));
}
// The edited animator up (by -1) or down (by 1) the stack.
inline bool moveTextAnimator(Layer &l, int by) {
  QJsonObject a = l.characterAnimation;
  QStringList order = textAnimatorOrder(a);
  const int at = order.indexOf(textAnimatorUid(a)), to = at + by;
  if (at < 0 || to < 0 || to >= order.size()) return false;
  order.swapItemsAt(at, to);
  a["stackOrder"] = QJsonArray::fromStringList(order);
  l.characterAnimation = a;
  return true;
}
// The edited animator gone; the next one below it (or above, at the bottom)
// comes in. The last one goes back to a single fresh animator.
inline void removeTextAnimator(Layer &l) {
  const QJsonObject a = l.characterAnimation;
  QStringList order = textAnimatorOrder(a);
  const QString here = textAnimatorUid(a);
  if (order.size() <= 1) {
    l.characterAnimation = QJsonObject{{"enabled", a.value("enabled").toBool()}};
    l.p[6] = Property{};
    l.p[6].base = 100;
    l.p[7] = Property{};
    clearTextAnimatorCurves(l);
    return;
  }
  const int at = order.indexOf(here);
  selectTextAnimator(l, order.value(at + 1 < order.size() ? at + 1 : at - 1));
  QJsonObject now = l.characterAnimation;
  QJsonArray stack;
  for (const auto &v : now.value("stack").toArray())
    if (v.toObject().value("uid").toString() != here) stack.append(v);
  order.removeAll(here);
  now["stack"] = stack;
  now["stackOrder"] = QJsonArray::fromStringList(order);
  l.characterAnimation = now;
  layerRemoveExtra(l, "TextAnim/" + here + "/In");
  layerRemoveExtra(l, "TextAnim/" + here + "/Out");
  for (const QString &name : textRangeCurves()) layerRemoveExtra(l, "TextAnim/" + here + "/" + name);
}
// Every animator of the layer at one rank (the same for all of them).
inline CharacterPose characterPose(const Layer &l, double time, int rank, int count, int fps = 30) {
  std::vector<CharacterPose> steps;
  for (const auto &animator : textAnimators(l)) steps.push_back(animatorPose(animator, time, rank, count, fps));
  return composePoses(steps);
}
struct AnimatedGlyph {
  QPainterPath path;
  QColor fill;
  QPen outline;
  QPointF center;
  int word = 0, line = 0; // which word and which line of the text it is in, counted from 0
};
inline QPainterPath cachedCharacterPath(const QRawFont &font, quint32 glyph, bool fauxBold = false) {
  // Geometry does not change with animation time, transforms or effect sliders.
  // Keep a bounded cache per worker; never share a mutable font cache across threads.
  static thread_local QRawFont cachedFont;
  static thread_local QHash<quint64,QPainterPath> paths;
  if(cachedFont!=font){cachedFont=font;paths.clear();}
  const quint64 key = quint64(glyph) | (fauxBold ? quint64(1) << 32 : 0);
  auto found=paths.constFind(key);
  if(found!=paths.cend())return *found;
  auto path=font.pathForGlyph(glyph);
  // Faux bold: the outline grown all round, the same for every face, the
  // way a bold is made where the family has none.
  if (fauxBold && !path.isEmpty()) {
    QPainterPathStroker thicken;
    thicken.setWidth(std::max(.5, font.pixelSize() * .045));
    thicken.setJoinStyle(Qt::RoundJoin);
    thicken.setCapStyle(Qt::RoundCap);
    path = path.united(thicken.createStroke(path)).simplified();
  }
  if(paths.size()>=512)paths.clear();
  paths.insert(key,path);
  return path;
}
inline QVector<AnimatedGlyph> buildCharacterGlyphs(QTextDocument &doc,
                                                   const Layer &l) {
  QVector<AnimatedGlyph> result;
  int word = -1, lineBase = 0;
  for (auto block = doc.begin(); block.isValid(); block = block.next()) {
    auto *layout = block.layout();
    if (!layout)
      continue;
    QString text = block.text();
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
    int start = 0, end;
    bool inWord = false;
    while ((end = boundaries.toNextBoundary()) != -1) {
      if (text.mid(start, end - start).trimmed().isEmpty()) {
        inWord = false;
        start = end;
        continue;
      }
      if (!inWord) ++word;
      inWord = true;
      AnimatedGlyph glyph;
      glyph.word = word;
      const QTextLine textLine = layout->lineForTextPosition(start);
      glyph.line = lineBase + (textLine.isValid() ? textLine.lineNumber() : 0);
      QTextCursor cursor(&doc);
      cursor.setPosition(block.position() + start);
      cursor.setPosition(block.position() + end, QTextCursor::KeepAnchor);
      auto fmt = cursor.charFormat();
      glyph.fill = fmt.foreground().style() == Qt::NoBrush
                       ? l.color
                       : fmt.foreground().color();
      glyph.outline = fmt.textOutline();
      for (auto run : layout->glyphRuns(start, end - start)) {
        auto indices = run.glyphIndexes();
        auto positions = run.positions();
        auto font = run.rawFont();
        for (int i = 0; i < indices.size(); i++) {
          QTransform tr;
          tr.translate(layout->position().x() + positions[i].x(),
                       layout->position().y() + positions[i].y());
          // Faux italic: each letter slanted about its own baseline.
          if (l.fauxItalic) tr.shear(-.21, 0);
          glyph.path.addPath(tr.map(cachedCharacterPath(font, indices[i], l.fauxBold)));
        }
      }
      if (!glyph.path.isEmpty()) {
        glyph.center = glyph.path.boundingRect().center();
        // Kept on the document and handed on: a blurred letter's job fills
        // its copy on another thread (readyPathForThreads).
        readyPathForThreads(glyph.path);
        result.push_back(glyph);
      }
      start = end;
    }
    lineBase += std::max(1, layout->lineCount());
    // A new paragraph starts a new word even without a space before it.
    inWord = false;
  }
  return result;
}
// The letters of a document, kept on the document itself. Building them asks
// the text engine for every letter's format and glyph runs one by one -- about
// a hundred and seventy microseconds for a line of animated text -- and they
// were built afresh every time anything asked: for where the letters are, for
// drawing them, on every motion-blur sample of every frame. Nothing about them
// moves with time, so they are built once per document and what decides them
// (its text, its width, the layer's colour and faux styles). Kept on the
// document, they go when it does, and a document is only ever used by the
// thread that made it (cachedTextDocument), so neither needs a lock.
inline QVector<AnimatedGlyph> characterGlyphs(QTextDocument &doc,
                                              const Layer &l) {
  struct Kept {
    int revision, characters; double width; QSizeF page; size_t text;
    QRgba64 colour; bool italic, bold;
    QVector<AnimatedGlyph> glyphs;
    bool same(const Kept &o) const {
      return revision == o.revision && characters == o.characters && width == o.width && page == o.page &&
             text == o.text && colour == o.colour && italic == o.italic && bold == o.bold;
    }
  };
  struct Holder : QObject { std::vector<Kept> kept; };
  // A document another thread owns cannot take anything kept on it.
  if (doc.thread() != QThread::currentThread()) return buildCharacterGlyphs(doc, l);
  static const QString name = QStringLiteral("wavy-flo letters");
  Kept wanted{doc.revision(), doc.characterCount(), doc.textWidth(), doc.pageSize(),
              size_t(qHash(doc.toRawText())), l.color.rgba64(), l.fauxItalic, l.fauxBold, {}};
  auto *holder = static_cast<Holder *>(doc.findChild<QObject *>(name, Qt::FindDirectChildrenOnly));
  if (holder)
    for (const Kept &k : holder->kept)
      if (k.same(wanted)) return k.glyphs;
  wanted.glyphs = buildCharacterGlyphs(doc, l);
  if (!holder) {
    holder = new Holder;
    holder->setObjectName(name);
    holder->setParent(&doc);
  }
  if (holder->kept.size() >= 4) holder->kept.erase(holder->kept.begin());
  holder->kept.push_back(wanted);
  return wanted.glyphs;
}
// ---------------------------------------------------------------------------
// Handwriting: the letters appear as though a pen were drawing them.
//
// The pen follows the line down the middle of each stroke of a letter -- the
// spine of the ink, not its outline -- so an "o" is drawn round once and the
// stem of an "a" comes after its bowl, the way a hand would. The nib is as wide
// as the letter is thick at the point it has reached, so what appears is the
// letter itself rather than a brush passing over it.
//
// The spine is found from the letter's own pixels: the shape is drawn small,
// thinned to a line one pixel wide, and that line is walked from the end a hand
// would start at. Nothing about the letters changes -- the same text, font,
// colour, stroke and per-character animation are drawn as always -- so this is
// a mask over the drawing rather than another way of drawing it.
// ---------------------------------------------------------------------------
// The first animator that writes, if any does.
inline std::optional<TextAnimator> writingAnimator(const Layer &l) {
  if (l.type != "text" || !l.characterAnimation.value("enabled").toBool() ||
      l.hiddenEffects.value("Text-Animation").toBool())
    return {};
  for (const auto &animator : textAnimators(l))
    if (animator.settings.value("write").toBool()) return animator;
  return {};
}
inline bool handwritingEnabled(const Layer &l) { return writingAnimator(l).has_value(); }
// How far the pen has got, from the Text Animation's own In strength. Read
// without the per-character delay: the delay staggers the characters' own
// entrance, while the pen crosses the whole line once.
inline double handwritingProgress(const Layer &l, double time) {
  const auto writer = writingAnimator(l);
  return writer ? std::clamp(animatorProgress(*writer->in, writer->settings, time, 0, 0), 0., 1.)
                : std::clamp(characterProgress(l, 6, time, 0, 0), 0., 1.);
}
// One run of the pen: where it goes, and how thick the letter is beneath it.
struct PenStroke {
  std::vector<QPointF> points;
  std::vector<float> half;
  double length = 0;
};
struct PenPath {
  std::vector<PenStroke> strokes;
  double length = 0;
};
// The spine of one letter, in the letter's own coordinates. An empty result
// means the shape was too small or too strange to thin, and the caller falls
// back to showing the letter whole.
inline PenPath letterSpine(const QPainterPath &glyph) {
  PenPath result;
  const QRectF bounds = glyph.boundingRect();
  if (bounds.isEmpty()) return result;
  // Thinning works on pixels, so the letter is drawn at a size where a stroke
  // is a good few pixels across and the whole thing still costs little.
  const double scale = std::clamp(110. / std::max(bounds.width(), bounds.height()), .02, 12.);
  constexpr int margin = 2;
  const int w = int(std::ceil(bounds.width() * scale)) + margin * 2;
  const int h = int(std::ceil(bounds.height() * scale)) + margin * 2;
  if (w < 5 || h < 5 || qint64(w) * h > 500000) return result;
  QImage mask(w, h, QImage::Format_Grayscale8);
  if (mask.isNull()) return result;
  mask.fill(0);
  {
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(margin, margin);
    p.scale(scale, scale);
    p.translate(-bounds.left(), -bounds.top());
    p.fillPath(glyph, Qt::white);
  }
  const int count = w * h;
  std::vector<quint8> ink(size_t(count), 0);
  for (int y = 0; y < h; ++y) {
    const uchar *row = mask.constScanLine(y);
    for (int x = 0; x < w; ++x) ink[size_t(y) * w + x] = row[x] >= 128 ? 1 : 0;
  }
  // How thick the letter is at each pixel: a chamfer distance to the nearest
  // pixel outside it. Near enough for the width of a nib.
  constexpr float near = 1.f, diagonal = 1.41421356f;
  std::vector<float> thickness(size_t(count), 0.f);
  for (int i = 0; i < count; ++i) thickness[size_t(i)] = ink[size_t(i)] ? 1e9f : 0.f;
  auto at = [&](int x, int y) -> float & { return thickness[size_t(y) * w + x]; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (!ink[size_t(y) * w + x]) continue;
      float best = at(x, y);
      if (y > 0) best = std::min(best, at(x, y - 1) + near);
      if (x > 0) best = std::min(best, at(x - 1, y) + near);
      if (y > 0 && x > 0) best = std::min(best, at(x - 1, y - 1) + diagonal);
      if (y > 0 && x + 1 < w) best = std::min(best, at(x + 1, y - 1) + diagonal);
      at(x, y) = best;
    }
  for (int y = h - 1; y >= 0; --y)
    for (int x = w - 1; x >= 0; --x) {
      if (!ink[size_t(y) * w + x]) continue;
      float best = at(x, y);
      if (y + 1 < h) best = std::min(best, at(x, y + 1) + near);
      if (x + 1 < w) best = std::min(best, at(x + 1, y) + near);
      if (y + 1 < h && x + 1 < w) best = std::min(best, at(x + 1, y + 1) + diagonal);
      if (y + 1 < h && x > 0) best = std::min(best, at(x - 1, y + 1) + diagonal);
      at(x, y) = best;
    }
  // Zhang-Suen thinning: peel the letter down to a line one pixel wide,
  // keeping every branch and every loop it had.
  std::vector<quint8> thin = ink;
  auto neighbours = [&](int x, int y, quint8 *p) {
    // p2..p9, clockwise from north.
    static const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    static const int dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    for (int k = 0; k < 8; ++k) {
      const int nx = x + dx[k], ny = y + dy[k];
      p[k] = (nx < 0 || ny < 0 || nx >= w || ny >= h) ? 0 : thin[size_t(ny) * w + nx];
    }
  };
  std::vector<int> doomed;
  for (int pass = 0; pass < 80; ++pass) {
    bool changed = false;
    for (int step = 0; step < 2; ++step) {
      doomed.clear();
      for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
          if (!thin[size_t(y) * w + x]) continue;
          quint8 p[8];
          neighbours(x, y, p);
          int filled = 0, crossings = 0;
          for (int k = 0; k < 8; ++k) {
            filled += p[k];
            if (!p[k] && p[(k + 1) % 8]) ++crossings;
          }
          if (filled < 2 || filled > 6 || crossings != 1) continue;
          const quint8 north = p[0], east = p[2], south = p[4], west = p[6];
          if (step == 0) {
            if (north * east * south || east * south * west) continue;
          } else {
            if (north * east * west || north * south * west) continue;
          }
          doomed.push_back(y * w + x);
        }
      for (int i : doomed) thin[size_t(i)] = 0;
      changed = changed || !doomed.empty();
    }
    if (!changed) break;
  }
  // Walk the line. Each piece of it is one component; the biggest is the body
  // of the letter and the small ones are its dots and accents, which a hand
  // adds afterwards.
  std::vector<int> spine;
  for (int i = 0; i < count; ++i) if (thin[size_t(i)]) spine.push_back(i);
  if (spine.empty()) return result;
  std::vector<int> component(size_t(count), -1);
  std::vector<std::vector<int>> parts;
  std::vector<int> queue;
  for (int seed : spine) {
    if (component[size_t(seed)] >= 0) continue;
    const int id = int(parts.size());
    parts.emplace_back();
    queue.clear();
    queue.push_back(seed);
    component[size_t(seed)] = id;
    while (!queue.empty()) {
      const int here = queue.back();
      queue.pop_back();
      parts[size_t(id)].push_back(here);
      const int x = here % w, y = here / w;
      for (int ny = std::max(0, y - 1); ny <= std::min(h - 1, y + 1); ++ny)
        for (int nx = std::max(0, x - 1); nx <= std::min(w - 1, x + 1); ++nx) {
          const int there = ny * w + nx;
          if (!thin[size_t(there)] || component[size_t(there)] >= 0) continue;
          component[size_t(there)] = id;
          queue.push_back(there);
        }
    }
  }
  std::sort(parts.begin(), parts.end(), [](const std::vector<int> &a, const std::vector<int> &b) {
    return a.size() > b.size();
  });
  std::vector<quint8> used(size_t(count), 0);
  auto degree = [&](int index) {
    const int x = index % w, y = index / w;
    int n = 0;
    for (int ny = std::max(0, y - 1); ny <= std::min(h - 1, y + 1); ++ny)
      for (int nx = std::max(0, x - 1); nx <= std::min(w - 1, x + 1); ++nx)
        if ((nx != x || ny != y) && thin[size_t(ny) * w + nx]) ++n;
    return n;
  };
  const double inverse = 1. / scale;
  auto place = [&](int index) {
    return QPointF((index % w + .5 - margin) * inverse + bounds.left(),
                   (index / w + .5 - margin) * inverse + bounds.top());
  };
  for (const auto &part : parts) {
    // Start where a hand would: a loose end rather than the middle of a line,
    // the higher of them, and of two ends at much the same height the left one.
    // The heights are compared in bands, or the two arms of a "V" -- level to
    // the eye, a pixel apart after thinning -- would be started from either
    // side depending on which came out a hair higher.
    const int band = std::max(2, h / 8);
    int start = -1;
    bool startIsEnd = false;
    for (int index : part) {
      const bool loose = degree(index) <= 1;
      if (start < 0) { start = index; startIsEnd = loose; continue; }
      if (loose != startIsEnd) { if (loose) { start = index; startIsEnd = true; } continue; }
      const int here = (index / w) / band, there = (start / w) / band;
      if (here < there || (here == there && index % w < start % w)) start = index;
    }
    if (start < 0) continue;
    int at = start;
    QPointF heading(0, 1);
    while (at >= 0) {
      PenStroke stroke;
      while (at >= 0) {
        used[size_t(at)] = 1;
        const QPointF here = place(at);
        if (!stroke.points.empty()) stroke.length += QLineF(stroke.points.back(), here).length();
        stroke.points.push_back(here);
        stroke.half.push_back(thickness[size_t(at)] * float(inverse));
        // Carry straight on where the line allows it, so a stroke that crosses
        // another does not turn off down it.
        const int x = at % w, y = at / w;
        int next = -1;
        double best = -2;
        for (int ny = std::max(0, y - 1); ny <= std::min(h - 1, y + 1); ++ny)
          for (int nx = std::max(0, x - 1); nx <= std::min(w - 1, x + 1); ++nx) {
            const int there = ny * w + nx;
            if (there == at || !thin[size_t(there)] || used[size_t(there)]) continue;
            QPointF step(nx - x, ny - y);
            const double length = std::hypot(step.x(), step.y());
            if (length < 1e-9) continue;
            step /= length;
            const double straightness = heading.x() * step.x() + heading.y() * step.y();
            if (straightness > best) { best = straightness; next = there; }
          }
        if (next < 0) { at = -1; break; }
        heading = QPointF((next % w) - x, (next / w) - y);
        const double length = std::hypot(heading.x(), heading.y());
        if (length > 1e-9) heading /= length;
        at = next;
      }
      if (stroke.points.size() > 1) {
        result.length += stroke.length;
        result.strokes.push_back(std::move(stroke));
      }
      // Anything of this piece the walk could not reach in one run: carry on
      // from whatever is left, nearest first.
      int nearest = -1;
      double closest = 0;
      const QPointF from = result.strokes.empty() || result.strokes.back().points.empty()
                               ? place(start) : result.strokes.back().points.back();
      for (int index : part) {
        if (used[size_t(index)]) continue;
        const double distance = QLineF(from, place(index)).length();
        if (nearest < 0 || distance < closest) { nearest = index; closest = distance; }
      }
      at = nearest;
      heading = QPointF(0, 1);
    }
  }
  return result;
}
// The spine of a letter is the same wherever the letter is put, so it is worked
// out once per shape and kept. Bounded, and one cache per worker: a QPainterPath
// is not safe to share between threads.
inline const PenPath &cachedLetterSpine(const QPainterPath &glyph) {
  static thread_local QHash<QByteArray, PenPath> spines;
  static const PenPath none;
  const QRectF bounds = glyph.boundingRect();
  if (bounds.isEmpty()) return none;
  const QPainterPath local = glyph.translated(-bounds.topLeft());
  QByteArray key;
  {
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << local.elementCount() << bounds.width() << bounds.height();
    for (int i = 0; i < local.elementCount(); ++i) {
      const auto element = local.elementAt(i);
      stream << qint32(element.type) << qRound(element.x * 64) << qRound(element.y * 64);
    }
  }
  auto found = spines.constFind(key);
  if (found != spines.cend()) return *found;
  PenPath spine = letterSpine(local);
  for (auto &stroke : spine.strokes)
    for (auto &point : stroke.points) point += bounds.topLeft();
  if (spines.size() >= 512) spines.clear();
  return *spines.insert(key, std::move(spine));
}
// What the pen has written by now: whole letters behind it, and in the letter
// it is on, the ink it has already run the nib over.
//
// What is shown is the stretch of writing between two points along it: the In
// strength drives the far end forward, which is the pen, and the Out strength
// drives the near end after it, which takes the ink away in the order it was
// laid down. With Out at nothing, that near end sits at the start and the whole
// of what has been written stands.
inline QPainterPath handwritingReveal(const QVector<AnimatedGlyph> &glyphs,
                                      double progress, double erased, double nibScale) {
  QPainterPath mask;
  mask.setFillRule(Qt::WindingFill);
  if (glyphs.isEmpty() || progress <= 0 || erased >= progress) return mask;
  if (progress >= 1 && erased <= 0) {
    for (const auto &glyph : glyphs) mask.addPath(glyph.path);
    return mask;
  }
  std::vector<const PenPath *> spines;
  spines.reserve(size_t(glyphs.size()));
  double total = 0;
  for (const auto &glyph : glyphs) {
    const PenPath &spine = cachedLetterSpine(glyph.path);
    spines.push_back(&spine);
    total += spine.length;
  }
  if (total <= 0) {
    // Nothing could be thinned (a letter too small to have a spine, or a
    // coloured glyph with no outline): show whole letters in turn instead of
    // nothing at all.
    const int first = int(std::ceil(erased * glyphs.size()));
    const int last = int(progress * glyphs.size());
    for (int i = first; i < last && i < glyphs.size(); ++i)
      if (i >= 0) mask.addPath(glyphs[i].path);
    return mask;
  }
  const double from = erased * total, to = progress * total;
  double start = 0;
  for (int index = 0; index < glyphs.size(); ++index) {
    const PenPath &spine = *spines[size_t(index)];
    const double end = start + spine.length;
    if (end <= from || start >= to) { start = end; continue; } // written and taken away, or not yet
    if (start >= from && end <= to) {
      // Standing whole: the letter is simply itself, which keeps its edges
      // crisp and its counters open.
      mask.addPath(glyphs[index].path);
      start = end;
      continue;
    }
    // Part of this letter. The nib is swept along the spine between the two
    // ends, and what shows is the letter itself where the sweep has been: the
    // sweep decides how far along the letter the ink reaches, the letter
    // decides its shape. Taking the sweep alone would leave the corners and
    // the flat ends of the strokes uncovered, which is what made a letter
    // look brushed on rather than written.
    QPainterPath sweep;
    sweep.setFillRule(Qt::WindingFill);
    double walked = 0;
    for (const auto &stroke : spine.strokes) {
      if (start + walked >= to) break;
      if (start + walked + stroke.length <= from) { walked += stroke.length; continue; }
      // The length of spine the nib has covered in this stroke, as a line. One
      // line drawn with a round pen rather than a disc at every sample: the
      // same shape, and a shape the letter can be cut against quickly.
      QPolygonF line;
      double widest = 0, along = walked;
      for (size_t i = 0; i < stroke.points.size(); ++i) {
        const double back = start + along;
        if (i > 0) along += QLineF(stroke.points[i - 1], stroke.points[i]).length();
        const double here = start + along;
        const double step = here - back;
        auto between = [&](double edge) {
          const double part = step > 1e-9 ? (edge - back) / step : 0.;
          return stroke.points[i - 1] + (stroke.points[i] - stroke.points[i - 1]) * part;
        };
        // The two ends fall between samples, so they are placed exactly: the
        // pen's tip is where it is, not at the last whole step behind it.
        if (i > 0 && from > back && from <= here) { line.clear(); line << between(from); }
        if (i > 0 && to > back && to < here) {
          line << between(to);
          widest = std::max(widest, double(stroke.half[i]));
          break;
        }
        if (here >= from && here <= to) {
          line << stroke.points[i];
          widest = std::max(widest, double(stroke.half[i]));
        }
      }
      // Wider than the letter is thick, so a corner or a serif is reached as
      // the spine passes it, and the letter cuts the surplus away again.
      const double radius = std::max(.5, widest * nibScale);
      if (line.size() == 1) sweep.addEllipse(line.front(), radius, radius);
      else if (line.size() > 1) {
        QPainterPath run;
        run.moveTo(line.front());
        for (int i = 1; i < line.size(); ++i) run.lineTo(line[i]);
        QPainterPathStroker nib;
        nib.setWidth(radius * 2);
        nib.setCapStyle(Qt::RoundCap);
        nib.setJoinStyle(Qt::RoundJoin);
        sweep.addPath(nib.createStroke(run));
      }
      walked += stroke.length;
    }
    if (!sweep.isEmpty()) mask.addPath(glyphs[index].path.intersected(sweep));
    start = end;
  }
  return mask;
}
// The mask for a layer at a time, or an empty path when nothing is written yet.
// The nib is given as a share of how thick the letter is where the pen is, so
// one setting suits a hairline and a heavy weight alike.
inline QPainterPath handwritingMask(QTextDocument &doc, const Layer &l, double time) {
  const double progress = handwritingProgress(l, time);
  const double erased = std::clamp(characterProgress(l, 7, time, 0, 0), 0., 1.);
  if (progress <= 0 || erased >= progress) return {};
  const double nib = std::clamp(l.characterAnimation.value("writeNib").toDouble(115), 20., 400.) / 100.;
  return handwritingReveal(characterGlyphs(doc, l), progress, erased, nib);
}
// Where every letter stands at one moment: each animator of the stack ranks
// the letters, words or lines its own way and poses them, and the poses are
// composed. What drawAnimatedCharacters draws, and what the box of an
// animated text is measured on.
inline QVector<CharacterPose> animatedCharacterPoses(const QVector<AnimatedGlyph> &glyphs, const Layer &l,
                                                    double frame, int fps = 30) {
  const int count = int(glyphs.size());
  // The units an animation moves: each letter, each word, or each line.
  // unitOf[basedOn][i] is the unit letter i belongs to, in reading order.
  std::array<QVector<int>, 3> unitOf;
  std::array<int, 3> unitCount{count, 0, 0};
  std::array<QVector<QPointF>, 3> unitCenter;
  for (int based = 0; based < 3; ++based) {
    unitOf[size_t(based)].resize(count);
    QHash<int, int> seen;
    QVector<QRectF> boxes;
    for (int i = 0; i < count; ++i) {
      const int id = based == 0 ? i : based == 1 ? glyphs[i].word : glyphs[i].line;
      auto found = seen.constFind(id);
      int unit;
      if (found == seen.cend()) {
        unit = int(boxes.size());
        seen.insert(id, unit);
        boxes.push_back(glyphs[i].path.boundingRect());
      } else {
        unit = *found;
        boxes[unit] = boxes[unit].united(glyphs[i].path.boundingRect());
      }
      unitOf[size_t(based)][i] = unit;
    }
    unitCount[size_t(based)] = int(boxes.size());
    for (const QRectF &box : boxes) unitCenter[size_t(based)].push_back(box.center());
  }
  // Each letter's place along its line, for Tracking: letters from the middle.
  QVector<double> trackIndex(count);
  {
    QHash<int, QVector<int>> lines;
    for (int i = 0; i < count; ++i) lines[glyphs[i].line].push_back(i);
    for (const auto &members : lines)
      for (int k = 0; k < members.size(); ++k) trackIndex[members[k]] = k - (members.size() - 1) / 2.;
  }
  // Each animator orders its units its own way (left to right, from the
  // middle, at random...), so each has ranks of its own; a letter takes its
  // unit's rank, and a whole word or line moves as one.
  auto ranksFor = [&](const QJsonObject &settings) {
    const int based = std::clamp(settings["basedOn"].toInt(), 0, 2);
    const int units = unitCount[size_t(based)];
    const auto &centers = unitCenter[size_t(based)];
    QVector<int> order(units);
    std::iota(order.begin(), order.end(), 0);
    const int direction = settings["order"].toInt();
    const double center =
        units ? QRectF(centers.front(), centers.back()).center().x() : 0;
    const int seed = settings["seed"].toInt(1);
    auto randomRank = [&](int i) {
      return seededWord(quint32(seed + i * 7193));
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
      auto A = centers[a], B = centers[b];
      switch (direction) {
      case 1:
        return a > b;
      case 2:
        return A.y() == B.y() ? A.x() < B.x() : A.y() < B.y();
      case 3:
        return A.y() == B.y() ? A.x() < B.x() : A.y() > B.y();
      case 4:
        return std::abs(A.x() - center) < std::abs(B.x() - center);
      case 5:
        return std::abs(A.x() - center) > std::abs(B.x() - center);
      case 6:
        return randomRank(a) < randomRank(b);
      default:
        return a < b;
      }
    });
    QVector<int> unitRank(units);
    for (int i = 0; i < units; i++)
      unitRank[order[i]] = i;
    QVector<int> ranks(count);
    for (int i = 0; i < count; ++i) ranks[i] = unitRank[unitOf[size_t(based)][i]];
    return ranks;
  };
  const auto animators = textAnimators(l);
  std::vector<QVector<int>> animatorRanks;
  for (const auto &animator : animators) animatorRanks.push_back(ranksFor(animator.settings));
  QVector<CharacterPose> poses(count);
  for (int i = 0; i < count; ++i) {
    std::vector<CharacterPose> steps;
    for (size_t k = 0; k < animators.size(); ++k) {
      const int based = std::clamp(animators[k].settings["basedOn"].toInt(), 0, 2);
      const int unit = unitOf[size_t(based)][i], units = std::max(1, unitCount[size_t(based)]);
      GlyphPlace place;
      place.from = double(unit) / units;
      place.to = double(unit + 1) / units;
      place.pivot = unitCenter[size_t(based)][unit] - glyphs[i].center;
      place.trackIndex = trackIndex[i];
      steps.push_back(animatorPose(animators[k], frame, animatorRanks[k][i], units, fps, &place));
    }
    poses[i] = composePoses(steps);
  }
  return poses;
}
// How much smaller than its place a letter blurred `radius` pixels is drawn,
// stroked and blurred, to be enlarged into it: six pixels to the blur's
// radius keep every detail a blur that wide leaves. Held at no less than a
// fifth, a letter blurred across a good part of the frame was drawn, stroked
// and blurred at many times the detail the blur then took away.
inline double blurredLetterShrink(double radius) { return std::clamp(6. / std::max(1., radius), .05, 1.); }
// With `deferred`, the Character strokes are left to the raster stage, which
// builds them from the letters' distance as it does for still text: only the
// letters are painted, each whole whatever its opacity, and what the raster
// stage needs of each is collected in `deferred`.
inline void drawAnimatedCharacters(QPainter &p, QTextDocument &doc,
                                   const Layer &l, double frame, int fps = 30,
                                   const LetterPaint *letters = nullptr,
                                   AnimatedLetterStrokes *deferred = nullptr) {
  auto glyphs = characterGlyphs(doc, l);
  // With the Stroke effect applied, every glyph carries the effect's outline
  // rather than the Character panel's, which is what makes an animated stroke
  // follow each character through its own move, scale and fade.
  const bool effectStroke = vectorEffectStroke(l);
  // The Stroke effect goes round the letters after their Character strokes.
  auto textStrokes = textStrokeStack(l);
  const bool useCharacterOutline = textStrokes.empty() && !effectStroke;
  // Stroke Only (the Character option, or the Stroke effect's): no letters, and
  // each outside stroke kept to the outside of its glyph.
  const bool hollow = !textStrokes.empty() && (textStrokeOnly(l) || (effectStroke && effectStrokeOnly(l)));
  const int count = int(glyphs.size());
  const QVector<CharacterPose> poses = animatedCharacterPoses(glyphs, l, frame, fps);
  auto paintGlyph = [&](QPainter &p, int i, const std::function<void(AnimatedGlyph &)> &paint) {
    auto &g = glyphs[i];
    const auto &pose = poses[i];
    if (pose.opacity <= 0) return;
    p.save();
    p.setOpacity(p.opacity() * pose.opacity);
    p.translate(g.center);
    p.setTransform(pose.matrix, true);
    p.translate(-g.center);
    paint(g);
    p.restore();
  };
  // A letter's fill with its animation's colour laid over it.
  auto fillOf = [&](int i) {
    const QColor base = glyphs[i].fill;
    const auto &pose = poses[i];
    if (pose.tintAmount <= 0) return base;
    const double k = pose.tintAmount;
    return QColor::fromRgbF(base.redF() + (pose.tint.redF() - base.redF()) * k,
                            base.greenF() + (pose.tint.greenF() - base.greenF()) * k,
                            base.blueF() + (pose.tint.blueF() - base.blueF()) * k, base.alphaF());
  };
  const bool textured = letters && letters->letterBrush.style() != Qt::NoBrush;
  // Of a blurred letter's picture of `size`, laid over `onPicture` and blurred
  // `blur` of its own pixels, the whole pixels that can matter where anything
  // shows (deferred->within): a letter scaled up past the frame was drawn and
  // blurred whole, most of it where it never shows. The margin kept is what
  // the blur reaches on the picture (three box passes of ceil(blur/2), and a
  // pixel each for the enlargement and the rounding) and `extra` more of its
  // pixels (strokes measured on the picture, as wide each way), turned into
  // the raster's pixels axis by axis -- a picture held at 2048 pixels covers
  // more of them each. The pixels kept keep their places.
  auto kept = [&](const QRectF &onPicture, QSize size, double blur, double extra) {
    const QRect whole(QPoint(), size);
    if (!deferred || deferred->within.isNull()) return whole;
    const double kx = size.width() / onPicture.width(), ky = size.height() / onPicture.height();
    const double reach = 3 * std::clamp(std::ceil(blur / 2), 1., 4096.) + 2;
    const double mx = (reach + extra) / kx, my = (reach + extra) / ky;
    QRectF shows = deferred->within.adjusted(-mx, -my, mx, my);
    // Across, never cut inside the raster: a picture enlarged into a rect that
    // starts at another column lands a step off in columns after it. Rows are
    // each worked out afresh.
    if (!deferred->raster.isNull()) {
      shows.setLeft(std::min(shows.left(), deferred->raster.left() - 1));
      shows.setRight(std::max(shows.right(), deferred->raster.right() + 1));
    }
    const QPointF from((shows.left() - onPicture.left()) * kx, (shows.top() - onPicture.top()) * ky);
    const QPointF to((shows.right() - onPicture.left()) * kx, (shows.bottom() - onPicture.top()) * ky);
    if (to.x() <= 0 || to.y() <= 0 || from.x() >= size.width() || from.y() >= size.height()) return QRect();
    QRect keep = whole & QRect(QPoint(int(std::floor(std::max(0., from.x()))), int(std::floor(std::max(0., from.y())))),
                               QPoint(int(std::ceil(std::min(double(size.width()), to.x()))) - 1,
                                      int(std::ceil(std::min(double(size.height()), to.y()))) - 1));
    // Never one pixel: Qt lays that as a flat colour.
    if (keep.width() < 2 || keep.height() < 2) keep = whole & keep.adjusted(-1, -1, 1, 1);
    return keep;
  };
  // Of what is kept, what the blur carries into what shows -- as far round it
  // as the blur reaches, columns included -- in the kept pixels. The rest is
  // there for the letter's pixels the strokes reach in from and for the
  // columns the picture lands on, and needs no strokes nor blur of its own
  // (textStrokeStackPass's within, fullResolutionBlurPart): at 900% a stroke
  // 66 px wide reached 600 px, and every blurred letter's strokes were worked
  // out across all of it, 270 ms of a frame. Never one row.
  auto worked = [&](const QRectF &onPicture, QSize size, const QRect &keep, double blur) {
    const QRect all(QPoint(), keep.size());
    if (!deferred || deferred->within.isNull()) return all;
    const double kx = size.width() / onPicture.width(), ky = size.height() / onPicture.height();
    const double reach = 3 * std::clamp(std::ceil(blur / 2), 1., 4096.) + 2;
    const QRectF &shows = deferred->within;
    QRect part = all & QRect(QPoint(int(std::floor((shows.left() - onPicture.left()) * kx - reach)) - keep.left(),
                                    int(std::floor((shows.top() - onPicture.top()) * ky - reach)) - keep.top()),
                             QPoint(int(std::ceil((shows.right() - onPicture.left()) * kx + reach)) - keep.left(),
                                    int(std::ceil((shows.bottom() - onPicture.top()) * ky + reach)) - keep.top()));
    if (!part.isEmpty() && part.height() < 2) part = all & part.adjusted(0, -1, 0, 1);
    return part;
  };
  // Where the kept part lands: all of onPicture when nothing was cut.
  auto landing = [](const QRectF &onPicture, QSize size, const QRect &keep) {
    if (keep == QRect(QPoint(), size)) return onPicture;
    const double sx = onPicture.width() / size.width(), sy = onPicture.height() / size.height();
    return QRectF(onPicture.left() + keep.left() * sx, onPicture.top() + keep.top() * sy, keep.width() * sx,
                  keep.height() * sy);
  };
  if (deferred && !textStrokes.empty()) {
    const QTransform world = p.worldTransform();
    const double worldScale = std::sqrt(std::abs(world.determinant()));
    // As far as the strokes reach outside a letter, Miter corners included.
    double outward = 0, across = 0;
    bool mitred = false;
    for (const auto &stroke : textStrokes) {
      const double width = std::max(0., stroke.width);
      across += width;
      outward += stroke.mode == 1 ? width : stroke.mode == 2 ? width / 2 : 0;
      mitred |= stroke.join == 0 && stroke.mode != 0;
    }
    // The backgrounds, apart (AnimatedLetterStrokes::backdrop), each where the
    // letter carrying it is and as much as that letter shows -- a blurred
    // letter's blurred with it -- in their own order.
    std::vector<std::pair<size_t, int>> blurredBackgrounds; // a piece, and the letter carrying it
    if (deferred->collect && deferred->backdrop && letters &&
        letters->backgroundCarrier.size() == letters->backgrounds.size()) {
      LetterBackdrop &backdrop = *deferred->backdrop;
      const auto paint = std::make_shared<const LetterPaint>(*letters);
      for (int b = 0; b < letters->backgrounds.size(); ++b) {
        const int i = letters->backgroundCarrier[b];
        if (i < 0 || i >= count || poses[i].opacity <= 0) continue;
        const auto &g = glyphs[i];
        QTransform local;
        local.translate(g.center.x(), g.center.y());
        local = poses[i].matrix * local;
        local.translate(-g.center.x(), -g.center.y());
        const double radius = poses[i].blur * worldScale;
        if (radius < .4) {
          backdrop.add(p, paint, b, local * world, p.opacity() * poses[i].opacity * letters->backgroundOpacity).shown =
              poses[i].opacity;
          continue;
        }
        const QRectF onPicture = (local * world).mapRect(letters->backgroundBox(b).adjusted(-2, -2, 2, 2))
                                     .adjusted(-radius * 3, -radius * 3, radius * 3, radius * 3);
        if (onPicture.isEmpty() || (!deferred->within.isNull() && !onPicture.intersects(deferred->within))) continue;
        const double shrink = blurredLetterShrink(radius);
        const QSize size(std::clamp(int(std::ceil(onPicture.width() * shrink)), 1, 2048),
                         std::clamp(int(std::ceil(onPicture.height() * shrink)), 1, 2048));
        const QRect keep = kept(onPicture, size, radius * shrink, 0);
        if (keep.isEmpty()) continue;
        const QRect part = worked(onPicture, size, keep, radius * shrink);
        if (part.isEmpty()) continue;
        QTransform place;
        place.scale(double(size.width()) / onPicture.width(), double(size.height()) / onPicture.height());
        place.translate(-onPicture.left(), -onPicture.top());
        LetterBackdrop::Piece &piece = backdrop.add(p, paint, b, local * world, p.opacity());
        piece.onto = landing(onPicture, size, QRect(keep.left(), keep.top() + part.top(), keep.width(), part.height()));
        piece.shown = poses[i].opacity;
        blurredBackgrounds.push_back({backdrop.pieces.size() - 1, i});
        // Its picture made with the blurred letters' (AnimatedLetterStrokes::jobs).
        deferred->jobs.push_back([&backdrop, slot = backdrop.pieces.size() - 1, size = keep.size(),
                                  onIt = local * world * place * QTransform::fromTranslate(-keep.left(), -keep.top()),
                                  hints = p.renderHints(), shown = poses[i].opacity * letters->backgroundOpacity,
                                  blur = radius * shrink, part] {
          LetterBackdrop::Piece &piece = backdrop.pieces[slot];
          QImage picture(size, QImage::Format_ARGB32_Premultiplied);
          picture.fill(Qt::transparent);
          {
            QPainter q(&picture);
            q.setRenderHints(hints);
            q.setWorldTransform(onIt);
            q.setOpacity(shown);
            piece.paint->drawBackground(q, piece.index);
          }
          piece.picture = fullResolutionBlurPart(picture, blur, part);
        });
      }
    }
    std::vector<int> overlayOf(size_t(count), -1);
    for (int i = 0; i < count; ++i) {
      const auto &g = glyphs[i];
      const auto &pose = poses[i];
      if (pose.opacity <= 0) continue;
      const QBrush fill = textured ? letters->letterBrush : QBrush(fillOf(i));
      QTransform local;
      local.translate(g.center.x(), g.center.y());
      local = pose.matrix * local;
      local.translate(-g.center.x(), -g.center.y());
      const double radius = pose.blur * worldScale;
      if (radius < .4) {
        // Whole, at full strength: its opacity is applied once its strokes
        // are on, to all of it at once.
        p.save();
        p.setTransform(local, true);
        p.fillPath(g.path, fill);
        if (deferred->collect) {
          deferred->outlines.push_back(p.combinedTransform().map(g.path));
          deferred->opacity.push_back(pose.opacity);
          deferred->scale.push_back(std::sqrt(std::abs(pose.matrix.determinant())));
        }
        p.restore();
        continue;
      }
      // A blurred letter is finished on its own -- letter, strokes, blur --
      // and laid over the rest once their strokes are built. The finishing is
      // left for when the strips are painted (AnimatedLetterStrokes::jobs).
      if (!deferred->collect) continue;
      const QRectF box = (mitred ? mitredStrokeBox(g.path, outward) : g.path.boundingRect().adjusted(-outward, -outward, outward, outward))
                             .adjusted(-2, -2, 2, 2);
      const QRectF onPicture = (local * world).mapRect(box).adjusted(-radius * 3, -radius * 3, radius * 3, radius * 3);
      // Nor one that lands wholly where nothing shows.
      if (onPicture.isEmpty() || (!deferred->within.isNull() && !onPicture.intersects(deferred->within))) continue;
      const double shrink = blurredLetterShrink(radius);
      const QSize size(std::clamp(int(std::ceil(onPicture.width() * shrink)), 1, 2048),
                       std::clamp(int(std::ceil(onPicture.height() * shrink)), 1, 2048));
      // Kept as far round what shows as its blur reaches, and its strokes,
      // out and in, as wide as the picture strokes them: a letter cut short
      // has an edge there, which strokes too.
      const double onPictureScale = std::sqrt(double(size.width()) / onPicture.width() * size.height() / onPicture.height());
      const QRect keep = kept(onPicture, size, radius * shrink,
                              across * std::sqrt(std::abs((local * world).determinant())) * onPictureScale + 2);
      if (keep.isEmpty()) continue;
      const QRect part = worked(onPicture, size, keep, radius * shrink);
      if (part.isEmpty()) continue;
      QTransform place;
      place.scale(double(size.width()) / onPicture.width(), double(size.height()) / onPicture.height());
      place.translate(-onPicture.left(), -onPicture.top());
      // Its picture the part's rows (fullResolutionBlurPart).
      deferred->overlays.push_back(
          {QImage(), landing(onPicture, size, QRect(keep.left(), keep.top() + part.top(), keep.width(), part.height())),
           pose.opacity});
      // Inside whatever the letters are clipped to (a hand writing them):
      // laid apart, they showed where the pen had not been yet.
      if (p.hasClipping()) {
        deferred->overlays.back().clipped = true;
        deferred->overlays.back().clip = p.worldTransform().map(p.clipPath());
      }
      overlayOf[size_t(i)] = int(deferred->overlays.size()) - 1;
      deferred->jobs.push_back([deferred, slot = deferred->overlays.size() - 1, path = g.path, fill, size = keep.size(),
                                onIt = local * world * place * QTransform::fromTranslate(-keep.left(), -keep.top()),
                                hints = p.renderHints(), textStrokes, hollow, blur = radius * shrink, worked = part] {
        QImage picture(size, QImage::Format_ARGB32_Premultiplied);
        picture.fill(Qt::transparent);
        {
          QPainter q(&picture);
          q.setRenderHints(hints);
          q.setWorldTransform(onIt);
          q.fillPath(path, fill);
        }
        textStrokeStackPass(picture, textStrokes, std::sqrt(std::abs(onIt.determinant())), 1., {onIt.map(path)}, hollow,
                            {}, {}, nullptr, worked);
        deferred->overlays[slot].picture = fullResolutionBlurPart(picture, blur, worked);
        ++blurredLetterPictures();
      });
    }
    // A blurred background fades with its letter's picture (layLetterBackdrop).
    for (const auto &[slot, i] : blurredBackgrounds) deferred->backdrop->pieces[slot].overlay = overlayOf[size_t(i)];
    return;
  }
  // Everything of the letters listed, onto any painter: backgrounds under all,
  // then whole-layer strokes keeping their order while every character has a
  // different pose -- all outlines first, then all fills.
  auto drawLetters = [&](QPainter &p, const QVector<int> &which) {
    // A background goes with the letter that carries it: its own letter, or
    // the first of its word or line -- which, when the animation moves whole
    // words or lines, moves the way all of them do.
    if (letters && letters->backgroundCarrier.size() == letters->backgrounds.size())
      for (int b = 0; b < letters->backgrounds.size(); ++b) {
        const int i = letters->backgroundCarrier[b];
        if (i < 0 || i >= count || !which.contains(i)) continue;
        paintGlyph(p, i, [&, b](AnimatedGlyph &) {
          p.save();
          p.setOpacity(p.opacity() * letters->backgroundOpacity);
          letters->drawBackground(p, b);
          p.restore();
        });
      }
    // A letter part way through fading in or out is drawn see-through, and its
    // fill no longer hides the inner half of the pen under it: an Outside
    // stroke showed inside the letter, tinting it. Such a letter's outside
    // strokes are kept to the outside, as Stroke Only keeps them.
    for (int strokeIndex = int(textStrokes.size()) - 1; strokeIndex >= 0; --strokeIndex)
      for (int i : which)
        paintGlyph(p, i, [&, strokeIndex, i](AnimatedGlyph &g) {
          drawTextStrokeStackPass(p, g.path, textStrokes, strokeIndex, hollow || poses[i].opacity < .999);
        });
    if (useCharacterOutline)
      for (int i : which)
        paintGlyph(p, i, [&](AnimatedGlyph &g) {
          drawTextStrokePass(p, g.path, Qt::NoBrush, g.outline, l.strokeMode, false);
        });
    if (!hollow)
      for (int i : which)
        paintGlyph(p, i, [&, i](AnimatedGlyph &g) {
          drawTextFillAfterStrokes(p, g.path, textured ? letters->letterBrush : QBrush(fillOf(i)), textStrokes,
                                   useCharacterOutline ? g.outline : QPen(), l.strokeMode);
        });
    for (int strokeIndex = int(textStrokes.size()) - 1; strokeIndex >= 0; --strokeIndex)
      for (int i : which)
        paintGlyph(p, i, [&, strokeIndex](AnimatedGlyph &g) {
          drawTextStrokeStackInsidePass(p, g.path, textStrokes, strokeIndex);
        });
  };
  // A blurred letter is drawn on its own, small, blurred and laid in place;
  // the rest are drawn as always.
  const QTransform world = p.worldTransform();
  const double worldScale = std::sqrt(std::abs(world.determinant()));
  QVector<int> sharp, blurred;
  for (int i = 0; i < count; ++i)
    (poses[i].blur * worldScale >= .4 && poses[i].opacity > 0 ? blurred : sharp).push_back(i);
  drawLetters(p, sharp);
  for (int i : blurred) {
    const auto &g = glyphs[i];
    const double radius = poses[i].blur * worldScale; // in pixels on the picture
    QTransform local;
    local.translate(g.center.x(), g.center.y());
    local = poses[i].matrix * local;
    local.translate(-g.center.x(), -g.center.y());
    QRectF box = g.path.boundingRect();
    if (letters && letters->backgroundCarrier.size() == letters->backgrounds.size())
      for (int b = 0; b < letters->backgrounds.size(); ++b)
        if (letters->backgroundCarrier[b] == i) box = box.united(letters->backgroundBox(b));
    double strokeReach = 2;
    for (const auto &stroke : textStrokes) strokeReach += stroke.width;
    box.adjust(-strokeReach, -strokeReach, strokeReach, strokeReach);
    QRectF onPicture = (local * world).mapRect(box).adjusted(-radius * 3, -radius * 3, radius * 3, radius * 3);
    if (onPicture.isEmpty()) continue;
    // A wide blur needs no full detail: drawn smaller, blurred, then enlarged.
    const double shrink = blurredLetterShrink(radius);
    const QSize size(std::clamp(int(std::ceil(onPicture.width() * shrink)), 1, 2048),
                     std::clamp(int(std::ceil(onPicture.height() * shrink)), 1, 2048));
    // Set apart (`deferred`, a raster painted in strips): each strip drew and
    // blurred every blurred letter again for its own rows -- ten strips, ten
    // times, 245 ms of a frame of a title at 900% -- and drew the whole of a
    // letter mostly outside the frame. The strip that collects draws the part
    // that can show; the blur is left to be done once with the others
    // (AnimatedLetterStrokes::jobs), laid over the raster after.
    // Not under a clip (a hand writing the letters): laid apart, they would
    // show past it.
    const bool apart = deferred && !p.hasClipping();
    const QRect keep = apart ? kept(onPicture, size, radius * shrink, 0) : QRect(QPoint(), size);
    if (apart && (!deferred->collect || keep.isEmpty())) continue;
    const QRect part = apart ? worked(onPicture, size, keep, radius * shrink) : QRect();
    if (apart && part.isEmpty()) continue;
    QImage picture(keep.size(), QImage::Format_ARGB32_Premultiplied);
    picture.fill(Qt::transparent);
    {
      QPainter q(&picture);
      q.setRenderHints(p.renderHints());
      QTransform place;
      place.scale(double(size.width()) / onPicture.width(), double(size.height()) / onPicture.height());
      place.translate(-onPicture.left(), -onPicture.top());
      q.setWorldTransform(world * place * QTransform::fromTranslate(-keep.left(), -keep.top()));
      drawLetters(q, {i});
    }
    if (apart) {
      // Its picture the part's rows (fullResolutionBlurPart).
      deferred->overlays.push_back(
          {QImage(), landing(onPicture, size, QRect(keep.left(), keep.top() + part.top(), keep.width(), part.height())),
           p.opacity(), p.renderHints()});
      deferred->jobs.push_back([deferred, slot = deferred->overlays.size() - 1, picture, blur = radius * shrink, part] {
        deferred->overlays[slot].picture = fullResolutionBlurPart(picture, blur, part);
        ++blurredLetterPictures();
      });
      continue;
    }
    picture = fullResolutionBlur(picture, radius * shrink);
    ++blurredLetterPictures();
    p.save();
    p.resetTransform();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(onPicture, picture);
    p.restore();
  }
}
