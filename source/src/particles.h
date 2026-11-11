#pragma once
// Particle layers: petals, snow, bokeh, sparks, confetti, dust.
//
// Every particle's place is worked out from its age alone -- where it was born,
// how fast and which way it left, gravity, wind and drag in closed form, a
// smooth wobble on top -- so any frame is drawn directly, without playing the
// ones before it. Scrubbing backwards, jumping, and rendering frames on many
// threads at once all give the same picture.
#include "engine.h"
#include <memory>

struct ParticleState {
  QPointF position; // in the layer's own coordinates
  QPointF velocity; // the same, per second
  double size = 0;      // across, in layer pixels
  double rotation = 0;  // degrees
  double opacity = 1;
  double life = 0;      // 0 at birth .. 1 at the end
  double flutter = 0;   // a phase for shapes that tumble
  QColor color;
  // How deep it stands in the layer's own space, and how fast that changes:
  // nought unless the layer spreads its particles in depth (particleVolume).
  double z = 0;
  double velocityZ = 0;
};
// Everything alive at this layer frame, in the order it is drawn.
std::shared_ptr<const std::vector<ParticleState>> particleStates(const Layer &, const Scene &, double frame);
// What decides this frame's particles, for a cache to be keyed on: the same
// bytes mean the same picture.
QByteArray particleIdentity(const Layer &, const Scene &, double frame);
// The emitter, in the layer's own coordinates: what the viewer's box shows and
// what is grabbed to move the layer. It holds still while the particles spread
// out, fall off the frame and fade, which the box around them never would.
QRectF particleEmitterBounds(const Layer &);
// Where the particles are at this frame, in the layer's own coordinates.
QRectF particleBounds(const Layer &, const Scene &, double frame);
void drawParticles(QPainter &, const Layer &, const Scene &, double frame);
// A 3D particle layer whose particles have depths of their own (Depth or Z
// Speed set): each particle is its own point in the scene, seen through the
// camera -- nearer ones larger, farther ones smaller -- rather than a dot
// painted on the layer's flat plane. Such a layer is drawn in slices between
// the other 3D layers, so a character can stand among the particles: those
// behind it drawn before it, those in front after.
bool particleVolume(const Layer &);
// Makes a particle layer one of those: 3D, its particles spread in depth by
// a share of the frame's width (less for sparks and confetti, which burst
// from a place), its emitter widened and its flow raised to match -- farther
// particles cover more of the frame, nearer ones less. False when it is not a
// particle layer, or already stands in depth. Presets do this themselves.
bool makeParticleLayerDeep(Layer &, const Scene &);
// Draws the particles of a volume layer whose distance from the camera lies in
// [nearDepth, farDepth), far to near, onto a painter holding the composition's
// own transform. `frame` is the layer's own frame.
void drawParticleVolume(QPainter &, const Layer &, const Scene &, double frame, double nearDepth, double farDepth);
// The nearest and farthest distances from the camera of a volume's particles
// at layer frame `frame`; false when none are in front of the camera.
bool particleVolumeDepths(const Layer &, const Scene &, double frame, double &nearest, double &farthest);
// The named starting points: Snow, Petals, Bokeh, Sparks, Confetti, Dust, Fireflies.
QStringList particlePresetNames();
void applyParticlePreset(Layer &, const Scene &, const QString &name);
// A setting from the layer's particle settings, or its default.
double particleSetting(const Layer &, const char *key);
QColor particleColor(const Layer &, const char *key);
int checkParticles(QStringList &report);

// One particle as the pixels it is written into. A soft dot, a disc and a
// bokeh are a distance from a centre, a streak a distance from a line, and a
// petal, square, star or piece of confetti a shape in its own turned units:
// written pixel by pixel, on the CPU or by a graphics card, from these.
struct ParticleMark {
  int left, top, right, bottom; // the pixels it can touch, inclusive
  float cx, cy;                 // centre, or the tail of a streak, in pixels
  float sx, sy, lsx, lsy;       // a streak's tail to head, in pixels and in its own units
  float i11, i12, i21, i22;     // pixels back to the particle's own units
  float radius, pixelScale;     // its radius or half width, in its own units
  float r, g, b, a;             // straight colour 0..255, alpha 0..1
};
// A square, star or piece of confetti -- the shapes the CPU fills with
// QPainter -- as QPainter fills it, so a card can cover each pixel the way
// Qt's raster engine does, and with the colour Qt fills with: eight bits,
// premultiplied. Two rasterizers do it (see outlineOf in particles.cpp):
//   rule 0, the gray rasterizer: from the outline, its corners in the
//     device's pixels on the 1/64 grid Qt puts them on;
//   rule 1, the line rasterizer, for a rectangle whose transform has no shear:
//     its own levels, worked out here, for the pixels of `levelsBox` row by
//     row from `levelsAt` in ParticleOutlines::levels.
struct ParticleOutline {
  float x[10], y[10]; // rule 0: its corners, in pixels
  int corners;        // rule 0: 4 or 10
  int rule;
  float r, g, b, a;   // premultiplied, 0..255
  int levelsAt;       // rule 1
  QRect levelsBox;    // rule 1, in the device's pixels
};
// One outline for each mark, and the levels rule 1 refers to.
struct ParticleOutlines {
  std::vector<ParticleOutline> shapes;
  std::vector<uchar> levels;
};
// Draws a layer's particles -- `marks` in the painter's device pixels, inside
// `area` -- on a painter with a graphics card behind it, as one picture: over
// what is there, or added to it, at the painter's opacity. Squares, stars and
// confetti come with an outline for each mark; the other shapes with none.
// False when it could not, and the particles are drawn the ordinary way.
using ParticleCardHook = std::function<bool(QPainter &, const std::vector<ParticleMark> &marks,
                                            const ParticleOutlines &outlines, QRect area, int shape, bool add)>;
// While one is alive, particles drawn on this thread onto a painter with no
// image behind it go to `hook`.
struct ParticleCardScope {
  explicit ParticleCardScope(const ParticleCardHook *hook);
  ~ParticleCardScope();
  const ParticleCardHook *previous;
};
