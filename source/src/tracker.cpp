#include "tracker.h"
#include "native_media.h"
#include "render_workers.h"
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <functional>

// ---------------------------------------------------------------------------
// Track points as the layer keeps them.
// ---------------------------------------------------------------------------
QPointF TrackPoint::at(double frame) const {
  if (samples.empty()) return {};
  if (frame <= samples.front().frame) return samples.front().at;
  if (frame >= samples.back().frame) return samples.back().at;
  const auto after = std::lower_bound(samples.begin(), samples.end(), frame,
                                      [](const TrackSample &s, double f) { return s.frame < f; });
  const auto before = after - 1;
  const double span = after->frame - before->frame;
  const double t = span > 0 ? (frame - before->frame) / span : 0;
  return before->at + (after->at - before->at) * t;
}
const TrackSample *TrackPoint::sampleAt(double frame) const {
  const auto found = std::lower_bound(samples.begin(), samples.end(), frame - 1e-6,
                                      [](const TrackSample &s, double f) { return s.frame < f; });
  return found != samples.end() && std::abs(found->frame - frame) < 1e-6 ? &*found : nullptr;
}
void TrackPoint::set(const TrackSample &sample) {
  const auto found = std::lower_bound(samples.begin(), samples.end(), sample.frame - 1e-6,
                                      [](const TrackSample &s, double f) { return s.frame < f; });
  if (found != samples.end() && std::abs(found->frame - sample.frame) < 1e-6) *found = sample;
  else samples.insert(found, sample);
}
std::vector<TrackPoint> layerTracks(const Layer &l) {
  std::vector<TrackPoint> points;
  for (const auto &value : l.tracks) {
    const QJsonObject o = value.toObject();
    TrackPoint point;
    point.name = o.value("name").toString("Track Point");
    point.feature = std::clamp(o.value("feature").toInt(40), 8, 400);
    point.search = std::clamp(o.value("search").toInt(120), point.feature + 4, 1200);
    for (const auto &entry : o.value("samples").toArray()) {
      const QJsonArray s = entry.toArray();
      if (s.size() < 3) continue;
      point.samples.push_back({s[0].toDouble(), QPointF(s[1].toDouble(), s[2].toDouble()), s.size() > 3 ? s[3].toDouble(1) : 1.});
    }
    std::sort(point.samples.begin(), point.samples.end(), [](const TrackSample &a, const TrackSample &b) { return a.frame < b.frame; });
    points.push_back(std::move(point));
  }
  return points;
}
void setLayerTracks(Layer &l, const std::vector<TrackPoint> &points) {
  QJsonArray out;
  for (const auto &point : points) {
    QJsonArray samples;
    for (const auto &s : point.samples)
      samples.append(QJsonArray{s.frame, std::round(s.at.x() * 1000) / 1000, std::round(s.at.y() * 1000) / 1000,
                                std::round(s.confidence * 1000) / 1000});
    out.append(QJsonObject{{"name", point.name}, {"feature", point.feature}, {"search", point.search}, {"samples", samples}});
  }
  l.tracks = out;
}

// ---------------------------------------------------------------------------
// Matching.
// ---------------------------------------------------------------------------
Luma lumaOf(const QImage &source) {
  Luma out;
  if (source.isNull()) return out;
  const QImage image = source.format() == QImage::Format_RGB32 || source.format() == QImage::Format_ARGB32
                           ? source : source.convertToFormat(QImage::Format_RGB32);
  out.width = image.width();
  out.height = image.height();
  out.pixels.resize(size_t(out.width) * size_t(out.height));
  for (int y = 0; y < out.height; ++y) {
    const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    uint8_t *to = out.pixels.data() + size_t(y) * size_t(out.width);
    for (int x = 0; x < out.width; ++x) to[x] = uint8_t((77 * qRed(row[x]) + 150 * qGreen(row[x]) + 29 * qBlue(row[x])) >> 8);
  }
  return out;
}

namespace {
// Pixel (i, j) covers [i, i+1) x [j, j+1); its value sits at its centre.
double sampleLuma(const Luma &l, double x, double y) {
  const double u = std::clamp(x - .5, 0., l.width - 1.), v = std::clamp(y - .5, 0., l.height - 1.);
  const int x0 = int(u), y0 = int(v);
  const int x1 = std::min(x0 + 1, l.width - 1), y1 = std::min(y0 + 1, l.height - 1);
  const double fx = u - x0, fy = v - y0;
  const uint8_t *row0 = l.pixels.data() + size_t(y0) * size_t(l.width);
  const uint8_t *row1 = l.pixels.data() + size_t(y1) * size_t(l.width);
  const double top = row0[x0] + (row0[x1] - row0[x0]) * fx;
  const double bottom = row1[x0] + (row1[x1] - row1[x0]) * fx;
  return top + (bottom - top) * fy;
}
// The vertex of the parabola through three scores, as an offset from the middle.
double parabolaPeak(double left, double middle, double right) {
  const double curve = left - 2 * middle + right;
  if (curve >= -1e-12) return 0;
  return std::clamp((left - right) / (2 * curve), -.5, .5);
}
}  // namespace

PatchMatch matchPatch(const Luma &reference, QPointF from, int feature, const Luma &next, QPointF guess, int search) {
  PatchMatch result;
  if (reference.isNull() || next.isNull()) return result;
  const int half = std::max(2, feature / 2), side = 2 * half + 1;
  const int n = side * side;
  // The patch, taken at exactly where the point is, sub-pixel and all, with its
  // mean taken off so brightness changes between frames do not count.
  std::vector<float> cells(static_cast<size_t>(n));
  double mean = 0;
  for (int j = 0; j < side; ++j)
    for (int i = 0; i < side; ++i) {
      const double v = sampleLuma(reference, from.x() + (i - half), from.y() + (j - half));
      cells[size_t(j * side + i)] = float(v);
      mean += v;
    }
  mean /= n;
  double energy = 0;
  for (auto &v : cells) { v = float(v - mean); energy += double(v) * v; }
  // A flat patch matches everything equally: nothing to follow.
  if (energy < n * 4.) return result;
  // Where its centre may be: whole pixels around the guess, the window kept
  // inside the frame.
  const int reach = std::max(2, (search - side) / 2 + 1);
  const int gx = int(std::floor(guess.x())), gy = int(std::floor(guess.y()));
  const int left = std::max(half, gx - reach), right = std::min(next.width - 1 - half, gx + reach);
  const int top = std::max(half, gy - reach), bottom = std::min(next.height - 1 - half, gy + reach);
  if (left > right || top > bottom) return result;
  const int columns = right - left + 1, rows = bottom - top + 1;
  // Sums of the frame and of its square over the searched area, so each
  // window's mean and spread cost four look-ups.
  const int ax = left - half, ay = top - half, aw = columns + side - 1, ah = rows + side - 1;
  std::vector<double> sum(size_t(aw + 1) * size_t(ah + 1), 0.), square(sum.size(), 0.);
  for (int y = 0; y < ah; ++y) {
    const uint8_t *row = next.pixels.data() + size_t(ay + y) * size_t(next.width) + ax;
    double runSum = 0, runSquare = 0;
    for (int x = 0; x < aw; ++x) {
      runSum += row[x];
      runSquare += double(row[x]) * row[x];
      const size_t at = size_t(y + 1) * size_t(aw + 1) + size_t(x + 1), above = size_t(y) * size_t(aw + 1) + size_t(x + 1);
      sum[at] = sum[above] + runSum;
      square[at] = square[above] + runSquare;
    }
  }
  auto boxSum = [&](const std::vector<double> &table, int x, int y) {
    const size_t w = size_t(aw + 1);
    return table[size_t(y + side) * w + size_t(x + side)] - table[size_t(y) * w + size_t(x + side)] -
           table[size_t(y + side) * w + size_t(x)] + table[size_t(y) * w + size_t(x)];
  };
  std::vector<double> scores(size_t(columns) * size_t(rows), -1.);
  renderParallelEach(rows, [&](int r) {
    for (int c = 0; c < columns; ++c) {
      const int wx = c, wy = r; // window corner within the searched area
      double cross = 0;
      for (int j = 0; j < side; ++j) {
        const uint8_t *row = next.pixels.data() + size_t(ay + wy + j) * size_t(next.width) + size_t(ax + wx);
        const float *p = cells.data() + size_t(j * side);
        float line = 0;
        for (int i = 0; i < side; ++i) line += p[i] * float(row[i]);
        cross += line;
      }
      const double s = boxSum(sum, wx, wy), s2 = boxSum(square, wx, wy);
      const double spread = s2 - s * s / n;
      scores[size_t(r) * size_t(columns) + size_t(c)] = spread > 1e-6 ? cross / std::sqrt(energy * spread) : 0.;
    }
  });
  size_t best = 0;
  for (size_t i = 1; i < scores.size(); ++i)
    if (scores[i] > scores[best]) best = i;
  const int bc = int(best % size_t(columns)), br = int(best / size_t(columns));
  auto score = [&](int c, int r) { return scores[size_t(r) * size_t(columns) + size_t(c)]; };
  const double peak = scores[best];
  const double dx = bc > 0 && bc + 1 < columns ? parabolaPeak(score(bc - 1, br), peak, score(bc + 1, br)) : 0;
  const double dy = br > 0 && br + 1 < rows ? parabolaPeak(score(bc, br - 1), peak, score(bc, br + 1)) : 0;
  result.at = QPointF(left + bc + .5 + dx, top + br + .5 + dy);
  result.score = std::clamp(peak, -1., 1.);
  result.found = true;
  // The parabola through the correlation peak is only as sharp as the peak: on
  // smooth ground -- sky, a wall, skin -- the peak is broad and the parabola
  // misses by half a pixel. From there, slide the patch down the gradient of
  // the difference (Lucas-Kanade), its contrast matched to the patch at each
  // step so a change of light does not pull it: a few steps settle it to a
  // few hundredths of a pixel on any texture.
  const double patchSpread = std::sqrt(energy / n);
  std::vector<double> seen(static_cast<size_t>(n)), slopeX(static_cast<size_t>(n)), slopeY(static_cast<size_t>(n));
  QPointF refined = result.at;
  for (int step = 0; step < 8; ++step) {
    double mean = 0;
    for (int j = 0, k = 0; j < side; ++j)
      for (int i = 0; i < side; ++i, ++k) {
        const double x = refined.x() + (i - half), y = refined.y() + (j - half);
        seen[size_t(k)] = sampleLuma(next, x, y);
        slopeX[size_t(k)] = sampleLuma(next, x + .5, y) - sampleLuma(next, x - .5, y);
        slopeY[size_t(k)] = sampleLuma(next, x, y + .5) - sampleLuma(next, x, y - .5);
        mean += seen[size_t(k)];
      }
    mean /= n;
    double spread = 0;
    for (double v : seen) spread += (v - mean) * (v - mean);
    spread = std::sqrt(spread / n);
    if (spread < 1e-6) break;
    const double gain = patchSpread / spread;
    double h11 = 0, h12 = 0, h22 = 0, b1 = 0, b2 = 0;
    for (size_t k = 0; k < size_t(n); ++k) {
      const double ex = gain * slopeX[k], ey = gain * slopeY[k];
      const double error = gain * (seen[k] - mean) - cells[k];
      h11 += ex * ex; h12 += ex * ey; h22 += ey * ey;
      b1 += ex * error; b2 += ey * error;
    }
    const double det = h11 * h22 - h12 * h12;
    if (det <= 1e-9 * (h11 + h22) * (h11 + h22) || det <= 0) break;
    const QPointF move(-(h22 * b1 - h12 * b2) / det, -(h11 * b2 - h12 * b1) / det);
    refined += move;
    if (std::hypot(move.x(), move.y()) < .002) break;
  }
  // Kept only when it stayed near the peak it started from: a step that ran
  // off along an edge is not a better answer.
  if (QLineF(refined, result.at).length() < 1) result.at = refined;
  return result;
}

// ---------------------------------------------------------------------------
// Tracking through footage.
// ---------------------------------------------------------------------------
namespace {
qint64 sourceTime(const TrackJob &job, double frame) {
  return qint64(std::llround((job.sourceOffsetMs + frame * 1000. / std::max(1, job.fps)) * 10000.));
}
// Decodes the pictures on screen at `times` (ascending, 100 ns) in one pass
// and hands each over in grey. A picture stays on screen until the next one
// starts, so a frame between two gets the earlier.
bool collectFrames(NativeMedia &media, const QString &path, const std::vector<qint64> &times, TrackJob &job,
                   const std::function<bool(size_t, Luma &&)> &use) {
  if (times.empty()) return true;
  size_t next = 0;
  QImage held;
  bool stopped = false;
  // Half a millisecond either way: containers round their timestamps.
  constexpr qint64 slack = 5000;
  const bool opened = media.readVideo(path, times.front(), [&](qint64 timestamp, const QImage &picture) {
    if (job.cancel) { stopped = true; return false; }
    while (next < times.size() && times[next] + slack < timestamp) {
      if (!use(next, lumaOf(held.isNull() ? picture : held))) { stopped = true; return false; }
      ++next;
    }
    held = picture;
    return next < times.size();
  });
  if (!opened) throw std::runtime_error("The footage has no video the tracker can read");
  // The footage ran out first: what was last on screen stays there.
  while (!stopped && next < times.size() && !held.isNull()) {
    if (!use(next, lumaOf(held))) return false;
    ++next;
  }
  return !stopped;
}
}  // namespace

void runTrack(TrackJob &job) {
  try {
    const int direction = job.toFrame >= job.fromFrame ? 1 : -1;
    std::vector<double> frames;
    for (double f = std::round(job.fromFrame); direction > 0 ? f <= job.toFrame + 1e-9 : f >= job.toFrame - 1e-9; f += direction) {
      if (f < 0) break;
      if (job.sourceDurationMs > 0 && job.sourceOffsetMs + f * 1000. / std::max(1, job.fps) >= job.sourceDurationMs) break;
      frames.push_back(f);
    }
    if (frames.size() >= 2) {
      NativeMedia media;
      Luma reference, previous;
      QPointF referenceAt, previousAt, velocity;
      bool first = true;
      auto step = [&](double frame, Luma &&luma) -> bool {
        if (job.cancel) return false;
        const QPointF centre(luma.width / 2., luma.height / 2.);
        if (first) {
          // The frame the point was put on: what it looks like there is what
          // every later match is held to.
          referenceAt = previousAt = job.start + centre;
          reference = luma;
          previous = std::move(luma);
          first = false;
          return true;
        }
        // Where it went from the last frame, looked for where its speed says.
        PatchMatch found = matchPatch(previous, previousAt, job.feature, luma, previousAt + velocity, job.search);
        // Matching each frame against the one before follows a patch that slowly
        // changes, but every match adds a sliver of error. Where the patch as it
        // was first placed still matches closely, that is where it is: the
        // point stays on the same spot for hundreds of frames.
        if (found.found) {
          const PatchMatch held = matchPatch(reference, referenceAt, job.feature, luma, found.at, job.feature + 7);
          if (held.found && held.score > .85 && QLineF(held.at, found.at).length() < 3) {
            found.at = held.at;
            found.score = std::max(found.score, held.score);
          }
        }
        if (!found.found || found.score < job.lostBelow) {
          std::lock_guard<std::mutex> lock(job.guard);
          job.lostAt = frame;
          job.error = QString("Lost the point at frame %1 (match %2). Move it back onto the feature and track on from there.")
                          .arg(frame).arg(found.found ? found.score : 0., 0, 'f', 2);
          return false;
        }
        velocity = found.at - previousAt;
        previousAt = found.at;
        previous = std::move(luma);
        std::lock_guard<std::mutex> lock(job.guard);
        job.samples.push_back({frame, found.at - centre, std::clamp(found.score, 0., 1.)});
        return true;
      };
      if (direction > 0) {
        std::vector<qint64> times;
        for (double f : frames) times.push_back(sourceTime(job, f));
        collectFrames(media, job.path, times, job, [&](size_t i, Luma &&luma) { return step(frames[i], std::move(luma)); });
      } else {
        // Footage only decodes forwards: back in pieces, each decoded forwards
        // and then walked backwards.
        constexpr size_t piece = 24;
        for (size_t i = 0; i < frames.size() && !job.cancel;) {
          const size_t end = std::min(frames.size(), i + piece); // frames[i..end) run downwards
          std::vector<qint64> times;
          for (size_t k = end; k-- > i;) times.push_back(sourceTime(job, frames[k]));
          std::vector<Luma> pictures(end - i);
          const bool whole = collectFrames(media, job.path, times, job, [&](size_t at, Luma &&luma) {
            pictures[pictures.size() - 1 - at] = std::move(luma);
            return true;
          });
          if (!whole) break;
          bool going = true;
          for (size_t k = i; k < end && going; ++k) going = step(frames[k], std::move(pictures[k - i]));
          if (!going) break;
          i = end;
        }
      }
    }
  } catch (const std::exception &e) {
    std::lock_guard<std::mutex> lock(job.guard);
    job.error = QString::fromLocal8Bit(e.what());
  }
  job.done = true;
}

// ---------------------------------------------------------------------------
// Using a track.
// ---------------------------------------------------------------------------
namespace {
// Where a point of the layer lands in the composition, the Main Transform's
// scale and turn included.
QPointF compositionPoint(const Layer &l, const Scene &s, double compFrame, QPointF local) {
  QTransform t = layerWorldTransform(l, s, compFrame);
  if (layerHasMainTransform(l)) t = layerMainTransform(l) * t;
  return t.map(local);
}
// The same point in the space a layer's Position is written in.
QPointF inParentSpace(const Layer &l, const Scene &s, double compFrame, QPointF point) {
  bool invertible = false;
  const QTransform back = layerParentTransform(l, s, compFrame).inverted(&invertible);
  return invertible ? back.map(point) : point;
}
double clampToTrack(const TrackPoint &point, double layerFrame) {
  return std::clamp(layerFrame, point.samples.front().frame, point.samples.back().frame);
}
}  // namespace

int applyTrackToLayer(Scene &s, int tracked, const TrackPoint &point, int target, double referenceFrame) {
  if (tracked < 0 || target < 0 || tracked >= int(s.layers.size()) || target >= int(s.layers.size()) || tracked == target ||
      point.samples.empty())
    return 0;
  const Layer video = s.layers[size_t(tracked)];
  Layer &layer = s.layers[size_t(target)];
  const double reference = clampToTrack(point, referenceFrame - video.start);
  const double referenceComp = video.start + reference;
  // Where the layer sits now against the point: kept all the way along.
  const QPointF now(layer.p[0].at(referenceComp - layer.start), layer.p[1].at(referenceComp - layer.start));
  const QPointF offset = now - inParentSpace(layer, s, referenceComp, compositionPoint(video, s, referenceComp, point.at(reference)));
  std::vector<std::pair<double, QPointF>> keys;
  for (const auto &sample : point.samples) {
    const double comp = video.start + sample.frame;
    keys.push_back({comp - layer.start, inParentSpace(layer, s, comp, compositionPoint(video, s, comp, sample.at)) + offset});
  }
  layer.p[0].keys.clear();
  layer.p[1].keys.clear();
  for (const auto &[frame, at] : keys) {
    layer.p[0].set(frame, at.x());
    layer.p[1].set(frame, at.y());
  }
  return int(keys.size());
}

int stabilizeWithTrack(Scene &s, int tracked, const TrackPoint &point, double referenceFrame) {
  if (tracked < 0 || tracked >= int(s.layers.size()) || point.samples.empty()) return 0;
  Layer &video = s.layers[size_t(tracked)];
  if (video.threeD) return 0;
  const double reference = clampToTrack(point, referenceFrame - video.start);
  const double referenceComp = video.start + reference;
  // Where the point is on screen now, in the space Position is written in.
  const QPointF pinned = inParentSpace(video, s, referenceComp, compositionPoint(video, s, referenceComp, point.at(reference)));
  // The anchor rides on the point and Position holds still: whatever the
  // layer's scale and turn, the anchor is where Position puts it.
  const QTransform main = layerHasMainTransform(video) ? layerMainTransform(video) : QTransform();
  for (int p : {0, 1, int(AnchorX), int(AnchorY)}) video.p[size_t(p)].keys.clear();
  for (const auto &sample : point.samples) {
    const QPointF anchor = main.map(sample.at);
    video.p[AnchorX].set(sample.frame, anchor.x());
    video.p[AnchorY].set(sample.frame, anchor.y());
  }
  video.p[0].base = pinned.x() - video.mainX;
  video.p[1].base = pinned.y() - video.mainY;
  return int(point.samples.size());
}
