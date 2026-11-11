#pragma once
#include "engine.h"
#include <QVector>
// GUI-thread GPU operations. Null means unsupported/unavailable; caller falls back.
void enableGpuPixels(bool enabled);
bool canUseGpuPixels();
// Gives worker threads a card too: a thread of its own owns a context whose
// handles are made here, on the GUI thread, and workers hand their chains to it.
// Call once from the GUI thread; without a usable card it does nothing.
void startGpuPixelService();
void stopGpuPixelService();
// Scoped per-thread CPU execution; does not change the user's compositor.
struct CpuPixelScope { bool previous; CpuPixelScope(); ~CpuPixelScope(); };
quint64 gpuPixelOperations();
QImage gpuPixelOperation(const QImage &, int operation, const QVector4D &params = {},
                        const QVector4D &a = {}, const QVector4D &b = {},
                        const QImage &aux = {});
// One step of an effect, as the card sees it: a shader op, its uniforms, the
// size it writes, and where its two inputs come from. Effects are written as a
// list of these and the runner does the rest -- allocation, ping-ponging,
// viewport -- so a new effect gets the whole pipeline by describing itself.
struct GpuPass {
  enum Input { Current, Previous }; // the image entering this effect, or the last pass
  int op = 0;
  QVector4D params, a, b;
  Input input = Current;
  QSize size;                    // empty: the size of the image entering the effect
  Input auxFrom = Previous;      // where the second sampler reads, when used
  bool useAux = false;
  bool auxSmooth = false;        // interpolate the second sampler, for scaled reads
  QImage auxImage;               // an uploaded second input instead of a slot
  bool endsEffect = false;       // this pass's result becomes the current image
  // A program of its own for this pass, compiled from a GLSL body supplied by
  // a plugin, with its parameters as a float array. Empty means the shared
  // effect shader and the op above. Everything else -- allocation, ping-pong,
  // viewport, upload, readback -- is the same either way, which is the point:
  // a plugin gets the whole pipeline for the price of a function body.
  QString source;
  QVector<float> values;
  int processingDepth = 8; // custom shader: original SDK channel depth
  // What the effect knows of where and when it is (WfxContext): the layer's
  // time, raster pixels per layer pixel, and its content box in raster pixels.
  // A custom shader reads them as wavySeconds, wavyFrame, wavyScale, wavyBox.
  double seconds = 0, frame = 0, scale = 1;
  QRectF box;
  int shaderPass = 0, shaderPasses = 1; // which round of a shader run in several, of how many (wavyPass, wavyPasses)
};
// The whole program around a plugin's or a pack's GLSL body: the uniforms it
// is promised, and what it may call (wavyPixel, to read any pixel of the
// picture entering it). One source for every runner, so a body behaves the
// same wherever it is run.
QString packShaderSource(const QString &body);

// Whether a body compiles here at all. Asked once per body; the answer is kept,
// including the failure, so a snippet that cannot compile is not retried on
// every frame. Empty log means it compiled.
QString gpuShaderProblem(const QString &body);

// Run a whole chain in one submission: the image goes up once, every pass runs
// between textures, and the result comes back once. Null means the caller must
// fall back to the CPU, and the chain must then be run there in full.
QImage gpuRunPasses(const QImage &source, const QVector<GpuPass> &passes);

// The exact Euclidean distance transform on the card: for every pixel of a
// width x height grid, the squared distance from its centre to the centre of
// the nearest pixel whose id is not 0 (1e12 or more where there is none), and,
// when `nearest` is given, that pixel's id. The same lower-envelope algorithm,
// in the same float arithmetic, as the CPU's distance fields, so the distances
// are the same numbers. False when there is no card that can (no OpenGL 4.3
// compute shaders, not enough memory for the grid): the caller works it out on
// the processor, as it always did.
bool gpuDistanceTransform(const std::vector<qint32> &ids, int width, int height, std::vector<float> &squared,
                          std::vector<qint32> *nearest = nullptr);
// How many distance transforms the card has worked out, and why the last one
// it was asked for was not (empty when it was), for the checks.
quint64 gpuDistanceTransforms();
QString gpuDistanceProblem();

// Glow and drop shadow in one submission, without leaving the graphics card.
// Null means the caller must fall back to the CPU.
QImage gpuHaloBehind(const QImage &, double blurRadius, QColor, QPointF offset);
QVector<GpuPass> gpuHaloPasses(QSize, double blurRadius, QColor, QPointF offset);
QVector<GpuPass> gpuHaloPasses(QSize, double blurRadius, const QVector4D &colour, QPointF offset);
QVector<GpuPass> gpuBlurPasses(QSize, double blurRadius);
// The same blur, resolved at the resolution it can actually express. Use this
// wherever the result is looked at rather than compared against the exact one.
QVector<GpuPass> gpuSoftenPasses(QSize, double radius);
// Both distance fields measured on the card by jump flooding, then the
// stroke blended with them. Empty when the stroke would draw nothing.
QVector<GpuPass> gpuStrokePasses(QSize, double width, int mode, QColor);
// Straight alpha for the passes that reason about coverage, premultiplied for
// the ones that filter. Converting is a pass like any other here, rather than a
// walk over tens of megabytes of RAM between one effect and the next.
QVector<GpuPass> gpuAlphaPass(bool toPremultiplied);
// The effect shader, so the compositor can build the same program in its own
// context and run a chain on textures that never leave it.
const char *effectVertexShader();
const char *effectFragmentShader();
QString gpuPixelStatus();
int checkGpuPixels(QStringList &failures);
