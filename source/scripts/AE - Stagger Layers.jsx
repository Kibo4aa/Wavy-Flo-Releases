// An After Effects-style script (ExtendScript + ScriptUI), as it would be
// written for After Effects: staggers the selected layers' start times.
// Wavy-Flo runs it through its After Effects API; File > Scripts > AE - Stagger Layers.
(function staggerLayers() {
  var comp = app.project.activeItem;
  if (!(comp && comp instanceof CompItem)) {
    alert('Open a composition first.');
    return;
  }
  var layers = comp.selectedLayers;
  if (layers.length < 2) {
    alert('Select two layers or more.');
    return;
  }

  var w = new Window('dialog', 'Stagger Layers');
  w.orientation = 'column';
  w.alignChildren = ['fill', 'top'];
  var row = w.add('group');
  row.add('statictext', undefined, 'Frames between layers:');
  var frames = row.add('edittext', undefined, '3');
  frames.characters = 5;
  var order = w.add('dropdownlist', undefined, ['Top layer first', 'Bottom layer first']);
  order.selection = 0;
  var buttons = w.add('group');
  buttons.alignment = 'right';
  buttons.add('button', undefined, 'OK', { name: 'ok' });
  buttons.add('button', undefined, 'Cancel', { name: 'cancel' });
  if (w.show() !== 1) return;

  var step = (parseFloat(frames.text) || 0) * comp.frameDuration;
  var list = [];
  for (var i = 0; i < layers.length; i++) list.push(layers[i]);
  list.sort(function (a, b) { return a.index - b.index; });
  if (order.selection.index === 1) list.reverse();

  app.beginUndoGroup('Stagger Layers');
  var first = list[0].startTime;
  for (var j = 0; j < list.length; j++) list[j].startTime = first + j * step;
  app.endUndoGroup();
})();
