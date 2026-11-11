#include <QElapsedTimer>
#include "engine.h"
#include <cmath>

int checkScene3D(QStringList &report) {
  int failures = 0;
  auto check = [&](bool ok, const char *name) {
    QFile progress(QCoreApplication::applicationDirPath()+"/3d-check-progress.txt");
    if(progress.open(QIODevice::WriteOnly|QIODevice::Append)) {
      progress.write((QString(ok?"PASS: ":"FAIL: ")+name+"\n").toUtf8());progress.flush();
    }
    report << QString(ok ? "PASS: " : "FAIL: ") + name;
    if (!ok) ++failures;
  };
  auto close = [](QPointF a, QPointF b) { return QLineF(a,b).length() < .01; };
  Scene s;
  s.width=1280;s.height=720; // fixed framing fixture, independent of new-project defaults
  {
  {
    // 3D Extrude: a shape given depth grows side walls that a flat one has not,
    // the walls sit behind the face, and a layer that is not 3D ignores depth.
    Scene scene; scene.width = 400; scene.height = 300; scene.frames = 10;
    Layer box; box.type = "shape"; box.shapeKind = "rect"; box.color = QColor(255, 0, 0); box.duration = 10;
    box.p[0].base = 200; box.p[1].base = 150; box.p[ShapeWidth].base = 100; box.p[ShapeHeight].base = 100;
    box.threeD = true; box.p[RotationY].base = 40;
    Layer flat = box;
    box.p[ExtrudeDepth].base = 80;
    Layer notThreeD = box; notThreeD.threeD = false; notThreeD.p[RotationY].base = 0;
    auto covered = [](const QImage &image) {
      int count = 0;
      for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) if (qAlpha(image.pixel(x, y)) > 10) ++count;
      return count;
    };
    scene.layers = {flat};
    const int flatPixels = covered(renderCPU(scene, 0, false).convertToFormat(QImage::Format_ARGB32));
    scene.layers = {box};
    const int solidPixels = covered(renderCPU(scene, 0, false).convertToFormat(QImage::Format_ARGB32));
    check(solidPixels > flatPixels * 1.15, "Extrude adds side walls to a turned 3D shape");
    {
      // Extrude drawn quickly: the walls meet without a hairline between them
      // (a pen used to cover it, and stroking every wall was most of their
      // cost), a frame split into bands is the frame drawn whole, and a
      // bevelled title keeps well inside a frame's time.
      Scene quick; quick.width = 640; quick.height = 360; quick.frames = 10; quick.motionBlur = false;
      Layer block; block.type = "shape"; block.shapeKind = "rect"; block.color = QColor(255, 255, 255); block.duration = 10;
      block.p[0].base = 320; block.p[1].base = 180; block.p[ShapeWidth].base = 200; block.p[ShapeHeight].base = 120;
      block.threeD = true; block.p[RotationY].base = 55; block.p[ExtrudeDepth].base = 160;
      block.p[ExtrudeBevel].base = 20; block.p[ExtrudeBevelRound].base = 100;
      block.p[ExtrudeShade].base = 0; // every wall the face's own white: a hairline would be all that shows
      quick.layers = {block};
      const QImage solid = renderCPU(quick, 0, false).convertToFormat(QImage::Format_ARGB32);
      int first = -1, last = -1, holes = 0;
      for (int x = 0; x < solid.width(); ++x)
        if (qAlpha(solid.pixel(x, 180)) > 250) { if (first < 0) first = x; last = x; }
      for (int x = first; first >= 0 && x <= last; ++x) holes += qAlpha(solid.pixel(x, 180)) < 250;
      check(first >= 0 && last - first > 100 && holes == 0, "Extruded walls meet without a hairline between them");
      Layer title; title.type = "text"; title.text = "WAVY"; title.fontSize = 150; title.color = QColor(240, 230, 220);
      title.duration = 10; title.threeD = true; title.p[0].base = 320; title.p[1].base = 180; title.p[RotationY].base = 25;
      title.p[ExtrudeDepth].base = 60; title.p[ExtrudeBevel].base = 8; title.p[ExtrudeBevelRound].base = 100;
      quick.layers = {title};
      const QImage whole = renderCPU(quick, 0, false).convertToFormat(QImage::Format_ARGB32);
      QImage stitched(whole.size(), QImage::Format_ARGB32);
      stitched.fill(Qt::transparent);
      {
        QPainter painter(&stitched);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        for (int band = 0; band < 3; ++band)
          painter.drawImage(0, band * 120, renderCPU(quick, 0, false, {}, QRect(0, band * 120, 640, 120)).convertToFormat(QImage::Format_ARGB32));
      }
      int worst = 0;
      for (int y = 0; y < whole.height(); ++y)
        for (int x = 0; x < whole.width(); ++x) {
          const QRgb a = whole.pixel(x, y), b = stitched.pixel(x, y);
          worst = std::max({worst, std::abs(qAlpha(a) - qAlpha(b)), std::abs(qRed(a) * qAlpha(a) - qRed(b) * qAlpha(b)) / 255});
        }
      check(compositionBands(quick, QSize(640, 360), 0) && worst <= 1,
            "A 3D title shares the frame out over the cores, and its parts make the whole");
      Scene big = quick;
      big.width = 1920; big.height = 1080;
      big.layers[0].fontSize = 230; big.layers[0].text = "WAVY-FLO"; big.layers[0].p[0].base = 960; big.layers[0].p[1].base = 540;
      big.layers[0].p[RotationY].set(0, -30); big.layers[0].p[RotationY].set(9, 30);
      big.layers[0].p[ExtrudeDepth].base = 70;
      renderCPU(big, 0, false);
      std::vector<double> times;
      for (int f = 1; f < 10; ++f) {
        QElapsedTimer timer; timer.start();
        renderCPU(big, f, false);
        times.push_back(timer.nsecsElapsed() / 1e6);
      }
      std::sort(times.begin(), times.end());
      check(times[times.size() / 2] < 20, "A bevelled 3D title in Full HD draws in under 20 ms");
      report << QString("Bevelled 3D title, Full HD: median %1 ms").arg(times[times.size() / 2], 0, 'f', 1);
    }
    check(layerExtrudeDepth(notThreeD, 0) == 0, "A layer that is not 3D has no extrude depth");
    box.p[ExtrudeDepth].base = 0;
    scene.layers = {box};
    const int zeroPixels = covered(renderCPU(scene, 0, false).convertToFormat(QImage::Format_ARGB32));
    check(zeroPixels == flatPixels, "Depth 0 draws exactly what a flat layer draws");
  }
  {
    // Handwriting and Extrude together: the pen gives the letters their depth
    // as it reaches them, so the end of a line has no sides yet while the
    // start already stands. Counted as ink, so it holds whatever face the
    // machine running this has (and says nothing when it has none).
    Scene scene; scene.width = 900; scene.height = 260; scene.frames = 10;
    Layer text; text.type = "text"; text.text = "Wavy Flo Wavy"; text.fontSize = 72;
    text.color = QColor(0, 0, 0); text.duration = 10; text.threeD = true;
    text.p[0].base = 450; text.p[1].base = 130;
    text.p[ExtrudeDepth].base = 40; text.p[RotationY].base = 25; text.p[RotationX].base = -10;
    text.characterAnimation = QJsonObject{{"enabled", true}, {"write", true}, {"writeNib", 115.},
                                          {"x", 0.}, {"y", 0.}, {"opacity", 100.}, {"scale", 100.}};
    auto ink = [&](double progress, int from, int to) {
      Scene one = scene; one.layers = {text}; one.layers[0].p[6].base = progress * 100;
      const QImage image = renderCPU(one, 0, false).convertToFormat(QImage::Format_ARGB32);
      int count = 0;
      for (int y = 0; y < image.height(); ++y)
        for (int x = from; x < std::min(to, image.width()); ++x)
          if (qAlpha(image.pixel(x, y)) > 40) ++count;
      return count;
    };
    const int wholeRight = ink(1., 560, 900);
    if (wholeRight > 0) {
      check(ink(.3, 560, 900) < wholeRight / 4,
            "Handwriting and Extrude together: the end of the line has no depth before the pen reaches it");
      check(ink(.3, 0, 300) > 0,
            "Handwriting and Extrude together: what has been written already stands in depth");
      Layer flat = text; flat.p[ExtrudeDepth].base = 0;
      Scene noDepth = scene; noDepth.layers = {flat}; noDepth.layers[0].p[6].base = 30;
      Scene withDepth = scene; withDepth.layers = {text}; withDepth.layers[0].p[6].base = 30;
      const QImage a = renderCPU(noDepth, 0, false).convertToFormat(QImage::Format_ARGB32);
      const QImage b = renderCPU(withDepth, 0, false).convertToFormat(QImage::Format_ARGB32);
      check(a != b, "Handwriting and Extrude together: depth still shows on what is written");
    }
  }
  {
    // Bevel: the wall leans outward by the bevel size before it runs straight, so
    // a beveled layer covers more of the frame than a plain extrude, and a
    // bevel larger than the depth is held to it rather than folding over.
    Scene scene; scene.width = 400; scene.height = 300; scene.frames = 10;
    Layer box; box.type = "shape"; box.shapeKind = "rect"; box.color = QColor(0, 0, 255); box.duration = 10;
    box.p[0].base = 200; box.p[1].base = 150; box.p[ShapeWidth].base = 100; box.p[ShapeHeight].base = 100;
    box.threeD = true; box.p[RotationY].base = 40; box.p[ExtrudeDepth].base = 60;
    auto covered = [&](const Layer &layer) {
      scene.layers = {layer};
      const QImage image = renderCPU(scene, 0, false).convertToFormat(QImage::Format_ARGB32);
      int count = 0;
      for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) if (qAlpha(image.pixel(x, y)) > 10) ++count;
      return count;
    };
    const int plain = covered(box);
    Layer beveled = box; beveled.p[ExtrudeBevel].base = 20;
    Layer rounded = beveled; rounded.p[ExtrudeBevelRound].base = 100;
    Layer excessive = box; excessive.p[ExtrudeBevel].base = 5000;
    Layer atDepth = box; atDepth.p[ExtrudeBevel].base = 60;
    check(covered(beveled) > plain, "Bevel widens the extruded body");
    check(covered(rounded) > plain && covered(rounded) != covered(beveled), "Round bevel differs from a chamfer");
    // Held to it: exactly what a bevel as deep as the extrude covers. (This used
    // to ask for under three times the plain body, which only held while a frame
    // drawn in bands left out the walls that stand beyond the face.)
    check(covered(excessive) > 0 && covered(excessive) == covered(atDepth), "A bevel deeper than half the extrude is held to half (it leans in again at the back)");
  }
    Scene original;
    Layer image; image.type="image"; image.image=QImage(32,32,QImage::Format_RGBA8888);
    image.image.fill(Qt::red); original.layers={image};
    Scene snapshot=original;
    check(snapshot.layers[0].image.constBits()==original.layers[0].image.constBits(),
          "Undo snapshot shares image storage without PNG encoding");
    original.layers[0].image.fill(Qt::blue);
    original.layers[0].p[0].base=99;
    check(snapshot.layers[0].image.pixelColor(0,0)==QColor(Qt::red) && snapshot.layers[0].p[0].base!=99,
          "Undo snapshot remains independent after image and property edits");
  }
  {
    Layer rectangle; rectangle.type="rect";
    rectangle.p[ShapeWidth].base=200; rectangle.p[ShapeHeight].base=200;
    rectangle.p[ShapeRoundness].base=100;
    auto round=shapePath(rectangle,0);
    check(round.contains(QPointF(0,0)) && !round.contains(QPointF(95,95)) && round.boundingRect().width()==200,
          "Square roundness reaches a circular outline without changing size");
    rectangle.p[ShapeWidth].set(0,100); rectangle.p[ShapeWidth].set(20,300);
    check(std::abs(shapePath(rectangle,10).boundingRect().width()-200)<.01,"Path size evaluates animated properties");
    Layer star; star.type="star";
    star.p[ShapePoints].base=5;
    auto five=shapePath(star,0);
    star.p[ShapePoints].base=8;
    check(shapePath(star,0)!=five,"Star points update its geometry");
    star.p[ShapeRoundness].base=12;
    check(shapePath(star,0).elementCount()>18,"Star roundness creates curved corners");
    star.p[ShapePoints].base=1;
    check(!shapePath(star,0).isEmpty(),"One-point star remains a valid degenerate path");
    Scene source; source.layers={rectangle,star}; Scene restored; QString error;
    check(restored.load(source.json(),error) && restored.layers[1].type=="shape" &&
          restored.layers[1].shapeKind=="star" &&
          restored.layers[0].p[ShapeWidth].keys.size()==2,"Parametric paths and keyframes persist");
  }
  {
    Scene shapes; shapes.width=200; shapes.height=200; shapes.motionBlur=false;
    Layer shape; shape.type="shape"; shape.p[0].base=100; shape.p[1].base=100;
    for (QPointF pt : {QPointF(-60,-60),QPointF(60,-60),QPointF(60,60),QPointF(-60,60)})
      shape.shapePoints.append(QJsonObject{{"p",QJsonArray{pt.x(),pt.y()}},{"in",QJsonArray{0,0}},{"out",QJsonArray{0,0}}});
    shape.color=Qt::red;
    shapes.layers={shape};
    auto full=renderCPU(shapes,0,false);
    check(full.pixelColor(100,100)==QColor(Qt::red),"Closed vector shape fills its interior");
    QJsonArray mask;
    for(QPointF pt : {QPointF(-60,-60),QPointF(0,-60),QPointF(0,60),QPointF(-60,60)})
      mask.append(QJsonObject{{"p",QJsonArray{pt.x(),pt.y()}}});
    shapes.layers[0].masks.append(QJsonObject{{"points",mask},{"enabled",true}});
    auto clipped=renderCPU(shapes,0,false);
    check(clipped.pixelColor(70,100).alpha()==255 && clipped.pixelColor(130,100).alpha()==0,"Mask clips a shape in local coordinates");
    auto m=shapes.layers[0].masks[0].toObject(); m["inverted"]=true; shapes.layers[0].masks[0]=m;
    auto inverted=renderCPU(shapes,0,false);
    check(inverted.pixelColor(70,100).alpha()==0 && inverted.pixelColor(130,100).alpha()==255,"Inverted mask retains outside area");
    m["inverted"]=false;m["expansion"]=20.;shapes.layers[0].masks[0]=m;
    auto expanded=renderCPU(shapes,0,false);
    check(expanded.pixelColor(115,100).alpha()==255,"Positive Mask Expansion grows the visible contour");
    m["expansion"]=-20.;shapes.layers[0].masks[0]=m;
    auto contracted=renderCPU(shapes,0,false);
    check(contracted.pixelColor(90,100).alpha()==0,"Negative Mask Expansion contracts the visible contour");
    m["expansion"]=0.;m["feather"]=12.;shapes.layers[0].masks[0]=m;
    auto feathered=renderCPU(shapes,0,false);
    check(feathered.pixelColor(100,100).alpha()>0 && feathered.pixelColor(100,100).alpha()<255 &&
          feathered.pixelColor(75,100).alpha()>feathered.pixelColor(115,100).alpha(),
          "Mask Feather produces a soft alpha boundary");
    check(feathered.pixelColor(60,100).alpha()==255,
          "Mask Feather preserves 100% solid opacity inside the core");
    Scene sharpScene; sharpScene.width=200; sharpScene.height=200; sharpScene.motionBlur=false;
    Layer sharpLayer; sharpLayer.type="shape"; sharpLayer.p[0].base=100; sharpLayer.p[1].base=100;
    for (QPointF pt : {QPointF(-100,-100),QPointF(100,-100),QPointF(100,100),QPointF(-100,100)})
      sharpLayer.shapePoints.append(QJsonObject{{"p",QJsonArray{pt.x(),pt.y()}}});
    sharpLayer.color=Qt::red;
    QJsonArray triangleMask;
    for (QPointF pt : {QPointF(0,-50), QPointF(50,50), QPointF(-50,50)})
      triangleMask.append(QJsonObject{{"p",QJsonArray{pt.x(),pt.y()}}});
    sharpLayer.masks.append(QJsonObject{{"points",triangleMask},{"enabled",true},{"expansion",20.0}});
    sharpScene.layers={sharpLayer};
    auto sharpRendered = renderCPU(sharpScene, 0, false);
    check(sharpRendered.pixelColor(100, 15).alpha() == 255, "Mask expansion preserves sharp pointy corners on acute vertices");
    Scene restored; QString error;
    check(restored.load(shapes.json(),error) && restored.layers[0].shapePoints==shape.shapePoints && restored.layers[0].masks==shapes.layers[0].masks,"Shape and mask paths survive project save/load");
  }
  s.motionBlur = false;
  Layer plane;
  plane.type = "rect";
  check(transformDimensions(plane,0) == std::vector<int>({0,1}), "2D Position has X and Y");
  const auto flat = layerWorldTransform(plane, s, 0);
  plane.threeD = true;
  check(transformDimensions(plane,0) == std::vector<int>({0,1,PositionZ}) &&
        transformDimensions(plane,2) == std::vector<int>({2,3,ScaleZ}) &&
        transformDimensions(plane,4) == std::vector<int>({RotationX,RotationY,4}) &&
        transformDimensions(plane,AnchorX) == std::vector<int>({AnchorX,AnchorY,AnchorZ}),
        "3D transform groups map X Y Z to their actual properties");
  check(!separatedDimensions(plane,RotationX) && !separatedDimensions(plane,AnchorZ),
        "Rotation and anchor dimensions are combined by default");
  check(close(flat.map(QPointF(70,20)), layerWorldTransform(plane,s,0).map(QPointF(70,20))),
        "Enabling 3D at Z zero preserves default framing");
  plane.p[PositionZ].base = s.width * 1.2;
  check(close(layerWorldTransform(plane,s,0).map(QPointF(100,0)), QPointF(690,360)),
        "Positive Z moves layer away with perspective");
  auto delta = layerPositionDelta(plane,s,0,QPointF(20,10));
  check(close(delta,QPointF(40,20)), "3D drag converts screen delta to world position");
  Layer camera;
  camera.type = "camera"; camera.threeD = true;
  camera.p[CameraZoom].base = s.width * 1.2;
  camera.p[PositionZ].base = -s.width * 1.2;
  s.layers = {camera, plane};
  auto before = layerWorldTransform(s.layers[1],s,0).map(QPointF(100,0));
  s.layers[0].p[0].base += 100;
  check(close(layerWorldTransform(s.layers[1],s,0).map(QPointF(100,0)),before-QPointF(50,0)),
        "Camera position controls 3D view");
  s.layers[1].threeD = false;
  check(close(layerWorldTransform(s.layers[1],s,0).map(QPointF(100,0)),QPointF(740,360)),
        "2D layers ignore active camera");
  s.layers[1].threeD = true;
  s.layers[1].p[PositionZ].base = -3000;
  check(!layerInFrontOfCamera(s.layers[1],s,0), "Behind-camera plane is culled");
  {
    Scene repeated;
    repeated.width = 200; repeated.height = 120; repeated.motionBlur = false;
    Layer repeatedCamera;
    repeatedCamera.type = "camera"; repeatedCamera.threeD = true;
    repeatedCamera.p[0].base = 100; repeatedCamera.p[1].base = 60;
    repeatedCamera.p[PositionZ].base = -240;
    repeatedCamera.p[CameraZoom].base = 240;
    Layer repeatedPlane;
    repeatedPlane.type = "rect"; repeatedPlane.threeD = true;
    repeatedPlane.p[0].base = 100; repeatedPlane.p[1].base = 60;
    repeatedPlane.p[PositionZ].base = -300;
    repeatedPlane.p[ShapeWidth].base = 40; repeatedPlane.p[ShapeHeight].base = 40;
    repeatedPlane.repeaterEnabled = true;
    repeatedPlane.repeaterCopies = 2;
    repeatedPlane.repeaterPositionX = 0;
    repeatedPlane.repeaterPositionZ = 200;
    repeated.layers = {repeatedCamera, repeatedPlane};
    check(!repeaterCopyInFrontOfCamera(repeated.layers[1], repeated, 0, 0) &&
              repeaterCopyInFrontOfCamera(repeated.layers[1], repeated, 0, 1) &&
              layerOrRepeaterInFrontOfCamera(repeated.layers[1], repeated, 0),
          "3D Repeater survives when its source crosses behind the camera");
    const auto repeatedImage = renderCPU(repeated, 0, false);
    bool repeatedVisible = false;
    for (int y = 0; y < repeatedImage.height() && !repeatedVisible; ++y)
      for (int x = 0; x < repeatedImage.width(); ++x)
        if (repeatedImage.pixelColor(x, y).alpha() != 0) {
          repeatedVisible = true;
          break;
        }
    check(repeatedVisible,
          "Visible Repeater Z copies render after the source crosses the camera");
  }
  s.layers[1].p[PositionZ].base = 0;
  s.layers[1].p[RotationY].base = 45;
  check(!layerWorldTransform(s.layers[1],s,0).isAffine(), "Y rotation creates perspective homography");
  Layer empty;
  empty.type = "empty"; empty.threeD = true;
  s.layers = {empty, plane};
  const auto oldMatrix = layerWorldMatrix(s.layers[1],s,0);
  check(setLayerParent(s,1,0) &&
        (layerWorldMatrix(s.layers[1],s,0).map(QVector3D(50,20,0)) - oldMatrix.map(QVector3D(50,20,0))).length() < .01,
        "Parenting to Empty preserves 3D world transform");
  s.layers[0].p[PositionZ].base = 100;
  check(std::abs(layerWorldMatrix(s.layers[1],s,0).map(QVector3D()).z() - oldMatrix.map(QVector3D()).z() - 100) < .01,
        "Empty moves child in Z");
  check(!setLayerParent(s,0,1), "3D parenting rejects cycles");
  s.layers[1].p[AnchorX].set(0,20);
  s.layers[1].separateAnchor = true;
  s.layers[1].separateRotation = true;
  s.layers[1].p[AnchorX].set(30,60);
  s.layers.push_back(camera);
  Scene loaded; QString error;
  check(loaded.load(s.json(),error) && loaded.layers[0].type == "empty" &&
        loaded.layers[2].type == "camera" && loaded.layers[1].threeD &&
        loaded.layers[1].separateAnchor && loaded.layers[1].separateRotation &&
        loaded.layers[1].p[AnchorX].keys.size() == 2 &&
        loaded.layers[1].parentBind3D == s.layers[1].parentBind3D,
        "Camera, 3D parent binding and anchor keys round-trip");
  Layer anchor;
  anchor.p[AnchorX].base = 40;
  anchor.p[4].base = 90;
  check(close(layerWorldTransform(anchor,s,0).map(QPointF(40,0)),QPointF(640,360)),
        "Rotation pivots around Anchor Point");
  s.layers = {camera, empty};
  auto image = renderCPU(s,0,false);
  bool transparent = true;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) transparent &= image.pixelColor(x,y).alpha() == 0;
  check(transparent, "Camera and Empty never appear in export");
  Layer near = plane, far = plane;
  near.id = "near"; far.id = "far";
  near.p[PositionZ].base = 0; far.p[PositionZ].base = 800;
  s.layers = {far,near};
  check(layerPaintOrder(s,0) == std::vector<int>({0,1}), "3D depth overrides stack order within group");
  {
    // Extruded letters that animate keep their walls with them: each letter's
    // sides where its face is, as faded as it is.
    Layer title; title.type = "text"; title.text = "Wavy"; title.font = "Arial"; title.fontSize = 120; title.duration = 90;
    title.threeD = true; title.p[ExtrudeDepth].base = 40;
    Scene still; still.width = 640; still.height = 300; still.fps = 30; still.frames = 90;
    const auto rest = layerExtrudePieces(title, still, 15);
    Layer rising = title;
    rising.characterAnimation = QJsonObject{{"enabled", true}, {"y", 60}, {"opacity", 0}};
    rising.p[6].set(0, 0); rising.p[6].set(30, 100);
    const auto moving = layerExtrudePieces(rising, still, 15);
    double lowest = 1;
    QRectF restBox, movingBox;
    for (const auto &piece : rest) restBox = restBox.united(piece.outline.boundingRect());
    for (const auto &piece : moving) { movingBox = movingBox.united(piece.outline.boundingRect()); lowest = std::min(lowest, piece.opacity); }
    check(rest.size() == 1 && moving.size() == 4 && movingBox.center().y() > restBox.center().y() + 5 && lowest > 0 && lowest < 1,
          "animated extruded letters keep their walls with their faces, moved and faded with them");
    // An Outside stroke is the edge of the face: the walls stand from it, not
    // from the letters inside it, still or animated -- the outline they stand
    // on covers the stroke as it is drawn, sharp corners and all, and no more.
    Layer stroked = title; stroked.textStrokes = {{12, QColor(255, 45, 85), 1, 0}}; syncLegacyTextStroke(stroked);
    stroked.font = "Arial"; stroked.fontStyle = "Black"; stroked.text = "Wavy-AV4";
    stroked.p[0].base = 320; stroked.p[1].base = 150;
    Scene flat = still; flat.layers = {Layer(stroked)}; flat.layers[0].threeD = false;
    const QImage drawn = renderCPU(flat, 15, false, QSize(640, 300)).convertToFormat(QImage::Format_ARGB32);
    QImage walls(drawn.size(), QImage::Format_ARGB32);
    walls.fill(0);
    {
      QPainter painter(&walls);
      painter.setRenderHint(QPainter::Antialiasing);
      painter.translate(320, 150);
      for (const auto &piece : layerExtrudePieces(stroked, still, 15)) painter.fillPath(piece.outline, Qt::white);
    }
    int strokeOutside = 0, wallsOutside = 0;
    for (int y = 0; y < drawn.height(); ++y)
      for (int x = 0; x < drawn.width(); ++x) {
        const int a = qAlpha(drawn.pixel(x, y)), b = qAlpha(walls.pixel(x, y));
        strokeOutside += a > 200 && b < 55;
        wallsOutside += b > 200 && a < 55;
      }
    Layer strokedRising = rising; strokedRising.textStrokes = stroked.textStrokes; syncLegacyTextStroke(strokedRising);
    QRectF strokedMovingBox;
    for (const auto &piece : layerExtrudePieces(strokedRising, still, 15)) strokedMovingBox = strokedMovingBox.united(piece.outline.boundingRect());
    check(strokeOutside == 0 && wallsOutside < 60 && strokedMovingBox.width() > movingBox.width() + 20,
          QString("extruded walls stand from the outer edge of an Outside stroke (%1 stroke pixels outside them, %2 wall pixels past it)")
              .arg(strokeOutside).arg(wallsOutside).toUtf8().constData());
    // And the sides carry the stroke on round the solid, in its colour; with no
    // stroke showing past the letters, the letters' own.
    Layer insideOnly = stroked; insideOnly.textStrokes = {{6, QColor(0, 0, 255), 0, 0}}; syncLegacyTextStroke(insideOnly);
    title.color = QColor(255, 210, 63);
    check(layerExtrudeSideColour(stroked) == QColor(255, 45, 85) && layerExtrudeSideColour(title) == QColor(255, 210, 63) &&
              layerExtrudeSideColour(insideOnly) == insideOnly.color,
          "extruded sides take the colour of the stroke at the face's edge, else the fill");
  }
  {
    // Turned past ninety degrees an extruded title is seen from behind: its
    // back carries the same picture as its face, at the full depth, over its
    // walls -- the face itself, on the far side, under them.
    Layer title; title.type = "text"; title.text = "Wavy"; title.font = "Arial"; title.fontSize = 120; title.duration = 90;
    title.color = QColor(255, 210, 63); title.threeD = true; title.p[ExtrudeDepth].base = 60; title.p[ExtrudeShade].base = 55;
    title.p[0].base = 320; title.p[1].base = 180;
    Scene seen; seen.width = 640; seen.height = 360; seen.fps = 30; seen.frames = 90;
    auto facePixels = [&](double turn) {
      Layer l = title; l.p[RotationY].base = turn;
      seen.layers = {l}; seen.frame = 0;
      const QImage image = renderCPU(seen, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
      int n = 0;
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
          const QColor c = image.pixelColor(x, y);
          n += c.alpha() > 250 && std::abs(c.red() - 255) < 4 && std::abs(c.green() - 210) < 4 && std::abs(c.blue() - 63) < 4;
        }
      return n;
    };
    Layer front = title; front.p[RotationY].base = 30;
    Layer behind = title; behind.p[RotationY].base = 150;
    const int faceOn = facePixels(20), faceAway = facePixels(180);
    // Where the yellow lies across the picture, from the front at 150 degrees
    // and from behind: set back by the depth, the back's picture is elsewhere.
    auto yellowCentre = [&](double turn) {
      Layer l = title; l.p[RotationY].base = turn;
      seen.layers = {l}; seen.frame = 0;
      const QImage image = renderCPU(seen, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
      double sum = 0; int n = 0;
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
          const QColor c = image.pixelColor(x, y);
          if (c.alpha() > 250 && std::abs(c.red() - 255) < 4 && std::abs(c.green() - 210) < 4 && std::abs(c.blue() - 63) < 4) { sum += x; ++n; }
        }
      return n ? sum / n : -1.;
    };
    Layer flat = title; flat.p[ExtrudeDepth].base = 0; flat.p[RotationY].base = 150;
    seen.layers = {flat};
    const QImage faceOnly = renderCPU(seen, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    double faceSum = 0; int faceCount = 0;
    for (int y = 0; y < faceOnly.height(); ++y)
      for (int x = 0; x < faceOnly.width(); ++x)
        if (faceOnly.pixelColor(x, y).alpha() > 250) { faceSum += x; ++faceCount; }
    const double backAt = yellowCentre(150), faceAt = faceCount ? faceSum / faceCount : -1;
    check(extrudeFrontFacesCamera(front, seen, 0, 0) && !extrudeFrontFacesCamera(behind, seen, 0, 0) &&
              faceOn > 2000 && faceAway > faceOn / 2 && std::abs(backAt - faceAt) > 10,
          QString("an extruded title seen from behind shows its back, the same picture as its face, set back by the depth "
                  "(back at x %1, face at x %2)").arg(backAt, 0, 'f', 0).arg(faceAt, 0, 'f', 0).toUtf8().constData());
  }
  return failures;
}
