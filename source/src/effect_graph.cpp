#include "effect_graph.h"
#include "gpu_pixels.h"
#include "render_workers.h"
#include <QElapsedTimer>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
namespace {
struct Plan { std::vector<WavyOperation> ops; bool invalid=false; };
int append(void *context,const WavyOperation *op) {
  auto &p=*static_cast<Plan *>(context);
  if(!op || op->structSize!=sizeof(WavyOperation) || p.ops.size()>=256 ||
     op->code<WAVY_RGB_AFFINE || op->code>WAVY_GRADIENT){p.invalid=true;return 1;}
  for(float v:op->values)if(!std::isfinite(v)){p.invalid=true;return 1;}
  if((op->code==WAVY_EXPOSURE && std::abs(op->values[0])>32) ||
     (op->code==WAVY_INVERT && (op->values[0]<0 || op->values[0]>1))){p.invalid=true;return 1;}
  auto unit=[](float v){return v>=0 && v<=1;};const auto *v=op->values;
  bool valid=true;
  if(op->code==WAVY_BLUR || op->code==WAVY_HALO)valid=v[0]>=0 && v[0]<=8192;
  if(op->code==WAVY_HALO)valid=valid && unit(v[1])&&unit(v[2])&&unit(v[3])&&unit(v[4])&&
      std::abs(v[5])<=8192 && std::abs(v[6])<=8192 && std::floor(v[5])==v[5] && std::floor(v[6])==v[6];
  if(op->code==WAVY_STROKE)valid=v[0]>=0&&v[0]<=200&&v[1]>=0&&v[1]<=2&&std::floor(v[1])==v[1]&&unit(v[2])&&unit(v[3])&&unit(v[4])&&unit(v[5]);
  if(op->code==WAVY_FILL)for(int i=0;i<4;++i)valid=valid&&unit(v[i]);
  if(op->code==WAVY_GRADIENT){for(int i=0;i<7;++i)valid=valid&&unit(v[i]);
    valid=valid&&(v[8]==0||v[8]==1)&&v[11]>0&&v[12]>0&&v[11]<=1e7&&v[12]<=1e7&&std::abs(v[7])<=360&&std::abs(v[9])<=1e7&&std::abs(v[10])<=1e7;}
  if(!valid){p.invalid=true;return 1;}
  p.ops.push_back(*op);return 0;
}
}
int executeEffectGraph(const WavyEffectGraphV3 *api,QImage &image,bool gpu,float amount) {
  if(!api || api->structSize!=sizeof(*api) || api->abiVersion!=3 || !api->describe || image.isNull())return 1;
  Plan plan;
  WavyGraphRequest request{sizeof(WavyGraphRequest),3,image.width(),image.height(),image.depth()>64?32:image.depth()>32?16:8,amount};
  WavyGraphSink sink{sizeof(WavyGraphSink),&plan,append};
  try {if(api->describe(&request,&sink) || plan.invalid)return 1;}catch(...){return 1;}
  std::vector<WavyOperation> optimized;
  for(const auto &op:plan.ops){
    if(op.code<=WAVY_INVERT){
      WavyOperation affine{};affine.structSize=sizeof(affine);affine.code=WAVY_RGB_AFFINE;
      for(int c=0;c<3;++c){affine.values[c]=op.code==WAVY_RGB_AFFINE?op.values[c]:op.code==WAVY_EXPOSURE?std::exp2(op.values[0]):1-2*op.values[0];
        affine.values[c+3]=op.code==WAVY_RGB_AFFINE?op.values[c+3]:op.code==WAVY_INVERT?op.values[0]:0;}
      if(!optimized.empty() && optimized.back().code==WAVY_RGB_AFFINE){auto &last=optimized.back();
        for(int c=0;c<3;++c){last.values[c]*=affine.values[c];last.values[c+3]=last.values[c+3]*affine.values[c]+affine.values[c+3];
          if(!std::isfinite(last.values[c])||!std::isfinite(last.values[c+3]))return 1;}}
      else optimized.push_back(affine);
    }else optimized.push_back(op);
  }
  plan.ops=std::move(optimized);
  bool spatial=false;for(const auto &op:plan.ops)spatial|=op.code>=WAVY_BLUR;
  if(spatial) {
    const auto format=image.format();QImage result=image.convertToFormat(QImage::Format_RGBA32FPx4);
    QVector<GpuPass> chain;bool gpuSupported=true,premul=false;
    auto want=[&](bool on){if(premul!=on){chain+=gpuAlphaPass(on);premul=on;}};
    auto blur=[&](float radius){int r=int(std::ceil(radius/2));if(r>32)gpuSupported=false;
      for(int n=0;n<6;++n){GpuPass p;p.op=2;p.params={float(r),n%2?0.f:1.f,n%2?1.f:0.f,0};
        p.input=n?GpuPass::Previous:GpuPass::Current;p.size=image.size();chain<<p;}};
    for(const auto &op:plan.ops){const auto *v=op.values;GpuPass p;p.endsEffect=true;
      if(op.code<=WAVY_INVERT){want(false);p.op=15;
        for(int c=0;c<3;++c){p.a[c]=op.code==WAVY_RGB_AFFINE?v[c]:op.code==WAVY_EXPOSURE?std::exp2(v[0]):1-2*v[0];p.b[c]=op.code==WAVY_RGB_AFFINE?v[c+3]:op.code==WAVY_INVERT?v[0]:0;}chain<<p;
      } else if(op.code==WAVY_FILL){want(false);p.op=14;p.a={v[0],v[1],v[2],v[3]};chain<<p;}
      else if(op.code==WAVY_GRADIENT){want(false);p.op=1;p.params={v[9],v[10],v[7],v[5]};p.a={v[0],v[1],v[2],v[8]?-v[6]:v[6]};p.b={v[3],v[4],v[11],v[12]};chain<<p;}
      else if(op.code==WAVY_STROKE){gpuSupported=false; /* exact CPU Euclidean contour, no approximate JFA */}
      else if(op.code==WAVY_BLUR){if(v[0]>0){want(true);blur(v[0]);chain.back().endsEffect=true;}}
      else if(op.code==WAVY_HALO){want(true);blur(v[0]);p.op=3;p.params={v[5],v[6],0,0};p.a={v[1],v[2],v[3],v[4]};p.useAux=true;p.auxFrom=GpuPass::Previous;chain<<p;}
    }
    want(false);
    if(gpu && gpuSupported && !chain.isEmpty()){
      auto out=gpuRunPasses(result,chain);if(!out.isNull()){out=out.convertToFormat(format);if(out.isNull())return 1;image=out;return 0;}
    }
    CpuPixelScope cpuOnly;
    try {for(const auto &op:plan.ops){const auto *v=op.values;
      if(op.code==WAVY_BLUR){result=fullResolutionBlur(result,v[0]);continue;}
      if(op.code==WAVY_HALO){
        result=result.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
        auto halo=fullResolutionBlur(result,v[0]);const auto *hb=halo.constBits();const auto hs=halo.bytesPerLine();
        auto *bits=result.bits();const auto stride=result.bytesPerLine();
        renderParallel(result.height(),[&](int begin,int end){for(int y=begin;y<end;++y){auto *p=reinterpret_cast<float *>(bits+y*stride);
          for(int x=0;x<result.width();++x){int sx=x-int(v[5]),sy=y-int(v[6]);
            float a=sx>=0&&sy>=0&&sx<halo.width()&&sy<halo.height()?reinterpret_cast<const float *>(hb+sy*hs)[sx*4+3]*v[4]:0;
            a*=1-p[x*4+3];for(int c=0;c<3;++c)p[x*4+c]+=v[c+1]*a;p[x*4+3]+=a;}
        }});continue;
      }
      result=result.convertToFormat(QImage::Format_RGBA32FPx4);
      if(op.code==WAVY_STROKE){Layer l;l.type="image";l.surfaceEffects["Stroke"]=QJsonObject{{"width",v[0]},{"mode",int(v[1])},{"color",QColor::fromRgbF(v[2],v[3],v[4],v[5]).name(QColor::HexArgb)}};strokePass(result,l,1);continue;}
      const double angle=v[7]*3.141592653589793/180,dxAxis=std::cos(angle),dyAxis=std::sin(angle);
      const double gradientSpan=std::max(1.,std::abs(dxAxis)*v[11]+std::abs(dyAxis)*v[12]);
      const double radialExtent=std::max(1.,double(std::max(v[11],v[12]))*.5);
      float pointGain[3],pointBias[3];
      for(int c=0;c<3;++c){pointGain[c]=op.code==WAVY_RGB_AFFINE?v[c]:op.code==WAVY_EXPOSURE?std::exp2(v[0]):1-2*v[0];pointBias[c]=op.code==WAVY_RGB_AFFINE?v[c+3]:op.code==WAVY_INVERT?v[0]:0;}
      auto *bits=result.bits();const auto stride=result.bytesPerLine();
      renderParallel(result.height(),[&](int begin,int end){for(int y=begin;y<end;++y){auto *p=reinterpret_cast<float *>(bits+y*stride);
        for(int x=0;x<result.width();++x){
          if(op.code==WAVY_FILL){for(int c=0;c<3;++c)p[x*4+c]=v[c];p[x*4+3]*=v[3];}
          else if(op.code==WAVY_GRADIENT){double dx=x+.5-v[9],dy=y+.5-v[10];
            double t=v[8]?std::hypot(dx,dy)/radialExtent:.5+(dx*dxAxis+dy*dyAxis)/gradientSpan;t=std::clamp(t,0.,1.);
            for(int c=0;c<3;++c)p[x*4+c]=float(p[x*4+c]*(1-v[6])+(v[c]*(1-t)+v[c+3]*t)*v[6]);}
          else for(int c=0;c<3;++c)p[x*4+c]=p[x*4+c]*pointGain[c]+pointBias[c];
        }
      }});
    }}catch(...){return 1;}
    result=result.convertToFormat(format);if(result.isNull())return 1;image=result;return 0;
  }
  // Fuse the ordered point operations into one pass, without intermediate
  // clamping or images. This is identical on CPU/GPU at every source depth.
  float gain[3]={1,1,1},bias[3]={0,0,0};
  for(const auto &op:plan.ops)for(int c=0;c<3;++c){
    float a=1,b=0;
    if(op.code==WAVY_RGB_AFFINE){a=op.values[c];b=op.values[c+3];}
    else if(op.code==WAVY_EXPOSURE)a=std::exp2(op.values[0]);
    else {a=1-2*op.values[0];b=op.values[0];}
    gain[c]*=a;bias[c]=bias[c]*a+b;
    if(!std::isfinite(gain[c])||!std::isfinite(bias[c]))return 1;
  }
  if(plan.ops.empty())return 0;
  if(gain[0]==1 && gain[1]==1 && gain[2]==1 && bias[0]==0 && bias[1]==0 && bias[2]==0)return 0;
  const auto format=image.format();
  QImage result=image.convertToFormat(QImage::Format_RGBA32FPx4);
  if(result.isNull())return 1;
  if(gpu){GpuPass pass;pass.op=15;pass.a={gain[0],gain[1],gain[2],0};pass.b={bias[0],bias[1],bias[2],0};pass.endsEffect=true;
    auto accelerated=gpuRunPasses(result,{pass});
    if(!accelerated.isNull()){auto converted=accelerated.convertToFormat(format);if(converted.isNull())return 1;image=converted;return 0;}}
  auto *bits=result.bits();auto stride=result.bytesPerLine();
  renderParallel(result.height(),[&](int begin,int end){for(int y=begin;y<end;++y){
    auto *p=reinterpret_cast<float *>(bits+y*stride);
    for(int x=0;x<result.width();++x)for(int c=0;c<3;++c)p[x*4+c]=p[x*4+c]*gain[c]+bias[c];
  }});
  result=result.convertToFormat(format);if(result.isNull())return 1;image=result;return 0;
}
int runPluginAcrossCores(QImage &image, bool reentrant, int rowReach,
                         const PluginPixelCall &call, double *cost) {
  if (image.isNull() || image.width() < 1 || image.height() < 1 || !call) return 1;
  const int height = image.height(), width = image.width(), bpp = image.depth() / 8;
  const qsizetype stride = image.bytesPerLine();
  // Detached once, here, before any worker exists: QImage::bits() and the
  // non-const scanLine() both detach, and two threads detaching the same image
  // is a race on its reference count rather than a picture.
  uchar *const base = image.bits();
  const double pixels = double(width) * height;
  auto whole = [&] {
    QElapsedTimer clock;
    clock.start();
    const int result = call(base, width, height, int(stride));
    // Only a whole run is worth remembering. A banded one is already faster by
    // however many cores it used, so timing it would teach the wrong lesson.
    if (cost && pixels > 0) *cost = clock.nsecsElapsed() / pixels;
    return result;
  };
  if (!reentrant || rowReach < 0 || inRenderWorker) return whole();
  // Splitting costs a fraction of a millisecond in dispatch, which a cheap
  // effect never earns back: inverting a 1080p frame takes about a millisecond
  // on one core and came out slower on twenty. So the first run is whole and
  // timed, and only an effect that proved expensive is spread after that.
  if (!cost || *cost <= 0 || *cost * pixels < 4e6) return whole();
  // A band has to be worth a thread of its own, and worth the rows it borrows
  // from its neighbours. At four times the reach the borrowing is a quarter of
  // the work rather than all of it.
  const int least = std::max(64, 4 * rowReach);
  const int bands = std::min(std::max(1, QThread::idealThreadCount()), height / least);
  if (bands <= 1) return whole();
  // A plugin that reads its neighbours must read them as they were, not as
  // another band has already left them, so those bands read from an untouched
  // copy and write back only the rows they own. That is what keeps a seam from
  // appearing at every boundary.
  const QImage source = rowReach > 0 ? image.copy() : QImage();
  if (rowReach > 0 && source.isNull()) return whole();
  std::atomic<int> failure{0};
  std::vector<std::future<void>> jobs;
  jobs.reserve(size_t(bands));
  for (int band = 0; band < bands; ++band) {
    const int begin = height * band / bands, end = height * (band + 1) / bands;
    if (begin >= end) continue;
    jobs.push_back(renderJob([&, begin, end] {
      if (rowReach == 0) {
        // Each band owns its rows outright, so it is handed the real buffer:
        // nothing copied in, nothing copied out, nothing allocated.
        if (const int result = call(base + qsizetype(begin) * stride, width, end - begin,
                                    int(stride)))
          failure.store(result);
        return;
      }
      const int top = std::max(0, begin - rowReach), bottom = std::min(height, end + rowReach);
      QImage window = source.copy(0, top, width, bottom - top);
      if (window.isNull()) { failure.store(1); return; }
      if (const int result = call(window.bits(), width, bottom - top, int(window.bytesPerLine()))) {
        failure.store(result);
        return;
      }
      for (int y = begin; y < end; ++y)
        memcpy(base + qsizetype(y) * stride, window.constScanLine(y - top), size_t(width) * bpp);
    }));
  }
  for (auto &job : jobs) {
    try {
      job.get();
    } catch (...) {
      failure.store(1);
    }
  }
  return failure.load();
}

int checkEffectGraph(QStringList &failures) {
  {
    WavyEffectGraphV3 spatial{sizeof(WavyEffectGraphV3),3,[](const WavyGraphRequest *,const WavyGraphSink *sink){
      WavyOperation op{};op.structSize=sizeof(op);op.code=WAVY_FILL;op.values[0]=1;op.values[3]=1;
      if(sink->append(sink->context,&op))return 1;
      op={};op.structSize=sizeof(op);op.code=WAVY_HALO;op.values[0]=4;op.values[2]=1;op.values[4]=.75f;
      return sink->append(sink->context,&op);
    }};
    for(auto format:{QImage::Format_RGBA8888,QImage::Format_RGBA64,QImage::Format_RGBA32FPx4}){
      QImage src(41,39,format);src.fill(Qt::transparent);src.setPixelColor(20,19,Qt::white);
      if(executeEffectGraph(&spatial,src,false) || src.format()!=format || src.pixelColor(20,19).red()<250 || src.pixelColor(21,19).alphaF()<=0)
        failures<<"Fill -> Halo supports 8/16/32 bpc and expands alpha";
    }
    spatial.describe=[](const WavyGraphRequest *,const WavyGraphSink *sink){
      WavyOperation op{};op.structSize=sizeof(op);op.code=WAVY_BLUR;op.values[0]=80;return sink->append(sink->context,&op);
    };
    QImage src(67,65,QImage::Format_RGBA32FPx4);src.fill(Qt::transparent);src.setPixelColor(33,32,Qt::white);
    auto expected=fullResolutionBlur(src,80).convertToFormat(src.format());auto result=src;
    if(executeEffectGraph(&spatial,result,false) || result!=expected)failures<<"wide SDK blur must not downscale";
    spatial.describe=[](const WavyGraphRequest *,const WavyGraphSink *sink){
      WavyOperation op{};op.structSize=sizeof(op);op.code=WAVY_INVERT;
      for(int i=0;i<257;++i)sink->append(sink->context,&op);return 0;
    };
    if(!executeEffectGraph(&spatial,src,false) || src.pixelColor(33,32)!=Qt::white)failures<<"graph operation limit";
    spatial.describe=[](const WavyGraphRequest *request,const WavyGraphSink *sink){
      WavyOperation op{};op.structSize=sizeof(op);op.code=WAVY_STROKE;op.values[0]=3;op.values[1]=1;op.values[2]=1;op.values[5]=1;
      if(sink->append(sink->context,&op))return 1;
      op={};op.structSize=sizeof(op);op.code=WAVY_GRADIENT;
      op.values[1]=op.values[4]=1;op.values[6]=1;
      op.values[9]=request->width/2.f;op.values[10]=request->height/2.f;
      op.values[11]=float(request->width);op.values[12]=float(request->height);
      return sink->append(sink->context,&op);
    };
    src.fill(Qt::transparent);src.setPixelColor(33,32,Qt::white);
    if(executeEffectGraph(&spatial,src,false) || src.pixelColor(34,32).green()<250 || src.pixelColor(34,32).red()!=0)
      failures<<"Stroke -> Gradient processes expanded coverage in order";
    auto unchanged=src;
    spatial.describe=[](const WavyGraphRequest *,const WavyGraphSink *sink){
      WavyOperation op{};op.structSize=sizeof(op);op.code=WAVY_BLUR;op.values[0]=-1;sink->append(sink->context,&op);return 0;
    };
    if(!executeEffectGraph(&spatial,src,false)||src!=unchanged)failures<<"negative radius rejection is transactional";
  }
  WavyEffectGraphV3 api{sizeof(WavyEffectGraphV3),3,[](const WavyGraphRequest *,const WavyGraphSink *sink){
    WavyOperation a{};a.structSize=sizeof(a);a.code=WAVY_EXPOSURE;a.values[0]=1;
    if(sink->append(sink->context,&a))return 1;
    a.code=WAVY_INVERT;a.values[0]=1;return sink->append(sink->context,&a);
  }};
  QImage image(67,65,QImage::Format_RGBA32FPx4);image.fill(Qt::transparent);
  auto *p=reinterpret_cast<float *>(image.scanLine(4))+12;p[0]=2;p[1]=.25f;p[3]=.5f;
  if(executeEffectGraph(&api,image,false))failures<<"graph execution";
  p=reinterpret_cast<float *>(image.scanLine(4))+12;
  if(p[0]!=-3 || p[1]!=.5f || p[3]!=.5f)failures<<"ordered fusion HDR/alpha";
  auto original=image;
  api.describe=[](const WavyGraphRequest *,const WavyGraphSink *sink){WavyOperation op{};op.structSize=sizeof(op);op.code=999;sink->append(sink->context,&op);return 0;};
  if(!executeEffectGraph(&api,image,false)||image!=original)failures<<"invalid graph must not modify input";

  // Spreading a plugin across cores without the plugin knowing. The promise is
  // that the picture does not depend on how many bands it was cut into -- a
  // promise worth testing rather than assuming, because the way it fails is a
  // faint line across the frame at every boundary.
  {
    auto canvas = [] {
      QImage source(97, 800, QImage::Format_RGBA8888);
      quint32 state = 0x9e3779b9;
      for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<quint32 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
          state = state * 1664525u + 1013904223u;
          row[x] = state | 0xff000000u;
        }
      }
      return source;
    };
    std::atomic<int> calls{0};
    std::atomic<int> tallest{0};
    // Pointwise: every output pixel depends on the input pixel in its own place
    // and nothing else, which is most colour work.
    PluginPixelCall invert = [&](void *pixels, int w, int h, int stride) {
      ++calls;
      int was = tallest.load();
      while (h > was && !tallest.compare_exchange_weak(was, h)) {}
      for (int y = 0; y < h; ++y) {
        auto *row = static_cast<uint8_t *>(pixels) + qsizetype(y) * stride;
        for (int x = 0; x < w; ++x)
          for (int c = 0; c < 3; ++c) row[x * 4 + c] = uint8_t(255 - row[x * 4 + c]);
      }
      return 0;
    };
    // Expensive enough to be worth spreading, stated rather than timed so the
    // test is about the decision and not about how busy this machine is.
    // A whole run measures itself and writes what it found, which would wash
    // the injected number away, so the reference runs get their own.
    double dear = 1000, cheap = 1, unknown = 0, reference = 0;
    QImage alone = canvas(), spread = canvas();
    calls = 0;
    runPluginAcrossCores(alone, false, 0, invert, &reference);
    const int aloneCalls = calls.load(), aloneTallest = tallest.load();
    calls = 0;
    tallest = 0;
    runPluginAcrossCores(spread, true, 0, invert, &dear);
    if (aloneCalls != 1 || aloneTallest != alone.height())
      failures << "a plugin that declares nothing is called once with the whole image";
    if (QThread::idealThreadCount() > 1 && calls.load() < 2)
      failures << QString("and one that declares itself reentrant is cut into bands, got %1")
                      .arg(calls.load());
    if (spread != alone) failures << "cutting a pointwise plugin into bands changes no pixel";

    // Threading is not free, and an effect that costs less than the dispatch
    // must not be spread: inverting a frame on twenty cores measured slower
    // than inverting it on one.
    QImage trivial = canvas();
    calls = 0;
    runPluginAcrossCores(trivial, true, 0, invert, &cheap);
    if (calls.load() != 1)
      failures << QString("an effect too cheap to be worth a thread is left whole, got %1 calls")
                      .arg(calls.load());
    // And until it has been seen once, nothing is known, so it runs whole and
    // is timed. A first frame slightly slower buys every later one being right.
    QImage first = canvas();
    calls = 0;
    runPluginAcrossCores(first, true, 0, invert, &unknown);
    if (calls.load() != 1) failures << "a plugin nobody has timed yet runs whole";
    if (unknown <= 0) failures << "and is timed while it does, so the next frame knows";

    // A plugin that reads the rows around the one it writes. It takes its own
    // copy first, which is the obligation that comes with declaring a reach.
    const int reach = 6;
    PluginPixelCall blur = [&](void *pixels, int w, int h, int stride) {
      ++calls;
      QImage read(w, h, QImage::Format_RGBA8888);
      for (int y = 0; y < h; ++y)
        memcpy(read.scanLine(y), static_cast<uint8_t *>(pixels) + qsizetype(y) * stride,
               size_t(w) * 4);
      for (int y = 0; y < h; ++y) {
        auto *out = static_cast<uint8_t *>(pixels) + qsizetype(y) * stride;
        for (int x = 0; x < w; ++x)
          for (int c = 0; c < 4; ++c) {
            int total = 0, taken = 0;
            for (int dy = -reach; dy <= reach; ++dy) {
              const int at = y + dy;
              if (at < 0 || at >= h) continue;
              total += read.constScanLine(at)[x * 4 + c];
              ++taken;
            }
            out[x * 4 + c] = uint8_t(total / std::max(1, taken));
          }
      }
      return 0;
    };
    QImage single = canvas(), banded = canvas();
    runPluginAcrossCores(single, false, reach, blur, &reference);
    calls = 0;
    runPluginAcrossCores(banded, true, reach, blur, &dear);
    if (QThread::idealThreadCount() > 1 && calls.load() < 2)
      failures << "a plugin with a reach is still cut into bands";
    if (banded != single) {
      int worst = 0, seams = 0;
      for (int y = 0; y < single.height(); ++y)
        for (int x = 0; x < single.width() * 4; ++x) {
          const int gap = std::abs(int(single.constScanLine(y)[x]) - int(banded.constScanLine(y)[x]));
          if (gap) ++seams;
          worst = std::max(worst, gap);
        }
      failures << QString("bands borrow the rows around them, so no seam appears: %1 bytes "
                          "differ, worst %2")
                      .arg(seams)
                      .arg(worst);
    }
    // Failure has to come back, not be swallowed by the threading.
    QImage refused = canvas();
    PluginPixelCall broken = [](void *, int, int, int) { return 7; };
    if (runPluginAcrossCores(refused, true, 0, broken, &dear) != 7)
      failures << "a plugin that refuses a band refuses the frame";
    if (runPluginAcrossCores(refused, false, 0, broken, &reference) != 7)
      failures << "and so does one that was never split";
  }
  return failures.isEmpty()?0:1;
}
int checkEffectGraphGpu(QStringList &failures) {
  WavyEffectGraphV3 api{sizeof(WavyEffectGraphV3),3,[](const WavyGraphRequest *,const WavyGraphSink *sink){
    WavyOperation op{};op.structSize=sizeof(op);op.code=WAVY_EXPOSURE;op.values[0]=1;
    if(sink->append(sink->context,&op))return 1;
    op={};op.structSize=sizeof(op);op.code=WAVY_HALO;op.values[0]=4;op.values[1]=.25f;op.values[2]=.5f;op.values[3]=1;op.values[4]=.8f;op.values[5]=1;op.values[6]=-2;
    if(sink->append(sink->context,&op))return 1;
    op={};op.structSize=sizeof(op);op.code=WAVY_BLUR;op.values[0]=2;return sink->append(sink->context,&op);
  }};
  QImage source(67,65,QImage::Format_RGBA32FPx4);source.fill(Qt::transparent);
  for(int y=12;y<50;++y){auto *p=reinterpret_cast<float *>(source.scanLine(y));for(int x=9;x<48;++x){p[x*4]=1.5f;p[x*4+1]=.25f;p[x*4+3]=.7f;}}
  auto cpu=source,gpu=source;const auto before=gpuPixelOperations();
  if(executeEffectGraph(&api,cpu,false)||executeEffectGraph(&api,gpu,true)){failures<<"GPU graph execution error";return 1;}
  if(gpuPixelOperations()==before)return 77;
  float error=0;for(int y=0;y<cpu.height();++y){auto *a=reinterpret_cast<const float *>(cpu.constScanLine(y));auto *b=reinterpret_cast<const float *>(gpu.constScanLine(y));
    for(int x=0;x<cpu.width()*4;++x)error=std::max(error,std::abs(a[x]-b[x]));}
  if(error>.001f)failures<<"CPU/GPU graph HDR and alpha parity";
  return failures.isEmpty()?0:1;
}
