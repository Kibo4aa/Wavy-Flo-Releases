// Wavy-Flo's own scripting API: `wavy`.
//
// Nothing is held back. Every layer, every property and keyframe -- its
// easing, its handles, its motion path handles and timing keys, its
// expression -- every effect and its settings, text, text animation,
// particles, masks, shapes, markers, rendering, files, programs, timers and
// windows. Times are frames of the Main (wavy.project.fps to turn them into
// seconds); colours are [r, g, b, a] from 0 to 1.
//
// A layer can also be read and changed whole, as the project file keeps it:
// layer.data is that object, layer.patch({...}) merges a change into it --
// so anything Wavy-Flo can do to a layer, a script can do, including what it
// learns to do after this was written.
//
//   var title = wavy.project.active.addText("Hello", { fontSize: 120 });
//   title.position.setAt(0, [-200, 540]);
//   title.position.setAt(30, [960, 540]);
//   title.position.ease(30, "easeOut");
//   title.addEffect("Glow");
//   wavy.project.addMarker(30, "landed");
(function (global) {
  'use strict';
  var N = global.__wf;

  function text(v) {
    if (v === undefined) return 'undefined';
    if (v === null) return 'null';
    if (typeof v === 'object') {
      try { return JSON.stringify(v); } catch (e) { return String(v); }
    }
    return String(v);
  }

  // --- Property numbers ---------------------------------------------------
  var table = null;
  function properties() {
    if (table) return table;
    table = { byKey: {}, byName: {}, list: N.propertyNames() };
    for (var i = 0; i < table.list.length; i++) {
      table.byKey[table.list[i].key.toLowerCase()] = table.list[i].index;
      table.byName[table.list[i].name.toLowerCase()] = table.list[i].index;
    }
    return table;
  }
  var count = 0;
  function fixedCount() { return count || (count = properties().list.length); }
  // A property's number on a layer, from its number, its key ("positionX"),
  // its name ("Position X") or an extra curve's id ("Glow/radius").
  function propertyIndex(layerId, which) {
    if (typeof which === 'number') return which;
    var t = properties(), k = String(which).toLowerCase();
    if (t.byKey.hasOwnProperty(k)) return t.byKey[k];
    if (t.byName.hasOwnProperty(k)) return t.byName[k];
    var at = N.curve(layerId, String(which), false, 0);
    if (at >= 0) return at;
    throw new Error('The layer has no property "' + which + '"');
  }

  // --- A property -----------------------------------------------------------
  function Property(layer, index) {
    this.layer = layer;
    this.index = index;
  }
  Property.prototype = {
    get name() {
      var t = properties();
      return this.index < fixedCount() ? t.list[this.index].name : N.curveIds(this.layer.id)[this.index - fixedCount()];
    },
    get key() {
      var t = properties();
      return this.index < fixedCount() ? t.list[this.index].key : this.name;
    },
    // At the playhead, expression and all.
    get value() { return N.value(this.layer.id, this.index, NaN); },
    set value(v) { this.set(v); },
    valueAt: function (frame) { return N.value(this.layer.id, this.index, frame); },
    keyedAt: function (frame) { return N.keyedValue(this.layer.id, this.index, frame); },
    get animated() { return N.keys(this.layer.id, this.index).length > 0; },
    // Without keys, the value; with keys, a key at the playhead.
    set: function (v) {
      if (this.animated) N.setKey(this.layer.id, this.index, wavy.project.frame, Number(v));
      else N.setBase(this.layer.id, this.index, Number(v));
      return this;
    },
    setAt: function (frame, v) { N.setKey(this.layer.id, this.index, frame, Number(v)); return this; },
    // [{frame, value, ease, custom, absolute, curve, continuous, spatial,
    //   spatialIn, spatialOut, timing}], first to last.
    get keys() { return N.keys(this.layer.id, this.index); },
    key: function (i) { return this.keys[i]; },
    addKey: function (frame) { N.setKey(this.layer.id, this.index, frame, this.keyedAt(frame)); return this; },
    removeKey: function (frame) {
      N.removeKey(this.layer.id, this.index, typeof frame === 'object' ? frame.frame : frame);
      return this;
    },
    clearKeys: function () { N.clearKeys(this.layer.id, this.index); return this; },
    // The key at `frame` changed: see setKeyData in script_host.cpp.
    setKey: function (frame, data) { N.setKeyData(this.layer.id, this.index, frame, data || {}); return this; },
    // "linear", "hold", "easeIn", "easeOut", "easeInOut".
    ease: function (frame, type) { N.setKeyData(this.layer.id, this.index, frame, { ease: type }); return this; },
    // Keys `first` to `last` (by number from 0; all of them when left out)
    // played backwards in the same span, their easing turned round with them.
    reverseKeys: function (first, last) {
      return N.reverseKeys(this.layer.id, this.index, first === undefined ? 0 : first, last === undefined ? -1 : last);
    },
    // The move from the key at `frame` to the next: its handles as
    // [frame, value] points -- out of this key, into the next.
    handles: function (frame) {
      var h = N.handles(this.layer.id, this.index, frame);
      return h.length ? { out: [h[0], h[1]], 'in': [h[2], h[3]] } : null;
    },
    setHandles: function (frame, out, into) {
      N.setHandles(this.layer.id, this.index, frame, out[0], out[1], into[0], into[1]);
      return this;
    },
    get expression() { return N.expression(this.layer.id, this.index); },
    set expression(code) {
      var why = N.setExpression(this.layer.id, this.index, String(code || ''));
      this.expressionError = why;
    },
    expressionError: '',
    get expressionEnabled() { return N.expressionOn(this.layer.id, this.index); },
    set expressionEnabled(on) { N.setExpressionOn(this.layer.id, this.index, !!on); },
    toString: function () { return '[Property ' + this.name + ']'; }
  };

  // Two or three properties moved as one: Position, Scale, Anchor Point.
  function Vector(layer, indices, name) {
    this.layer = layer;
    this.parts = indices.map(function (i) { return new Property(layer, i); });
    this.name = name;
  }
  function frameSet(parts) {
    var seen = {}, out = [];
    parts.forEach(function (p) {
      p.keys.forEach(function (k) {
        var f = Math.round(k.frame * 1000) / 1000;
        if (!seen[f]) { seen[f] = true; out.push(k.frame); }
      });
    });
    return out.sort(function (a, b) { return a - b; });
  }
  Vector.prototype = {
    get value() { return this.parts.map(function (p) { return p.value; }); },
    set value(v) { this.set(v); },
    valueAt: function (frame) { return this.parts.map(function (p) { return p.valueAt(frame); }); },
    set: function (v) {
      for (var i = 0; i < this.parts.length && i < v.length; i++) this.parts[i].set(v[i]);
      return this;
    },
    setAt: function (frame, v) {
      for (var i = 0; i < this.parts.length && i < v.length; i++) this.parts[i].setAt(frame, v[i]);
      return this;
    },
    get animated() { return this.parts.some(function (p) { return p.animated; }); },
    // The frames any of them has a key at.
    get keyFrames() { return frameSet(this.parts); },
    removeKey: function (frame) { this.parts.forEach(function (p) { p.removeKey(frame); }); return this; },
    clearKeys: function () { this.parts.forEach(function (p) { p.clearKeys(); }); return this; },
    ease: function (frame, type) {
      this.parts.forEach(function (p) {
        if (p.keys.some(function (k) { return Math.abs(k.frame - frame) < 1e-3; })) p.ease(frame, type);
      });
      return this;
    },
    setKey: function (frame, data) {
      this.parts.forEach(function (p) {
        if (p.keys.some(function (k) { return Math.abs(k.frame - frame) < 1e-3; })) p.setKey(frame, data);
      });
      return this;
    },
    get expression() { return this.parts.map(function (p) { return p.expression; }); },
    toString: function () { return '[Property ' + this.name + ']'; }
  };

  // --- An effect on a layer -------------------------------------------------
  function Effect(layer, name) {
    this.layer = layer;
    this.name = name;
  }
  Effect.prototype = {
    get enabled() {
      var list = N.effects(this.layer.id);
      for (var i = 0; i < list.length; i++) if (list[i].name === this.name) return list[i].enabled;
      return false;
    },
    set enabled(on) { N.setEffectEnabled(this.layer.id, this.name, !!on); },
    // What it is set to, by setting id, as the layer keeps it.
    get settings() { return N.effectSettings(this.layer.id, this.name); },
    get: function (setting) { return this.settings[setting]; },
    set: function (setting, value) { N.setEffectSetting(this.layer.id, this.name, setting, value); return this; },
    // A setting that changes over time: its curve on the layer, made if need be.
    curve: function (setting, initial) {
      var at = N.curve(this.layer.id, this.name + '/' + setting, true,
                       initial === undefined ? Number(this.get(setting) || 0) : initial);
      return new Property(this.layer, at);
    },
    remove: function () { N.removeEffect(this.layer.id, this.name); },
    toString: function () { return '[Effect ' + this.name + ']'; }
  };

  // --- A layer ----------------------------------------------------------------
  function Layer(id) { this.id = id; }
  function field(name, convert) {
    Object.defineProperty(Layer.prototype, name, {
      get: function () { return N.layerGet(this.id, name); },
      set: function (v) { N.layerSet(this.id, name, convert ? convert(v) : v); },
      enumerable: true
    });
  }
  ['name', 'visible', 'isolated', 'locked', 'threeD', 'blendMode', 'motionBlur', 'color', 'labelColor',
   'trackMatteInverted', 'audioEnabled', 'text', 'font', 'fontStyle', 'fontSize', 'tracking', 'leading',
   'align', 'fauxBold', 'fauxItalic', 'bold', 'capitals', 'paragraphWidth', 'strokeOnly', 'strokeWidth',
   'strokeColor', 'strokes', 'blur', 'glow', 'shadow', 'separatePosition', 'separateScale', 'lockScale',
   'timeRemap', 'start', 'duration', 'path'].forEach(function (f) { field(f); });
  Object.defineProperties(Layer.prototype, {
    type: { get: function () { return N.layerGet(this.id, 'type'); } },
    kind: { get: function () { return N.layerGet(this.id, 'shapeKind'); } },
    index: { get: function () { return N.layerGet(this.id, 'index'); } },
    end: {
      get: function () { return this.start + this.duration; },
      set: function (f) { this.duration = Math.max(1, f - this.start); }
    },
    composition: { get: function () { return new Composition(N.layerGet(this.id, 'composition')); } },
    parent: {
      get: function () { var p = N.layerGet(this.id, 'parent'); return p ? new Layer(p) : null; },
      set: function (l) { N.layerSet(this.id, 'parent', l ? (l.id || String(l)) : ''); }
    },
    trackMatte: {
      get: function () { var m = N.layerGet(this.id, 'trackMatte'); return m ? new Layer(m) : null; },
      set: function (l) { N.layerSet(this.id, 'trackMatte', l ? (l.id || String(l)) : ''); }
    },
    // Everything the layer is, as the project file has it.
    data: { get: function () { return N.layerData(this.id); } },
    // Text animation, particles, masks, a shape's path points, effects'
    // settings: each as the project file has it, read and set whole.
    textAnimation: {
      get: function () { return this.data.characterAnimation; },
      set: function (v) { this.patch({ characterAnimation: v }); }
    },
    particles: {
      get: function () { return this.data.particles; },
      set: function (v) { this.patch({ particles: v }); }
    },
    masks: {
      get: function () { return this.data.masks; },
      set: function (v) { N.layerPatch(this.id, { masks: v }); }
    },
    shapePoints: {
      get: function () { return this.data.shapePoints; },
      set: function (v) { N.layerPatch(this.id, { shapePoints: v }); }
    },
    // Read as a property (.value, .setAt, .keys ...); "layer.position = [x, y]"
    // sets it as .set does.
    position: {
      get: function () { return new Vector(this, this.threeD ? ['positionX', 'positionY', 'positionZ'].map(this._p, this) : [0, 1], 'Position'); },
      set: function (v) { this.position.set(v); }
    },
    scale: {
      get: function () { return new Vector(this, this.threeD ? [2, 3, this._p('scaleZ')] : [2, 3], 'Scale'); },
      set: function (v) { this.scale.set(typeof v === 'number' ? [v, v, v] : v); }
    },
    anchor: {
      get: function () { return new Vector(this, this.threeD ? ['anchorX', 'anchorY', 'anchorZ'].map(this._p, this) : ['anchorX', 'anchorY'].map(this._p, this), 'Anchor Point'); },
      set: function (v) { this.anchor.set(v); }
    },
    rotation: {
      get: function () { return new Property(this, 4); },
      set: function (v) { this.rotation.set(v); }
    },
    opacity: {
      get: function () { return new Property(this, 5); },
      set: function (v) { this.opacity.set(v); }
    },
    // Its effects in their order.
    effects: {
      get: function () {
        var self = this;
        return N.effects(this.id).map(function (e) { return new Effect(self, e.name); });
      }
    },
    selected: {
      get: function () { return N.selectedLayers().indexOf(this.id) >= 0; },
      set: function (on) {
        var ids = N.selectedLayers().filter(function (i) { return i !== this.id; }, this);
        if (on) ids.unshift(this.id);
        N.selectLayers(ids);
      }
    }
  });
  Layer.prototype._p = function (key) { return propertyIndex(this.id, key); };
  Layer.prototype.prop = function (which) { return new Property(this, propertyIndex(this.id, which)); };
  // Every property it has: the fixed ones, then its extra curves.
  Layer.prototype.props = function () {
    var names = properties().list.map(function (p) { return p.key; });
    return names.concat(N.curveIds(this.id));
  };
  Layer.prototype.patch = function (change) { N.layerPatch(this.id, change); return this; };
  Layer.prototype.get = function (f) { return N.layerGet(this.id, f); };
  Layer.prototype.set = function (f, v) { N.layerSet(this.id, f, v); return this; };
  Layer.prototype.effect = function (name) {
    var list = N.effects(this.id);
    for (var i = 0; i < list.length; i++) if (list[i].name === name) return new Effect(this, name);
    return null;
  };
  // An effect by its name (wavy.project.effectNames, or as the Effects panel
  // shows it), with settings to start at.
  Layer.prototype.addEffect = function (name, settings) {
    // An Expression Control by its effect name ("Slider Control"): a control.
    var control = /^(slider|angle|checkbox|point|color|layer) control$/i.exec(String(name));
    if (control) return this.addControl(control[1].toLowerCase(), String(name));
    var before = N.effects(this.id).map(function (e) { return e.name; });
    if (!N.applyEffect(this.id, name)) throw new Error('There is no effect "' + name + '" (wavy.project.effectNames lists them)');
    var made = name;
    N.effects(this.id).forEach(function (e) { if (before.indexOf(e.name) < 0) made = e.name; });
    var e = new Effect(this, made);
    if (settings) for (var k in settings) if (settings.hasOwnProperty(k)) e.set(k, settings[k]);
    return e;
  };
  Layer.prototype.removeEffect = function (name) { N.removeEffect(this.id, name); return this; };
  Layer.prototype.duplicate = function () { return new Layer(N.duplicateLayer(this.id)); };
  Layer.prototype.remove = function () { N.removeLayer(this.id); };
  Layer.prototype.moveTo = function (index) { N.moveLayer(this.id, index); return this; };
  Layer.prototype.moveToTop = function () { return this.moveTo(1); };
  Layer.prototype.moveToBottom = function () { return this.moveTo(1e9); };
  // Its picture's box in its own space: {left, top, width, height}.
  Layer.prototype.bounds = function (frame) {
    var b = N.layerBounds(this.id, frame === undefined ? NaN : frame);
    return { left: b[0], top: b[1], width: b[2], height: b[3] };
  };
  Layer.prototype.toComposition = function (point, frame) {
    return N.toComposition(this.id, point[0], point[1], frame === undefined ? NaN : frame);
  };
  Layer.prototype.select = function () { N.selectLayers([this.id]); return this; };
  Layer.prototype.toString = function () { return '[Layer ' + this.name + ']'; };
  Layer.prototype.equals = function (other) { return other && other.id === this.id; };

  // --- Expression Controls ----------------------------------------------------
  // Named values a layer carries for expressions to read --
  // effect("Speed")("Slider") -- any number of each kind: "slider", "angle",
  // "checkbox", "point", "color", "layer". They key and show in the timeline
  // like any property.
  function Control(layer, uid) {
    this.layer = layer;
    this.uid = uid;
  }
  function controlInfo(c) {
    var all = N.controls(c.layer.id);
    for (var i = 0; i < all.length; i++) if (all[i].uid === c.uid) return all[i];
    throw new Error('The control is gone from ' + c.layer.name);
  }
  Control.prototype = {
    get name() { return controlInfo(this).name; },
    set name(v) { N.renameControl(this.layer.id, this.uid, String(v)); },
    get kind() { return controlInfo(this).kind; },
    // Its keyable value: a property (slider, angle, checkbox), or two or four
    // moved as one (point, color). A Layer control has none.
    get property() {
      var info = controlInfo(this);
      if (!info.curves.length) return null;
      return info.curves.length === 1 ? new Property(this.layer, info.curves[0]) : new Vector(this.layer, info.curves, info.name);
    },
    get value() {
      var info = controlInfo(this);
      if (info.kind === 'layer') return info.layer ? new Layer(info.layer) : null;
      return this.property.value;
    },
    set value(v) {
      if (this.kind === 'layer') { N.setControlLayer(this.layer.id, this.uid, v ? (v.id || String(v)) : ''); return; }
      this.property.set(typeof v === 'boolean' ? (v ? 1 : 0) : v);
    },
    setAt: function (frame, v) { this.property.setAt(frame, typeof v === 'boolean' ? (v ? 1 : 0) : v); return this; },
    get keys() { var p = this.property; return p ? (p.keys || p.keyFrames) : []; },
    // How an expression reads it: effect("Speed")("Slider").
    get reference() {
      var info = controlInfo(this);
      return 'effect("' + info.name + '")("' + info.valueName + '")';
    },
    remove: function () { N.removeControl(this.layer.id, this.uid); },
    toString: function () { return '[Control ' + this.name + ']'; }
  };
  Layer.prototype.addControl = function (kind, name, value) {
    var made = new Control(this, N.addControl(this.id, String(kind).toLowerCase(), name ? String(name) : ''));
    if (value !== undefined) made.value = value;
    return made;
  };
  Object.defineProperty(Layer.prototype, 'controls', {
    get: function () {
      var self = this;
      return N.controls(this.id).map(function (c) { return new Control(self, c.uid); });
    }
  });
  Layer.prototype.control = function (name) {
    var all = N.controls(this.id), n = String(name).toLowerCase();
    for (var i = 0; i < all.length; i++) if (all[i].name.toLowerCase() === n) return new Control(this, all[i].uid);
    return null;
  };

  // --- Motion tracking -------------------------------------------------------------
  // A point followed through a video layer's footage. Places are in the
  // layer's own pixels (the footage's, from its middle); frames are the Main's.
  function TrackPointRef(layer, name) {
    this.layer = layer;
    this.name = name;
  }
  function trackInfo(t) {
    var all = N.tracks(t.layer.id);
    for (var i = 0; i < all.length; i++) if (all[i].name === t.name) return all[i];
    throw new Error('The track point "' + t.name + '" is gone from ' + t.layer.name);
  }
  TrackPointRef.prototype = {
    get feature() { return trackInfo(this).feature; },
    set feature(v) { N.setTrackPoint(this.layer.id, this.name, { feature: v }); },
    get search() { return trackInfo(this).search; },
    set search(v) { N.setTrackPoint(this.layer.id, this.name, { search: v }); },
    rename: function (to) { N.setTrackPoint(this.layer.id, this.name, { name: String(to) }); this.name = String(to); return this; },
    // [{frame, x, y, confidence}], first to last.
    get samples() {
      return trackInfo(this).samples.map(function (s) { return { frame: s[0], x: s[1], y: s[2], confidence: s[3] }; });
    },
    // Where it is at a frame: between samples in a line, held outside them.
    at: function (frame) {
      var s = this.samples;
      if (!s.length) return null;
      if (frame <= s[0].frame) return [s[0].x, s[0].y];
      if (frame >= s[s.length - 1].frame) return [s[s.length - 1].x, s[s.length - 1].y];
      for (var i = 1; i < s.length; i++)
        if (s[i].frame >= frame) {
          var a = s[i - 1], b = s[i], t = (frame - a.frame) / Math.max(1e-9, b.frame - a.frame);
          return [a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t];
        }
      return null;
    },
    // Placed by hand at a frame.
    place: function (frame, point) { N.setTrackSample(this.layer.id, this.name, frame, point[0], point[1]); return this; },
    clear: function () { N.setTrackPoint(this.layer.id, this.name, { clear: true }); return this; },
    // Followed from `from` (the playhead) to `to` (the layer's end; before
    // `from` goes backwards), waited for: { frames, lostAt, lowest, stopped,
    // error }. Options: lostBelow (0..1, the match that counts as lost),
    // onProgress(done, total) -- false stops it.
    track: function (options) {
      var o = options || {};
      var from = o.from !== undefined ? o.from : project.frame;
      var to = o.to !== undefined ? o.to : (o.backwards ? this.layer.start : this.layer.end - 1);
      global.__wfProgress = typeof o.onProgress === 'function' ? o.onProgress : null;
      try { return N.runTracker(this.layer.id, this.name, from, to, o.lostBelow || 0); }
      finally { global.__wfProgress = null; }
    },
    // Position keys that carry `target` with the point, kept where it is now
    // relative to the point at `reference` (the playhead). How many keys.
    applyTo: function (target, reference) {
      return N.applyTrack(this.layer.id, this.name, target.id || String(target), reference === undefined ? project.frame : reference);
    },
    // The footage held still about the point, as it is at `reference`.
    stabilize: function (reference) {
      return N.stabilizeTrack(this.layer.id, this.name, reference === undefined ? project.frame : reference);
    },
    remove: function () { N.removeTrackPoint(this.layer.id, this.name); },
    toString: function () { return '[TrackPoint ' + this.name + ']'; }
  };
  // A point to track, placed at `at` ([x, y] in the layer's pixels from its
  // middle) at the playhead (or options.frame). Options: feature, search
  // (sides of the patch followed and the area it is looked for in, pixels).
  Layer.prototype.addTrackPoint = function (name, at, options) {
    var o = options || {};
    var made = N.addTrackPoint(this.id, name ? String(name) : '', at[0], at[1],
                               o.frame !== undefined ? o.frame : project.frame, o.feature || 40, o.search || 120);
    return new TrackPointRef(this, made);
  };
  Object.defineProperty(Layer.prototype, 'trackPoints', {
    get: function () {
      var self = this;
      return N.tracks(this.id).map(function (t) { return new TrackPointRef(self, t.name); });
    }
  });
  Layer.prototype.trackPoint = function (name) {
    var all = N.tracks(this.id);
    for (var i = 0; i < all.length; i++) if (all[i].name === name) return new TrackPointRef(this, name);
    return null;
  };

  // Pulse on Beats: keys that jump on the markers (the detected beats; with
  // handMarkers, the hand-placed ones too) and settle before the next.
  //   move: "scale" (pulse), "opacity" (flash), "bounce" (Position Y), "kick" (Rotation)
  //   strength: in that property's units (scale 0.15 = 15 % bigger on the hit)
  //   settle: frames a hit takes to die away; every: one marker in that many
  var pulseMoves = { scale: 0, opacity: 1, bounce: 2, kick: 3 };
  Layer.prototype.pulseOnBeats = function (options) {
    var o = {};
    options = options || {};
    for (var k in options) if (options.hasOwnProperty(k)) o[k] = options[k];
    if (typeof o.move === 'string') {
      if (!pulseMoves.hasOwnProperty(o.move)) throw new Error('pulseOnBeats: move is one of scale, opacity, bounce, kick');
      o.move = pulseMoves[o.move];
    }
    return N.pulse(this.id, o);
  };

  // Transforms after the first: each with its own Position, Scale, Rotation
  // and Anchor Point, applied on top of the layer's own.
  function Transform(layer, id, name) {
    this.layer = layer;
    this.id = id;
    this.name = name;
  }
  Transform.prototype = {
    part: function (which) { return this.layer.prop('Xform/' + this.id + '/' + which); },
    get position() { return new Vector(this.layer, ['PositionX', 'PositionY', 'PositionZ'].map(this.part, this).map(function (p) { return p.index; }), 'Position'); },
    set position(v) { this.position.set(v); },
    get scale() { return new Vector(this.layer, ['ScaleX', 'ScaleY', 'ScaleZ'].map(this.part, this).map(function (p) { return p.index; }), 'Scale'); },
    set scale(v) { this.scale.set(typeof v === 'number' ? [v, v, v] : v); },
    get anchor() { return new Vector(this.layer, ['AnchorX', 'AnchorY', 'AnchorZ'].map(this.part, this).map(function (p) { return p.index; }), 'Anchor Point'); },
    set anchor(v) { this.anchor.set(v); },
    get rotation() { return this.part('Rotation'); },
    set rotation(v) { this.part('Rotation').set(v); },
    remove: function () { N.removeTransform(this.layer.id, this.id); },
    toString: function () { return '[Transform ' + this.name + ']'; }
  };
  Object.defineProperty(Layer.prototype, 'transforms', {
    get: function () {
      var self = this;
      return N.transforms(this.id).map(function (t) { return new Transform(self, t.id, t.name); });
    }
  });
  Layer.prototype.addTransform = function (name) {
    var id = N.addTransform(this.id, name || '');
    var list = this.transforms;
    for (var i = 0; i < list.length; i++) if (list[i].id === id) return list[i];
    return null;
  };

  // Masks: a path of points (wavy.path) cutting the layer, in its own pixels.
  //   mode: "Add", "Subtract", "Intersect", "None"; feather, expansion in
  //   pixels; opacity 0..100; inverted.
  Layer.prototype.addMask = function (points, options) {
    var o = options || {};
    var masks = (this.masks || []).slice();
    var mask = { name: o.name || ('Mask ' + (masks.length + 1)), points: points, mode: o.mode || 'Add',
                 feather: o.feather || 0, opacity: o.opacity === undefined ? 100 : o.opacity,
                 expansion: o.expansion || 0, inverted: !!o.inverted, enabled: o.enabled !== false };
    masks.push(mask);
    this.masks = masks;
    return masks.length - 1;
  };
  Layer.prototype.removeMask = function (index) {
    var masks = (this.masks || []).slice();
    masks.splice(index, 1);
    this.masks = masks;
  };
  Layer.prototype.setMask = function (index, change) {
    var masks = (this.masks || []).slice();
    for (var k in change) if (change.hasOwnProperty(k)) masks[index][k] = change[k];
    this.masks = masks;
  };

  // Presets: the layer's look -- effects, text, shape, transform and their
  // keys -- saved to a file, and put on any layer. Nothing is left out that the
  // project keeps; who the layer is (its id, name, place, parent) stays.
  var identity = { id: 1, name: 1, groupId: 1, parentId: 1, trackMatteId: 1, start: 1, duration: 1, path: 1, type: 1 };
  Layer.prototype.preset = function () {
    var data = this.data, out = {};
    for (var k in data) if (data.hasOwnProperty(k) && !identity[k]) out[k] = data[k];
    return out;
  };
  Layer.prototype.savePreset = function (file) {
    wavy.fs.writeJSON(file, { wavyFloPreset: 1, layer: this.preset() });
    return this;
  };
  Layer.prototype.applyPreset = function (presetOrFile) {
    var p = typeof presetOrFile === 'string' ? wavy.fs.readJSON(presetOrFile) : presetOrFile;
    var change = p && p.wavyFloPreset ? p.layer : p, clean = {};
    for (var k in change) if (change.hasOwnProperty(k) && !identity[k]) clean[k] = change[k];
    return this.patch(clean);
  };

  // --- A composition: the Main, or a group ----------------------------------
  function Composition(id) { this.id = id || ''; }
  Object.defineProperties(Composition.prototype, {
    isMain: { get: function () { return this.id === ''; } },
    name: {
      get: function () { return this.id ? N.layerGet(this.id, 'name') : N.project().mainName; },
      set: function (v) { if (this.id) N.layerSet(this.id, 'name', String(v)); }
    },
    width: { get: function () { return N.project().width; } },
    height: { get: function () { return N.project().height; } },
    fps: { get: function () { return N.project().fps; } },
    frames: {
      get: function () { return this.id ? N.layerGet(this.id, 'duration') : N.project().frames; },
      set: function (f) { if (this.id) N.layerSet(this.id, 'duration', f); else N.setProject('frames', f); }
    },
    frame: {
      get: function () { return N.project().frame; },
      set: function (f) { N.setProject('frame', f); }
    },
    // Its layers, top first.
    layers: {
      get: function () { return N.layerIds(this.id).map(function (i) { return new Layer(i); }); }
    },
    // The group layer it is, in the Main (null for the Main).
    groupLayer: { get: function () { return this.id ? new Layer(this.id) : null; } }
  });
  // By name, or by number from the top (1 the first).
  Composition.prototype.layer = function (which) {
    var ids = N.layerIds(this.id);
    if (typeof which === 'number') return which >= 1 && which <= ids.length ? new Layer(ids[which - 1]) : null;
    for (var i = 0; i < ids.length; i++) if (N.layerGet(ids[i], 'name') === which) return new Layer(ids[i]);
    return null;
  };
  // A new layer at the top: "text", "shape", "fill", "empty", "camera",
  // "light", "effect", "group", "particles". Options: name, color, width,
  // height, start, duration, shapeKind, text -- and any other layer field,
  // set after.
  Composition.prototype.add = function (type, options) {
    options = options || {};
    var made = new Layer(N.addLayer(this.id, type, options));
    var direct = { name: 1, color: 1, width: 1, height: 1, start: 1, duration: 1, shapeKind: 1, text: 1, path: 1 };
    for (var k in options)
      if (options.hasOwnProperty(k) && !direct[k]) made.set(k, options[k]);
    return made;
  };
  Composition.prototype.addText = function (words, options) {
    var o = options || {};
    o.text = words;
    return this.add('text', o);
  };
  Composition.prototype.addShape = function (kind, options) {
    var o = options || {};
    o.shapeKind = kind || 'rect';
    return this.add('shape', o);
  };
  Composition.prototype.addFill = function (color, options) {
    var o = options || {};
    if (color) o.color = color;
    return this.add('fill', o);
  };
  Composition.prototype.addEmpty = function (options) { return this.add('empty', options); };
  Composition.prototype.addCamera = function (options) { return this.add('camera', options); };
  Composition.prototype.addLight = function (options) { return this.add('light', options); };
  Composition.prototype.addGroup = function (name, options) {
    var o = options || {};
    if (name) o.name = name;
    return new Composition(this.add('group', o).id);
  };
  Composition.prototype.addEffectLayer = function (options) { return this.add('effect', options); };
  // These layers moved into a new group in this composition.
  Composition.prototype.group = function (layers, name) {
    var made = this.addGroup(name || 'Group');
    layers.forEach(function (l) { N.layerPatch(l.id, { groupId: made.id }); });
    return made;
  };
  Composition.prototype.open = function () { N.openComposition(this.id); return this; };
  // A frame of it written as a picture (PNG, JPG ...), over its background
  // unless options.transparent. Returns where it went.
  Composition.prototype.render = function (frame, path, width, options) {
    var where = N.renderFrame(this.id, frame === undefined ? NaN : frame, path, width || 0, !!(options && options.transparent));
    if (!where) throw new Error('The frame could not be written to ' + path);
    return where;
  };
  // A frame of it as pixels (wavy.image).
  Composition.prototype.renderImage = function (frame, width, options) { return image.render(frame, width, this, options); };
  // A movie of it, waited for: the file (or a PNG sequence's folder), or null
  // when onProgress stopped it. Options: format ("mp4", "png", "mov",
  // "movpng", "prores"), from, to (frames, both rendered), depth (8, 16, 32),
  // bitrate (Mb/s), audio, alpha, onProgress(done, total) -- false stops it.
  Composition.prototype.renderVideo = function (file, options) {
    var o = options || {}, settings = {};
    for (var k in o) if (o.hasOwnProperty(k) && k !== 'onProgress') settings[k] = o[k];
    global.__wfProgress = typeof o.onProgress === 'function' ? o.onProgress : null;
    var r;
    try { r = N.renderVideo(this.id, String(file), settings); } finally { global.__wfProgress = null; }
    if (r.ok) return r.file;
    if (r.error === 'Canceled') return null;
    throw new Error('The render failed: ' + r.error);
  };
  // In the Render Queue, to render in the background while work goes on.
  Composition.prototype.addToRenderQueue = function (file, options) { return renderQueue.add(this, file, options); };
  Composition.prototype.toString = function () { return '[Composition ' + this.name + ']'; };

  // --- The project --------------------------------------------------------------
  function setting(name) {
    return {
      get: function () { return N.project()[name]; },
      set: function (v) { N.setProject(name, v); },
      enumerable: true
    };
  }
  var project = {};
  Object.defineProperties(project, {
    width: setting('width'),
    height: setting('height'),
    fps: { get: function () { return N.project().fps; } },
    frames: setting('frames'),
    frame: setting('frame'),
    // Color depth: 8, 16 or 32 bits per channel (Project Settings).
    depth: setting('depth'),
    background: setting('background'),
    linearLight: setting('linearLight'),
    motionBlur: setting('motionBlur'),
    playRange: {
      get: function () {
        var p = N.project();
        return p.playStart >= 0 && p.playEnd > p.playStart ? [p.playStart, p.playEnd] : null;
      },
      set: function (r) {
        N.setProject('playStart', r ? r[0] : -1);
        N.setProject('playEnd', r ? r[1] : -1);
      }
    },
    file: { get: function () { return N.project().file; } },
    main: { get: function () { return new Composition(''); } },
    // The composition shown: the Main, or the group opened.
    active: { get: function () { return new Composition(N.project().active); } },
    compositions: {
      get: function () { return N.compositions().map(function (c) { return new Composition(c.id); }); }
    },
    // Every layer in the project, groups' too.
    allLayers: { get: function () { return N.layerIds('*').map(function (i) { return new Layer(i); }); } },
    markers: { get: function () { return N.markers(); } },
    selection: { get: function () { return N.selectedLayers().map(function (i) { return new Layer(i); }); } },
    effectNames: { get: function () { return N.effectNames(); } }
  });
  project.layer = function (id) { return new Layer(id); };
  project.composition = function (name) {
    var all = N.compositions();
    for (var i = 0; i < all.length; i++) if (all[i].name === name || all[i].id === name) return new Composition(all[i].id);
    return null;
  };
  project.select = function (layers) { N.selectLayers(layers.map(function (l) { return l.id || String(l); })); };
  project.addMarker = function (frame, label, beat) { N.addMarker(frame, label || '', !!beat); };
  project.removeMarker = function (index) { N.removeMarker(index); };
  project.setMarker = function (index, frame, label) { N.setMarker(index, frame, label); };
  project.save = function (path) {
    if (!N.saveProject(path || '')) throw new Error('The project could not be saved');
  };
  project.importFiles = function (paths) { N.importFiles([].concat(paths)); };
  project.undo = function () { N.undo(); };
  project.redo = function () { N.redo(); };
  // Line Up in Time: the layers start one after another in the order given --
  // the first at `from`, each next `step` frames later, or with onMarkers on
  // the next marker each. Their keys and footage move with them.
  project.lineUp = function (layers, options) {
    return N.lineUp(layers.map(function (l) { return l.id || String(l); }), options || {});
  };
  project.seconds = function (frames) { return frames / N.project().fps; };
  project.framesOf = function (seconds) { return seconds * N.project().fps; };

  // --- Files ----------------------------------------------------------------------
  var fs = {
    read: function (path, encoding) {
      var t = N.fsRead(path, encoding || '');
      if (t === null || t === undefined) throw new Error('Cannot read ' + path);
      return t;
    },
    write: function (path, contents, encoding) {
      if (!N.fsWrite(path, String(contents), false, encoding || '')) throw new Error('Cannot write ' + path);
    },
    append: function (path, contents, encoding) {
      if (!N.fsWrite(path, String(contents), true, encoding || '')) throw new Error('Cannot write ' + path);
    },
    readJSON: function (path) { return JSON.parse(fs.read(path)); },
    writeJSON: function (path, value) { fs.write(path, JSON.stringify(value, null, 2)); },
    exists: function (path) { return N.fsExists(path); },
    isFolder: function (path) { return N.fsIsFolder(path); },
    list: function (folder, mask) { return N.fsList(folder, mask || ''); },
    makeFolder: function (path) { return N.fsMakeFolder(path); },
    remove: function (path) { return N.fsRemove(path); },
    copy: function (from, to) { return N.fsCopy(from, to); },
    move: function (from, to) { return N.fsRename(from, to); },
    stat: function (path) { return N.fsStat(path); },
    native: function (path) { return N.fsNative(path); },
    // documents, desktop, temp, home, program, scripts, appData, userData
    folder: function (which) { return N.fsSpecial(which); },
    openFile: function (title, filter, multiple, start) {
      var r = N.fsOpenDialog(title || 'Open', filter || '', !!multiple, start || '');
      return multiple ? r : (r.length ? r[0] : null);
    },
    saveFile: function (title, filter, start) { return N.fsSaveDialog(title || 'Save', filter || '', start || '') || null; },
    chooseFolder: function (title, start) { return N.fsFolderDialog(title || 'Choose a folder', start || '') || null; },
    open: function (pathOrUrl) { return N.fsOpen(pathOrUrl); }
  };

  // --- Timers -------------------------------------------------------------------
  var callbacks = {};
  global.__wfTimer = function (id) {
    var c = callbacks[id];
    if (!c) return;
    if (!c.repeat) delete callbacks[id];
    c.fn();
  };
  var timer = {
    after: function (ms, fn) { var id = N.timer(ms, false); callbacks[id] = { fn: fn, repeat: false }; return id; },
    every: function (ms, fn) { var id = N.timer(ms, true); callbacks[id] = { fn: fn, repeat: true }; return id; },
    cancel: function (id) { delete callbacks[id]; N.cancelTimer(id); }
  };

  // --- The Render Queue ----------------------------------------------------------
  // Renders lined up and run on their own thread while work goes on; the
  // same list as File > Render Queue. wavy.on("renderQueue") hears it change
  // and finish.
  var renderQueue = {
    // Each {id, name, composition, output, target, enabled, state ("queued",
    // "rendering", "done", "failed", "stopped"), done, total, message, settings}.
    get items() { return N.renderQueueItems(); },
    add: function (composition, file, options) {
      var c = composition || project.active;
      return N.renderQueueAdd(c.id !== undefined ? c.id : String(c), String(file), options || {});
    },
    remove: function (id) { return N.renderQueueRemove(id && id.id ? id.id : String(id)); },
    clear: function () { N.renderQueueItems().forEach(function (i) { N.renderQueueRemove(i.id); }); },
    render: function () { N.renderQueueRun(true); },
    stop: function () { N.renderQueueRun(false); },
    get rendering() { return N.renderQueueBusy(); }
  };

  // --- Text Animation --------------------------------------------------------------
  // A text layer's animations, stacked: each moves, turns, scales, fades,
  // blurs, tints and wiggles its letters (or words, or lines) as its In and
  // Out go from 0 to 100, letter after letter in its order.
  var textOrders = { leftToRight: 0, rightToLeft: 1, topToBottom: 2, bottomToTop: 3, fromMiddle: 4, fromEdges: 5, random: 6 };
  var textUnits = { letters: 0, characters: 0, words: 1, lines: 2 };
  var textModes = { same: 0, random: 1, alternate: 2 };
  function hex(c) {
    function two(v) { var s = Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16); return s.length < 2 ? '0' + s : s; }
    return '#' + two(c[0]) + two(c[1]) + two(c[2]);
  }
  // A setting as the project keeps it, from a name a script can read.
  function textSetting(key, value) {
    if (key === 'order' && typeof value === 'string') {
      if (!textOrders.hasOwnProperty(value)) throw new Error('order is one of ' + Object.keys(textOrders).join(', '));
      return textOrders[value];
    }
    if (key === 'basedOn' && typeof value === 'string') {
      if (!textUnits.hasOwnProperty(value)) throw new Error('basedOn is one of letters, words, lines');
      return textUnits[value];
    }
    if (key === 'mode' && typeof value === 'string') {
      if (!textModes.hasOwnProperty(value)) throw new Error('mode is one of same, random, alternate');
      return textModes[value];
    }
    if (key === 'fillColor' && typeof value === 'object' && value) return hex(value);
    return value;
  }
  function TextAnimator(layer, uid) {
    this.layer = layer;
    this.uid = uid;
  }
  function animatorInfo(a) {
    var all = N.textAnimators(a.layer.id);
    for (var i = 0; i < all.length; i++) if (all[i].uid === a.uid) { all[i].index = i; return all[i]; }
    throw new Error('Text Animation ' + a.uid + ' is gone from ' + a.layer.name);
  }
  TextAnimator.prototype = {
    get name() { return animatorInfo(this).name; },
    set name(v) { N.setTextAnimatorSetting(this.layer.id, this.uid, 'name', String(v)); },
    get enabled() { return !animatorInfo(this).off; },
    set enabled(on) { N.setTextAnimatorSetting(this.layer.id, this.uid, 'off', !on); },
    // Its place in the stack, 0 the top; whether the panel shows it.
    get index() { return animatorInfo(this).index; },
    get edited() { return animatorInfo(this).edited; },
    get settings() { return animatorInfo(this).settings; },
    get summary() { return animatorInfo(this).summary; },
    get: function (key) { return this.settings[key]; },
    // One setting, or several: set({ y: 60, opacity: 0, order: 'random' }).
    set: function (key, value) {
      if (typeof key === 'object') {
        for (var k in key) if (key.hasOwnProperty(k)) this.set(k, key[k]);
        return this;
      }
      N.setTextAnimatorSetting(this.layer.id, this.uid, String(key), textSetting(key, value));
      return this;
    },
    // In: 0 the letters away (as the settings put them), 100 at rest. Out: 0
    // at rest, 100 gone. Asked for afresh each time: where they are kept moves
    // when another animation is opened in the panel.
    get 'in'() { return new Property(this.layer, N.textAnimatorCurve(this.layer.id, this.uid, 'In')); },
    get out() { return new Property(this.layer, N.textAnimatorCurve(this.layer.id, this.uid, 'Out')); },
    // The Range it works on, keyable: "start", "end", "offset" (percent of the text).
    range: function (which) {
      var names = { start: 'Range Start', end: 'Range End', offset: 'Range Offset' };
      if (!names[which]) throw new Error('range is "start", "end" or "offset"');
      return new Property(this.layer, N.textAnimatorCurve(this.layer.id, this.uid, names[which]));
    },
    // The letters come in from `from` to `to` (frames of the Main).
    animateIn: function (from, to, ease) {
      var p = this['in'];
      p.clearKeys();
      p.setAt(from, 0);
      p.setAt(to, 100);
      p.ease(from, ease || 'easeInOut');
      return this;
    },
    // ... and go out.
    animateOut: function (from, to, ease) {
      var p = this.out;
      p.clearKeys();
      p.setAt(from, 0);
      p.setAt(to, 100);
      p.ease(from, ease || 'easeInOut');
      return this;
    },
    // Open in the Text Animation panel.
    edit: function () { N.editTextAnimator(this.layer.id, this.uid); return this; },
    moveTo: function (index) { N.moveTextAnimator(this.layer.id, this.uid, index); return this; },
    moveUp: function () { return this.moveTo(Math.max(0, this.index - 1)); },
    moveDown: function () { return this.moveTo(this.index + 1); },
    duplicate: function () { return new TextAnimator(this.layer, N.duplicateTextAnimator(this.layer.id, this.uid)); },
    remove: function () { N.removeTextAnimator(this.layer.id, this.uid); },
    toString: function () { return '[TextAnimator ' + (this.name || this.uid) + ']'; }
  };
  // The ready-made presets and the person's own: [{name, settings}].
  function textPresets() { return N.textPresets(); }
  Object.defineProperty(Layer.prototype, 'textAnimators', {
    get: function () {
      var self = this;
      return N.textAnimators(this.id).map(function (a) { return new TextAnimator(self, a.uid); });
    }
  });
  // A Text Animation at the bottom of the stack: a preset's ("Fade Up",
  // "Pop", "Typewriter" ... wavy.textPresets), its settings, or both:
  //   title.addTextAnimator({ preset: 'Pop', delay: 3, from: 10, duration: 25 })
  //   title.addTextAnimator({ y: 120, opacity: 0, order: 'random', basedOn: 'words' })
  // from / duration key In (from the playhead, over 20 frames, for a preset).
  Layer.prototype.addTextAnimator = function (options) {
    var o = typeof options === 'string' ? { preset: options } : (options || {});
    var settings = {}, preset = null, k;
    if (o.preset) {
      var all = N.textPresets(), wanted = String(o.preset).toLowerCase();
      for (var i = 0; i < all.length; i++) if (all[i].name.toLowerCase() === wanted) preset = all[i];
      if (!preset) throw new Error('There is no Text Animation preset "' + o.preset + '" (wavy.textPresets lists them)');
      for (k in preset.settings) if (preset.settings.hasOwnProperty(k)) settings[k] = preset.settings[k];
      if (preset.name !== 'Blank') settings.name = preset.name;
    }
    var keysOnly = { preset: 1, from: 1, duration: 1, ease: 1 };
    for (k in o) if (o.hasOwnProperty(k) && !keysOnly[k]) settings[k] = textSetting(k, o[k]);
    // Smooth Strength is no setting any more: asked for (an old preset), it
    // eases the In and Out keys laid below where they run straight.
    var smooth = !!settings.smooth;
    delete settings.smooth;
    var made = new TextAnimator(this, N.addTextAnimator(this.id, settings));
    var from = o.from !== undefined ? o.from : (preset ? project.frame : undefined);
    if (preset && (preset['in'] || preset.out)) {
      // The person's own preset: its keys, laid from `from`, handles and all.
      var lay = function (keys, p) {
        (keys || []).forEach(function (key) {
          p.setAt(from + key[0], key[1]);
          p.ease(from + key[0], key[2] || 'linear');
          if (key.length >= 8)
            p.setKey(from + key[0], { custom: true, curve: [key[3], key[4], key[5], key[6]], absolute: !!key[7] });
        });
      };
      lay(preset['in'], made['in']);
      lay(preset.out, made.out);
    } else if (from !== undefined && !(preset && (preset.name === 'Blank' || preset.settings.wiggleSpeed))) {
      made.animateIn(from, from + (o.duration || 20), o.ease || 'easeInOut');
    }
    if (smooth) N.setTextAnimatorSetting(this.id, made.uid, 'smooth', true);
    return made;
  };
  // A frame as pixels, or any picture: read, change, save, put in as a layer.
  // Pixels are [r, g, b, a], 0..255, row after row.
  function Picture(id) {
    if (!id) throw new Error('No picture was made');
    this.id = id;
  }
  Picture.prototype = {
    get width() { return N.imageInfo(this.id).width; },
    get height() { return N.imageInfo(this.id).height; },
    pixel: function (x, y) { return N.imagePixels(this.id, x, y, 1, 1); },
    setPixel: function (x, y, rgba) {
      N.imageSetPixels(this.id, x, y, 1, 1, rgba.length === 3 ? rgba.concat([255]) : rgba);
      return this;
    },
    pixels: function (x, y, w, h) {
      return arguments.length ? N.imagePixels(this.id, x, y, w, h) : N.imagePixels(this.id, 0, 0, this.width, this.height);
    },
    setPixels: function (x, y, w, h, values) { N.imageSetPixels(this.id, x, y, w, h, values); return this; },
    average: function (x, y, w, h) {
      return arguments.length ? N.imageAverage(this.id, x, y, w, h) : N.imageAverage(this.id, 0, 0, this.width, this.height);
    },
    scaled: function (w, h, smooth) { return new Picture(N.imageScaled(this.id, w, h || Math.round(w * this.height / this.width), smooth !== false)); },
    crop: function (x, y, w, h) { return new Picture(N.imageCopy(this.id, x, y, w, h)); },
    save: function (file, quality) {
      var where = N.imageSave(this.id, String(file), quality === undefined ? -1 : quality);
      if (!where) throw new Error('The picture could not be written to ' + file);
      return where;
    },
    // Put in the project as a layer of the composition shown.
    toLayer: function (name) {
      var file = wavy.fs.folder('temp') + '/wavy-flo-' + (name || 'picture').replace(/[^A-Za-z0-9_-]+/g, '-') + '-' + Date.now() + '.png';
      this.save(file);
      var before = {};
      wavy.project.active.layers.forEach(function (l) { before[l.id] = true; });
      wavy.project.importFiles([file]);
      var made = wavy.project.active.layers.filter(function (l) { return !before[l.id]; })[0] || null;
      if (made && name) made.name = name;
      return made;
    },
    free: function () { N.imageFree(this.id); }
  };
  var image = {
    load: function (file) { return new Picture(N.imageLoad(String(file))); },
    create: function (width, height, color) { return new Picture(N.imageNew(width, height, color || [0, 0, 0, 0])); },
    // A frame of the composition shown (or `composition`), `width` wide if
    // given, over its background -- or, with { transparent: true }, without.
    render: function (frame, width, composition, options) {
      var c = composition || wavy.project.active;
      return new Picture(N.imageRender(c.id, frame === undefined ? NaN : frame, width || 0, !!(options && options.transparent)));
    },
    Picture: Picture
  };

  // --- The network -----------------------------------------------------------------
  // HTTP(S), waited for: { ok, status, headers, body, error, json() }.
  var net = {
    request: function (method, url, options) {
      var o = options || {};
      var r = N.http(String(method), String(url), o.headers || {}, o.body === undefined ? null : o.body,
                     o.timeout || 0, o.saveTo ? String(o.saveTo) : '');
      // status 0: not HTTP (a file:// address), read all the same.
      r.ok = !r.error && (r.status === 0 || (r.status >= 200 && r.status < 300));
      r.json = function () { return JSON.parse(r.body); };
      return r;
    },
    get: function (url, options) { return net.request('GET', url, options); },
    post: function (url, body, options) {
      var o = {};
      for (var k in (options || {})) if (options.hasOwnProperty(k)) o[k] = options[k];
      o.body = body;
      return net.request('POST', url, o);
    },
    getJSON: function (url, options) {
      var r = net.get(url, options);
      if (!r.ok) throw new Error('GET ' + url + ': ' + (r.error || r.status));
      return r.json();
    },
    download: function (url, file, options) {
      var o = {};
      for (var k in (options || {})) if (options.hasOwnProperty(k)) o[k] = options[k];
      o.saveTo = file;
      var r = net.request('GET', url, o);
      if (!r.saved) throw new Error('Download of ' + url + ' failed: ' + (r.error || r.status));
      return file;
    }
  };

  // --- Effects ---------------------------------------------------------------
  // Every effect there is -- Wavy-Flo's, the .wfx packs', plug-ins', OpenFX's --
  // with each of its parameters: id, label, kind, min, max, default, step,
  // choices, unit. layer.addEffect takes the name; effect.set the ids.
  var effects = {
    get list() { return N.effectInfo(); },
    get names() { return N.effectNames(); },
    describe: function (name) {
      var all = N.effectInfo(), n = String(name).toLowerCase();
      for (var i = 0; i < all.length; i++)
        if (all[i].name.toLowerCase() === n || all[i].label.toLowerCase() === n) return all[i];
      return null;
    },
    inCategory: function (category) {
      var c = String(category).toLowerCase();
      return N.effectInfo().filter(function (e) { return e.category.toLowerCase().indexOf(c) >= 0; });
    }
  };

  // --- Paths --------------------------------------------------------------------
  // Points for shapes (layer.shapePoints) and masks (layer.addMask): each
  // { p: [x, y], in: [dx, dy], out: [dx, dy] }, handles relative to the point.
  var kappa = 0.5522847498;
  function pt(x, y, inx, iny, outx, outy) {
    return { p: [x, y], 'in': [inx || 0, iny || 0], out: [outx || 0, outy || 0] };
  }
  var path = {
    point: pt,
    rect: function (x, y, w, h) { return [pt(x, y), pt(x + w, y), pt(x + w, y + h), pt(x, y + h)]; },
    ellipse: function (cx, cy, rx, ry) {
      if (ry === undefined) ry = rx;
      var kx = rx * kappa, ky = ry * kappa;
      return [pt(cx, cy - ry, -kx, 0, kx, 0), pt(cx + rx, cy, 0, -ky, 0, ky),
              pt(cx, cy + ry, kx, 0, -kx, 0), pt(cx - rx, cy, 0, ky, 0, -ky)];
    },
    polygon: function (points) { return points.map(function (q) { return pt(q[0], q[1]); }); },
    star: function (cx, cy, outer, inner, count) {
      var out = [], n = Math.max(2, count || 5);
      for (var i = 0; i < n * 2; i++) {
        var r = i % 2 ? inner : outer, a = -Math.PI / 2 + i * Math.PI / n;
        out.push(pt(cx + r * Math.cos(a), cy + r * Math.sin(a)));
      }
      return out;
    },
    // Through the points, smoothly (Catmull-Rom made into handles).
    smooth: function (points, closed) {
      var n = points.length, out = [];
      for (var i = 0; i < n; i++) {
        var a = points[closed ? (i - 1 + n) % n : Math.max(0, i - 1)], b = points[i];
        var c = points[closed ? (i + 1) % n : Math.min(n - 1, i + 1)];
        var tx = (c[0] - a[0]) / 6, ty = (c[1] - a[1]) / 6;
        out.push(pt(b[0], b[1], -tx, -ty, tx, ty));
      }
      return out;
    },
    // Several paths as one: each after the first starts apart from the last.
    join: function () {
      var out = [];
      for (var i = 0; i < arguments.length; i++)
        arguments[i].forEach(function (q, j) {
          var c = JSON.parse(JSON.stringify(q));
          if (i > 0 && j === 0) c['break'] = true;
          out.push(c);
        });
      return out;
    }
  };

  // --- Events ----------------------------------------------------------------
  // wavy.on("frame" | "selection" | "play" | "stop" | "change" | "save" |
  // "open" | "new", fn): told as it happens, for as long as the script lives
  // (its window open, or for good when it runs from Scripts/Startup).
  var listeners = {};
  function on(event, fn) {
    if (typeof fn !== 'function') throw new Error('wavy.on("' + event + '", fn): fn is not a function');
    (listeners[event] = listeners[event] || []).push(fn);
    N.listen(String(event), true);
    return fn;
  }
  function off(event, fn) {
    var list = listeners[event] || [];
    listeners[event] = fn ? list.filter(function (f) { return f !== fn; }) : [];
    if (!listeners[event].length) N.listen(String(event), false);
  }
  global.__wfEvent = function (event, data) {
    if (event === 'selection') data = data.map(function (i) { return new Layer(i); });
    (listeners[event] || []).slice().forEach(function (fn) { fn(data); });
  };

  // --- The editor ----------------------------------------------------------------
  var commandFns = {};
  global.__wfCommand = function (id) { var fn = commandFns[id]; if (fn) fn(); };
  var app = {
    // The playhead, in frames of the Main. Moving it is not an Undo step.
    get frame() { return N.project().frame; },
    set frame(f) { N.setProject('frame', f); },
    get playing() { return N.playing(); },
    set playing(on) { N.setPlaying(!!on); },
    play: function () { N.setPlaying(true); },
    stop: function () { N.setPlaying(false); },
    // Every menu item, as "File/Save Project": anything the menus do, a script can.
    get commands() { return N.commands(); },
    command: function (path) {
      if (!N.runCommand(String(path))) throw new Error('There is no command "' + path + '" (wavy.app.commands lists them)');
    },
    // A menu item of the script's own, with a shortcut if asked:
    //   wavy.app.addCommand("Scripts/Center Layer", fn, "Ctrl+Alt+C")
    addCommand: function (path, fn, shortcut) {
      var id = N.addCommand(String(path), shortcut ? String(shortcut) : '');
      if (!id) throw new Error('The command "' + path + '" could not be added');
      commandFns[id] = fn;
      return { id: id, remove: function () { delete commandFns[id]; N.removeCommand(id); } };
    },
    status: function (text, ms) { N.status(String(text), ms || 5000); },
    get version() { return N.project().version; }
  };
  // What `fn` does, as an Undo step of its own (a run is one step otherwise).
  function undoGroup(name, fn) {
    if (typeof name === 'function') { fn = name; }
    N.newStep();
    try { return fn(); } finally { N.newStep(); }
  }

  var wavy = {
    project: project,
    app: app,
    effects: effects,
    path: path,
    image: image,
    net: net,
    renderQueue: renderQueue,
    get textPresets() { return textPresets(); },
    TextAnimator: TextAnimator,
    Control: Control,
    TrackPoint: TrackPointRef,
    on: on,
    off: off,
    undoGroup: undoGroup,
    fs: fs,
    timer: timer,
    Layer: Layer,
    Composition: Composition,
    Property: Property,
    Effect: Effect,
    get version() { return N.project().version; },
    log: function () { N.log(Array.prototype.map.call(arguments, text).join(' ')); },
    alert: function (message, title) { N.alert(text(message), title || 'Wavy-Flo', false); },
    confirm: function (message, title) { return N.confirm(text(message), title || 'Wavy-Flo', false); },
    prompt: function (message, preset, title) {
      var r = N.prompt(text(message), preset === undefined ? '' : String(preset), title || 'Wavy-Flo');
      return r === undefined ? null : r;
    },
    settings: {
      get: function (section, key, fallback) {
        return N.settingHave(section, key) ? N.settingGet(section, key) : fallback;
      },
      set: function (section, key, value) { N.settingSet(section, key, value); },
      has: function (section, key) { return N.settingHave(section, key); }
    },
    system: {
      // A command line, waited for: {code, output, error}.
      run: function (command, timeoutMs) { return N.run(command, timeoutMs || 0); },
      env: function (name) { return N.env(name); },
      get os() { return N.os(); },
      get windows() { return N.windows(); },
      sleep: function (ms) { N.sleep(ms); },
      open: function (url) { return N.fsOpen(url); }
    },
    // The script being run; end() lets go of its timers, events and menu items.
    script: { file: '', folder: '', end: function () { listeners = {}; commandFns = {}; N.endScript(); } }
  };
  global.__wfBegin = function (file) {
    wavy.script.file = file;
    wavy.script.folder = file.replace(/[\\\/][^\\\/]*$/, '');
  };
  global.wavy = wavy;
})(this);
