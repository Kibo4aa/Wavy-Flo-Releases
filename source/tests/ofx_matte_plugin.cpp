// A general effect that takes a second picture, for --ofx-self-test: built
// into ofx-test (CMakeLists.txt). Its Matte clip is needed, and a mask: the
// layer is kept where the matte's alpha is, faded where it fades, gone where it
// is not. With no matte connected the layer comes through as it is.
#include "ofxImageEffect.h"
#include <cstring>

static OfxHost *host = nullptr;
static OfxImageEffectSuiteV1 *effects = nullptr;
static OfxPropertySuiteV1 *props = nullptr;

static OfxStatus describe(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->getPropertySet(effect, &p);
  props->propSetString(p, kOfxPropLabel, 0, "Wavy-Flo Test Matte");
  props->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Wavy-Flo Tests");
  props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGeneral);
  props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  return kOfxStatOK;
}

static OfxStatus describeInContext(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->clipDefine(effect, kOfxImageEffectOutputClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  effects->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  effects->clipDefine(effect, "Matte", &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  props->propSetInt(p, kOfxImageClipPropOptional, 0, 0);
  props->propSetInt(p, kOfxImageClipPropIsMask, 0, 1);
  return kOfxStatOK;
}

struct Picture {
  OfxPropertySetHandle handle = nullptr;
  unsigned char *data = nullptr;
  int rowBytes = 0, bounds[4] = {0, 0, 0, 0};
  bool fetch(OfxImageClipHandle clip, OfxTime time) {
    if (effects->clipGetImage(clip, time, nullptr, &handle) != kOfxStatOK) return false;
    void *at = nullptr;
    props->propGetPointer(handle, kOfxImagePropData, 0, &at);
    props->propGetInt(handle, kOfxImagePropRowBytes, 0, &rowBytes);
    props->propGetIntN(handle, kOfxImagePropBounds, 4, bounds);
    data = static_cast<unsigned char *>(at);
    return data != nullptr;
  }
  unsigned char *pixel(int x, int y) const { return data + (y - bounds[1]) * rowBytes + (x - bounds[0]) * 4; }
  bool has(int x, int y) const { return x >= bounds[0] && x < bounds[2] && y >= bounds[1] && y < bounds[3]; }
  ~Picture() {
    if (handle) effects->clipReleaseImage(handle);
  }
};

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  OfxTime time = 0;
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  OfxImageClipHandle source, matte, output;
  effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, nullptr);
  effects->clipGetHandle(effect, "Matte", &matte, nullptr);
  effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, nullptr);
  OfxPropertySetHandle matteProps = nullptr;
  int connected = 0;
  effects->clipGetPropertySet(matte, &matteProps);
  props->propGetInt(matteProps, kOfxImageClipPropConnected, 0, &connected);
  Picture from, to, mask;
  if (!from.fetch(source, time) || !to.fetch(output, time)) return kOfxStatFailed;
  const bool masked = connected && mask.fetch(matte, time);
  for (int y = to.bounds[1]; y < to.bounds[3]; ++y)
    for (int x = to.bounds[0]; x < to.bounds[2]; ++x) {
      unsigned char *out = to.pixel(x, y);
      if (!from.has(x, y)) {
        out[0] = out[1] = out[2] = out[3] = 0;
        continue;
      }
      const unsigned char *src = from.pixel(x, y);
      const int alpha = !masked ? 255 : mask.has(x, y) ? mask.pixel(x, y)[3] : 0;
      for (int k = 0; k < 4; ++k) out[k] = (unsigned char)(src[k] * alpha / 255);
    }
  return kOfxStatOK;
}

static OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
  auto effect = (OfxImageEffectHandle)handle;
  if (!std::strcmp(action, kOfxActionLoad)) {
    effects = (OfxImageEffectSuiteV1 *)host->fetchSuite(host->host, kOfxImageEffectSuite, 1);
    props = (OfxPropertySuiteV1 *)host->fetchSuite(host->host, kOfxPropertySuite, 1);
    return effects && props ? kOfxStatOK : kOfxStatErrMissingHostFeature;
  }
  if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect);
  if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect);
  if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, in);
  return kOfxStatReplyDefault;
}

static void setHost(OfxHost *h) { host = h; }

static OfxPlugin plugin = {kOfxImageEffectPluginApi, 1, "com.wavyflo.test.matte", 1, 0, setHost, mainEntry};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &plugin : nullptr; }
