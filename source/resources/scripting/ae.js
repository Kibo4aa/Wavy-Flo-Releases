// After Effects' scripting world, laid over Wavy-Flo (wavy.js): app,
// app.project, CompItem (the Main and each group), the layers, their
// properties and keyframes, TextDocument, markers, effects, $, File, Folder,
// alert, confirm, prompt and system -- so scripts written for After Effects
// run here as far as Wavy-Flo has what they ask for. Times are seconds, as
// in After Effects; Scale and Opacity are percent.
//
// What Wavy-Flo has no counterpart for -- text animators built through the
// After Effects API, expression controls, presets (.ffx), shape layer
// contents, masks' paths -- stops the script with a message that says so,
// rather than doing something else. Wavy-Flo answers as itself: app.name is
// "Wavy-Flo", app.version its own.
(function (global) {
  'use strict';
  var N = global.__wf, wavy = global.wavy;

  function fps() { return N.project().fps; }
  function toFrame(seconds) { return Math.round(Number(seconds) * fps() * 1000) / 1000; }
  function toSeconds(frame) { return frame / fps(); }
  function unavailable(what) { throw new Error(what + ' is not available in Wavy-Flo'); }

  // --- Enumerations (After Effects' numbers where they are known) ------------
  global.PropertyValueType = { NO_VALUE: 6412, ThreeD_SPATIAL: 6413, ThreeD: 6414, TwoD_SPATIAL: 6415, TwoD: 6416,
                               OneD: 6417, COLOR: 6418, CUSTOM_VALUE: 6419, MARKER: 6420, LAYER_INDEX: 6421,
                               MASK_INDEX: 6422, SHAPE: 6423, TEXT_DOCUMENT: 6424 };
  global.PropertyType = { PROPERTY: 6812, INDEXED_GROUP: 6813, NAMED_GROUP: 6814 };
  global.KeyframeInterpolationType = { LINEAR: 6612, BEZIER: 6613, HOLD: 6614 };
  global.ParagraphJustification = { LEFT_JUSTIFY: 7413, RIGHT_JUSTIFY: 7414, CENTER_JUSTIFY: 7415,
                                    FULL_JUSTIFY_LASTLINE_LEFT: 7416, FULL_JUSTIFY_LASTLINE_RIGHT: 7417,
                                    FULL_JUSTIFY_LASTLINE_CENTER: 7418, FULL_JUSTIFY_LASTLINE_FULL: 7419,
                                    MULTIPLE_JUSTIFICATIONS: 7420 };
  global.TrackMatteType = { NO_TRACK_MATTE: 5012, ALPHA: 5013, ALPHA_INVERTED: 5014, LUMA: 5015, LUMA_INVERTED: 5016 };
  global.LayerQuality = { BEST: 4612, DRAFT: 4613, WIREFRAME: 4614 };
  global.AutoOrientType = { NO_AUTO_ORIENT: 4212, ALONG_PATH: 4213, CAMERA_OR_POINT_OF_INTEREST: 4214, CHARACTERS_TOWARD_CAMERA: 4215 };
  global.LightType = { PARALLEL: 4412, SPOT: 4413, POINT: 4414, AMBIENT: 4415 };
  global.ImportAsType = { COMP_CROPPED_LAYERS: 3812, FOOTAGE: 3813, COMP: 3814, PROJECT: 3815 };
  global.CloseOptions = { DO_NOT_SAVE_CHANGES: 1212, PROMPT_TO_SAVE_CHANGES: 1213, SAVE_CHANGES: 1214 };
  global.FrameBlendingType = { FRAME_MIX: 4012, NO_FRAME_BLEND: 4013, PIXEL_MOTION: 4014 };
  var BLEND = { normal: 5212, darken: 5214, multiply: 5215, colorBurn: 5216, lighten: 5219, screen: 5220,
                colorDodge: 5221, add: 5222, overlay: 5223, softLight: 5224, hardLight: 5225, difference: 5227,
                exclusion: 5228 };
  global.BlendingMode = { NORMAL: 5212, DISSOLVE: 5213, DARKEN: 5214, MULTIPLY: 5215, COLOR_BURN: 5216,
                          CLASSIC_COLOR_BURN: 5217, LINEAR_BURN: 5218, LIGHTEN: 5219, SCREEN: 5220,
                          COLOR_DODGE: 5221, ADD: 5222, OVERLAY: 5223, SOFT_LIGHT: 5224, HARD_LIGHT: 5225,
                          LINEAR_LIGHT: 5226, DIFFERENCE: 5227, EXCLUSION: 5228, HUE: 5229, SATURATION: 5230,
                          COLOR: 5231, LUMINOSITY: 5232 };
  function blendName(number) {
    for (var k in BLEND) if (BLEND[k] === number) return k;
    unavailable('That blending mode');
  }
  function KeyframeEase(speed, influence) {
    if (!(this instanceof KeyframeEase)) return new KeyframeEase(speed, influence);
    this.speed = Number(speed) || 0;
    this.influence = Math.min(100, Math.max(0.1, Number(influence === undefined ? 16.666666667 : influence)));
  }
  global.KeyframeEase = KeyframeEase;
  function Shape() { this.vertices = []; this.inTangents = []; this.outTangents = []; this.closed = true; }
  global.Shape = Shape;
  // E4X (XML written in the script itself) in a compiled script: no
  // JavaScript engine of today reads it, so it stops there and says so.
  global.__wfNoE4X = function () {
    throw new Error('This script writes XML in the language itself (E4X), which Wavy-Flo does not run');
  };

  // --- $ ----------------------------------------------------------------------
  var hires = Date.now();
  var dollar = {
    fileName: '',
    get os() { return N.windows() ? 'Windows/64 ' + N.os() : N.os(); },
    version: '4.5.5',
    build: '1',
    buildDate: new Date(2026, 0, 1),
    engineName: 'main',
    global: global,
    level: 0,
    locale: 'en_US',
    decimalPoint: '.',
    error: null,
    flags: 0,
    memCache: 0,
    includePath: '',
    get line() { return 0; },
    get stack() { return ''; },
    get screens() { return [{ left: 0, top: 0, right: 1920, bottom: 1080, primary: true }]; },
    get hiresTimer() { var now = Date.now(), d = (now - hires) * 1000; hires = now; return d; },
    writeln: function () { N.log(Array.prototype.join.call(arguments, '')); },
    write: function () { N.log(Array.prototype.join.call(arguments, '')); },
    sleep: function (ms) { N.sleep(ms); },
    gc: function () {},
    bp: function () {},
    clearbp: function () {},
    setbp: function () {},
    summary: function () { return ''; },
    list: function () { return ''; },
    getenv: function (name) { return N.env(name) || null; },
    setenv: function () {},
    colorPicker: function () { return -1; },
    evalFile: function (path) {
      var p = path && path.fsName ? path.fsName : String(path);
      return (0, eval)(N.evalFileText(p, ''));
    },
    toString: function () { return '[object $]'; }
  };
  global.$ = dollar;
  var begin = global.__wfBegin;
  global.__wfBegin = function (file) {
    begin(file);
    dollar.fileName = file;
  };

  global.alert = function (message, title, errorIcon) { N.alert(String(message), title || '', !!errorIcon); };
  global.confirm = function (message, noAsDefault, title) { return N.confirm(String(message), title || '', !!noAsDefault); };
  global.prompt = function (message, preset, title) {
    var r = N.prompt(String(message), preset === undefined ? '' : String(preset), title || '');
    return r === undefined ? null : r;
  };
  global.system = {
    callSystem: function (command) { return N.run(String(command), 0).output; },
    osName: 'Windows',
    userName: N.env('USERNAME') || N.env('USER') || '',
    machineName: N.env('COMPUTERNAME') || ''
  };

  // --- File and Folder --------------------------------------------------------
  function uriOf(native) {
    var p = String(native).replace(/\\/g, '/');
    var m = /^([A-Za-z]):\/?(.*)$/.exec(p);
    if (m) p = '/' + m[1].toLowerCase() + '/' + m[2];
    return p.split('/').map(function (part) { return encodeURIComponent(part).replace(/%3A/g, ':'); }).join('/');
  }
  function FsEntry() {}
  FsEntry.prototype = {
    get fsName() { return N.fsNative(this.__path).replace(/\//g, N.windows() ? '\\' : '/'); },
    get fullName() { return uriOf(N.fsNative(this.__path)); },
    get absoluteURI() { return this.fullName; },
    get relativeURI() { return this.fullName; },
    get name() { var n = N.fsNative(this.__path).replace(/[\\\/]+$/, ''); return encodeURIComponent(n.substring(n.search(/[^\\\/]*$/))); },
    get displayName() { return decodeURIComponent(this.name); },
    get path() { var f = this.fullName; return f.substring(0, f.lastIndexOf('/')); },
    get parent() {
      var n = N.fsNative(this.__path).replace(/[\\\/]+$/, '');
      var at = Math.max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
      return at > 0 ? new Folder(n.substring(0, at)) : null;
    },
    get exists() { return N.fsExists(this.__path); },
    get hidden() { var s = N.fsStat(this.__path); return !!(s && s.hidden); },
    get readonly() { var s = N.fsStat(this.__path); return !!(s && s.readonly); },
    get modified() { var s = N.fsStat(this.__path); return s && s.modified ? new Date(s.modified) : null; },
    get created() { var s = N.fsStat(this.__path); return s && s.created ? new Date(s.created) : null; },
    get alias() { return false; },
    error: '',
    remove: function () { return N.fsRemove(this.__path); },
    rename: function (newName) {
      var target = (this.parent ? this.parent.fsName + '/' : '') + newName;
      if (!N.fsRename(this.__path, target)) return false;
      this.__path = target;
      return true;
    },
    execute: function () { return N.fsOpen(this.__path); },
    resolve: function () { return this; },
    changePath: function (p) { this.__path = resolvePath(p, this.parent ? this.parent.fsName : ''); return true; },
    getRelativeURI: function () { return this.fullName; },
    toString: function () { return this.fullName; },
    valueOf: function () { return this.fullName; }
  };
  function resolvePath(p, base) {
    p = p && p.fsName ? p.fsName : String(p === undefined ? '' : p);
    p = decodeURIComponent(p.replace(/%(?![0-9A-Fa-f]{2})/g, '%25'));
    var n = N.fsNative(p);
    if (/^([A-Za-z]:|[\\\/])/.test(n) || !base) return n;
    return N.fsNative(base + '/' + p);
  }
  function File(path) {
    if (!(this instanceof File)) return new File(path);
    this.__path = path === undefined ? N.fsSpecial('temp') + '/untitled-' + Date.now() : resolvePath(path, N.fsSpecial('current'));
    this.encoding = 'UTF-8';
    this.lineFeed = N.windows() ? 'Windows' : 'Unix';
    this.__open = false;
    this.__mode = '';
    this.__text = '';
    this.__at = 0;
  }
  File.prototype = Object.create(FsEntry.prototype);
  File.prototype.constructor = File;
  Object.defineProperties(File.prototype, {
    length: {
      get: function () { var s = N.fsStat(this.__path); return s && s.size !== undefined ? s.size : 0; },
      set: function () {}
    },
    eof: { get: function () { return !this.__open || this.__at >= this.__text.length; } },
    type: { get: function () { return '????'; } },
    creator: { get: function () { return '????'; } }
  });
  File.prototype.open = function (mode) {
    var m = String(mode || 'r').toLowerCase();
    this.__mode = m;
    this.__open = true;
    this.__at = 0;
    if (m === 'r' || m === 'e') {
      var t = N.fsRead(this.__path, this.encoding);
      if (t === null || t === undefined) {
        if (m === 'r') { this.__open = false; this.error = 'cannot open'; return false; }
        t = '';
      }
      this.__text = t;
    } else if (m === 'a') {
      var existing = N.fsRead(this.__path, this.encoding);
      this.__text = existing === null || existing === undefined ? '' : existing;
      this.__at = this.__text.length;
    } else {
      this.__text = '';
    }
    return true;
  };
  File.prototype.close = function () {
    if (!this.__open) return false;
    var ok = true;
    if (this.__mode !== 'r') ok = N.fsWrite(this.__path, this.__text, false, this.encoding);
    this.__open = false;
    return ok;
  };
  File.prototype.read = function (chars) {
    if (!this.__open) return '';
    var n = chars === undefined ? this.__text.length - this.__at : chars;
    var out = this.__text.substr(this.__at, n);
    this.__at += out.length;
    return out;
  };
  File.prototype.readch = function () { return this.read(1); };
  File.prototype.readln = function () {
    if (!this.__open) return '';
    var rest = this.__text.substring(this.__at), at = rest.search(/\r\n|\n|\r/);
    if (at < 0) { this.__at = this.__text.length; return rest; }
    var nl = rest.substr(at, 2) === '\r\n' ? 2 : 1;
    this.__at += at + nl;
    return rest.substring(0, at);
  };
  File.prototype.write = function () {
    if (!this.__open) return false;
    var s = Array.prototype.join.call(arguments, '');
    this.__text = this.__text.substring(0, this.__at) + s + this.__text.substring(this.__at + s.length);
    this.__at += s.length;
    return true;
  };
  File.prototype.writeln = function () {
    return this.write(Array.prototype.join.call(arguments, '') + (this.lineFeed === 'Windows' ? '\r\n' : '\n'));
  };
  File.prototype.seek = function (pos, mode) {
    var base = mode === 1 ? this.__at : mode === 2 ? this.__text.length : 0;
    this.__at = Math.max(0, Math.min(this.__text.length, base + pos));
    return true;
  };
  File.prototype.tell = function () { return this.__at; };
  File.prototype.copy = function (target) { return N.fsCopy(this.__path, resolvePath(target, '')); };
  File.prototype.createAlias = function () { return false; };
  File.prototype.openDlg = function (prompt, filter, multi) { return File.openDialog(prompt, filter, multi, this.__path); };
  File.prototype.saveDlg = function (prompt, filter) { return File.saveDialog(prompt, filter, this.__path); };
  File.openDialog = function (prompt, filter, multi, start) {
    var picked = N.fsOpenDialog(prompt || 'Open', typeof filter === 'string' ? filter : '', !!multi, start || '');
    if (!picked.length) return null;
    var files = picked.map(function (p) { return new File(p); });
    return multi ? files : files[0];
  };
  File.saveDialog = function (prompt, filter, start) {
    var p = N.fsSaveDialog(prompt || 'Save', typeof filter === 'string' ? filter : '', start || '');
    return p ? new File(p) : null;
  };
  File.decode = function (s) { return decodeURIComponent(s); };
  File.encode = function (s) { return encodeURIComponent(s); };
  File.isEncodingAvailable = function () { return true; };
  File.fs = N.windows() ? 'Windows' : 'Unix';

  function Folder(path) {
    if (!(this instanceof Folder)) return new Folder(path);
    this.__path = path === undefined ? N.fsSpecial('current') : resolvePath(path, N.fsSpecial('current'));
  }
  Folder.prototype = Object.create(FsEntry.prototype);
  Folder.prototype.constructor = Folder;
  Folder.prototype.create = function () { return N.fsMakeFolder(this.__path); };
  Folder.prototype.getFiles = function (mask) {
    var all = N.fsList(this.__path, typeof mask === 'string' ? mask : '');
    var out = all.map(function (p) { return N.fsIsFolder(p) ? new Folder(p) : new File(p); });
    return typeof mask === 'function' ? out.filter(mask) : out;
  };
  Folder.prototype.selectDlg = function (prompt) { return Folder.selectDialog(prompt, this.__path); };
  Folder.selectDialog = function (prompt, start) {
    var p = N.fsFolderDialog(prompt || 'Choose a folder', start || '');
    return p ? new Folder(p) : null;
  };
  Folder.decode = File.decode;
  Folder.encode = File.encode;
  Folder.fs = File.fs;
  Object.defineProperties(Folder, {
    current: { get: function () { return new Folder(N.fsSpecial('current')); }, set: function () {} },
    desktop: { get: function () { return new Folder(N.fsSpecial('desktop')); } },
    myDocuments: { get: function () { return new Folder(N.fsSpecial('documents')); } },
    temp: { get: function () { return new Folder(N.fsSpecial('temp')); } },
    userData: { get: function () { return new Folder(N.fsSpecial('userData')); } },
    appData: { get: function () { return new Folder(N.fsSpecial('appData')); } },
    appPackage: { get: function () { return new Folder(N.fsSpecial('program')); } },
    startup: { get: function () { return new Folder(N.fsSpecial('program')); } },
    system: { get: function () { return new Folder(N.env('SystemRoot') || '/'); } },
    commonFiles: { get: function () { return new Folder(N.env('CommonProgramFiles') || '/'); } },
    trash: { get: function () { return null; } }
  });
  global.File = File;
  global.Folder = Folder;

  // --- Properties --------------------------------------------------------------
  function Base() {}
  Base.prototype = {
    get propertyDepth() { var d = 0, p = this.parentProperty; while (p) { d++; p = p.parentProperty; } return d; },
    get isModified() { return true; },
    get canSetEnabled() { return false; },
    get elided() { return false; },
    get active() { return this.enabled; },
    get enabled() { return true; },
    selected: false,
    propertyGroup: function (n) {
      var p = this, steps = n === undefined ? 1 : n;
      while (steps-- > 0 && p) p = p.parentProperty;
      return p;
    },
    toString: function () { return '[object Property]'; }
  };

  // A group: its properties by number (1 the first), name or match name.
  function Group(name, matchName, parent, children, indexInParent) {
    this.name = name;
    this.matchName = matchName;
    this.parentProperty = parent || null;
    this.__children = children || function () { return []; };
    this.propertyIndex = indexInParent || 1;
    this.propertyType = PropertyType.NAMED_GROUP;
  }
  Group.prototype = Object.create(Base.prototype);
  Object.defineProperty(Group.prototype, 'numProperties', { get: function () { return this.__children().length; } });
  Group.prototype.property = function (which) {
    var list = this.__children();
    if (typeof which === 'number') return list[which - 1] || null;
    var w = String(which);
    for (var i = 0; i < list.length; i++)
      if (list[i].name === w || list[i].matchName === w || (list[i].altName && list[i].altName === w)) return list[i];
    return null;
  };
  Group.prototype.canAddProperty = function () { return false; };
  Group.prototype.addProperty = function (name) { unavailable('Adding "' + name + '" to ' + this.name); };
  Group.prototype.remove = function () { unavailable('Removing ' + this.name); };

  // A property made of Wavy-Flo properties, one for each dimension.
  //   keys: wavy property keys; factor: After Effects' units per Wavy-Flo's
  function Prop(layer, spec, parent, index) {
    this.__layer = layer;
    this.__spec = spec;
    this.name = spec.name;
    this.altName = spec.alt;
    this.matchName = spec.match;
    this.parentProperty = parent;
    this.propertyIndex = index;
    this.propertyType = PropertyType.PROPERTY;
    this.expressionError = '';
  }
  Prop.prototype = Object.create(Base.prototype);
  Object.defineProperties(Prop.prototype, {
    __parts: { get: function () { var l = this.__layer.__w; return this.__spec.keys.map(function (k) { return l.prop(k); }); } },
    __factor: { get: function () { return this.__spec.factor || 1; } },
    __single: { get: function () { return this.__spec.keys.length === 1; } },
    propertyValueType: {
      get: function () {
        var s = this.__spec;
        if (s.keys.length === 1) return PropertyValueType.OneD;
        return s.spatial ? PropertyValueType.ThreeD_SPATIAL : PropertyValueType.ThreeD;
      }
    },
    value: { get: function () { return this.valueAtTime(toSeconds(N.project().frame), false); } },
    isSpatial: { get: function () { return !!this.__spec.spatial; } },
    canVaryOverTime: { get: function () { return true; } },
    canSetExpression: { get: function () { return true; } },
    unitsText: { get: function () { return this.__spec.units || 'pixels'; } },
    hasMin: { get: function () { return this.__spec.min !== undefined; } },
    hasMax: { get: function () { return this.__spec.max !== undefined; } },
    minValue: { get: function () { return this.__spec.min; } },
    maxValue: { get: function () { return this.__spec.max; } },
    isTimeVarying: { get: function () { return this.numKeys > 0 || (this.expressionEnabled && this.expression !== ''); } },
    __frames: {
      get: function () {
        var seen = {}, out = [];
        this.__parts.forEach(function (p) {
          p.keys.forEach(function (k) {
            var f = Math.round(k.frame * 1000);
            if (!seen[f]) { seen[f] = true; out.push(k.frame); }
          });
        });
        return out.sort(function (a, b) { return a - b; });
      }
    },
    numKeys: { get: function () { return this.__frames.length; } },
    expression: {
      get: function () { return this.__parts[0].expression; },
      set: function (code) {
        var self = this;
        this.__parts.forEach(function (p) { p.expression = code; if (p.expressionError) self.expressionError = p.expressionError; });
      }
    },
    expressionEnabled: {
      get: function () { return this.__parts[0].expressionEnabled; },
      set: function (on) { this.__parts.forEach(function (p) { p.expressionEnabled = on; }); }
    },
    dimensionsSeparated: {
      get: function () { return this.__spec.separable ? this.__layer.__w.get(this.__spec.separable) : false; },
      set: function (on) { if (this.__spec.separable) this.__layer.__w.set(this.__spec.separable, !!on); }
    },
    selectedKeys: { get: function () { return []; } }
  });
  function scaledOut(prop, values) {
    var f = prop.__factor;
    var out = values.map(function (v) { return v * f; });
    if (prop.__spec.pad !== undefined) while (out.length < prop.__spec.pad.length) out.push(prop.__spec.pad[out.length]);
    return prop.__single ? out[0] : out;
  }
  function valuesIn(prop, v) {
    var list = typeof v === 'number' ? [v] : v;
    if (!list || list.length === undefined) throw new Error('A value for ' + prop.name + ' must be a number or an array');
    var f = prop.__factor;
    return list.map(function (x) { return Number(x) / f; });
  }
  Prop.prototype.valueAtTime = function (t) {
    var frame = toFrame(t);
    return scaledOut(this, this.__parts.map(function (p) { return p.valueAt(frame); }));
  };
  Prop.prototype.setValue = function (v) {
    if (this.numKeys > 0) throw new Error('Unable to call "setValue" because the property has keyframes: use setValueAtTime');
    var values = valuesIn(this, v), parts = this.__parts;
    for (var i = 0; i < parts.length && i < values.length; i++) parts[i].set(values[i]);
  };
  Prop.prototype.setValueAtTime = function (t, v) {
    var values = valuesIn(this, v), parts = this.__parts, frame = toFrame(t);
    for (var i = 0; i < parts.length && i < values.length; i++) parts[i].setAt(frame, values[i]);
  };
  Prop.prototype.setValuesAtTimes = function (times, values) {
    for (var i = 0; i < times.length; i++) this.setValueAtTime(times[i], values[i]);
  };
  Prop.prototype.__frameOf = function (index) {
    var frames = this.__frames;
    if (index < 1 || index > frames.length) throw new Error('There is no keyframe ' + index + ' (it has ' + frames.length + ')');
    return frames[index - 1];
  };
  Prop.prototype.keyTime = function (i) { return toSeconds(this.__frameOf(i)); };
  Prop.prototype.keyValue = function (i) {
    var frame = this.__frameOf(i);
    return scaledOut(this, this.__parts.map(function (p) { return p.keyedAt(frame); }));
  };
  Prop.prototype.addKey = function (t) {
    var frame = toFrame(t);
    this.__parts.forEach(function (p) { p.setAt(frame, p.keyedAt(frame)); });
    return this.nearestKeyIndex(t);
  };
  Prop.prototype.removeKey = function (i) {
    var frame = this.__frameOf(i);
    this.__parts.forEach(function (p) { p.removeKey(frame); });
  };
  Prop.prototype.nearestKeyIndex = function (t) {
    var frames = this.__frames, frame = toFrame(t), best = 1, gap = Infinity;
    for (var i = 0; i < frames.length; i++) if (Math.abs(frames[i] - frame) < gap) { gap = Math.abs(frames[i] - frame); best = i + 1; }
    return frames.length ? best : 0;
  };
  Prop.prototype.setValueAtKey = function (i, v) { this.setValueAtTime(this.keyTime(i), v); };
  // Interpolation: a move between two keys is one thing in Wavy-Flo -- held,
  // straight, or a curve with handles -- so a key's out-type says what its
  // move becomes, and its in-type what the move into it does.
  function keyOf(part, frame) {
    var keys = part.keys;
    for (var i = 0; i < keys.length; i++) if (Math.abs(keys[i].frame - frame) < 1e-3) return { list: keys, at: i };
    return null;
  }
  function makeCurve(part, list, at) {
    // A straight move as a curve: handles a third of the way along it.
    var a = list[at], b = list[at + 1];
    if (!b || a.custom) return;
    var dt = b.frame - a.frame, dv = b.value - a.value;
    part.setHandles(a.frame, [a.frame + dt / 3, a.value + dv / 3], [b.frame - dt / 3, b.value - dv / 3]);
  }
  Prop.prototype.setInterpolationTypeAtKey = function (i, inType, outType) {
    if (outType === undefined) outType = inType;
    var frame = this.__frameOf(i);
    this.__parts.forEach(function (p) {
      var k = keyOf(p, frame);
      if (!k) return;
      if (outType === KeyframeInterpolationType.HOLD) p.ease(frame, 'hold');
      else if (outType === KeyframeInterpolationType.LINEAR) p.ease(frame, 'linear');
      else makeCurve(p, k.list, k.at);
      if (k.at > 0) {
        var before = k.list[k.at - 1];
        if (inType === KeyframeInterpolationType.HOLD) p.ease(before.frame, 'hold');
        else if (inType === KeyframeInterpolationType.BEZIER) makeCurve(p, k.list, k.at - 1);
        else if (before.ease === 'hold') p.ease(before.frame, 'linear');
      }
    });
  };
  function typeOf(key) {
    if (!key) return KeyframeInterpolationType.LINEAR;
    if (key.ease === 'hold') return KeyframeInterpolationType.HOLD;
    return key.ease === 'linear' && !key.custom ? KeyframeInterpolationType.LINEAR : KeyframeInterpolationType.BEZIER;
  }
  Prop.prototype.keyOutInterpolationType = function (i) {
    var k = keyOf(this.__parts[0], this.__frameOf(i));
    return typeOf(k ? k.list[k.at] : null);
  };
  Prop.prototype.keyInInterpolationType = function (i) {
    var k = keyOf(this.__parts[0], this.__frameOf(i));
    if (!k || k.at === 0) return KeyframeInterpolationType.LINEAR;
    var before = k.list[k.at - 1];
    return before.ease === 'hold' ? KeyframeInterpolationType.LINEAR : typeOf(before);
  };
  // Temporal ease: a speed (units a second) and an influence (% of the time
  // to the neighbouring key), as the handle of a Wavy-Flo curve.
  Prop.prototype.setTemporalEaseAtKey = function (i, inEase, outEase) {
    if (outEase === undefined) outEase = inEase;
    var frame = this.__frameOf(i), rate = fps(), factor = this.__factor, spatial = this.__spec.spatial;
    var parts = this.__parts;
    // A spatial property's one ease is a speed along its path: shared out
    // between the dimensions as the move between the two keys is.
    function share(d, list, at, neighbour) {
      if (!spatial) return 1;
      var total = 0, mine = 0;
      parts.forEach(function (p, n) {
        var k = keyOf(p, frame);
        if (!k || !k.list[neighbour]) return;
        var dv = k.list[neighbour].value - k.list[k.at].value;
        total += dv * dv;
        if (n === d) mine = dv;
      });
      return total > 0 ? mine / Math.sqrt(total) : 0;
    }
    parts.forEach(function (p, d) {
      var k = keyOf(p, frame);
      if (!k) return;
      var list = k.list, at = k.at;
      var ein = inEase[Math.min(d, inEase.length - 1)], eout = outEase[Math.min(d, outEase.length - 1)];
      if (at + 1 < list.length && eout) {
        var a = list[at], b = list[at + 1], dt = b.frame - a.frame, h = p.handles(a.frame) || { 'in': [b.frame - dt / 3, b.value] };
        var reach = dt * eout.influence / 100, speed = eout.speed / factor / rate * (spatial ? Math.abs(share(d, list, at, at + 1)) * (b.value >= a.value ? 1 : -1) : 1);
        if (a.ease === 'hold') p.ease(a.frame, 'linear');
        p.setHandles(a.frame, [a.frame + reach, a.value + speed * reach], h['in']);
      }
      if (at > 0 && ein) {
        var a2 = list[at - 1], b2 = list[at], dt2 = b2.frame - a2.frame;
        var h2 = p.handles(a2.frame) || { out: [a2.frame + dt2 / 3, a2.value] };
        var reach2 = dt2 * ein.influence / 100, speed2 = ein.speed / factor / rate * (spatial ? Math.abs(share(d, list, at, at - 1)) * (b2.value >= a2.value ? 1 : -1) : 1);
        p.setHandles(a2.frame, h2.out, [b2.frame - reach2, b2.value - speed2 * reach2]);
      }
    });
  };
  function easeOf(prop, i, outgoing) {
    var frame = prop.__frameOf(i), rate = fps(), factor = prop.__factor;
    var parts = prop.__spec.spatial ? [prop.__parts[0]] : prop.__parts;
    return parts.map(function (p) {
      var k = keyOf(p, frame);
      if (!k) return new KeyframeEase(0, 16.666666667);
      var list = k.list, at = k.at;
      var from = outgoing ? at : at - 1;
      if (from < 0 || from + 1 >= list.length) return new KeyframeEase(0, 16.666666667);
      var a = list[from], b = list[from + 1], dt = b.frame - a.frame;
      var h = p.handles(a.frame);
      if (!h || a.ease === 'linear' || !a.custom) return new KeyframeEase((b.value - a.value) / dt * rate * factor, 16.666666667);
      var handle = outgoing ? h.out : h['in'], anchor = outgoing ? a : b;
      var span = Math.abs(handle[0] - anchor.frame);
      return new KeyframeEase(span > 1e-9 ? (handle[1] - anchor.value) / (handle[0] - anchor.frame) * rate * factor : 0,
                              Math.max(0.1, span / dt * 100));
    });
  }
  Prop.prototype.keyInTemporalEase = function (i) { return easeOf(this, i, false); };
  Prop.prototype.keyOutTemporalEase = function (i) { return easeOf(this, i, true); };
  // The motion path's handles at a key: offsets from the key, as Wavy-Flo keeps them.
  Prop.prototype.setSpatialTangentsAtKey = function (i, inTangent, outTangent) {
    if (!this.__spec.spatial) throw new Error(this.name + ' has no motion path');
    var frame = this.__frameOf(i);
    if (outTangent === undefined) outTangent = [-inTangent[0], -inTangent[1]];
    this.__parts.slice(0, 2).forEach(function (p) {
      p.setKey(frame, { spatialIn: [inTangent[0], inTangent[1]], spatialOut: [outTangent[0], outTangent[1]] });
    });
  };
  Prop.prototype.keyInSpatialTangent = function (i) {
    var k = keyOf(this.__parts[0], this.__frameOf(i)), key = k ? k.list[k.at] : null;
    return key && key.spatial ? [key.spatialIn[0], key.spatialIn[1], 0] : [0, 0, 0];
  };
  Prop.prototype.keyOutSpatialTangent = function (i) {
    var k = keyOf(this.__parts[0], this.__frameOf(i)), key = k ? k.list[k.at] : null;
    return key && key.spatial ? [key.spatialOut[0], key.spatialOut[1], 0] : [0, 0, 0];
  };
  ['setTemporalContinuousAtKey', 'setTemporalAutoBezierAtKey', 'setSpatialContinuousAtKey',
   'setSpatialAutoBezierAtKey', 'setRovingAtKey', 'setSelectedAtKey', 'setLabelAtKey'].forEach(function (name) {
    Prop.prototype[name] = function () {};
  });
  ['keyTemporalContinuous', 'keyTemporalAutoBezier', 'keySpatialContinuous', 'keySpatialAutoBezier',
   'keyRoving', 'keySelected'].forEach(function (name) {
    Prop.prototype[name] = function () { return false; };
  });
  Prop.prototype.keyLabel = function () { return 0; };
  Prop.prototype.getSeparationFollower = function (dim) {
    var t = this.parentProperty;
    return t ? t.property(['X Position', 'Y Position', 'Z Position'][dim]) : null;
  };
  Prop.prototype.addToMotionGraphicsTemplate = function () { return false; };

  // Source Text: a TextDocument.
  function TextDocument(text) {
    if (!(this instanceof TextDocument)) return new TextDocument(text);
    this.text = text === undefined ? '' : String(text);
    this.fontSize = 72;
    this.font = 'Arial';
    this.fillColor = [1, 1, 1];
    this.strokeColor = [0, 0, 0];
    this.strokeWidth = 0;
    this.applyFill = true;
    this.applyStroke = false;
    this.strokeOverFill = false;
    this.justification = ParagraphJustification.CENTER_JUSTIFY;
    this.tracking = 0;
    this.leading = 0;
    this.autoLeading = true;
    this.fauxBold = false;
    this.fauxItalic = false;
    this.allCaps = false;
    this.smallCaps = false;
    this.baselineShift = 0;
    this.horizontalScale = 100;
    this.verticalScale = 100;
    this.boxText = false;
    this.pointText = true;
    this.boxTextSize = [0, 0];
  }
  TextDocument.prototype.resetCharStyle = function () {};
  TextDocument.prototype.resetParagraphStyle = function () {};
  TextDocument.prototype.toString = function () { return this.text; };
  global.TextDocument = TextDocument;
  var JUSTIFY = [ParagraphJustification.LEFT_JUSTIFY, ParagraphJustification.CENTER_JUSTIFY,
                 ParagraphJustification.RIGHT_JUSTIFY, ParagraphJustification.FULL_JUSTIFY_LASTLINE_LEFT];
  function documentOf(w) {
    var d = new TextDocument(w.text);
    var size = w.fontSize;
    d.fontSize = size;
    d.font = w.fontStyle ? w.font + '-' + w.fontStyle.replace(/\s+/g, '') : w.font;
    var c = w.color;
    d.fillColor = [c[0], c[1], c[2]];
    var strokes = w.strokes;
    if (strokes.length) {
      d.applyStroke = true;
      d.strokeWidth = strokes[0].width;
      d.strokeColor = strokes[0].color.slice(0, 3);
    }
    d.applyFill = !w.strokeOnly;
    d.justification = JUSTIFY[w.align] || ParagraphJustification.CENTER_JUSTIFY;
    d.tracking = size > 0 ? w.tracking / size * 1000 : 0;
    d.leading = w.leading * size;
    d.autoLeading = Math.abs(w.leading - 1.2) < 1e-6;
    d.fauxBold = w.fauxBold;
    d.fauxItalic = w.fauxItalic;
    d.allCaps = w.capitals === 1;
    d.smallCaps = w.capitals === 2;
    d.boxText = w.paragraphWidth > 0;
    d.pointText = !d.boxText;
    d.boxTextSize = [w.paragraphWidth, 0];
    return d;
  }
  function writeDocument(w, d) {
    if (typeof d === 'string') { w.text = d; return; }
    if (d.text !== undefined) w.text = String(d.text);
    if (d.fontSize) w.fontSize = d.fontSize;
    if (d.font) {
      // A PostScript name ("Arial-BoldMT") as family and face.
      var parts = String(d.font).split('-');
      w.font = parts[0];
      w.fontStyle = parts.length > 1 ? parts.slice(1).join(' ').replace(/MT$/, '').replace(/([a-z])([A-Z])/g, '$1 $2') : '';
    }
    if (d.fillColor) w.color = [d.fillColor[0], d.fillColor[1], d.fillColor[2], 1];
    if (d.applyFill !== undefined) w.strokeOnly = !d.applyFill && !!d.applyStroke;
    if (d.applyStroke && d.strokeWidth > 0) w.strokes = [{ width: d.strokeWidth, color: d.strokeColor || [0, 0, 0], mode: d.strokeOverFill ? 2 : 1, join: 0 }];
    else if (d.applyStroke === false) w.strokes = [];
    var at = JUSTIFY.indexOf(d.justification);
    if (at >= 0) w.align = at;
    var size = d.fontSize || w.fontSize;
    if (d.tracking !== undefined) w.tracking = d.tracking / 1000 * size;
    if (d.leading && !d.autoLeading) w.leading = d.leading / size;
    if (d.fauxBold !== undefined) w.fauxBold = !!d.fauxBold;
    if (d.fauxItalic !== undefined) w.fauxItalic = !!d.fauxItalic;
    if (d.allCaps) w.capitals = 1;
    else if (d.smallCaps) w.capitals = 2;
    else if (d.allCaps === false && d.smallCaps === false) w.capitals = 0;
    if (d.boxText && d.boxTextSize) w.paragraphWidth = d.boxTextSize[0];
  }
  function SourceText(layer, parent) {
    this.__layer = layer;
    this.name = 'Source Text';
    this.matchName = 'ADBE Text Document';
    this.parentProperty = parent;
    this.propertyIndex = 1;
    this.propertyType = PropertyType.PROPERTY;
    this.propertyValueType = PropertyValueType.TEXT_DOCUMENT;
    this.numKeys = 0;
    this.expression = '';
    this.expressionEnabled = false;
    this.expressionError = '';
    this.isTimeVarying = false;
    this.canSetExpression = false;
  }
  SourceText.prototype = Object.create(Base.prototype);
  Object.defineProperty(SourceText.prototype, 'value', { get: function () { return documentOf(this.__layer.__w); } });
  SourceText.prototype.valueAtTime = function () { return this.value; };
  SourceText.prototype.setValue = function (d) { writeDocument(this.__layer.__w, d); };
  // Wavy-Flo's words do not change over time: the value is set.
  SourceText.prototype.setValueAtTime = function (t, d) { this.setValue(d); };
  SourceText.prototype.keyValue = function () { return this.value; };

  // Markers: Wavy-Flo keeps one set, the Main's; a layer's markers are those.
  function MarkerValue(comment, chapter, url, frameTarget, cuePointName, params) {
    if (!(this instanceof MarkerValue)) return new MarkerValue(comment, chapter, url, frameTarget, cuePointName, params);
    this.comment = comment === undefined ? '' : String(comment);
    this.chapter = chapter || '';
    this.url = url || '';
    this.frameTarget = frameTarget || '';
    this.cuePointName = cuePointName || '';
    this.duration = 0;
    this.label = 0;
    this.protectedRegion = false;
    this.eventCuePoint = false;
    this.__params = params || {};
  }
  MarkerValue.prototype.getParameters = function () { return this.__params; };
  MarkerValue.prototype.setParameters = function (p) { this.__params = p || {}; };
  global.MarkerValue = MarkerValue;
  function Markers(parent) {
    this.name = 'Marker';
    this.matchName = 'ADBE Marker';
    this.parentProperty = parent || null;
    this.propertyIndex = 1;
    this.propertyType = PropertyType.PROPERTY;
    this.propertyValueType = PropertyValueType.MARKER;
    this.expression = '';
    this.expressionEnabled = false;
  }
  Markers.prototype = Object.create(Base.prototype);
  Object.defineProperty(Markers.prototype, 'numKeys', { get: function () { return N.markers().length; } });
  Object.defineProperty(Markers.prototype, 'isTimeVarying', { get: function () { return this.numKeys > 0; } });
  Markers.prototype.__marker = function (i) {
    var all = N.markers();
    if (i < 1 || i > all.length) throw new Error('There is no marker ' + i);
    return all[i - 1];
  };
  Markers.prototype.keyTime = function (i) { return toSeconds(this.__marker(i).frame); };
  Markers.prototype.keyValue = function (i) { return new MarkerValue(this.__marker(i).label); };
  Markers.prototype.setValueAtTime = function (t, mv) {
    var label = mv && mv.comment !== undefined ? mv.comment : String(mv === undefined ? '' : mv);
    N.addMarker(toFrame(t), label, false);
  };
  Markers.prototype.setValueAtKey = function (i, mv) { N.setMarker(i - 1, this.__marker(i).frame, mv.comment || ''); };
  Markers.prototype.removeKey = function (i) { this.__marker(i); N.removeMarker(i - 1); };
  Markers.prototype.nearestKeyIndex = function (t) {
    var all = N.markers(), frame = toFrame(t), best = 0, gap = Infinity;
    for (var i = 0; i < all.length; i++) if (Math.abs(all[i].frame - frame) < gap) { gap = Math.abs(all[i].frame - frame); best = i + 1; }
    return best;
  };
  Markers.prototype.addKey = function (t) { N.addMarker(toFrame(t), '', false); return this.nearestKeyIndex(t); };
  Markers.prototype.valueAtTime = function (t) { var i = this.nearestKeyIndex(t); return i ? this.keyValue(i) : null; };

  // --- Effects -------------------------------------------------------------------
  // After Effects' match names for the effects Wavy-Flo has its own of.
  var EFFECTS = { 'ADBE Gaussian Blur 2': 'Gaussian Blur', 'ADBE Gaussian Blur': 'Gaussian Blur',
                  'ADBE Fast Blur': 'Gaussian Blur', 'ADBE Box Blur2': 'Gaussian Blur', 'ADBE Glo2': 'Glow',
                  'ADBE Drop Shadow': 'Drop Shadow', 'ADBE Fill': 'Fill' };
  var BUILT_IN = { 'Gaussian Blur': { field: 'blur', setting: 'Blurriness' }, 'Glow': { field: 'glow', setting: 'Glow Radius' },
                   'Drop Shadow': { field: 'shadow', setting: 'Distance' } };
  function matchNameOf(name) {
    for (var k in EFFECTS) if (EFFECTS[k] === name) return k;
    return 'WAVY ' + name;
  }
  function EffectSetting(effect, key, index) {
    this.__effect = effect;
    this.__key = key;
    this.name = key;
    this.matchName = effect.matchName + '-' + key;
    this.parentProperty = effect;
    this.propertyIndex = index;
    this.propertyType = PropertyType.PROPERTY;
    this.propertyValueType = PropertyValueType.OneD;
    this.expression = '';
    this.expressionEnabled = false;
    this.expressionError = '';
  }
  EffectSetting.prototype = Object.create(Base.prototype);
  Object.defineProperties(EffectSetting.prototype, {
    __builtIn: { get: function () { return BUILT_IN[this.__effect.name]; } },
    value: {
      get: function () {
        var b = this.__builtIn;
        if (b) return this.__effect.__layer.__w.get(b.field);
        var w = this.__effect.__wavy, curves = N.curveIds(w.layer.id);
        if (curves.indexOf(w.name + '/' + this.__key) >= 0) return w.curve(this.__key).value;
        return w.get(this.__key);
      }
    },
    numKeys: {
      get: function () {
        var w = this.__effect.__wavy;
        return N.curveIds(w.layer.id).indexOf(w.name + '/' + this.__key) >= 0 ? w.curve(this.__key).keys.length : 0;
      }
    }
  });
  EffectSetting.prototype.valueAtTime = function (t) {
    var w = this.__effect.__wavy;
    if (N.curveIds(w.layer.id).indexOf(w.name + '/' + this.__key) >= 0) return w.curve(this.__key).valueAt(toFrame(t));
    return this.value;
  };
  EffectSetting.prototype.setValue = function (v) {
    var b = this.__builtIn;
    if (b) { this.__effect.__layer.__w.set(b.field, Number(v)); return; }
    var w = this.__effect.__wavy;
    if (N.curveIds(w.layer.id).indexOf(w.name + '/' + this.__key) >= 0) w.curve(this.__key).set(v);
    else w.set(this.__key, v);
  };
  EffectSetting.prototype.setValueAtTime = function (t, v) {
    if (this.__builtIn) unavailable('Keyframing ' + this.__effect.name + ' from a script');
    this.__effect.__wavy.curve(this.__key).setAt(toFrame(t), Number(v));
  };
  function AEEffect(layer, name, parent, index) {
    this.__layer = layer;
    this.__wavy = new wavy.Effect(layer.__w, name);
    this.name = name;
    this.matchName = matchNameOf(name);
    this.parentProperty = parent;
    this.propertyIndex = index;
    this.propertyType = PropertyType.INDEXED_GROUP;
  }
  AEEffect.prototype = Object.create(Group.prototype);
  AEEffect.prototype.__children = function () {
    var self = this, b = BUILT_IN[this.name];
    var keys = b ? [b.setting] : Object.keys(this.__wavy.settings).filter(function (k) { return k.charAt(0) !== '@'; });
    return keys.map(function (k, i) { return new EffectSetting(self, k, i + 1); });
  };
  Object.defineProperty(AEEffect.prototype, 'enabled', {
    get: function () { return this.__wavy.enabled; },
    set: function (on) { this.__wavy.enabled = on; }
  });
  AEEffect.prototype.property = function (which) {
    var list = this.__children();
    if (typeof which === 'number') return list[which - 1] || null;
    var w = String(which).toLowerCase();
    for (var i = 0; i < list.length; i++) if (list[i].name.toLowerCase() === w) return list[i];
    return null;
  };
  AEEffect.prototype.remove = function () { this.__wavy.remove(); };

  // --- Layers --------------------------------------------------------------------
  var TRANSFORM = [
    { attr: 'anchorPoint', name: 'Anchor Point', match: 'ADBE Anchor Point', keys: ['anchorX', 'anchorY', 'anchorZ'], factor: 1 },
    { attr: 'position', name: 'Position', match: 'ADBE Position', keys: ['positionX', 'positionY', 'positionZ'], factor: 1,
      spatial: true, separable: 'separatePosition' },
    { attr: 'xPosition', name: 'X Position', match: 'ADBE Position_0', keys: ['positionX'], factor: 1 },
    { attr: 'yPosition', name: 'Y Position', match: 'ADBE Position_1', keys: ['positionY'], factor: 1 },
    { attr: 'zPosition', name: 'Z Position', match: 'ADBE Position_2', keys: ['positionZ'], factor: 1 },
    { attr: 'scale', name: 'Scale', match: 'ADBE Scale', keys: ['scaleX', 'scaleY', 'scaleZ'], factor: 100, units: 'percent' },
    { attr: 'xRotation', name: 'X Rotation', match: 'ADBE Rotate X', keys: ['rotationX'], factor: 1, units: 'degrees' },
    { attr: 'yRotation', name: 'Y Rotation', match: 'ADBE Rotate Y', keys: ['rotationY'], factor: 1, units: 'degrees' },
    { attr: 'rotation', alt: 'Z Rotation', name: 'Rotation', match: 'ADBE Rotate Z', keys: ['rotation'], factor: 1, units: 'degrees' },
    { attr: 'opacity', name: 'Opacity', match: 'ADBE Opacity', keys: ['opacity'], factor: 100, units: 'percent', min: 0, max: 100 }
  ];
  function AELayer(id) { this.__w = new wavy.Layer(id); this.__id = id; }
  AELayer.prototype = {
    get name() { return this.__w.name; },
    set name(v) { this.__w.name = String(v); },
    get index() { return this.__w.index; },
    get id() { var h = 0, s = this.__id; for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return Math.abs(h); },
    get enabled() { return this.__w.visible; },
    set enabled(on) { this.__w.visible = !!on; },
    get active() { var f = N.project().frame, l = this.__w; return l.visible && f >= l.start && f < l.end; },
    get solo() { return this.__w.isolated; },
    set solo(on) { this.__w.isolated = !!on; },
    shy: false,
    get locked() { return this.__w.locked; },
    set locked(on) { this.__w.locked = !!on; },
    comment: '',
    label: 0,
    stretch: 100,
    quality: 4612,
    autoOrient: 4212,
    get isNameSet() { return this.__w.get('namedByHand'); },
    get threeDLayer() { return this.__w.threeD; },
    set threeDLayer(on) { this.__w.threeD = !!on; },
    get threeDPerChar() { return false; },
    get motionBlur() { return this.__w.motionBlur; },
    set motionBlur(on) { this.__w.motionBlur = !!on; },
    get blendingMode() { return BLEND[this.__w.blendMode] || BlendingMode.NORMAL; },
    set blendingMode(v) { this.__w.blendMode = blendName(v); },
    get parent() { var p = this.__w.parent; return p ? layerObject(p.id) : null; },
    set parent(l) { this.__w.parent = l ? l.__id : null; },
    get startTime() { return toSeconds(this.__w.start); },
    set startTime(t) { this.__w.start = toFrame(t); },
    get inPoint() { return toSeconds(this.__w.start); },
    set inPoint(t) {
      // Trimmed at the front: its keys stay where they are in the Main.
      var w = this.__w, f = toFrame(t), shift = f - w.start, end = w.end;
      if (!shift) return;
      w.start = f;
      w.duration = Math.max(1, end - f);
      N.shiftKeys(this.__id, -shift);
    },
    get outPoint() { return toSeconds(this.__w.end); },
    set outPoint(t) { this.__w.end = toFrame(t); },
    get time() { return toSeconds(N.project().frame); },
    get nullLayer() { return this.__w.type === 'empty'; },
    get adjustmentLayer() { return this.__w.type === 'effect'; },
    get hasVideo() { return this.__w.type !== 'audio'; },
    get hasAudio() { return this.__w.type === 'audio' || this.__w.type === 'video'; },
    get audioEnabled() { return this.__w.audioEnabled; },
    set audioEnabled(on) { this.__w.audioEnabled = !!on; },
    get isTrackMatte() {
      var me = this.__id;
      return N.layerIds('*').some(function (id) { return N.layerGet(id, 'trackMatte') === me; });
    },
    get hasTrackMatte() { return !!this.__w.trackMatte; },
    get trackMatteType() {
      if (!this.__w.trackMatte) return TrackMatteType.NO_TRACK_MATTE;
      return this.__w.trackMatteInverted ? TrackMatteType.ALPHA_INVERTED : TrackMatteType.ALPHA;
    },
    set trackMatteType(type) {
      if (type === TrackMatteType.NO_TRACK_MATTE) { this.__w.trackMatte = null; return; }
      // As in After Effects before it could be chosen: the layer above.
      var above = this.containingComp.layer(this.index - 1);
      if (!above) throw new Error('There is no layer above to be the matte');
      this.setTrackMatte(above, type);
    },
    get trackMatteLayer() { var m = this.__w.trackMatte; return m ? layerObject(m.id) : null; },
    setTrackMatte: function (layer, type) {
      this.__w.trackMatte = layer ? layer.__id : null;
      this.__w.trackMatteInverted = type === TrackMatteType.ALPHA_INVERTED || type === TrackMatteType.LUMA_INVERTED;
    },
    removeTrackMatte: function () { this.__w.trackMatte = null; },
    get containingComp() { return new CompItem(N.layerGet(this.__id, 'composition')); },
    get source() { return this.__w.type === 'group' ? new CompItem(this.__id) : null; },
    get selected() { return this.__w.selected; },
    set selected(on) { this.__w.selected = !!on; },
    get width() { return Math.round(this.__w.bounds().width); },
    get height() { return Math.round(this.__w.bounds().height); },
    frameBlending: false,
    timeRemapEnabled: false,
    guideLayer: false,
    collapseTransformation: false,
    preserveTransparency: false,
    environmentLayer: false,
    canSetCollapseTransformation: false,
    canSetTimeRemapEnabled: false,
    // Its properties' groups.
    __groups: function () {
      var self = this, out = [new Markers(this)];
      var type = this.__w.type;
      if (type === 'text') out.push(textGroup(this, out.length + 1));
      if (type === 'shape') out.push(new Group('Contents', 'ADBE Root Vectors Group', this, null, out.length + 1));
      out.push(masksGroup(this, out.length + 1));
      out.push(effectsGroup(this, out.length + 1));
      out.push(transformGroup(this, out.length + 1));
      return out;
    },
    get numProperties() { return this.__groups().length; },
    property: function (which) {
      var list = this.__groups();
      if (typeof which === 'number') return list[which - 1] || null;
      var w = String(which);
      for (var i = 0; i < list.length; i++) if (list[i].name === w || list[i].matchName === w) return list[i];
      // As After Effects takes them: "Position", "Source Text" straight from the layer.
      for (var g = 0; g < list.length; g++) {
        if (!list[g].__children || list[g].matchName === 'ADBE Effect Parade') continue;
        var found = list[g].property(w);
        if (found) return found;
      }
      return null;
    },
    get transform() { return this.property('ADBE Transform Group'); },
    get marker() { return this.property('ADBE Marker'); },
    get Effects() { return this.property('ADBE Effect Parade'); },
    get effect() { return this.property('ADBE Effect Parade'); },
    get mask() { return this.property('ADBE Mask Parade'); },
    get Masks() { return this.property('ADBE Mask Parade'); },
    get timeRemap() { return null; },
    remove: function () { this.__w.remove(); },
    duplicate: function () { return layerObject(this.__w.duplicate().id); },
    moveToBeginning: function () { this.__w.moveTo(1); },
    moveToEnd: function () { this.__w.moveTo(1e9); },
    moveBefore: function (other) {
      var me = this.index, target = other.index;
      this.__w.moveTo(me < target ? target - 1 : target);
    },
    moveAfter: function (other) {
      var me = this.index, target = other.index;
      this.__w.moveTo(me < target ? target : target + 1);
    },
    setParentWithJump: function (parent) { N.layerPatch(this.__id, { parentId: parent ? parent.__id : '' }); },
    // Its box in its own space: {top, left, width, height}.
    sourceRectAtTime: function (t) {
      var b = this.__w.bounds(t === undefined ? NaN : toFrame(t));
      return { top: b.top, left: b.left, width: b.width, height: b.height };
    },
    sourcePointToComp: function (p) { return this.__w.toComposition(p); },
    applyPreset: function () { unavailable('Applying an After Effects preset (.ffx)'); },
    replaceSource: function () { unavailable('Replacing a layer\'s source'); },
    openInViewer: function () { return null; },
    copyToComp: function () { unavailable('Copying a layer to another composition'); },
    toString: function () { return '[object Layer]'; }
  };
  // Transform's properties right on the layer too: layer.position ...
  TRANSFORM.forEach(function (spec) {
    Object.defineProperty(AELayer.prototype, spec.attr, {
      get: function () { return this.transform.property(spec.name); }
    });
  });
  function transformGroup(layer, index) {
    var g = new Group('Transform', 'ADBE Transform Group', layer, null, index);
    g.__children = function () {
      var threeD = layer.__w.threeD;
      return TRANSFORM.filter(function (s) {
        return threeD || (s.attr !== 'zPosition' && s.attr !== 'xRotation' && s.attr !== 'yRotation');
      }).map(function (s, i) {
        var spec = s;
        if (spec.keys.length === 3 && !threeD) {
          spec = Object.create(s);
          spec.keys = s.keys.slice(0, 2);
          spec.pad = [0, 0, s.attr === 'scale' ? 100 : 0];
        }
        return new Prop(layer, spec, g, i + 1);
      });
    };
    // layer.transform.position, .opacity ...
    TRANSFORM.forEach(function (spec) {
      Object.defineProperty(g, spec.attr, { get: function () { return g.property(spec.name); } });
    });
    return g;
  }
  function textGroup(layer, index) {
    var g = new Group('Text', 'ADBE Text Properties', layer, null, index);
    var animators = new Group('Animators', 'ADBE Text Animators', g, null, 2);
    animators.addProperty = function () {
      unavailable('Text animators built through the After Effects API (Wavy-Flo\'s own Text Animation can be set with wavy: layer.textAnimation)');
    };
    animators.canAddProperty = function () { return false; };
    g.__children = function () {
      return [new SourceText(layer, g), animators, new Group('Path Options', 'ADBE Text Path Options', g, null, 3),
              new Group('More Options', 'ADBE Text More Options', g, null, 4)];
    };
    Object.defineProperty(g, 'sourceText', { get: function () { return g.property(1); } });
    return g;
  }
  function masksGroup(layer, index) {
    var g = new Group('Masks', 'ADBE Mask Parade', layer, null, index);
    g.propertyType = PropertyType.INDEXED_GROUP;
    g.__children = function () {
      var masks = layer.__w.masks || [];
      return masks.map(function (m, i) { return new Group(m.name || ('Mask ' + (i + 1)), 'ADBE Mask Atom', g, null, i + 1); });
    };
    g.addProperty = function () { unavailable('Drawing a mask through the After Effects API'); };
    return g;
  }
  function effectsGroup(layer, index) {
    var g = new Group('Effects', 'ADBE Effect Parade', layer, null, index);
    g.propertyType = PropertyType.INDEXED_GROUP;
    g.__children = function () {
      return layer.__w.effects.map(function (e, i) { return new AEEffect(layer, e.name, g, i + 1); });
    };
    g.property = function (which) {
      var list = this.__children();
      if (typeof which === 'number') return list[which - 1] || null;
      var w = String(which), mapped = EFFECTS[w];
      for (var i = 0; i < list.length; i++)
        if (list[i].name === w || list[i].matchName === w || list[i].name === mapped) return list[i];
      return null;
    };
    g.canAddProperty = function (name) { return !!resolveEffect(name); };
    g.addProperty = function (name) {
      var effect = resolveEffect(name);
      if (!effect) unavailable('The effect "' + name + '"');
      layer.__w.addEffect(effect);
      return this.property(effect);
    };
    return g;
  }
  function resolveEffect(name) {
    var w = String(name), names = N.effectNames();
    if (EFFECTS[w]) return EFFECTS[w];
    for (var i = 0; i < names.length; i++) if (names[i] === w || names[i].toLowerCase() === w.toLowerCase()) return names[i];
    return null;
  }
  function TextLayer(id) { AELayer.call(this, id); }
  function AVLayer(id) { AELayer.call(this, id); }
  AVLayer.prototype = Object.create(AELayer.prototype);
  function ShapeLayer(id) { AELayer.call(this, id); }
  ShapeLayer.prototype = Object.create(AVLayer.prototype);
  function CameraLayer(id) { AELayer.call(this, id); }
  CameraLayer.prototype = Object.create(AELayer.prototype);
  function LightLayer(id) { AELayer.call(this, id); }
  LightLayer.prototype = Object.create(AELayer.prototype);
  TextLayer.prototype = Object.create(AVLayer.prototype);
  Object.defineProperty(TextLayer.prototype, 'text', { get: function () { return this.property('ADBE Text Properties'); } });
  function layerObject(id) {
    switch (N.layerGet(id, 'type')) {
      case 'text': return new TextLayer(id);
      case 'shape': return new ShapeLayer(id);
      case 'camera': return new CameraLayer(id);
      case 'light': return new LightLayer(id);
      default: return new AVLayer(id);
    }
  }
  global.Layer = AELayer;
  global.AVLayer = AVLayer;
  global.TextLayer = TextLayer;
  global.ShapeLayer = ShapeLayer;
  global.CameraLayer = CameraLayer;
  global.LightLayer = LightLayer;
  global.Property = Prop;
  global.PropertyGroup = Group;
  global.PropertyBase = Base;
  global.MaskPropertyGroup = Group;

  // --- Collections, numbered from 1 and indexed with [] ------------------------
  function collection(items, methods) {
    var target = methods || {};
    return new Proxy(target, {
      get: function (t, key) {
        if (typeof key === 'string' && /^[0-9]+$/.test(key)) return items()[Number(key) - 1];
        if (key === 'length') return items().length;
        return t[key];
      }
    });
  }

  // --- Compositions: the Main and each group -----------------------------------
  function CompItem(id) {
    this.__c = new wavy.Composition(id);
    this.__id = id || '';
  }
  CompItem.prototype = {
    typeName: 'Composition',
    get id() { if (!this.__id) return 1; var h = 7; for (var i = 0; i < this.__id.length; i++) h = (h * 31 + this.__id.charCodeAt(i)) | 0; return Math.abs(h) + 2; },
    get name() { return this.__c.name; },
    set name(v) { this.__c.name = v; },
    get width() { return this.__c.width; },
    set width(v) { if (!this.__id) N.setProject('width', v); },
    get height() { return this.__c.height; },
    set height(v) { if (!this.__id) N.setProject('height', v); },
    pixelAspect: 1,
    get frameRate() { return fps(); },
    set frameRate(v) { if (Number(v) !== fps()) unavailable('Changing the frame rate from a script'); },
    get frameDuration() { return 1 / fps(); },
    get duration() { return toSeconds(this.__c.frames); },
    set duration(t) { this.__c.frames = Math.max(1, Math.round(toFrame(t))); },
    get time() { return toSeconds(N.project().frame); },
    set time(t) { N.setProject('frame', toFrame(t)); },
    displayStartTime: 0,
    get workAreaStart() { var r = wavy.project.playRange; return r ? toSeconds(r[0]) : 0; },
    set workAreaStart(t) { var r = wavy.project.playRange || [0, this.__c.frames - 1]; wavy.project.playRange = [toFrame(t), r[1]]; },
    get workAreaDuration() { var r = wavy.project.playRange; return r ? toSeconds(r[1] - r[0]) : this.duration; },
    set workAreaDuration(d) { var r = wavy.project.playRange || [0, 0]; wavy.project.playRange = [r[0], r[0] + toFrame(d)]; },
    get bgColor() { var c = N.project().background; return [c[0], c[1], c[2]]; },
    set bgColor(c) { N.setProject('background', [c[0], c[1], c[2], 1]); },
    get motionBlur() { return N.project().motionBlur; },
    set motionBlur(on) { N.setProject('motionBlur', !!on); },
    shutterAngle: 180,
    shutterPhase: -90,
    resolutionFactor: [1, 1],
    renderer: 'ADBE Advanced 3d',
    renderers: ['ADBE Advanced 3d'],
    hideShyLayers: false,
    draft3d: false,
    frameBlending: false,
    preserveNestedFrameRate: false,
    preserveNestedResolution: false,
    dropFrame: false,
    comment: '',
    label: 0,
    selected: false,
    hasVideo: true,
    get hasAudio() { return this.__c.layers.some(function (l) { return l.type === 'audio' || l.type === 'video'; }); },
    footageMissing: false,
    usedIn: [],
    parentFolder: null,
    get numLayers() { return N.layerIds(this.__id).length; },
    get layers() { return layerCollection(this); },
    layer: function (which, relative) {
      var ids = N.layerIds(this.__id);
      if (relative !== undefined && which && which.__id) {
        var at = ids.indexOf(which.__id) + relative;
        return at >= 0 && at < ids.length ? layerObject(ids[at]) : null;
      }
      if (typeof which === 'number') {
        if (which < 1 || which > ids.length) throw new Error('There is no layer ' + which + ' (it has ' + ids.length + ')');
        return layerObject(ids[which - 1]);
      }
      for (var i = 0; i < ids.length; i++) if (N.layerGet(ids[i], 'name') === which) return layerObject(ids[i]);
      return null;
    },
    get selectedLayers() {
      var mine = N.layerIds(this.__id);
      return N.selectedLayers().filter(function (id) { return mine.indexOf(id) >= 0; }).map(layerObject);
    },
    get selectedProperties() { return []; },
    get markerProperty() { return new Markers(null); },
    openInViewer: function () { this.__c.open(); return null; },
    remove: function () {
      if (!this.__id) unavailable('Removing the Main');
      N.removeLayer(this.__id);
    },
    duplicate: function () { unavailable('Duplicating a composition'); },
    toString: function () { return '[object CompItem]'; }
  };
  function layerCollection(comp) {
    var id = comp.__id;
    function list() { return N.layerIds(id).map(layerObject); }
    function add(type, options) { return layerObject(N.addLayer(id, type, options || {})); }
    return collection(list, {
      addText: function (text) { return add('text', { text: text === undefined ? '' : String(text && text.text !== undefined ? text.text : text) }); },
      addBoxText: function (size, text) {
        var l = add('text', { text: text === undefined ? '' : String(text) });
        if (size) l.__w.paragraphWidth = size[0];
        return l;
      },
      // A solid: a fill of the whole frame, or a rectangle of its own size.
      addSolid: function (color, name, width, height) {
        var p = N.project(), c = [color[0], color[1], color[2], 1];
        if ((!width || width === p.width) && (!height || height === p.height)) return add('fill', { color: c, name: name });
        return add('shape', { shapeKind: 'rect', color: c, name: name, width: width, height: height });
      },
      addNull: function (duration) {
        var o = { name: 'Null ' + (N.layerIds(id).length + 1) };
        if (duration) o.duration = toFrame(duration);
        return add('empty', o);
      },
      addShape: function () { return add('shape', { shapeKind: 'path' }); },
      addCamera: function (name, center) {
        var l = add('camera', name ? { name: name } : {});
        if (center) l.__w.position.set([center[0], center[1]]);
        return l;
      },
      addLight: function (name, center) {
        var l = add('light', name ? { name: name } : {});
        if (center) l.__w.position.set([center[0], center[1]]);
        return l;
      },
      add: function (item) {
        if (item instanceof CompItem) unavailable('Placing a composition in another (a group is one already)');
        unavailable('Adding footage this way (import it with app.project.importFile)');
      },
      byName: function (name) { return comp.layer(name); },
      // These layers moved into a new group: a precomposition.
      precompose: function (indices, name) {
        var gid = N.addLayer(id, 'group', { name: name || 'Pre-comp' });
        var ids = N.layerIds(id);
        indices.forEach(function (i) { N.layerPatch(ids[i - 1], { groupId: gid }); });
        return new CompItem(gid);
      }
    });
  }
  global.CompItem = CompItem;
  global.Item = CompItem;
  global.AVItem = CompItem;
  function FootageItem() {}
  function FolderItem() {}
  global.FootageItem = FootageItem;
  global.FolderItem = FolderItem;
  function ImportOptions(file) {
    if (!(this instanceof ImportOptions)) return new ImportOptions(file);
    this.file = file;
    this.importAs = ImportAsType.FOOTAGE;
    this.sequence = false;
    this.forceAlphabetical = false;
  }
  ImportOptions.prototype.canImportAs = function () { return true; };
  global.ImportOptions = ImportOptions;

  // --- app ------------------------------------------------------------------------
  var tasks = {};
  var project = {
    get file() { var f = N.project().file; return f ? new File(f) : null; },
    get activeItem() { return new CompItem(N.project().active); },
    set activeItem(c) { if (c && c.__id !== undefined) N.openComposition(c.__id); },
    get items() {
      return collection(function () { return N.compositions().map(function (c) { return new CompItem(c.id); }); }, {
        addComp: function (name, width, height, pixelAspect, duration) {
          var gid = N.addLayer('', 'group', { name: name || 'Comp', duration: duration ? toFrame(duration) : undefined });
          return new CompItem(gid);
        },
        addFolder: function (name) { var f = new FolderItem(); f.name = name; return f; }
      });
    },
    get numItems() { return N.compositions().length; },
    item: function (i) {
      var all = N.compositions();
      if (i < 1 || i > all.length) throw new Error('There is no item ' + i);
      return new CompItem(all[i - 1].id);
    },
    get selection() { return []; },
    get rootFolder() { var f = new FolderItem(); f.name = 'Root'; f.items = this.items; f.numItems = this.numItems; return f; },
    get bitsPerChannel() { return N.project().depth; },
    set bitsPerChannel(bits) { N.setProject('depth', bits); },
    get linearBlending() { return N.project().linearLight; },
    set linearBlending(on) { N.setProject('linearLight', !!on); },
    workingSpace: 'sRGB IEC61966-2.1',
    workingGamma: 2.4,
    expressionEngine: 'javascript-1.0',
    timeDisplayType: 0,
    framesCountType: 0,
    transparencyGridThumbnails: false,
    renderQueue: { items: [], numItems: 0, render: function () { unavailable('Rendering through app.project.renderQueue'); }, showWindow: function () {} },
    save: function (file) { wavy.project.save(file ? file.fsName || String(file) : ''); return true; },
    saveWithDialog: function () { return false; },
    close: function () { unavailable('Closing the project from a script'); },
    importFile: function (options) {
      var file = options && options.file ? options.file : options;
      N.importFiles([file.fsName || String(file)]);
      return null;
    },
    importFileWithDialog: function () {
      var picked = File.openDialog('Import', '', true);
      if (picked) N.importFiles(picked.map(function (f) { return f.fsName; }));
      return null;
    },
    autoFixExpressions: function () {},
    consolidateFootage: function () { return 0; },
    removeUnusedFootage: function () { return 0; },
    reduceProject: function () { return 0; },
    showWindow: function () {},
    layerByID: function () { return null; }
  };
  var app = {
    name: 'Wavy-Flo',
    get version() { return N.project().version; },
    get buildName() { return N.project().version; },
    buildNumber: 0,
    language: 0,
    isoLanguage: 'en_US',
    isRenderEngine: false,
    isWatchFolder: false,
    isUISuppressed: false,
    memoryInUse: 0,
    effects: [],
    fonts: { allFonts: [] },
    project: project,
    activeViewer: null,
    preferences: { getPrefAsLong: function () { return 0; }, getPrefAsString: function () { return ''; },
                   getPrefAsBool: function () { return false; }, getPrefAsFloat: function () { return 0; },
                   havePref: function () { return false; }, savePrefAsLong: function () {},
                   savePrefAsString: function () {}, savePrefAsBool: function () {}, savePrefAsFloat: function () {},
                   reload: function () {}, deletePref: function () {} },
    settings: {
      getSetting: function (section, key) { var v = N.settingGet(section, key); return v === undefined || v === null ? '' : String(v); },
      saveSetting: function (section, key, value) { N.settingSet(section, key, String(value)); },
      haveSetting: function (section, key) { return N.settingHave(section, key); }
    },
    // Undo: the whole run of a script is one step, whatever groups it opens.
    beginUndoGroup: function () {},
    endUndoGroup: function () {},
    beginSuppressDialogs: function () {},
    endSuppressDialogs: function () {},
    activate: function () {},
    purge: function () {},
    pauseWatchFolder: function () {},
    endWatchFolder: function () {},
    executeCommand: function () { return false; },
    findMenuCommandId: function () { return 0; },
    getenv: function (name) { return N.env(name) || ''; },
    setTimeout: function (fn, ms) { return wavy.timer.after(ms || 0, fn); },
    cancelTimeout: function (id) { wavy.timer.cancel(id); },
    scheduleTask: function (code, delay, repeat) {
      var run = function () { (0, eval)(code); };
      var id = repeat ? wavy.timer.every(delay, run) : wavy.timer.after(delay, run);
      tasks[id] = true;
      return id;
    },
    cancelTask: function (id) { delete tasks[id]; wavy.timer.cancel(id); },
    newProject: function () { unavailable('Starting a new project from a script'); },
    open: function () { unavailable('Opening a project from a script'); },
    quit: function () {},
    watchFolder: function () {},
    parseSwatchFile: function () { return null; },
    onError: null,
    exitCode: 0,
    exitAfterLaunchAndEval: false,
    saveProjectOnCrash: true
  };
  global.app = app;
})(this);
