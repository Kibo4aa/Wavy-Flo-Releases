#include "plugin_runtime.h"
#include "gpu_pixels.h"
#include <QElapsedTimer>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>

bool PluginRouteCost::chooseGpu(bool available) {
  if (!available) return false;
  if (retryAfter > 0) { --retryAfter; return false; }
  // Measure the real CPU path twice: the first teaches the band scheduler,
  // the next measures its result, so GPU is compared against the worker path.
  if (cpuSamples < 2) return false;
  if (gpu <= 0) return true;
  const double ratio = std::max(cpu, gpu) / std::max(1e-6, std::min(cpu, gpu));
  if (++sinceProbe >= int(120 * std::clamp(ratio, 1., 16.))) {
    sinceProbe = 0;
    return cpu < gpu; // occasionally measure the losing route again
  }
  return gpu < cpu * .9; // do not switch for measurement noise
}
void PluginRouteCost::note(bool card, double value) {
  if (!std::isfinite(value) || value <= 0) return;
  double &slot = card ? gpu : cpu;
  slot = (!card && cpuSamples==1) || slot<=0 ? value : slot*.7 + value*.3;
  if (!card) ++cpuSamples;
}

int PluginRuntime::processPixels(const void *identity, const WavyParallelV1 *parallel,
                                 QImage &image, const PluginPixelCall &call) {
  const QByteArray key="cpu:"+QByteArray::number(quintptr(identity),16)+":"+
      QByteArray::number(image.depth());
  double wholeCost;
  {QMutexLocker lock(&guard);wholeCost=costs[key].wholeNsPerPixel;}
  QImage working=image;
  int result;
  if(parallel && parallel->reentrant)
    result=runPluginAcrossCores(working,true,parallel->rowReach,call,&wholeCost);
  else {QMutexLocker lock(&serialCalls);result=runPluginAcrossCores(working,false,0,call,&wholeCost);}
  if(!result)image=working;
  {QMutexLocker lock(&guard);costs[key].wholeNsPerPixel=wholeCost;}
  return result;
}

namespace {
int depthOf(const QImage &image) { return image.depth() <= 32 ? 8 : image.depth() == 64 ? 16 : 32; }
QVector<float> defaults(const WavyEffectV4 &api) {
  QVector<float> result;
  for (int i=0;i<api.parameterCount;++i) {
    const auto &p=api.parameters[i];
    if(p.kind==WAVY_COLOR) result << .81f << .23f << .57f << .65f;
    else result << std::clamp(p.initial,p.minimum,p.maximum);
  }
  return result;
}
} // namespace
QString validatePluginShader(const WavyEffectV4 &api, const WavyShaderV1 &shader, int depth) {
  if (!api.process || api.parameterCount<0 || api.parameterCount>64 ||
      (api.parameterCount && !api.parameters) || !shader.source)
    return "invalid shader descriptor";
  const auto problem=gpuShaderProblem(QString::fromUtf8(shader.source));
  if(!problem.isEmpty()) return problem;
  QVector<QVector<float>> cases{defaults(api)};
  int slot=0;
  for(int i=0;i<api.parameterCount;++i) {
    const auto &p=api.parameters[i];
    auto low=cases[0], high=cases[0];
    if(p.kind==WAVY_COLOR) {
      for(int c=0;c<4;++c) {low[slot+c]=0; high[slot+c]=1;}
      slot+=4;
    } else {
      low[slot]=p.minimum; high[slot]=p.maximum;
      if(p.kind==WAVY_CHOICE) {
        // Cover every ordinary menu entry, not only the middle one.
        for(int v=int(p.minimum)+1;v<p.maximum && v<int(p.minimum)+32;++v) {
          auto choice=cases[0];choice[slot]=float(v);cases<<choice;
        }
      }
      ++slot;
    }
    cases << low << high;
  }
  if(slot>256) return "shader exceeds 256 parameter values";
  for(const QSize size : {QSize(19,13),QSize(7,23)}) {
    QImage patch(size,QImage::Format_RGBA32FPx4);
    for(int y=0;y<size.height();++y) {
      auto *p=reinterpret_cast<float *>(patch.scanLine(y));
      for(int x=0;x<size.width();++x) {
        p[x*4]=float((x*13+y*7)%31)/30;
        p[x*4+1]=float((x*3+y*19)%29)/28;
        p[x*4+2]=float((x*17+y*11)%37)/36;
        p[x*4+3]=float((x+y*5)%17)/16; // includes zero and full alpha
        if(depth==32) {p[x*4]*=3.7f; p[x*4+1]-=.25f;}
      }
    }
    patch=patch.convertToFormat(processingFormat(depth,false));
    for(int n=0;n<cases.size();++n) {
      QImage cpu=patch;
      const auto &values=cases[n];
      if(api.process(cpu.bits(),size.width(),size.height(),int(cpu.bytesPerLine()),depth,
                     values.constData(),values.size())) return "CPU validation failed";
      GpuPass pass;pass.source=QString::fromUtf8(shader.source);pass.values=values;
      pass.processingDepth=depth;pass.endsEffect=true;
      auto card=gpuRunPasses(patch,{pass});
      if(card.isNull() || card.size()!=cpu.size()) return "GPU validation unavailable";
      for(int y=0;y<size.height();++y) {
        const auto *a=cpu.constScanLine(y), *b=card.constScanLine(y);
        for(int c=0;c<size.width()*4;++c) {
          auto channel=[&](const uchar *p) {
            return depth==8 ? p[c]/255.f : depth==16 ? reinterpret_cast<const quint16 *>(p)[c]/65535.f
                                                    : reinterpret_cast<const float *>(p)[c];
          };
          const float av=channel(a),bv=channel(b);
          const float tolerance=depth==8 ? 2.f/255 : depth==16 ? 4.f/65535
              // HDR brightness-preserving kernels can amplify float/FMA
              // rounding when negative channels nearly cancel in luminance.
              : 1e-4f*std::max(1.f,std::abs(av));
          if(!std::isfinite(av) || !std::isfinite(bv) || std::abs(av-bv)>tolerance)
            return QString("%1 bpc mismatch, case %2, pixel %3,%4 channel %5: CPU %6 GPU %7")
                .arg(depth).arg(n).arg(c/4).arg(y).arg(c%4).arg(av,0,'g',9).arg(bv,0,'g',9);
        }
      }
    }
  }
  return {};
}

int PluginRuntime::process(const void *identity, const WavyEffectV4 &api,
                          const WavyShaderV1 *shader, const WavyParallelV1 *parallel,
                          QImage &image, const QVector<float> &values, bool allowGpu) {
  if(image.isNull()) return 1;
  int count=0;
  for(int i=0;i<api.parameterCount;++i) count+=wavyParameterFloats(&api.parameters[i]);
  const int depth=depthOf(image);
  if(count!=values.size() || !(api.supportedDepths & (depth==8?1u:depth==16?2u:4u)))return 1;
  const double pixels=double(image.width())*image.height(), mp=pixels/1e6;
  const int bucket=std::max(0,int(std::log2(std::max(1.,pixels/65536.))));
  const QByteArray key=QByteArray::number(quintptr(identity),16)+":"+
      QByteArray::number(depth)+":"+QByteArray::number(bucket);
  bool card=false;
  double wholeCost=0;
  {
    QMutexLocker lock(&guard);
    auto &cost=costs[key];
    wholeCost=cost.wholeNsPerPixel;
    card=cost.chooseGpu(allowGpu && shader && canUseGpuPixels() && count<=256);
  }
  // GPU compilation/validation only occurs on the GUI GL thread. Workers
  // update the timing table under a mutex and never touch programs or widgets.
  if(card) {
    const QByteArray trustKey=QByteArray::number(quintptr(identity),16)+":"+QByteArray::number(depth);
    QString issue;
    bool known=false;
    {QMutexLocker lock(&guard);known=validation.contains(trustKey);issue=validation.value(trustKey);}
    if(!known) {
      if(!parallel || !parallel->reentrant) {
        QMutexLocker lock(&serialCalls);issue=validatePluginShader(api,*shader,depth);
      } else issue=validatePluginShader(api,*shader,depth);
      QMutexLocker lock(&guard);validation.insert(trustKey,issue);
    }
    card=issue.isEmpty();
  }
  const auto wanted=image.format();
  QElapsedTimer timer;timer.start();
  // Transactional: a failed plugin must not leave a half-modified raster.
  QImage working=image.convertToFormat(processingFormat(depth,false));
  if(card) {
    GpuPass pass;pass.source=QString::fromUtf8(shader->source);pass.values=values;
    pass.processingDepth=depth;pass.endsEffect=true;
    auto output=gpuRunPasses(working,{pass});
    if(!output.isNull()) {
      image=output.convertToFormat(wanted);
      QMutexLocker lock(&guard);costs[key].note(true,timer.nsecsElapsed()/1e6/mp);
      return 0;
    }
    {QMutexLocker lock(&guard);costs[key].retryAfter=120;}
    timer.restart();
  }
  const auto call=[&](void *p,int w,int h,int stride) {
    return api.process(p,w,h,stride,depth,values.constData(),values.size());
  };
  int result;
  if(parallel && parallel->reentrant)
    result=runPluginAcrossCores(working,true,parallel->rowReach,call,&wholeCost);
  else {QMutexLocker lock(&serialCalls);result=runPluginAcrossCores(working,false,0,call,&wholeCost);}
  if(!result) image=working.convertToFormat(wanted);
  {
    QMutexLocker lock(&guard);
    auto &cost=costs[key];cost.wholeNsPerPixel=wholeCost;
    if(!result)cost.note(false,timer.nsecsElapsed()/1e6/mp);
  }
  return result;
}
