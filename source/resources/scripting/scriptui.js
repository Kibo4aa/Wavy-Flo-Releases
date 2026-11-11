// ScriptUI, as After Effects scripts build their windows with it: Window,
// its controls (add("button") ...), resource strings ("group{orientation:
// 'row', ok: Button{text:'OK'}}"), events (onClick, onChange, onChanging,
// addEventListener) and the layout properties (orientation, alignChildren,
// alignment, spacing, margins, preferredSize). Each control is a Qt widget
// made by the bridge (__wf.ui*); its state lives in the widget, its handlers
// and anything else a script hangs on it in the object here. Drawing a
// control oneself (onDraw, graphics' paths) is taken and passed over: the
// control draws as it is.
(function (global) {
  'use strict';
  var N = global.__wf;
  var controls = {};

  var TYPES = {
    window: 'window', dialog: 'dialog', palette: 'palette', panel: 'panel', group: 'group',
    tabbedpanel: 'tabbedpanel', tab: 'tab', button: 'button', iconbutton: 'iconbutton',
    statictext: 'statictext', edittext: 'edittext', checkbox: 'checkbox', radiobutton: 'radiobutton',
    slider: 'slider', scrollbar: 'scrollbar', progressbar: 'progressbar', dropdownlist: 'dropdownlist',
    listbox: 'listbox', treeview: 'listbox', image: 'image', flashplayer: 'group', divider: 'panel',
    // wavy.ui's own (wavyui.js)
    wfcolor: 'wfcolor', wfnumber: 'wfnumber', wfline: 'wfline', wfpicture: 'wfpicture'
  };
  var CONTAINERS = { window: 1, dialog: 1, palette: 1, panel: 1, group: 1, tab: 1, tabbedpanel: 1 };

  // --- Resource strings ------------------------------------------------------
  // type{key: value, child: Type{...}, ...}: values are JavaScript literals.
  function parseResource(source) {
    var s = String(source), i = 0;
    function space() { while (i < s.length && /\s/.test(s[i])) i++; }
    function name() {
      space();
      var start = i;
      while (i < s.length && /[A-Za-z0-9_$]/.test(s[i])) i++;
      return s.substring(start, i);
    }
    function literal() {
      space();
      var c = s[i];
      if (c === '"' || c === "'") {
        var q = c, out = '';
        i++;
        while (i < s.length && s[i] !== q) {
          if (s[i] === '\\' && i + 1 < s.length) { i++; out += ({ n: '\n', t: '\t', r: '\r' })[s[i]] || s[i]; }
          else out += s[i];
          i++;
        }
        i++;
        return out;
      }
      if (c === '[') {
        i++;
        var list = [];
        space();
        while (i < s.length && s[i] !== ']') {
          list.push(value());
          space();
          if (s[i] === ',') i++;
          space();
        }
        i++;
        return list;
      }
      if (c === '{') return object();
      var start = i;
      while (i < s.length && /[A-Za-z0-9_$.\-+]/.test(s[i])) i++;
      var word = s.substring(start, i);
      if (word === 'true') return true;
      if (word === 'false') return false;
      if (word === 'undefined' || word === 'null') return undefined;
      if (!isNaN(Number(word))) return Number(word);
      // ScriptUI.Alignment.LEFT and the like, read as their names.
      var dot = word.lastIndexOf('.');
      return (dot >= 0 ? word.substring(dot + 1) : word).toLowerCase();
    }
    function object() {
      var out = {};
      i++; // {
      space();
      while (i < s.length && s[i] !== '}') {
        var key = name();
        space();
        if (s[i] === ':') i++;
        out[key] = value();
        space();
        if (s[i] === ',') i++;
        space();
      }
      i++;
      return out;
    }
    // A value: a control of its own (Type{...}), or a literal.
    function value() {
      space();
      var save = i, word = name();
      space();
      if (word && s[i] === '{' && TYPES[word.toLowerCase()]) {
        var spec = object();
        return { __type: word.toLowerCase(), spec: spec };
      }
      i = save;
      return literal();
    }
    var top = name();
    space();
    var spec = s[i] === '{' ? object() : {};
    return { type: top.toLowerCase(), spec: spec };
  }

  // --- Controls ----------------------------------------------------------------
  function Control() {}
  function bind(object, id, type, parent) {
    object.__id = id;
    object.type = type;
    object.parent = parent || null;
    object.children = [];
    object.properties = object.properties || {};
    object.__listeners = {};
    object.__items = [];
    object.__align = null;
    object.__alignChildren = null;
    object.__orientation = null;
    object.__spacing = undefined;
    object.__margins = undefined;
    object.__preferred = null;
    object.graphics = new Graphics(object);
    object.layout = new Layout(object);
    controls[id] = object;
    if (parent) parent.children.push(object);
    return object;
  }
  function synced(name) {
    return {
      get: function () { return N.uiGet(this.__id, name); },
      set: function (v) { N.uiSet(this.__id, name, v); },
      enumerable: true
    };
  }
  Object.defineProperties(Control.prototype, {
    text: synced('text'),
    enabled: synced('enabled'),
    minvalue: synced('minvalue'),
    maxvalue: synced('maxvalue'),
    helpTip: {
      get: function () { return this.__tip || ''; },
      set: function (v) { this.__tip = v; N.uiSet(this.__id, 'helpTip', v); }
    },
    visible: {
      get: function () { return this.__visible !== false; },
      set: function (v) { this.__visible = !!v; N.uiSet(this.__id, 'visible', !!v); }
    },
    value: {
      get: function () { return N.uiGet(this.__id, 'value'); },
      set: function (v) {
        // A radio button set: the others of its group let go.
        if (this.type === 'wfcolor') N.uiSet(this.__id, 'value', v);
        else N.uiSet(this.__id, 'value', this.type === 'slider' || this.type === 'progressbar' || this.type === 'scrollbar' ||
                                         this.type === 'wfnumber' ? Number(v) : !!v);
      }
    },
    active: {
      get: function () { return false; },
      set: function (v) { N.uiSet(this.__id, 'active', !!v); }
    },
    orientation: {
      get: function () { return this.__orientation || (this.type === 'group' ? 'row' : 'column'); },
      set: function (v) { this.__orientation = String(v).toLowerCase(); N.uiSet(this.__id, 'orientation', this.__orientation); }
    },
    alignChildren: {
      get: function () { return this.__alignChildren || ['center', 'center']; },
      set: function (v) { this.__alignChildren = normaliseAlign(v); N.uiSet(this.__id, 'alignChildren', this.__alignChildren); }
    },
    alignment: {
      get: function () { return this.__align; },
      set: function (v) { this.__align = normaliseAlign(v); N.uiSet(this.__id, 'alignment', this.__align); }
    },
    spacing: {
      get: function () { return this.__spacing === undefined ? 10 : this.__spacing; },
      set: function (v) { this.__spacing = v; N.uiSet(this.__id, 'spacing', v); }
    },
    margins: {
      get: function () { return this.__margins === undefined ? [10, 10, 10, 10] : this.__margins; },
      set: function (v) { this.__margins = typeof v === 'number' ? [v, v, v, v] : v; N.uiSet(this.__id, 'margins', this.__margins); }
    },
    preferredSize: {
      get: function () { return sizeOf(this.__preferred || [-1, -1]); },
      set: function (v) { this.__preferred = toSize(v); N.uiSet(this.__id, 'preferredSize', this.__preferred); }
    },
    minimumSize: {
      get: function () { return sizeOf(this.__minimum || [0, 0]); },
      set: function (v) { this.__minimum = toSize(v); N.uiSet(this.__id, 'minimumSize', this.__minimum); }
    },
    maximumSize: {
      get: function () { return sizeOf(this.__maximum || [10000, 10000]); },
      set: function (v) { this.__maximum = toSize(v); N.uiSet(this.__id, 'maximumSize', this.__maximum); }
    },
    size: {
      get: function () { return sizeOf(N.uiGet(this.__id, 'size')); },
      set: function (v) { N.uiSet(this.__id, 'size', toSize(v)); }
    },
    bounds: {
      get: function () { return boundsOf(N.uiGet(this.__id, 'bounds')); },
      set: function (v) { N.uiSet(this.__id, 'bounds', toBounds(v)); }
    },
    location: {
      get: function () { return sizeOf(N.uiGet(this.__id, 'location')); },
      set: function (v) { N.uiSet(this.__id, 'location', toSize(v)); }
    },
    characters: {
      get: function () { return this.__characters || 0; },
      set: function (v) { this.__characters = v; N.uiSet(this.__id, 'characters', v); }
    },
    justify: {
      get: function () { return this.__justify || 'left'; },
      set: function (v) { this.__justify = v; N.uiSet(this.__id, 'justify', v); }
    },
    image: {
      get: function () { return this.__image; },
      set: function (v) { this.__image = v; N.uiSet(this.__id, 'image', imagePath(v)); }
    },
    icon: {
      get: function () { return this.__image; },
      set: function (v) { this.image = v; }
    },
    window: {
      get: function () { var c = this; while (c.parent) c = c.parent; return c; }
    },
    items: { get: function () { return this.__items; } },
    selection: {
      get: function () {
        var s = N.uiGet(this.__id, 'selection');
        if (this.type === 'tabbedpanel') return this.children[s] || null;
        if (s === null || s === undefined) return null;
        if (this.type === 'listbox') {
          if (!s.length) return null;
          return this.properties && this.properties.multiselect ? s.map(function (i) { return this.__items[i]; }, this) : this.__items[s[0]];
        }
        return this.__items[s] || null;
      },
      set: function (v) {
        if (this.type === 'tabbedpanel') { N.uiSet(this.__id, 'selection', this.children.indexOf(v)); return; }
        var index = v === null || v === undefined ? null : (typeof v === 'number' ? v : (v.length !== undefined && typeof v !== 'string' ? v.map(function (x) { return typeof x === 'number' ? x : x.index; }) : v.index));
        N.uiSet(this.__id, 'selection', index);
      }
    }
  });
  function normaliseAlign(v) {
    if (typeof v === 'string') {
      v = v.toLowerCase();
      if (v === 'left' || v === 'right') return [v, 'center'];
      if (v === 'top' || v === 'bottom') return ['center', v];
      return [v, v];
    }
    return [String(v[0]).toLowerCase(), String(v[1]).toLowerCase()];
  }
  function toSize(v) {
    if (!v) return [-1, -1];
    if (v.length !== undefined) return [Number(v[0]), Number(v[1])];
    return [Number(v.width !== undefined ? v.width : v.x), Number(v.height !== undefined ? v.height : v.y)];
  }
  function sizeOf(a) {
    var r = [a[0], a[1]];
    r.width = a[0]; r.height = a[1]; r.x = a[0]; r.y = a[1];
    return r;
  }
  function toBounds(v) {
    if (!v) return undefined;
    if (v.length !== undefined) return [Number(v[0]), Number(v[1]), Number(v[2]), Number(v[3])];
    if (v.width !== undefined) return [v.x || v.left || 0, v.y || v.top || 0, (v.x || v.left || 0) + v.width, (v.y || v.top || 0) + v.height];
    return [v.left, v.top, v.right, v.bottom];
  }
  function boundsOf(b) {
    var r = [b[0], b[1], b[2], b[3]];
    r.left = b[0]; r.top = b[1]; r.right = b[2]; r.bottom = b[3];
    r.x = b[0]; r.y = b[1]; r.width = b[2] - b[0]; r.height = b[3] - b[1];
    return r;
  }
  function imagePath(v) {
    if (!v) return '';
    if (typeof v === 'string') return v;
    if (v.fsName) return v.fsName;
    if (v.__path) return v.__path;
    return String(v);
  }

  // add(type, bounds, text-or-items-or-value, ...more, properties)
  Control.prototype.add = function (type, bounds) {
    var args = Array.prototype.slice.call(arguments);
    if (String(type).indexOf('{') >= 0) return build(this, parseResource(type));
    var t = String(type).toLowerCase();
    if ((t === 'item' || t === 'separator') && (this.type === 'dropdownlist' || this.type === 'listbox')) return addItem(this, t, args[1]);
    var kind = TYPES[t];
    if (!kind) throw new Error('ScriptUI has no control "' + type + '"');
    var props = {}, extra = args.slice(2), more = {};
    if (kind === 'dropdownlist' || kind === 'listbox') {
      more.items = extra[0];
      if (extra[1]) props = extra[1];
    } else if (kind === 'slider' || kind === 'scrollbar') {
      more.value = extra[0]; more.minvalue = extra[1]; more.maxvalue = extra[2];
      if (extra[3]) props = extra[3];
    } else if (kind === 'progressbar') {
      more.value = extra[0]; more.maxvalue = extra[1];
      if (extra[2]) props = extra[2];
    } else if (kind === 'image' || kind === 'iconbutton') {
      more.image = extra[0];
      if (extra[1]) props = extra[1];
    } else {
      if (extra[0] !== undefined && extra[0] !== null) more.text = String(extra[0]);
      if (extra[1]) props = extra[1];
    }
    if (kind === 'divider') { kind = 'panel'; }
    return make(this, kind, bounds, props, more);
  };
  function make(parent, kind, bounds, props, more) {
    props = props || {};
    var native = {};
    for (var k in props) if (props.hasOwnProperty(k)) native[k] = props[k];
    if (more.text !== undefined) native.text = more.text;
    var b = toBounds(bounds);
    if (b) native.bounds = b;
    var object = new (CLASSES[kind] || Control)();
    var id = N.uiCreate(parent ? parent.__id : 0, kind, native);
    bind(object, id, kind, parent);
    object.properties = props;
    if (props.name) {
      object.name = props.name;
      // A name is a way in from the window: w.findElement(name), and (as
      // resource strings do) w[name].
    }
    if (more.minvalue !== undefined) object.minvalue = Number(more.minvalue);
    if (more.maxvalue !== undefined) object.maxvalue = Number(more.maxvalue);
    if (more.value !== undefined) object.value = Number(more.value);
    if (more.image !== undefined) object.image = more.image;
    if (more.items) for (var i = 0; i < more.items.length; i++) addItem(object, 'item', more.items[i]);
    if (kind === 'radiobutton' || kind === 'checkbox') object.value = false;
    return object;
  }
  function build(parent, resource, key) {
    var kind = TYPES[resource.type] || resource.type;
    var spec = resource.spec, props = {}, children = [], direct = {};
    // A child named by its key in the resource string is found by it.
    if (key) props.name = key;
    for (var k in spec) {
      if (!spec.hasOwnProperty(k)) continue;
      var v = spec[k];
      if (v && v.__type) children.push({ key: k, child: v });
      else if (k === 'properties') { for (var p in v) if (v.hasOwnProperty(p)) props[p] = v[p]; }
      else direct[k] = v;
    }
    var more = {};
    if (direct.text !== undefined) more.text = String(direct.text);
    var object;
    if (!parent) {
      object = new Window(kind, direct.text, direct.bounds, props);
    } else {
      if (kind === 'dropdownlist' || kind === 'listbox') more.items = direct.items || props.items;
      if (kind === 'slider' || kind === 'progressbar') { more.value = direct.value; more.minvalue = direct.minvalue; more.maxvalue = direct.maxvalue; }
      object = make(parent, kind, direct.bounds, props, more);
    }
    for (var key in direct) {
      if (!direct.hasOwnProperty(key) || key === 'text' || key === 'bounds' || key === 'items' || key === 'value' ||
          key === 'minvalue' || key === 'maxvalue') continue;
      object[key] = direct[key];
    }
    if (direct.value !== undefined && kind !== 'slider' && kind !== 'progressbar') object.value = direct.value;
    for (var c = 0; c < children.length; c++) {
      var made = build(object, { type: children[c].child.__type, spec: children[c].child.spec }, children[c].key);
      object[children[c].key] = made;
      if (!made.name) made.name = children[c].key;
    }
    return object;
  }

  function ListItem(list, text, index) {
    this.parent = list;
    this.text = String(text);
    this.index = index;
    this.type = 'item';
  }
  Object.defineProperty(ListItem.prototype, 'selected', {
    get: function () {
      var s = N.uiGet(this.parent.__id, 'selection');
      return s !== null && s !== undefined && (s.length !== undefined ? s.indexOf(this.index) >= 0 : s === this.index);
    },
    set: function (on) { if (on) this.parent.selection = this.index; }
  });
  ListItem.prototype.toString = function () { return this.text; };
  function syncItems(list) {
    for (var i = 0; i < list.__items.length; i++) list.__items[i].index = i;
    N.uiItems(list.__id, list.__items.map(function (item) { return item.text; }));
  }
  function addItem(list, kind, text) {
    var item = new ListItem(list, kind === 'separator' ? '-' : (text === undefined ? '' : text), list.__items.length);
    list.__items.push(item);
    syncItems(list);
    return item;
  }
  Control.prototype.remove = function (what) {
    if (this.type === 'dropdownlist' || this.type === 'listbox') {
      var at = typeof what === 'number' ? what : this.__items.indexOf(what);
      if (at < 0 && typeof what === 'string') for (var i = 0; i < this.__items.length; i++) if (this.__items[i].text === what) at = i;
      if (at >= 0) { this.__items.splice(at, 1); syncItems(this); }
      return;
    }
    var child = typeof what === 'number' ? this.children[what] : what;
    if (!child) return;
    this.children.splice(this.children.indexOf(child), 1);
    N.uiRemove(child.__id);
    delete controls[child.__id];
  };
  Control.prototype.removeAll = function () {
    if (this.type === 'dropdownlist' || this.type === 'listbox') { this.__items = []; syncItems(this); return; }
    while (this.children.length) this.remove(this.children[0]);
  };
  Control.prototype.find = function (text) {
    for (var i = 0; i < this.__items.length; i++) if (this.__items[i].text === text) return this.__items[i];
    return null;
  };
  Control.prototype.findElement = function (name) {
    for (var i = 0; i < this.children.length; i++) {
      var c = this.children[i];
      if (c.name === name || (c.properties && c.properties.name === name)) return c;
      var deeper = c.findElement(name);
      if (deeper) return deeper;
    }
    return null;
  };
  Control.prototype.addEventListener = function (type, fn) {
    (this.__listeners[type] = this.__listeners[type] || []).push(fn);
  };
  Control.prototype.removeEventListener = function (type, fn) {
    var list = this.__listeners[type] || [];
    var at = list.indexOf(fn);
    if (at >= 0) list.splice(at, 1);
  };
  var HANDLER = { click: 'onClick', change: 'onChange', changing: 'onChanging', show: 'onShow',
                  doubleclick: 'onDoubleClick', close: 'onClose', resize: 'onResize', resizing: 'onResizing',
                  activate: 'onActivate', deactivate: 'onDeactivate', draw: 'onDraw' };
  Control.prototype.notify = function (event) {
    var name = event || 'onClick';
    if (name.indexOf('on') === 0) name = name.substring(2).toLowerCase();
    fire(this, name);
  };
  function fire(control, event) {
    var handler = control[HANDLER[event]];
    var result;
    if (typeof handler === 'function') result = handler.call(control);
    var list = control.__listeners[event] || [];
    for (var i = 0; i < list.length; i++) list[i].call(control, { type: event, target: control });
    return result;
  }
  Control.prototype.show = function () { this.visible = true; return 0; };
  Control.prototype.hide = function () { this.visible = false; };
  Control.prototype.toString = function () { return '[object ' + (this.type || 'Control') + ']'; };

  // The control's own drawing (onDraw) is passed over; colours and fonts set
  // through it are taken.
  function Graphics(control) {
    this.__control = control;
    this.BrushType = { SOLID_COLOR: 0, THEME_COLOR: 1 };
    this.PenType = { SOLID_COLOR: 0, THEME_COLOR: 1 };
  }
  Graphics.prototype = {
    newBrush: function (type, color) { return { type: type, color: color }; },
    newPen: function (type, color, width) { return { type: type, color: color, lineWidth: width }; },
    newFont: function (name, style, size) { return ScriptUI.newFont(name, style, size); },
    get font() { return this.__font || ScriptUI.newFont('', 'REGULAR', 0); },
    set font(f) { this.__font = f; N.uiSet(this.__control.__id, 'font', { family: f.family || f.name, size: f.size, style: f.style }); },
    get foregroundColor() { return this.__fg; },
    set foregroundColor(pen) { this.__fg = pen; if (pen && pen.color) N.uiSet(this.__control.__id, 'foregroundColor', pen.color); },
    get backgroundColor() { return this.__bg; },
    set backgroundColor(brush) { this.__bg = brush; if (brush && brush.color) N.uiSet(this.__control.__id, 'backgroundColor', brush.color); },
    newPath: function () {},
    moveTo: function () {},
    lineTo: function () {},
    rectPath: function () {},
    ellipsePath: function () {},
    closePath: function () {},
    fillPath: function () {},
    strokePath: function () {},
    drawString: function () {},
    drawImage: function () {},
    drawOSControl: function () {},
    drawFocusRing: function () {},
    measureString: function (text, font) {
      var size = (font && font.size) || 12;
      var r = [String(text).length * size * 0.55, size * 1.3];
      r.width = r[0]; r.height = r[1];
      return r;
    }
  };

  function Layout(control) { this.__control = control; }
  Layout.prototype.layout = function () { N.uiLayout(this.__control.__id); };
  Layout.prototype.resize = function () { N.uiLayout(this.__control.__id); };

  // --- The classes a script may test with instanceof -------------------------
  function subclass(name) {
    var C = function () {};
    C.prototype = Object.create(Control.prototype);
    C.prototype.constructor = C;
    Object.defineProperty(C, 'name', { value: name });
    return C;
  }
  var Panel = subclass('Panel');
  var Group = subclass('Group');
  var CLASSES = {
    panel: Panel, group: Group, tabbedpanel: subclass('TabbedPanel'), tab: subclass('Tab'),
    button: subclass('Button'), iconbutton: subclass('IconButton'), statictext: subclass('StaticText'),
    edittext: subclass('EditText'), checkbox: subclass('Checkbox'), radiobutton: subclass('RadioButton'),
    slider: subclass('Slider'), scrollbar: subclass('Scrollbar'), progressbar: subclass('Progressbar'),
    dropdownlist: subclass('DropDownList'), listbox: subclass('ListBox'), image: subclass('Image')
  };

  // new Window(type, title, bounds, properties), or a resource string.
  function Window(type, title, bounds, props) {
    if (!(this instanceof Window)) return new Window(type, title, bounds, props);
    if (String(type).indexOf('{') >= 0) {
      var resource = parseResource(type);
      return build(null, resource);
    }
    var kind = String(type || 'window').toLowerCase();
    if (kind !== 'dialog' && kind !== 'palette') kind = 'window';
    var native = {};
    if (props) for (var k in props) if (props.hasOwnProperty(k)) native[k] = props[k];
    native.text = title === undefined ? '' : String(title);
    var b = toBounds(bounds);
    if (b) native.bounds = b;
    bind(this, N.uiCreate(0, kind, native), kind, null);
    this.properties = props || {};
    this.__result = 0;
  }
  Window.prototype = Object.create(Control.prototype);
  Window.prototype.constructor = Window;
  Window.prototype.show = function () {
    var r = N.uiShow(this.__id);
    return this.__closedWith || r;
  };
  Window.prototype.close = function (result) {
    this.__closedWith = result === undefined ? 1 : result;
    N.uiClose(this.__id, this.__closedWith);
  };
  Window.prototype.hide = function () { N.uiHide(this.__id); };
  Window.prototype.center = function () {};
  Window.prototype.update = function () {};
  Object.defineProperty(Window.prototype, 'defaultElement', {
    get: function () { return this.__default || null; },
    set: function (c) { this.__default = c; }
  });
  Object.defineProperty(Window.prototype, 'cancelElement', {
    get: function () { return this.__cancel || null; },
    set: function (c) { this.__cancel = c; }
  });
  Window.alert = function (m, t, e) { return global.alert(m, t, e); };
  Window.confirm = function (m, n, t) { return global.confirm(m, n, t); };
  Window.prompt = function (m, p, t) { return global.prompt(m, p, t); };
  Window.find = function () { return null; };

  // Events from the widgets.
  global.__wfUi = function (id, event) {
    var control = controls[id];
    if (!control) return;
    if (event === 'click' && control.type === 'button' && typeof control.onClick !== 'function' &&
        !(control.__listeners.click || []).length) {
      // A dialog's OK and Cancel close it when nothing else is said of them.
      var w = control.window;
      var name = String(control.name || (control.properties && control.properties.name) || '').toLowerCase();
      var label = String(control.text || '').toLowerCase();
      if (w.type === 'dialog' || w.type === 'palette' || w.type === 'window') {
        if (control === w.__default || name === 'ok' || label === 'ok') { w.close(1); return; }
        if (control === w.__cancel || name === 'cancel' || label === 'cancel') { w.close(2); return; }
      }
    }
    fire(control, event);
  };
  global.__wfUiClosing = function (id) {
    var w = controls[id];
    if (!w || typeof w.onClose !== 'function') return true;
    return w.onClose.call(w) !== false;
  };
  // A docked panel's Panel, laid into what the editor gave.
  global.__wfPanel = function (id, title) {
    var p = new Panel();
    bind(p, id, 'panel', null);
    p.text = title;
    p.__orientation = 'column';
    return p;
  };

  var ScriptUI = {
    version: '6.2.2',
    frameworkName: 'Wavy-Flo',
    coreVersion: '1.0',
    compatibility: {},
    FontStyle: { REGULAR: 'REGULAR', BOLD: 'BOLD', ITALIC: 'ITALIC', BOLDITALIC: 'BOLDITALIC' },
    Alignment: { LEFT: 'left', RIGHT: 'right', CENTER: 'center', FILL: 'fill', TOP: 'top', BOTTOM: 'bottom' },
    newFont: function (name, style, size) {
      if (typeof name === 'object' && name) return name;
      return { name: name || '', family: name || '', style: style || 'REGULAR', size: Number(size) || 0 };
    },
    newImage: function (normal) {
      var path = imagePath(normal);
      return { __path: path, size: [16, 16], fsName: path, toString: function () { return path; } };
    },
    environment: { keyboardState: { shiftKey: false, ctrlKey: false, altKey: false, metaKey: false, keyName: '' } },
    events: { createEvent: function (type) { return { type: type }; } },
    getResourceText: function (text) { return text; },
    applicationFonts: {}
  };

  global.ScriptUI = ScriptUI;
  global.Window = Window;
  global.Panel = Panel;
  global.Group = Group;
  for (var c in CLASSES) if (CLASSES.hasOwnProperty(c) && !global[CLASSES[c].name]) global[CLASSES[c].name] = CLASSES[c];
  global.ListItem = ListItem;
  global.__wfParseResource = parseResource;
})(this);
