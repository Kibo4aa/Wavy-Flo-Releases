#include "effect_packs.h"
#include "render_workers.h"
#include "shader_cpu.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QPainterPath>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cstddef>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace {
using EffectMap = QHash<QString, const PackEffect *>;
struct Registry {
  std::vector<std::unique_ptr<PackEffect>> effects; // in the order they were loaded
  // Looked up by every thread that draws, while an effect written as text may
  // be read again on the interface thread: so never changed in place, only
  // replaced whole (publish). A map replaced, and an effect replaced or taken
  // away, are kept a while -- a lookup or a frame may be in one still.
  std::atomic<const EffectMap *> byId{nullptr};
  std::vector<std::pair<qint64, std::unique_ptr<const EffectMap>>> maps;     // when replaced (0: current)
  std::vector<std::pair<qint64, std::unique_ptr<PackEffect>>> retired;       // when retired
  QSet<QString> files;
  QStringList problems; // packs refused when loaded
  // Effects written as text, by file: the id each provides, and why one does
  // not load as it is now (the version that last did stays in use).
  QHash<QString, QString> textIds;
  QMap<QString, QString> textProblems;
  std::vector<std::unique_ptr<QLibrary>> libraries; // kept loaded for the life of the process
};
Registry &registry() {
  static Registry r;
  return r;
}
const EffectMap &ids() {
  static const EffectMap none;
  const EffectMap *map = registry().byId.load(std::memory_order_acquire);
  return map ? *map : none;
}
qint64 nowMs() {
  static QElapsedTimer clock;
  static std::once_flag started;
  std::call_once(started, [] { clock.start(); });
  return clock.elapsed();
}
// What is kept for a lookup or a frame still under way is let go after a while
// -- long after any could still be in it.
constexpr qint64 kKeepRetiredMs = 5 * 60 * 1000;
// The effects as they are now, for every thread to look up. Interface thread only.
void publish(Registry &r) {
  auto next = std::make_unique<EffectMap>();
  for (const auto &e : r.effects) next->insert(e->id, e.get());
  const qint64 now = nowMs();
  for (auto &[replaced, map] : r.maps)
    if (replaced == 0) replaced = now;
  r.byId.store(next.get(), std::memory_order_release);
  r.maps.push_back({0, std::move(next)});
  r.maps.erase(std::remove_if(r.maps.begin(), r.maps.end(),
                              [&](const auto &m) { return m.first != 0 && now - m.first > kKeepRetiredMs; }),
               r.maps.end());
  r.retired.erase(std::remove_if(r.retired.begin(), r.retired.end(),
                                 [&](const auto &e) { return now - e.first > kKeepRetiredMs; }),
                  r.retired.end());
}
// Each effect read again while running is told apart from the one before it,
// for the caches that keep what a layer with it looked like.
std::atomic<quint64> &revisions() {
  static std::atomic<quint64> counter{0};
  return counter;
}

// A pack made for an earlier SDK describes itself with what that SDK had:
// everything up to the fields the next one added at the end.
constexpr uint32_t kFiveSize = uint32_t(offsetof(WfxEffect, reachWithData));
constexpr uint32_t kSixSize = uint32_t(offsetof(WfxEffect, help));
constexpr uint32_t kSevenSize = uint32_t(offsetof(WfxEffect, shaderPasses));
QString validate(const WfxEffect *e) {
  if (!e) return "no description";
  const bool five = e->abiVersion == 5 && e->structSize == kFiveSize;
  const bool six = e->abiVersion == 6 && e->structSize == kSixSize;
  const bool modern = (e->abiVersion == 7 && e->structSize == kSevenSize) ||
                      (e->abiVersion >= 8 && e->abiVersion <= 10 && e->structSize == sizeof(WfxEffect));
  if (!five && !six && !modern) return "made for a different SDK version";
  if (!e->id || !*e->id || !e->name || !*e->name) return "an effect without a name";
  // From SDK 10 a shader alone is enough: the processor runs the same shader.
  if (!e->process && !(e->abiVersion >= 10 && e->shader && *e->shader)) return "an effect with nothing to run";
  if (!(e->supportedDepths & 7u)) return "an effect that takes no pixel format";
  if (e->parameterCount < 0 || e->parameterCount > 64 || (e->parameterCount && !e->parameters)) return "a bad parameter list";
  int open = 0; // groups not closed yet
  for (int i = 0; i < e->parameterCount; ++i) {
    const auto &p = e->parameters[i];
    const uint32_t newest = modern ? WFX_POINT_3D : six ? WFX_BUTTON : WFX_CURVE;
    if (p.structSize != sizeof(WfxParameter)) return "a bad parameter";
    if (p.kind < WFX_FLOAT || p.kind > newest) return QString("parameter %1 of an unknown kind").arg(p.id ? p.id : "?");
    // Groups only arrange the controls: no id to save, no range.
    if (p.kind == WFX_GROUP) {
      if (!p.label || !*p.label) return "a group without a name";
      ++open;
      continue;
    }
    if (p.kind == WFX_GROUP_END) {
      if (--open < 0) return "a group closed that was never opened";
      continue;
    }
    if (!p.id || !*p.id || !p.label) return "a bad parameter";
    if (!std::isfinite(p.minimum) || !std::isfinite(p.maximum) || !std::isfinite(p.initial) || p.minimum > p.maximum)
      return QString("parameter %1 with an impossible range").arg(p.id);
    if (p.kind == WFX_CHOICE && (!p.choices || !*p.choices)) return QString("choice %1 without choices").arg(p.id);
    for (int j = 0; j < i; ++j)
      if (e->parameters[j].id && qstrcmp(e->parameters[j].id, p.id) == 0) return QString("two parameters called %1").arg(p.id);
  }
  if (open != 0) return "a group left open";
  return {};
}

void hostParallelFor(int32_t count, void (*job)(int32_t, int32_t, void *), void *user) {
  renderParallel(count, [job, user](int begin, int end) { job(begin, end, user); });
}
// A layer's data for one call: the bytes, and the list pointing at them.
struct DataHold {
  std::vector<QByteArray> ids, bytes;
  std::vector<WfxData> items;
  void fill(const Layer *l, const PackEffect &e) {
    if (e.dataIds.isEmpty()) return;
    for (const QString &id : e.dataIds) {
      ids.push_back(id.toUtf8());
      bytes.push_back(l ? packEffectData(*l, e, id) : QByteArray());
    }
    for (size_t i = 0; i < ids.size(); ++i)
      items.push_back(WfxData{ids[i].constData(),
                              bytes[i].isEmpty() ? nullptr : reinterpret_cast<const uint8_t *>(bytes[i].constData()),
                              int32_t(bytes[i].size())});
  }
};
WfxContext contextFor(int depth, QRectF box, double scale, double seconds, double frame, const DataHold *data = nullptr) {
  WfxContext context{};
  context.structSize = sizeof(WfxContext);
  context.depth = depth;
  context.seconds = seconds;
  context.frame = frame;
  context.scale = scale;
  context.boxX = box.x(); context.boxY = box.y(); context.boxWidth = box.width(); context.boxHeight = box.height();
  context.parallelFor = hostParallelFor;
  if (data && !data->items.empty()) {
    context.dataCount = int32_t(data->items.size());
    context.data = data->items.data();
  }
  return context;
}

// A colour kept in a project, as four floats.
std::array<float, 4> colourOf(const QJsonValue &stored, const WfxParameter &p) {
  const QColor colour = stored.isString() ? QColor(stored.toString()) : QColor();
  if (!colour.isValid()) return {p.initialColor[0], p.initialColor[1], p.initialColor[2], p.initialColor[3]};
  return {float(colour.redF()), float(colour.greenF()), float(colour.blueF()), float(colour.alphaF())};
}
}  // namespace

namespace {
// One effect of a pack, as the host keeps it; null with why when it cannot be used.
std::unique_ptr<PackEffect> makePackEffect(const WfxEffect *api, const QString &path, QString *problem) {
  if (const QString why = validate(api); !why.isEmpty()) {
    if (problem) *problem = why;
    return nullptr;
  }
  auto effect = std::make_unique<PackEffect>();
  effect->api = api;
  effect->id = QString::fromUtf8(api->id);
  effect->name = QString::fromUtf8(api->name);
  effect->category = api->category && *api->category ? QString::fromUtf8(api->category) : QString("Plugins");
  effect->file = path;
  for (int p = 0; p < api->parameterCount; ++p) {
    effect->offsets.push_back(effect->valueCount);
    effect->valueCount += wfxParameterFloats(&api->parameters[p]);
    if (api->parameters[p].kind == WFX_DATA) effect->dataIds << QString::fromUtf8(api->parameters[p].id);
  }
  effect->inert = api->inert;
  effect->varies = api->varies;
  effect->reach = api->reach;
  if (api->abiVersion >= 6) {
    effect->reachWithData = api->reachWithData;
    effect->pressed = api->pressed;
    effect->describe = api->describe;
  }
  if (api->abiVersion >= 7) {
    effect->help = api->help;
    effect->controlState = api->controlState;
  }
  if (api->abiVersion >= 8) effect->shaderPasses = api->shaderPasses;
  if (api->shader && *api->shader) {
    effect->shader = QString::fromUtf8(api->shader);
    if (api->abiVersion >= 10) effect->shader = packShaderPrelude(api->parameters, api->parameterCount) + effect->shader;
  }
  // A shader and nothing else: compiled for the processor now, so one that
  // cannot be is refused here, with where and why, rather than drawing
  // nothing on every frame.
  if (!api->process) {
    QString why;
    effect->cpuProgram = cpuShaderProgram(effect->shader, &why);
    if (!effect->cpuProgram) {
      if (problem) *problem = effect->id + ": its shader does not compile: " + why;
      return nullptr;
    }
  }
  return effect;
}
}  // namespace

namespace {
// The loaded effect with this id, published yet or not.
const PackEffect *providerOf(const Registry &r, const QString &id) {
  for (const auto &e : r.effects)
    if (e->id == id) return e.get();
  return nullptr;
}
// The effect written as text in `path` from `bytes`, in place of the version
// that file gave before, if any. False, with why, when it cannot be used: the
// version before stays. `revision` tells this version apart (0 at start-up).
bool installTextEffect(Registry &r, const QString &path, const QByteArray &bytes, QString &problem, quint64 revision) {
  auto effect = readTextEffect(bytes, path, &problem);
  if (!effect) return false;
  const QString before = r.textIds.value(path);
  if (const PackEffect *other = providerOf(r, effect->id); other && other->file != path) {
    problem = effect->id + " is already provided by " + QFileInfo(other->file).fileName();
    return false;
  }
  effect->revision = revision;
  r.textIds[path] = effect->id;
  // Where the version before was in the list, so the browser keeps its order.
  const auto at = std::find_if(r.effects.begin(), r.effects.end(), [&](const auto &e) { return !before.isEmpty() && e->id == before; });
  if (at != r.effects.end()) {
    r.retired.push_back({nowMs(), std::move(*at)});
    *at = std::move(effect);
  } else {
    r.effects.push_back(std::move(effect));
  }
  return true;
}
}  // namespace

QStringList effectFilesIn(const QString &folder, const QStringList &patterns, bool subfolders) {
  const QDir dir(folder);
  if (!subfolders) return dir.entryList(patterns, QDir::Files, QDir::Name);
  QStringList found;
  // Folder by folder, so a bundle's insides are never walked into.
  QStringList waiting{dir.absolutePath()};
  while (!waiting.isEmpty()) {
    const QDir here(waiting.takeFirst());
    for (const QString &name : here.entryList(patterns, QDir::Files, QDir::Name))
      found << dir.relativeFilePath(here.filePath(name));
    for (const QString &inner : here.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
      if (!inner.endsWith(".ofx.bundle", Qt::CaseInsensitive)) waiting << here.filePath(inner);
  }
  // The folder's own first, then each subfolder's, by path.
  std::stable_sort(found.begin(), found.end(), [](const QString &a, const QString &b) {
    const auto depthA = a.count('/'), depthB = b.count('/');
    if ((depthA == 0) != (depthB == 0)) return depthA == 0;
    return a.compare(b, Qt::CaseInsensitive) < 0;
  });
  return found;
}

int loadEffectPacks(const QString &folder, bool textOnly) {
  auto &r = registry();
  int added = 0;
  const qsizetype problemsBefore = r.problems.size();
  const QDir dir(folder);
  // The program's effects folder is read the way After Effects reads its
  // Plug-ins folder: a pack in a maker's folder of its own is found too.
  for (const QString &name : textOnly ? QStringList() : effectFilesIn(folder, {"*.wfx"}, true)) {
    const QString path = QFileInfo(dir.filePath(name)).canonicalFilePath();
    if (path.isEmpty() || r.files.contains(path)) continue;
    r.files.insert(path);
    auto library = std::make_unique<QLibrary>(path);
    if (!library->load()) {
      r.problems << name + ": " + library->errorString();
      continue;
    }
    using Count = int32_t (*)();
    using At = const WfxEffect *(*)(int32_t);
    const auto count = reinterpret_cast<Count>(library->resolve("wfxEffectCount"));
    const auto at = reinterpret_cast<At>(library->resolve("wfxEffectAt"));
    // A .wfx without these is an older single-effect plugin, which the
    // plugin host loads through its own entry points. Not a problem here.
    if (!count || !at) { library->unload(); continue; }
    const int32_t n = count();
    for (int32_t i = 0; i < std::clamp(n, 0, 256); ++i) {
      QString problem;
      auto effect = makePackEffect(at(i), path, &problem);
      if (!effect) {
        r.problems << name + ": " + problem;
        continue;
      }
      if (const PackEffect *other = providerOf(r, effect->id)) {
        r.problems << name + ": " + effect->id + " is already provided by " + QFileInfo(other->file).fileName();
        continue;
      }
      r.effects.push_back(std::move(effect));
      ++added;
    }
    r.libraries.push_back(std::move(library));
  }
  // Said where an author can read it: an effect that does not load is not in
  // the browser, and nothing on screen would say why.
  for (qsizetype i = problemsBefore; i < r.problems.size(); ++i) qWarning().noquote() << "Effect not loaded:" << r.problems[i];
  // Effects written as text: a description and a shader, nothing to build.
  // The person's own folder at its top only, where the editor watches it.
  for (const QString &name : effectFilesIn(folder, {"*.wfxs"}, !textOnly)) {
    const QString path = QFileInfo(dir.filePath(name)).canonicalFilePath();
    if (path.isEmpty() || r.files.contains(path)) continue;
    r.files.insert(path);
    QFile file(path);
    QString problem = "cannot be read";
    if (file.open(QIODevice::ReadOnly) && installTextEffect(r, path, file.readAll(), problem, 0)) {
      ++added;
      continue;
    }
    r.textProblems[path] = problem;
    qWarning().noquote() << "Effect not loaded:" << name + ": " + problem;
  }
  publish(r);
  return added;
}

QString userEffectsFolder() {
  // Somewhere else when asked (the self-tests, a shared folder of effects).
  if (const QString elsewhere = qEnvironmentVariable("WAVY_EFFECTS_FOLDER"); !elsewhere.isEmpty()) return QDir::cleanPath(elsewhere);
  return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/Wavy-Flo/Effects");
}

QStringList reloadTextEffect(const QString &file, const QByteArray *text) {
  auto &r = registry();
  // The name it was loaded under; a file taken away has no canonical one.
  QString path = QFileInfo(file).canonicalFilePath();
  if (path.isEmpty()) {
    const QString absolute = QFileInfo(file).absoluteFilePath();
    for (const QString &known : r.files)
      if (known.compare(absolute, Qt::CaseInsensitive) == 0) path = known;
  }
  if (path.isEmpty()) return {};
  const QString before = r.textIds.value(path);
  QByteArray bytes;
  if (text) {
    bytes = *text;
  } else {
    QFile f(path);
    if (!f.exists()) {
      // Taken away: so is its effect. A project using it says so as it would
      // for any effect not installed.
      r.textProblems.remove(path);
      r.files.remove(path);
      r.textIds.remove(path);
      const auto at = std::find_if(r.effects.begin(), r.effects.end(), [&](const auto &e) { return !before.isEmpty() && e->id == before; });
      if (at == r.effects.end()) return {};
      r.retired.push_back({nowMs(), std::move(*at)});
      r.effects.erase(at);
      publish(r);
      return {before};
    }
    if (!f.open(QIODevice::ReadOnly)) {
      r.textProblems[path] = "cannot be read";
      return {};
    }
    bytes = f.readAll();
  }
  r.files.insert(path);
  QString problem;
  // A version of its own even for a file new to the registry: a frame drawn
  // while its id was not found shows the layer without it, and must not be
  // taken for one drawn with it.
  if (!installTextEffect(r, path, bytes, problem, ++revisions())) {
    r.textProblems[path] = problem;
    return {};
  }
  r.textProblems.remove(path);
  publish(r);
  QStringList changed{r.textIds.value(path)};
  if (!before.isEmpty() && before != changed.front()) changed << before;
  return changed;
}

QString textEffectProblem(const QString &file) {
  const auto &r = registry();
  const QString path = QFileInfo(file).canonicalFilePath();
  return r.textProblems.value(path.isEmpty() ? QFileInfo(file).absoluteFilePath() : path);
}

QMap<QString, QString> textEffectProblems() { return registry().textProblems; }

QString textEffectId(const QString &file) {
  const QString path = QFileInfo(file).canonicalFilePath();
  return path.isEmpty() ? QString() : registry().textIds.value(path);
}

const PackEffect *packEffect(const QString &id) { return ids().value(id, nullptr); }
QString packEffectProblem(const WfxEffect *e) { return validate(e); }
std::vector<const PackEffect *> packEffects() {
  std::vector<const PackEffect *> all;
  for (const auto &effect : registry().effects) all.push_back(effect.get());
  return all;
}
QStringList packProblems() {
  const auto &r = registry();
  QStringList all = r.problems;
  for (auto it = r.textProblems.constBegin(); it != r.textProblems.constEnd(); ++it)
    all << QFileInfo(it.key()).fileName() + ": " + it.value();
  return all;
}

QString packCurveId(const QString &effect, const QString &parameter) { return effect + "/" + parameter; }

int packLegacyProperty(const QString &effect, const QString &parameter) {
  // The wipes' sliders were layer properties before the wipes moved into a
  // pack; projects keep their keyframes there, and the timeline shows them.
  if (effect == QLatin1String("Linear Wipe")) {
    if (parameter == QLatin1String("completion")) return WipeLinearCompletion;
    if (parameter == QLatin1String("angle")) return WipeLinearAngle;
    if (parameter == QLatin1String("feather")) return WipeLinearFeather;
  } else if (effect == QLatin1String("Radial Wipe")) {
    if (parameter == QLatin1String("completion")) return WipeRadialCompletion;
    if (parameter == QLatin1String("start")) return WipeRadialStart;
    if (parameter == QLatin1String("feather")) return WipeRadialFeather;
  }
  return -1;
}

QVector<float> packEffectValues(const Layer &l, const PackEffect &e, double frame) {
  QVector<float> values;
  values.reserve(e.valueCount);
  const QJsonObject stored = l.surfaceEffects.value(e.id).toObject();
  for (int i = 0; i < e.api->parameterCount; ++i) {
    const WfxParameter &p = e.api->parameters[i];
    const QString id = QString::fromUtf8(p.id);
    if (wfxParameterFloats(&p) == 0) continue; // data, buttons, groups: no floats
    if (p.kind == WFX_COLOR) {
      for (float channel : colourOf(stored.value(id), p)) values << channel;
      continue;
    }
    if (p.kind == WFX_CURVE) {
      const QJsonArray saved = stored.value(id).toArray();
      for (int k = 0; k < 5; ++k) {
        const double straight = k * .25;
        const double v = k < saved.size() ? saved[k].toDouble(straight) : straight;
        values << float(std::clamp(std::isfinite(v) ? v : straight, 0., 1.));
      }
      continue;
    }
    if (p.kind == WFX_POINT) {
      const QJsonArray saved = stored.value(id).toArray();
      const double x = saved.size() > 0 ? saved[0].toDouble(p.initialColor[0]) : p.initialColor[0];
      const double y = saved.size() > 1 ? saved[1].toDouble(p.initialColor[1]) : p.initialColor[1];
      values << float(x) << float(y);
      continue;
    }
    if (p.kind == WFX_POINT_3D) {
      const QJsonArray saved = stored.value(id).toArray();
      const double x = saved.size() > 0 ? saved[0].toDouble(p.initialColor[0]) : p.initialColor[0];
      const double y = saved.size() > 1 ? saved[1].toDouble(p.initialColor[1]) : p.initialColor[1];
      const double z = saved.size() > 2 ? saved[2].toDouble(p.initialColor[2]) : p.initialColor[2];
      values << float(x) << float(y) << float(z);
      continue;
    }
    double v = p.initial;
    const int legacy = packLegacyProperty(e.id, id);
    const int curve = legacy < 0 ? layerFindExtra(l, packCurveId(e.id, id)) : -1;
    if (legacy >= 0) v = l.p[size_t(legacy)].at(frame);
    else if (curve >= 0) {
      if (const Property *animated = layerProperty(l, curve)) v = animated->at(frame);
    } else if (stored.contains(id)) {
      const QJsonValue s = stored.value(id);
      v = s.isBool() ? (s.toBool() ? 1. : 0.) : s.toDouble(p.initial);
    }
    if (!std::isfinite(v)) v = p.initial;
    // Clamped to what was declared: a project file can be edited by hand, and
    // an effect was promised its range. The wipes' old sliders were never
    // limited, so they are left as they are.
    if (legacy < 0) v = std::clamp(v, double(p.minimum), double(p.maximum));
    if (p.kind == WFX_CHOICE || p.kind == WFX_BOOL || p.kind == WFX_LAYER || p.kind == WFX_MASK) v = std::round(v);
    values << float(v);
  }
  return values;
}

bool packEffectApplied(const Layer &l, const PackEffect &e) {
  return l.appliedEffects.value(e.id).toBool() && !l.hiddenEffects.value(e.id).toBool();
}
QByteArray packEffectData(const Layer &l, const PackEffect &e, const QString &id) {
  const QJsonValue stored = l.surfaceEffects.value(e.id).toObject().value(id);
  return stored.isString() ? QByteArray::fromBase64(stored.toString().toLatin1()) : QByteArray();
}
void setPackEffectData(Layer &l, const PackEffect &e, const QString &id, const QByteArray &bytes) {
  QJsonObject settings = l.surfaceEffects.value(e.id).toObject();
  if (bytes.isEmpty()) settings.remove(id);
  else settings[id] = QString::fromLatin1(bytes.toBase64());
  l.surfaceEffects[e.id] = settings;
}
QString packEffectDescription(const Layer &l, const PackEffect &e) {
  if (!e.describe) return {};
  DataHold data;
  data.fill(&l, e);
  const WfxContext context = contextFor(32, QRectF(), 1, 0, 0, &data);
  QByteArray text(1024, '\0');
  const int32_t length = e.describe(&context, text.data(), int32_t(text.size()));
  return length > 0 ? QString::fromUtf8(text.constData(), std::min<int>(length, int(text.size()))) : QString();
}
bool packEffectInert(const PackEffect &e, const QVector<float> &values) {
  return e.inert && e.inert(values.constData(), int32_t(values.size())) != 0;
}

namespace {
// Every applied pack effect on the layer, cheaply: most layers have none.
template <class F> void eachApplied(const Layer &l, F f) {
  if (l.appliedEffects.isEmpty() || ids().isEmpty()) return;
  for (auto it = l.appliedEffects.constBegin(); it != l.appliedEffects.constEnd(); ++it) {
    if (!it.value().toBool() || l.hiddenEffects.value(it.key()).toBool()) continue;
    if (const PackEffect *e = packEffect(it.key())) f(*e);
  }
}
}  // namespace

bool layerCarriesPackEffect(const Layer &l) {
  bool any = false;
  eachApplied(l, [&](const PackEffect &) { any = true; });
  return any;
}
bool layerHasPackEffect(const Layer &l, double frame) {
  bool any = false;
  eachApplied(l, [&](const PackEffect &e) { if (!any && !packEffectInert(e, packEffectValues(l, e, frame))) any = true; });
  return any;
}
bool layerPackEffectMoves(const Layer &l, double frame) {
  bool moves = false;
  eachApplied(l, [&](const PackEffect &e) {
    if (moves) return;
    const auto values = packEffectValues(l, e, frame);
    if ((e.api->flags & WFX_MOVES_ALONE) && (!e.varies || e.varies(values.constData(), int32_t(values.size()))))
      moves = true;
    // A slider with keys changes the picture too.
    for (int i = 0; !moves && i < e.api->parameterCount; ++i) {
      const QString id = QString::fromUtf8(e.api->parameters[i].id);
      const int legacy = packLegacyProperty(e.id, id);
      if (legacy >= 0) moves = !l.p[size_t(legacy)].keys.empty() || l.p[size_t(legacy)].hasExpression();
      else if (const int curve = layerFindExtra(l, packCurveId(e.id, id)); curve >= 0)
        if (const Property *animated = layerProperty(l, curve)) moves = animated->varies();
    }
  });
  return moves;
}
bool layerPackNeedsWholeLayer(const Layer &l) {
  bool whole = false;
  eachApplied(l, [&](const PackEffect &e) { whole |= (e.api->flags & WFX_WHOLE_LAYER) != 0; });
  return whole;
}
double layerPackReach(const Layer &l) {
  double reach = 0;
  eachApplied(l, [&](const PackEffect &e) {
    if (!e.reach) return;
    // The widest the effect reaches anywhere in the layer's life: at the start
    // and at every key of every animated slider. Asked without a frame so the
    // plan of a frame and the drawing of it cannot disagree.
    std::vector<double> frames{0.};
    for (int i = 0; i < e.api->parameterCount; ++i) {
      const QString id = QString::fromUtf8(e.api->parameters[i].id);
      const int legacy = packLegacyProperty(e.id, id);
      const Property *animated = legacy >= 0 ? &l.p[size_t(legacy)] : nullptr;
      if (!animated)
        if (const int curve = layerFindExtra(l, packCurveId(e.id, id)); curve >= 0) animated = layerProperty(l, curve);
      if (animated)
        for (const auto &key : animated->keys) frames.push_back(key.frame);
    }
    for (double frame : frames) {
      const auto values = packEffectValues(l, e, frame);
      if (packEffectInert(e, values)) continue;
      const double r = e.reach(values.constData(), int32_t(values.size()), 1.);
      if (std::isfinite(r)) reach = std::max(reach, std::min(r, 4096.));
    }
  });
  // An effect whose reach follows its data, and the size of its layer (a
  // bloom that scales with the frame): asked with both. The layer's box is
  // known wherever a frame is being drawn, from the scene that thread draws.
  eachApplied(l, [&](const PackEffect &e) {
    if (!e.reachWithData) return;
    DataHold data;
    data.fill(&l, e);
    QRectF box;
    if (expressionScene) {
      box = layerBounds(l, *expressionScene);
      if (layerHasMainTransform(l)) box = layerMainTransform(l).mapRect(box);
    }
    const WfxContext context = contextFor(32, box, 1, 0, 0, &data);
    const auto values = packEffectValues(l, e, 0);
    const double r = e.reachWithData(values.constData(), int32_t(values.size()), 1., &context);
    if (std::isfinite(r)) reach = std::max(reach, std::min(r, 4096.));
  });
  return reach;
}
QByteArray layerPackIdentity(const Layer &l, double frame) {
  QByteArray identity;
  eachApplied(l, [&](const PackEffect &e) {
    identity += e.id.toUtf8();
    if (e.revision) identity += '#' + QByteArray::number(e.revision);
    const auto values = packEffectValues(l, e, frame);
    identity.append(reinterpret_cast<const char *>(values.constData()), values.size() * qsizetype(sizeof(float)));
    // Its data is part of what it draws: a look changed in its editor is another picture.
    for (const QString &id : e.dataIds)
      identity += QCryptographicHash::hash(packEffectData(l, e, id), QCryptographicHash::Sha1);
  });
  return identity;
}
QByteArray layerPackRevisions(const Layer &l) {
  QByteArray revisions;
  eachApplied(l, [&](const PackEffect &e) {
    if (e.revision) revisions += e.id.toUtf8() + '#' + QByteArray::number(e.revision) + ';';
  });
  return revisions;
}

namespace {
// What a shader reads besides its pixels, from what the effect is told.
cpu_shader::Inputs shaderInputs(const QVector<float> &values, int depth, QRectF box, double scale, double seconds,
                                double frame) {
  cpu_shader::Inputs in;
  in.values = values.constData();
  in.valueCount = int(values.size());
  in.seconds = float(seconds);
  in.frame = float(frame);
  in.scale = float(scale);
  in.box[0] = float(box.x());
  in.box[1] = float(box.y());
  in.box[2] = float(box.width());
  in.box[3] = float(box.height());
  in.depth = depth;
  return in;
}
// How many rounds a shader runs at these values, for the raster the context
// names. An effect with process() leaves a run to it by asking for none, or for
// more than a chain takes; one without runs its shader at least once, at most 64 times.
int shaderRounds(const PackEffect &e, const QVector<float> &values, const WfxContext &context) {
  if (!e.shaderPasses) return 1;
  const int rounds = e.shaderPasses(values.constData(), int32_t(values.size()), &context);
  if (e.api->process) return rounds >= 1 && rounds <= 64 ? rounds : 0;
  return std::clamp(rounds, 1, 64);
}
// A shader-only effect over one strip of the raster, in place.
bool runShaderStrip(QImage &strip, const PackEffect &e, const QVector<float> &values, const WfxContext &context) {
  if (!e.cpuProgram) return false;
  const int rounds = shaderRounds(e, values, context);
  cpu_shader::Inputs in = shaderInputs(values, context.depth, QRectF(context.boxX, context.boxY, context.boxWidth, context.boxHeight),
                                       context.scale, context.seconds, context.frame);
  in.passes = rounds;
  const QImage original = strip;
  QImage out(strip.size(), strip.format());
  for (int round = 0; round < rounds; ++round) {
    in.pass = round;
    if (out.isNull() || !cpu_shader::run(*e.cpuProgram, strip, original, out, in, QPoint(context.originX, context.originY),
                                         QSize(context.rasterWidth, context.rasterHeight)))
      return false;
    std::swap(strip, out);
  }
  return true;
}
}  // namespace

namespace {
thread_local QPoint g_partOrigin;
thread_local QSize g_partRaster;
// Where the raster at hand lies, for an effect's context.
void placePart(WfxContext &context, int width, int height, int top = 0) {
  const bool part = !g_partRaster.isEmpty();
  context.originX = part ? g_partOrigin.x() : 0;
  context.originY = (part ? g_partOrigin.y() : 0) + top;
  context.rasterWidth = part ? g_partRaster.width() : width;
  context.rasterHeight = part ? g_partRaster.height() : height;
}
}  // namespace
void runPackEffect(QImage &straight, const PackEffect &e, const QVector<float> &values, QRectF box, double scale,
                   double seconds, double frame, const Layer *owner) {
  if (straight.isNull()) return;
  runPackEffectInPlace(straight, e, values, box, scale, seconds, frame, owner);
  straight = straight.convertToFormat(QImage::Format_RGBA32FPx4);
}
void runPackEffectInPlace(QImage &image, const PackEffect &e, const QVector<float> &values, QRectF box, double scale,
                          double seconds, double frame, const Layer *owner) {
  if (image.isNull()) return;
  // Float when the effect takes it -- that is how the host works between
  // effects -- otherwise the widest the effect does take.
  const uint32_t depths = e.api->supportedDepths;
  const int depth = (depths & 4u) ? 32 : (depths & 2u) ? 16 : 8;
  if (!e.api->process) {
    // A shader and nothing else (SDK 10): the passes the card would run, run
    // here, on straight float as the card has them. One that fails leaves the
    // picture as it was.
    QImage working = image.convertToFormat(QImage::Format_RGBA32FPx4);
    QImage done = cpuRunPasses(working, packEffectPasses(e, values, depth, working.size(), box, scale, seconds, frame));
    image = done.isNull() ? std::move(working) : std::move(done);
    return;
  }
  const bool premultiplied = e.api->flags & WFX_PREMULTIPLIED;
  const QImage::Format format = depth == 32 ? (premultiplied ? QImage::Format_RGBA32FPx4_Premultiplied : QImage::Format_RGBA32FPx4)
                                : depth == 16 ? (premultiplied ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA64)
                                              : (premultiplied ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBA8888);
  // Already in that format (after another such effect), it is worked on where
  // it is: a copy to convert into would share its pixels, and writing to them
  // would copy the whole picture. A failed effect leaves it as it was.
  const bool already = image.format() == format;
  QImage working = already ? std::move(image) : image.convertToFormat(format);
  DataHold data;
  data.fill(owner, e);
  WfxContext context = contextFor(depth, box, scale, seconds, frame, &data);
  if (!g_partRaster.isEmpty()) placePart(context, working.width(), working.height());
  const int failed = e.api->process(working.bits(), working.width(), working.height(), int32_t(working.bytesPerLine()),
                                    values.constData(), int32_t(values.size()), &context);
  if (failed && !already) return;
  image = std::move(working);
}

bool packEffectPerPixel(const PackEffect &e) {
  return (e.api->flags & WFX_PER_PIXEL) && (e.api->supportedDepths & 4u);
}
bool packEffectPartable(const PackEffect &e) {
  if (e.api->span && e.api->prepare && e.api->release) return true;
  return packEffectPerPixel(e) && e.api->process;
}
RasterPartScope::RasterPartScope(QPoint origin, QSize raster) : previousOrigin(g_partOrigin), previousRaster(g_partRaster) {
  g_partOrigin = origin;
  g_partRaster = raster;
}
RasterPartScope::~RasterPartScope() {
  g_partOrigin = previousOrigin;
  g_partRaster = previousRaster;
}
// Effects that each work pixel by pixel, run as one: the raster a strip of rows
// at a time, every strip widened to float, worked on by all of them and
// narrowed to the format asked for while it is still in the processor's cache,
// the strips spread over the cores. Run on the whole raster, film grain and a
// vignette over a 1080p frame wrote and read a 33 MB float copy five times
// over -- 20 ms, of which the effects were six. Each step is the one the whole
// raster takes, done to fewer pixels: Qt's conversions are pixel by pixel, and
// an effect flagged WFX_PER_PIXEL is told where its strip lies.
bool runPerPixelPacks(QImage &image, QImage::Format outputFormat, const std::vector<PackStep> &steps, QRectF box,
                      double scale, double seconds, double frame, const Layer *owner) {
  if (image.isNull() || steps.empty()) return false;
  for (const PackStep &step : steps)
    if (!step.effect || !packEffectPerPixel(*step.effect)) return false;
  const int width = image.width(), height = image.height();
  std::vector<std::unique_ptr<DataHold>> data;
  std::vector<WfxContext> contexts;
  std::vector<QImage::Format> formats;
  for (const PackStep &step : steps) {
    data.push_back(std::make_unique<DataHold>());
    data.back()->fill(owner, *step.effect);
    WfxContext context = contextFor(32, box, scale, seconds, frame, data.back().get());
    // The strips are already spread over the cores; inside one, rows run inline.
    context.parallelFor = nullptr;
    placePart(context, width, height);
    contexts.push_back(context);
    // A shader works in straight colour, whatever the flag says of process().
    formats.push_back((step.effect->api->flags & WFX_PREMULTIPLIED) && step.effect->api->process
                          ? QImage::Format_RGBA32FPx4_Premultiplied
                          : QImage::Format_RGBA32FPx4);
    if (!step.effect->api->process && !step.effect->cpuProgram) return false;
  }
  QImage out(image.size(), outputFormat);
  if (out.isNull()) return false;
  out.setDevicePixelRatio(image.devicePixelRatio());
  if (image.colorSpace().isValid()) return false;
  // Row addresses taken once, here: scanLine() from several threads detaches.
  const uchar *source = image.constBits();
  uchar *target = out.bits();
  const qsizetype sourceStride = image.bytesPerLine(), targetStride = out.bytesPerLine();
  const QImage::Format from = image.format();
  constexpr int strip = 16;
  std::atomic<bool> failed{false};
  renderParallelEach((height + strip - 1) / strip, [&](int n) {
    if (failed.load(std::memory_order_relaxed)) return;
    const int top = n * strip, rows = std::min(strip, height - top);
    QImage working = QImage(source + sourceStride * top, width, rows, sourceStride, from).convertToFormat(formats[0]);
    for (size_t k = 0; k < steps.size(); ++k) {
      if (working.format() != formats[k]) working = working.convertToFormat(formats[k]);
      WfxContext context = contexts[k];
      context.originY = contexts[k].originY + top;
      if (working.isNull()) {
        failed = true;
        return;
      }
      if (!steps[k].effect->api->process) {
        // Its shader, on this strip, told where the strip lies in the raster.
        if (!runShaderStrip(working, *steps[k].effect, steps[k].values, context)) {
          failed = true;
          return;
        }
        continue;
      }
      if (steps[k].effect->api->process(working.bits(), working.width(), working.height(), int32_t(working.bytesPerLine()),
                                        steps[k].values.constData(), int32_t(steps[k].values.size()), &context)) {
        failed = true;
        return;
      }
    }
    const QImage done = working.convertToFormat(outputFormat);
    const qsizetype bytes = std::min(targetStride, done.bytesPerLine());
    for (int y = 0; y < rows; ++y) std::memcpy(target + targetStride * (top + y), done.constScanLine(y), size_t(bytes));
  });
  if (failed) return false;
  image = std::move(out);
  return true;
}

namespace {
// What a pressed() says back, gathered on its own thread and handed over after.
struct PressReply {
  QHash<QString, QByteArray> changed;
  QString message;
};
void pressSetData(void *host, const char *id, const uint8_t *bytes, int32_t size) {
  if (!host || !id) return;
  static_cast<PressReply *>(host)->changed.insert(QString::fromUtf8(id),
                                                  bytes && size > 0 ? QByteArray(reinterpret_cast<const char *>(bytes), size)
                                                                    : QByteArray());
}
void pressMessage(void *host, const char *text) {
  if (host && text) static_cast<PressReply *>(host)->message = QString::fromUtf8(text);
}
}  // namespace

int pressPackButton(const PackEffect &e, const QString &button, const Layer &layer, const QImage &picture,
                    const QString &pictureNote, double frame, QHash<QString, QByteArray> &changed, QString &message) {
  if (!e.pressed) return -1;
  DataHold data;
  data.fill(&layer, e);
  const QImage pixels = picture.isNull() ? QImage() : picture.convertToFormat(QImage::Format_RGBA32FPx4);
  const WfxContext context = contextFor(32, QRectF(0, 0, pixels.width(), pixels.height()), 1,
                                        0, frame, &data);
  const auto values = packEffectValues(layer, e, frame);
  const QByteArray id = button.toUtf8(), note = pictureNote.toUtf8();
  PressReply reply;
  WfxPress press{};
  press.structSize = sizeof(WfxPress);
  press.context = &context;
  press.values = values.constData();
  press.valueCount = int32_t(values.size());
  if (!pixels.isNull()) {
    press.picture = reinterpret_cast<const float *>(pixels.constBits());
    press.pictureWidth = pixels.width();
    press.pictureHeight = pixels.height();
    press.pictureStride = int32_t(pixels.bytesPerLine());
  }
  press.pictureNote = note.isEmpty() ? nullptr : note.constData();
  press.host = &reply;
  press.setData = pressSetData;
  press.message = pressMessage;
  const int result = e.pressed(id.constData(), &press);
  message = reply.message;
  if (result == 0) changed = reply.changed;
  return result;
}

QImage packEffectInput(const Scene &s, int index, const QString &effect, double frame, QString &note) {
  note.clear();
  if (index < 0 || index >= int(s.layers.size())) {
    note = "no frame (the layer is gone)";
    return {};
  }
  const Layer &l = s.layers[size_t(index)];
  const double at = frame - l.start;
  if (at < 0 || at >= l.duration) {
    note = "no frame (the layer isn't on at the playhead)";
    return {};
  }
  // Longest edge handed over, as After Effects' capture keeps it (FrameCapture.h).
  constexpr int kMaxEdge = 2560;
  const auto held = [&](QSize size) {
    const int longest = std::max(size.width(), size.height());
    return longest <= kMaxEdge ? size : QSize(std::max(1, qRound(size.width() * double(kMaxEdge) / longest)),
                                              std::max(1, qRound(size.height() * double(kMaxEdge) / longest)));
  };
  const auto kept = [](const Layer &other) {
    // Hidden rather than removed: what is parented to them or reads them in an
    // expression still finds them, and a camera still looks.
    return other.type == "camera" || other.type == "light" || other.type == "empty" || other.type == "audio";
  };
  Scene only = s;
  only.exposure = 0;
  Layer &target = only.layers[size_t(index)];
  // This effect and the ones after it are what the picture goes into.
  const QStringList order = layerEffectOrder(l);
  for (int i = std::max(0, int(order.indexOf(effect))); i < order.size(); ++i) target.hiddenEffects[order[i]] = true;
  QImage picture;
  if (l.type == "effect") {
    // Everything beneath it, as it lies in the Main, where the layer acts.
    for (int i = 0; i < index; ++i)
      if (!kept(only.layers[size_t(i)])) only.layers[size_t(i)].visible = false;
    target.visible = true;
    const QSize full(s.width, s.height), size = held(full);
    QPainterPath box;
    box.addRect(layerBounds(l, s));
    if (layerHasMainTransform(l)) box = layerMainTransform(l).map(box);
    const QTransform toDevice = layerWorldTransform(l, s, frame) *
                                QTransform::fromScale(double(size.width()) / full.width(), double(size.height()) / full.height());
    const QRect acting = toDevice.map(box).boundingRect().toAlignedRect().intersected(QRect(QPoint(), size));
    if (acting.isEmpty()) {
      note = "no frame (the Effect Layer covers nothing in the Main)";
      return {};
    }
    picture = renderCPU(only, frame, false, size).copy(acting);
    note = QString("Wavy-Flo frame: what lies beneath the Effect Layer, %1x%2%3")
               .arg(picture.width()).arg(picture.height())
               .arg(size == full ? QString(", full resolution") : QString(", reduced from the Main's %1x%2").arg(full.width()).arg(full.height()));
  } else {
    const QRectF box = layerContentBounds(l, s);
    const QSize full(std::max(1, int(std::ceil(box.width() - 1e-6))), std::max(1, int(std::ceil(box.height() - 1e-6))));
    if (box.isEmpty() || !std::isfinite(box.width()) || !std::isfinite(box.height()) || full.width() > 32768 ||
        full.height() > 32768) {
      note = "no frame (the layer has no size)";
      return {};
    }
    for (size_t i = 0; i < only.layers.size(); ++i) {
      Layer &other = only.layers[i];
      other.trackMatteId.clear(); // nothing uses this layer as a matte, so it is drawn
      if (int(i) != index && !kept(other)) other.visible = false;
    }
    // The layer alone, unmoved: its content box on the frame, one layer
    // pixel to a pixel. What happens after its effects -- where it is put,
    // turned, faded, blended, matted, blurred by its motion -- is not part of
    // what goes into them.
    target.visible = true;
    target.groupId.clear();
    target.parentId.clear();
    target.parentBind = QTransform();
    target.threeD = false;
    target.motionBlur = false;
    target.repeaterEnabled = false;
    target.blendMode = "normal";
    const auto fixed = [&](int slot, double value) { target.p[size_t(slot)] = Property{}; target.p[size_t(slot)].base = value; };
    fixed(0, -l.mainX - box.left());
    fixed(1, -l.mainY - box.top());
    fixed(2, 1);
    fixed(3, 1);
    fixed(4, 0);
    fixed(5, 1);
    fixed(AnchorX, 0);
    fixed(AnchorY, 0);
    only.width = full.width();
    only.height = full.height();
    const QSize size = held(full);
    picture = renderCPU(only, frame, false, size);
    note = QString("Wavy-Flo frame: the layer before this effect, %1x%2%3")
               .arg(picture.width()).arg(picture.height())
               .arg(size == full ? QString(", full resolution") : QString(", reduced from %1x%2").arg(full.width()).arg(full.height()));
  }
  if (picture.isNull()) {
    note = "no frame (Wavy-Flo could not draw the layer at the playhead)";
    return {};
  }
  return picture.convertToFormat(QImage::Format_RGBA32FPx4);
}

bool packEffectPass(const PackEffect &e, const QVector<float> &values, int depth, GpuPass &pass,
                   QRectF box, double scale, double seconds, double frame) {
  if (e.shader.isEmpty() || values.size() > 256) return false;
  pass = GpuPass{};
  pass.source = e.shader;
  pass.values = values;
  pass.processingDepth = depth;
  pass.box = box;
  pass.scale = scale;
  pass.seconds = seconds;
  pass.frame = frame;
  // One that runs in rounds is not one pass.
  if (e.shaderPasses) return false;
  pass.input = GpuPass::Current;
  pass.endsEffect = true;
  pass.useAux = true; // wavyOriginal(): in one pass, the picture it reads anyway
  pass.auxFrom = GpuPass::Current;
  return true;
}

// Colour grades straight over 8-bit pixels. Widening a 1080p layer to float
// for each grade and narrowing it again afterwards moves tens of megabytes; here
// each row is widened into a buffer that stays in the cache, every grade in the
// run is applied to it, and it is narrowed straight back. The grades are the
// same functions, so the result is the same to within the last 8-bit step.
QSet<QString> gradeLeadingEightBit(QImage &image, const Layer &l, double frame, double seconds, QRectF box,
                                   double scale, const std::function<bool(const QString &)> &inert) {
  const bool argb = image.format() == QImage::Format_ARGB32_Premultiplied;
  const bool rgba = image.format() == QImage::Format_RGBA8888_Premultiplied;
  if ((!argb && !rgba) || image.isNull() || ids().isEmpty()) return {};
  QSet<QString> used;
  struct Link { const PackEffect *effect; void *state; };
  std::vector<Link> chain;
  const WfxContext context = contextFor(32, box, scale, seconds, frame);
  for (const auto &name : layerEffectOrder(l)) {
    // The halo effects are on whenever they have a size, not by a switch.
    const double halo = name == "Drop Shadow" ? l.shadow : name == "Glow" ? l.glow : name == "Gaussian Blur" ? l.blur : 0;
    const bool applied = (halo > 0 || l.appliedEffects[name].toBool()) && !l.hiddenEffects[name].toBool();
    if (!applied || (inert && inert(name))) continue;
    const PackEffect *e = packEffect(name);
    if (!e || !e->api->span || !e->api->prepare || !e->api->release) break;
    const auto values = packEffectValues(l, *e, frame);
    used.insert(name);
    if (packEffectInert(*e, values)) continue;
    if (void *state = e->api->prepare(values.constData(), int32_t(values.size()), &context)) chain.push_back({e, state});
  }
  if (chain.empty()) return used;
  auto *bits = image.bits(); const auto stride = image.bytesPerLine(); const int width = image.width();
  // Byte positions of red, green, blue and alpha in a pixel.
  const bool little = QSysInfo::ByteOrder == QSysInfo::LittleEndian;
  const int ri = argb ? (little ? 2 : 1) : 0, gi = argb ? (little ? 1 : 2) : 1;
  const int bi = argb ? (little ? 0 : 3) : 2, ai = argb ? (little ? 3 : 0) : 3;
  static const auto inverse = [] { std::array<float, 256> t{}; for (int a = 1; a < 256; ++a) t[size_t(a)] = 1.f / float(a); return t; }();
  renderParallel(image.height(), [&](int begin, int end) {
    std::vector<float> row(size_t(width) * 4);
    for (int y = begin; y < end; ++y) {
      uchar *line = bits + y * stride;
      bool any = false;
      for (int x = 0; x < width; ++x) {
        const uchar *c = line + x * 4;
        float *p = row.data() + x * 4;
        const int a = c[ai];
        if (a == 0) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
        any = true;
        const float s = inverse[size_t(a)];
        p[0] = c[ri] * s; p[1] = c[gi] * s; p[2] = c[bi] * s; p[3] = a * (1.f / 255.f);
      }
      if (!any) continue;
      for (const auto &link : chain) link.effect->api->span(row.data(), width, link.state);
      for (int x = 0; x < width; ++x) {
        uchar *c = line + x * 4;
        const int a = c[ai];
        if (a == 0) continue;
        // Grades leave alpha alone, so the byte that came in goes back out,
        // and colour is clamped after the alpha as the float conversion does.
        const float *p = row.data() + x * 4;
        const float alpha = float(a);
        c[ri] = uchar(int(std::clamp(p[0] * alpha, 0.f, 255.f) + .5f));
        c[gi] = uchar(int(std::clamp(p[1] * alpha, 0.f, 255.f) + .5f));
        c[bi] = uchar(int(std::clamp(p[2] * alpha, 0.f, 255.f) + .5f));
      }
    }
  });
  for (const auto &link : chain) link.effect->api->release(link.state);
  return used;
}

QVector<GpuPass> packEffectPasses(const PackEffect &e, const QVector<float> &values, int depth, QSize raster,
                                  QRectF box, double scale, double seconds, double frame) {
  if (e.shader.isEmpty() || values.size() > 256) return {};
  int rounds = 1;
  if (e.shaderPasses) {
    WfxContext context = contextFor(depth, box, scale, seconds, frame);
    context.rasterWidth = raster.width();
    context.rasterHeight = raster.height();
    rounds = shaderRounds(e, values, context);
    if (rounds < 1) return {};
  }
  GpuPass pass;
  pass.source = e.shader;
  pass.values = values;
  pass.processingDepth = depth;
  pass.box = box;
  pass.scale = scale;
  pass.seconds = seconds;
  pass.frame = frame;
  // Each round reads the one before; the picture the effect started from stays
  // where it was (the image a chain's effect begins with), read through
  // wavyOriginal(), until the last round takes its place.
  pass.useAux = true;
  pass.auxFrom = GpuPass::Current;
  QVector<GpuPass> passes;
  pass.shaderPasses = rounds;
  for (int round = 0; round < rounds; ++round) {
    pass.shaderPass = round;
    pass.input = round ? GpuPass::Previous : GpuPass::Current;
    pass.endsEffect = round + 1 == rounds;
    passes.push_back(pass);
  }
  return passes;
}

// ---- SDK 10: one shader, run everywhere ---------------------------------------------
namespace {
// A parameter id as a GLSL name: anything but letters, digits and _ becomes _.
QString glslName(const char *id) {
  QString name = QString::fromLatin1(id ? id : "");
  for (QChar &c : name)
    if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != '_') c = '_';
  return name;
}
// What both a shader and an effect's expressions see: the parameters by name,
// and the helpers that need nothing but numbers.
QString valuesPrelude(const WfxParameter *parameters, int count) {
  QString text = "// Wavy-Flo SDK 10: the parameters by name.\n";
  QSet<QString> named;
  int at = 0;
  for (int i = 0; i < count && parameters; ++i) {
    const WfxParameter &p = parameters[i];
    const int floats = wfxParameterFloats(&p);
    const QString name = "param_" + glslName(p.id);
    const QString v = QString("values[%1]").arg(at);
    const auto value = [&](int k) { return QString("values[%1]").arg(at + k); };
    QString meaning;
    switch (p.kind) {
    case WFX_FLOAT: meaning = "(" + v + ")"; break;
    case WFX_BOOL: meaning = "(" + v + " > 0.5)"; break;
    case WFX_CHOICE:
    case WFX_LAYER:
    case WFX_MASK: meaning = "int(" + v + ")"; break;
    case WFX_COLOR: meaning = QString("vec4(%1, %2, %3, %4)").arg(value(0), value(1), value(2), value(3)); break;
    case WFX_POINT: meaning = QString("vec2(%1, %2)").arg(value(0), value(1)); break;
    case WFX_POINT_3D: meaning = QString("vec3(%1, %2, %3)").arg(value(0), value(1), value(2)); break;
    case WFX_CURVE: meaning = QString("(x) wavyCurve(%1, x)").arg(at); break;
    default: break;
    }
    if (!meaning.isEmpty() && p.id && *p.id && !named.contains(name)) {
      named.insert(name);
      text += "#define " + name + (p.kind == WFX_CURVE ? "" : " ") + meaning + "\n";
    }
    at += floats;
  }
  text += "#define WAVY_SHOWN 0\n#define WAVY_DIMMED 1\n#define WAVY_HIDDEN 2\n"
          // A curve's five points (its output at 0, .25, .5, .75, 1), joined by
          // straight lines, as the host's own Curves reads them.
          "float wavyCurve(int at, float x) {\n"
          "  x = clamp(x, 0.0, 1.0);\n"
          "  float f = x * 4.0;\n"
          "  int i = min(3, int(f));\n"
          "  return mix(values[at + i], values[at + i + 1], f - float(i));\n"
          "}\n"
          "float wavyLuma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
          // Whole-number hashing gives the same bits on every card and processor;
          // fract(sin(...)) does not.
          "uint wavyHash(uint x) {\n"
          "  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;\n"
          "  return x;\n"
          "}\n"
          "float wavyRandom(vec2 p, float seed) {\n"
          "  ivec2 i = ivec2(floor(p));\n"
          "  uint h = wavyHash(uint(i.x) ^ wavyHash(uint(i.y) ^ wavyHash(floatBitsToUint(seed))));\n"
          "  return float(h >> 8u) * (1.0 / 16777216.0);\n"
          "}\n"
          "float wavyNoise(vec2 p, float seed) {\n"
          "  vec2 i = floor(p);\n"
          "  vec2 f = p - i;\n"
          "  vec2 u = f * f * (3.0 - 2.0 * f);\n"
          "  float a = wavyRandom(i, seed);\n"
          "  float b = wavyRandom(i + vec2(1.0, 0.0), seed);\n"
          "  float c = wavyRandom(i + vec2(0.0, 1.0), seed);\n"
          "  float d = wavyRandom(i + vec2(1.0, 1.0), seed);\n"
          "  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
          "}\n";
  return text;
}
// How many lines packShaderSource() puts before a body, for an error in a
// body without a #line of its own.
int wrapperLines() {
  static const int lines = [] {
    const QString wrapped = packShaderSource(QStringLiteral("\x01"));
    return int(wrapped.left(wrapped.indexOf(QChar(1))).count('\n'));
  }();
  return lines;
}
QString bodyLines(QString problem, const QString &body) {
  static const QRegularExpression line("^line (\\d+): ");
  const auto match = line.match(problem);
  if (match.hasMatch() && !body.contains("#line"))
    problem.replace(0, match.capturedLength(), QString("line %1: ").arg(std::max(1, match.captured(1).toInt() - wrapperLines())));
  return problem;
}
struct CpuPrograms {
  std::mutex lock;
  std::map<QString, std::shared_ptr<const cpu_shader::Program>> made;
  std::map<QString, QString> refused;
};
CpuPrograms &cpuPrograms() {
  static CpuPrograms programs;
  return programs;
}
}  // namespace

QString packShaderPrelude(const WfxParameter *parameters, int count) {
  return valuesPrelude(parameters, count) +
         "vec4 wavyPremultiply(vec4 c) { return vec4(c.rgb * c.a, c.a); }\n"
         "vec4 wavyUnpremultiply(vec4 c) { return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0); }\n"
         // The picture between pixels, at p in raster pixels (a pixel's centre
         // is at x + .5), mixed in premultiplied colour so clear pixels lend no
         // colour; clear beyond the raster. The same sums on the card and here,
         // where the card's own filtering keeps only 8 bits of where between.
         "vec4 wavySample(vec2 p) {\n"
         "  vec2 q = p - 0.5;\n"
         "  vec2 f = floor(q);\n"
         "  vec2 t = q - f;\n"
         "  ivec2 i = ivec2(f);\n"
         "  vec4 a = wavyPremultiply(wavyPixel(i));\n"
         "  vec4 b = wavyPremultiply(wavyPixel(i + ivec2(1, 0)));\n"
         "  vec4 c = wavyPremultiply(wavyPixel(i + ivec2(0, 1)));\n"
         "  vec4 d = wavyPremultiply(wavyPixel(i + ivec2(1, 1)));\n"
         "  return wavyUnpremultiply(mix(mix(a, b, t.x), mix(c, d, t.x), t.y));\n"
         "}\n"
         // The same with the raster's edge held instead of clear beyond it.
         "vec4 wavySampleEdge(vec2 p) { return wavySample(clamp(p, vec2(0.5), sourceSize - 0.5)); }\n"
         "#line 1\n";
}

std::shared_ptr<const cpu_shader::Program> cpuShaderProgram(const QString &body, QString *problem) {
  auto &c = cpuPrograms();
  {
    std::lock_guard<std::mutex> hold(c.lock);
    if (const auto found = c.made.find(body); found != c.made.end()) return found->second;
    if (const auto found = c.refused.find(body); found != c.refused.end()) {
      if (problem) *problem = found->second;
      return nullptr;
    }
  }
  // Compiled outside the lock: a thread wanting another body need not wait.
  QString why;
  const auto program = cpu_shader::compile(packShaderSource(body), "main", &why);
  std::lock_guard<std::mutex> hold(c.lock);
  // An effect being edited compiles a new text at every pause in the typing:
  // the ones nothing uses any more are let go now and then.
  if (c.made.size() >= 256)
    for (auto it = c.made.begin(); it != c.made.end();) it = it->second.use_count() == 1 ? c.made.erase(it) : std::next(it);
  if (c.refused.size() >= 256) c.refused.clear();
  if (program) return c.made.emplace(body, program).first->second;
  why = bodyLines(why, body);
  c.refused.emplace(body, why);
  if (problem) *problem = why;
  return nullptr;
}

QImage cpuRunPasses(const QImage &source, const QVector<GpuPass> &passes) {
  if (source.isNull() || passes.isEmpty()) return {};
  // The numbers the card is handed: float, premultiplied or not as they are.
  const bool premultiplied = source.format() == QImage::Format_RGBA32FPx4_Premultiplied ||
                             source.format() == QImage::Format_RGBA8888_Premultiplied ||
                             source.format() == QImage::Format_RGBA64_Premultiplied ||
                             source.format() == QImage::Format_ARGB32_Premultiplied;
  const QImage::Format working = premultiplied ? QImage::Format_RGBA32FPx4_Premultiplied : QImage::Format_RGBA32FPx4;
  QImage current = source.convertToFormat(working), last;
  for (const GpuPass &pass : passes) {
    if (pass.source.isEmpty() || pass.values.size() > 256) return {};
    const auto program = cpuShaderProgram(pass.source);
    if (!program) return {};
    const QImage &input = pass.input == GpuPass::Previous && !last.isNull() ? last : current;
    if (!pass.size.isEmpty() && pass.size != input.size()) return {}; // a pass that resizes is the card's alone
    // wavyOriginalTexture: what the pass's second input is, as the card binds it.
    QImage original;
    if (pass.useAux && !pass.auxImage.isNull())
      original = pass.auxImage.convertToFormat(pass.auxImage.format() == QImage::Format_RGBA32FPx4_Premultiplied
                                                   ? QImage::Format_RGBA32FPx4_Premultiplied
                                                   : QImage::Format_RGBA32FPx4);
    else
      original = pass.useAux && pass.auxFrom == GpuPass::Previous && !last.isNull() ? last : current;
    cpu_shader::Inputs in = shaderInputs(pass.values, pass.processingDepth, pass.box, pass.scale, pass.seconds, pass.frame);
    in.pass = pass.shaderPass;
    in.passes = pass.shaderPasses;
    QImage output(input.size(), input.format());
    if (output.isNull() || !cpu_shader::run(*program, input, original, output, in)) return {};
    last = std::move(output);
    if (pass.endsEffect) current = last;
  }
  return last.convertToFormat(source.format());
}

// ---- effects written as text ------------------------------------------------------------
namespace {
// An effect written as text, as loaded: its description, everything the
// description points at, and its expressions compiled for the processor.
struct TextEffect {
  WfxEffect api{};
  std::vector<WfxParameter> parameters;
  std::deque<QByteArray> strings; // where every string the description names lives
  std::vector<const char *> help;
  std::shared_ptr<const cpu_shader::Program> inert, varies, reach, passes;
  std::vector<std::shared_ptr<const cpu_shader::Program>> controls; // by parameter, null for none
  const char *keep(const QString &text) {
    strings.push_back(text.toUtf8());
    return strings.back().constData();
  }
};
// An expression of the parameters, compiled to give one number.
std::shared_ptr<const cpu_shader::Program> textExpression(const TextEffect &t, const QString &type, const QString &text,
                                                          QString *problem) {
  const QString source = "uniform float values[256];uniform vec2 sourceSize;uniform int processingDepth;\n"
                         "uniform float wavySeconds;uniform float wavyFrame;uniform float wavyScale;uniform vec4 wavyBox;\n"
                         "uniform int wavyPass;uniform int wavyPasses;\n" +
                         valuesPrelude(t.parameters.data(), int(t.parameters.size())) + "#line 1\n" + type +
                         " wavyAnswer() { return " + type + "(" + text + "); }\n";
  return cpu_shader::compile(source, "wavyAnswer", problem);
}
double answer(const cpu_shader::Program &program, const float *values, int32_t count, double scale = 1,
              const WfxContext *context = nullptr) {
  cpu_shader::Inputs in;
  in.values = values;
  in.valueCount = count;
  in.scale = float(scale);
  if (context) {
    in.seconds = float(context->seconds);
    in.frame = float(context->frame);
    in.scale = float(context->scale);
    in.box[0] = float(context->boxX);
    in.box[1] = float(context->boxY);
    in.box[2] = float(context->boxWidth);
    in.box[3] = float(context->boxHeight);
    in.depth = context->depth;
    in.rasterWidth = context->rasterWidth;
    in.rasterHeight = context->rasterHeight;
  }
  return cpu_shader::evaluate(program, in);
}
// A colour as a project or a description writes it: "#rrggbb", "#rrggbbaa", or [r, g, b, a] from 0 to 1.
bool readColour(const QJsonValue &v, float out[4]) {
  if (v.isArray()) {
    const QJsonArray a = v.toArray();
    if (a.size() < 3 || a.size() > 4) return false;
    for (int k = 0; k < 4; ++k) out[k] = float(k < a.size() ? a[k].toDouble() : 1.);
    return true;
  }
  QString text = v.toString().trimmed();
  if (!text.startsWith('#') || (text.size() != 7 && text.size() != 9)) return false;
  bool ok = true;
  for (int k = 0; k < 4 && ok; ++k)
    out[k] = 1 + k * 2 < text.size() ? float(text.mid(1 + k * 2, 2).toInt(&ok, 16)) / 255.f : 1.f;
  return ok;
}
}  // namespace

std::unique_ptr<PackEffect> readTextEffect(const QByteArray &bytes, const QString &file, QString *problem) {
  const auto fail = [&](const QString &why) {
    if (problem) *problem = why;
    return std::unique_ptr<PackEffect>();
  };
  const QString text = QString::fromUtf8(bytes);
  // The description: the leading comment, "Wavy-Flo effect" and then JSON.
  int start = 0;
  while (start < text.size() && text[start].isSpace()) ++start;
  if (!QStringView(text).mid(start).startsWith(QLatin1String("/*")))
    return fail("it does not start with its description: /* Wavy-Flo effect { ... } */");
  const int close = text.indexOf("*/", start + 2);
  if (close < 0) return fail("its description is never closed with */");
  const QString raw = text.mid(start + 2, close - start - 2);
  QString head = raw.trimmed();
  if (!head.startsWith(QLatin1String("Wavy-Flo effect"))) return fail("its first comment does not begin \"Wavy-Flo effect\"");
  head = head.mid(15);
  QJsonParseError error{};
  const QByteArray headBytes = head.toUtf8();
  const QJsonDocument json = QJsonDocument::fromJson(headBytes, &error);
  if (error.error != QJsonParseError::NoError || !json.isObject()) {
    // Where in the file: the JSON starts after "Wavy-Flo effect", and the
    // parser counts bytes.
    int lead = 0;
    while (lead < raw.size() && raw[lead].isSpace()) ++lead;
    const int at = start + 2 + lead + 15 + int(QString::fromUtf8(headBytes.left(error.offset)).size());
    return fail(QString("its description is not JSON, line %1: %2").arg(text.left(at).count('\n') + 1).arg(error.errorString()));
  }
  const QJsonObject o = json.object();
  for (auto it = o.begin(); it != o.end(); ++it) {
    static const QStringList known{"id",     "name",       "category", "parameters", "perPixel", "movesAlone", "wholeLayer",
                                   "inert",  "varies",     "reach",    "passes",     "controls", "description", "author",
                                   "version", "flags"};
    if (!known.contains(it.key())) return fail("its description has \"" + it.key() + "\", which is not a key Wavy-Flo knows");
  }
  // The body keeps the file's line numbers: the description's lines come back
  // blank, so an error at line 12 of the body is line 12 of the file.
  const QString body = QString(int(text.left(close + 2).count('\n')), '\n') + text.mid(close + 2);

  auto t = std::make_shared<TextEffect>();
  const QString id = o.value("id").toString().trimmed();
  if (id.isEmpty()) return fail("its description has no \"id\"");
  const QJsonArray flags = o.value("flags").toArray();
  const auto flag = [&](const char *key) { return o.value(key).toBool() || flags.contains(QJsonValue(QLatin1String(key))); };
  t->api.structSize = sizeof(WfxEffect);
  t->api.abiVersion = 10;
  t->api.id = t->keep(id);
  t->api.name = t->keep(o.value("name").toString(id));
  t->api.category = t->keep(o.value("category").toString("Plugins"));
  t->api.supportedDepths = 4;
  t->api.flags = (flag("perPixel") ? WFX_PER_PIXEL : 0u) | (flag("movesAlone") ? WFX_MOVES_ALONE : 0u) |
                 (flag("wholeLayer") ? WFX_WHOLE_LAYER : 0u);
  bool anyHelp = false;
  const QJsonArray list = o.value("parameters").toArray();
  if (list.size() > 64) return fail("it has more than 64 parameters");
  for (const QJsonValue &entry : list) {
    const QJsonObject p = entry.toObject();
    WfxParameter w{};
    w.structSize = sizeof(WfxParameter);
    const QString kind = p.value("kind").toString("float");
    static const QHash<QString, uint32_t> kinds{{"float", WFX_FLOAT},   {"bool", WFX_BOOL},       {"choice", WFX_CHOICE},
                                                {"color", WFX_COLOR},   {"colour", WFX_COLOR},    {"curve", WFX_CURVE},
                                                {"point", WFX_POINT},   {"point3d", WFX_POINT_3D}, {"group", WFX_GROUP},
                                                {"end", WFX_GROUP_END}, {"angle", WFX_FLOAT}};
    if (!kinds.contains(kind)) return fail("a parameter of a kind Wavy-Flo does not know: " + kind);
    w.kind = kinds.value(kind);
    const QString pid = p.value("id").toString();
    const QString what = pid.isEmpty() ? QString("a %1").arg(kind) : pid;
    if (w.kind != WFX_GROUP && w.kind != WFX_GROUP_END) {
      if (pid.isEmpty()) return fail("a " + kind + " parameter without an \"id\"");
      static const QRegularExpression plain("^[A-Za-z_][A-Za-z0-9_]*$");
      if (!plain.match(pid).hasMatch()) return fail("parameter id \"" + pid + "\": use letters, digits and _ (it names param_" + pid + " in the shader)");
      w.id = t->keep(pid);
    }
    w.label = t->keep(p.value("label").toString(pid));
    w.unit = p.contains("unit") ? t->keep(p.value("unit").toString()) : nullptr;
    w.section = p.contains("section") ? t->keep(p.value("section").toString()) : nullptr;
    w.step = float(p.value("step").toDouble(0));
    if (p.value("still").toBool()) w.flags |= WFX_PARAM_STILL;
    if (p.value("collapsed").toBool()) w.flags |= WFX_PARAM_COLLAPSED;
    if (p.value("wheel").toBool()) w.flags |= WFX_PARAM_WHEEL;
    if (kind == "angle" || p.value("angle").toBool()) w.flags |= WFX_PARAM_ANGLE;
    switch (w.kind) {
    case WFX_FLOAT: {
      const bool angle = w.flags & WFX_PARAM_ANGLE;
      w.minimum = float(p.value("min").toDouble(angle ? -36000 : 0));
      w.maximum = float(p.value("max").toDouble(angle ? 36000 : 100));
      w.initial = float(p.value("default").toDouble(w.minimum > 0 ? w.minimum : std::min(0.f, w.maximum)));
      // A step to suit the range when none is given: whole numbers across a
      // hundred or more (a percentage, pixels), tenths below that, hundredths
      // below one -- not three decimals on "24 px".
      if (!p.contains("step")) {
        const float range = w.maximum - w.minimum;
        w.step = angle ? .1f : range >= 100 ? 1.f : range >= 1 ? .1f : .01f;
      }
      break;
    }
    case WFX_BOOL:
      w.maximum = 1;
      w.initial = p.value("default").toBool() || p.value("default").toDouble() > .5 ? 1 : 0;
      break;
    case WFX_CHOICE: {
      QStringList choices;
      for (const QJsonValue &c : p.value("choices").toArray()) choices << c.toString();
      if (choices.isEmpty()) return fail("choice " + what + " has no \"choices\"");
      w.choices = t->keep(choices.join('\n'));
      w.maximum = float(choices.size() - 1);
      w.initial = float(std::clamp(p.value("default").toInt(0), 0, int(choices.size()) - 1));
      break;
    }
    case WFX_COLOR:
      w.maximum = 1;
      w.initialColor[0] = w.initialColor[1] = w.initialColor[2] = w.initialColor[3] = 1;
      if (p.contains("default") && !readColour(p.value("default"), w.initialColor))
        return fail("colour " + what + ": its default is not \"#rrggbb\", \"#rrggbbaa\" or [r, g, b, a]");
      break;
    case WFX_POINT:
    case WFX_POINT_3D: {
      const QJsonArray d = p.value("default").toArray();
      for (int k = 0; k < (w.kind == WFX_POINT ? 2 : 3); ++k) w.initialColor[k] = float(k < d.size() ? d[k].toDouble() : 0.);
      w.minimum = float(p.value("min").toDouble(-1e6));
      w.maximum = float(p.value("max").toDouble(1e6));
      break;
    }
    default: break;
    }
    t->parameters.push_back(w);
    const QString help = p.value("help").toString();
    t->help.push_back(help.isEmpty() ? nullptr : t->keep(help));
    anyHelp = anyHelp || !help.isEmpty();
  }
  t->api.parameterCount = int32_t(t->parameters.size());
  t->api.parameters = t->parameters.data();
  t->api.shader = t->keep(body);
  if (anyHelp) t->api.help = t->help.data();
  if (const QString why = validate(&t->api); !why.isEmpty()) return fail(why);

  auto effect = std::make_unique<PackEffect>();
  effect->api = &t->api;
  effect->id = id;
  effect->name = QString::fromUtf8(t->api.name);
  effect->category = QString::fromUtf8(t->api.category);
  effect->file = file;
  for (const WfxParameter &p : t->parameters) {
    effect->offsets.push_back(effect->valueCount);
    effect->valueCount += wfxParameterFloats(&p);
  }
  effect->help = t->api.help;
  effect->shader = packShaderPrelude(t->parameters.data(), int(t->parameters.size())) + body;
  QString why;
  effect->cpuProgram = cpuShaderProgram(effect->shader, &why);
  if (!effect->cpuProgram) return fail("its shader does not compile: " + why);

  // What it says about its values, as expressions of them.
  struct Expression {
    const char *key, *type;
    std::shared_ptr<const cpu_shader::Program> TextEffect::*slot;
  };
  for (const Expression &x : {Expression{"inert", "bool", &TextEffect::inert}, Expression{"varies", "bool", &TextEffect::varies},
                              Expression{"reach", "float", &TextEffect::reach}, Expression{"passes", "int", &TextEffect::passes}}) {
    const QJsonValue v = o.value(x.key);
    if (v.isUndefined() || v.isNull()) continue;
    const QString expression = v.isString() ? v.toString() : v.isBool() ? QString(v.toBool() ? "true" : "false")
                                                                         : QString::number(v.toDouble(), 'g', 9);
    (*t).*(x.slot) = textExpression(*t, x.type, expression, &why);
    if (!((*t).*(x.slot))) return fail(QString("its \"%1\" does not compile: %2").arg(x.key, why));
  }
  const QJsonObject controls = o.value("controls").toObject();
  t->controls.resize(t->parameters.size());
  for (auto it = controls.begin(); it != controls.end(); ++it) {
    int index = -1;
    for (size_t i = 0; i < t->parameters.size(); ++i)
      if (t->parameters[i].id && it.key() == QLatin1String(t->parameters[i].id)) index = int(i);
    if (index < 0) return fail("\"controls\" names " + it.key() + ", which is no parameter of it");
    t->controls[size_t(index)] = textExpression(*t, "int", it.value().toString(), &why);
    if (!t->controls[size_t(index)]) return fail(QString("its control rule for %1 does not compile: %2").arg(it.key(), why));
  }
  // Answers held to what the host can use: a broken one (NaN) changes nothing.
  if (t->inert)
    effect->inert = [t](const float *v, int32_t n) {
      const double r = answer(*t->inert, v, n);
      return r == r && r != 0 ? 1 : 0;
    };
  if (t->varies)
    effect->varies = [t](const float *v, int32_t n) {
      const double r = answer(*t->varies, v, n);
      return r != r || r != 0 ? 1 : 0; // when it cannot tell, it moves
    };
  if (t->reach)
    effect->reach = [t](const float *v, int32_t n, double scale) {
      const double r = answer(*t->reach, v, n, scale);
      return std::isfinite(r) ? std::max(0., r) : 0.;
    };
  if (t->passes)
    effect->shaderPasses = [t](const float *v, int32_t n, const WfxContext *context) {
      const double r = answer(*t->passes, v, n, 1, context);
      return int32_t(r == r ? std::clamp(r, 1., 64.) : 1.);
    };
  if (!controls.isEmpty())
    effect->controlState = [t](const float *v, int32_t n, int32_t parameter) -> uint32_t {
      if (parameter < 0 || size_t(parameter) >= t->controls.size() || !t->controls[size_t(parameter)]) return 0;
      const double r = answer(*t->controls[size_t(parameter)], v, n);
      return r == r ? uint32_t(std::clamp(int(r), 0, 3)) : 0u;
    };
  effect->description = t;
  return effect;
}

// ---- --check-effect ---------------------------------------------------------------------
namespace {
// A picture with something in it for every kind of effect: colour across,
// light down, hard edges, a soft shape, and a clear border with half-clear
// pixels inside it.
QImage checkPicture(QSize size) {
  QImage picture(size, QImage::Format_RGBA32FPx4);
  const double w = size.width(), h = size.height();
  for (int y = 0; y < size.height(); ++y) {
    float *row = reinterpret_cast<float *>(picture.scanLine(y));
    for (int x = 0; x < size.width(); ++x) {
      const QColor hue = QColor::fromHsvF(float(x / w), .8f, 1);
      const double light = .15 + .85 * (1 - y / h);
      const bool checker = ((x * 16 / size.width()) + (y * 9 / size.height())) % 2;
      const double round = std::hypot(x / w - .62, (y / h - .45) * h / w);
      const double spot = std::clamp((.18 - round) / .03, 0., 1.);
      const bool border = x < w * .04 || x >= w * .96 || y < h * .05 || y >= h * .95;
      float *p = row + x * 4;
      for (int k = 0; k < 3; ++k) {
        const double c = (k == 0 ? hue.redF() : k == 1 ? hue.greenF() : hue.blueF()) * light * (checker ? 1 : .7);
        p[k] = float(c * (1 - spot) + spot);
      }
      p[3] = border ? 0.f : y > h * .8 ? .5f : 1.f;
    }
  }
  return picture;
}
}  // namespace

int checkEffectCommand(const QString &path) {
  QStringList out;
  bool ok = true;
  const auto bad = [&](const QString &line) {
    out << "  PROBLEM: " + line;
    ok = false;
  };
  const QFileInfo info(path);
  std::vector<std::unique_ptr<PackEffect>> effects;
  std::unique_ptr<QLibrary> library;
  out << "Wavy-Flo effect check: " + QDir::toNativeSeparators(info.absoluteFilePath());
  if (!info.isFile()) {
    bad("there is no such file");
  } else if (info.suffix().compare("wfxs", Qt::CaseInsensitive) == 0) {
    QFile file(info.absoluteFilePath());
    QString problem;
    auto effect = file.open(QIODevice::ReadOnly) ? readTextEffect(file.readAll(), info.canonicalFilePath(), &problem) : nullptr;
    if (effect) effects.push_back(std::move(effect));
    else bad("not loaded: " + (problem.isEmpty() ? QString("it cannot be read") : problem));
  } else {
    library = std::make_unique<QLibrary>(info.canonicalFilePath());
    using Count = int32_t (*)();
    using At = const WfxEffect *(*)(int32_t);
    const auto count = library->load() ? reinterpret_cast<Count>(library->resolve("wfxEffectCount")) : nullptr;
    const auto at = count ? reinterpret_cast<At>(library->resolve("wfxEffectAt")) : nullptr;
    if (!library->isLoaded()) bad("not loaded: " + library->errorString());
    else if (!count || !at) bad("not a pack: it exports no wfxEffectCount and wfxEffectAt");
    else
      for (int32_t i = 0; i < std::clamp(count(), 0, 256); ++i) {
        QString problem;
        auto effect = makePackEffect(at(i), info.canonicalFilePath(), &problem);
        if (effect) effects.push_back(std::move(effect));
        else bad(QString("effect %1 not loaded: %2").arg(i).arg(problem));
      }
  }
  const QImage small = checkPicture({480, 270}), large = checkPicture({1920, 1080});
  for (const auto &e : effects) {
    const WfxEffect &api = *e->api;
    out << "";
    out << QString("== %1 (id \"%2\", %3)").arg(e->name, e->id, e->category);
    QStringList flags;
    if (api.flags & WFX_PER_PIXEL) flags << "per pixel";
    if (api.flags & WFX_MOVES_ALONE) flags << "moves with time";
    if (api.flags & WFX_WHOLE_LAYER) flags << "whole layer";
    if (api.flags & WFX_PREMULTIPLIED) flags << "premultiplied process()";
    out << QString("  %1 parameter(s), %2 value(s)%3; %4").arg(api.parameterCount).arg(e->valueCount)
               .arg(flags.isEmpty() ? QString() : "; " + flags.join(", "))
               .arg(api.process ? "has process()" : "a shader and nothing else: the host runs it everywhere");
    if (e->shader.isEmpty()) {
      out << "  no shader: it runs on the processor only, through process(), and keeps a layer's chain off the card";
      continue;
    }
    QString why;
    const auto program = cpuShaderProgram(e->shader, &why);
    if (program) out << QString("  processor: compiles (%1 instructions)").arg(cpu_shader::instructionCount(*program));
    else bad("processor: does not compile: " + why);
    const QString cardProblem = gpuShaderProblem(e->shader);
    const bool card = cardProblem.isEmpty();
    if (card) out << "  card: compiles";
    else if (cardProblem.contains("no usable graphics card")) out << "  card: " + cardProblem + " (not checked)";
    else bad("card: does not compile: " + cardProblem);
    if (!program) continue;
    // Its defaults, and every slider at either end of its range.
    std::vector<std::pair<QString, Layer>> cases;
    cases.push_back({"defaults", Layer()});
    for (const bool high : {false, true}) {
      Layer l;
      QJsonObject settings;
      for (int i = 0; i < api.parameterCount; ++i)
        if (api.parameters[i].kind == WFX_FLOAT && api.parameters[i].id)
          settings[QString::fromUtf8(api.parameters[i].id)] = double(high ? api.parameters[i].maximum : api.parameters[i].minimum);
      l.surfaceEffects[e->id] = settings;
      cases.push_back({high ? "every slider at its maximum" : "every slider at its minimum", l});
    }
    for (const auto &[label, layer] : cases) {
      const auto values = packEffectValues(layer, *e, 30);
      if (packEffectInert(*e, values)) {
        out << "  " + label + ": inert (the host skips it)";
        continue;
      }
      const QRectF box(small.width() * .04, small.height() * .05, small.width() * .92, small.height() * .9);
      const auto passes = packEffectPasses(*e, values, 32, small.size(), box, 1, 1, 30);
      if (passes.isEmpty()) {
        out << "  " + label + ": left to process() at these values";
        continue;
      }
      const QImage processor = cpuRunPasses(small, passes);
      if (processor.isNull()) {
        bad(label + ": the processor could not run it");
        continue;
      }
      QString line = "  " + label + ": ";
      if (card) {
        const QImage gpu = gpuRunPasses(small, passes);
        if (gpu.isNull()) line += "the card could not run it; ";
        else {
          // Compared as seen: premultiplied, so colour under clear pixels does not count.
          const QImage a = processor.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied),
                       b = gpu.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
          double most = 0, total = 0;
          for (int y = 0; y < a.height(); ++y) {
            const float *pa = reinterpret_cast<const float *>(a.constScanLine(y)), *pb = reinterpret_cast<const float *>(b.constScanLine(y));
            for (int x = 0; x < a.width() * 4; ++x) {
              const double d = std::abs(double(pa[x]) - double(pb[x]));
              most = std::max(most, d);
              total += d;
            }
          }
          const double mean = total / (double(a.width()) * a.height() * 4);
          const bool same = most < 2e-3 || (mean < 2e-4 && most < .05);
          line += QString("card and processor %1 (largest difference %2, mean %3)")
                      .arg(same ? "draw the same picture" : "DIFFER").arg(most, 0, 'g', 3).arg(mean, 0, 'g', 3);
          if (!same) {
            ok = false;
            line += " -- see EFFECT-TEXT.md, \"What gives the same picture everywhere\"";
          }
        }
      } else {
        line += "drawn on the processor";
      }
      out << line;
    }
    // Timed where it matters: a full HD frame.
    Layer plain;
    const auto values = packEffectValues(plain, *e, 30);
    const QRectF wide(large.width() * .04, large.height() * .05, large.width() * .92, large.height() * .9);
    const auto passes = packEffectPasses(*e, values, 32, large.size(), wide, 1, 1, 30);
    const auto time = [](const std::function<void()> &run) {
      run(); // once to warm up
      QElapsedTimer clock;
      clock.start();
      int runs = 0;
      do {
        run();
        ++runs;
      } while (clock.elapsed() < 600 && runs < 20);
      return clock.nsecsElapsed() / 1e6 / runs;
    };
    QStringList timings;
    if (!passes.isEmpty()) {
      timings << QString("processor %1 ms").arg(time([&] { cpuRunPasses(large, passes); }), 0, 'f', 1);
      if (card) timings << QString("card %1 ms (the picture there and back included)").arg(time([&] { gpuRunPasses(large, passes); }), 0, 'f', 1);
    }
    if (api.process)
      timings << QString("its process() %1 ms").arg(time([&] {
        QImage copy = large;
        runPackEffect(copy, *e, values, wide, 1, 1, 30);
      }), 0, 'f', 1);
    out << "  1920x1080 at its defaults: " + timings.join(", ");
    // What it says about its values at its defaults.
    QStringList answers;
    if (e->reach) answers << QString("reach %1 px").arg(e->reach(values.constData(), int32_t(values.size()), 1.), 0, 'g', 4);
    if (e->varies) answers << QString("moves with time: %1").arg(e->varies(values.constData(), int32_t(values.size())) ? "yes" : "no");
    if (e->shaderPasses) {
      WfxContext context{};
      context.structSize = sizeof(WfxContext);
      context.depth = 32;
      context.scale = 1;
      context.rasterWidth = large.width();
      context.rasterHeight = large.height();
      answers << QString("%1 pass(es)").arg(e->shaderPasses(values.constData(), int32_t(values.size()), &context));
    }
    if (!answers.isEmpty()) out << "  at its defaults: " + answers.join(", ");
    // A picture of it, to look at.
    QString name = e->id;
    for (QChar &c : name)
      if (!c.isLetterOrNumber()) c = '-';
    const QString picturePath = info.absolutePath() + "/" + info.completeBaseName() + "." + name + ".check.png";
    const QImage shown = cpuRunPasses(small, packEffectPasses(*e, values, 32, small.size(),
                                                              QRectF(small.width() * .04, small.height() * .05, small.width() * .92,
                                                                     small.height() * .9),
                                                              1, 1, 30));
    if (!shown.isNull() && shown.convertToFormat(QImage::Format_RGBA8888).save(picturePath))
      out << "  its picture at its defaults: " + QDir::toNativeSeparators(picturePath);
  }
  out << "";
  out << (ok ? QString("Everything holds.") : QString("Something needs looking at: see PROBLEM and DIFFER above."));
  const QString report = out.join('\n') + '\n';
  QFile file(info.absolutePath() + "/" + info.fileName() + ".check.txt");
  if (file.open(QIODevice::WriteOnly)) file.write(report.toUtf8());
  std::fputs(report.toUtf8().constData(), stdout);
  std::fflush(stdout);
  return ok ? 0 : 1;
}
