#include "gpu_pixels.h"
#include "route_choice.h"
#include <QVector2D>
#include <map>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <memory>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <utility>
#include <QThread>
#include <QCoreApplication>
#include <QMutex>
#include <cstring>
#include <vector>

const char *effectVertexShader(){return R"(#version 330 core
    void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);})";}
const char *effectFragmentShader(){return R"(#version 330 core
    uniform sampler2D src;uniform sampler2D aux;uniform int op;
    uniform vec4 params;uniform vec4 ca;uniform vec4 cb;
    out vec4 result;
    vec4 fetch(ivec2 p){ivec2 s=textureSize(src,0);if(any(lessThan(p,ivec2(0)))||any(greaterThanEqual(p,s)))return vec4(0);return texelFetch(src,p,0);}
    void main(){ivec2 p=ivec2(gl_FragCoord.xy);vec4 v=fetch(p);result=v;
      if(op==0){result.rgb=v.rgb*exp2(params.x);}
      else if(op==15){result.rgb=v.rgb*ca.rgb+cb.rgb;}
      else if(op==1){vec2 d=vec2(p)+.5-params.xy;float angle=radians(params.z);
        vec2 axis=vec2(cos(angle),sin(angle));float span=max(1.,dot(abs(axis),cb.zw));
        float t=ca.w<0.?length(d)/max(1.,max(cb.z,cb.w)*.5):.5+dot(d,axis)/span;
        vec3 endColor=vec3(cb.x,cb.y,params.w);result.rgb=mix(v.rgb,mix(ca.rgb,endColor,clamp(t,0.,1.)),abs(ca.w));
      }
      else if(op==2){int r=int(params.x);ivec2 axis=ivec2(params.yz);vec4 sum=vec4(0);
        for(int i=-r;i<=r;++i)sum+=fetch(p+axis*i);result=sum/float(2*r+1);}
      else if(op==16){ // inclusive parallel prefix sum, one axis per pass
        result=v+fetch(p-ivec2(params.yz)*int(params.x));
      }
      else if(op==17){ // box window from two prefix values; transparent borders
        int r=int(params.x);ivec2 axis=ivec2(params.yz);
        ivec2 hi=min(p+axis*r,textureSize(src,0)-1);
        result=(fetch(hi)-fetch(p-axis*(r+1)))/float(2*r+1);
      }
      else if(op==3){ // premultiplied halo under source
        ivec2 q=p-ivec2(params.xy);ivec2 sz=textureSize(aux,0);float alpha=0.;
        if(all(greaterThanEqual(q,ivec2(0)))&&all(lessThan(q,sz)))alpha=texelFetch(aux,q,0).a*ca.a;
        result=v+vec4(ca.rgb*alpha,alpha)*(1.-v.a);
      }
      else if(op==14){ // solid fill: the shape keeps its coverage, not its colour
        result=vec4(ca.rgb,clamp(v.a,0.,1.)*ca.a);
      }
      else if(op==13){ // box downsample by params.x, so a wide blur resolves small
        int k=int(params.x);vec4 sum=vec4(0);float count=0.;
        for(int dy=0;dy<k;++dy)for(int dx=0;dx<k;++dx){sum+=fetch(p*k+ivec2(dx,dy));count+=1.;}
        result=sum/max(count,1.);
      }
      else if(op==6){ // tint the halo and put the layer over it, in one pass
        vec2 uv=(gl_FragCoord.xy-params.xy)/vec2(textureSize(src,0));
        vec4 halo=vec4(0);
        if(all(greaterThanEqual(uv,vec2(0)))&&all(lessThanEqual(uv,vec2(1)))){
          float a=clamp(texture(aux,uv).a,0.,1.)*ca.a;
          halo=vec4(ca.rgb*a,a);
          if(params.z>0.5)halo=texture(aux,uv);
        }
        result=v+halo*(1.-clamp(v.a,0.,1.));
      }
      else if(op==10){ // jump flood seed: this pixel's own coordinate where it is one
        vec2 c=gl_FragCoord.xy;float a=clamp(v.a,0.,1.);
        result=vec4(a>=0.5?c:vec2(-1e6),a<0.5?c:vec2(-1e6));
      }
      else if(op==11){ // one jump of params.x pixels, carrying both seed sets at once
        vec2 c=gl_FragCoord.xy;ivec2 sz=textureSize(src,0);int step=int(params.x);
        vec4 best=v;
        float bi=best.x<-1e5?1e12:dot(c-best.xy,c-best.xy);
        float bo=best.z<-1e5?1e12:dot(c-best.zw,c-best.zw);
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
          ivec2 q=ivec2(c)+ivec2(dx,dy)*step;
          if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,sz)))continue;
          vec4 n=texelFetch(src,q,0);
          if(n.x>-1e5){float e=dot(c-n.xy,c-n.xy);if(e<bi){bi=e;best.xy=n.xy;}}
          if(n.z>-1e5){float e=dot(c-n.zw,c-n.zw);if(e<bo){bo=e;best.zw=n.zw;}}
        }
        result=best;
      }
      else if(op==12){ // seeds become the distances the stroke pass reads
        vec2 c=gl_FragCoord.xy;
        result=vec4(v.x<-1e5?1e6:length(c-v.xy),v.z<-1e5?1e6:length(c-v.zw),0,1);
      }
      else if(op==7){ // resample to the size being written, for a blur resolved small
        result=texture(src,gl_FragCoord.xy/params.xy);
      }
      else if(op==8){ result=vec4(v.rgb*v.a,v.a); }                       // premultiply
      else if(op==9){ result=vec4(v.a>0.?v.rgb/v.a:vec3(0),v.a); }        // undo it
      else if(op==4){ // straight-alpha stroke, exact distances prepared by CPU workers
        vec2 d=texelFetch(aux,p,0).rg;float a=clamp(v.a,0.,1.);int mode=int(params.y);
        float inner=mode==1?0.:clamp(params.x+.5-d.y,0.,1.)*a*ca.a;
        float outer=mode==0?0.:clamp(params.x+.5-d.x,0.,1.)*(1.-a)*ca.a;
        float next=a+outer;result=vec4(next>0.?(v.rgb*(a-inner)+ca.rgb*(inner+outer))/next:vec3(0),next);
      }
    })";}

namespace {
std::atomic<bool> enabled{true};
thread_local bool suppressPixels=false;
std::atomic<quint64> completed{0};
// An eight or sixteen bit picture's two crossings done on the card, each the
// driver's own arithmetic (Pixels::calibrateBytes, Pixels::calibrateShorts):
// its integers widened to floats through a table of what the driver makes of
// each value, 256 values to a row, and floats narrowed to integers by the
// driver's rule -- clamped, times the top value (255 or 65535) in float,
// rounded to nearest even.
const char *widenFragmentShader(){return R"(#version 330 core
    uniform usampler2D src;uniform sampler2D table;out vec4 result;
    float widened(uint v){return texelFetch(table,ivec2(int(v&255u),int(v>>8u)),0).r;}
    void main(){uvec4 v=texelFetch(src,ivec2(gl_FragCoord.xy),0);
      result=vec4(widened(v.r),widened(v.g),widened(v.b),widened(v.a));})";}
const char *narrowFragmentShader(){return R"(#version 330 core
    uniform sampler2D src;uniform float top;out uvec4 result;
    void main(){result=uvec4(roundEven(clamp(texelFetch(src,ivec2(gl_FragCoord.xy),0),0.,1.)*top));})";}
struct Pixels {
  QOffscreenSurface surface; QOpenGLContext context;
  std::unique_ptr<QOpenGLShaderProgram> shader,widener,narrower;
  GLint widenerSrc=-1,widenerTable=-1,narrowerSrc=-1,narrowerTop=-1;
  bool attempted=false,ready=false; GLuint vao=0,fbo=0;
  // The textures a chain draws in, each bound to the texture unit of its own
  // number: three rotating slots, the uploaded second input, and for an eight
  // or sixteen bit picture its integers going up and its integers coming back
  // (runPassesHere), then the tables the integers going up are widened
  // through, bytes' and shorts'. Each keeps the size and format it was last
  // given.
  struct Texture { GLuint id=0; QSize size; GLenum format=0; };
  enum { IntegerSource=4, IntegerResult=5, ByteTable=6, ShortTable=7 };
  Texture tex[8];
  static bool integer(GLenum format){return format==GL_RGBA8UI || format==GL_RGBA16UI;}
  // Textures a chain let go of when it needed another size, kept for the next
  // chain that needs theirs. New storage for a texture cost the card about
  // 1.2 ms here (RTX 5050 Laptop, textures of 1 to 11 MB), and chains come in
  // sizes that take turns: a Promo frame drawn from empty caches runs six
  // chains at six sizes and gave seventeen textures new storage, 42.8 ms of
  // chains; with spares one, 23.4 ms. A Kotonoha beat, band parts at 960x1
  // and 960x27 and the whole layer at 960x540, gave six new storage a frame,
  // 20.7 ms of chains; with spares none, 15.9 ms. Past this many bytes the
  // least lately put by are deleted: 128 MB holds Promo's six sizes at
  // 960x540 with their byte textures, where 96 MB still left twelve to give
  // storage. 0 gives every texture its new size where it is.
  struct Spare { Texture texture; quint64 put=0; };
  std::vector<Spare> spares;quint64 spareClock=0;
  qint64 spareBytes=128ll<<20;
  static qint64 bytesOf(const Texture &t){return qint64(t.size.width())*t.size.height()*(t.format==GL_RGBA32F?16:t.format==GL_RGBA16UI?8:4);}
  // Storage of this size and format for `held`, on the texture unit that is
  // active: what it has, a spare that has it, or new.
  void fit(QOpenGLExtraFunctions *gl,Texture &held,QSize size,GLenum format) {
    if(held.id && held.size==size && held.format==format)return;
    const bool keep=spareBytes>0;
    const auto found=keep?std::find_if(spares.begin(),spares.end(),[&](const Spare &k){return k.texture.size==size&&k.texture.format==format;})
                         :spares.end();
    Texture next;
    if(found!=spares.end()){next=found->texture;spares.erase(found);}
    if(keep && held.id){spares.push_back({held,++spareClock});held={};}
    if(next.id)held=next;
    else {
      if(!held.id)gl->glGenTextures(1,&held.id);
      gl->glBindTexture(GL_TEXTURE_2D,held.id);
      gl->glTexImage2D(GL_TEXTURE_2D,0,GLint(format),size.width(),size.height(),0,integer(format)?GL_RGBA_INTEGER:GL_RGBA,
                       format==GL_RGBA32F?GL_FLOAT:format==GL_RGBA16UI?GL_UNSIGNED_SHORT:GL_UNSIGNED_BYTE,nullptr);
      held.size=size;held.format=format;
    }
    qint64 total=0;
    for(const Spare &k:spares)total+=bytesOf(k.texture);
    while(total>spareBytes && !spares.empty()) {
      const auto oldest=std::min_element(spares.begin(),spares.end(),[](const Spare &a,const Spare &b){return a.put<b.put;});
      total-=bytesOf(oldest->texture);gl->glDeleteTextures(1,&oldest->texture.id);spares.erase(oldest);
    }
  }
  // Whether an eight bit picture may go up as bytes and come back as bytes and
  // still give exactly the bytes of the float route, asked of each context by
  // its first eight bit chain (calibrateBytes); and the same of a sixteen bit
  // picture's shorts, by its first sixteen bit chain (calibrateShorts).
  bool bytesCalibrated=false,bytesUpExact=false,bytesBackExact=false;
  void calibrateBytes(QOpenGLExtraFunctions *gl);
  bool shortsCalibrated=false,shortsUpExact=false,shortsBackExact=false;
  void calibrateShorts(QOpenGLExtraFunctions *gl);
  // What a chain used to ask the driver for each time, asked once here: the
  // texture limit, and the shared program's uniforms, looked up when it links
  // rather than by name in every pass (see runPassesHere for what each
  // question cost).
  GLint maxTextureSize=0;
  struct EffectUniforms { GLint src=-1,aux=-1,op=-1,params=-1,ca=-1,cb=-1; } effectAt;
  // Whether this context may hold an error nobody has asked about: at the
  // start, and after a chain or a distance transform that gave up before its
  // own check. Only then are errors cleared before the next one begins.
  bool errorsUnasked=true;
  QString status="GPU pixel effects not initialized";
  // The two things that must be made on the GUI thread: the context, and the
  // offscreen surface, which on Windows is a window. Made there, they can then be
  // used by whichever thread the context is moved to.
  bool handlesTried=false,handlesOk=false;
  bool createHandles() {
    if(handlesTried)return handlesOk;handlesTried=true;
    // 4.3 where the driver has it, for the compute shaders of the distance
    // transform; 3.3 otherwise, which is all every other pass here needs.
    QSurfaceFormat fmt;fmt.setVersion(4,3);fmt.setProfile(QSurfaceFormat::CoreProfile);
    context.setFormat(fmt);
    if(!context.create() || context.format().version()<qMakePair(4,3)) {
      fmt.setVersion(3,3);context.setFormat(fmt);
      if(!context.create()){status="GPU effects unavailable: OpenGL 3.3 context creation failed";return false;}
    }
    surface.setFormat(context.format());surface.create();
    handlesOk=true;return true;
  }
  bool init() {
    if(attempted)return ready;attempted=true;
    if(!createHandles())return false;
    if(!context.makeCurrent(&surface)){status="GPU effects unavailable: cannot activate offscreen context";return false;}
    auto *gl=context.extraFunctions();gl->initializeOpenGLFunctions();
    const QString renderer=QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER)));
    if(renderer.contains("llvmpipe",Qt::CaseInsensitive)||renderer.contains("software",Qt::CaseInsensitive)){status="GPU effects unavailable: software renderer "+renderer;return false;}
    shader=std::make_unique<QOpenGLShaderProgram>();
    const char *vertex=effectVertexShader();
    const char *fragment=effectFragmentShader();
    if(!shader->addShaderFromSourceCode(QOpenGLShader::Vertex,vertex)||!shader->addShaderFromSourceCode(QOpenGLShader::Fragment,fragment)||!shader->link()) {status=shader->log();return false;}
    effectAt={shader->uniformLocation("src"),shader->uniformLocation("aux"),shader->uniformLocation("op"),
              shader->uniformLocation("params"),shader->uniformLocation("ca"),shader->uniformLocation("cb")};
    // Without them eight bit pictures take the float route, as they always did.
    auto build=[&](const char *body) {
      auto program=std::make_unique<QOpenGLShaderProgram>();
      if(!program->addShaderFromSourceCode(QOpenGLShader::Vertex,vertex) ||
         !program->addShaderFromSourceCode(QOpenGLShader::Fragment,body) || !program->link())program.reset();
      return program;
    };
    widener=build(widenFragmentShader());
    narrower=build(narrowFragmentShader());
    if(widener){widenerSrc=widener->uniformLocation("src");widenerTable=widener->uniformLocation("table");}
    if(narrower){narrowerSrc=narrower->uniformLocation("src");narrowerTop=narrower->uniformLocation("top");}
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maxTextureSize);
    gl->glGenVertexArrays(1,&vao);gl->glGenFramebuffers(1,&fbo);
    status="GPU float pixel effects: "+renderer;ready=true;return true;
  }
  // Programs built from plugin bodies, each with its uniforms looked up once it
  // links, and the bodies that would not build. Kept for the life of the
  // context: compiling is slow and a body does not change between frames.
  // std::map rather than QHash: the value owns a program and cannot be copied.
  struct PackProgram {
    std::unique_ptr<QOpenGLShaderProgram> program;
    GLint src=-1,original=-1,sourceSize=-1,values=-1,depth=-1,seconds=-1,frame=-1,scale=-1,pass=-1,passes=-1,box=-1;
  };
  std::map<QString,PackProgram> plugins;
  QHash<QString,QString> refused;
  const PackProgram *programFor(const QString &body) {
    if(auto found=plugins.find(body);found!=plugins.end())return &found->second;
    if(refused.contains(body))return nullptr;
    // The plugin writes a function; everything around it is ours, so it never
    // sees a sampler, a coordinate system or a version pragma.
    const QString source=packShaderSource(body);
    auto program=std::make_unique<QOpenGLShaderProgram>();
    if(!program->addShaderFromSourceCode(QOpenGLShader::Vertex,effectVertexShader()) ||
       !program->addShaderFromSourceCode(QOpenGLShader::Fragment,source.toUtf8().constData()) ||
       !program->link()) {
      refused.insert(body,program->log().isEmpty()?QString("shader did not compile"):program->log());
      return nullptr;
    }
    PackProgram made;
    auto at=[&](const char *name){return program->uniformLocation(name);};
    made.src=at("src");made.original=at("wavyOriginalTexture");made.sourceSize=at("sourceSize");
    made.values=at("values");made.depth=at("processingDepth");made.seconds=at("wavySeconds");
    made.frame=at("wavyFrame");made.scale=at("wavyScale");made.pass=at("wavyPass");made.passes=at("wavyPasses");
    made.box=at("wavyBox");
    made.program=std::move(program);
    return &plugins.emplace(body,std::move(made)).first->second;
  }
  // The distance transform's two compute programs (OpenGL 4.3) and its
  // buffers: ids, the rows' squared distances and nearest columns, the
  // envelopes' scratch, the squared distances and the nearest ids.
  std::unique_ptr<QOpenGLShaderProgram> edtRows,edtColumns;
  struct EdtUniforms { GLint width=-1,height=-1,first=-1,count=-1,wantSites=-1; } edtRowsAt,edtColumnsAt;
  bool edtTried=false,edtOk=false;
  GLuint edtBuffers[7]{};
  qint64 edtBytes[7]{};
  bool released=false;
  // Frees what the context holds, on the thread that owns the context.
  void release(){if(released)return;released=true;if(handlesOk&&context.makeCurrent(&surface)){auto *gl=context.extraFunctions();plugins.clear();shader.reset();widener.reset();narrower.reset();edtRows.reset();edtColumns.reset();if(edtBuffers[0])gl->glDeleteBuffers(7,edtBuffers);for(Texture &t:tex)if(t.id)gl->glDeleteTextures(1,&t.id);for(Spare &k:spares)gl->glDeleteTextures(1,&k.texture.id);spares.clear();if(fbo)gl->glDeleteFramebuffers(1,&fbo);if(vao)gl->glDeleteVertexArrays(1,&vao);context.doneCurrent();}}
  ~Pixels(){release();}
};
// Whether an eight bit picture can cross as bytes both ways, asked of a
// context once, by its first eight bit chain. The float route has the driver
// widen the bytes into a float texture on the way up, and narrow the last
// pass's floats into bytes as it reads them back. The byte route uploads the
// bytes as they are and widens them on the card, and narrows the last pass on
// the card into bytes the readback only copies -- each with the driver's own
// arithmetic, not the card's, which differs from it: the card reads a byte
// texture as v/255 rounded, the driver widens to v*(1/255.f), different in 126
// of the 256 values; the card rounds into a byte target by a rule of its own,
// and differs from the driver at 3,407,743 floats of [0,1] on this card (RTX
// 5050 Laptop), each a 1 either side of a half -- 1,031 to 72,502 bytes of
// each 960x540 frame that had chains, up to 3/255, and no quicker than
// narrowing on the card with the driver's rule. So:
// - up: the bytes are widened through a table of the driver's own floats for
//   each byte value, read back from it here; every value in every channel is
//   then compared, the card's floats against the driver's, bit for bit;
// - back: the narrowing pass takes the driver's rule as measured on this card
//   over all 2^32 floats -- clamped, times 255 in float, rounded to nearest
//   even; the pass and the driver then agreed on every one of them. Where each
//   byte value begins under the pass is found by cutting in sixteen, all 255
//   at once, and the pass and the driver are asked on either side of each, at
//   0 and 1, and at what lies outside them (negatives, values above 1, the
//   infinities, NaN). Both being monotonic, agreeing at every edge is agreeing
//   on every float.
// Anything else, and that crossing stays on the float route.
//
// The floats outside [0,1] the narrowing pass and the driver are both asked at:
// zeros and denormals of either sign, negatives, values above 1, the
// infinities, and seven NaNs.
const quint32 outsideUnit[]={
    0x80000000u,0x80000001u,0x807FFFFFu,0x80800000u,0xBB808081u,0xBF000000u,0xBF800000u,0xFF7FFFFFu,0xFF800000u,
    0x00000001u,0x007FFFFFu,0x00800000u,
    0x3F800001u,0x3F800100u,0x3FC00000u,0x40000000u,0x437F0000u,0x7F7FFFFFu,0x7F800000u,
    0x7FC00000u,0x7F800001u,0x7FBFFFFFu,0x7FFFFFFFu,0xFFC00000u,0xFF800001u,0xFFFFFFFFu};
void Pixels::calibrateBytes(QOpenGLExtraFunctions *gl) {
  bytesCalibrated=true;
  if(!widener || !narrower)return;
  GLuint t[3];gl->glGenTextures(3,t);
  auto allocate=[&](GLuint id,GLenum format,int width,const void *bits) {
    gl->glBindTexture(GL_TEXTURE_2D,id);
    gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    gl->glTexImage2D(GL_TEXTURE_2D,0,GLint(format),width,1,0,format==GL_RGBA8UI?GL_RGBA_INTEGER:GL_RGBA,
                     format==GL_RGBA32F && !bits?GL_FLOAT:GL_UNSIGNED_BYTE,bits);
  };
  auto draw=[&](GLuint from,GLuint into,int width) {
    gl->glBindTexture(GL_TEXTURE_2D,from);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,into,0);
    gl->glViewport(0,0,width,1);gl->glDrawArrays(GL_TRIANGLES,0,3);
  };
  auto read=[&](GLuint from,int width,GLenum format,GLenum type,void *into) {
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,from,0);
    gl->glReadPixels(0,0,width,1,format,type,into);
  };
  gl->glPixelStorei(GL_UNPACK_ALIGNMENT,1);gl->glPixelStorei(GL_PACK_ALIGNMENT,1);
  gl->glPixelStorei(GL_UNPACK_ROW_LENGTH,0);gl->glPixelStorei(GL_PACK_ROW_LENGTH,0);
  gl->glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  gl->glDisable(GL_BLEND);gl->glDisable(GL_DEPTH_TEST);gl->glDisable(GL_SCISSOR_TEST);gl->glDisable(GL_DITHER);
  gl->glBindVertexArray(vao);
  // Up: every byte value in every channel, widened by the driver.
  gl->glActiveTexture(GL_TEXTURE0);
  uchar bytes[1024];
  for(int i=0;i<1024;++i)bytes[i]=uchar(i/4+i%4*64);
  allocate(t[0],GL_RGBA32F,256,bytes);
  quint32 widenedByDriver[1024],widenedByCard[1024];
  read(t[0],256,GL_RGBA,GL_FLOAT,widenedByDriver);
  quint32 table[256];
  for(int v=0;v<256;++v)table[v]=widenedByDriver[4*v];
  bool upSame=true;
  for(int i=0;i<1024;++i)upSame=upSame && widenedByDriver[i]==table[bytes[i]];
  // The table the chains widen through, kept with the context.
  gl->glActiveTexture(GL_TEXTURE0+ByteTable);
  Texture &kept=tex[ByteTable];
  if(!kept.id)gl->glGenTextures(1,&kept.id);
  gl->glBindTexture(GL_TEXTURE_2D,kept.id);
  gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  gl->glTexImage2D(GL_TEXTURE_2D,0,GL_R32F,256,1,0,GL_RED,GL_FLOAT,table);
  kept.size=QSize(256,1);kept.format=GL_R32F;
  gl->glActiveTexture(GL_TEXTURE0);
  // And by the card.
  allocate(t[1],GL_RGBA8UI,256,bytes);
  allocate(t[2],GL_RGBA32F,256,nullptr);
  widener->bind();widener->setUniformValue(widenerSrc,0);widener->setUniformValue(widenerTable,int(ByteTable));
  draw(t[1],t[2],256);
  read(t[2],256,GL_RGBA,GL_FLOAT,widenedByCard);
  upSame=upSame && std::memcmp(widenedByDriver,widenedByCard,sizeof widenedByCard)==0;
  // Back: where byte value v begins under the narrowing pass, between lo[v]
  // (narrowed below v) and hi[v] (to v or above), cut in sixteen until the two
  // are neighbours -- each value's fifteen cuts and its hi in a round, so the
  // 2^30 floats of [0,1] take eight rounds of asking. Floats of one sign order
  // as their bits do.
  narrower->bind();narrower->setUniformValue(narrowerSrc,0);narrower->setUniformValue(narrowerTop,255.f);
  quint32 lo[256]{},hi[256]{};
  for(int v=1;v<256;++v)hi[v]=0x3F800000u;
  std::vector<quint32> cuts(256*16);
  std::vector<uchar> narrowed(256*16);
  allocate(t[0],GL_RGBA32F,1024,nullptr);
  allocate(t[1],GL_RGBA8UI,1024,nullptr);
  for(int round=0;round<16;++round) {
    bool open=false;
    for(int v=1;v<256;++v) {
      open|=hi[v]-lo[v]>1;
      for(int k=0;k<16;++k)cuts[size_t(v*16+k)]=lo[v]+quint32(quint64(hi[v]-lo[v])*quint64(k+1)/16);
    }
    if(!open)break;
    gl->glBindTexture(GL_TEXTURE_2D,t[0]);
    gl->glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1024,1,GL_RGBA,GL_FLOAT,cuts.data());
    draw(t[0],t[1],1024);
    read(t[1],1024,GL_RGBA_INTEGER,GL_UNSIGNED_BYTE,narrowed.data());
    for(int v=1;v<256;++v) {
      int k=0;
      while(k<16 && narrowed[size_t(v*16+k)]<v)++k;
      if(k<16)hi[v]=cuts[size_t(v*16+k)];
      if(k>0)lo[v]=cuts[size_t(v*16+k-1)];
    }
  }
  // The pass and the driver on either side of every edge, at 0 and 1, and
  // outside them.
  quint32 asked[1024]{};
  asked[1]=0x3F800000u;
  for(int v=1;v<256;++v){asked[2*v]=lo[v];asked[2*v+1]=hi[v];}
  std::memcpy(asked+512,outsideUnit,sizeof outsideUnit);
  allocate(t[0],GL_RGBA32F,256,nullptr);
  gl->glTexSubImage2D(GL_TEXTURE_2D,0,0,0,256,1,GL_RGBA,GL_FLOAT,asked);
  allocate(t[1],GL_RGBA8UI,256,nullptr);
  draw(t[0],t[1],256);
  uchar byDriver[1024],byPass[1024];
  read(t[0],256,GL_RGBA,GL_UNSIGNED_BYTE,byDriver);
  read(t[1],256,GL_RGBA_INTEGER,GL_UNSIGNED_BYTE,byPass);
  bool backSame=std::memcmp(byDriver,byPass,sizeof byPass)==0 && byPass[0]==0 && byPass[1]==255;
  for(int v=1;v<256 && backSame;++v)
    backSame=hi[v]-lo[v]==1 && byPass[2*v]<v && byPass[2*v+1]>=v && hi[v-1]<=hi[v];
  gl->glDeleteTextures(3,t);
  narrower->release();
  // An error anywhere and neither is trusted.
  const bool clean=gl->glGetError()==GL_NO_ERROR;
  for(int i=0;i<16&&gl->glGetError()!=GL_NO_ERROR;++i){}
  bytesUpExact=clean && upSame;
  bytesBackExact=clean && backSame;
}
// The same for a sixteen bit picture's shorts, asked of a context once, by its
// first sixteen bit chain. Measured here over every value and every float, as
// for bytes: the driver widens a short to v*(1/65535.f), and the card reads an
// RGBA16 texture as v/65535 rounded, different in 512 of the 65536 values; the
// driver narrows by the bytes' rule with 65535 for 255, and the card's own
// rounding into an RGBA16 target differs from it at 7,570,777 floats of [0,1],
// each a 1 either side of a half. The narrowing pass with 65535 for its top
// value agreed with the driver on all 2^32 floats. So:
// - up: the table is the driver's own floats for all 65536 values, 256 to a
//   row, and every value in every channel is compared, card against driver;
// - back: the first float of [0,1] the driver's rule takes to each of the
//   65535 values is worked out here, in the same float arithmetic, and the
//   pass and the driver are asked on either side of every one, at 0 and 1, and
//   outside them. Each must give the value below the edge and the value at it.
//   Both being monotonic, they then follow that rule on every float of [0,1].
//   That is one round of asking, where cutting in sixteen would take eight, of
//   a million floats each.
// It took 14 to 26 ms here, once a context, most of it waiting on the
// driver's own conversions of the 393,000 values it is given -- on a machine
// busy with a game at the time, when the eight bit calibration took 11 to
// 34 ms (3.2 to 5.5 on a quiet one).
void Pixels::calibrateShorts(QOpenGLExtraFunctions *gl) {
  shortsCalibrated=true;
  if(!widener || !narrower)return;
  GLuint t[4];gl->glGenTextures(4,t);
  auto allocate=[&](GLuint id,GLenum format,int width,int height,GLenum type,const void *bits) {
    gl->glBindTexture(GL_TEXTURE_2D,id);
    gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    gl->glTexImage2D(GL_TEXTURE_2D,0,GLint(format),width,height,0,integer(format)?GL_RGBA_INTEGER:GL_RGBA,type,bits);
  };
  auto draw=[&](GLuint from,GLuint into,int width,int height) {
    gl->glBindTexture(GL_TEXTURE_2D,from);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,into,0);
    gl->glViewport(0,0,width,height);gl->glDrawArrays(GL_TRIANGLES,0,3);
  };
  auto read=[&](GLuint from,int width,int height,GLenum format,GLenum type,void *into) {
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,from,0);
    gl->glReadPixels(0,0,width,height,format,type,into);
  };
  gl->glPixelStorei(GL_UNPACK_ALIGNMENT,1);gl->glPixelStorei(GL_PACK_ALIGNMENT,1);
  gl->glPixelStorei(GL_UNPACK_ROW_LENGTH,0);gl->glPixelStorei(GL_PACK_ROW_LENGTH,0);
  gl->glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  gl->glDisable(GL_BLEND);gl->glDisable(GL_DEPTH_TEST);gl->glDisable(GL_SCISSOR_TEST);gl->glDisable(GL_DITHER);
  gl->glBindVertexArray(vao);
  // Up: every value in every channel -- 256x256 pixels, each channel running
  // through all 65536 values in an order of its own -- given to the driver to
  // widen, and as they are for the card.
  gl->glActiveTexture(GL_TEXTURE0);
  const int count=65536*4;
  std::vector<quint16> shorts(count);
  for(int i=0;i<65536;++i) {
    shorts[size_t(4*i)]=quint16(i);shorts[size_t(4*i+1)]=quint16(i^0x5A5A);
    shorts[size_t(4*i+2)]=quint16(quint32(i)*40503u);shorts[size_t(4*i+3)]=quint16(65535-i);
  }
  allocate(t[0],GL_RGBA32F,256,256,GL_UNSIGNED_SHORT,shorts.data());
  allocate(t[1],GL_RGBA16UI,256,256,GL_UNSIGNED_SHORT,shorts.data());
  // Back, while the driver widens: the driver's rule, as the pass works it,
  // and where it begins each value -- found from where v-1/2 lies, a float or
  // two either way. Floats of one sign order as their bits do.
  auto rule=[](quint32 bits) {
    float x;std::memcpy(&x,&bits,4);
    const float y=x*65535.f,whole=std::floor(y),part=y-whole;
    return int(whole)+int(part>.5f || (part==.5f && (int(whole)&1)));
  };
  // Asked: 0 and 1, both sides of every edge, then what lies outside [0,1].
  const int rows=(2*65536+int(std::size(outsideUnit))+1023)/1024;
  std::vector<quint32> asked(size_t(rows)*1024,0);
  asked[1]=0x3F800000u;
  bool edgesFound=true;
  for(int v=1;v<65536 && edgesFound;++v) {
    const float halfway=float((v-.5)/65535.);
    quint32 at;std::memcpy(&at,&halfway,4);
    int steps=0;
    while(rule(at)>=v && steps<64){--at;++steps;}
    while(rule(at)<v && steps<64){++at;++steps;}
    edgesFound=steps<64;
    asked[size_t(2*v)]=at-1;asked[size_t(2*v+1)]=at;
  }
  std::copy(std::begin(outsideUnit),std::end(outsideUnit),asked.begin()+2*65536);
  allocate(t[2],GL_RGBA32F,256,rows,GL_FLOAT,asked.data());
  allocate(t[3],GL_RGBA16UI,256,rows,GL_UNSIGNED_SHORT,nullptr);
  narrower->bind();narrower->setUniformValue(narrowerSrc,0);narrower->setUniformValue(narrowerTop,65535.f);
  draw(t[2],t[3],256,rows);
  // What the driver made of the shorts, then of the asked floats, then what the
  // pass made of them: one wait for the card.
  std::vector<quint32> widenedByDriver(count),widenedByCard(count),table(65536);
  std::vector<quint16> byDriver(asked.size()),byPass(asked.size());
  read(t[0],256,256,GL_RGBA,GL_FLOAT,widenedByDriver.data());
  read(t[2],256,rows,GL_RGBA,GL_UNSIGNED_SHORT,byDriver.data());
  read(t[3],256,rows,GL_RGBA_INTEGER,GL_UNSIGNED_SHORT,byPass.data());
  bool backSame=edgesFound && byDriver==byPass && byPass[0]==0 && byPass[1]==65535;
  for(int v=1;v<65536 && backSame;++v)
    backSame=byPass[size_t(2*v)]==v-1 && byPass[size_t(2*v+1)]==v;
  for(int v=0;v<65536;++v)table[size_t(v)]=widenedByDriver[size_t(4*v)];
  bool upSame=true;
  for(int i=0;i<count;++i)upSame=upSame && widenedByDriver[size_t(i)]==table[shorts[size_t(i)]];
  // The table the chains widen through, kept with the context, and the card's
  // widening through it, into the texture the driver's floats were read from.
  gl->glActiveTexture(GL_TEXTURE0+ShortTable);
  Texture &kept=tex[ShortTable];
  if(!kept.id)gl->glGenTextures(1,&kept.id);
  gl->glBindTexture(GL_TEXTURE_2D,kept.id);
  gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  gl->glTexImage2D(GL_TEXTURE_2D,0,GL_R32F,256,256,0,GL_RED,GL_FLOAT,table.data());
  kept.size=QSize(256,256);kept.format=GL_R32F;
  gl->glActiveTexture(GL_TEXTURE0);
  widener->bind();widener->setUniformValue(widenerSrc,0);widener->setUniformValue(widenerTable,int(ShortTable));
  draw(t[1],t[0],256,256);
  read(t[0],256,256,GL_RGBA,GL_FLOAT,widenedByCard.data());
  upSame=upSame && widenedByDriver==widenedByCard;
  gl->glDeleteTextures(4,t);
  widener->release();
  // An error anywhere and neither is trusted.
  const bool clean=gl->glGetError()==GL_NO_ERROR;
  for(int i=0;i<16&&gl->glGetError()!=GL_NO_ERROR;++i){}
  shortsUpExact=clean && upSame;
  shortsBackExact=clean && backSame;
}
struct GpuService {
  QThread *thread=nullptr; QObject *dispatcher=nullptr; Pixels *pixels=nullptr;
  std::atomic<bool> ready{false};
};
GpuService gpuService;
Pixels &pixels(){
  thread_local std::unique_ptr<Pixels> instance;
  if(!instance){
    instance=std::make_unique<Pixels>();
  }
  return *instance;
}
}
void enableGpuPixels(bool value){enabled=value;}
CpuPixelScope::CpuPixelScope():previous(suppressPixels){suppressPixels=true;}
CpuPixelScope::~CpuPixelScope(){suppressPixels=previous;}
quint64 gpuPixelOperations(){return completed.load();}
QString gpuPixelStatus(){return pixels().status;}
bool canUseGpuPixels(){
  return enabled && !suppressPixels && qApp &&
      (QThread::currentThread() == qApp->thread() || gpuService.ready.load()) &&
      !qApp->arguments().contains("--self-test") && !qApp->arguments().contains("--precision-self-test") &&
      !qApp->arguments().contains("--surface-effects-self-test");
}
namespace {
bool gpuAvailableHere() {
  return enabled && !suppressPixels && qApp &&
      (QThread::currentThread() == qApp->thread()) &&
      !qApp->arguments().contains("--self-test") &&
      !qApp->arguments().contains("--precision-self-test") &&
      !qApp->arguments().contains("--surface-effects-self-test");
}
}
QString gpuShaderProblem(const QString &body) {
  if(body.isEmpty())return "no shader supplied";
  if(!gpuAvailableHere())return "no usable graphics card here";
  auto *previous=QOpenGLContext::currentContext();QSurface *oldSurface=previous?previous->surface():nullptr;
  struct Restore{QOpenGLContext *context;QSurface *surface;~Restore(){if(context&&surface)context->makeCurrent(surface);else if(QOpenGLContext::currentContext())QOpenGLContext::currentContext()->doneCurrent();}} restore{previous,oldSurface};
  auto &s=pixels();
  if(!s.init() || !s.context.makeCurrent(&s.surface))return "graphics card unavailable";
  if(s.programFor(body))return QString();
  return s.refused.value(body,QString("shader did not compile"));
}
// The whole pipeline lives here, once. An effect never touches OpenGL: it
// describes its passes and this runs them, which is what keeps a new effect
// from having to get any of this right a second time.
// The whole chain on the given context, which the calling thread owns.
static QImage runPassesHere(const QImage &source, const QVector<GpuPass> &passes, Pixels &s) {
  auto *previous=QOpenGLContext::currentContext();QSurface *oldSurface=previous?previous->surface():nullptr;
  struct Restore{QOpenGLContext *context;QSurface *surface;~Restore(){if(context&&surface)context->makeCurrent(surface);else if(QOpenGLContext::currentContext())QOpenGLContext::currentContext()->doneCurrent();}} restore{previous,oldSurface};
  if(!s.init() || !s.context.makeCurrent(&s.surface))return {};
  auto *gl=s.context.extraFunctions();
  // A chain asks the driver one question, after its readback. Under a threaded
  // driver (NVIDIA's) every question waits for the driver's thread to work
  // through all that was queued before it. The chain used to clear errors
  // before it, ask for the texture limit, and in every pass check the
  // framebuffer and look up each uniform by name. Most of that waiting was for
  // work the readback waits for anyway, and it moved there; what went is each
  // question's own cost, about 3 microseconds. That shows where chains are
  // many: a Kotonoha beat frame with every effect on the card, 24 chains and
  // some 680 questions, went from 19.1 to 17.0 ms of chains (the frame 31.2 to
  // 28.8 ms). Frames of one to six large chains (Promo) came out the same.
  // Errors are cleared first only when something before may have left one.
  if(s.errorsUnasked){for(int i=0;i<16&&gl->glGetError()!=GL_NO_ERROR;++i){}s.errorsUnasked=false;}
  const GLint limit=s.maxTextureSize;
  if(source.width()>limit||source.height()>limit)return {};
  // A chain that gives up before its check leaves its errors to the next one.
  struct Unasked{bool &flag;bool asked=false;~Unasked(){if(!asked)flag=true;}} unasked{s.errorsUnasked};

  // An eight or sixteen bit raster crosses the bus in its own depth. Widening
  // it to float first was a walk over the whole image here and four or two
  // times the traffic there, for values the card reads as normalised floats
  // either way.
  const bool eightBit=source.format()==QImage::Format_RGBA8888 ||
                      source.format()==QImage::Format_RGBA8888_Premultiplied;
  const bool sixteenBit=source.format()==QImage::Format_RGBA64 ||
                        source.format()==QImage::Format_RGBA64_Premultiplied;
  const bool premul=source.format()==QImage::Format_RGBA32FPx4_Premultiplied ||
                    source.format()==QImage::Format_RGBA8888_Premultiplied ||
                    source.format()==QImage::Format_RGBA64_Premultiplied ||
                    source.format()==QImage::Format_ARGB32_Premultiplied;
  auto image = (eightBit || sixteenBit) ? source
                              : source.convertToFormat(premul?QImage::Format_RGBA32FPx4_Premultiplied
                                                             :QImage::Format_RGBA32FPx4);
  const GLenum uploadType = eightBit ? GL_UNSIGNED_BYTE : sixteenBit ? GL_UNSIGNED_SHORT : GL_FLOAT;
  const int pixelBytes=eightBit?4:sixteenBit?8:16;
  if(image.bytesPerLine()%pixelBytes)image=image.copy();
  // And comes back in its own depth. Both crossings are worked on the card,
  // with the driver's own arithmetic, wherever that gives the float route's
  // values exactly (Pixels::calibrateBytes, Pixels::calibrateShorts). For a
  // 960x540 chain here the driver took 0.59 ms to widen the bytes into a float
  // texture and 2.01 ms to narrow the floats as it read them back; the byte
  // upload takes 0.17 ms, and the readback of the bytes narrowed on the card
  // 0.53. A warm Promo frame's one chain went from 4.6 to 2.2 ms by the
  // readback, to 1.6 with the upload too. With shorts the driver is slower
  // still: 5.2 ms to widen a 960x540 picture's and 3.9 to narrow them (the
  // readback alone, once the card had caught up), against 0.5 and 1.4 for the
  // shorts as they are. The same Promo frame at 16 bpc runs eight such chains:
  // 92 ms of them became 33, the frame 287 ms 225; a Kotonoha beat's 24
  // chains, 37 ms 19, the frame 59 ms 42.
  if(eightBit && !s.bytesCalibrated)s.calibrateBytes(gl);
  if(sixteenBit && !s.shortsCalibrated)s.calibrateShorts(gl);
  const bool integersUp=eightBit?s.bytesUpExact:sixteenBit && s.shortsUpExact;
  const bool integersBack=eightBit?s.bytesBackExact:sixteenBit && s.shortsBackExact;
  const GLenum integerFormat=eightBit?GL_RGBA8UI:GL_RGBA16UI;
  // Three rotating slots: the image the current effect started from, and two to
  // work between. A fourth carries an uploaded second input when a pass wants
  // one. A pass never writes to a slot it is also reading.
  int current=0;
  QSize slotSize[3]={image.size(),{}, {}};
  auto define=[&](int slot,QSize size,const void *bits,bool smooth,GLenum type=GL_FLOAT,GLenum format=GL_RGBA32F){
    gl->glActiveTexture(GL_TEXTURE0+slot);
    s.fit(gl,s.tex[slot],size,format);
    gl->glBindTexture(GL_TEXTURE_2D,s.tex[slot].id);
    gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,smooth?GL_LINEAR:GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,smooth?GL_LINEAR:GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if(bits)gl->glTexSubImage2D(GL_TEXTURE_2D,0,0,0,size.width(),size.height(),Pixels::integer(format)?GL_RGBA_INTEGER:GL_RGBA,type,bits);
  };
  gl->glPixelStorei(GL_UNPACK_ALIGNMENT,1);
  gl->glPixelStorei(GL_PACK_ALIGNMENT,1);
  // QImage may wrap a buffer with row padding; OpenGL must use that stride.
  gl->glPixelStorei(GL_UNPACK_ROW_LENGTH,GLint(image.bytesPerLine()/pixelBytes));
  if(integersUp) {
    define(Pixels::IntegerSource,image.size(),image.constBits(),false,uploadType,integerFormat);
    define(0,image.size(),nullptr,true);
  } else define(0,image.size(),image.constBits(),true,uploadType);
  gl->glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
  gl->glPixelStorei(GL_PACK_ROW_LENGTH,0);

  gl->glBindFramebuffer(GL_FRAMEBUFFER,s.fbo);
  gl->glDisable(GL_BLEND);gl->glDisable(GL_DEPTH_TEST);gl->glDisable(GL_SCISSOR_TEST);
  // Rounding into integers as calibrated: undithered.
  gl->glDisable(GL_DITHER);
  gl->glBindVertexArray(s.vao);
  if(integersUp) { // widened into slot 0 on the card, through the driver's table
    const int table=eightBit?Pixels::ByteTable:Pixels::ShortTable;
    s.widener->bind();
    s.widener->setUniformValue(s.widenerSrc,int(Pixels::IntegerSource));
    s.widener->setUniformValue(s.widenerTable,table);
    gl->glActiveTexture(GL_TEXTURE0+table);gl->glBindTexture(GL_TEXTURE_2D,s.tex[table].id);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.tex[0].id,0);
    gl->glViewport(0,0,image.width(),image.height());
    gl->glDrawArrays(GL_TRIANGLES,0,3);
  }
  s.shader->bind();
  QOpenGLShaderProgram *bound=s.shader.get();

  int lastWritten=-1;QSize lastSize=image.size();
  for(qsizetype at=0;at<passes.size();++at) {
    const GpuPass &pass=passes[at];
    const bool fromPrevious = pass.input==GpuPass::Previous && lastWritten>=0;
    const int inputSlot = fromPrevious ? lastWritten : current;
    const QSize inputSize = fromPrevious ? lastSize : slotSize[current];
    const QSize target = pass.size.isEmpty() ? inputSize : pass.size;
    if(target.isEmpty() || target.width()>limit || target.height()>limit ||
       pass.values.size()>256)return {};

    // Resolved before a target is chosen, so the two can never collide.
    int auxSlot=-1;
    if(pass.useAux && pass.auxImage.isNull()) {
      const bool auxPrevious = pass.auxFrom==GpuPass::Previous && lastWritten>=0;
      auxSlot = auxPrevious ? lastWritten : current;
    }
    int write=-1;
    for(int candidate=0;candidate<3;++candidate)
      if(candidate!=current && candidate!=inputSlot && candidate!=auxSlot){write=candidate;break;}
    if(write<0)return {};
    if(slotSize[write]!=target){define(write,target,nullptr,true);slotSize[write]=target;}

    if(pass.useAux) {
      if(!pass.auxImage.isNull()) {
        const auto extra=pass.auxImage.convertToFormat(
            pass.auxImage.format()==QImage::Format_RGBA32FPx4_Premultiplied
                ? QImage::Format_RGBA32FPx4_Premultiplied : QImage::Format_RGBA32FPx4);
        define(3,extra.size(),extra.constBits(),pass.auxSmooth);
        auxSlot=3;
      } else if(pass.source.isEmpty()) {
        gl->glActiveTexture(GL_TEXTURE0+auxSlot);gl->glBindTexture(GL_TEXTURE_2D,s.tex[auxSlot].id);
        gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,pass.auxSmooth?GL_LINEAR:GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,pass.auxSmooth?GL_LINEAR:GL_NEAREST);
      }
      // A pack's body reads it by texelFetch, which no filter touches; it is
      // left as it is for the passes after that sample it smoothly.
    }
    // A pass carrying its own body runs under its own program. One that will
    // not compile is not a reason to draw the wrong thing: the whole chain is
    // abandoned and the caller falls back to the CPU, which is what a null
    // return has always meant here.
    QOpenGLShaderProgram *program=s.shader.get();
    const Pixels::PackProgram *pack=nullptr;
    if(!pass.source.isEmpty()) {
      pack=s.programFor(pass.source);
      if(!pack)return {};
      program=pack->program.get();
    }
    if(program!=bound){program->bind();bound=program;}
    // Uniforms belong to the current program. A custom pass can precede a
    // built-in pass using aux, so set it only after switching programs. Each
    // is set where the program said it was when it linked: asking by name is
    // a question to the driver.
    if(pass.useAux && pass.source.isEmpty())program->setUniformValue(s.effectAt.aux,auxSlot);
    if(pass.useAux && !pass.source.isEmpty())program->setUniformValue(pack->original,auxSlot);
    gl->glActiveTexture(GL_TEXTURE0+inputSlot);gl->glBindTexture(GL_TEXTURE_2D,s.tex[inputSlot].id);
    program->setUniformValue(pass.source.isEmpty()?s.effectAt.src:pack->src,inputSlot);
    // Not asked whether the framebuffer is complete: one float texture just
    // given this size always makes one that is, and drawing into one that is
    // not is an error, which the check after the readback finds.
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.tex[write].id,0);
    gl->glViewport(0,0,target.width(),target.height());
    if(pass.source.isEmpty()) {
      program->setUniformValue(s.effectAt.op,pass.op);
      program->setUniformValue(s.effectAt.params,pass.params);
      program->setUniformValue(s.effectAt.ca,pass.a);program->setUniformValue(s.effectAt.cb,pass.b);
    } else {
      program->setUniformValue(pack->sourceSize,QVector2D(float(inputSize.width()),
                                                          float(inputSize.height())));
      QVector<float> padded=pass.values;
      padded.resize(256);
      program->setUniformValueArray(pack->values,padded.constData(),256,1);
      program->setUniformValue(pack->depth,pass.processingDepth);
      program->setUniformValue(pack->seconds,float(pass.seconds));
      program->setUniformValue(pack->frame,float(pass.frame));
      program->setUniformValue(pack->scale,float(pass.scale));
      program->setUniformValue(pack->pass,pass.shaderPass);
      program->setUniformValue(pack->passes,pass.shaderPasses);
      program->setUniformValue(pack->box,QVector4D(float(pass.box.x()),float(pass.box.y()),
                                                   float(pass.box.width()),float(pass.box.height())));
    }
    gl->glDrawArrays(GL_TRIANGLES,0,3);
    lastWritten=write;lastSize=target;
    // The result becomes the image the next effect starts from.
    if(pass.endsEffect)current=write;
  }
  if(lastWritten<0)return {};

  QImage result(lastSize,image.format());if(result.isNull())return {};
  if(integersBack) { // narrowed into integers on the card, by the driver's rule
    define(Pixels::IntegerResult,lastSize,nullptr,false,uploadType,integerFormat);
    s.narrower->bind();s.narrower->setUniformValue(s.narrowerSrc,lastWritten);
    s.narrower->setUniformValue(s.narrowerTop,eightBit?255.f:65535.f);
    gl->glActiveTexture(GL_TEXTURE0+lastWritten);gl->glBindTexture(GL_TEXTURE_2D,s.tex[lastWritten].id);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.tex[Pixels::IntegerResult].id,0);
    gl->glViewport(0,0,lastSize.width(),lastSize.height());
    gl->glDrawArrays(GL_TRIANGLES,0,3);
  } else gl->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.tex[lastWritten].id,0);
  // Through a pixel buffer it came back no quicker (2.36 against 2.20 ms).
  gl->glReadPixels(0,0,lastSize.width(),lastSize.height(),integersBack?GL_RGBA_INTEGER:GL_RGBA,uploadType,result.bits());
  s.shader->release();
  // The chain's question. An error anywhere in it, an incomplete framebuffer
  // included, and the result is not used: what comes back is kept in the
  // caches the processor reads too. Every error is cleared, so the next chain
  // starts clean.
  unasked.asked=true;
  if(gl->glGetError()!=GL_NO_ERROR) {
    for(int i=0;i<16&&gl->glGetError()!=GL_NO_ERROR;++i){}
    s.status="GPU pass chain failed; CPU fallback";return {};
  }
  completed+=passes.size();
  return result.convertToFormat(source.format());
}

// Effects on the card for every thread, not only the GUI thread.
//
// A frame drawn in the background -- the live preview when it is busy, the
// pre-render -- runs its effects on worker threads, and those had no card: a
// context and its offscreen surface cannot be made there (on Windows the surface
// is a window, and making one off the GUI thread crashed). So one thread of its
// own owns a context whose handles were made on the GUI thread, and a worker
// hands its chain over and waits. The card runs them one after another, which
// it does far faster than a frame's worth of them arrives.
QImage gpuRunPasses(const QImage &source, const QVector<GpuPass> &passes) {
  if(source.isNull() || passes.isEmpty())return {};
  if(gpuAvailableHere()) return runPassesHere(source,passes,pixels());
  if(!enabled || suppressPixels || !gpuService.ready.load() || !qApp)return {};
  if(qApp->arguments().contains("--self-test") || qApp->arguments().contains("--precision-self-test") ||
     qApp->arguments().contains("--surface-effects-self-test"))return {};
  QImage result;
  QMetaObject::invokeMethod(gpuService.dispatcher,[&]{result=runPassesHere(source,passes,*gpuService.pixels);},
                            Qt::BlockingQueuedConnection);
  return result;
}
// ---------------------------------------------------------------------------
// The exact distance transform, on the card.
//
// Felzenszwalb and Huttenlocher's lower envelope of parabolas, once along every
// row and then down every column, each row or column one invocation -- the CPU
// runs the very same loops, a row or a band of columns per worker. The
// envelope's intersections are computed in the same float arithmetic (marked
// precise, so nothing is fused into a different rounding), and the squared
// distances that come out are whole numbers, or the 1e12 that stands for
// "none": the same numbers the CPU's field holds before its square root.
// ---------------------------------------------------------------------------
namespace {
std::atomic<quint64> distanceTransforms{0};
QMutex distanceProblemGuard;
QString distanceProblem;
void noteDistanceProblem(const QString &why) { QMutexLocker lock(&distanceProblemGuard); distanceProblem = why; }
const char *kEdtCommon = R"(#version 430
layout(local_size_x = 64) in;
uniform int width, height, first, count, wantSites;
layout(std430, binding = 3) buffer Envelope { int v[]; };
layout(std430, binding = 4) buffer Bounds { float z[]; };
)";
const char *kEdtRows = R"(
layout(std430, binding = 0) readonly buffer Ids { int ids[]; };
layout(std430, binding = 1) writeonly buffer RowSquares { float rowSq[]; };
layout(std430, binding = 2) writeonly buffer RowSites { int rowSite[]; };
float f(int x, int y) { return ids[y * width + x] != 0 ? 0.0 : 1e12; }
void main() {
  int local = int(gl_GlobalInvocationID.x);
  if (local >= count) return;
  int y = first + local;
  int vb = local * width, zb = local * (width + 1);
  int k = 0;
  v[vb] = 0; z[zb] = -1e20; z[zb + 1] = 1e20;
  for (int q = 1; q < width; ++q) {
    precise float fq = f(q, y) + float(q) * float(q);
    precise float s;
    while (true) {
      int r = v[vb + k];
      precise float fr = f(r, y) + float(r) * float(r);
      s = (fq - fr) / (2.0 * float(q - r));
      if (s > z[zb + k] || k == 0) break;
      --k;
    }
    ++k; v[vb + k] = q; z[zb + k] = s; z[zb + k + 1] = 1e20;
  }
  k = 0;
  for (int q = 0; q < width; ++q) {
    while (z[zb + k + 1] < float(q)) ++k;
    int r = v[vb + k];
    precise float d = float(q - r);
    precise float value = d * d + f(r, y);
    rowSq[y * width + q] = value;
    if (wantSites != 0) rowSite[y * width + q] = r;
  }
})";
const char *kEdtColumns = R"(
layout(std430, binding = 0) readonly buffer Ids { int ids[]; };
layout(std430, binding = 1) readonly buffer RowSquares { float rowSq[]; };
layout(std430, binding = 2) readonly buffer RowSites { int rowSite[]; };
layout(std430, binding = 5) writeonly buffer Squares { float squared[]; };
layout(std430, binding = 6) writeonly buffer Nearest { int nearest[]; };
float g(int x, int y) { return rowSq[y * width + x]; }
void main() {
  int local = int(gl_GlobalInvocationID.x);
  if (local >= count) return;
  int x = first + local;
  int vb = local * height, zb = local * (height + 1);
  int k = 0;
  v[vb] = 0; z[zb] = -1e20; z[zb + 1] = 1e20;
  for (int q = 1; q < height; ++q) {
    precise float fq = g(x, q) + float(q) * float(q);
    precise float s;
    while (true) {
      int r = v[vb + k];
      precise float fr = g(x, r) + float(r) * float(r);
      s = (fq - fr) / (2.0 * float(q - r));
      if (s > z[zb + k] || k == 0) break;
      --k;
    }
    ++k; v[vb + k] = q; z[zb + k] = s; z[zb + k + 1] = 1e20;
  }
  k = 0;
  for (int q = 0; q < height; ++q) {
    while (z[zb + k + 1] < float(q)) ++k;
    int r = v[vb + k];
    precise float d = float(q - r);
    precise float value = d * d + g(x, r);
    squared[q * width + x] = value;
    if (wantSites != 0) nearest[q * width + x] = value < 1e11 ? ids[r * width + rowSite[r * width + x]] : 0;
  }
})";
bool runDistanceHere(const std::vector<qint32> &ids, int width, int height, std::vector<float> &squared,
                     std::vector<qint32> *nearest, Pixels &s) {
  auto *previous=QOpenGLContext::currentContext();QSurface *oldSurface=previous?previous->surface():nullptr;
  struct Restore{QOpenGLContext *context;QSurface *surface;~Restore(){if(context&&surface)context->makeCurrent(surface);else if(QOpenGLContext::currentContext())QOpenGLContext::currentContext()->doneCurrent();}} restore{previous,oldSurface};
  if(!s.init() || !s.context.makeCurrent(&s.surface)) { noteDistanceProblem("no card context"); return false; }
  auto *gl=s.context.extraFunctions();
  // One question to the driver, after the readback, as for a chain of passes
  // (runPassesHere): errors are cleared first only when something may have
  // left one, and the uniforms are set where the programs said they were.
  if(s.errorsUnasked){for(int i=0;i<16&&gl->glGetError()!=GL_NO_ERROR;++i){}s.errorsUnasked=false;}
  struct Unasked{bool &flag;bool asked=false;~Unasked(){if(!asked)flag=true;}} unasked{s.errorsUnasked};
  if(!s.edtTried) {
    s.edtTried=true;
    // Compute shaders are OpenGL 4.3; a 3.3 context from a driver that has
    // more is usually given what it has.
    const auto version=s.context.format().version();
    if(version<qMakePair(4,3) || !QOpenGLShader::hasOpenGLShaders(QOpenGLShader::Compute,&s.context)) {
      noteDistanceProblem(QString("OpenGL %1.%2 has no compute shaders").arg(version.first).arg(version.second));
      return false;
    }
    auto build=[&](const char *body) {
      auto program=std::make_unique<QOpenGLShaderProgram>();
      const QByteArray source=QByteArray(kEdtCommon)+body;
      if(!program->addShaderFromSourceCode(QOpenGLShader::Compute,source) || !program->link()) {
        noteDistanceProblem("distance shader: "+program->log());
        return std::unique_ptr<QOpenGLShaderProgram>();
      }
      return program;
    };
    s.edtRows=build(kEdtRows);
    s.edtColumns=build(kEdtColumns);
    if(!s.edtRows || !s.edtColumns)return false;
    auto locate=[](QOpenGLShaderProgram &program) {
      return Pixels::EdtUniforms{program.uniformLocation("width"),program.uniformLocation("height"),
                                 program.uniformLocation("first"),program.uniformLocation("count"),
                                 program.uniformLocation("wantSites")};
    };
    s.edtRowsAt=locate(*s.edtRows);
    s.edtColumnsAt=locate(*s.edtColumns);
    gl->glGenBuffers(7,s.edtBuffers);
    s.edtOk=true;
  }
  if(!s.edtOk)return false;
  const qint64 pixels=qint64(width)*height;
  const bool sites=nearest!=nullptr;
  // The scratch of the envelopes, a batch of rows or columns at a time.
  const qint64 scratchBudget=64ll<<20;
  const int rowBatch=int(std::clamp<qint64>(scratchBudget/(qint64(width+1)*8),64,height));
  const int columnBatch=int(std::clamp<qint64>(scratchBudget/(qint64(height+1)*8),64,width));
  const qint64 vCount=std::max(qint64(rowBatch)*width,qint64(columnBatch)*height);
  const qint64 zCount=std::max(qint64(rowBatch)*(width+1),qint64(columnBatch)*(height+1));
  const qint64 bytes[7]={pixels*4,pixels*4,sites?pixels*4:4,vCount*4,zCount*4,pixels*4,sites?pixels*4:4};
  qint64 total=0;for(qint64 b:bytes)total+=b;
  // A grid that would take more than this of the card's memory is left to the
  // processor rather than risk the card running out under everything else on it.
  if(total>(768ll<<20)) { noteDistanceProblem("grid too large for the card's share"); return false; }
  for(int i=0;i<7;++i) {
    gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER,s.edtBuffers[i]);
    if(i==0) gl->glBufferData(GL_SHADER_STORAGE_BUFFER,bytes[0],ids.data(),GL_DYNAMIC_DRAW);
    else if(s.edtBytes[i]<bytes[i]) { gl->glBufferData(GL_SHADER_STORAGE_BUFFER,bytes[i],nullptr,GL_DYNAMIC_COPY); s.edtBytes[i]=bytes[i]; }
    gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER,GLuint(i),s.edtBuffers[i]);
  }
  s.edtBytes[0]=bytes[0];
  auto run=[&](QOpenGLShaderProgram &program,const Pixels::EdtUniforms &at,int lines,int batch) {
    program.bind();
    program.setUniformValue(at.width,width);
    program.setUniformValue(at.height,height);
    program.setUniformValue(at.wantSites,sites?1:0);
    for(int first=0;first<lines;first+=batch) {
      const int count=std::min(batch,lines-first);
      program.setUniformValue(at.first,first);
      program.setUniformValue(at.count,count);
      gl->glDispatchCompute(GLuint((count+63)/64),1,1);
      // The next batch reuses the scratch, and the columns read the rows.
      gl->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    program.release();
  };
  run(*s.edtRows,s.edtRowsAt,height,rowBatch);
  run(*s.edtColumns,s.edtColumnsAt,width,columnBatch);
  gl->glMemoryBarrier(GL_ALL_BARRIER_BITS);
  auto read=[&](GLuint buffer,void *into) {
    gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffer);
    const void *mapped=gl->glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,pixels*4,GL_MAP_READ_BIT);
    if(!mapped)return false;
    std::memcpy(into,mapped,size_t(pixels)*4);
    gl->glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    return true;
  };
  squared.resize(size_t(pixels));
  if(!read(s.edtBuffers[5],squared.data()))return false;
  if(sites) {
    nearest->resize(size_t(pixels));
    if(!read(s.edtBuffers[6],nearest->data()))return false;
  }
  gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER,0);
  unasked.asked=true;
  if(const GLenum error=gl->glGetError(); error!=GL_NO_ERROR) {
    for(int i=0;i<16&&gl->glGetError()!=GL_NO_ERROR;++i){}
    noteDistanceProblem(QString("GL error %1").arg(error)); return false;
  }
  noteDistanceProblem({});
  ++distanceTransforms;
  return true;
}
}
quint64 gpuDistanceTransforms() { return distanceTransforms.load(); }
QString gpuDistanceProblem() { QMutexLocker lock(&distanceProblemGuard); return distanceProblem; }
bool gpuDistanceTransform(const std::vector<qint32> &ids, int width, int height, std::vector<float> &squared,
                          std::vector<qint32> *nearest) {
  if(width<=0 || height<=0 || qint64(ids.size())!=qint64(width)*height)return false;
  if(gpuAvailableHere()) return runDistanceHere(ids,width,height,squared,nearest,pixels());
  if(!enabled || suppressPixels || !gpuService.ready.load() || !qApp)return false;
  if(qApp->arguments().contains("--self-test") || qApp->arguments().contains("--precision-self-test") ||
     qApp->arguments().contains("--surface-effects-self-test"))return false;
  bool done=false;
  QMetaObject::invokeMethod(gpuService.dispatcher,[&]{done=runDistanceHere(ids,width,height,squared,nearest,*gpuService.pixels);},
                            Qt::BlockingQueuedConnection);
  return done;
}
void startGpuPixelService() {
  if(gpuService.ready.load() || !enabled || !qApp || QThread::currentThread()!=qApp->thread())return;
  if(qApp->arguments().contains("--self-test") || qApp->arguments().contains("--precision-self-test") ||
     qApp->arguments().contains("--surface-effects-self-test"))return;
  auto *px=new Pixels;
  if(!px->createHandles()){delete px;return;}
  auto *thread=new QThread;thread->setObjectName("gpu-effects");
  auto *dispatcher=new QObject;dispatcher->moveToThread(thread);
  px->context.moveToThread(thread);
  thread->start();
  bool ok=false;
  QMetaObject::invokeMethod(dispatcher,[&]{ok=px->init();},Qt::BlockingQueuedConnection);
  if(!ok) { // no usable card: leave the workers on the CPU
    QMetaObject::invokeMethod(dispatcher,[&]{px->release();px->context.moveToThread(qApp->thread());},Qt::BlockingQueuedConnection);
    thread->quit();thread->wait();delete dispatcher;delete thread;delete px;return;
  }
  gpuService.thread=thread;gpuService.dispatcher=dispatcher;gpuService.pixels=px;gpuService.ready=true;
}
void stopGpuPixelService() {
  if(!gpuService.ready.exchange(false))return;
  QMetaObject::invokeMethod(gpuService.dispatcher,[&]{gpuService.pixels->release();gpuService.pixels->context.moveToThread(qApp->thread());},
                            Qt::BlockingQueuedConnection);
  gpuService.thread->quit();gpuService.thread->wait();
  delete gpuService.dispatcher;delete gpuService.thread;delete gpuService.pixels;
  gpuService.dispatcher=nullptr;gpuService.thread=nullptr;gpuService.pixels=nullptr;
}
QImage gpuPixelOperation(const QImage &source,int op,const QVector4D &params,const QVector4D &a,const QVector4D &b,const QImage &aux) {
  if(op==2) { // six ping-pong passes between one upload and one readback
    if(params.x()>32)return gpuRunPasses(source,gpuBlurPasses(source.size(),double(params.x())*2));
    QVector<GpuPass> chain;
    for(int n=0;n<6;++n) {
      GpuPass step;
      step.op=2;step.params=QVector4D(params.x(),n%2?0.f:1.f,n%2?1.f:0.f,0);
      step.input=n?GpuPass::Previous:GpuPass::Current;
      chain.push_back(step);
    }
    return gpuRunPasses(source,chain);
  }
  if(op==5) // the halo an op 3 needs prepared for it, prepared here instead
    return gpuRunPasses(source,gpuHaloPasses(source.size(),b.x(),a,
                                             QPointF(params.x(),params.y())));
  GpuPass pass;
  pass.op=op;pass.params=params;pass.a=a;pass.b=b;
  pass.useAux=!aux.isNull();pass.auxImage=aux;
  return gpuRunPasses(source,{pass});
}
// Keep full resolution. Wide boxes use parallel prefix sums instead of
// thousands of texture fetches per pixel or a reduced-resolution image.
static QVector<GpuPass> fullSizeBlurPasses(QSize size, double radius) {
  const int r=std::clamp(int(std::ceil(radius/2)),1,4096);
  QVector<GpuPass> chain;
  auto append=[&](int op,int amount,int axis) {
    GpuPass pass;pass.op=op;pass.size=size;
    pass.params=QVector4D(float(amount),axis==0?1.f:0.f,axis==1?1.f:0.f,0);
    pass.input=chain.isEmpty()?GpuPass::Current:GpuPass::Previous;
    chain.push_back(pass);
  };
  for(int n=0;n<6;++n) {
    const int axis=n%2;
    if(r<=32) append(2,r,axis);
    else {
      const int length=axis?size.height():size.width();
      for(int stride=1;stride<length;stride*=2) append(16,stride,axis);
      append(17,r,axis);
    }
  }
  return chain;
}
// The resolution a blur is resolved at on the card, and the reason it matters
// more here than on the CPU. A box wider than 32 cannot be walked with a
// running sum in a shader, so it falls back to parallel prefix sums: log2(width)
// full-resolution passes per axis, six axis passes -- a single glow on a wide
// raster was running seventy-eight passes over every pixel of it. Reduced by
// r/12 the box is narrow again and the cheap six-pass path applies, on an image
// k*k times smaller. Same rule and same twelve-sample floor as soften() in
// precision.cpp, because a box of half-width r carries no detail finer than r.
//
// gpuBlurPasses stays exact: it is the reference the GPU blur is tested against,
// and the CPU calls it only after doing its own reduction.
QVector<GpuPass> gpuSoftenPasses(QSize size, double radius) {
  const int r=std::clamp(int(std::ceil(radius/2)),1,4096);
  // Same floor as the CPU: the reduced image stays at least 64 pixels on its
  // shorter side, so the interpolation back never has to stretch a handful of
  // samples across the whole picture.
  const int room=std::min(size.width(),size.height())/64;
  const int k=std::clamp(std::min(r/12,room),1,16);
  if(k<=1)return fullSizeBlurPasses(size,radius);
  const QSize small(std::max(1,size.width()/k),std::max(1,size.height()/k));
  QVector<GpuPass> chain;
  GpuPass down;down.op=13;down.params=QVector4D(float(k),0,0,0);
  down.size=small;down.input=GpuPass::Current;
  chain.push_back(down);
  auto inner=fullSizeBlurPasses(small,radius/k);
  if(!inner.isEmpty())inner.front().input=GpuPass::Previous;
  chain+=inner;
  GpuPass up;up.op=7;up.size=size;
  up.params=QVector4D(float(size.width()),float(size.height()),0,0);
  up.input=GpuPass::Previous;
  chain.push_back(up);
  return chain;
}
QVector<GpuPass> gpuHaloPasses(QSize size, double blurRadius, const QVector4D &colour, QPointF offset) {
  auto chain=gpuSoftenPasses(size,blurRadius);
  GpuPass over;over.op=6;
  over.params=QVector4D(float(offset.x()),float(offset.y()),0,0);
  over.a=colour;
  over.input=GpuPass::Current;over.useAux=true;over.auxFrom=GpuPass::Previous;over.auxSmooth=true;
  over.size=size;over.endsEffect=true;
  chain.push_back(over);
  return chain;
}
QVector<GpuPass> gpuBlurPasses(QSize size, double blurRadius) {
  auto chain=fullSizeBlurPasses(size,blurRadius);
  chain.back().endsEffect=true;
  return chain;
}
QVector<GpuPass> gpuAlphaPass(bool toPremultiplied) {
  GpuPass convert;convert.op=toPremultiplied?8:9;
  convert.input=GpuPass::Current;convert.endsEffect=true;
  return {convert};
}
// The stroke, measured on the card. Jump flooding carries the nearest seed
// coordinate outward in halving steps, so both distance fields fall out of one
// chain of full-resolution passes -- and because they are measured from the
// image as it stands at that point, the stroke can sit anywhere in the order
// rather than only ahead of everything that touches alpha.
QVector<GpuPass> gpuStrokePasses(QSize size, double width, int mode, QColor colour) {
  if(width<=0 || colour.alpha()==0)return {};
  QVector<GpuPass> chain;
  GpuPass seed;seed.op=10;seed.input=GpuPass::Current;seed.size=size;
  chain.push_back(seed);
  for(int step=std::max(size.width(),size.height())/2;step>=1;step/=2) {
    GpuPass jump;jump.op=11;jump.params=QVector4D(float(step),0,0,0);
    jump.input=GpuPass::Previous;jump.size=size;
    chain.push_back(jump);
  }
  GpuPass resolve;resolve.op=12;resolve.input=GpuPass::Previous;resolve.size=size;
  chain.push_back(resolve);
  GpuPass blend;blend.op=4;
  blend.params=QVector4D(mode==2?float(width/2):float(width),float(mode),0,0);
  blend.a=QVector4D(float(colour.redF()),float(colour.greenF()),float(colour.blueF()),
                    float(colour.alphaF()));
  blend.input=GpuPass::Current;blend.useAux=true;blend.auxFrom=GpuPass::Previous;
  blend.size=size;blend.endsEffect=true;
  chain.push_back(blend);
  return chain;
}
QVector<GpuPass> gpuHaloPasses(QSize size, double blurRadius, QColor color, QPointF offset) {
  auto chain=gpuHaloPasses(size,blurRadius,QVector4D(float(color.redF()),float(color.greenF()),
                                                 float(color.blueF()),float(color.alphaF())),offset);
  if(!color.isValid() && !chain.isEmpty())chain.back().params.setZ(1);
  return chain;
}
QImage gpuHaloBehind(const QImage &source,double blurRadius,QColor color,QPointF offset) {
  return gpuRunPasses(source,gpuHaloPasses(source.size(),blurRadius,color,offset));
}
int checkGpuPixels(QStringList &failures){
  {
    // Choosing between the processor and the card by what each took: the
    // processor first, then the card, then the quicker one, the other again
    // now and then; a card that could not is left alone for a while; each size
    // learns for itself.
    RouteChoice route("checks");
    const qint64 size=2'000'000;
    QString wrong;
    for(int k=0;k<2;++k) { if(route.card(size)) wrong+=" processor not timed first"; route.took(size,false,10); }
    for(int k=0;k<2;++k) { if(!route.card(size)) wrong+=" card not timed next"; route.took(size,true,4); }
    int onCard=0;
    for(int k=0;k<96;++k) onCard+=route.card(size);
    if(onCard<90 || onCard==96) wrong+=QString(" the quicker card took %1 of 96, the processor never tried again").arg(onCard);
    route.took(size,true,40); route.took(size,true,40); route.took(size,true,40); route.took(size,true,40);
    int afterSlow=0;
    for(int k=0;k<96;++k) afterSlow+=route.card(size);
    if(afterSlow>6) wrong+=QString(" a card grown slower still took %1 of 96").arg(afterSlow);
    if(route.card(64'000)) wrong+=" a smaller size went to the card untimed";
    RouteChoice refusing("checks");
    for(int k=0;k<2;++k) { refusing.card(size); refusing.took(size,false,10); }
    refusing.refused(size);
    if(refusing.card(size)) wrong+=" a card that could not was asked again at once";
    if(!wrong.isEmpty()) failures<<"Choosing processor or card:"+wrong;
  }
  {
    // The distance transform on the card gives the processor's numbers exactly:
    // letters, discs, a turned box and a thin line, a raster wider than 4096
    // (where a column's square no longer fits a float's whole numbers), and
    // the nearest letter of every pixel. Each worked out on the card, not
    // handed back.
    auto canvas=[](QSize size,const std::function<void(QPainter &)> &paint) {
      QImage image(size,QImage::Format_RGBA32FPx4);
      image.fill(Qt::transparent);
      QPainter p(&image);
      p.setRenderHint(QPainter::Antialiasing);
      paint(p);
      return image;
    };
    const QImage letters=canvas(QSize(1203,407),[](QPainter &p) {
      QFont font("Arial");font.setPixelSize(260);font.setBold(true);
      p.setFont(font);p.setPen(Qt::white);p.drawText(QRect(0,0,1203,407),Qt::AlignCenter,"WAVY");
      p.setBrush(Qt::white);p.setPen(Qt::NoPen);
      p.drawEllipse(QPointF(60,60),41.3,33.7);
      p.save();p.translate(1100,320);p.rotate(23);p.drawRect(QRectF(-50,-30,100,60));p.restore();
      p.drawRect(QRectF(20,390,900,1.4));
    });
    const QImage wide=canvas(QSize(5003,61),[](QPainter &p) {
      p.setBrush(Qt::white);p.setPen(Qt::NoPen);
      p.drawEllipse(QPointF(7,30),5,5);p.drawEllipse(QPointF(4990,12),9,7);p.drawRect(QRectF(2400,20,300,20));
    });
    for(const auto &[label,image]:std::vector<std::pair<QString,QImage>>{{"letters",letters},{"wider than 4096",wide}}) {
      for(bool inside:{true,false}) {
        const quint64 before=gpuDistanceTransforms();
        std::vector<float> card;
        { RouteChoice::Force onCard(true); card=strokeDistanceFieldOf(image,inside); }
        const bool onCard=gpuDistanceTransforms()>before;
        std::vector<float> processor;
        { CpuPixelScope cpu; RouteChoice::Force onProcessor(false); processor=strokeDistanceFieldOf(image,inside); }
        double worst=0;
        for(size_t i=0;i<processor.size() && i<card.size();++i) worst=std::max(worst,double(std::abs(card[i]-processor[i])));
        if(!onCard || card.size()!=processor.size() || worst>0)
          failures<<QString("Distance field on the card, %1 (%2): %3, off by %4")
                        .arg(label,inside?"to the letters":"to the space around them",onCard?"worked there":"not worked there: "+gpuDistanceProblem()).arg(worst);
      }
    }
    // The nearest letter: four labels, one of them on no pixel at all.
    std::vector<int> labels(size_t(letters.width())*letters.height(),0);
    for(int y=0;y<letters.height();++y)
      for(int x=0;x<letters.width();++x) labels[size_t(y)*letters.width()+x]=x<300?1:x<600?2:x<900?4:0;
    const quint64 before=gpuDistanceTransforms();
    std::vector<int> card;
    { RouteChoice::Force onCard(true); card=nearestLetterOf(letters,labels); }
    const bool onCard=gpuDistanceTransforms()>before;
    std::vector<int> processor;
    { CpuPixelScope cpu; RouteChoice::Force onProcessor(false); processor=nearestLetterOf(letters,labels); }
    qint64 differ=0;
    for(size_t i=0;i<processor.size() && i<card.size();++i) differ+=card[i]!=processor[i];
    // Where two letters are exactly as near, either is right.
    if(!onCard || card.size()!=processor.size() || differ>qint64(processor.size()/2000))
      failures<<QString("Nearest letter on the card: %1, %2 pixels differ").arg(onCard?"worked there":"not worked there").arg(differ);
  }
  {
    QImage red(65,65,QImage::Format_RGBA32FPx4_Premultiplied);red.fill(Qt::transparent);
    {QPainter p(&red);p.fillRect(25,25,15,15,Qt::red);}
    auto out=gpuHaloBehind(red,8,QColor(),{});
    if(!out.isNull()) {
      auto halo=out.pixelColor(23,32);
      if(halo.alphaF()<=0 || halo.redF()<.99 || halo.greenF()>.01 || halo.blueF()>.01)
        failures<<"GPU Glow follows input RGB";
    }
  }
  QImage image(32,32,QImage::Format_RGBA32FPx4);image.fill(Qt::transparent);
  auto *p=reinterpret_cast<float *>(image.scanLine(4))+3*4;p[0]=2.5f;p[1]=.25f;p[3]=.5f;
  auto out=gpuPixelOperation(image,0,{-1,0,0,0});if(out.isNull())return 77;
  {
    // An eight or sixteen bit picture crosses in its own depth both ways where
    // the card's own passes give the driver's values (Pixels::calibrateBytes,
    // Pixels::calibrateShorts). Whatever this card was found to do, a chain
    // must come back with the values of the float route: each chain runs again
    // with both crossings left to the driver, and the two are compared byte
    // for byte -- a halo (blur, tint, over), a blur, a premultiply undone and
    // redone, and a pack body whose sums land on halves of a step.
    auto &s=pixels();
    for(const bool sixteen:{false,true}) {
      QImage patch(203,117,sixteen?QImage::Format_RGBA64_Premultiplied:QImage::Format_RGBA8888_Premultiplied);
      for(int y=0;y<patch.height();++y) {
        uchar *row=patch.scanLine(y);
        auto *shorts=reinterpret_cast<quint16 *>(row);
        for(int x=0;x<patch.width();++x) {
          if(sixteen) { // every bit of the shorts in play, not bytes times 257
            const int alpha=(x*1793+y*3329)&65535;
            for(int c=0;c<3;++c)shorts[x*4+c]=quint16(std::min(alpha,(x*(c+3)*263+y*(5-c)*257+c*4099)&65535));
            shorts[x*4+3]=quint16(alpha);
          } else {
            const int alpha=(x*7+y*13)&255;
            for(int c=0;c<3;++c)row[x*4+c]=uchar(std::min(alpha,(x*(c+3)+y*(5-c))&255));
            row[x*4+3]=uchar(alpha);
          }
        }
      }
      GpuPass halves;halves.endsEffect=true;
      halves.source=QString("vec4 wavyEffect(vec4 c,vec2 p,vec2 s){return vec4(c.rgb*0.5+vec3(0.5/%1.0),c.a);}").arg(sixteen?65535:255);
      const QVector<QVector<GpuPass>> chains={gpuHaloPasses(patch.size(),9,QColor(40,200,90),QPointF(3,-2)),
                                              gpuBlurPasses(patch.size(),5),gpuAlphaPass(false)+gpuAlphaPass(true),{halves}};
      bool &up=sixteen?s.shortsUpExact:s.bytesUpExact,&back=sixteen?s.shortsBackExact:s.bytesBackExact;
      QStringList wrong;
      for(int k=0;k<chains.size();++k) {
        const QImage integers=gpuRunPasses(patch,chains[k]);
        const bool wasUp=up,wasBack=back;
        up=back=false;
        const QImage floats=gpuRunPasses(patch,chains[k]);
        up=wasUp;back=wasBack;
        if(integers.isNull() || floats.isNull() || integers.size()!=floats.size() || integers.format()!=floats.format()) {
          wrong<<QString("chain %1 did not run").arg(k);continue;
        }
        qint64 differ=0;
        const int rowBytes=integers.width()*integers.depth()/8;
        for(int y=0;y<integers.height();++y)
          for(int x=0;x<rowBytes;++x)differ+=integers.constScanLine(y)[x]!=floats.constScanLine(y)[x];
        if(differ)wrong<<QString("chain %1: %2 bytes differ").arg(k).arg(differ);
      }
      if(!wrong.isEmpty())
        failures<<QString(sixteen?"Sixteen bit chains come back with the float route's shorts: "
                                 :"Eight bit chains come back with the float route's bytes: ")+wrong.join(", ");
    }
  }
  // Wide full-resolution kernels, non-power-of-two sizes and HDR/alpha.
  // This bypasses compositor caching so both backends really execute.
  for (double radius : {66., 100., 512.}) {
    QImage sample(131,97,QImage::Format_RGBA32FPx4_Premultiplied);
    sample.fill(Qt::transparent);
    for(int y=11;y<79;++y) {
      auto *row=reinterpret_cast<float *>(sample.scanLine(y));
      for(int x=9;x<111;++x) {
        row[x*4]=1.4f;row[x*4+1]=.2f;row[x*4+2]=.1f;row[x*4+3]=.7f;
      }
    }
    const auto expected=fullResolutionBlur(sample,radius);
    const auto actual=gpuRunPasses(sample,gpuBlurPasses(sample.size(),radius));
    double error=0;
    if(actual.isNull() || actual.size()!=sample.size()) {
      failures<<"Full-resolution wide GPU blur unavailable";continue;
    }
    for(int y=0;y<sample.height();++y) {
      const auto *a=reinterpret_cast<const float *>(actual.constScanLine(y));
      const auto *b=reinterpret_cast<const float *>(expected.constScanLine(y));
      for(int x=0;x<sample.width()*4;++x)error=std::max(error,double(std::abs(a[x]-b[x])));
    }
    if(error>.0002)failures<<QString("Wide GPU blur differs from CPU: radius %1 error %2").arg(radius).arg(error);
  }
  {
    // The same blur, resolved at the resolution it can express, on a raster
    // shaped like a real one: the blur is a fraction of the picture rather than
    // most of it. It may differ from the exact blur, but only by the amount the
    // reduction costs.
    QImage sample(512,384,QImage::Format_RGBA32FPx4_Premultiplied);
    sample.fill(Qt::transparent);
    for(int y=70;y<300;++y) {
      auto *row=reinterpret_cast<float *>(sample.scanLine(y));
      for(int x=60;x<430;++x) { row[x*4]=1.4f;row[x*4+1]=.2f;row[x*4+2]=.1f;row[x*4+3]=.7f; }
    }
    const auto expected=fullResolutionBlur(sample,100);
    const auto softened=gpuRunPasses(sample,gpuSoftenPasses(sample.size(),100));
    if(softened.isNull() || softened.size()!=sample.size())
      failures<<"Reduced-resolution GPU blur unavailable";
    else {
      double worst=0,total=0;qint64 count=0;
      for(int y=0;y<sample.height();++y) {
        const auto *a=reinterpret_cast<const float *>(softened.constScanLine(y));
        const auto *b=reinterpret_cast<const float *>(expected.constScanLine(y));
        for(int x=0;x<sample.width()*4;++x) {
          const double d=std::abs(double(a[x])-double(b[x]));
          worst=std::max(worst,d);total+=d;++count;
        }
      }
      // Measured at 0.007 mean and 0.040 worst against the exact blur -- a hard
      // synthetic edge, which is where a reduction costs most. What that comes
      // to on real frames is in gpu-self-test: Glow compares at 0.022/255.
      if(total/std::max<qint64>(1,count)>.009 || worst>.045)
        failures<<QString("Reduced GPU blur stays on the same picture: mean %1 worst %2")
                    .arg(total/std::max<qint64>(1,count)).arg(worst);
    }
  }
  auto *q=reinterpret_cast<const float *>(out.constScanLine(4))+3*4;
  if(std::abs(q[0]-1.25f)>.0001f||q[3]!=.5f)failures<<"GPU HDR exposure and alpha";
  if(out.pixelColor(3,27).alpha()!=0)failures<<"GPU pixel orientation";
  // Reuse existing texture storage, then resize it and return to the original
  // size. Slider edits and switching preview quality exercise both paths.
  for(QSize size : {QSize(32,32),QSize(17,19),QSize(32,32)}) {
    QImage sample(size,QImage::Format_RGBA32FPx4);sample.fill(QColor(128,64,32,255));
    auto result=gpuPixelOperation(sample,0,{0,0,0,0});
    if(result.isNull() || result.size()!=size || result.pixelColor(0,0)!=sample.pixelColor(0,0))
      failures<<"GPU texture reuse/resize";
  }
  auto blur=gpuPixelOperation(image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied),2,{1,0,0,0});
  if(blur.isNull())failures<<"GPU six-pass blur";
  else {
    // Three radius-one box passes per axis: central coefficient (7/27)^2.
    const auto *center=reinterpret_cast<const float *>(blur.constScanLine(4))+3*4;
    if(std::abs(center[3]-.5f*49.f/729.f)>.00001f)failures<<"GPU blur ping-pong result";
  }
  auto premul=image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  QVector4D color(.2f,.5f,.7f,.8f);
  auto separate=gpuPixelOperation(premul,3,{1,2,0,0},color,{},blur);
  auto fused=gpuPixelOperation(premul,5,{1,2,0,0},color,{1,0,0,0});
  if(separate.isNull()||fused.isNull())failures<<"GPU fused halo unavailable";
  else {
    float error=0;
    for(int y=0;y<fused.height();++y){auto *a=reinterpret_cast<const float *>(fused.constScanLine(y));
      auto *b=reinterpret_cast<const float *>(separate.constScanLine(y));
      for(int x=0;x<fused.width()*4;++x)error=std::max(error,std::abs(a[x]-b[x]));}
    if(error>.00001f)failures<<"GPU fused halo matches separate passes";
  }
  {
    // The halo pipeline, which never leaves the card between its passes.
    QImage layer(64,64,QImage::Format_RGBA32FPx4_Premultiplied);
    layer.fill(Qt::transparent);
    {QPainter p(&layer);p.fillRect(QRect(24,24,16,16),Qt::white);}
    auto halo=gpuHaloBehind(layer,12,QColor(255,0,0),QPointF());
    if(halo.isNull())failures<<"GPU halo unavailable while GPU pixels work";
    else {
      if(halo.pixelColor(32,32)!=QColor(255,255,255))failures<<"GPU halo keeps the layer on top of it";
      const auto beside=halo.pixelColor(18,32);
      if(beside.alpha()==0||beside.red()<=beside.blue())failures<<"GPU halo spreads its own colour outside the layer";
      if(halo.pixelColor(1,1).alpha()!=0)failures<<"GPU halo stops short of the far corner";
    }
    // A wide radius takes the downsampling path; the reach has to grow with it.
    QImage wide(256,256,QImage::Format_RGBA32FPx4_Premultiplied);
    wide.fill(Qt::transparent);
    {QPainter p(&wide);p.fillRect(QRect(112,112,32,32),Qt::white);}
    auto spread=gpuHaloBehind(wide,120,QColor(0,255,0),QPointF());
    if(spread.isNull())failures<<"GPU halo unavailable for a wide radius";
    else {
      if(spread.pixelColor(128,128)!=QColor(255,255,255))failures<<"wide GPU halo keeps the layer on top of it";
      if(spread.pixelColor(60,128).alpha()==0)failures<<"wide GPU halo reaches as far as its radius";
      if(spread.pixelColor(128,128).alpha()<255)failures<<"wide GPU halo leaves the layer opaque";
    }
    auto shifted=gpuHaloBehind(wide,120,QColor(0,255,0),QPointF(40,0));
    if(!shifted.isNull() && !spread.isNull() &&
        shifted.pixelColor(200,128).alpha()<=spread.pixelColor(200,128).alpha())
      failures<<"a GPU halo offset moves it";
  }
  {
    // Jump flooding measures both distance fields on the card. The coverage
    // these check is the same the CPU stroke is checked for, so the two paths
    // are held to one description of what a stroke is.
    QImage shape(64,64,QImage::Format_RGBA32FPx4);
    shape.fill(Qt::transparent);
    {QPainter p(&shape);p.fillRect(QRect(24,24,16,16),Qt::red);}
    for(int mode=0;mode<3;++mode) {
      const auto stroked=gpuRunPasses(shape,gpuStrokePasses(shape.size(),4,mode,QColor("#00ff00")));
      const QString where=QString(" (mode %1)").arg(mode);
      if(stroked.isNull()){failures<<"GPU stroke unavailable"+where;continue;}
      if((stroked.pixelColor(22,32).alpha()>0)!=(mode!=0))failures<<"GPU stroke outside coverage"+where;
      if((stroked.pixelColor(25,32).green()>0)!=(mode!=1))failures<<"GPU stroke inside coverage"+where;
      if(stroked.pixelColor(32,32).red()<200)failures<<"GPU stroke leaves the middle alone"+where;
      if(stroked.pixelColor(2,2).alpha()!=0)failures<<"GPU stroke stops at its own width"+where;
    }
    if(!gpuStrokePasses(QSize(64,64),0,1,QColor("#00ff00")).isEmpty())
      failures<<"a stroke of no width describes no passes";
    if(!gpuStrokePasses(QSize(64,64),4,1,QColor(0,255,0,0)).isEmpty())
      failures<<"a fully transparent stroke describes no passes";
  }
  {
    // A pass carrying a plugin's own body. What a plugin author relies on is
    // not that this runs at all but that the conventions around it hold: the
    // values land where they were declared, alpha is straight and untouched,
    // and the coordinate points where they think it does. Each is checked on
    // its own, because each is a different way for an effect to come out wrong
    // on the card and right on the processor.
    QImage patch(16,16,QImage::Format_RGBA8888);
    for(int y=0;y<patch.height();++y) {
      auto *row=reinterpret_cast<quint32 *>(patch.scanLine(y));
      for(int x=0;x<patch.width();++x)
        row[x]=(quint32(255-y*15)<<24)|(quint32(y*16)<<16)|(quint32(128)<<8)|quint32(x*16);
    }
    auto run=[&](const char *body,const QVector<float> &values){
      GpuPass pass;pass.source=QString::fromUtf8(body);pass.values=values;pass.endsEffect=true;
      return gpuRunPasses(patch,{pass});
    };
    const auto constant=run("vec4 wavyEffect(vec4 c,vec2 p,vec2 s){"
                            "return vec4(values[0],values[1],values[2],c.a);}",
                            {1.f,.5f,0.f});
    if(constant.isNull()) failures<<"a plugin body runs on the card";
    else {
      const auto shown=constant.pixelColor(5,7);
      if(std::abs(shown.redF()-1)>.01 || std::abs(shown.greenF()-.5)>.01 || shown.blueF()>.01)
        failures<<QString("the declared values reach the shader in order, got %1")
                       .arg(shown.name());
      if(shown.alpha()!=patch.pixelColor(5,7).alpha())
        failures<<"and alpha is the plugin's to keep or change, not the host's to touch";
    }
    // The same maths twice, once here and once there. This is the comparison a
    // plugin has to survive before the host will use its shader, so the
    // conventions it rests on are worth proving here first.
    QImage byHand=patch;
    for(int y=0;y<byHand.height();++y) {
      auto *row=byHand.scanLine(y);
      for(int x=0;x<byHand.width();++x)
        for(int c=0;c<3;++c) row[x*4+c]=uchar(255-row[x*4+c]);
    }
    const auto byCard=run("vec4 wavyEffect(vec4 c,vec2 p,vec2 s){"
                          "return vec4(vec3(1.0)-c.rgb,c.a);}",{});
    if(byCard.isNull() || byCard.size()!=byHand.size()) failures<<"the same maths runs on the card";
    else {
      int worst=0;qint64 total=0;
      for(int y=0;y<byHand.height();++y)
        for(int x=0;x<byHand.width()*4;++x) {
          const int gap=std::abs(int(byHand.constScanLine(y)[x])-int(byCard.constScanLine(y)[x]));
          worst=std::max(worst,gap);total+=gap;
        }
      const double mean=double(total)/(byHand.width()*byHand.height()*4.);
      if(worst>4 || mean>1)
        failures<<QString("and agrees with the processor: mean %1, worst %2 of 255")
                       .arg(mean,0,'f',2).arg(worst);
    }
    // Where the plugin thinks it is. A shader that has this upside down makes
    // a gradient run the wrong way and nothing else complains.
    const auto ramp=run("vec4 wavyEffect(vec4 c,vec2 p,vec2 s){"
                        "return vec4(vec3(p.x/s.x),1.0);}",{});
    if(!ramp.isNull() && ramp.pixelColor(1,8).red()>=ramp.pixelColor(14,8).red())
      failures<<"the coordinate runs left to right across the image";
    // A body that will not compile must stop the chain rather than draw
    // something else, and must say so rather than failing silently.
    GpuPass broken;broken.source="this is not glsl";broken.endsEffect=true;
    if(!gpuRunPasses(patch,{broken}).isNull())
      failures<<"a body that does not compile runs nothing at all";
    if(gpuShaderProblem("this is not glsl").isEmpty())
      failures<<"and says why, so the plugin can be told";
    if(!gpuShaderProblem("vec4 wavyEffect(vec4 c,vec2 p,vec2 s){return c;}").isEmpty())
      failures<<"while a body that does compile reports nothing wrong";
  }
  return failures.isEmpty()?0:1;
}

QString packShaderSource(const QString &body) {
  // The plugin writes a function; everything around it is ours, so it never
  // sees a sampler, a coordinate system or a version pragma.
  return QString(
      "#version 330 core\n"
      "uniform sampler2D src;uniform float values[256];uniform vec2 sourceSize;\n"
      "uniform int processingDepth;\n"
      "uniform float wavySeconds;uniform float wavyFrame;uniform float wavyScale;uniform vec4 wavyBox;\n"
      "uniform int wavyPass;uniform int wavyPasses;\n"
      "uniform sampler2D wavyOriginalTexture;\n"
      "out vec4 result;\n"
      // Any pixel of the picture entering the effect, straight alpha;
      // transparent beyond its edges.
      "vec4 wavyPixel(ivec2 p){if(p.x<0||p.y<0||p.x>=int(sourceSize.x)||p.y>=int(sourceSize.y))return vec4(0.);"
      "return texelFetch(src,p,0);}\n"
      // The same of the picture as it came to the effect's first pass, for a
      // last pass that needs it again (Sharpen, Light Rays).
      "vec4 wavyOriginal(ivec2 p){ivec2 s=textureSize(wavyOriginalTexture,0);"
      "if(p.x<0||p.y<0||p.x>=s.x||p.y>=s.y)return vec4(0.);return texelFetch(wavyOriginalTexture,p,0);}\n"
      "#define anjimEffect wavyEffect\n") + body + QString(
      "\nvoid main(){ivec2 p=ivec2(gl_FragCoord.xy);\n"
      "result=wavyEffect(texelFetch(src,p,0),gl_FragCoord.xy,sourceSize);}\n");
}
