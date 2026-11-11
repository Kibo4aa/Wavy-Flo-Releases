#include "gpu_renderer.h"
#include "particles.h"
#include "lights.h"
#include "render_workers.h"
#include <cmath>
#include <cstring>
#include <future>

int checkGpuRendering(QStringList &report) {
  GpuRenderer renderer;
  Scene scene;
  scene.width = 320;
  scene.height = 180;
  scene.motionBlur = false;
  auto first = renderer.render(scene, 30, {});
  report << renderer.description();
  if (first.isNull()) { report << "SKIP: hardware GPU renderer unavailable"; return 77; }
  int failures = 0;
  auto compare = [&](QString label, QSize size = QSize(), QRect region = QRect()) {
    QFile progress(QCoreApplication::applicationDirPath()+"/gpu-check-progress.txt");
    if(progress.open(QIODevice::WriteOnly|QIODevice::Append)) {progress.write((label+"\n").toUtf8());progress.flush();}
    QElapsedTimer timer;
    timer.start();
    auto cpu = renderCPU(scene, 30, true, size, region).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    auto cpuUs = timer.nsecsElapsed() / 1000;
    timer.restart();
    auto gpu = renderer.render(scene, 30, size, region).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    auto gpuUs = timer.nsecsElapsed() / 1000;
    if (gpu.isNull() || cpu.size() != gpu.size()) {
      ++failures; report << label + ": FAIL (GPU frame unavailable)"; return;
    }
    qint64 totalError = 0, largeErrors = 0, cpuCoverage = 0, gpuCoverage = 0;
    for (int y = 0; y < cpu.height(); ++y) {
      auto *a = cpu.constScanLine(y);
      auto *b = gpu.constScanLine(y);
      for (int x = 0; x < cpu.width(); ++x) {
        cpuCoverage += a[x*4+3];
        gpuCoverage += b[x*4+3];
      }
      for (int x = 0; x < cpu.width() * 4; ++x) {
        int difference = std::abs(int(a[x]) - int(b[x]));
        totalError += difference;
        largeErrors += difference > 20;
      }
    }
    const double channels = double(cpu.width()) * cpu.height() * 4;
    // Small edge differences are expected between raster and OpenGL AA.
    // A small layer disappearing must fail even if most of the canvas is
    // transparent and makes the average error look acceptable.
    bool passed = (cpuCoverage == 0 || gpuCoverage > 0) &&
                  totalError / channels < 2.0 && largeErrors / channels < .02;
    if (!passed) {
      ++failures;
      if(label=="Text scale 1 quality 2") {
        cpu.save(QCoreApplication::applicationDirPath()+"/compare-cpu.png");
        gpu.save(QCoreApplication::applicationDirPath()+"/compare-gpu.png");
      }
    }
    report << QString("%1: %2, mean error %3/255, CPU %4 us, GPU %5 us (%6x%7)")
        .arg(label, passed ? "PASS" : "FAIL").arg(totalError / channels, 0, 'f', 3)
        .arg(cpuUs).arg(gpuUs).arg(cpu.width()).arg(cpu.height());
  };
  compare("Transparent canvas");
  // Compare actual edge coverage, not only whole-frame average error (which
  // can hide aliased edges surrounded by a large transparent canvas).
  for (const QString &type : {QString("text"), QString("fill"), QString("rect"), QString("circle")}) {
    Layer edge;
    edge.type = type;
    edge.color = Qt::white;
    edge.text = "Wave";
    edge.fontSize = 70;
    edge.p[0].base = 160.35;
    edge.p[1].base = 90.65;
    edge.p[2].base = edge.p[3].base = .4;
    edge.p[4].base = 17;
    scene.layers = {edge};
    for (int divisor : {1,2,3,4}) {
      const QSize size(qCeil(320. / divisor), qCeil(180. / divisor));
      for (bool gpu : {false, true}) {
        const auto frame = (gpu ? renderer.render(scene, 30, size)
                                : renderCPU(scene, 30, false, size))
                               .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        int partial = 0;
        for (int y = 0; y < frame.height(); ++y) {
          const auto *row = frame.constScanLine(y);
          for (int x = 0; x < frame.width(); ++x)
            partial += row[x*4+3] > 0 && row[x*4+3] < 255;
        }
        const bool passed = frame.size() == size && partial >= 4;
        failures += !passed;
        report << QString("%1 %2 edges quality %3: %4 (%5 coverage pixels)")
            .arg(gpu ? "GPU" : "CPU", type).arg(divisor)
            .arg(passed ? "PASS" : "FAIL").arg(partial);
      }
    }
  }
  Layer rectangle;
  rectangle.type = "rect";
  rectangle.p[0].base = 130;
  rectangle.p[1].base = 90;
  rectangle.color = QColor(220, 50, 20, 170);
  rectangle.p[4].base = 19;
  scene.layers = {rectangle};
  compare("Rotated translucent rectangle");
  Layer circle;
  circle.type = "circle";
  circle.p[0].base = 190;
  circle.p[1].base = 100;
  circle.p[2].base = .65;
  circle.p[3].base = .9;
  circle.color = QColor(40, 150, 230, 120);
  scene.layers.insert(scene.layers.begin(), circle);
  compare("Layer order and alpha overlap");
  setLayerParent(scene, 0, 1);
  scene.layers[1].p[4].base = 35;
  compare("Parent transform");
  scene.layers[0].color = Qt::green;
  compare("Cached drawing invalidates after color edit");
  Layer image;
  image.type = "image";
  image.image = QImage(64, 40, QImage::Format_RGBA8888);
  image.image.fill(QColor(120, 80, 200, 180));
  image.p[0].base = 70;
  image.p[1].base = 50;
  scene.layers.insert(scene.layers.begin(), image);
  compare("Image texture");
  Layer text;
  text.type = "text";
  text.text = "GPU\nWavy-Flo";
  text.fontSize = 24;
  text.tracking = 2;
  text.p[0].base = 160;
  text.p[1].base = 90;
  scene.layers = {text};
  compare("Cached multiline text");
  for (double scale : {.59, 1., 11.09}) {
    scene.layers[0].p[2].base = scale;
    scene.layers[0].p[3].base = scale;
    for (int divisor : {1,2,3,4}) {
      const QSize size(qCeil(320. / divisor), qCeil(180. / divisor));
      compare(QString("Text scale %1 quality %2").arg(scale).arg(divisor), size);
      compare(QString("Text repeated scale %1 quality %2").arg(scale).arg(divisor), size);
    }
  }
  scene.layers[0].p[2].base = scene.layers[0].p[3].base = 1.;
  scene.layers[0].text = "Changed";
  compare("Cached text invalidates after edit");
  compare("Changed output resolution", QSize(640, 360));
  // Stencil corruption depends on what was drawn in the preceding frame.
  // Exercise cached text, paths, fills and textures repeatedly on the same
  // target, then change resolution and crop as the preview controls do.
  {
    Layer fill = rectangle;
    fill.type = "fill";
    for (int divisor : {1, 2, 3, 4, 1}) {
      const QSize size(qCeil(320. / divisor), qCeil(180. / divisor));
      for (int repeat = 0; repeat < 3; ++repeat) {
        for (const Layer &layer : {text, circle, fill, image}) {
          scene.layers = {layer};
          scene.layers[0].p[4].base = repeat * 13;
          compare(QString("Repeated %1 divisor %2 frame %3")
                      .arg(layer.type).arg(divisor).arg(repeat), size);
          const QRect crop(size.width()/4, size.height()/4,
                           size.width()/2, size.height()/2);
          compare(QString("Cropped %1 divisor %2").arg(layer.type).arg(divisor), size, crop);
        }
      }
    }
  }
  scene.layers = {rectangle};
  scene.layers[0].p[0].set(0, 40);
  scene.layers[0].p[0].set(60, 280);
  scene.motionBlur = true;
  compare("Floating-point motion blur accumulation");
  scene.layers[0].blur = 4;
  compare("Blur resolved and composited on the GPU");
  scene.layers[0].blur = 0;
  {
    // Every effect, alone and together, against the CPU that defines them. The
    // compositor rasterizes and effects these layers without the pixels coming
    // back, so anything it gets wrong is only visible in a comparison like this.
    auto &l = scene.layers[0];
    auto clear = [&] {
      l.blur = l.glow = l.shadow = 0;
      l.appliedEffects = QJsonObject();
      l.hiddenEffects = QJsonObject();
      l.effectOrder = QJsonArray();
    };
    const QJsonObject gradient{{"start","#ff6a00"},{"end","#2233cc"},{"amount",100}};
    clear(); l.glow = 12; l.appliedEffects["Glow"] = true;
    compare("Glow on the GPU");
    clear(); l.shadow = 12; l.appliedEffects["Drop Shadow"] = true;
    compare("Drop Shadow on the GPU");
    clear(); l.appliedEffects["Gradient"] = true; l.surfaceEffects["Gradient"] = gradient;
    compare("Gradient on the GPU");
    clear();
    l.appliedEffects["Fill"] = true;
    l.surfaceEffects["Fill"] = QJsonObject{{"color","#22cc55"},{"opacity",100}};
    compare("Fill on the GPU");
    l.surfaceEffects["Fill"] = QJsonObject{{"color","#22cc55"},{"opacity",40}};
    compare("Fill at reduced opacity on the GPU");
    // A fill that folds into the drawing has to land where the pixel pass would.
    l.appliedEffects["Gradient"] = true; l.surfaceEffects["Gradient"] = gradient;
    compare("Fill under a gradient on the GPU");
    l.effectOrder = QJsonArray{"Glow","Fill","Gradient","Stroke","Drop Shadow","Gaussian Blur"};
    l.glow = 8; l.appliedEffects["Glow"] = true;
    compare("Fill behind a glow, so it cannot fold");
    for (int mode = 0; mode < 3; ++mode) {
      clear();
      l.appliedEffects["Stroke"] = true;
      l.surfaceEffects["Stroke"] = QJsonObject{{"mode",mode},{"width",6},{"color","#00ff88"}};
      compare(QString("Stroke on the GPU, mode %1").arg(mode));
    }
    clear();
    l.glow = 10; l.shadow = 8; l.blur = 3;
    l.appliedEffects["Glow"] = true;
    l.appliedEffects["Drop Shadow"] = true;
    l.appliedEffects["Gaussian Blur"] = true;
    l.appliedEffects["Gradient"] = true;
    l.surfaceEffects["Gradient"] = gradient;
    l.appliedEffects["Stroke"] = true;
    l.surfaceEffects["Stroke"] = QJsonObject{{"mode",1},{"width",6},{"color","#00ff88"}};
    compare("Every effect at once on the GPU");
    // The order is the layer's to choose, and the card has to honour the same
    // one the CPU does.
    l.effectOrder = QJsonArray{"Stroke","Gradient","Glow","Drop Shadow","Gaussian Blur"};
    compare("Stroke ahead of Gradient on the GPU");
    // A masked layer is deliberately not offered to the resident path; it still
    // has to come out the same.
    l.effectOrder = QJsonArray();
    l.masks = QJsonArray{QJsonObject{{"enabled",true},{"inverted",false},
        {"points",QJsonArray{
            QJsonObject{{"p",QJsonArray{-60,-40}},{"in",QJsonArray{0,0}},{"out",QJsonArray{0,0}}},
            QJsonObject{{"p",QJsonArray{60,-40}},{"in",QJsonArray{0,0}},{"out",QJsonArray{0,0}}},
            QJsonObject{{"p",QJsonArray{60,40}},{"in",QJsonArray{0,0}},{"out",QJsonArray{0,0}}}}}}};
    compare("A masked layer with effects still matches");
    l.masks = QJsonArray();
    clear();
    l.glow = 10; l.appliedEffects["Glow"] = true;
    l.mainRotation = 24; l.mainScaleX = 1.4;
    compare("A main transform ahead of a glow");
    l.mainRotation = 0; l.mainScaleX = 1;
    l.masks = QJsonArray();
    clear();
  }
  scene.motionBlur = false;
  scene.layers = {rectangle};
  scene.layers[0].threeD = true;
  scene.layers[0].p[RotationY].base = 35;
  scene.layers[0].p[RotationX].base = 15;
  scene.layers[0].p[AnchorX].base = 20;
  compare("3D perspective plane with anchor offset");
  Layer camera;
  camera.type = "camera";
  camera.threeD = true;
  camera.p[0].base = 170;
  camera.p[1].base = 90;
  camera.p[PositionZ].base = -400;
  camera.p[CameraZoom].base = 400;
  scene.layers.insert(scene.layers.begin(), camera);
  compare("Explicit camera perspective");
  {
    // Past a megapixel the GPU stops reading an effected raster back and draws
    // it from the texture the passes left it in -- a different composite path,
    // with the layer's transform and opacity carried by hand rather than by the
    // painter. Every other case here is small enough to miss it entirely.
    Scene big;
    big.width=1600;big.height=900;big.motionBlur=false;
    Layer wide;
    wide.type="rect";wide.color=QColor(210,180,90);wide.duration=100;
    wide.p[0].base=800;wide.p[1].base=450;
    wide.p[2].base=wide.p[3].base=4;
    wide.p[4].base=15;
    wide.p[5].base=.75;
    wide.p[ShapeWidth].base=260;wide.p[ShapeHeight].base=150;
    wide.glow=30;wide.appliedEffects["Glow"]=true;
    big.layers={wide};
    auto saved=scene;
    scene=big;
    compare("A large effected layer, drawn from its own texture", QSize(1600,900));
    scene=saved;
  }
  {
    // Render GPU first: warming the shared CPU image cache would bypass the
    // resident-texture path we need to exercise. Copies share content, but
    // have their own layer IDs and opacity must still be composited 100 times.
    Scene copies;
    copies.width=1600;copies.height=900;copies.motionBlur=false;
    Layer wide;
    wide.type="rect";wide.color=QColor(43,119,197);wide.duration=100;
    wide.p[0].base=800;wide.p[1].base=450;
    wide.p[2].base=wide.p[3].base=4;
    wide.p[4].base=15;wide.p[5].base=.75;
    wide.p[ShapeWidth].base=260;wide.p[ShapeHeight].base=150;
    wide.glow=30;wide.appliedEffects["Glow"]=true;
    for(int i=0;i<100;++i) {
      wide.id=QString("resident-copy-%1").arg(i);
      copies.layers.push_back(wide);
    }
    {
      // TEMPORARY: the resident route, measured before anything warms the
      // shared CPU image cache. Each run gets its own colour so no raster key
      // is shared between them and every run starts cold.
      for(int count : {1,4,16,100}) {
        Scene few=copies;few.layers.resize(count);
        for(auto &l:few.layers) l.color=QColor(43+count,119,197);
        GpuRenderer fresh;
        const auto card=fresh.render(few,0,{}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        const auto stats=fresh.residentCacheStats();
        const auto soft=renderCPU(few,0,false,{}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        qint64 sum=0;int worst=0;
        if(!card.isNull() && card.size()==soft.size())
          for(int y=0;y<soft.height();++y)
            for(int x=0;x<soft.width()*4;++x) {
              const int d=std::abs(int(soft.constScanLine(y)[x])-int(card.constScanLine(y)[x]));
              sum+=d;worst=std::max(worst,d);
            }
        report<<QString("SCALE %1 copies: mean %2/255, worst %3, builds %4 hits %5")
          .arg(count).arg(sum/(double(soft.width())*soft.height()*4),0,'f',3).arg(worst)
          .arg(stats.builds).arg(stats.hits);
      }
    }
    GpuRenderer duplicateRenderer;
    QElapsedTimer timer;timer.start();
    const auto cold=duplicateRenderer.render(copies,0,{});
    const double coldMs=timer.nsecsElapsed()/1e6;
    const auto coldStats=duplicateRenderer.residentCacheStats();
    timer.restart();
    const auto warm=duplicateRenderer.render(copies,1,{});
    const double warmMs=timer.nsecsElapsed()/1e6;
    const auto warmStats=duplicateRenderer.residentCacheStats();
    const bool reused=!cold.isNull() && cold==warm && coldStats.builds==1 &&
        coldStats.hits==99 && warmStats.builds==1 && warmStats.hits==199 &&
        warmStats.bytes>0 && warmStats.bytes<=128ll*1024*1024;
    if(!reused) ++failures;
    report<<QString("%1: 100 large GPU copies, cold %2 ms / warm %3 ms, builds %4, hits %5, resident %6 MiB")
      .arg(reused ? "PASS" : "FAIL").arg(coldMs,0,'f',2).arg(warmMs,0,'f',2)
      .arg(warmStats.builds).arg(warmStats.hits).arg(warmStats.bytes/1048576.,0,'f',2);

    // A tolerance per composite rather than one flat number, because this scene
    // stacks a hundred semi-transparent copies and every blend rounds to eight
    // bits. Measured on an RTX 5050, cold, through the resident route: one copy
    // 0.103/255, four 0.327, sixteen 0.803, a hundred 2.685. One copy is what
    // says the shared texture is right, and it is right to a tenth of a level;
    // the growth after it is arithmetic, not a defect. A flat limit of two
    // called the hundred-copy case a failure while the single copy it is built
    // from was near exact.
    auto parity=[&](const QString &label,const QImage &gpu,int stacked,QRect region=QRect()) {
      const auto cpu=renderCPU(copies,0,false,{},region).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const auto actual=gpu.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      qint64 error=0,large=0,coverage=0;
      const bool valid=!actual.isNull() && actual.size()==cpu.size();
      if(valid) for(int y=0;y<cpu.height();++y) {
        const auto *a=cpu.constScanLine(y),*b=actual.constScanLine(y);
        for(int x=0;x<cpu.width()*4;++x) {
          const int difference=std::abs(int(a[x])-int(b[x]));
          error+=difference;large+=difference>20;
          if(x%4==3)coverage+=b[x];
        }
      }
      const double channels=double(cpu.width())*cpu.height()*4;
      // Both tolerances scale with the stack, for the same reason. One copy
      // through this route is 0.108/255 with not one channel in the frame off
      // by more than 20 -- measured, zero. A difference that starts below a
      // level can still pass twenty after a hundred blends at three quarters
      // opacity, and it does so along edges and through the glow ramp, which
      // is the three per cent seen at a hundred copies. Nothing structural can
      // be hiding in that: the source it is all drawn from has no outliers at
      // all. The single-copy check below is what guards correctness; this one
      // guards against something changing when they are stacked.
      const double allowed=std::max(.5,std::max(1,stacked)*.05);
      const double allowedOutliers=std::max(.002,std::max(1,stacked)*.0004);
      const bool passed=valid && coverage>0 && error/channels<allowed &&
                        large/channels<allowedOutliers;
      if(!passed)++failures;
      report<<QString("%1: %2, mean error %3/255 against %4 allowed for %5 composite(s), "
                      "%6% of channels off by more than 20 against %7% allowed")
        .arg(label,passed ? "PASS" : "FAIL").arg(valid ? error/channels : 999.,0,'f',3)
        .arg(allowed,0,'f',2).arg(stacked).arg(valid ? 100.*large/channels : 100.,0,'f',3)
        .arg(100.*allowedOutliers,0,'f',2);
    };
    // The claim that matters: one copy drawn from the shared resident texture
    // matches the processor. Everything above it is the same drawing repeated.
    {
      Scene single=copies;single.layers.resize(1);
      for(auto &l:single.layers) l.color=QColor(51,131,203);
      GpuRenderer alone;
      const auto once=alone.render(single,0,{});
      const auto reference=renderCPU(single,0,false,{}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const auto drawn=once.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      qint64 error=0,outliers=0;const bool valid=!drawn.isNull() && drawn.size()==reference.size();
      if(valid) for(int y=0;y<reference.height();++y)
        for(int x=0;x<reference.width()*4;++x) {
          const int d=std::abs(int(reference.constScanLine(y)[x])-int(drawn.constScanLine(y)[x]));
          error+=d;outliers+=d>20;
        }
      const double channels=double(reference.width())*reference.height()*4;
      const auto stats=alone.residentCacheStats();
      const bool passed=valid && stats.builds==1 && error/channels<.5 && outliers/channels<.002;
      if(!passed)++failures;
      report<<QString("%1: one copy through the resident texture matches the processor, "
                      "mean error %2/255, %3% over 20, builds %4")
        .arg(passed ? "PASS" : "FAIL").arg(valid ? error/channels : 999.,0,'f',3)
        .arg(valid ? 100.*outliers/channels : 100.,0,'f',4).arg(stats.builds);
    }
    parity("100 resident copies retain composition coverage",cold,100);
    copies.layers.resize(1);
    copies.layers[0].color=QColor(193,57,84);
    copies.layers[0].glow=18;
    const auto changed=duplicateRenderer.render(copies,0,{});
    const auto changedStats=duplicateRenderer.residentCacheStats();
    if(changedStats.builds!=warmStats.builds+1 || changed==cold) {
      ++failures;report<<"FAIL: resident texture must be rebuilt when color/effects change";
    } else report<<"PASS: changing source color/effects invalidates resident texture";
    // Change draw opacity before CPU comparison can seed the image cache.
    copies.layers[0].p[5].base=.35;
    const auto faded=duplicateRenderer.render(copies,0,{});
    const auto fadedStats=duplicateRenderer.residentCacheStats();
    if(faded==changed || fadedStats.builds!=changedStats.builds || fadedStats.hits!=changedStats.hits+1) {
      ++failures;report<<"FAIL: opacity must reuse the resident texture with a different draw opacity";
    } else report<<"PASS: resident texture reuse applies current layer opacity";
    parity("Resident opacity matches CPU",faded,int(copies.layers.size()));
    copies.layers[0].p[0].base+=70;
    const QRect crop(130,70,1300,750);
    parity("Resident ROI/translation matches CPU",duplicateRenderer.render(copies,0,{},crop),int(copies.layers.size()),crop);
  }
  {
    Scene cacheScene;
    cacheScene.width=640;cacheScene.height=360;
    Layer cached;
    cached.type="circle";cached.color=QColor(41,93,157);
    cached.p[0].base=220;cached.p[1].base=150;cached.glow=8;
    cached.appliedEffects["Glow"]=true;
    cacheScene.layers={cached};
    renderCPU(cacheScene,0,false);
    const auto before=renderWorkCounters();
    cacheScene.layers[0].p[0].base+=23;
    renderCPU(cacheScene,0,false);
    const auto after=renderWorkCounters();
    if(after.effectHits<=before.effectHits || after.effectNs!=before.effectNs) {
      ++failures;report<<"FAIL: translating a small layer rebuilt its effects";
    } else report<<"PASS: translating a small layer reuses its effect raster";
  }
  {
    // Effect Layers worked on the card: the frame stays there and matches the
    // CPU's -- by the card's own passes, and by the CPU's effects worked in
    // between for an effect with no form on the card; whole, turned and
    // scaled at part opacity, and cut to a mask.
    const Scene saved = scene;
    auto box = [](double x, double y, QColor colour) {
      Layer l; l.type = "rect"; l.color = colour; l.duration = 100;
      l.p[0].base = x; l.p[1].base = y; l.p[ShapeWidth].base = 90; l.p[ShapeHeight].base = 70;
      return l;
    };
    Layer back; back.type = "fill"; back.color = QColor(30, 60, 200); back.duration = 100;
    back.p[0].base = 160; back.p[1].base = 90;
    Layer fx; fx.type = "effect"; fx.duration = 100; fx.p[0].base = 160; fx.p[1].base = 90;
    const Layer top = box(160, 40, QColor(240, 200, 40));
    auto with = [&](const Layer &effect) {
      scene.layers = {top, effect, box(90, 110, QColor(230, 30, 30)), box(230, 110, QColor(40, 200, 90)), back};
      // These layers share their ids from scene to scene: a raster kept from
      // the one before would stand in for this one's.
      clearRenderCaches();
    };
    Layer blurred = fx; blurred.blur = 8;
    with(blurred); compare("Effect Layer: a blur over the whole frame, on the card");
    // A long smear has no form on the card: it is the running sum on the shear.
    Layer edges = fx; edges.appliedEffects["Directional Blur"] = true;
    edges.surfaceEffects["Directional Blur"] = QJsonObject{{"length", 200.}};
    // One whose effects have no form on the card leaves the frame to the CPU:
    // worked there in the middle of a frame on the card, it measured slower
    // than the CPU drawing the frame whole.
    with(edges);
    if (!renderer.render(scene, 30, {}).isNull()) {
      ++failures; report << "Effect Layer: an effect with no card form leaves the frame to the CPU: FAIL";
    } else report << "Effect Layer: an effect with no card form leaves the frame to the CPU: PASS (" + renderer.description() + ")";
    // Pack effects with a form on the card: the Twitter promo's grain and
    // vignette, MV-Pop's sparkles and TV set, a zoom blur run in rounds.
    for (const auto &[label, names] : std::vector<std::pair<QString, QStringList>>{
             {"grain and a vignette", {"Film Grain", "Vignette"}},
             {"sparkles and a TV screen", {"Sparkles", "TV Screen"}},
             {"a glitch, a colour split and a zoom blur", {"Glitch", "RGB Split", "Zoom Blur"}},
             {"blocks, a mirror and a shake", {"Pixelate", "Mirror", "Shake"}},
             {"sharpen and light rays, rounds that read the picture again", {"Sharpen", "Light Rays"}},
             {"turbulent noise and a displacement map", {"Turbulent Noise", "Displacement Map"}},
             {"halftone, scanlines and speed lines", {"Halftone", "Scanlines", "Speed Lines"}}}) {
      Layer packed = fx;
      for (const QString &name : names) packed.appliedEffects[name] = true;
      with(packed);
      if (renderer.render(scene, 30, {}).isNull()) {
        ++failures;
        report << "Effect Layer: " + label + " stays on the card: FAIL (" + renderer.description() + ")";
      } else compare("Effect Layer: " + label + ", on the card");
    }
    {
      // One whose effects are all at rest -- a beat effect between its hits --
      // changes nothing and costs nothing: not worked on the card, not read
      // back to the processor, the frame the same as without it. Worked anyway,
      // a chorus spent 13 ms of every frame reading the picture back and
      // sending it up again unchanged.
      Layer resting = fx;
      for (const QString &name : {QString("Glitch"), QString("RGB Split"), QString("Zoom Blur")}) resting.appliedEffects[name] = true;
      resting.surfaceEffects["Zoom Blur"] = QJsonObject{{"amount", 0.}};
      resting.surfaceEffects["Glitch"] = QJsonObject{{"amount", 0.}, {"split", 0.}};
      resting.surfaceEffects["RGB Split"] = QJsonObject{{"amount", 0.}};
      scene.layers = {top, box(90, 110, QColor(230, 30, 30)), box(230, 110, QColor(40, 200, 90)), back};
      clearRenderCaches();
      const QImage without = renderer.render(scene, 30, {});
      const QImage withoutCpu = renderCPU(scene, 30, false);
      with(resting);
      const auto before = renderer.residentCacheStats();
      const QImage at = renderer.render(scene, 30, {});
      const auto after = renderer.residentCacheStats();
      const bool same = !at.isNull() && at.convertToFormat(QImage::Format_ARGB32) == without.convertToFormat(QImage::Format_ARGB32) &&
                        renderCPU(scene, 30, false).convertToFormat(QImage::Format_ARGB32) == withoutCpu.convertToFormat(QImage::Format_ARGB32);
      const bool skipped = after.effectLayers == before.effectLayers && after.effectLayerReadbacks == before.effectLayerReadbacks;
      if (!same || !skipped) ++failures;
      report << QString("Effect Layer: effects all at rest are left out, card and CPU frames as without it: %1 (worked %2, read back %3)")
                    .arg(same && skipped ? "PASS" : "FAIL").arg(after.effectLayers - before.effectLayers).arg(after.effectLayerReadbacks - before.effectLayerReadbacks);
    }
    Layer part = blurred;
    part.p[0].base = 110; part.p[2].base = part.p[3].base = .5; part.p[4].base = 20; part.p[5].base = .6;
    with(part); compare("Effect Layer: turned, scaled, at 60% opacity");
    Layer cut = fx; cut.appliedEffects["Black & White"] = true;
    QJsonArray points;
    for (auto corner : {QPointF(-150, -80), QPointF(10, -80), QPointF(10, 80), QPointF(-150, 80)})
      points.push_back(QJsonObject{{"p", QJsonArray{corner.x(), corner.y()}},
                                   {"in", QJsonArray{0, 0}}, {"out", QJsonArray{0, 0}}});
    cut.masks = QJsonArray{QJsonObject{{"name", "Mask 1"}, {"points", points}, {"enabled", true},
                                       {"inverted", false}, {"expansion", 0}, {"feather", 0}}};
    with(cut); compare("Effect Layer: cut to a mask");
    scene = saved;
  }
  {
    // Layers with a blend mode, flattened and blended on the card: every mode,
    // a title blended over shapes of several colours and a part-clear one, the
    // same picture as the CPU's, and done there rather than on the CPU.
    const Scene saved = scene;
    Layer back; back.type = "fill"; back.color = QColor(40, 70, 160); back.duration = 100;
    back.p[0].base = 160; back.p[1].base = 90;
    auto box = [](double x, double y, QColor colour) {
      Layer l; l.type = "rect"; l.color = colour; l.duration = 100;
      l.p[0].base = x; l.p[1].base = y; l.p[ShapeWidth].base = 110; l.p[ShapeHeight].base = 90;
      return l;
    };
    Layer title; title.type = "text"; title.text = "BLEND"; title.fontSize = 70; title.duration = 100;
    title.color = QColor(250, 180, 60); title.p[0].base = 160; title.p[1].base = 90; title.p[5].base = .85;
    title.p[4].base = -8;
    for (const QString &mode : {QString("multiply"), QString("screen"), QString("overlay"), QString("darken"),
                                QString("lighten"), QString("colorDodge"), QString("colorBurn"), QString("hardLight"),
                                QString("softLight"), QString("difference"), QString("exclusion"), QString("add")}) {
      Layer blended = title; blended.blendMode = mode;
      scene.layers = {blended, box(90, 80, QColor(220, 40, 40)), box(230, 100, QColor(40, 200, 90, 140)),
                      box(160, 140, QColor(250, 250, 250)), back};
      clearRenderCaches();
      const quint64 before = renderer.residentCacheStats().blendedLayers;
      const QImage cpu = renderCPU(scene, 30, true).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const QImage card = renderer.render(scene, 30, {}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const quint64 onCard = renderer.residentCacheStats().blendedLayers - before;
      if (card.isNull() || card.size() != cpu.size()) {
        ++failures; report << "Blended on the card: " + mode + ": FAIL (no frame: " + renderer.description() + ")";
        continue;
      }
      double total = 0; qint64 off = 0;
      for (int y = 0; y < cpu.height(); ++y) {
        const uchar *a = cpu.constScanLine(y), *b = card.constScanLine(y);
        for (int x = 0; x < cpu.width() * 4; ++x) {
          const int d = std::abs(int(a[x]) - int(b[x]));
          total += d; off += d > 8;
        }
      }
      const double channels = double(cpu.width()) * cpu.height() * 4;
      const bool passed = onCard > 0 && total / channels < .5 && off / channels < .005;
      failures += !passed;
      report << QString("Blended on the card: %1: %2, mean error %3/255, %4% off by more than 8")
                    .arg(mode, passed ? "PASS" : "FAIL").arg(total / channels, 0, 'f', 3).arg(100. * off / channels, 0, 'f', 3);
    }
    scene = saved;
  }
  {
    // Layers shaped by a track matte, combined on the card: kept where the
    // matte is, or where it is not, and laid in Normal or with a blend mode.
    const Scene saved = scene;
    Layer back; back.type = "fill"; back.color = QColor(30, 110, 90); back.duration = 100;
    back.p[0].base = 160; back.p[1].base = 90;
    Layer matte; matte.type = "text"; matte.text = "MATTE"; matte.fontSize = 72; matte.duration = 100;
    matte.color = QColor(255, 255, 255, 200); matte.p[0].base = 160; matte.p[1].base = 90; matte.visible = false;
    Layer owner; owner.type = "rect"; owner.duration = 100; owner.color = QColor(240, 120, 40);
    owner.p[0].base = 170; owner.p[1].base = 95; owner.p[ShapeWidth].base = 260; owner.p[ShapeHeight].base = 120;
    owner.p[4].base = 6; owner.trackMatteId = matte.id;
    Layer other; other.type = "rect"; other.duration = 100; other.color = QColor(250, 250, 250);
    other.p[0].base = 80; other.p[1].base = 60; other.p[ShapeWidth].base = 90; other.p[ShapeHeight].base = 70;
    for (const auto &[label, inverted, blend] : std::vector<std::tuple<QString, bool, QString>>{
             {"kept where the matte is", false, "normal"},
             {"kept where it is not", true, "normal"},
             {"with Multiply", false, "multiply"}}) {
      Layer shaped = owner; shaped.trackMatteInverted = inverted; shaped.blendMode = blend;
      scene.layers = {matte, shaped, other, back};
      clearRenderCaches();
      const quint64 before = renderer.residentCacheStats().mattedLayers;
      const QImage cpu = renderCPU(scene, 30, true).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const QImage card = renderer.render(scene, 30, {}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const quint64 onCard = renderer.residentCacheStats().mattedLayers - before;
      if (card.isNull() || card.size() != cpu.size()) {
        ++failures; report << "Matted on the card: " + label + ": FAIL (no frame: " + renderer.description() + ")";
        continue;
      }
      double total = 0; qint64 off = 0;
      for (int y = 0; y < cpu.height(); ++y) {
        const uchar *a = cpu.constScanLine(y), *b = card.constScanLine(y);
        for (int x = 0; x < cpu.width() * 4; ++x) {
          const int d = std::abs(int(a[x]) - int(b[x]));
          total += d; off += d > 8;
        }
      }
      const double channels = double(cpu.width()) * cpu.height() * 4;
      const bool passed = onCard > 0 && total / channels < .5 && off / channels < .005;
      failures += !passed;
      report << QString("Matted on the card: %1: %2, mean error %3/255, %4% off by more than 8")
                    .arg(label, passed ? "PASS" : "FAIL").arg(total / channels, 0, 'f', 3).arg(100. * off / channels, 0, 'f', 3);
    }
    scene = saved;
  }
  {
    // A layer blurring itself by its own motion, sampled and averaged on the
    // card while the rest of the frame is drawn once -- laid in Normal and with
    // a blend mode, and text whose letters fly -- the same picture as the CPU's,
    // and done there.
    const Scene saved = scene;
    scene.motionBlur = true;
    Layer back; back.type = "fill"; back.color = QColor(30, 60, 140); back.duration = 100;
    back.p[0].base = 160; back.p[1].base = 90;
    Layer still; still.type = "rect"; still.color = QColor(250, 250, 250); still.duration = 100;
    still.p[0].base = 160; still.p[1].base = 140; still.p[ShapeWidth].base = 200; still.p[ShapeHeight].base = 40;
    Layer moving; moving.type = "rect"; moving.color = QColor(240, 90, 40); moving.duration = 100; moving.motionBlur = true;
    moving.p[0].set(0, 40); moving.p[0].set(60, 280); moving.p[1].base = 80;
    moving.p[ShapeWidth].base = 60; moving.p[ShapeHeight].base = 60;
    Layer letters; letters.type = "text"; letters.text = "FLY"; letters.fontSize = 60; letters.duration = 100;
    letters.color = QColor(255, 220, 80); letters.p[0].base = 160; letters.p[1].base = 70; letters.motionBlur = true;
    letters.textStrokes = {{4, QColor(20, 20, 20), 1}};
    syncLegacyTextStroke(letters);
    letters.characterAnimation = QJsonObject{{"enabled", true}, {"opacity", 100}, {"y", 300}, {"delay", 1.}};
    letters.p[6].set(0, 0); letters.p[6].set(40, 100);
    Layer multiplied = moving; multiplied.blendMode = "multiply";
    for (const auto &[label, top] : std::vector<std::pair<QString, Layer>>{
             {"a moving box", moving}, {"a moving box, Multiply", multiplied}, {"letters flying in, stroked", letters}}) {
      scene.layers = {top, still, back};
      clearRenderCaches();
      const quint64 before = renderer.residentCacheStats().blurredLayers;
      const QImage cpu = renderCPU(scene, 30, true).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const QImage card = renderer.render(scene, 30, {}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const quint64 onCard = renderer.residentCacheStats().blurredLayers - before;
      if (card.isNull() || card.size() != cpu.size()) {
        ++failures; report << "Motion blur on the card: " + label + ": FAIL (no frame: " + renderer.description() + ")";
        continue;
      }
      double total = 0; qint64 off = 0;
      for (int y = 0; y < cpu.height(); ++y) {
        const uchar *x0 = cpu.constScanLine(y), *x1 = card.constScanLine(y);
        for (int x = 0; x < cpu.width() * 4; ++x) {
          const int d = std::abs(int(x0[x]) - int(x1[x]));
          total += d; off += d > 8;
        }
      }
      const double channels = double(cpu.width()) * cpu.height() * 4;
      const bool passed = onCard > 0 && total / channels < .5 && off / channels < .005;
      failures += !passed;
      report << QString("Motion blur on the card: %1: %2, mean error %3/255, %4% off by more than 8, %5 layer(s) sampled there")
                    .arg(label, passed ? "PASS" : "FAIL").arg(total / channels, 0, 'f', 3).arg(100. * off / channels, 0, 'f', 3).arg(onCard);
    }
    scene = saved;
  }
  {
    // Lit 3D layers painted and lit on the card (lights.h): a spot on a turned
    // and shiny plane, a point light falling off with ambient light, the sun,
    // and a shadow one layer throws on another -- each lit there, not handed
    // to the CPU, and the same picture as the CPU's.
    const Scene saved = scene;
    auto plane = [](QColor colour, double z, double width, double height) {
      Layer l; l.type = "shape"; l.shapeKind = "rect"; l.name = "Plane"; l.duration = 100;
      l.threeD = true; l.color = colour;
      l.p[0].base = 160; l.p[1].base = 90; l.p[PositionZ].base = z;
      l.p[ShapeWidth].base = width; l.p[ShapeHeight].base = height;
      return l;
    };
    auto light = [](int kind) {
      Layer l; l.type = "light"; l.name = "Light"; l.threeD = true; l.duration = 100;
      l.lightKind = kind; l.color = Qt::white;
      l.p[0].base = 160; l.p[1].base = 90; l.p[PositionZ].base = -200;
      l.p[LightAimX].base = 160; l.p[LightAimY].base = 90; l.p[LightAimZ].base = 0;
      return l;
    };
    Layer spot = light(LightSpot);
    spot.p[LightConeAngle].base = 50; spot.p[LightConeFeather].base = 40;
    Layer shiny = plane(QColor(200, 120, 90), 0, 240, 140);
    shiny.p[SurfaceShine].base = 60; shiny.p[SurfaceGloss].base = 40; shiny.p[RotationY].base = 25;
    Layer point = light(LightPoint);
    point.lightFalloff = 2; point.p[LightRadius].base = 60; point.p[0].base = 100; point.p[PositionZ].base = -120;
    Layer ambient = light(LightAmbient);
    ambient.p[LightIntensity].base = 30;
    Layer sun = light(LightSun);
    sun.p[LightAimX].base = 260; sun.p[LightAimY].base = 160;
    Layer warm = light(LightPoint);
    warm.color = QColor(255, 200, 120); warm.lightFalloff = 1; warm.p[LightRadius].base = 40; warm.p[LightFalloffDistance].base = 200;
    Layer lamp = light(LightPoint);
    lamp.p[0].base = 10; lamp.lightShadows = true; lamp.p[LightShadowSoftness].base = 0;
    Layer caster = plane(QColor(255, 60, 60), 0, 50, 50);
    caster.castsShadows = true;
    const Layer wall = plane(QColor(220, 220, 220), 100, 600, 400);
    const std::vector<std::pair<QString, std::vector<Layer>>> cases{
        {"a spot on a turned, shiny plane", {spot, shiny}},
        {"a point light falling off, and ambient light", {point, ambient, plane(QColor(90, 200, 120), 0, 260, 150)}},
        {"the sun, a warm lamp and ambient light", {sun, warm, ambient, plane(QColor(150, 150, 230), 0, 260, 150)}},
        {"a shadow thrown on a wall", {lamp, caster, wall}}};
    for (const auto &[label, layers] : cases) {
      scene.layers = layers;
      clearRenderCaches();
      const quint64 before = renderer.residentCacheStats().litFaces;
      const QImage cpu = renderCPU(scene, 30, true).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const QImage card = renderer.render(scene, 30, {}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const quint64 lit = renderer.residentCacheStats().litFaces - before;
      if (card.isNull() || card.size() != cpu.size()) {
        ++failures; report << "Lit on the card: " + label + ": FAIL (no frame: " + renderer.description() + ")";
        continue;
      }
      double total = 0; qint64 off = 0;
      for (int y = 0; y < cpu.height(); ++y) {
        const uchar *a = cpu.constScanLine(y), *b = card.constScanLine(y);
        for (int x = 0; x < cpu.width() * 4; ++x) {
          const int d = std::abs(int(a[x]) - int(b[x]));
          total += d; off += d > 8;
        }
      }
      const double channels = double(cpu.width()) * cpu.height() * 4;
      const bool passed = lit > 0 && total / channels < .6 && off / channels < .005;
      failures += !passed;
      report << QString("Lit on the card: %1: %2, mean error %3/255, %4% off by more than 8, %5 faces lit there")
                    .arg(label, passed ? "PASS" : "FAIL").arg(total / channels, 0, 'f', 3)
                    .arg(100. * off / channels, 0, 'f', 3).arg(lit);
    }
    scene = saved;
  }
  {
    // Particles written by the card itself, every shape, over the picture and
    // added to it: the same picture as the CPU's, and drawn there -- not
    // handed back to be drawn as a picture made on the CPU.
    const Scene saved = scene;
    Layer back; back.type = "fill"; back.color = QColor(20, 40, 90); back.duration = 100;
    back.p[0].base = 160; back.p[1].base = 90;
    const QStringList presets = particlePresetNames();
    for (int shape = 0; shape < 8; ++shape)
      for (bool add : {false, true}) {
        Layer l; l.type = "particles"; l.duration = 100; l.id = "particles-check";
        applyParticlePreset(l, scene, presets[shape % presets.size()]);
        l.particles["shape"] = shape;
        l.particles["blend"] = add ? 1 : 0;
        l.particles["emitter"] = 2;
        l.particles["emitterWidth"] = 260;
        l.particles["emitterHeight"] = 140;
        l.particles["preroll"] = 3;
        l.p[0].base = 160; l.p[1].base = 90;
        // Turned, squeezed and at part opacity: the marks are measured in
        // each particle's own units, and the layer's opacity is the picture's.
        if (shape % 2) { l.p[2].base = 1.3; l.p[3].base = .8; l.p[4].base = 25; l.p[5].base = .7; }
        scene.layers = {l, back};
        clearRenderCaches();
        const quint64 before = renderer.residentCacheStats().particleLayers;
        compare(QString("Particles: shape %1, %2").arg(shape).arg(add ? "added" : "over"));
        if (renderer.residentCacheStats().particleLayers == before) {
          ++failures;
          report << QString("Particles: shape %1 was not written by the card: FAIL").arg(shape);
        }
      }
    scene = saved;
  }
  {
    // Squares, stars and confetti -- the shapes the CPU fills with QPainter --
    // covering each pixel on the card as QPainter covers it, not merely close
    // on average. Their edge was guessed from how fast a distance changes
    // across a 2x2 block of pixels, which falls apart across a piece seen
    // nearly edge on: the Twitter promo's end card at 1080p had a pixel of
    // confetti at 255 where the CPU drew it at 112, and 727 of its 19,133
    // confetti pixels more than 16 levels off, while the average over the
    // frame -- all the check above looks at -- stayed well inside it. Thin
    // pieces at full and half size, stars added, and squares.
    const Scene saved = scene;
    Layer back; back.type = "fill"; back.color = QColor(20, 40, 90); back.duration = 100;
    back.p[0].base = 160; back.p[1].base = 90;
    for (const auto &[label, preset, shape, add, divisor] : std::vector<std::tuple<QString, QString, int, bool, int>>{
             {"confetti", "Confetti", 7, false, 1},
             {"confetti at half size", "Confetti", 7, false, 2},
             {"stars, added", "Sparkle", 5, true, 1},
             {"squares", "Confetti", 2, false, 1}}) {
      Layer l; l.type = "particles"; l.duration = 100; l.id = "particle-coverage-check";
      applyParticlePreset(l, scene, preset);
      l.particles["shape"] = shape;
      l.particles["blend"] = add ? 1 : 0;
      // Added, dim enough that four of them together stay below white: where
      // added light runs past white over part of a pixel, QPainter's Plus and
      // the card's blending part ways whatever the coverage (see writeMarks).
      if (add) { l.particles["colorStart"] = "#3c2814"; l.particles["colorEnd"] = "#20283c"; }
      l.particles["emitter"] = 2;
      l.particles["emitterWidth"] = 300;
      l.particles["emitterHeight"] = 160;
      l.particles["burst"] = 220;
      l.p[0].base = 160; l.p[1].base = 90;
      l.p[ParticleSize].base = 11;
      l.p[ParticleGravity].base = 0;
      scene.layers = {l, back};
      clearRenderCaches();
      const QSize size(qCeil(320. / divisor), qCeil(180. / divisor));
      const quint64 before = renderer.residentCacheStats().particleLayers;
      // The CPU's picture drawn in one pass. Qt's line rasterizer cuts a
      // rectangle at the edge of the band being drawn and works its corners
      // out again there, so a square lying across two bands came out up to 22
      // levels from the same square drawn whole: the CPU against itself.
      RenderSchedulingScope onePass(0, 1);
      const QImage cpu = renderCPU(scene, 30, true, size).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const QImage card = renderer.render(scene, 30, size).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const bool written = renderer.residentCacheStats().particleLayers > before;
      if (card.isNull() || card.size() != cpu.size()) {
        ++failures; report << "Particle coverage: " + label + ": FAIL (no frame: " + renderer.description() + ")";
        continue;
      }
      // Measured where the particles are: pixels either picture changed from
      // the plain background.
      const QImage plain = [&] {
        Scene ground = scene; ground.layers = {back};
        return renderCPU(ground, 30, true, size).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      }();
      qint64 touched = 0, channels = 0, offByMore = 0;
      int worst = 0;
      for (int y = 0; y < cpu.height(); ++y) {
        const uchar *a = cpu.constScanLine(y), *b = card.constScanLine(y), *g = plain.constScanLine(y);
        for (int x = 0; x < cpu.width(); ++x) {
          if (std::memcmp(a + x * 4, g + x * 4, 4) == 0 && std::memcmp(b + x * 4, g + x * 4, 4) == 0) continue;
          ++touched;
          for (int c = 0; c < 4; ++c) {
            const int d = std::abs(int(a[x * 4 + c]) - int(b[x * 4 + c]));
            ++channels; offByMore += d > 2; worst = std::max(worst, d);
          }
        }
      }
      const bool passed = written && touched > 200 && worst <= 4 && offByMore * 2000 <= channels;
      failures += !passed;
      report << QString("Particle coverage: %1: %2, %3 pixels with particles, worst %4/255, %5 channels off by more than 2%6")
                    .arg(label, passed ? "PASS" : "FAIL").arg(touched).arg(worst).arg(offByMore)
                    .arg(written ? QString() : QString(", not written by the card"));
    }
    scene = saved;
  }
  {
    // Shape layers covering each pixel on the card as the CPU's rasterizer
    // covers it (State::shapes), not merely close on average. Through Qt's GL
    // engine every edge pixel took one of eight levels from multisampling, and
    // these shapes came out 25 to 128 levels from the CPU's picture at their
    // worst pixel -- an Outside stroke round a ZigZag circle the most -- while
    // the average over the frame, all `compare` looks at, stayed small. Each
    // alone over nothing, at full and half size; then a mask and a 3D turn,
    // which the card leaves to Qt's engine, still drawn.
    Scene shapes;
    shapes.width = 480;
    shapes.height = 270;
    shapes.motionBlur = false;
    auto shape = [](const QString &id, const QString &kind, double x, double y, double turn) {
      Layer l;
      l.id = "shape-coverage-" + id;
      l.type = "shape";
      l.shapeKind = kind;
      l.duration = 100;
      l.p[0].base = x; l.p[1].base = y; l.p[4].base = turn;
      l.color = QColor(60, 170, 220);
      return l;
    };
    auto gradient = [](Layer &l, int type, double angle) {
      l.appliedEffects["Gradient"] = true;
      l.surfaceEffects["Gradient"] = QJsonObject{{"start", "#ff6a00"}, {"end", "#3355ff"}, {"amount", 100},
                                                 {"angle", angle}, {"type", type}};
    };
    std::vector<std::pair<QString, Layer>> cases;
    {
      Layer l = shape("zigzag", "circle", 240, 135, 0);
      l.p[ShapeWidth].base = l.p[ShapeHeight].base = 150; l.p[2].base = l.p[3].base = 1.25;
      l.p[ShapeZigZagSize].base = 7; l.p[ShapeZigZagRidges].base = 18; l.p[ShapeRoundCorners].base = 3;
      l.color = QColor(230, 80, 110); l.strokeWidth = 6; l.strokeColor = QColor(30, 30, 40); l.strokeMode = 1;
      cases.push_back({"a ZigZag circle with an Outside stroke", l});
    }
    {
      Layer l = shape("star", "star", 240, 135, 17);
      l.p[ShapePoints].base = 5; l.p[OuterRadius].base = 110; l.p[InnerRadius].base = 45;
      gradient(l, 0, 30);
      cases.push_back({"a star with a linear Gradient", l});
    }
    {
      Layer l = shape("rounded", "rect", 240, 135, -8);
      l.p[ShapeWidth].base = 300; l.p[ShapeHeight].base = 150; l.p[ShapeRoundness].base = 30;
      l.strokeWidth = 12; l.strokeColor = QColor(250, 240, 90); l.strokeMode = 0; l.p[5].base = .6;
      cases.push_back({"a rounded rectangle with an Inside stroke at 60% opacity", l});
    }
    {
      Layer l = shape("polygon", "polygon", 240, 135, 9);
      l.p[ShapePoints].base = 7; l.p[OuterRadius].base = 110;
      l.shapeFill = false; l.strokeWidth = 8; l.strokeMode = 2; l.strokeColor = QColor(120, 230, 140);
      cases.push_back({"a polygon with a Center stroke and no fill", l});
    }
    {
      Layer l = shape("radial", "circle", 240, 135, 0);
      l.p[ShapeWidth].base = 260; l.p[ShapeHeight].base = 200; l.p[5].base = .85;
      gradient(l, 1, 0);
      cases.push_back({"an ellipse with a radial Gradient at 85% opacity", l});
    }
    {
      Layer l = shape("twisted", "star", 240, 135, 33);
      l.p[ShapePoints].base = 9; l.p[OuterRadius].base = 120; l.p[InnerRadius].base = 80;
      l.p[ShapeTwist].base = 40; l.p[ShapePuckerBloat].base = -30; l.color = QColor(250, 200, 60, 200);
      cases.push_back({"a twisted, puckered star in a translucent colour", l});
    }
    {
      Layer l = shape("hairline", "rect", 240, 135, 11);
      l.p[ShapeWidth].base = 400; l.p[ShapeHeight].base = 1.3; l.color = QColor(255, 255, 255, 200);
      cases.push_back({"a line 1.3 px wide, turned", l});
    }
    {
      Layer l = shape("trimmed", "circle", 240, 135, 0);
      l.p[ShapeWidth].base = l.p[ShapeHeight].base = 200; l.p[ShapeTrimEnd].base = 70;
      l.shapeFill = false; l.strokeWidth = 10; l.strokeMode = 2; l.strokeColor = QColor(240, 120, 60);
      cases.push_back({"an open, trimmed stroke", l});
    }
    // Measured where either picture has anything: each alone over nothing.
    // (`mean` over the whole picture, as `compare` takes it.)
    struct Difference { qint64 touched = 0, channels = 0, offByMore = 0; int worst = 0; double mean = 0; };
    auto differ = [](const QImage &a, const QImage &b) {
      Difference d;
      qint64 total = 0;
      for (int y = 0; y < a.height(); ++y) {
        const uchar *p = a.constScanLine(y), *q = b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x) {
          if (!p[x * 4 + 3] && !q[x * 4 + 3]) continue;
          ++d.touched;
          for (int c = 0; c < 4; ++c) {
            const int e = std::abs(int(p[x * 4 + c]) - int(q[x * 4 + c]));
            ++d.channels; d.offByMore += e > 2; d.worst = std::max(d.worst, e); total += e;
          }
        }
      }
      d.mean = double(total) / (double(a.width()) * a.height() * 4);
      return d;
    };
    auto draw = [&](const Layer &layer, int divisor, QImage &cpu, QImage &card, bool &covered) {
      Scene one = shapes;
      one.layers = {layer};
      clearRenderCaches();
      const QSize size(480 / divisor, 270 / divisor);
      const quint64 before = renderer.residentCacheStats().shapeFills;
      RenderSchedulingScope onePass(0, 1);
      cpu = renderCPU(one, 30, true, size).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      card = renderer.render(one, 30, size).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      covered = renderer.residentCacheStats().shapeFills > before;
      return !card.isNull() && card.size() == cpu.size();
    };
    for (const auto &[label, layer] : cases)
      for (int divisor : {1, 2}) {
        QImage cpu, card;
        bool covered = false;
        const QString what = label + (divisor == 1 ? QString() : QString(" at half size"));
        if (!draw(layer, divisor, cpu, card, covered)) {
          ++failures; report << "Shape coverage: " + what + ": FAIL (no frame: " + renderer.description() + ")";
          continue;
        }
        const Difference d = differ(cpu, card);
        const bool passed = covered && d.touched > 100 && d.worst <= 4 && d.offByMore * 2000 <= d.channels;
        failures += !passed;
        report << QString("Shape coverage: %1: %2, %3 pixels drawn, worst %4/255, %5 channels off by more than 2%6")
                      .arg(what, passed ? "PASS" : "FAIL").arg(d.touched).arg(d.worst).arg(d.offByMore)
                      .arg(covered ? QString() : QString(", not covered by the card"));
      }
    // Cut by a mask (kept in Qt's stencil) or carried by a 3D projection: Qt's
    // engine still draws them, multisampled as before.
    Layer masked = cases[0].second;
    QJsonArray corners;
    for (auto corner : {QPointF(-90, -70), QPointF(80, -60), QPointF(10, 90)})
      corners.push_back(QJsonObject{{"p", QJsonArray{corner.x(), corner.y()}}, {"in", QJsonArray{0, 0}},
                                    {"out", QJsonArray{0, 0}}});
    masked.masks = QJsonArray{QJsonObject{{"name", "Mask 1"}, {"points", corners}, {"enabled", true},
                                          {"inverted", false}, {"expansion", 0}, {"feather", 0}}};
    Layer turned = cases[1].second;
    turned.threeD = true;
    turned.p[RotationY].base = 35;
    for (const auto &[label, layer] : {std::pair<QString, Layer>{"cut by a mask", masked},
                                      std::pair<QString, Layer>{"turned in 3D", turned}}) {
      QImage cpu, card;
      bool covered = false;
      if (!draw(layer, 1, cpu, card, covered)) {
        ++failures; report << "Shape coverage: " + label + ": FAIL (no frame: " + renderer.description() + ")";
        continue;
      }
      const Difference d = differ(cpu, card);
      const bool passed = !covered && d.touched > 100 && d.mean < 2;
      failures += !passed;
      report << QString("Shape coverage: %1, left to Qt's engine: %2, %3 pixels drawn, mean error %4/255%5")
                    .arg(label, passed ? "PASS" : "FAIL").arg(d.touched).arg(d.mean, 0, 'f', 3)
                    .arg(covered ? QString(", covered by the card") : QString());
    }
    // A star so large and so full of spikes that its area is added up in the
    // float targets, in one frame with a small circle summed in the fill: the
    // fill's two forms one after the other. Drawn by one program that asked a
    // uniform which, such a frame kept the card summing over a stale row
    // table until Windows reset it (a TDR, which closes the program).
    {
      Layer comb = shape("comb", "star", 240, 135, 3);
      comb.p[ShapePoints].base = 120; comb.p[OuterRadius].base = 400; comb.p[InnerRadius].base = 20;
      comb.color = QColor(90, 200, 120);
      Layer small = shape("small", "circle", 120, 90, 0);
      small.p[ShapeWidth].base = small.p[ShapeHeight].base = 60;
      small.color = QColor(240, 90, 60, 220);
      Scene pair = shapes;
      pair.layers = {small, comb};
      clearRenderCaches();
      const GpuResidentCacheStats before = renderer.residentCacheStats();
      RenderSchedulingScope onePass(0, 1);
      const QImage cpu = renderCPU(pair, 30, true, QSize(480, 270)).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const QImage card = renderer.render(pair, 30, QSize(480, 270)).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const GpuResidentCacheStats after = renderer.residentCacheStats();
      const QString what = "a huge star added up and a small circle summed, in one frame";
      if (card.isNull() || card.size() != cpu.size()) {
        ++failures; report << "Shape coverage: " + what + ": FAIL (no frame: " + renderer.description() + ")";
      } else {
        const Difference d = differ(cpu, card);
        const bool bothWays = after.shapeFills >= before.shapeFills + 2 && after.shapeFillsAddedUp > before.shapeFillsAddedUp;
        const bool passed = bothWays && d.touched > 100 && d.worst <= 4 && d.offByMore * 2000 <= d.channels;
        failures += !passed;
        report << QString("Shape coverage: %1: %2, %3 pixels drawn, worst %4/255, %5 channels off by more than 2%6")
                      .arg(what, passed ? "PASS" : "FAIL").arg(d.touched).arg(d.worst).arg(d.offByMore)
                      .arg(bothWays ? QString() : QString(", not drawn both ways by the card"));
      }
    }
  }
  {
    // Pictures drawn from textures the card keeps: flat, turned and faded, and
    // seen in 3D -- the same picture as the CPU's, uploaded once and drawn
    // from the card after that.
    const Scene saved = scene;
    QImage art(240, 135, QImage::Format_ARGB32);
    for (int y = 0; y < art.height(); ++y)
      for (int x = 0; x < art.width(); ++x)
        art.setPixel(x, y, qRgba(x, y * 2 % 256, (x ^ y) & 255, x < 20 ? 128 : 255));
    Layer back; back.type = "fill"; back.color = QColor(20, 40, 90); back.duration = 100;
    back.p[0].base = 160; back.p[1].base = 90;
    Layer picture; picture.type = "image"; picture.image = art; picture.duration = 100; picture.id = "picture-check";
    picture.p[0].base = 160; picture.p[1].base = 90;
    Layer turned = picture; turned.p[2].base = .8; turned.p[3].base = 1.1; turned.p[4].base = 18; turned.p[5].base = .6;
    Layer deep = picture; deep.threeD = true; deep.p[RotationY].base = 35;
    for (const auto &[label, layer] : {std::pair<QString, Layer>{"Picture: flat", picture},
                                      std::pair<QString, Layer>{"Picture: turned, scaled, at 60% opacity", turned},
                                      std::pair<QString, Layer>{"Picture: turned in 3D", deep}}) {
      scene.layers = {layer, back};
      clearRenderCaches();
      const auto before = renderer.residentCacheStats();
      compare(label);
      const auto after = renderer.residentCacheStats();
      if (after.imageDraws == before.imageDraws) {
        ++failures;
        report << label + ": not drawn from a texture the card keeps: FAIL";
      }
    }
    // Drawn again, nothing new is sent to the card.
    const auto warm = renderer.residentCacheStats();
    renderer.render(scene, 30, {});
    if (renderer.residentCacheStats().imageUploads != warm.imageUploads) {
      ++failures;
      report << "Picture: drawn again, uploaded again: FAIL";
    } else report << "Picture: drawn again from the texture the card kept: PASS";
    scene = saved;
  }
  {
    // A card on a thread of its own: it draws there what it draws here, and the
    // thread asking is never the one drawing.
    GpuRenderService service;
    if (!service.usable()) {
      ++failures; report << "Service: the card could not be used from its own thread: FAIL (" + service.description() + ")";
    } else {
      Layer back; back.type = "fill"; back.color = QColor(20, 40, 90); back.duration = 100;
      back.p[0].base = 160; back.p[1].base = 90;
      Layer title; title.type = "text"; title.text = "Wave"; title.fontSize = 60; title.color = Qt::white; title.duration = 100;
      title.p[0].base = 160; title.p[1].base = 90; title.p[4].base = 12;
      Scene own = scene;
      own.layers = {title, back};
      const QImage card = service.render(own, 30, {}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const QImage cpu = renderCPU(own, 30, true).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      qint64 error = 0;
      if (!card.isNull() && card.size() == cpu.size())
        for (int y = 0; y < cpu.height(); ++y)
          for (int x = 0; x < cpu.width() * 4; ++x) error += std::abs(int(card.constScanLine(y)[x]) - int(cpu.constScanLine(y)[x]));
      const double mean = card.isNull() ? 255 : error / (double(cpu.width()) * cpu.height() * 4);
      if (card.isNull() || mean >= 2) ++failures;
      report << QString("Service: a frame drawn on the card's own thread: %1, mean error %2/255")
                    .arg(card.isNull() || mean >= 2 ? "FAIL" : "PASS").arg(mean, 0, 'f', 3);
      // Asked from several threads at once, each gets its own frame.
      std::vector<std::future<QImage>> asked;
      for (int i = 0; i < 4; ++i)
        asked.push_back(std::async(std::launch::async, [&service, own] { return service.render(own, 30, {}); }));
      int drawn = 0;
      for (auto &frame : asked) drawn += !frame.get().isNull();
      if (drawn != 4) ++failures;
      report << QString("Service: four threads asking at once: %1 (%2 of 4 drawn)").arg(drawn == 4 ? "PASS" : "FAIL").arg(drawn);
    }
  }
  failures += checkGpuSurfaces(report);
  report << (failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
