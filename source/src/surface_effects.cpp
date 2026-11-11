#include "engine.h"
#include "effect_packs.h"
#include "render_workers.h"
#include "gpu_pixels.h"
#include "route_choice.h"
#include <QCache>
#include <QSet>
#include <atomic>
#include <QMutex>
#include <QDataStream>
#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <QPainterPath>

namespace {
std::vector<QPainterPath> strokeContours(const QPainterPath &path) {
  std::vector<QPainterPath> contours;
  QPainterPath contour;
  QPointF start;
  bool hasStart=false;
  auto flush=[&] {
    if(!contour.isEmpty()) {
      // Glyph outlines are closed curves. Reconstructing them as merely an
      // open curve whose last coordinate happens to equal its first makes the
      // stroker install two caps at that seam. At broad widths those caps were
      // the flat/rounded bumps and small wedges visible on V and y.
      if(hasStart && QLineF(contour.currentPosition(),start).length()<1e-5)
        contour.closeSubpath();
      contours.push_back(contour);
    }
    contour=QPainterPath();
    hasStart=false;
  };
  for(int i=0;i<path.elementCount();++i) {
    const auto element=path.elementAt(i);
    if(element.type==QPainterPath::MoveToElement) {
      flush();start=QPointF(element.x,element.y);hasStart=true;contour.moveTo(start);
    } else if(element.type==QPainterPath::LineToElement) {
      contour.lineTo(element.x,element.y);
    } else if(element.type==QPainterPath::CurveToElement && i+2<path.elementCount()) {
      const auto control2=path.elementAt(i+1),end=path.elementAt(i+2);
      contour.cubicTo(element.x,element.y,control2.x,control2.y,end.x,end.y);
      i+=2;
    }
  }
  flush();
  return contours;
}
void drawStrokeContours(QPainter &p,const QPainterPath &path,const QPen &pen) {
  p.setBrush(Qt::NoBrush);p.setPen(pen);
  // A compound QPainterPath uses an even/odd style overlap while stroking.
  // With a very broad text outline, the outer glyph edge, its counter and a
  // neighbouring glyph can overlap and cancel to transparency. Separate
  // SourceOver draws merge those regions instead.
  for(const auto &contour:strokeContours(path)) p.drawPath(contour);
}
}

void drawAlignedStroke(QPainter &p,const QPainterPath &path,QBrush fill,QPen outline,int mode) {
  p.save(); p.setPen(Qt::NoPen); p.setBrush(fill); p.drawPath(path);
  if(outline.style()!=Qt::NoPen && outline.widthF()>0) {
    // Intersecting/subtracting the generated stroke path becomes unreliable
    // for a very wide outline: Qt can flip the result's winding and paint the
    // whole raster with a shape-sized hole. Draw the outline normally and use
    // a clip to keep the requested half. This is also considerably cheaper.
    const double width=outline.widthF();
    outline.setJoinStyle(Qt::SvgMiterJoin);
    // A 2x miter keeps 90-degree corners square but bevels very acute glyph
    // corners before they grow into long black spikes.
    outline.setMiterLimit(2);
    if(mode!=2) outline.setWidthF(width*2);
    if(mode==0) {
      p.setClipPath(path,Qt::IntersectClip);
    } else if(mode==1) {
      QPainterPath outside;
      outside.setFillRule(Qt::OddEvenFill);
      outside.addRect(path.controlPointRect().adjusted(-width-2,-width-2,width+2,width+2));
      outside.addPath(path);
      p.setClipPath(outside,Qt::IntersectClip);
    }
    p.setBrush(Qt::NoBrush); p.setPen(outline); p.drawPath(path);
  }
  p.restore();
}
void drawTextStrokePass(QPainter &p,const QPainterPath &path,QBrush fill,QPen outline,int mode,bool fillPass) {
  QPainterPath edge;
  std::vector<QPainterPath> edges;
  if(outline.style()!=Qt::NoPen && outline.widthF()>0) {
    QPainterPathStroker stroker;stroker.setWidth(outline.widthF()*(mode==2?1:2));
    stroker.setJoinStyle(Qt::SvgMiterJoin);stroker.setMiterLimit(2);stroker.setCapStyle(Qt::RoundCap);
    for(const auto &contour:strokeContours(path)) {
      auto part=stroker.createStroke(contour);
      edge.addPath(part);edges.push_back(part);
    }
  }
  p.save();p.setPen(Qt::NoPen);
  if(fillPass) {
    // The fill only has to avoid a band the stroke will cover from the inside.
    p.setBrush(fill);
    p.drawPath(mode==1 || edge.isEmpty() ? path : path.subtracted(edge));
  } else {
    // Half of the ring is trimmed away by the rasterizer's clip rather than by
    // a path boolean. Intersecting or subtracting two paths this complex costs
    // ~25 ms at a 200 px width, on every step of a scrub; clipping is free by
    // comparison, and Center mode -- which never needed the boolean -- always
    // ran at half a millisecond to prove it.
    p.setBrush(outline.brush());
    if(mode==0) p.setClipPath(path,Qt::IntersectClip);
    else if(mode==1) {
      QPainterPath outside;
      outside.setFillRule(Qt::OddEvenFill);
      outside.addRect(edge.controlPointRect().adjusted(-1,-1,1,1));
      outside.addPath(path);
      p.setClipPath(outside,Qt::IntersectClip);
    }
    for(const auto &part:edges) p.drawPath(part);
  }
  p.restore();
}
namespace {
struct StrokeDepth { double outside = 0, inside = 0; };
StrokeDepth strokeDepthBefore(const std::vector<TextStrokeStyle> &strokes, int stop) {
  StrokeDepth depth;
  for (int i = 0; i < std::min(stop, int(strokes.size())); ++i) {
    const auto &stroke = strokes[size_t(i)];
    const double width = std::max(0., stroke.width);
    if (stroke.mode == 0) depth.inside += width;
    else if (stroke.mode == 1) depth.outside += width;
    else { depth.inside += width / 2; depth.outside += width / 2; }
  }
  return depth;
}
}
void drawTextStrokeStackPass(QPainter &p,const QPainterPath &path,
                             const std::vector<TextStrokeStyle> &strokes,int index,bool outsideOnly) {
  if (index < 0 || index >= int(strokes.size())) return;
  const auto &stroke = strokes[size_t(index)];
  const double width = std::max(0., stroke.width);
  if (width <= 0 || stroke.color.alpha() == 0) return;
  const auto before = strokeDepthBefore(strokes, index);
  const double outside = before.outside +
      (stroke.mode == 1 ? width : stroke.mode == 2 ? width / 2 : 0);
  if (outside <= before.outside) return;
  p.save();
  const auto joinStyle = stroke.join == 1 ? Qt::RoundJoin : Qt::SvgMiterJoin;
  QPen pen(stroke.color, outside * 2, Qt::SolidLine, Qt::FlatCap, joinStyle);
  if (joinStyle == Qt::SvgMiterJoin) pen.setMiterLimit(1.5);
  if (outsideOnly) {
    // With no fill to cover it, the inner half of the pen would show inside the
    // letters: clipped to what lies outside them (a counter, like the hole of
    // an "o", counts as outside), the same way drawTextStrokePass does.
    QPainterPath around;
    around.setFillRule(Qt::OddEvenFill);
    around.addRect(path.controlPointRect().adjusted(-outside - 2, -outside - 2, outside + 2, outside + 2));
    around.addPath(path);
    p.setClipPath(around, Qt::IntersectClip);
  }
  drawStrokeContours(p,path,pen);
  p.restore();
}
void drawTextStrokeStackInsidePass(QPainter &p,const QPainterPath &path,
                                   const std::vector<TextStrokeStyle> &strokes,int index) {
  if (index < 0 || index >= int(strokes.size())) return;
  const auto &stroke = strokes[size_t(index)];
  const double width = std::max(0., stroke.width);
  if (width <= 0 || stroke.color.alpha() == 0) return;
  const auto before = strokeDepthBefore(strokes, index);
  const double inside = before.inside +
      (stroke.mode == 0 ? width : stroke.mode == 2 ? width / 2 : 0);
  if (inside <= before.inside) return;
  p.save();
  p.setClipPath(path, Qt::IntersectClip);
  const auto joinStyle = stroke.join == 1 ? Qt::RoundJoin : Qt::SvgMiterJoin;
  QPen pen(stroke.color, inside * 2, Qt::SolidLine, Qt::FlatCap, joinStyle);
  if (joinStyle == Qt::SvgMiterJoin) pen.setMiterLimit(1.5);
  drawStrokeContours(p,path,pen);
  p.restore();
}
void drawTextFillAfterStrokes(QPainter &p,const QPainterPath &path,QBrush fill,
                              const std::vector<TextStrokeStyle> &strokes,
                              QPen characterOutline,int characterMode) {
  QPainterPath visible = path;
  auto removeCoveredInterior = [&](QPen pen, int mode) {
    if (mode == 1 || pen.style() == Qt::NoPen || pen.widthF() <= 0) return;
    QPainterPathStroker stroker;
    stroker.setWidth(pen.widthF() * (mode == 2 ? 1 : 2));
    stroker.setJoinStyle(Qt::SvgMiterJoin);
    stroker.setMiterLimit(2);
    stroker.setCapStyle(Qt::RoundCap);
    visible = visible.subtracted(stroker.createStroke(path));
  };
  // Stack interiors are painted in one clipped pass after all glyph fills.
  // Therefore a stack needs no path subtraction here. Rich per-character
  // outlines still use the legacy path because they are not part of the stack.
  removeCoveredInterior(characterOutline, std::clamp(characterMode, 0, 2));
  p.save();
  p.setPen(Qt::NoPen);
  p.setBrush(fill);
  p.drawPath(visible);
  p.restore();
}
// A raster this large or larger has its distance transform worked out on the
// card where there is one that can (gpuDistanceTransform): below it, sending it
// there and back costs more than the processor takes.
constexpr qint64 kCardFieldPixels = 160 * 1024;
// Which of the two is quicker for a field of a size, here: on a desktop card
// with a fast processor the processor's twenty cores win once the field has to
// come back over the bus; on a laptop chip sharing the processor's memory, or
// beside a processor with a few cores, the card does. Measured, not guessed.
static RouteChoice &fieldRoute() { static RouteChoice route("Distance fields"); return route; }
static RouteChoice &labelRoute() { static RouteChoice route("Nearest letters"); return route; }
static RouteChoice &strokeBlendRoute() { static RouteChoice route("Stroke effect"); return route; }
// Exact squared Euclidean distance transform, separable lower envelopes.
// Unlike chamfer propagation, circular offsets do not become octagons.
static std::vector<float> distanceField(const QImage &image,bool inside) {
  const int w=image.width(),h=image.height(),nmax=std::max(w,h);
  const qint64 pixels=qint64(w)*h;
  const bool cardCan=pixels>=kCardFieldPixels && canUseGpuPixels();
  QElapsedTimer clock;
  clock.start();
  // On the card, the same envelopes in the same arithmetic: the same field.
  // Without a card, or where the processor is the quicker, the processor below.
  if(cardCan && fieldRoute().card(pixels)) {
    std::vector<qint32> ids(size_t(w)*h);
    renderParallel(h,[&](int begin,int end){for(int y=begin;y<end;++y){
      auto *p=reinterpret_cast<const float *>(image.constScanLine(y));
      for(int x=0;x<w;++x) ids[size_t(y)*w+x]=((p[x*4+3]>=.5f)==inside)?1:0;
    }});
    std::vector<float> squared;
    if(gpuDistanceTransform(ids,w,h,squared)) {
      renderParallel(h,[&](int begin,int end){
        for(size_t i=size_t(begin)*w;i<size_t(end)*w;++i) squared[i]=std::sqrt(squared[i]);
      });
      fieldRoute().took(pixels,true,clock.nsecsElapsed()/1e6);
      return squared;
    }
    fieldRoute().refused(pixels);
    clock.restart();
  }
  struct Timed {
    bool on; qint64 pixels; QElapsedTimer &clock;
    ~Timed() { if(on) fieldRoute().took(pixels,false,clock.nsecsElapsed()/1e6); }
  } timed{cardCan,pixels,clock};
  std::vector<float> d(size_t(w)*h);
  auto batch=[&](int begin,int end,bool columns) {
  std::vector<float> f(nmax),out(nmax),z(nmax+1);
  std::vector<int> v(nmax);
  auto transform=[&](int n) {
    int k=0;v[0]=0;z[0]=-1e20f;z[1]=1e20f;
    for(int q=1;q<n;++q) {
      float s;
      do {
        int r=v[k];s=((f[q]+float(q)*q)-(f[r]+float(r)*r))/(2.f*(q-r));
        if(s>z[k] || k==0) break;
        --k;
      } while(true);
      ++k;v[k]=q;z[k]=s;z[k+1]=1e20f;
    }
    k=0;
    for(int q=0;q<n;++q) {while(z[k+1]<q) ++k;float dx=float(q-v[k]);out[q]=dx*dx+f[v[k]];}
  };
  if(!columns) for(int y=begin;y<end;++y) {
    auto *p=reinterpret_cast<const float *>(image.constScanLine(y));
    for(int x=0;x<w;++x) f[x]=((p[x*4+3]>=.5f)==inside)?0.f:1e12f;
    transform(w);
    std::copy_n(out.data(),w,d.data()+size_t(y)*w);
  }
  // Down the columns, a band at a time. One column on its own is a jump of a
  // whole row between every two values it reads, so every read of a large
  // field misses the cache; a band is copied out and back along the rows,
  // which reads each cache line once, and is small enough to be swept where
  // it lies.
  if(columns) {
    constexpr int band=16;
    std::vector<float> strip(size_t(band)*size_t(h));
    for(int x0=begin;x0<end;x0+=band) {
      const int count=std::min(band,end-x0);
      for(int y=0;y<h;++y)
        std::copy_n(d.data()+size_t(y)*w+x0,count,strip.data()+size_t(y)*band);
      for(int c=0;c<count;++c) {
        for(int y=0;y<h;++y) f[y]=strip[size_t(y)*band+c];
        transform(h);
        for(int y=0;y<h;++y) strip[size_t(y)*band+c]=std::sqrt(out[y]);
      }
      for(int y=0;y<h;++y)
        std::copy_n(strip.data()+size_t(y)*band,count,d.data()+size_t(y)*w+x0);
    }
  }
  };
  renderParallel(h,[&](int begin,int end){batch(begin,end,false);});
  // A band is a lot of work, so they are handed out one at a time rather than
  // in equal shares, which left most of the cores idle.
  {
    constexpr int band=16;
    const int bands=(w+band-1)/band;
    renderParallelEach(bands,[&](int n){
      batch(n*band,std::min(w,(n+1)*band),true);
    });
  }
  return d;
}
// A distance field describes the layer's alpha and nothing else: the stroke's
// width, colour and position never enter it. Dragging Width therefore re-blends
// a field that is already correct, so hold on to it. The key is the alpha
// channel itself, which is exact -- there is no list of layer properties to
// keep in sync with whatever drawLayer() does next.
static std::atomic<quint64> computedFields{0};
quint64 strokeFieldComputations() { return computedFields.load(); }
static quint64 computeAlphaSignature(const QImage &image) {
  std::vector<quint64> rows(image.height());
  renderParallel(image.height(),[&](int begin,int end){for(int y=begin;y<end;++y) {
    quint64 hash=0xcbf29ce484222325ULL;
    auto *p=reinterpret_cast<const float *>(image.constScanLine(y));
    for(int x=0;x<image.width();++x) {
      quint32 bits; std::memcpy(&bits,p+x*4+3,4);
      hash=(hash^bits)*0x100000001b3ULL;
    }
    rows[y]=hash;
  }});
  quint64 hash=0xcbf29ce484222325ULL;
  for(auto row:rows)hash=(hash^row)*0x100000001b3ULL;
  return hash;
}
static quint64 alphaSignature(const QImage &image) {
  static QMutex guard;
  static QHash<qint64,quint64> cache;
  const qint64 ck=image.cacheKey();
  { QMutexLocker lock(&guard);
    auto it=cache.find(ck);
    if(it!=cache.end()) return it.value();
  }
  const quint64 sig=computeAlphaSignature(image);
  { QMutexLocker lock(&guard);
    if(cache.size()>=64) cache.clear();
    cache.insert(ck,sig);
  }
  return sig;
}
using DistanceField = std::shared_ptr<const std::vector<float>>;
static DistanceField cachedDistanceField(const QImage &image,bool inside,quint64 signature) {
  // Export can drive this from a worker thread; the cache is shared, so guard it.
  static QMutex guard;
  static QCache<QByteArray,DistanceField> cache{131072}; // KiB, 128 MiB.
  QByteArray key(24,0);
  {
    auto *k=reinterpret_cast<quint64 *>(key.data());
    k[0]=signature;
    k[1]=quint64(quint32(image.width()))<<32 | quint32(image.height());
    k[2]=inside?1:0;
  }
  {
    QMutexLocker lock(&guard);
    if(auto *hit=cache.object(key)) return *hit;
  }
  ++computedFields;
  DistanceField field=std::make_shared<const std::vector<float>>(distanceField(image,inside));
  const int cost=std::max(1,int((field->size()*sizeof(float)+1023)/1024));
  {
    QMutexLocker lock(&guard);
    cache.insert(key,new DistanceField(field),cost);
  }
  return field;
}
// Each surface effect is one pass over straight-alpha float pixels, so the
// layer's own effect order decides when each of them runs.
// The layer's shape in one colour, at whatever opacity is asked for. Straight
// alpha, so coverage is the alpha channel and the colour is written flat.
void fillPass(QImage &image,const Layer &l) {
  auto s=l.surfaceEffects["Fill"].toObject();
  const QColor colour(s["color"].toString("#ffffff"));
  const double opacity=std::clamp(s["opacity"].toDouble(100)/100.,0.,1.)*colour.alphaF();
  const float r=float(colour.redF()),g=float(colour.greenF()),b=float(colour.blueF());
  auto *bits=image.bits();const auto stride=image.bytesPerLine();
  const int width=image.width();
  renderParallel(image.height(),[&](int begin,int end){for(int y=begin;y<end;++y) {
    auto *p=reinterpret_cast<float *>(bits+y*stride);
    for(int x=0;x<width;++x) {
      p[x*4]=r;p[x*4+1]=g;p[x*4+2]=b;
      p[x*4+3]=float(p[x*4+3]*opacity);
    }
  }});
}
void gradientPass(QImage &image,const Layer &l,QRectF box) {
  {
    auto s=l.surfaceEffects["Gradient"].toObject();
    QColor a(s["start"].toString("#ff6a00")),b(s["end"].toString("#6633ff"));
    double angle=s["angle"].toDouble()*3.141592653589793/180.;
    double dx=std::cos(angle),dy=std::sin(angle),span=std::max(1.,std::abs(dx)*box.width()+std::abs(dy)*box.height());
    double amount=std::clamp(s["amount"].toDouble(100)/100.,0.,1.);
    if(amount==0)return;
    // On the card or the processor, whichever is the quicker here.
    static RouteChoice route("Gradient effect");
    const qint64 pixels=qint64(image.width())*image.height();
    const bool cardCan=canUseGpuPixels();
    QImage gpu;
    const bool triedCard=cardCan && route.card(pixels);
    QElapsedTimer clock;
    clock.start();
    if(triedCard)
      gpu=gpuPixelOperation(image,1,{float(box.center().x()),float(box.center().y()),float(s["angle"].toDouble()),float(b.blueF())},
          {float(a.redF()),float(a.greenF()),float(a.blueF()),float(s["type"].toInt()==1?-amount:amount)},
          {float(b.redF()),float(b.greenF()),float(box.width()),float(box.height())});
    if(!gpu.isNull()) { image=gpu; route.took(pixels,true,clock.nsecsElapsed()/1e6); }
    else {
    if(triedCard) { route.refused(pixels); clock.restart(); }
    const ProcessorTiming timing(cardCan ? &route : nullptr, pixels);
    // Everything the gradient needs is the same for every pixel, so it is read
    // once. This loop used to look "type" up in the QJsonObject -- a string
    // hash and a search -- for every pixel of every frame.
    const bool radial=s["type"].toInt()==1;
    const QPointF center=box.center();
    const double radius=std::max(1.,std::max(box.width(),box.height())/2);
    const double ar=a.redF(),ag=a.greenF(),ab=a.blueF();
    const double br=b.redF(),bg=b.greenF(),bb=b.blueF();
    auto *bits=image.bits();const auto stride=image.bytesPerLine();
    const int width=image.width();
    renderParallel(image.height(),[&](int begin,int end) {for(int y=begin;y<end;++y) {
      auto *p=reinterpret_cast<float *>(bits+y*stride);
      const double vy=y+.5-center.y();
      for(int x=0;x<width;++x) {
        const double vx=x+.5-center.x();
        double t=radial?std::hypot(vx,vy)/radius:.5+(vx*dx+vy*dy)/span;
        t=std::clamp(t,0.,1.);
        const double rgb[3]={ar*(1-t)+br*t,ag*(1-t)+bg*t,ab*(1-t)+bb*t};
        for(int c=0;c<3;++c) p[x*4+c]=float(p[x*4+c]*(1-amount)+rgb[c]*amount);
      }
    }});
    }
  }
}
// Packed the way the shader reads it: outward distance in red, inward in green,
// with the side a mode does not use pushed out of reach.
QImage strokeDistanceField(const QImage &image,const Layer &l) {
  auto s=l.surfaceEffects["Stroke"].toObject();
  if(std::clamp(s["width"].toDouble(4),0.,200.)<=0)return {};
  const int mode=std::clamp(s["mode"].toInt(1),0,2);
  const QImage source=image.format()==QImage::Format_RGBA32FPx4
                          ? image : image.convertToFormat(QImage::Format_RGBA32FPx4);
  QImage distances(source.size(),QImage::Format_RGBA32FPx4);
  if(distances.isNull())return {};
  const auto signature=alphaSignature(source);
  auto out=mode==0?DistanceField{}:cachedDistanceField(source,true,signature);
  auto in=mode==1?DistanceField{}:cachedDistanceField(source,false,signature);
  auto *bits=distances.bits();const auto stride=distances.bytesPerLine();
  renderParallel(source.height(),[&](int begin,int end){for(int y=begin;y<end;++y) {
    auto *p=reinterpret_cast<float *>(bits+y*stride);
    for(int x=0;x<source.width();++x) {
      const size_t i=size_t(y)*source.width()+x;
      p[x*4]=mode==0?1e6f:(*out)[i];
      p[x*4+1]=mode==1?1e6f:(*in)[i];
      p[x*4+2]=0;p[x*4+3]=1;
    }
  }});
  return distances;
}
void strokePass(QImage &image,const Layer &l,double scale) {
  {
    auto s=l.surfaceEffects["Stroke"].toObject();
    float width=float(std::clamp(s["width"].toDouble(4),0.,200.)*scale);
    if(width>0) {
      int mode=std::clamp(s["mode"].toInt(1),0,2); QColor color(s["color"].toString("#ffffff"));
      // Stroke Only: the stroke on its own, what it strokes left out.
      const bool only=s["strokeOnly"].toBool(false);
      if(color.alpha()==0) { if(only) image.fill(Qt::transparent); return; }
      const auto signature=alphaSignature(image);
      auto out=mode==0?DistanceField{}:cachedDistanceField(image,true,signature);
      auto in=mode==1?DistanceField{}:cachedDistanceField(image,false,signature);
      float radius=mode==2?width/2:width;
      QImage gpu;
      const qint64 strokePixels=qint64(image.width())*image.height();
      const bool strokeCardCan=canUseGpuPixels() && !only;
      QElapsedTimer strokeClock;
      strokeClock.start();
      const bool triedCard=strokeCardCan && strokeBlendRoute().card(strokePixels);
      if(triedCard) {
      auto distances=strokeDistanceField(image,l);
      if(!distances.isNull()) {
      gpu=gpuPixelOperation(image,4,{radius,float(mode),0,0},{float(color.redF()),float(color.greenF()),float(color.blueF()),float(color.alphaF())},{},distances);
      }
      }
      if(!gpu.isNull()) {
        image=gpu;
        strokeBlendRoute().took(strokePixels,true,strokeClock.nsecsElapsed()/1e6);
      }
      else {
      if(triedCard) { strokeBlendRoute().refused(strokePixels); strokeClock.restart(); }
      struct Timed {
        bool on; qint64 pixels; QElapsedTimer &clock;
        ~Timed() { if(on) strokeBlendRoute().took(pixels,false,clock.nsecsElapsed()/1e6); }
      } timed{strokeCardCan,strokePixels,strokeClock};
      auto *bits=image.bits();const auto stride=image.bytesPerLine();
      // The stroke lies within its own width of the edge. Everywhere else both
      // distances are past it and the arithmetic below returns the pixel
      // unchanged -- which, on anything larger than the stroke is wide, is
      // nearly every pixel of the raster. Read the two fields, and where
      // neither reaches, leave the pixel alone.
      const float *inField=in?in->data():nullptr,*outField=out?out->data():nullptr;
      const float strokeAlpha=float(color.alphaF());
      const float rgb[3]={float(color.redF()),float(color.greenF()),float(color.blueF())};
      const float reach=radius+.5f;
      renderParallel(image.height(),[&](int begin,int end){for(int y=begin;y<end;++y) {
        auto *p=reinterpret_cast<float *>(bits+y*stride);
        const size_t row=size_t(y)*size_t(image.width());
        for(int x=0;x<image.width();++x) {
          const size_t i=row+size_t(x);
          const float raw=p[x*4+3],alpha=std::clamp(raw,0.f,1.f);
          const float innerReach=inField?reach-inField[i]:0.f;
          const float outerReach=outField?reach-outField[i]:0.f;
          if(innerReach<=0.f && outerReach<=0.f) {
            if(only) { p[x*4]=p[x*4+1]=p[x*4+2]=p[x*4+3]=0.f; continue; }
            // Nothing of the stroke here. The old arithmetic still wrote the
            // clamped alpha back, so a raster carrying one outside nought to
            // one keeps being clamped.
            if(raw!=alpha) p[x*4+3]=alpha;
            continue;
          }
          const float inner=std::clamp(innerReach,0.f,1.f)*alpha*strokeAlpha;
          const float outer=std::clamp(outerReach,0.f,1.f)*(1-alpha)*strokeAlpha;
          if(only) {
            for(int c=0;c<3;++c) p[x*4+c]=rgb[c];
            p[x*4+3]=inner+outer;
            continue;
          }
          const float nextAlpha=alpha+outer;
          if(nextAlpha>0) {
            const float inverse=1.f/nextAlpha,paint=inner+outer,keep=alpha-inner;
            for(int c=0;c<3;++c) p[x*4+c]=(p[x*4+c]*keep+rgb[c]*paint)*inverse;
          } else for(int c=0;c<3;++c) p[x*4+c]=0;
          p[x*4+3]=nextAlpha;
        }
      }});
      }
    }
  }
}
namespace {
// A point on the letters' outline where its direction jumps: where a stem
// meets a serif, the top of a "T", the foot of a "W". A curve's own points are
// not corners, so an "o" has none.
struct LetterCorner {
  QPointF at;
  // Normals of the two edges meeting there, and the bisector between them,
  // all pointing away from the side the corner is sharp on.
  QPointF n1, n2, bisector;
  // Sharp towards the space around the letters (their outer corners), or
  // towards the letters (the inner corner of an "L", seen from inside).
  bool outward = true;
};
double dotOf(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
double crossOf(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }
QPointF unit(QPointF v) {
  const double length = std::hypot(v.x(), v.y());
  return length > 1e-12 ? v / length : QPointF();
}
// Corners of every contour in the letters, in the raster's own pixels. Only
// corners that sit on the outline everybody sees are kept: one letter's corner
// buried inside a neighbour it overlaps is sharp towards nothing.
std::vector<LetterCorner> letterCorners(const std::vector<QPainterPath> &letters, QRectF area) {
  const int n = int(letters.size());
  if(n == 0) return {};
  struct Piece { QPointF from, to, leaves, arrives; };
  const double eps = 1e-9;
  auto firstDirection = [](std::initializer_list<QPointF> candidates) {
    for (auto candidate : candidates)
      if (std::hypot(candidate.x(), candidate.y()) > 1e-7) return unit(candidate);
    return QPointF();
  };
  auto letterRects = std::vector<QRectF>(n);
  for(int i = 0; i < n; ++i) letterRects[i] = letters[i].controlPointRect();
  auto material = [&](QPointF point) {
    for (int i = 0; i < n; ++i)
      if (letterRects[i].contains(point) && letters[i].contains(point)) return true;
    return false;
  };
  auto perLetter = std::vector<std::vector<LetterCorner>>(n);
  renderParallel(n, [&](int begin, int end) {
    for(int li = begin; li < end; ++li) {
      const auto &letter = letters[li];
      if (!letterRects[li].intersects(area)) continue;
      auto &out = perLetter[li];
      std::vector<Piece> pieces;
      auto finish = [&](QPointF start, QPointF current) {
        if (std::hypot(current.x() - start.x(), current.y() - start.y()) > 1e-7) {
          const QPointF d = unit(start - current);
          pieces.push_back({current, start, d, d});
        }
        const int count = int(pieces.size());
        for (int k = 0; k < count && count > 1; ++k) {
          const auto &in = pieces[size_t(k)];
          const auto &next = pieces[size_t((k + 1) % count)];
          const QPointF tin = in.arrives, tout = next.leaves;
          if (tin.isNull() || tout.isNull()) continue;
          if (std::atan2(std::abs(crossOf(tin, tout)), dotOf(tin, tout)) < 3.141592653589793 / 180)
            continue;
          const QPointF inward = unit(tout - tin);
          if (inward.isNull()) continue;
          const QPointF at = in.to;
          const double probe = .05;
          const bool inside = material(at + inward * probe), outside = material(at - inward * probe);
          if (inside == outside) continue;
          LetterCorner corner;
          corner.at = at;
          corner.outward = inside;
          corner.bisector = -inward;
          QPointF n1(-tin.y(), tin.x()), n2(-tout.y(), tout.x());
          if (dotOf(n1, corner.bisector) < 0) n1 = -n1;
          if (dotOf(n2, corner.bisector) < 0) n2 = -n2;
          corner.n1 = n1;
          corner.n2 = n2;
          out.push_back(corner);
        }
        pieces.clear();
      };
      QPointF start, current;
      bool open = false;
      for (int i = 0; i < letter.elementCount(); ++i) {
        const auto e = letter.elementAt(i);
        const QPointF point(e.x, e.y);
        if (e.isMoveTo()) {
          if (open) finish(start, current);
          start = current = point;
          open = true;
        } else if (e.isLineTo()) {
          if (std::hypot(point.x() - current.x(), point.y() - current.y()) > eps) {
            const QPointF d = unit(point - current);
            pieces.push_back({current, point, d, d});
            current = point;
          }
        } else if (e.isCurveTo() && i + 2 < letter.elementCount()) {
          const auto c2e = letter.elementAt(i + 1), ende = letter.elementAt(i + 2);
          const QPointF c1 = point, c2(c2e.x, c2e.y), end(ende.x, ende.y);
          i += 2;
          const QPointF leaves = firstDirection({c1 - current, c2 - current, end - current});
          const QPointF arrives = firstDirection({end - c2, end - c1, end - current});
          if (!leaves.isNull() && !arrives.isNull()) {
            pieces.push_back({current, end, leaves, arrives});
            current = end;
          }
        }
      }
      if (open) finish(start, current);
    }
  });
  std::vector<LetterCorner> corners;
  for(auto &v : perLetter)
    for(auto &c : v) corners.push_back(c);
  return corners;
}
// Lowers a distance field to what a mitred join measures near each corner.
//
// Round and mitred strokes agree everywhere except in the wedge beyond a
// corner, between the normals of the two edges meeting there. In that wedge a
// round stroke reaches the corner's distance; a mitred one reaches as far as
// both edges' own lines allow, which is the larger of the distances to those
// two lines -- and, past the miter limit, to a line across the bisector. That
// is never more than the distance to the corner, so it only ever adds: the
// field keeps every other part of the letters exactly as measured, and no
// width can open a hole in it. Like the field, it is a distance, so every
// width and every stroke of the stack reads the same one.
//
// Distances follow the field's own convention: half a pixel beyond the
// outline, measured from pixel centres.
// The region a miter `reach` wide can cover at a corner: out to its tip, or to
// the cut across the bisector where the limit stops it. Empty where the corner
// is no corner (its edges run on in a line).
QPolygonF mitreArea(const LetterCorner &corner, double reach, double limit) {
  const QPointF n1 = corner.n1, n2 = corner.n2, b = corner.bisector;
  if (std::abs(crossOf(n1, n2)) < 1e-6) return {};
  const double halfCos = std::max(1e-6, dotOf(n1, b));
  QPolygonF area;
  area << corner.at << corner.at + n1 * reach;
  const double tip = reach / halfCos;
  if (tip <= limit * reach) area << corner.at + b * tip;
  else {
    const QPointF e1(-n1.y(), n1.x()), e2(-n2.y(), n2.x());
    const QPointF along1 = dotOf(e1, b) > 0 ? e1 : -e1, along2 = dotOf(e2, b) > 0 ? e2 : -e2;
    const double run = (limit * reach - reach * halfCos) / std::max(1e-6, dotOf(along1, b));
    area << corner.at + n1 * reach + along1 * run << corner.at + n2 * reach + along2 * run;
  }
  area << corner.at + n2 * reach;
  return area;
}
void mitreField(std::vector<float> &field, int width, int height,
                const std::vector<LetterCorner> &corners, bool outward, double reach,
                double limit) {
  for (const auto &corner : corners) {
    if (corner.outward != outward) continue;
    const QPointF n1 = corner.n1, n2 = corner.n2, b = corner.bisector;
    const double spread = crossOf(n1, n2);
    const QPolygonF area = mitreArea(corner, reach, limit);
    if (area.isEmpty()) continue;
    const QRect box = area.boundingRect().toAlignedRect().adjusted(-1, -1, 1, 1)
                          .intersected(QRect(0, 0, width, height));
    if (box.isEmpty()) continue;
    const float nx1 = float(n1.x()), ny1 = float(n1.y()), nx2 = float(n2.x()), ny2 = float(n2.y());
    const float bx = float(b.x()), by = float(b.y()), sign = spread > 0 ? 1.f : -1.f;
    const float cx = float(corner.at.x()), cy = float(corner.at.y()), cut = float(1 / limit);
    auto rows = [&](int begin, int end) {
      for (int y = box.top() + begin; y < box.top() + end; ++y) {
        const float dy = y + .5f - cy;
        auto *row = field.data() + size_t(y) * width;
        for (int x = box.left(); x <= box.right(); ++x) {
          const float dx = x + .5f - cx;
          // Inside the wedge: on n2's side of n1 and n1's side of n2.
          if ((nx1 * dy - ny1 * dx) * sign < 0 || (dx * ny2 - dy * nx2) * sign < 0) continue;
          const float mitred = std::max({dx * nx1 + dy * ny1, dx * nx2 + dy * ny2,
                                         (dx * bx + dy * by) * cut}) + .5f;
          if (mitred < row[x]) row[x] = mitred;
        }
      }
    };
    if (qint64(box.width()) * box.height() >= 65536) renderParallel(box.height(), rows);
    else rows(0, box.height());
  }
}
// Which letter is nearest each pixel: the label (from `labels`, 0 for none) of
// the nearest pixel that is one of the letters -- alpha at least half, as the
// field counts them -- found exactly by the same lower envelopes as the field,
// each envelope carrying the pixel it came from. 0 where there are no letters.
std::vector<int> nearestLetterLabels(const QImage &work, const std::vector<int> &labels) {
  const int w = work.width(), h = work.height();
  // On the card, where there is one that can and it is the quicker: the same
  // envelopes, carrying the id of the pixel each one came from.
  const qint64 pixels = qint64(w) * h;
  const bool cardCan = pixels >= kCardFieldPixels && canUseGpuPixels();
  QElapsedTimer clock;
  clock.start();
  if (cardCan && labelRoute().card(pixels)) {
    std::vector<qint32> ids(size_t(w) * h);
    renderParallel(h, [&](int begin, int end) {
      for (int y = begin; y < end; ++y) {
        const auto *p = reinterpret_cast<const float *>(work.constScanLine(y));
        for (int x = 0; x < w; ++x) {
          const size_t i = size_t(y) * w + x;
          ids[i] = p[x * 4 + 3] >= .5f && labels[i] > 0 ? labels[i] : 0;
        }
      }
    });
    std::vector<float> squared;
    std::vector<qint32> nearest;
    if (gpuDistanceTransform(ids, w, h, squared, &nearest)) {
      labelRoute().took(pixels, true, clock.nsecsElapsed() / 1e6);
      return std::vector<int>(nearest.begin(), nearest.end());
    }
    labelRoute().refused(pixels);
    clock.restart();
  }
  struct Timed {
    bool on; qint64 pixels; QElapsedTimer &clock;
    ~Timed() { if (on) labelRoute().took(pixels, false, clock.nsecsElapsed() / 1e6); }
  } timed{cardCan, pixels, clock};
  constexpr float far = 1e12f;
  std::vector<float> rowDistance(size_t(w) * h);
  std::vector<int> rowSite(size_t(w) * h), nearest(size_t(w) * h, 0);
  auto envelope = [](int n, const float *f, int *v, float *z, float *out, int *site) {
    int k = 0;
    v[0] = 0; z[0] = -1e20f; z[1] = 1e20f;
    for (int q = 1; q < n; ++q) {
      float s;
      do {
        const int r = v[k];
        s = ((f[q] + float(q) * q) - (f[r] + float(r) * r)) / (2.f * (q - r));
        if (s > z[k] || k == 0) break;
        --k;
      } while (true);
      ++k; v[k] = q; z[k] = s; z[k + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
      while (z[k + 1] < q) ++k;
      const float d = float(q - v[k]);
      out[q] = d * d + f[v[k]];
      site[q] = v[k];
    }
  };
  renderParallel(h, [&](int begin, int end) {
    std::vector<float> f(w), out(w), z(size_t(w) + 1);
    std::vector<int> v(w), site(w);
    for (int y = begin; y < end; ++y) {
      const auto *p = reinterpret_cast<const float *>(work.constScanLine(y));
      const int *label = labels.data() + size_t(y) * w;
      for (int x = 0; x < w; ++x) f[x] = p[x * 4 + 3] >= .5f && label[x] > 0 ? 0.f : far;
      envelope(w, f.data(), v.data(), z.data(), out.data(), site.data());
      std::copy_n(out.data(), w, rowDistance.data() + size_t(y) * w);
      std::copy_n(site.data(), w, rowSite.data() + size_t(y) * w);
    }
  });
  renderParallel(w, [&](int begin, int end) {
    std::vector<float> f(h), out(h), z(size_t(h) + 1);
    std::vector<int> v(h), site(h);
    for (int x = begin; x < end; ++x) {
      for (int y = 0; y < h; ++y) f[y] = rowDistance[size_t(y) * w + x];
      envelope(h, f.data(), v.data(), z.data(), out.data(), site.data());
      for (int y = 0; y < h; ++y) {
        if (out[y] >= far / 10) continue;
        const size_t from = size_t(site[y]) * w;
        nearest[size_t(y) * w + x] = labels[from + size_t(rowSite[from + x])];
      }
    }
  });
  return nearest;
}
}
std::vector<float> strokeDistanceFieldOf(const QImage &straight, bool inside) { return distanceField(straight, inside); }
std::vector<int> nearestLetterOf(const QImage &straight, const std::vector<int> &labels) {
  return nearestLetterLabels(straight, labels);
}
QRectF mitredStrokeBox(const QPainterPath &glyph, double reach, double limit) {
  QRectF box = glyph.boundingRect().adjusted(-reach, -reach, reach, reach);
  if (reach <= 0 || glyph.isEmpty()) return box;
  for (const auto &corner : letterCorners({glyph}, box.adjusted(-reach * limit, -reach * limit, reach * limit, reach * limit)))
    if (corner.outward)
      if (const QPolygonF area = mitreArea(corner, reach, limit); !area.isEmpty()) box = box.united(area.boundingRect());
  return box;
}
// Character strokes, built from the distance to the letters rather than by
// stroking their outlines with a very wide pen.
//
// A stroke that follows the letters at any width is exactly the set of points
// within that distance of them, which is what a distance field measures. The
// pen that did this before was twice the stroke wide -- 192 pixels for a 96
// pixel stroke -- with mitred joins, and at that width Qt's outline filling
// flips winding where the letters' strokes overlap: a "W" became a pointed
// pentagon, the counter of an "o" became a ring hanging off the word, and the
// shape stopped having anything to do with the text it was around.
//
// The field depends only on the letters' coverage, so it is computed once per
// set of letters and every width, colour and mode after that is a blend over
// a field that is already right. Straight alpha in, straight alpha out.
//
// A distance on its own rounds every corner, and letters with square corners
// are not round. A Miter stroke therefore also takes the letters' outlines,
// in the raster's pixels, and keeps their corners sharp (see mitreField).
// Without outlines a Miter stroke is drawn round.
namespace {
// ---------------------------------------------------------------------------
// The exact edge of a Character stroke.
//
// A distance measured between pixel centres puts every edge somewhere within
// half a pixel of the letters' outline, and where exactly changes with the
// raster: a curve's stroke came out as a polygon of flat runs and kinks -- the
// wider the stroke and the larger the letters, the plainer -- and as a letter
// grew or shrank the pattern moved and the stroke's edge crawled. That field
// still says which pixels lie well inside a stroke and which well outside it.
// For the pixel or two an edge passes through, the edge is measured exactly:
// the distance to the letters' own outlines.
// ---------------------------------------------------------------------------

// A distance field between pixel centres, as distanceField measures it, and
// -- asked for -- the pixel each one measured to: for an animated text, the
// letter nearest a pixel is the one that pixel belongs to.
struct SiteField {
  std::vector<float> distance; // 1e6 where there is nothing to measure to
  std::vector<qint32> site;    // y * width + x of the image, or -1; empty unless asked for
  // Of the part measured, where the pixel at (x, y) of the image is kept.
  int left = 0, top = 0, width = 0;
  size_t at(int x, int y) const { return size_t(y - top) * size_t(width) + size_t(x - left); }
};
using SiteFieldPtr = std::shared_ptr<const SiteField>;

// The same lower envelopes as distanceField, each carrying the pixel it came
// from when `sites` asks for that. `inside` is one byte a pixel, non-zero for
// the letters' own (half covered or more); measured towards the letters, or
// away from them. Only the pixels of `part` are measured and kept -- to every
// letter pixel of the image, wherever it lies: a raster cut back to what
// shows has letters past it only for their strokes, which reach in.
SiteField siteField(const std::vector<uint8_t> &inside, int w, int h, bool towardsLetters, bool sites, QRect part) {
  constexpr float far = 1e12f;
  part = part.isValid() ? part & QRect(0, 0, w, h) : QRect(0, 0, w, h);
  const int left = part.left(), top = part.top(), pw = part.width(), ph = part.height();
  SiteField field;
  field.left = left;
  field.top = top;
  field.width = pw;
  field.distance.resize(size_t(pw) * ph);
  if (sites) field.site.resize(size_t(pw) * ph);
  // Along every row, the part's columns kept: a letter above or below the
  // part is reached down its column. The whole image goes through the field
  // itself, as it always has. Every value is written before it is read.
  const bool whole = part == QRect(0, 0, w, h);
  std::unique_ptr<float[]> acrossStore(whole ? nullptr : new float[size_t(pw) * h]);
  float *const across = whole ? field.distance.data() : acrossStore.get();
  std::unique_ptr<qint32[]> rowSite(sites ? new qint32[size_t(pw) * h] : nullptr);
  auto envelope = [](int n, const float *f, int *v, float *z, float *out, int *site) {
    int k = 0;
    v[0] = 0; z[0] = -1e20f; z[1] = 1e20f;
    for (int q = 1; q < n; ++q) {
      float s;
      do {
        const int r = v[k];
        s = ((f[q] + float(q) * q) - (f[r] + float(r) * r)) / (2.f * (q - r));
        if (s > z[k] || k == 0) break;
        --k;
      } while (true);
      ++k; v[k] = q; z[k] = s; z[k + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
      while (z[k + 1] < q) ++k;
      const float d = float(q - v[k]);
      out[q] = d * d + f[v[k]];
      site[q] = v[k];
    }
  };
  renderParallel(h, [&](int begin, int end) {
    std::vector<float> f(w), out(w), z(size_t(w) + 1);
    std::vector<int> v(w), s(w);
    for (int y = begin; y < end; ++y) {
      const uint8_t *row = inside.data() + size_t(y) * w;
      for (int x = 0; x < w; ++x) f[x] = (row[x] != 0) == towardsLetters ? 0.f : far;
      envelope(w, f.data(), v.data(), z.data(), out.data(), s.data());
      std::copy_n(out.data() + left, pw, across + size_t(y) * pw);
      if (sites) {
        qint32 *found = rowSite.get() + size_t(y) * pw;
        for (int x = 0; x < pw; ++x) found[x] = qint32(s[left + x]);
      }
    }
  });
  // Down the part's columns, a band of them at a time, copied out and back
  // along the rows, as distanceField does; the part's rows kept.
  constexpr int band = 16;
  renderParallelEach((pw + band - 1) / band, [&](int n) {
    const int x0 = n * band, count = std::min(band, pw - x0);
    std::vector<float> strip(size_t(band) * h), f(h), out(h), z(size_t(h) + 1);
    std::vector<qint32> found(sites ? size_t(band) * ph : 0);
    std::vector<int> v(h), s(h);
    for (int y = 0; y < h; ++y)
      std::copy_n(across + size_t(y) * pw + x0, count, strip.data() + size_t(y) * band);
    for (int c = 0; c < count; ++c) {
      for (int y = 0; y < h; ++y) f[y] = strip[size_t(y) * band + c];
      envelope(h, f.data(), v.data(), z.data(), out.data(), s.data());
      for (int y = top; y < top + ph; ++y) {
        const size_t at = size_t(y) * band + c, kept = size_t(y - top) * band + c;
        if (out[y] >= far / 10) {
          strip[at] = 1e6f;
          if (sites) found[kept] = -1;
          continue;
        }
        strip[at] = std::sqrt(out[y]);
        if (sites) {
          const size_t from = size_t(s[y]);
          found[kept] = qint32(from * size_t(w) + size_t(rowSite[from * size_t(pw) + size_t(x0 + c)]));
        }
      }
    }
    for (int y = top; y < top + ph; ++y) {
      std::copy_n(strip.data() + size_t(y) * band, count, field.distance.data() + size_t(y - top) * pw + x0);
      if (sites)
        std::copy_n(found.data() + size_t(y - top) * band, count, field.site.data() + size_t(y - top) * pw + x0);
    }
  });
  return field;
}
// What the letters' pixels are, as the field sees them: two rasters alike in
// that have the same field, whatever their colours or soft edges.
quint64 insideSignature(const std::vector<uint8_t> &inside, int w, int h) {
  std::vector<quint64> rows(size_t(std::max(0, h)));
  renderParallel(h, [&](int begin, int end) {
    for (int y = begin; y < end; ++y) {
      quint64 hash = 0xcbf29ce484222325ULL;
      const uint8_t *row = inside.data() + size_t(y) * w;
      int x = 0;
      for (; x + 8 <= w; x += 8) {
        quint64 word;
        std::memcpy(&word, row + x, 8);
        hash = (hash ^ word) * 0x100000001b3ULL;
      }
      for (; x < w; ++x) hash = (hash ^ row[x]) * 0x100000001b3ULL;
      rows[size_t(y)] = hash;
    }
  });
  quint64 hash = 0xcbf29ce484222325ULL ^ (quint64(quint32(w)) << 32 | quint32(h));
  for (quint64 row : rows) hash = (hash ^ row) * 0x100000001b3ULL;
  return hash;
}
// Kept as the plain field is (cachedDistanceField): a field depends on the
// letters' pixels alone, so dragging a stroke's Width re-blends one that is
// already right.
SiteFieldPtr cachedSiteField(const std::vector<uint8_t> &inside, int w, int h, bool towardsLetters,
                             bool sites, quint64 signature, QRect part) {
  static QMutex guard;
  static QCache<QByteArray, SiteFieldPtr> cache{131072}; // KiB, 128 MiB.
  part = part.isValid() ? part & QRect(0, 0, w, h) : QRect(0, 0, w, h);
  QByteArray key(40, 0);
  {
    auto *k = reinterpret_cast<quint64 *>(key.data());
    k[0] = signature;
    k[1] = quint64(quint32(w)) << 32 | quint32(h);
    k[2] = (towardsLetters ? 1 : 0) | (sites ? 2 : 0);
    k[3] = quint64(quint32(part.left())) << 32 | quint32(part.top());
    k[4] = quint64(quint32(part.width())) << 32 | quint32(part.height());
  }
  {
    QMutexLocker lock(&guard);
    if (auto *hit = cache.object(key)) return *hit;
  }
  ++computedFields;
  auto field = std::make_shared<const SiteField>(siteField(inside, w, h, towardsLetters, sites, part));
  const int cost = std::max(1, int((size_t(part.width()) * part.height() * (sites ? 8 : 4) + 1023) / 1024));
  {
    QMutexLocker lock(&guard);
    cache.insert(key, new SiteFieldPtr(field), cost);
  }
  return field;
}
// The letters' outlines as straight pieces no longer than a cell, filed by the
// cells they cross: what lies near a point is in the cells around it. Curves
// are cut until no piece strays more than a few hundredths of a pixel.
struct OutlinePieces {
  static constexpr float kCell = 8;
  float left = 0, top = 0;
  int columns = 0, rows = 0;
  std::vector<float> ax, ay, bx, by;
  std::vector<int> first, members;
  bool empty() const { return ax.empty(); }
  // From (px, py) to the nearest piece within `outer`, knowing that none lies
  // nearer than `inner`: only the cells the ring between the two crosses are
  // looked through, whichever way the nearest piece lies. 1e15 when nothing
  // lies within `outer`.
  float nearestInRing(float px, float py, float inner, float outer) const {
    if (empty()) return 1e15f;
    inner = std::max(0.f, inner);
    float best = outer * outer;
    bool found = false;
    auto visit = [&](int r, int c) {
      const size_t cell = size_t(r) * size_t(columns) + size_t(c);
      for (int k = first[cell]; k < first[cell + 1]; ++k) {
        const size_t m = size_t(members[size_t(k)]);
        const float ex = bx[m] - ax[m], ey = by[m] - ay[m];
        const float wx = px - ax[m], wy = py - ay[m];
        const float length = ex * ex + ey * ey;
        const float t = length > 0 ? std::clamp((wx * ex + wy * ey) / length, 0.f, 1.f) : 0.f;
        const float dx = wx - t * ex, dy = wy - t * ey;
        const float d = dx * dx + dy * dy;
        if (d <= best) { best = d; found = true; }
      }
    };
    const int r0 = std::max(0, int(std::floor((py - outer - top) / kCell)));
    const int r1 = std::min(rows - 1, int(std::floor((py + outer - top) / kCell)));
    for (int r = r0; r <= r1; ++r) {
      const float y0 = top + float(r) * kCell, y1 = y0 + kCell;
      const float nearY = py < y0 ? y0 - py : py > y1 ? py - y1 : 0.f;
      const float farY = std::max(std::abs(py - y0), std::abs(py - y1));
      if (nearY > outer) continue;
      const float half = std::sqrt(outer * outer - nearY * nearY);
      const int c0 = std::max(0, int(std::floor((px - half - left) / kCell)));
      const int c1 = std::min(columns - 1, int(std::floor((px + half - left) / kCell)));
      // The cells of this row wholly inside the inner circle hold nothing.
      int h0 = c1 + 1, h1 = c1;
      if (farY < inner) {
        const float hollow = std::sqrt(inner * inner - farY * farY);
        h0 = int(std::ceil((px - hollow - left) / kCell));
        h1 = int(std::floor((px + hollow - left) / kCell)) - 1;
      }
      if (h0 > h1) {
        for (int c = c0; c <= c1; ++c) visit(r, c);
        continue;
      }
      for (int c = c0; c <= std::min(c1, h0 - 1); ++c) visit(r, c);
      for (int c = std::max(c0, h1 + 1); c <= c1; ++c) visit(r, c);
    }
    return found ? std::sqrt(best) : 1e15f;
  }
};
double distanceToSegment(QPointF p, QPointF a, QPointF b) {
  const QPointF e = b - a, w = p - a;
  const double length = dotOf(e, e);
  const double t = length > 1e-18 ? std::clamp(dotOf(w, e) / length, 0., 1.) : 0.;
  return std::hypot(w.x() - t * e.x(), w.y() - t * e.y());
}
// A cubic's points, cut in halves until both inner control points lie within
// `tolerance` of the chord (the curve, three quarters of that).
void flattenCubic(QPointF a, QPointF b, QPointF c, QPointF d, double tolerance, int depth, std::vector<QPointF> &points) {
  if (depth >= 18 || std::max(distanceToSegment(b, a, d), distanceToSegment(c, a, d)) <= tolerance) {
    points.push_back(d);
    return;
  }
  const QPointF ab = (a + b) / 2, bc = (b + c) / 2, cd = (c + d) / 2;
  const QPointF abc = (ab + bc) / 2, bcd = (bc + cd) / 2, middle = (abc + bcd) / 2;
  flattenCubic(a, ab, abc, middle, tolerance, depth + 1, points);
  flattenCubic(middle, bcd, cd, d, tolerance, depth + 1, points);
}
OutlinePieces outlinePieces(const std::vector<QPainterPath> &letters, QRectF area, double tolerance) {
  OutlinePieces pieces;
  auto add = [&](QPointF a, QPointF b) {
    if (std::max(a.x(), b.x()) < area.left() || std::min(a.x(), b.x()) > area.right() ||
        std::max(a.y(), b.y()) < area.top() || std::min(a.y(), b.y()) > area.bottom())
      return;
    const int parts = std::max(1, int(std::ceil(std::hypot(b.x() - a.x(), b.y() - a.y()) / OutlinePieces::kCell)));
    for (int k = 0; k < parts; ++k) {
      const QPointF from = a + (b - a) * (double(k) / parts), to = a + (b - a) * (double(k + 1) / parts);
      pieces.ax.push_back(float(from.x()));
      pieces.ay.push_back(float(from.y()));
      pieces.bx.push_back(float(to.x()));
      pieces.by.push_back(float(to.y()));
    }
  };
  std::vector<QPointF> points;
  for (const auto &path : letters) {
    QPointF start, current;
    bool open = false;
    for (int i = 0; i < path.elementCount(); ++i) {
      const auto e = path.elementAt(i);
      const QPointF point(e.x, e.y);
      if (e.isMoveTo()) {
        if (open && current != start) add(current, start);
        start = current = point;
        open = true;
      } else if (e.isLineTo()) {
        add(current, point);
        current = point;
      } else if (e.isCurveTo() && i + 2 < path.elementCount()) {
        const auto second = path.elementAt(i + 1), end = path.elementAt(i + 2);
        points.clear();
        flattenCubic(current, point, QPointF(second.x, second.y), QPointF(end.x, end.y), tolerance, 0, points);
        for (const QPointF &next : points) {
          add(current, next);
          current = next;
        }
        i += 2;
      }
    }
    if (open && current != start) add(current, start);
  }
  if (pieces.empty()) return pieces;
  pieces.left = float(area.left());
  pieces.top = float(area.top());
  pieces.columns = std::max(1, int(std::ceil(area.width() / OutlinePieces::kCell)) + 1);
  pieces.rows = std::max(1, int(std::ceil(area.height() / OutlinePieces::kCell)) + 1);
  const size_t cells = size_t(pieces.columns) * size_t(pieces.rows);
  auto cellRange = [&](size_t m, int &c0, int &c1, int &r0, int &r1) {
    c0 = std::clamp(int(std::floor((std::min(pieces.ax[m], pieces.bx[m]) - pieces.left) / OutlinePieces::kCell)), 0, pieces.columns - 1);
    c1 = std::clamp(int(std::floor((std::max(pieces.ax[m], pieces.bx[m]) - pieces.left) / OutlinePieces::kCell)), 0, pieces.columns - 1);
    r0 = std::clamp(int(std::floor((std::min(pieces.ay[m], pieces.by[m]) - pieces.top) / OutlinePieces::kCell)), 0, pieces.rows - 1);
    r1 = std::clamp(int(std::floor((std::max(pieces.ay[m], pieces.by[m]) - pieces.top) / OutlinePieces::kCell)), 0, pieces.rows - 1);
  };
  pieces.first.assign(cells + 1, 0);
  for (size_t m = 0; m < pieces.ax.size(); ++m) {
    int c0, c1, r0, r1;
    cellRange(m, c0, c1, r0, r1);
    for (int r = r0; r <= r1; ++r)
      for (int c = c0; c <= c1; ++c) ++pieces.first[size_t(r) * size_t(pieces.columns) + size_t(c) + 1];
  }
  for (size_t cell = 0; cell < cells; ++cell) pieces.first[cell + 1] += pieces.first[cell];
  pieces.members.resize(size_t(pieces.first[cells]));
  std::vector<int> filled(pieces.first.begin(), pieces.first.end() - 1);
  for (size_t m = 0; m < pieces.ax.size(); ++m) {
    int c0, c1, r0, r1;
    cellRange(m, c0, c1, r0, r1);
    for (int r = r0; r <= r1; ++r)
      for (int c = c0; c <= c1; ++c) pieces.members[size_t(filled[size_t(r) * size_t(pieces.columns) + size_t(c)]++)] = int(m);
  }
  return pieces;
}
// The corners of one side (outward or inward) whose mitred join can reach
// each cell of 32 pixels: the mitred distance at a pixel is the least over
// them, as mitreField works out over a whole field -- but only where asked,
// leaving the field itself as it is kept.
struct CornerCells {
  static constexpr int kCell = 32;
  int columns = 0, rows = 0;
  std::vector<LetterCorner> corners;
  std::vector<int> first, members;
  bool empty() const { return corners.empty(); }
  float at(int x, int y, double limit) const {
    if (empty()) return 1e30f;
    const size_t cell = size_t(y / kCell) * size_t(columns) + size_t(x / kCell);
    float best = 1e30f;
    const float cut = float(1 / limit);
    for (int k = first[cell]; k < first[cell + 1]; ++k) {
      const LetterCorner &corner = corners[size_t(members[size_t(k)])];
      const float nx1 = float(corner.n1.x()), ny1 = float(corner.n1.y());
      const float nx2 = float(corner.n2.x()), ny2 = float(corner.n2.y());
      const float bx = float(corner.bisector.x()), by = float(corner.bisector.y());
      const float sign = crossOf(corner.n1, corner.n2) > 0 ? 1.f : -1.f;
      const float dx = x + .5f - float(corner.at.x()), dy = y + .5f - float(corner.at.y());
      // Inside the wedge: on n2's side of n1 and n1's side of n2.
      if ((nx1 * dy - ny1 * dx) * sign < 0 || (dx * ny2 - dy * nx2) * sign < 0) continue;
      best = std::min(best, std::max({dx * nx1 + dy * ny1, dx * nx2 + dy * ny2, (dx * bx + dy * by) * cut}) + .5f);
    }
    return best;
  }
};
CornerCells cornerCells(const std::vector<LetterCorner> &all, bool outward, double reach, double limit, int width, int height) {
  CornerCells cells;
  std::vector<QRect> boxes;
  for (const auto &corner : all) {
    if (corner.outward != outward) continue;
    const QPolygonF area = mitreArea(corner, reach, limit);
    if (area.isEmpty()) continue;
    const QRect box = area.boundingRect().toAlignedRect().adjusted(-1, -1, 1, 1).intersected(QRect(0, 0, width, height));
    if (box.isEmpty()) continue;
    cells.corners.push_back(corner);
    boxes.push_back(box);
  }
  if (cells.corners.empty()) return cells;
  cells.columns = (width + CornerCells::kCell - 1) / CornerCells::kCell;
  cells.rows = (height + CornerCells::kCell - 1) / CornerCells::kCell;
  const size_t count = size_t(cells.columns) * size_t(cells.rows);
  cells.first.assign(count + 1, 0);
  for (const QRect &box : boxes)
    for (int r = box.top() / CornerCells::kCell; r <= box.bottom() / CornerCells::kCell; ++r)
      for (int c = box.left() / CornerCells::kCell; c <= box.right() / CornerCells::kCell; ++c)
        ++cells.first[size_t(r) * size_t(cells.columns) + size_t(c) + 1];
  for (size_t cell = 0; cell < count; ++cell) cells.first[cell + 1] += cells.first[cell];
  cells.members.resize(size_t(cells.first[count]));
  std::vector<int> filled(cells.first.begin(), cells.first.end() - 1);
  for (size_t k = 0; k < boxes.size(); ++k)
    for (int r = boxes[k].top() / CornerCells::kCell; r <= boxes[k].bottom() / CornerCells::kCell; ++r)
      for (int c = boxes[k].left() / CornerCells::kCell; c <= boxes[k].right() / CornerCells::kCell; ++c)
        cells.members[size_t(filled[size_t(r) * size_t(cells.columns) + size_t(c)]++)] = int(k);
  return cells;
}
} // namespace
void textStrokeStackPass(QImage &image,const std::vector<TextStrokeStyle> &strokes,
                         double scale,double opacity,const std::vector<QPainterPath> &letters,
                         bool strokeOnly,const std::vector<double> &letterOpacity,
                         const std::vector<double> &letterScale,std::vector<uint16_t> *owners,
                         QRect within) {
  if(image.isNull())return;
  std::vector<TextStrokeStyle> active;
  for(const auto &stroke:strokes)
    if(stroke.width>0 && stroke.color.alpha()>0) active.push_back(stroke);
  const float fade=float(std::clamp(opacity,0.,1.));
  if(active.empty() && strokeOnly) {
    // Nothing but strokes, and no strokes: nothing.
    image.fill(Qt::transparent);
    return;
  }
  if(active.empty()) {
    if(fade<1) {
      const auto format=image.format();
      QImage work=image.convertToFormat(QImage::Format_RGBA32FPx4);
      if(work.isNull())return;
      auto *bits=work.bits();const auto stride=work.bytesPerLine();const int width=work.width();
      renderParallel(work.height(),[&](int begin,int end){for(int y=begin;y<end;++y){
        auto *p=reinterpret_cast<float *>(bits+y*stride);
        for(int x=0;x<width;++x) p[x*4+3]*=fade;
      }});
      image=work.convertToFormat(format);
    }
    return;
  }
  // How far each stroke reaches from the edge of the letters, out and in, in
  // the raster's own pixels. Stroke zero sits against the letters and each
  // one after it sits beyond the last, exactly as the stack has always meant.
  struct Band { float outside=0,inside=0,r=0,g=0,b=0,a=0; bool mitred=false; };
  std::vector<Band> bands;
  bool anyOutside=false,anyInside=false;
  double outsideSoFar=0,insideSoFar=0,mitredOutside=0,mitredInside=0;
  std::vector<float> outsideEdges,insideEdges;
  for(const auto &stroke:active) {
    const double width=std::max(0.,stroke.width);
    const int mode=std::clamp(stroke.mode,0,2);
    outsideSoFar+=mode==1?width:mode==2?width/2:0;
    insideSoFar+=mode==0?width:mode==2?width/2:0;
    Band band;
    band.outside=mode==0?0:float(outsideSoFar*scale);
    band.inside=mode==1?0:float(insideSoFar*scale);
    band.r=float(stroke.color.redF());band.g=float(stroke.color.greenF());
    band.b=float(stroke.color.blueF());band.a=float(stroke.color.alphaF());
    band.mitred=stroke.join==0 && !letters.empty();
    anyOutside|=band.outside>0;anyInside|=band.inside>0;
    if(band.outside>0) outsideEdges.push_back(band.outside);
    if(band.inside>0) insideEdges.push_back(band.inside);
    if(band.mitred) {
      mitredOutside=std::max(mitredOutside,double(band.outside));
      mitredInside=std::max(mitredInside,double(band.inside));
    }
    bands.push_back(band);
  }
  const float outsideMost=float(outsideSoFar*scale);
  // An 8-bit premultiplied raster -- an 8 bpc layer's (RGBA8888) or a picture
  // QPainter made (ARGB32) -- is worked on where it lies, a pixel at a time;
  // any other as straight float, as before. Made float and back whole, a
  // large raster spent more on that than on its strokes. Both keep alpha in
  // the fourth byte; red and blue change places.
  const int w=image.width(),h=image.height();
  const auto format=image.format();
  const bool eightBit=format==QImage::Format_RGBA8888_Premultiplied || format==QImage::Format_ARGB32_Premultiplied;
  const int redAt=format==QImage::Format_RGBA8888_Premultiplied?0:2,blueAt=2-redAt;
  QImage work;
  if(!eightBit) {
    work=image.convertToFormat(QImage::Format_RGBA32FPx4);
    if(work.isNull())return;
  }
  uchar *const base=eightBit?image.bits():work.bits();
  const qsizetype stride=eightBit?image.bytesPerLine():work.bytesPerLine();
  // The letters' pixels as the field counts them, half covered or more: read
  // once, a byte each.
  std::vector<uint8_t> inside(size_t(w)*h);
  renderParallel(h,[&](int begin,int end){for(int y=begin;y<end;++y){
    uint8_t *row=inside.data()+size_t(y)*w;
    if(eightBit) {
      const uchar *p=base+y*stride;
      for(int x=0;x<w;++x) row[x]=p[x*4+3]>=128;
    } else {
      const auto *p=reinterpret_cast<const float *>(base+y*stride);
      for(int x=0;x<w;++x) row[x]=p[x*4+3]>=.5f;
    }
  }});
  // Letters of an animated text, each at its own opacity and size: every pixel
  // belongs to the letter nearest it, its strokes are drawn at that letter's
  // size, and the whole of what is there -- strokes, letter, inner strokes --
  // shows at that letter's opacity, as one picture fades, not each pass on its
  // own over the last. The letter nearest a pixel is the one the outside field
  // measured to, so no second field is needed to find it.
  const bool labelled=!letters.empty() && letterOpacity.size()==letters.size() &&
                      letterScale.size()==letters.size();
  // Which letter each pixel went with, for whoever asked.
  const bool tellOwners=owners && labelled && letters.size()<65535;
  if(owners) owners->assign(tellOwners?size_t(w)*h:0,0);
  QImage ids;
  double largest=1;
  if(labelled) {
    ids=QImage(w,h,QImage::Format_RGB32);
    if(ids.isNull())return;
    // In bands of rows on the cores at once, each through a view of its own
    // rows: one painter over a raster of a title at 900% (twelve megapixels,
    // a wide stroke's reach round what shows) took 20-30 ms a frame. Every
    // band strokes each outline reaching it afresh, so there are no more bands
    // than two for each core, and one, all of it, where no cores are to be had
    // (inside a worker, or a lane kept to one). The outlines, drawn by every
    // band, are made ready first.
    for(const auto &letter:letters) readyPathForThreads(letter);
    uchar *const idBits=ids.bits();
    const qsizetype idStride=ids.bytesPerLine();
    const int cores=inRenderWorker?1:renderWorkerLimit();
    const int idBands=cores<=1?1:std::clamp((h+63)/64,1,2*cores);
    const int idBand=(h+idBands-1)/idBands;
    renderParallelEach(idBands,[&](int n) {
      const int top=n*idBand,rows=std::min(idBand,h-top);
      if(rows<=0) return;
      std::memset(idBits+top*idStride,0,size_t(rows)*size_t(idStride));
      QImage part(idBits+top*idStride,w,rows,idStride,QImage::Format_RGB32);
      QPainter q(&part);
      q.setRenderHint(QPainter::Antialiasing,false);
      q.translate(0,-top);
      for(size_t i=0;i<letters.size();++i) {
        const QRectF reach=letters[i].controlPointRect().adjusted(-2,-2,2,2);
        if(reach.bottom()<top || reach.top()>top+rows) continue;
        const QColor id=QColor::fromRgb(QRgb(0xff000000u|quint32(i+1)));
        q.setPen(QPen(id,1.5));q.setBrush(id);q.drawPath(letters[i]);
      }
    });
    for(double grow:letterScale) largest=std::max(largest,grow);
  }
  // Only what can show is measured and shaded -- measured to every letter.
  const QRect area=within.isValid()?within.intersected(QRect(0,0,w,h)):QRect(0,0,w,h);
  if(area.isEmpty())return;
  const quint64 signature=insideSignature(inside,w,h);
  const SiteFieldPtr out=anyOutside?cachedSiteField(inside,w,h,true,labelled,signature,area):SiteFieldPtr{};
  const SiteFieldPtr in=anyInside?cachedSiteField(inside,w,h,false,false,signature,area):SiteFieldPtr{};
  // The outlines for the exact edge, and the corners a Miter stroke keeps
  // sharp, out to the tips of their joins.
  double widest=0;
  for(const auto &band:bands) widest=std::max({widest,double(band.outside),double(band.inside)});
  const double reach=widest*largest+4;
  const OutlinePieces pieces=letters.empty()?OutlinePieces():
      outlinePieces(letters,QRectF(0,0,w,h).adjusted(-reach,-reach,reach,reach),.04);
  const double miterLimit=4;
  CornerCells outCorners,inCorners;
  if(mitredOutside>0 || mitredInside>0) {
    const double cornerReach=std::max(mitredOutside,mitredInside)*largest+2;
    const QRectF area=QRectF(0,0,w,h).adjusted(-cornerReach*miterLimit,-cornerReach*miterLimit,
                                               cornerReach*miterLimit,cornerReach*miterLimit);
    const auto corners=letterCorners(letters,area);
    if(mitredOutside>0 && out) outCorners=cornerCells(corners,true,mitredOutside*largest+2,miterLimit,w,h);
    if(mitredInside>0 && in) inCorners=cornerCells(corners,false,mitredInside*largest+2,miterLimit,w,h);
  }
  // The field's distance at a pixel, or -- where an edge of one of `edges`
  // passes through it -- the distance to the outlines themselves, in the
  // field's terms (half a pixel past the outline). Never more than two
  // pixels from what the field says: a pixel the field puts somewhere
  // else entirely (a letter's texture left a hole, the outlines carry no
  // such shape) keeps the field's word.
  // The field measures to the centres of the pixels that count as the
  // letters': from about a third of a pixel short of the outline's own
  // distance to a pixel past it. So a band's soft edge -- its distance from
  // a pixel short of the band to the band itself -- lies where the field
  // reads from a pixel and a half short to a pixel past, and the outline in
  // the ring from a pixel short of the field's distance to a third past it.
  auto measured=[&](const SiteField &field,const std::vector<float> &edges,int x,int y,float grow) {
    const float coarse=field.distance[field.at(x,y)];
    if(pieces.empty()) return coarse;
    bool edge=false;
    for(float width:edges) {
      const float off=coarse-width*grow;
      if(off>=-1.5f && off<=1.1f) { edge=true; break; }
    }
    if(!edge) return coarse;
    const float exact=pieces.nearestInRing(x+.5f,y+.5f,coarse-1.1f,coarse+.45f)+.5f;
    return std::abs(exact-coarse)<=2.f?exact:coarse;
  };
  // A Miter join only reaches past where a round one does: a pixel the
  // narrowest mitred band covers whole already has every band it can have.
  float narrowestMitredOutside=1e30f,narrowestMitredInside=1e30f;
  for(const auto &band:bands)
    if(band.mitred) {
      if(band.outside>0) narrowestMitredOutside=std::min(narrowestMitredOutside,band.outside);
      if(band.inside>0) narrowestMitredInside=std::min(narrowestMitredInside,band.inside);
    }
  // One pixel, straight colour in and out; false where nothing reaches it,
  // which stays as it is.
  auto shade=[&](size_t i,int x,int y,float &r,float &g,float &b,float &a) {
    const float letter=std::clamp(a,0.f,1.f);
    int own=-1;
    if(labelled) {
      const qint32 from=inside[i]?qint32(i):(out?out->site[out->at(x,y)]:-1);
      if(from>=0) own=int(reinterpret_cast<const QRgb *>(ids.constScanLine(from/w))[from%w]&0xffffff)-1;
      if(own>=int(letters.size())) own=-1;
      if(tellOwners) (*owners)[i]=uint16_t(own+1);
    }
    const float grow=own>=0?float(letterScale[size_t(own)]):1.f;
    const float show=own>=0?float(std::clamp(letterOpacity[size_t(own)],0.,1.)):1.f;
    // An opaque letter pixel with nothing drawn over it: the outside strokes
    // under it do not show, so it is the letter, faded with it.
    if(letter>=1 && !strokeOnly && !in) {
      a=fade*show;
      return true;
    }
    float outRound=1e6f,outMitred=1e6f,inRound=1e6f,inMitred=1e6f;
    if(out) {
      const float coarse=out->distance[out->at(x,y)];
      const float mitre=outCorners.empty() || coarse<narrowestMitredOutside*grow-1.5f
                            ? 1e30f : outCorners.at(x,y,miterLimit);
      // Past every outside stroke and none of the letters: nothing.
      if(letter<=0 && std::min(coarse,mitre)>outsideMost*grow+2.5f) return false;
      // Measured out from the letters only where the pixel lies outside them;
      // the letters' own pixels are at nought.
      outRound=inside[i]?coarse:measured(*out,outsideEdges,x,y,grow);
      outMitred=std::min(outRound,mitre);
    } else if(letter<=0) return false;
    if(in) {
      const float inward=in->distance[in->at(x,y)];
      inRound=inside[i]?measured(*in,insideEdges,x,y,grow):inward;
      inMitred=inCorners.empty() || inward<narrowestMitredInside*grow-1.5f
                   ? inRound : std::min(inRound,inCorners.at(x,y,miterLimit));
    }
    float cr=0,cg=0,cb=0,ca=0;
    auto over=[&](float sr,float sg,float sb,float sa){
      if(sa<=0)return;
      // Opaque on top, or nothing under it: it, as it is -- what the sums
      // below come to, without them. Most pixels of a wide stroke are one
      // band, whole.
      if(sa>=1 || ca<=0) {
        cr=sr;cg=sg;cb=sb;ca=std::min(sa,1.f);
        return;
      }
      const float next=sa+ca*(1-sa);
      if(next<=0)return;
      const float k=1.f/next;
      cr=(sr*sa+cr*ca*(1-sa))*k;cg=(sg*sa+cg*ca*(1-sa))*k;cb=(sb*sa+cb*ca*(1-sa))*k;
      ca=next;
    };
    // Outside bands, widest first, so each narrower one restores the band
    // inside it. They reach under the letters, which cover them next.
    if(out) for(int k=int(bands.size())-1;k>=0;--k) {
      const auto &band=bands[size_t(k)];
      if(band.outside<=0)continue;
      const float distance=band.mitred?outMitred:outRound;
      // Stroke only: the part under the letters goes with them.
      over(band.r,band.g,band.b,std::clamp(band.outside*grow+.5f-distance,0.f,1.f)*band.a*(strokeOnly?1-letter:1.f));
    }
    // The letters themselves, keeping whatever colour each one was given.
    if(!strokeOnly) over(r,g,b,letter);
    // Inside bands on top, held to the letters' own coverage.
    if(in) for(int k=int(bands.size())-1;k>=0;--k) {
      const auto &band=bands[size_t(k)];
      if(band.inside<=0)continue;
      const float distance=band.mitred?inMitred:inRound;
      float cover=std::clamp(band.inside*grow+.5f-distance,0.f,1.f)*letter;
      // A Center stroke runs both sides of the edge. On the edge's own
      // smoothed pixels the letter covers part of its outer half and the
      // inner half is held to the letter's share, so the two halves left the
      // letter's colour showing between them: a hairline down the middle of
      // the stroke. There the outer half's share makes up the rest.
      if(band.outside>0 && letter>0 && letter<1 && out)
        cover+=std::clamp(band.outside*grow+.5f-(band.mitred?outMitred:outRound),0.f,1.f)*(1-letter);
      over(band.r,band.g,band.b,std::min(cover,1.f)*band.a);
    }
    r=cr;g=cg;b=cb;a=ca*fade*show;
    return true;
  };
  // An opaque letter pixel with nothing over it stays as it is: the outside
  // strokes lie under it.
  const bool lettersOnTop=!in && !strokeOnly && !labelled && fade>=1;
  const int left=area.left(),right=area.left()+area.width();
  renderParallel(area.height(),[&](int begin,int end){for(int y=area.top()+begin;y<area.top()+end;++y){
    uchar *line=base+y*stride;
    if(eightBit) {
      for(int x=left;x<right;++x) {
        uchar *pixel=line+x*4;
        const int alpha=pixel[3];
        if(alpha==255 && lettersOnTop) continue;
        float a=alpha/255.f,r=0,g=0,b=0;
        if(alpha) {
          const float k=1.f/float(alpha);
          r=pixel[redAt]*k;g=pixel[1]*k;b=pixel[blueAt]*k;
        }
        if(!shade(size_t(y)*w+x,x,y,r,g,b,a)) continue;
        a=std::clamp(a,0.f,1.f);
        const float scaleBy=a*255.f;
        pixel[redAt]=uchar(std::clamp(r,0.f,1.f)*scaleBy+.5f);
        pixel[1]=uchar(std::clamp(g,0.f,1.f)*scaleBy+.5f);
        pixel[blueAt]=uchar(std::clamp(b,0.f,1.f)*scaleBy+.5f);
        pixel[3]=uchar(scaleBy+.5f);
      }
    } else {
      auto *p=reinterpret_cast<float *>(line);
      for(int x=left;x<right;++x) {
        float r=p[x*4],g=p[x*4+1],b=p[x*4+2],a=p[x*4+3];
        if(a>=1 && lettersOnTop) continue;
        if(!shade(size_t(y)*w+x,x,y,r,g,b,a)) {
          p[x*4]=p[x*4+1]=p[x*4+2]=p[x*4+3]=0;
          continue;
        }
        p[x*4]=r;p[x*4+1]=g;p[x*4+2]=b;p[x*4+3]=a;
      }
    }
  }});
  if(!eightBit) image=work.convertToFormat(format);
}
// ---------------------------------------------------------------------------
// Noise, Turbulent Noise and Displacement Map.
// ---------------------------------------------------------------------------
namespace {
thread_local double g_effectSeconds = 0, g_effectFrame = 0;
} // namespace

EffectTimeScope::EffectTimeScope(double seconds, double frame) : previous(g_effectSeconds), previousFrame(g_effectFrame) { g_effectSeconds = seconds; g_effectFrame = frame; }
EffectTimeScope::~EffectTimeScope() { g_effectSeconds = previous; g_effectFrame = previousFrame; }
double currentEffectSeconds() { return g_effectSeconds; }
double currentEffectFrame() { return g_effectFrame; }

// The effects whose picture changes with time: a moving noise, or a slider with
// keys. Their rasters must not be reused from one frame to the next, and
// neither may a frame be taken for its neighbour.
bool layerHasAnimatedEffect(const Layer &l) { return layerEmitsParticles(l) || layerPackEffectMoves(l, 0); }

// Every effect the layer applies, in its order, on straight float pixels and
// never on the card: the slow, plain way, kept for the checks to measure the
// faster ones against.
QImage applySurfaceEffects(QImage image,const Layer &l,QRectF box,double scale) {
  auto format=image.format(); image=image.convertToFormat(QImage::Format_RGBA32FPx4);
  for(const auto &name:layerEffectOrder(l)) {
    if(!l.appliedEffects[name].toBool() || l.hiddenEffects[name].toBool())continue;
    if(name=="Fill")fillPass(image,l);
    else if(name=="Gradient")gradientPass(image,l,box);
    else if(name=="Stroke")strokePass(image,l,scale);
    else if(const PackEffect *pack=packEffect(name)) {
      const auto values=packEffectValues(l,*pack,g_effectFrame);
      if(!packEffectInert(*pack,values))runPackEffect(image,*pack,values,box,scale,g_effectSeconds,g_effectFrame,&l);
    }
  }
  return image.convertToFormat(format);
}
int checkSurfaceEffects(QStringList &failures) {
  {
    // Colour grades over 8-bit pixels give what the float passes give, to
    // within one 8-bit step, on opaque, faint and see-through pixels alike,
    // in both 8-bit layouts; and only the grades at the head of the stack are
    // taken, so an effect between two grades still runs in its place.
    QImage source(64, 48, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y)
      for (int x = 0; x < source.width(); ++x) {
        const int alpha = y < 16 ? 255 : y < 32 ? 40 + x * 3 : (x % 3 == 0 ? 0 : 128);
        source.setPixelColor(x, y, QColor((x * 37 + y * 11) % 256, (x * 13 + y * 29) % 256, (x * 7 + y * 53) % 256, alpha));
      }
    Layer graded;
    graded.surfaceEffects["HSL"] = QJsonObject{{"hue", 40.}, {"saturation", 20.}, {"lightness", -10.}};
    graded.surfaceEffects["Levels"] = QJsonObject{{"inputBlack", 12.}, {"inputWhite", 240.}, {"gamma", 1.4}};
    graded.surfaceEffects["Exposure"] = QJsonObject{{"exposure", .4}, {"gamma", .8}, {"offset", .01}};
    graded.surfaceEffects["Curves"] = QJsonObject{{"master", QJsonArray{0, .3, .55, .8, 1}}};
    graded.surfaceEffects["Brightness & Contrast"] = QJsonObject{{"brightness", 10.}, {"contrast", 25.}};
    graded.surfaceEffects["Vibrance"] = QJsonObject{{"vibrance", 40.}, {"saturation", 10.}};
    graded.surfaceEffects["Color Balance"] = QJsonObject{{"shadowsRed", 20.}, {"highlightsBlue", -15.}};
    graded.surfaceEffects["Three-Way Color Corrector"] = QJsonObject{{"lift", .03}, {"gamma", 1.2}, {"gain", 1.1}, {"saturation", 15.}};
    graded.surfaceEffects["Temperature & Tint"] = QJsonObject{{"temperature", 30.}, {"tint", -10.}};
    graded.surfaceEffects["Black & White"] = QJsonObject{{"tintAmount", 30.}, {"tintColor", "#ffcc88"}};
    auto worst = [](const QImage &a, const QImage &b) {
      const QImage x = a.convertToFormat(QImage::Format_ARGB32_Premultiplied), y = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
      int most = 0;
      for (int row = 0; row < x.height(); ++row)
        for (int col = 0; col < x.width(); ++col) {
          const QRgb p = x.pixel(col, row), q = y.pixel(col, row);
          most = std::max({most, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                           std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
      return most;
    };
    const QStringList names{"HSL", "Levels", "Exposure", "Curves", "Brightness & Contrast", "Vibrance",
                            "Color Balance", "Three-Way Color Corrector", "Temperature & Tint", "Black & White"};
    for (const auto &name : names) {
      Layer one = graded;
      one.appliedEffects = QJsonObject{{name, true}};
      const QImage reference = applySurfaceEffects(source, one, {0, 0, 64, 48}, 1);
      for (const auto format : {QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA8888_Premultiplied}) {
        QImage fast = source.convertToFormat(format);
        const auto used = gradeLeadingEightBit(fast, one, 0, 0, {0, 0, 64, 48}, 1);
        const int difference = worst(fast, reference);
        if (!used.contains(name) || difference > 1)
          failures << QString("%1 over 8-bit %2 pixels differs from the float pass by %3 steps")
                          .arg(name, format == QImage::Format_ARGB32_Premultiplied ? "ARGB" : "RGBA").arg(difference);
      }
    }
    Layer chain = graded;
    chain.appliedEffects = QJsonObject{{"HSL", true}, {"Levels", true}, {"Vibrance", true}};
    chain.effectOrder = QJsonArray{"HSL", "Levels", "Vibrance"};
    QImage fused = source;
    const auto used = gradeLeadingEightBit(fused, chain, 0, 0, {0, 0, 64, 48}, 1);
    if (used.size() != 3 || worst(fused, applySurfaceEffects(source, chain, {0, 0, 64, 48}, 1)) > 1)
      failures << "three grades in a row over 8-bit pixels differ from the float passes";
    Layer interrupted = chain;
    interrupted.appliedEffects["Vignette"] = true;
    interrupted.effectOrder = QJsonArray{"HSL", "Vignette", "Levels", "Vibrance"};
    QImage head = source;
    const auto taken = gradeLeadingEightBit(head, interrupted, 0, 0, {0, 0, 64, 48}, 1);
    if (taken != QSet<QString>{"HSL"})
      failures << "an effect between two grades lets the grades after it run early";
    QImage wide = source.convertToFormat(QImage::Format_RGBA32FPx4);
    if (!gradeLeadingEightBit(wide, chain, 0, 0, {0, 0, 64, 48}, 1).isEmpty()) failures << "a float raster is taken for an 8-bit one";
  }
  {
    // Handwriting: the pen shows what it has passed and no more. Counted as
    // ink in each half of the line, so it holds for whatever face the machine
    // running this happens to have (and says so when it has none at all).
    Scene scene; scene.width = 900; scene.height = 240; scene.frames = 10;
    Layer text; text.type = "text"; text.text = "Wavy Flo Wavy Flo"; text.fontSize = 70;
    text.color = QColor(0, 0, 0); text.duration = 10;
    text.p[0].base = 450; text.p[1].base = 120;
    text.characterAnimation = QJsonObject{{"enabled", true}, {"write", true}, {"writeNib", 45.},
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
    const int wholeLeft = ink(1., 0, 450), wholeRight = ink(1., 450, 900);
    if (wholeLeft == 0 && wholeRight == 0) {
      // No typeface on this machine: there is no ink to reveal, so nothing here
      // can be told apart. Not a failure of the handwriting.
    } else {
      if (ink(0., 0, 900) != 0) failures << "Handwriting draws nothing before the pen starts";
      const int partLeft = ink(.35, 0, 450), partRight = ink(.35, 450, 900);
      if (partLeft == 0) failures << "Handwriting has written the start of the line a third of the way through";
      if (partRight >= wholeRight / 2) failures << "Handwriting leaves the end of the line for later";
      if (ink(1., 0, 450) != wholeLeft || partLeft > wholeLeft)
        failures << "Handwriting writes no more than the letters themselves";
    }
    // The Out strength takes the writing away again, from the start, in the
    // order it was laid down: with In full and Out a third of the way, the
    // beginning of the line is gone and the end still stands.
    if (wholeLeft != 0 || wholeRight != 0) {
      Scene erasing = scene; erasing.layers = {text};
      erasing.layers[0].p[6].base = 100; erasing.layers[0].p[7].base = 35;
      const QImage image = renderCPU(erasing, 0, false).convertToFormat(QImage::Format_ARGB32);
      int left = 0, right = 0;
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
          if (qAlpha(image.pixel(x, y)) > 40) { if (x < 450) ++left; else ++right; }
      if (left >= wholeLeft / 2) failures << "Handwriting Out takes the start of the line away";
      if (right < wholeRight / 2) failures << "Handwriting Out leaves the end of the line standing";
      erasing.layers[0].p[7].base = 100;
      const QImage gone = renderCPU(erasing, 0, false).convertToFormat(QImage::Format_ARGB32);
      int any = 0;
      for (int y = 0; y < gone.height(); ++y)
        for (int x = 0; x < gone.width(); ++x) if (qAlpha(gone.pixel(x, y)) > 40) ++any;
      if (any != 0) failures << "Handwriting Out at the end leaves nothing behind";
    }
  }
  {
    // Stroke: the outline is laid outside the shape's edge, the inside keeps
    // the colour it had (the pass now leaves those pixels alone rather than
    // passing them through the blend), and it reaches as far as its width.
    CpuPixelScope cpu;
    QImage image(120,120,QImage::Format_RGBA32FPx4);
    image.fill(Qt::transparent);
    { QPainter p(&image); p.fillRect(30,30,60,60,QColor(255,0,0)); }
    Layer layer;layer.type="image";layer.appliedEffects["Stroke"]=true;
    layer.surfaceEffects["Stroke"]=QJsonObject{{"width",8.},{"mode",1},{"color","#00ff00"}};
    const QImage done=applyLayerEffects(image,layer,{0,0,120,120},1).convertToFormat(QImage::Format_ARGB32);
    const QColor middle=done.pixelColor(60,60),just_outside=done.pixelColor(26,60),
                 far_outside=done.pixelColor(14,60),corner=done.pixelColor(2,2);
    if(middle!=QColor(255,0,0))failures<<"Stroke leaves the inside of the shape as it was";
    if(just_outside.green()<200||just_outside.alpha()<200)failures<<"Stroke paints outside the edge";
    if(far_outside.alpha()>10)failures<<"Stroke reaches no further than its width";
    if(corner.alpha()>0)failures<<"Stroke leaves the empty corners empty";
  }
  {
    // Turbulent Noise: the same settings give the same picture (the field is
    // kept and reused), another seed a different one, and a scale so small that
    // its finest layers are finer than a pixel still gives real texture.
    CpuPixelScope cpu;
    auto noisy=[](double scaleValue,int seed,double seconds) {
      QImage image(96,96,QImage::Format_RGBA32FPx4);image.fill(Qt::white);
      Layer layer;layer.type="image";layer.appliedEffects["Turbulent Noise"]=true;
      layer.surfaceEffects["Turbulent Noise"]=QJsonObject{{"scale",scaleValue},{"complexity",8.},{"seed",seed},{"speed",1.}};
      EffectTimeScope time(seconds,0);
      return applyLayerEffects(image,layer,{0,0,96,96},1).convertToFormat(QImage::Format_ARGB32);
    };
    const QImage first=noisy(6,1,.5),again=noisy(6,1,.5),otherSeed=noisy(6,2,.5),later=noisy(6,1,1.5);
    if(first!=again)failures<<"Turbulent Noise repeats itself for the same settings";
    if(first==otherSeed)failures<<"Turbulent Noise seed changes the picture";
    if(first==later)failures<<"Turbulent Noise moves with time";
    int lowest=255,highest=0;
    for(int y=0;y<96;++y)for(int x=0;x<96;++x){const int g=qGray(first.pixel(x,y));lowest=std::min(lowest,g);highest=std::max(highest,g);}
    if(highest-lowest<60)failures<<"Turbulent Noise at a tiny scale still has contrast";
  }
  {
    // Linear and Radial Wipe: completion 0 leaves the picture whole, 100 takes
    // all of it, and in between the edge sits where the completion says.
    CpuPixelScope cpu;
    auto wiped=[](const QString &name,int first,double completion,double angle,double start) {
      QImage image(100,100,QImage::Format_RGBA32FPx4);image.fill(Qt::white);
      Layer layer;layer.type="image";layer.appliedEffects[name]=true;
      layer.p[first].base=completion;
      if(name=="Linear Wipe")layer.p[WipeLinearAngle].base=angle;else layer.p[WipeRadialStart].base=start;
      EffectTimeScope time(0,0);
      return applyLayerEffects(image,layer,{0,0,100,100},1);
    };
    auto none=wiped("Linear Wipe",WipeLinearCompletion,0,0,0),half=wiped("Linear Wipe",WipeLinearCompletion,50,0,0),
         all=wiped("Linear Wipe",WipeLinearCompletion,100,0,0);
    if(none.pixelColor(2,50).alphaF()<.99||none.pixelColor(97,50).alphaF()<.99)failures<<"Linear Wipe at 0% leaves the layer whole";
    if(all.pixelColor(2,50).alphaF()>.01||all.pixelColor(97,50).alphaF()>.01)failures<<"Linear Wipe at 100% takes all of it";
    if(half.pixelColor(20,50).alphaF()>.01||half.pixelColor(80,50).alphaF()<.99)failures<<"Linear Wipe at 50% sweeps from the left (angle 0)";
    auto rnone=wiped("Radial Wipe",WipeRadialCompletion,0,0,0),rhalf=wiped("Radial Wipe",WipeRadialCompletion,50,0,0),
         rall=wiped("Radial Wipe",WipeRadialCompletion,100,0,0);
    if(rnone.pixelColor(75,25).alphaF()<.99)failures<<"Radial Wipe at 0% leaves the layer whole";
    if(rall.pixelColor(75,25).alphaF()>.01||rall.pixelColor(25,25).alphaF()>.01)failures<<"Radial Wipe at 100% takes all of it";
    // Clockwise from straight up: the right half goes first, the left half stays.
    if(rhalf.pixelColor(80,50).alphaF()>.01||rhalf.pixelColor(20,50).alphaF()<.99)failures<<"Radial Wipe at 50% has swept the right half";
  }
  {
    // The compact colour-correction suite must alter straight RGB while
    // preserving alpha. These probes also exercise the defaults used by old
    // projects that do not yet carry settings for the new effects.
    CpuPixelScope cpu;
    auto corrected=[](QColor input,const QString &name,const QJsonObject &settings) {
      QImage image(1,1,QImage::Format_RGBA32FPx4);image.fill(input);
      Layer layer;layer.type="image";layer.appliedEffects[name]=true;
      layer.surfaceEffects[name]=settings;
      return applyLayerEffects(image,layer,{0,0,1,1},1).pixelColor(0,0);
    };
    const QJsonArray identity{0.,.25,.5,.75,1.};
    auto curves=corrected(QColor::fromRgbF(.25,.25,.25,.4),"Curves",
        QJsonObject{{"master",identity},{"red",QJsonArray{1.,.75,.5,.25,0.}},
                    {"green",identity},{"blue",identity}});
    if(curves.redF()<.7 || std::abs(curves.alphaF()-.4)>.01)failures<<"Curves channel and alpha";
    auto hsl=corrected(Qt::red,"HSL",QJsonObject{{"hue",120.},{"saturation",0.},{"lightness",0.}});
    if(hsl.greenF()<.9 || hsl.redF()>.1)failures<<"HSL hue rotation";
    auto levels=corrected(QColor::fromRgbF(.25,.25,.25),"Levels",
        QJsonObject{{"inputBlack",64.},{"inputWhite",255.},{"gamma",1.},{"outputBlack",0.},{"outputWhite",255.}});
    if(levels.redF()>.02)failures<<"Levels input range";
    auto exposure=corrected(QColor::fromRgbF(.25,.25,.25),"Exposure",
        QJsonObject{{"exposure",1.},{"offset",0.},{"gamma",1.}});
    if(std::abs(exposure.redF()-.5)>.02)failures<<"Exposure stops";
    auto brightness=corrected(QColor::fromRgbF(.5,.5,.5),"Brightness & Contrast",
        QJsonObject{{"brightness",25.},{"contrast",0.}});
    if(std::abs(brightness.redF()-.75)>.02)failures<<"Brightness correction";
    auto vibrance=corrected(QColor::fromRgbF(.55,.45,.45),"Vibrance",
        QJsonObject{{"vibrance",100.},{"saturation",0.}});
    if(vibrance.redF()-vibrance.greenF()<=.1)failures<<"Vibrance selective saturation";
    auto balance=corrected(QColor::fromRgbF(.15,.15,.15),"Color Balance",
        QJsonObject{{"shadowsRed",100.},{"preserveLuminosity",false}});
    if(balance.redF()<=balance.greenF())failures<<"Color Balance shadows";
    auto threeWay=corrected(QColor::fromRgbF(.8,.8,.8),"Three-Way Color Corrector",
        QJsonObject{{"highlights","#0000ff"},{"lift",0.},{"gamma",1.},{"gain",1.},
                    {"saturation",0.},{"balance",0.}});
    if(threeWay.blueF()<=threeWay.redF())failures<<"Three-Way highlights wheel";
    auto warmth=corrected(QColor::fromRgbF(.5,.5,.5),"Temperature & Tint",
        QJsonObject{{"temperature",100.},{"tint",0.}});
    if(warmth.redF()<=warmth.blueF())failures<<"Temperature correction";
    auto monochrome=corrected(QColor::fromRgbF(.8,.25,.1),"Black & White",QJsonObject{});
    if(std::abs(monochrome.redF()-monochrome.greenF())>.001 ||
       std::abs(monochrome.greenF()-monochrome.blueF())>.001)failures<<"Black & White output";

    Layer gpuLayer;gpuLayer.type="image";
    const QStringList gpuEffects{"Curves","HSL","Levels","Exposure","Brightness & Contrast",
        "Vibrance","Color Balance","Three-Way Color Corrector","Temperature & Tint","Black & White"};
    for(const auto &name:gpuEffects)gpuLayer.appliedEffects[name]=true;
    gpuLayer.surfaceEffects["Curves"]=QJsonObject{{"master",QJsonArray{0.,.2,.5,.8,1.}}};
    gpuLayer.surfaceEffects["HSL"]=QJsonObject{{"hue",10.}};
    gpuLayer.surfaceEffects["Levels"]=QJsonObject{{"inputBlack",5.}};
    gpuLayer.surfaceEffects["Exposure"]=QJsonObject{{"exposure",.25}};
    gpuLayer.surfaceEffects["Brightness & Contrast"]=QJsonObject{{"contrast",5.}};
    gpuLayer.surfaceEffects["Vibrance"]=QJsonObject{{"vibrance",5.}};
    gpuLayer.surfaceEffects["Color Balance"]=QJsonObject{{"shadowsRed",5.}};
    gpuLayer.surfaceEffects["Three-Way Color Corrector"]=QJsonObject{{"highlights","#6688ff"},{"lift",.01},{"saturation",5.}};
    gpuLayer.surfaceEffects["Temperature & Tint"]=QJsonObject{{"temperature",5.}};
    // Each grade goes to the card as its own pack's shader, in the same chain.
    const auto passes=layerEffectPasses(gpuLayer,{0,0,16,16},1,{16,16},false);
    int shaded=0;
    for(const auto &pass:passes)shaded+=!pass.source.isEmpty();
    if(shaded!=gpuEffects.size())
      failures<<QString("%1 of %2 colour grades reached the card chain as shaders").arg(shaded).arg(gpuEffects.size());
  }
  {
    // Every pixel effect consumes the preceding result, irrespective of order.
    const QStringList names{"Fill","Gradient","Tint","Stroke","Glow","Drop Shadow","Gaussian Blur"};
    QImage base(65,65,QImage::Format_RGBA32FPx4);base.fill(Qt::transparent);
    {QPainter p(&base);p.fillRect(25,25,15,15,QColor(255,255,255,200));}
    for(const auto &first:names)for(const auto &second:names){
      if(first==second)continue;
      Layer l;l.type="image";l.color=Qt::white;
      l.appliedEffects[first]=true;l.appliedEffects[second]=true;
      l.glow=(first=="Glow"||second=="Glow")?4:0;
      l.shadow=(first=="Drop Shadow"||second=="Drop Shadow")?4:0;
      l.blur=(first=="Gaussian Blur"||second=="Gaussian Blur")?4:0;
      l.surfaceEffects["Fill"]=QJsonObject{{"color","#ff0000"},{"opacity",65}};
      l.surfaceEffects["Gradient"]=QJsonObject{{"start","#00ff00"},{"end","#0000ff"},{"amount",75}};
      l.surfaceEffects["Tint"]=QJsonObject{{"color","#00ffff"},{"amount",65}};
      l.surfaceEffects["Stroke"]=QJsonObject{{"color","#ffff00"},{"width",3},{"mode",1}};
      l.effectOrder=QJsonArray{first,second};
      auto actual=applyLayerEffects(base,l,{0,0,65,65},1);
      auto expected=base;
      for(const auto &name:{first,second}){
        bool spatial=name=="Glow"||name=="Drop Shadow"||name=="Gaussian Blur";
        expected=expected.convertToFormat(spatial?QImage::Format_RGBA32FPx4_Premultiplied:QImage::Format_RGBA32FPx4);
        if(name=="Fill")fillPass(expected,l);
        else if(name=="Gradient")gradientPass(expected,l,{0,0,65,65});
        else if(name=="Tint") {
          // Tint lives in a pack now; run through the same entry the engine uses.
          if(const PackEffect *tint=packEffect("Tint"))runPackEffect(expected,*tint,packEffectValues(l,*tint,0),{0,0,65,65},1,0,0);
          else failures<<"the Tint effect (Wavy-Stylize.wfx) is not installed";
        }
        else if(name=="Stroke")strokePass(expected,l,1);
        else if(name=="Glow")glowPass(expected,l,1);
        else if(name=="Drop Shadow")shadowPass(expected,l,1);
        else blurPass(expected,l,1);
      }
      expected=expected.convertToFormat(base.format());
      if(actual!=expected)failures<<"ordered stack mismatch: "+first+" -> "+second;
    }
  }
  {
    QImage source(65,65,QImage::Format_RGBA32FPx4);source.fill(Qt::transparent);
    {QPainter p(&source);p.fillRect(25,25,15,15,Qt::white);}
    Layer l;l.type="image";l.color=Qt::white;l.glow=8;
    l.appliedEffects["Fill"]=true;
    l.surfaceEffects["Fill"]=QJsonObject{{"color","#ff0000"},{"opacity",100}};
    l.effectOrder=QJsonArray{"Fill","Glow"};
    auto output=applyLayerEffects(source,l,QRectF(0,0,65,65),1);
    auto halo=output.pixelColor(23,32);
    if(halo.alphaF()<=0 || halo.redF()<.99 || halo.greenF()>.01 || halo.blueF()>.01)
      failures<<"Fill red -> Glow must use preceding red pixels instead of white layer colour";
    QImage mixed(65,65,QImage::Format_RGBA32FPx4_Premultiplied);mixed.fill(Qt::transparent);
    {QPainter p(&mixed);p.fillRect(12,25,12,15,Qt::red);p.fillRect(42,25,12,15,Qt::blue);}
    auto soft=renderHaloOnly(mixed,4,QColor());
    if(soft.pixelColor(10,32).redF()<=soft.pixelColor(10,32).blueF() ||
       soft.pixelColor(55,32).blueF()<=soft.pixelColor(55,32).redF())
      failures<<"Glow preserves local colours of a multicolour source";
  }
  {
    Scene s;Layer l;l.duration=180;l.p[0].set(0,0);l.p[0].set(30,100);s.layers={l};
    if(compositionHasMotion(s,15))failures<<"keys without enabled layer motion blur need one sample";
    s.layers[0].motionBlur=true;
    if(!compositionHasMotion(s,15))failures<<"moving motion-blur layer needs shutter samples";
    if(compositionHasMotion(s,60))failures<<"settled motion-blur keys need one sample";
    s.layers[0].visible=false;
    if(compositionHasMotion(s,15))failures<<"hidden motion-blur layers need no shutter samples";
  }
  {
    Scene s;s.width=64;s.height=48;s.motionBlur=false;
    Layer l;l.type="fill";l.color=Qt::white;l.duration=180;
    l.p[0].base=32;l.p[1].base=24;l.p[2].base=14692;l.p[3].base=14692;
    s.layers.push_back(l);
    if(!sameCompositionPixels(s,1,90))failures<<"static playback reuses pixels";
    if(sameCompositionPixels(s,1,180))failures<<"layer end invalidates playback pixels";
    auto plain=renderCPU(s,1,false);
    auto snapshot=s;
    auto background=std::async(std::launch::async,[snapshot]{return renderCPU(snapshot,1,false);});
    s.layers[0].color=Qt::red;
    if(background.get()!=plain)failures<<"background render isolates the scene snapshot";
    s.layers[0].color=Qt::white;
    s.layers[0].glow=100;
    auto halo=renderCPU(s,1,false);
    if(plain!=halo)failures<<"opaque viewport halo elision preserves image";
    s.layers[0].p[0].set(0,32);s.layers[0].p[0].set(20,50);
    if(sameCompositionPixels(s,1,2))failures<<"keyed playback must render new pixels";
    if(!sameCompositionPixels(s,30,60))failures<<"playback after last key reuses unchanged pixels";
    s.layers[0].p[0].keys.clear();s.layers[0].type="video";
    if(sameCompositionPixels(s,1,2))failures<<"video playback must render new pixels";
  }
  {
    QImage point(17,17,QImage::Format_RGBA32FPx4);point.fill(Qt::transparent);
    auto *p=reinterpret_cast<float *>(point.scanLine(8))+8*4;p[3]=1;
    auto d=distanceField(point,true);
    if(std::abs(d[12*17+11]-5.f)>.0001f) failures<<"Euclidean 3-4-5 distance";
    point.fill(Qt::transparent);d=distanceField(point,true);
    if(d[8*17+8]<1000) failures<<"empty alpha must have no contour";
    point.fill(Qt::white);d=distanceField(point,false);
    if(d[8*17+8]<1000) failures<<"solid alpha must have no interior seed";
  }
  {
    QImage image(48,32,QImage::Format_RGBA8888);image.fill(Qt::transparent);
    QPainterPath left,right;left.addRect(8,8,12,16);right.addRect(21,8,12,16);
    QPainter p(&image);
    for(int pass=0;pass<2;++pass) for(const auto &path:{left,right})
      drawTextStrokePass(p,path,Qt::white,QPen(Qt::black,8),1,pass==1);
    p.end();
    if(image.pixelColor(18,16)!=QColor(Qt::white)) failures<<"neighbor stroke covers text fill";
    if(image.pixelColor(5,16)!=QColor(Qt::black)) failures<<"outside text stroke missing";
  }
  {
    QImage image(48,32,QImage::Format_RGBA8888);image.fill(Qt::transparent);
    QPainterPath left,right;left.addRect(8,8,12,16);right.addRect(21,8,12,16);
    QPainter p(&image);
    for(int pass=0;pass<2;++pass) for(const auto &path:{left,right})
      drawTextStrokePass(p,path,Qt::white,QPen(Qt::black,8),1,pass==1);
    p.end();
    if(image.pixelColor(18,16)!=QColor(Qt::white)) failures<<"neighbor stroke covers text fill";
    if(image.pixelColor(5,16)!=QColor(Qt::black)) failures<<"outside text stroke missing";
  }
  QImage source(32,32,QImage::Format_RGBA32FPx4); source.fill(Qt::transparent);
  {QPainter p(&source);p.fillRect(QRect(8,8,16,16),Qt::red);}
  Layer l;l.appliedEffects["Stroke"]=true;
  for(int mode=0;mode<3;++mode) {
    l.surfaceEffects["Stroke"]=QJsonObject{{"mode",mode},{"width",4},{"color","#00ff00"}};
    auto result=applySurfaceEffects(source,l,{8,8,16,16},1);
    if((result.pixelColor(7,16).alpha()>0)!=(mode!=0)) failures<<"stroke outside coverage";
    if((result.pixelColor(8,16).green()>0)!=(mode!=1)) failures<<"stroke inside coverage";
    if(result.pixelColor(16,16).red()<200) failures<<"stroke center preservation";
  }
  l.hiddenEffects["Stroke"]=true;l.appliedEffects["Gradient"]=true;
  auto result=applySurfaceEffects(source,l,{8,8,16,16},1);
  if(result.pixelColor(0,0).alpha()!=0) failures<<"gradient alpha";
  if(result.pixelColor(9,16)==result.pixelColor(22,16)) failures<<"gradient progression";
  for(int mode=0;mode<3;++mode) {
    QImage image(32,32,QImage::Format_RGBA8888);image.fill(Qt::transparent);
    QPainterPath path;path.addRect(8,8,16,16);
    {QPainter p(&image);drawAlignedStroke(p,path,Qt::red,QPen(Qt::green,4),mode);}
    if((image.pixelColor(7,16).alpha()>0)!=(mode!=0)) failures<<"vector stroke outside";
    if((image.pixelColor(9,16).green()>0)!=(mode!=1)) failures<<"vector stroke inside";
  }
  {
    QImage image(512,512,QImage::Format_RGBA8888);image.fill(Qt::transparent);
    QPainterPath path;path.addRect(206,206,100,100);
    {QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
      drawAlignedStroke(p,path,QColor("#287eea"),QPen(QColor("#ed3030"),180),1);}
    if(image.pixelColor(256,256)!=QColor("#287eea"))
      failures<<"wide outside shape stroke preserves its fill";
    if(image.pixelColor(200,256).red()<200)
      failures<<"wide outside shape stroke reaches outside";
    if(image.pixelColor(10,256).alpha()!=0)
      failures<<"wide outside shape stroke does not flood its raster";
  }
  {
    QImage image(96,96,QImage::Format_RGBA8888);image.fill(Qt::transparent);
    QPainterPath path;path.addRect(30,30,40,40);
    {QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
      drawAlignedStroke(p,path,Qt::NoBrush,QPen(Qt::green,12),1);}
    if(image.pixelColor(19,19).alpha()==0)
      failures<<"shape stroke preserves sharp corners";
    image.fill(Qt::transparent);
    TextStrokeStyle stroke{12,Qt::green,1};
    {QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
      drawTextStrokeStackPass(p,path,{stroke},0);}
    if(image.pixelColor(19,19).alpha()==0)
      failures<<"text stroke preserves sharp corners";
  }
  {
    QImage image(96,96,QImage::Format_RGBA8888);image.fill(Qt::transparent);
    QPainterPath acute;acute.moveTo(48,30);acute.lineTo(40,72);acute.lineTo(56,72);
    acute.closeSubpath();
    TextStrokeStyle stroke{12,Qt::black,1};
    {QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
      drawTextStrokeStackPass(p,acute,{stroke},0);}
    if(image.pixelColor(48,8).alpha()!=0)
      failures<<"acute text corners do not produce miter spikes";
  }
  {
    QImage image(160,120,QImage::Format_RGBA8888);image.fill(Qt::transparent);
    QPainterPath outer;outer.addRect(30,30,70,60);
    QPainterPath inner;inner.addRect(45,45,40,30);
    QPainterPath glyph=outer;glyph.addPath(inner.toReversed());
    TextStrokeStyle broad{40,Qt::black,1};
    {QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
      drawTextStrokeStackPass(p,glyph,{broad},0);}
    // Both the outer edge and the counter reach here. They must merge rather
    // than cancel, which was the D-shaped checkerboard tear visible beside O.
    if(image.pixelColor(110,60).alpha()<200)
      failures<<"colliding text stroke contours merge without transparent tears";
  }
  {
    Layer ordered; ordered.type="rect";
    const QStringList defaults{"Fill","Gradient","Tint","Exposure","Levels","Curves",
      "Brightness & Contrast","HSL","Vibrance","Color Balance","Three-Way Color Corrector",
      "Temperature & Tint","Black & White","RGB Split","Film Grain","Vignette","Noise","Turbulent Noise","Displacement Map","Linear Wipe","Radial Wipe","Stroke","Drop Shadow","Glow","Gaussian Blur"};
    if(layerEffectOrder(ordered)!=defaults) failures<<"default effect order";
    ordered.effectOrder=QJsonArray{"Glow","Stroke","Glow","Nonsense"};
    const auto names=layerEffectOrder(ordered);
    if(names.size()!=defaults.size() || names[0]!="Glow" || names[1]!="Stroke" ||
        names.count("Glow")!=1 || names.contains("Nonsense"))
      failures<<"recorded order is honoured, completed and deduplicated";

    QImage base(32,32,QImage::Format_RGBA32FPx4); base.fill(Qt::transparent);
    {QPainter p(&base);p.fillRect(QRect(8,8,16,16),Qt::red);}
    Layer both; both.type="rect";
    both.appliedEffects["Gradient"]=true; both.appliedEffects["Stroke"]=true;
    both.surfaceEffects["Gradient"]=QJsonObject{{"start","#00ff00"},{"end","#00ff00"},{"amount",100}};
    both.surfaceEffects["Stroke"]=QJsonObject{{"mode",1},{"width",3},{"color","#ff0000"}};
    const auto strokeLast=applyLayerEffects(base,both,{8,8,16,16},1);
    both.effectOrder=QJsonArray{"Stroke","Gradient","Drop Shadow","Glow","Gaussian Blur"};
    const auto gradientLast=applyLayerEffects(base,both,{8,8,16,16},1);
    // Stroked first, the gradient reaches the stroke as well and greens it.
    if(strokeLast.pixelColor(7,16).red()<200) failures<<"stroke after gradient keeps its own colour";
    if(gradientLast.pixelColor(7,16).green()<200) failures<<"gradient after stroke tints the stroke";
    if(strokeLast==gradientLast) failures<<"effect order changes the rendered result";
    both.appliedEffects["Gradient"]=false;both.glow=4;both.color=Qt::green;
    both.effectOrder=QJsonArray{"Stroke","Glow"};
    auto chained=applyLayerEffects(base,both,{8,8,16,16},1);
    auto reference=base.convertToFormat(QImage::Format_RGBA32FPx4);
    strokePass(reference,both,1);
    reference=reference.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    glowPass(reference,both,1);reference=reference.convertToFormat(base.format());
    if(chained!=reference)failures<<"Glow consumes the stroked output";
    both.effectOrder=QJsonArray{"Glow","Stroke"};
    auto reversed=applyLayerEffects(base,both,{8,8,16,16},1);
    if(chained==reversed)failures<<"Stroke/Glow order must change output";
    both.type="text";
    if(vectorEffectStroke(both))failures<<"text Stroke cannot bypass an earlier Glow";
    both.effectOrder=QJsonArray{"Stroke","Glow"};
    if(!vectorEffectStroke(both))failures<<"first text Stroke keeps vector fast path";
    both.effectOrder=QJsonArray{"Glow","Stroke"};both.hiddenEffects["Glow"]=true;
    if(!vectorEffectStroke(both))failures<<"hidden Glow does not block vector Stroke";
  }
  {
    Scene fillScene; fillScene.width=120; fillScene.height=80; fillScene.motionBlur=false;
    Layer fill; fill.type="fill"; fill.duration=100; fill.color=QColor("#204080");
    fill.p[0].base=60; fill.p[1].base=40;
    fillScene.layers={fill};
    auto covered=renderCPU(fillScene,0,false);
    if(covered.pixelColor(0,0)!=QColor("#204080") || covered.pixelColor(119,79)!=QColor("#204080"))
      failures<<"fill layer covers the composition corner to corner";
    // A baked-in size would leave the new corners empty; this one grows.
    fillScene.width=240; fillScene.height=160;
    fillScene.layers[0].p[0].base=120; fillScene.layers[0].p[1].base=80;
    auto grown=renderCPU(fillScene,0,false);
    if(grown.size()!=QSize(240,160) || grown.pixelColor(239,159)!=QColor("#204080"))
      failures<<"fill layer takes the resized composition's size";
    // It is still an ordinary layer: its transform reaches it.
    fillScene.layers[0].p[2].base=.25; fillScene.layers[0].p[3].base=.25;
    if(renderCPU(fillScene,0,false).pixelColor(0,0).alpha()!=0)
      failures<<"fill layer honours its own transform";
  }
  {
    // A region render has to be indistinguishable from the same crop of a
    // whole frame, on the multi-threaded band path and on the serial path an
    // effect forces. The viewer renders only what its viewport can show, so a
    // seam or an off-by-one here would appear as the preview is panned.
    Scene s; s.width=120; s.height=80; s.motionBlur=false;
    Layer back; back.type="fill"; back.duration=100; back.color=QColor("#204080");
    back.p[0].base=60; back.p[1].base=40;
    Layer front; front.type="rect"; front.duration=100; front.color=QColor("#e0a030");
    front.p[0].base=44; front.p[1].base=33;
    front.p[ShapeWidth].base=46; front.p[ShapeHeight].base=27;
    s.layers={back,front};
    const QRect region(17,9,63,44); // deliberately odd, and not band aligned
    for (int pass=0; pass<3; ++pass) {
      if (pass==1) s.layers[1].blur=3; // forces the serial path
      if (pass==2) { // position dependent, so a misplaced offscreen would show
        s.layers[1].blur=0;
        s.layers[1].appliedEffects["Gradient"]=true;
        s.layers[1].surfaceEffects["Gradient"]=QJsonObject{{"start","#00ff00"},{"end","#ff00ff"},{"amount",100}};
      }
      const auto full=renderCPU(s,0,false);
      const auto part=renderCPU(s,0,false,QSize(),region);
      const QString where=pass==0?QString():pass==1?" with a blur":" with a gradient";
      if(part.size()!=region.size()) failures<<"region render returns the region's size"+where;
      else if(part!=full.copy(region)) failures<<"region render matches the same crop of a full render"+where;
    }
    if(!renderCPU(s,0,false,QSize(),QRect(500,500,10,10)).isNull())
      failures<<"a region outside the frame renders nothing";
  }
  {
    // Large layer scale and a zoomed target: clipping the effect must preserve
    // the gradient's original coordinate system, not restart it at the viewport.
    Scene s;s.width=160;s.height=90;s.motionBlur=false;
    Layer huge;huge.type="rect";huge.color=Qt::white;huge.duration=180;
    huge.p[0].base=80;huge.p[1].base=45;
    huge.p[2].base=5641;huge.p[3].base=5641;
    huge.appliedEffects["Gradient"]=true;
    huge.surfaceEffects["Gradient"]=QJsonObject{{"start","#ff6a00"},{"end","#6633ff"},{"amount",100},{"angle",23}};
    s.layers.push_back(huge);
    QSize target(640,360);QRect region(211,109,173,127);
    auto full=renderCPU(s,0,false,target).copy(region).convertToFormat(QImage::Format_RGBA8888);
    auto cropped=renderCPU(s,0,false,target,region).convertToFormat(QImage::Format_RGBA8888);
    int maxError=0;
    if(cropped.size()!=full.size()) failures<<"large-scale gradient viewport size";
    else {
      for(int y=0;y<full.height();++y)for(int x=0;x<full.width()*4;++x)
        maxError=std::max(maxError,std::abs(int(full.constScanLine(y)[x])-int(cropped.constScanLine(y)[x])));
      if(maxError>2)failures<<"large-scale gradient maintains coordinates when cropped";
    }
  }
  {
    // Layer, effect, movement are three stages, and only the first two decide
    // what an effected raster contains. Moving or rotating a layer that is
    // wholly on screen has to reuse the pixels it already has; changing what
    // the layer is, or what its effects are, has to build them again.
    Scene s; s.width=200; s.height=200; s.motionBlur=false;
    Layer t; t.type="rect"; t.duration=100; t.color=QColor("#c04020");
    t.p[0].base=100; t.p[1].base=100;
    t.p[ShapeWidth].base=40; t.p[ShapeHeight].base=40;
    t.glow=6; t.appliedEffects["Glow"]=true;
    s.layers={t};
    auto built=[&]{ renderCPU(s,0,false); return effectedRasterBuilds(); };
    const quint64 first=built();
    if(built()!=first) failures<<"an unchanged layer is rasterized once";
    s.layers[0].p[0].base=108; s.layers[0].p[1].base=94;
    if(built()!=first) failures<<"moving a layer does not rebuild its raster";
    s.layers[0].p[4].base=17;
    if(built()!=first) failures<<"rotating a layer does not rebuild its raster";
    s.layers[0].glow=9;
    const quint64 afterEffect=built();
    if(afterEffect==first) failures<<"changing an effect rebuilds the raster";
    s.layers[0].color=QColor("#2040c0");
    if(built()==afterEffect) failures<<"changing the layer rebuilds the raster";
    // And the pixels have to be right, not merely reused.
    Scene fresh=s;
    const auto reused=renderCPU(s,0,false);
    if(reused!=renderCPU(fresh,0,false)) failures<<"a reused raster matches a fresh one";
  }
  {
    // Fill replaces the colour and keeps the shape. On a layer of one flat
    // colour it folds into the brush; behind anything that needs pixels it runs
    // as a pass. Both have to put the same colour in the same place.
    Scene s; s.width=120; s.height=120; s.motionBlur=false;
    Layer f; f.type="rect"; f.duration=100; f.color=QColor("#c04020");
    f.p[0].base=60; f.p[1].base=60;
    f.p[ShapeWidth].base=60; f.p[ShapeHeight].base=60;
    f.appliedEffects["Fill"]=true;
    f.surfaceEffects["Fill"]=QJsonObject{{"color","#22cc55"},{"opacity",100}};
    s.layers={f};
    const auto folded=renderCPU(s,0,false);
    if(folded.pixelColor(60,60)!=QColor("#22cc55")) failures<<"a folded fill replaces the colour";
    if(folded.pixelColor(60,60).alpha()!=255) failures<<"a folded fill keeps full coverage";
    if(folded.pixelColor(2,2).alpha()!=0) failures<<"a fill stays inside the shape";
    // Ahead of it sits an effect that needs pixels, so the fill cannot fold.
    s.layers[0].glow=6;
    s.layers[0].appliedEffects["Glow"]=true;
    s.layers[0].effectOrder=QJsonArray{"Glow","Fill","Gradient","Stroke","Drop Shadow","Gaussian Blur"};
    const auto passed=renderCPU(s,0,false);
    if(passed.pixelColor(60,60)!=QColor("#22cc55")) failures<<"an unfolded fill replaces the colour too";
    // And the opacity reaches the alpha, on the path that runs as a pass.
    s.layers[0].surfaceEffects["Fill"]=QJsonObject{{"color","#22cc55"},{"opacity",40}};
    const int faded=renderCPU(s,0,false).pixelColor(60,60).alpha();
    const QColor fadedColour=renderCPU(s,0,false).pixelColor(60,60);
    if(std::abs(faded-102)>3) failures<<QString("fill opacity reaches the alpha channel (alpha %1, colour %2, wanted alpha 102)").arg(faded).arg(fadedColour.name(QColor::HexArgb));
    s.layers[0].glow=0; s.layers[0].appliedEffects=QJsonObject{{"Fill",true}};
    s.layers[0].effectOrder=QJsonArray();
    const int foldedFade=renderCPU(s,0,false).pixelColor(60,60).alpha();
    if(std::abs(foldedFade-faded)>3) failures<<QString("both paths fade a fill by the same amount (folded %1, as a pass %2)").arg(foldedFade).arg(faded);
  }
  {
    // The two transforms sit on opposite sides of the effects, so the same
    // rotation means different things. Before them, the shadow is measured on
    // the rotated shape and keeps its own offset; after them, the shadow is
    // measured first and turns along with the layer. If these ever came out the
    // same, the main transform would be a duplicate rather than a stage.
    Scene s; s.width=160; s.height=160; s.motionBlur=false;
    Layer m; m.type="rect"; m.duration=100; m.color=QColor("#e0e0e0");
    m.p[0].base=80; m.p[1].base=80;
    m.p[ShapeWidth].base=70; m.p[ShapeHeight].base=24;
    m.shadow=4; m.appliedEffects["Drop Shadow"]=true;
    Scene beforeEffects=s, afterEffects=s;
    Layer early=m; early.mainRotation=90;
    Layer late=m; late.p[4].base=90;
    beforeEffects.layers={early};
    afterEffects.layers={late};
    const auto rotatedFirst=renderCPU(beforeEffects,0,false);
    const auto rotatedLast=renderCPU(afterEffects,0,false);
    if(rotatedFirst==rotatedLast)
      failures<<"the main transform runs before the effects, not with them";
    if(rotatedFirst.pixelColor(80,80)!=QColor("#e0e0e0"))
      failures<<"a main rotation still draws the layer";
    // Turning the layer on its side has to change what the layer covers.
    Layer upright=m; Scene plain=s; plain.layers={upright};
    if(renderCPU(plain,0,false)==rotatedFirst)
      failures<<"a main rotation reaches the drawing";
    // A layer has one opacity, the Transform's. A project saved when the main
    // transform had an opacity of its own still looks as it did: that opacity
    // is folded into the Transform's (its keys too) as the project is read.
    {
      Layer faded=m; faded.shadow=0; faded.appliedEffects=QJsonObject();
      faded.p[5].base=.8;
      Scene old=s; old.layers={faded};
      QJsonObject saved=old.json();
      QJsonArray layers=saved["layers"].toArray();
      QJsonObject layer=layers[0].toObject();
      QJsonObject main=layer["mainTransform"].toObject();
      main["opacity"]=.5;
      layer["mainTransform"]=main;
      layers[0]=layer;
      saved["layers"]=layers;
      Scene reopened; QString error;
      if(!reopened.load(saved,error) || std::abs(reopened.layers[0].p[5].base-.4)>1e-9)
        failures<<"a saved main-transform opacity is folded into the Transform Opacity";
      const int fadedAlpha=reopened.layers.empty() ? 0 : renderCPU(reopened,0,false).pixelColor(80,80).alpha();
      if(std::abs(fadedAlpha-102)>6) failures<<"a project with a main-transform opacity looks as it did";
    }
    {
      // What the effects are computed from decides what has to be computed
      // again. The main rotation, scale and anchor are inside the
      // raster, so they rebuild it; the main position sits beside the movement
      // position, outside, so sliding a layer by it must reuse everything.
      Scene moved=s;
      Layer probe=m; probe.shadow=0; probe.appliedEffects=QJsonObject();
      probe.glow=10; probe.appliedEffects["Glow"]=true;
      moved.layers={probe};
      auto rebuildsAfter=[&](void (*change)(Layer &)) {
        renderCPU(moved,0,false);
        const auto before=effectedRasterBuilds();
        change(moved.layers[0]);
        renderCPU(moved,0,false);
        return effectedRasterBuilds()-before;
      };
      if(rebuildsAfter([](Layer &x){ x.mainX+=23; x.mainY-=11; })!=0)
        failures<<"sliding a layer by its main position reuses its effects";
      if(rebuildsAfter([](Layer &x){ x.mainRotation+=9; })==0)
        failures<<"turning a layer by its main rotation recomputes its effects";
      if(rebuildsAfter([](Layer &x){ x.mainScaleX*=1.3; })==0)
        failures<<"scaling a layer by its main scale recomputes its effects";
    }
    Scene restored; QString error;
    Scene saved=s; saved.layers={early};
    saved.layers[0].mainScaleX=1.5; saved.layers[0].mainAnchorY=7;
    if(!restored.load(saved.json(),error) ||
        restored.layers[0].mainRotation!=90 || restored.layers[0].mainScaleX!=1.5 ||
        restored.layers[0].mainAnchorY!=7)
      failures<<"the main transform survives save and load";
  }
  {
    // The main position has to move the layer, and it has to carry the anchor
    // point along with it: turning the layer with the movement transform
    // afterwards still turns it about its own middle. An earlier attempt put
    // the main position on the anchor's side of the movement transform, where
    // the transform subtracts its own pivot -- so the layer did not move at
    // all, and neither did the main anchor point.
    Scene s; s.width=160; s.height=160; s.motionBlur=false;
    Layer m; m.type="rect"; m.duration=100; m.color=QColor("#e0e0e0");
    m.p[0].base=80; m.p[1].base=80;
    m.p[ShapeWidth].base=70; m.p[ShapeHeight].base=24;
    m.mainX=30;
    auto centre=[&](const Layer &one) {
      Scene single=s; single.layers={one};
      const auto image=renderCPU(single,0,false);
      double sx=0,sy=0,total=0;
      for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) {
        const double a=image.pixelColor(x,y).alphaF();
        sx+=a*x; sy+=a*y; total+=a;
      }
      return total>0?QPointF(sx/total,sy/total):QPointF(-1,-1);
    };
    Layer upright=m, turned=m; turned.p[4].base=90;
    const auto still=centre(upright), spun=centre(turned);
    if(std::hypot(still.x()-110.,still.y()-80.)>1.)
      failures<<"the main position moves the layer";
    if(std::hypot(still.x()-spun.x(),still.y()-spun.y())>1.)
      failures<<"a main translation carries the anchor point with the layer";
    // The main anchor moves the layer too, the way an anchor point does: it is
    // what the main rotation and scale turn around.
    Layer shifted=m; shifted.mainX=0; shifted.mainAnchorX=20;
    const auto moved=centre(shifted);
    if(std::hypot(moved.x()-60.,moved.y()-80.)>1.)
      failures<<"the main anchor point moves the layer";
    // The composition renders in horizontal bands and each band culls layers it
    // cannot see. A layer the main transform has moved has to survive that.
    Scene banded=s; banded.layers={turned};
    double covered=0;
    const auto whole=renderCPU(banded,0,false);
    for(int y=0;y<whole.height();++y) for(int x=0;x<whole.width();++x)
      covered+=whole.pixelColor(x,y).alphaF();
    if(std::abs(covered-70*24)>70)
      failures<<"band culling keeps a layer the main transform moved";
    const auto marker=layerAnchorPosition(upright,s,0);
    if(std::hypot(marker.x()-still.x(),marker.y()-still.y())>1.)
      failures<<"the anchor point marker sits on the layer the main transform placed";
  }
  {
    // Qt gives a proportional line height all of its extra space below the
    // baseline, so a single line used to climb inside its own box as the
    // leading grew. Changing the leading of one line must not move that line.
    Scene s; s.width=400; s.height=300; s.motionBlur=false;
    Layer t; t.type="text"; t.text="aaaaa"; t.fontSize=60; t.duration=100;
    t.color=Qt::white; t.p[0].base=200; t.p[1].base=150;
    auto inkCentre=[&](double leading) {
      Layer one=t; one.leading=leading;
      Scene single=s; single.layers={one};
      const auto image=renderCPU(single,0,false);
      int top=9999,bottom=-1;
      for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x)
        if(image.pixelColor(x,y).alphaF()>.02){top=std::min(top,y);bottom=std::max(bottom,y);}
      return bottom<0?-1.:(top+bottom)/2.;
    };
    const double tight=inkCentre(1.), loose=inkCentre(1.5);
    if(tight<0) failures<<"the leading test draws any text at all";
    else if(std::abs(tight-loose)>1.)
      failures<<"leading leaves a single line of text where it was";
  }
  {
    // A layer whose only pixel effect is one halo is drawn in two pieces: the
    // halo laid down at its own resolution, then the layer over it as vectors,
    // instead of both being composited inside a raster of the layer's size.
    // The two forms have to produce the same picture. An opacity a hair
    // under one takes the raster form without visibly changing what it draws,
    // which is what makes the comparison possible at all.
    // Both sizes put the raster past the megapixel threshold, which is what
    // selects this path at all: below it the finished pixels are cached whole
    // and blitting them back is the cheaper of the two.
    for(double scale : {2., 3.}) for(int shadow : {0, 1}) {
      Scene s; s.width=1400; s.height=1000; s.motionBlur=false;
      Layer g; g.type="rect"; g.duration=100; g.color=QColor("#e8e8e8");
      g.p[0].base=700; g.p[1].base=500; g.p[2].base=g.p[3].base=scale;
      g.p[ShapeWidth].base=300; g.p[ShapeHeight].base=200;
      if(shadow) { g.shadow=40; g.appliedEffects["Drop Shadow"]=true; }
      else { g.glow=40; g.appliedEffects["Glow"]=true; }
      Scene split=s, raster=s;
      split.layers={g};
      Layer viaRaster=g; viaRaster.p[5].base=.999999;
      raster.layers={viaRaster};
      // Premultiplied, which is what reaches the screen. Comparing straight
      // colours turns a one-level rounding in the far tail of a glow, where
      // alpha is near zero, into a difference of half the range.
      const auto a=renderCPU(split,0,false).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      const auto b=renderCPU(raster,0,false).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
      double worst=0,total=0; qint64 n=0;
      for(int y=0;y<a.height();++y) {
        const auto *p1=a.constScanLine(y);
        const auto *p2=b.constScanLine(y);
        for(int i=0;i<a.width()*4;++i) {
          const double d=std::abs(int(p1[i])-int(p2[i]));
          worst=std::max(worst,d); total+=d; ++n;
        }
      }
      if(worst>6 || total/std::max<qint64>(1,n)>.6)
        failures<<QString("the halo path draws what the raster path draws "
                          "(%1, scale %2, mean %3 worst %4)")
                    .arg(shadow?"shadow":"glow").arg(scale)
                    .arg(total/std::max<qint64>(1,n)).arg(worst);
    }
  }
  {
    QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    QPainterPath glyph;
    glyph.addRect(QRectF(40, 40, 20, 20));
    const std::vector<TextStrokeStyle> strokes{{5, QColor("#d92735"), 1},
                                                {4, QColor("#2576e8"), 1}};
    for (int index = int(strokes.size()) - 1; index >= 0; --index)
      drawTextStrokeStackPass(painter, glyph, strokes, index);
    drawTextFillAfterStrokes(painter, glyph, Qt::white, strokes);
    painter.end();
    if (image.pixelColor(37, 50) != QColor("#d92735"))
      failures << "the first Character stroke occupies its own width nearest the text";
    if (image.pixelColor(33, 50) != QColor("#2576e8"))
      failures << "the next Character stroke begins after the preceding width";
    if (image.pixelColor(50, 50) != QColor(Qt::white))
      failures << "all Character strokes stay behind the text fill";
  }
  {
    // When broad stacked outlines meet around counters such as a/o, the join
    // must close cleanly. Acute miters used to leave isolated checkerboard
    // shards trapped inside an otherwise solid stack.
    Scene collision;collision.width=500;collision.height=300;collision.frames=30;
    Layer text;text.type="text";text.text="ao";text.font="Arial";text.bold=true;
    text.fontSize=80;text.duration=30;text.p[0].base=250;text.p[1].base=150;
    text.textStrokes={{20,QColor("#ffffff"),1},{45,QColor("#000000"),1}};
    syncLegacyTextStroke(text);collision.layers={text};
    const auto joined=renderCPU(collision,0,false);
    const int w=joined.width(),h=joined.height();
    std::vector<quint8> outside(size_t(w)*h);
    QVector<QPoint> queue;
    auto visit=[&](int x,int y) {
      const size_t at=size_t(y)*w+x;
      if(!outside[at] && joined.pixelColor(x,y).alpha()<16) {
        outside[at]=1;queue.push_back(QPoint(x,y));
      }
    };
    for(int x=0;x<w;++x) {visit(x,0);visit(x,h-1);}
    for(int y=0;y<h;++y) {visit(0,y);visit(w-1,y);}
    for(int head=0;head<queue.size();++head) {
      const auto p=queue[head];
      if(p.x()>0) visit(p.x()-1,p.y());if(p.x()+1<w) visit(p.x()+1,p.y());
      if(p.y()>0) visit(p.x(),p.y()-1);if(p.y()+1<h) visit(p.x(),p.y()+1);
    }
    int trapped=0;
    for(int y=0;y<h;++y) for(int x=0;x<w;++x)
      if(joined.pixelColor(x,y).alpha()<16 && !outside[size_t(y)*w+x]) ++trapped;
    if(trapped) failures << QString("stacked text strokes leave %1 transparent collision pixels").arg(trapped);
  }
  {
    // A text layer's Miter stroke gets the letters' outlines from the drawing
    // itself: square corners add coverage a Round stroke does not have, and
    // take none of it away. Checked small, and big enough that the letters are
    // painted in strips.
    for(const double size:{80.,700.}) {
      Scene corners;corners.width=size>100?2400:500;corners.height=size>100?1200:300;corners.frames=30;
      corners.motionBlur=false;corners.background=QColor(0,0,0,0);
      Layer text;text.type="text";text.text="HE";text.font="Arial";text.bold=true;
      text.fontSize=size;text.duration=30;
      text.p[0].base=corners.width/2.;text.p[1].base=corners.height/2.;
      auto render=[&](int join){
        Layer stroked=text;
        stroked.textStrokes={{size/8,QColor(Qt::black),1,join}};
        syncLegacyTextStroke(stroked);corners.layers={stroked};
        return renderCPU(corners,0,false).convertToFormat(QImage::Format_RGBA8888);
      };
      const QImage mitred=render(0),round=render(1);
      qint64 gained=0,lost=0;
      for(int y=0;y<mitred.height();++y) {
        auto *m=mitred.constScanLine(y),*r=round.constScanLine(y);
        for(int x=0;x<mitred.width();++x) {
          const int difference=int(m[x*4+3])-int(r[x*4+3]);
          if(difference>8) gained+=difference; else if(difference<-8) lost-=difference;
        }
      }
      if(gained<255*size/8 || lost>255)
        failures<<QString("a %1 px text layer's Miter stroke squares its corners: coverage gained %2, lost %3")
                      .arg(size).arg(gained).arg(lost);
    }
    // And the choice is kept with the project.
    Scene saved;
    Layer text;text.type="text";text.textStrokes={{6,QColor(Qt::black),1,1},{9,QColor(Qt::red),1,0}};
    syncLegacyTextStroke(text);saved.layers={text};
    Scene reopened;QString problem;
    if(!reopened.load(saved.json(),problem) || reopened.layers.size()!=1 ||
       reopened.layers[0].textStrokes.size()!=2 || reopened.layers[0].textStrokes[0].join!=1 ||
       reopened.layers[0].textStrokes[1].join!=0)
      failures<<"a Character stroke's line join is saved with the project";
  }
  Scene scene;scene.layers.push_back(l);scene.layers[0].type="text";scene.layers[0].strokeMode=1;
  scene.layers[0].effectOrder=QJsonArray{"Stroke","Gradient"};
  Scene restored;QString error;
  if(!restored.load(scene.json(),error) || restored.layers[0].strokeMode!=1 ||
      !restored.layers[0].appliedEffects["Gradient"].toBool() ||
      restored.layers[0].effectOrder!=scene.layers[0].effectOrder ||
      restored.layers[0].surfaceEffects!=l.surfaceEffects) failures<<"surface settings roundtrip";
  {
    // Character strokes follow the letters because they are measured from
    // them. These shapes stand in for letters so no font is needed, and each
    // check is one way the old wide-pen stroke went wrong.
    auto canvas=[](int size){
      QImage image(size,size,QImage::Format_RGBA8888_Premultiplied);
      image.fill(Qt::transparent);
      return image;
    };
    auto alphaAt=[](const QImage &image,int x,int y){
      return image.pixelColor(x,y).alphaF();
    };
    auto colourAt=[](const QImage &image,int x,int y){
      return image.convertToFormat(QImage::Format_RGBA8888).pixelColor(x,y);
    };

    // A Round stroke is round where the shape has a corner: it stops at twenty
    // in every direction, short of the square corner of the offset box.
    QImage square=canvas(200);
    {QPainter p(&square);p.fillRect(80,80,40,40,Qt::white);}
    QPainterPath squareOutline;squareOutline.addRect(80,80,40,40);
    textStrokeStackPass(square,{{20,QColor(Qt::black),1,1}},1,1,{squareOutline});
    if(alphaAt(square,120+15,100)<.95) failures<<"a stroke reaches its full width out from a straight edge";
    if(alphaAt(square,120+25,100)>.05) failures<<"and no further";
    if(alphaAt(square,119+16,80-16)>.05)
      failures<<QString("a Round stroke is round at a corner: %1 alpha where the corner "
                        "of a miter would be").arg(alphaAt(square,135,64));
    if(alphaAt(square,119+12,80-12)<.95)
      failures<<"while still covering everything within its width of that corner";

    // A Miter stroke keeps the corner square, as the letters have it: out to
    // twenty along both edges' own lines, and not a pixel beyond either.
    QImage sharp=canvas(200);
    {QPainter p(&sharp);p.fillRect(80,80,40,40,Qt::white);}
    textStrokeStackPass(sharp,{{20,QColor(Qt::black),1,0}},1,1,{squareOutline});
    if(alphaAt(sharp,119+16,80-16)<.95 || alphaAt(sharp,119+19,80-19)<.95)
      failures<<QString("a Miter stroke fills the square corner of the letters' outline: %1 alpha")
                    .arg(alphaAt(sharp,138,61));
    if(alphaAt(sharp,120+25,80-10)>.05 || alphaAt(sharp,120+10,80-25)>.05 ||
       alphaAt(sharp,120+23,80-23)>.05)
      failures<<"and stops at its width from each edge, corner included";
    if(alphaAt(sharp,120+15,100)<.95 || alphaAt(sharp,120+25,100)>.05)
      failures<<"and along a straight edge is exactly the Round stroke";

    // A stroke's edge is the outline's own, to a small part of a pixel, all
    // the way round a curve, and moves with the outline by any fraction of a
    // pixel. Measured only between the centres of the pixels the curve had
    // been drawn into, a wide stroke's edge followed their steps -- flat runs
    // and kinks on every curve, worse the wider the stroke -- and as letters
    // were scaled it stood still and then jumped. Round a circle of radius R a
    // stroke W wide covers each pixel by R + W less the pixel's distance from
    // the centre (the field's soft edge runs over the pixel inside that).
    {
      double worstEdge=0;
      for(double radius:{60.,60.25,60.6}) {
        const QPointF centre(150.3,150.7);
        QPainterPath circle;circle.addEllipse(centre,radius,radius);
        QImage disc=canvas(300);
        {QPainter p(&disc);p.setRenderHint(QPainter::Antialiasing);p.fillPath(circle,Qt::white);}
        textStrokeStackPass(disc,{{40,QColor(Qt::black),1,1}},1,1,{circle});
        for(int y=0;y<300;++y)
          for(int x=0;x<300;++x) {
            const double reach=radius+40-std::hypot(x+.5-centre.x(),y+.5-centre.y());
            if(reach<-1.5 || reach>2.5) continue;
            worstEdge=std::max(worstEdge,std::abs(alphaAt(disc,x,y)-std::clamp(reach,0.,1.)));
          }
      }
      if(worstEdge>.05)
        failures<<QString("a stroke's edge is the outline's own, all round a curve and at any size (off by %1 of a pixel)").arg(worstEdge,0,'f',3);
    }
    // Without outlines there are no corners to know about, and it is drawn round.
    QImage unknown=canvas(200);
    {QPainter p(&unknown);p.fillRect(80,80,40,40,Qt::white);}
    textStrokeStackPass(unknown,{{20,QColor(Qt::black),1,0}},1,1);
    if(alphaAt(unknown,119+16,80-16)>.05)failures<<"a Miter stroke with no outlines falls back to round";

    // The inner corner of an "L" is sharp from inside the letter. An inside
    // Miter stroke lines it with a square corner; a Round one curves across it.
    QPainterPath ell;ell.setFillRule(Qt::WindingFill);
    ell.addRect(40,40,40,120);ell.addRect(40,120,120,40);
    ell=ell.simplified();
    auto insideL=[&](int join){
      QImage image=canvas(200);
      {QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);p.fillPath(ell,Qt::white);}
      textStrokeStackPass(image,{{10,QColor(255,0,0),0,join}},1,1,{ell});
      return image;
    };
    const QImage ellMitred=insideL(0),ellRound=insideL(1);
    // Eight and a half in from both edges at the inner corner (80,120): inside
    // a ten pixel miter, twelve from the corner itself and so outside a round one.
    const QColor mitredCorner=colourAt(ellMitred,71,128),roundCorner=colourAt(ellRound,71,128);
    if(mitredCorner.red()<200 || mitredCorner.green()>60)
      failures<<"an inside Miter stroke keeps a letter's inner corner square: "+mitredCorner.name();
    if(roundCorner.green()<200)
      failures<<"and an inside Round stroke rounds it: "+roundCorner.name();
    if(alphaAt(ellMitred,90,110)>.05)failures<<"and neither reaches outside the letter";
    if(colourAt(ellMitred,60,60).green()<200)failures<<"or into the middle of its stem";

    // A very sharp point is cut across at four times the width, the usual miter
    // limit, instead of running out to a spike nearly six times as long.
    const double halfAngle=10*3.141592653589793/180;
    QPainterPath spike;
    spike.moveTo(150,60);
    spike.lineTo(150-200*std::tan(halfAngle),260);spike.lineTo(150+200*std::tan(halfAngle),260);
    spike.closeSubpath();
    QImage point=canvas(300);
    {QPainter p(&point);p.setRenderHint(QPainter::Antialiasing);p.fillPath(spike,Qt::white);}
    textStrokeStackPass(point,{{10,QColor(Qt::black),1,0}},1,1,{spike});
    if(alphaAt(point,150,60-35)<.95)
      failures<<QString("a Miter stroke carries a sharp point on past a round stroke: %1")
                    .arg(alphaAt(point,150,25));
    if(alphaAt(point,150,60-45)>.05)
      failures<<QString("and cuts it off at the miter limit: %1").arg(alphaAt(point,150,15));

    // No holes where a stroke overlaps itself. A wide pen filled by winding
    // cancelled out where a ring's own stroke crossed, which is what left a
    // ring hanging off the "o".
    // Drawn with the default Miter join and its outline: a curve has no
    // corners, so nothing about it may change.
    QImage ring=canvas(240);
    QPainterPath band;band.setFillRule(Qt::OddEvenFill);
    band.addEllipse(QPointF(120,120),40,40);band.addEllipse(QPointF(120,120),25,25);
    {
      QPainter p(&ring);p.setRenderHint(QPainter::Antialiasing);
      p.fillPath(band,Qt::white);
    }
    textStrokeStackPass(ring,{{20,QColor(Qt::black),1}},1,1,{band});
    int gaps=0;
    for(int step=0;step<24;++step){
      const double angle=step*3.141592653589793/12;
      for(double radius:{44.,50.,56.}){
        const int x=qRound(120+radius*std::cos(angle)),y=qRound(120+radius*std::sin(angle));
        if(alphaAt(ring,x,y)<.95)++gaps;
      }
    }
    if(gaps)failures<<QString("a stroke around a ring has no holes in it: %1 samples uncovered").arg(gaps);
    // Inside the ring's hole the stroke reaches twenty in from the inner edge,
    // which leaves the very centre -- twenty-five away -- untouched.
    if(alphaAt(ring,120,120)>.05)failures<<"and it stops short inside the hole as well";
    if(alphaAt(ring,120+15,120)<.95)failures<<"while filling the part of the hole within reach";

    // A stack sits outward in order: the first stroke against the shape.
    QImage stack=canvas(200);
    {QPainter p(&stack);p.fillRect(80,80,40,40,Qt::white);}
    textStrokeStackPass(stack,{{10,QColor(255,0,0),1},{10,QColor(0,0,255),1}},1,1);
    const QColor near=colourAt(stack,120+5,100),far=colourAt(stack,120+15,100);
    if(near.red()<200 || near.blue()>50)failures<<"the first stroke sits against the shape: "+near.name();
    if(far.blue()<200 || far.red()>50)failures<<"and the second beyond it: "+far.name();

    // Inside stays inside.
    QImage inner=canvas(200);
    {QPainter p(&inner);p.fillRect(60,60,80,80,Qt::white);}
    textStrokeStackPass(inner,{{8,QColor(255,0,0),0}},1,1);
    const QColor edge=colourAt(inner,64,100),middle=colourAt(inner,100,100);
    if(edge.red()<200 || edge.green()>60)failures<<"an inside stroke lines the inside edge: "+edge.name();
    if(middle.green()<200)failures<<"and leaves the middle alone: "+middle.name();
    if(alphaAt(inner,56,100)>.05)failures<<"and nothing outside the shape";

    // Letters keep their own colours: a word can be coloured letter by letter.
    QImage rich=canvas(200);
    {QPainter p(&rich);p.fillRect(60,80,40,40,QColor(255,0,0));p.fillRect(100,80,40,40,QColor(0,255,0));}
    textStrokeStackPass(rich,{{6,QColor(Qt::black),1}},1,1);
    if(colourAt(rich,75,100).red()<200 || colourAt(rich,125,100).green()<200)
      failures<<"a stroke leaves each letter the colour it was";

    // Faded after the stroke, not before. Letters at thirty per cent would
    // otherwise read as empty space and lose their stroke entirely.
    QImage faint=canvas(200);
    {QPainter p(&faint);p.fillRect(80,80,40,40,Qt::white);}
    textStrokeStackPass(faint,{{10,QColor(Qt::black),1}},1,.3);
    if(std::abs(alphaAt(faint,125,100)-.3)>.05)
      failures<<QString("a faded layer keeps its stroke, faded with it: %1").arg(alphaAt(faint,125,100));
    if(std::abs(alphaAt(faint,100,100)-.3)>.05)failures<<"and its letters faded the same";

    // Widths are in the layer's own units, so a raster drawn at twice the
    // size draws a stroke twice as wide.
    QImage doubled=canvas(200);
    {QPainter p(&doubled);p.fillRect(80,80,40,40,Qt::white);}
    textStrokeStackPass(doubled,{{10,QColor(Qt::black),1}},2,1);
    if(alphaAt(doubled,120+15,100)<.95 || alphaAt(doubled,120+25,100)>.05)
      failures<<"a stroke scales with the raster it is drawn into";
  }
  {
    // Character strokes through Text Animation and handwriting: an Outside
    // stroke goes round the letters whatever they are doing.
    auto check = [&](bool ok, const QString &what) { if (!ok) failures << what; };
    Scene sc; sc.width = 640; sc.height = 240; sc.fps = 30; sc.frames = 90; sc.motionBlur = false;
    Layer bg; bg.type = "fill"; bg.name = "bg"; bg.color = QColor("#202848"); bg.p[0].base = 320; bg.p[1].base = 120; bg.duration = 90;
    Layer still; still.type = "text"; still.name = "t"; still.text = "Wavy"; still.font = "Arial"; still.fontStyle = "Black";
    still.fontSize = 140; still.color = Qt::white; still.p[0].base = 320; still.p[1].base = 120; still.duration = 90;
    still.strokeWidth = 10; still.strokeColor = Qt::red; still.strokeMode = 1;
    Layer animated = still;
    animated.characterAnimation = QJsonObject{{"enabled", true}, {"opacity", 0}, {"delay", 1.0}};
    animated.p[6].set(0, 0); animated.p[6].set(10, 100);
    Layer written = animated; written.characterAnimation["write"] = true;
    auto render = [&](const Layer &l, int frame) {
      Scene s = sc; s.layers = {l, bg}; s.frame = frame;
      return renderCPU(s, frame, true, QSize(640, 240)).convertToFormat(QImage::Format_RGB32);
    };
    auto redPixels = [](const QImage &image) {
      int n = 0;
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) { const QRgb c = image.pixel(x, y); n += qRed(c) > 200 && qGreen(c) < 60 && qBlue(c) < 60; }
      return n;
    };
    const QImage animatedDone = render(animated, 60), writtenDone = render(written, 60);
    int differ = 0;
    for (int y = 0; y < 240; ++y)
      for (int x = 0; x < 640; ++x) {
        const QRgb a2 = animatedDone.pixel(x, y), b2 = writtenDone.pixel(x, y);
        differ += std::max({std::abs(qRed(a2) - qRed(b2)), std::abs(qGreen(a2) - qGreen(b2)), std::abs(qBlue(a2) - qBlue(b2))}) > 40;
      }
    check(redPixels(writtenDone) > 2000 && differ < 40,
          QString("handwritten letters keep their Outside stroke once written (%1 red pixels, %2 differ from the same text animated)")
              .arg(redPixels(writtenDone)).arg(differ));
    // Fading in, the stroke stays outside: inside the letters (white when
    // still) nothing turns red.
    const QImage plain = render(still, 60), fading = render(animated, 4);
    double tint = 0; int inside = 0;
    for (int y = 0; y < 240; ++y)
      for (int x = 0; x < 640; ++x) {
        const QRgb c = plain.pixel(x, y);
        if (qRed(c) < 250 || qGreen(c) < 250 || qBlue(c) < 250) continue;
        const QRgb f = fading.pixel(x, y);
        tint += qRed(f) - qBlue(f); ++inside;
      }
    check(inside > 2000 && tint / inside < 8,
          QString("a letter fading in shows no Outside stroke through itself (redness inside %1)").arg(inside ? tint / inside : -1, 0, 'f', 1));
    // A Center stroke on still text is one band: no line of the letter's own
    // colour down its middle, where the two halves meet.
    Layer centred = still; centred.strokeMode = 2;
    const QImage band = render(centred, 60);
    // Across the red band the letter's white showed as a faint rise in green
    // between two pure-red neighbours.
    int seams = 0;
    for (int y = 0; y < 240; ++y)
      for (int x = 2; x < 638; ++x) {
        const QRgb c = band.pixel(x, y), l = band.pixel(x - 1, y), r = band.pixel(x + 1, y);
        if (qRed(c) < 240 || qRed(l) < 240 || qRed(r) < 240) continue;
        const int g = qGreen(c);
        seams += g >= 6 && g <= 140 && qGreen(l) <= g - 5 && qGreen(r) <= g - 5;
      }
    check(seams < 10, QString("a Center stroke on still text has no light seam down its middle (%1 seam pixels)").arg(seams));

    // A stack of Character strokes on animated letters is built the way still
    // text's is, from the letters' distance: at rest the same pixels, Miter
    // corners and all. Stroked with a wide pen per letter, a "W" became a
    // pentagon and the corners ran past the picture and were cut off.
    // The largest difference in any channel, and where it is.
    QString worstAt;
    auto worst = [&worstAt](const QImage &a, const QImage &b) {
      int most = 0;
      for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
          const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
          const int here = std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)), std::abs(qBlue(p) - qBlue(q))});
          if (here > most) {
            most = here;
            worstAt = QString(" at %1,%2: %3 against %4").arg(x).arg(y).arg(QColor(p).name(), QColor(q).name());
          }
        }
      return most;
    };
    Layer stack = still;
    stack.text = "wa";
    stack.fontSize = 100;
    stack.tracking = 80;
    stack.textStrokes = {{6, QColor("#1A3CF0"), 1}, {6, QColor("#D04050"), 1}, {6, QColor("#F4F4F4"), 1}, {8, QColor("#3A4048"), 1}};
    syncLegacyTextStroke(stack);
    Layer stackAnimated = stack;
    stackAnimated.characterAnimation = QJsonObject{{"enabled", true}, {"opacity", 0}, {"y", 0}, {"delay", 0.}};
    stackAnimated.p[6] = Property();
    stackAnimated.p[6].base = 100;
    const QImage stillStack = render(stack, 60);
    const int atRest = worst(stillStack, render(stackAnimated, 60));
    check(atRest <= 2, QString("animated letters at rest have the still text's stack of strokes (off by %1%2)").arg(atRest).arg(worstAt));
    // Fading, each letter fades whole -- strokes and all, one picture -- and
    // not each band over the one before: letters at 70 % are the still text at
    // a layer opacity of 70 %. Faded band by band, the strokes stayed strong
    // while the letters went.
    stackAnimated.p[6].base = 70;
    Layer stackFaded = stack;
    stackFaded.p[5].base = .7;
    const int faded = worst(render(stackFaded, 60), render(stackAnimated, 60));
    check(faded <= 3, QString("animated letters fading fade their strokes with them (off by %1%2)").arg(faded).arg(worstAt));
    // Moving, the letters' picture is cut to where they are, out to their
    // Miter corners' tips: every letter 20 down is the still text 20 down.
    Layer dropped = stackAnimated;
    dropped.characterAnimation["y"] = 40;
    dropped.characterAnimation["opacity"] = 100;
    dropped.p[6].base = 50;
    Layer lowered = stack;
    lowered.p[1].base += 20;
    const int moved = worst(render(lowered, 60), render(dropped, 60));
    check(moved <= 3, QString("moving letters keep the whole of their strokes (off by %1 from the still text moved as far%2)").arg(moved).arg(worstAt));

    // The Stroke effect on text is drawn the way a Character stroke is, from
    // the letters' distance: the same pixels as a Character stroke as wide,
    // still or animated, narrow or wide. Drawn with a pen round the letters'
    // outlines united, a wide one came apart -- holes where the union failed,
    // a "W" grown into a hexagon.
    for (double width : {12., 120.}) {
      Layer character = still;
      character.text = "wa";
      character.fontSize = 100;
      character.tracking = 80;
      character.textStrokes = {{width, QColor("#D04050"), 1}};
      syncLegacyTextStroke(character);
      Layer effect = character;
      effect.textStrokes.clear();
      syncLegacyTextStroke(effect);
      effect.appliedEffects["Stroke"] = true;
      effect.surfaceEffects["Stroke"] = QJsonObject{{"width", width}, {"mode", 1}, {"color", "#D04050"}};
      const QImage wanted = render(character, 60);
      const int off = worst(wanted, render(effect, 60));
      check(off <= 2, QString("a Stroke effect %1 wide on text is a Character stroke as wide (off by %2)").arg(width).arg(off));
      Layer moving = effect;
      moving.characterAnimation = QJsonObject{{"enabled", true}, {"opacity", 0}, {"y", 0}, {"delay", 0.}};
      moving.p[6] = Property();
      moving.p[6].base = 100;
      const int animatedOff = worst(wanted, render(moving, 60));
      check(animatedOff <= 2, QString("a Stroke effect %1 wide on animated letters at rest is the still one (off by %2)").arg(width).arg(animatedOff));
    }
    // A text with a wide Stroke effect, larger than a raster kept whole, is
    // drawn whole too: cut into bands, every band built the letters' distance
    // field again over most of them -- twenty times a frame, 600 ms.
    {
      Layer wide = still;
      wide.text = "wa";
      wide.fontSize = 100;
      wide.tracking = 80;
      wide.textStrokes.clear();
      syncLegacyTextStroke(wide);
      wide.appliedEffects["Stroke"] = true;
      wide.surfaceEffects["Stroke"] = QJsonObject{{"width", 190.}, {"mode", 1}, {"color", "#D04050"}, {"join", "round"}};
      Scene s = sc;
      s.layers = {wide};
      s.frame = 60;
      const quint64 before = strokeFieldComputations();
      renderCPU(s, 60, true, QSize(2560, 960));
      const quint64 built = strokeFieldComputations() - before;
      check(built <= 2, QString("a wide Stroke effect on text builds its distance field once a frame (%1 times)").arg(built));
    }
  }
  return failures.isEmpty()?0:1;
}
