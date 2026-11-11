#include "plugin_runtime.h"
#include "gpu_pixels.h"
#include "render_workers.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLibrary>
#include <QThread>
#include <atomic>
#include <algorithm>
#include <cmath>

namespace {
QImage patch(QSize size,int depth=8) {
  QImage result(size,QImage::Format_RGBA32FPx4);
  for(int y=0;y<size.height();++y) {
    auto *p=reinterpret_cast<float *>(result.scanLine(y));
    for(int x=0;x<size.width();++x) {
      p[x*4]=float(x%23)/22;p[x*4+1]=float(y%19)/18;
      p[x*4+2]=float((x+y)%17)/16;p[x*4+3]=float((x+3*y)%13)/12;
      if(depth==32)p[x*4]*=3;
    }
  }
  return result.convertToFormat(processingFormat(depth,false));
}
bool close(const QImage &left,const QImage &right,int depth) {
  if(left.isNull() || right.size()!=left.size())return false;
  const auto a=left.convertToFormat(processingFormat(depth,false)),b=right.convertToFormat(processingFormat(depth,false));
  for(int y=0;y<a.height();++y) {
    const auto *p=a.constScanLine(y), *q=b.constScanLine(y);
    for(int x=0;x<a.width()*4;++x) {
      auto channel=[&](const uchar *v) {
        return depth==8 ? v[x]/255.f : depth==16 ? reinterpret_cast<const quint16 *>(v)[x]/65535.f
                                                : reinterpret_cast<const float *>(v)[x];
      };
      if(!std::isfinite(channel(q)) || std::abs(channel(p)-channel(q))>
          (depth==8?2.f/255:depth==16?4.f/65535:5e-5f))return false;
    }
  }
  return true;
}
std::atomic<int> workerCalls{0};
int observed(void *data,int w,int h,int stride,int,const float *,int32_t) {
  if(QThread::currentThread()!=qApp->thread())++workerCalls;
  // Deterministic 6ms scheduling workload: this teaches the scheduler that
  // splitting is worthwhile without a machine-dependent timing assertion.
  QThread::msleep(6);
  for(int y=0;y<h;++y)for(int x=0;x<w;++x)
    static_cast<uchar *>(data)[y*stride+x*4]^=255;
  return 0;
}
int failed(void *data,int,int,int,int,const float *,int32_t) {
  static_cast<uchar *>(data)[0]=19;return 7;
}
static const WavyParameter mode={sizeof(WavyParameter),WAVY_CHOICE,"mode","Mode",0,2,1,1,"Zero\nOne\nTwo"};
int modes(void *data,int w,int h,int stride,int,const float *values,int32_t) {
  for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
    auto *p=static_cast<uchar *>(data)+y*stride+x*4;
    if(values[0]<.5f)p[0]=0;
  }
  return 0;
}
int maths(void *data,int w,int h,int stride,int,const float *values,int32_t) {
  const int loops=int(values[0]);
  for(int y=0;y<h;++y) {
    auto *p=static_cast<uchar *>(data)+y*stride;
    for(int x=0;x<w;++x)for(int c=0;c<3;++c) {
      float result=0,colour=p[x*4+c]/255.f;
      for(int k=1;k<=loops;++k)result+=std::sin(colour*float(k));
      p[x*4+c]=uchar(std::clamp((result/loops+1)*.5f,0.f,1.f)*255+.5f);
    }
  }
  return 0;
}
const char *mathBody=R"(
vec4 wavyEffect(vec4 c,vec2 p,vec2 s){
  vec3 total=vec3(0.0);int count=int(values[0]);
  for(int k=1;k<=count;++k)total+=sin(c.rgb*float(k));
  return vec4(clamp((total/float(count)+1.0)*0.5,0.0,1.0),c.a);
})";
}

int checkPluginRuntime(QStringList &report,bool gpu) {
  auto check=[&](bool good,const QString &name){if(!good)report<<name;};
  PluginRouteCost cost;
  check(!cost.chooseGpu(true),"new workload starts on CPU");
  cost.note(false,100);check(!cost.chooseGpu(true),"banded CPU measured before GPU");
  cost.note(false,10);check(cost.cpu==10 && cost.chooseGpu(true),"GPU compares against learned CPU route");
  cost.note(true,20);check(!cost.chooseGpu(true),"a slower card is avoided");
  bool reprobed=false;for(int i=0;i<400;++i)reprobed|=cost.chooseGpu(true);
  check(reprobed,"slower backend gets remeasured");
  cost.gpu=1;check(cost.chooseGpu(true),"faster card is selected");
  check(!cost.chooseGpu(false),"CPU selection is respected");
  cost.retryAfter=3;check(!cost.chooseGpu(true),"GPU errors back off");

  WavyEffectV4 observedApi{sizeof(WavyEffectV4),4,1,0,nullptr,observed};
  WavyParallelV1 parallel{sizeof(WavyParallelV1),1,1,0};
  PluginRuntime runtime;
  auto input=patch({193,513});auto a=input;
  check(runtime.process(&observedApi,observedApi,nullptr,&parallel,a,{},false)==0,"CPU plugin runs");
  auto b=input;workerCalls=0;
  check(runtime.process(&observedApi,observedApi,nullptr,&parallel,b,{},false)==0 && a==b,
        "learned CPU bands have identical pixels");
  if(QThread::idealThreadCount()>1)check(workerCalls>0,"layer plugin actually dispatches worker calls");
  observedApi.process=failed;auto original=input;
  check(runtime.process(&observedApi,observedApi,nullptr,&parallel,original,{},false)==7 && original==input,
        "failed plugin leaves input untouched");
  if(!gpu)return report.isEmpty()?0:1;
  enableGpuPixels(true);
  if(!gpuShaderProblem("vec4 wavyEffect(vec4 c,vec2 p,vec2 s){return c;}").isEmpty())return 77;
  // By its full name: an older WavyTint.dll left in the folder (Anjim-era exports)
  // is what a bare "WavyTint" finds first.
  QLibrary tint(QCoreApplication::applicationDirPath()+"/effects/WavyTint.wfx");
  auto get=reinterpret_cast<WavyGetEffectV4>(tint.resolve("wavyGetEffectV4"));
  auto getShader=reinterpret_cast<WavyGetShaderV1>(tint.resolve("wavyGetShaderV1"));
  check(get && getShader,"Tint sample exports both CPU and shader ABI");
  if(get && getShader) {
    for(int depth:{8,16,32}) {
      const auto issue=validatePluginShader(*get(),*getShader(),depth);
      check(issue.isEmpty(),QString("Tint %1 bpc: ").arg(depth)+issue);
      for(int mode=0;mode<3;++mode) {
        QVector<float> values{.8f,.24f,.12f,.7f,.9f,float(mode),1.f};
        auto source=patch({91,67},depth),cpu=source;
        get()->process(cpu.bits(),cpu.width(),cpu.height(),int(cpu.bytesPerLine()),depth,values.constData(),values.size());
        GpuPass pass;pass.source=QString::fromUtf8(getShader()->source);pass.values=values;
        pass.processingDepth=depth;pass.endsEffect=true;
        check(close(cpu,gpuRunPasses(source,{pass}),depth),QString("Tint mode %1 depth %2 parity").arg(mode).arg(depth));
      }
    }
    WavyShaderV1 hdrWrong=*getShader();
    const QByteArray clamp=QByteArray(hdrWrong.source).replace("processingDepth == 32", "false");
    hdrWrong.source=clamp.constData();
    check(!validatePluginShader(*get(),hdrWrong,32).isEmpty(),"8-bit-looking shader must not clip HDR silently");
    const QVector<float> values{.7f,.1f,.3f,1.f,.8f,1.f,0.f};
    const auto source=patch({97,71});auto expected=source;
    get()->process(expected.bits(),97,71,int(expected.bytesPerLine()),8,values.data(),values.size());
    const auto operations=gpuPixelOperations();
    for(int i=0;i<3;++i) {
      auto actual=source;
      check(runtime.process(get(),*get(),getShader(),&parallel,actual,values,true)==0 && close(expected,actual,8),
            "trusted shader route agrees with plugin CPU result");
    }
    check(gpuPixelOperations()>operations,"eligible shader is actually dispatched after CPU measurement");
    const auto after=gpuPixelOperations();auto cpuOnly=source;
    runtime.process(get(),*get(),getShader(),&parallel,cpuOnly,values,false);
    check(gpuPixelOperations()==after,"disabled GPU never dispatches plugin shaders");
  }
  WavyEffectV4 options{sizeof(WavyEffectV4),4,1,1,&mode,modes};
  WavyShaderV1 wrong{sizeof(WavyShaderV1),1,"vec4 wavyEffect(vec4 c,vec2 p,vec2 s){return c;}"};
  check(!validatePluginShader(options,wrong,8).isEmpty(),"mismatch outside the middle choice is rejected");
  WavyShaderV1 invalid{sizeof(WavyShaderV1),1,"invalid GLSL"};
  auto fallback=patch({19,13});auto expected=fallback;
  const QVector<float> values{0};
  modes(expected.bits(),19,13,int(expected.bytesPerLine()),8,values.data(),1);
  for(int i=0;i<4;++i) {
    auto actual=fallback;
    runtime.process(&options,options,&invalid,&parallel,actual,values,true);
    check(actual==expected,"refused shader falls back to correct CPU pixels");
  }

  // Padded external rows: direct upload must not read padding as image pixels.
  QByteArray storage((13*4+28)*17,char(123));
  QImage padded(reinterpret_cast<uchar *>(storage.data()),13,17,13*4+28,QImage::Format_RGBA8888);
  for(int y=0;y<17;++y)for(int x=0;x<13;++x)padded.setPixelColor(x,y,QColor(x*11,y*7,41,255));
  GpuPass identity;identity.source=wrong.source;identity.endsEffect=true;
  check(close(padded,gpuRunPasses(padded,{identity}),8),"shader honors padded upload rows");
  // 64 declared colours occupy 256 values, rather than 64 floats.
  GpuPass last=identity;last.source="vec4 wavyEffect(vec4 c,vec2 p,vec2 s){return vec4(values[255],0,0,1);}";
  last.values.fill(0,256);last.values[255]=.8f;
  const auto wide=gpuRunPasses(padded,{last});
  check(!wide.isNull() && std::abs(wide.pixelColor(4,3).redF()-.8)<.01,"all 256 packed parameter values reach shader");

  // A custom program followed by a built-in that uses both input textures.
  auto base=patch({65,49});
  GpuPass tintPass=identity;tintPass.source="vec4 wavyEffect(vec4 c,vec2 p,vec2 s){return vec4(c.rgb*0.5,c.a);}";
  auto chain=QVector<GpuPass>{tintPass};chain+=gpuAlphaPass(true);
  chain+=gpuHaloPasses(base.size(),5,QColor(),{});chain+=gpuAlphaPass(false);
  auto staged=gpuRunPasses(base,{tintPass});
  auto builtins=gpuAlphaPass(true);builtins+=gpuHaloPasses(base.size(),5,QColor(),{});builtins+=gpuAlphaPass(false);
  staged=gpuRunPasses(staged,builtins);
  check(close(staged,gpuRunPasses(base,chain),8),"custom shader -> built-in aux pass chain parity");
  // No alpha-conversion pass between the two programs: this is the switch
  // that previously wrote an aux uniform into the still-bound custom program.
  const auto premul=base.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  GpuPass over;over.op=3;over.a={0,1,0,.4f};over.useAux=true;over.auxImage=premul;over.endsEffect=true;
  const auto direct=gpuRunPasses(premul,{tintPass,over});
  const auto separate=gpuRunPasses(gpuRunPasses(premul,{tintPass}),{over});
  check(close(direct,separate,32),"custom-to-aux program switch sets uniforms on the correct program");
  return report.isEmpty()?0:1;
}

int benchmarkPluginRuntime(QStringList &report) {
  enableGpuPixels(true);
  if(!gpuShaderProblem(mathBody).isEmpty())return 77;
  report<<gpuPixelStatus()<<QString("Logical CPU threads: %1; full 1920x1080 RGBA8; median of 5 warm runs; includes transfers").arg(QThread::idealThreadCount());
  auto input=patch({1920,1080});
  for(int loops:{1,64}) {
    QVector<float> values{float(loops)};
    double cost=0;QVector<double> cpuTimes,gpuTimes;
    GpuPass pass;pass.source=mathBody;pass.values=values;pass.endsEffect=true;
    QImage cpu,gpu;
    for(int n=0;n<7;++n) {
      cpu=input;QElapsedTimer timer;timer.start();
      runPluginAcrossCores(cpu,true,0,[&](void *p,int w,int h,int s){return maths(p,w,h,s,8,values.data(),1);},&cost);
      const double ms=timer.nsecsElapsed()/1e6;
      timer.restart();gpu=gpuRunPasses(input,{pass});const double card=timer.nsecsElapsed()/1e6;
      if(n>=2){cpuTimes<<ms;gpuTimes<<card;}
    }
    std::sort(cpuTimes.begin(),cpuTimes.end());std::sort(gpuTimes.begin(),gpuTimes.end());
    if(!close(cpu,gpu,8)){report<<"FAIL benchmark CPU/GPU parity";return 1;}
    report<<QString("%1 sine operations/channel: CPU %2 ms, GPU %3 ms, parity PASS")
        .arg(loops).arg(cpuTimes[2],0,'f',3).arg(gpuTimes[2],0,'f',3);
  }
  return 0;
}
