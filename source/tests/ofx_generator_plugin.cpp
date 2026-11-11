// A plug-in that works only as a generator -- no picture in, one out -- for
// --ofx-self-test: built into ofx-test (CMakeLists.txt). It fills what it is
// given, opaque, at eight bits: red is Level (0 to 1) at the frame drawn, green
// Level a frame earlier, blue ten levels for each keyframe Level has -- what
// the host says of the parameter at other times -- or 200 when the host asks
// for a draft.
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include <cstring>

static OfxHost *host = nullptr;
static OfxImageEffectSuiteV1 *effects = nullptr;
static OfxPropertySuiteV1 *props = nullptr;
static OfxParameterSuiteV1 *params = nullptr;

static OfxStatus describe(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->getPropertySet(effect, &p);
  props->propSetString(p, kOfxPropLabel, 0, "Wavy-Flo Test Generator");
  props->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Wavy-Flo Tests");
  props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGenerator);
  props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  return kOfxStatOK;
}

static OfxStatus describeInContext(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->clipDefine(effect, kOfxImageEffectOutputClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  params->paramDefine(set, kOfxParamTypeDouble, "Level", &p);
  props->propSetDouble(p, kOfxParamPropDefault, 0, .5);
  props->propSetDouble(p, kOfxParamPropMin, 0, 0);
  props->propSetDouble(p, kOfxParamPropMax, 0, 1);
  props->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0);
  props->propSetDouble(p, kOfxParamPropDisplayMax, 0, 1);
  return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  OfxTime time = 0;
  int draft = 0;
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  props->propGetInt(in, kOfxImageEffectPropRenderQualityDraft, 0, &draft);
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  OfxParamHandle level;
  double value = 0, before = 0;
  unsigned int keys = 0;
  if (params->paramGetHandle(set, "Level", &level, nullptr) != kOfxStatOK) return kOfxStatFailed;
  params->paramGetValueAtTime(level, time, &value);
  params->paramGetValueAtTime(level, time - 1, &before);
  params->paramGetNumKeys(level, &keys);
  OfxImageClipHandle output;
  effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, nullptr);
  OfxPropertySetHandle to = nullptr;
  if (effects->clipGetImage(output, time, nullptr, &to) != kOfxStatOK) return kOfxStatFailed;
  void *data = nullptr;
  int rowBytes = 0, bounds[4] = {0, 0, 0, 0};
  props->propGetPointer(to, kOfxImagePropData, 0, &data);
  props->propGetInt(to, kOfxImagePropRowBytes, 0, &rowBytes);
  props->propGetIntN(to, kOfxImagePropBounds, 4, bounds);
  auto byte = [](double v) { return (unsigned char)(v < 0 ? 0 : v > 1 ? 255 : v * 255 + .5); };
  const unsigned char red = byte(value), green = byte(before);
  const unsigned char blue = draft ? 200 : (unsigned char)(keys > 25 ? 250 : keys * 10);
  for (int y = 0; y < bounds[3] - bounds[1]; ++y) {
    unsigned char *row = static_cast<unsigned char *>(data) + y * rowBytes;
    for (int x = 0; x < bounds[2] - bounds[0]; ++x) {
      row[x * 4 + 0] = red;
      row[x * 4 + 1] = green;
      row[x * 4 + 2] = blue;
      row[x * 4 + 3] = 255;
    }
  }
  effects->clipReleaseImage(to);
  return kOfxStatOK;
}

static OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
  auto effect = (OfxImageEffectHandle)handle;
  if (!std::strcmp(action, kOfxActionLoad)) {
    effects = (OfxImageEffectSuiteV1 *)host->fetchSuite(host->host, kOfxImageEffectSuite, 1);
    props = (OfxPropertySuiteV1 *)host->fetchSuite(host->host, kOfxPropertySuite, 1);
    params = (OfxParameterSuiteV1 *)host->fetchSuite(host->host, kOfxParameterSuite, 1);
    return effects && props && params ? kOfxStatOK : kOfxStatErrMissingHostFeature;
  }
  if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect);
  if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect);
  if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, in);
  return kOfxStatReplyDefault;
}

static void setHost(OfxHost *h) { host = h; }

static OfxPlugin plugin = {kOfxImageEffectPluginApi, 1, "com.wavyflo.test.generator", 1, 0, setHost, mainEntry};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &plugin : nullptr; }
