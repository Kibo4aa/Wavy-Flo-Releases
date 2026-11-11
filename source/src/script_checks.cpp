// --scripts-self-test: scripts run against a scene of their own, through
// both ways in -- wavy and After Effects' -- and their windows pressed.
#include "script_host.h"
#include "character_animation.h"
#include "jsxbin.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJSEngine>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QWidget>

namespace {
struct Rig {
  Scene scene;
  QStringList selection;
  int checkpoints = 0, refreshes = 0;
  QStringList log;
  ScriptHost host() {
    ScriptHost h;
    h.scene = [this]() -> Scene & { return scene; };
    h.mainName = [] { return QString("Main 01"); };
    h.activeComposition = [] { return QString(); };
    h.selectedLayers = [this] { return selection; };
    h.selectLayers = [this](const QStringList &ids) { selection = ids; };
    h.checkpoint = [this] { ++checkpoints; };
    h.refresh = [this] { ++refreshes; };
    h.effectNames = [] { return QStringList{"Gaussian Blur", "Glow", "Drop Shadow"}; };
    h.applyEffect = [this](const QString &id, const QString &name) {
      for (Layer &l : scene.layers)
        if (l.id == id) {
          l.appliedEffects[name] = true;
          if (name == "Gaussian Blur") l.blur = 6;
          if (name == "Glow") l.glow = 12;
          return true;
        }
      return false;
    };
    h.log = [this](const QString &line) { log << line; };
    h.textPresets = [] {
      return QVariantList{QVariantMap{{"name", "Fade Up"},
                                      {"settings", QVariantMap{{"x", 0}, {"y", 60}, {"opacity", 0}, {"delay", 2}, {"smooth", true}}}},
                          QVariantMap{{"name", "Pop"}, {"settings", QVariantMap{{"scale", 0}, {"opacity", 0}, {"pop", true}}}}};
    };
    h.version = "0.40.2";
    return h;
  }
};
const Layer *named(const Scene &s, const QString &name) {
  for (const Layer &l : s.layers)
    if (l.name == name) return &l;
  return nullptr;
}
const Key *keyAt(const Property &p, double frame) {
  for (const Key &k : p.keys)
    if (std::abs(k.frame - frame) < 1e-6) return &k;
  return nullptr;
}
} // namespace

int checkScripts(QStringList &failures) {
  auto fail = [&](const QString &what) { failures << what; };
  QTemporaryDir place;

  // Wavy-Flo's own API: a title made, keyed, eased, with an effect and a
  // marker -- one step for Undo, and the editor told once.
  {
    Rig rig;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const bool ran = run.runText(R"(
      var t = wavy.project.main.addText("Hello", { fontSize: 120, name: "Title" });
      t.position.setAt(0, [100, 540]);
      t.position.setAt(30, [960, 540]);
      t.position.ease(30, "easeOut");
      t.opacity.set(0.5);
      t.addEffect("Glow");
      t.patch({ tracking: 12 });
      wavy.project.addMarker(30, "landed");
      wavy.log("made", t.name, t.index);
    )", "wavy-test.js", &error);
    const Layer *title = named(rig.scene, "Title");
    const Key *landed = title ? keyAt(title->p[0], 30) : nullptr;
    if (!ran) fail("wavy: the script failed: " + error);
    else if (!title || title->text != "Hello" || title->fontSize != 120 || !keyAt(title->p[0], 0) || !landed ||
             landed->value != 960 || title->p[5].base != .5 || !title->appliedEffects.value("Glow").toBool() ||
             title->tracking != 12 || rig.scene.markers.size() != 1 || rig.scene.markers[0].label != "landed")
      fail("wavy: the title did not come out as the script made it");
    const Key *eased = title ? keyAt(title->p[0], 30) : nullptr;
    if (title && eased && keyAt(title->p[0], 0) && title->p[0].keys[1].ease != "easeOut")
      fail("wavy: ease(30, \"easeOut\") did not reach the key at 30 (" + title->p[0].keys[1].ease + ")");
    if (rig.checkpoints != 1 || rig.refreshes != 1)
      fail(QString("wavy: a run is one Undo step and one refresh, not %1 and %2").arg(rig.checkpoints).arg(rig.refreshes));
    if (!run.output().contains("made Title 1")) fail("wavy: wavy.log wrote " + run.output().join(" | "));
  }

  // After Effects' way: a text layer, keyed in seconds, Opacity in percent,
  // Source Text from a TextDocument, a temporal ease turned into a handle.
  {
    Rig rig;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const bool ran = run.runText(R"(
      app.beginUndoGroup("Make");
      var comp = app.project.activeItem;
      var t = comp.layers.addText("Hello");
      t.name = "AE Title";
      var pos = t.property("Transform").property("Position");
      pos.setValueAtTime(0, [100, 100]);
      pos.setValueAtTime(1, [300, 100]);
      t.transform.opacity.setValue(50);
      t.property("Source Text").setValue(new TextDocument("World"));
      var still = [new KeyframeEase(0, 75)];
      t.transform.scale.setValueAtTime(0, [100, 100]);
      t.transform.scale.setValueAtTime(1, [200, 200]);
      t.transform.scale.setTemporalEaseAtKey(2, still, still);
      var found = comp.layer("AE Title") !== null && comp.layer(1).name === "AE Title" && comp.numLayers === 1 &&
                  pos.numKeys === 2 && pos.keyTime(2) === 1 && t instanceof TextLayer && comp instanceof CompItem;
      var opacity = t.transform.opacity.value;
      var second = pos.keyValue(2);
      app.endUndoGroup();
    )", "ae-test.jsx", &error);
    const Layer *title = named(rig.scene, "AE Title");
    if (!ran) fail("ae: the script failed: " + error);
    else if (!title || title->text != "World" || !keyAt(title->p[0], 0) || !keyAt(title->p[0], 30) || keyAt(title->p[0], 30)->value != 300 ||
             std::abs(title->p[5].base - .5) > 1e-9)
      fail("ae: the layer did not come out as the script made it");
    if (run.evaluate("found") != "true") fail("ae: comp.layer / numKeys / keyTime / instanceof came out wrong");
    if (run.evaluate("opacity") != "50") fail("ae: Opacity read back as " + run.evaluate("opacity") + ", not 50");
    if (run.evaluate("second") != "[300,100,0]") fail("ae: Position's second key read back as " + run.evaluate("second"));
    // Scale's key at 1 s (frame 30), eased in still over 75% of the move:
    // the handle into it 22.5 frames back, at its own value.
    if (title && title->p[2].keys.size() == 2) {
      const auto c = segmentPoints(title->p[2].keys[0], title->p[2].keys[1]);
      if (std::abs(c[2].x() - 7.5) > 1e-6 || std::abs(c[2].y() - 2) > 1e-6)
        fail(QString("ae: an ease of speed 0, influence 75 at 1 s made the handle %1,%2, not 7.5,2").arg(c[2].x()).arg(c[2].y()));
    } else fail("ae: Scale was not keyed twice");
    // setValue on a keyed property is refused, as After Effects refuses it.
    QString refused;
    run.runText("app.project.activeItem.layer(1).transform.position.setValue([1, 2]);", "refused.jsx", &refused);
    if (!refused.contains("keyframes") || !refused.contains("line 1")) fail("ae: setValue on a keyed property said " + refused);  }

  // #include, and an error placed in the file and line it was written on.
  {
    QFile part(place.filePath("part.jsxinc"));
    part.open(QIODevice::WriteOnly);
    part.write("var fromPart = 41;\nfunction broken() {\n  return missing.thing;\n}\n");
    part.close();
    QFile main(place.filePath("main.jsx"));
    main.open(QIODevice::WriteOnly);
    main.write("#target aftereffects\n#include \"part.jsxinc\"\nvar total = fromPart + 1;\nbroken();\n");
    main.close();
    Rig rig;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    run.runFile(place.filePath("main.jsx"), &error);
    if (run.evaluate("total") != "42") fail("include: the included file did not run first (total " + run.evaluate("total") + ")");
    if (!error.startsWith("part.jsxinc, line 3:")) fail("include: the error was placed at \"" + error + "\", not part.jsxinc, line 3");
    if (run.evaluate("$.fileName").isEmpty() || !run.evaluate("$.fileName").endsWith("main.jsx"))
      fail("include: $.fileName is " + run.evaluate("$.fileName"));
  }

  // A compiled script that is not whole says so, by name.
  {
    QFile compiled(place.filePath("broken.jsxbin"));
    compiled.open(QIODevice::WriteOnly);
    compiled.write("@JSXBIN@ES@2.0@MyBbyBn0AB");
    compiled.close();
    Rig rig;
    ScriptRunner run(rig.host());
    QString error;
    if (run.runFile(place.filePath("broken.jsxbin"), &error) || !error.contains("broken.jsxbin"))
      fail("jsxbin: a broken compiled script was not reported (" + error + ")");
  }

  // ScriptUI: a palette from a resource string, its button pressed and its
  // slider moved; a dialog's OK closes it with 1.
  {
    Rig rig;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const bool ran = run.runText(R"(
      var clicks = 0, seen = -1, changed = "";
      var w = new Window("palette{text:'Tools', orientation:'column', alignChildren:['fill','top'], \
        row: Group{go: Button{text:'Go'}, amount: Slider{minvalue:0, maxvalue:10, value:5}}, \
        name: EditText{text:'abc', characters: 12}, pick: DropDownList{}}");
      w.row.go.onClick = function () { clicks++; };
      w.row.amount.onChange = function () { seen = Math.round(this.value); };
      w.pick.add("item", "One"); w.pick.add("item", "Two");
      w.pick.selection = 1;
      w.pick.onChange = function () { changed = this.selection.text; };
      w.show();
      var picked = w.pick.selection.text;
      var d = new Window("dialog", "Ask");
      d.add("statictext", undefined, "Sure?");
      var ok = d.add("button", undefined, "OK", { name: "ok" });
      var result = d.show();
    )", "ui-test.jsx", &error);
    if (!ran) fail("scriptui: the script failed: " + error);
    if (!run.press("Go") || run.evaluate("clicks") != "1") fail("scriptui: the button's onClick did not run (" + run.evaluate("clicks") + ")");
    if (!run.setControlValue("amount", 7) || run.evaluate("seen") != "7") fail("scriptui: the slider's onChange read " + run.evaluate("seen"));
    if (run.evaluate("picked") != "Two") fail("scriptui: the dropdown's selection read " + run.evaluate("picked"));
    if (run.evaluate("w.name.text") != "abc") fail("scriptui: the edit text read " + run.evaluate("w.name.text"));
    if (!run.setControlValue("pick", 0) || run.evaluate("changed") != "One") fail("scriptui: the dropdown's onChange read " + run.evaluate("changed"));
    if (!run.alive()) fail("scriptui: a palette left open did not keep its script alive");
  }

  // Files and settings: written, read, listed; a setting kept.
  {
    Rig rig;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const QString folder = QDir::fromNativeSeparators(place.path());
    const bool ran = run.runText(QString(R"(
      var f = new File("%1/note.txt");
      f.open("w"); f.writeln("first"); f.write("second"); f.close();
      f.open("r"); var one = f.readln(); var two = f.readln(); f.close();
      var listed = new Folder("%1").getFiles("*.txt").length;
      var size = f.length;
      app.settings.saveSetting("WavyFloSelfTest", "Answer", "42");
      var kept = app.settings.haveSetting("WavyFloSelfTest", "Answer") && app.settings.getSetting("WavyFloSelfTest", "Answer");
      wavy.fs.writeJSON("%1/data.json", { a: [1, 2] });
      var back = wavy.fs.readJSON("%1/data.json").a[1];
    )").arg(folder), "files-test.jsx", &error);
    QSettings("Wavy-Flo", "Wavy-Flo").remove("Scripts/WavyFloSelfTest");
    if (!ran) fail("files: the script failed: " + error);
    if (run.evaluate("one") != "first" || run.evaluate("two") != "second") fail("files: read back " + run.evaluate("one") + " / " + run.evaluate("two"));
    if (run.evaluate("listed") != "1") fail("files: Folder.getFiles found " + run.evaluate("listed"));
    if (run.evaluate("kept") != "42") fail("files: app.settings kept " + run.evaluate("kept"));
    if (run.evaluate("back") != "2") fail("files: wavy.fs JSON came back " + run.evaluate("back"));
  }

  // Nothing held back: a layer read and changed whole; groups; a frame rendered.
  {
    Rig rig;
    rig.scene.width = 64;
    rig.scene.height = 36;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const QString picture = QDir::fromNativeSeparators(place.filePath("frame.png"));
    const bool ran = run.runText(QString(R"(
      var main = wavy.project.main;
      var box = main.addShape("rect", { name: "Box", width: 20, height: 10, color: [1, 0, 0, 1] });
      var dot = main.addShape("circle", { name: "Dot" });
      var data = box.data;
      box.patch({ blendMode: "screen", masks: [] });
      var grouped = main.group([box, dot], "Pair");
      var inside = grouped.layers.length;
      var rendered = main.render(0, "%1");
      var pre = app.project.activeItem.layers.precompose([1], "Again");
    )").arg(picture), "deep-test.js", &error);
    const Layer *box = named(rig.scene, "Box");
    if (!ran) fail("deep: the script failed: " + error);
    else if (!box || box->blendMode != "screen" || box->groupId.isEmpty()) fail("deep: patch / group did not reach the layer");
    if (run.evaluate("inside") != "2") fail("deep: the group holds " + run.evaluate("inside"));
    if (run.evaluate("typeof data.properties") != "object") fail("deep: layer.data has no properties: " + run.evaluate("typeof data.properties"));
    if (!QFile::exists(picture)) fail("deep: the frame was not rendered to " + picture);
    if (run.evaluate("pre.name") != "Again") fail("deep: precompose made " + run.evaluate("pre.name"));
  }
  // What Wavy-Flo does that the API reaches: Pulse on Beats, Line Up in Time,
  // Reverse Keyframes, more transforms, masks from paths, presets, pictures,
  // the network.
  {
    Rig rig;
    rig.scene.frames = 120;
    rig.scene.markers = {Marker{10, "", true}, Marker{40, "", true}, Marker{70, "", true}};
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const QString preset = place.filePath("look.wavypreset");
    const QString picture = place.filePath("dot.png");
    QFile page(place.filePath("page.txt"));
    page.open(QIODevice::WriteOnly);
    page.write("{\"answer\": 42}");
    page.close();
    // A web server of the test's own, on this machine: one answer to anything.
    QTcpServer server;
    server.listen(QHostAddress::LocalHost);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
      QTcpSocket *socket = server.nextPendingConnection();
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
        const QByteArray asked = socket->readAll();
        const QByteArray body = asked.startsWith("POST") ? QByteArray("{\"posted\": true}") : QByteArray("{\"answer\": 7}");
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
    });
    const QString web = QString("http://127.0.0.1:%1/").arg(server.serverPort());
    const bool ran = run.runText(QString(R"(
      var main = wavy.project.main;
      var a = main.addText('A', { name: 'A' }), b = main.addText('B', { name: 'B' }), c = main.addText('C', { name: 'C' });
      var pulses = a.pulseOnBeats({ move: 'scale', strength: 0.2, settle: 5 });
      var lined = wavy.project.lineUp([a, b, c], { from: 10, step: 5 });
      var starts = [a.start, b.start, c.start].join(',');
      b.opacity.setAt(0, 0);
      b.opacity.setAt(30, 1);
      var reversed = b.opacity.reverseKeys();
      var opacityAtStart = b.opacity.keyedAt(0);
      var t2 = c.addTransform('Wobble');
      t2.position = [5, 7];
      var transforms = c.transforms.length + ':' + c.transforms[0].name + ':' + t2.position.value.slice(0, 2).join(',');
      var mask = a.addMask(wavy.path.rect(0, 0, 50, 20), { mode: 'Subtract', feather: 3 });
      a.addMask(wavy.path.ellipse(25, 10, 8));
      var masks = a.masks.length + ':' + a.masks[0].mode + ':' + a.masks[1].points.length;
      a.glow = 9;
      a.tracking = 33;
      a.savePreset('%1');
      c.applyPreset('%1');
      var copied = c.glow + ':' + c.tracking + ':' + c.name;
      var pic = wavy.image.create(4, 3, [1, 0, 0, 1]);
      pic.setPixel(1, 1, [0, 0, 255]);
      var px = pic.pixel(1, 1).join(',') + '|' + pic.pixel(0, 0).join(',');
      var avg = Math.round(pic.average()[0]);
      pic.save('%2');
      var back = wavy.image.load('%2');
      var size = back.width + 'x' + back.height + ':' + back.pixel(1, 1).join(',');
      var frame = main.renderImage(0, 64);
      var rendered = frame.width + 'x' + frame.height;
      var got = wavy.net.get('%3');
      var fetched = got.ok + ':' + got.json().answer;
      var web = wavy.net.getJSON('%4').answer + ':' + wavy.net.post('%4', { hello: 1 }).json().posted;
    )").arg(preset, picture, QUrl::fromLocalFile(place.filePath("page.txt")).toString(), web), "features-test.js", &error);
    if (!ran) fail("features: the script failed: " + error);
    if (run.evaluate("pulses").toInt() < 3) fail("pulseOnBeats wrote " + run.evaluate("pulses") + " keys");
    if (run.evaluate("starts") != "10,15,20") fail("lineUp put the layers at " + run.evaluate("starts"));
    if (run.evaluate("reversed") != "true" || run.evaluate("opacityAtStart") != "1")
      fail("reverseKeys: " + run.evaluate("reversed") + ", opacity at 0 now " + run.evaluate("opacityAtStart"));
    if (run.evaluate("transforms") != "1:Wobble:5,7") fail("addTransform: " + run.evaluate("transforms"));
    if (run.evaluate("masks") != "2:Subtract:4") fail("addMask: " + run.evaluate("masks"));
    if (run.evaluate("copied") != "9:33:C") fail("presets: " + run.evaluate("copied"));
    if (run.evaluate("px") != "0,0,255,255|255,0,0,255") fail("picture pixels: " + run.evaluate("px"));
    if (run.evaluate("avg") != "234") fail("picture average red: " + run.evaluate("avg"));
    if (run.evaluate("size") != "4x3:0,0,255,255") fail("picture saved and read back: " + run.evaluate("size"));
    if (run.evaluate("rendered") != "64x36") fail("renderImage: " + run.evaluate("rendered"));
    if (run.evaluate("fetched") != "true:42") fail("wavy.net.get: " + run.evaluate("fetched") + " " + run.evaluate("JSON.stringify(got)"));
    if (run.evaluate("web") != "7:true") fail("wavy.net over HTTP: " + run.evaluate("web"));
    if (rig.checkpoints != 1) fail(QString("features: one run, one Undo step, not %1").arg(rig.checkpoints));
  }

  // Text Animations from a script: a preset and settings of its own, keyed,
  // stacked, reordered, copied, switched off, taken away; a Range keyed.
  {
    Rig rig;
    rig.scene.frames = 120;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const bool ran = run.runText(R"(
      var t = wavy.project.main.addText('WAVY FLO', { name: 'Anim' });
      var a = t.addTextAnimator({ preset: 'Fade Up', from: 0, duration: 20 });
      var b = t.addTextAnimator({ y: 120, opacity: 0, order: 'random', basedOn: 'words', mode: 'alternate', name: 'Rise' });
      b.animateIn(5, 30);
      b.animateOut(60, 80, 'easeIn');
      var names = t.textAnimators.map(function (x) { return x.name; }).join(',');
      var aKeys = a['in'].keys.map(function (k) { return k.frame + '=' + k.value; }).join(' ');
      var bOwn = [b.get('order'), b.get('basedOn'), b.get('mode'), b.get('y'), b.edited, a.edited].join(':');
      var bOut = b.out.keys.length + ':' + b.out.keys[0].ease;
      a.set({ y: 30, fillColor: [1, 0, 0, 1], colorAmount: 50 });
      var aOwn = a.get('y') + ':' + a.get('fillColor') + ':' + a.summary;
      var c = a.duplicate();
      c.name = 'Copy';
      c.moveTo(0);
      var ordered = t.textAnimators.map(function (x) { return x.name; }).join(',');
      b.enabled = false;
      var off = b.enabled;
      c.remove();
      var left = t.textAnimators.map(function (x) { return x.name; }).join(',');
      var end = a.range('end');
      end.setAt(0, 100);
      end.setAt(30, 40);
      a.edit();
      var editedNow = a.edited + ':' + a['in'].keys.length;
      // Smooth Strength is no setting: an old preset's or script's eases the
      // straight keys once (Fade Up's own ease-in-out stays) and is not kept.
      var old = wavy.project.main.addText('OLD', { name: 'Old' });
      var s = old.addTextAnimator({ y: 40, opacity: 0, smooth: true, from: 0, duration: 10, ease: 'linear' });
      var sKey = s['in'].keys[0], aKey = a['in'].keys[0];
      var smoothed = [String(s.get('smooth')), String(a.get('smooth')), sKey.custom, sKey.ease,
                      sKey.curve.map(function (c) { return Math.round(c * 1000) / 1000; }).join(','),
                      aKey.ease, aKey.custom].join(':');
      var presets = wavy.textPresets.map(function (p) { return p.name; }).join(',');
      var refused = '';
      try { wavy.project.main.addFill([0, 0, 0, 1]).addTextAnimator('Pop'); } catch (e) { refused = e.message; }
      var held = t.data.appliedEffects['Text-Animation'] + ':' + t.textAnimators.length + ':' +
                 (t.props().indexOf('TextAnim/' + a.uid + '/Range End') >= 0);
      // What it does, seen: faded out at the start, there once it has come in.
      wavy.project.main.layers.forEach(function (l) { if (l.name !== 'Anim') l.remove(); });
      t.remove();
      var seen = wavy.project.main.addText('O', { name: 'Seen', fontSize: 300, color: [1, 1, 1, 1] });
      seen.position = [wavy.project.width / 2, wavy.project.height / 2];
      seen.addTextAnimator({ preset: 'Fade Up', from: 0, duration: 10 });
      var dark = wavy.image.render(0, 192).average()[0], lit = wavy.image.render(20, 192).average()[0];
    )", "text-animation-test.js", &error);
    if (!ran) fail("text animation: the script failed: " + error);
    if (run.evaluate("names") != "Fade Up,Rise") fail("text animation: the stack reads " + run.evaluate("names"));
    if (run.evaluate("aKeys") != "0=0 20=100") fail("text animation: the preset's In keys are " + run.evaluate("aKeys"));
    if (run.evaluate("bOwn") != "6:1:2:120:true:false") fail("text animation: settings " + run.evaluate("bOwn"));
    if (run.evaluate("bOut") != "2:easeIn") fail("text animation: Out keys " + run.evaluate("bOut"));
    if (!run.evaluate("aOwn").startsWith("30:#ff0000:")) fail("text animation: set() " + run.evaluate("aOwn"));
    if (run.evaluate("ordered") != "Copy,Fade Up,Rise") fail("text animation: duplicated and moved " + run.evaluate("ordered"));
    if (run.evaluate("off") != "false") fail("text animation: switching off");
    if (run.evaluate("left") != "Fade Up,Rise") fail("text animation: removed, the stack reads " + run.evaluate("left"));
    if (run.evaluate("editedNow") != "true:2") fail("text animation: edit() " + run.evaluate("editedNow"));
    if (run.evaluate("smoothed") != "undefined:undefined:true:custom:0.333,0,0.667,1:easeInOut:false")
      fail("text animation: an old smooth setting " + run.evaluate("smoothed"));
    if (run.evaluate("presets") != "Fade Up,Pop") fail("text animation: presets " + run.evaluate("presets"));
    if (!run.evaluate("refused").contains("text layers")) fail("text animation on a fill: " + run.evaluate("refused"));
    if (run.evaluate("held") != "true:2:true") fail("text animation: the layer holds " + run.evaluate("held"));
    if (run.evaluate("lit - dark > 2") != "true")
      fail("text animation: the letters do not come in (" + run.evaluate("dark") + " then " + run.evaluate("lit") + ")");
  }

  // Expression Controls from a script: made, keyed, read by an expression on
  // another layer, renamed, a Layer control pointed, one taken away.
  {
    Rig rig;
    rig.scene.frames = 60;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const bool ran = run.runText(R"(
      var main = wavy.project.main;
      var rig = main.addEmpty({ name: 'Rig' });
      var speed = rig.addControl('slider', 'Speed', 10);
      speed.setAt(0, 0);
      speed.setAt(30, 90);
      var aim = rig.addControl('point', 'Aim', [200, 100]);
      var tint = rig.addControl('color', 'Tint', [0, 0.5, 1, 1]);
      var lead = rig.addControl('layer', 'Lead');
      var box = main.addShape('rect', { name: 'Box' });
      lead.value = box;
      box.rotation.expression = 'thisComp.layer("Rig").' + speed.reference;
      var turned = box.rotation.valueAt(15);
      var names = rig.controls.map(function (c) { return c.name + ':' + c.kind; }).join(',');
      var aimValue = aim.value.join(',');
      var tintValue = tint.value.join(',');
      var leadName = lead.value.name;
      speed.name = 'Pace';
      var renamed = speed.reference;
      var fromEffect = rig.addEffect('Checkbox Control').kind;
      aim.remove();
      var left = rig.controls.map(function (c) { return c.name; }).join(',');
      var found = rig.control('pace') !== null && rig.control('Aim') === null;
    )", "controls-test.js", &error);
    if (!ran) fail("controls: the script failed: " + error);
    if (run.evaluate("turned") != "45") fail("controls: an expression reading a keyed Slider gave " + run.evaluate("turned"));
    if (run.evaluate("names") != "Speed:slider,Aim:point,Tint:color,Lead:layer") fail("controls: " + run.evaluate("names"));
    if (run.evaluate("aimValue") != "200,100" || run.evaluate("tintValue") != "0,0.5,1,1")
      fail("controls: values " + run.evaluate("aimValue") + " / " + run.evaluate("tintValue"));
    if (run.evaluate("leadName") != "Box") fail("controls: the Layer control holds " + run.evaluate("leadName"));
    if (run.evaluate("renamed") != "effect(\"Pace\")(\"Slider\")") fail("controls: renamed " + run.evaluate("renamed"));
    if (run.evaluate("fromEffect") != "checkbox") fail("controls: addEffect('Checkbox Control') made " + run.evaluate("fromEffect"));
    if (run.evaluate("left") != "Pace,Tint,Lead,Checkbox Control") fail("controls: left " + run.evaluate("left"));
    if (run.evaluate("found") != "true") fail("controls: control(name)");
    // The renamed control breaks the expression that named it, as in After Effects.
    if (rig.checkpoints != 1) fail(QString("controls: one run, one Undo step, not %1").arg(rig.checkpoints));
  }

  // Motion tracking from a script: footage made by the script itself (a dot
  // drifting right and down, rendered to H.264), a point tracked through it,
  // a title carried with it, and the shot held still about it.
  {
    Rig rig;
    rig.scene.width = 320;
    rig.scene.height = 180;
    rig.scene.frames = 30;
    rig.scene.background = Qt::black;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const QString movie = place.filePath("dot.mp4");
    const bool ran = run.runText(QString(R"(
      var main = wavy.project.main;
      var dot = main.addShape('circle', { name: 'Dot', width: 24, height: 24, color: [1, 1, 1, 1] });
      dot.position.setAt(0, [80, 60]);
      dot.position.setAt(29, [200, 120]);
      main.renderVideo('%1', { format: 'mp4', audio: false });
      dot.remove();
      var shot = main.add('video', { name: 'Shot', path: '%1', duration: 30 });
      var point = shot.addTrackPoint('Dot', [80 - 160, 60 - 90], { frame: 0, feature: 31, search: 81 });
      var calls = 0;
      var result = point.track({ from: 0, to: 29, onProgress: function () { calls++; } });
      var samples = point.samples;
      var last = point.at(29);
      var title = main.addText('On it', { name: 'Label' });
      title.position = [100, 40];
      var keys = point.applyTo(title, 0);
      var carried = title.position.valueAt(29);
      var held = point.stabilize(0);
      var names = shot.trackPoints.map(function (p) { return p.name; }).join(',');
    )").arg(QDir::fromNativeSeparators(movie)), "tracking-test.js", &error);
    if (!ran) fail("tracking: the script failed: " + error);
    if (run.evaluate("result.frames") != "29" || run.evaluate("result.error") != "")
      fail("tracking: " + run.evaluate("JSON.stringify(result)"));
    // The dot goes from (80,60) to (200,120): from the middle, (-80,-30) to (40,30).
    const double lastX = run.evaluate("last[0]").toDouble(), lastY = run.evaluate("last[1]").toDouble();
    if (std::abs(lastX - 40) > 1.5 || std::abs(lastY - 30) > 1.5)
      fail(QString("tracking: the point ended at %1,%2, not 40,30").arg(lastX).arg(lastY));
    if (run.evaluate("samples.length") != "30") fail("tracking: samples " + run.evaluate("samples.length"));
    if (run.evaluate("calls > 0") != "true") fail("tracking: onProgress was not called");
    const double carriedX = run.evaluate("carried[0]").toDouble(), carriedY = run.evaluate("carried[1]").toDouble();
    if (run.evaluate("keys").toInt() < 20 || std::abs(carriedX - 220) > 2 || std::abs(carriedY - 100) > 2)
      fail(QString("tracking: applyTo wrote %1 keys and carried the title to %2,%3, not 220,100")
               .arg(run.evaluate("keys")).arg(carriedX).arg(carriedY));
    if (run.evaluate("held").toInt() < 20) fail("tracking: stabilize wrote " + run.evaluate("held") + " keys");
    if (run.evaluate("names") != "Dot") fail("tracking: points " + run.evaluate("names"));
  }

  // A movie of the Main from a script: a PNG sequence of three frames, and
  // one stopped part way by its progress.
  {
    Rig rig;
    rig.scene.width = 320;
    rig.scene.height = 180;
    rig.scene.frames = 30;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const QString out = place.filePath("renders");
    const bool ran = run.runText(QString(R"(
      wavy.project.main.addText('Frames', { name: 'F' });
      var seen = [];
      var folder = wavy.project.main.renderVideo('%1', { format: 'png', from: 0, to: 2, audio: false,
                                                         onProgress: function (done, total) { seen.push(done); } });
      var stopped = wavy.project.main.renderVideo('%1', { format: 'png', from: 0, to: 9, audio: false,
                                                          onProgress: function (done) { return done < 2; } });
      var bad = '';
      try { wavy.project.main.renderVideo('%1', { format: 'gif' }); } catch (e) { bad = e.message; }
    )").arg(out), "render-test.js", &error);
    if (!ran) fail("renderVideo: the script failed: " + error);
    const QString folder = run.evaluate("folder");
    const int pictures = QDir(folder).entryList({"*.png"}, QDir::Files).size();
    if (pictures != 3) fail(QString("renderVideo: %1 pictures in %2").arg(pictures).arg(folder));
    if (run.evaluate("stopped") != "null") fail("renderVideo: a render stopped by its progress gave " + run.evaluate("stopped"));
    if (run.evaluate("seen.length > 0") != "true") fail("renderVideo: onProgress was not called");
    if (!run.evaluate("bad").contains("format")) fail("renderVideo with a bad format said " + run.evaluate("bad"));
  }

  // wavy.ui: a window of lines, its values read and set, its button pressed.
  {
    Rig rig;
    ScriptRunner run(rig.host());
    run.setUnattended(true);
    QString error;
    const bool ran = run.runText(R"(
      var made = '';
      var size, fill, glow, mode, words, amount, pic, folded;
      var w = wavy.ui.window('Title Tools', function (ui) {
        size = ui.slider('Size', 10, 400, 120);
        amount = ui.number('Amount', 2.5, { min: 0, max: 10, step: 0.5 });
        fill = ui.color('Fill', [1, 0.5, 0, 1]);
        glow = ui.checkbox('Glow', true);
        mode = ui.dropdown('Mode', ['Pop', 'Slide', 'Fade'], 1);
        words = ui.text('Words', 'Hello');
        ui.separator();
        ui.section('More', function (more) { folded = more.label('inside'); }, false);
        pic = ui.picture(wavy.image.create(8, 8, [0, 1, 0, 1]));
        ui.button('Make', function () {
          var t = wavy.project.main.addText(words.value, { name: 'Made', fontSize: size.value });
          t.color = fill.value;
          made = mode.text + ':' + glow.value + ':' + amount.value;
        });
      });
      size.value = 200;
      fill.value = [0, 0, 1, 1];
      amount.value = 7.5;
      var read = [Math.round(size.value), fill.value.join(','), amount.value, glow.value, mode.value, mode.text, words.value].join('|');
    )", "ui-test.js", &error);
    if (!ran) fail("wavy.ui: the script failed: " + error);
    if (run.evaluate("read") != "200|0,0,1,1|7.5|true|1|Slide|Hello") fail("wavy.ui values: " + run.evaluate("read"));
    if (!run.press("Make")) fail("wavy.ui: no Make button");
    if (run.evaluate("made") != "Slide:true:7.5") fail("wavy.ui button: " + run.evaluate("made"));
    const Layer *made = named(rig.scene, "Made");
    if (!made || made->fontSize != 200 || made->color.rgba() != QColor(0, 0, 255).rgba())
      fail(QString("wavy.ui: the button's layer came out wrong (%1 %2)")
               .arg(made ? made->fontSize : -1)
               .arg(made ? made->color.name(QColor::HexArgb) : QString("none")));
    if (run.evaluate("folded.visible") != "false") fail("wavy.ui: a section shut at first shows its lines");
    // How it looks, kept beside the program to be looked at.
    for (QWidget *top : QApplication::topLevelWidgets())
      if (top->windowTitle() == "Title Tools" && top->isVisible())
        top->grab().save(QCoreApplication::applicationDirPath() + "/scripts-ui-self-test.png");
    if (!run.alive()) fail("wavy.ui: the window's script did not stay alive");
  }
  return failures.isEmpty() ? 0 : 1;
}

// --jsxbin-self-test <folder>: every compiled script under the folder read
// back and compiled, not run. What a script says is never written out --
// only each file's name, and what went wrong where -- unless
// --jsxbin-self-test-context asks for the lines around a failure, to mend
// the reader with.
int checkJsxbinFolder(const QString &folder, QStringList &report, bool context) {
  int read = 0, bad = 0;
  QDirIterator it(folder, {"*.jsxbin"}, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString path = it.next();
    const QString name = QDir(folder).relativeFilePath(path);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
      report << "FAIL " + name + ": cannot read";
      ++bad;
      continue;
    }
    QElapsedTimer clock;
    clock.start();
    const JsxbinScript compiled = decodeJsxbin(QString::fromUtf8(f.readAll()));
    const qint64 ms = clock.elapsed();
    if (!compiled.ok()) {
      report << "FAIL " + name + ": " + compiled.error;
      ++bad;
      continue;
    }
    QJSEngine engine;
    const QJSValue result = engine.evaluate("(function(){" + compiled.code + "\n})", name, 1);
    if (result.isError()) {
      const int line = result.property("lineNumber").toInt();
      report << QString("FAIL %1: line %2 (from line %3): %4")
                    .arg(name)
                    .arg(line)
                    .arg(line > 0 && line <= int(compiled.lines.size()) ? compiled.lines[size_t(line - 1)] : 0)
                    .arg(result.property("message").toString());
      if (context) {
        const QStringList lines = compiled.code.split('\n');
        for (int i = std::max(1, line - 2); i <= std::min(int(lines.size()), line + 2); ++i)
          report << QString("    %1| %2").arg(i).arg(lines[i - 1].left(400));
      }
      ++bad;
      continue;
    }
    ++read;
    report << QString("ok %1 (%2 ms, %3 lines)").arg(name).arg(ms).arg(compiled.lines.size());
  }
  report.prepend(QString("%1 read, %2 failed").arg(read).arg(bad));
  return bad ? 1 : 0;
}

// --script-self-test-run <file>: a script run on a new, empty project of its
// own (1920x1080, 30 fps, 300 frames), no editor, dialogs answered at once --
// for trying scripts out and rendering what they make. Its log and any error
// go in the report.
int runScriptAlone(const QString &file, QStringList &report, std::function<QVariantList()> textPresets) {
  Rig rig;
  rig.scene.frames = 300;
  ScriptHost host = rig.host();
  if (textPresets) host.textPresets = textPresets;
  ScriptRunner run(host);
  run.setUnattended(true);
  QString error;
  const bool ran = run.runFile(file, &error);
  report << run.output() << rig.log;
  if (!ran) report << "ERROR " + error;
  return ran ? 0 : 1;
}
