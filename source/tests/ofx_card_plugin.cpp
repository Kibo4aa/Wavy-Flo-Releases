// A filter that draws only on the graphics card, through the OpenGL render
// suite, for --ofx-self-test: built into ofx-test (CMakeLists.txt). It takes
// its source as a texture and draws it over the whole output, every channel
// times Gain (0 to 1) -- a picture drawn on the card, or nothing: it says it
// cannot draw on the processor, and fails a render that is not on the card or
// comes before it was told of the context.
#include "ofxGPURender.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include <cstring>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <GL/gl.h>

static OfxHost *host = nullptr;
static OfxImageEffectSuiteV1 *effects = nullptr;
static OfxPropertySuiteV1 *props = nullptr;
static OfxParameterSuiteV1 *params = nullptr;
static OfxImageEffectOpenGLRenderSuiteV1 *card = nullptr;
static int attached = 0;

static OfxStatus describe(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->getPropertySet(effect, &p);
  props->propSetString(p, kOfxPropLabel, 0, "Wavy-Flo Test Card");
  props->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Wavy-Flo Tests");
  props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  props->propSetString(p, kOfxImageEffectPropOpenGLRenderSupported, 0, "needed");
  props->propSetString(p, kOfxImageEffectPropCPURenderSupported, 0, "false");
  return kOfxStatOK;
}

static OfxStatus describeInContext(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->clipDefine(effect, kOfxImageEffectOutputClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  effects->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  params->paramDefine(set, kOfxParamTypeDouble, "Gain", &p);
  props->propSetDouble(p, kOfxParamPropDefault, 0, .5);
  props->propSetDouble(p, kOfxParamPropMin, 0, 0);
  props->propSetDouble(p, kOfxParamPropMax, 0, 1);
  return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  int enabled = 0;
  OfxTime time = 0;
  props->propGetInt(in, kOfxImageEffectPropOpenGLEnabled, 0, &enabled);
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  if (!enabled || !card || attached <= 0) return kOfxStatFailed;
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  OfxParamHandle gain;
  double g = .5;
  if (params->paramGetHandle(set, "Gain", &gain, nullptr) == kOfxStatOK) params->paramGetValueAtTime(gain, time, &g);
  OfxImageClipHandle source, output;
  effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, nullptr);
  effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, nullptr);
  OfxPropertySetHandle from = nullptr, to = nullptr;
  if (card->clipLoadTexture(source, time, nullptr, nullptr, &from) != kOfxStatOK) return kOfxStatFailed;
  if (card->clipLoadTexture(output, time, nullptr, nullptr, &to) != kOfxStatOK) {
    card->clipFreeTexture(from);
    return kOfxStatFailed;
  }
  int texture = 0, target = 0;
  props->propGetInt(from, kOfxImageEffectPropOpenGLTextureIndex, 0, &texture);
  props->propGetInt(from, kOfxImageEffectPropOpenGLTextureTarget, 0, &target);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, 1, 0, 1, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glEnable(GLenum(target));
  glBindTexture(GLenum(target), GLuint(texture));
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glColor4f(float(g), float(g), float(g), float(g));
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0); glVertex2f(0, 0);
  glTexCoord2f(1, 0); glVertex2f(1, 0);
  glTexCoord2f(1, 1); glVertex2f(1, 1);
  glTexCoord2f(0, 1); glVertex2f(0, 1);
  glEnd();
  glBindTexture(GLenum(target), 0);
  glDisable(GLenum(target));
  card->clipFreeTexture(from);
  card->clipFreeTexture(to);
  return glGetError() == GL_NO_ERROR ? kOfxStatOK : kOfxStatFailed;
}

static OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
  auto effect = (OfxImageEffectHandle)handle;
  if (!std::strcmp(action, kOfxActionLoad)) {
    effects = (OfxImageEffectSuiteV1 *)host->fetchSuite(host->host, kOfxImageEffectSuite, 1);
    props = (OfxPropertySuiteV1 *)host->fetchSuite(host->host, kOfxPropertySuite, 1);
    params = (OfxParameterSuiteV1 *)host->fetchSuite(host->host, kOfxParameterSuite, 1);
    card = (OfxImageEffectOpenGLRenderSuiteV1 *)host->fetchSuite(host->host, kOfxOpenGLRenderSuite, 1);
    return effects && props && params && card ? kOfxStatOK : kOfxStatErrMissingHostFeature;
  }
  if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect);
  if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect);
  if (!std::strcmp(action, kOfxActionOpenGLContextAttached)) {
    ++attached;
    return kOfxStatOK;
  }
  if (!std::strcmp(action, kOfxActionOpenGLContextDetached)) {
    --attached;
    return kOfxStatOK;
  }
  if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, in);
  return kOfxStatReplyDefault;
}

static void setHost(OfxHost *h) { host = h; }

static OfxPlugin plugin = {kOfxImageEffectPluginApi, 1, "com.wavyflo.test.card", 1, 0, setHost, mainEntry};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &plugin : nullptr; }
