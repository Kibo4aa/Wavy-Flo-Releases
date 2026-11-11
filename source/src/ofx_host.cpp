#include "ofx_host.h"
#include "ofx_internal.h"
#include "ofxCore.h"
#include "ofxGPURender.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxOld.h"
#include "ofxParam.h"
#include "ofxParametricParam.h"
#include "ofxProgress.h"
#include "ofxProperty.h"
#include "ofxTimeLine.h"
#include <QCoreApplication>
#include <QStringList>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QtGlobal>
#undef slots
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cfloat>
#include <climits>
#include <limits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <malloc.h>
#endif

// ---------------------------------------------------------------------------
// Calls into a plug-in. Its code is not ours: one that faults is caught here
// and marked broken, rather than taking Wavy-Flo and the person's work with
// it. Plain functions, because a structured exception handler cannot sit in a
// function that has objects to destroy.
// ---------------------------------------------------------------------------
#ifdef _WIN32
static OfxStatus sehMain(OfxPluginEntryPoint *entry, const char *action, const void *handle,
                         OfxPropertySetHandle in, OfxPropertySetHandle out, int *crashed) {
  __try {
    return entry(action, handle, in, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *crashed = 1;
    return kOfxStatErrFatal;
  }
}
static void sehSetHost(void (*setHost)(OfxHost *), OfxHost *host, int *crashed) {
  __try {
    setHost(host);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *crashed = 1;
  }
}
static OfxStatus sehSetHostAll(OfxStatus (*setHost)(const OfxHost *), const OfxHost *host, int *crashed) {
  __try {
    return setHost(host);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *crashed = 1;
    return kOfxStatErrFatal;
  }
}
static int sehCount(int (*count)(void), int *crashed) {
  __try {
    return count();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *crashed = 1;
    return 0;
  }
}
static OfxPlugin *sehPlugin(OfxPlugin *(*get)(int), int nth, int *crashed) {
  __try {
    return get(nth);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *crashed = 1;
    return nullptr;
  }
}
static HMODULE sehLoad(const wchar_t *path, int *crashed) {
  __try {
    // The bundle's own folder first, for the libraries it brings with it.
    return LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *crashed = 1;
    return nullptr;
  }
}
static void sehFree(HMODULE module, int *crashed) {
  __try {
    FreeLibrary(module);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *crashed = 1;
  }
}
static void sehThread(OfxThreadFunctionV1 *func, unsigned index, unsigned count, void *argument, int *crashed) {
  __try {
    func(index, count, argument);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *crashed = 1;
  }
}
#endif

namespace {

// ---------------------------------------------------------------------------
// Property sets: what every OFX object is described by. Plug-ins read and
// write them by name from any of their threads.
// ---------------------------------------------------------------------------
struct Property {
  enum Type { Int, Double, String, Pointer };
  Type type = Int;
  std::vector<int> ints;
  std::vector<double> doubles;
  std::vector<std::string> strings;
  std::vector<void *> pointers;
  int dimension() const {
    switch (type) {
      case Int: return int(ints.size());
      case Double: return int(doubles.size());
      case String: return int(strings.size());
      default: return int(pointers.size());
    }
  }
};

struct PropertySet {
  std::map<std::string, Property> values;
  mutable std::recursive_mutex guard;
  PropertySet() = default;
  PropertySet(const PropertySet &other) {
    std::lock_guard<std::recursive_mutex> lock(other.guard);
    values = other.values;
  }
  PropertySet &operator=(const PropertySet &other) {
    if (this == &other) return *this;
    std::scoped_lock lock(guard, other.guard);
    values = other.values;
    return *this;
  }
  Property &define(const std::string &name, Property::Type type) {
    std::lock_guard<std::recursive_mutex> lock(guard);
    Property &p = values[name];
    p = Property{};
    p.type = type;
    return p;
  }
  void setInt(const std::string &name, std::initializer_list<int> v) { define(name, Property::Int).ints = v; }
  void setDouble(const std::string &name, std::initializer_list<double> v) { define(name, Property::Double).doubles = v; }
  void setString(const std::string &name, std::initializer_list<std::string> v) { define(name, Property::String).strings = v; }
  void setPointer(const std::string &name, void *v) { define(name, Property::Pointer).pointers = {v}; }
  void empty(const std::string &name, Property::Type type) { define(name, type); }
  const Property *find(const std::string &name) const {
    auto it = values.find(name);
    return it == values.end() ? nullptr : &it->second;
  }
  int getInt(const std::string &name, int index = 0, int fallback = 0) const {
    std::lock_guard<std::recursive_mutex> lock(guard);
    const Property *p = find(name);
    if (!p || index >= p->dimension()) return fallback;
    return p->type == Property::Int ? p->ints[size_t(index)]
           : p->type == Property::Double ? int(p->doubles[size_t(index)]) : fallback;
  }
  double getDouble(const std::string &name, int index = 0, double fallback = 0) const {
    std::lock_guard<std::recursive_mutex> lock(guard);
    const Property *p = find(name);
    if (!p || index >= p->dimension()) return fallback;
    return p->type == Property::Double ? p->doubles[size_t(index)]
           : p->type == Property::Int ? double(p->ints[size_t(index)]) : fallback;
  }
  std::string getString(const std::string &name, int index = 0, const std::string &fallback = {}) const {
    std::lock_guard<std::recursive_mutex> lock(guard);
    const Property *p = find(name);
    if (!p || p->type != Property::String || index >= p->dimension()) return fallback;
    return p->strings[size_t(index)];
  }
  std::vector<std::string> strings(const std::string &name) const {
    std::lock_guard<std::recursive_mutex> lock(guard);
    const Property *p = find(name);
    return p && p->type == Property::String ? p->strings : std::vector<std::string>{};
  }
  int dimension(const std::string &name) const {
    std::lock_guard<std::recursive_mutex> lock(guard);
    const Property *p = find(name);
    return p ? p->dimension() : 0;
  }
};
PropertySet *propertySet(OfxPropertySetHandle h) { return reinterpret_cast<PropertySet *>(h); }
OfxPropertySetHandle handleOf(PropertySet *p) { return reinterpret_cast<OfxPropertySetHandle>(p); }

template <class T> void put(std::vector<T> &v, int index, T value) {
  if (size_t(index) >= v.size()) v.resize(size_t(index) + 1);
  v[size_t(index)] = value;
}
// The property `name` ready to take a value of `type` at `index`: made if it
// is not there, and an empty one takes the type it is given. Whole numbers and
// fractions go into each other; anything else is the wrong kind of value.
Property *writable(PropertySet *set, const char *name, Property::Type type) {
  Property &p = set->values[name];
  if (p.dimension() == 0) p.type = type;
  if (p.type == type) return &p;
  const bool numbers = (p.type == Property::Int || p.type == Property::Double) &&
                       (type == Property::Int || type == Property::Double);
  return numbers ? &p : nullptr;
}

OfxStatus propSetPointer(OfxPropertySetHandle h, const char *name, int index, void *value) {
  if (!h || !name || index < 0) return kOfxStatErrBadHandle;
  auto *set = propertySet(h);
  std::lock_guard<std::recursive_mutex> lock(set->guard);
  Property *p = writable(set, name, Property::Pointer);
  if (!p) return kOfxStatErrValue;
  put(p->pointers, index, value);
  return kOfxStatOK;
}
OfxStatus propSetString(OfxPropertySetHandle h, const char *name, int index, const char *value) {
  if (!h || !name || index < 0) return kOfxStatErrBadHandle;
  auto *set = propertySet(h);
  std::lock_guard<std::recursive_mutex> lock(set->guard);
  Property *p = writable(set, name, Property::String);
  if (!p) return kOfxStatErrValue;
  put(p->strings, index, std::string(value ? value : ""));
  return kOfxStatOK;
}
OfxStatus propSetDouble(OfxPropertySetHandle h, const char *name, int index, double value) {
  if (!h || !name || index < 0) return kOfxStatErrBadHandle;
  auto *set = propertySet(h);
  std::lock_guard<std::recursive_mutex> lock(set->guard);
  Property *p = writable(set, name, Property::Double);
  if (!p) return kOfxStatErrValue;
  if (p->type == Property::Int) put(p->ints, index, int(value));
  else put(p->doubles, index, value);
  return kOfxStatOK;
}
OfxStatus propSetInt(OfxPropertySetHandle h, const char *name, int index, int value) {
  if (!h || !name || index < 0) return kOfxStatErrBadHandle;
  auto *set = propertySet(h);
  std::lock_guard<std::recursive_mutex> lock(set->guard);
  Property *p = writable(set, name, Property::Int);
  if (!p) return kOfxStatErrValue;
  if (p->type == Property::Double) put(p->doubles, index, double(value));
  else put(p->ints, index, value);
  return kOfxStatOK;
}
// Setting n values makes the property exactly n long.
template <class Setter, class T>
OfxStatus setN(OfxPropertySetHandle h, const char *name, int count, const T *values, Setter one, Property::Type type) {
  if (!h || !name || count < 0 || (count && !values)) return kOfxStatErrBadHandle;
  auto *set = propertySet(h);
  std::lock_guard<std::recursive_mutex> lock(set->guard);
  Property *p = writable(set, name, type);
  if (!p) return kOfxStatErrValue;
  p->ints.clear(); p->doubles.clear(); p->strings.clear(); p->pointers.clear();
  for (int i = 0; i < count; ++i) {
    const OfxStatus status = one(h, name, i, values[i]);
    if (status != kOfxStatOK) return status;
  }
  return kOfxStatOK;
}
OfxStatus propSetPointerN(OfxPropertySetHandle h, const char *name, int count, void *const *value) {
  return setN(h, name, count, value, propSetPointer, Property::Pointer);
}
OfxStatus propSetStringN(OfxPropertySetHandle h, const char *name, int count, const char *const *value) {
  return setN(h, name, count, value, propSetString, Property::String);
}
OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char *name, int count, const double *value) {
  return setN(h, name, count, value, propSetDouble, Property::Double);
}
OfxStatus propSetIntN(OfxPropertySetHandle h, const char *name, int count, const int *value) {
  return setN(h, name, count, value, propSetInt, Property::Int);
}

// What plug-ins asked the host for that it does not have -- a suite, or a
// property of the host or of the objects it hands them -- for --ofx-list.
std::mutex gapsGuard;
std::set<std::string> gaps;
void noteGap(const std::string &what) {
  std::lock_guard<std::mutex> lock(gapsGuard);
  if (gaps.size() < 512) gaps.insert(what);
}
const Property *readable(OfxPropertySetHandle h, const char *name, int index, OfxStatus &status) {
  status = kOfxStatOK;
  if (!h || !name || index < 0) { status = kOfxStatErrBadHandle; return nullptr; }
  const Property *p = propertySet(h)->find(name);
  if (!p) {
    status = kOfxStatErrUnknown;
    const Property *kind = propertySet(h)->find(kOfxPropType);
    const std::string owner = kind && kind->type == Property::String && !kind->strings.empty() ? kind->strings[0] : "arguments";
    noteGap("property " + std::string(name) + " (on " + owner + ")");
    return nullptr;
  }
  if (index >= p->dimension()) { status = kOfxStatErrBadIndex; return nullptr; }
  return p;
}
OfxStatus propGetPointer(OfxPropertySetHandle h, const char *name, int index, void **value) {
  if (!value) return kOfxStatErrBadHandle;
  OfxStatus status;
  std::unique_lock<std::recursive_mutex> lock;
  if (h) lock = std::unique_lock<std::recursive_mutex>(propertySet(h)->guard);
  const Property *p = readable(h, name, index, status);
  if (!p) return status;
  if (p->type != Property::Pointer) return kOfxStatErrValue;
  *value = p->pointers[size_t(index)];
  return kOfxStatOK;
}
OfxStatus propGetString(OfxPropertySetHandle h, const char *name, int index, char **value) {
  if (!value) return kOfxStatErrBadHandle;
  OfxStatus status;
  std::unique_lock<std::recursive_mutex> lock;
  if (h) lock = std::unique_lock<std::recursive_mutex>(propertySet(h)->guard);
  const Property *p = readable(h, name, index, status);
  if (!p) return status;
  if (p->type != Property::String) return kOfxStatErrValue;
  *value = const_cast<char *>(p->strings[size_t(index)].c_str());
  return kOfxStatOK;
}
OfxStatus propGetDouble(OfxPropertySetHandle h, const char *name, int index, double *value) {
  if (!value) return kOfxStatErrBadHandle;
  OfxStatus status;
  std::unique_lock<std::recursive_mutex> lock;
  if (h) lock = std::unique_lock<std::recursive_mutex>(propertySet(h)->guard);
  const Property *p = readable(h, name, index, status);
  if (!p) return status;
  if (p->type == Property::Double) *value = p->doubles[size_t(index)];
  else if (p->type == Property::Int) *value = double(p->ints[size_t(index)]);
  else return kOfxStatErrValue;
  return kOfxStatOK;
}
OfxStatus propGetInt(OfxPropertySetHandle h, const char *name, int index, int *value) {
  if (!value) return kOfxStatErrBadHandle;
  OfxStatus status;
  std::unique_lock<std::recursive_mutex> lock;
  if (h) lock = std::unique_lock<std::recursive_mutex>(propertySet(h)->guard);
  const Property *p = readable(h, name, index, status);
  if (!p) return status;
  if (p->type == Property::Int) *value = p->ints[size_t(index)];
  else if (p->type == Property::Double) *value = int(p->doubles[size_t(index)]);
  else return kOfxStatErrValue;
  return kOfxStatOK;
}
template <class Getter, class T>
OfxStatus getN(OfxPropertySetHandle h, const char *name, int count, T *values, Getter one) {
  if (!values || count < 0) return kOfxStatErrBadHandle;
  for (int i = 0; i < count; ++i) {
    const OfxStatus status = one(h, name, i, values + i);
    if (status != kOfxStatOK) return status;
  }
  return kOfxStatOK;
}
OfxStatus propGetPointerN(OfxPropertySetHandle h, const char *name, int count, void **value) {
  return getN(h, name, count, value, propGetPointer);
}
OfxStatus propGetStringN(OfxPropertySetHandle h, const char *name, int count, char **value) {
  return getN(h, name, count, value, propGetString);
}
OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char *name, int count, double *value) {
  return getN(h, name, count, value, propGetDouble);
}
OfxStatus propGetIntN(OfxPropertySetHandle h, const char *name, int count, int *value) {
  return getN(h, name, count, value, propGetInt);
}
OfxStatus propReset(OfxPropertySetHandle h, const char *name) {
  if (!h || !name) return kOfxStatErrBadHandle;
  std::lock_guard<std::recursive_mutex> lock(propertySet(h)->guard);
  return propertySet(h)->find(name) ? kOfxStatOK : kOfxStatErrUnknown;
}
OfxStatus propGetDimension(OfxPropertySetHandle h, const char *name, int *count) {
  if (!h || !name || !count) return kOfxStatErrBadHandle;
  std::lock_guard<std::recursive_mutex> lock(propertySet(h)->guard);
  const Property *p = propertySet(h)->find(name);
  if (!p) return kOfxStatErrUnknown;
  *count = p->dimension();
  return kOfxStatOK;
}

OfxPropertySuiteV1 propertySuite = {
    propSetPointer, propSetString, propSetDouble, propSetInt,
    propSetPointerN, propSetStringN, propSetDoubleN, propSetIntN,
    propGetPointer, propGetString, propGetDouble, propGetInt,
    propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN,
    propReset, propGetDimension};

// ---------------------------------------------------------------------------
// Parameters, clips and effects.
// ---------------------------------------------------------------------------
int doubleCount(const std::string &type) {
  if (type == kOfxParamTypeDouble) return 1;
  if (type == kOfxParamTypeDouble2D) return 2;
  if (type == kOfxParamTypeDouble3D || type == kOfxParamTypeRGB) return 3;
  if (type == kOfxParamTypeRGBA) return 4;
  return 0;
}
int intCount(const std::string &type) {
  if (type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice) return 1;
  if (type == kOfxParamTypeInteger2D) return 2;
  if (type == kOfxParamTypeInteger3D) return 3;
  return 0;
}
bool isText(const std::string &type) {
  return type == kOfxParamTypeString || type == kOfxParamTypeCustom || type == kOfxParamTypeStrChoice;
}
// A parameter that holds something a project keeps: a number or more, or text.
bool holdsValue(const std::string &type) { return doubleCount(type) || intCount(type) || isText(type); }

struct Effect;
struct Param {
  std::string type, name;
  PropertySet props;
  Effect *owner = nullptr; // an instance's: the copy of the effect it is in
  double d[4] = {0, 0, 0, 0};
  int i[4] = {0, 0, 0, 0};
  std::string s;
  // A curve parameter: per curve, its control points (position, value) in
  // order of position.
  std::vector<std::vector<std::pair<double, double>>> curves;
  void loadDefault() {
    for (int k = 0; k < doubleCount(type); ++k) d[k] = props.getDouble(kOfxParamPropDefault, k, d[k]);
    for (int k = 0; k < intCount(type); ++k) i[k] = props.getInt(kOfxParamPropDefault, k, i[k]);
    if (isText(type)) s = props.getString(kOfxParamPropDefault, 0, "");
  }
};

// A parameter as text, and back: what the host keeps of one it shows no
// control for -- a plug-in's own data in a Custom parameter (a look made in
// the plug-in's window, a tracked path) or one it keeps secret. Text as it is;
// numbers as numbers, one after another.
std::string valueText(const Param &p) {
  if (isText(p.type)) return p.s;
  std::string text;
  char number[40];
  for (int k = 0; k < doubleCount(p.type); ++k) {
    std::snprintf(number, sizeof number, "%.17g", p.d[k]);
    text += (text.empty() ? "" : " ") + std::string(number);
  }
  for (int k = 0; k < intCount(p.type); ++k) text += (text.empty() ? "" : " ") + std::to_string(p.i[k]);
  return text;
}
// True when it changed the parameter.
bool setValueText(Param &p, const std::string &text) {
  if (isText(p.type)) {
    if (p.s == text) return false;
    p.s = text;
    return true;
  }
  std::istringstream in(text);
  bool changed = false;
  for (int k = 0; k < doubleCount(p.type); ++k) {
    double v = 0;
    if (!(in >> v)) break;
    changed = changed || v != p.d[k];
    p.d[k] = v;
  }
  for (int k = 0; k < intCount(p.type); ++k) {
    int v = 0;
    if (!(in >> v)) break;
    changed = changed || v != p.i[k];
    p.i[k] = v;
  }
  return changed;
}
// Its default, as text, from what the plug-in described.
std::string defaultText(const PropertySet &props, const std::string &type) {
  if (isText(type)) return props.getString(kOfxParamPropDefault, 0, "");
  std::string text;
  char number[40];
  for (int k = 0; k < doubleCount(type); ++k) {
    std::snprintf(number, sizeof number, "%.17g", props.getDouble(kOfxParamPropDefault, k, 0));
    text += (text.empty() ? "" : " ") + std::string(number);
  }
  for (int k = 0; k < intCount(type); ++k)
    text += (text.empty() ? "" : " ") + std::to_string(props.getInt(kOfxParamPropDefault, k, 0));
  return text;
}

// A parameter's properties before the plug-in sets any, as the standard has
// them for each type (and as the Support library's validation checks them).
void defineParamProperties(PropertySet &p, const std::string &type, const std::string &name) {
  p.setString(kOfxPropType, {kOfxTypeParameter});
  p.setString(kOfxPropName, {name});
  p.setString(kOfxPropLabel, {name});
  p.setString(kOfxPropShortLabel, {name});
  p.setString(kOfxPropLongLabel, {name});
  p.setString(kOfxParamPropType, {type});
  p.setInt(kOfxParamPropSecret, {0});
  p.setString(kOfxParamPropHint, {""});
  p.setString(kOfxParamPropScriptName, {name});
  p.setString(kOfxParamPropParent, {""});
  p.setInt(kOfxParamPropEnabled, {1});
  p.setPointer(kOfxParamPropDataPtr, nullptr);
  p.setString(kOfxPropIcon, {"", ""});
  const int doubles = doubleCount(type), ints = intCount(type);
  const bool colour = type == kOfxParamTypeRGB || type == kOfxParamTypeRGBA;
  const bool parametric = type == kOfxParamTypeParametric;
  const bool valued = doubles || ints || isText(type) || parametric;
  // Every parameter but a group or a page can have its control drawn by the
  // plug-in instead (none is, here).
  if (type != kOfxParamTypeGroup && type != kOfxParamTypePage) {
    p.setPointer(kOfxParamPropInteractV1, nullptr);
    p.setDouble(kOfxParamPropInteractSize, {0, 0});
    p.setDouble(kOfxParamPropInteractSizeAspect, {1});
    p.setDouble(kOfxParamPropInteractMinimumSize, {10, 10});
    p.setInt(kOfxParamPropInteractPreferedSize, {10, 10});
  }
  if (valued) {
    // Animated by default only where this host animates the type at all.
    const bool steps = isText(type) || type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice;
    p.setInt(kOfxParamPropAnimates, {steps ? 0 : 1});
    p.setInt(kOfxParamPropIsAnimating, {0});
    p.setInt(kOfxParamPropIsAutoKeying, {0});
    p.setInt(kOfxParamPropPersistant, {1});
    p.setInt(kOfxParamPropEvaluateOnChange, {1});
    p.setInt(kOfxParamPropPluginMayWrite, {0});
    p.setString(kOfxParamPropCacheInvalidation, {kOfxParamInvalidateValueChange});
    p.setInt(kOfxParamPropCanUndo, {1});
  }
  if (doubles) {
    auto &def = p.define(kOfxParamPropDefault, Property::Double).doubles;
    def.assign(size_t(doubles), 0.);
    if (type == kOfxParamTypeRGBA) def[3] = 1;
    // A colour runs from 0 to 1 unless the plug-in says otherwise.
    const double low = colour ? 0. : -DBL_MAX, high = colour ? 1. : DBL_MAX;
    p.define(kOfxParamPropMin, Property::Double).doubles.assign(size_t(doubles), low);
    p.define(kOfxParamPropMax, Property::Double).doubles.assign(size_t(doubles), high);
    p.define(kOfxParamPropDisplayMin, Property::Double).doubles.assign(size_t(doubles), low);
    p.define(kOfxParamPropDisplayMax, Property::Double).doubles.assign(size_t(doubles), high);
    if (!colour) {
      p.setDouble(kOfxParamPropIncrement, {1});
      p.setInt(kOfxParamPropDigits, {2});
      p.setString(kOfxParamPropDoubleType, {kOfxParamDoubleTypePlain});
      p.setString(kOfxParamPropDefaultCoordinateSystem, {kOfxParamCoordinatesCanonical});
    }
    if (type == kOfxParamTypeDouble) p.setInt(kOfxParamPropShowTimeMarker, {0});
    if (type == kOfxParamTypeRGB) p.setString(kOfxParamPropDimensionLabel, {"r", "g", "b"});
    if (type == kOfxParamTypeRGBA) p.setString(kOfxParamPropDimensionLabel, {"r", "g", "b", "a"});
    if (type == kOfxParamTypeDouble2D) p.setString(kOfxParamPropDimensionLabel, {"x", "y"});
    if (type == kOfxParamTypeDouble3D) p.setString(kOfxParamPropDimensionLabel, {"x", "y", "z"});
  }
  if (ints) {
    p.define(kOfxParamPropDefault, Property::Int).ints.assign(size_t(ints), 0);
    if (type != kOfxParamTypeBoolean) {
      p.define(kOfxParamPropMin, Property::Int).ints.assign(size_t(ints), INT_MIN);
      p.define(kOfxParamPropMax, Property::Int).ints.assign(size_t(ints), INT_MAX);
      p.define(kOfxParamPropDisplayMin, Property::Int).ints.assign(size_t(ints), INT_MIN);
      p.define(kOfxParamPropDisplayMax, Property::Int).ints.assign(size_t(ints), INT_MAX);
    }
    if (type == kOfxParamTypeInteger2D) p.setString(kOfxParamPropDimensionLabel, {"x", "y"});
    if (type == kOfxParamTypeInteger3D) p.setString(kOfxParamPropDimensionLabel, {"x", "y", "z"});
  }
  if (type == kOfxParamTypeChoice || type == kOfxParamTypeStrChoice) p.empty(kOfxParamPropChoiceOption, Property::String);
  if (type == kOfxParamTypeStrChoice) p.empty(kOfxParamPropChoiceEnum, Property::String);
  if (isText(type)) p.setString(kOfxParamPropDefault, {""});
  // A string choice is a string as well to the Support library, which asks it
  // for these.
  if (type == kOfxParamTypeString || type == kOfxParamTypeStrChoice) {
    p.setString(kOfxParamPropStringMode, {kOfxParamStringIsSingleLine});
    p.setInt(kOfxParamPropStringFilePathExists, {1});
  }
  if (type == kOfxParamTypeCustom) p.setPointer(kOfxParamPropCustomInterpCallbackV1, nullptr);
  if (type == kOfxParamTypeGroup) p.setInt(kOfxParamPropGroupOpen, {1});
  if (type == kOfxParamTypePage) p.empty(kOfxParamPropPageChild, Property::String);
  if (parametric) {
    p.setInt(kOfxParamPropParametricDimension, {1});
    p.setDouble(kOfxParamPropParametricRange, {0, 1});
    p.empty(kOfxParamPropParametricUIColour, Property::Double);
    p.setPointer(kOfxParamPropParametricInteractBackground, nullptr);
  }
}

struct ParamSet {
  PropertySet props;
  std::vector<std::unique_ptr<Param>> params;
  Param *find(const char *name) const {
    if (!name) return nullptr;
    for (const auto &p : params)
      if (p->name == name) return p.get();
    return nullptr;
  }
};

struct Effect;
struct Clip {
  std::string name;
  PropertySet props;
  Effect *owner = nullptr;
};

// A texture handed to a plug-in drawing on the card (clipLoadTexture): its
// properties first, so their address is the texture's.
struct Binding;
struct Texture {
  PropertySet props;
  unsigned id = 0;
  bool owned = false; // made for it, and let go of with it; the output's is not
  Binding *binding = nullptr;
};
// The pictures of a render under way: what clipGetImage hands out.
struct Binding {
  void *source = nullptr, *output = nullptr;
  int rowBytes = 0, width = 0, height = 0;
  // The image's lower left corner in pixels, which is below and left of 0
  // when it reaches past the layer's own picture -- that is 0 to contentW
  // and contentH -- to leave room for a plug-in drawing beyond it.
  int x1 = 0, y1 = 0, contentW = 0, contentH = 0;
  std::string depth;
  double scale = 1, time = 0;
  // The clip that is the layer -- Source, or one side of a transition; none
  // for a generator -- and, for a transition, nothing on its other side.
  std::string layerClip;
  void *blank = nullptr;
  // The other pictures the effect takes (OfxControl::Input), by clip, bottom
  // row first in the plug-in's format.
  std::map<std::string, QImage> inputs;
  // Drawing on the card: the output's texture, and the textures handed out.
  bool card = false;
  unsigned cardOutput = 0;
  std::vector<std::unique_ptr<Texture>> textures;
  // The source at other times: where it comes from, the format the plug-in
  // reads (as `source` is), and each one fetched in this render, kept until it
  // ends -- a plug-in asks for the same frame for every strip it draws.
  const OfxOtherTimes *frames = nullptr;
  QImage::Format format = QImage::Format_Invalid;
  std::mutex fetchedGuard;
  std::map<double, QImage> fetched;
  // The parameters at other times, by time and name, as they were asked for;
  // and the size of the frame, for the controls set as fractions of it.
  std::map<double, std::map<std::string, std::unique_ptr<Param>>> paramSnapshots;
  double canonicalW = 0, canonicalH = 0;
  // The picture at `time` as the plug-in reads it, bottom row first; null when
  // there is none to be had.
  const QImage *otherFrame(double when) {
    if (!frames || !frames->at) return nullptr;
    if (frames->last >= frames->first) when = std::clamp(when, frames->first, frames->last);
    std::lock_guard<std::mutex> lock(fetchedGuard);
    auto known = fetched.find(when);
    if (known == fetched.end()) {
      const QImage made = frames->at(when);
      QImage ready;
      if (made.width() == width && made.height() == height) ready = made.convertToFormat(format).mirrored(false, true);
      if (!ready.isNull() && ready.bytesPerLine() != rowBytes) ready = QImage();
      known = fetched.emplace(when, std::move(ready)).first;
    }
    return known->second.isNull() ? nullptr : &known->second;
  }
};

struct Plugin;
struct Effect {
  Plugin *plugin = nullptr;
  bool instance = false;
  PropertySet props;
  ParamSet params;
  std::vector<std::unique_ptr<Clip>> clips;
  Clip *clip(const char *name) const {
    if (!name) return nullptr;
    for (const auto &c : clips)
      if (c->name == name) return c.get();
    return nullptr;
  }
  // An instance's own.
  std::mutex render;
  // The pixel depth its pictures are at (OfxBitDepthByte, Short, Float):
  // chosen for the frames it is given (depthFor).
  std::string depth;
  bool cardAttached = false; // told of the card's context (kOfxActionOpenGLContextAttached)
  Binding *binding = nullptr;
  std::vector<float> applied;
  bool usable = false;
  bool outputPremultiplied = true;
  std::mutex messageGuard;
  QString message;    // the last thing it said, for a render that fails
  QString persistent; // ofxEffectMessage: kept up until the plug-in clears it
  QStringList notices; // said in passing, for ofxTakeNotices
  // Hears the plug-in's progress while a button is answered; false stops it.
  std::function<bool(const QString &, double)> progress;
  QString progressLabel;
};
Effect *effectOf(OfxImageEffectHandle h) { return reinterpret_cast<Effect *>(h); }
OfxImageEffectHandle handleOf(Effect *e) { return reinterpret_cast<OfxImageEffectHandle>(e); }
ParamSet *paramSetOf(OfxParamSetHandle h) { return reinterpret_cast<ParamSet *>(h); }
Param *paramOf(OfxParamHandle h) { return reinterpret_cast<Param *>(h); }
Clip *clipOf(OfxImageClipHandle h) { return reinterpret_cast<Clip *>(h); }

// Messages arrive with a handle that may be an effect or anything else; only
// the effects made here are taken as effects.
std::mutex knownGuard;
std::set<const void *> knownEffects;
void know(Effect *e) {
  std::lock_guard<std::mutex> lock(knownGuard);
  knownEffects.insert(e);
}
Effect *knownEffect(const void *handle) {
  std::lock_guard<std::mutex> lock(knownGuard);
  return knownEffects.count(handle) ? const_cast<Effect *>(static_cast<const Effect *>(handle)) : nullptr;
}

// One how-to per control: which parameter it sets, and how.
struct Slot {
  // Direction: a transition's, set by this host -- 0 the layer leaves, 1 it arrives.
  // Offset: a retimer's, the frames its source time is from the layer's own.
  enum How { Double, Int, Bool, Choice, StrChoice, Colour, Direction, Offset };
  How how = Double;
  std::string param;
  int component = 0;
  int floats = 1;
  // 1 or 2: the person sets a fraction of the frame's width or height, and the
  // plug-in reads it in the frame's own coordinates.
  int normalise = 0;
  bool alpha = false;
  std::vector<std::string> enums;
};

struct Bundle {
  QString path;
#ifdef _WIN32
  HMODULE module = nullptr;
#endif
};

struct Plugin {
  Bundle *bundle = nullptr;
  OfxPlugin *ofx = nullptr;
  QString binary;
  std::unique_ptr<Effect> descriptor, contextDescriptor;
  // `depth` is the best it takes, for an instance with no frame yet to go by;
  // `depths` all it takes.
  std::string context, depth, threadSafety;
  std::vector<std::string> depths;
  // Drawing on the card: 0 not, 1 can, 2 only there; and whether it can draw
  // on the processor.
  int card = 0;
  bool processor = true;
  // Descriptors of contexts it was described in and that did not fit: kept,
  // since the plug-in may hold on to their handles.
  std::vector<std::unique_ptr<Effect>> triedContexts;
  bool multiResolution = true;
  std::atomic<bool> broken{false};
  // Told to unload as the application closes: nothing more is drawn with it.
  std::atomic<bool> unloaded{false};
  // Whether its output changes from frame to frame on its own: -1 until an
  // instance has been asked.
  std::atomic<int> varies{-1};
  std::mutex unsafe;
  std::mutex instancesGuard;
  std::map<QString, std::shared_ptr<Effect>> instances;
  std::vector<Slot> slots;
  std::vector<float> initial;
};
// The plug-in a handle names, when it is loaded in this process.
Plugin *localOf(const OfxEffectEntry &entry) {
  return entry.plugin ? static_cast<Plugin *>(entry.plugin->local) : nullptr;
}

// One control's value, `v` (as many floats as the slot takes), into the
// parameter it sets.
void setSlot(const Slot &s, Param &p, const float *v, double canonicalW, double canonicalH, double time) {
  switch (s.how) {
    case Slot::Double:
      p.d[s.component] = s.normalise == 1 ? v[0] * canonicalW : s.normalise == 2 ? v[0] * canonicalH : v[0];
      break;
    case Slot::Int: p.i[s.component] = int(std::lround(v[0])); break;
    case Slot::Bool: p.i[0] = v[0] >= .5f ? 1 : 0; break;
    case Slot::Choice: p.i[0] = int(std::lround(v[0])); break;
    case Slot::StrChoice:
      if (!s.enums.empty()) p.s = s.enums[size_t(std::clamp(int(std::lround(v[0])), 0, int(s.enums.size()) - 1))];
      break;
    case Slot::Colour:
      for (int k = 0; k < (s.alpha ? 4 : 3); ++k) p.d[k] = v[k];
      break;
    case Slot::Direction: break;
    case Slot::Offset: p.d[0] = time + v[0]; break;
  }
}

// --- Parameters at other times --------------------------------------------
// In a render the host was told the layer's controls at any time it asks for
// (OfxOtherTimes::values) and where their keyframes are. Outside a render, or
// without them, a parameter has the one value it was given for the frame
// being drawn, and no keyframes.
Binding *bindingOf(const Param *p) { return p && p->owner ? p->owner->binding : nullptr; }

// The parameter's keyframes: those of every control it is made of, in order.
std::vector<double> keysOf(const Param *p) {
  Binding *b = bindingOf(p);
  if (!b || !b->frames) return {};
  std::vector<double> keys;
  const auto &slots = p->owner->plugin->slots;
  for (size_t k = 0; k < slots.size(); ++k)
    if (slots[k].param == p->name && int(k) < b->frames->keys.size())
      for (double t : b->frames->keys[int(k)]) keys.push_back(t);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end(), [](double a, double c) { return std::abs(a - c) < 1e-6; }), keys.end());
  return keys;
}

// The parameter as it is at each of `when`: itself at the time being drawn,
// and wherever nothing else is known. The times not asked for before are asked
// for together -- through the plug-in host, one question.
std::vector<const Param *> paramsAt(Param *p, const std::vector<double> &when) {
  std::vector<const Param *> found(when.size(), p);
  Binding *b = bindingOf(p);
  if (!b || !b->frames || !b->frames->values) return found;
  const Plugin &plugin = *p->owner->plugin;
  if (std::none_of(plugin.slots.begin(), plugin.slots.end(), [&](const Slot &s) { return s.param == p->name; }))
    return found;
  auto other = [&](double t) { return std::abs(t - b->time) > 1e-9; };
  std::lock_guard<std::mutex> lock(b->fetchedGuard);
  QVector<double> missing;
  for (double t : when)
    if (other(t) && !b->paramSnapshots.count(t) && !missing.contains(t)) missing << t;
  if (!missing.isEmpty()) {
    const QVector<QVector<float>> got = b->frames->values(missing);
    for (int n = 0; n < missing.size(); ++n) {
      auto &snapshot = b->paramSnapshots[missing[n]];
      if (n >= got.size()) continue;
      const QVector<float> &v = got[n];
      size_t at = 0;
      for (const Slot &s : plugin.slots) {
        if (at + size_t(s.floats) > size_t(v.size())) break;
        const float *value = v.constData() + at;
        at += size_t(s.floats);
        if (s.how == Slot::Direction) continue;
        const Param *now = p->owner->params.find(s.param.c_str());
        if (!now) continue;
        auto &copy = snapshot[s.param];
        if (!copy) {
          copy = std::make_unique<Param>();
          copy->type = now->type;
          copy->name = now->name;
          std::copy(std::begin(now->d), std::end(now->d), std::begin(copy->d));
          std::copy(std::begin(now->i), std::end(now->i), std::begin(copy->i));
          copy->s = now->s;
        }
        setSlot(s, *copy, value, b->canonicalW, b->canonicalH, missing[n]);
      }
    }
  }
  for (size_t n = 0; n < when.size(); ++n) {
    if (!other(when[n])) continue;
    const auto at = b->paramSnapshots.find(when[n]);
    if (at == b->paramSnapshots.end()) continue;
    const auto named = at->second.find(p->name);
    if (named != at->second.end()) found[n] = named->second.get();
  }
  return found;
}

OfxStatus call(Plugin *plugin, const char *action, const void *handle, PropertySet *in, PropertySet *out) {
  if (!plugin || plugin->broken || !plugin->ofx || !plugin->ofx->mainEntry) return kOfxStatErrFatal;
#ifdef _WIN32
  int crashed = 0;
  const OfxStatus status = sehMain(plugin->ofx->mainEntry, action, handle, in ? handleOf(in) : nullptr,
                                   out ? handleOf(out) : nullptr, &crashed);
  if (crashed) {
    plugin->broken = true;
    qWarning("OFX: %s crashed in %s and is switched off", qPrintable(plugin->binary), action);
  }
  return status;
#else
  return plugin->ofx->mainEntry(action, handle, in ? handleOf(in) : nullptr, out ? handleOf(out) : nullptr);
#endif
}

// ---------------------------------------------------------------------------
// The image effect suite.
// ---------------------------------------------------------------------------
struct Image {
  PropertySet props;
  Binding *binding = nullptr;
};
std::mutex imagesGuard;
std::unordered_map<Image *, std::unique_ptr<Image>> liveImages;

// What every render-time action's arguments say about the card: not used.
void noGpu(PropertySet &args) {
  args.setInt(kOfxImageEffectPropOpenGLEnabled, {0});
  args.setInt(kOfxImageEffectPropCudaEnabled, {0});
  args.setPointer(kOfxImageEffectPropCudaStream, nullptr);
  args.setInt(kOfxImageEffectPropMetalEnabled, {0});
  args.setPointer(kOfxImageEffectPropMetalCommandQueue, nullptr);
  args.setInt(kOfxImageEffectPropOpenCLEnabled, {0});
  args.setPointer(kOfxImageEffectPropOpenCLCommandQueue, nullptr);
}

void defineClipProperties(PropertySet &p, const std::string &name) {
  p.setString(kOfxPropType, {kOfxTypeClip});
  p.setString(kOfxPropName, {name});
  p.setString(kOfxPropLabel, {name});
  p.setString(kOfxPropShortLabel, {name});
  p.setString(kOfxPropLongLabel, {name});
  p.empty(kOfxImageEffectPropSupportedComponents, Property::String);
  p.setInt(kOfxImageEffectPropTemporalClipAccess, {0});
  p.setInt(kOfxImageClipPropOptional, {0});
  p.setInt(kOfxImageClipPropIsMask, {0});
  p.setString(kOfxImageClipPropFieldExtraction, {kOfxImageFieldDoubled});
  p.setInt(kOfxImageEffectPropSupportsTiles, {1});
}

OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle *props) {
  if (!e || !props) return kOfxStatErrBadHandle;
  *props = handleOf(&effectOf(e)->props);
  return kOfxStatOK;
}
OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle *params) {
  if (!e || !params) return kOfxStatErrBadHandle;
  *params = reinterpret_cast<OfxParamSetHandle>(&effectOf(e)->params);
  return kOfxStatOK;
}
OfxStatus clipDefine(OfxImageEffectHandle e, const char *name, OfxPropertySetHandle *props) {
  if (!e || !name) return kOfxStatErrBadHandle;
  Effect *effect = effectOf(e);
  Clip *clip = effect->clip(name);
  if (!clip) {
    auto made = std::make_unique<Clip>();
    made->name = name;
    made->owner = effect;
    defineClipProperties(made->props, name);
    clip = made.get();
    effect->clips.push_back(std::move(made));
  }
  if (props) *props = handleOf(&clip->props);
  return kOfxStatOK;
}
OfxStatus clipGetHandle(OfxImageEffectHandle e, const char *name, OfxImageClipHandle *clip, OfxPropertySetHandle *props) {
  if (!e || !name || !clip) return kOfxStatErrBadHandle;
  Clip *found = effectOf(e)->clip(name);
  if (!found) return kOfxStatErrUnknown;
  *clip = reinterpret_cast<OfxImageClipHandle>(found);
  if (props) *props = handleOf(&found->props);
  return kOfxStatOK;
}
OfxStatus clipGetPropertySet(OfxImageClipHandle clip, OfxPropertySetHandle *props) {
  if (!clip || !props) return kOfxStatErrBadHandle;
  *props = handleOf(&clipOf(clip)->props);
  return kOfxStatOK;
}
OfxStatus clipGetImage(OfxImageClipHandle h, OfxTime time, const OfxRectD *, OfxPropertySetHandle *imageHandle) {
  if (!h || !imageHandle) return kOfxStatErrBadHandle;
  Clip *clip = clipOf(h);
  Effect *owner = clip->owner;
  Binding *b = owner ? owner->binding : nullptr;
  if (!b || !clip->props.getInt(kOfxImageClipPropConnected, 0, 0)) return kOfxStatFailed;
  const bool otherSide = clip->name == kOfxImageEffectTransitionSourceFromClipName ||
                         clip->name == kOfxImageEffectTransitionSourceToClipName;
  const auto input = b->inputs.find(clip->name);
  void *data = clip->name == kOfxImageEffectOutputClipName ? b->output
               : clip->name == b->layerClip                ? b->source
               : otherSide                                 ? b->blank
               : input != b->inputs.end()                  ? input->second.bits()
                                                           : nullptr;
  if (!data) return kOfxStatFailed;
  // Another frame of the source. Where there is none to be had -- no frames
  // given, or the layer could not be drawn then -- the one being drawn stands
  // in for it: a trail of echoes of the same picture rather than a failed
  // render.
  if (data == b->source && std::abs(time - b->time) > 1e-6)
    if (const QImage *other = b->otherFrame(time)) data = const_cast<uchar *>(other->constBits());
  auto image = std::make_unique<Image>();
  image->binding = b;
  PropertySet &p = image->props;
  static std::atomic<int> serial{0};
  p.setString(kOfxPropType, {kOfxTypeImage});
  p.setPointer(kOfxImagePropData, data);
  p.setPointer(kOfxImageEffectPropOpenCLImage, nullptr);
  p.setInt(kOfxImagePropBounds, {b->x1, b->y1, b->x1 + b->width, b->y1 + b->height});
  // A picture given is defined over the layer's own; the output over all of it.
  if (clip->name == kOfxImageEffectOutputClipName)
    p.setInt(kOfxImagePropRegionOfDefinition, {b->x1, b->y1, b->x1 + b->width, b->y1 + b->height});
  else
    p.setInt(kOfxImagePropRegionOfDefinition, {0, 0, b->contentW, b->contentH});
  p.setInt(kOfxImagePropRowBytes, {b->rowBytes});
  p.setString(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
  p.setString(kOfxImageEffectPropPixelDepth, {b->depth});
  p.setString(kOfxImageEffectPropPreMultiplication, {clip->name == kOfxImageEffectOutputClipName && !owner->outputPremultiplied
                                                         ? kOfxImageUnPreMultiplied : kOfxImagePreMultiplied});
  p.setDouble(kOfxImageEffectPropRenderScale, {b->scale, b->scale});
  p.setDouble(kOfxImagePropPixelAspectRatio, {1});
  p.setString(kOfxImagePropField, {kOfxImageFieldNone});
  p.setString(kOfxImagePropUniqueIdentifier, {clip->name + std::to_string(serial++)});
  p.setDouble(kOfxPropTime, {time});
  *imageHandle = handleOf(&image->props);
  std::lock_guard<std::mutex> lock(imagesGuard);
  Image *raw = image.get();
  liveImages[raw] = std::move(image);
  return kOfxStatOK;
}
OfxStatus clipReleaseImage(OfxPropertySetHandle h) {
  if (!h) return kOfxStatErrBadHandle;
  // The property set is the first member, so its address is the image's.
  Image *image = reinterpret_cast<Image *>(propertySet(h));
  std::lock_guard<std::mutex> lock(imagesGuard);
  return liveImages.erase(image) ? kOfxStatOK : kOfxStatErrBadHandle;
}
OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle h, OfxTime, OfxRectD *bounds) {
  if (!h || !bounds) return kOfxStatErrBadHandle;
  Clip *clip = clipOf(h);
  Effect *owner = clip->owner;
  // The pictures given are the layer's, its own size; the output reaches as far
  // as the image does.
  if (owner && owner->binding) {
    const Binding *b = owner->binding;
    if (clip->name == kOfxImageEffectOutputClipName)
      *bounds = {b->x1 / b->scale, b->y1 / b->scale, (b->x1 + b->width) / b->scale, (b->y1 + b->height) / b->scale};
    else
      *bounds = {0, 0, b->contentW / b->scale, b->contentH / b->scale};
    return kOfxStatOK;
  }
  const double w = owner ? owner->props.getDouble(kOfxImageEffectPropProjectSize, 0, 1920) : 0;
  const double hgt = owner ? owner->props.getDouble(kOfxImageEffectPropProjectSize, 1, 1080) : 0;
  *bounds = {0, 0, w, hgt};
  return kOfxStatOK;
}
int abortRender(OfxImageEffectHandle) { return 0; }
struct ImageMemory { void *data = nullptr; };
OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t bytes, OfxImageMemoryHandle *memory) {
  if (!memory) return kOfxStatErrBadHandle;
  auto *m = new ImageMemory;
#ifdef _WIN32
  m->data = _aligned_malloc(std::max<size_t>(bytes, 1), 64);
#else
  m->data = std::malloc(std::max<size_t>(bytes, 1));
#endif
  if (!m->data) { delete m; return kOfxStatErrMemory; }
  *memory = reinterpret_cast<OfxImageMemoryHandle>(m);
  return kOfxStatOK;
}
OfxStatus imageMemoryFree(OfxImageMemoryHandle memory) {
  if (!memory) return kOfxStatErrBadHandle;
  auto *m = reinterpret_cast<ImageMemory *>(memory);
#ifdef _WIN32
  _aligned_free(m->data);
#else
  std::free(m->data);
#endif
  delete m;
  return kOfxStatOK;
}
OfxStatus imageMemoryLock(OfxImageMemoryHandle memory, void **pointer) {
  if (!memory || !pointer) return kOfxStatErrBadHandle;
  *pointer = reinterpret_cast<ImageMemory *>(memory)->data;
  return kOfxStatOK;
}
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle memory) { return memory ? kOfxStatOK : kOfxStatErrBadHandle; }

// The OpenGL render suite: a clip's picture as a texture on the card. The
// output's is the texture behind the framebuffer the render draws into; any
// other is the picture clipGetImage would give, uploaded.
OfxStatus clipLoadTexture(OfxImageClipHandle h, OfxTime time, const char *, const OfxRectD *region,
                          OfxPropertySetHandle *textureHandle) {
  if (!h || !textureHandle) return kOfxStatErrBadHandle;
  Clip *clip = clipOf(h);
  Effect *owner = clip->owner;
  Binding *b = owner ? owner->binding : nullptr;
  if (!b || !b->card) return kOfxStatFailed;
  auto texture = std::make_unique<Texture>();
  texture->binding = b;
  PropertySet &p = texture->props;
  const bool output = clip->name == kOfxImageEffectOutputClipName;
  int bounds[4] = {b->x1, b->y1, b->x1 + b->width, b->y1 + b->height};
  if (output) {
    texture->id = b->cardOutput;
  } else {
    OfxPropertySetHandle image = nullptr;
    const OfxStatus got = clipGetImage(h, time, region, &image);
    if (got != kOfxStatOK) return got;
    const PropertySet &picture = *propertySet(image);
    const Property *at = picture.find(kOfxImagePropData);
    void *data = at && at->type == Property::Pointer && !at->pointers.empty() ? at->pointers[0] : nullptr;
    for (int k = 0; k < 4; ++k) bounds[k] = picture.getInt(kOfxImagePropBounds, k, bounds[k]);
    texture->id = ofx_gl::upload(data, bounds[2] - bounds[0], bounds[3] - bounds[1],
                                 picture.getInt(kOfxImagePropRowBytes), b->depth);
    texture->owned = true;
    clipReleaseImage(image);
    if (!texture->id) return kOfxStatGLOutOfMemory;
  }
  static std::atomic<int> serial{0};
  p.setString(kOfxPropType, {kOfxTypeImage});
  p.setInt(kOfxImageEffectPropOpenGLTextureIndex, {int(texture->id)});
  p.setInt(kOfxImageEffectPropOpenGLTextureTarget, {0x0DE1}); // GL_TEXTURE_2D
  p.setString(kOfxImageEffectPropPixelDepth, {b->depth});
  p.setString(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
  p.setString(kOfxImageEffectPropPreMultiplication, {output && !owner->outputPremultiplied ? kOfxImageUnPreMultiplied
                                                                                            : kOfxImagePreMultiplied});
  p.setDouble(kOfxImageEffectPropRenderScale, {b->scale, b->scale});
  p.setDouble(kOfxImagePropPixelAspectRatio, {1});
  p.setInt(kOfxImagePropBounds, {bounds[0], bounds[1], bounds[2], bounds[3]});
  if (output) p.setInt(kOfxImagePropRegionOfDefinition, {bounds[0], bounds[1], bounds[2], bounds[3]});
  else p.setInt(kOfxImagePropRegionOfDefinition, {0, 0, b->contentW, b->contentH});
  p.setString(kOfxImagePropField, {kOfxImageFieldNone});
  p.setString(kOfxImagePropUniqueIdentifier, {clip->name + "-texture" + std::to_string(serial++)});
  p.setDouble(kOfxPropTime, {time});
  *textureHandle = handleOf(&texture->props);
  b->textures.push_back(std::move(texture));
  return kOfxStatOK;
}
OfxStatus clipFreeTexture(OfxPropertySetHandle h) {
  if (!h) return kOfxStatErrBadHandle;
  Texture *texture = reinterpret_cast<Texture *>(propertySet(h));
  Binding *b = texture->binding;
  if (!b) return kOfxStatErrBadHandle;
  for (auto it = b->textures.begin(); it != b->textures.end(); ++it)
    if (it->get() == texture) {
      if (texture->owned) ofx_gl::drop(texture->id);
      b->textures.erase(it);
      return kOfxStatOK;
    }
  return kOfxStatErrBadHandle;
}
OfxStatus flushResources() { return kOfxStatOK; }
OfxImageEffectOpenGLRenderSuiteV1 openGLSuite = {clipLoadTexture, clipFreeTexture, flushResources};

OfxImageEffectSuiteV1 imageEffectSuite = {
    getPropertySet, getParamSet, clipDefine, clipGetHandle, clipGetPropertySet, clipGetImage,
    clipReleaseImage, clipGetRegionOfDefinition, abortRender, imageMemoryAlloc, imageMemoryFree,
    imageMemoryLock, imageMemoryUnlock};

// ---------------------------------------------------------------------------
// The parameter suite. Values are read and written through C varargs, in the
// kinds the parameter's type calls for.
// ---------------------------------------------------------------------------
OfxStatus paramDefine(OfxParamSetHandle set, const char *type, const char *name, OfxPropertySetHandle *props) {
  if (!set || !type || !name) return kOfxStatErrBadHandle;
  ParamSet *params = paramSetOf(set);
  if (params->find(name)) return kOfxStatErrExists;
  auto param = std::make_unique<Param>();
  param->type = type;
  param->name = name;
  defineParamProperties(param->props, type, name);
  if (props) *props = handleOf(&param->props);
  params->params.push_back(std::move(param));
  return kOfxStatOK;
}
OfxStatus paramGetHandle(OfxParamSetHandle set, const char *name, OfxParamHandle *param, OfxPropertySetHandle *props) {
  if (!set || !name || !param) return kOfxStatErrBadHandle;
  Param *found = paramSetOf(set)->find(name);
  if (!found) return kOfxStatErrUnknown;
  *param = reinterpret_cast<OfxParamHandle>(found);
  if (props) *props = handleOf(&found->props);
  return kOfxStatOK;
}
OfxStatus paramSetGetPropertySet(OfxParamSetHandle set, OfxPropertySetHandle *props) {
  if (!set || !props) return kOfxStatErrBadHandle;
  *props = handleOf(&paramSetOf(set)->props);
  return kOfxStatOK;
}
OfxStatus paramGetPropertySet(OfxParamHandle param, OfxPropertySetHandle *props) {
  if (!param || !props) return kOfxStatErrBadHandle;
  *props = handleOf(&paramOf(param)->props);
  return kOfxStatOK;
}
OfxStatus readValue(Param *p, va_list ap, double factor = 1) {
  if (!p) return kOfxStatErrBadHandle;
  const int doubles = doubleCount(p->type), ints = intCount(p->type);
  if (doubles) {
    for (int k = 0; k < doubles; ++k)
      if (double *out = va_arg(ap, double *)) *out = p->d[k] * factor;
  } else if (ints) {
    for (int k = 0; k < ints; ++k)
      if (int *out = va_arg(ap, int *)) *out = int(p->i[k] * factor);
  } else if (isText(p->type)) {
    if (char **out = va_arg(ap, char **)) *out = const_cast<char *>(p->s.c_str());
  } else {
    return kOfxStatErrBadHandle;
  }
  return kOfxStatOK;
}
OfxStatus writeValue(Param *p, va_list ap) {
  if (!p) return kOfxStatErrBadHandle;
  const int doubles = doubleCount(p->type), ints = intCount(p->type);
  if (doubles) {
    for (int k = 0; k < doubles; ++k) p->d[k] = va_arg(ap, double);
  } else if (ints) {
    for (int k = 0; k < ints; ++k) p->i[k] = va_arg(ap, int);
  } else if (isText(p->type)) {
    const char *text = va_arg(ap, const char *);
    p->s = text ? text : "";
  } else {
    return kOfxStatErrBadHandle;
  }
  return kOfxStatOK;
}
OfxStatus paramGetValue(OfxParamHandle h, ...) {
  va_list ap;
  va_start(ap, h);
  const OfxStatus status = readValue(paramOf(h), ap);
  va_end(ap);
  return status;
}
OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime time, ...) {
  if (!h) return kOfxStatErrBadHandle;
  const Param *at = paramsAt(paramOf(h), {time})[0];
  va_list ap;
  va_start(ap, time);
  const OfxStatus status = readValue(const_cast<Param *>(at), ap);
  va_end(ap);
  return status;
}
// A number parameter's components, as doubles.
double component(const Param *p, int k) { return doubleCount(p->type) ? p->d[k] : double(p->i[k]); }
// `values`, one per component, out through the caller's pointers.
OfxStatus writeNumbers(const Param *p, va_list ap, const double *values) {
  const int doubles = doubleCount(p->type), ints = intCount(p->type);
  if (doubles) {
    for (int k = 0; k < doubles; ++k)
      if (double *out = va_arg(ap, double *)) *out = values[k];
  } else if (ints) {
    for (int k = 0; k < ints; ++k)
      if (int *out = va_arg(ap, int *)) *out = int(std::lround(values[k]));
  } else {
    return kOfxStatErrBadHandle;
  }
  return kOfxStatOK;
}
// How fast it changes at `time`, per frame: nothing unless it is animated,
// and then from the values half a frame to either side.
OfxStatus paramGetDerivative(OfxParamHandle h, OfxTime time, ...) {
  if (!h) return kOfxStatErrBadHandle;
  Param *p = paramOf(h);
  va_list ap;
  va_start(ap, time);
  OfxStatus status;
  if (keysOf(p).empty()) {
    status = readValue(p, ap, 0);
  } else {
    constexpr double half = .5;
    const auto around = paramsAt(p, {time - half, time + half});
    double rate[4] = {0, 0, 0, 0};
    for (int k = 0; k < std::max(doubleCount(p->type), intCount(p->type)); ++k)
      rate[k] = (component(around[1], k) - component(around[0], k)) / (2 * half);
    status = writeNumbers(p, ap, rate);
  }
  va_end(ap);
  return status;
}
// Its sum from `time1` to `time2`: the value times the span unless it is
// animated, and then by the trapezium rule over every frame and keyframe in
// between (exact where it changes in straight lines from key to key).
OfxStatus paramGetIntegral(OfxParamHandle h, OfxTime time1, OfxTime time2, ...) {
  if (!h) return kOfxStatErrBadHandle;
  Param *p = paramOf(h);
  va_list ap;
  va_start(ap, time2);
  OfxStatus status;
  const std::vector<double> keys = keysOf(p);
  if (keys.empty() || time1 == time2) {
    status = readValue(p, ap, time2 - time1);
  } else {
    const double from = std::min(time1, time2), to = std::max(time1, time2);
    // A frame at a time, or coarser over a very long span.
    const double step = std::max(1., (to - from) / 20000);
    std::vector<double> at{from, to};
    for (double t = std::floor(from / step) * step + step; t < to; t += step) at.push_back(t);
    for (double k : keys)
      if (k > from && k < to) at.push_back(k);
    std::sort(at.begin(), at.end());
    at.erase(std::unique(at.begin(), at.end()), at.end());
    const auto values = paramsAt(p, at);
    double sum[4] = {0, 0, 0, 0};
    const int count = std::max(doubleCount(p->type), intCount(p->type));
    for (size_t n = 0; n + 1 < at.size(); ++n)
      for (int k = 0; k < count; ++k)
        sum[k] += (component(values[n], k) + component(values[n + 1], k)) * .5 * (at[n + 1] - at[n]);
    if (time2 < time1)
      for (double &v : sum) v = -v;
    status = writeNumbers(p, ap, sum);
  }
  va_end(ap);
  return status;
}
OfxStatus paramSetValue(OfxParamHandle h, ...) {
  va_list ap;
  va_start(ap, h);
  const OfxStatus status = writeValue(paramOf(h), ap);
  va_end(ap);
  return status;
}
OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime time, ...) {
  va_list ap;
  va_start(ap, time);
  const OfxStatus status = writeValue(paramOf(h), ap);
  va_end(ap);
  return status;
}
OfxStatus paramGetNumKeys(OfxParamHandle h, unsigned int *count) {
  if (!h || !count) return kOfxStatErrBadHandle;
  *count = unsigned(keysOf(paramOf(h)).size());
  return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle h, unsigned int nth, OfxTime *time) {
  if (!h || !time) return kOfxStatErrBadHandle;
  const auto keys = keysOf(paramOf(h));
  if (nth >= keys.size()) return kOfxStatErrBadIndex;
  *time = keys[nth];
  return kOfxStatOK;
}
// The key at `time` (direction 0), the last one before it (-1), or the first
// one after it (1).
OfxStatus paramGetKeyIndex(OfxParamHandle h, OfxTime time, int direction, int *index) {
  if (!h || !index) return kOfxStatErrBadHandle;
  const auto keys = keysOf(paramOf(h));
  int found = -1;
  for (int k = 0; k < int(keys.size()); ++k) {
    if (direction == 0 && std::abs(keys[size_t(k)] - time) < 1e-6) found = k;
    if (direction < 0 && keys[size_t(k)] < time - 1e-6) found = k;
    if (direction > 0 && keys[size_t(k)] > time + 1e-6 && found < 0) found = k;
  }
  if (found < 0) return kOfxStatFailed;
  *index = found;
  return kOfxStatOK;
}
OfxStatus paramDeleteKey(OfxParamHandle h, OfxTime) { return h ? kOfxStatOK : kOfxStatErrBadHandle; }
OfxStatus paramDeleteAllKeys(OfxParamHandle h) { return h ? kOfxStatOK : kOfxStatErrBadHandle; }
OfxStatus paramCopy(OfxParamHandle to, OfxParamHandle from, OfxTime, const OfxRangeD *) {
  if (!to || !from) return kOfxStatErrBadHandle;
  Param *a = paramOf(to), *b = paramOf(from);
  if (a->type != b->type) return kOfxStatErrValue;
  std::copy(std::begin(b->d), std::end(b->d), std::begin(a->d));
  std::copy(std::begin(b->i), std::end(b->i), std::begin(a->i));
  a->s = b->s;
  return kOfxStatOK;
}
OfxStatus paramEditBegin(OfxParamSetHandle set, const char *) { return set ? kOfxStatOK : kOfxStatErrBadHandle; }
OfxStatus paramEditEnd(OfxParamSetHandle set) { return set ? kOfxStatOK : kOfxStatErrBadHandle; }

OfxParameterSuiteV1 parameterSuite = {
    paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet, paramGetValue,
    paramGetValueAtTime, paramGetDerivative, paramGetIntegral, paramSetValue, paramSetValueAtTime,
    paramGetNumKeys, paramGetKeyTime, paramGetKeyIndex, paramDeleteKey, paramDeleteAllKeys,
    paramCopy, paramEditBegin, paramEditEnd};

// ---------------------------------------------------------------------------
// Memory, threads, messages, progress and the timeline.
// ---------------------------------------------------------------------------
OfxStatus memoryAlloc(void *, size_t bytes, void **data) {
  if (!data) return kOfxStatErrBadHandle;
#ifdef _WIN32
  *data = _aligned_malloc(std::max<size_t>(bytes, 1), 64);
#else
  *data = std::malloc(std::max<size_t>(bytes, 1));
#endif
  return *data ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus memoryFree(void *data) {
#ifdef _WIN32
  _aligned_free(data);
#else
  std::free(data);
#endif
  return kOfxStatOK;
}
OfxMemorySuiteV1 memorySuite = {memoryAlloc, memoryFree};

thread_local unsigned threadIndex = 0;
thread_local bool spawnedThread = false;
// A plug-in's own threads, spawned for it: the calling thread takes the first
// share and new ones the rest. Not the render pool's workers -- this is
// usually called from inside one, and work handed back to that pool there
// runs on one core.
OfxStatus multiThread(OfxThreadFunctionV1 func, unsigned int count, void *argument) {
  if (!func) return kOfxStatErrBadHandle;
  const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
  if (count == 0) count = cpus;
  std::atomic<bool> crashed{false};
  auto run = [&](unsigned index) {
#ifdef _WIN32
    int failed = 0;
    sehThread(func, index, count, argument, &failed);
    if (failed) crashed = true;
#else
    func(index, count, argument);
#endif
  };
  std::vector<std::thread> threads;
  threads.reserve(count > 1 ? count - 1 : 0);
  for (unsigned index = 1; index < count; ++index)
    threads.emplace_back([&, index] {
      threadIndex = index;
      spawnedThread = true;
      run(index);
    });
  const unsigned before = threadIndex;
  threadIndex = 0;
  run(0);
  threadIndex = before;
  for (auto &t : threads) t.join();
  return crashed ? kOfxStatFailed : kOfxStatOK;
}
OfxStatus multiThreadNumCPUs(unsigned int *count) {
  if (!count) return kOfxStatErrBadHandle;
  *count = std::max(1u, std::thread::hardware_concurrency());
  return kOfxStatOK;
}
OfxStatus multiThreadIndex(unsigned int *index) {
  if (!index) return kOfxStatErrBadHandle;
  *index = threadIndex;
  return kOfxStatOK;
}
int multiThreadIsSpawnedThread(void) { return spawnedThread ? 1 : 0; }
struct Mutex { std::recursive_mutex m; };
OfxStatus mutexCreate(OfxMutexHandle *mutex, int lockCount) {
  if (!mutex) return kOfxStatErrBadHandle;
  auto *m = new Mutex;
  for (int k = 0; k < lockCount; ++k) m->m.lock();
  *mutex = reinterpret_cast<OfxMutexHandle>(m);
  return kOfxStatOK;
}
OfxStatus mutexDestroy(const OfxMutexHandle mutex) {
  if (!mutex) return kOfxStatErrBadHandle;
  delete reinterpret_cast<Mutex *>(mutex);
  return kOfxStatOK;
}
OfxStatus mutexLock(const OfxMutexHandle mutex) {
  if (!mutex) return kOfxStatErrBadHandle;
  reinterpret_cast<Mutex *>(mutex)->m.lock();
  return kOfxStatOK;
}
OfxStatus mutexUnLock(const OfxMutexHandle mutex) {
  if (!mutex) return kOfxStatErrBadHandle;
  reinterpret_cast<Mutex *>(mutex)->m.unlock();
  return kOfxStatOK;
}
OfxStatus mutexTryLock(const OfxMutexHandle mutex) {
  if (!mutex) return kOfxStatErrBadHandle;
  return reinterpret_cast<Mutex *>(mutex)->m.try_lock() ? kOfxStatOK : kOfxStatFailed;
}
OfxMultiThreadSuiteV1 multiThreadSuite = {multiThread, multiThreadNumCPUs, multiThreadIndex,
                                          multiThreadIsSpawnedThread, mutexCreate, mutexDestroy,
                                          mutexLock, mutexUnLock, mutexTryLock};

// What kind a message is, as Wavy-Flo shows it.
QString messageKind(const char *type) {
  if (type && !std::strcmp(type, kOfxMessageError)) return "error";
  if (type && !std::strcmp(type, kOfxMessageWarning)) return "warning";
  return "message";
}
// A message: logged, kept as the last thing said (what a failed render shows),
// and -- an error, a warning or a note, not a log line -- shown in passing, or
// kept up beside the effect when `persistent`.
OfxStatus note(void *handle, const char *type, const char *format, va_list ap, bool persistent) {
  char text[2048] = {0};
  if (format) std::vsnprintf(text, sizeof text, format, ap);
  qWarning("OFX %s: %s", type ? type : "message", text);
  const bool log = type && !std::strcmp(type, kOfxMessageLog);
  if (Effect *e = knownEffect(handle)) {
    std::lock_guard<std::mutex> lock(e->messageGuard);
    e->message = QString::fromUtf8(text);
    if (persistent) e->persistent = messageKind(type) + ": " + e->message;
    else if (!log && text[0] && e->notices.size() < 16) e->notices << e->message;
  }
  // Nobody is there to answer a question in the middle of a render.
  return type && !std::strcmp(type, kOfxMessageQuestion) ? kOfxStatReplyDefault : kOfxStatOK;
}
OfxStatus message(void *handle, const char *type, const char *, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  const OfxStatus status = note(handle, type, format, ap, false);
  va_end(ap);
  return status;
}
OfxStatus setPersistentMessage(void *handle, const char *type, const char *, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  const OfxStatus status = note(handle, type, format, ap, true);
  va_end(ap);
  return status;
}
OfxStatus clearPersistentMessage(void *handle) {
  if (Effect *e = knownEffect(handle)) {
    std::lock_guard<std::mutex> lock(e->messageGuard);
    e->persistent.clear();
  }
  return kOfxStatOK;
}
OfxMessageSuiteV1 messageSuiteV1 = {message};
OfxMessageSuiteV2 messageSuiteV2 = {message, setPersistentMessage, clearPersistentMessage};

// Progress: heard by whoever is waiting on the effect (a button pressed), and
// stopped when they say so; at any other time, nobody.
OfxStatus progressStart(void *handle, const char *label) {
  if (Effect *e = knownEffect(handle)) {
    e->progressLabel = QString::fromUtf8(label ? label : "");
    if (e->progress && !e->progress(e->progressLabel, 0)) return kOfxStatReplyNo;
  }
  return kOfxStatOK;
}
OfxStatus progressUpdate(void *handle, double fraction) {
  if (Effect *e = knownEffect(handle))
    if (e->progress && !e->progress(e->progressLabel, std::clamp(fraction, 0., 1.))) return kOfxStatReplyNo;
  return kOfxStatOK;
}
OfxStatus progressEnd(void *handle) {
  if (Effect *e = knownEffect(handle))
    if (e->progress) e->progress(e->progressLabel, 1);
  return kOfxStatOK;
}
OfxProgressSuiteV1 progressSuite = {progressStart, progressUpdate, progressEnd};
OfxStatus progressStartV2(void *handle, const char *label, const char *) { return progressStart(handle, label); }
OfxProgressSuiteV2 progressSuiteV2 = {progressStartV2, progressUpdate, progressEnd};

// No overlays are drawn in the viewer, so no interact is ever made: a plug-in
// that fetches the suite has it, and never has a handle to use it with.
OfxStatus interactSwapBuffers(OfxInteractHandle) { return kOfxStatErrBadHandle; }
OfxStatus interactRedraw(OfxInteractHandle) { return kOfxStatErrBadHandle; }
OfxStatus interactGetPropertySet(OfxInteractHandle, OfxPropertySetHandle *) { return kOfxStatErrBadHandle; }
OfxInteractSuiteV1 interactSuite = {interactSwapBuffers, interactRedraw, interactGetPropertySet};

// Curves: straight lines between the control points, held flat past the ends.
std::mutex curvesGuard;
std::vector<std::pair<double, double>> *curveOf(OfxParamHandle h, int index) {
  Param *p = paramOf(h);
  if (!p || p->type != kOfxParamTypeParametric || index < 0) return nullptr;
  const int dimension = std::max(1, p->props.getInt(kOfxParamPropParametricDimension, 0, 1));
  if (index >= dimension) return nullptr;
  if (int(p->curves.size()) < dimension) p->curves.resize(size_t(dimension));
  return &p->curves[size_t(index)];
}
OfxStatus curveValue(OfxParamHandle h, int index, OfxTime, double at, double *value) {
  if (!value) return kOfxStatErrBadHandle;
  std::lock_guard<std::mutex> lock(curvesGuard);
  auto *curve = curveOf(h, index);
  if (!curve) return kOfxStatErrBadIndex;
  if (curve->empty()) { *value = 0; return kOfxStatOK; }
  if (at <= curve->front().first) { *value = curve->front().second; return kOfxStatOK; }
  if (at >= curve->back().first) { *value = curve->back().second; return kOfxStatOK; }
  for (size_t k = 1; k < curve->size(); ++k) {
    const auto &a = (*curve)[k - 1], &b = (*curve)[k];
    if (at <= b.first) {
      const double span = b.first - a.first;
      *value = span > 0 ? a.second + (b.second - a.second) * (at - a.first) / span : b.second;
      return kOfxStatOK;
    }
  }
  *value = curve->back().second;
  return kOfxStatOK;
}
OfxStatus curvePoints(OfxParamHandle h, int index, double, int *count) {
  if (!count) return kOfxStatErrBadHandle;
  std::lock_guard<std::mutex> lock(curvesGuard);
  auto *curve = curveOf(h, index);
  if (!curve) return kOfxStatErrBadIndex;
  *count = int(curve->size());
  return kOfxStatOK;
}
OfxStatus curvePoint(OfxParamHandle h, int index, double, int nth, double *key, double *value) {
  std::lock_guard<std::mutex> lock(curvesGuard);
  auto *curve = curveOf(h, index);
  if (!curve || nth < 0 || nth >= int(curve->size())) return kOfxStatErrBadIndex;
  if (key) *key = (*curve)[size_t(nth)].first;
  if (value) *value = (*curve)[size_t(nth)].second;
  return kOfxStatOK;
}
void sortCurve(std::vector<std::pair<double, double>> &curve) {
  std::stable_sort(curve.begin(), curve.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
}
OfxStatus curveSetPoint(OfxParamHandle h, int index, double, int nth, double key, double value, bool) {
  std::lock_guard<std::mutex> lock(curvesGuard);
  auto *curve = curveOf(h, index);
  if (!curve || nth < 0 || nth >= int(curve->size())) return kOfxStatErrBadIndex;
  (*curve)[size_t(nth)] = {key, value};
  sortCurve(*curve);
  return kOfxStatOK;
}
OfxStatus curveAddPoint(OfxParamHandle h, int index, double, double key, double value, bool) {
  std::lock_guard<std::mutex> lock(curvesGuard);
  auto *curve = curveOf(h, index);
  if (!curve) return kOfxStatErrBadIndex;
  curve->push_back({key, value});
  sortCurve(*curve);
  return kOfxStatOK;
}
OfxStatus curveDeletePoint(OfxParamHandle h, int index, int nth) {
  std::lock_guard<std::mutex> lock(curvesGuard);
  auto *curve = curveOf(h, index);
  if (!curve || nth < 0 || nth >= int(curve->size())) return kOfxStatErrBadIndex;
  curve->erase(curve->begin() + nth);
  return kOfxStatOK;
}
OfxStatus curveDeleteAll(OfxParamHandle h, int index) {
  std::lock_guard<std::mutex> lock(curvesGuard);
  auto *curve = curveOf(h, index);
  if (!curve) return kOfxStatErrBadIndex;
  curve->clear();
  return kOfxStatOK;
}
OfxParametricParameterSuiteV1 parametricSuite = {curveValue, curvePoints, curvePoint, curveSetPoint,
                                                 curveAddPoint, curveDeletePoint, curveDeleteAll};

OfxStatus getTime(void *instance, double *time) {
  if (!time) return kOfxStatErrBadHandle;
  Effect *e = knownEffect(instance);
  *time = e && e->binding ? e->binding->time : 0;
  return kOfxStatOK;
}
OfxStatus gotoTime(void *, double) { return kOfxStatOK; }
OfxStatus getTimeBounds(void *, double *first, double *last) {
  if (first) *first = 0;
  if (last) *last = 100000;
  return kOfxStatOK;
}
OfxTimeLineSuiteV1 timeLineSuite = {getTime, gotoTime, getTimeBounds};

// ---------------------------------------------------------------------------
// The host: what it can do, and the suites it hands out.
// ---------------------------------------------------------------------------
PropertySet &hostProperties() {
  static PropertySet p;
  static std::once_flag once;
  std::call_once(once, [] {
    p.setString(kOfxPropType, {kOfxTypeImageEffectHost});
    p.setString(kOfxPropName, {"com.wavyflo.WavyFlo"});
    p.setString(kOfxPropLabel, {"Wavy-Flo"});
    p.setInt(kOfxPropAPIVersion, {1, 4});
    p.setInt(kOfxPropVersion, {0, 40, 1});
    p.setString(kOfxPropVersionLabel, {"0.40.1"});
    p.setInt(kOfxImageEffectHostPropIsBackground, {0});
    p.setInt(kOfxImageEffectPropSupportsOverlays, {0});
    p.setInt(kOfxImageEffectPropSupportsMultiResolution, {1});
    p.setInt(kOfxImageEffectPropSupportsTiles, {0});
    p.setInt(kOfxImageEffectPropTemporalClipAccess, {1});
    p.setString(kOfxImageEffectPropSupportedComponents, {kOfxImageComponentRGBA});
    p.setString(kOfxImageEffectPropSupportedContexts, {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral,
                                                       kOfxImageEffectContextTransition, kOfxImageEffectContextGenerator});
    p.setString(kOfxImageEffectPropSupportedPixelDepths, {kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthFloat});
    p.setInt(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
    p.setInt(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
    p.setInt(kOfxImageEffectPropSetableFrameRate, {0});
    p.setInt(kOfxImageEffectPropSetableFielding, {0});
    p.setInt(kOfxParamHostPropSupportsCustomInteract, {0});
    p.setInt(kOfxParamHostPropSupportsStringAnimation, {0});
    p.setInt(kOfxParamHostPropSupportsChoiceAnimation, {0});
    p.setInt(kOfxParamHostPropSupportsBooleanAnimation, {0});
    p.setInt(kOfxParamHostPropSupportsCustomAnimation, {0});
    p.setInt(kOfxParamHostPropSupportsStrChoice, {1});
    p.setInt(kOfxParamHostPropSupportsStrChoiceAnimation, {0});
    p.setInt(kOfxParamHostPropSupportsParametricAnimation, {0});
    p.setInt(kOfxParamHostPropMaxParameters, {-1});
    p.setInt(kOfxParamHostPropMaxPages, {0});
    p.setInt(kOfxParamHostPropPageRowColumnCount, {0, 0});
    p.setPointer(kOfxPropHostOSHandle, nullptr);
    p.setInt(kOfxImageEffectInstancePropSequentialRender, {0});
    // OpenGL, when the card gives a context (ofx_gl); a plug-in that can also
    // draw on the processor is drawn there when it does not.
    p.setString(kOfxImageEffectPropOpenGLRenderSupported, {"true"});
    p.setString(kOfxImageEffectPropCudaRenderSupported, {"false"});
    p.setString(kOfxImageEffectPropMetalRenderSupported, {"false"});
    p.setString(kOfxImageEffectPropOpenCLRenderSupported, {"false"});
    p.setString(kOfxImageEffectPropCudaStreamSupported, {"false"});
    p.setInt(kOfxImageEffectPropRenderQualityDraft, {1}); // asks for draft renders while a preview plays
    p.setString(kOfxImageEffectHostPropNativeOrigin, {kOfxHostNativeOriginBottomLeft});
  });
  return p;
}

const void *fetchSuite(OfxPropertySetHandle, const char *name, int version) {
  if (!name) return nullptr;
  if (!std::strcmp(name, kOfxPropertySuite) && version == 1) return &propertySuite;
  if (!std::strcmp(name, kOfxOpenGLRenderSuite) && version == 1) return &openGLSuite;
  if (!std::strcmp(name, kOfxImageEffectSuite) && version == 1) return &imageEffectSuite;
  if (!std::strcmp(name, kOfxParameterSuite) && version == 1) return &parameterSuite;
  if (!std::strcmp(name, kOfxMemorySuite) && version == 1) return &memorySuite;
  if (!std::strcmp(name, kOfxMultiThreadSuite) && version == 1) return &multiThreadSuite;
  if (!std::strcmp(name, kOfxMessageSuite) && version == 1) return &messageSuiteV1;
  if (!std::strcmp(name, kOfxMessageSuite) && version == 2) return &messageSuiteV2;
  if (!std::strcmp(name, kOfxProgressSuite) && version == 1) return &progressSuite;
  if (!std::strcmp(name, kOfxProgressSuite) && version == 2) return &progressSuiteV2;
  if (!std::strcmp(name, kOfxInteractSuite) && version == 1) return &interactSuite;
  if (!std::strcmp(name, kOfxParametricParameterSuite) && version == 1) return &parametricSuite;
  if (!std::strcmp(name, kOfxTimeLineSuite) && version == 1) return &timeLineSuite;
  noteGap("suite " + std::string(name) + " v" + std::to_string(version));
  return nullptr;
}

OfxHost &host() {
  static OfxHost h = {handleOf(&hostProperties()), fetchSuite};
  return h;
}

// ---------------------------------------------------------------------------
// Loading: bundles, the plug-ins in them, and what each can be set to.
// ---------------------------------------------------------------------------
std::mutex registryGuard;
std::vector<std::unique_ptr<Bundle>> bundles;
std::vector<std::unique_ptr<Plugin>> loadedPlugins;
// Loaded, then turned away (a transition, a feature this host lacks): kept
// until the application closes and unloaded with the rest. Told to unload
// straight after describing itself, one effect of a commercial suite ends the
// process from inside its own code.
std::vector<std::unique_ptr<Plugin>> turnedAway;
std::map<QString, std::vector<OfxEffectEntry>> loadedFolders;

// Plug-ins turned away for what this host cannot do -- a transition, a
// feature it lacks -- written down so the next start does not load them again
// to hear the same answer (one commercial suite alone is a hundred of them,
// seconds at every start). Kept per binary, by its name, size and date, so a
// changed plug-in is asked again; and asked again anyway after two weeks, or
// when this host learns more (kRefusalsHost). Not remembered: a plug-in that
// refused to load (licensing is decided there, and a licence can be bought)
// or that could not be loaded at all.
constexpr int kRefusalsHost = 3; // 2: other frames are offered; 3: transitions and generators
constexpr qint64 kRefusalsDays = 14;
QString refusalsFile;
QJsonObject refusals;
bool refusalsChanged = false;
QString bundleStamp(const QString &binary) {
  const QFileInfo info(binary);
  return QString("%1|%2|%3").arg(info.fileName()).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
}
bool lasting(const QString &problem) {
  return !problem.startsWith("refused to load") && !problem.startsWith("could not be loaded") &&
         !problem.startsWith("crashed while loading");
}

void defineEffectProperties(PropertySet &p, const QString &binary) {
  p.setString(kOfxPropType, {kOfxTypeImageEffect});
  p.setString(kOfxPropLabel, {""});
  p.setString(kOfxPropShortLabel, {""});
  p.setString(kOfxPropLongLabel, {""});
  p.empty(kOfxPropVersion, Property::Int);
  p.setString(kOfxPropVersionLabel, {""});
  p.setString(kOfxPropPluginDescription, {""});
  p.empty(kOfxImageEffectPropSupportedContexts, Property::String);
  p.setString(kOfxImageEffectPluginPropGrouping, {""});
  p.setInt(kOfxImageEffectPluginPropSingleInstance, {0});
  p.setString(kOfxImageEffectPluginRenderThreadSafety, {kOfxImageEffectRenderInstanceSafe});
  p.setInt(kOfxImageEffectPluginPropHostFrameThreading, {0});
  p.setPointer(kOfxImageEffectPluginPropOverlayInteractV1, nullptr);
  p.setInt(kOfxImageEffectPropSupportsMultiResolution, {1});
  p.setInt(kOfxImageEffectPropSupportsTiles, {1});
  p.setInt(kOfxImageEffectPropTemporalClipAccess, {0});
  p.empty(kOfxImageEffectPropSupportedPixelDepths, Property::String);
  p.setInt(kOfxImageEffectPluginPropFieldRenderTwiceAlways, {1});
  p.setInt(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
  p.setInt(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
  p.empty(kOfxImageEffectPropClipPreferencesSlaveParam, Property::String);
  p.setString(kOfxPluginPropFilePath, {QDir::toNativeSeparators(binary).toStdString()});
  p.setString(kOfxImageEffectPropOpenGLRenderSupported, {"false"});
  p.setString(kOfxImageEffectPropCudaRenderSupported, {"false"});
  p.setString(kOfxImageEffectPropMetalRenderSupported, {"false"});
  p.setString(kOfxImageEffectPropOpenCLRenderSupported, {"false"});
}

QString label(const PropertySet &p, const std::string &fallback) {
  for (const char *key : {kOfxPropLabel, kOfxPropLongLabel, kOfxPropShortLabel}) {
    const std::string text = p.getString(key);
    if (!text.empty()) return QString::fromStdString(text);
  }
  return QString::fromStdString(fallback);
}
double finite(double v, double fallback) { return std::isfinite(v) ? v : fallback; }
double bounded(double v) { return std::clamp(finite(v, 0.), -1e6, 1e6); }

// What a parameter shows as, and how its value goes back: in the order the
// plug-in defined them, its secret and valueless ones left out.
void describeControls(Plugin &plugin, OfxEffectEntry &entry) {
  // A transition's progress is the host's to set, and the plug-in may hide
  // its parameter: here it is a control of its own, 0 to 1, with which way
  // the layer goes.
  const bool transition = plugin.context == kOfxImageEffectContextTransition;
  if (transition) {
    OfxControl progress;
    progress.kind = OfxControl::Number;
    progress.id = kOfxImageEffectTransitionParamName;
    progress.label = "Transition";
    progress.minimum = 0;
    progress.maximum = 1;
    progress.step = .01;
    progress.initial = 0;
    Slot s;
    s.how = Slot::Double;
    s.param = kOfxImageEffectTransitionParamName;
    entry.controls.push_back(progress);
    plugin.slots.push_back(s);
    plugin.initial.push_back(0);
    OfxControl direction;
    direction.kind = OfxControl::Choice;
    direction.id = "WavyFlo.TransitionDirection";
    direction.label = "Layer";
    direction.choices = {"Leaves (from the layer to nothing)", "Arrives (from nothing to the layer)"};
    direction.minimum = 0;
    direction.maximum = 1;
    direction.initial = 0;
    Slot d;
    d.how = Slot::Direction;
    entry.controls.push_back(direction);
    plugin.slots.push_back(d);
    plugin.initial.push_back(0);
  }
  // A retimer's source time is the host's to set: here the layer's own time,
  // moved by an offset in frames that can be animated -- held, slowed, run
  // backwards by its keys.
  const bool retimer = plugin.context == kOfxImageEffectContextRetimer;
  if (retimer) {
    OfxControl offset;
    offset.kind = OfxControl::Number;
    offset.id = "WavyFlo.TimeOffset";
    offset.label = "Time Offset";
    offset.minimum = -100000;
    offset.maximum = 100000;
    offset.step = 1;
    offset.initial = 0;
    Slot s;
    s.how = Slot::Offset;
    s.param = kOfxImageEffectRetimerParamName;
    entry.controls.push_back(offset);
    plugin.slots.push_back(s);
    plugin.initial.push_back(0);
  }
  // The other pictures it takes, first: each is a layer to choose.
  for (const auto &clip : plugin.contextDescriptor->clips) {
    const std::string &n = clip->name;
    if (n == kOfxImageEffectOutputClipName || n == kOfxImageEffectSimpleSourceClipName ||
        (transition && (n == kOfxImageEffectTransitionSourceFromClipName || n == kOfxImageEffectTransitionSourceToClipName)))
      continue;
    OfxControl c;
    c.kind = OfxControl::Input;
    c.id = QString::fromStdString(n);
    c.label = label(clip->props, n);
    c.optional = clip->props.getInt(kOfxImageClipPropOptional) != 0;
    c.mask = clip->props.getInt(kOfxImageClipPropIsMask) != 0;
    entry.controls.push_back(c);
  }
  // The groups the plug-in made, and the group a parameter sits in (none for
  // one at the top, or one whose group is not there).
  const auto &all = plugin.contextDescriptor->params.params;
  std::set<std::string> groups;
  for (const auto &holder : all)
    if (holder->type == kOfxParamTypeGroup) groups.insert(holder->name);
  auto parentOf = [&](const Param &q) {
    const std::string parent = q.props.getString(kOfxParamPropParent);
    return groups.count(parent) && parent != q.name ? parent : std::string();
  };
  std::function<void(const Param &, int)> describeParam;
  // The parameters in `group` (the top when empty), in the order made.
  auto emitIn = [&](const std::string &group, int depth) {
    for (const auto &holder : all)
      if (parentOf(*holder) == group) describeParam(*holder, depth);
  };
  describeParam = [&](const Param &param, int depth) {
    const PropertySet &p = param.props;
    if (transition && param.name == kOfxImageEffectTransitionParamName) return;
    if (retimer && param.name == kOfxImageEffectRetimerParamName) return;
    if (p.getInt(kOfxParamPropSecret)) return;
    const QString name = QString::fromStdString(param.name);
    const QString shown = label(p, param.name);
    const std::string &type = param.type;
    auto number = [&](int component, const QString &id, const QString &text, bool whole, int normalise) {
      OfxControl c;
      c.kind = OfxControl::Number;
      c.id = id;
      c.label = text;
      const double low = whole ? p.getInt(kOfxParamPropMin, component, INT_MIN) : p.getDouble(kOfxParamPropMin, component, -DBL_MAX);
      const double high = whole ? p.getInt(kOfxParamPropMax, component, INT_MAX) : p.getDouble(kOfxParamPropMax, component, DBL_MAX);
      const double showLow = whole ? p.getInt(kOfxParamPropDisplayMin, component, INT_MIN) : p.getDouble(kOfxParamPropDisplayMin, component, -DBL_MAX);
      const double showHigh = whole ? p.getInt(kOfxParamPropDisplayMax, component, INT_MAX) : p.getDouble(kOfxParamPropDisplayMax, component, DBL_MAX);
      c.initial = whole ? p.getInt(kOfxParamPropDefault, component, 0) : p.getDouble(kOfxParamPropDefault, component, 0);
      if (normalise) {
        // A fraction of the frame, whatever size the layer is drawn at.
        c.minimum = std::max(-10., finite(low, -10.));
        c.maximum = std::min(10., finite(high, 10.));
        c.step = .001;
      } else {
        c.minimum = bounded(low);
        c.maximum = bounded(high);
        if (c.minimum > c.maximum) std::swap(c.minimum, c.maximum);
        const double increment = whole ? 1 : p.getDouble(kOfxParamPropIncrement, 0, 0);
        const double span = bounded(showHigh) - bounded(showLow);
        c.step = increment > 0 ? increment : span > 0 && span < 2e6 ? span / 100 : .1;
      }
      c.initial = std::clamp(finite(c.initial, 0.), c.minimum, c.maximum);
      Slot s;
      s.how = whole ? Slot::Int : Slot::Double;
      s.param = param.name;
      s.component = component;
      s.normalise = normalise;
      entry.controls.push_back(c);
      plugin.slots.push_back(s);
      plugin.initial.push_back(float(c.initial));
    };
    const std::string doubleType = p.getString(kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
    const bool normalisedDefault = p.getString(kOfxParamPropDefaultCoordinateSystem) == kOfxParamCoordinatesNormalised;
    if (type == kOfxParamTypeDouble) {
      int normalise = 0;
      if (normalisedDefault && (doubleType == kOfxParamDoubleTypeX || doubleType == kOfxParamDoubleTypeXAbsolute)) normalise = 1;
      if (normalisedDefault && (doubleType == kOfxParamDoubleTypeY || doubleType == kOfxParamDoubleTypeYAbsolute)) normalise = 2;
      number(0, name, shown, false, normalise);
    } else if (type == kOfxParamTypeInteger) {
      number(0, name, shown, true, 0);
    } else if (type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D || type == kOfxParamTypeDouble3D ||
               type == kOfxParamTypeInteger3D) {
      const bool whole = type == kOfxParamTypeInteger2D || type == kOfxParamTypeInteger3D;
      const bool point = normalisedDefault && (doubleType == kOfxParamDoubleTypeXY || doubleType == kOfxParamDoubleTypeXYAbsolute);
      const int count = type == kOfxParamTypeDouble3D || type == kOfxParamTypeInteger3D ? 3 : 2;
      static const char *axes[] = {"x", "y", "z"};
      // A position in the picture gets a handle on the preview: an absolute
      // point, or a point the plug-in gives as a fraction of the frame.
      const bool handle = type == kOfxParamTypeDouble2D &&
                          (doubleType == kOfxParamDoubleTypeXYAbsolute || (point && doubleType == kOfxParamDoubleTypeXY));
      for (int k = 0; k < count; ++k) {
        const QString axis = QString::fromStdString(p.getString(kOfxParamPropDimensionLabel, k, axes[k]));
        number(k, name + "/" + axes[k], shown + " " + axis.toUpper(), whole, point && k < 2 ? k + 1 : 0);
        if (handle) {
          entry.controls.back().point = name;
          entry.controls.back().axis = k;
          entry.controls.back().normalised = point;
        }
      }
    } else if (type == kOfxParamTypeBoolean) {
      OfxControl c;
      c.kind = OfxControl::Toggle;
      c.id = name;
      c.label = shown;
      c.initial = p.getInt(kOfxParamPropDefault) ? 1 : 0;
      Slot s;
      s.how = Slot::Bool;
      s.param = param.name;
      entry.controls.push_back(c);
      plugin.slots.push_back(s);
      plugin.initial.push_back(float(c.initial));
    } else if (type == kOfxParamTypeChoice || type == kOfxParamTypeStrChoice) {
      const auto options = p.strings(kOfxParamPropChoiceOption);
      if (options.empty()) return;
      OfxControl c;
      c.kind = OfxControl::Choice;
      c.id = name;
      c.label = shown;
      for (const auto &o : options) c.choices << QString::fromStdString(o);
      c.minimum = 0;
      c.maximum = double(options.size() - 1);
      Slot s;
      s.param = param.name;
      if (type == kOfxParamTypeStrChoice) {
        s.how = Slot::StrChoice;
        s.enums = p.strings(kOfxParamPropChoiceEnum);
        s.enums.resize(options.size());
        const std::string chosen = p.getString(kOfxParamPropDefault);
        const auto at = std::find(s.enums.begin(), s.enums.end(), chosen);
        c.initial = at == s.enums.end() ? 0 : double(at - s.enums.begin());
      } else {
        s.how = Slot::Choice;
        c.initial = std::clamp(p.getInt(kOfxParamPropDefault), 0, int(options.size()) - 1);
      }
      entry.controls.push_back(c);
      plugin.slots.push_back(s);
      plugin.initial.push_back(float(c.initial));
    } else if (type == kOfxParamTypeRGB || type == kOfxParamTypeRGBA) {
      OfxControl c;
      c.kind = OfxControl::Colour;
      c.id = name;
      c.label = shown;
      const bool alpha = type == kOfxParamTypeRGBA;
      for (int k = 0; k < 4; ++k)
        c.colour[k] = k < 3 || alpha ? std::clamp(p.getDouble(kOfxParamPropDefault, k, 1), 0., 1.) : 1.;
      Slot s;
      s.how = Slot::Colour;
      s.param = param.name;
      s.floats = 4;
      s.alpha = alpha;
      entry.controls.push_back(c);
      plugin.slots.push_back(s);
      for (double v : c.colour) plugin.initial.push_back(float(v));
    }
    // A group: its parameters fold under it -- and nothing is shown for one
    // with nothing in it to show.
    else if (type == kOfxParamTypeGroup) {
      if (depth > 16) return;
      OfxControl begin;
      begin.kind = OfxControl::GroupBegin;
      begin.id = name;
      begin.label = shown;
      begin.open = p.getInt(kOfxParamPropGroupOpen, 0, 1) != 0;
      const size_t before = entry.controls.size();
      entry.controls.push_back(begin);
      emitIn(param.name, depth + 1);
      if (entry.controls.size() == before + 1) {
        entry.controls.pop_back();
      } else {
        OfxControl close;
        close.kind = OfxControl::GroupEnd;
        close.id = name;
        entry.controls.push_back(close);
      }
    } else if (type == kOfxParamTypeString) {
      OfxControl c;
      c.kind = OfxControl::Text;
      c.id = name;
      c.label = shown;
      c.text = QString::fromStdString(p.getString(kOfxParamPropDefault));
      const std::string mode = p.getString(kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine);
      c.textMode = mode == kOfxParamStringIsMultiLine     ? OfxControl::MultiLine
                   : mode == kOfxParamStringIsFilePath    ? OfxControl::FilePath
                   : mode == kOfxParamStringIsDirectoryPath ? OfxControl::FolderPath
                   : mode == kOfxParamStringIsLabel       ? OfxControl::Label
                                                          : OfxControl::SingleLine;
      entry.controls.push_back(c);
    } else if (type == kOfxParamTypePushButton) {
      OfxControl c;
      c.kind = OfxControl::Button;
      c.id = name;
      c.label = shown;
      entry.controls.push_back(c);
    }
    // Custom data, pages and curves have no control here. (A page only says
    // where its parameters go on a panel of the plug-in's own design; they
    // are shown in the order made.)
  };
  emitIn(std::string(), 0);
  // What it keeps that has no control: its Custom parameters and the ones it
  // keeps secret -- where a plug-in with a window of its own puts what was
  // made there (a look suite, its look). Kept with the layer as text,
  // handed back at every render and part of the picture's identity, so a look
  // applied in that window is drawn at once and is still there when the
  // project is opened again.
  std::set<std::string> shown;
  for (const Slot &s : plugin.slots) shown.insert(s.param);
  for (const OfxControl &c : entry.controls)
    if (c.kind == OfxControl::Text) shown.insert(c.id.toStdString());
  for (const auto &holder : all) {
    if (!holdsValue(holder->type) || shown.count(holder->name)) continue;
    if (holder->name == kOfxImageEffectTransitionParamName || holder->name == kOfxImageEffectRetimerParamName) continue;
    if (!holder->props.getInt(kOfxParamPropPersistant, 0, 1)) continue;
    entry.hidden.insert(QString::fromStdString(holder->name),
                        QString::fromStdString(defaultText(holder->props, holder->type)));
  }
}

// One plug-in, described: null with `problem` when it cannot work on a layer.
std::unique_ptr<Plugin> describe(Bundle *bundle, OfxPlugin *ofx, OfxEffectEntry &entry, QString &problem) {
  auto plugin = std::make_unique<Plugin>();
  plugin->bundle = bundle;
  plugin->ofx = ofx;
  plugin->binary = bundle->path;
  const std::string id = ofx->pluginIdentifier ? ofx->pluginIdentifier : "";
#ifdef _WIN32
  int crashed = 0;
  sehSetHost(ofx->setHost, &host(), &crashed);
  if (crashed) { problem = "crashed when given the host"; return nullptr; }
#else
  ofx->setHost(&host());
#endif
  OfxStatus status = call(plugin.get(), kOfxActionLoad, nullptr, nullptr, nullptr);
  if (status != kOfxStatOK && status != kOfxStatReplyDefault) {
    problem = QString("refused to load (%1)").arg(status);
    return nullptr;
  }
  // Loaded: from here on, one that is turned away is still unloaded, when
  // everything is (ofxUnloadAll).
  auto reject = [&](const QString &why) -> std::unique_ptr<Plugin> {
    problem = why;
    turnedAway.push_back(std::move(plugin));
    return nullptr;
  };
  plugin->descriptor = std::make_unique<Effect>();
  plugin->descriptor->plugin = plugin.get();
  defineEffectProperties(plugin->descriptor->props, bundle->path);
  know(plugin->descriptor.get());
  status = call(plugin.get(), kOfxActionDescribe, handleOf(plugin->descriptor.get()), nullptr, nullptr);
  if (status != kOfxStatOK && status != kOfxStatReplyDefault) {
    return reject(QString("could not describe itself (%1)").arg(status));
  }
  const PropertySet &described = plugin->descriptor->props;
  const auto contexts = described.strings(kOfxImageEffectPropSupportedContexts);
  // The contexts it has, best first for an effect on a layer; each described
  // in turn until one fits: a general effect may want two pictures (a
  // transition that is also a general effect), and then is better as the
  // transition.
  QString why;
  const Clip *layer = nullptr;
  for (const char *wanted : {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral,
                             kOfxImageEffectContextTransition, kOfxImageEffectContextGenerator,
                             kOfxImageEffectContextRetimer}) {
    if (std::find(contexts.begin(), contexts.end(), wanted) == contexts.end()) continue;
    auto tried = std::make_unique<Effect>();
    tried->plugin = plugin.get();
    tried->props = described;
    tried->props.setString(kOfxImageEffectPropContext, {wanted});
    know(tried.get());
    PropertySet in;
    in.setString(kOfxImageEffectPropContext, {wanted});
    status = call(plugin.get(), kOfxImageEffectActionDescribeInContext, handleOf(tried.get()), &in, nullptr);
    const Effect &context = *tried;
    const bool transition = !std::strcmp(wanted, kOfxImageEffectContextTransition);
    const bool generator = !std::strcmp(wanted, kOfxImageEffectContextGenerator);
    const bool retimer = !std::strcmp(wanted, kOfxImageEffectContextRetimer);
    // The clips it reads the layer from, and the one other side of a transition.
    const std::vector<std::string> given =
        transition ? std::vector<std::string>{kOfxImageEffectTransitionSourceFromClipName, kOfxImageEffectTransitionSourceToClipName}
        : generator ? std::vector<std::string>{}
                    : std::vector<std::string>{kOfxImageEffectSimpleSourceClipName};
    const Clip *first = given.empty() ? nullptr : context.clip(given[0].c_str());
    QString problem;
    if (plugin->broken) problem = "crashed while describing itself";
    else if (status != kOfxStatOK && status != kOfxStatReplyDefault) problem = QString("could not describe itself as an effect (%1)").arg(status);
    else if (!context.clip(kOfxImageEffectOutputClipName)) problem = "has no picture to draw into";
    else {
      for (const auto &name : given)
        if (!context.clip(name.c_str())) problem = "has no picture to work on";
      // Any other picture it takes is an input, chosen from the other
      // layers (OfxControl::Input) -- needed or not.
    }
    if (problem.isEmpty() && first) {
      const auto components = first->props.strings(kOfxImageEffectPropSupportedComponents);
      if (!components.empty() && std::find(components.begin(), components.end(), kOfxImageComponentRGBA) == components.end())
        problem = "does not work on colour pictures";
    }
    if (!problem.isEmpty()) {
      if (why.isEmpty()) why = problem;
      plugin->triedContexts.push_back(std::move(tried));
      if (plugin->broken) break;
      continue;
    }
    plugin->context = wanted;
    plugin->contextDescriptor = std::move(tried);
    layer = first;
    entry.kind = transition ? "transition" : generator ? "generator" : retimer ? "retimer" : "filter";
    break;
  }
  if (plugin->context.empty())
    return reject(!why.isEmpty() ? why : "is not an effect on a layer (a paint effect or the like)");
  const auto depths = described.strings(kOfxImageEffectPropSupportedPixelDepths);
  for (const char *wanted : {kOfxBitDepthFloat, kOfxBitDepthShort, kOfxBitDepthByte})
    if (std::find(depths.begin(), depths.end(), wanted) != depths.end()) {
      plugin->depths.push_back(wanted);
      if (plugin->depth.empty()) plugin->depth = wanted;
    }
  if (plugin->depth.empty()) {
    plugin->depth = kOfxBitDepthByte;
    plugin->depths = {kOfxBitDepthByte};
  }
  plugin->threadSafety = described.getString(kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe);
  // Whether it draws on the card ("true"), only there ("needed"), and whether
  // it can draw on the processor at all.
  const PropertySet &chosen = plugin->contextDescriptor->props;
  const std::string onCard = chosen.getString(kOfxImageEffectPropOpenGLRenderSupported, 0, "false");
  plugin->card = onCard == "needed" ? 2 : onCard == "true" ? 1 : 0;
  plugin->processor = chosen.getString(kOfxImageEffectPropCPURenderSupported, 0, "true") != "false";
  entry.card = plugin->card;
  plugin->multiResolution = described.getInt(kOfxImageEffectPropSupportsMultiResolution, 0, 1) != 0;
  entry.name = label(described, id);
  entry.otherFrames = described.getInt(kOfxImageEffectPropTemporalClipAccess) != 0 ||
                      (layer && layer->props.getInt(kOfxImageEffectPropTemporalClipAccess) != 0);
  entry.group = QString::fromStdString(described.getString(kOfxImageEffectPluginPropGrouping));
  entry.identifier = QString::fromStdString(id);
  entry.binary = bundle->path;
  describeControls(*plugin, entry);
  return plugin;
}

void loadBundle(const QString &binary, std::vector<OfxEffectEntry> &found, QStringList *problems) {
  // What was remembered about it, if it is still the same file.
  const QString stamp = bundleStamp(binary);
  QJsonObject remembered;
  if (!refusalsFile.isEmpty()) {
    const QJsonObject entry = refusals.value(stamp).toObject();
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - qint64(entry.value("checked").toDouble());
    if (!entry.isEmpty() && age >= 0 && age < kRefusalsDays * 24 * 3600 * 1000) remembered = entry.value("plugins").toObject();
    else if (!entry.isEmpty()) {
      refusals.remove(stamp);
      refusalsChanged = true;
    }
  }
  const auto rememberedWhole = refusals.value(stamp).toObject().value("whole").toString();
  if (!rememberedWhole.isEmpty() && !remembered.isEmpty()) {
    if (problems) *problems << QString("%1: %2 (remembered)").arg(binary, rememberedWhole);
    return;
  }
  QJsonObject turned;          // this time's refusals that last
  bool anyTaken = false, anyAskedAgain = false;
  auto note = [&](const QString &id, const QString &problem) {
    if (problems) *problems << QString("%1 (%2): %3").arg(id, binary, problem);
    if (lasting(problem)) turned[id] = problem;
    else anyAskedAgain = true;
  };
  auto remember = [&](const QString &whole) {
    if (refusalsFile.isEmpty()) return;
    if (turned.isEmpty() && whole.isEmpty()) {
      if (refusals.contains(stamp)) { refusals.remove(stamp); refusalsChanged = true; }
      return;
    }
    // Unchanged since it was written: it keeps its date, so it is still asked
    // again when that is two weeks old.
    const QJsonObject before = refusals.value(stamp).toObject();
    if (before.value("plugins").toObject() == turned && before.value("whole").toString() == whole) return;
    QJsonObject entry{{"checked", double(QDateTime::currentMSecsSinceEpoch())}, {"plugins", turned}};
    if (!whole.isEmpty()) entry["whole"] = whole;
    refusals[stamp] = entry;
    refusalsChanged = true;
  };
  auto bundle = std::make_unique<Bundle>();
  bundle->path = binary;
#ifdef _WIN32
  int crashed = 0;
  const std::wstring path = QDir::toNativeSeparators(binary).toStdWString();
  bundle->module = sehLoad(path.c_str(), &crashed);
  if (!bundle->module) {
    if (problems) *problems << QString("%1: %2").arg(binary, crashed ? "crashed while loading" : "could not be loaded");
    return;
  }
  Bundle *kept = bundle.get();
  bundles.push_back(std::move(bundle));
  auto count = reinterpret_cast<int (*)(void)>(GetProcAddress(kept->module, "OfxGetNumberOfPlugins"));
  auto get = reinterpret_cast<OfxPlugin *(*)(int)>(GetProcAddress(kept->module, "OfxGetPlugin"));
  auto setHost = reinterpret_cast<OfxStatus (*)(const OfxHost *)>(GetProcAddress(kept->module, "OfxSetHost"));
  if (!count || !get) {
    if (problems) *problems << binary + ": not an OpenFX plug-in";
    turned["*"] = "not an OpenFX plug-in";
    remember("not an OpenFX plug-in");
    return;
  }
  if (setHost) {
    const OfxStatus status = sehSetHostAll(setHost, &host(), &crashed);
    if (crashed || (status != kOfxStatOK && status != kOfxStatReplyDefault)) {
      if (problems) *problems << QString("%1: refused the host (%2)").arg(binary).arg(status);
      return;
    }
  }
  const int n = sehCount(count, &crashed);
  if (crashed) {
    if (problems) *problems << binary + ": crashed when asked what it holds";
    return;
  }
  for (int nth = 0; nth < n; ++nth) {
    OfxPlugin *ofx = sehPlugin(get, nth, &crashed);
    if (crashed || !ofx) continue;
    const QString id = QString::fromUtf8(ofx->pluginIdentifier ? ofx->pluginIdentifier : "?");
    if (remembered.contains(id)) {
      // Turned away before, for something that has not changed.
      if (problems) *problems << QString("%1 (%2): %3 (remembered)").arg(id, binary, remembered.value(id).toString());
      turned[id] = remembered.value(id);
      continue;
    }
    if (!ofx->pluginApi || std::strcmp(ofx->pluginApi, kOfxImageEffectPluginApi) || ofx->apiVersion != 1 ||
        !ofx->setHost || !ofx->mainEntry) {
      note(id, "not an image effect");
      continue;
    }
    OfxEffectEntry entry;
    QString problem;
    auto plugin = describe(kept, ofx, entry, problem);
    if (!plugin) {
      note(id, problem);
      continue;
    }
    entry.plugin = std::make_shared<OfxPluginEffect>();
    entry.plugin->local = plugin.get();
    entry.plugin->key = entry.identifier + "|" + entry.binary;
    loadedPlugins.push_back(std::move(plugin));
    found.push_back(entry);
    anyTaken = true;
  }
  // Nothing in it this host can use, for reasons that last: next time the
  // library is not even loaded.
  remember(!anyTaken && !anyAskedAgain && n > 0 ? QString("nothing in it works here") : QString());
#else
  if (problems) *problems << binary + ": OpenFX is only read on Windows for now";
#endif
}

} // namespace

std::function<void(const QString &binary)> ofxBundleHook;
QStringList ofxSkippedBundles;

bool ofx_local::isBroken(const OfxEffectEntry &entry) {
  const Plugin *plugin = localOf(entry);
  return !plugin || plugin->broken;
}

void ofx_local::rememberBundleRefused(const QString &file, const QString &binary, const QString &why) {
  if (file.isEmpty()) return;
  std::lock_guard<std::mutex> lock(registryGuard);
  QJsonObject saved;
  {
    QFile in(file);
    if (in.open(QIODevice::ReadOnly)) saved = QJsonDocument::fromJson(in.readAll()).object();
  }
  QJsonObject bundles = saved.value("host").toInt() == kRefusalsHost ? saved.value("bundles").toObject() : QJsonObject();
  bundles[bundleStamp(binary)] = QJsonObject{{"checked", double(QDateTime::currentMSecsSinceEpoch())},
                                             {"plugins", QJsonObject{{"*", why}}},
                                             {"whole", why}};
  QDir().mkpath(QFileInfo(file).absolutePath());
  QSaveFile out(file);
  if (!out.open(QIODevice::WriteOnly)) return;
  out.write(QJsonDocument(QJsonObject{{"host", kRefusalsHost}, {"bundles", bundles}}).toJson());
  out.commit();
  // What is in memory here is read again from the file before the next load.
}

QStringList ofxPluginFolders() {
  // Wavy-Flo's own effects folder first, read like After Effects' Plug-ins
  // folder: a bundle dropped in it, or in a maker's folder inside it, is
  // found beside the .wfx effects. Then the "ofx" folder beside it, the
  // folders OFX_PLUGIN_PATH names, and the one every host reads. A plug-in
  // in more than one of them is taken from the first (loadEffects).
  QStringList folders{QCoreApplication::applicationDirPath() + "/effects",
                      QCoreApplication::applicationDirPath() + "/ofx"};
#ifdef _WIN32
  for (const QString &path : qEnvironmentVariable("OFX_PLUGIN_PATH").split(';', Qt::SkipEmptyParts))
    folders << QDir::fromNativeSeparators(path);
  const QString common = qEnvironmentVariable("CommonProgramFiles");
  if (!common.isEmpty()) folders << QDir::fromNativeSeparators(common) + "/OFX/Plugins";
#endif
  folders.removeDuplicates();
  return folders;
}

void ofx_local::unloadAll() {
  std::lock_guard<std::mutex> lock(registryGuard);
  for (auto &plugin : loadedPlugins) {
    // Nothing is drawn with it from here on.
    if (plugin->unloaded.exchange(true) || plugin->broken) continue;
    std::lock_guard<std::mutex> held(plugin->instancesGuard);
    for (auto &instance : plugin->instances) {
      Effect *e = instance.second.get();
      if (!e) continue;
      std::lock_guard<std::mutex> drawing(e->render);
      if (!e->usable) continue;
      if (e->cardAttached)
        ofx_gl::run([&] { call(plugin.get(), kOfxActionOpenGLContextDetached, handleOf(e), nullptr, nullptr); });
      call(plugin.get(), kOfxActionDestroyInstance, handleOf(e), nullptr, nullptr);
      e->usable = false;
    }
    call(plugin.get(), kOfxActionUnload, nullptr, nullptr, nullptr);
  }
  for (auto &plugin : turnedAway)
    if (!plugin->unloaded.exchange(true) && !plugin->broken)
      call(plugin.get(), kOfxActionUnload, nullptr, nullptr, nullptr);
#ifdef _WIN32
  // Then the libraries themselves, newest first.
  for (auto it = bundles.rbegin(); it != bundles.rend(); ++it) {
    Bundle &b = **it;
    if (!b.module) continue;
    int crashed = 0;
    sehFree(b.module, &crashed);
    if (crashed) qWarning("OFX: %s failed while being let go of", qPrintable(b.path));
    b.module = nullptr;
  }
#endif
}

void ofx_local::dropInstances(const std::function<bool(const QString &)> &keep) {
  std::vector<std::pair<Plugin *, std::shared_ptr<Effect>>> going;
  {
    std::lock_guard<std::mutex> lock(registryGuard);
    for (auto &plugin : loadedPlugins) {
      std::lock_guard<std::mutex> held(plugin->instancesGuard);
      for (auto it = plugin->instances.begin(); it != plugin->instances.end();) {
        if (keep(it->first)) { ++it; continue; }
        if (it->second) going.push_back({plugin.get(), it->second});
        it = plugin->instances.erase(it);
      }
    }
  }
  // Told outside the registry, each after any render still using it.
  for (auto &[plugin, e] : going) {
    std::lock_guard<std::mutex> drawing(e->render);
    if (!e->usable) continue;
    e->usable = false;
    if (!plugin->broken && !plugin->unloaded) {
      if (e->cardAttached)
        ofx_gl::run([&] { call(plugin, kOfxActionOpenGLContextDetached, handleOf(e.get()), nullptr, nullptr); });
      call(plugin, kOfxActionDestroyInstance, handleOf(e.get()), nullptr, nullptr);
    }
  }
}

int ofx_local::instanceCount() {
  std::lock_guard<std::mutex> lock(registryGuard);
  int count = 0;
  for (auto &plugin : loadedPlugins) {
    std::lock_guard<std::mutex> held(plugin->instancesGuard);
    count += int(plugin->instances.size());
  }
  return count;
}

QStringList ofx_local::hostGaps() {
  std::lock_guard<std::mutex> lock(gapsGuard);
  QStringList out;
  for (const auto &gap : gaps) out << QString::fromStdString(gap);
  return out;
}

void ofx_local::rememberRefusals(const QString &file, bool askAgain) {
  std::lock_guard<std::mutex> lock(registryGuard);
  refusalsFile = file;
  refusals = {};
  refusalsChanged = false;
  if (file.isEmpty()) return;
  QFile in(file);
  if (!askAgain && in.open(QIODevice::ReadOnly)) {
    const QJsonObject saved = QJsonDocument::fromJson(in.readAll()).object();
    if (saved.value("host").toInt() == kRefusalsHost) refusals = saved.value("bundles").toObject();
  }
  if (askAgain) refusalsChanged = true;
}

std::vector<OfxEffectEntry> ofx_local::loadEffects(const QStringList &folders, QStringList *problems, QSet<QString> *taken) {
  std::lock_guard<std::mutex> lock(registryGuard);
  struct Save {
    ~Save() {
      if (refusalsFile.isEmpty() || !refusalsChanged) return;
      QDir().mkpath(QFileInfo(refusalsFile).absolutePath());
      QSaveFile out(refusalsFile);
      if (!out.open(QIODevice::WriteOnly)) return;
      out.write(QJsonDocument(QJsonObject{{"host", kRefusalsHost}, {"bundles", refusals}}).toJson());
      if (out.commit()) refusalsChanged = false;
    }
  } save;
  // Unloaded as the application closes, while every plug-in is still there to
  // be told: some clean up after themselves then, and fail at exit if not.
  static bool unloadRegistered = false;
  if (!unloadRegistered && QCoreApplication::instance()) {
    qAddPostRoutine(ofxUnloadAll);
    unloadRegistered = true;
  }
  std::vector<OfxEffectEntry> all;
  // The same plug-in twice in folders read together (a copy in effects, the
  // installed one in Common Files; or in two makers' folders) is loaded once,
  // from the first place that has it.
  QSet<QString> takenHere;
  QSet<QString> &bundlesTaken = taken ? *taken : takenHere;
  for (const QString &folder : folders) {
    const QString key = QFileInfo(folder).absoluteFilePath();
    auto known = loadedFolders.find(key);
    if (known == loadedFolders.end()) {
      std::vector<OfxEffectEntry> found;
      QDirIterator bundlesIn(key, {"*.ofx.bundle"}, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
      while (bundlesIn.hasNext()) {
        const QString bundle = bundlesIn.next();
        const QString folderOfBinary = bundle + "/Contents/Win64";
        QString binary = folderOfBinary + "/" + QFileInfo(bundle).completeBaseName();
        if (!QFileInfo::exists(binary)) {
          const QStringList inside = QDir(folderOfBinary).entryList({"*.ofx"}, QDir::Files);
          binary = inside.isEmpty() ? QString() : folderOfBinary + "/" + inside.first();
        }
        if (binary.isEmpty()) {
          if (problems) *problems << bundle + ": has nothing for 64-bit Windows";
          continue;
        }
        const QString whole = QDir::cleanPath(QFileInfo(binary).absoluteFilePath());
        const QString bundleName = QFileInfo(bundle).fileName().toLower();
        if (bundlesTaken.contains(bundleName)) {
          if (problems) *problems << bundle + ": a copy of a plug-in found before it, left out";
          continue;
        }
        bundlesTaken.insert(bundleName);
        if (ofxSkippedBundles.contains(whole, Qt::CaseInsensitive)) {
          if (problems) *problems << binary + ": stopped the plug-in host while loading, left out";
          continue;
        }
        if (ofxBundleHook) ofxBundleHook(whole);
        loadBundle(binary, found, problems);
      }
      known = loadedFolders.emplace(key, std::move(found)).first;
    }
    all.insert(all.end(), known->second.begin(), known->second.end());
  }
  return all;
}

namespace {
// The pixel depth an instance works at for frames in `format`: the frame's own
// where the plug-in takes it -- an 8-bit frame turned into float and back was
// four times the memory to go through, for the same 8-bit picture at the end
// -- else the nearest finer one it takes, else the best it takes.
std::string depthFor(const Plugin *plugin, QImage::Format format) {
  const bool floats = format == QImage::Format_RGBA32FPx4 || format == QImage::Format_RGBA32FPx4_Premultiplied ||
                      format == QImage::Format_RGBX32FPx4 || format == QImage::Format_RGBA16FPx4 ||
                      format == QImage::Format_RGBA16FPx4_Premultiplied || format == QImage::Format_RGBX16FPx4;
  const bool shorts = format == QImage::Format_RGBA64 || format == QImage::Format_RGBA64_Premultiplied ||
                      format == QImage::Format_RGBX64;
  const std::vector<const char *> order = floats   ? std::vector<const char *>{kOfxBitDepthFloat, kOfxBitDepthShort, kOfxBitDepthByte}
                                          : shorts ? std::vector<const char *>{kOfxBitDepthShort, kOfxBitDepthFloat, kOfxBitDepthByte}
                                                   : std::vector<const char *>{kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthFloat};
  for (const char *wanted : order)
    if (std::find(plugin->depths.begin(), plugin->depths.end(), wanted) != plugin->depths.end()) return wanted;
  return plugin->depth;
}
// The format of the frame last drawn: what a new instance asked for before
// its first frame (its region, a button) goes by, so it is not made again at
// that frame's depth.
std::atomic<int> lastFrameFormat{QImage::Format_Invalid};

// The copy of the effect one layer keeps: made the first time it is drawn,
// with the plug-in told the shape of the pictures it will be given -- at
// `depth`, or with none given at the depth it has (a new one at the last
// frame's). One kept at another depth is let go of and made again.
std::shared_ptr<Effect> makeInstance(Plugin *plugin, const QString &key, double fps, const std::string &depth);
std::shared_ptr<Effect> instanceFor(Plugin *plugin, const QString &key, double fps, const std::string &depth = {}) {
  std::shared_ptr<Effect> other;
  {
    std::lock_guard<std::mutex> lock(plugin->instancesGuard);
    const auto found = plugin->instances.find(key);
    if (found != plugin->instances.end() && found->second && !depth.empty() && found->second->depth != depth &&
        found->second->usable) {
      other = std::move(found->second);
      plugin->instances.erase(found);
    }
  }
  if (other) {
    // As ofxDropInstances lets one go: after any render still using it.
    std::lock_guard<std::mutex> drawing(other->render);
    other->usable = false;
    if (!plugin->broken && !plugin->unloaded) {
      if (other->cardAttached)
        ofx_gl::run([&] { call(plugin, kOfxActionOpenGLContextDetached, handleOf(other.get()), nullptr, nullptr); });
      call(plugin, kOfxActionDestroyInstance, handleOf(other.get()), nullptr, nullptr);
    }
  }
  std::string at = depth;
  if (at.empty()) {
    const auto last = QImage::Format(lastFrameFormat.load());
    at = last == QImage::Format_Invalid ? plugin->depth : depthFor(plugin, last);
  }
  return makeInstance(plugin, key, fps, at);
}
std::shared_ptr<Effect> makeInstance(Plugin *plugin, const QString &key, double fps, const std::string &depth) {
  std::lock_guard<std::mutex> lock(plugin->instancesGuard);
  auto &held = plugin->instances[key];
  if (held) return held->usable ? held : nullptr;
  held = std::make_shared<Effect>();
  Effect *e = held.get();
  e->depth = depth;
  const Effect &from = *plugin->contextDescriptor;
  e->plugin = plugin;
  e->instance = true;
  e->props = from.props;
  PropertySet &p = e->props;
  p.setString(kOfxPropType, {kOfxTypeImageEffectInstance});
  p.setString(kOfxImageEffectPropContext, {plugin->context});
  p.setPointer(kOfxPropInstanceData, nullptr);
  p.setDouble(kOfxImageEffectPropProjectSize, {1920, 1080});
  p.setDouble(kOfxImageEffectPropProjectOffset, {0, 0});
  p.setDouble(kOfxImageEffectPropProjectExtent, {1920, 1080});
  p.setDouble(kOfxImageEffectPropProjectPixelAspectRatio, {1});
  p.setDouble(kOfxImageEffectInstancePropEffectDuration, {100000});
  p.setInt(kOfxImageEffectInstancePropSequentialRender, {0});
  p.setDouble(kOfxImageEffectPropFrameRate, {fps});
  p.setInt(kOfxPropIsInteractive, {0});
  p.setPointer(kOfxImageEffectPropPluginHandle, plugin->ofx);
  e->params.props = from.params.props;
  for (const auto &d : from.params.params) {
    auto param = std::make_unique<Param>();
    param->type = d->type;
    param->name = d->name;
    param->props = d->props;
    param->owner = e;
    param->loadDefault();
    {
      std::lock_guard<std::mutex> lock(curvesGuard);
      param->curves = d->curves;
    }
    e->params.params.push_back(std::move(param));
  }
  for (const auto &d : from.clips) {
    auto clip = std::make_unique<Clip>();
    clip->name = d->name;
    clip->owner = e;
    clip->props = d->props;
    const bool transition = plugin->context == kOfxImageEffectContextTransition;
    const bool connected = d->name == kOfxImageEffectOutputClipName ||
                           (transition ? d->name == kOfxImageEffectTransitionSourceFromClipName ||
                                             d->name == kOfxImageEffectTransitionSourceToClipName
                                       : d->name == kOfxImageEffectSimpleSourceClipName &&
                                             plugin->context != kOfxImageEffectContextGenerator);
    PropertySet &c = clip->props;
    c.setInt(kOfxImageClipPropConnected, {connected ? 1 : 0});
    c.setString(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
    c.setString(kOfxImageEffectPropPixelDepth, {e->depth});
    c.setString(kOfxImageClipPropUnmappedComponents, {kOfxImageComponentRGBA});
    c.setString(kOfxImageClipPropUnmappedPixelDepth, {e->depth});
    c.setString(kOfxImageEffectPropPreMultiplication, {kOfxImagePreMultiplied});
    c.setDouble(kOfxImagePropPixelAspectRatio, {1});
    c.setDouble(kOfxImageEffectPropFrameRate, {fps});
    c.setDouble(kOfxImageEffectPropFrameRange, {0, 100000});
    c.setDouble(kOfxImageEffectPropUnmappedFrameRate, {fps});
    c.setDouble(kOfxImageEffectPropUnmappedFrameRange, {0, 100000});
    c.setString(kOfxImageClipPropFieldOrder, {kOfxImageFieldNone});
    c.setInt(kOfxImageClipPropContinuousSamples, {0});
    e->clips.push_back(std::move(clip));
  }
  know(e);
  const OfxStatus created = call(plugin, kOfxActionCreateInstance, handleOf(e), nullptr, nullptr);
  if (created != kOfxStatOK && created != kOfxStatReplyDefault) {
    qWarning("OFX: %s would not make an instance (%d)", qPrintable(plugin->binary), created);
    return nullptr;
  }
  // What it wants its output to be. Only whether it comes back premultiplied
  // matters here: every picture goes in and out as RGBA at the plug-in's depth.
  PropertySet out;
  for (const auto &clip : e->clips) {
    out.setString("OfxImageClipPropComponents_" + clip->name, {kOfxImageComponentRGBA});
    out.setString("OfxImageClipPropDepth_" + clip->name, {e->depth});
    out.setDouble("OfxImageClipPropPAR_" + clip->name, {1});
  }
  out.setDouble(kOfxImageEffectPropFrameRate, {fps});
  out.setString(kOfxImageClipPropFieldOrder, {kOfxImageFieldNone});
  out.setString(kOfxImageEffectPropPreMultiplication, {kOfxImagePreMultiplied});
  out.setInt(kOfxImageClipPropContinuousSamples, {0});
  out.setInt(kOfxImageEffectFrameVarying, {0});
  call(plugin, kOfxImageEffectActionGetClipPreferences, handleOf(e), nullptr, &out);
  e->outputPremultiplied = out.getString(kOfxImageEffectPropPreMultiplication, 0, kOfxImagePreMultiplied) != kOfxImageUnPreMultiplied;
  if (out.getInt(kOfxImageEffectFrameVarying)) plugin->varies = 1;
  else if (plugin->varies < 0) plugin->varies = 0;
  e->applied = plugin->initial;
  e->usable = !plugin->broken;
  return e->usable ? held : nullptr;
}

QImage::Format premultipliedFormat(const std::string &depth) {
  return depth == kOfxBitDepthFloat ? QImage::Format_RGBA32FPx4_Premultiplied
         : depth == kOfxBitDepthShort ? QImage::Format_RGBA64_Premultiplied
                                      : QImage::Format_RGBA8888_Premultiplied;
}
QImage::Format straightFormat(const std::string &depth) {
  return depth == kOfxBitDepthFloat ? QImage::Format_RGBA32FPx4
         : depth == kOfxBitDepthShort ? QImage::Format_RGBA64
                                      : QImage::Format_RGBA8888;
}

// --- Pictures in and out of a render ---------------------------------------
// What a render costs this host, not the plug-in: the frame turned upside
// down (OpenFX counts rows from the bottom) into the plug-in's format, and the
// answer back. Measured at 1080p on a commercial suite's effects, that was 23 ms of
// every 35 -- the plug-ins drew in 5 to 12 -- from converting on one core,
// copying twice over and asking the system for fresh memory every time.

// `work(first, last)` for bands of rows, over the cores: the shared pool and
// the calling thread together. The caller takes bands as well, so it finishes
// even with the pool busy; a pool thread starting late finds nothing left.
void overRows(int rows, const std::function<void(int, int)> &work) {
  const int bands = std::clamp(rows / 32, 1, std::max(1, QThread::idealThreadCount()));
  if (bands == 1) {
    work(0, rows);
    return;
  }
  struct Shared {
    std::function<void(int, int)> work;
    int rows = 0, bands = 0;
    std::atomic<int> next{0}, done{0};
    std::mutex guard;
    std::condition_variable finished;
  };
  auto shared = std::make_shared<Shared>();
  shared->work = work;
  shared->rows = rows;
  shared->bands = bands;
  auto run = [shared] {
    for (int k; (k = shared->next.fetch_add(1)) < shared->bands;) {
      shared->work(shared->rows * k / shared->bands, shared->rows * (k + 1) / shared->bands);
      if (shared->done.fetch_add(1) + 1 == shared->bands) {
        std::lock_guard<std::mutex> lock(shared->guard);
        shared->finished.notify_all();
      }
    }
  };
  for (int t = 1; t < bands; ++t) QThreadPool::globalInstance()->start(run);
  run();
  std::unique_lock<std::mutex> lock(shared->guard);
  shared->finished.wait(lock, [&] { return shared->done.load() == shared->bands; });
}

// One row from one premultiplied RGBA format into another, for the pairs a
// render meets every frame: the engine's frames (8-bit RGBA, or ARGB32) and
// the plug-in's (8-bit RGBA, or float). Null for any other pair: Qt does those.
using RowConvert = void (*)(const uchar *from, uchar *to, int width);
inline uchar floatToByte(float v) { return v > 0 ? (v < 1 ? uchar(int(v * 255.f + .5f)) : uchar(255)) : uchar(0); }
const float *byteToFloat() {
  static const auto table = [] {
    std::array<float, 256> t{};
    // Times a 255th, as Qt has it: divided, a few came out a bit apart.
    for (int i = 0; i < 256; ++i) t[size_t(i)] = float(i) * (1.f / 255.f);
    return t;
  }();
  return table.data();
}
RowConvert rowConvert(QImage::Format from, QImage::Format to) {
  using F = QImage::Format;
  if (from == F::Format_RGBA8888_Premultiplied && to == F::Format_RGBA32FPx4_Premultiplied)
    return [](const uchar *in, uchar *out, int width) {
      const float *f = byteToFloat();
      float *o = reinterpret_cast<float *>(out);
      for (int k = 0; k < width * 4; ++k) o[k] = f[in[k]];
    };
  if (from == F::Format_ARGB32_Premultiplied && to == F::Format_RGBA32FPx4_Premultiplied)
    return [](const uchar *in, uchar *out, int width) {
      const float *f = byteToFloat();
      float *o = reinterpret_cast<float *>(out);
      for (int x = 0; x < width; ++x, in += 4, o += 4) {
        o[0] = f[in[2]];
        o[1] = f[in[1]];
        o[2] = f[in[0]];
        o[3] = f[in[3]];
      }
    };
  if (from == F::Format_RGBA32FPx4_Premultiplied && to == F::Format_RGBA8888_Premultiplied)
    return [](const uchar *in, uchar *out, int width) {
      const float *i = reinterpret_cast<const float *>(in);
      for (int k = 0; k < width * 4; ++k) out[k] = floatToByte(i[k]);
    };
  if (from == F::Format_RGBA32FPx4_Premultiplied && to == F::Format_ARGB32_Premultiplied)
    return [](const uchar *in, uchar *out, int width) {
      const float *i = reinterpret_cast<const float *>(in);
      for (int x = 0; x < width; ++x, i += 4, out += 4) {
        out[0] = floatToByte(i[2]);
        out[1] = floatToByte(i[1]);
        out[2] = floatToByte(i[0]);
        out[3] = floatToByte(i[3]);
      }
    };
  if ((from == F::Format_ARGB32_Premultiplied && to == F::Format_RGBA8888_Premultiplied) ||
      (from == F::Format_RGBA8888_Premultiplied && to == F::Format_ARGB32_Premultiplied))
    return [](const uchar *in, uchar *out, int width) {
      for (int x = 0; x < width; ++x, in += 4, out += 4) {
        out[0] = in[2];
        out[1] = in[1];
        out[2] = in[0];
        out[3] = in[3];
      }
    };
  return nullptr;
}

// `from` into `to`, the same size, upside down and converted on the way, over
// the cores. `to` is written where it is: no picture is made in between.
void flipInto(const QImage &from, QImage &to) {
  const int w = from.width(), h = from.height();
  if (w != to.width() || h != to.height() || from.isNull()) return;
  const QImage::Format a = from.format(), b = to.format();
  const uchar *source = from.constBits();
  uchar *target = to.bits(); // detached here, once, not by every band
  const qsizetype inRow = from.bytesPerLine(), outRow = to.bytesPerLine();
  const size_t pixels = size_t(w) * size_t(from.depth() / 8);
  const RowConvert quick = rowConvert(a, b);
  overRows(h, [&](int first, int last) {
    if (a == b) {
      for (int y = first; y < last; ++y) std::memcpy(target + (h - 1 - y) * outRow, source + y * inRow, pixels);
    } else if (quick) {
      for (int y = first; y < last; ++y) quick(source + y * inRow, target + (h - 1 - y) * outRow, w);
    } else {
      const QImage band = QImage(source + first * inRow, w, last - first, inRow, a).convertToFormat(b);
      const size_t row = size_t(std::min(band.bytesPerLine(), outRow));
      for (int y = first; y < last; ++y) std::memcpy(target + (h - 1 - y) * outRow, band.constScanLine(y - first), row);
    }
  });
}

// Every byte of `image` 0 -- transparent, in any of these formats -- over the
// cores.
void clearImage(QImage &image) {
  uchar *bits = image.bits();
  const qsizetype row = image.bytesPerLine();
  overRows(image.height(), [&](int first, int last) { std::memset(bits + first * row, 0, size_t(row) * size_t(last - first)); });
}

// Pictures the size of a frame, kept from one render to the next: memory new
// from the system is handed over a page at a time, as it is first touched,
// which cost as much again as the converting. The few last let go of.
std::mutex sparesGuard;
std::vector<QImage> spares;
QImage spareImage(int width, int height, QImage::Format format) {
  {
    std::lock_guard<std::mutex> lock(sparesGuard);
    for (auto it = spares.begin(); it != spares.end(); ++it)
      if (it->width() == width && it->height() == height && it->format() == format) {
        QImage found = std::move(*it);
        spares.erase(it);
        return found;
      }
  }
  return QImage(width, height, format);
}
struct Lent {
  QImage image;
  ~Lent() {
    if (image.isNull()) return;
    std::lock_guard<std::mutex> lock(sparesGuard);
    spares.push_back(std::move(image));
    // A handful, and no more than half a gigabyte between them.
    qsizetype bytes = 0;
    for (const QImage &kept : spares) bytes += kept.sizeInBytes();
    while (spares.size() > 8 || (spares.size() > 1 && bytes > (qsizetype(512) << 20))) {
      bytes -= spares.front().sizeInBytes();
      spares.erase(spares.begin());
    }
  }
};
} // namespace

// What plug-ins said in passing, in this process, waiting to be shown.
static std::mutex noticesGuard;
static QStringList noticesHeld;
void ofx_local::addNotices(const QStringList &said) {
  std::lock_guard<std::mutex> lock(noticesGuard);
  noticesHeld << said;
  // Shown in passing: a plug-in that says something every frame is heard,
  // not hoarded.
  while (noticesHeld.size() > 64) noticesHeld.removeFirst();
}
QStringList ofx_local::takeNotices() {
  std::lock_guard<std::mutex> lock(noticesGuard);
  QStringList said;
  said.swap(noticesHeld);
  return said;
}
QString ofx_local::effectMessage(const OfxEffectEntry &entry, const QString &key) {
  Plugin *plugin = localOf(entry);
  if (!plugin) return {};
  std::shared_ptr<Effect> held;
  {
    std::lock_guard<std::mutex> lock(plugin->instancesGuard);
    const auto found = plugin->instances.find(key);
    if (found != plugin->instances.end()) held = found->second;
  }
  if (!held) return {};
  std::lock_guard<std::mutex> lock(held->messageGuard);
  return held->persistent;
}

bool ofx_local::variesWithTime(const OfxEffectEntry &entry) {
  const Plugin *plugin = localOf(entry);
  return plugin && plugin->varies != 0;
}

// The controls into an instance's parameters, and the plug-in told which of
// them changed since it last drew. True when a transition is to bring the
// layer in rather than take it away.
static bool applyControls(Plugin *plugin, Effect *e, const QVector<float> &values, double canonicalW, double canonicalH,
                   double time, double scale, const QMap<QString, QString> *texts = nullptr) {
  std::vector<std::string> changed;
  bool arriving = false;
  size_t at = 0;
  std::vector<float> applied = e->applied;
  applied.resize(plugin->initial.size());
  for (const Slot &s : plugin->slots) {
    if (at + size_t(s.floats) > size_t(values.size())) break;
    Param *p = e->params.find(s.param.c_str());
    const float *v = values.constData() + at;
    bool differs = false;
    for (int k = 0; k < s.floats; ++k) differs = differs || applied[at + size_t(k)] != v[k];
    for (int k = 0; k < s.floats; ++k) applied[at + size_t(k)] = v[k];
    at += size_t(s.floats);
    if (s.how == Slot::Direction) {
      arriving = v[0] >= .5f;
      continue;
    }
    if (!p) continue;
    setSlot(s, *p, v, canonicalW, canonicalH, time);
    if (differs && (changed.empty() || changed.back() != s.param)) changed.push_back(s.param);
  }
  e->applied = applied;
  // The text controls, and what the plug-in keeps that has no control
  // (OfxEffectEntry::hidden), each as text.
  if (texts)
    for (auto it = texts->cbegin(); it != texts->cend(); ++it) {
      Param *p = e->params.find(it.key().toStdString().c_str());
      if (!p || !holdsValue(p->type) || !setValueText(*p, it.value().toStdString())) continue;
      changed.push_back(p->name);
    }
  if (!changed.empty()) {
    PropertySet begin;
    begin.setString(kOfxPropChangeReason, {kOfxChangeUserEdited});
    call(plugin, kOfxActionBeginInstanceChanged, handleOf(e), &begin, nullptr);
    for (const auto &name : changed) {
      PropertySet in;
      in.setString(kOfxPropType, {kOfxTypeParameter});
      in.setString(kOfxPropName, {name});
      in.setString(kOfxPropChangeReason, {kOfxChangeUserEdited});
      in.setDouble(kOfxPropTime, {time});
      in.setDouble(kOfxImageEffectPropRenderScale, {scale, scale});
      call(plugin, kOfxActionInstanceChanged, handleOf(e), &in, nullptr);
    }
    call(plugin, kOfxActionEndInstanceChanged, handleOf(e), &begin, nullptr);
  }
  return arriving;
}

double ofx_local::regionReach(const OfxEffectEntry &entry, const QString &key, QSizeF content, double time, double fps,
                              const QVector<float> &values) {
  Plugin *plugin = localOf(entry);
  if (!plugin || plugin->broken || plugin->unloaded || plugin->context == kOfxImageEffectContextGenerator) return 0;
  if (!(content.width() > 0) || !(content.height() > 0)) return 0;
  const std::shared_ptr<Effect> held = instanceFor(plugin, key, fps > 0 ? fps : 30);
  if (!held) return 0;
  Effect *e = held.get();
  std::unique_lock<std::mutex> unsafe(plugin->unsafe, std::defer_lock);
  if (plugin->threadSafety == kOfxImageEffectRenderUnsafe) unsafe.lock();
  std::lock_guard<std::mutex> lock(e->render);
  if (!e->usable) return 0;
  const double w = content.width(), h = content.height();
  e->props.setDouble(kOfxImageEffectPropProjectSize, {w, h});
  e->props.setDouble(kOfxImageEffectPropProjectExtent, {w, h});
  e->props.setDouble(kOfxImageEffectPropFrameRate, {fps});
  const bool arriving = applyControls(plugin, e, values, w, h, time, 1);
  // Bound with no pictures: the plug-in may ask its clips' regions, not for
  // their images.
  Binding b;
  b.width = b.contentW = int(std::ceil(w));
  b.height = b.contentH = int(std::ceil(h));
  b.depth = e->depth;
  b.time = time;
  b.canonicalW = w;
  b.canonicalH = h;
  if (plugin->context == kOfxImageEffectContextTransition)
    b.layerClip = arriving ? kOfxImageEffectTransitionSourceToClipName : kOfxImageEffectTransitionSourceFromClipName;
  else
    b.layerClip = kOfxImageEffectSimpleSourceClipName;
  e->binding = &b;
  PropertySet in, out;
  in.setDouble(kOfxPropTime, {time});
  in.setDouble(kOfxImageEffectPropRenderScale, {1, 1});
  out.setDouble(kOfxImageEffectPropRegionOfDefinition, {0, 0, w, h});
  const OfxStatus status = call(plugin, kOfxImageEffectActionGetRegionOfDefinition, handleOf(e), &in, &out);
  e->binding = nullptr;
  if (status != kOfxStatOK) return 0;
  double reach = 0;
  const double sides[4] = {-out.getDouble(kOfxImageEffectPropRegionOfDefinition, 0, 0),
                           -out.getDouble(kOfxImageEffectPropRegionOfDefinition, 1, 0),
                           out.getDouble(kOfxImageEffectPropRegionOfDefinition, 2, w) - w,
                           out.getDouble(kOfxImageEffectPropRegionOfDefinition, 3, h) - h};
  for (double side : sides) {
    // Infinite, by the standard's flags or otherwise.
    if (!std::isfinite(side) || side >= 1e9) return std::numeric_limits<double>::infinity();
    reach = std::max(reach, side);
  }
  return reach;
}

// An effect's notices, taken from it and added to the rest under its name.
static void takeNoticesOf(Effect *e) {
  QStringList said;
  {
    std::lock_guard<std::mutex> lock(e->messageGuard);
    said.swap(e->notices);
  }
  if (said.isEmpty()) return;
  const QString name = e->plugin ? label(e->plugin->descriptor->props, e->plugin->ofx->pluginIdentifier) : QString();
  for (QString &line : said) line = name + ": " + line;
  ofx_local::addNotices(said);
}

// The parameters back into the controls: what a plug-in set them to itself.
static void readControls(const Plugin *plugin, Effect *e, double canonicalW, double canonicalH, double time,
                         QVector<float> &values, QMap<QString, QString> &texts) {
  size_t at = 0;
  for (const Slot &s : plugin->slots) {
    if (at + size_t(s.floats) > size_t(values.size())) break;
    float *v = values.data() + at;
    at += size_t(s.floats);
    const Param *p = e->params.find(s.param.c_str());
    if (!p) continue;
    switch (s.how) {
      case Slot::Double: {
        const double d = p->d[s.component];
        v[0] = float(s.normalise == 1 ? d / std::max(1e-9, canonicalW) : s.normalise == 2 ? d / std::max(1e-9, canonicalH) : d);
        break;
      }
      case Slot::Int: v[0] = float(p->i[s.component]); break;
      case Slot::Bool: v[0] = p->i[0] ? 1.f : 0.f; break;
      case Slot::Choice: v[0] = float(p->i[0]); break;
      case Slot::StrChoice: {
        const auto found = std::find(s.enums.begin(), s.enums.end(), p->s);
        if (found != s.enums.end()) v[0] = float(found - s.enums.begin());
        break;
      }
      case Slot::Colour:
        for (int k = 0; k < (s.alpha ? 4 : 3); ++k) v[k] = float(p->d[k]);
        break;
      case Slot::Direction: break;
      case Slot::Offset: v[0] = float(p->d[0] - time); break;
    }
  }
  for (auto it = texts.begin(); it != texts.end(); ++it)
    if (const Param *p = e->params.find(it.key().toStdString().c_str()))
      if (holdsValue(p->type)) it.value() = QString::fromStdString(valueText(*p));
}

bool ofx_local::pressButton(const OfxEffectEntry &entry, const QString &key, const QString &button, double time,
                            double fps, QVector<float> &values, QMap<QString, QString> &texts, const QImage &picture,
                            QString *error, const std::function<bool(const QString &, double)> &progress) {
  auto fail = [&](const QString &why) {
    if (error) *error = entry.name + ": " + why;
    return false;
  };
  Plugin *plugin = localOf(entry);
  if (!plugin) return fail("not loaded");
  if (plugin->broken) return fail("switched off after it crashed");
  if (plugin->unloaded) return fail("unloaded");
  const std::shared_ptr<Effect> held = instanceFor(plugin, key, fps > 0 ? fps : 30);
  if (!held) return fail("could not be started");
  Effect *e = held.get();
  std::unique_lock<std::mutex> unsafe(plugin->unsafe, std::defer_lock);
  if (plugin->threadSafety == kOfxImageEffectRenderUnsafe) unsafe.lock();
  std::lock_guard<std::mutex> lock(e->render);
  if (!e->usable) return fail("was closed");
  Param *pressed = e->params.find(button.toStdString().c_str());
  if (!pressed || pressed->type != kOfxParamTypePushButton) return fail("has no button " + button);
  // The frame as the plug-in knows it: the picture's size when there is one,
  // the last it drew at when not.
  const double canonicalW = picture.isNull() ? e->props.getDouble(kOfxImageEffectPropProjectSize, 0, 1920) : picture.width();
  const double canonicalH = picture.isNull() ? e->props.getDouble(kOfxImageEffectPropProjectSize, 1, 1080) : picture.height();
  e->props.setDouble(kOfxImageEffectPropProjectSize, {canonicalW, canonicalH});
  e->props.setDouble(kOfxImageEffectPropProjectExtent, {canonicalW, canonicalH});
  const bool arriving = applyControls(plugin, e, values, canonicalW, canonicalH, time, 1, &texts);
  // The picture, for a plug-in that asks its clip for it while it answers.
  QImage source, output;
  Binding b;
  if (!picture.isNull()) {
    source = picture.convertToFormat(premultipliedFormat(e->depth)).mirrored(false, true);
    output = QImage(source.size(), source.format());
    output.fill(Qt::transparent);
    b.source = source.bits();
    b.output = output.bits();
    b.rowBytes = int(source.bytesPerLine());
    b.width = b.contentW = source.width();
    b.height = b.contentH = source.height();
    b.format = source.format();
  }
  b.depth = e->depth;
  b.time = time;
  b.canonicalW = canonicalW;
  b.canonicalH = canonicalH;
  if (plugin->context == kOfxImageEffectContextTransition)
    b.layerClip = arriving ? kOfxImageEffectTransitionSourceToClipName : kOfxImageEffectTransitionSourceFromClipName;
  else if (plugin->context != kOfxImageEffectContextGenerator)
    b.layerClip = kOfxImageEffectSimpleSourceClipName;
  QImage blank;
  if (!picture.isNull() && plugin->context == kOfxImageEffectContextTransition) {
    blank = QImage(source.size(), source.format());
    blank.fill(Qt::transparent);
    b.blank = blank.bits();
  }
  e->binding = &b;
  e->progress = progress;
  struct Unbind {
    Effect *e;
    Binding *b;
    ~Unbind() {
      e->binding = nullptr;
      e->progress = nullptr;
      takeNoticesOf(e);
      std::lock_guard<std::mutex> lock(imagesGuard);
      for (auto it = liveImages.begin(); it != liveImages.end();)
        it = it->first->binding == b ? liveImages.erase(it) : std::next(it);
    }
  } unbind{e, &b};
  {
    std::lock_guard<std::mutex> lock(e->messageGuard);
    e->message.clear();
  }
  PropertySet begin;
  begin.setString(kOfxPropChangeReason, {kOfxChangeUserEdited});
  call(plugin, kOfxActionBeginInstanceChanged, handleOf(e), &begin, nullptr);
  PropertySet in;
  in.setString(kOfxPropType, {kOfxTypeParameter});
  in.setString(kOfxPropName, {pressed->name});
  in.setString(kOfxPropChangeReason, {kOfxChangeUserEdited});
  in.setDouble(kOfxPropTime, {time});
  in.setDouble(kOfxImageEffectPropRenderScale, {1, 1});
  const OfxStatus status = call(plugin, kOfxActionInstanceChanged, handleOf(e), &in, nullptr);
  call(plugin, kOfxActionEndInstanceChanged, handleOf(e), &begin, nullptr);
  if (plugin->broken) return fail("crashed and is switched off");
  if (status != kOfxStatOK && status != kOfxStatReplyDefault) {
    QString said;
    {
      std::lock_guard<std::mutex> lock(e->messageGuard);
      said = e->message;
    }
    return fail(!said.isEmpty() ? said : QString("did not take the button (%1)").arg(status));
  }
  readControls(plugin, e, canonicalW, canonicalH, time, values, texts);
  // What it set them to is what it now has: the next render tells it of no
  // change it made itself.
  e->applied.assign(values.begin(), values.end());
  e->applied.resize(plugin->initial.size());
  return true;
}

QStringList ofx_local::pictureConversionProblems() {
  QStringList problems;
  using F = QImage::Format;
  const std::pair<F, const char *> formats[] = {{F::Format_RGBA8888_Premultiplied, "8-bit RGBA"},
                                                {F::Format_ARGB32_Premultiplied, "ARGB32"},
                                                {F::Format_RGBA64_Premultiplied, "16-bit"},
                                                {F::Format_RGBA32FPx4_Premultiplied, "float"},
                                                {F::Format_RGBA32FPx4, "float straight"}};
  // Every kind of pixel a frame has -- clear, faint, half, whole, each colour
  // no more than its alpha -- in an odd size whose rows are split in bands.
  QImage made(37, 203, QImage::Format_RGBA32FPx4_Premultiplied);
  for (int y = 0; y < made.height(); ++y) {
    float *row = reinterpret_cast<float *>(made.scanLine(y));
    for (int x = 0; x < made.width(); ++x) {
      const float a = float((x * 7 + y * 3) % 256) / 255.f * (y % 5 == 0 ? .37f : 1.f);
      row[x * 4 + 0] = a * float((x * 13 + y) % 101) / 100.f;
      row[x * 4 + 1] = a * float((y * 11) % 97) / 96.f;
      row[x * 4 + 2] = a * float((x + y * 5) % 89) / 88.f;
      row[x * 4 + 3] = a;
    }
  }
  for (const auto &[from, fromName] : formats) {
    const QImage picture = made.convertToFormat(from);
    for (const auto &[to, toName] : formats) {
      if (to == F::Format_RGBA32FPx4) continue; // nothing is ever written straight
      const QImage expected = picture.convertToFormat(to).mirrored(false, true);
      QImage got(picture.size(), to);
      got.fill(Qt::red);
      flipInto(picture, got);
      const size_t bytes = size_t(got.width()) * size_t(got.depth() / 8);
      int wrong = 0;
      QString first;
      for (int y = 0; y < got.height(); ++y)
        if (std::memcmp(got.constScanLine(y), expected.constScanLine(y), bytes))
          for (int x = 0; x < got.width(); ++x)
            if (got.pixelColor(x, y) != expected.pixelColor(x, y) && wrong++ == 0)
              first = QString("at %1,%2 %3 for %4").arg(x).arg(y).arg(got.pixelColor(x, y).name(QColor::HexArgb),
                                                                       expected.pixelColor(x, y).name(QColor::HexArgb));
      if (wrong) problems << QString("%1 into %2: %3 pixel(s) unlike Qt's, first %4").arg(fromName, toName).arg(wrong).arg(first);
    }
  }
  // Floats past the ends -- a plug-in's highlights, its mistakes -- go into
  // bytes as the nearest that can be, nonsense as nothing.
  QImage wild(3, 1, F::Format_RGBA32FPx4_Premultiplied);
  float *w = reinterpret_cast<float *>(wild.bits());
  const float values[12] = {1.7f, -.2f, std::numeric_limits<float>::quiet_NaN(), 1,
                            .5f, .5f, .5f, std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), 0, 1e30f, 2};
  std::memcpy(w, values, sizeof values);
  QImage bytes(3, 1, F::Format_RGBA8888_Premultiplied);
  flipInto(wild, bytes);
  const uchar *b = bytes.constBits();
  const uchar due[12] = {255, 0, 0, 255, 128, 128, 128, 255, 0, 0, 255, 255};
  if (std::memcmp(b, due, 12))
    problems << QString("floats past the ends came out as %1").arg(QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(b), 12).toHex(' ')));
  return problems;
}

bool ofx_local::render(const OfxEffectEntry &entry, const QString &key, QImage &image, double time, double fps,
                       const QVector<float> &values, double renderScale, QString *error, const OfxRenderOptions &options) {
  const OfxOtherTimes *frames = options.times;
  const bool draft = options.draft;
  auto fail = [&](const QString &why) {
    if (error) *error = entry.name + ": " + why;
    return false;
  };
  Plugin *plugin = localOf(entry);
  if (!plugin) return fail("not loaded");
  if (plugin->broken) return fail("switched off after it crashed");
  if (plugin->unloaded) return fail("unloaded");
  if (image.isNull()) return true;
  lastFrameFormat = int(image.format());
  const std::shared_ptr<Effect> held = instanceFor(plugin, key, fps > 0 ? fps : 30, depthFor(plugin, image.format()));
  if (!held) return fail("could not be started");
  Effect *e = held.get();
  std::unique_lock<std::mutex> unsafe(plugin->unsafe, std::defer_lock);
  if (plugin->threadSafety == kOfxImageEffectRenderUnsafe) unsafe.lock();
  std::lock_guard<std::mutex> lock(e->render);
  // Let go of (ofxDropInstances) while this render waited for it.
  if (!e->usable) return fail("was closed");
  const std::string &depth = e->depth;
  const double scale = plugin->multiResolution && renderScale > 0 && std::isfinite(renderScale) ? renderScale : 1;
  const int w = image.width(), h = image.height();
  // The frame the plug-in sees is the layer's picture: where it lies in the
  // image (all of it unless told otherwise), the rest room to draw beyond.
  const QRect content = options.content.isEmpty() ? QRect(0, 0, w, h) : options.content.intersected(QRect(0, 0, w, h));
  if (content.isEmpty()) return true;
  const double canonicalW = content.width() / scale, canonicalH = content.height() / scale;
  e->props.setDouble(kOfxImageEffectPropProjectSize, {canonicalW, canonicalH});
  e->props.setDouble(kOfxImageEffectPropProjectExtent, {canonicalW, canonicalH});
  e->props.setDouble(kOfxImageEffectPropFrameRate, {fps});

  // The controls into the plug-in's parameters, which it is told of first.
  const bool arriving = applyControls(plugin, e, values, canonicalW, canonicalH, time, scale, &options.texts);

  // The pictures, bottom row first as OpenFX has them.
  const QImage::Format wanted = image.format();
  // In memory kept from the last render the size of this one (spareImage),
  // turned and converted over the cores (flipInto); the output cleared there.
  Lent sourceBuffer{spareImage(w, h, premultipliedFormat(depth))}, outputBuffer{spareImage(w, h, premultipliedFormat(depth))};
  QImage &source = sourceBuffer.image, &output = outputBuffer.image;
  if (source.isNull() || output.isNull()) return fail("out of memory");
  flipInto(image, source);
  clearImage(output);
  // The frames the layer has, where they are known: a plug-in reading
  // others keeps inside them.
  const bool span = frames && frames->last >= frames->first;
  const double first = span ? frames->first : 0, last = span ? frames->last : 100000;
  e->props.setDouble(kOfxImageEffectInstancePropEffectDuration, {last - first + 1});
  for (const auto &clip : e->clips) {
    clip->props.setString(kOfxImageEffectPropPixelDepth, {depth});
    clip->props.setDouble(kOfxImageEffectPropFrameRate, {fps});
    clip->props.setDouble(kOfxImageEffectPropFrameRange, {first, last});
    clip->props.setDouble(kOfxImageEffectPropUnmappedFrameRange, {first, last});
  }
  Binding b;
  b.source = source.bits();
  b.output = output.bits();
  b.rowBytes = int(source.bytesPerLine());
  b.width = w;
  b.height = h;
  // Bottom up, as OpenFX counts: the rows below the layer's picture first.
  b.x1 = -content.left();
  b.y1 = -(h - content.top() - content.height());
  b.contentW = content.width();
  b.contentH = content.height();
  b.depth = depth;
  b.scale = scale;
  b.time = time;
  b.frames = frames;
  b.format = source.format();
  b.canonicalW = canonicalW;
  b.canonicalH = canonicalH;
  // Which clip is the layer, and nothing on a transition's other side.
  Lent blankBuffer;
  QImage &blank = blankBuffer.image;
  if (plugin->context == kOfxImageEffectContextTransition) {
    b.layerClip = arriving ? kOfxImageEffectTransitionSourceToClipName : kOfxImageEffectTransitionSourceFromClipName;
    blank = spareImage(w, h, source.format());
    if (blank.isNull()) return fail("out of memory");
    clearImage(blank);
    b.blank = blank.bits();
  } else if (plugin->context != kOfxImageEffectContextGenerator) {
    b.layerClip = kOfxImageEffectSimpleSourceClipName;
  }
  e->binding = &b;
  // Which parameters are animated, for a plug-in that looks before asking.
  for (const auto &param : e->params.params)
    param->props.setInt(kOfxParamPropIsAnimating, {keysOf(param.get()).empty() ? 0 : 1});
  // The other pictures it takes: those given are connected, the rest not.
  for (const auto &clip : e->clips) {
    const std::string &n = clip->name;
    if (n == kOfxImageEffectOutputClipName || n == b.layerClip || (b.blank && (n == kOfxImageEffectTransitionSourceFromClipName ||
                                                                               n == kOfxImageEffectTransitionSourceToClipName)))
      continue;
    const QImage given = options.inputs.value(QString::fromStdString(n));
    QImage ready;
    if (given.size() == QSize(w, h)) ready = given.convertToFormat(source.format()).mirrored(false, true);
    if (!ready.isNull() && ready.bytesPerLine() == source.bytesPerLine()) b.inputs[n] = std::move(ready);
    clip->props.setInt(kOfxImageClipPropConnected, {b.inputs.count(n) ? 1 : 0});
  }
  struct Unbind {
    Effect *e;
    Binding *b;
    ~Unbind() {
      e->binding = nullptr;
      takeNoticesOf(e);
      // Pictures the plug-in did not give back are let go with the render.
      std::lock_guard<std::mutex> lock(imagesGuard);
      for (auto it = liveImages.begin(); it != liveImages.end();)
        it = it->first->binding == b ? liveImages.erase(it) : std::next(it);
    }
  } unbind{e, &b};
  {
    std::lock_guard<std::mutex> lock(e->messageGuard);
    e->message.clear();
  }

  // Asked first whether it would change anything at all with these settings.
  PropertySet identityIn, identityOut;
  identityIn.setDouble(kOfxPropTime, {time});
  identityIn.setString(kOfxImageEffectPropFieldToRender, {kOfxImageFieldNone});
  identityIn.setInt(kOfxImageEffectPropRenderWindow, {b.x1, b.y1, b.x1 + w, b.y1 + h});
  identityIn.setDouble(kOfxImageEffectPropRenderScale, {scale, scale});
  identityIn.setInt(kOfxImageEffectPropRenderQualityDraft, {draft ? 1 : 0});
  identityIn.setInt(kOfxImageEffectPropInteractiveRenderStatus, {draft ? 1 : 0});
  noGpu(identityIn);
  identityOut.setString(kOfxPropName, {""});
  identityOut.setDouble(kOfxPropTime, {time});
  // The layer as it is; or, a transition at its far end, nothing.
  if (call(plugin, kOfxImageEffectActionIsIdentity, handleOf(e), &identityIn, &identityOut) == kOfxStatOK) {
    const std::string same = identityOut.getString(kOfxPropName);
    if (!b.layerClip.empty() && same == b.layerClip) return true;
    if (b.blank && (same == kOfxImageEffectTransitionSourceFromClipName || same == kOfxImageEffectTransitionSourceToClipName)) {
      // A picture of its own, not the one given cleared where it lies: the
      // plug-in host sends back only a picture that is a new one.
      QImage nothing(w, h, wanted);
      if (nothing.isNull()) return fail("out of memory");
      nothing.fill(Qt::transparent);
      image = nothing;
      return true;
    }
  }
  if (plugin->broken) return fail("crashed and is switched off");

  // The render itself, on the card (OpenGL enabled, the output a framebuffer
  // bound before it starts) or on the processor.
  auto draw = [&](bool onCard) {
    PropertySet sequence;
    sequence.setDouble(kOfxImageEffectPropFrameRange, {time, time});
    sequence.setDouble(kOfxImageEffectPropFrameStep, {1});
    sequence.setInt(kOfxPropIsInteractive, {0});
    sequence.setDouble(kOfxImageEffectPropRenderScale, {scale, scale});
    sequence.setInt(kOfxImageEffectPropSequentialRenderStatus, {0});
    // Playing, the render is interactive and may be a draft; otherwise neither.
    sequence.setInt(kOfxImageEffectPropInteractiveRenderStatus, {draft ? 1 : 0});
    sequence.setInt(kOfxImageEffectPropRenderQualityDraft, {draft ? 1 : 0});
    noGpu(sequence);
    sequence.setInt(kOfxImageEffectPropOpenGLEnabled, {onCard ? 1 : 0});
    call(plugin, kOfxImageEffectActionBeginSequenceRender, handleOf(e), &sequence, nullptr);
    PropertySet render;
    render.setDouble(kOfxPropTime, {time});
    render.setString(kOfxImageEffectPropFieldToRender, {kOfxImageFieldNone});
    render.setInt(kOfxImageEffectPropRenderWindow, {b.x1, b.y1, b.x1 + w, b.y1 + h});
    render.setDouble(kOfxImageEffectPropRenderScale, {scale, scale});
    render.setInt(kOfxImageEffectPropSequentialRenderStatus, {0});
    render.setInt(kOfxImageEffectPropInteractiveRenderStatus, {draft ? 1 : 0});
    render.setInt(kOfxImageEffectPropRenderQualityDraft, {draft ? 1 : 0});
    noGpu(render);
    render.setInt(kOfxImageEffectPropOpenGLEnabled, {onCard ? 1 : 0});
    const OfxStatus drawn = call(plugin, kOfxImageEffectActionRender, handleOf(e), &render, nullptr);
    call(plugin, kOfxImageEffectActionEndSequenceRender, handleOf(e), &sequence, nullptr);
    return drawn;
  };
  OfxStatus status = kOfxStatFailed;
  bool drawnOnCard = false;
  if (plugin->card && !plugin->broken) {
    // On the card's thread, with its context: told of the context the first
    // time, the output a framebuffer, read back into `output` after.
    const bool ran = ofx_gl::run([&] {
      if (!e->cardAttached) {
        call(plugin, kOfxActionOpenGLContextAttached, handleOf(e), nullptr, nullptr);
        e->cardAttached = true;
      }
      ofx_gl::Target target = ofx_gl::target(w, h, depth);
      if (!target) return;
      b.card = true;
      b.cardOutput = target.texture;
      status = draw(true);
      if (status == kOfxStatOK && !ofx_gl::read(target, output.bits(), int(output.bytesPerLine()))) status = kOfxStatFailed;
      for (auto &texture : b.textures)
        if (texture->owned) ofx_gl::drop(texture->id);
      b.textures.clear();
      b.card = false;
      ofx_gl::release(target);
      drawnOnCard = status == kOfxStatOK;
    });
    if (!ran && !plugin->processor) return fail("draws only on the graphics card, which gave it no OpenGL here");
  }
  // On the processor: the plug-in's own way, or where the card could not --
  // on the output cleared again if the card had it.
  if (!drawnOnCard && plugin->processor && !plugin->broken) {
    if (plugin->card) clearImage(output);
    status = draw(false);
  }
  if (status != kOfxStatOK) {
    QString said;
    {
      std::lock_guard<std::mutex> lock(e->messageGuard);
      said = e->message;
    }
    return fail(plugin->broken ? "crashed and is switched off"
                : !said.isEmpty() ? said : QString("could not draw (%1)").arg(status));
  }
  // Back the right way up, into the frame itself in its own format.
  if (e->outputPremultiplied) flipInto(output, image);
  else flipInto(QImage(output.constBits(), w, h, output.bytesPerLine(), straightFormat(depth)), image);
  if (image.isNull() || image.format() != wanted) return fail("out of memory");
  return true;
}
