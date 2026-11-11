// wavy.ui: windows and docked panels for scripts, built line by line the way
// Wavy-Flo lays out its own settings -- a name on the left, the control on the
// right -- without ScriptUI's bookkeeping:
//
//   wavy.ui.panel("Title Tools", function (ui) {
//     var size = ui.slider("Size", 10, 400, 120);
//     var fill = ui.color("Fill", [1, 0.8, 0.2, 1]);
//     ui.button("Make Title", function () {
//       wavy.project.active.addText("Hello", { fontSize: size.value, color: fill.value });
//     });
//   });
//
// Every control has .value (read and set), .enabled, .visible and calls the
// function given to it -- or its .onChange -- with its value when the person
// changes it. Built on the same widgets as ScriptUI (scriptui.js), so the two
// mix: ui.container is the ScriptUI group the lines go into.
(function (global) {
  'use strict';
  var N = global.__wf, wavy = global.wavy;

  function format(v, decimals) {
    var d = decimals === undefined ? 2 : decimals;
    return String(Math.round(v * Math.pow(10, d)) / Math.pow(10, d));
  }

  // A control: its value, and what happens when it changes.
  function Field(control, read, write, fn) {
    this.control = control;
    this._read = read;
    this._write = write;
    this.onChange = fn || null;
    this.onChanging = null;
  }
  Object.defineProperties(Field.prototype, {
    value: {
      get: function () { return this._read(); },
      set: function (v) { this._write(v); }
    },
    enabled: {
      get: function () { return this.control.enabled; },
      set: function (on) { this.control.enabled = on; if (this.line) this.line.enabled = on; }
    },
    // Shown: it, its line and every container it is in (a shut section hides them).
    visible: {
      get: function () {
        for (var c = this.line || this.control; c; c = c.parent) if (c.visible === false) return false;
        return this.control.visible;
      },
      set: function (on) { (this.line || this.control).visible = on; relayout(this.control); }
    }
  });
  Field.prototype.changed = function () { if (typeof this.onChange === 'function') this.onChange(this.value); };
  Field.prototype.changing = function () { if (typeof this.onChanging === 'function') this.onChanging(this.value); };

  function windowOf(control) {
    var c = control;
    while (c && c.parent) c = c.parent;
    return c;
  }
  function relayout(control) {
    var w = windowOf(control);
    if (w && w.layout) w.layout.layout(true);
  }

  // Lines into a container.
  function Builder(container) {
    this.container = container;
  }
  Builder.prototype = {
    // A name on the left (a fixed column, so the controls line up), then
    // whatever goes on the line.
    line: function (label) {
      var g = this.container.add('group');
      g.orientation = 'row';
      g.alignChildren = ['left', 'center'];
      g.alignment = ['fill', 'top'];
      g.spacing = 6;
      g.margins = 0;
      if (label !== undefined && label !== null) {
        var name = g.add('statictext', undefined, String(label));
        name.preferredSize = [96, -1];
      }
      return g;
    },
    label: function (text) {
      var t = this.container.add('statictext', undefined, String(text), { multiline: true });
      t.alignment = ['fill', 'top'];
      return new Field(t, function () { return t.text; }, function (v) { t.text = String(v); });
    },
    button: function (text, fn) {
      var b = this.container.add('button', undefined, String(text));
      b.alignment = ['fill', 'top'];
      var f = new Field(b, function () { return b.text; }, function (v) { b.text = String(v); }, fn);
      b.onClick = function () { if (typeof f.onChange === 'function') f.onChange(); };
      f.click = b.onClick;
      return f;
    },
    // A slider and the number beside it, typed or dragged.
    slider: function (label, min, max, value, fn, decimals) {
      var g = this.line(label);
      var s = g.add('slider', undefined, value === undefined ? min : value, min, max);
      s.alignment = ['fill', 'center'];
      // The value itself: what was typed or set, exactly; a drag, to `decimals` places.
      var exact = Math.max(min, Math.min(max, value === undefined ? min : Number(value)));
      var n = g.add('edittext', undefined, format(exact, decimals));
      n.characters = 6;
      function put(v) {
        exact = Math.max(min, Math.min(max, Number(v)));
        s.value = exact;
        n.text = format(exact, decimals);
      }
      var f = new Field(s, function () { return exact; }, put, fn);
      f.line = g;
      function dragged() {
        exact = Number(format(s.value, decimals));
        n.text = format(exact, decimals);
      }
      s.onChanging = function () { dragged(); f.changing(); };
      s.onChange = function () { dragged(); f.changed(); };
      n.onChange = function () {
        var v = parseFloat(n.text);
        if (isNaN(v)) { n.text = format(exact, decimals); return; }
        put(v);
        f.changed();
      };
      return f;
    },
    // A number, typed or stepped: options min, max, step, decimals.
    number: function (label, value, options, fn) {
      if (typeof options === 'function') { fn = options; options = {}; }
      var o = options || {};
      var g = this.line(label);
      var spin = g.add('wfnumber');
      N.uiSet(spin.__id, 'decimals', o.decimals === undefined ? 2 : o.decimals);
      N.uiSet(spin.__id, 'minvalue', o.min === undefined ? -1e9 : o.min);
      N.uiSet(spin.__id, 'maxvalue', o.max === undefined ? 1e9 : o.max);
      N.uiSet(spin.__id, 'step', o.step === undefined ? 1 : o.step);
      spin.value = value === undefined ? 0 : value;
      var f = new Field(spin, function () { return spin.value; }, function (v) { spin.value = Number(v); }, fn);
      f.line = g;
      spin.onChange = function () { f.changed(); };
      return f;
    },
    checkbox: function (label, value, fn) {
      var g = this.line(null);
      var box = g.add('checkbox', undefined, String(label));
      box.value = !!value;
      var f = new Field(box, function () { return box.value; }, function (v) { box.value = !!v; }, fn);
      f.line = g;
      box.onClick = function () { f.changed(); };
      return f;
    },
    // One of `items`: .value is its number, .text what it says.
    dropdown: function (label, items, index, fn) {
      var g = this.line(label);
      var list = g.add('dropdownlist', undefined, items);
      list.alignment = ['fill', 'center'];
      list.selection = index || 0;
      var f = new Field(list, function () { return list.selection ? list.selection.index : -1; },
                        function (v) { list.selection = v; }, fn);
      Object.defineProperty(f, 'text', { get: function () { return list.selection ? list.selection.text : ''; } });
      f.line = g;
      list.onChange = function () { f.changed(); };
      return f;
    },
    text: function (label, value, fn) {
      var g = this.line(label);
      var edit = g.add('edittext', undefined, value === undefined ? '' : String(value));
      edit.alignment = ['fill', 'center'];
      var f = new Field(edit, function () { return edit.text; }, function (v) { edit.text = String(v); }, fn);
      f.line = g;
      edit.onChange = function () { f.changed(); };
      edit.onChanging = function () { f.changing(); };
      return f;
    },
    // A colour, [r, g, b, a] from 0 to 1, chosen in Wavy-Flo's own picker.
    color: function (label, rgba, fn) {
      var g = this.line(label);
      var swatch = g.add('wfcolor');
      swatch.value = rgba || [1, 1, 1, 1];
      var f = new Field(swatch, function () { return swatch.value; }, function (v) { swatch.value = v; }, fn);
      f.line = g;
      swatch.onChange = function () { f.changed(); };
      swatch.onChanging = function () { f.changing(); };
      return f;
    },
    // How far along: 0 to 1.
    progress: function (value) {
      var bar = this.container.add('progressbar', undefined, 0, 1);
      bar.alignment = ['fill', 'top'];
      bar.value = value || 0;
      return new Field(bar, function () { return bar.value; }, function (v) { bar.value = Number(v); });
    },
    // A picture (wavy.image): .value is set to another to show it.
    picture: function (picture) {
      var label = this.container.add('wfpicture');
      var shown = null;
      var f = new Field(label, function () { return shown; },
                        function (p) { shown = p; if (p) N.uiSet(label.__id, 'picture', p.id); relayout(label); });
      if (picture) f.value = picture;
      return f;
    },
    list: function (items, fn) {
      var list = this.container.add('listbox', undefined, items || []);
      list.alignment = ['fill', 'fill'];
      list.preferredSize = [-1, 120];
      var f = new Field(list, function () { return list.selection ? (list.selection.length !== undefined ? list.selection.map(function (i) { return i.index; }) : list.selection.index) : -1; },
                        function (v) { list.selection = v; }, fn);
      f.setItems = function (next) {
        list.removeAll();
        next.forEach(function (t) { list.add('item', t); });
        relayout(list);
      };
      list.onChange = function () { f.changed(); };
      return f;
    },
    separator: function () {
      var line = this.container.add('wfline');
      line.alignment = ['fill', 'top'];
      return line;
    },
    // Lines side by side.
    row: function (build) {
      var g = this.container.add('group');
      g.orientation = 'row';
      g.alignChildren = ['fill', 'center'];
      g.alignment = ['fill', 'top'];
      g.spacing = 6;
      var inner = new Builder(g);
      if (build) build(inner);
      return inner;
    },
    // Lines under a heading that folds them away.
    section: function (title, build, open) {
      var shown = open !== false;
      var head = this.container.add('button', undefined, (shown ? '▾ ' : '▸ ') + title);
      head.alignment = ['fill', 'top'];
      var body = this.container.add('group');
      body.orientation = 'column';
      body.alignChildren = ['fill', 'top'];
      body.alignment = ['fill', 'top'];
      body.spacing = 4;
      body.margins = [10, 0, 0, 0];
      var inner = new Builder(body);
      if (build) build(inner);
      body.visible = shown;
      head.onClick = function () {
        shown = !shown;
        body.visible = shown;
        head.text = (shown ? '▾ ' : '▸ ') + title;
        relayout(body);
      };
      return inner;
    }
  };

  function dress(container) {
    container.orientation = 'column';
    container.alignChildren = ['fill', 'top'];
    container.spacing = 4;
    container.margins = 10;
  }
  var ui = {
    // Docked in the editor (Window menu, beside the other panels); stays
    // while the script lives. Returns the ScriptUI Panel.
    panel: function (title, build) {
      var p = global.__wfPanel(N.uiDock(String(title)), String(title));
      dress(p);
      if (build) build(new Builder(p));
      p.layout.layout(true);
      return p;
    },
    // A floating window that stays open beside the editor.
    window: function (title, build) {
      var w = new global.Window('palette', String(title), undefined, { resizeable: true });
      dress(w);
      if (build) build(new Builder(w));
      w.show();
      return w;
    },
    // A window that waits for OK or Cancel: true for OK.
    dialog: function (title, build, okText) {
      var w = new global.Window('dialog', String(title));
      dress(w);
      if (build) build(new Builder(w));
      var buttons = w.add('group');
      buttons.alignment = ['right', 'top'];
      buttons.add('button', undefined, okText || 'OK', { name: 'ok' });
      buttons.add('button', undefined, 'Cancel', { name: 'cancel' });
      return w.show() === 1;
    },
    Builder: Builder
  };
  wavy.ui = ui;
})(this);
