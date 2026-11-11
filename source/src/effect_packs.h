#pragma once
// Effects that live in .wfx packs beside the application (sdk/wavy_effect.h):
// Wavy-Flo's own colour, stylize, noise and transition effects, and anyone
// else's written to the same header. Loaded once at start-up, before anything
// is drawn. After that the registry changes only on the interface thread: more
// packs loaded, or an effect written as text read again (reloadTextEffect) --
// which every thread that draws may meet half way through a frame, so an
// effect is never changed in place: its new version takes its id, and the old
// one is kept a while for whoever still holds it.
#include "engine.h"
#include "gpu_pixels.h"
#include "wavy_effect.h"
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <vector>

namespace cpu_shader { struct Program; }

struct PackEffect {
  const WfxEffect *api = nullptr;
  QString id, name, category, file;
  int valueCount = 0;
  std::vector<int> offsets; // where each parameter starts in the value list
  // What a pack made for SDK 6 adds; null for one made for 5, whose
  // description ends before these.
  double (*reachWithData)(const float *, int32_t, double, const WfxContext *) = nullptr;
  int (*pressed)(const char *, const WfxPress *) = nullptr;
  int32_t (*describe)(const WfxContext *, char *, int32_t) = nullptr;
  QStringList dataIds; // its WFX_DATA parameters, in order
  // What SDK 7 adds for Effect Setting; empty for a pack made for 5 or 6.
  const char *const *help = nullptr;
  std::function<uint32_t(const float *, int32_t, int32_t)> controlState;
  // What SDK 8 adds: a shader run in several passes; empty before 8.
  std::function<int32_t(const float *, int32_t, const WfxContext *)> shaderPasses;
  // What the effect says about its values (inert(), varies(), reach()): the
  // pack's own functions, or for an effect written as text (.wfxs) its
  // expressions. Empty when it says nothing.
  std::function<int(const float *, int32_t)> inert, varies;
  std::function<double(const float *, int32_t, double)> reach;
  // The shader as the host runs it, made once: every layer with the effect
  // asks for it every frame. From SDK 10, with what the host puts before an
  // effect's own body (its parameters by name, the helpers).
  QString shader;
  // From SDK 10 an effect may bring a shader and nothing else: the processor
  // then runs that same shader, compiled here when the effect is loaded.
  std::shared_ptr<const cpu_shader::Program> cpuProgram;
  // An effect written as text: its description (api points into it) and the
  // strings it is made of.
  std::shared_ptr<const void> description;
  // Which version of it this is, when it was read again while running: part of
  // what a layer with it looks like to the caches. 0 for one loaded at start.
  quint64 revision = 0;
};

// Why a description cannot be used, or nothing when it can.
QString packEffectProblem(const WfxEffect *);

// The files matching `patterns` in `folder`, as paths relative to it: with
// `subfolders`, those in its folders too, at any depth (a maker's folder of
// its own, as After Effects' Plug-ins folder takes them), the folder's own
// first and then by path. An OpenFX bundle (*.ofx.bundle) is not looked into.
QStringList effectFilesIn(const QString &folder, const QStringList &patterns, bool subfolders);
// Loads every .wfx, and every effect written as text (.wfxs), in `folder` and
// its subfolders, not loaded yet -- with `textOnly`, the effects written as
// text alone, from the folder itself (a folder of the person's own, where
// nothing that runs as a program is taken from). Returns how many effects
// were added.
int loadEffectPacks(const QString &folder, bool textOnly = false);
// Where a person's own effects written as text live: Documents/Wavy-Flo/Effects,
// or the folder WAVY_EFFECTS_FOLDER names (the self-tests use a scratch one).
QString userEffectsFolder();
// Reads the effect written as text in `file` again -- or, given `text`, takes
// that instead of what the file holds (an effect being edited, not saved yet).
// One that no longer loads leaves the version that last did in use, and says
// why in textEffectProblem; one whose file is gone is taken away; a file not
// loaded before is added. Interface thread only. Returns the ids whose effect
// changed (the new and, when it was renamed, the old), empty when none did.
QStringList reloadTextEffect(const QString &file, const QByteArray *text = nullptr);
// Why the effect written as text in `file` does not load as it is now, or nothing.
QString textEffectProblem(const QString &file);
// Every such file, with why.
QMap<QString, QString> textEffectProblems();
// The id the effect written as text in `file` goes by, or nothing.
QString textEffectId(const QString &file);
// One effect written as text: a GLSL body after a leading comment that
// describes it ("/* Wavy-Flo effect {json} */", sdk/EFFECT-TEXT.md). Null with
// `problem` saying why when it cannot be used. `file` is where it came from.
std::unique_ptr<PackEffect> readTextEffect(const QByteArray &text, const QString &file, QString *problem);
const PackEffect *packEffect(const QString &id);
std::vector<const PackEffect *> packEffects();
// Packs that were found but refused, with why.
QStringList packProblems();

// A parameter's own curve among the layer's extra curves.
QString packCurveId(const QString &effect, const QString &parameter);
// Where a first-party effect keeps a parameter that was a layer property before
// effects lived in packs (the wipes' sliders), or -1.
int packLegacyProperty(const QString &effect, const QString &parameter);
// The values the effect gets for this layer at this layer frame: animated
// curves at the frame, stored settings otherwise, defaults for the rest.
QVector<float> packEffectValues(const Layer &, const PackEffect &, double frame);
bool packEffectApplied(const Layer &, const PackEffect &);
// A WFX_DATA parameter's bytes, kept with the layer (base64 in its settings).
QByteArray packEffectData(const Layer &, const PackEffect &, const QString &id);
void setPackEffectData(Layer &, const PackEffect &, const QString &id, const QByteArray &bytes);
// The effect's own line about its data (describe()), or nothing.
QString packEffectDescription(const Layer &, const PackEffect &);
bool packEffectInert(const PackEffect &, const QVector<float> &values);

// Across all the layer's applied pack effects.
bool layerCarriesPackEffect(const Layer &);             // any at all, whatever its settings
bool layerHasPackEffect(const Layer &, double frame);   // any that does something
bool layerPackEffectMoves(const Layer &, double frame); // any whose picture changes with time
double layerPackReach(const Layer &);                   // widest reach at any key, in layer pixels
bool layerPackNeedsWholeLayer(const Layer &);           // one reads the whole layer for any pixel (tiling)
QByteArray layerPackIdentity(const Layer &, double frame);
// Which versions of its effects the layer has, where one was read again while
// running; empty for every other layer. For the frame cache's per-layer key.
QByteArray layerPackRevisions(const Layer &);

// Runs one effect on a straight-alpha float raster (RGBA32FPx4). `owner` is
// the layer it belongs to, for effects that keep data.
void runPackEffect(QImage &straight, const PackEffect &, const QVector<float> &values, QRectF box,
                   double scale, double seconds, double frame, const Layer *owner = nullptr);
// The same on a raster in any format, left in the one the effect works in
// (premultiplied float for one that moves pixels, straight float otherwise):
// converted straight from what it was and not back, so a layer's chain turns
// its pixels over only when the next step needs them another way.
void runPackEffectInPlace(QImage &image, const PackEffect &, const QVector<float> &values, QRectF box,
                          double scale, double seconds, double frame, const Layer *owner = nullptr);
// Whether an effect works pixel by pixel in float (WFX_PER_PIXEL), so it can
// be run a strip of the raster at a time.
bool packEffectPerPixel(const PackEffect &);
// Whether running it on a part of a raster -- told where the part lies --
// gives there the pixels the whole raster would: a colour grade worked a span
// at a time, or an effect worked pixel by pixel by its own process(), which
// reads where a pixel is from its context (a grain keyed to the raster, a
// vignette to the box).
bool packEffectPartable(const PackEffect &);
// While one is alive, a raster handed to the pack effects on this thread is
// part of a larger one: its top left at `origin` in a raster of `raster`
// pixels, which is where they are told it lies -- the part of an Effect Layer
// over a dragged layer comes out as the whole would there.
struct RasterPartScope {
  RasterPartScope(QPoint origin, QSize raster);
  ~RasterPartScope();
  QPoint previousOrigin;
  QSize previousRaster;
};
// Such effects one after another on `image`, which comes back in
// `outputFormat`: the same pixels as running each on the whole raster and
// converting at the end, made a strip at a time across the cores. False, with
// `image` untouched, when one is not per-pixel or one fails -- the caller then
// runs them the ordinary way.
struct PackStep { const PackEffect *effect = nullptr; QVector<float> values; };
bool runPerPixelPacks(QImage &image, QImage::Format outputFormat, const std::vector<PackStep> &steps, QRectF box,
                      double scale, double seconds, double frame, const Layer *owner = nullptr);
// A WFX_BUTTON pressed on `layer`: runs the effect's pressed() with the picture
// going into the effect (straight RGBA32F, may be null). Blocks until it
// returns -- the caller keeps the interface alive around it. On success the
// data it set is in `changed` (id -> bytes); `message` is what it said.
int pressPackButton(const PackEffect &, const QString &button, const Layer &layer, const QImage &picture,
                    const QString &pictureNote, double frame, QHash<QString, QByteArray> &changed, QString &message);
// The picture going into `effect` on the layer at `index` at `frame`, as After
// Effects' render "upstream of the effect" gives it: the layer in its own
// pixels with this effect and every one after it left out -- for an Effect
// Layer, the picture beneath it where it acts -- at full resolution, the long
// edge held to 2560. Straight RGBA32F. `note` says what it is, or why there is
// none (a null image).
QImage packEffectInput(const Scene &, int index, const QString &effect, double frame, QString &note);
// The effect as a pass in a card chain; empty when it has no shader. `box`
// (raster pixels), `scale` and the time are what its WfxContext would carry.
bool packEffectPass(const PackEffect &, const QVector<float> &values, int depth, GpuPass &pass,
                   QRectF box = {}, double scale = 1, double seconds = 0, double frame = 0);
// The same as every pass it takes, for a raster of `raster` pixels: one for
// most, several for a shader run in rounds (SDK 8). Empty when it has none.
QVector<GpuPass> packEffectPasses(const PackEffect &, const QVector<float> &values, int depth, QSize raster,
                                  QRectF box = {}, double scale = 1, double seconds = 0, double frame = 0);

// What the host puts before an SDK 10 effect's shader body: each parameter by
// name (param_<id>), the helpers (wavySample, wavyRandom, ...), and a #line so
// the body's own lines read from 1.
QString packShaderPrelude(const WfxParameter *parameters, int count);
// A shader body (as passes carry it) compiled for the processor from the same
// text the card compiles (packShaderSource). Kept: asked for every frame. Null
// with `problem` saying why when it cannot be compiled here.
std::shared_ptr<const cpu_shader::Program> cpuShaderProgram(const QString &body, QString *problem = nullptr);
// A chain of passes on the processor, as gpuRunPasses runs it on the card: the
// same shaders, the same inputs, the same order. Null when a pass has no
// shader of its own (a built-in op) or one that cannot be compiled here.
QImage cpuRunPasses(const QImage &source, const QVector<GpuPass> &passes);

// When the first effects a layer applies are per-pixel pack effects (colour
// grades, Tint) and its raster is 8-bit premultiplied, applies all of them in
// one pass straight over the 8-bit pixels and returns their names, for the
// caller to skip. `inert` names effects that do nothing in this call.
QSet<QString> gradeLeadingEightBit(QImage &image, const Layer &, double frame, double seconds, QRectF box,
                                   double scale, const std::function<bool(const QString &)> &inert = {});

int checkEffectPacks(QStringList &failures);

// --check-effect <file>: what an effect's author runs on a .wfxs or a .wfx
// before handing it out. Every effect in the file is loaded as Wavy-Flo loads
// it; its shader is compiled for the processor and for the card, run over a
// test picture on both -- at its defaults and with every slider at either end
// -- the two compared, and both timed at 1920x1080 beside process() when it
// has one. The report goes to <file>.check.txt and to standard output, with a
// picture of the result at its defaults beside it; 0 when everything holds.
int checkEffectCommand(const QString &file);
