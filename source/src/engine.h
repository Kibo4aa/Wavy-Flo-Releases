#pragma once
#include <QtWidgets>
#include <QPicture>
#include <QMatrix4x4>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
struct Key {
  double frame = 0, value = 0;
  QString ease = "linear";
  std::array<double, 4> curve{.42, 0, .58, 1};
  bool custom = false, absolute = false, continuous = true;
  bool hasSpatial = false;
  QPointF spatialOut{0, 0};
  QPointF spatialIn{0, 0};
  bool hasNormCurve = false;
  std::array<double, 2> normCurve{0.0, 0.0};
  // Position only: a key whose value was set in the graph editor to time the
  // move -- an anticipation, an overshoot -- not to put a point on the motion
  // path. The path keeps its points; this only says how far along it the
  // layer is (layerLocalPosition). Set by dragging its value in the graph,
  // cleared by placing the layer at its frame.
  bool timing = false;
};
struct Layer;
QPointF layerLocalPosition(const Layer &l, double t);
// The frames Position's path has its points at: where X and Y are both keyed.
// A key on one alone between them only times the move along the path
// (layerLocalPosition). Fewer than two such frames: every key's.
std::vector<double> positionPathFrames(const Layer &l);
struct ExpressionProgram;
struct Property {
  double base = 0;
  std::vector<Key> keys;
  // An expression computes the value from the keyed one (value), time, other
  // layers... It is kept as text, and compiled once when it is set.
  QString expression;
  bool expressionOn = true;
  std::shared_ptr<const ExpressionProgram> program;
  // Which of a layer's fixed properties this is; -1 for any other curve. It
  // says where the property sits, not what it holds, so a copy made on its own
  // has none and a property assigned into a layer keeps the place it is in.
  int slot = -1;
  Property() = default;
  Property(const Property &o)
      : base(o.base), keys(o.keys), expression(o.expression), expressionOn(o.expressionOn), program(o.program) {}
  Property(Property &&o) noexcept
      : base(o.base), keys(std::move(o.keys)), expression(std::move(o.expression)),
        expressionOn(o.expressionOn), program(std::move(o.program)) {}
  Property &operator=(const Property &o) {
    base = o.base; keys = o.keys; expression = o.expression; expressionOn = o.expressionOn; program = o.program;
    return *this;
  }
  Property &operator=(Property &&o) noexcept {
    base = o.base; keys = std::move(o.keys); expression = std::move(o.expression);
    expressionOn = o.expressionOn; program = std::move(o.program);
    return *this;
  }
  double at(double frame) const;
  // The value the keyframes (or the base) give, before any expression.
  double keyedAt(double frame) const;
  bool hasExpression() const { return program && expressionOn; }
  // Whether the value can differ from one frame to the next.
  bool varies() const { return keys.size() > 1 || hasExpression(); }
  // Sets the expression; empty text removes it. Text that does not compile is
  // kept so it can be fixed, but has no effect, and `error` says why.
  bool setExpression(const QString &text, QString *error = nullptr);
  void set(double frame, double value);
};
enum { AnchorX = 8, AnchorY, AnchorZ, PositionZ, RotationX, RotationY, ScaleZ, CameraZoom,
       ShapeWidth, ShapeHeight, ShapeRoundness, ShapePoints, OuterRadius, InnerRadius,
       PathRotation, ShapeTrimStart, ShapeTrimOffset, ShapeTrimEnd,
       // Shape modifiers: they reshape whatever the shape draws, before Trim Path.
       ShapeRoundCorners, ShapePuckerBloat, ShapeZigZagSize, ShapeZigZagRidges, ShapeTwist,
       ShapeOffsetPath,
       // Linear Wipe and Radial Wipe: they are effects, but their sliders animate like any property.
       WipeLinearCompletion, WipeLinearAngle, WipeLinearFeather,
       WipeRadialCompletion, WipeRadialStart, WipeRadialFeather,
       // 3D Extrude: a property of the layer itself, not an effect.
       ExtrudeDepth, ExtrudeShade, ExtrudeBevel, ExtrudeBevelRound,
       // A particle layer's sliders that can be keyframed. Each particle takes
       // the values in force at the moment it is born and keeps them.
       ParticleRate, ParticleLife, ParticleSpeed, ParticleDirection, ParticleSpread,
       ParticleSize, ParticleGravity, ParticleWind, ParticleTurbulence, ParticleSpin,
       // A video layer's own clock, in seconds of its footage, when Time Remap
       // is on: slowed, sped up, held or run backwards by its keyframes.
       TimeRemap,
       // A light: how strong, its cone, how it fades with distance, and the
       // point in the scene it is aimed at.
       LightIntensity, LightConeAngle, LightConeFeather, LightRadius, LightFalloffDistance,
       LightAimX, LightAimY, LightAimZ,
       // How a 3D layer takes light: the highlight's strength and how tight it is.
       SurfaceShine, SurfaceGloss,
       // A light's shadows: how much of it they hold back, and how soft their edge is.
       LightShadowDarkness, LightShadowSoftness,
       PropertyCount };
inline bool effectProperty(int p) { return p == 6 || p == 7; }
inline bool percentProperty(int p) { return p == 2 || p == 3 || p == 5 || p == ScaleZ; }
struct TextStrokeStyle {
  double width = 4;
  QColor color = Qt::black;
  // A new stroke goes around the letters, not over them. Projects saved
  // before this was the default keep the alignment they were saved with.
  int mode = 1; // 0 inside, 1 outside, 2 center
  // 0 miter: the typeface's own corners stay sharp. 1 round.
  int join = 0;
};
// A layer's fixed properties. Each is numbered with its place however the
// layer is made or copied, which is how an expression finds the layer it is on.
struct LayerProperties : std::array<Property, PropertyCount> {
  LayerProperties() { number(); }
  LayerProperties(const LayerProperties &o) : std::array<Property, PropertyCount>(o) { number(); }
  LayerProperties(LayerProperties &&o) noexcept : std::array<Property, PropertyCount>(std::move(o)) { number(); }
  LayerProperties &operator=(const LayerProperties &) = default;
  LayerProperties &operator=(LayerProperties &&) = default;
private:
  void number() { for (size_t i = 0; i < size(); ++i) (*this)[i].slot = int(i); }
};
struct Layer;
struct AdditionalTransform {
  QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QString name; // e.g. "Transform 2", "Transform 3"
  bool expanded = false;
  bool separatePosition = false;
  bool separateScale = false;
  bool separateRotation = false;
  bool separateAnchor = false;
  bool lockScale = true;

  mutable int propAnchorX = -1;
  mutable int propAnchorY = -1;
  mutable int propAnchorZ = -1;
  mutable int propPosX = -1;
  mutable int propPosY = -1;
  mutable int propPosZ = -1;
  mutable int propScaleX = -1;
  mutable int propScaleY = -1;
  mutable int propScaleZ = -1;
  mutable int propRotation = -1;
  mutable int propRotationX = -1;
  mutable int propRotationY = -1;

  void resolveProps(const Layer &l) const;
};
struct Layer {
  QString id = QUuid::createUuid().toString(QUuid::WithoutBraces), parentId;
  QString groupId; // id of the group layer this belongs to, empty if top-level
  // The Layer Struktur (type "structure") this layer is listed under in the
  // timeline, empty if none. A structure only holds layers together: they
  // stay layers of their own Main, drawn as if it were not there, and they
  // follow it in the list (tidyStructures keeps them so).
  QString structureId;
  bool groupExpanded = true; // for type=="group": whether children are shown
  // A group whose 3D layers are seen through the Main's camera and lit by its
  // lights, as they would be outside the group. Off, the group is a Main of
  // its own: its own camera, if any, and no light from outside.
  bool sharesMainView = false;
  QTransform parentBind;
  QMatrix4x4 parentBind3D;
  bool threeD = false;
  QString name = "Text", type = "text", text = "Wavy-Flo", font = "Arial",
          path, richHtml;
  // Every generated/vector shape is one layer type. The generator lives here
  // instead of masquerading as a separate timeline layer type.
  QString shapeKind = "path"; // path, rect, circle, polygon, star
  QColor color = Qt::white;
  QColor labelColor;
  double fontSize = 80, tracking = 0, leading = 1.2, strokeWidth = 0, blur = 0,
         glow = 0, shadow = 0;
  QColor strokeColor = Qt::black;
  int strokeMode = 1; // 0 inside, 1 outside (the default), 2 center
  int shapeStrokeCap = 0; // 0 round, 1 flat
  // Character strokes are a stack.  Earlier entries are painted first, so a
  // broad outside stroke can sit behind a narrower differently coloured one.
  // The legacy single-stroke fields above remain for shapes and old projects.
  std::vector<TextStrokeStyle> textStrokes;
  // Only the Character strokes are drawn: the letters themselves are left out,
  // as hollow outlines.
  bool textStrokeOnly = false;
  QJsonObject surfaceEffects;
  // A new layer opens collapsed and without motion blur: both are things the
  // user turns on for a layer, not things they have to turn off for every one.
  bool bold = true, expanded = false, separatePosition = false,
       separateScale = false, motionBlur = false, lockScale = true;
  // Text: the family's own face, by its style name ("Light", "Semibold
  // Italic"); empty, Regular or Bold as `bold` says. Over whatever face,
  // faux bold thickens the letters and faux italic slants them -- drawn into
  // the letters here, so every font has both. And the letters' case: 0 as
  // typed, 1 ALL CAPITALS, 2 Small Capitals, 3 all lowercase.
  QString fontStyle;
  bool fauxBold = false, fauxItalic = false;
  int capitals = 0;
  double scaleRatio = 1;
  bool transformExpanded = false, effectsExpanded = false,
       textAnimationExpanded = true, masksExpanded = true;
  // The Text Animations of a stack folded shut in the timeline, by uid.
  QStringList foldedTextAnimations;
  bool separateAnchor = false, separateRotation = false;
  int transformFilter = -1;
  int align = 1;
  double paragraphWidth = 0;
  double start = 0, duration = 180;
  double sourceDurationMs = 0, sourceOffsetMs = 0;
  bool pendingVideoDuration = false;
  // Footage time from the TimeRemap property instead of straight on from the
  // in-point; and whether a time between two pictures mixes them.
  bool timeRemap = false;
  int frameBlend = 0; // 0 off, 1 frame mix
  bool visible = true;
  // Isolate: while any layer of the Main is isolated, only the isolated ones
  // (and what they need -- their mattes, cameras, lights) are drawn.
  bool isolated = false;
  // Locked: it cannot be selected, moved or changed until it is unlocked; it
  // is drawn as ever, and its visibility and solo still switch.
  bool locked = false;
  bool audioEnabled = true;
  // How the layer's finished pixels combine with everything beneath it. Stored
  // by name rather than by position in a list, so a project keeps meaning the
  // same thing if the list of modes ever grows.
  QString blendMode = "normal";
  // Track matte: this layer is shaped by another specific layer.
  // The matte layer is then not drawn on its own -- it exists only to cut this one out.
  QString trackMatteId = "";
  bool trackMatteInverted = false;
  // Whether the name was typed by a person rather than taken from what the
  // layer says. A text layer's name follows its own words until someone gives
  // it one of their own, and from that moment it stays where they put it.
  bool namedByHand = false;
  QImage image;
  // A picture layer whose picture could not be read when the project was
  // opened (its file moved, deleted or unreadable): the project opens all the
  // same, the layer draws nothing and is marked. Not saved; its path is.
  bool pictureMissing = false;
  // A video layer's footage size as shown, when its picture is made smaller
  // for the viewer (footageShrink): the layer keeps the footage's size, and
  // the smaller picture is drawn to fill it. Not saved.
  QSize footageSize;
  QJsonObject characterAnimation;
  QJsonObject appliedEffects;
  QJsonObject hiddenEffects;
  // A transform applied to the layer before its effects run, so a shadow or a
  // glow is computed on the transformed shape rather than transformed along
  // with it afterwards. Deliberately plain numbers and not properties: this
  // describes what the layer is, not what it does over time, so it cannot be
  // keyframed. Animation belongs to the transform after the effects.
  double mainAnchorX = 0, mainAnchorY = 0;
  double mainX = 0, mainY = 0, mainZ = 0;
  double mainScaleX = 1, mainScaleY = 1;
  double mainRotation = 0;
  // Whether the two main scale dimensions keep their ratio when either moves.
  bool mainScaleLocked = true;
  // Pixel effects in the order this layer applies them; empty means the
  // application's own default order.
  QJsonArray effectOrder;
  QJsonArray shapePoints, masks;
  bool shapeClosed = true, shapeFill = true;
  bool repeaterEnabled = false;
  int repeaterCopies = 3;
  double repeaterOffset = 0;
  double repeaterPositionX = 100, repeaterPositionY = 0,
         repeaterPositionZ = 0;
  double repeaterScale = 100, repeaterRotation = 0;
  // Off (default): each copy adds Position and Rotation straight, the way it
  // always has, so existing projects keep the arrangement they were built
  // with. On: copy after copy carries every earlier copy's rotation with it,
  // so a straight offset curls into a logarithmic spiral -- a helix once
  // Position Z joins in -- instead of a row of spinning copies.
  bool repeaterSpiral = false;
  // Camera only, and all plain settings rather than keyframed curves -- set
  // once for the shot, the way picking a lens and stopping down the aperture
  // happens before a take rather than during it, and edited in the Lens tab
  // rather than as Transform rows. Millimetres are stated against a 36mm-wide
  // full-frame sensor, the reference every real focal length is quoted
  // against, so 50mm here is the same angle of view 50mm is on a camera.
  double lensFocalLength = 50;
  // Off keeps every 3D layer sharp, exactly as before this existed. On, a
  // layer softens by the thin-lens circle of confusion its own depth earns
  // against the focus plane: f-stop is a real f-number, and one scene unit is
  // read as one millimetre of real distance, which is what makes stopping
  // down from f/1.4 to f/16 behave the way it does on a lens.
  bool dofEnabled = false;
  double dofFocusDistance = 1000;
  double dofFStop = 2.8;
  // An artistic trim over the physical result, in percent. The optics are
  // right, but a shot sometimes wants more or less of them than right.
  double dofBlurLevel = 100;
  bool contentsExpanded = true, pathExpanded = true;
  // 3D Extrude has a section of its own: a text layer has no "Shape Setup" to
  // keep it under, and on a shape it is not part of the path either.
  bool extrudeExpanded = true;
  // A particle layer's settings that are not keyframed: the emitter's shape
  // and size, what a particle looks like, how it varies, how it fades.
  QJsonObject particles;
  bool particlesExpanded = true;
  // Any other layer can give off particles too -- sparkles over a title, dust
  // off a logo -- drawn over what the layer itself draws.
  bool particlesOn = false;
  // Points followed through the footage (tracker.h). Nothing is drawn from
  // them; they are what Apply Motion and Stabilize are made from.
  QJsonArray tracks;
  // A light (type "light", lights.h): Point, Spot, Sun or Ambient, and how it
  // fades with distance (0 none, 1 smooth, 2 inverse square). Its colour is
  // the layer's colour.
  int lightKind = 1, lightFalloff = 0;
  bool lightExpanded = true;
  // A 3D layer takes the scene's lights unless this is off.
  bool acceptsLights = true;
  // Shadows: a light that throws them, and a 3D layer that stands in the way.
  bool lightShadows = false, castsShadows = false;
  bool surfaceExpanded = true;
  LayerProperties p;
  // Curves for values that are not one of the fixed properties. A plugin
  // declares its own parameters, so they can have no slot in the enum; these
  // are addressed by index from PropertyCount upward instead, which is how the
  // timeline can carry a plugin parameter without knowing what a plugin is.
  // Ids read "<effect name>/<parameter id>".
  std::vector<Property> extraCurves;
  QStringList extraCurveIds;
  std::vector<AdditionalTransform> additionalTransforms;
  AdditionalTransform *findAdditionalTransform(const QString &id);
  const AdditionalTransform *findAdditionalTransform(const QString &id) const;
  bool isAdditionalTransformPercent(int p) const;
  // Addressing that reaches the extra curves too, so anything working from a
  // property index does not have to know which kind it is holding.
  Property &at(int index);
  const Property &at(int index) const;
  Layer();
};
void addAdditionalTransform(Layer &l, const QString &name = QString());
void removeAdditionalTransform(Layer &l, const QString &id);
inline bool isPercentProperty(const Layer &l, int p) {
  if (percentProperty(p)) return true;
  return l.isAdditionalTransformPercent(p);
}
// The property a dimension group is edited through while it is not separated.
inline int transformGroup(int p) {
  if (p == 0 || p == 1 || p == PositionZ) return 0;
  if (p == 2 || p == 3 || p == ScaleZ) return 2;
  if (p == 4 || p == RotationX || p == RotationY) return 4;
  if (p == AnchorX || p == AnchorY || p == AnchorZ) return AnchorX;
  return p;
}
// The properties a dimension group covers, in X, Y, Z order. A 2D layer has no
// Z, and a 2D rotation is a single dimension, so it cannot be separated.
inline std::vector<int> transformDimensions(const Layer &l, int p) {
  switch (transformGroup(p)) {
  case 0: return l.threeD ? std::vector<int>{0, 1, PositionZ} : std::vector<int>{0, 1};
  case 2: return l.threeD ? std::vector<int>{2, 3, ScaleZ} : std::vector<int>{2, 3};
  case 4: return l.threeD ? std::vector<int>{RotationX, RotationY, 4} : std::vector<int>{4};
  case AnchorX:
    return l.threeD ? std::vector<int>{AnchorX, AnchorY, AnchorZ}
                    : std::vector<int>{AnchorX, AnchorY};
  }
  return {p};
}
// Whether that group is edited one dimension at a time. A property that belongs
// to no group is always its own, so it counts as separated.
inline bool separatedDimensions(const Layer &l, int p) {
  switch (transformGroup(p)) {
  case 0: return l.separatePosition;
  case 2: return l.separateScale;
  case 4: return l.separateRotation;
  case AnchorX: return l.separateAnchor;
  }
  return true;
}
// The most layers one composition may carry. A guard against a file that is
// broken or hostile rather than a working limit: a scene of a thousand moving
// shapes draws in about fourteen milliseconds, so this is far above anything
// that still plays. It is the same number wherever layers are added, so a
// project can always be opened again after being built.
inline constexpr int maxLayersPerComposition = 2000;
// A named point in time on the composition: a cue, a lyric, a beat. Markers
// change nothing on screen; they are there to cut, key and snap to.
struct Marker {
  double frame = 0;
  QString label;
  bool beat = false; // placed by beat detection rather than by hand
};
void sortMarkers(std::vector<Marker> &markers);
// Pulse on Beats: keys a layer to jump on markers and settle before the next,
// riding on whatever the property already does. `move` is 0 Scale (pulse),
// 1 Opacity (flash), 2 Position Y (bounce), 3 Rotation (kick, one way then
// the other); `strength` is in that property's units (Scale 0.15 is 15 %
// bigger on the hit, Opacity how much of it drops away between hits, pixels,
// degrees). `settle` is how many frames a hit takes to die away, `every`
// takes one marker in that many, and `handMarkers` counts the markers placed
// by hand as well as the detected beats. Returns how many keys were written.
struct PulseSettings {
  int move = 0;
  double strength = .15;
  int settle = 6;
  int every = 1;
  bool handMarkers = false;
};
int pulseOnMarkers(Layer &, const std::vector<Marker> &, const PulseSettings &);
// Line Up in Time: the layers given start one after another, in the order
// given -- the first at `from`, each next `step` frames later, or, onMarkers,
// each on the next marker from `from` on (and `step` apart once the markers run
// out). Each moves whole: its keys, footage and length go with it. Returns how
// many moved.
struct LineUpSettings {
  double from = 0;
  double step = 5;
  bool onMarkers = false;
};
int lineUpInTime(std::vector<Layer> &layers, const std::vector<int> &order, const std::vector<Marker> &markers,
                 const LineUpSettings &);
// Reverse Keyframes: keys `first` to `last` of a property play backwards in the
// same span -- the last one's value where the first was, and every move in
// between run the other way, its easing turned round with it (Ease In becomes
// Ease Out). False when there is nothing to turn round.
bool reverseKeys(Property &, int first, int last);
struct Scene {
  int width = 1920, height = 1080, fps = 30, frames = 180;
  int processingDepth = 8;
  double frame = 0;
  // Set on the copy a preview renders while it plays, never saved: effects
  // that can trade quality for speed (OpenFX plug-ins' draft render) may. An
  // export, a pre-render or the preview standing still draws the real thing.
  bool previewDraft = false;
  // Set on every frame an export or the Render Queue draws, never saved: the
  // economies of a preview that is playing -- its cap on motion blur samples
  // (setPreviewMotionBlurCap) -- do not reach it, even when the preview plays
  // while it renders.
  bool finalRender = false;
  std::vector<Layer> layers;
  QJsonArray assets;
  // What each effect plugin has been set to, keyed by plugin name and then by
  // the parameter ids that plugin declared. Kept with the project rather than
  // with the application: an effect turned up to eleven is part of the work,
  // and it has to survive being opened on another machine.
  QJsonObject pluginSettings;
  QColor background = QColor("#101014");
  int selected = -1;
  bool motionBlur = true;
  double exposure = 0;
  std::vector<Marker> markers; // in frame order
  // The Render Queue's list (render_queue.h), kept with the project as it was written.
  QJsonArray renderQueue;
  // Glow, blur and shadow worked out in linear light rather than on the
  // encoded values: light that spreads the way light does (precision.cpp).
  bool linearLight = false;
  // The Play Range: the frames playback loops over, first and last included
  // (B and N set them at the playhead). -1 when none is set: the whole Main.
  int playStart = -1, playEnd = -1;
  QJsonObject json() const;
  bool load(const QJsonObject &, QString &error);
};
// The first and last frame playback runs over: the Play Range when one is set
// and makes sense for this Main, otherwise all of it.
inline bool hasPlayRange(const Scene &s) {
  return s.playStart >= 0 && s.playEnd > s.playStart && s.playStart < s.frames;
}
// A Layer Struktur: a fold in the timeline that holds layers (and other
// structures) under it. It draws nothing, has no transform of its own, and
// its time is the span of what it holds. Its eye, solo and lock switch
// everything inside it.
inline bool layerIsStructure(const Layer &l) { return l.type == "structure"; }
// Puts the scene's structures in order: a member whose structure is gone, is
// not a structure, sits in another Main or holds it (a loop) is let go of;
// every structure's members follow it in the list, in their own order; and
// each structure's span is fitted to what it holds. Returns whether anything
// changed; newIndexOf, when given, says where each old place went (filled
// only when the order changed, otherwise left empty).
bool tidyStructures(Scene &s, std::vector<int> *newIndexOf = nullptr);
// The places of everything inside the structure at `index` (structures in
// it too), in list order. Empty for a layer that is not a structure.
std::vector<int> structureContents(const Scene &s, int index);
// How many structures a layer is inside.
int structureDepth(const Scene &s, int index);
inline int playFirst(const Scene &s) { return hasPlayRange(s) ? s.playStart : 0; }
inline int playLast(const Scene &s) {
  return hasPlayRange(s) ? std::min(s.playEnd, std::max(0, s.frames - 1)) : std::max(0, s.frames - 1);
}

double eased(double t, const Key &k);
std::array<QPointF, 4> segmentPoints(const Key &, const Key &);
void setSegment(Key &, const Key &, QPointF, QPointF);
int splitSegment(Property &, int, double);
extern const char *propNames[PropertyCount];
extern const char *propKeys[PropertyCount];

// A text layer's `capitals` (0 as typed, 1 all caps, 2 small caps, 3 all lowercase) as Qt's.
QFont::Capitalization textCapitalization(int capitals);
QTextDocument *textDocument(const Layer &layer);
std::shared_ptr<QTextDocument> cachedTextDocument(const Layer &layer);
void clearTextDocumentCache();
// How far the text has to move down inside its own box. Qt lays a proportional
// line height out by adding every bit of the extra space below the baseline, so
// a line sat high in its box by the whole of the leading; this is the half of
// that extra which belongs above it.
double textLeadingOffset(const Layer &, const QTextDocument &);
void clearTextBackgrounds(QTextDocument *doc);
QRectF layerBounds(const Layer &layer, const Scene &scene);
// Whether a layer carrying only effects, for everything beneath it, is showing
// at this frame. Such a frame is drawn whole and on the CPU.
bool sceneUsesEffectLayers(const Scene &, double frame);
// Whether every Effect Layer acting at this frame has its whole chain in a
// form the card can run (true when there are none).
bool effectLayersRunOnCard(const Scene &, double frame);
// Isolate: true when a layer of the Main is isolated. isolatedScene is then the
// Main with every other layer that draws switched off; cameras, lights and
// sound stay as they are, and a hidden layer still works as a matte.
bool sceneIsolates(const Scene &);
Scene isolatedScene(const Scene &);
// The transform the layer carries before its effects, and the bounds its
// content occupies afterwards -- which is what the effects and the raster see.
QTransform layerMainTransform(const Layer &);
bool layerHasMainTransform(const Layer &);
QRectF layerContentBounds(const Layer &, const Scene &);
// The letters of a text layer, dressed: a background behind each letter, and a
// texture -- another layer's own picture -- inside the letters or inside their
// backgrounds. Kept in the layer's surfaceEffects["Letters"]:
//   background (bool), per (0 a background for each letter, 1 for each word,
//   2 for each line), color, opacity %, padding px, round %, shape (0 fits
//   what it is behind, 1 the same size for every one, 2 a circle),
//   backgroundTexture / letterTexture (a layer id), textureScale %,
//   textureEach (bool: every background its own copy of the texture's
//   picture, the picture's own shape -- a triangle drawn on another layer
//   puts a triangle behind each letter -- rather than one picture across
//   them all, cut to the boxes).
struct LetterPaint {
  QVector<QPainterPath> backgrounds; // one per letter, word or line, in the text document's coordinates
  QVector<int> backgroundCarrier;    // for each background, the letter whose animation carries it
  QBrush backgroundBrush;
  double backgroundOpacity = 1;
  // textureEach: the texture's picture (or two, crossfading between keys,
  // their weights adding to one; a null picture is the box in its colour).
  QVector<std::pair<QImage, double>> backgroundPictures;
  double backgroundPictureScale = 1;
  QBrush letterBrush;                // Qt::NoBrush: each letter keeps its own colour
  // Where background `b` is drawn: its box, or the picture laid into it.
  QRectF backgroundBox(int b) const {
    const QRectF box = backgrounds[b].boundingRect();
    if (backgroundPictures.isEmpty()) return box;
    const double k = backgroundPictureScale;
    return QRectF(box.center().x() - box.width() * k / 2, box.center().y() - box.height() * k / 2, box.width() * k,
                  box.height() * k);
  }
  void drawBackground(QPainter &p, int b) const {
    if (backgroundPictures.isEmpty()) {
      p.fillPath(backgrounds[b], backgroundBrush);
      return;
    }
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double before = p.opacity();
    for (const auto &[picture, weight] : backgroundPictures) {
      p.setOpacity(before * weight);
      if (picture.isNull()) p.fillPath(backgrounds[b], backgroundBrush);
      else p.drawImage(backgroundBox(b), picture);
    }
    p.restore();
  }
};
// The letters' backgrounds of a text whose Character strokes the raster stage
// builds from the letters' distance, kept apart meanwhile -- in the raster with
// the letters, a background was measured as a letter and stroked round -- and
// laid under the letters and their strokes once those are built, in their own
// order. Each is one of `paint`'s backgrounds drawn through `at`, or a blurred
// letter's, finished as a picture laid over `onto` (in the raster's pixels).
struct LetterBackdrop {
  struct Piece {
    std::shared_ptr<const LetterPaint> paint;
    int index = -1;
    QTransform at;
    QImage picture;
    QRectF onto;
    double opacity = 1;
    bool clipped = false;
    QPainterPath clip; // in the raster's pixels
    // An animated letter's: how much the letter carrying it shows, and -- a
    // blurred one -- which of AnimatedLetterStrokes::overlays is that letter
    // (how they fade together: layLetterBackdrop).
    double shown = 1;
    int overlay = -1;
  };
  std::vector<Piece> pieces;
  // Background `index` of `paint` as `p` would draw it now through `at`,
  // inside whatever `p` is clipped to.
  Piece &add(const QPainter &p, std::shared_ptr<const LetterPaint> paint, int index, const QTransform &at,
             double opacity) {
    Piece piece;
    piece.paint = std::move(paint);
    piece.index = index;
    piece.at = at;
    piece.opacity = opacity;
    if (p.hasClipping()) {
      piece.clipped = true;
      piece.clip = p.worldTransform().map(p.clipPath());
    }
    pieces.push_back(std::move(piece));
    return pieces.back();
  }
};
bool layerHasLetterBackground(const Layer &);
bool layerUsesLetterTexture(const Layer &);
// How far the backgrounds reach beyond the letters' own box; `textSize`, the
// text's own size, for backgrounds of words and lines whose picture is laid
// larger than its box.
double letterBackgroundReach(const Layer &, const QSizeF &textSize = {});
// How far a text layer's letter animations can carry a letter past its box.
double textAnimationReach(const Layer &);
// Where its animated letters are at one moment (local frame t), in the layer's
// own coordinates: the box drawn for them. Null when the animation is off.
QRectF animatedTextBounds(const Layer &, double t, int fps);
// While one is alive, animatedTextBounds on this thread -- and on every worker
// it starts -- remembers what it worked out, by layer and time. Only for a
// stretch in which no layer changes -- one frame drawn from one scene -- where
// the same letters were otherwise posed again for every motion-blur sample,
// every band and every pass that asks where they are: 2,500 times a frame for
// a page of animated lyrics drawn on the CPU, most of what it cost. Nested in
// one already alive, it keeps that one.
struct TextBoundsMemoScope {
  TextBoundsMemoScope();
  ~TextBoundsMemoScope();
  bool owned;
};
// The same over the motion-blur shutter about t; null when the letters are still.
QRectF movingLettersBounds(const Layer &, const Scene &, double t);
// A layer's own picture, unmoved and unfaded, as a texture: its content box at
// one layer pixel to a pixel (at most 2048 on its longer side), at this
// composition frame. Kept while the layer does not change.
QImage layerTexturePicture(const Layer &source, const Scene &, double frame);
// What decides that picture, and where the layer is, at this composition
// frame: for a raster made from another layer to know when it is out of date.
QByteArray layerPictureKey(const Layer &, const Scene &, double frame);
// What decides the textures a layer's letters carry at this layer frame.
QByteArray letterTextureKey(const Layer &, const Scene &, double t);
// What the viewer draws around a selected layer, puts its handles on, and
// scales it by. Same size as layerBounds; for text it is centred on the
// letters instead of on the line box, which ascent and descent leave lopsided.
QRectF layerSelectionBounds(const Layer &, const Scene &);
QRectF layerSelectionContentBounds(const Layer &, const Scene &);
// The main transform's rotation and scale, which run ahead of the layer's
// effects. Its position is not part of this: that is added to the movement
// position, so the layer moves and its anchor point moves with it.
QPainterPath vectorPath(const QJsonArray &points, bool closed);
bool parametricShape(const Layer &);
// Per-character animation whose every character has arrived and none has begun
// to leave: the pose is the plain one whatever the frame, so the layer is as
// still as any other text. t is the layer's own time.
bool characterPoseAtRest(const Layer &, double t);
// A layer's own time as Posterize Time steps it: held for a few frames, then
// on, at the effect's Frame Rate. The time as it is when it has none.
double posterizedTime(const Layer &, const Scene &, double t);
QString shapeGeneratorKind(const Layer &);
QPainterPath shapePath(const Layer &, double t);

QTransform layerWorldTransform(const Layer &, const Scene &, double frame);
// Whether the layer gives off particles: a particle layer always, any other
// when they are switched on for it.
inline bool layerEmitsParticles(const Layer &l) { return l.type == "particles" || l.particlesOn; }
// The layer's own drawing, without any particles it gives off.
QRectF layerOwnBounds(const Layer &, const Scene &);
QTransform layerParentTransform(const Layer &, const Scene &, double frame);
bool setLayerParent(Scene &, int child, int parent);
QMatrix4x4 layerWorldMatrix(const Layer &, const Scene &, double frame);
// A layer's own space as the camera sees it (x right, y down, z away from the
// lens), and the camera's zoom: a point p of the layer lands at the frame's
// centre plus zoom * (x, y) / z of the result.
QMatrix4x4 layerCameraMatrix(const Layer &, const Scene &, double frame, double &zoom);
QTransform projectLayer(const Layer &, const Scene &, double frame);
// Where the camera that frames the scene stands, in the scene.
QVector3D cameraEyePosition(const Scene &, double frame);
// What a layer covers in its own plane, as it is drawn at t (masks and
// opacity aside, effects left out), blurred by `softness` units: the shape its
// shadow takes. Alpha8, `resolution` pixels per unit from bounds.topLeft().
// Kept, so a layer that does not change is drawn once. Null if it covers nothing.
QImage layerSilhouette(const Layer &, const Scene &, double t, double softness, QRectF &bounds, double &resolution);
// The cameras and lights at one level of a scene (`level`: the group id they
// sit in, empty for the Main), put into a scene a group there is drawn as --
// with whatever they are linked to, hidden, so they stand where they stand.
// The group's own come first, so a camera of its own still wins.
void addSharedView(Scene &inner, const Scene &outer, const QString &level);
// Where a point in the scene lands in the frame; false when it is behind the camera.
bool projectScenePoint(const Scene &, double frame, const QVector3D &point, QPointF &at);
// 3D Extrude: the depth a layer is given, in pixels (0 = flat), and the side walls it draws.
double layerExtrudeDepth(const Layer &, double t);
// `backCap` adds the back of the solid, at the full depth, over the walls: what
// shows of it from behind, once the face has turned away.
void drawExtrudeWalls(QPainter &, const Layer &, const Scene &, double frame, int repeaterCopy, bool backCap = false);
// Whether an extruded layer's face is turned toward the camera. Turned away,
// it is the far side of the solid: drawn first, under its walls and its back.
bool extrudeFrontFacesCamera(const Layer &, const Scene &, double frame, int repeaterCopy);
// Where those walls can land in the frame: `face` (the layer's box, in its own
// space) grown by the bevel, from the face back to the full depth. Empty with
// no depth; the whole frame when part of it is behind the camera.
QRectF extrudedWallReach(const Layer &, const Scene &, double frame, int repeaterCopy, const QRectF &face);
QPainterPath layerExtrudeOutline(const Layer &, const Scene &, double t);
// The same, in the pieces that move apart: one per letter of a text whose
// letters are animated, each where its letter is and as see-through as it is;
// otherwise the one outline, wholly there.
struct ExtrudePiece { QPainterPath outline; double opacity = 1; };
std::vector<ExtrudePiece> layerExtrudePieces(const Layer &, const Scene &, double t);
// The colour of an extruded layer's sides (and of its back where no picture is
// put on it): the colour at the face's edge -- the outermost stroke that shows
// past the letters or the shape, else the fill -- so the stroke runs on round
// the solid as its sides instead of ending at the face.
QColor layerExtrudeSideColour(const Layer &);
QTransform projectRepeaterCopy(const Layer &, const Scene &, double frame,
                               int copy);
// Where one repeater copy's plane sits in the scene (the layer's own with no repeater).
QMatrix4x4 repeaterCopyWorldMatrix(const Layer &, const Scene &, double frame, int copy);
bool repeaterCopyInFrontOfCamera(const Layer &, const Scene &, double frame,
                                 int copy);
bool layerOrRepeaterInFrontOfCamera(const Layer &, const Scene &, double frame);
// Where one repeater copy lands, "step" copies from the original (fractional
// when Offset is animated smoothly). Position and Rotation always add up the
// same way copy to copy -- rotations around the same axis commute, and so
// does a uniform scale with them -- so only the translation formula changes
// between straight and spiral: shared here so the flat 2D path and the 3D
// camera path can never disagree about where a copy sits.
struct RepeaterCopyTransform { QPointF offset; double offsetZ; double angle; double scale; };
RepeaterCopyTransform repeaterCopyTransform(const Layer &, double step);
// How much extra Gaussian blur a layer's depth earns it under the active
// camera's Depth of Field: 0 with DOF off, no active camera, or a 2D layer
// that ignores the camera in the first place. Added on top of whatever blur
// the layer already carries, in the same source-space radius l.blur uses, so
// it flows through the one blur pass rather than needing a second.
double layerDepthOfFieldBlur(const Layer &, const Scene &, double frame);
double layerCameraDepth(const Layer &, const Scene &, double frame);
// How far a camera sits from the composition's own Z=0 plane -- the plane Zoom
// frames and flat layers live on. What "focus on what I am pointed at"
// resolves to, and the sane starting point for Focus Distance. 0 when this
// camera's own transform cannot be inverted.
double cameraPlaneDistance(const Layer &camera, const Scene &, double frame);
// The angle of view a focal length gives across a given sensor dimension, in
// degrees, the way a lens is specified: 50mm over 36mm of sensor is 39.6°.
inline double lensAngleOfView(double focalLengthMM, double sensorMM) {
  if (focalLengthMM <= 0) return 0;
  return 2. * std::atan(sensorMM / (2. * focalLengthMM)) * 180. / 3.141592653589793;
}
bool layerInFrontOfCamera(const Layer &, const Scene &, double frame);
QPointF layerPositionDelta(const Layer &, const Scene &, double frame, QPointF delta);
QPointF layerAnchorPosition(const Layer &, const Scene &, double frame);
std::vector<int> layerPaintOrder(const Scene &, double frame);

bool vectorEffectStroke(const Layer &);
// A gradient over one solid fill is a brush, not a pass over pixels. Drawn with
// the shape it belongs to it costs nothing beyond the drawing, and it stays
// sharp at any zoom, the way the Character panel's stroke already does.
bool vectorEffectGradient(const Layer &);
bool vectorEffectFill(const Layer &);
// The brush a layer is drawn with, including every colour effect at the head of
// its chain that folds into the drawing.
QBrush layerDrawingBrush(const Layer &, QRectF box);
std::vector<TextStrokeStyle> layerTextStrokes(const Layer &);
// The strokes the raster stage builds round a text's letters: its Character
// strokes, then the Stroke effect where it goes into the letters' own picture.
std::vector<TextStrokeStyle> textStrokeStack(const Layer &);
bool textStackStrokeOnly(const Layer &);
void syncLegacyTextStroke(Layer &);
double textStrokeOutwardReach(const Layer &);
// The Stroke effect expressed as a vector pen, so static and per-character
// animated text stroke from one set of settings.
QPen effectStrokePen(const Layer &);
int effectStrokeMode(const Layer &);
// Whether the Stroke effect is on and set to leave out what it strokes, and
// whether a text layer's letters are left out for its Character strokes.
bool effectStrokeOnly(const Layer &);
bool textStrokeOnly(const Layer &);
void drawAlignedStroke(QPainter &, const QPainterPath &, QBrush fill, QPen outline, int mode);
void drawTextStrokePass(QPainter &, const QPainterPath &, QBrush fill, QPen outline, int mode, bool fillPass);
// Draw one band from the Character stroke stack. Width is the thickness of
// this band; preceding strokes consume the space nearest the glyph.
// Applies a layer's Character strokes to a raster holding only its letters,
// from the distance to them. opacity is applied afterwards, so letters faded
// below half are still recognised as letters by the field.
// letters are the outlines of the letters in the raster's pixels; Miter
// strokes take their corners from them, and are drawn round without them.
// strokeOnly leaves the letters out: only the strokes come back, and the part
// of an outside stroke that would lie under the letters is left out with them.
// letterOpacity and letterScale, one for each of `letters`, are an animated
// text's: each pixel goes with the letter nearest it, its strokes drawn at that
// letter's size and all of it shown at that letter's opacity. `owners`, given,
// says which: for each pixel one more than its letter's index, 0 for none.
// `within`, given, is the only part of the image that can show: elsewhere the
// letters are left as they are, there only as what the strokes reach out from.
void textStrokeStackPass(QImage &, const std::vector<TextStrokeStyle> &, double scale,
                         double opacity, const std::vector<QPainterPath> &letters = {},
                         bool strokeOnly = false, const std::vector<double> &letterOpacity = {},
                         const std::vector<double> &letterScale = {}, std::vector<uint16_t> *owners = nullptr,
                         QRect within = QRect());
// How far a letter's strokes reach, `reach` all round it, as the stroke stack
// draws them: a Miter corner runs out past that to its tip, or to `limit` times
// the reach where it is cut.
QRectF mitredStrokeBox(const QPainterPath &glyph, double reach, double limit = 4);
// What textStrokeStackPass works from, worked out afresh (not kept) for the
// checks: the distance from each pixel of a straight RGBA32F raster to the
// nearest one at least half covered (`inside`) or less than half (not), and
// the label (from `labels`) of the nearest covered labelled one. On the card
// where there is one that can, else the processor -- the same numbers.
std::vector<float> strokeDistanceFieldOf(const QImage &straight, bool inside);
std::vector<int> nearestLetterOf(const QImage &straight, const std::vector<int> &labels);
// An animated text's letters painted for the raster stage, which builds their
// Character strokes itself: each letter's outline in the raster's pixels, how
// much it shows and how big its strokes are, and the blurred letters, finished
// apart, to be laid over the result in the raster's pixels.
struct AnimatedLetterStrokes {
  std::vector<QPainterPath> outlines;
  std::vector<double> opacity, scale;
  // `hints`: those of the painter the letter was once laid with itself (its
  // edges smoothed or not), on top of a smooth enlargement. `clip`, in the
  // raster's pixels when `clipped`: what that painter was clipped to (a hand
  // writing the letters).
  struct Overlay {
    QImage picture;
    QRectF at;
    double opacity = 1;
    QPainter::RenderHints hints = {};
    bool clipped = false;
    QPainterPath clip;
  };
  std::vector<Overlay> overlays;
  // Only one of the strips a raster is painted in collects; the others paint.
  bool collect = true;
  // Where the one that collects keeps the letters' backgrounds, all of them, a
  // blurred letter's blurred with it.
  LetterBackdrop *backdrop = nullptr;
  // What the blurred letters, and their backgrounds, still need done -- each
  // drawn, stroked and blurred on a picture of its own, into its overlay or
  // backdrop piece -- once the strips are painted, all at once.
  std::vector<std::function<void()>> jobs;
  // The raster's pixels that can show; a blurred letter or background landing
  // wholly outside them is left out. Null: all of them. `raster`: all of the
  // raster's pixels.
  QRectF within, raster;
};
// outsideOnly keeps the band to the outside of the path, for strokes drawn
// without the fill that would otherwise cover their inner half.
void drawTextStrokeStackPass(QPainter &, const QPainterPath &,
                             const std::vector<TextStrokeStyle> &, int index, bool outsideOnly = false);
void drawTextStrokeStackInsidePass(QPainter &, const QPainterPath &,
                                   const std::vector<TextStrokeStyle> &, int index);
void drawTextFillAfterStrokes(QPainter &, const QPainterPath &, QBrush fill,
                              const std::vector<TextStrokeStyle> &,
                              QPen characterOutline = {}, int characterMode = 2);

QImage::Format processingFormat(int depth, bool premultiplied = false);
QJsonObject encodeFloatImage(const QImage &);
QImage decodeFloatImage(const QJsonObject &);
QImage precisionEffects(QImage image, const Layer &, double scale);
QImage applySurfaceEffects(QImage image, const Layer &, QRectF contentPixels, double scale);
// One pass each, so a layer can order them however it likes. Surface colour
// work uses straight-alpha float; filters that spread pixels use premultiplied.
// The colour, stylize, noise and transition effects live in .wfx packs
// (effect_packs.h); what stays here is what the drawing itself depends on.
void fillPass(QImage &, const Layer &);
// The time, in seconds and frames into the layer, the effect chain being run is
// for: what moving noise and animated sliders read.
struct EffectTimeScope { double previous, previousFrame; explicit EffectTimeScope(double seconds, double frame = 0); ~EffectTimeScope(); };
double currentEffectSeconds();
double currentEffectFrame();
// Whether the layer has an effect whose picture changes with time (a noise with
// a speed, or a slider with keys).
bool layerHasAnimatedEffect(const Layer &);
void gradientPass(QImage &, const Layer &, QRectF contentPixels);
void strokePass(QImage &, const Layer &, double scale);
// The stroke's distance field, so the card can be handed it as a second
// input instead of measuring it there. Null when the stroke draws nothing.
QImage strokeDistanceField(const QImage &, const Layer &);
// Colour management (precision.cpp). While a scope says so, on this thread
// Glow, Gaussian Blur and Drop Shadow (depth of field too) are worked out in
// linear light; the pictures going in and coming out stay sRGB.
bool effectsInLinearLight();
struct LinearLightScope {
  bool previous;
  explicit LinearLightScope(bool on);
  ~LinearLightScope();
};
// Premultiplied float pixels, sRGB-encoded to linear light and back, in place.
void toLinearLight(QImage &);
void fromLinearLight(QImage &);
// Linear light back to encoded pixels that, laid over what is beneath, fall off
// the way light does: exact over black and for white on white. How a glow
// comes back from linear light; its core, fully solid, is unchanged.
void fromLinearLightAsLight(QImage &);
// A picture as the Main works with it, in sRGB: one that carries another
// colour profile (Display P3 from a phone, Adobe RGB from a camera) is
// converted; one with none, or sRGB's own, is left as it is.
QImage imageInWorkingSpace(QImage);
void shadowPass(QImage &, const Layer &, double scale);
void glowPass(QImage &, const Layer &, double scale);
void blurPass(QImage &, const Layer &, double scale);
// The layer's effects in application order, always complete and never repeated.
// The name a text layer takes from what it says: its first line, tidied, and
// short enough to read in a column. Empty when the layer says nothing, because
// a blank name is worse than a stale one.
QString nameFromText(const QString &);
// Applies that, unless a person has already named this layer. Safe on any
// layer; it does nothing to the ones that are not text.
void followTextName(Layer &);

QStringList layerEffectOrder(const Layer &);

// Properties are addressed by one integer everywhere in the editor. Below
// PropertyCount they are the fixed ones; from there up they are this layer's
// extra curves, in the order it recorded them.
int layerPropertyCount(const Layer &);
Property *layerProperty(Layer &, int index);
const Property *layerProperty(const Layer &, int index);
// The id of an extra curve, or an empty string for a fixed property.
QString layerExtraId(const Layer &, int index);
// Where this id sits, or -1. layerAddExtra creates it, starting flat at
// `initial`, and returns its index either way.
int layerFindExtra(const Layer &, const QString &id);
int layerAddExtra(Layer &, const QString &id, double initial);
void layerRemoveExtra(Layer &, const QString &id);
// `stopBefore`: the chain only as far as the effect of that name, which is
// left out with everything after it.
QImage applyLayerEffects(QImage, const Layer &, QRectF contentPixels, double scale, double time=0,
                         const QString &stopBefore=QString());
struct GpuPass;
// The layer's whole effect chain, described for the card. Empty when the card
// cannot take all of it, and the chain belongs on the CPU.
QVector<GpuPass> layerEffectPasses(const Layer &, QRectF box, double scale, QSize,
                                   bool premultiplied);
// Supplied by the GPU compositor: it rasterizes the layer and runs its effect
// chain on the card, and hands the result back. A null image means the ordinary
// CPU route. It hands the image back rather than compositing it because leaving
// and re-entering the painter for every layer costs more than the effects do --
// and because the painter is where the layer's mask and opacity live.
// What an effect backend hands back. A backend that can put the result into the
// frame itself says so, and the caller then neither caches nor draws it: on a
// large raster, reading those pixels back off the card and letting the painter
// upload them again costs several times what the effects themselves cost.
// `masked` says a clip path is in force, which native drawing cannot see.
// Effects the host supplies rather than the engine: plugins, sitting in a
// layer's own effect stack beside Glow and Blur instead of being applied once
// to the finished frame. Registered globally rather than threaded through every
// render entry point, because plugins belong to the application and not to one
// render, and because the workers need them too.
// What a host effect may know of the render it is in. `otherFrame` gives the
// picture as it reaches this effect at another time (frames into the layer),
// for an effect that reads the frames around the one it draws; it is empty
// where there is no such picture to be had, and gives a null image when that
// time cannot be drawn. `draft`: a preview that is playing (Scene::previewDraft).
// `content`: where the layer's own picture lies in the raster, in pixels; the
// rest is room the effect asked for to draw beyond it (HostLayerEffects::reach).
// `otherLayer` gives another layer's picture, the raster's size, for an
// effect that takes one as an input (a matte, a map): where that layer is in
// the Main, seen from this one -- or, `stretched`, its own picture stretched
// over this layer's. Null when there is no such layer (or it is this one).
struct HostEffectRender {
  std::function<QImage(double t)> otherFrame;
  bool draft = false;
  QRectF content;
  std::function<QImage(const QString &layerId, bool stretched)> otherLayer;
};
struct HostLayerEffects {
  // Whether this layer has any host effect at all. Asked by the gates that
  // decide whether a layer needs a raster stage, so it has to be cheap.
  std::function<bool(const Layer &)> active;
  // Everything about those effects at this time, folded into the raster key so
  // that changing a plugin setting rebuilds the raster rather than showing the
  // one from before. The scene is the one being drawn: a preview that is
  // playing (Scene::previewDraft) may look different and must not be kept as
  // the real thing, and other layers an effect takes as inputs are in it.
  std::function<QByteArray(const Layer &, double t, const Scene &)> identity;
  // One named effect, at its position in the shared layer stack, told what
  // else it may know of this render (HostEffectRender).
  std::function<void(const Layer &, double t, const QString &name, QImage &, double rasterScale,
                     const HostEffectRender &)> apply;
  // How far, in the layer's own units, these effects draw beyond its picture
  // of `content` size: the raster is made that much bigger on every side, as
  // it is for a glow. 0 for none; infinity for everything (held to the size
  // of the composition).
  std::function<double(const Layer &, double t, QSizeF content)> reach;
};
void setHostLayerEffects(const HostLayerEffects *);
// The layer's own picture as a host effect is given it, in the layer's units
// as its raster has them (after its main transform): what the effect takes as
// its frame (HostEffectRender::content).
QRectF hostEffectContentBox(const Layer &, const Scene &);
// A point a host effect works with -- in its frame's units from the picture's
// lower left, y up, as an OpenFX plug-in has its points -- where it is in the
// composition at this composition frame; and back.
QPointF hostEffectPointToComposition(const Layer &, const Scene &, double frame, QPointF point);
QPointF compositionToHostEffectPoint(const Layer &, const Scene &, double frame, QPointF at);
const HostLayerEffects *hostLayerEffects();

struct LayerEffectResult { QImage image; bool composited = false; };
// Supplied by the GPU compositor for an Effect Layer. There the picture beneath
// the layer is on the card, not in an image the painter can hand over, so the
// layer is worked there: `area` is where it acts (device pixels, its box cut to
// its masks), `acting` the pixels it may change, `source` what its effects
// read around them, `scale` the size its effects are measured at, `opacity`
// how much of the result replaces the picture. False means it could not be
// done, and the frame has to be drawn some other way.
using EffectLayerHook = std::function<bool(QPainter &, const Layer &, const Scene &, double t,
                                           const QPainterPath &area, QRect acting, QRect source,
                                           double scale, double opacity)>;
// While one is alive, Effect Layers drawn on this thread that meet a painter
// with no image behind it go to `hook`.
// Draws a picture layer's picture into `box` (layer units) through the
// painter as it stands -- its transform, flat or projected, and its opacity --
// on a painter with a graphics card behind it, from a texture the card keeps.
// False when it could not, and the painter draws it.
using ImageCardHook = std::function<bool(QPainter &, const QRectF &box, const QImage &)>;
// While one is alive, pictures drawn on this thread onto a painter with no
// image behind it go to `hook`.
struct ImageCardScope {
  explicit ImageCardScope(const ImageCardHook *hook);
  ~ImageCardScope();
  const ImageCardHook *previous;
};
// A shape layer's drawing as what it fills, in drawLayer's order: each a path
// in the layer's own units and the brush it is filled with, a stroke also the
// path it is cut to (its own outline for Inside, the room outside it for
// Outside) when `clipped`.
struct ShapeFill {
  QPainterPath path;
  QBrush brush;
  QPainterPath clip;
  bool clipped = false;
  // What a card keeps the two outlines' flattened forms under, worked out the
  // first time it draws them (0: not yet).
  mutable size_t pathKey = 0, clipKey = 0;
};
using ShapeFills = std::vector<ShapeFill>;
// What drawLayer fills for a shape layer at `localFrame`, its own time.
ShapeFills shapeLayerFills(const Layer &, const Scene &, double localFrame);
// Fills a shape layer through the painter as it stands, on a painter with a
// graphics card behind it, every pixel covered by the exact area of it inside
// the outline as the processor's rasterizer covers it. `fills` null: the ones
// the card made ready with the layer's recorded picture. False when it could
// not, and the painter draws them.
using ShapeCardHook = std::function<bool(QPainter &, const Layer &, const ShapeFills *fills)>;
// While one is alive, shape layers drawn on this thread onto a painter with no
// image behind it go to `hook`.
struct ShapeCardScope {
  explicit ShapeCardScope(const ShapeCardHook *hook);
  ~ShapeCardScope();
  const ShapeCardHook *previous;
};
// A 3D layer's face lit on a card (lights.h). `paintFace` draws the face with
// a painter whose device is the size of the one `p` paints on, the face's
// `dirty` rectangle of it at its top left; the hook has it painted on the card,
// lights every pixel at the point of the layer's plane it shows and lays the
// result into the frame where `dirty` is. False when it could not: the CPU
// paints and lights the face, as it always has.
struct SceneLighting;
using LitFaceHook = std::function<bool(QPainter &p, QRect dirty, const std::function<void(QPainter &)> &paintFace,
                                       const QTransform &localToDevice, const QMatrix4x4 &world,
                                       const SceneLighting &lighting, float shine, float gloss, int self)>;
// While one is alive, lit faces drawn on this thread onto a painter with no
// image behind it go to `hook`.
struct LitFaceScope {
  explicit LitFaceScope(const LitFaceHook *hook);
  ~LitFaceScope();
  const LitFaceHook *previous;
};
// Whether everything the painter is cut to is one rectangle on its device,
// and which: what a card drawing past the painter can cut to as well.
bool painterClipIsDeviceRect(const QPainter &, QRect *rect = nullptr);
// The same, answered without asking the painter while a pass draws a layer
// with it that nothing has cut since the pass began: the pass asked once.
bool passClipIsDeviceRect(const QPainter &, QRect *rect = nullptr);
struct EffectLayerHookScope {
  explicit EffectLayerHookScope(const EffectLayerHook *hook);
  ~EffectLayerHookScope();
  const EffectLayerHook *previous;
};
using LayerEffectHook =
    std::function<LayerEffectResult(const Layer &, double t, QRectF logical, double rasterScale,
                                    QRectF box, const QImage &source, bool masked)>;
// A layer with a blend mode, on a card. `paintAlone` draws the layer by
// itself, where it lies on the device `p` paints on, with a painter onto
// another device of that size, its own effects going to the hook handed to it
// (null: the CPU's way); the hook has it flattened on the card and blended
// there with what lies beneath, within `dirty`. False when it could not: the
// layer is flattened on the CPU, as it always was.
using BlendCardHook = std::function<bool(QPainter &p, QRect dirty, QPainter::CompositionMode mode,
                                         const std::function<void(QPainter &, const LayerEffectHook *)> &paintAlone)>;
// A layer shaped by a track matte, on a card: both flattened there the way
// BlendCardHook flattens one (`paintOwner`, `paintMatte`), the owner kept where
// the matte covers it (or does not, inverted) and laid in with its own blend
// mode. False when it could not: the CPU does it, as it always did.
using MatteCardHook = std::function<bool(QPainter &p, QRect dirty, QPainter::CompositionMode mode, bool inverted,
                                         const std::function<void(QPainter &, const LayerEffectHook *)> &paintOwner,
                                         const std::function<void(QPainter &, const LayerEffectHook *)> &paintMatte)>;
struct MatteCardScope {
  explicit MatteCardScope(const MatteCardHook *hook);
  ~MatteCardScope();
  const MatteCardHook *previous;
};
// While one is alive, blended layers drawn on this thread onto a painter with
// no image behind it go to `hook`.
struct BlendCardScope {
  explicit BlendCardScope(const BlendCardHook *hook);
  ~BlendCardScope();
  const BlendCardHook *previous;
};
// A layer blurring itself by its own motion, on a card: `paintSample` draws it
// at its k-th sample across the shutter (k = 0 .. samples - 1), by itself, the
// way BlendCardHook's paintAlone does; the hook averages the samples laid over
// what lies beneath with `mode`, within `dirty`. False when it could not: the
// CPU does it, as it always did.
using MotionBlurCardHook = std::function<bool(QPainter &p, QRect dirty, QPainter::CompositionMode mode, int samples,
                                              const std::function<void(QPainter &, int, const LayerEffectHook *)> &paintSample)>;
struct MotionBlurCardScope {
  explicit MotionBlurCardScope(const MotionBlurCardHook *hook);
  ~MotionBlurCardScope();
  const MotionBlurCardHook *previous;
};
// Whether a frame's moving layers can each blur themselves -- every layer that
// moves drawn several times across the shutter, the rest once -- rather than
// the whole frame being drawn again for every sample.
bool motionBlurPerLayer(const Scene &, double frame);
// How many of the frame's layers move enough to be sampled, and how many of
// those are text whose letters are what moves (each sample a new picture).
struct MovingLayers { int moving = 0, letters = 0, drawn = 0; };
MovingLayers movingLayers(const Scene &, double frame, QSize outputSize);
// While one is alive with `on`, a frame painted on this thread is drawn that way.
struct LayerMotionBlurScope {
  explicit LayerMotionBlurScope(bool on);
  ~LayerMotionBlurScope();
  bool previous;
};
// The pictures a frame is about to ask for, built first on every core: each
// moving text's picture at every time its blur samples, then the rest at once.
// `effects` is what they will be drawn with (null: the CPU's own way); a card
// hands one that keeps only the sources, as it works the effects itself.
void warmFrameLayers(const Scene &, double frame, QSize outputSize, QRect region, bool perLayerBlur,
                     const LayerEffectHook *effects = nullptr);
struct RenderWorkCounters { quint64 sourceNs=0, effectNs=0, sourceHits=0, effectHits=0; };
RenderWorkCounters renderWorkCounters();
QString renderWorkReport(const RenderWorkCounters &before);

QPicture prepareLayerDrawing(const Layer &, const Scene &, double localFrame);
// Everything that decides what an effected raster contains, so an unchanged
// layer is neither rasterized nor effected a second time.
QByteArray effectedRasterKey(const Layer &, const Scene &, double t, QRectF logical,
                             double rasterScale, QRectF box, const QByteArray &host = {});
// Rasters actually built. A frame that reuses them does not advance it, which
// is how the three stages are held apart: moving a layer must not rebuild it.
quint64 effectedRasterBuilds();
// Layer sources rasterized on the calling thread so far, cache hits not
// counted: how a pass finds out the frame it is drawing is cold.
quint64 layerSourcesBuiltHere();
// Empties the raster caches: for tests that must start cold.
void clearRenderCaches();
// Frames may be planned once and drawn in bands from the plan; the self-test
// turns this off to draw the same frame both ways and compare.
void setCompositionPlanning(bool on);
bool compositionPlanning();
QImage cachedEffectedRaster(const QByteArray &key);
// Starts a frame's claim on the raster cache. Called by renderCPU; anything
// else driving paintComposition directly should call it once per frame too.
void beginRasterFrame();
// Marks the calling thread as one of the workers a cold frame uses to build every
// layer ahead of the pass that draws them: it never waits for another thread's
// raster, which is being built by the thread waiting for it.
void setPrewarmWorker(bool on);
// The effected-raster budget, in kibibytes. Sized from the machine at startup;
// settable so a test can prove what happens when a scene does not fit in it.
void setRasterCacheBudget(int kibibytes);
int rasterCacheBudget();
void storeEffectedRaster(const QByteArray &key, const QImage &);
// `isolate`, when set, paints that one layer and nothing else, into a painter
// set up exactly as the whole composition's would be. It is how a blended layer
// is flattened on its own before being combined with what lies beneath it.
void paintComposition(QPainter &, const Scene &, double frame, double center, QSize outputSize,
                      const QHash<QString, QPicture> *prepared = nullptr,
                      const LayerEffectHook *effects = nullptr,
                      const QString &isolate = {});
// The blend modes a layer can use, as {stored name, label shown}, in menu
// order. Each maps onto a QPainter composition mode, so all of them are drawn
// by the same raster engine that draws everything else.
const QVector<QPair<QString, QString>> &layerBlendModes();
QPainter::CompositionMode layerCompositionMode(const Layer &);
// Whether anything contributing to this frame is combined other than normally.
bool sceneUsesBlendModes(const Scene &, double frame);
// The index of the layer that mattes layer `owner`, or -1 when it has no matte
// or nothing drawable sits directly above it. The matte is the row above in the
// timeline, which is the previous drawable entry in the list.
int layerMatteSource(const Scene &, int owner);
// Whether any layer contributing to this frame is shaped by a track matte.
bool sceneUsesTrackMattes(const Scene &, double frame);
bool compositionHasMotion(const Scene &, double frame);
int motionBlurSampleCount(const Scene &, double frame, QSize outputSize);
bool sameCompositionPixels(const Scene &, double from, double to);
QImage renderHaloOnly(QImage source, double radius, QColor color);
QImage fullResolutionBlur(QImage source, double radius);
// fullResolutionBlur where only `part` of the picture matters: the rows of the
// part, every column, its pixels blurred from its own alone and the columns
// either side transparent. A pixel as far inside the part as the blur reaches
// (three box passes of ceil(radius/2)) comes out as the whole picture's blur
// has it -- the edges of the picture count as inside.
QImage fullResolutionBlurPart(const QImage &source, double radius, QRect part);
// How far a blur of this radius over an image of this size can be reduced
// before the interpolation back would show. One rule for the CPU chain, the
// GPU chain and the halo path, so all three resolve a blur the same way.
int blurReduction(double radius, QSize);
// region is in output pixels and empty means the whole frame. The viewer
// passes the part it can actually show, which is what keeps a zoomed-in
// preview costing a viewport rather than a whole magnified composition.
// Whether this frame may be cut into horizontal bands across cores, or whether
// a band would repeat work rather than divide it. Exposed so the decision can
// be asked directly instead of inferred from a stopwatch.
bool compositionBands(const Scene &, QSize outputSize, double frame);
QImage renderCPU(const Scene &, double frame, bool blur = true, QSize outputSize = {}, QRect region = {});
// A layer being dragged in the viewer: from one step of the drag to the next
// nothing beneath it changes. The picture of everything beneath it is drawn
// once and kept here; each step paints the layer and everything above it onto
// a copy in one ordinary pass -- the pixels of the whole frame drawn again.
struct DragPlate {
  QMutex lock;          // two preview frames may be drawn at once
  quint64 key = 0;      // what the kept picture was drawn for
  bool holds = false;   // whether this drag can be drawn so at all
  QImage below;         // premultiplied, in the processing format
  // Read without the lock, by whoever chooses between this and the card: the
  // key the picture is ready for, whether it held, and what the last step
  // over it cost (the picture beneath not counted).
  std::atomic<quint64> readyKey{0};
  std::atomic<bool> readyHolds{false};
  std::atomic<double> stepMs{0};
  std::atomic<bool> building{false}; // being made off to the side while the card draws
  // What a whole step of this gesture took on each, as the preview drew them:
  // on the card (only steps drawn while no plate was being made beside it) and
  // over the plate. Zero until measured.
  std::atomic<double> cardMs{0}, plateMs{0};
  std::atomic<int> cardSteps{0}; // how many steps cardMs has seen
  // The frame with the layer left out (the plate with everything above it),
  // kept too: a step then draws only where the layer is, and the rest is this.
  // Under `lock`, with the key it was drawn for, and the part the last step drew.
  QImage without;
  quint64 withoutKey = 0;
  QRect lastDrawn;
  static void note(std::atomic<double> &slot, double ms) {
    const double was = slot;
    slot = was > 0 ? was * .5 + ms * .5 : ms;
  }
};
// The frame as renderCPU(s, frame, blur, outputSize, region) draws it, made over
// `plate`, with the layer `layerId` moving. `key` must change whenever anything
// but that layer does (the editor's revision); the frame, size and part are
// added to it here. Null when this frame cannot be drawn so -- draw it whole:
// the layer is in 3D or a group, something beneath it reads it (as a matte, a
// parent, a texture, in an expression), or the frame averages whole motion-blur
// samples.
QImage renderOverPlate(const Scene &, double frame, bool blur, QSize outputSize, QRect region, const QString &layerId,
                       quint64 key, DragPlate &plate);
// The same with several layers moving together: the plate is what lies
// beneath the lowest of them, and none of them may be read by it.
QImage renderOverPlate(const Scene &, double frame, bool blur, QSize outputSize, QRect region, const QStringList &layerIds,
                       quint64 key, DragPlate &plate);
// The key a plate made by renderOverPlate with these arguments is kept under
// (DragPlate::readyKey): whether a plate is ready for a frame, asked without
// drawing anything.
quint64 renderOverPlateKey(const Scene &, double frame, bool blur, QSize outputSize, QRect region, const QString &layerId,
                           quint64 key);
quint64 renderOverPlateKey(const Scene &, double frame, bool blur, QSize outputSize, QRect region, const QStringList &layerIds,
                           quint64 key);
void gradeCPU(QImage &image, float exposure);
void setPreviewMotionBlurCap(int maxSamples);
int previewMotionBlurCap();
// The cap as it applies to a render of `s`: none for a final render.
int previewMotionBlurCap(const Scene &s);

// Distance fields actually recomputed so far; a frame that reuses cached
// fields does not advance it. Reported by the viewer's backend panel.
quint64 strokeFieldComputations();
// Blurred letters' pictures blurred so far (drawAnimatedCharacters): a raster
// painted in strips blurs each once, not once a strip.
std::atomic<quint64> &blurredLetterPictures();
// A QPainterPath makes what it is drawn from (its vector form, its bounds) the
// first time it is drawn, in the data all its copies share, with no lock: a
// path filled or clipped to on two threads at once had both make it, and one
// freed what the other was still drawing -- the heap went with it. Made here,
// on the thread that owns the path, every thread after only reads it.
void readyPathForThreads(const QPainterPath &path);
int checkSurfaceEffects(QStringList &failures);
int checkPrecision(QStringList &failures);
void setFastMaskScrubbing(bool active);
bool fastMaskScrubbing();
