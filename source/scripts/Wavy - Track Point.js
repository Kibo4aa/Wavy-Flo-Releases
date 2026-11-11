// Wavy-Flo script: a window for tracking a point in the selected video layer
// and putting something on it. Move the playhead to where the point is
// clearest, place it, track, then attach the layer chosen -- or hold the
// shot still about the point.
function shot() {
  var picked = wavy.project.selection.filter(function (l) { return l.type === 'video'; })[0];
  if (!picked) wavy.alert('Select a video layer first.');
  return picked || null;
}
function pointOf(layer) {
  var points = layer.trackPoints;
  return points.length ? points[points.length - 1] : null;
}
wavy.ui.window('Track Point', function (ui) {
  var x = ui.number('X (from middle)', 0, { decimals: 1 });
  var y = ui.number('Y (from middle)', 0, { decimals: 1 });
  var feature = ui.slider('Feature size', 16, 160, 40, null, 0);
  var search = ui.slider('Search area', 40, 400, 120, null, 0);
  var status = ui.label('Place a point at the playhead, then track it.');
  var bar = ui.progress(0);

  ui.button('Place Point at the Playhead', function () {
    var layer = shot();
    if (!layer) return;
    var p = layer.addTrackPoint('Point', [x.value, y.value], { feature: feature.value, search: search.value });
    status.value = p.name + ' placed at frame ' + wavy.app.frame;
  });
  ui.row(function (row) {
    var run = function (backwards) {
      var layer = shot(), p = layer && pointOf(layer);
      if (!p) { if (layer) wavy.alert('Place a point first.'); return; }
      var result = p.track({ backwards: backwards, onProgress: function (done, total) { bar.value = total ? done / total : 0; } });
      bar.value = 1;
      status.value = result.error || (result.frames + ' frames tracked' + (result.lostAt >= 0 ? ', lost at ' + result.lostAt : '') +
                                      '; the worst match ' + Math.round(result.lowest * 100) + '%');
    };
    row.button('Track Backward', function () { run(true); });
    row.button('Track Forward', function () { run(false); });
  });

  ui.separator();
  var names = wavy.project.active.layers.map(function (l) { return l.name; });
  var target = ui.dropdown('Attach', names.length ? names : ['(no layers)'], 0);
  ui.row(function (row) {
    row.button('Attach to Point', function () {
      var layer = shot(), p = layer && pointOf(layer);
      if (!p) return;
      var onto = wavy.project.active.layer(target.text);
      if (!onto || onto.equals(layer)) { wavy.alert('Choose another layer to attach.'); return; }
      status.value = p.applyTo(onto) + ' Position keys on ' + onto.name;
    });
    row.button('Stabilize', function () {
      var layer = shot(), p = layer && pointOf(layer);
      if (!p) return;
      status.value = p.stabilize() + ' Anchor keys: the shot is held about ' + p.name;
    });
  });
});
