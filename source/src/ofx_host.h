#pragma once
// OpenFX: image effect plug-ins made for other hosts -- DaVinci Resolve, Nuke,
// Vegas, Natron -- installed as .ofx bundles, run as effects on a layer.
// Wavy-Flo is the host: it loads a bundle, asks each plug-in in it what it is
// and what can be set on it, shows those settings as a plug-in's controls, and
// hands it the layer's picture to work on.
//
// A plug-in works on one layer, in whichever of its contexts fits that best:
// a filter (the layer's picture in, a picture out); a transition, between the
// layer and nothing -- the layer leaving or arriving as Transition goes from 0
// to 1, both of which this host sets as controls; a generator, whose picture
// takes the layer's place; or a retimer, which shows the layer at another time
// -- the layer's own time moved by Time Offset, a control this host sets. Overlays drawn in the viewer, more than one input
// and the card's own render paths are not offered to them; every plug-in is
// told so and is expected to manage without. The layer's picture at other times
// is offered (an echo, a frame blend, noise reduction read the frames around
// the one they draw).
#include <QImage>
#include <QMap>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <vector>

// One control as Wavy-Flo shows it. A point is two numbers, a colour one
// control of four. Numbers, toggles, choices and colours hold values, packed
// in their order for ofxRender; text is a string of its own (by id, in
// OfxRenderOptions::texts); a button holds nothing and is pressed
// (ofxPressButton); a group folds the controls up to its GroupEnd, and groups
// nest, as the plug-in arranged them. An input is another picture the effect
// takes besides the layer's (a matte, a map): its id is the plug-in's name for
// it, and the picture goes in OfxRenderOptions::inputs.
struct OfxControl {
  enum Kind { Number, Toggle, Choice, Colour, Text, Button, GroupBegin, GroupEnd, Input };
  // Text: one line, several, a file's path, a folder's, or words to read only.
  enum TextMode { SingleLine, MultiLine, FilePath, FolderPath, Label };
  Kind kind = Number;
  QString id;    // saved with the project: the parameter's own name, "/x" and "/y" for a point
  QString label; // what the person reads
  double minimum = 0, maximum = 1, initial = 0, step = .01;
  QStringList choices;
  double colour[4] = {1, 1, 1, 1}; // Colour: what it starts at
  QString text;                    // Text: what it starts as
  TextMode textMode = SingleLine;
  bool open = true;                // GroupBegin: open until the person folds it
  bool optional = true;            // Input: the effect works without it
  bool mask = false;               // Input: it is a mask (its alpha is what counts)
  // A Number that is one coordinate of a point in the picture -- a flare's
  // centre -- shown as a handle the person can drag on the preview: the
  // point's own name, the axis (0 x, 1 y), and whether the value is a
  // fraction of the layer's picture rather than its units. The point is in
  // the plug-in's own way round: from the picture's lower left, y up
  // (hostEffectPointToComposition).
  QString point;
  int axis = -1;
  bool normalised = false;
};
// Whether the control holds values (packed for ofxRender) rather than text, a
// button or a group.
inline bool ofxControlHasValues(const OfxControl &c) { return c.kind <= OfxControl::Colour; }

struct OfxPluginEffect; // a plug-in as loaded, and its instances

struct OfxEffectEntry {
  QString name;       // the plug-in's own label
  QString group;      // where it files itself ("Blur/Lens"), may be empty
  QString identifier; // reverse domain name, e.g. "uk.co.thefoundry.OfxInvertExample"
  QString binary;     // the .ofx file it came from
  bool otherFrames = false; // reads the layer's picture at times other than the one it draws
  int card = 0;              // draws on the graphics card: 0 not, 1 it can, 2 only there
  // How it works on the layer: "filter" (the general context too),
  // "transition", "generator" or "retimer".
  QString kind = "filter";
  QVector<OfxControl> controls;
  // What the plug-in keeps that has no control: its Custom parameters and the
  // ones it keeps secret -- a look made in its own window, say -- by name,
  // each with its default, as text. They travel with the text controls
  // (OfxRenderOptions::texts, ofxPressButton), are kept with the layer and
  // are handed back at every render.
  QMap<QString, QString> hidden;
  std::shared_ptr<OfxPluginEffect> plugin;
};

// The folders OFX plug-ins are installed in, in the order they are read:
// "effects" beside Wavy-Flo (as After Effects' Plug-ins folder, makers'
// folders inside it too), "ofx" beside it, those in OFX_PLUGIN_PATH, and the
// one every host reads (Common Files\OFX\Plugins).
QStringList ofxPluginFolders();

// Every image effect plug-in in the bundles under `folders`, at any depth,
// that can work as a filter. Bundles stay loaded for the life of the program,
// and a folder asked for again is not loaded twice; a bundle found twice is
// loaded from the first folder that has it. Why the others were left out is
// added to `problems`.
std::vector<OfxEffectEntry> ofxLoadEffects(const QStringList &folders, QStringList *problems = nullptr);

// Where plug-ins turned away for what this host cannot do are written down,
// so later loads skip them without loading them (a bundle with nothing usable
// is not loaded at all). Empty: nothing is remembered. `askAgain` forgets what
// was written and asks every plug-in afresh. Set before ofxLoadEffects.
void ofxRememberRefusals(const QString &file, bool askAgain = false);

// The layer at times other than the one being drawn, all in frames into the
// layer. `at` makes its picture as it reaches the effect -- the size and
// format of the image being drawn -- or gives a null image when it cannot.
// `values` gives the effect's controls at each of the times asked for, packed
// as ofxRender's `values` are; `keys` holds, for each control, the times of
// its keyframes (none for one that is not animated). `first` and `last` are
// the layer's own frames; a picture asked for outside them is held at the
// nearer one.
struct OfxOtherTimes {
  std::function<QImage(double time)> at;
  std::function<QVector<QVector<float>>(const QVector<double> &times)> values;
  QVector<QVector<double>> keys;
  double first = 0, last = 0;
};

// What a render may be told beyond the picture and the controls.
struct OfxRenderOptions {
  // The layer at other times -- its pictures, and the controls as they are
  // animated -- without which a plug-in asking for another time is given this
  // one.
  const OfxOtherTimes *times = nullptr;
  // A preview that is playing, where a plug-in may draw a quicker, rougher
  // picture.
  bool draft = false;
  // Where the layer's own picture lies in the image, in pixels from its top
  // left: what the plug-in takes as its frame. The rest of the image is room
  // for it to draw beyond that (ofxRegionReach). Empty: the whole image.
  QRect content;
  // The text controls, by id, and what has no control (OfxEffectEntry::hidden),
  // by name; one not given keeps what it was.
  QMap<QString, QString> texts;
  // The inputs' pictures, by id, each the size of the image (any format). One
  // not given is not connected.
  QMap<QString, QImage> inputs;
};

// The effect run on `image`, which comes back in the format it went in.
// `instance` names the copy of the effect one layer keeps (plug-ins keep state
// between frames), `time` is in frames into the layer, `values` the controls
// packed in their order (a colour taking four), `renderScale` how many pixels
// of the image a pixel of the layer is. False, with `error`, when the plug-in
// could not run; the image is then as it was.
bool ofxRender(const OfxEffectEntry &, const QString &instance, QImage &image, double time, double fps,
               const QVector<float> &values, double renderScale = 1, QString *error = nullptr,
               const OfxRenderOptions &options = {});

// How far the effect draws beyond a layer's picture `content` big (in the
// layer's own units), at these settings: what the plug-in says of its region
// of definition, as the widest of its four sides. 0 when it keeps within the
// picture or does not say; infinity when it covers everything. A generator,
// whose picture is the layer's, is 0.
double ofxRegionReach(const OfxEffectEntry &, const QString &instance, QSizeF content, double time, double fps,
                      const QVector<float> &values);

// One of the effect's buttons pressed, at `time`, with the controls as `values`
// and `texts` have them and `picture` -- the layer as it reaches the effect,
// any format, or none -- for a plug-in that looks at it (an "Analyze"). What
// the plug-in set its controls to in answer comes back in `values` and
// `texts` -- those given in `texts`, the hidden ones among them (a look made
// in the plug-in's window). False, with `error`, when it could not be told.
// `progress`, when given, hears how far the plug-in says it has gone (a label,
// a fraction from 0 to 1) while it answers; returning false asks it to stop.
bool ofxPressButton(const OfxEffectEntry &, const QString &instance, const QString &button, double time, double fps,
                    QVector<float> &values, QMap<QString, QString> &texts, const QImage &picture = {},
                    QString *error = nullptr,
                    const std::function<bool(const QString &label, double fraction)> &progress = {});

// What the plug-in keeps saying about this copy of the effect -- an error or a
// warning it wants seen beside it until it takes it back ("Licence expired",
// "Choose a matte") -- as "error: ...", "warning: ..." or "message: ...".
// Empty when it says nothing.
QString ofxEffectMessage(const OfxEffectEntry &, const QString &instance);
// What plug-ins said in passing since this was last asked (a warning, an
// error, a note), each "Effect: what it said", oldest first.
QStringList ofxTakeNotices();

// The copies of effects layers keep, let go of unless `keep` says otherwise
// (by their `instance` name): each is destroyed once any render still using it
// is done. One asked for again later is simply made again.
void ofxDropInstances(const std::function<bool(const QString &instance)> &keep);
int ofxInstanceCount();

// Every loaded plug-in told to let go -- its instances destroyed, then the
// Unload action -- after which none of them draws. Run by itself as the
// application closes; safe to run twice.
void ofxUnloadAll();

// What plug-ins asked the host for that it does not have (a suite, a
// property), for working out why one would not load.
QStringList ofxHostGaps();

// Whether the effect draws something different at every frame even with its
// settings held still (grain, flicker, a shake). True until a copy of it has
// been made and has said otherwise, so a picture is never kept too long.
bool ofxVariesWithTime(const OfxEffectEntry &);

// Where plug-ins run. With the worker on, every OpenFX plug-in runs in a
// process of its own -- this same program started with --ofx-worker -- so one
// that crashes, ends its process or never returns takes only that process
// down: Wavy-Flo switches that plug-in off, says so, and starts the worker
// again with everything it had loaded. Off, they run in this process (the
// self-tests; --ofx-in-process). `program` is what to start as the worker,
// this executable by default. Set before ofxLoadEffects.
void ofxUseWorker(bool on, const QString &program = {});
bool ofxWorkerInUse();
// How long one render, or loading one bundle, may take in the worker before
// it is taken to be hung and ended (30 s and 60 s unless set).
void ofxSetWorkerTimeouts(int renderMs, int loadMs);
// What happened to the worker: each start, and each time it stopped and why.
QStringList ofxWorkerLog();
// A button may open a window of the plug-in's own (a look suite's Edit Look).
// Called by Wavy-Flo while it is the program in front: the first lets the
// plug-in host (and programs it starts) come in front, the second puts the
// first window they show in front -- true once there is one.
void ofxLetPluginWindowsForward();
bool ofxRaisePluginWindow();
// The worker process's own main(): called first thing for --ofx-worker.
int ofxWorkerMain(int argc, char **argv);

// The host's own checks, run against the example plug-ins built beside the
// program (ofx-test).
int checkOfxHost(QStringList &failures, const QString &testPlugins);
// The plug-in host process against plug-ins that misbehave on purpose
// (ofx-hazards); the worker must be in use.
int checkOfxWorkerHazards(QStringList &failures, const QString &hazardPlugins, const QString &testPlugins);
