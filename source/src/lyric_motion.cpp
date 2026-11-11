#include "lyric_motion.h"
#include "waveform.h"
#include <QFontMetricsF>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace {
// Loudness per second, the mean of the fine RMS levels.
std::vector<double> loudnessPerSecond(const AudioPeaks &peaks) {
  const int per = AudioPeaks::perSecond;
  const int seconds = int(peaks.rms.size()) / per;
  std::vector<double> out(static_cast<size_t>(std::max(0, seconds)), 0.);
  for (int s = 0; s < seconds; ++s) {
    double sum = 0;
    for (int i = s * per; i < (s + 1) * per; ++i) sum += peaks.rms[size_t(i)];
    out[size_t(s)] = sum / per;
  }
  return out;
}
double percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[size_t(std::clamp(p, 0., 1.) * double(values.size() - 1))];
}
// The beats that begin bars: of the four ways of counting, the one whose beats
// hit hardest.
std::vector<double> downbeats(const BeatAnalysis &beat) {
  if (beat.beats.size() < 8) return beat.beats;
  double best = -1; size_t phase = 0;
  for (size_t k = 0; k < 4; ++k) {
    double sum = 0; int n = 0;
    for (size_t i = k; i < beat.beats.size(); i += 4) {
      sum += i < beat.strengths.size() ? beat.strengths[i] : 0; ++n;
    }
    if (n && sum / n > best) { best = sum / n; phase = k; }
  }
  std::vector<double> out;
  for (size_t i = phase; i < beat.beats.size(); i += 4) out.push_back(beat.beats[i]);
  return out;
}
bool ideograph(QChar c) {
  const ushort u = c.unicode();
  return (u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3400 && u <= 0x4DBF);
}
bool visible(QChar c) { return !c.isSpace(); }
}  // namespace

std::vector<LyricCue> parseLyricCues(const QString &content, bool *timed) {
  static const QRegularExpression stamp("\\[(\\d+):(\\d+(?:[.:]\\d+)?)\\]");
  std::vector<LyricCue> withTimes, plain;
  for (QString raw : content.split('\n')) {
    raw = raw.trimmed();
    if (raw.startsWith(QChar(0xFEFF))) raw.remove(0, 1);
    if (raw.isEmpty() || raw.startsWith('#')) continue;
    std::vector<double> stamps;
    int end = 0;
    auto it = stamp.globalMatch(raw);
    while (it.hasNext()) {
      const auto m = it.next();
      if (m.capturedStart() != end) break; // times lead the line
      QString seconds = m.captured(2);
      seconds.replace(':', '.');
      stamps.push_back(m.captured(1).toDouble() * 60. + seconds.toDouble());
      end = int(m.capturedEnd());
    }
    // Word times inside a line (<mm:ss.xx>) are dropped; the line keeps its words.
    QString text = raw.mid(end);
    text.remove(QRegularExpression("<\\d+:\\d+(?:[.:]\\d+)?>"));
    text = text.trimmed();
    if (stamps.empty()) {
      if (!raw.startsWith('[')) plain.push_back({-1, -1, raw});
      continue;
    }
    // An empty timed line is a pause: it ends the line before it.
    for (double s : stamps) withTimes.push_back({s, -1, text});
  }
  const bool hasTimes = !withTimes.empty();
  if (timed) *timed = hasTimes;
  if (!hasTimes) return plain;
  std::stable_sort(withTimes.begin(), withTimes.end(), [](const LyricCue &a, const LyricCue &b) { return a.start < b.start; });
  std::vector<LyricCue> out;
  for (size_t i = 0; i < withTimes.size(); ++i) {
    LyricCue c = withTimes[i];
    const double next = i + 1 < withTimes.size() ? withTimes[i + 1].start : c.start + 5;
    c.end = std::min(next - .08, c.start + 10.);
    if (!c.text.isEmpty() && c.end > c.start + .2) out.push_back(c);
  }
  return out;
}

std::vector<std::pair<double, double>> loudSpans(const AudioPeaks &peaks) {
  const auto level = loudnessPerSecond(peaks);
  std::vector<std::pair<double, double>> spans;
  if (level.size() < 8) return spans;
  // Smoothed over four seconds, against the loud end of the song.
  std::vector<double> smooth(level.size());
  for (size_t i = 0; i < level.size(); ++i) {
    double sum = 0; int n = 0;
    for (size_t j = i >= 2 ? i - 2 : 0; j <= std::min(level.size() - 1, i + 2); ++j) { sum += level[j]; ++n; }
    smooth[i] = sum / n;
  }
  const double threshold = percentile(smooth, .9) * .86;
  double from = -1;
  for (size_t i = 0; i <= smooth.size(); ++i) {
    const bool loud = i < smooth.size() && smooth[i] >= threshold;
    if (loud && from < 0) from = double(i);
    if (!loud && from >= 0) {
      if (double(i) - from >= 8) {
        // Close to the last one: one span.
        if (!spans.empty() && from - spans.back().second < 4) spans.back().second = double(i);
        else spans.push_back({from, double(i)});
      }
      from = -1;
    }
  }
  return spans;
}

void fitLyricCues(std::vector<LyricCue> &cues, const AudioPeaks &peaks, const BeatAnalysis &beat) {
  if (cues.empty()) return;
  const auto level = loudnessPerSecond(peaks);
  const auto bars = downbeats(beat);
  const double seconds = peaks.seconds();
  if (bars.size() < 4 || level.empty()) {
    for (size_t i = 0; i < cues.size(); ++i) {
      cues[i].start = 2 + (seconds - 4) * double(i) / double(cues.size());
      cues[i].end = cues[i].start + (seconds - 4) / double(cues.size()) - .1;
    }
    return;
  }
  // Where the music is playing, and where it is not.
  const double on = percentile(level, .9) * .3;
  auto playing = [&](double t) {
    const int s = std::clamp(int(t), 0, int(level.size()) - 1);
    return level[size_t(s)] >= on;
  };
  // Singing starts after an intro of a few bars and stops before the tail.
  size_t first = 0;
  while (first < bars.size() && !playing(bars[first] + .5)) ++first;
  first = std::min(bars.size() - 1, first + 4);
  size_t last = bars.size() - 1;
  while (last > first && !playing(bars[last] - .5)) --last;
  last = last > first + 2 ? last - 2 : last;
  // The bars in between that have music in them.
  std::vector<double> usable;
  for (size_t b = first; b < last; ++b) if (playing(bars[b] + 1)) usable.push_back(bars[b]);
  if (usable.empty()) usable.push_back(bars[first]);
  const double barLength = bars.size() > 1 ? (bars.back() - bars.front()) / double(bars.size() - 1) : 2.4;
  for (size_t i = 0; i < cues.size(); ++i) {
    const size_t at = std::min(usable.size() - 1, size_t(double(i) * double(usable.size()) / double(cues.size())));
    const size_t next = std::min(usable.size() - 1, size_t(double(i + 1) * double(usable.size()) / double(cues.size())));
    cues[i].start = usable[at];
    const double until = next > at ? usable[next] : usable[at] + 2 * barLength;
    cues[i].end = std::min(until - .1, cues[i].start + 3 * barLength);
  }
}

QString lyricCuesToLrc(const std::vector<LyricCue> &cues) {
  QString out;
  for (const auto &c : cues) {
    const int minutes = int(c.start / 60);
    const double rest = c.start - minutes * 60;
    out += QString("[%1:%2]%3\n").arg(minutes, 2, 10, QChar('0')).arg(rest, 5, 'f', 2, QChar('0')).arg(c.text);
  }
  return out;
}

std::vector<Layer> lyricMotionLayers(const std::vector<LyricCue> &cues, const LyricMotionOptions &o) {
  std::vector<Layer> out;
  const double fps = std::max(1., o.fps);
  const double W = o.width, H = o.height;
  std::vector<QPointF> places = o.places;
  if (places.empty())
    places = {{W * .35, H * .42}, {W * .65, H * .44}, {W * .5, H * .76}, {W * .37, H * .3}, {W * .63, H * .7}};
  auto frame = [&](double seconds) { return o.songStartFrame + seconds * fps; };
  // Keys in a layer's own frames; ease 0 linear, 1 in-out, 2 out, 3 in, 4 hold, 6 expo out.
  auto key = [](Property &p, double f, double v, int kind) {
    p.set(f, v);
    for (Key &k : p.keys)
      if (std::abs(k.frame - f) < 1e-6) {
        k.custom = false;
        k.ease = kind == 0 ? "linear" : kind == 2 ? "easeOut" : kind == 3 ? "easeIn" : kind == 4 ? "hold" : "easeInOut";
        if (kind == 6) { k.ease = "easeOut"; k.custom = true; k.curve = {.16, 1, .3, 1}; }
      }
  };
  auto isLoud = [&](const LyricCue &c) {
    if (o.style == 1) return false;
    if (o.style == 2) return true;
    const double mid = (c.start + c.end) / 2;
    for (const auto &[a, b] : o.loud) if (mid >= a && mid < b) return true;
    return false;
  };
  std::vector<Marker> beatMarkers;
  for (double t : o.beats) { Marker m; m.frame = frame(t); m.beat = true; beatMarkers.push_back(m); }
  int quietIndex = 0;
  for (size_t i = 0; i < cues.size(); ++i) {
    const LyricCue &cue = cues[i];
    const QString number = QString("%1").arg(int(i) + 1, 2, 10, QChar('0'));
    const QString tag = "Lyric " + number;
    // The line arrives a little before it is sung and is gone as the next begins.
    const double from = std::floor(frame(cue.start) - 4), to = std::ceil(frame(std::max(cue.end, cue.start + 1.)));
    const double length = std::max(20., to - from);
    int letters = 0;
    for (QChar c : cue.text) letters += visible(c);
    letters = std::max(1, letters);
    // Every piece of the line drifts the same way while it lasts.
    const double side = i % 2 ? 1 : -1;
    const QPointF drift(side * 26, -18);
    auto base = [&](Layer &l, const QString &name) {
      l.name = tag + " " + name; l.namedByHand = true;
      l.start = from; l.duration = length;
    };
    auto move = [&](Layer &l, QPointF at, QPointF by) {
      l.p[0] = Property{}; l.p[1] = Property{};
      key(l.p[0], 0, at.x(), 0); key(l.p[0], length, at.x() + by.x(), 0);
      key(l.p[1], 0, at.y(), 0); key(l.p[1], length, at.y() + by.y(), 0);
    };
    const bool loud = isLoud(cue);
    if (loud) {
      // Loud: large, the letters punching in one after another, an outline
      // echo behind, the whole line pulsing with the beat.
      const double size = letters <= 6 ? 170 : letters <= 11 ? 124 : letters <= 18 ? 92 : 70;
      const QPointF at(W / 2, (i % 2 ? H * .72 : H * .5));
      Layer words;
      words.type = "text"; words.text = cue.text; words.font = o.keyFont; words.bold = true; words.fontSize = size;
      words.color = o.ink; words.tracking = size * .08;
      words.shadow = 8; words.appliedEffects["Drop Shadow"] = true;
      base(words, "(words)");
      move(words, at, QPointF(side * 14, 0));
      const double delay = std::clamp((length * .3) / letters, 1., 4.);
      words.characterAnimation = QJsonObject{{"enabled", true}, {"scale", 240}, {"opacity", 0}, {"y", 0}, {"delay", delay},
                                             {"separateOut", true}, {"outScale", 60}, {"outOpacity", 0},
                                             {"outY", -30}, {"outDelay", 1}, {"wiggleSpeed", 5}, {"wigglePosition", 3}};
      words.p[6] = Property{}; words.p[7] = Property{};
      key(words.p[6], 0, 0, 6); key(words.p[6], 7, 100, 0);
      key(words.p[7], length - 10, 0, 3); key(words.p[7], length, 100, 0);
      std::vector<Marker> within;
      for (const Marker &m : beatMarkers) if (m.frame >= from + 8 && m.frame < from + length - 8) within.push_back(m);
      PulseSettings pulse; pulse.strength = .05; pulse.settle = 6;
      pulseOnMarkers(words, within, pulse);
      Layer echo = words;
      echo.characterAnimation = QJsonObject();
      echo.textStrokes = {{2, o.accent, 1, 0}};
      syncLegacyTextStroke(echo);
      echo.textStrokeOnly = true;
      echo.shadow = 0; echo.appliedEffects["Drop Shadow"] = false;
      echo.color = o.accent;
      echo.name = tag + " (echo)";
      echo.p[2] = Property{}; echo.p[3] = Property{};
      key(echo.p[2], 0, 1.35, 6); key(echo.p[2], 10, 1.08, 1); key(echo.p[2], length, 1.14, 0);
      echo.p[3] = echo.p[2];
      echo.p[5] = Property{};
      key(echo.p[5], 0, 0, 2); key(echo.p[5], 6, .7, 1); key(echo.p[5], length - 8, .45, 1); key(echo.p[5], length, 0, 0);
      pulseOnMarkers(echo, within, pulse);
      out.push_back(echo);
      out.push_back(words);
      continue;
    }
    // Quiet: small words written on by hand, upright when short, with the line's
    // first ideograph set large on a highlight beside them, a hairline drawn on
    // and the line's number.
    QPointF at = places[size_t(quietIndex++) % places.size()];
    // Upright only when the column fits between the bars of a letterboxed frame.
    const bool upright = letters <= 10 && quietIndex % 3 != 0;
    if (upright) at.setY(H * .5);
    QString keyChar;
    for (QChar c : cue.text) if (ideograph(c)) { keyChar = c; break; }
    if (keyChar.isEmpty()) for (QChar c : cue.text) if (visible(c)) { keyChar = c; break; }
    const double size = upright ? std::min(58., H * .6 / (letters * 1.18)) : std::clamp(1500. / letters, 48., 74.);
    QString shown = cue.text;
    if (upright) {
      shown.clear();
      for (QChar c : cue.text) if (visible(c)) shown += QString(c) + "\n";
      shown = shown.trimmed();
    }
    QFont metricsFont(o.font); metricsFont.setPixelSize(int(size));
    const QFontMetricsF metrics(metricsFont);
    const double tracking = upright ? 0 : size * .12;
    const double extent = upright ? letters * size * 1.18 : metrics.horizontalAdvance(cue.text) + tracking * letters;
    Layer words;
    words.type = "text"; words.text = shown; words.font = o.font; words.bold = false; words.fontSize = size;
    words.color = o.ink; words.tracking = tracking; words.leading = 1.18;
    // A soft shadow keeps small words readable over a bright picture.
    words.shadow = 6; words.appliedEffects["Drop Shadow"] = true;
    base(words, "(words)");
    move(words, at, drift);
    // Written on over the first part of the line, the way it is sung.
    const double writing = std::clamp(length * .55, 12., 90.);
    words.characterAnimation = QJsonObject{{"enabled", true}, {"write", true}, {"writeNib", 120}, {"opacity", 100}, {"y", 0},
                                           {"separateOut", true}, {"outY", -24}, {"outOpacity", 0}, {"outDelay", 1}};
    words.p[6] = Property{}; words.p[7] = Property{};
    key(words.p[6], 0, 0, 0); key(words.p[6], writing, 100, 0);
    key(words.p[7], length - 12, 0, 3); key(words.p[7], length, 100, 0);
    out.push_back(words);
    // The key character, large, on a highlight that wipes in.
    const double keySize = upright ? 150 : 140;
    const QPointF keyAt = upright ? QPointF(at.x() + size * .9 + keySize * .55, at.y() - extent / 2 + keySize * .5)
                                  : QPointF(at.x() - extent / 2 + keySize * .45, at.y() - size * .6 - keySize * .55);
    Layer mark;
    mark.type = "shape"; mark.shapeKind = "rect"; mark.color = o.accent;
    base(mark, "(highlight)");
    move(mark, keyAt, drift);
    mark.p[ShapeHeight].base = keySize * 1.05;
    mark.p[ShapeWidth] = Property{};
    key(mark.p[ShapeWidth], 2, 0, 6); key(mark.p[ShapeWidth], 14, keySize * 1.05, 0);
    key(mark.p[ShapeWidth], length - 10, keySize * 1.05, 3); key(mark.p[ShapeWidth], length, 0, 0);
    mark.p[5].base = .82;
    out.push_back(mark);
    Layer big;
    big.type = "text"; big.text = keyChar; big.font = o.keyFont; big.bold = true; big.fontSize = keySize; big.color = o.ink;
    big.shadow = 5; big.appliedEffects["Drop Shadow"] = true;
    base(big, "(key)");
    move(big, keyAt, drift);
    big.characterAnimation = QJsonObject{{"enabled", true}, {"scale", 160}, {"opacity", 0}, {"y", 0},
                                         {"separateOut", true}, {"outScale", 80}, {"outOpacity", 0}};
    big.p[6] = Property{}; big.p[7] = Property{};
    key(big.p[6], 4, 0, 6); key(big.p[6], 14, 100, 0);
    key(big.p[7], length - 10, 0, 3); key(big.p[7], length, 100, 0);
    out.push_back(big);
    // A hairline drawn on beside the words.
    Layer rule;
    rule.type = "shape"; rule.shapeKind = "path"; rule.shapeClosed = false; rule.shapeFill = false;
    rule.strokeWidth = 1.4; rule.strokeColor = o.ink; rule.color = o.ink;
    const double reach = extent / 2 + 30;
    rule.shapePoints = upright ? QJsonArray{QJsonObject{{"p", QJsonArray{0, -reach}}}, QJsonObject{{"p", QJsonArray{0, reach}}}}
                               : QJsonArray{QJsonObject{{"p", QJsonArray{-reach, 0}}}, QJsonObject{{"p", QJsonArray{reach, 0}}}};
    base(rule, "(rule)");
    move(rule, upright ? QPointF(at.x() - size * .95, at.y()) : QPointF(at.x(), at.y() + size * .85), drift);
    rule.p[ShapeTrimEnd] = Property{}; rule.p[ShapeTrimStart] = Property{};
    key(rule.p[ShapeTrimEnd], 0, 0, 6); key(rule.p[ShapeTrimEnd], std::min(writing, 30.), 100, 0);
    key(rule.p[ShapeTrimStart], length - 14, 0, 3); key(rule.p[ShapeTrimStart], length, 100, 0);
    rule.p[5].base = .65;
    out.push_back(rule);
    // The line's number, small, at the end of the rule.
    Layer label;
    label.type = "text"; label.text = QString::fromUtf16(u"— ") + number; label.font = "Helvetica Neue"; label.bold = false;
    label.fontSize = 16; label.tracking = 4; label.color = o.ink;
    base(label, "(number)");
    move(label, upright ? QPointF(at.x() - size * .95, at.y() + reach + 26) : QPointF(at.x() + reach - 30, at.y() + size * .85 + 22), drift);
    label.p[5] = Property{};
    key(label.p[5], 0, 0, 1); key(label.p[5], 12, .7, 1); key(label.p[5], length - 10, .7, 1); key(label.p[5], length, 0, 0);
    out.push_back(label);
  }
  return out;
}

// ---- where the voice is ----------------------------------------------------------------
// A singer sits in the middle of a mix and holds a pitch: the band spreads out
// to the sides and plays chords. So, over the voice's range, the sound is
// scored by how much of it is in the middle (mid against side) and by how
// clearly it repeats at a single pitch (the peak of its autocorrelation),
// every 20 ms.
std::vector<float> voiceScore(const QByteArray &pcm) {
  const qint64 frames = pcm.size() / 4;
  const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
  constexpr int rate = 48000, hop = 960; // 20 ms
  const int count = int(frames / hop);
  std::vector<float> score(static_cast<size_t>(std::max(0, count)), 0.f);
  if (count < 10) return score;
  // Band-limit the middle and the sides to about 250 Hz - 3.5 kHz.
  const double hpA = std::exp(-2 * 3.14159265358979 * 250. / rate), lpA = std::exp(-2 * 3.14159265358979 * 3500. / rate);
  std::vector<float> mid(static_cast<size_t>(frames)), side(static_cast<size_t>(frames));
  double mh = 0, mPrev = 0, ml = 0, sh = 0, sPrev = 0, sl = 0;
  for (qint64 i = 0; i < frames; ++i) {
    const double l = samples[i * 2] / 32768., r = samples[i * 2 + 1] / 32768.;
    const double m = (l + r) * .5, s = (l - r) * .5;
    mh = hpA * (mh + m - mPrev); mPrev = m; ml = ml * lpA + mh * (1 - lpA);
    sh = hpA * (sh + s - sPrev); sPrev = s; sl = sl * lpA + sh * (1 - lpA);
    mid[size_t(i)] = float(ml); side[size_t(i)] = float(sl);
  }
  // The middle at 8 kHz, for the pitch.
  std::vector<float> low(static_cast<size_t>(frames / 6));
  for (size_t i = 0; i < low.size(); ++i) low[i] = mid[i * 6];
  std::vector<double> level(static_cast<size_t>(count));
  for (int f = 0; f < count; ++f) {
    double em = 0, es = 0;
    for (int i = f * hop; i < (f + 1) * hop; ++i) { em += double(mid[size_t(i)]) * mid[size_t(i)]; es += double(side[size_t(i)]) * side[size_t(i)]; }
    const double centred = em / (em + es + 1e-12);
    // Pitch: a 40 ms window, lags for 100 Hz - 800 Hz.
    const int start = f * hop / 6, length = 320;
    double pitch = 0;
    if (start + length + 80 < int(low.size())) {
      double e0 = 0;
      for (int i = 0; i < length; ++i) e0 += double(low[size_t(start + i)]) * low[size_t(start + i)];
      for (int lag = 10; lag <= 80; ++lag) {
        double c = 0, e1 = 0;
        for (int i = 0; i < length; ++i) {
          c += double(low[size_t(start + i)]) * low[size_t(start + i + lag)];
          e1 += double(low[size_t(start + i + lag)]) * low[size_t(start + i + lag)];
        }
        if (e0 > 0 && e1 > 0) pitch = std::max(pitch, c / std::sqrt(e0 * e1));
      }
    }
    level[size_t(f)] = em / hop;
    score[size_t(f)] = float(std::max(0., pitch) * pitch * centred);
  }
  // Nothing counts where the music itself is near silence.
  std::vector<double> sorted = level;
  std::sort(sorted.begin(), sorted.end());
  const double quiet = sorted[size_t(sorted.size() * .2)] * .5;
  for (int f = 0; f < count; ++f) if (level[size_t(f)] < quiet) score[size_t(f)] = 0;
  return score;
}

// Stretches where somebody is singing, in seconds: the score smoothed over a
// tenth of a second, held above what is usual for this song, short gaps
// bridged and short blips dropped.
std::vector<std::pair<double, double>> voicePhrases(const std::vector<float> &score) {
  std::vector<std::pair<double, double>> phrases;
  if (score.size() < 20) return phrases;
  std::vector<float> smooth(score.size());
  for (size_t i = 0; i < score.size(); ++i) {
    double sum = 0; int n = 0;
    for (size_t j = i >= 3 ? i - 3 : 0; j <= std::min(score.size() - 1, i + 3); ++j) { sum += score[j]; ++n; }
    smooth[i] = float(sum / n);
  }
  std::vector<float> sorted(smooth.begin(), smooth.end());
  std::sort(sorted.begin(), sorted.end());
  const float on = sorted[size_t(sorted.size() * .55)], off = sorted[size_t(sorted.size() * .42)];
  bool singing = false;
  double from = 0;
  for (size_t i = 0; i <= smooth.size(); ++i) {
    const float v = i < smooth.size() ? smooth[i] : 0.f;
    const double t = i * .02;
    if (!singing && v >= on) { singing = true; from = t; }
    else if (singing && v < off) {
      singing = false;
      if (!phrases.empty() && from - phrases.back().second < .22) phrases.back().second = t;
      else phrases.push_back({from, t});
    }
  }
  phrases.erase(std::remove_if(phrases.begin(), phrases.end(), [](const auto &p) { return p.second - p.first < .35; }),
                phrases.end());
  return phrases;
}

// Lines laid on the singing: each line begins where a phrase begins, in order,
// choosing the beginnings that best give every line room for its letters and
// that follow the longest silences.
void fitLyricCuesToVoice(std::vector<LyricCue> &cues, const std::vector<std::pair<double, double>> &phrases) {
  const int n = int(cues.size()), m = int(phrases.size());
  if (n == 0 || m < n) return;
  std::vector<double> letters(static_cast<size_t>(n));
  double totalLetters = 0;
  for (int i = 0; i < n; ++i) {
    int c = 0;
    for (QChar ch : cues[size_t(i)].text) c += !ch.isSpace();
    letters[size_t(i)] = std::max(1, c);
    totalLetters += letters[size_t(i)];
  }
  double singing = 0;
  for (const auto &p : phrases) singing += p.second - p.first;
  const double perLetter = singing / totalLetters; // seconds of voice a letter takes, on average
  // How strongly a phrase looks like the start of a line: the silence before it.
  std::vector<double> gapBefore(static_cast<size_t>(m));
  for (int j = 0; j < m; ++j) gapBefore[size_t(j)] = j ? phrases[size_t(j)].first - phrases[size_t(j - 1)].second : 4.;
  // best[i][j]: line i starting at phrase j, lines before it placed well.
  const double none = -1e18;
  std::vector<std::vector<double>> best(static_cast<size_t>(n), std::vector<double>(static_cast<size_t>(m), none));
  std::vector<std::vector<int>> from(static_cast<size_t>(n), std::vector<int>(static_cast<size_t>(m), -1));
  auto voiceBetween = [&](int a, int b) { // the singing in phrases a .. b-1
    double v = 0;
    for (int k = a; k < b; ++k) v += phrases[size_t(k)].second - phrases[size_t(k)].first;
    return v;
  };
  auto startScore = [&](int j) { return std::log(1 + std::min(gapBefore[size_t(j)], 3.) * 4); };
  for (int j = 0; j <= m - n; ++j) best[0][size_t(j)] = startScore(j) - (j > 0 ? .15 * j : 0);
  for (int i = 1; i < n; ++i)
    for (int j = i; j <= m - (n - i); ++j)
      for (int k = i - 1; k < j; ++k) {
        if (best[size_t(i - 1)][size_t(k)] <= none) continue;
        // Line i-1 sang phrases k .. j-1: about as long as its letters want?
        const double want = letters[size_t(i - 1)] * perLetter, got = voiceBetween(k, j);
        const double fit = -std::abs(std::log((got + .2) / (want + .2))) * 1.6;
        const double v = best[size_t(i - 1)][size_t(k)] + fit + startScore(j);
        if (v > best[size_t(i)][size_t(j)]) { best[size_t(i)][size_t(j)] = v; from[size_t(i)][size_t(j)] = k; }
      }
  // The last line takes what is left, as its letters want.
  int bestJ = -1; double bestV = none;
  for (int j = n - 1; j < m; ++j) {
    if (best[size_t(n - 1)][size_t(j)] <= none) continue;
    int end = j + 1;
    const double want = letters[size_t(n - 1)] * perLetter;
    while (end < m && voiceBetween(j, end) < want) ++end;
    const double v = best[size_t(n - 1)][size_t(j)] - std::abs(std::log((voiceBetween(j, end) + .2) / (want + .2))) * 1.6;
    if (v > bestV) { bestV = v; bestJ = j; }
  }
  if (bestJ < 0) return;
  std::vector<int> start(static_cast<size_t>(n));
  for (int i = n - 1, j = bestJ; i >= 0; --i) { start[size_t(i)] = j; j = from[size_t(i)][size_t(j)]; if (i > 0 && j < 0) return; }
  for (int i = 0; i < n; ++i) {
    const int j = start[size_t(i)];
    const int next = i + 1 < n ? start[size_t(i + 1)] : m;
    cues[size_t(i)].start = phrases[size_t(j)].first;
    // Until the last of its phrases is sung, and a breath more; never into the next line.
    const double sungTo = phrases[size_t(std::max(j, next - 1))].second + .7;
    const double nextStart = i + 1 < n ? phrases[size_t(next)].first - .08 : sungTo + 1.5;
    cues[size_t(i)].end = std::min(nextStart, std::max(sungTo, cues[size_t(i)].start + 1.2));
  }
}

// ---- times kept apart from the words ------------------------------------------------------
std::vector<double> parseLyricTimes(const QString &content) {
  static const QRegularExpression stamp(R"(^\[?(\d+):(\d+(?:\.\d+)?)\]?$)");
  std::vector<double> times;
  for (QString raw : content.split('\n')) {
    raw = raw.trimmed();
    if (raw.startsWith(QChar(0xFEFF))) raw.remove(0, 1);
    if (raw.isEmpty() || raw.startsWith('#')) continue;
    const auto m = stamp.match(raw);
    bool ok = false;
    const double seconds = m.hasMatch() ? m.captured(1).toDouble() * 60 + m.captured(2).toDouble() : raw.toDouble(&ok);
    if (m.hasMatch() || ok) times.push_back(seconds);
  }
  return times;
}

bool applyLyricTimes(std::vector<LyricCue> &cues, const std::vector<double> &times) {
  if (cues.empty() || times.size() < cues.size()) return false;
  for (size_t i = 0; i < cues.size(); ++i) {
    cues[i].start = times[i];
    const double next = i + 1 < times.size() ? times[i + 1] : times[i] + 5;
    // The last line keeps all the time it is given; the others stop short of the next.
    cues[i].end = i + 1 == cues.size() && i + 1 < times.size() ? next : std::min(next - .08, times[i] + 10);
  }
  return true;
}
