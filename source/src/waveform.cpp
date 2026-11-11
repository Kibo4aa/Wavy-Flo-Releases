#include "waveform.h"
#include "native_media.h"
#include "engine.h"
#include "prerender.h"
#include "lyric_motion.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QThreadPool>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cmath>

std::pair<double, double> AudioPeaks::range(double from, double to) const {
  if (peak.empty() || to <= 0 || from >= seconds()) return {0, 0};
  // The coarse levels once a pixel covers more than a few of them: the same
  // answer (the loudest in the span) for a twentieth of the reading.
  const bool coarse = (to - from) * perSecond > 60 && !coarsePeak.empty();
  const auto &peaks = coarse ? coarsePeak : peak;
  const auto &levels = coarse ? coarseRms : rms;
  const double rate = coarse ? coarsePerSecond : perSecond;
  const qint64 count = qint64(peaks.size());
  const qint64 a = std::clamp<qint64>(qint64(std::floor(from * rate)), 0, count - 1);
  const qint64 b = std::clamp<qint64>(qint64(std::ceil(to * rate)), a + 1, count);
  int p = 0, r = 0;
  for (qint64 i = a; i < b; ++i) {
    p = std::max(p, int(peaks[size_t(i)]));
    r = std::max(r, int(levels[size_t(i)]));
  }
  return {p / 255., r / 255.};
}

void AudioPeaks::summarise() {
  const size_t step = perSecond / coarsePerSecond;
  coarsePeak.clear(); coarseRms.clear();
  for (size_t i = 0; i < peak.size(); i += step) {
    quint8 p = 0, r = 0;
    for (size_t k = i; k < std::min(peak.size(), i + step); ++k) {
      p = std::max(p, peak[k]);
      r = std::max(r, rms[k]);
    }
    coarsePeak.push_back(p);
    coarseRms.push_back(r);
  }
}

AudioPeakSink::AudioPeakSink() { open(QIODevice::WriteOnly); }

void AudioPeakSink::flush() {
  if (bucketFrames == 0) return;
  const double level = std::sqrt(bucketSquares / bucketFrames);
  peaks.peak.push_back(quint8(std::lround(std::clamp(bucketPeak, 0., 1.) * 255)));
  peaks.rms.push_back(quint8(std::lround(std::clamp(level, 0., 1.) * 255)));
  peaks.energy.push_back(float(bucketSquares / bucketFrames));
  peaks.bass.push_back(float(bucketBass / bucketFrames));
  bucketPeak = bucketSquares = bucketBass = 0;
  bucketFrames = 0;
}

qint64 AudioPeakSink::writeData(const char *data, qint64 size) {
  constexpr int framesPerBucket = 48000 / AudioPeaks::perSecond;
  QByteArray joined;
  const char *bytes = data;
  qint64 length = size;
  // A write can end part way through a stereo frame; that part waits for the next.
  if (!carry.isEmpty()) {
    joined = carry + QByteArray(data, int(size));
    carry.clear();
    bytes = joined.constData();
    length = joined.size();
  }
  const qint64 whole = length / 4 * 4;
  for (qint64 i = 0; i < whole; i += 4) {
    const double l = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(bytes + i)) / 32768.;
    const double r = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(bytes + i + 2)) / 32768.;
    bucketPeak = std::max({bucketPeak, std::abs(l), std::abs(r)});
    bucketSquares += (l * l + r * r) * .5;
    // About 150 Hz, twice over: the kick and the bass line, not the hats.
    constexpr double pull = 0.0194;
    low1 += pull * ((l + r) * .5 - low1);
    low2 += pull * (low1 - low2);
    bucketBass += low2 * low2;
    if (++bucketFrames == framesPerBucket) flush();
  }
  if (whole < length) carry = QByteArray(bytes + whole, int(length - whole));
  return size;
}

AudioPeaks AudioPeakSink::finish() {
  flush();
  peaks.summarise();
  return std::move(peaks);
}

namespace {
struct Entry {
  std::shared_ptr<const AudioPeaks> peaks;
  bool pending = false, done = false;
  QDateTime modified;
  qint64 size = -1;
  std::vector<QPointer<QWidget>> waiting;
};
QHash<QString, Entry> &entries() {
  static QHash<QString, Entry> table;
  return table;
}
std::atomic<bool> &cancelled() {
  static std::atomic<bool> flag{false};
  return flag;
}
QThreadPool &decoders() {
  // Two at a time: a project opened with a dozen clips does not take every
  // core away from the preview while it catches up.
  static QThreadPool *pool = [] {
    auto *p = new QThreadPool;
    p->setMaxThreadCount(2);
    p->setExpiryTimeout(10000);
    return p;
  }();
  return *pool;
}
}  // namespace

bool audioPeaksPending(const QString &path) {
  const auto found = entries().constFind(path);
  return found != entries().cend() && found->pending;
}

void cancelAudioPeaks() {
  cancelled() = true;
  decoders().waitForDone(5000);
}

std::shared_ptr<const AudioPeaks> audioPeaks(const QString &path, QWidget *repaint) {
  if (path.isEmpty()) return nullptr;
  const QFileInfo info(path);
  if (!info.exists()) return nullptr;
  Entry &entry = entries()[path];
  // A file replaced on disk is decoded again.
  if (entry.done && (entry.modified != info.lastModified() || entry.size != info.size())) {
    entry.done = false;
    entry.peaks.reset();
  }
  if (entry.done) return entry.peaks;
  if (repaint && std::find(entry.waiting.begin(), entry.waiting.end(), repaint) == entry.waiting.end())
    entry.waiting.push_back(repaint);
  if (entry.pending) return nullptr;
  entry.pending = true;
  entry.modified = info.lastModified();
  entry.size = info.size();
  QPointer<QCoreApplication> app(QCoreApplication::instance());
  decoders().start([path, app] {
    std::shared_ptr<AudioPeaks> result;
    try {
      NativeMedia media;
      AudioPeakSink sink;
      if (media.decodeAudio(path, sink, [] { return cancelled().load(); }))
        result = std::make_shared<AudioPeaks>(sink.finish());
    } catch (...) {
      result.reset();
    }
    if (!app || cancelled()) return;
    QMetaObject::invokeMethod(app, [path, result] {
      auto found = entries().find(path);
      if (found == entries().end()) return;
      found->pending = false;
      found->done = true;
      found->peaks = result && !result->peak.empty() ? result : nullptr;
      const auto waiting = std::move(found->waiting);
      found->waiting.clear();
      for (const auto &widget : waiting)
        if (widget) widget->update();
    }, Qt::QueuedConnection);
  });
  return nullptr;
}


// Beat tracking after Ellis (2007): an onset strength from how fast the sound
// gets louder, a tempo from where that strength repeats itself, and then the
// set of beats that lands on strong onsets while keeping close to the tempo,
// found all at once by dynamic programming rather than one beat at a time, so
// a missed kick or an off-beat accent does not knock the rest off the grid.
namespace {
struct OnsetSignals {
  int n = 0;
  // What beats are tracked on: the rise in loudness, whole band and bass,
  // since a kick is what a beat is most often made of.
  std::vector<double> onset;
  // How sharp the sound is, from the whole band alone, where a snare counts
  // as much as a kick: for judging which tempo the beats really are at.
  std::vector<double> sharpness;
  const std::vector<float> *energy = nullptr;
  double meanEnergy = 0;
};
bool onsetSignals(const AudioPeaks &peaks, OnsetSignals &s) {
  const int rate = AudioPeaks::perSecond;
  const int n = int(std::min(peaks.energy.size(), peaks.bass.size()));
  if (n < rate * 4) return false;
  double meanEnergy = 0, meanBass = 0;
  for (int i = 0; i < n; ++i) { meanEnergy += peaks.energy[size_t(i)]; meanBass += peaks.bass[size_t(i)]; }
  meanEnergy /= n; meanBass /= n;
  if (meanEnergy <= 1e-9) return false;
  const double floorEnergy = meanEnergy * 1e-3 + 1e-12, floorBass = meanBass * 1e-3 + 1e-12;
  auto smoothed = [&](const std::vector<float> &v, int i) {
    const int a = std::max(0, i - 1), b = std::min(n - 1, i + 1);
    double sum = 0;
    for (int k = a; k <= b; ++k) sum += v[size_t(k)];
    return sum / (b - a + 1);
  };
  std::vector<double> whole(static_cast<size_t>(n)), low(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    whole[size_t(i)] = std::log(floorEnergy + smoothed(peaks.energy, i));
    low[size_t(i)] = std::log(floorBass + smoothed(peaks.bass, i));
  }
  s.n = n;
  s.onset.assign(size_t(n), 0.);
  s.sharpness.assign(size_t(n), 0.);
  for (int i = 2; i < n; ++i) {
    s.sharpness[size_t(i)] = std::max(0., whole[size_t(i)] - whole[size_t(i - 2)]);
    s.onset[size_t(i)] = s.sharpness[size_t(i)] + std::max(0., low[size_t(i)] - low[size_t(i - 2)]);
  }
  // Less its local average, so a loud chorus and a quiet verse count alike,
  // and in units of its own spread.
  auto normalise = [&](std::vector<double> &signal) {
    std::vector<double> prefix(size_t(n) + 1, 0.);
    for (int i = 0; i < n; ++i) prefix[size_t(i) + 1] = prefix[size_t(i)] + signal[size_t(i)];
    std::vector<double> local(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const int a = std::max(0, i - rate / 2), b = std::min(n, i + rate / 2 + 1);
      local[size_t(i)] = signal[size_t(i)] - (prefix[size_t(b)] - prefix[size_t(a)]) / (b - a);
    }
    double spread = 0;
    for (double v : local) spread += v * v;
    spread = std::sqrt(spread / n);
    if (spread <= 1e-12) return false;
    for (int i = 0; i < n; ++i) signal[size_t(i)] = local[size_t(i)] / spread;
    return true;
  };
  s.energy = &peaks.energy;
  s.meanEnergy = meanEnergy;
  return normalise(s.onset) && normalise(s.sharpness);
}
// The beat period, in levels, between two tempos: the lag at which the onsets
// line up with themselves best, leaning towards 120 BPM when `lean` is set.
double tempoPeriod(const OnsetSignals &s, double fastest, double slowest, bool lean) {
  const int rate = AudioPeaks::perSecond;
  const int shortest = std::max(3, int(std::floor(rate * 60 / fastest)));
  const int longest = int(std::ceil(rate * 60 / slowest));
  if (longest + 2 >= s.n || shortest >= longest) return 0;
  std::vector<double> score(size_t(longest) + 2, 0.);
  for (int lag = shortest - 1; lag <= longest + 1; ++lag) {
    double sum = 0;
    for (int i = lag; i < s.n; ++i) sum += s.onset[size_t(i)] * s.onset[size_t(i - lag)];
    const double octaves = std::log2(lag / (rate * .5));
    score[size_t(lag)] = sum / (s.n - lag) * (lean ? std::exp(-.5 * (octaves / 1.4) * (octaves / 1.4)) : 1.);
  }
  int best = shortest;
  for (int lag = shortest; lag <= longest; ++lag)
    if (score[size_t(lag)] > score[size_t(best)]) best = lag;
  if (score[size_t(best)] <= 0) return 0;
  // Between two lags, from the shape of the peak.
  const double a = score[size_t(best - 1)], b = score[size_t(best)], c = score[size_t(best + 1)];
  const double bend = a - 2 * b + c;
  return bend < 0 ? best + std::clamp(.5 * (a - c) / bend, -.5, .5) : best;
}
// The beats at a period: each one's score is its onset plus the best earlier
// beat's score, less a cost for straying from the period.
BeatAnalysis trackBeats(const OnsetSignals &s, double period) {
  BeatAnalysis result;
  const int rate = AudioPeaks::perSecond, n = s.n;
  if (period <= 2) return result;
  const double tightness = 400;
  std::vector<double> total(static_cast<size_t>(n));
  std::vector<int> previous(size_t(n), -1);
  const int reachBack = int(std::lround(2 * period)), reachNear = std::max(1, int(std::lround(period / 2)));
  for (int t = 0; t < n; ++t) {
    double chosen = -1e300;
    int from = -1;
    for (int p = std::max(0, t - reachBack); p <= t - reachNear; ++p) {
      const double stray = std::log((t - p) / period);
      const double v = total[size_t(p)] - tightness * stray * stray;
      if (v > chosen) { chosen = v; from = p; }
    }
    total[size_t(t)] = s.onset[size_t(t)] + (from >= 0 ? chosen : 0.);
    previous[size_t(t)] = from;
  }
  int last = n - 1;
  for (int t = std::max(0, n - int(period)); t < n; ++t)
    if (total[size_t(t)] > total[size_t(last)]) last = t;
  std::vector<int> found;
  for (int t = last; t >= 0; t = previous[size_t(t)]) found.push_back(t);
  std::reverse(found.begin(), found.end());
  // Not in silence: an intro before the music starts has no beat in it.
  const double quiet = s.meanEnergy * .01;
  for (int t : found) {
    const int a = std::max(0, t - rate / 4), b = std::min(n, t + rate / 4);
    double level = 0;
    for (int i = a; i < b; ++i) level = std::max(level, double((*s.energy)[size_t(i)]));
    if (level > quiet) {
      result.beats.push_back(double(t) / rate);
      result.strengths.push_back(s.sharpness[size_t(t)]);
    }
  }
  if (result.beats.size() < 4) return {};
  result.bpm = 60. * rate / period;
  return result;
}
// How well a set of beats fits the music, 0..1: how many of them land on a
// sharp sound, and how many of the sharpest sounds have a beat on them. Too
// slow a tempo lands well but leaves sounds between its beats; too fast a
// one covers everything but puts half its beats on nothing.
double beatFit(const OnsetSignals &s, const BeatAnalysis &beats) {
  if (beats.beats.empty()) return 0;
  const int rate = AudioPeaks::perSecond;
  auto around = [&](int t, int reach) {
    double most = -1e9;
    for (int i = std::max(0, t - reach); i <= std::min(s.n - 1, t + reach); ++i) most = std::max(most, s.sharpness[size_t(i)]);
    return most;
  };
  std::vector<int> strong;
  std::vector<double> heights;
  for (int i = 1; i + 1 < s.n; ++i)
    if (s.sharpness[size_t(i)] > 1 && around(i, 10) == s.sharpness[size_t(i)]) {
      strong.push_back(i);
      heights.push_back(s.sharpness[size_t(i)]);
    }
  if (strong.size() < 8) return 0;
  // The sharpest sounds, relative to this piece: the hats of a busy track
  // are not what its beat is made of.
  std::vector<double> sorted = heights;
  std::sort(sorted.begin(), sorted.end());
  const double bar = std::max(1.5, .5 * sorted[sorted.size() * 9 / 10]);
  std::vector<int> positions;
  for (double b : beats.beats) positions.push_back(int(std::lround(b * rate)));
  int landed = 0;
  for (int t : positions) landed += around(t, 2) >= 1;
  int covered = 0, counted = 0;
  for (size_t k = 0; k < strong.size(); ++k) {
    if (heights[k] < bar) continue;
    ++counted;
    const auto near = std::lower_bound(positions.begin(), positions.end(), strong[k] - 7);
    covered += near != positions.end() && *near <= strong[k] + 7;
  }
  if (counted == 0) return 0;
  const double precision = double(landed) / positions.size(), recall = double(covered) / counted;
  return precision + recall > 0 ? 2 * precision * recall / (precision + recall) : 0;
}
}  // namespace

BeatAnalysis detectBeats(const AudioPeaks &peaks, double tempoHint) {
  OnsetSignals s;
  if (!onsetSignals(peaks, s)) return {};
  if (tempoHint >= 30 && tempoHint <= 300) {
    // Asked for, a tempo is followed even where the music does not repeat
    // at it very clearly.
    double period = tempoPeriod(s, tempoHint * 1.06, tempoHint * .94, false);
    if (period <= 0) period = 60. * AudioPeaks::perSecond / tempoHint;
    BeatAnalysis result = trackBeats(s, period);
    if (!result.beats.empty()) result.tempos = {{result.bpm, beatFit(s, result)}};
    return result;
  }
  const double period = tempoPeriod(s, 200, 60, true);
  if (period <= 0) return {};
  // The strongest repetition is not always the beat: it can be every other
  // beat, or a syncopation every beat and a half. Each tempo it could be
  // related to is tracked and judged by how well its beats fit, with a lean
  // towards the tempos music is usually written in.
  const double found = 60. * AudioPeaks::perSecond / period;
  BeatAnalysis best;
  double bestScore = -1;
  std::vector<std::pair<double, double>> tempos;
  for (const double ratio : {1., 2., .5, 1.5, 2. / 3}) {
    const double bpm = found * ratio;
    if (ratio != 1 && (bpm < 60 || bpm > 180)) continue;
    BeatAnalysis candidate = ratio == 1 ? trackBeats(s, period)
                                        : trackBeats(s, tempoPeriod(s, bpm * 1.06, bpm * .94, false));
    if (candidate.beats.empty()) continue;
    const double octaves = std::log2(candidate.bpm / 110);
    const double score = beatFit(s, candidate) * std::exp(-.5 * octaves * octaves);
    tempos.push_back({candidate.bpm, score});
    if (score > bestScore) { bestScore = score; best = std::move(candidate); }
  }
  std::sort(tempos.begin(), tempos.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
  best.tempos = tempos;
  return best;
}
int checkWaveforms(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  // One second: half a second of silence, then a full-scale 440 Hz tone on the
  // left only, then a tone at a quarter of full scale.
  QByteArray pcm;
  auto frame = [&](double l, double r) {
    qint16 values[2] = {qint16(std::lround(l * 32767)), qint16(std::lround(r * 32767))};
    pcm.append(reinterpret_cast<const char *>(values), 4);
  };
  for (int i = 0; i < 24000; ++i) frame(0, 0);
  for (int i = 0; i < 12000; ++i) frame(std::sin(i * 2 * 3.14159265358979 * 440 / 48000), 0);
  for (int i = 0; i < 12000; ++i) {
    const double v = .25 * std::sin(i * 2 * 3.14159265358979 * 440 / 48000);
    frame(v, v);
  }
  AudioPeakSink sink;
  // Written in awkward pieces, as a decoder hands them over: not whole frames.
  for (qsizetype at = 0; at < pcm.size(); at += 1001) sink.write(pcm.mid(at, 1001));
  const AudioPeaks peaks = sink.finish();
  check(peaks.peak.size() == 200 && std::abs(peaks.seconds() - 1) < 1e-9,
        QString("one second of sound is two hundred levels (%1)").arg(peaks.peak.size()));
  const auto quiet = peaks.range(0, .45), loud = peaks.range(.55, .7), soft = peaks.range(.8, .95);
  check(quiet.first == 0 && quiet.second == 0, "silence reads as silence");
  check(loud.first > .98 && loud.second > .45 && loud.second < .55,
        QString("a full-scale tone on one side peaks at full scale, RMS about half (%1, %2)").arg(loud.first).arg(loud.second));
  check(soft.first > .23 && soft.first < .27 && soft.second > .15 && soft.second < .2,
        QString("a quieter tone reads quieter (%1, %2)").arg(soft.first).arg(soft.second));
  const auto wide = peaks.range(0, 1);
  check(peaks.coarsePeak.size() == 10 && wide.first > .98,
        "the coarse levels keep the loudest peak of what they cover");
  check(peaks.range(-3, -1).first == 0 && peaks.range(2, 3).first == 0,
        "outside the file there is nothing");
  // Beats. Music made to a known tempo after a second of silence: a kick on
  // every beat (or on 1 and 3 with a snare on 2 and 4), hats between, a pad
  // underneath. The beats found have to be the ones it was made with.
  for (const auto &[tempo, halfTime] : std::vector<std::pair<double, bool>>{{128., false}, {95., true}, {140., true}}) {
    const double period = 60. / tempo, start = 1.0, length = 26.0;
    AudioPeakSink music;
    quint32 noise = 12345;
    auto white = [&] { noise = noise * 1664525u + 1013904223u; return (noise >> 8) / double(1 << 24) * 2 - 1; };
    QByteArray block;
    for (int i = 0; i < int(length * 48000); ++i) {
      const double t = i / 48000.;
      double v = 0;
      if (t >= start) {
        const double inMusic = t - start;
        const int beat = int(inMusic / period);
        const double sinceBeat = inMusic - beat * period;
        const double sinceHalf = std::fmod(inMusic + period / 2, period);
        const bool kick = !halfTime || beat % 2 == 0;
        if (kick) v += .8 * std::sin(2 * 3.14159265358979 * 55 * sinceBeat) * std::exp(-sinceBeat / .12);
        else v += .45 * white() * std::exp(-sinceBeat / .08);                       // snare
        v += .18 * white() * std::exp(-sinceHalf / .025);                           // hats on the off-beat
        v += .08 * std::sin(2 * 3.14159265358979 * 220 * t) * (.6 + .4 * std::sin(t)); // pad
      }
      const qint16 s = qint16(std::lround(std::clamp(v, -1., 1.) * 32767));
      block.append(reinterpret_cast<const char *>(&s), 2);
      block.append(reinterpret_cast<const char *>(&s), 2);
      if (block.size() >= 65536) { music.write(block); block.clear(); }
    }
    music.write(block);
    const AudioPeaks levels = music.finish();
    QElapsedTimer clock; clock.start();
    const BeatAnalysis found = detectBeats(levels);
    const double took = clock.nsecsElapsed() / 1e6;
    int hits = 0, truth = 0;
    for (double b = start; b < length - .2; b += period) {
      ++truth;
      for (double f : found.beats) if (std::abs(f - b) <= .025) { ++hits; break; }
    }
    int stray = 0;
    for (double f : found.beats) {
      bool near = false;
      for (double b = start; b < length; b += period) near |= std::abs(f - b) <= .025;
      stray += !near;
    }
    QStringList considered;
    for (const auto &t : found.tempos) considered << QString("%1 at %2").arg(t.first, 0, 'f', 1).arg(t.second, 0, 'f', 2);
    check(std::abs(found.bpm - tempo) < 1,
          QString("%1 BPM music is found to be %2 BPM (considered %3)").arg(tempo).arg(found.bpm, 0, 'f', 2).arg(considered.join(", ")));
    check(hits >= truth * 9 / 10 && stray <= truth / 10,
          QString("%1 BPM: %2 of %3 beats found within 25 ms, %4 elsewhere (%5 ms to work out)")
              .arg(tempo).arg(hits).arg(truth).arg(stray).arg(took, 0, 'f', 1));
    check(!found.beats.empty() && found.beats.front() > start - .03,
          QString("%1 BPM: no beats in the silence before the music (first at %2 s)").arg(tempo)
              .arg(found.beats.empty() ? -1. : found.beats.front()));
  }
  {
    AudioPeakSink silence;
    silence.write(QByteArray(48000 * 4 * 6, 0));
    check(detectBeats(silence.finish()).beats.empty(), "silence has no beat");
  }
  {
    // Markers are kept with the project, in order, and are not part of any
    // picture: adding one must not throw away frames already rendered.
    Scene scene; scene.frames = 300;
    Layer box; box.type = "shape"; box.shapeKind = "rect"; box.duration = 300;
    scene.layers = {box};
    const auto before = preRenderSignature(scene, {});
    scene.markers = {{120, "Chorus", false}, {30, {}, true}, {60, {}, true}};
    sortMarkers(scene.markers);
    const auto after = preRenderSignature(scene, {});
    Scene reopened; QString error;
    const bool loaded = reopened.load(scene.json(), error);
    check(loaded && reopened.markers.size() == 3 && reopened.markers[0].frame == 30 && reopened.markers[0].beat &&
              reopened.markers[2].label == "Chorus" && !reopened.markers[2].beat,
          "markers are saved with the project and come back in order");
    check(before.global == after.global && before.layers == after.layers, "markers do not change what the cache knows frames by");
  }
  {
    // Pulse on Beats: a hit on every beat, settling before the next, riding on
    // the value the property already has.
    Layer title; title.type = "text"; title.start = 10; title.duration = 200;
    title.p[2].base = title.p[3].base = 1.;
    const std::vector<Marker> markers{{20, {}, true}, {40, {}, true}, {50, "Drop", false}, {60, {}, true}, {400, {}, true}};
    PulseSettings pulse;
    pulse.strength = .2; pulse.settle = 6;
    const int written = pulseOnMarkers(title, markers, pulse);
    const Property &scale = title.p[2];
    check(written > 0 && std::abs(scale.at(0) - 1) < 1e-9 && std::abs(scale.at(9) - 1) < 1e-9 &&
              std::abs(scale.at(10) - 1.2) < 1e-9 && std::abs(scale.at(16) - 1) < 1e-9 && std::abs(scale.at(29) - 1) < 1e-9 &&
              std::abs(scale.at(30) - 1.2) < 1e-9 && scale.at(12) > 1 && scale.at(12) < 1.2 &&
              std::abs(title.p[3].at(50) - 1.2) < 1e-9,
          "Pulse on Beats jumps on each beat in the layer's own time and settles before the next");
    check(std::abs(scale.at(40) - 1) < 1e-9, "a marker placed by hand is left out unless asked for");
    Layer kicked = title;
    PulseSettings kick; kick.move = 3; kick.strength = 5; kick.every = 2; kick.handMarkers = true;
    pulseOnMarkers(kicked, markers, kick);
    check(std::abs(kicked.p[4].at(10) - 5) < 1e-9 && std::abs(kicked.p[4].at(40) + 5) < 1e-9 && std::abs(kicked.p[4].at(30)) < 1e-9,
          "every other marker, kicking one way and then the other");
    Layer faded; faded.type = "shape"; faded.duration = 100; faded.p[5].base = 1;
    PulseSettings flash; flash.move = 1; flash.strength = .7;
    pulseOnMarkers(faded, markers, flash);
    check(std::abs(faded.p[5].at(20) - 1) < 1e-9 && std::abs(faded.p[5].at(30) - .3) < 1e-9,
          "a flash is full on the beat and low between");
  }
  {
    // Line Up in Time: one after another in the order given, a step apart or
    // on the markers; each layer moves whole.
    std::vector<Layer> layers(3);
    for (int i = 0; i < 3; ++i) { layers[size_t(i)].start = 50 * i; layers[size_t(i)].duration = 40; }
    layers[0].p[0].keys = {Key{0, 10}, Key{20, 90}};
    LineUpSettings apart; apart.from = 30; apart.step = 10;
    const int moved = lineUpInTime(layers, {2, 0, 1}, {}, apart);
    check(moved == 2 && layers[2].start == 30 && layers[0].start == 40 && layers[1].start == 50 &&
              layers[0].p[0].keys[1].frame == 20 && layers[0].duration == 40,
          "Line Up in Time starts the layers a step apart in the order given, each moving whole");
    LineUpSettings beats; beats.from = 25; beats.step = 10; beats.onMarkers = true;
    lineUpInTime(layers, {0, 1, 2}, {{20, {}, true}, {40, {}, true}, {60, "Drop", false}}, beats);
    check(layers[0].start == 40 && layers[1].start == 60 && layers[2].start == 70,
          "on the markers from where it starts, and a step apart once they run out");
  }
  {
    // Reverse Keyframes: the move played backwards in the same span, easing turned round.
    Property forward;
    forward.keys = {Key{0, 0}, Key{10, 100}, Key{30, 50}, Key{40, 0}};
    forward.keys[0].ease = "easeIn";
    forward.keys[1].ease = "linear";
    forward.keys[2].ease = "hold";
    Property back = forward;
    const bool turned = reverseKeys(back, 0, 2);
    bool mirrored = true;
    for (double t = 0; t <= 30; t += .5) mirrored = mirrored && std::abs(back.at(t) - forward.at(30 - t)) < 1e-6;
    check(turned && mirrored && back.keys[0].value == 50 && back.keys[1].frame == 20 && back.keys[1].ease == "easeOut" &&
              back.keys[2].frame == 30 && back.keys[2].ease == "hold" && back.keys[3].frame == 40,
          "Reverse Keyframes plays the picked keys backwards in their span, Ease In turned to Ease Out, the rest left alone");
    Property shaped;
    shaped.keys = {Key{0, 0}, Key{24, 200}};
    setSegment(shaped.keys[0], shaped.keys[1], QPointF(2, 150), QPointF(20, 210));
    Property shapedBack = shaped;
    reverseKeys(shapedBack, 0, 1);
    bool same = true;
    for (double t = 0; t <= 24; t += .25) same = same && std::abs(shapedBack.at(t) - shaped.at(24 - t)) < 1e-3;
    check(same && !reverseKeys(shapedBack, 1, 1), "a hand-shaped move comes out the same shape, backwards");
  }
  {
    // Lyric Motion: an .lrc read with its times, plain lines without, and each
    // line set as its layers.
    bool timed = false;
    const auto cues = parseLyricCues("[ti:Title]\n[00:01.00]first line\n[00:03.50]second line\n[00:06.00]\n", &timed);
    check(timed && cues.size() == 2 && std::abs(cues[0].start - 1) < 1e-9 && std::abs(cues[0].end - 3.42) < 1e-9 &&
              std::abs(cues[1].end - 5.92) < 1e-9 && cues[1].text == "second line",
          "Lyric Motion reads an .lrc: tags dropped, each line lasting until the next");
    bool again = false;
    const auto back = parseLyricCues(lyricCuesToLrc(cues), &again);
    check(again && back.size() == 2 && std::abs(back[1].start - 3.5) < 1e-6 && back[0].text == "first line",
          "and writes one that reads back the same");
    const auto plain = parseLyricCues("one\ntwo\n\nthree\n", &timed);
    check(!timed && plain.size() == 3, "plain lines are read without times");
    // Times kept apart from the words, read as the dialog reads a .times file:
    // minutes and seconds with or without brackets, or seconds alone, with blank
    // lines and "#" notes skipped. A byte-order mark at the head of the file (as
    // some editors save UTF-8) is no hindrance, whether the decoding drops it or
    // leaves it in the text.
    auto timesIn = [](const QString &file) {
      QStringList read;
      for (double t : parseLyricTimes(file)) read << QString::number(t);
      return read.join(' ');
    };
    const QString saved = timesIn(QString::fromUtf8("\xEF\xBB\xBF" "[01:23.45]\r\n1:23.45\r\n\r\n# the chorus\r\n83.45\r\n"));
    check(saved == "83.45 83.45 83.45",
          QString("a times file reads [01:23.45], 1:23.45 and 83.45 as one time, blank lines, notes and a byte-order mark "
                  "no hindrance (%1)").arg(saved));
    const QString kept = timesIn(QString(QChar(0xFEFF)) + "[01:23.45]\n1:23.45\n \n# the chorus\n83.45\n");
    check(kept == "83.45 83.45 83.45", QString("and the same with the byte-order mark left in the text (%1)").arg(kept));
    LyricMotionOptions quiet; quiet.style = 1;
    LyricMotionOptions loud; loud.style = 2;
    const auto written = lyricMotionLayers(cues, quiet), punched = lyricMotionLayers(cues, loud);
    bool inTime = true;
    for (const Layer &l : written) inTime = inTime && l.start >= 26 && l.start <= 106 && l.duration >= 20;
    check(written.size() == 10 && punched.size() == 4 && inTime && written.front().name.startsWith("Lyric 01"),
          QString("each line set as its layers, in time with the song (%1 written, %2 punched)").arg(written.size()).arg(punched.size()));
  }
  // A real file through the decoder: an AAC movie written and read back.
  {
    const QString path = QDir::temp().filePath(QString("wavy-flo-waveform-%1.mp4").arg(QCoreApplication::applicationPid()));
    try {
      NativeMedia writer;
      writer.beginMovie(path, 64, 64, 30, 400000, true);
      QByteArray tone;
      for (int i = 0; i < 48000; ++i) {
        const qint16 v = qint16(std::lround(.5 * 32767 * std::sin(i * 2 * 3.14159265358979 * 220 / 48000)));
        tone.append(reinterpret_cast<const char *>(&v), 2);
        tone.append(reinterpret_cast<const char *>(&v), 2);
      }
      QImage black(64, 64, QImage::Format_RGB32);
      black.fill(Qt::black);
      for (int f = 0; f < 30; ++f) writer.writeFrame(black, f, 30);
      writer.writeAudio(tone, 0);
      writer.finishMovie();
    } catch (...) {
    }
    // Nothing yet: it is being decoded. Asked again, it arrives by itself.
    const bool waited = audioPeaks(path) == nullptr;
    std::shared_ptr<const AudioPeaks> decoded;
    QElapsedTimer clock; clock.start();
    while (!decoded && clock.elapsed() < 20000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      decoded = audioPeaks(path);
    }
    const bool arrived = waited && decoded != nullptr;
    const auto level = decoded ? decoded->range(.2, .8) : std::pair<double, double>{0, 0};
    check(arrived && decoded && std::abs(decoded->seconds() - 1) < .1 && level.first > .4 && level.first < .6,
          QString("a sound file is decoded in the background (%1 s, peak %2)")
              .arg(decoded ? decoded->seconds() : -1).arg(level.first));
    check(audioPeaks(path) == decoded, "a file asked for again is not decoded again");
    QFile::remove(path);
  }
  return failures;
}
