#pragma once
// 3D lights. A layer of type "light" lights every 3D layer that takes light;
// with no light in the scene a 3D layer is drawn exactly as it always was.
//
// A layer is lit after its own effects and masks, as the flat picture it has
// become, in the place the camera sees it: every pixel is taken back to the
// point on the layer's plane it shows, and that point is lit. A plane has one
// facing, so only the distance to a lamp, its cone and the highlight change
// across it. Extruded sides are lit per face of the wall.
#include "engine.h"
#include <QImage>
#include <QMatrix4x4>
#include <QStringList>
#include <QVector3D>
#include <vector>

enum LightKind { LightPoint = 0, LightSpot = 1, LightSun = 2, LightAmbient = 3 };
// In the order of LightKind; and of the falloff choices, 0 none.
QStringList lightKindNames();
QStringList lightFalloffNames();

// A 3D layer that stands between a light and what it lights: its plane, and
// what of that plane it covers.
struct ShadowCaster {
  int layer = -1;            // its place in the scene: a layer never shadows itself
  QVector3D point, normal;   // its plane, in the scene
  QMatrix4x4 toLocal;        // the scene into the layer's own coordinates
  QImage map;                // Alpha8 coverage (layerSilhouette)
  QRectF bounds;
  float resolution = 1, opacity = 1;
};
struct SceneLight {
  int kind = LightPoint;
  QVector3D position;  // world
  QVector3D direction; // world, the way it shines (Spot, Sun)
  QVector3D colour;    // the colour times the intensity: 1 is full
  // Spot: cosines of half the cone, and of half the part of it at full strength.
  float coneOuter = -1, coneInner = -1;
  int falloff = 0;              // 0 none, 1 smooth, 2 inverse square
  float radius = 0, reach = 0;  // where the falloff starts, and over how far it fades (smooth)
  // What stands in its way, and how much of its light a solid one holds back.
  std::vector<ShadowCaster> casters;
  float shadowDarkness = 1;
};
struct SceneLighting {
  std::vector<SceneLight> lights;
  QVector3D eye; // where the camera is
  bool any() const { return !lights.empty(); }
};

// Whether any light is switched on and showing at this frame.
bool sceneHasLights(const Scene &, double frame);
SceneLighting sceneLighting(const Scene &, double frame);
// A 3D layer that draws something and takes light, in a scene that has one.
bool layerCanTakeLight(const Layer &);
bool layerTakesLight(const Layer &, const Scene &, double frame);

// The light on one point of a surface facing `normal` (either way: the side the
// camera sees is the one lit): what its colour is multiplied by, and the
// highlight added on top. `shine` 0..1, `gloss` 0..1.
struct LightAmount { QVector3D multiply, add; };
LightAmount lightAt(const SceneLighting &, const QVector3D &point, const QVector3D &normal,
                    float shine, float gloss, int self = -1);
// How much of a light reaches a point past the layers in its way: 1 in the
// clear, down to 1 - darkness behind a solid one. `self` is the lit layer.
float lightGetsThrough(const SceneLight &, const QVector3D &point, int self);

// Lights a layer already drawn, premultiplied, into `image`, whose top left
// pixel is `origin` in device pixels. `localToDevice` takes the layer's own
// plane to those device pixels, `world` the same plane into the scene.
// `deviceToFrame` is where the device's own top left lies in the frame -- a
// band's top row, when the frame is drawn in bands -- so the light is worked
// out on the same points of the frame whichever band a pixel is in.
void lightLayerImage(QImage &image, QPoint origin, const QTransform &localToDevice,
                     const QMatrix4x4 &world, const SceneLighting &, float shine, float gloss, int self = -1,
                     QPoint deviceToFrame = QPoint());

int checkLights(QStringList &report); // --lights-self-test
