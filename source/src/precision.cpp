#include "engine.h"
#include "psd.h"
#include "render_workers.h"
#include "gpu_pixels.h"
#include "route_choice.h"
#include <QColorSpace>
#include <array>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <QtEndian>
#include <cstring>
#include <vector>
#include <atomic>
#include <future>
#include <optional>

QJsonObject encodeFloatImage(const QImage &image) {
  auto img=image.convertToFormat(QImage::Format_RGBA32FPx4);
  const int w=img.width(),h=img.height();
  QByteArray bytes(qsizetype(w)*h*16,Qt::Uninitialized);
  auto *dst=reinterpret_cast<uchar *>(bytes.data());
  const auto *imgBits=img.constBits();const auto imgStride=img.bytesPerLine();
  renderParallel(h,[&](int begin,int end){
    for(int y=begin;y<end;++y) for(int x=0;x<w*4;++x) {
      quint32 bits; std::memcpy(&bits,imgBits+y*imgStride+x*4,4);
      qToLittleEndian(bits,dst+(qsizetype(y)*w*4+x)*4);
    }
  });
  return {{"width",w},{"height",h},{"rgba32fLE",QString::fromLatin1(qCompress(bytes).toBase64())}};
}
QImage decodeFloatImage(const QJsonObject &object) {
  int w=object["width"].toInt(),h=object["height"].toInt();
  if(w<=0||h<=0||quint64(w)*h>33000000) return {};
  auto packed=QByteArray::fromBase64(object["rgba32fLE"].toString().toLatin1());
  const quint64 expected=quint64(w)*h*16;
  if(packed.size()<4 || qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(packed.constData()))!=expected) return {};
  auto bytes=qUncompress(packed); if(quint64(bytes.size())!=expected) return {};
  QImage img(w,h,QImage::Format_RGBA32FPx4); if(img.isNull()) return {};
  std::atomic<bool> invalid{false};
  const auto *src=reinterpret_cast<const uchar *>(bytes.constData());
  auto *imgBits=img.bits();const auto imgStride=img.bytesPerLine();
  renderParallel(h,[&](int begin,int end){
    for(int y=begin;y<end;++y) {
      if(invalid.load(std::memory_order_relaxed)) return;
      auto *dst=imgBits+y*imgStride;
      for(int x=0;x<w*4;++x) {
        auto bits=qFromLittleEndian<quint32>(src+(qsizetype(y)*w*4+x)*4);
        float value; std::memcpy(&value,&bits,4);
        if(!std::isfinite(value)){invalid.store(true,std::memory_order_relaxed);return;}
        std::memcpy(dst+x*4,&value,4);
      }
    }
  });
  if(invalid) return {};
  return img;
}

QImage::Format processingFormat(int depth, bool premultiplied) {
  if (depth == 32) return premultiplied ? QImage::Format_RGBA32FPx4_Premultiplied : QImage::Format_RGBA32FPx4;
  if (depth == 16) return premultiplied ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA64;
  return premultiplied ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBA8888;
}

// Three separable box passes approximate a Gaussian, with transparent borders.
// SSE2 is part of every x86-64 CPU, so it needs no runtime check, and its
// double-precision add, subtract and multiply are the same IEEE operations the
// scalar code performs: the sweeps below give bit-identical results either way.
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define WAVY_BLUR_SSE2 1
#endif
// Accumulate premultiplied floats, preserving HDR RGB and avoiding alpha halos.
// Three box passes over the whole image: the exact result, for radii small
// enough that it is also the cheap one.
static QImage softenInterleaved(QImage input, int r) {
  // One division here instead of one per channel per pixel per sweep: on a
  // small raster the divisions were most of the blur.
  const double inverse = 1.0 / (2 * r + 1);
  QImage scratch(input.size(), QImage::Format_RGBA32FPx4_Premultiplied);
  if(scratch.isNull())throw std::runtime_error("Cannot allocate high precision blur");
  for (int pass = 0; pass < 3; ++pass) {
    for (int axis = 0; axis < 2; ++axis) {
      auto &out=scratch;
      const int lines = axis ? input.width() : input.height();
      const int length = axis ? input.height() : input.width();
      auto *outBits=out.bits();const auto outStride=out.bytesPerLine();
      const auto *inputBits=input.constBits();const auto inputStride=input.bytesPerLine();
      if(axis) {
        // Adjacent columns share cache lines. Traverse tiles row-by-row instead
        // of walking the entire large image once for every single column.
        renderParallel(input.width(),[&](int begin,int end){
          for(int x0=begin;x0<end;x0+=32) {
            const int channels=std::min(32,end-x0)*4;
            double sum[128]{};
            auto row=[&](int y){return reinterpret_cast<const float *>(inputBits+y*inputStride)+x0*4;};
#ifdef WAVY_BLUR_SSE2
            // channels is a multiple of four: whole pixels, four floats each.
            auto accumulate=[&](const float *p,bool subtract) {
              for(int c=0;c<channels;c+=4) {
                const __m128 f=_mm_loadu_ps(p+c);
                const __m128d a=_mm_cvtps_pd(f), b=_mm_cvtps_pd(_mm_movehl_ps(f,f));
                const __m128d s0=_mm_loadu_pd(sum+c), s1=_mm_loadu_pd(sum+c+2);
                _mm_storeu_pd(sum+c,subtract?_mm_sub_pd(s0,a):_mm_add_pd(s0,a));
                _mm_storeu_pd(sum+c+2,subtract?_mm_sub_pd(s1,b):_mm_add_pd(s1,b));
              }
            };
            const __m128d scale=_mm_set1_pd(inverse);
            for(int y=0;y<=std::min(r,input.height()-1);++y) accumulate(row(y),false);
            for(int y=0;y<input.height();++y) {
              auto *dest=reinterpret_cast<float *>(outBits+y*outStride)+x0*4;
              for(int c=0;c<channels;c+=4) {
                const __m128 lo=_mm_cvtpd_ps(_mm_mul_pd(_mm_loadu_pd(sum+c),scale));
                const __m128 hi=_mm_cvtpd_ps(_mm_mul_pd(_mm_loadu_pd(sum+c+2),scale));
                _mm_storeu_ps(dest+c,_mm_movelh_ps(lo,hi));
              }
              if(y-r>=0) accumulate(row(y-r),true);
              if(y+r+1<input.height()) accumulate(row(y+r+1),false);
            }
#else
            for(int y=0;y<=std::min(r,input.height()-1);++y) {
              const auto *p=row(y);for(int c=0;c<channels;++c)sum[c]+=p[c];
            }
            for(int y=0;y<input.height();++y) {
              auto *dest=reinterpret_cast<float *>(outBits+y*outStride)+x0*4;
              for(int c=0;c<channels;++c)dest[c]=float(sum[c]*inverse);
              if(y-r>=0){const auto *p=row(y-r);for(int c=0;c<channels;++c)sum[c]-=p[c];}
              if(y+r+1<input.height()){const auto *p=row(y+r+1);for(int c=0;c<channels;++c)sum[c]+=p[c];}
            }
#endif
          }
        });
        input.swap(out);
        continue;
      }
      renderParallel(lines,[&](int begin,int end){for (int line = begin; line < end; ++line) {
        auto pixel = [&](int n) {
          return reinterpret_cast<const float *>(inputBits+line*inputStride) + n*4;
        };
#ifdef WAVY_BLUR_SSE2
        // axis is 0 here: a row, swept left to right.
        __m128d lo=_mm_setzero_pd(), hi=_mm_setzero_pd();
        const __m128d scale=_mm_set1_pd(inverse);
        auto accumulate=[&](const float *p,bool subtract) {
          const __m128 f=_mm_loadu_ps(p);
          const __m128d a=_mm_cvtps_pd(f), b=_mm_cvtps_pd(_mm_movehl_ps(f,f));
          lo=subtract?_mm_sub_pd(lo,a):_mm_add_pd(lo,a);
          hi=subtract?_mm_sub_pd(hi,b):_mm_add_pd(hi,b);
        };
        for (int n = 0; n <= std::min(r,length-1); ++n) accumulate(pixel(n),false);
        auto *destRow = reinterpret_cast<float *>(outBits+line*outStride);
        for (int n = 0; n < length; ++n) {
          _mm_storeu_ps(destRow+n*4,_mm_movelh_ps(_mm_cvtpd_ps(_mm_mul_pd(lo,scale)),
                                                   _mm_cvtpd_ps(_mm_mul_pd(hi,scale))));
          if (n-r >= 0) accumulate(pixel(n-r),true);
          if (n+r+1 < length) accumulate(pixel(n+r+1),false);
        }
#else
        double sum[4]{};
        for (int n = 0; n <= std::min(r,length-1); ++n)
          for (int c = 0; c < 4; ++c) sum[c] += pixel(n)[c];
        for (int n = 0; n < length; ++n) {
          auto *dest = reinterpret_cast<float *>(outBits+(axis ? n : line)*outStride) + (axis ? line : n)*4;
          for (int c = 0; c < 4; ++c) dest[c] = float(sum[c]*inverse);
          if (n-r >= 0) for (int c = 0; c < 4; ++c) sum[c] -= pixel(n-r)[c];
          if (n+r+1 < length) for (int c = 0; c < 4; ++c) sum[c] += pixel(n+r+1)[c];
        }
#endif
      }});
      input.swap(out);
    }
  }
  return input;
}
// One box pass along a line of `length` pixels, four floats each and `stride`
// floats apart: the window is [n-r, n+r] clipped to the ends of the line and
// always divided by 2r+1, which is what the sweep above does when it reads and
// writes whole images. Sums are kept in double and each result is rounded to
// float exactly where the image would have rounded it, so a stage here holds
// the numbers a stage there held.
static void boxLine(const float *in, int inStride, float *out, int outStride,
                    int length, int r, double inverse) {
  const int first = std::min(r, length - 1);
#ifdef WAVY_BLUR_SSE2
  __m128d lo = _mm_setzero_pd(), hi = _mm_setzero_pd();
  const __m128d scale = _mm_set1_pd(inverse);
  auto accumulate = [&](const float *p, bool subtract) {
    const __m128 f = _mm_loadu_ps(p);
    const __m128d a = _mm_cvtps_pd(f), b = _mm_cvtps_pd(_mm_movehl_ps(f, f));
    lo = subtract ? _mm_sub_pd(lo, a) : _mm_add_pd(lo, a);
    hi = subtract ? _mm_sub_pd(hi, b) : _mm_add_pd(hi, b);
  };
  for (int n = 0; n <= first; ++n) accumulate(in + n * inStride, false);
  for (int n = 0; n < length; ++n) {
    _mm_storeu_ps(out + n * outStride,
                  _mm_movelh_ps(_mm_cvtpd_ps(_mm_mul_pd(lo, scale)),
                                _mm_cvtpd_ps(_mm_mul_pd(hi, scale))));
    if (n - r >= 0) accumulate(in + (n - r) * inStride, true);
    if (n + r + 1 < length) accumulate(in + (n + r + 1) * inStride, false);
  }
#else
  double sum[4]{};
  for (int n = 0; n <= first; ++n)
    for (int c = 0; c < 4; ++c) sum[c] += in[n * inStride + c];
  for (int n = 0; n < length; ++n) {
    for (int c = 0; c < 4; ++c) out[n * outStride + c] = float(sum[c] * inverse);
    if (n - r >= 0) for (int c = 0; c < 4; ++c) sum[c] -= in[(n - r) * inStride + c];
    if (n + r + 1 < length) for (int c = 0; c < 4; ++c) sum[c] += in[(n + r + 1) * inStride + c];
  }
#endif
}
// The same three box passes, but all three of one axis in a single sweep.
//
// Interleaving them (across, down, across, down, across, down) walks the whole
// image six times, and at a small radius almost none of that time is the
// arithmetic: it is carrying tens of megabytes past the processor over and over.
// Box passes along different axes commute, so doing the three across passes
// first and then the three down ones gives the same picture while touching the
// image twice. What lies between the passes is a line (or a narrow band of
// columns) small enough to stay in cache, holding the same numbers the images
// held, so only the order of the roundings differs.
//
// Written into `out`, the input's size: a picture of its own, or a view of the
// part of a larger one it goes in.
static void softenFusedInto(const QImage &input, int r, QImage &out) {
  const double inverse = 1.0 / (2 * r + 1);
  const int width = input.width(), height = input.height();
  {
    const auto *inputBits = input.constBits(); const auto inputStride = input.bytesPerLine();
    auto *outBits = out.bits(); const auto outStride = out.bytesPerLine();
    renderParallel(height, [&](int begin, int end) {
      std::vector<float> first(size_t(width) * 4), second(size_t(width) * 4);
      for (int y = begin; y < end; ++y) {
        const auto *source = reinterpret_cast<const float *>(inputBits + y * inputStride);
        auto *dest = reinterpret_cast<float *>(outBits + y * outStride);
        boxLine(source, 4, first.data(), 4, width, r, inverse);
        boxLine(first.data(), 4, second.data(), 4, width, r, inverse);
        boxLine(second.data(), 4, dest, 4, width, r, inverse);
      }
    });
  }
  {
    // A band of columns at a time, copied out whole rows at a time: a single
    // column of a large image is one cache miss per row, while the band is a
    // run of neighbouring pixels on every row it takes and small enough to
    // stay in cache while its columns are swept. Each band is read out whole
    // before it is written back, so this works on the picture where it lies:
    // handing the caller's own image back to be written into would have made
    // Qt copy it first, another two walks over tens of megabytes.
    constexpr int band = 8;
    auto *outBits = out.bits(); const auto outStride = out.bytesPerLine();
    const auto *inputBits = outBits; const auto inputStride = outStride;
    // One band at a time to whichever worker is free: a band is a lot of work,
    // and handing them out in equal shares left most of the cores idle.
    renderParallelEach((width + band - 1) / band, [&](int block) {
      static thread_local std::vector<float> columns, first, second;
      columns.resize(size_t(band) * size_t(height) * 4);
      first.resize(size_t(height) * 4); second.resize(size_t(height) * 4);
      {
        const int x0 = block * band, count = std::min(band, width - x0);
        if (count <= 0) return;
        const size_t rowFloats = size_t(count) * 4;
        for (int y = 0; y < height; ++y)
          std::memcpy(&columns[size_t(y) * size_t(band) * 4],
                      reinterpret_cast<const float *>(inputBits + y * inputStride) + size_t(x0) * 4,
                      rowFloats * sizeof(float));
        for (int c = 0; c < count; ++c) {
          float *column = &columns[size_t(c) * 4];
          boxLine(column, band * 4, first.data(), 4, height, r, inverse);
          boxLine(first.data(), 4, second.data(), 4, height, r, inverse);
          boxLine(second.data(), 4, column, band * 4, height, r, inverse);
        }
        for (int y = 0; y < height; ++y)
          std::memcpy(reinterpret_cast<float *>(outBits + y * outStride) + size_t(x0) * 4,
                      &columns[size_t(y) * size_t(band) * 4], rowFloats * sizeof(float));
      }
    });
  }
}
static QImage softenFused(QImage input, int r) {
  QImage out(input.size(), QImage::Format_RGBA32FPx4_Premultiplied);
  if (out.isNull()) throw std::runtime_error("Cannot allocate high precision blur");
  softenFusedInto(input, r, out);
  return out;
}
static QImage softenExact(QImage input, int r) {
  // The lines the fused sweep holds in cache are as long as the picture. Past a
  // size where they no longer fit, walking the image is the cheaper of the two.
  if (std::max(input.width(), input.height()) <= 8192 && input.width() > 0 && input.height() > 0)
    return softenFused(std::move(input), r);
  return softenInterleaved(std::move(input), r);
}
static QImage boxDownsample(const QImage &src, int k) {
  const int w=std::max(1,src.width()/k), h=std::max(1,src.height()/k);
  QImage out(w,h,QImage::Format_RGBA32FPx4_Premultiplied);
  if(out.isNull())throw std::runtime_error("Cannot allocate blur downsample");
  auto *outBits=out.bits();const auto outStride=out.bytesPerLine();
  const auto *srcBits=src.constBits();const auto srcStride=src.bytesPerLine();
  renderParallel(h,[&](int begin,int end){for(int y=begin;y<end;++y) {
    auto *dest=reinterpret_cast<float *>(outBits+y*outStride);
    const int y0=int(qint64(y)*src.height()/h), y1=int(qint64(y+1)*src.height()/h);
    for(int x=0;x<w;++x) {
      const int x0=int(qint64(x)*src.width()/w), x1=int(qint64(x+1)*src.width()/w);
      double sum[4]{};
      for(int sy=y0;sy<y1;++sy) {
        const auto *p=reinterpret_cast<const float *>(srcBits+sy*srcStride);
        for(int sx=x0;sx<x1;++sx) for(int c=0;c<4;++c) sum[c]+=p[sx*4+c];
      }
      const double count=std::max(1,(y1-y0)*(x1-x0));
      for(int c=0;c<4;++c) dest[x*4+c]=float(sum[c]/count);
    }
  }});
  return out;
}
static QImage bilinearUpsample(const QImage &src, QSize size) {
  QImage out(size,QImage::Format_RGBA32FPx4_Premultiplied);
  if(out.isNull())throw std::runtime_error("Cannot allocate blur upsample");
  auto *outBits=out.bits();const auto outStride=out.bytesPerLine();
  const auto *srcBits=src.constBits();const auto srcStride=src.bytesPerLine();
  const double stepX=double(src.width())/size.width(), stepY=double(src.height())/size.height();
  renderParallel(size.height(),[&](int begin,int end){for(int y=begin;y<end;++y) {
    const double fy=(y+.5)*stepY-.5;
    const int y0=std::clamp(int(std::floor(fy)),0,src.height()-1);
    const int y1=std::clamp(int(std::floor(fy))+1,0,src.height()-1);
    const double ty=std::clamp(fy-std::floor(fy),0.,1.);
    const auto *rowA=reinterpret_cast<const float *>(srcBits+y0*srcStride);
    const auto *rowB=reinterpret_cast<const float *>(srcBits+y1*srcStride);
    auto *dest=reinterpret_cast<float *>(outBits+y*outStride);
    for(int x=0;x<size.width();++x) {
      const double fx=(x+.5)*stepX-.5;
      const int x0=std::clamp(int(std::floor(fx)),0,src.width()-1);
      const int x1=std::clamp(int(std::floor(fx))+1,0,src.width()-1);
      const double tx=std::clamp(fx-std::floor(fx),0.,1.);
      for(int c=0;c<4;++c) {
        const double top=rowA[x0*4+c]*(1-tx)+rowA[x1*4+c]*tx;
        const double bottom=rowB[x0*4+c]*(1-tx)+rowB[x1*4+c]*tx;
        dest[x*4+c]=float(top*(1-ty)+bottom*ty);
      }
    }
  }});
  return out;
}
// ---------------------------------------------------------------------------
// Colour management.
// ---------------------------------------------------------------------------
namespace {
thread_local bool g_linearLight = false;
float encodedToLinear(float c) {
  const float m = std::abs(c);
  const float v = m <= .04045f ? m / 12.92f : std::pow((m + .055f) / 1.055f, 2.4f);
  return c < 0 ? -v : v;
}
float linearToEncoded(float c) {
  const float m = std::abs(c);
  const float v = m <= .0031308f ? m * 12.92f : 1.055f * std::pow(m, 1.f / 2.4f) - .055f;
  return c < 0 ? -v : v;
}
// sRGB's curve looked up in 4096 steps between black and white, and worked out
// exactly past them: a float picture can be brighter than white.
struct Curve {
  std::array<float, 4097> table;
  float (*exact)(float);
  explicit Curve(float (*f)(float)) : exact(f) {
    for (int i = 0; i <= 4096; ++i) table[size_t(i)] = f(float(i) / 4096.f);
  }
  float operator()(float c) const {
    if (!(c >= 0.f) || c > 1.f) return exact(c);
    const float at = c * 4096.f;
    const int i = std::min(int(at), 4095);
    return table[size_t(i)] + (table[size_t(i) + 1] - table[size_t(i)]) * (at - float(i));
  }
};
const Curve &linearCurve() { static const Curve curve(encodedToLinear); return curve; }
const Curve &encodedCurve() { static const Curve curve(linearToEncoded); return curve; }
// Premultiplied colour is taken apart by its alpha, re-curved, and put back.
void recode(QImage &image, const Curve &curve) {
  if (image.format() != QImage::Format_RGBA32FPx4_Premultiplied)
    image = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  uchar *bits = image.bits();
  const qsizetype stride = image.bytesPerLine();
  const int width = image.width();
  renderParallel(image.height(), [&](int from, int to) {
    for (int y = from; y < to; ++y) {
      auto *p = reinterpret_cast<float *>(bits + stride * y);
      for (int x = 0; x < width; ++x, p += 4) {
        const float a = p[3];
        if (a <= 0.f) continue;
        for (int c = 0; c < 3; ++c) p[c] = curve(p[c] / a) * a;
      }
    }
  });
}
}  // namespace
bool effectsInLinearLight() { return g_linearLight; }
LinearLightScope::LinearLightScope(bool on) : previous(g_linearLight) { g_linearLight = on; }
LinearLightScope::~LinearLightScope() { g_linearLight = previous; }
void toLinearLight(QImage &image) { recode(image, linearCurve()); }
void fromLinearLight(QImage &image) { recode(image, encodedCurve()); }
void fromLinearLightAsLight(QImage &image) {
  if (image.format() != QImage::Format_RGBA32FPx4_Premultiplied)
    image = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  const Curve &curve = encodedCurve();
  uchar *bits = image.bits();
  const qsizetype stride = image.bytesPerLine();
  const int width = image.width();
  // Over black the right answer is the encoded light itself; its coverage is
  // encoded the same way, which keeps white on white white and the colour
  // never brighter than the coverage.
  renderParallel(image.height(), [&](int from, int to) {
    for (int y = from; y < to; ++y) {
      auto *p = reinterpret_cast<float *>(bits + stride * y);
      for (int x = 0; x < width; ++x, p += 4) {
        const float a = p[3];
        if (a <= 0.f) continue;
        if (a >= 1.f) {
          // Solid: the plain encoding, brighter than white included.
          for (int c = 0; c < 3; ++c) p[c] = curve(p[c] / a) * a;
          continue;
        }
        const float coverage = curve(a);
        for (int c = 0; c < 3; ++c) p[c] = std::min(curve(p[c]), coverage);
        p[3] = coverage;
      }
    }
  });
}
QImage imageInWorkingSpace(QImage image) {
  if (image.isNull()) return image;
  const QColorSpace space = image.colorSpace();
  if (!space.isValid() || space == QColorSpace(QColorSpace::SRgb)) return image;
  if (space.colorModel() != QColorSpace::ColorModel::Rgb) return image;
  if (image.format() == QImage::Format_Indexed8 || image.format() == QImage::Format_Mono ||
      image.format() == QImage::Format_MonoLSB)
    image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
  image.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
  return image;
}

QImage fullResolutionBlur(QImage source,double radius) {
  if(radius<=0)return source;
  return softenExact(source.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied),
                     std::clamp(int(std::ceil(radius/2)),1,4096));
}
QImage fullResolutionBlurPart(const QImage &source, double radius, QRect part) {
  // A picture that could not be made fails as fullResolutionBlur fails on it:
  // handed back null, a blurred background was laid sharp and whole.
  if (source.isNull() && radius > 0) throw std::runtime_error("Cannot allocate high precision blur");
  part &= source.rect();
  if (part.isEmpty()) return QImage();
  const QRect rows(0, part.top(), source.width(), part.height());
  if (radius <= 0) return source.copy(rows);
  if (part == source.rect()) return fullResolutionBlur(source, radius);
  // The part's pixels converted where they lie, no copy first.
  const QImage within(source.constBits() + part.top() * source.bytesPerLine() + part.left() * (source.depth() / 8),
                      part.width(), part.height(), source.bytesPerLine(), source.format());
  const QImage input = within.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  QImage out(rows.size(), QImage::Format_RGBA32FPx4_Premultiplied);
  if (input.isNull() || out.isNull()) throw std::runtime_error("Cannot allocate high precision blur");
  // The columns either side, transparent.
  uchar *const bits = out.bits();
  const qsizetype stride = out.bytesPerLine();
  const size_t pixel = 4 * sizeof(float);
  renderParallel(out.height(), [&](int from, int to) {
    for (int y = from; y < to; ++y) {
      uchar *line = bits + y * stride;
      std::memset(line, 0, size_t(part.left()) * pixel);
      std::memset(line + size_t(part.right() + 1) * pixel, 0, size_t(out.width() - part.right() - 1) * pixel);
    }
  });
  const int r = std::clamp(int(std::ceil(radius / 2)), 1, 4096);
  if (std::max(input.width(), input.height()) <= 8192) {
    QImage into(bits + part.left() * pixel, part.width(), part.height(), stride, out.format());
    softenFusedInto(input, r, into);
  } else {
    const QImage blurred = softenExact(input, r);
    renderParallel(out.height(), [&](int from, int to) {
      for (int y = from; y < to; ++y)
        std::memcpy(bits + y * stride + part.left() * pixel, blurred.constScanLine(y), size_t(part.width()) * pixel);
    });
  }
  return out;
}
static QImage softenPass(QImage input, int r) {
  // On the card or the processor, whichever is the quicker here for a blur
  // this wide on a picture this size (route_choice.h).
  static RouteChoice narrow("Blur"), wide("Wide blur");
  QImage result;
  quickerWay(r <= 32 ? narrow : wide, qint64(input.width()) * input.height(), canUseGpuPixels(),
             [&] { result = gpuPixelOperation(input, 2, {float(r), 0, 0, 0}); return !result.isNull(); },
             [&] {
               if (input.format() != QImage::Format_RGBA32FPx4_Premultiplied)
                 input = input.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
               result = softenExact(input, r);
             });
  return result;
}
// The resolution a blur is resolved at. A box blur of half-width r carries no
// detail finer than r, so once r is wide, sweeping every full-resolution pixel
// is arithmetic the result cannot express -- and at a high layer scale r is
// hundreds of pixels. The kernel is held at twelve samples or wider: at eight
// the worst pixel drifted 9/255 on a hard-edged test, at twelve it is 5/255 and
// the speed is the same, because what is left costs the two full-resolution
// sweeps rather than the blur. The layer's own sharp pixels are never touched.
// The reduced image also has to stay big enough for the interpolation back not
// to show: on a blur nearly as wide as the picture, a handful of samples cannot
// be stretched over it. Sixty-four pixels on the shorter side is the floor.
int blurReduction(double radius, QSize size) {
  const int r = std::clamp(int(std::ceil(radius/2)), 1, 4096);
  const int room = std::min(size.width(), size.height()) / 64;
  return std::clamp(std::min(r / 12, room), 1, 16);
}
static QImage soften(QImage input, double radius) {
  const int r = std::clamp(int(std::ceil(radius/2)), 1, 4096);
  const int k = blurReduction(radius, input.size());
  if(k<=1) return softenPass(std::move(input), r);
  if(input.format()!=QImage::Format_RGBA32FPx4_Premultiplied)
    input=input.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  const QSize full=input.size();
  auto small=softenPass(boxDownsample(input,k),std::max(1,(r+k/2)/k));
  if(small.format()!=QImage::Format_RGBA32FPx4_Premultiplied)
    small=small.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  return bilinearUpsample(small, full);
}

// One soft copy laid behind the layer: a drop shadow is offset and black, a
// glow sits centred in the layer's own colour. Both expect premultiplied float.
//
// A halo carries no detail finer than its own blur, so soften() resolves a wide
// one on a reduced image and interpolates it back. What is left at very high
// layer scales is the pair of full-resolution sweeps that the reduction and the
// interpolation still cost: on a wide glow that raster is tens of megabytes, so
// drawing the halo's own source small is the step after this one.
QImage renderHaloOnly(QImage source,double radius,QColor color) {
  source=source.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  // A glow spreads in linear light when the Main asks; a tinted halo (a
  // shadow) is only its alpha, which has no curve to undo.
  const bool linear=effectsInLinearLight() && !color.isValid();
  if(linear) toLinearLight(source);
  source=soften(source,radius);
  if(linear) fromLinearLightAsLight(source);
  // An invalid tint denotes source-colour Glow, preserving blurred RGB/HDR.
  if(!color.isValid())return source;
  auto *bits=source.bits();const auto stride=source.bytesPerLine();
  renderParallel(source.height(),[&](int begin,int end){for(int y=begin;y<end;++y){
    auto *p=reinterpret_cast<float *>(bits+y*stride);
    for(int x=0;x<source.width();++x){float alpha=std::clamp(p[x*4+3],0.f,1.f)*color.alphaF();
      p[x*4]=color.redF()*alpha;p[x*4+1]=color.greenF()*alpha;p[x*4+2]=color.blueF()*alpha;p[x*4+3]=alpha;}
  }});
  return source;
}
static void haloBehind(QImage &image, double radius, QColor color, QPointF offset, double scale) {
  const double blurRadius = std::max(0., radius*scale);
  // One submission on the graphics card, or none: the whole halo is resolved
  // there when it can be, rather than crossing the bus once per pass -- and
  // only when that is the quicker here (route_choice.h).
  static RouteChoice route("Glow and shadow");
  const qint64 pixels = qint64(image.width()) * image.height();
  const bool cardCan = canUseGpuPixels();
  if (cardCan && route.card(pixels)) {
    QElapsedTimer clock;
    clock.start();
    auto gpu = gpuHaloBehind(image, blurRadius, color, offset*scale);
    if(!gpu.isNull() && gpu.size()==image.size()){image=gpu;route.took(pixels,true,clock.nsecsElapsed()/1e6);return;}
    route.refused(pixels);
  }
  const ProcessorTiming timing(cardCan ? &route : nullptr, pixels);

  QImage halo = soften(image, blurRadius);
  if(offset.isNull()) {
    // Same-sized, aligned images need no bilinear sampling. Fuse tint and
    // source-over into a single contiguous pass (the usual Glow path).
    auto *bits=image.bits();const auto stride=image.bytesPerLine();
    const auto *blurred=halo.constBits();const auto blurStride=halo.bytesPerLine();
    const bool tint=color.isValid();
    const float cr=color.redF(),cg=color.greenF(),cb=color.blueF(),ca=color.alphaF();
    renderParallel(image.height(),[&](int begin,int end){for(int y=begin;y<end;++y) {
      auto *dst=reinterpret_cast<float *>(bits+y*stride);
      const auto *src=reinterpret_cast<const float *>(blurred+y*blurStride);
      for(int x=0;x<image.width();++x) {
        const float clear=1.f-std::clamp(dst[3],0.f,1.f);
        if(tint) {
          const float a=std::clamp(src[3],0.f,1.f)*ca*clear;
          dst[0]+=cr*a;dst[1]+=cg*a;dst[2]+=cb*a;dst[3]+=a;
        } else for(int c=0;c<4;++c)dst[c]+=src[c]*clear;
        dst+=4;src+=4;
      }
    }});
    return;
  }

  // Colour is constant, so tinting before the interpolation gives the same
  // premultiplied result as tinting after it, at a fraction of the pixels.
  if(color.isValid()) {
    auto *bits=halo.bits();const auto stride=halo.bytesPerLine();
    const float cr=float(color.redF()),cg=float(color.greenF()),cb=float(color.blueF());
    const float ca=float(color.alphaF());
    renderParallel(halo.height(),[&](int begin,int end){for(int y=begin;y<end;++y) {
      auto *p=reinterpret_cast<float *>(bits+y*stride);
      for(int x=0;x<halo.width();++x) {
        const float a=std::clamp(p[x*4+3],0.f,1.f)*ca;
        p[x*4]=cr*a;p[x*4+1]=cg*a;p[x*4+2]=cb*a;p[x*4+3]=a;
      }
    }});
  }
  // The layer over the halo, in premultiplied form, sampling the halo where it
  // lies rather than materialising a full-size copy of it first.
  const double stepX=double(halo.width())/image.width(), stepY=double(halo.height())/image.height();
  const double offX=offset.x()*scale, offY=offset.y()*scale;
  auto *bits=image.bits();const auto stride=image.bytesPerLine();
  const auto *haloBits=halo.constBits();const auto haloStride=halo.bytesPerLine();
  // Where each column samples the halo does not depend on the row: worked out
  // once, not once per pixel. Same arithmetic as before, so the same numbers.
  struct Column { int x0,x1; float tx; bool inside; };
  std::vector<Column> columns(size_t(image.width()));
  for(int x=0;x<image.width();++x) {
    const double fx=(x-offX+.5)*stepX-.5;
    Column &column=columns[size_t(x)];
    column.inside=fx>-1 && fx<halo.width();
    if(!column.inside){column.x0=column.x1=0;column.tx=0;continue;}
    column.x0=std::clamp(int(std::floor(fx)),0,halo.width()-1);
    column.x1=std::clamp(int(std::floor(fx))+1,0,halo.width()-1);
    column.tx=float(std::clamp(fx-std::floor(fx),0.,1.));
  }
  // A tinted halo is all zero wherever its alpha is, so four transparent
  // neighbours mean nothing is laid down; most of a text raster is like that.
  const bool skipEmpty=color.isValid();
  renderParallel(image.height(),[&](int begin,int end){for(int y=begin;y<end;++y) {
    auto *p=reinterpret_cast<float *>(bits+y*stride);
    const double fy=(y-offY+.5)*stepY-.5;
    if(!(fy>-1 && fy<halo.height())) continue; // no halo on this row: adds zero everywhere
    const int y0=std::clamp(int(std::floor(fy)),0,halo.height()-1);
    const int y1=std::clamp(int(std::floor(fy))+1,0,halo.height()-1);
    const float ty=float(std::clamp(fy-std::floor(fy),0.,1.));
    const auto *rowA=reinterpret_cast<const float *>(haloBits+y0*haloStride);
    const auto *rowB=reinterpret_cast<const float *>(haloBits+y1*haloStride);
    for(int x=0;x<image.width();++x) {
      const Column &column=columns[size_t(x)];
      if(!column.inside) continue;
      const float *a0=rowA+column.x0*4,*a1=rowA+column.x1*4,*b0=rowB+column.x0*4,*b1=rowB+column.x1*4;
      if(skipEmpty && a0[3]==0.f && a1[3]==0.f && b0[3]==0.f && b1[3]==0.f) continue;
      const float tx=column.tx;
      float behind[4];
      for(int c=0;c<4;++c) {
        const float top=a0[c]*(1-tx)+a1[c]*tx;
        const float bottom=b0[c]*(1-tx)+b1[c]*tx;
        behind[c]=top*(1-ty)+bottom*ty;
      }
      const float clear=1.f-std::clamp(p[x*4+3],0.f,1.f);
      for(int c=0;c<4;++c) p[x*4+c]+=behind[c]*clear;
    }
  }});
}
namespace {
// While a pass works in linear light: the pixels go in linear and come back
// encoded, whatever the pass left in the variable.
struct InLinearLight {
  QImage &image;
  bool on, asLight;
  explicit InLinearLight(QImage &i, bool light = false) : image(i), on(effectsInLinearLight()), asLight(light) {
    if (on) toLinearLight(image);
  }
  ~InLinearLight() {
    if (!on) return;
    if (asLight) fromLinearLightAsLight(image);
    else fromLinearLight(image);
  }
};
}
void shadowPass(QImage &image, const Layer &l, double scale) {
  InLinearLight linear(image);
  haloBehind(image, l.shadow, QColor(0,0,0,200), {5,7}, scale);
}
void glowPass(QImage &image, const Layer &l, double scale) {
  // A glow is light: it comes back falling off as light does. A shadow and a
  // blur only mix their colours in linear light -- dark laid down as light
  // would come out far too heavy over anything bright.
  InLinearLight linear(image, true);
  haloBehind(image, l.glow, QColor(), {}, scale);
}
void blurPass(QImage &image, const Layer &l, double scale) {
  InLinearLight linear(image);
  image = soften(image, l.blur*scale);
}
QImage precisionEffects(QImage image, const Layer &layer, double scale) {
  auto outputFormat = image.format();
  image = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  if (layer.shadow) shadowPass(image, layer, scale);
  if (layer.glow) glowPass(image, layer, scale);
  if (layer.blur) blurPass(image, layer, scale);
  return image.convertToFormat(outputFormat);
}

int checkPrecision(QStringList &failures) {
  auto check=[&](bool ok, QString message) { if (!ok) failures << message; };
  {
    // The fused sweep (three passes of one axis at a time) against the
    // interleaved one it replaced: the same picture, give or take where the
    // roundings fall. Sizes that are not multiples of the band, and a radius
    // wider than the picture, so the clipped windows at the edges are covered.
    for (auto size : {QSize(67,65), QSize(120,8), QSize(9,140), QSize(301,203)})
      for (int r : {1, 3, 15, 400}) {
        QImage seed(size,QImage::Format_RGBA32FPx4_Premultiplied);
        for(int y=0;y<seed.height();++y){auto *p=reinterpret_cast<float *>(seed.scanLine(y));
          for(int x=0;x<seed.width();++x){const float a=float((x*7+y*11)%23)/23.f;
            p[x*4]=a*2;p[x*4+1]=a*.3f;p[x*4+2]=0;p[x*4+3]=a;}}
        const QImage fused=softenFused(seed,r), plain=softenInterleaved(seed,r);
        float worst=0;
        for(int y=0;y<seed.height();++y) {
          const auto *a=reinterpret_cast<const float *>(fused.constScanLine(y));
          const auto *b=reinterpret_cast<const float *>(plain.constScanLine(y));
          for(int i=0;i<seed.width()*4;++i) worst=std::max(worst,std::abs(a[i]-b[i]));
        }
        check(fused.size()==seed.size() && worst<1e-5f,
              QString("fused blur matches the interleaved one at %1x%2 r%3 (worst %4)")
                  .arg(size.width()).arg(size.height()).arg(r).arg(double(worst)));
      }
  }
  {
    // A part blurred alone (fullResolutionBlurPart): as far inside it as the
    // blur reaches, or out to the picture's own edges, what the whole
    // picture's blur has there; the part's rows only, the columns either side
    // transparent.
    QImage seed(301,203,QImage::Format_ARGB32_Premultiplied);
    for(int y=0;y<seed.height();++y){auto *p=reinterpret_cast<QRgb *>(seed.scanLine(y));
      for(int x=0;x<seed.width();++x){const int a=(x*7+y*11)%256;p[x]=qRgba(a*3%(a+1),a/2,a/3,a);}}
    for (double radius : {.53, 2.4, 6., 13.})
      for (const QRect part : {QRect(40,30,150,100), QRect(0,0,120,203), QRect(180,60,121,143), QRect(0,50,301,40)}) {
        const QImage whole=fullResolutionBlur(seed,radius), alone=fullResolutionBlurPart(seed,radius,part);
        const int reach=3*std::clamp(int(std::ceil(radius/2)),1,4096);
        bool fits=alone.size()==QSize(seed.width(),part.height()) && alone.format()==whole.format(), clear=true;
        float worst=0;
        for(int y=0;fits && y<part.height();++y) {
          const int at=part.top()+y;
          const bool rowInside=(at-part.top()>=reach || part.top()==0) &&
                               (part.bottom()-at>=reach || part.bottom()==seed.height()-1);
          const auto *a=reinterpret_cast<const float *>(alone.constScanLine(y));
          const auto *b=reinterpret_cast<const float *>(whole.constScanLine(at));
          for(int x=0;x<seed.width();++x) {
            if(x<part.left() || x>part.right()) {
              clear=clear && a[x*4]==0 && a[x*4+1]==0 && a[x*4+2]==0 && a[x*4+3]==0;
              continue;
            }
            if(rowInside && (x-part.left()>=reach || part.left()==0) && (part.right()-x>=reach || part.right()==seed.width()-1))
              for(int c=0;c<4;++c) worst=std::max(worst,std::abs(a[x*4+c]-b[x*4+c]));
          }
        }
        check(fits && clear && worst<1e-6f,
              QString("a part blurred alone matches the whole picture's blur at r%1, %2,%3 %4x%5 (worst %6)")
                  .arg(radius).arg(part.left()).arg(part.top()).arg(part.width()).arg(part.height()).arg(double(worst)));
      }
    bool threw=false;
    try { fullResolutionBlurPart(QImage(),3,QRect(0,0,4,4)); } catch(const std::runtime_error &) { threw=true; }
    check(threw,"a part of a picture that could not be made fails as the whole picture's blur does");
  }
  {
    QImage source(41,37,QImage::Format_RGBA32FPx4_Premultiplied);source.fill(Qt::transparent);
    auto *pixel=reinterpret_cast<float *>(source.scanLine(18))+20*4;
    pixel[0]=2.f;pixel[3]=1.f;
    const auto original=source.copy();
    auto halo=renderHaloOnly(source,12,QColor(0,255,0,128));
    check(source==original,"halo-only pass preserves its sharp source");
    auto *center=reinterpret_cast<const float *>(halo.constScanLine(18))+20*4;
    check(center[0]==0 && center[2]==0 && center[1]==center[3] && center[3]>0,
          "halo-only pass contains the tinted halo without original RGB");
  }
  {
    QImage odd(5,5,QImage::Format_RGBA32FPx4_Premultiplied);odd.fill(Qt::transparent);
    auto *last=reinterpret_cast<float *>(odd.scanLine(4))+4*4;
    last[0]=last[3]=1.f;
    auto small=boxDownsample(odd,2);
    auto *corner=reinterpret_cast<const float *>(small.constScanLine(1))+4;
    check(std::abs(corner[3]-1.f/9.f)<.00001f,"blur downsample includes the final row and column");
    QImage edge(2,2,QImage::Format_RGBA32FPx4_Premultiplied);edge.fill(Qt::transparent);
    auto *first=reinterpret_cast<float *>(edge.scanLine(0));first[0]=first[3]=1.f;
    auto large=bilinearUpsample(edge,{8,8});
    check(reinterpret_cast<const float *>(large.constScanLine(0))[3]==1.f,
          "blur upsample clamps both neighbours at the top-left edge");
  }
  {
    // Odd dimensions exercise partial column tiles and independent worker
    // boundaries. Compare the optimized sliding window to direct convolution.
    QImage seed(67,65,QImage::Format_RGBA32FPx4_Premultiplied);
    seed.fill(Qt::transparent);
    for(int y=0;y<seed.height();++y){auto *p=reinterpret_cast<float *>(seed.scanLine(y));
      for(int x=0;x<seed.width();++x){float a=float((x*7+y*3)%17)/17;
        p[x*4]=a*2;p[x*4+1]=a*.3f;p[x*4+2]=0;p[x*4+3]=a;}}
    {
      // A wide blur is resolved below full resolution. It has to land on the
      // same picture: fullResolutionBlur is the reference the GPU is held to,
      // so it is the reference this is held to as well.
      QImage wide(320,240,QImage::Format_RGBA32FPx4_Premultiplied);
      wide.fill(Qt::transparent);
      { QPainter p(&wide); p.fillRect(60,40,180,120,Qt::white); }
      const auto exact=fullResolutionBlur(wide,96), reduced=soften(wide,96);
      if(reduced.size()!=exact.size()) failures<<"a reduced blur comes back at full size";
      else {
        double sum=0; float worst=0; qint64 n=0;
        for(int y=0;y<exact.height();++y) {
          const auto *a=reinterpret_cast<const float *>(exact.constScanLine(y));
          const auto *b=reinterpret_cast<const float *>(reduced.constScanLine(y));
          for(int i=0;i<exact.width()*4;++i) {
            const float d=std::abs(a[i]-b[i]);sum+=d;worst=std::max(worst,d);++n;
          }
        }
        // Measured at 0.0054 mean and 0.021 worst; a reduction coarser than
        // one kernel sample in twelve pushes past this.
        if(sum/std::max<qint64>(1,n)>.007 || worst>.03)
          failures<<QString("a wide blur resolved below full resolution stays on the same "
                            "picture (mean %1 worst %2)").arg(sum/std::max<qint64>(1,n)).arg(worst);
      }
    }
    auto actual=soften(seed,4);
    auto reference=seed;
    for(int pass=0;pass<6;++pass){
      QImage next(reference.size(),reference.format());next.fill(Qt::transparent);
      for(int y=0;y<next.height();++y){auto *dest=reinterpret_cast<float *>(next.scanLine(y));
        for(int x=0;x<next.width();++x)for(int c=0;c<4;++c){double sum=0;
          for(int k=-2;k<=2;++k){int sx=x+(pass%2?0:k),sy=y+(pass%2?k:0);
            if(sx>=0&&sx<next.width()&&sy>=0&&sy<next.height())
              sum+=reinterpret_cast<const float *>(reference.constScanLine(sy))[sx*4+c];}
          dest[x*4+c]=float(sum/5);}}
      reference=next;
    }
    double error=0;
    for(int y=0;y<actual.height();++y){auto *a=reinterpret_cast<const float *>(actual.constScanLine(y));
      auto *b=reinterpret_cast<const float *>(reference.constScanLine(y));
      for(int x=0;x<actual.width()*4;++x)error=std::max(error,double(std::abs(a[x]-b[x])));}
    check(error<.00001,"tiled multicore blur matches direct convolution including borders");
    auto halo=seed;
    QColor color(50,100,150,190);
    haloBehind(halo,4,color,{},1);
    float haloError=0;
    for(int y=0;y<seed.height();++y){
      auto *src=reinterpret_cast<const float *>(seed.constScanLine(y));
      auto *soft=reinterpret_cast<const float *>(reference.constScanLine(y));
      auto *out=reinterpret_cast<const float *>(halo.constScanLine(y));
      for(int x=0;x<seed.width();++x){
        float a=soft[x*4+3]*color.alphaF()*(1-src[x*4+3]);
        float expected[4]={src[x*4]+float(color.redF())*a,src[x*4+1]+float(color.greenF())*a,
                           src[x*4+2]+float(color.blueF())*a,src[x*4+3]+a};
        for(int c=0;c<4;++c)haloError=std::max(haloError,std::abs(out[x*4+c]-expected[c]));
      }
    }
    check(haloError<.00001f,"alpha-only halo matches full RGBA reference and retains HDR");
  }
  Scene s; s.width=16; s.height=16; s.motionBlur=false; s.processingDepth=32;
  Layer l; l.type="image"; l.duration=180; l.p[0].base=8; l.p[1].base=8;
  l.image=QImage(16,16,QImage::Format_RGBA32FPx4); l.image.fill(Qt::transparent);
  for(int y=0;y<16;++y) { auto *p=reinterpret_cast<float *>(l.image.scanLine(y));
    for(int x=0;x<16;++x) {p[x*4]=2.5f;p[x*4+1]=.123456f;p[x*4+2]=.25f;p[x*4+3]=1.f;} }
  s.layers.push_back(l); auto out=renderCPU(s,0,false);
  auto *p=reinterpret_cast<const float *>(out.constScanLine(8))+8*4;
  check(out.format()==QImage::Format_RGBA32FPx4 && std::abs(p[0]-2.5f)<.001f,"HDR render retains values above 1");
  gradeCPU(out,-1); p=reinterpret_cast<const float *>(out.constScanLine(8))+8*4;
  check(std::abs(p[0]-1.25f)<.001f,"float exposure retains HDR");
  s.processingDepth=16; out=renderCPU(s,0,false);
  auto q=reinterpret_cast<const QRgba64 *>(out.constScanLine(8))[8];
  check(out.depth()==64 && std::abs(int(q.green())-int(.123456*65535))<4,"16 bpc retains sub-8-bit detail");
  QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); out.save(&buffer,"PNG");
  auto restored=QImage::fromData(png).convertToFormat(QImage::Format_RGBA64);
  check(!restored.isNull() && reinterpret_cast<const QRgba64 *>(restored.constScanLine(8))[8].green()==q.green(),"PNG 16-bit roundtrip");
  QString error; Scene loaded; s.processingDepth=32;
  check(loaded.load(s.json(),error) && loaded.processingDepth==32,"precision project roundtrip");
  if (!loaded.layers.empty()) {
    auto img=loaded.layers[0].image.convertToFormat(QImage::Format_RGBA32FPx4);
    check(std::abs(reinterpret_cast<const float *>(img.constScanLine(8))[8*4]-2.5f)<.001f,"HDR image roundtrip");
  }
  for(int depth : {16,32}) {
    QByteArray psd; QDataStream data(&psd,QIODevice::WriteOnly); data.setByteOrder(QDataStream::BigEndian);
    data.writeRawData("8BPS",4); data<<quint16(1); data.writeRawData("\0\0\0\0\0\0",6);
    data<<quint16(3)<<quint32(1)<<quint32(1)<<quint16(depth)<<quint16(3);
    data<<quint32(0)<<quint32(0)<<quint32(0)<<quint16(0);
    for(int c=0;c<3;++c) {
      if(depth==16) data<<quint16(12345);
      else {float value=2.5f; quint32 bits; std::memcpy(&bits,&value,4); data<<bits;}
    }
    auto image=decodePSD(psd,error);
    check(!image.isNull() && image.depth()==(depth==16?64:128),"PSD high precision decode");
    if(!image.isNull() && depth==32) check(reinterpret_cast<const float *>(image.constBits())[0]==2.5f,"PSD HDR value");
    if(!image.isNull() && depth==16) check(reinterpret_cast<const QRgba64 *>(image.constBits())[0].red()==12345,"PSD 16-bit value");
    check(decodePSD(psd.left(psd.size()-1),error).isNull(),"truncated high precision PSD rejected");
  }
  Layer blur; blur.blur=1;
  auto filtered=precisionEffects(l.image,blur,1).convertToFormat(QImage::Format_RGBA32FPx4);
  check(reinterpret_cast<const float *>(filtered.constScanLine(8))[8*4]>2.f,"blur retains HDR");
  {
    // A frame with a group, a grouped layer or a track matte in it has its
    // motion blur averaged from whole samples of the frame, at any depth. Drawn
    // side by side, one to a worker, and added in their order, the samples make
    // the same frame to the bit as drawn one after another, and as the sum was
    // always made: every sample drawn alone and premultiplied whole, then
    // widened to float and divided in one value at a time when deep, added up a
    // byte at a time and divided once, rounded to the nearest, at 8 bpc. Two
    // fast boxes, one of them Multiply, over still layers and under a blur over
    // the whole frame, the first box seen through a still bar (a track matte)
    // or the second in a group with the still bar beneath it; a width that is
    // no multiple of four; ten samples. The background is see-through, so that
    // every sample's way from the picture it is painted in to the straight one
    // it is handed over as, and back, shows. Drawn off the interface thread, as
    // the preview draws, which is where samples go to the workers.
    const int cap = previewMotionBlurCap();
    setPreviewMotionBlurCap(0);
    struct Case { int depth; const char *with; };
    for (const Case &c : {Case{16, "a track matte"}, Case{16, "a group"}, Case{32, "a track matte"}, Case{32, "a group"},
                          Case{8, "a track matte"}, Case{8, "a group"}}) {
      const int depth = c.depth;
      const QString with = c.with;
      Scene s; s.width = 333; s.height = 251; s.fps = 30; s.frames = 60; s.motionBlur = true; s.processingDepth = depth;
      Layer back; back.type = "fill"; back.color = QColor(30, 60, 140); back.duration = 60;
      back.p[0].base = 166; back.p[1].base = 125; back.p[5].base = .6;
      Layer still; still.type = "rect"; still.color = QColor(250, 250, 250); still.duration = 60;
      still.p[0].base = 166; still.p[1].base = 180; still.p[ShapeWidth].base = 200; still.p[ShapeHeight].base = 40;
      Layer moving; moving.type = "rect"; moving.color = QColor(240, 90, 40, 220); moving.duration = 60; moving.motionBlur = true;
      moving.p[0].set(0, -600); moving.p[0].set(20, 700); moving.p[1].base = 80;
      moving.p[ShapeWidth].base = 60; moving.p[ShapeHeight].base = 60;
      // A layer of its own, not the first box again: a copy keeps the id, and
      // the Multiply box's own pass would draw both.
      Layer multiplied = moving; multiplied.id = "multiplied"; multiplied.blendMode = "multiply";
      multiplied.color = QColor(90, 200, 120); multiplied.p[1].base = 150;
      Layer grade; grade.type = "effect"; grade.duration = 60; grade.blur = 3;
      grade.p[0].base = 166; grade.p[1].base = 125;
      s.layers = {grade, moving, multiplied, still, back};
      if (with == "a track matte") {
        Layer bar; bar.type = "rect"; bar.color = Qt::white; bar.duration = 60;
        bar.p[0].base = 166; bar.p[1].base = 90; bar.p[ShapeWidth].base = 260; bar.p[ShapeHeight].base = 50;
        s.layers[1].trackMatteId = bar.id;
        s.layers.insert(s.layers.begin() + 1, bar);
      } else {
        Layer group; group.type = "group"; group.text.clear(); group.color = QColor(Qt::transparent);
        group.duration = 60; group.sharesMainView = true;
        s.layers[2].groupId = s.layers[3].groupId = group.id;
        s.layers.insert(s.layers.begin() + 2, group);
      }
      const double frame = 10;
      s.frame = frame;
      const QSize size(s.width, s.height);
      const int samples = std::clamp(motionBlurSampleCount(s, frame, size), 8, 48);
      const auto offThread = [&](bool one) {
        return std::async(std::launch::async, [&] {
          std::optional<RenderSchedulingScope> alone;
          if (one) alone.emplace(0, 1);
          return renderCPU(s, frame, true, size);
        }).get();
      };
      clearRenderCaches();
      const QImage together = offThread(false);
      clearRenderCaches();
      const QImage inTurn = offThread(true);
      QImage made;
      {
        RenderSchedulingScope alone(0, 1);
        const auto sample = [&](int k, QImage::Format format) {
          return renderCPU(s, frame + ((k + .5) / samples - .5) * .5, false, size).convertToFormat(format);
        };
        if (depth == 8) {
          const int values = size.width() * 4;
          std::vector<quint16> sums(size_t(values) * size.height(), 0);
          for (int k = 0; k < samples; ++k) {
            const QImage one = sample(k, QImage::Format_RGBA8888_Premultiplied);
            for (int y = 0; y < size.height() && !one.isNull(); ++y)
              for (int x = 0; x < values; ++x)
                sums[size_t(y) * values + x] = quint16(sums[size_t(y) * values + x] + one.constScanLine(y)[x]);
          }
          QImage mean(size, QImage::Format_RGBA8888_Premultiplied);
          for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < values; ++x)
              mean.scanLine(y)[x] = uchar((sums[size_t(y) * values + x] + samples / 2) / samples);
          made = mean.convertToFormat(QImage::Format_RGBA8888);
        } else {
          QImage sum(size, QImage::Format_RGBA32FPx4_Premultiplied);
          sum.fill(Qt::transparent);
          for (int k = 0; k < samples; ++k) {
            const QImage one = sample(k, QImage::Format_RGBA32FPx4_Premultiplied);
            for (int y = 0; y < sum.height() && !one.isNull(); ++y) {
              auto *out = reinterpret_cast<float *>(sum.scanLine(y));
              const auto *in = reinterpret_cast<const float *>(one.constScanLine(y));
              for (int x = 0; x < sum.width() * 4; ++x) out[x] += in[x] / float(samples);
            }
          }
          made = sum.convertToFormat(processingFormat(depth));
        }
      }
      // The frame is one averaged from whole samples, and enough of it is partly
      // see-through for the samples' way through straight to show.
      const QImage bytes = made.convertToFormat(QImage::Format_RGBA8888);
      qint64 partly = 0;
      for (int y = 0; y < bytes.height(); ++y)
        for (int x = 0; x < bytes.width(); ++x) {
          const int alpha = bytes.constScanLine(y)[x * 4 + 3];
          partly += alpha > 0 && alpha < 255;
        }
      check(samples > 8 && !together.isNull() && together.format() == processingFormat(depth) && together == inTurn &&
                together == made && !motionBlurPerLayer(s, frame) && partly > 10000,
            QString("%1 bpc motion blur with %2: samples drawn side by side, one after another and summed as always are "
                    "the same frame").arg(depth).arg(with));
    }
    setPreviewMotionBlurCap(cap);
  }
  {
    // Any other deep frame blurs each moving layer on its own, as an 8-bit one
    // does: the frame drawn once, each moving layer sampled across the shutter
    // alone and its samples averaged at the frame's depth -- a blended one
    // blended into what lies beneath sample by sample -- and an Effect Layer
    // above worked once on the result. Two boxes that cross each other in the
    // shutter, the upper one striped and Difference, over still layers and
    // under a blur:
    // at 16 and 32 bpc the frame is the 8 bpc one to a level or two, where
    // averaged from whole frames, as deep frames were, it is far off where the
    // boxes cross; and drawn in bands it is the frame drawn on one core.
    const int cap = previewMotionBlurCap();
    setPreviewMotionBlurCap(0);
    Scene s; s.width = 333; s.height = 251; s.fps = 30; s.frames = 60; s.motionBlur = true;
    Layer back; back.id = "back"; back.type = "fill"; back.color = QColor(30, 60, 140); back.duration = 60;
    back.p[0].base = 166; back.p[1].base = 125;
    Layer still; still.id = "still"; still.type = "rect"; still.color = QColor(250, 250, 250); still.duration = 60;
    still.p[0].base = 166; still.p[1].base = 180; still.p[ShapeWidth].base = 200; still.p[ShapeHeight].base = 40;
    Layer right; right.id = "right"; right.type = "rect"; right.color = QColor(240, 90, 40, 220); right.duration = 60;
    right.motionBlur = true; right.p[0].set(0, -600); right.p[0].set(20, 900); right.p[1].base = 110;
    right.p[ShapeWidth].base = 60; right.p[ShapeHeight].base = 60;
    // Striped, so that a pixel meets other colours in other samples: a blend of
    // one colour over an opaque picture goes with how much of the pixel it
    // covers, and averaged first and blended once would come out the same.
    Layer left; left.id = "left"; left.type = "image"; left.duration = 60; left.motionBlur = true; left.blendMode = "difference";
    left.image = QImage(60, 60, QImage::Format_ARGB32);
    for (int y = 0; y < 60; ++y)
      for (int x = 0; x < 60; ++x) left.image.setPixel(x, y, (x / 3) % 2 ? qRgb(240, 240, 60) : qRgb(20, 40, 200));
    left.p[0].set(0, 900); left.p[0].set(20, -600); left.p[1].base = 140;
    Layer grade; grade.id = "grade"; grade.type = "effect"; grade.duration = 60; grade.blur = 2;
    grade.p[0].base = 166; grade.p[1].base = 125;
    s.layers = {grade, left, right, still, back};
    const double frame = 10;
    s.frame = frame;
    const QSize size(s.width, s.height);
    const int samples = motionBlurSampleCount(s, frame, size);
    const auto atDepth = [&](int depth) { Scene d = s; d.processingDepth = depth; return d; };
    const auto bytes = [](const QImage &image) { return image.convertToFormat(QImage::Format_ARGB32_Premultiplied); };
    // The largest difference in any channel, and how many differ by more than `by`.
    const auto apart = [](const QImage &a, const QImage &b, int by, int &over) {
      over = 0;
      if (a.isNull() || a.size() != b.size()) return 256;
      int worst = 0;
      for (int y = 0; y < a.height(); ++y) {
        const uchar *p = a.constScanLine(y), *q = b.constScanLine(y);
        for (int x = 0; x < a.width() * 4; ++x) {
          const int d = std::abs(int(p[x]) - int(q[x]));
          worst = std::max(worst, d);
          over += d > by;
        }
      }
      return worst;
    };
    clearRenderCaches();
    const QImage eight = bytes(renderCPU(atDepth(8), frame, true, size));
    for (int depth : {16, 32}) {
      const Scene d = atDepth(depth);
      clearRenderCaches();
      const QImage banded = renderCPU(d, frame, true, size);
      QImage oneCore;
      {
        RenderSchedulingScope alone(0, 1);
        clearRenderCaches();
        oneCore = renderCPU(d, frame, true, size);
      }
      QImage sum(size, QImage::Format_RGBA32FPx4_Premultiplied);
      sum.fill(Qt::transparent);
      {
        RenderSchedulingScope alone(0, 1);
        for (int k = 0; k < samples; ++k) {
          const QImage one = renderCPU(d, frame + ((k + .5) / samples - .5) * .5, false, size)
                                 .convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
          for (int y = 0; y < sum.height() && !one.isNull(); ++y) {
            auto *out = reinterpret_cast<float *>(sum.scanLine(y));
            const auto *in = reinterpret_cast<const float *>(one.constScanLine(y));
            for (int x = 0; x < sum.width() * 4; ++x) out[x] += in[x] / float(samples);
          }
        }
      }
      // In bands and on one core: the same frame at 16 bpc, and at 32 to a
      // float's last place -- as it is there without any motion, the Effect
      // Layer worked whole on the one and inside the pass on the other.
      double floatOff = 1;
      {
        const QImage x = banded.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
        const QImage y = oneCore.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
        if (!x.isNull() && x.size() == y.size()) {
          floatOff = 0;
          for (int r = 0; r < x.height(); ++r) {
            const float *p = reinterpret_cast<const float *>(x.constScanLine(r));
            const float *q = reinterpret_cast<const float *>(y.constScanLine(r));
            for (int c = 0; c < x.width() * 4; ++c) floatOff = std::max(floatOff, std::abs(double(p[c]) - q[c]));
          }
        }
      }
      const bool asOneCore = depth == 16 ? banded == oneCore : floatOff < 1e-6;
      int closeBy = 0, farOff = 0;
      const int worst = apart(bytes(banded), eight, 2, closeBy);
      const int wholeWorst = apart(bytes(sum), eight, 8, farOff);
      check(banded.format() == processingFormat(depth) && asOneCore && worst <= 3 && closeBy < 50 && wholeWorst > 24 &&
                farOff > 200,
            QString("%1 bpc motion blur: each moving layer blurs itself as at 8 bpc, banded as on one core (worst %2/255, "
                    "%3 values more than 2 off; from whole frames %4/255, %5 more than 8 off; bands %6 off)")
                .arg(depth).arg(worst).arg(closeBy).arg(wholeWorst).arg(farOff).arg(floatOff, 0, 'g', 3));
    }
    // And a trail keeps what 8 bits do not: a green between two 8-bit steps at
    // 16 bpc, light four times white at 32 -- its samples averaged at the
    // frame's own depth, never in bytes.
    for (int depth : {16, 32}) {
      Scene t; t.width = 200; t.height = 60; t.fps = 30; t.frames = 60; t.motionBlur = true; t.processingDepth = depth;
      Layer dot; dot.id = "dot"; dot.type = "image"; dot.duration = 60; dot.motionBlur = true;
      dot.image = QImage(20, 20, QImage::Format_RGBA32FPx4);
      const float level = depth == 16 ? 12465.f / 65535.f : 4.f;
      for (int y = 0; y < 20; ++y) {
        auto *px = reinterpret_cast<float *>(dot.image.scanLine(y));
        for (int x = 0; x < 20; ++x) { px[x * 4] = px[x * 4 + 1] = px[x * 4 + 2] = level; px[x * 4 + 3] = 1.f; }
      }
      dot.p[0].set(0, -400); dot.p[0].set(20, 600); dot.p[1].base = 30;
      t.layers = {dot};
      clearRenderCaches();
      const QImage trail = renderCPU(t, 10, true, QSize(t.width, t.height));
      int partial = 0, between = 0;
      double off = trail.isNull() ? 1 : 0;
      for (int x = 0; x < trail.width(); ++x) {
        double a = 0, c = 0;
        bool stepped = false;
        if (depth == 16) {
          const QRgba64 v = reinterpret_cast<const QRgba64 *>(trail.constScanLine(30))[x];
          a = v.alpha() / 65535.; c = v.green() / 65535.; stepped = v.alpha() % 257 != 0;
        } else {
          const float *v = reinterpret_cast<const float *>(trail.constScanLine(30)) + x * 4;
          a = v[3]; c = v[1]; stepped = std::abs(a * 255 - std::round(a * 255)) > .01;
        }
        if (a < .05 || a > .95) continue;
        between += stepped;
        if (a < .3) continue;
        ++partial;
        off = std::max(off, std::abs(c - level));
      }
      check(partial >= 8 && between >= 4 && off < (depth == 16 ? 8 / 65535. : 1e-3),
            QString("%1 bpc motion blur keeps %2 in a trail (%3 pixels partly covered, %4 between 8-bit steps, colour %5 off)")
                .arg(depth).arg(depth == 16 ? "16-bit steps" : "light above white").arg(partial).arg(between).arg(off));
    }
    setPreviewMotionBlurCap(cap);
  }
  {
    // A halo is kept in the depth its frame is painted in. A 16 bpc frame
    // handed a float halo widened it back to 16 bits at every draw -- four
    // neighbours of every pixel it covered, in every motion-blur sample of
    // every band -- and the Twitter promo at 1080p took 464 ms a frame for it
    // against 162. A 32 bpc frame keeps its halos in floats, light above white
    // and all. What the composition hands its painter is watched by a device
    // that only takes notes; a layer this large is too big to keep whole, so
    // it goes into the frame as its vectors over one halo.
    struct Notes final : QPaintDevice {
      struct Engine final : QPaintEngine {
        std::vector<QImage::Format> &formats;
        explicit Engine(std::vector<QImage::Format> &f) : QPaintEngine(QPaintEngine::AllFeatures), formats(f) {}
        bool begin(QPaintDevice *) override { return true; }
        bool end() override { return true; }
        void updateState(const QPaintEngineState &) override {}
        void drawPath(const QPainterPath &) override {}
        void drawPolygon(const QPointF *, int, PolygonDrawMode) override {}
        void drawTextItem(const QPointF &, const QTextItem &) override {}
        void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {}
        void drawImage(const QRectF &, const QImage &image, const QRectF &, Qt::ImageConversionFlags) override {
          formats.push_back(image.format());
        }
        Type type() const override { return QPaintEngine::User; }
      };
      QSize size;
      std::vector<QImage::Format> formats;
      mutable Engine engine{formats};
      explicit Notes(QSize s) : size(s) {}
      QPaintEngine *paintEngine() const override { return &engine; }
      int metric(PaintDeviceMetric m) const override {
        switch (m) {
        case PdmWidth: return size.width();
        case PdmHeight: return size.height();
        case PdmWidthMM: return size.width() * 254 / 960;
        case PdmHeightMM: return size.height() * 254 / 960;
        case PdmDpiX: case PdmDpiY: case PdmPhysicalDpiX: case PdmPhysicalDpiY: return 96;
        case PdmDepth: return 32;
        default: return QPaintDevice::metric(m);
        }
      }
    };
    for (const char *kind : {"shadow", "glow"})
      for (int depth : {8, 16, 32}) {
        Scene s; s.width = 1920; s.height = 1080; s.fps = 30; s.frames = 30; s.processingDepth = depth;
        Layer big; big.id = "big"; big.type = "shape"; big.shapeKind = "rect"; big.color = QColor(230, 90, 40);
        big.duration = 30; big.p[0].base = 960; big.p[1].base = 540;
        big.p[ShapeWidth].base = 2600; big.p[ShapeHeight].base = 1800;
        if (QByteArray(kind) == "shadow") { big.shadow = 120; big.appliedEffects["Drop Shadow"] = true; }
        else { big.glow = 120; big.appliedEffects["Glow"] = true; }
        s.layers = {big};
        clearRenderCaches();
        Notes notes(QSize(s.width, s.height));
        beginRasterFrame();
        {
          QPainter painter(&notes);
          paintComposition(painter, s, 0, 0, QSize(s.width, s.height));
        }
        const QImage::Format wanted = depth == 8 ? QImage::Format_ARGB32_Premultiplied
                                    : depth == 16 ? QImage::Format_RGBA64_Premultiplied
                                                  : QImage::Format_RGBA32FPx4_Premultiplied;
        QStringList seen;
        for (const QImage::Format f : notes.formats) seen << QString::number(int(f));
        check(notes.formats.size() == 1 && notes.formats[0] == wanted,
              QString("a %1 bpc frame is handed its %2 in its own depth (pictures drawn, by format: %3; wanted %4)")
                  .arg(depth).arg(kind).arg(seen.isEmpty() ? QString("none") : seen.join(", ")).arg(int(wanted)));
      }
    clearRenderCaches();
    // And converted first or as it is drawn, a 16 bpc frame gets the same
    // pixels: Qt widens a float picture with this same conversion as it reads
    // it. Laid enlarged between pixels, as a halo resolved small is, and
    // turned, as a turned layer's is.
    QImage seed(97, 61, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < seed.height(); ++y) {
      auto *px = reinterpret_cast<float *>(seed.scanLine(y));
      for (int x = 0; x < seed.width(); ++x) {
        const float a = float((x * 13 + y * 7) % 31) / 30.f;
        px[x * 4] = a * float(x) / 96.f; px[x * 4 + 1] = a * .37f; px[x * 4 + 2] = a * float(y) / 60.f; px[x * 4 + 3] = a;
      }
    }
    int same = 0, laidOut = 0;
    for (const QColor &tint : {QColor(0, 0, 0, 200), QColor()}) {
      const QImage halo = renderHaloOnly(seed, 9, tint);
      const QImage words = halo.convertToFormat(QImage::Format_RGBA64_Premultiplied);
      for (double turn : {0., 17.}) {
        auto laid = [&](const QImage &picture) {
          QImage frame(400, 260, QImage::Format_RGBA64_Premultiplied);
          frame.fill(QColor(30, 140, 200));
          QPainter p(&frame);
          p.setRenderHint(QPainter::SmoothPixmapTransform);
          p.translate(13.37, 8.6);
          p.rotate(turn);
          p.scale(1.21, 1.21);
          p.drawImage(QRectF(.25, .5, picture.width() * 3.1, picture.height() * 3.1), picture);
          return frame;
        };
        ++laidOut;
        same += halo.format() == QImage::Format_RGBA32FPx4_Premultiplied && laid(halo) == laid(words);
      }
    }
    check(same == laidOut, QString("a halo drawn from 16 bits gives a 16 bpc frame the pixels its floats gave (%1 of %2 alike)")
                               .arg(same).arg(laidOut));
  }
  return failures.isEmpty()?0:1;
}
