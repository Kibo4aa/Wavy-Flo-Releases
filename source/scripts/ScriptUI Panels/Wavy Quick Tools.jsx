// A docked panel: Window > Script Panels > Wavy Quick Tools.
// Written the way After Effects' ScriptUI panels are -- `this` is the Panel
// when docked, and run from File > Scripts it opens as a floating palette --
// with Wavy-Flo's own API (wavy) doing the work. Each button is one Undo step.
(function (thisObj) {
  var w = (thisObj instanceof Panel) ? thisObj : new Window('palette', 'Wavy Quick Tools', undefined, { resizeable: true });
  w.orientation = 'column';
  w.alignChildren = ['fill', 'top'];
  w.spacing = 6;

  var add = w.add('panel', undefined, 'Add');
  add.orientation = 'row';
  var titleButton = add.add('button', undefined, 'Title');
  var backgroundButton = add.add('button', undefined, 'Background');

  var picked = w.add('panel', undefined, 'Selected layers');
  picked.orientation = 'column';
  picked.alignChildren = ['fill', 'top'];
  var names = wavy.project.effectNames;
  var effects = picked.add('dropdownlist', undefined, names);
  effects.selection = Math.max(0, names.indexOf('Glow'));
  var effectButton = picked.add('button', undefined, 'Add Effect');
  var centerButton = picked.add('button', undefined, 'Center');
  var fadeButton = picked.add('button', undefined, 'Fade In (10 frames)');

  function selection() {
    var layers = wavy.project.selection;
    if (!layers.length) wavy.alert('Select a layer first.');
    return layers;
  }

  titleButton.onClick = function () {
    var comp = wavy.project.active;
    var title = comp.addText('Title', { name: 'Title', fontSize: 120 });
    title.position = [comp.width / 2, comp.height / 2];
    title.select();
  };
  backgroundButton.onClick = function () {
    wavy.project.active.addFill([0.07, 0.07, 0.09, 1], { name: 'Background' }).moveToBottom();
  };
  effectButton.onClick = function () {
    if (!effects.selection) return;
    selection().forEach(function (layer) { layer.addEffect(effects.selection.text); });
  };
  centerButton.onClick = function () {
    var comp = wavy.project.active;
    selection().forEach(function (layer) { layer.position = [comp.width / 2, comp.height / 2]; });
  };
  fadeButton.onClick = function () {
    var at = Math.round(wavy.project.frame);
    selection().forEach(function (layer) {
      layer.opacity.setAt(at, 0);
      layer.opacity.setAt(at + 10, 1);
    });
  };

  if (w instanceof Window) w.show();
  else w.layout.layout(true);
})(this);
