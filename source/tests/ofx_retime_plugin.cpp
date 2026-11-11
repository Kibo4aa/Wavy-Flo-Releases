// A retimer and nothing else, that talks, for --ofx-self-test: built into
// ofx-test (CMakeLists.txt). It shows its source at SourceTime -- the time the
// host sets -- and says so in passing ("showing frame 5"); while SourceTime is
// before the layer starts it keeps an error up, and takes it back after. Its
// button, Analyse, reports progress in four steps and stops when told to.
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include <cstring>

static OfxHost *host = nullptr;
static OfxImageEffectSuiteV1 *effects = nullptr;
static OfxPropertySuiteV1 *props = nullptr;
static OfxParameterSuiteV1 *params = nullptr;
static OfxMessageSuiteV2 *messages = nullptr;
static OfxProgressSuiteV1 *progress = nullptr;

static OfxStatus describe(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->getPropertySet(effect, &p);
  props->propSetString(p, kOfxPropLabel, 0, "Wavy-Flo Test Retime");
  props->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Wavy-Flo Tests");
  props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextRetimer);
  props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  props->propSetInt(p, kOfxImageEffectPropTemporalClipAccess, 0, 1);
  return kOfxStatOK;
}

static OfxStatus describeInContext(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->clipDefine(effect, kOfxImageEffectOutputClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  effects->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &p);
  props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  props->propSetInt(p, kOfxImageEffectPropTemporalClipAccess, 0, 1);
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  params->paramDefine(set, kOfxParamTypeDouble, kOfxImageEffectRetimerParamName, &p);
  params->paramDefine(set, kOfxParamTypePushButton, "Analyse", &p);
  return kOfxStatOK;
}

static OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  char *name = nullptr;
  props->propGetString(in, kOfxPropName, 0, &name);
  if (!name || std::strcmp(name, "Analyse")) return kOfxStatReplyDefault;
  progress->progressStart(effect, "Analysing the layer");
  for (int step = 1; step <= 4; ++step)
    if (progress->progressUpdate(effect, step / 4.) == kOfxStatReplyNo) {
      progress->progressEnd(effect);
      messages->message(effect, kOfxMessageError, nullptr, "Analysis stopped at step %d", step);
      return kOfxStatFailed;
    }
  progress->progressEnd(effect);
  return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  OfxTime time = 0;
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  OfxParamHandle sourceTime;
  double from = time;
  if (params->paramGetHandle(set, kOfxImageEffectRetimerParamName, &sourceTime, nullptr) == kOfxStatOK)
    params->paramGetValueAtTime(sourceTime, time, &from);
  if (from < 0)
    messages->setPersistentMessage(effect, kOfxMessageError, nullptr, "Source time %g is before the layer starts", from);
  else
    messages->clearPersistentMessage(effect);
  messages->message(effect, kOfxMessageWarning, nullptr, "showing frame %g", from);
  OfxImageClipHandle source, output;
  effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, nullptr);
  effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, nullptr);
  OfxPropertySetHandle a = nullptr, b = nullptr;
  if (effects->clipGetImage(source, from < 0 ? 0 : from, nullptr, &a) != kOfxStatOK) return kOfxStatFailed;
  if (effects->clipGetImage(output, time, nullptr, &b) != kOfxStatOK) {
    effects->clipReleaseImage(a);
    return kOfxStatFailed;
  }
  void *pa = nullptr, *pb = nullptr;
  int rowA = 0, rowB = 0, bounds[4] = {0, 0, 0, 0};
  props->propGetPointer(a, kOfxImagePropData, 0, &pa);
  props->propGetPointer(b, kOfxImagePropData, 0, &pb);
  props->propGetInt(a, kOfxImagePropRowBytes, 0, &rowA);
  props->propGetInt(b, kOfxImagePropRowBytes, 0, &rowB);
  props->propGetIntN(b, kOfxImagePropBounds, 4, bounds);
  for (int y = 0; y < bounds[3] - bounds[1]; ++y)
    std::memcpy(static_cast<char *>(pb) + y * rowB, static_cast<char *>(pa) + y * rowA, size_t(rowA < rowB ? rowA : rowB));
  effects->clipReleaseImage(a);
  effects->clipReleaseImage(b);
  return kOfxStatOK;
}

static OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
  auto effect = (OfxImageEffectHandle)handle;
  if (!std::strcmp(action, kOfxActionLoad)) {
    effects = (OfxImageEffectSuiteV1 *)host->fetchSuite(host->host, kOfxImageEffectSuite, 1);
    props = (OfxPropertySuiteV1 *)host->fetchSuite(host->host, kOfxPropertySuite, 1);
    params = (OfxParameterSuiteV1 *)host->fetchSuite(host->host, kOfxParameterSuite, 1);
    messages = (OfxMessageSuiteV2 *)host->fetchSuite(host->host, kOfxMessageSuite, 2);
    progress = (OfxProgressSuiteV1 *)host->fetchSuite(host->host, kOfxProgressSuite, 1);
    return effects && props && params && messages && progress ? kOfxStatOK : kOfxStatErrMissingHostFeature;
  }
  if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect);
  if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect);
  if (!std::strcmp(action, kOfxActionInstanceChanged)) return instanceChanged(effect, in);
  if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, in);
  return kOfxStatReplyDefault;
}

static void setHost(OfxHost *h) { host = h; }

static OfxPlugin plugin = {kOfxImageEffectPluginApi, 1, "com.wavyflo.test.retime", 1, 0, setHost, mainEntry};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &plugin : nullptr; }
