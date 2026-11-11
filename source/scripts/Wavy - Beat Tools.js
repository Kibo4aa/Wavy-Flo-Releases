// Wavy-Flo script: a window of beat tools for the selected layers.
// The beats are the Main's markers (detected from the music, or placed by
// hand). Each button press is one Undo step.
wavy.ui.window('Beat Tools', function (ui) {
  var move = ui.dropdown('Move', ['Pulse (scale)', 'Flash (opacity)', 'Bounce', 'Kick'], 0);
  var strength = ui.slider('Strength', 0, 100, 15, null, 0);
  var settle = ui.slider('Settle', 1, 30, 6, null, 0);
  var every = ui.number('Every', 1, { min: 1, max: 16, step: 1, decimals: 0 });
  var hand = ui.checkbox('Hand markers too', false);
  ui.button('Pulse Selected', function () {
    var layers = wavy.project.selection;
    if (!layers.length) {
      wavy.alert('Select the layers to pulse first.');
      return;
    }
    var moves = ['scale', 'opacity', 'bounce', 'kick'];
    // Scale and opacity move by a fraction; a bounce in pixels, a kick in degrees.
    var amount = move.value <= 1 ? strength.value / 100 : strength.value;
    var keys = 0;
    layers.forEach(function (layer) {
      keys += layer.pulseOnBeats({ move: moves[move.value], strength: amount, settle: settle.value,
                                   every: every.value, handMarkers: hand.value });
    });
    wavy.app.status('Pulse on Beats: ' + keys + ' keys on ' + layers.length + ' layers');
  });

  ui.separator();
  var step = ui.number('Frames apart', 5, { min: 0, max: 300, step: 1, decimals: 0 });
  var onMarkers = ui.checkbox('On the markers', false);
  ui.button('Line Up Selected', function () {
    var layers = wavy.project.selection;
    if (layers.length < 2) {
      wavy.alert('Select two layers or more.');
      return;
    }
    wavy.project.lineUp(layers, { from: wavy.app.frame, step: step.value, onMarkers: onMarkers.value });
  });
});
