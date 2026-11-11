#pragma once
// Wavy-Flo effect packs: .wfx
//
// A pack is a shared library with the extension .wfx placed in the
// application's effects folder. One pack can hold any number of effects; the
// host asks how many there are and for each one's description. Wavy-Flo's own
// colour, stylize, noise and transition effects are packs built on exactly
// this header -- nothing here is reserved for the application.
//
// Everything is plain C: no toolkit, no classes across the boundary, no
// allocation the host has to free. Descriptions (strings, parameter lists) are
// static and must stay valid until the pack is unloaded.
#include <stdint.h>
#ifdef _WIN32
#define WFX_EXPORT extern "C" __declspec(dllexport)
#else
#define WFX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// 6 adds data and buttons (WFX_DATA, WFX_BUTTON, pressed(), describe(),
// reachWithData()) at the end of what 5 had; 7 adds how the controls read in
// Effect Setting (WFX_GROUP, help, controlState()); 8 lets a shader run in
// several passes (shaderPasses()); 9 adds layer and mask controls, and 2D/3D
// point controls (WFX_LAYER, WFX_MASK, WFX_POINT, WFX_POINT_3D); 10 lets an
// effect bring a shader and nothing else -- the host runs that one shader on
// the graphics card and, compiled for it, on every core of the processor --
// and gives the shader its parameters by name and a set of helpers (see
// `shader` below). The description is the same size as 9's. A host that knows
// 10 loads packs made for 5 to 9 unchanged.
#define WFX_ABI_VERSION 10

// ---------------------------------------------------------------------------
// Parameters: what the host draws controls for, saves with the project, and
// can animate. Values reach the effect as floats, in declared order.
// ---------------------------------------------------------------------------
enum WfxParameterKind {
  WFX_FLOAT = 1,  // one float in [minimum, maximum]
  WFX_BOOL = 2,   // one float, 0 or 1
  WFX_CHOICE = 3, // one float, 0 .. number of choices - 1
  WFX_COLOR = 4,  // four floats: red, green, blue, alpha, each 0..1
  WFX_CURVE = 5,  // five floats 0..1: the output at inputs 0, .25, .5, .75 and 1;
                  // it starts as the straight line. Consecutive curves are shown
                  // as channels of one curve editor.
  // ABI 6. Neither takes a float in the value list.
  WFX_DATA = 6,   // bytes the effect owns (a whole look, a table), saved with the
                  // layer and handed back in WfxContext::data; not animatable, no control
  WFX_BUTTON = 7, // a button labelled `label`; pressing it calls pressed()
  // ABI 7: how the controls are laid out. Neither takes a float or is saved.
  WFX_GROUP = 8,     // the controls after it, up to its WFX_GROUP_END, fold under a
                     // twirl named `label`. Groups nest. WFX_PARAM_COLLAPSED starts it shut.
  WFX_GROUP_END = 9, // closes the innermost open group; every group must be closed
  // ABI 9: After Effects style layer, mask, and 2D/3D point controls
  WFX_LAYER = 10,    // one float: index of selected layer in composition (-1 for None)
  WFX_MASK = 11,     // one float: index of selected mask on layer (-1 for None, -2 for All Masks)
  WFX_POINT = 12,    // two floats: x and y position (in layer pixels or comp pixels)
  WFX_POINT_3D = 13  // three floats: x, y, and z position
};
enum WfxParameterFlags {
  WFX_PARAM_WHEEL = 1,     // a colour chosen on a hue/saturation wheel instead of a swatch
  WFX_PARAM_STILL = 2,     // not animatable: the host shows no stopwatch
  WFX_PARAM_COLLAPSED = 4, // ABI 7, WFX_GROUP: shut until the person opens it
  WFX_PARAM_ANGLE = 8,     // After Effects style rotary angle dial control with revolutions
  WFX_PARAM_ALL_MASKS = 16 // ABI 9, WFX_MASK: includes "All Masks" option in dropdown (-2)
};
struct WfxParameter {
  uint32_t structSize; // sizeof(WfxParameter)
  uint32_t kind;       // WfxParameterKind
  const char *id;      // stable ASCII, unique within the effect, saved in projects
  const char *label;   // UTF-8, what the person reads
  float minimum, maximum, initial, step;
  const char *choices; // WFX_CHOICE: labels separated by newlines
  const char *unit;    // shown after a number (" %", "\xc2\xb0", " px"); may be null
  const char *section; // a heading shown above this parameter; may be null
  uint32_t flags;      // WfxParameterFlags
  float initialColor[4]; // WFX_COLOR: its default
};
// How many floats a parameter takes in the value list.
static inline int wfxParameterFloats(const WfxParameter *p) {
  return !p ? 0 : p->kind == WFX_COLOR ? 4 : p->kind == WFX_CURVE ? 5
         : p->kind == WFX_POINT ? 2 : p->kind == WFX_POINT_3D ? 3
         : (p->kind == WFX_DATA || p->kind == WFX_BUTTON || p->kind == WFX_GROUP || p->kind == WFX_GROUP_END) ? 0 : 1;
}

// ABI 6: one WFX_DATA parameter's bytes, as the layer keeps them.
struct WfxData {
  const char *id;       // the parameter's id
  const uint8_t *bytes; // null when there are none yet
  int32_t size;
};

// ---------------------------------------------------------------------------
// What the host tells an effect about the picture it is working on.
// ---------------------------------------------------------------------------
struct WfxContext {
  uint32_t structSize;  // sizeof(WfxContext)
  int32_t depth;        // the pixels handed over: 8 (RGBA8), 16 (RGBA16) or 32 (RGBA32F)
  double seconds;       // the layer's own time, for effects that move by themselves
  double frame;         // the same, in frames
  double scale;         // raster pixels per layer pixel: .5 in a half-resolution preview,
                        // so sizes given in layer pixels stay the same on screen
  // The layer's content box within the raster, in raster pixels. A raster is
  // larger than its content when other effects need room around it.
  double boxX, boxY, boxWidth, boxHeight;
  // Runs job(begin, end, user) over 0..count split across the host's workers,
  // and returns when all of them have. Nested calls run inline.
  void (*parallelFor)(int32_t count, void (*job)(int32_t begin, int32_t end, void *user), void *user);
  // ABI 6: the effect's WFX_DATA parameters for this layer, in declared order.
  // Valid for the length of the call. A host older than 6 gives a shorter
  // context: read these only when structSize reaches them.
  int32_t dataCount;
  const WfxData *data;
  // Where the pixels handed over lie in the layer's raster, and the raster's
  // whole size. An effect flagged WFX_PER_PIXEL may be handed the raster a strip
  // of rows at a time: its pixel (x, y) is then the raster's pixel
  // (originX + x, originY + y), and the box above stays in raster pixels. Zero
  // and the size of what was handed over when it is the whole raster. Added
  // after ABI 7 without changing it: read these only when structSize reaches
  // them, and take the whole buffer as the raster when it does not.
  int32_t originX, originY;
  int32_t rasterWidth, rasterHeight;
};

// ABI 6: what the host hands pressed() when a WFX_BUTTON is pressed.
struct WfxPress {
  uint32_t structSize;          // sizeof(WfxPress)
  const WfxContext *context;    // the layer's data (and box, time) as they are now
  const float *values;          // the effect's values at the playhead
  int32_t valueCount;
  // The picture going into this effect at the playhead, in its own layer's
  // pixels at full resolution: straight RGBA32F, stride in bytes. Null when
  // none could be made.
  const float *picture;
  int32_t pictureWidth, pictureHeight, pictureStride;
  const char *pictureNote;      // where the picture came from, for the person; may be null
  void *host;                   // pass back to the calls below
  // Replaces a WFX_DATA parameter's bytes; the host copies them. Takes effect,
  // as one undoable change, once pressed() returns 0.
  void (*setData)(void *host, const char *id, const uint8_t *bytes, int32_t size);
  // Something to tell the person: why nothing changed, what went wrong.
  void (*message)(void *host, const char *text);
};

enum WfxEffectFlags {
  WFX_MOVES_ALONE = 1,   // the picture can change with time while no value does; see varies()
  WFX_PREMULTIPLIED = 2, // process() gets and gives premultiplied colour: right for anything
                         // that moves or mixes pixels, where straight colour would bleed
  WFX_WHOLE_LAYER = 4,   // any pixel may come from anywhere in the layer (tiling, mirroring):
                         // the host always hands over the whole layer, never just the part
                         // on screen or one band of it
  WFX_PER_PIXEL = 8      // each pixel out depends on nothing but the same pixel in, where it
                         // lies in the raster (WfxContext::originX/originY) and the values:
                         // no neighbours, nothing measured over the picture. The host may
                         // then run it, and the conversions around it, a strip at a time,
                         // which keeps a full frame's float copy out of memory altogether
};

struct WfxEffect {
  uint32_t structSize;  // sizeof(WfxEffect)
  uint32_t abiVersion;  // WFX_ABI_VERSION
  const char *id;       // stable, saved in projects and effect orders
  const char *name;     // what the person reads
  const char *category; // where the effect browser files it
  uint32_t supportedDepths; // bit 1 = RGBA8, bit 2 = RGBA16, bit 4 = RGBA32F
  uint32_t flags;           // WfxEffectFlags
  int32_t parameterCount;   // at most 64
  const WfxParameter *parameters;

  // The effect itself: straight-alpha RGBA, in place, row stride in bytes.
  // Return 0 on success. Never keep the pixel pointer after returning.
  // ABI 10: may be null when `shader` is given. The host then runs the shader
  // everywhere -- on the card, and where there is none (or the frame is drawn
  // on the processor anyway: a render, a busy preview) compiled for the
  // processor and spread over its cores, a row of pixels at a time. Such an
  // effect is refused when it loads if its shader does not compile there, with
  // the line and why.
  int (*process)(void *pixels, int32_t width, int32_t height, int32_t stride,
                 const float *values, int32_t valueCount, const WfxContext *context);

  // Optional. Nonzero when, at these values, the effect changes nothing: the
  // host then skips it entirely, and a layer needs no raster for it.
  int (*inert)(const float *values, int32_t valueCount);
  // Optional, with WFX_MOVES_ALONE. Nonzero when at these values the picture
  // changes with time (a noise with a speed); the host then renders every frame.
  int (*varies)(const float *values, int32_t valueCount);
  // Optional. How far, in layer pixels, the effect can carry a pixel beyond
  // where it was (a colour fringe, a displacement). The host makes that much
  // room around the layer so nothing is cut off. Round it up to a coarse step
  // if it follows a slider, so dragging does not resize the raster each time.
  double (*reach)(const float *values, int32_t valueCount, double scale);

  // Optional, for effects that treat every pixel on its own (colour work).
  // With these the host can run several such effects in one pass over 8-bit
  // pixels without widening the whole picture to float for each of them.
  // prepare() works out anything that depends only on the values (a table, a
  // matrix) and returns it; span() applies the effect to `pixels` straight
  // float RGBA pixels, leaving alpha alone and skipping pixels whose alpha is
  // 0; release() frees what prepare() made. span() may run on several threads
  // at once with the same prepared state.
  void *(*prepare)(const float *values, int32_t valueCount, const WfxContext *context);
  void (*span)(float *rgba, int32_t pixels, const void *prepared);
  void (*release)(void *prepared);

  // Optional: the same maths for the graphics card, as a GLSL 3.30 body
  // defining exactly
  //   vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size)
  // colour is straight alpha; the values arrive in `uniform float values[256]`,
  // packed as they are for process(). The host runs it inside its own chain,
  // next to its own effects, so the picture never leaves the card between them.
  // coord is the pixel's centre in the raster (x + .5, row + .5, the top row
  // first) and size the raster's. The body may also read what the context
  // carries -- `wavySeconds`, `wavyFrame`, `wavyScale` and `wavyBox` (x, y,
  // width, height in raster pixels, all zero when there is none) -- and call
  //   vec4 wavyPixel(ivec2 pixel)
  // for any pixel of the picture entering it, straight alpha, transparent
  // beyond the raster; with shaderPasses(), `wavyPass` says which pass it is
  // and `wavyPasses` how many there are, and
  //   vec4 wavyOriginal(ivec2 pixel)
  // reads the picture as it came to the first pass (SDK 8). Between its own
  // passes an effect may keep whatever it likes in the four channels: what a
  // pass returns is what the next one reads, unconverted; only the last
  // pass's colour must be straight alpha again.
  //
  // ABI 10 puts these before the body (its own lines still count from 1):
  //   param_<id>        each parameter by its id: a float, a bool (switch), an
  //                     int (choice, layer, mask), a vec4 (colour), a vec2 or
  //                     vec3 (point), or for a curve a function param_<id>(x)
  //   wavySample(vec2 p)      the picture between pixels, p in raster pixels
  //                           (a pixel's centre at x + .5), mixed in premultiplied
  //                           colour, clear beyond the raster; wavySampleEdge(p)
  //                           holds the edge instead
  //   wavyRandom(vec2 p, float seed)  0..1, the same for the same whole pixel
  //   wavyNoise(vec2 p, float seed)   smooth value noise, 0..1
  //   wavyHash(uint)  wavyLuma(vec3)  wavyCurve(int at, float x)
  //   wavyPremultiply(vec4)  wavyUnpremultiply(vec4)
  // They give the same numbers on every card and on the processor, which
  // fract(sin(...)) and the card's own texture filtering do not. The processor
  // takes GLSL 3.30 as a fragment shader uses it, apart from structs, switch,
  // derivatives (dFdx, dFdy, fwidth) and texture functions other than
  // texelFetch, texture and textureSize; `discard` leaves the pixel clear.
  const char *shader;

  // --- ABI 6 -----------------------------------------------------------------
  // Optional. reach() for an effect whose reach depends on its data; the
  // context carries the data and the layer's box. Asked instead of reach().
  double (*reachWithData)(const float *values, int32_t valueCount, double scale, const WfxContext *context);
  // Optional. A WFX_BUTTON was pressed. Runs away from the interface thread
  // while the host waits (it may open a window of its own and wait for it).
  // Return 0 to keep what setData() was given, anything else to drop it.
  int (*pressed)(const char *button, const WfxPress *press);
  // Optional. A line about the effect's data for the person ("Exposure, Curves"),
  // UTF-8 into `text`; returns its length, 0 for nothing to say.
  int32_t (*describe)(const WfxContext *context, char *text, int32_t capacity);

  // --- ABI 7: how the controls read in Effect Setting -------------------------
  // Every control is one line: a stopwatch, the name, the value beside it.
  // WFX_GROUP entries fold lines under a twirl; a parameter's `section`, as
  // before, starts a group of its own that runs to the next section.
  //
  // Optional. What each control does, one line each, in the order of
  // `parameters` (groups included; null where there is nothing to say). Shown
  // when the pointer rests on the control.
  const char *const *help;
  // Optional. Whether a control is of use at these values (the ones at the
  // playhead): 0 shows it as usual, WFX_CONTROL_DIMMED greys it out (it keeps
  // its value), WFX_CONTROL_HIDDEN takes its line away. `parameter` indexes
  // `parameters`; for a WFX_GROUP it covers the whole group. Asked after every
  // change, so keep it cheap.
  uint32_t (*controlState)(const float *values, int32_t valueCount, int32_t parameter);

  // --- ABI 8 -----------------------------------------------------------------
  // Optional, with `shader`: how many times the shader runs, one after the
  // other, each pass reading (through wavyPixel and its colour) what the pass
  // before it made, and knowing which it is by `int wavyPass`, 0 first, of
  // `int wavyPasses` (the number returned here). For a
  // blur done in rounds, as process() does it. The context carries the
  // raster's size (rasterWidth, rasterHeight). Null runs it once; 0 leaves
  // this run to process() (values the shader has no form for). For an effect
  // without process() (ABI 10), 0 runs it once and more than 64 runs it 64 times.
  int32_t (*shaderPasses)(const float *values, int32_t valueCount, const WfxContext *context);
};
enum WfxControlState {
  WFX_CONTROL_DIMMED = 1,
  WFX_CONTROL_HIDDEN = 2
};

// A pack exports these two.
WFX_EXPORT int32_t wfxEffectCount(void);
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index);
