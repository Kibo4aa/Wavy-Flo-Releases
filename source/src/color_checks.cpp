#include "engine.h"
#include "native_media.h"
#include "render_export.h"
#include "gpu_renderer.h"
#include <QColorSpace>
#include <QColorTransform>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTimer>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>
#include <cmath>

// --color-self-test
int checkColor(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  QTemporaryDir out;
  auto settle = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };

  {
    // A movie out and back: the colours it was given, at HD and SD sizes, and
    // the movie says what it is.
    struct Patch { QColor colour; };
    const std::vector<QColor> patches{QColor(220, 30, 30), QColor(30, 200, 60), QColor(30, 60, 220), QColor(240, 220, 40),
                                      QColor(224, 172, 140), QColor(128, 128, 128), QColor(20, 20, 20), QColor(240, 240, 240)};
    for (QSize size : {QSize(1280, 720), QSize(1920, 1080), QSize(640, 360)}) {
      QImage pattern(size, QImage::Format_RGB32);
      const int cellW = size.width() / 4, cellH = size.height() / 2;
      {
        QPainter painter(&pattern);
        for (size_t i = 0; i < patches.size(); ++i)
          painter.fillRect(QRect(int(i % 4) * cellW, int(i / 4) * cellH, cellW, cellH), patches[i]);
      }
      const QString movie = out.path() + QString("/colors-%1.mp4").arg(size.height());
      QImage back;
      try {
        NativeMedia media;
        media.beginMovie(movie, size.width(), size.height(), 30, 8000000, false);
        for (int f = 0; f < 15; ++f) media.writeFrame(pattern, f, 30);
        media.finishMovie();
        NativeMedia reader;
        back = reader.videoFrame(movie, 5 * 10000000LL / 30);
      } catch (const std::exception &e) {
        report << QString("movie: %1").arg(e.what());
      }
      int worst = 999;
      if (!back.isNull() && back.size() == size) {
        worst = 0;
        for (size_t i = 0; i < patches.size(); ++i) {
          const QColor got(back.pixel(int(i % 4) * cellW + cellW / 2, int(i / 4) * cellH + cellH / 2));
          worst = std::max({worst, std::abs(got.red() - patches[i].red()), std::abs(got.green() - patches[i].green()),
                            std::abs(got.blue() - patches[i].blue())});
        }
      }
      check(worst <= 3, QString("a %1p render keeps its colours through the movie and back (worst %2/255; read back %3x%4)")
                            .arg(size.height()).arg(worst).arg(back.width()).arg(back.height()));
      QMediaPlayer player;
      QVideoSink sink;
      player.setVideoSink(&sink);
      QVideoFrameFormat format;
      bool seen = false;
      QObject::connect(&sink, &QVideoSink::videoFrameChanged, [&](const QVideoFrame &frame) {
        if (seen || !frame.isValid()) return;
        format = frame.surfaceFormat();
        seen = true;
      });
      player.setSource(QUrl::fromLocalFile(movie));
      player.play();
      QElapsedTimer clock;
      clock.start();
      while (!seen && clock.elapsed() < 5000) settle(10);
      player.stop();
      check(seen && format.colorSpace() == QVideoFrameFormat::ColorSpace_BT709 &&
                format.colorTransfer() == QVideoFrameFormat::ColorTransfer_BT709 &&
                format.colorRange() == QVideoFrameFormat::ColorRange_Video,
            QString("the %1p movie says it is Rec. 709, video range, so players need not guess").arg(size.height()));
    }
  }
  {
    // PNG renders say they are sRGB.
    Scene s;
    s.width = 32; s.height = 18; s.fps = 30; s.frames = 2;
    Layer fill;
    fill.type = "fill"; fill.duration = 2; fill.color = QColor(200, 80, 40);
    fill.p[0].base = 16; fill.p[1].base = 9;
    s.layers = {fill};
    RenderSettings settings;
    settings.format = 1; settings.includeAudio = false;
    const QString folder = out.path() + "/png";
    const QString error = renderToFile(s, settings, folder, [](Scene &scene) { return renderCPU(scene, scene.frame, false); }, {});
    const QImage frame = QImageReader(folder + "/frame-000000.png").read();
    check(error.isEmpty() && frame.colorSpace() == QColorSpace(QColorSpace::SRgb), "a PNG render is tagged sRGB");
  }
  {
    // A photo from a phone, in Display P3, is brought into sRGB when opened.
    QImage p3(8, 8, QImage::Format_RGB32);
    p3.fill(QColor(200, 100, 50));
    p3.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QString file = out.path() + "/p3.png";
    p3.save(file);
    QImageReader reader(file);
    const QImage raw = reader.read();
    const QImage opened = imageInWorkingSpace(raw);
    const QColor expected = QColor::fromRgba(QColorSpace(QColorSpace::DisplayP3).transformationToColorSpace(QColorSpace(QColorSpace::SRgb))
                                                 .map(qRgb(200, 100, 50)));
    const QColor got(opened.pixel(4, 4));
    const bool converted = raw.colorSpace() == QColorSpace(QColorSpace::DisplayP3) &&
                           std::abs(got.red() - expected.red()) <= 1 && std::abs(got.green() - expected.green()) <= 1 &&
                           std::abs(got.blue() - expected.blue()) <= 1 && got != QColor(200, 100, 50);
    QImage plain(8, 8, QImage::Format_RGB32);
    plain.fill(QColor(200, 100, 50));
    QImage tagged = plain;
    tagged.setColorSpace(QColorSpace(QColorSpace::SRgb));
    check(converted && imageInWorkingSpace(plain) == plain && imageInWorkingSpace(tagged).pixel(4, 4) == tagged.pixel(4, 4),
          QString("a Display P3 photo is converted to sRGB (200,100,50 becomes %1,%2,%3); sRGB and untagged ones are left alone")
              .arg(got.red()).arg(got.green()).arg(got.blue()));
    // And through a project that names the file.
    Scene s;
    Layer image;
    image.type = "image"; image.path = file; image.image = raw;
    s.layers = {image};
    QJsonObject project = s.json();
    QJsonArray layers = project["layers"].toArray();
    QJsonObject layer = layers[0].toObject();
    layer.remove("imageData");
    layer["path"] = file;
    layers[0] = layer;
    project["layers"] = layers;
    Scene back;
    QString error;
    check(back.load(project, error) && QColor(back.layers[0].image.pixel(4, 4)) == got,
          "an image layer opened from its file is in sRGB too");
  }
  {
    // The curve itself.
    QImage pixels(3, 1, QImage::Format_RGBA32FPx4_Premultiplied);
    auto *p = reinterpret_cast<float *>(pixels.bits());
    const float values[12] = {.5f, .5f, .5f, 1.f, .25f, .1f, 0.f, .5f, 1.5f, 1.f, .02f, 1.f};
    std::copy(values, values + 12, p);
    toLinearLight(pixels);
    const bool grey = std::abs(p[0] - .21404f) < 2e-4f;
    // Half transparent: the colour of .5 encoded, times its alpha.
    const bool halfClear = std::abs(p[4] / .5f - .21404f) < 2e-4f && p[7] == .5f;
    fromLinearLight(pixels);
    float worst = 0;
    for (int i = 0; i < 12; ++i) worst = std::max(worst, std::abs(p[i] - values[i]));
    check(grey && halfClear && worst < 1e-4f,
          QString("sRGB to linear light and back: mid grey is 21.4%, alpha is kept, round trip within %1").arg(worst));
  }
  {
    // A blur where red meets green: in linear light the middle is the mix of
    // the two lights, bright yellow; on encoded values it goes dark and muddy.
    QImage meeting(200, 200, QImage::Format_RGBA32FPx4_Premultiplied);
    {
      QPainter painter(&meeting);
      painter.fillRect(QRect(0, 0, 100, 200), QColor(255, 0, 0));
      painter.fillRect(QRect(100, 0, 100, 200), QColor(0, 255, 0));
    }
    Layer l;
    l.blur = 60;
    QImage encoded = meeting, linear = meeting;
    blurPass(encoded, l, 1);
    {
      LinearLightScope on(true);
      blurPass(linear, l, 1);
    }
    const QColor mud = encoded.convertToFormat(QImage::Format_ARGB32).pixelColor(100, 100);
    const QColor light = linear.convertToFormat(QImage::Format_ARGB32).pixelColor(100, 100);
    check(std::abs(mud.red() - 128) <= 6 && std::abs(mud.green() - 128) <= 6 && std::abs(light.red() - 188) <= 6 &&
              std::abs(light.green() - 188) <= 6 && light.alpha() == 255,
          QString("where red meets green a blur in linear light is bright (%1,%2) and on encoded values dark (%3,%4)")
              .arg(light.red()).arg(light.green()).arg(mud.red()).arg(mud.green()));
  }
  {
    // A whole frame: the switch changes a glowing layer, and only it.
    Scene s;
    s.width = 320; s.height = 180; s.fps = 30; s.frames = 10; s.motionBlur = false;
    s.background = Qt::black;
    Layer glowing;
    glowing.type = "shape"; glowing.shapeKind = "rect"; glowing.duration = 10; glowing.color = QColor(255, 60, 0);
    glowing.p[0].base = 110; glowing.p[1].base = 90; glowing.p[ShapeWidth].base = 60; glowing.p[ShapeHeight].base = 60;
    glowing.glow = 25; glowing.appliedEffects["Glow"] = true;
    Layer plain = glowing;
    plain.glow = 0; plain.appliedEffects = {};
    plain.p[0].base = 240;
    plain.color = QColor(40, 120, 255);
    s.layers = {glowing, plain};
    const QImage before = renderCPU(s, 3, false).convertToFormat(QImage::Format_ARGB32);
    s.linearLight = true;
    const QImage after = renderCPU(s, 3, false).convertToFormat(QImage::Format_ARGB32);
    int glowChanged = 0, plainChanged = 0;
    for (int y = 0; y < 180; ++y)
      for (int x = 0; x < 320; ++x)
        if (before.pixel(x, y) != after.pixel(x, y)) ++(x < 180 ? glowChanged : plainChanged);
    // Where the glow is a quarter of the way out, as light it is brighter.
    // (The frame is drawn on transparency: how much of the glow is there is its alpha.)
    const int dimBefore = qAlpha(before.pixel(110, 90 - 30 - 10)), dimAfter = qAlpha(after.pixel(110, 90 - 30 - 10));
    const bool core = before.pixel(110, 90) == after.pixel(110, 90);
    check(glowChanged > 1000 && plainChanged == 0 && dimAfter > dimBefore + 20 && core,
          QString("linear light makes the glow fall off as light (%1 pixels changed; ten pixels out %2 becomes %3), "
                  "leaves the layer's own colour and anything without a glow alone (%4)")
              .arg(glowChanged).arg(dimBefore).arg(dimAfter).arg(plainChanged));
    GpuRenderer gpu;
    const QImage card = gpu.render(s, 3, QSize(320, 180));
    if (card.isNull()) report << "GPU unavailable here: " + gpu.description();
    else {
      const QImage converted = card.convertToFormat(QImage::Format_ARGB32);
      int worst = 0;
      for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) {
          const QRgb a = converted.pixel(x, y), b = after.pixel(x, y);
          worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
        }
      check(worst <= 3, QString("the card draws a linear-light glow as the processor does (worst %1/255)").arg(worst));
    }
    Scene back;
    QString error;
    check(back.load(s.json(), error) && back.linearLight && !Scene().linearLight,
          "linear light is kept with the project, and off for a new one");
  }
  return failures ? 1 : 0;
}
