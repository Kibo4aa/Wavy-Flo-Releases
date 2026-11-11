#include "particles.h"
#include "render_workers.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QMutex>
#include <QRadialGradient>
#include <QSet>
#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <future>
#include <unordered_map>

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Defaults { const char *key; double value; };
// The settings that are not keyframed, with what each starts as.
const Defaults defaults[] = {
    {"emitter", 0},        // 0 point, 1 line, 2 box, 3 disc, 4 ring, 5 the layer's own area
    {"emitterWidth", 200}, {"emitterHeight", 200},
    {"shape", 0},          // 0 dot, 1 circle, 2 square, 3 streak, 4 petal, 5 star, 6 bokeh, 7 confetti
    {"colorVariance", 0},  // % of the hue circle, either way
    {"sizeVariance", 30}, {"speedVariance", 30}, {"lifeVariance", 20},
    {"sizeEnd", 100},      // % of the size a particle is born with, at the end of its life
    {"fadeIn", 10}, {"fadeOut", 30}, // % of its life
    {"opacity", 100}, {"opacityVariance", 0},
    {"drag", 0},           // how quickly speed dies away, per second
    {"turbulenceFrequency", 1}, // wobbles per second
    {"burst", 0},          // born all at once at the start
    {"preroll", 0},        // seconds already run before the layer starts
    {"seed", 1}, {"maxParticles", 5000},
    {"space", 0},          // 0 world: a moving emitter leaves a trail; 1 layer: they move with it
    {"randomRotation", 1}, {"blend", 0}, // 0 normal, 1 add
    // In depth, for a 3D layer: how far apart they are born front to back, and
    // how fast they travel away from the camera (towards it when negative).
    {"depth", 0}, {"speedZ", 0}
};

uint32_t mixHash(uint32_t h) {
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
  return h;
}
// A number 0..1 for particle `index`, draw `k`: the same on every render.
double random01(uint32_t seed, int64_t index, int k) {
  const uint32_t h = mixHash(seed * 0x9e3779b9u ^ uint32_t(index) * 0x85ebca6bu ^ uint32_t(uint64_t(index) >> 32) * 0x27d4eb2fu ^
                             uint32_t(k) * 0xc2b2ae35u);
  return double(h & 0xffffffu) / 16777216.;
}
double signedRandom(uint32_t seed, int64_t index, int k) { return random01(seed, index, k) * 2 - 1; }
// Smooth noise -1..1 along one axis, the same stream for the same stream number.
double smoothNoise(uint32_t stream, double x) {
  const double floorX = std::floor(x);
  const int64_t i = int64_t(floorX);
  const double f = x - floorX, s = f * f * (3 - 2 * f);
  const double a = random01(stream, i, 91) * 2 - 1, b = random01(stream, i + 1, 91) * 2 - 1;
  return a + (b - a) * s;
}
QColor lerpColor(const QColor &a, const QColor &b, double t) {
  return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                          float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

struct Settings {
  int emitter, shape, space, blend;
  double emitterWidth, emitterHeight, colorVariance, sizeVariance, speedVariance, lifeVariance, sizeEnd, fadeIn, fadeOut,
      opacity, opacityVariance, drag, turbulenceFrequency, preroll, depth, speedZ;
  int burst, maxParticles;
  uint32_t seed;
  bool randomRotation;
  QColor colorStart, colorEnd;
};
Settings settingsOf(const Layer &l) {
  Settings s;
  s.emitter = std::clamp(int(particleSetting(l, "emitter")), 0, 5);
  s.shape = std::clamp(int(particleSetting(l, "shape")), 0, 7);
  s.space = std::clamp(int(particleSetting(l, "space")), 0, 1);
  s.blend = std::clamp(int(particleSetting(l, "blend")), 0, 1);
  s.emitterWidth = std::max(0., particleSetting(l, "emitterWidth"));
  s.emitterHeight = std::max(0., particleSetting(l, "emitterHeight"));
  s.colorVariance = std::clamp(particleSetting(l, "colorVariance"), 0., 100.) / 100.;
  s.sizeVariance = std::clamp(particleSetting(l, "sizeVariance"), 0., 100.) / 100.;
  s.speedVariance = std::clamp(particleSetting(l, "speedVariance"), 0., 100.) / 100.;
  s.lifeVariance = std::clamp(particleSetting(l, "lifeVariance"), 0., 90.) / 100.;
  s.sizeEnd = std::clamp(particleSetting(l, "sizeEnd"), 0., 1000.) / 100.;
  s.fadeIn = std::clamp(particleSetting(l, "fadeIn"), 0., 100.) / 100.;
  s.fadeOut = std::clamp(particleSetting(l, "fadeOut"), 0., 100.) / 100.;
  s.opacity = std::clamp(particleSetting(l, "opacity"), 0., 100.) / 100.;
  s.opacityVariance = std::clamp(particleSetting(l, "opacityVariance"), 0., 100.) / 100.;
  s.drag = std::clamp(particleSetting(l, "drag"), 0., 20.);
  s.turbulenceFrequency = std::clamp(particleSetting(l, "turbulenceFrequency"), 0., 20.);
  s.preroll = std::clamp(particleSetting(l, "preroll"), 0., 30.);
  s.burst = std::clamp(int(particleSetting(l, "burst")), 0, 20000);
  s.maxParticles = std::clamp(int(particleSetting(l, "maxParticles")), 1, 20000);
  s.seed = uint32_t(int64_t(particleSetting(l, "seed")));
  s.randomRotation = particleSetting(l, "randomRotation") >= .5;
  s.depth = std::clamp(particleSetting(l, "depth"), 0., 100000.);
  s.speedZ = std::clamp(particleSetting(l, "speedZ"), -100000., 100000.);
  s.colorStart = particleColor(l, "colorStart");
  s.colorEnd = particleColor(l, "colorEnd");
  return s;
}

// How many particles have been born by simulated time tau, and when the n-th
// one was: straight division when the rate never changes, a table otherwise.
struct Emission {
  bool steady = true;
  double rate = 0;
  double step = 0;
  std::vector<double> table; // count born by k * step
  double born(double tau) const {
    if (tau <= 0) return 0;
    if (steady) return rate * tau;
    const double at = tau / step;
    const size_t k = size_t(at);
    if (k + 1 >= table.size()) return table.empty() ? 0 : table.back();
    return table[k] + (table[k + 1] - table[k]) * (at - double(k));
  }
  double when(double n) const {
    if (steady) return rate > 0 ? n / rate : 1e300;
    if (table.empty() || n > table.back()) return 1e300;
    const auto found = std::lower_bound(table.begin(), table.end(), n);
    const size_t k = size_t(std::max<std::ptrdiff_t>(1, found - table.begin()));
    const double a = table[k - 1], b = table[k];
    const double f = b > a ? (n - a) / (b - a) : 0;
    return (double(k - 1) + f) * step;
  }
};
}  // namespace

double particleSetting(const Layer &l, const char *key) {
  for (const auto &d : defaults)
    if (qstrcmp(d.key, key) == 0) {
      const QJsonValue v = l.particles.value(QLatin1String(key));
      if (v.isBool()) return v.toBool() ? 1 : 0;
      const double value = v.toDouble(d.value);
      return std::isfinite(value) ? value : d.value;
    }
  return 0;
}
QColor particleColor(const Layer &l, const char *key) {
  const QColor stored(l.particles.value(QLatin1String(key)).toString());
  return stored.isValid() ? stored : QColor(Qt::white);
}

namespace {
std::shared_ptr<const std::vector<ParticleState>> simulate(const Layer &l, const Scene &s, double frame) {
  auto result = std::make_shared<std::vector<ParticleState>>();
  if (!layerEmitsParticles(l)) return result;
  const Settings set = settingsOf(l);
  const double fps = std::max(1, s.fps);
  const double tau = frame / fps + set.preroll;
  if (tau < 0) return result;
  // The layer frame a moment of simulated time corresponds to; the pre-roll
  // runs on the layer's first values.
  auto frameAt = [&](double time) { return std::max(0., (time - set.preroll) * fps); };
  const Property &rate = l.p[ParticleRate];
  Emission emission;
  if (!rate.varies()) {
    emission.rate = std::max(0., rate.at(0));
  } else {
    emission.steady = false;
    emission.step = 1. / (fps * 4);
    const size_t steps = size_t(std::ceil(tau / emission.step)) + 2;
    // The count born so far is the same curve on every frame: built once for
    // the whole layer and kept, rather than summed again from the start on each
    // frame -- minutes into a song that was tens of thousands of looks at the
    // rate's keys per frame, per layer, before a single particle was placed.
    QByteArray key;
    {
      QDataStream stream(&key, QIODevice::WriteOnly);
      stream << rate.base << qint64(rate.keys.size()) << fps << set.preroll;
      for (const auto &k : rate.keys) {
        stream << k.frame << k.value << k.ease << k.custom << k.absolute << k.continuous;
        for (double control : k.curve) stream << control;
      }
    }
    static QMutex tablesGuard;
    static std::deque<std::pair<QByteArray, std::shared_ptr<const std::vector<double>>>> tables;
    std::shared_ptr<const std::vector<double>> table;
    // An expression can look at anything, so its curve is summed afresh.
    const bool keep = !rate.hasExpression();
    if (keep) {
      QMutexLocker lock(&tablesGuard);
      for (const auto &kept : tables)
        if (kept.first == key && kept.second->size() >= steps + 1) { table = kept.second; break; }
    }
    if (!table) {
      // As far as the layer goes (and a little more), so later frames find it.
      const size_t whole = !keep ? steps : std::max(steps, size_t(std::ceil((l.duration / fps + set.preroll + 1) / emission.step)) + 2);
      auto made = std::make_shared<std::vector<double>>();
      made->reserve(whole + 1);
      made->push_back(0);
      for (size_t k = 0; k < whole; ++k)
        made->push_back(made->back() + std::max(0., rate.at(frameAt((double(k) + .5) * emission.step))) * emission.step);
      table = made;
      if (keep) {
        QMutexLocker lock(&tablesGuard);
        tables.push_back({key, table});
        if (tables.size() > 16) tables.pop_front();
      }
    }
    emission.table.assign(table->begin(), table->begin() + std::ptrdiff_t(steps + 1));
  }
  // The longest any particle can live, so the ones long gone are never looked at.
  const Property &lifeProperty = l.p[ParticleLife];
  double longest = std::max(lifeProperty.at(frameAt(tau)), lifeProperty.at(0));
  for (const auto &key : lifeProperty.keys) longest = std::max(longest, key.value);
  if (lifeProperty.hasExpression())
    for (int k = 0; k <= 32; ++k) longest = std::max(longest, lifeProperty.at(frameAt(tau * k / 32.)));
  longest = std::max(.05, longest) * (1 + set.lifeVariance) + 1e-6;

  // Who could be alive: the burst, then the steady stream, newest last.
  std::vector<std::pair<int64_t, double>> candidates; // index, birth time
  if (set.burst > 0 && tau < longest)
    for (int i = 0; i < set.burst; ++i) candidates.push_back({-1 - int64_t(i), 0.});
  const double first = std::ceil(emission.born(std::max(0., tau - longest)) - 1e-9);
  const double last = std::floor(emission.born(tau) - 1e-9);
  if (last >= first && std::isfinite(last)) {
    const int64_t from = std::max<int64_t>(int64_t(first), int64_t(last) - set.maxParticles + 1);
    for (int64_t i = from; i <= int64_t(last); ++i) candidates.push_back({i, emission.when(double(i))});
  }
  if (int(candidates.size()) > set.maxParticles)
    candidates.erase(candidates.begin(), candidates.end() - set.maxParticles);

  // Space. In world space a particle is born where the layer was at that
  // moment and then goes its own way, so a moving emitter leaves a trail.
  bool world = set.space == 0 && !l.threeD;
  QTransform now, inverse;
  if (world) {
    now = layerWorldTransform(l, s, l.start + frame);
    bool invertible = false;
    inverse = now.inverted(&invertible);
    world = invertible && now.isAffine();
  }
  const double worldScale = world ? std::sqrt(std::abs(now.m11() * now.m22() - now.m12() * now.m21())) : 1;
  std::unordered_map<int64_t, QTransform> birthTransforms;
  auto birthTransform = [&](double layerFrame) {
    const int64_t key = int64_t(std::llround(layerFrame * 4));
    auto found = birthTransforms.find(key);
    if (found != birthTransforms.end()) return found->second;
    const QTransform t = layerWorldTransform(l, s, l.start + double(key) / 4.);
    birthTransforms.emplace(key, t);
    return t;
  };

  // The layer's own area, for particles born over it.
  const QRectF ownArea = set.emitter == 5 ? layerOwnBounds(l, s) : QRectF();
  result->reserve(candidates.size());
  for (const auto &[index, birth] : candidates) {
    const double age = tau - birth;
    if (age < 0) continue;
    const double bornFrame = frameAt(birth);
    const double life = std::max(.05, l.p[ParticleLife].at(bornFrame) * (1 + signedRandom(set.seed, index, 1) * set.lifeVariance));
    if (age >= life) continue;
    const double t01 = age / life;
    // Where on the emitter it was born.
    QPointF offset;
    const double a = random01(set.seed, index, 2), b = random01(set.seed, index, 3);
    switch (set.emitter) {
      case 1: offset = {(a - .5) * set.emitterWidth, 0}; break;
      case 2: offset = {(a - .5) * set.emitterWidth, (b - .5) * set.emitterHeight}; break;
      case 3: {
        const double r = std::sqrt(a), angle = b * 2 * kPi;
        offset = {std::cos(angle) * r * set.emitterWidth / 2, std::sin(angle) * r * set.emitterHeight / 2};
        break;
      }
      case 4: {
        const double angle = b * 2 * kPi;
        offset = {std::cos(angle) * set.emitterWidth / 2, std::sin(angle) * set.emitterHeight / 2};
        break;
      }
      case 5: offset = {ownArea.left() + a * ownArea.width(), ownArea.top() + b * ownArea.height()}; break;
      default: break;
    }
    // How it left, and what pulls on it: all as they were at its birth.
    const double speed = std::max(0., l.p[ParticleSpeed].at(bornFrame) * (1 + signedRandom(set.seed, index, 4) * set.speedVariance));
    const double direction = (l.p[ParticleDirection].at(bornFrame) +
                              signedRandom(set.seed, index, 5) * l.p[ParticleSpread].at(bornFrame) / 2) * kPi / 180;
    const QPointF v0(std::sin(direction) * speed, -std::cos(direction) * speed); // 0 degrees is straight up
    const QPointF pull(l.p[ParticleWind].at(bornFrame), l.p[ParticleGravity].at(bornFrame));
    QPointF travel, velocity;
    if (set.drag > 1e-6) {
      const double k = set.drag, decay = std::exp(-k * age);
      const QPointF terminal = pull / k;
      travel = terminal * age + (v0 - terminal) * ((1 - decay) / k);
      velocity = terminal + (v0 - terminal) * decay;
    } else {
      travel = v0 * age + pull * (.5 * age * age);
      velocity = v0 + pull * age;
    }
    // A wobble on top, growing in over the first moments of its life.
    const double turbulence = l.p[ParticleTurbulence].at(bornFrame);
    if (turbulence != 0 && set.turbulenceFrequency > 0) {
      const double phase = age * set.turbulenceFrequency;
      const double grow = std::min(1., phase);
      const uint32_t stream = mixHash(set.seed ^ uint32_t(index) * 0x9e3779b9u);
      travel += QPointF(smoothNoise(stream, phase + 17.3), smoothNoise(stream ^ 0x5bd1e995u, phase + 3.1)) * (turbulence * grow);
    }
    ParticleState state;
    // Depth, for a 3D layer that asks for it: born somewhere in the Depth
    // front to back, carried by Z Speed (slowed by drag like the rest), and
    // wobbled by the turbulence the same way it wobbles them sideways.
    if (set.depth > 0 || set.speedZ != 0) {
      const double vz = set.speedZ * (1 + signedRandom(set.seed, index, 15) * set.speedVariance);
      double z = signedRandom(set.seed, index, 14) * set.depth / 2;
      if (set.drag > 1e-6) {
        const double decay = std::exp(-set.drag * age);
        z += vz * (1 - decay) / set.drag;
        state.velocityZ = vz * decay;
      } else {
        z += vz * age;
        state.velocityZ = vz;
      }
      if (turbulence != 0 && set.turbulenceFrequency > 0) {
        const double phase = age * set.turbulenceFrequency;
        const uint32_t stream = mixHash(set.seed ^ uint32_t(index) * 0x9e3779b9u) ^ 0x27d4eb2fu;
        z += smoothNoise(stream, phase + 9.7) * turbulence * std::min(1., phase);
      }
      state.z = z;
    }
    if (world) {
      const QTransform born = birthTransform(bornFrame);
      const QPointF where = born.map(offset) + travel;
      state.position = inverse.map(where);
      state.velocity = inverse.map(velocity) - inverse.map(QPointF());
    } else {
      state.position = offset + travel;
      state.velocity = velocity;
    }
    const double size = std::max(0., l.p[ParticleSize].at(bornFrame) * (1 + signedRandom(set.seed, index, 6) * set.sizeVariance));
    state.size = size * (1 + (set.sizeEnd - 1) * t01) / worldScale;
    const double spin = l.p[ParticleSpin].at(bornFrame) * (.5 + random01(set.seed, index, 7)) * (random01(set.seed, index, 8) < .5 ? -1 : 1);
    state.rotation = (set.randomRotation ? random01(set.seed, index, 9) * 360 : 0) + spin * age;
    state.flutter = random01(set.seed, index, 10) * 2 * kPi + age * (2 + 3 * random01(set.seed, index, 11));
    double alpha = set.opacity * (1 - random01(set.seed, index, 12) * set.opacityVariance);
    if (set.fadeIn > 0) alpha *= std::min(1., t01 / set.fadeIn);
    if (set.fadeOut > 0) alpha *= std::min(1., (1 - t01) / set.fadeOut);
    state.opacity = std::clamp(alpha, 0., 1.);
    state.life = t01;
    QColor colour = lerpColor(set.colorStart, set.colorEnd, t01);
    if (set.colorVariance > 0) {
      float h, sat, v, alphaF;
      colour.getHsvF(&h, &sat, &v, &alphaF);
      if (h < 0) h = 0;
      h = float(std::fmod(double(h) + signedRandom(set.seed, index, 13) * set.colorVariance * .5 + 1., 1.));
      colour = QColor::fromHsvF(h, sat, v, alphaF);
    }
    state.color = colour;
    if (state.opacity > 0 && state.size > 0) result->push_back(state);
  }
  return result;
}

// Everything a particle frame is worked out from, when all of it is plain
// keyframes: the settings, the sliders' whole curves -- a rate keyed a second
// ago still decides who is alive now -- and, in world space, the curves that
// moved the layer and everything it hangs from while its particles were being
// born. Empty when an expression is involved: its value can hang on anything.
QByteArray particleRecipe(const Layer &l, const Scene &s, double frame) {
  QByteArray content;
  QDataStream stream(&content, QIODevice::WriteOnly);
  auto write = [&](const Property &p) {
    if (p.hasExpression()) return false;
    stream << p.base << qint64(p.keys.size());
    for (const auto &k : p.keys) {
      stream << k.frame << k.value << k.ease << k.custom << k.absolute << k.continuous;
      for (double control : k.curve) stream << control;
    }
    return true;
  };
  stream << QJsonDocument(l.particles).toJson(QJsonDocument::Compact) << frame << s.fps << l.start << l.threeD << l.particlesOn;
  // Born over the layer's own area: that area -- the words of a title, the
  // outline of a shape -- is part of what decides where they are.
  if (settingsOf(l).emitter == 5) stream << layerOwnBounds(l, s);
  for (int i = ParticleRate; i <= ParticleSpin; ++i)
    if (!write(l.p[size_t(i)])) return {};
  if (settingsOf(l).space == 0 && !l.threeD) {
    QSet<QString> seen;
    for (const Layer *at = &l; at;) {
      for (int i : {0, 1, 2, 3, 4, int(AnchorX), int(AnchorY)})
        if (!write(at->p[size_t(i)])) return {};
      stream << at->mainX << at->mainY << at->parentBind << at->start << at->parentId;
      if (at->parentId.isEmpty() || seen.contains(at->id)) break;
      seen.insert(at->id);
      const Layer *parent = nullptr;
      for (const auto &candidate : s.layers)
        if (candidate.id == at->parentId) { parent = &candidate; break; }
      at = parent;
    }
  }
  return QCryptographicHash::hash(content, QCryptographicHash::Sha1);
}

// The last few frames worked out, shared by every band of a frame and every
// question asked about it: its bounds, then each band drawing its part. The
// first to ask works it out; the rest wait for that rather than repeat it.
using States = std::shared_ptr<const std::vector<ParticleState>>;
struct Remembered { QByteArray recipe; std::shared_future<States> states; };
QMutex rememberedGuard;
std::deque<Remembered> remembered;
}  // namespace

std::shared_ptr<const std::vector<ParticleState>> particleStates(const Layer &l, const Scene &s, double frame) {
  if (!layerEmitsParticles(l)) return std::make_shared<std::vector<ParticleState>>();
  const QByteArray recipe = particleRecipe(l, s, frame);
  if (recipe.isEmpty()) return simulate(l, s, frame);
  std::promise<States> promise;
  std::shared_future<States> future;
  bool mine = false;
  {
    QMutexLocker lock(&rememberedGuard);
    for (const auto &r : remembered)
      if (r.recipe == recipe) { future = r.states; break; }
    if (!future.valid()) {
      future = promise.get_future().share();
      remembered.push_back({recipe, future});
      if (remembered.size() > 16) remembered.pop_front();
      mine = true;
    }
  }
  if (mine) {
    try { promise.set_value(simulate(l, s, frame)); }
    catch (...) { promise.set_exception(std::current_exception()); }
  }
  return future.get();
}

QByteArray particleIdentity(const Layer &l, const Scene &s, double frame) {
  const QByteArray recipe = particleRecipe(l, s, frame);
  if (!recipe.isEmpty()) return recipe;
  // With an expression in it, what came out is the only honest name for it.
  QByteArray content;
  QDataStream stream(&content, QIODevice::WriteOnly);
  stream << QJsonDocument(l.particles).toJson(QJsonDocument::Compact);
  const States states = simulate(l, s, frame); // held: *simulate(...) in the loop would dangle
  for (const auto &p : *states)
    stream << p.position << p.velocity << p.size << p.rotation << p.opacity << p.flutter << p.color << p.z;
  return QCryptographicHash::hash(content, QCryptographicHash::Sha1);
}

namespace {
// How far a particle's drawing reaches from its centre.
double reachOf(const ParticleState &p, int shape) {
  double r = p.size / 2;
  if (shape == 3) r += std::hypot(p.velocity.x(), p.velocity.y()) * .04 + p.size;
  return r * 1.45 + 1;
}
}  // namespace

QRectF particleBounds(const Layer &l, const Scene &s, double frame) {
  const auto states = particleStates(l, s, frame);
  const int shape = std::clamp(int(particleSetting(l, "shape")), 0, 7);
  QRectF bounds;
  bool any = false;
  double left = 0, top = 0, right = 0, bottom = 0;
  for (const auto &p : *states) {
    const double r = reachOf(p, shape);
    if (!any) { left = p.position.x() - r; right = p.position.x() + r; top = p.position.y() - r; bottom = p.position.y() + r; any = true; }
    left = std::min(left, p.position.x() - r); right = std::max(right, p.position.x() + r);
    top = std::min(top, p.position.y() - r); bottom = std::max(bottom, p.position.y() + r);
  }
  if (any) bounds = QRectF(QPointF(left, top), QPointF(right, bottom));
  // The emitter itself, so an empty system still has somewhere to be.
  const double w = std::max(2., particleSetting(l, "emitterWidth")), h = std::max(2., particleSetting(l, "emitterHeight"));
  const int emitter = std::clamp(int(particleSetting(l, "emitter")), 0, 4);
  const QRectF source = emitter == 0 ? QRectF(-1, -1, 2, 2) : emitter == 1 ? QRectF(-w / 2, -1, w, 2) : QRectF(-w / 2, -h / 2, w, h);
  return any ? bounds.united(source) : source;
}

namespace {
const QPainterPath &starPath() {
  static const QPainterPath star = [] {
    QPainterPath path;
    for (int i = 0; i < 10; ++i) {
      const double r = i % 2 ? .42 : 1, a = -kPi / 2 + i * kPi / 5;
      const QPointF point(std::cos(a) * r, std::sin(a) * r);
      if (i == 0) path.moveTo(point); else path.lineTo(point);
    }
    path.closeSubpath();
    return path;
  }();
  return star;
}
// One particle, drawn by QPainter: the shapes that turn, and any shape where
// the pixels cannot be written directly (a projected 3D layer, deep colour).
void paintParticle(QPainter &p, const ParticleState &particle, int shape, const QTransform &base) {
  QColor colour = particle.color;
  colour.setAlphaF(float(std::clamp(colour.alphaF() * particle.opacity, 0., 1.)));
  const double r = particle.size / 2;
  const QPointF at = particle.position;
  switch (shape) {
    case 0: { // a soft dot
      QRadialGradient glow(at, r);
      QColor edge = colour; edge.setAlpha(0);
      QColor middle = colour; middle.setAlphaF(colour.alphaF() * .55f);
      glow.setColorAt(0, colour); glow.setColorAt(.45, middle); glow.setColorAt(1, edge);
      p.setBrush(glow);
      p.drawEllipse(at, r, r);
      break;
    }
    case 1: p.setBrush(colour); p.drawEllipse(at, r, r); break;
    case 3: { // a streak along the way it is going
      const double speed = std::hypot(particle.velocity.x(), particle.velocity.y());
      const QPointF along = speed > 1e-6 ? particle.velocity / speed : QPointF(0, -1);
      const double length = particle.size + speed * .04;
      QLinearGradient fade(at - along * length, at);
      QColor tail = colour; tail.setAlpha(0);
      fade.setColorAt(0, tail); fade.setColorAt(1, colour);
      p.setPen(QPen(QBrush(fade), std::max(.75, particle.size * .3), Qt::SolidLine, Qt::RoundCap));
      p.drawLine(at - along * length, at);
      p.setPen(Qt::NoPen);
      break;
    }
    case 6: { // bokeh: a disc of light, brighter at its rim
      QRadialGradient disc(at, r);
      QColor inner = colour; inner.setAlphaF(colour.alphaF() * .3f);
      QColor rim = colour; rim.setAlphaF(colour.alphaF() * .85f);
      QColor edge = colour; edge.setAlpha(0);
      disc.setColorAt(0, inner); disc.setColorAt(.78, inner); disc.setColorAt(.93, rim); disc.setColorAt(1, edge);
      p.setBrush(disc);
      p.drawEllipse(at, r, r);
      break;
    }
    default: { // shapes that turn: square, petal, star, confetti
      QTransform t = base;
      t.translate(at.x(), at.y());
      t.rotate(particle.rotation);
      if (shape == 4) t.scale(.35 + .65 * std::abs(std::cos(particle.flutter)), 1);
      if (shape == 7) t.scale(1, std::cos(particle.flutter * 1.7));
      p.setWorldTransform(t);
      p.setBrush(colour);
      if (shape == 2) p.drawRect(QRectF(-r, -r, 2 * r, 2 * r));
      else if (shape == 4) p.drawEllipse(QRectF(-r * .55, -r, r * 1.1, 2 * r));
      else if (shape == 5) { QTransform grow; grow.scale(r, r); p.drawPath(grow.map(starPath())); }
      else p.drawRect(QRectF(-r, -r * .6, 2 * r, 1.2 * r));
      p.setWorldTransform(base);
      break;
    }
  }
}

// A soft dot, a disc and a bokeh are each a distance from a centre, and a
// streak is a distance from a line: written straight into the pixels they are
// a few multiplies each -- no path to fill, no gradient to set up -- where
// QPainter spends ten microseconds a particle, and on the card's painter more.
// Distances are measured in the layer's own units and turned into pixels
// along the direction they are taken in, so a layer scaled more one way than
// the other stretches its dots into ellipses and its streaks' width with the
// way they point, exactly as QPainter's shapes and pens would be.
using Mark = ParticleMark;
// How opaque a round mark is at a fraction u of its radius.
inline float roundAlpha(int shape, float u) {
  if (shape == 1 || shape == 4) return 1;
  if (shape == 0) return u < .45f ? 1 - u / .45f * .45f : .55f * (1 - (u - .45f) / .55f);
  // bokeh
  if (u < .78f) return .3f;
  if (u < .93f) return .3f + (u - .78f) / .15f * .55f;
  return .85f * (1 - (u - .93f) / .07f);
}
// `strength` is how opaque the mark is here (its fade), `edge` how much of the
// pixel it covers. Added light is weighed by the part of the pixel it lands
// on, as QPainter does: a pixel half covered gets half of what adding the
// whole would have made it, which is not the same as adding half as much
// once the sum runs past white.
inline void blendPixel(QRgb &pixel, const Mark &m, float strength, float edge, bool add) {
  const float a = m.a * strength;
  if (a <= 0 || edge <= 0) return;
  const float sr = m.r * a, sg = m.g * a, sb = m.b * a, sa = 255 * a;
  float dr = float(qRed(pixel)), dg = float(qGreen(pixel)), db = float(qBlue(pixel)), da = float(qAlpha(pixel));
  if (add) {
    dr += (std::min(255.f, dr + sr) - dr) * edge; dg += (std::min(255.f, dg + sg) - dg) * edge;
    db += (std::min(255.f, db + sb) - db) * edge; da += (std::min(255.f, da + sa) - da) * edge;
  } else {
    const float keep = 1 - a * edge;
    dr = sr * edge + dr * keep; dg = sg * edge + dg * keep; db = sb * edge + db * keep; da = sa * edge + da * keep;
  }
  pixel = qRgba(int(dr + .5f), int(dg + .5f), int(db + .5f), int(da + .5f));
}
void writeMarks(QImage &buffer, int fromRow, int toRow, const std::vector<Mark> &marks, int shape, bool add) {
  const int width = buffer.width();
  for (const Mark &m : marks) {
    const int top = std::max(m.top, fromRow), bottom = std::min(m.bottom, toRow - 1);
    if (top > bottom) continue;
    const int left = std::max(0, m.left), right = std::min(width - 1, m.right);
    for (int y = top; y <= bottom; ++y) {
      QRgb *row = reinterpret_cast<QRgb *>(buffer.scanLine(y));
      const float py = y + .5f;
      for (int x = left; x <= right; ++x) {
        const float px = x + .5f;
        float strength, edge;
        const float dx = px - m.cx, dy = py - m.cy;
        const float lx = m.i11 * dx + m.i12 * dy, ly = m.i21 * dx + m.i22 * dy;
        if (shape == 3) {
          // The nearest point on the line, and how far along from the tail:
          // the fade is measured along the line in its own units, as a
          // gradient on the pen is.
          const float length2 = m.lsx * m.lsx + m.lsy * m.lsy;
          const float along = length2 > 1e-12f ? (lx * m.lsx + ly * m.lsy) / length2 : 1.f;
          const float t = std::clamp(along, 0.f, 1.f);
          const float ex = lx - m.lsx * t, ey = ly - m.lsy * t;
          const float distance = std::sqrt(ex * ex + ey * ey);
          const float ox = dx - m.sx * t, oy = dy - m.sy * t;
          const float scale = distance > 1e-6f ? std::sqrt(ox * ox + oy * oy) / distance : m.pixelScale;
          edge = std::min(1.f, (m.radius - distance) * scale + .5f);
          strength = t;
        } else if (shape == 4) {
          // A petal is a thin ellipse: how far a pixel is from its edge, in
          // pixels, is the distance in its own units over how fast that
          // distance grows across the pixels (its gradient), not along the line
          // from the middle, which is wrong wherever the ellipse is narrow.
          const float distance = std::sqrt(lx * lx + ly * ly);
          if (distance > m.radius + 4 * (std::abs(m.i11) + std::abs(m.i12) + std::abs(m.i21) + std::abs(m.i22))) continue;
          float gradient = m.pixelScale > 0 ? 1.f / m.pixelScale : 1.f;
          if (distance > 1e-6f) {
            const float qx = lx / distance, qy = ly / distance;
            const float gx = m.i11 * qx + m.i21 * qy, gy = m.i12 * qx + m.i22 * qy;
            gradient = std::sqrt(gx * gx + gy * gy);
          }
          edge = std::min(1.f, (m.radius - distance) / std::max(gradient, 1e-6f) + .5f);
          if (edge <= 0) continue;
          strength = 1;
        } else {
          const float distance = std::sqrt(lx * lx + ly * ly);
          const float scale = distance > 1e-6f ? std::sqrt(dx * dx + dy * dy) / distance : m.pixelScale;
          edge = std::min(1.f, (m.radius - distance) * scale + .5f);
          if (edge <= 0) continue;
          strength = roundAlpha(shape, std::min(1.f, distance / m.radius));
        }
        blendPixel(row[x], m, strength, edge, add);
      }
    }
  }
}
}  // namespace

QRectF particleEmitterBounds(const Layer &l) {
  // Never smaller than a null layer's handle, so a point can still be grabbed.
  constexpr double grab = 80;
  const double w = std::max(grab, particleSetting(l, "emitterWidth"));
  const double h = std::max(grab, particleSetting(l, "emitterHeight"));
  switch (std::clamp(int(particleSetting(l, "emitter")), 0, 4)) {
    case 0: return QRectF(-grab / 2, -grab / 2, grab, grab);
    case 1: return QRectF(-w / 2, -grab / 4, w, grab / 2);
    default: return QRectF(-w / 2, -h / 2, w, h);
  }
}

namespace {
// Qt's line rasterizer, QRasterizer::rasterizeLine (qrasterizer.cpp, Qt 6.8),
// antialiased, without square caps: the levels QPainter's raster engine gives
// the pixels of a rectangle drawn under a transform with no shear -- the line
// from the middle of its left side to the middle of its right, `width` its
// height over its width. The same arithmetic as Qt's: 16.16 fixed point in 64
// bits, as a 64-bit build has it, the corners on the 26.6 grid, the same
// truncations. It is not the exact share of each pixel the rectangle covers --
// a side lying nearly level gives its whole row one level -- and a card
// covering squares exactly parted from the CPU by up to 24 levels. `span` is
// handed each run of pixels with its level, as QSpanBuffer::addSpan is.
namespace line {
using Fixed = qint64;
constexpr Fixed kOne = 65536;
inline Fixed fromInt(int i) { return Fixed(i) * (1 << 16); }
inline int toInt(Fixed f) { return int(f >> 16); }
inline Fixed multiply(Fixed x, Fixed y) { return Fixed((qlonglong(x) * qlonglong(y)) >> 16); }
inline Fixed fastMultiply(Fixed x, Fixed y) { return (x * y) >> 16; }
inline Fixed fromReal(qreal x) {
  const qreal tmp = x * kOne;
  if (tmp > qreal(INT_MAX) * kOne) return Fixed(INT_MAX) * kOne;
  if (tmp < qreal(INT_MIN) * kOne) return Fixed(INT_MIN) * kOne;
  return Fixed(tmp);
}
inline bool same(qreal p1, qreal p2) { return int((p2 - p1) * 64.) == 0; }
inline QPointF snap(const QPointF &p) {
  return QPointF(std::floor(p.x() * 64) * (1 / qreal(64)), std::floor(p.y() * 64) * (1 / qreal(64)));
}
// Qt leans on a division by nought coming out as a very large number.
inline qreal divide(qreal x, qreal y) { return y == 0 ? (x > 0 ? 1e20 : -1e20) : x / y; }
// How much of pixel column x, between `top` and `bottom`, lies outside an edge
// that crosses that band from leftX to rightX (intersectPixelFP).
Fixed outside(int x, Fixed top, Fixed bottom, Fixed leftIntersectX, Fixed rightIntersectX, Fixed slope, Fixed invSlope) {
  const Fixed leftX = fromInt(x), rightX = fromInt(x) + kOne;
  Fixed leftIntersectY = 0, rightIntersectY = 0;
  auto intersectY = [&] {
    leftIntersectY = top + multiply(leftX - (slope > 0 ? leftIntersectX : rightIntersectX), invSlope);
    rightIntersectY = leftIntersectY + invSlope;
  };
  if (leftIntersectX >= leftX && rightIntersectX <= rightX)
    return multiply(bottom - top, leftIntersectX - leftX + ((rightIntersectX - leftIntersectX) >> 1));
  if (leftIntersectX >= rightX) return bottom - top;
  if (leftIntersectX >= leftX) {
    intersectY();
    return slope > 0 ? (bottom - top) - fastMultiply((rightX - leftIntersectX) >> 1, rightIntersectY - top)
                     : (bottom - top) - fastMultiply((rightX - leftIntersectX) >> 1, bottom - rightIntersectY);
  }
  if (rightIntersectX <= leftX) return 0;
  if (rightIntersectX <= rightX) {
    intersectY();
    return slope > 0 ? fastMultiply((rightIntersectX - leftX) >> 1, bottom - leftIntersectY)
                     : fastMultiply((rightIntersectX - leftX) >> 1, leftIntersectY - top);
  }
  intersectY();
  return slope > 0 ? (bottom - rightIntersectY) + ((rightIntersectY - leftIntersectY) >> 1)
                   : (rightIntersectY - top) + ((leftIntersectY - rightIntersectY) >> 1);
}
// The line cut to `clip`; true when nothing of it is left (qClipLine).
bool clip(QPointF *pt1, QPointF *pt2, const QRectF &clip) {
  qreal &x1 = pt1->rx(), &y1 = pt1->ry(), &x2 = pt2->rx(), &y2 = pt2->ry();
  if (!qIsFinite(x1) || !qIsFinite(y1) || !qIsFinite(x2) || !qIsFinite(y2)) return true;
  const qreal xmin = clip.left(), xmax = clip.right(), ymin = clip.top(), ymax = clip.bottom();
  if (x1 < xmin) {
    if (x2 <= xmin) return true;
    y1 += (y2 - y1) / (x2 - x1) * (xmin - x1);
    x1 = xmin;
  } else if (x1 > xmax) {
    if (x2 >= xmax) return true;
    y1 += (y2 - y1) / (x2 - x1) * (xmax - x1);
    x1 = xmax;
  }
  if (x2 < xmin) {
    y2 += (y2 - y1) / (x2 - x1) * (xmin - x2);
    x2 = xmin;
  } else if (x2 > xmax) {
    y2 += (y2 - y1) / (x2 - x1) * (xmax - x2);
    x2 = xmax;
  }
  if (y1 < ymin) {
    if (y2 <= ymin) return true;
    x1 += (x2 - x1) / (y2 - y1) * (ymin - y1);
    y1 = ymin;
  } else if (y1 > ymax) {
    if (y2 >= ymax) return true;
    x1 += (x2 - x1) / (y2 - y1) * (ymax - y1);
    y1 = ymax;
  }
  if (y2 < ymin) {
    x2 += (x2 - x1) / (y2 - y1) * (ymin - y2);
    y2 = ymin;
  } else if (y2 > ymax) {
    x2 += (x2 - x1) / (y2 - y1) * (ymax - y2);
    y2 = ymax;
  }
  return false;
}
void levels(const QPointF &a, const QPointF &b, qreal width, const QRect &clipRect,
            const std::function<void(int x, int len, int y, int level)> &span) {
  if (a == b || !(width > 0.0) || clipRect.isEmpty()) return;
  QPointF pa = a, pb = b;
  const QPointF offs = QPointF(qAbs(b.y() - a.y()), qAbs(b.x() - a.x())) * width * 0.5;
  const QRectF clipped(clipRect.topLeft() - offs, clipRect.bottomRight() + QPoint(1, 1) + offs);
  if (clip(&pa, &pb, clipped)) return;
  {
    const QPointF d0 = a - b, d = pa - pb;
    const qreal w0 = d0.x() * d0.x() + d0.y() * d0.y(), w = d.x() * d.x() + d.y() * d.y();
    if (w == 0) return;
    // The width is given relative to |b - a|.
    width *= std::sqrt(w0 / w);
  }
  auto add = [&](int x, int len, int y, int level) {
    if (level && len) span(x, len, y, level);
  };
  if (same(pa.y(), pb.y())) {
    // Level: turned into the upright case.
    const qreal x = (pa.x() + pb.x()) * 0.5f, dx = qAbs(pb.x() - pa.x()) * 0.5f;
    const qreal y = pa.y(), dy = width * dx;
    pa = QPointF(x, y - dy);
    pb = QPointF(x, y + dy);
    width = 1 / width;
  }
  if (same(pa.x(), pb.x())) {
    if (pa.y() > pb.y()) qSwap(pa, pb);
    const qreal dy = pb.y() - pa.y(), halfWidth = 0.5f * width * dy;
    qreal left = pa.x() - halfWidth, right = pa.x() + halfWidth;
    left = qBound(qreal(clipRect.left()), left, qreal(clipRect.right() + 1));
    right = qBound(qreal(clipRect.left()), right, qreal(clipRect.right() + 1));
    pa.ry() = qBound(qreal(clipRect.top()), pa.y(), qreal(clipRect.bottom() + 1));
    pb.ry() = qBound(qreal(clipRect.top()), pb.y(), qreal(clipRect.bottom() + 1));
    if (same(left, right) || same(pa.y(), pb.y())) return;
    const int iLeft = int(left), iRight = int(right);
    const Fixed leftWidth = fromInt(iLeft + 1) - fromReal(left), rightWidth = fromReal(right) - fromInt(iRight);
    Fixed coverage[3];
    int x[3], len[3], n = 1;
    if (iLeft == iRight) {
      coverage[0] = leftWidth == kOne ? rightWidth * 255 : (rightWidth + leftWidth - kOne) * 255;
      x[0] = iLeft;
      len[0] = 1;
    } else {
      coverage[0] = leftWidth * 255;
      x[0] = iLeft;
      len[0] = 1;
      if (leftWidth == kOne) {
        len[0] = iRight - iLeft;
      } else if (iRight - iLeft > 1) {
        coverage[1] = fromInt(255);
        x[1] = iLeft + 1;
        len[1] = iRight - iLeft - 1;
        ++n;
      }
      if (rightWidth) {
        coverage[n] = rightWidth * 255;
        x[n] = iRight;
        len[n] = 1;
        ++n;
      }
    }
    const Fixed iTopFP = fromInt(int(pa.y())), iBottomFP = fromInt(int(pb.y()));
    const Fixed yPa = fromReal(pa.y()), yPb = fromReal(pb.y());
    for (Fixed yFP = iTopFP; yFP <= iBottomFP; yFP += kOne) {
      const Fixed rowHeight = qMin(yFP + kOne, yPb) - qMax(yFP, yPa);
      const int y = toInt(yFP);
      if (y > clipRect.bottom()) break;
      for (int i = 0; i < n; ++i) add(x[i], len[i], y, toInt(multiply(rowHeight, coverage[i])));
    }
    return;
  }
  if (pa.y() > pb.y()) qSwap(pa, pb);
  QPointF delta = pb - pa;
  delta *= 0.5f * width;
  const QPointF perp(delta.y(), -delta.x());
  QPointF top, left, right, bottom;
  if (pa.x() < pb.x()) {
    top = pa + perp; left = pa - perp; right = pb + perp; bottom = pb - perp;
  } else {
    top = pa - perp; left = pb - perp; right = pa + perp; bottom = pb + perp;
  }
  top = snap(top); bottom = snap(bottom); left = snap(left); right = snap(right);
  const qreal topBound = qBound(qreal(clipRect.top()), top.y(), qreal(clipRect.bottom()));
  const qreal bottomBound = qBound(qreal(clipRect.top()), bottom.y(), qreal(clipRect.bottom()));
  const QPointF topLeftEdge = left - top, topRightEdge = right - top;
  const QPointF bottomLeftEdge = bottom - left, bottomRightEdge = bottom - right;
  const qreal topLeftSlope = divide(topLeftEdge.x(), topLeftEdge.y());
  const qreal bottomLeftSlope = divide(bottomLeftEdge.x(), bottomLeftEdge.y());
  const qreal topRightSlope = divide(topRightEdge.x(), topRightEdge.y());
  const qreal bottomRightSlope = divide(bottomRightEdge.x(), bottomRightEdge.y());
  const Fixed topLeftSlopeFP = fromReal(topLeftSlope), topRightSlopeFP = fromReal(topRightSlope);
  const Fixed bottomLeftSlopeFP = fromReal(bottomLeftSlope), bottomRightSlopeFP = fromReal(bottomRightSlope);
  const Fixed invTopLeftSlopeFP = fromReal(divide(1, topLeftSlope)), invTopRightSlopeFP = fromReal(divide(1, topRightSlope));
  const Fixed invBottomLeftSlopeFP = fromReal(divide(1, bottomLeftSlope));
  const Fixed invBottomRightSlopeFP = fromReal(divide(1, bottomRightSlope));
  const Fixed iTopFP = fromInt(int(topBound)), iLeftFP = fromInt(int(left.y()));
  const Fixed iRightFP = fromInt(int(right.y())), iBottomFP = fromInt(int(bottomBound));
  Fixed leftIntersectAf = fromReal(top.x() + (int(topBound) - top.y()) * topLeftSlope);
  Fixed rightIntersectAf = fromReal(top.x() + (int(topBound) - top.y()) * topRightSlope);
  Fixed leftIntersectBf = 0, rightIntersectBf = 0;
  if (iLeftFP < iTopFP) leftIntersectBf = fromReal(left.x() + (int(topBound) - left.y()) * bottomLeftSlope);
  if (iRightFP < iTopFP) rightIntersectBf = fromReal(right.x() + (int(topBound) - right.y()) * bottomRightSlope);
  Fixed rowTop, rowBottomLeft, rowBottomRight, rowTopLeft, rowTopRight, rowBottom;
  Fixed topLeftIntersectAf, topLeftIntersectBf, topRightIntersectAf, topRightIntersectBf;
  Fixed bottomLeftIntersectAf, bottomLeftIntersectBf, bottomRightIntersectAf, bottomRightIntersectBf;
  int leftMin, leftMax, rightMin, rightMax;
  const Fixed yTopFP = fromReal(top.y()), yLeftFP = fromReal(left.y());
  const Fixed yRightFP = fromReal(right.y()), yBottomFP = fromReal(bottom.y());
  rowTop = qMax(iTopFP, yTopFP);
  topLeftIntersectAf = leftIntersectAf + multiply(topLeftSlopeFP, rowTop - iTopFP);
  topRightIntersectAf = rightIntersectAf + multiply(topRightSlopeFP, rowTop - iTopFP);
  Fixed yFP = iTopFP;
  while (yFP <= iBottomFP) {
    rowBottomLeft = qMin(yFP + kOne, yLeftFP);
    rowBottomRight = qMin(yFP + kOne, yRightFP);
    rowTopLeft = qMax(yFP, yLeftFP);
    rowTopRight = qMax(yFP, yRightFP);
    rowBottom = qMin(yFP + kOne, yBottomFP);
    if (yFP == iLeftFP) {
      const int y = toInt(yFP);
      leftIntersectBf = fromReal(left.x() + (y - left.y()) * bottomLeftSlope);
      topLeftIntersectBf = leftIntersectBf + multiply(bottomLeftSlopeFP, rowTopLeft - yFP);
      bottomLeftIntersectAf = leftIntersectAf + multiply(topLeftSlopeFP, rowBottomLeft - yFP);
    } else {
      topLeftIntersectBf = leftIntersectBf;
      bottomLeftIntersectAf = leftIntersectAf + topLeftSlopeFP;
    }
    if (yFP == iRightFP) {
      const int y = toInt(yFP);
      rightIntersectBf = fromReal(right.x() + (y - right.y()) * bottomRightSlope);
      topRightIntersectBf = rightIntersectBf + multiply(bottomRightSlopeFP, rowTopRight - yFP);
      bottomRightIntersectAf = rightIntersectAf + multiply(topRightSlopeFP, rowBottomRight - yFP);
    } else {
      topRightIntersectBf = rightIntersectBf;
      bottomRightIntersectAf = rightIntersectAf + topRightSlopeFP;
    }
    if (yFP == iBottomFP) {
      bottomLeftIntersectBf = leftIntersectBf + multiply(bottomLeftSlopeFP, rowBottom - yFP);
      bottomRightIntersectBf = rightIntersectBf + multiply(bottomRightSlopeFP, rowBottom - yFP);
    } else {
      bottomLeftIntersectBf = leftIntersectBf + bottomLeftSlopeFP;
      bottomRightIntersectBf = rightIntersectBf + bottomRightSlopeFP;
    }
    if (yFP < iLeftFP) {
      leftMin = toInt(bottomLeftIntersectAf);
      leftMax = toInt(topLeftIntersectAf);
    } else if (yFP == iLeftFP) {
      leftMin = toInt(qMax(bottomLeftIntersectAf, topLeftIntersectBf));
      leftMax = toInt(qMax(topLeftIntersectAf, bottomLeftIntersectBf));
    } else {
      leftMin = toInt(topLeftIntersectBf);
      leftMax = toInt(bottomLeftIntersectBf);
    }
    leftMin = qBound(clipRect.left(), leftMin, clipRect.right());
    leftMax = qBound(clipRect.left(), leftMax, clipRect.right());
    if (yFP < iRightFP) {
      rightMin = toInt(topRightIntersectAf);
      rightMax = toInt(bottomRightIntersectAf);
    } else if (yFP == iRightFP) {
      rightMin = toInt(qMin(topRightIntersectAf, bottomRightIntersectBf));
      rightMax = toInt(qMin(bottomRightIntersectAf, topRightIntersectBf));
    } else {
      rightMin = toInt(bottomRightIntersectBf);
      rightMax = toInt(topRightIntersectBf);
    }
    rightMin = qBound(clipRect.left(), rightMin, clipRect.right());
    rightMax = qBound(clipRect.left(), rightMax, clipRect.right());
    if (leftMax > rightMax) leftMax = rightMax;
    if (rightMin < leftMin) rightMin = leftMin;
    const Fixed rowHeight = rowBottom - rowTop;
    // Outside the right-hand side, between rows `from` and `to`.
    auto outsideRight = [&](int x) {
      Fixed excluded = 0;
      if (yFP <= iRightFP && rowBottomRight > rowTop)
        excluded += (rowBottomRight - rowTop) - outside(x, rowTop, rowBottomRight, topRightIntersectAf, bottomRightIntersectAf,
                                                        topRightSlopeFP, invTopRightSlopeFP);
      if (yFP >= iRightFP && rowBottom > rowTopRight)
        excluded += (rowBottom - rowTopRight) - outside(x, rowTopRight, rowBottom, bottomRightIntersectBf, topRightIntersectBf,
                                                        bottomRightSlopeFP, invBottomRightSlopeFP);
      return excluded;
    };
    int x = leftMin;
    while (x <= leftMax) {
      Fixed excluded = 0;
      if (yFP <= iLeftFP && rowBottomLeft > rowTop)
        excluded += outside(x, rowTop, rowBottomLeft, bottomLeftIntersectAf, topLeftIntersectAf, topLeftSlopeFP, invTopLeftSlopeFP);
      if (yFP >= iLeftFP && rowBottom > rowTopLeft)
        excluded += outside(x, rowTopLeft, rowBottom, topLeftIntersectBf, bottomLeftIntersectBf, bottomLeftSlopeFP,
                            invBottomLeftSlopeFP);
      if (x >= rightMin) excluded += outsideRight(x);
      add(x, 1, toInt(yFP), toInt(255 * (rowHeight - excluded)));
      ++x;
    }
    if (x < rightMin) {
      add(x, rightMin - x, toInt(yFP), toInt(255 * rowHeight));
      x = rightMin;
    }
    while (x <= rightMax) {
      add(x, 1, toInt(yFP), toInt(255 * (rowHeight - outsideRight(x))));
      ++x;
    }
    leftIntersectAf += topLeftSlopeFP;
    leftIntersectBf += bottomLeftSlopeFP;
    rightIntersectAf += topRightSlopeFP;
    rightIntersectBf += bottomRightSlopeFP;
    topLeftIntersectAf = leftIntersectAf;
    topRightIntersectAf = rightIntersectAf;
    yFP += kOne;
    rowTop = yFP;
  }
}
}  // namespace line
// Whether Qt's raster engine takes a transform to turn and scale evenly, with
// no shear (its tx_noshear, from qt_scaleForTransform).
bool transformKeepsShape(const QTransform &t) {
  const QTransform::TransformationType type = t.type();
  if (type <= QTransform::TxTranslate) return true;
  if (type == QTransform::TxScale) return qFuzzyCompare(qAbs(t.m11()), qAbs(t.m22()));
  const qreal xScale1 = t.m11() * t.m11() + t.m21() * t.m21();
  const qreal yScale1 = t.m12() * t.m12() + t.m22() * t.m22();
  const qreal xScale2 = t.m11() * t.m11() + t.m12() * t.m12();
  const qreal yScale2 = t.m21() * t.m21() + t.m22() * t.m22();
  if (qAbs(xScale1 - yScale1) > qAbs(xScale2 - yScale2)) return type == QTransform::TxRotate && qFuzzyCompare(xScale1, yScale1);
  return type == QTransform::TxRotate && qFuzzyCompare(xScale2, yScale2);
}
// A square, star or piece of confetti as QPainter fills it in paintParticle,
// for a card to cover each pixel as Qt's raster engine does (Qt 6.8,
// qpaintengine_raster.cpp). drawStates hands QPainter a picture of `area`,
// the pixels of the device the particles reach, and the same transforms,
// built the same way, put the corners where the CPU's are, bit for bit. A
// rectangle under a transform that only turns and scales evenly goes to Qt's
// line rasterizer, whose levels are worked out here, for the pixels of `box`
// (rule 1); anything else to its outline mapper, which rounds every corner to
// 1/64 of a pixel for the gray rasterizer, which the card works through
// (rule 0). The colour is the brush's as the raster engine fills with it:
// premultiplied in sixteen bits, then narrowed to eight.
ParticleOutline outlineOf(const ParticleState &particle, int shape, const QTransform &world, const QRect &area,
                          const QRect &box, std::vector<uchar> &levels) {
  ParticleOutline o{};
  QColor colour = particle.color;
  colour.setAlphaF(float(std::clamp(colour.alphaF() * particle.opacity, 0., 1.)));
  const QRgb fill = colour.rgba64().premultiplied().toArgb32();
  o.r = float(qRed(fill)); o.g = float(qGreen(fill)); o.b = float(qBlue(fill)); o.a = float(qAlpha(fill));
  QTransform t = world * QTransform::fromTranslate(-area.left(), -area.top()) * QTransform::fromScale(1, 1);
  t.translate(particle.position.x(), particle.position.y());
  t.rotate(particle.rotation);
  if (shape == 7) t.scale(1, std::cos(particle.flutter * 1.7));
  const double r = particle.size / 2;
  const QRectF rect = shape == 2 ? QRectF(-r, -r, 2 * r, 2 * r) : QRectF(-r, -r * .6, 2 * r, 1.2 * r);
  if (shape != 5 && transformKeepsShape(t)) {
    o.rule = 1;
    o.levelsBox = box & area;
    o.levelsAt = int(levels.size());
    levels.resize(levels.size() + size_t(std::max(0, o.levelsBox.width())) * size_t(std::max(0, o.levelsBox.height())), 0);
    const QRectF upright = rect.normalized();
    if (upright.isEmpty() || o.levelsBox.isEmpty()) return o;
    // drawRects: the line from the middle of the left side to the middle of
    // the right, in the picture's pixels; the box is in the device's.
    const QPointF a = t.map((upright.topLeft() + upright.bottomLeft()) * 0.5f);
    const QPointF b = t.map((upright.topRight() + upright.bottomRight()) * 0.5f);
    line::levels(a, b, upright.height() / upright.width(), QRect(QPoint(), area.size()), [&](int x, int len, int y, int level) {
      const int row = y + area.top() - o.levelsBox.top();
      if (row < 0 || row >= o.levelsBox.height()) return;
      for (int k = 0; k < len; ++k) {
        const int column = x + k + area.left() - o.levelsBox.left();
        if (column >= 0 && column < o.levelsBox.width())
          levels[size_t(o.levelsAt) + size_t(row) * size_t(o.levelsBox.width()) + size_t(column)] = uchar(level);
      }
    });
    return o;
  }
  QPointF corners[10];
  if (shape == 5) {
    QTransform grow;
    grow.scale(r, r);
    const QPainterPath star = grow.map(starPath());
    for (int i = 0; i < 10; ++i) corners[i] = t.map(QPointF(star.elementAt(i).x, star.elementAt(i).y));
    o.corners = 10;
  } else {
    // QPaintEngineEx::drawRects' outline.
    const qreal right = rect.x() + rect.width(), bottom = rect.y() + rect.height();
    const QPointF points[4] = {{rect.x(), rect.y()}, {right, rect.y()}, {right, bottom}, {rect.x(), bottom}};
    for (int i = 0; i < 4; ++i) corners[i] = t.map(points[i]);
    o.corners = 4;
  }
  // On the 26.6 grid (qreal_to_fixed_26_6), back in the device's pixels.
  for (int i = 0; i < o.corners; ++i) {
    o.x[i] = float(qRound(corners[i].x() * 64) + area.left() * 64) / 64.f;
    o.y[i] = float(qRound(corners[i].y() * 64) + area.top() * 64) / 64.f;
  }
  return o;
}
// Each particle as the pixels it is written into, in the pixels `toBuffer`
// takes its layer to; those that land outside `bufferRect` are left out.
// False when the transform flattens the layer to nothing. With `outlines`,
// squares, stars and confetti come with their outlines too, one a mark, as
// QPainter would fill them in a picture of `bufferRect` (see outlineOf).
bool buildMarks(const std::vector<ParticleState> &states, int shape, const QTransform &toBuffer,
                const QRectF &bufferRect, std::vector<Mark> &marks, ParticleOutlines *outlines = nullptr) {
  const double pixelScale = std::sqrt(std::abs(toBuffer.m11() * toBuffer.m22() - toBuffer.m12() * toBuffer.m21()));
  bool invertible = false;
  const QTransform linear(toBuffer.m11(), toBuffer.m12(), toBuffer.m21(), toBuffer.m22(), 0, 0);
  const QTransform back = linear.inverted(&invertible);
  if (!invertible || !(pixelScale > 0)) return false;
  marks.reserve(states.size());
  for (const auto &particle : states) {
    QColor colour = particle.color;
    float alpha = float(std::clamp(colour.alphaF() * particle.opacity, 0., 1.));
    if (alpha <= 0) continue;
    Mark m{};
    m.r = float(colour.redF() * 255); m.g = float(colour.greenF() * 255); m.b = float(colour.blueF() * 255);
    m.i11 = float(back.m11()); m.i12 = float(back.m21()); m.i21 = float(back.m12()); m.i22 = float(back.m22());
    m.pixelScale = float(pixelScale);
    QRectF reach;
    if (shape == 3) {
      const double speed = std::hypot(particle.velocity.x(), particle.velocity.y());
      const QPointF along = speed > 1e-6 ? particle.velocity / speed : QPointF(0, -1);
      const double length = particle.size + speed * .04;
      const QPointF tailLocal = particle.position - along * length;
      const QPointF head = toBuffer.map(particle.position), tail = toBuffer.map(tailLocal);
      double half = std::max(.75, particle.size * .3) / 2;
      // Thinner than a pixel: as wide as one, and fainter by as much.
      if (half * pixelScale < .5) { alpha *= float(half * pixelScale / .5); half = .5 / pixelScale; }
      m.cx = float(tail.x()); m.cy = float(tail.y());
      m.sx = float(head.x() - tail.x()); m.sy = float(head.y() - tail.y());
      m.lsx = float(along.x() * length); m.lsy = float(along.y() * length);
      m.radius = float(half);
      const QRectF cap = linear.mapRect(QRectF(-half, -half, 2 * half, 2 * half));
      reach = QRectF(head, tail).normalized().adjusted(cap.left() - 1, cap.top() - 1, cap.right() + 1, cap.bottom() + 1);
    } else if (shape == 4) {
      // The petal's own units: turned by its rotation, squeezed across by its
      // flutter, its width a little over half its length -- as paintParticle
      // draws it -- so it is the circle of its radius there.
      const double r = particle.size / 2;
      const double squeeze = (.35 + .65 * std::abs(std::cos(particle.flutter))) * .55;
      QTransform own;
      own.scale(squeeze, 1);
      own = own * QTransform().rotate(particle.rotation) * linear;
      bool ownInvertible = false;
      const QTransform ownBack = own.inverted(&ownInvertible);
      if (!ownInvertible) continue;
      m.i11 = float(ownBack.m11()); m.i12 = float(ownBack.m21()); m.i21 = float(ownBack.m12()); m.i22 = float(ownBack.m22());
      m.pixelScale = float(std::sqrt(std::abs(own.m11() * own.m22() - own.m12() * own.m21())));
      const QPointF centre = toBuffer.map(particle.position);
      m.cx = float(centre.x()); m.cy = float(centre.y());
      m.radius = float(r);
      reach = own.mapRect(QRectF(-r, -r, 2 * r, 2 * r)).translated(centre).adjusted(-1, -1, 1, 1);
    } else if (shape == 2 || shape == 5 || shape == 7) {
      // A square, a star, a piece of confetti: shapes in their own turned
      // units -- confetti squeezed by its tumble -- as paintParticle draws
      // them. Only the card writes these (see ParticleCardHook).
      const double r = particle.size / 2;
      QTransform own;
      if (shape == 7) own.scale(1, std::cos(particle.flutter * 1.7));
      own = own * QTransform().rotate(particle.rotation) * linear;
      bool ownInvertible = false;
      const QTransform ownBack = own.inverted(&ownInvertible);
      if (!ownInvertible) continue;
      m.i11 = float(ownBack.m11()); m.i12 = float(ownBack.m21()); m.i21 = float(ownBack.m12()); m.i22 = float(ownBack.m22());
      m.pixelScale = float(std::sqrt(std::abs(own.m11() * own.m22() - own.m12() * own.m21())));
      const QPointF centre = toBuffer.map(particle.position);
      m.cx = float(centre.x()); m.cy = float(centre.y());
      m.radius = float(r);
      reach = own.mapRect(QRectF(-r, -r, 2 * r, 2 * r)).translated(centre).adjusted(-1, -1, 1, 1);
    } else {
      double radius = particle.size / 2;
      // Smaller than a pixel: drawn as one, and fainter by the area it lacks,
      // so a field of fine dust keeps its brightness at any zoom.
      const double across = radius * pixelScale;
      if (across < .75) { alpha *= float((across / .75) * (across / .75)); radius = .75 / pixelScale; }
      const QPointF centre = toBuffer.map(particle.position);
      m.cx = float(centre.x()); m.cy = float(centre.y());
      m.radius = float(radius);
      reach = linear.mapRect(QRectF(-radius, -radius, 2 * radius, 2 * radius)).translated(centre).adjusted(-1, -1, 1, 1);
    }
    if (!reach.intersects(bufferRect)) continue;
    m.a = alpha;
    m.left = int(std::floor(reach.left())); m.right = int(std::ceil(reach.right()));
    m.top = int(std::floor(reach.top())); m.bottom = int(std::ceil(reach.bottom()));
    if (outlines && (shape == 2 || shape == 5 || shape == 7))
      outlines->shapes.push_back(outlineOf(particle, shape, toBuffer, bufferRect.toAlignedRect(),
                                           QRect(QPoint(m.left, m.top), QPoint(m.right, m.bottom)), outlines->levels));
    marks.push_back(m);
  }
  return true;
}
thread_local const ParticleCardHook *g_particleCard = nullptr;
// Particles drawn by a graphics card: worked out here, written there, all of
// them at once. Drawn through the card's painter as a picture they were
// written on the CPU and the picture sent to the card every frame -- slower
// than the whole frame drawn on the CPU. Only where the painter has no image
// behind it, the layer is flat (a projected plane goes the ordinary way), it
// lands over the picture or adds to it, and anything it is cut to is a
// rectangle, which the card can cut to as well.
bool drawOnCard(QPainter &p, const std::vector<ParticleState> &states, int shape, bool add, QRect area) {
  if (!g_particleCard || !*g_particleCard) return false;
  QPaintDevice *device = p.device();
  if (!device || device->devType() == QInternal::Image || device->devType() == QInternal::Picture ||
      device->devicePixelRatioF() != 1)
    return false;
  if (!add && p.compositionMode() != QPainter::CompositionMode_SourceOver) return false;
  const QTransform world = p.worldTransform();
  if (!world.isAffine()) return false;
  if (!painterClipIsDeviceRect(p)) return false;
  std::vector<Mark> marks;
  ParticleOutlines outlines;
  const bool outlined = shape == 2 || shape == 5 || shape == 7;
  if (!buildMarks(states, shape, world, QRectF(area), marks, outlined ? &outlines : nullptr)) return true; // nothing to see
  if (marks.empty()) return true;
  return (*g_particleCard)(p, marks, outlines, area, shape, add);
}
// Draws these particles through the painter as it is: a layer's own, or the
// composition's for a volume layer's particles already seen by the camera.
void drawStates(QPainter &p, const std::vector<ParticleState> *states, int shape, bool add) {
  if (states->empty()) return;
  const QTransform world = p.worldTransform();
  QPaintDevice *device = p.device();
  // Projected onto a 3D plane: particle by particle, as QPainter draws them.
  if (!world.isAffine() || !device || device->width() <= 0 || device->height() <= 0) {
    const QRectF clip = p.hasClipping() ? p.clipBoundingRect() : QRectF();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    if (add) p.setCompositionMode(QPainter::CompositionMode_Plus);
    for (const auto &particle : *states) {
      const double reach = reachOf(particle, shape);
      if (!clip.isNull() && !clip.intersects(QRectF(particle.position.x() - reach, particle.position.y() - reach, 2 * reach, 2 * reach)))
        continue;
      paintParticle(p, particle, shape, world);
    }
    p.restore();
    return;
  }
  // Where they land, cut to what is being drawn: a band of the frame, a zoomed view.
  double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
  for (const auto &particle : *states) {
    const double reach = reachOf(particle, shape);
    left = std::min(left, particle.position.x() - reach); right = std::max(right, particle.position.x() + reach);
    top = std::min(top, particle.position.y() - reach); bottom = std::max(bottom, particle.position.y() + reach);
  }
  QRectF target = world.mapRect(QRectF(QPointF(left, top), QPointF(right, bottom)));
  target &= QRectF(0, 0, device->width(), device->height());
  if (p.hasClipping()) target &= world.mapRect(p.clipBoundingRect());
  const QRect area = target.toAlignedRect() & QRect(0, 0, device->width(), device->height());
  if (area.isEmpty()) return;
  if (drawOnCard(p, *states, shape, add, area)) return;
  // Deep colour keeps its depth; everything else is drawn in eight bits.
  QImage::Format format = QImage::Format_ARGB32_Premultiplied;
  if (device->devType() == QInternal::Image) {
    const QImage::Format deep = static_cast<QImage *>(device)->format();
    if (deep == QImage::Format_RGBA64_Premultiplied || deep == QImage::Format_RGBA16FPx4_Premultiplied ||
        deep == QImage::Format_RGBA32FPx4_Premultiplied)
      format = deep;
  }
  // At the device's own resolution, so a high-density screen is not given a
  // picture drawn at a fraction of its pixels.
  const qreal ratio = std::max<qreal>(1, device->devicePixelRatioF());
  QImage buffer((QSizeF(area.size()) * ratio).toSize(), format);
  if (buffer.isNull()) return;
  buffer.fill(Qt::transparent);
  const QTransform toBuffer = world * QTransform::fromTranslate(-area.left(), -area.top()) * QTransform::fromScale(ratio, ratio);
  // Petals too: an ellipse turned and squeezed by its flutter is a circle in
  // its own turned, squeezed units, so it is written the same way. Drawn by
  // QPainter one at a time, a few hundred of them cost more than the picture.
  const bool direct = format == QImage::Format_ARGB32_Premultiplied &&
                      (shape == 0 || shape == 1 || shape == 3 || shape == 4 || shape == 6);
  if (direct) {
    std::vector<Mark> marks;
    if (!buildMarks(*states, shape, toBuffer, QRectF(0, 0, buffer.width(), buffer.height()), marks)) return;
    // Rows are independent: split over the cores when this is not already one
    // band of a frame being drawn on all of them.
    renderParallel(buffer.height(), [&](int from, int to) { writeMarks(buffer, from, to, marks, shape, add); });
  } else {
    QPainter q(&buffer);
    q.setRenderHint(QPainter::Antialiasing, true);
    q.setPen(Qt::NoPen);
    if (add) q.setCompositionMode(QPainter::CompositionMode_Plus);
    q.setWorldTransform(toBuffer);
    bool invertible = false;
    const QRectF visible = toBuffer.inverted(&invertible).mapRect(QRectF(0, 0, buffer.width(), buffer.height()));
    for (const auto &particle : *states) {
      const double reach = reachOf(particle, shape);
      if (invertible && !visible.intersects(QRectF(particle.position.x() - reach, particle.position.y() - reach, 2 * reach, 2 * reach)))
        continue;
      paintParticle(q, particle, shape, toBuffer);
    }
  }
  buffer.setDevicePixelRatio(ratio);
  // The layer as one picture: its opacity and blend mode apply to the whole of
  // it, as they do to any other layer, rather than to each particle over the last.
  p.save();
  p.resetTransform();
  if (add) p.setCompositionMode(QPainter::CompositionMode_Plus);
  p.drawImage(QPointF(area.topLeft()), buffer);
  p.restore();
}
}  // namespace

ParticleCardScope::ParticleCardScope(const ParticleCardHook *hook) : previous(g_particleCard) { g_particleCard = hook; }
ParticleCardScope::~ParticleCardScope() { g_particleCard = previous; }

void drawParticles(QPainter &p, const Layer &l, const Scene &s, double frame) {
  const auto states = particleStates(l, s, frame); // held: the list lives as long as this
  drawStates(p, states.get(), std::clamp(int(particleSetting(l, "shape")), 0, 7), particleSetting(l, "blend") >= .5);
}

bool particleVolume(const Layer &l) {
  return l.threeD && l.type == "particles" && layerEmitsParticles(l) &&
         (particleSetting(l, "depth") > 0 || particleSetting(l, "speedZ") != 0);
}

void drawParticleVolume(QPainter &p, const Layer &l, const Scene &s, double frame, double nearDepth, double farDepth) {
  const auto states = particleStates(l, s, frame);
  if (states->empty()) return;
  const double at = l.start + frame;
  double zoom = 1;
  const QMatrix4x4 m = layerCameraMatrix(l, s, at, zoom);
  // How much bigger a layer pixel is in the scene: the layer's own scale and
  // its parents', taken from its axes as the camera sees them.
  const double scale = (QVector3D(m(0, 0), m(1, 0), m(2, 0)).length() + QVector3D(m(0, 1), m(1, 1), m(2, 1)).length()) / 2;
  const double cx = s.width / 2., cy = s.height / 2.;
  std::vector<std::pair<double, ParticleState>> seen;
  seen.reserve(states->size());
  // Where a particle will be a hundredth of a second on, seen on screen: its
  // velocity there, which a streak is drawn along.
  constexpr double step = .01;
  for (const auto &particle : *states) {
    const QVector3D v = m.map(QVector3D(float(particle.position.x()), float(particle.position.y()), float(particle.z)));
    const double depth = v.z();
    if (depth <= 1 || depth < nearDepth || depth >= farDepth) continue;
    ParticleState onScreen = particle;
    onScreen.position = QPointF(cx + zoom * v.x() / depth, cy + zoom * v.y() / depth);
    onScreen.size = particle.size * zoom * scale / depth;
    const QVector3D ahead = m.map(QVector3D(float(particle.position.x() + particle.velocity.x() * step),
                                            float(particle.position.y() + particle.velocity.y() * step),
                                            float(particle.z + particle.velocityZ * step)));
    if (ahead.z() > 1) {
      const QPointF next(cx + zoom * ahead.x() / ahead.z(), cy + zoom * ahead.y() / ahead.z());
      onScreen.velocity = (next - onScreen.position) / step;
    }
    if (onScreen.size <= 0) continue;
    seen.push_back({depth, onScreen});
  }
  if (seen.empty()) return;
  // Far to near, so the nearer covers the farther.
  std::stable_sort(seen.begin(), seen.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  std::vector<ParticleState> ordered;
  ordered.reserve(seen.size());
  for (auto &entry : seen) ordered.push_back(std::move(entry.second));
  drawStates(p, &ordered, std::clamp(int(particleSetting(l, "shape")), 0, 7), particleSetting(l, "blend") >= .5);
}

bool particleVolumeDepths(const Layer &l, const Scene &s, double frame, double &nearest, double &farthest) {
  const auto states = particleStates(l, s, frame);
  double zoom = 1;
  const QMatrix4x4 m = layerCameraMatrix(l, s, l.start + frame, zoom);
  nearest = 1e300;
  farthest = -1e300;
  for (const auto &particle : *states) {
    const double depth = m.map(QVector3D(float(particle.position.x()), float(particle.position.y()), float(particle.z))).z();
    if (depth <= 1) continue;
    nearest = std::min(nearest, depth);
    farthest = std::max(farthest, depth);
  }
  return nearest <= farthest;
}

QStringList particlePresetNames() { return {"Sparkle", "Snow", "Petals", "Bokeh", "Sparks", "Confetti", "Dust", "Fireflies"}; }

void applyParticlePreset(Layer &l, const Scene &s, const QString &name) {
  auto set = [&](int property, double value) { l.p[size_t(property)] = Property{}; l.p[size_t(property)].base = value; };
  QJsonObject o;
  const double w = s.width, h = s.height;
  // A preset places a particle layer's emitter in the frame. Given to a title
  // or a shape, it leaves the layer where it is.
  const double keepX = l.p[0].base, keepY = l.p[1].base;
  if (name == "Sparkle") {
    // Glints over whatever the layer is: over its own area on a title or a
    // shape, over a box on a particle layer of its own.
    o = {{"emitter", l.type == "particles" ? 2 : 5}, {"emitterWidth", 400}, {"emitterHeight", 200}, {"shape", 5},
         {"colorStart", "#ffffff"}, {"colorEnd", "#ffd76a"}, {"sizeVariance", 60}, {"lifeVariance", 40}, {"sizeEnd", 0},
         {"fadeIn", 25}, {"fadeOut", 40}, {"turbulenceFrequency", .8}, {"seed", 17}, {"blend", 1}};
    set(ParticleRate, 18); set(ParticleLife, 1.2); set(ParticleSpeed, 12); set(ParticleDirection, 0); set(ParticleSpread, 360);
    set(ParticleSize, 18); set(ParticleGravity, 0); set(ParticleWind, 0); set(ParticleTurbulence, 6); set(ParticleSpin, 120);
    l.p[1].base = h / 2;
  } else if (name == "Snow") {
    o = {{"emitter", 1}, {"emitterWidth", w * 1.3}, {"shape", 0}, {"colorStart", "#ffffff"}, {"colorEnd", "#dde8ff"},
         {"sizeVariance", 60}, {"speedVariance", 40}, {"lifeVariance", 20}, {"fadeIn", 5}, {"fadeOut", 15}, {"opacity", 90},
         {"opacityVariance", 40}, {"turbulenceFrequency", .4}, {"preroll", 8}, {"seed", 7}};
    set(ParticleRate, 60); set(ParticleLife, 8); set(ParticleSpeed, 90); set(ParticleDirection, 180); set(ParticleSpread, 20);
    set(ParticleSize, 9); set(ParticleGravity, 4); set(ParticleWind, 8); set(ParticleTurbulence, 40); set(ParticleSpin, 0);
    l.p[1].base = -h * .05;
  } else if (name == "Petals") {
    o = {{"emitter", 1}, {"emitterWidth", w * 1.2}, {"shape", 4}, {"colorStart", "#ffc4dc"}, {"colorEnd", "#ff9ec4"},
         {"colorVariance", 6}, {"sizeVariance", 35}, {"speedVariance", 35}, {"fadeIn", 5}, {"fadeOut", 15},
         {"turbulenceFrequency", .6}, {"preroll", 7}, {"seed", 3}};
    set(ParticleRate, 14); set(ParticleLife, 9); set(ParticleSpeed, 70); set(ParticleDirection, 160); set(ParticleSpread, 30);
    set(ParticleSize, 22); set(ParticleGravity, 10); set(ParticleWind, 20); set(ParticleTurbulence, 60); set(ParticleSpin, 90);
    l.p[1].base = -h * .06;
  } else if (name == "Bokeh") {
    o = {{"emitter", 2}, {"emitterWidth", w}, {"emitterHeight", h}, {"shape", 6}, {"colorStart", "#ffd89a"}, {"colorEnd", "#ff9ad0"},
         {"colorVariance", 18}, {"sizeVariance", 60}, {"lifeVariance", 40}, {"fadeIn", 30}, {"fadeOut", 35}, {"opacity", 70},
         {"turbulenceFrequency", .25}, {"preroll", 6}, {"seed", 11}, {"blend", 1}};
    set(ParticleRate, 5); set(ParticleLife, 6); set(ParticleSpeed, 12); set(ParticleDirection, 0); set(ParticleSpread, 360);
    set(ParticleSize, 110); set(ParticleGravity, 0); set(ParticleWind, 0); set(ParticleTurbulence, 30); set(ParticleSpin, 0);
    l.p[1].base = h / 2;
  } else if (name == "Sparks") {
    o = {{"emitter", 0}, {"shape", 3}, {"colorStart", "#fff2b0"}, {"colorEnd", "#ff5a1a"}, {"sizeVariance", 50},
         {"speedVariance", 45}, {"lifeVariance", 40}, {"sizeEnd", 30}, {"fadeIn", 0}, {"fadeOut", 50}, {"drag", 1.2}, {"seed", 5},
         {"blend", 1}};
    set(ParticleRate, 120); set(ParticleLife, 1.4); set(ParticleSpeed, 520); set(ParticleDirection, 0); set(ParticleSpread, 70);
    set(ParticleSize, 6); set(ParticleGravity, 600); set(ParticleWind, 0); set(ParticleTurbulence, 0); set(ParticleSpin, 0);
    l.p[1].base = h * .75;
  } else if (name == "Confetti") {
    o = {{"emitter", 1}, {"emitterWidth", w}, {"shape", 7}, {"colorStart", "#ff4d6d"}, {"colorEnd", "#ff4d6d"},
         {"colorVariance", 100}, {"sizeVariance", 30}, {"speedVariance", 50}, {"fadeIn", 0}, {"fadeOut", 10}, {"drag", .8},
         {"turbulenceFrequency", .8}, {"burst", 150}, {"seed", 9}};
    set(ParticleRate, 25); set(ParticleLife, 5); set(ParticleSpeed, 160); set(ParticleDirection, 180); set(ParticleSpread, 50);
    set(ParticleSize, 16); set(ParticleGravity, 120); set(ParticleWind, 0); set(ParticleTurbulence, 50); set(ParticleSpin, 240);
    l.p[1].base = -h * .05;
  } else if (name == "Dust") {
    o = {{"emitter", 2}, {"emitterWidth", w}, {"emitterHeight", h}, {"shape", 0}, {"colorStart", "#fff4e0"}, {"colorEnd", "#fff4e0"},
         {"sizeVariance", 70}, {"lifeVariance", 50}, {"fadeIn", 40}, {"fadeOut", 40}, {"opacity", 55}, {"opacityVariance", 50},
         {"turbulenceFrequency", .3}, {"preroll", 8}, {"seed", 21}, {"blend", 1}};
    set(ParticleRate, 30); set(ParticleLife, 8); set(ParticleSpeed, 6); set(ParticleDirection, 0); set(ParticleSpread, 360);
    set(ParticleSize, 5); set(ParticleGravity, -2); set(ParticleWind, 3); set(ParticleTurbulence, 25); set(ParticleSpin, 0);
    l.p[1].base = h / 2;
  } else if (name == "Fireflies") {
    o = {{"emitter", 2}, {"emitterWidth", w * .9}, {"emitterHeight", h * .6}, {"shape", 0}, {"colorStart", "#d8ff7a"},
         {"colorEnd", "#9dff5a"}, {"colorVariance", 8}, {"sizeVariance", 40}, {"lifeVariance", 40}, {"fadeIn", 35}, {"fadeOut", 45},
         {"turbulenceFrequency", .5}, {"preroll", 5}, {"seed", 13}, {"blend", 1}};
    set(ParticleRate, 8); set(ParticleLife, 5); set(ParticleSpeed, 20); set(ParticleDirection, 0); set(ParticleSpread, 360);
    set(ParticleSize, 18); set(ParticleGravity, -4); set(ParticleWind, 0); set(ParticleTurbulence, 70); set(ParticleSpin, 0);
    l.p[1].base = h * .55;
  } else {
    return;
  }
  l.p[0].base = w / 2;
  if (l.type != "particles") { l.p[0].base = keepX; l.p[1].base = keepY; }
  l.particles = o;
  // A particle layer of its own is made in depth from the start: its particles
  // near and far, a 3D layer standing among them.
  if (l.type == "particles") makeParticleLayerDeep(l, s);
}

bool makeParticleLayerDeep(Layer &l, const Scene &s) {
  if (l.type != "particles" || particleVolume(l)) return false;
  const int shape = std::clamp(int(particleSetting(l, "shape")), 0, 7);
  const int emitter = std::clamp(int(particleSetting(l, "emitter")), 0, 5);
  l.threeD = true;
  l.particles["depth"] = std::round((shape == 3 || shape == 7 ? .4 : .8) * std::max(1, s.width));
  // A wider emitter, a quarter more each way it has, and as much more flow as
  // that is more area, so the frame holds as many as before.
  constexpr double grow = 1.25;
  double more = 1;
  if (emitter >= 1 && emitter <= 4) {
    l.particles["emitterWidth"] = particleSetting(l, "emitterWidth") * grow;
    more *= grow;
    if (emitter >= 2) {
      l.particles["emitterHeight"] = particleSetting(l, "emitterHeight") * grow;
      more *= grow;
    }
  }
  if (more != 1) {
    Property &rate = l.p[ParticleRate];
    rate.base *= more;
    for (auto &k : rate.keys) k.value *= more;
  }
  return true;
}
