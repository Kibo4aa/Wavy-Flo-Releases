// Wavy-Flo script: a "Spin" control on each selected layer (degrees a
// second), and an expression on its Rotation that reads it. Change, key or
// wire the Spin slider in the timeline (under Effect) and the layer follows.
var layers = wavy.project.selection;
if (!layers.length) {
  wavy.alert('Select the layers to rig first.');
} else {
  layers.forEach(function (layer) {
    var spin = layer.control('Spin') || layer.addControl('slider', 'Spin', 90);
    layer.rotation.expression = 'value + time * ' + spin.reference;
  });
  wavy.app.status('Spin rigged on ' + layers.length + ' layer(s): effect("Spin")("Slider") degrees a second');
}
