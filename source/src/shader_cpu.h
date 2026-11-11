#pragma once
// Shaders on the processor.
//
// A pack effect's shader is written once, in GLSL, for the graphics card. This
// compiles that same text -- the whole fragment shader, the host's wrapper
// around the effect's body included -- into a program the processor runs, a
// row of pixels at a time on every core. An effect that brings only a shader
// then runs everywhere: on the card where there is one, and on the cores where
// there is not, or where the frame is drawn there anyway. Nobody writes the
// same effect twice, and nobody has to make two versions agree.
//
// The language is GLSL 3.30 as a fragment shader uses it: bool, int, uint,
// float, their vectors, mat2..mat4, fixed-size arrays, functions (in, out,
// inout), if, for, while, do, break, continue, return, discard, the ternary
// and every operator; the built-in functions of 3.30 apart from derivatives;
// texelFetch, texture and textureSize on the two pictures the host binds; and
// a preprocessor with #define (with or without arguments), #undef, #if,
// #ifdef, #ifndef, #elif, #else and #endif. Not supported: structs, switch,
// derivatives (dFdx, dFdy, fwidth), and texture functions other than those.
//
// Pixels differing in which way an if goes, or how many times a loop runs, are
// worked on together: each lane carries a mask, as the card itself does it.
#include <QImage>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>
#include <memory>

namespace cpu_shader {

struct Program; // compiled; never changes; shared between threads

// What a program reads besides its pixels: the host's uniforms.
struct Inputs {
  const float *values = nullptr; // uniform float values[256]; missing entries read 0
  int valueCount = 0;
  float seconds = 0, frame = 0, scale = 1; // wavySeconds, wavyFrame, wavyScale
  float box[4] = {0, 0, 0, 0};             // wavyBox
  int depth = 32;                          // processingDepth
  int pass = 0, passes = 1;                // wavyPass, wavyPasses
  int rasterWidth = 0, rasterHeight = 0;   // sourceSize for evaluate(); run() uses its raster's
};

// Compiles a complete fragment shader. `entry` is "main" for a shader whose
// main() writes `out vec4 result`; or the name of a function taking nothing
// and returning a bool, int, uint or float, for a value worked out from the
// inputs alone (see evaluate()). Null, with `problem` saying where and why,
// when the text cannot be compiled.
std::shared_ptr<const Program> compile(const QString &source, const QString &entry, QString *problem);

// One pass of a `main` program over `input`, into `output`: both RGBA32F
// (Format_RGBA32FPx4, or its premultiplied twin: the four numbers of a pixel
// are read as they are, as the card reads a float texture), the same size,
// and not the same image. `original` is what
// wavyOriginalTexture reads (the picture as it came to the first pass; may be
// `input`). A strip of a larger raster is placed by `origin` within a raster
// of `raster` pixels: gl_FragCoord and sourceSize are the raster's, and the
// picture beyond the strip reads as clear. Rows are spread over the host's
// workers. False when the program cannot run (wrong entry, wrong sizes).
bool run(const Program &, const QImage &input, const QImage &original, QImage &output, const Inputs &,
         QPoint origin = {}, QSize raster = {});

// The value of a program compiled with a value-returning entry, at these
// inputs. NaN when it has no such entry.
double evaluate(const Program &, const Inputs &);

// Registers and instructions in a program, for the checks that hold a
// compiled shader to a size.
int registerCount(const Program &);
int instructionCount(const Program &);

// The compiler and the machine checked against small shaders with known
// answers. Appends what failed; returns how many checks ran.
int check(QStringList &failures);

} // namespace cpu_shader
