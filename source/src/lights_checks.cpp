#include "lights.h"
#include "gpu_renderer.h"
#include "render_queue.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

// --lights-self-test
int checkLights(QStringList &report) {
  constexpr double pi = 3.14159265358979323846;
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  Scene scene;
  scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 90;
  scene.background = Qt::black;
  scene.motionBlur = false;
  auto plane = [&](QColor colour = Qt::white) {
    Layer l;
    l.type = "shape"; l.shapeKind = "rect"; l.name = "Plane"; l.duration = 90;
    l.threeD = true; l.color = colour;
    l.p[0].base = 320; l.p[1].base = 180;
    l.p[ShapeWidth].base = 400; l.p[ShapeHeight].base = 240;
    l.p[SurfaceShine].base = 0;
    return l;
  };
  auto light = [&](int kind) {
    Layer l;
    l.type = "light"; l.name = "Light"; l.threeD = true; l.duration = 90;
    l.lightKind = kind; l.color = Qt::white;
    l.p[0].base = 320; l.p[1].base = 180; l.p[PositionZ].base = -400;
    l.p[LightAimX].base = 320; l.p[LightAimY].base = 180; l.p[LightAimZ].base = 0;
    return l;
  };
  auto render = [&](const Scene &s, double frame = 10, bool blur = false) {
    return renderCPU(s, frame, blur).convertToFormat(QImage::Format_ARGB32);
  };
  auto with = [&](std::vector<Layer> layers) {
    Scene s = scene;
    s.layers = std::move(layers);
    return s;
  };
  auto grey = [](const QImage &image, int x, int y) { return qGreen(image.pixel(x, y)); };
  auto worst = [](const QImage &a, const QImage &b) {
    if (a.size() != b.size()) return 999;
    int most = 0;
    for (int y = 0; y < a.height(); ++y)
      for (int x = 0; x < a.width(); ++x) {
        const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
        most = std::max({most, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                         std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
      }
    return most;
  };

  const QImage unlit = render(with({plane()}));
  {
    Layer hidden = light(LightSpot);
    hidden.visible = false;
    check(worst(render(with({hidden, plane()})), unlit) == 0, "a light switched off changes nothing");
    Layer refusing = plane();
    refusing.acceptsLights = false;
    check(worst(render(with({light(LightSpot), refusing})), unlit) == 0, "a layer that does not take light is drawn as it was");
    Layer flat = plane();
    flat.threeD = false;
    check(worst(render(with({light(LightSpot), flat})), render(with({flat}))) == 0, "a 2D layer is never lit");
  }
  {
    Layer ambient = light(LightAmbient);
    check(worst(render(with({ambient, plane()})), unlit) <= 1, "full white ambient light is the layer as it is");
    ambient.p[LightIntensity].base = 50;
    const QImage half = render(with({ambient, plane()}));
    check(std::abs(grey(half, 320, 180) - 128) <= 1 && qAlpha(half.pixel(320, 180)) == 255,
          QString("half an ambient light, half as bright; as solid (%1)").arg(grey(half, 320, 180)));
  }
  {
    // A sun from straight behind the camera: all of it falls on the plane.
    const QImage straight = render(with({light(LightSun), plane()}));
    check(worst(straight, unlit) <= 1, "a sun shining straight at a plane leaves it at its own colour");
    Layer slanted = light(LightSun);
    slanted.p[LightAimX].base = 320 + 400 * std::sin(60 * pi / 180);
    slanted.p[LightAimZ].base = -400 + 400 * std::cos(60 * pi / 180);
    const QImage sixty = render(with({slanted, plane()}));
    check(std::abs(grey(sixty, 320, 180) - 128) <= 1 && std::abs(grey(sixty, 200, 120) - 128) <= 1,
          QString("a sun sixty degrees off lights the plane by half, all over (%1, %2)")
              .arg(grey(sixty, 320, 180)).arg(grey(sixty, 200, 120)));
    Layer red = light(LightSun);
    red.color = QColor(255, 0, 0);
    const QRgb tinted = render(with({red, plane()})).pixel(320, 180);
    check(qRed(tinted) >= 254 && qGreen(tinted) <= 1 && qBlue(tinted) <= 1, "a red light makes a white layer red");
  }
  {
    // A thirty degree spot from 400 in front: a pool 107 wide around the middle.
    Layer spot = light(LightSpot);
    spot.p[LightConeAngle].base = 30; spot.p[LightConeFeather].base = 0;
    const QImage pool = render(with({spot, plane()}));
    const int inside = grey(pool, 320 + 90, 180), outside = grey(pool, 320 + 125, 180);
    check(grey(pool, 320, 180) >= 254 && inside >= 240 && outside <= 1 && qAlpha(pool.pixel(320 + 150, 180)) == 255,
          QString("a spot lights its cone and leaves the rest dark, not transparent (%1 inside, %2 outside)").arg(inside).arg(outside));
    spot.p[LightConeFeather].base = 100;
    const QImage soft = render(with({spot, plane()}));
    check(grey(soft, 320, 180) >= 250 && grey(soft, 320 + 60, 180) > 30 && grey(soft, 320 + 60, 180) < 220,
          QString("a feathered cone fades from its middle out (%1)").arg(grey(soft, 320 + 60, 180)));
    spot.p[LightConeFeather].base = 0;
    spot.p[LightConeAngle].keys.clear();
    spot.p[LightConeAngle].set(0, 10);
    spot.p[LightConeAngle].set(60, 60);
    const QImage narrow = render(with({spot, plane()}), 0), wide = render(with({spot, plane()}), 60);
    check(grey(narrow, 320 + 90, 180) <= 1 && grey(wide, 320 + 90, 180) >= 240, "a keyed cone widens over time");
  }
  {
    // A point 200 in front, fading with the square of the distance past 200.
    Layer bulb = light(LightPoint);
    bulb.p[PositionZ].base = -200;
    bulb.lightFalloff = 2; bulb.p[LightRadius].base = 200;
    const QImage bulbLit = render(with({bulb, plane()}));
    const double distance = std::hypot(180., 200.);
    const double expected = 255. * (200. / distance) * (200. / distance) * (200. / distance);
    check(grey(bulbLit, 320, 180) >= 254 && std::abs(grey(bulbLit, 320 + 180, 180) - expected) <= 3,
          QString("a point light falls off with distance and angle (%1, expected %2)")
              .arg(grey(bulbLit, 320 + 180, 180)).arg(expected, 0, 'f', 1));
    bulb.lightFalloff = 1; bulb.p[LightRadius].base = 100; bulb.p[LightFalloffDistance].base = 80;
    bulb.p[PositionZ].base = -80;
    const QImage smooth = render(with({bulb, plane()}));
    check(grey(smooth, 320, 180) >= 254 && grey(smooth, 320 + 190, 180) <= 1,
          "a smooth falloff is whole inside its radius and gone past its distance");
  }
  {
    // Shine: a highlight where the light would glance off towards the camera.
    Layer sun = light(LightSun);
    Layer glossy = plane(QColor(128, 128, 128));
    glossy.p[SurfaceShine].base = 50; glossy.p[SurfaceGloss].base = 60;
    const QImage shiny = render(with({sun, glossy}));
    glossy.p[SurfaceShine].base = 0;
    const QImage matte = render(with({sun, glossy}));
    check(grey(shiny, 320, 180) >= 250 && grey(shiny, 320 + 190, 180) < grey(shiny, 320, 180) - 40 &&
              std::abs(grey(matte, 320, 180) - grey(matte, 320 + 190, 180)) <= 1,
          QString("Shine puts a highlight where the light glances to the camera (%1 middle, %2 edge)")
              .arg(grey(shiny, 320, 180)).arg(grey(shiny, 320 + 190, 180)));
  }
  {
    // The per-pixel light worked out every fourth pixel is within a hair of
    // working it out for every pixel.
    Layer turned = plane();
    turned.p[RotationY].base = 35; turned.p[RotationX].base = -20;
    Scene s = with({light(LightSpot), turned});
    Layer &spot = s.layers[0];
    spot.p[LightConeAngle].base = 50; spot.p[LightConeFeather].base = 30;
    Layer bulb = light(LightPoint);
    bulb.p[0].base = 150; bulb.p[PositionZ].base = -150; bulb.lightFalloff = 2; bulb.p[LightRadius].base = 150;
    bulb.color = QColor(80, 160, 255);
    s.layers.insert(s.layers.begin(), bulb);
    const SceneLighting lighting = sceneLighting(s, 10);
    const QTransform toDevice = projectLayer(s.layers[2], s, 10);
    const QMatrix4x4 world = layerWorldMatrix(s.layers[2], s, 10);
    QImage image(320, 200, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x)
        image.setPixelColor(x, y, QColor((x * 7) % 256, (y * 5) % 256, 200, 255));
    const QPoint origin(160, 80);
    QImage lit = image;
    lightLayerImage(lit, origin, toDevice, world, lighting, .6f, .7f);
    const QTransform toLocal = toDevice.inverted();
    const QVector3D corner = world.column(3).toVector3D(), across = world.column(0).toVector3D(), down = world.column(1).toVector3D();
    const QMatrix3x3 normals = world.normalMatrix();
    const QVector3D normal = QVector3D(normals(0, 2), normals(1, 2), normals(2, 2)).normalized();
    int most = 0;
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x) {
        const QPointF local = toLocal.map(QPointF(origin.x() + x + .5, origin.y() + y + .5));
        const LightAmount amount = lightAt(lighting, corner + across * float(local.x()) + down * float(local.y()), normal, .6f, .7f);
        const QColor source = image.pixelColor(x, y), made = lit.pixelColor(x, y);
        auto expect = [](double c, float m, float a) { return int(std::clamp(c * m + a, 0., 1.) * 255 + .5); };
        most = std::max({most, std::abs(made.red() - expect(source.redF(), amount.multiply.x(), amount.add.x())),
                         std::abs(made.green() - expect(source.greenF(), amount.multiply.y(), amount.add.y())),
                         std::abs(made.blue() - expect(source.blueF(), amount.multiply.z(), amount.add.z()))});
      }
    check(most <= 2, QString("lit every fourth pixel and blended, within %1/255 of every pixel").arg(most));
  }
  {
    // A turned, extruded plane: the wall it shows is lit by the scene's sun,
    // brighter with the sun on its side than on the other.
    auto wallBrightness = [&](double turn, double sunSide) {
      Layer block = plane(QColor(200, 200, 200));
      block.p[RotationY].base = turn;
      block.p[ExtrudeDepth].base = 120;
      Layer sun = light(LightSun);
      sun.p[0].base = 320 + 400 * sunSide; sun.p[PositionZ].base = 0; // shining sideways
      QImage walls(640, 360, QImage::Format_ARGB32_Premultiplied);
      walls.fill(Qt::transparent);
      {
        QPainter painter(&walls);
        drawExtrudeWalls(painter, block, with({sun, block}), 10, 0);
      }
      qint64 sum = 0;
      for (int y = 0; y < walls.height(); ++y)
        for (int x = 0; x < walls.width(); ++x) sum += qGreen(walls.pixel(x, y));
      return sum;
    };
    const qint64 a = wallBrightness(40, 1), b = wallBrightness(40, -1), c = wallBrightness(-40, 1), d = wallBrightness(-40, -1);
    check((a > b * 2 + 1000 && d > c * 2 + 1000) || (b > a * 2 + 1000 && c > d * 2 + 1000),
          QString("an extruded wall is lit by the sun on its side; turned the other way, by the other (%1/%2, %3/%4)")
              .arg(a).arg(b).arg(c).arg(d));
  }
  {
    // Repeater copies are lit where each of them is.
    Layer row = plane();
    row.p[0].base = 170; row.p[ShapeWidth].base = 100; row.p[ShapeHeight].base = 100;
    row.repeaterEnabled = true; row.repeaterCopies = 3; row.repeaterPositionX = 150;
    Layer bulb = light(LightPoint);
    bulb.p[0].base = 170; bulb.p[PositionZ].base = -100; bulb.lightFalloff = 2; bulb.p[LightRadius].base = 100;
    const QImage copies = render(with({bulb, row}));
    check(grey(copies, 170, 180) >= 250 && grey(copies, 320, 180) < 120 && grey(copies, 470, 180) < grey(copies, 320, 180) &&
              grey(copies, 470, 180) > 0,
          QString("each repeater copy is lit where it stands (%1, %2, %3)")
              .arg(grey(copies, 170, 180)).arg(grey(copies, 320, 180)).arg(grey(copies, 470, 180)));
  }
  {
    // What else a layer can be doing: blending, blurring with motion, deep colour, bands.
    Layer ambient = light(LightAmbient);
    ambient.p[LightIntensity].base = 50;
    Layer added = plane();
    added.blendMode = "add";
    const QImage blended = render(with({ambient, added}));
    check(std::abs(grey(blended, 320, 180) - 128) <= 1, QString("a blended layer is lit before it blends (%1)").arg(grey(blended, 320, 180)));
    Scene moving = with({ambient, plane()});
    moving.motionBlur = true;
    moving.layers[1].motionBlur = true;
    moving.layers[1].p[0].set(0, 300);
    moving.layers[1].p[0].set(20, 340);
    const QImage blurred = render(moving, 10, true);
    check(std::abs(grey(blurred, 320, 180) - 128) <= 2, QString("a layer blurred with motion is lit in every sample (%1)").arg(grey(blurred, 320, 180)));
    Scene deep = with({ambient, plane()});
    deep.processingDepth = 16;
    const QImage sixteen = render(deep);
    deep.processingDepth = 32;
    const QImage thirtyTwo = render(deep);
    check(std::abs(grey(sixteen, 320, 180) - 128) <= 1 && std::abs(grey(thirtyTwo, 320, 180) - 128) <= 1,
          QString("lit the same at 16 and 32 bits (%1, %2)").arg(grey(sixteen, 320, 180)).arg(grey(thirtyTwo, 320, 180)));
    Layer spot = light(LightSpot);
    Layer glossy = plane();
    glossy.p[SurfaceShine].base = 40;
    const Scene banded = with({spot, glossy});
    const QImage whole = render(banded);
    QImage stitched(whole.size(), QImage::Format_ARGB32);
    stitched.fill(Qt::transparent);
    {
      QPainter painter(&stitched);
      painter.setCompositionMode(QPainter::CompositionMode_Source);
      for (int band = 0; band < 3; ++band) {
        const QRect region(0, band * 120, 640, 120);
        painter.drawImage(region.topLeft(), renderCPU(banded, 10, false, {}, region).convertToFormat(QImage::Format_ARGB32));
      }
    }
    check(worst(whole, stitched) <= 1, QString("a lit frame drawn in parts is the frame drawn whole (%1)").arg(worst(whole, stitched)));
  }
  {
    // Saved and opened again.
    Scene s = with({light(LightPoint), plane()});
    s.layers[0].lightFalloff = 2; s.layers[0].color = QColor(255, 200, 120);
    s.layers[0].p[LightIntensity].set(0, 40); s.layers[0].p[LightIntensity].set(30, 160);
    s.layers[1].acceptsLights = false; s.layers[1].p[SurfaceGloss].base = 77;
    Scene back;
    QString error;
    const bool loaded = back.load(s.json(), error);
    check(loaded && back.layers.size() == 2 && back.layers[0].type == "light" && back.layers[0].threeD &&
              back.layers[0].lightKind == LightPoint && back.layers[0].lightFalloff == 2 &&
              back.layers[0].color == QColor(255, 200, 120) && back.layers[0].p[LightIntensity].keys.size() == 2 &&
              !back.layers[1].acceptsLights && back.layers[1].p[SurfaceGloss].base == 77,
          "a light and how a layer takes light are saved and opened again");
    QJsonObject project = with({plane()}).json();
    QJsonArray layers = project["layers"].toArray();
    QJsonObject old = layers[0].toObject();
    old.remove("acceptsLights");
    layers[0] = old;
    project["layers"] = layers;
    Scene older;
    check(older.load(project, error) && older.layers.size() == 1 && older.layers[0].acceptsLights,
          "a project from before lights opens with its 3D layers taking light");
  }
  {
    // The card draws the lit layer the way the processor does.
    GpuRenderer gpu;
    const Scene s = with({light(LightSpot), plane()});
    const QImage card = gpu.render(s, 10, QSize(640, 360));
    if (card.isNull()) report << "GPU unavailable here: " + gpu.description();
    else {
      const QImage cpu = render(s);
      const QImage converted = card.convertToFormat(QImage::Format_ARGB32);
      check(std::abs(grey(converted, 320, 180) - grey(cpu, 320, 180)) <= 2 &&
                std::abs(grey(converted, 320 + 150, 180) - grey(cpu, 320 + 150, 180)) <= 2,
            QString("the card lights a layer as the processor does (%1/%2, %3/%4)")
                .arg(grey(converted, 320, 180)).arg(grey(cpu, 320, 180))
                .arg(grey(converted, 470, 180)).arg(grey(cpu, 470, 180)));
    }
  }
  {
    // Groups: a 3D layer inside one is seen and lit as it is outside, when the
    // group shares the Main's camera and lights; otherwise the group is a Main
    // of its own, as before.
    Layer camera;
    camera.type = "camera"; camera.name = "Camera"; camera.threeD = true; camera.duration = 90;
    camera.p[CameraZoom].base = 50. * 640 / 36;
    camera.p[PositionZ].base = -camera.p[CameraZoom].base;
    camera.p[0].base = 400; camera.p[1].base = 160; camera.p[RotationY].base = -8;
    Layer spot = light(LightSpot);
    spot.p[LightConeAngle].base = 40;
    Layer group;
    group.type = "group"; group.name = "Group"; group.duration = 90; group.sharesMainView = true;
    Layer inside = plane();
    inside.groupId = group.id;
    const QImage outside = render(with({camera, spot, plane()}));
    const QImage shared = render(with({camera, spot, group, inside}));
    group.sharesMainView = false;
    const QImage ownMain = render(with({camera, spot, group, inside}));
    const QImage unlitDefault = render(with({plane()}));
    check(worst(shared, outside) <= 2 && worst(ownMain, unlitDefault) <= 2 && worst(shared, ownMain) > 40,
          QString("a group that shares the Main's camera and lights looks as its layers do outside it (%1/255); "
                  "one that does not is its own Main, as before (%2/255)").arg(worst(shared, outside)).arg(worst(ownMain, unlitDefault)));
    // A camera hanging from an empty still stands where the empty puts it.
    Layer rig;
    rig.type = "empty"; rig.name = "Rig"; rig.duration = 90; rig.threeD = true; rig.p[0].base = 60; rig.p[1].base = 0;
    Layer hung = camera;
    hung.parentId = rig.id; hung.p[0].base = 340; hung.p[1].base = 160;
    group.sharesMainView = true;
    const QImage rigOutside = render(with({rig, hung, spot, plane()}));
    const QImage rigShared = render(with({rig, hung, spot, group, inside}));
    check(worst(rigOutside, rigShared) <= 2 && worst(rigOutside, outside) <= 2,
          QString("a linked camera is seen from where its links put it, inside a group too (%1/255)").arg(worst(rigOutside, rigShared)));
    // Groups inside groups pass the view down, and a group opened on its own
    // (or rendered from the queue) shows what it shows in the Main.
    Layer outer = group, inner = group;
    outer.id = "outer"; outer.name = "Outer";
    inner.id = "inner"; inner.name = "Inner"; inner.groupId = "outer";
    Layer deep = plane();
    deep.groupId = "inner";
    const Scene nested = with({camera, spot, outer, inner, deep});
    const QImage nestedShared = render(nested);
    const Scene opened = compositionScene(nested, "outer");
    int cameras = 0, lights = 0;
    for (const auto &l : opened.layers) { cameras += l.type == "camera"; lights += l.type == "light"; }
    check(worst(nestedShared, outside) <= 2 && cameras == 1 && lights == 1 && worst(render(opened), outside) <= 2,
          QString("a group in a group shares the view too (%1/255), and opened on its own it looks the same (%2/255)")
              .arg(worst(nestedShared, outside)).arg(worst(render(opened), outside)));
    Scene saved = with({camera, spot, group, inside});
    Scene back;
    QString error;
    Scene older;
    QJsonObject project = saved.json();
    QJsonArray layers = project["layers"].toArray();
    QJsonObject old = layers[2].toObject();
    old.remove("sharesMainView");
    layers[2] = old;
    project["layers"] = layers;
    check(back.load(saved.json(), error) && back.layers[2].sharesMainView && older.load(project, error) &&
              !older.layers[2].sharesMainView,
          "whether a group shares the view is saved; a group from before is its own Main, as it was");
  }
  {
    // Shadows. A white wall 200 behind the middle, a 100 square in the middle
    // throwing a shadow, a lamp 300 to the left and 400 in front. Through the
    // square the lamp's light lands on the wall 1.5 times as large and 150 to
    // the right: x 395 to 545, y 105 to 255 on the wall -- on screen, seen
    // from the camera, x 380 to 498 and y 121 to 239. The square itself is
    // seen at x 270 to 370, clear of its shadow.
    auto wall = [&] {
      Layer w = plane();
      w.name = "Wall"; w.p[ShapeWidth].base = 1200; w.p[ShapeHeight].base = 800; w.p[PositionZ].base = 200;
      return w;
    };
    auto square = [&](const QString &kind = "rect") {
      Layer c = plane(QColor(255, 60, 60));
      c.name = "Caster"; c.shapeKind = kind; c.p[ShapeWidth].base = 100; c.p[ShapeHeight].base = 100;
      c.castsShadows = true;
      return c;
    };
    auto lamp = [&](int kind = LightPoint) {
      Layer l = light(kind);
      l.p[0].base = 20; l.lightShadows = true; l.p[LightShadowSoftness].base = 0;
      return l;
    };
    const QImage shaded = render(with({lamp(), square(), wall()}));
    const int inShadow = grey(shaded, 440, 180), clear = grey(shaded, 520, 180);
    Layer noShadowLamp = lamp();
    noShadowLamp.lightShadows = false;
    Layer notCasting = square();
    notCasting.castsShadows = false;
    const int lampOff = grey(render(with({noShadowLamp, square(), wall()})), 440, 180);
    const int casterOff = grey(render(with({lamp(), notCasting, wall()})), 440, 180);
    check(inShadow <= 3 && clear > 180 && lampOff > 180 && casterOff > 180,
          QString("a layer in the light's way throws a shadow where the light would land, and only when the light and "
                  "the layer both cast them (%1 in shadow, %2 beside it; %3 with the light's off, %4 with the layer's off)")
              .arg(inShadow).arg(clear).arg(lampOff).arg(casterOff));
    check(qRed(shaded.pixel(320, 180)) > 150, "a layer does not shadow itself");
    Layer half = lamp();
    half.p[LightShadowDarkness].base = 50;
    const int halfDark = grey(render(with({half, square(), wall()})), 440, 180);
    check(std::abs(halfDark - lampOff / 2) <= 6,
          QString("Shadow Darkness 50% lets half the light through (%1, lit %2)").arg(halfDark).arg(lampOff));
    const QImage round = render(with({lamp(), square("circle"), wall()}));
    check(grey(round, 440, 180) <= 3 && grey(round, 388, 129) > 150,
          QString("a round layer throws a round shadow: its middle dark, the corner of its box lit (%1, %2)")
              .arg(grey(round, 440, 180)).arg(grey(round, 388, 129)));
    Layer soft = lamp();
    soft.p[LightShadowSoftness].base = 30;
    const int hard = grey(shaded, 480, 180), softened = grey(render(with({soft, square(), wall()})), 480, 180);
    check(hard <= 3 && softened > 20 && softened < 190,
          QString("Shadow Softness blurs the shadow's edge (%1 sharp, %2 soft)").arg(hard).arg(softened));
    // A sun throws the square's own size along its direction: x 420 to 520
    // on the wall, 399 to 478 on screen -- where the lamp's larger shadow
    // reaches 385, the sun's does not.
    const QImage sunlit = render(with({lamp(LightSun), square(), wall()}));
    check(grey(sunlit, 440, 180) <= 3 && grey(sunlit, 388, 180) > 150 && grey(shaded, 388, 180) <= 3,
          QString("a sun's shadow is the layer's own size, a lamp's grows with distance (%1 inside; at 388 %2 for the sun, "
                  "%3 for the lamp)").arg(grey(sunlit, 440, 180)).arg(grey(sunlit, 388, 180)).arg(grey(shaded, 388, 180)));
    Layer behind = square();
    behind.p[PositionZ].base = 300;
    check(grey(render(with({lamp(), behind, wall()})), 440, 180) > 180, "a layer behind the wall throws no shadow on it");
    Scene saved = with({lamp(), square(), wall()});
    saved.layers[0].p[LightShadowSoftness].base = 12;
    Scene back;
    QString error;
    check(back.load(saved.json(), error) && back.layers[0].lightShadows && back.layers[1].castsShadows &&
              !back.layers[2].castsShadows && back.layers[0].p[LightShadowSoftness].base == 12,
          "shadows are saved with the light and the layer");
    // Full HD: a wall, three casters, a spot with soft shadows, every frame different.
    Scene big;
    big.width = 1920; big.height = 1080; big.fps = 30; big.frames = 60; big.motionBlur = false;
    Layer backWall;
    backWall.type = "fill"; backWall.threeD = true; backWall.duration = 60; backWall.color = QColor(200, 200, 200);
    backWall.p[0].base = 960; backWall.p[1].base = 540; backWall.p[PositionZ].base = 300;
    backWall.p[2].base = backWall.p[3].base = 1.3;
    Layer spot = light(LightSpot);
    spot.lightShadows = true;
    spot.p[0].set(0, 500); spot.p[0].set(59, 1400); spot.p[1].base = 300; spot.p[PositionZ].base = -900;
    spot.p[LightAimX].base = 960; spot.p[LightAimY].base = 540; spot.p[LightConeAngle].base = 70;
    std::vector<Layer> layers{spot};
    for (int k = 0; k < 3; ++k) {
      Layer c = square(k == 1 ? "circle" : "rect");
      c.p[0].base = 600 + k * 360; c.p[1].base = 540; c.p[ShapeWidth].base = c.p[ShapeHeight].base = 220;
      c.p[RotationY].base = 20 * (k - 1);
      layers.push_back(c);
    }
    layers.push_back(backWall);
    big.layers = layers;
    renderCPU(big, 0, false);
    std::vector<double> times;
    for (int f = 1; f <= 12; ++f) {
      QElapsedTimer timer;
      timer.start();
      renderCPU(big, f * 4, false);
      times.push_back(timer.nsecsElapsed() / 1e6);
    }
    std::sort(times.begin(), times.end());
    check(times[6] < 30, QString("Full HD with three layers throwing soft shadows: median %1 ms").arg(times[6], 0, 'f', 1));
  }
  {
    // Full HD: a frame-filling plane under a moving spot with a highlight,
    // and a point light, every frame different.
    Scene big;
    big.width = 1920; big.height = 1080; big.fps = 30; big.frames = 60; big.motionBlur = false;
    Layer wall;
    wall.type = "fill"; wall.name = "Wall"; wall.threeD = true; wall.duration = 60; wall.color = QColor(180, 170, 160);
    wall.p[0].base = 960; wall.p[1].base = 540; wall.p[SurfaceShine].base = 30;
    Layer spot = light(LightSpot);
    spot.p[0].set(0, 600); spot.p[0].set(59, 1300); spot.p[1].base = 400; spot.p[PositionZ].base = -900;
    spot.p[LightAimX].set(0, 700); spot.p[LightAimX].set(59, 1200); spot.p[LightAimY].base = 540;
    spot.p[LightConeAngle].base = 45; spot.p[LightConeFeather].base = 40;
    Layer bulb = light(LightPoint);
    bulb.p[0].base = 400; bulb.p[1].base = 300; bulb.p[PositionZ].base = -300; bulb.lightFalloff = 1;
    auto medianOf = [&](const Scene &s) {
      renderCPU(s, 0, false);
      std::vector<double> times;
      for (int f = 1; f <= 12; ++f) {
        QElapsedTimer timer;
        timer.start();
        renderCPU(s, f * 4, false);
        times.push_back(timer.nsecsElapsed() / 1e6);
      }
      std::sort(times.begin(), times.end());
      return times[times.size() / 2];
    };
    big.layers = {spot, bulb, wall};
    const double lit = medianOf(big);
    big.layers = {wall};
    const double plain = medianOf(big);
    // The light pass alone, on a frame of pixels.
    big.layers = {spot, bulb, wall};
    QImage frame(1920, 1080, QImage::Format_RGBA8888_Premultiplied);
    frame.fill(QColor(180, 170, 160));
    const SceneLighting lighting = sceneLighting(big, 20);
    QElapsedTimer pass;
    pass.start();
    lightLayerImage(frame, QPoint(), projectLayer(big.layers[2], big, 20), layerWorldMatrix(big.layers[2], big, 20), lighting, .3f, .3f);
    const double alone = pass.nsecsElapsed() / 1e6;
    check(lit < 20, QString("a lit Full HD frame: median %1 ms (unlit %2 ms; the light pass alone %3 ms)")
                        .arg(lit, 0, 'f', 1).arg(plain, 0, 'f', 1).arg(alone, 0, 'f', 1));
  }
  return failures ? 1 : 0;
}
