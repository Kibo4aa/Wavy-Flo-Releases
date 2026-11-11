// A plug-in that misbehaves on purpose, for --ofx-self-test: the plug-in host
// process must take each of these, and Wavy-Flo must carry on. Built four
// times into ofx-hazards (CMakeLists.txt), once for each WAVY_HAZARD:
//   1  ends its process (std::terminate) while describing itself
//   2  writes through a null pointer while rendering
//   3  ends its process (std::terminate) while rendering
//   4  never returns from rendering
// Otherwise an ordinary filter that copies its source.
#include "ofxImageEffect.h"
#include <cstring>
#include <exception>
#include <thread>

#ifndef WAVY_HAZARD
#error WAVY_HAZARD names the misbehaviour
#endif
#if WAVY_HAZARD == 1
#define HAZARD_ID "com.wavyflo.test.hazard.load"
#define HAZARD_LABEL "Hazard Load"
#elif WAVY_HAZARD == 2
#define HAZARD_ID "com.wavyflo.test.hazard.crash"
#define HAZARD_LABEL "Hazard Crash"
#elif WAVY_HAZARD == 3
#define HAZARD_ID "com.wavyflo.test.hazard.terminate"
#define HAZARD_LABEL "Hazard Terminate"
#else
#define HAZARD_ID "com.wavyflo.test.hazard.hang"
#define HAZARD_LABEL "Hazard Hang"
#endif

static OfxHost *host = nullptr;
static OfxImageEffectSuiteV1 *effects = nullptr;
static OfxPropertySuiteV1 *props = nullptr;

static void misbehave() {
#if WAVY_HAZARD == 2
  volatile int *nowhere = nullptr;
  *nowhere = 1;
#elif WAVY_HAZARD == 3
  std::terminate();
#elif WAVY_HAZARD == 4
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
}

static OfxStatus describe(OfxImageEffectHandle effect) {
#if WAVY_HAZARD == 1
  std::terminate();
#endif
  OfxPropertySetHandle p;
  effects->getPropertySet(effect, &p);
  props->propSetString(p, kOfxPropLabel, 0, HAZARD_LABEL);
  props->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Wavy-Flo Tests");
  props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  return kOfxStatOK;
}

static OfxStatus describeInContext(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->clipDefine(effect, kOfxImageEffectOutputClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  effects->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  misbehave();
  OfxTime time = 0;
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  OfxImageClipHandle source, output;
  effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, nullptr);
  effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, nullptr);
  OfxPropertySetHandle from = nullptr, to = nullptr;
  if (effects->clipGetImage(source, time, nullptr, &from) != kOfxStatOK) return kOfxStatFailed;
  if (effects->clipGetImage(output, time, nullptr, &to) != kOfxStatOK) {
    effects->clipReleaseImage(from);
    return kOfxStatFailed;
  }
  void *a = nullptr, *b = nullptr;
  int rowA = 0, rowB = 0, boundsA[4] = {0, 0, 0, 0};
  props->propGetPointer(from, kOfxImagePropData, 0, &a);
  props->propGetPointer(to, kOfxImagePropData, 0, &b);
  props->propGetInt(from, kOfxImagePropRowBytes, 0, &rowA);
  props->propGetInt(to, kOfxImagePropRowBytes, 0, &rowB);
  props->propGetIntN(from, kOfxImagePropBounds, 4, boundsA);
  for (int y = 0; y < boundsA[3] - boundsA[1]; ++y)
    std::memcpy(static_cast<char *>(b) + y * rowB, static_cast<char *>(a) + y * rowA, size_t(rowA < rowB ? rowA : rowB));
  effects->clipReleaseImage(from);
  effects->clipReleaseImage(to);
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

static OfxPlugin plugin = {kOfxImageEffectPluginApi, 1, HAZARD_ID, 1, 0, setHost, mainEntry};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &plugin : nullptr; }
