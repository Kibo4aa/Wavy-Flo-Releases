// A filter with the kinds of control that hold no number, for
// --ofx-self-test: built into ofx-test (CMakeLists.txt). A button, Measure,
// and words to read, Note; then a group "Look" holding a number, Amount, and
// a line of text, Caption; a page lists them all. It draws an opaque
// picture: red Amount (0 to 1), green ten levels for each letter of Caption,
// and a blue dot three pixels across at Centre, a point given as a fraction
// of the frame (and so handed over in the frame's own units).
// Measure looks at the picture it is given -- its red, on average -- sets
// Amount to that and Caption to "measured", as an "Analyze" button would.
// Edit Look does what a look suite's own button does: the look made in its
// window goes into a Custom parameter, LookData ("warm"), and a secret
// counter, LookVersion, goes up by one -- neither has a control. A warm look
// draws the picture's blue at 100 (the dot stays 255).
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include <cstring>

static OfxHost *host = nullptr;
static OfxImageEffectSuiteV1 *effects = nullptr;
static OfxPropertySuiteV1 *props = nullptr;
static OfxParameterSuiteV1 *params = nullptr;

static OfxParamHandle param(OfxImageEffectHandle effect, const char *name) {
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  OfxParamHandle handle = nullptr;
  params->paramGetHandle(set, name, &handle, nullptr);
  return handle;
}

static OfxStatus describe(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p;
  effects->getPropertySet(effect, &p);
  props->propSetString(p, kOfxPropLabel, 0, "Wavy-Flo Test Controls");
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
  OfxParamSetHandle set;
  effects->getParamSet(effect, &set);
  params->paramDefine(set, kOfxParamTypePushButton, "Measure", &p);
  props->propSetString(p, kOfxPropLabel, 0, "Measure");
  params->paramDefine(set, kOfxParamTypeString, "Note", &p);
  props->propSetString(p, kOfxParamPropStringMode, 0, kOfxParamStringIsLabel);
  props->propSetString(p, kOfxParamPropDefault, 0, "Press Measure to read the picture.");
  // Amount made before the group it goes in: the panel goes by the group a
  // parameter is put in, not by the order it was made.
  params->paramDefine(set, kOfxParamTypeDouble, "Amount", &p);
  props->propSetString(p, kOfxParamPropParent, 0, "Look");
  props->propSetDouble(p, kOfxParamPropDefault, 0, .25);
  props->propSetDouble(p, kOfxParamPropMin, 0, 0);
  props->propSetDouble(p, kOfxParamPropMax, 0, 1);
  props->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0);
  props->propSetDouble(p, kOfxParamPropDisplayMax, 0, 1);
  params->paramDefine(set, kOfxParamTypeGroup, "Look", &p);
  props->propSetString(p, kOfxPropLabel, 0, "Look");
  params->paramDefine(set, kOfxParamTypeString, "Caption", &p);
  props->propSetString(p, kOfxParamPropParent, 0, "Look");
  props->propSetString(p, kOfxParamPropDefault, 0, "hi");
  params->paramDefine(set, kOfxParamTypeDouble2D, "Centre", &p);
  props->propSetString(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
  props->propSetString(p, kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesNormalised);
  props->propSetDouble(p, kOfxParamPropDefault, 0, .25);
  props->propSetDouble(p, kOfxParamPropDefault, 1, .75);
  params->paramDefine(set, kOfxParamTypePushButton, "EditLook", &p);
  props->propSetString(p, kOfxPropLabel, 0, "Edit Look...");
  params->paramDefine(set, kOfxParamTypeCustom, "LookData", &p);
  props->propSetString(p, kOfxParamPropDefault, 0, "");
  params->paramDefine(set, kOfxParamTypeInteger, "LookVersion", &p);
  props->propSetInt(p, kOfxParamPropSecret, 0, 1);
  props->propSetInt(p, kOfxParamPropDefault, 0, 0);
  params->paramDefine(set, kOfxParamTypePage, "Controls", &p);
  const char *children[] = {"Look", "Measure", "Note"};
  for (int k = 0; k < 3; ++k) props->propSetString(p, kOfxParamPropPageChild, k, children[k]);
  return kOfxStatOK;
}

static OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  char *name = nullptr;
  OfxTime time = 0;
  props->propGetString(in, kOfxPropName, 0, &name);
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  if (name && !std::strcmp(name, "EditLook")) {
    int version = 0;
    params->paramGetValue(param(effect, "LookVersion"), &version);
    params->paramSetValue(param(effect, "LookData"), "warm");
    params->paramSetValue(param(effect, "LookVersion"), version + 1);
    return kOfxStatOK;
  }
  if (!name || std::strcmp(name, "Measure")) return kOfxStatReplyDefault;
  OfxImageClipHandle source;
  effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, nullptr);
  OfxPropertySetHandle image = nullptr;
  if (effects->clipGetImage(source, time, nullptr, &image) != kOfxStatOK) return kOfxStatFailed;
  void *data = nullptr;
  int rowBytes = 0, bounds[4] = {0, 0, 0, 0};
  props->propGetPointer(image, kOfxImagePropData, 0, &data);
  props->propGetInt(image, kOfxImagePropRowBytes, 0, &rowBytes);
  props->propGetIntN(image, kOfxImagePropBounds, 4, bounds);
  double sum = 0;
  long count = 0;
  for (int y = 0; y < bounds[3] - bounds[1]; ++y) {
    const unsigned char *row = static_cast<const unsigned char *>(data) + y * rowBytes;
    for (int x = 0; x < bounds[2] - bounds[0]; ++x, ++count) sum += row[x * 4];
  }
  effects->clipReleaseImage(image);
  params->paramSetValue(param(effect, "Amount"), count ? sum / count / 255. : 0.);
  params->paramSetValue(param(effect, "Caption"), "measured");
  return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
  OfxTime time = 0;
  props->propGetDouble(in, kOfxPropTime, 0, &time);
  double amount = 0, centreX = 0, centreY = 0, scale[2] = {1, 1};
  char *caption = nullptr, *look = nullptr;
  params->paramGetValueAtTime(param(effect, "LookData"), time, &look);
  const unsigned char base = look && !std::strcmp(look, "warm") ? 100 : 0;
  params->paramGetValueAtTime(param(effect, "Amount"), time, &amount);
  params->paramGetValueAtTime(param(effect, "Caption"), time, &caption);
  params->paramGetValueAtTime(param(effect, "Centre"), time, &centreX, &centreY);
  props->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, scale);
  OfxImageClipHandle output;
  effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, nullptr);
  OfxPropertySetHandle to = nullptr;
  if (effects->clipGetImage(output, time, nullptr, &to) != kOfxStatOK) return kOfxStatFailed;
  void *data = nullptr;
  int rowBytes = 0, bounds[4] = {0, 0, 0, 0};
  props->propGetPointer(to, kOfxImagePropData, 0, &data);
  props->propGetInt(to, kOfxImagePropRowBytes, 0, &rowBytes);
  props->propGetIntN(to, kOfxImagePropBounds, 4, bounds);
  const unsigned char red = (unsigned char)(amount < 0 ? 0 : amount > 1 ? 255 : amount * 255 + .5);
  const size_t letters = caption ? std::strlen(caption) : 0;
  const unsigned char green = (unsigned char)(letters > 25 ? 250 : letters * 10);
  const double dotX = centreX * scale[0], dotY = centreY * scale[1];
  for (int y = 0; y < bounds[3] - bounds[1]; ++y) {
    unsigned char *row = static_cast<unsigned char *>(data) + y * rowBytes;
    for (int x = 0; x < bounds[2] - bounds[0]; ++x) {
      const double dx = bounds[0] + x + .5 - dotX, dy = bounds[1] + y + .5 - dotY;
      row[x * 4 + 0] = red;
      row[x * 4 + 1] = green;
      row[x * 4 + 2] = dx * dx + dy * dy <= 2.25 ? 255 : base;
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
  if (!std::strcmp(action, kOfxActionInstanceChanged)) return instanceChanged(effect, in);
  if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, in);
  return kOfxStatReplyDefault;
}

static void setHost(OfxHost *h) { host = h; }

static OfxPlugin plugin = {kOfxImageEffectPluginApi, 1, "com.wavyflo.test.controls", 1, 0, setHost, mainEntry};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &plugin : nullptr; }
