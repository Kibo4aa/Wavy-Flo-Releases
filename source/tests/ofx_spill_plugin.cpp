// A filter that draws beyond the picture it is given, for --ofx-self-test:
// built into ofx-test (CMakeLists.txt). Its region of definition is its
// source's grown by Spill (canonical units, 0 to 100) on every side; it copies
// the source where the source is defined, and fills the band around it with
// an opaque grey of 128. Everything outside that is left transparent. It
// works in the image's own pixel coordinates -- bounds that may begin below
// zero -- as a plug-in drawing past its frame has to.
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include <cstring>

static OfxHost *host = nullptr;
static OfxImageEffectSuiteV1 *effects = nullptr;
static OfxPropertySuiteV1 *props = nullptr;
static OfxParameterSuiteV1 *params = nullptr;

static double spillAt(OfxImageEffectHandle effect, OfxTime time) {
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  OfxParamHandle spill;
  double value = 0;
  if (params->paramGetHandle(set, "Spill", &spill, nullptr) == kOfxStatOK) params->paramGetValueAtTime(spill, time, &value);
  return value;
}

static OfxStatus describe(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->getPropertySet(effect, &p);
  props->propSetString(p, kOfxPropLabel, 0, "Wavy-Flo Test Spill");
  props->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Wavy-Flo Tests");
  props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  props->propSetInt(p, kOfxImageEffectPropSupportsTiles, 0, 0);
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
  params->paramDefine(set, kOfxParamTypeDouble, "Spill", &p);
  props->propSetDouble(p, kOfxParamPropDefault, 0, 10);
  props->propSetDouble(p, kOfxParamPropMin, 0, 0);
  props->propSetDouble(p, kOfxParamPropMax, 0, 100);
  props->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0);
  props->propSetDouble(p, kOfxParamPropDisplayMax, 0, 100);
  return kOfxStatOK;
}

static OfxStatus regionOfDefinition(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out) {
  OfxTime time = 0;
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  OfxImageClipHandle source;
  effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, nullptr);
  OfxRectD rod;
  if (effects->clipGetRegionOfDefinition(source, time, &rod) != kOfxStatOK) return kOfxStatFailed;
  const double spill = spillAt(effect, time);
  const double grown[4] = {rod.x1 - spill, rod.y1 - spill, rod.x2 + spill, rod.y2 + spill};
  props->propSetDoubleN(out, kOfxImageEffectPropRegionOfDefinition, 4, grown);
  return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  OfxTime time = 0;
  double scale[2] = {1, 1};
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  props->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, scale);
  OfxImageClipHandle source, output;
  effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, nullptr);
  effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, nullptr);
  OfxRectD rod;
  effects->clipGetRegionOfDefinition(source, time, &rod);
  OfxPropertySetHandle from = nullptr, to = nullptr;
  if (effects->clipGetImage(source, time, nullptr, &from) != kOfxStatOK) return kOfxStatFailed;
  if (effects->clipGetImage(output, time, nullptr, &to) != kOfxStatOK) {
    effects->clipReleaseImage(from);
    return kOfxStatFailed;
  }
  void *a = nullptr, *b = nullptr;
  int rowA = 0, rowB = 0, boundsA[4] = {0, 0, 0, 0}, boundsB[4] = {0, 0, 0, 0};
  props->propGetPointer(from, kOfxImagePropData, 0, &a);
  props->propGetPointer(to, kOfxImagePropData, 0, &b);
  props->propGetInt(from, kOfxImagePropRowBytes, 0, &rowA);
  props->propGetInt(to, kOfxImagePropRowBytes, 0, &rowB);
  props->propGetIntN(from, kOfxImagePropBounds, 4, boundsA);
  props->propGetIntN(to, kOfxImagePropBounds, 4, boundsB);
  // The source's region and the band around it, in pixels.
  const double spill = spillAt(effect, time);
  const double inner[4] = {rod.x1 * scale[0], rod.y1 * scale[1], rod.x2 * scale[0], rod.y2 * scale[1]};
  const double outer[4] = {inner[0] - spill * scale[0], inner[1] - spill * scale[1], inner[2] + spill * scale[0],
                           inner[3] + spill * scale[1]};
  for (int y = boundsB[1]; y < boundsB[3]; ++y) {
    unsigned char *row = static_cast<unsigned char *>(b) + (y - boundsB[1]) * rowB;
    for (int x = boundsB[0]; x < boundsB[2]; ++x) {
      unsigned char *pixel = row + (x - boundsB[0]) * 4;
      const double cx = x + .5, cy = y + .5;
      const bool inSource = cx > inner[0] && cx < inner[2] && cy > inner[1] && cy < inner[3];
      const bool inBand = cx > outer[0] && cx < outer[2] && cy > outer[1] && cy < outer[3];
      if (inSource && x >= boundsA[0] && x < boundsA[2] && y >= boundsA[1] && y < boundsA[3]) {
        std::memcpy(pixel, static_cast<unsigned char *>(a) + (y - boundsA[1]) * rowA + (x - boundsA[0]) * 4, 4);
      } else if (inBand && !inSource) {
        pixel[0] = pixel[1] = pixel[2] = 128;
        pixel[3] = 255;
      } else {
        pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
      }
    }
  }
  effects->clipReleaseImage(from);
  effects->clipReleaseImage(to);
  return kOfxStatOK;
}

static OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
  auto effect = (OfxImageEffectHandle)handle;
  if (!std::strcmp(action, kOfxActionLoad)) {
    effects = (OfxImageEffectSuiteV1 *)host->fetchSuite(host->host, kOfxImageEffectSuite, 1);
    props = (OfxPropertySuiteV1 *)host->fetchSuite(host->host, kOfxPropertySuite, 1);
    params = (OfxParameterSuiteV1 *)host->fetchSuite(host->host, kOfxParameterSuite, 1);
    return effects && props && params ? kOfxStatOK : kOfxStatErrMissingHostFeature;
  }
  if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect);
  if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect);
  if (!std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition)) return regionOfDefinition(effect, in, out);
  if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, in);
  return kOfxStatReplyDefault;
}

static void setHost(OfxHost *h) { host = h; }

static OfxPlugin plugin = {kOfxImageEffectPluginApi, 1, "com.wavyflo.test.spill", 1, 0, setHost, mainEntry};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &plugin : nullptr; }
