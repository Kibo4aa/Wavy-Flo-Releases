// Wavy-Flo script: render the composition shown to a movie (or a PNG
// sequence), its Play Range or all of it, with a progress window that can
// stop it. To render in the background instead, see wavy.renderQueue.
var comp = wavy.project.active;
var formats = ['mp4', 'mov', 'prores', 'png'];
var format, whole, bitrate;
var ok = wavy.ui.dialog('Render ' + comp.name, function (ui) {
  format = ui.dropdown('Format', ['MP4 (H.264)', 'MOV (Animation)', 'MOV (ProRes 4444)', 'PNG sequence'], 0);
  whole = ui.checkbox('Whole composition (not just the Play Range)', !wavy.project.playRange);
  bitrate = ui.number('Bitrate (Mb/s)', 50, { min: 1, max: 400, step: 5, decimals: 0 });
}, 'Render');

if (ok) {
  var kind = formats[format.value];
  var file = kind === 'png'
      ? wavy.fs.chooseFolder('Render the frames into')
      : wavy.fs.saveFile('Render to', kind === 'mp4' ? 'MP4 (*.mp4)' : 'QuickTime (*.mov)');
  if (file) {
    var range = whole.value || !wavy.project.playRange ? [0, comp.frames - 1] : wavy.project.playRange;
    var bar, status;
    var progress = wavy.ui.window('Rendering ' + comp.name, function (ui) {
      status = ui.label('Starting...');
      bar = ui.progress(0);
      ui.button('Stop', function () { stopped = true; });
    });
    var stopped = false;
    var made = comp.renderVideo(file, {
      format: kind, from: range[0], to: range[1], bitrate: bitrate.value,
      onProgress: function (done, total) {
        bar.value = total ? done / total : 0;
        status.value = done + ' of ' + total;
        return !stopped;
      }
    });
    progress.close();
    if (made) {
      wavy.app.status('Rendered ' + made);
      if (wavy.confirm('Rendered to\n' + made + '\n\nOpen it?')) wavy.fs.open(made);
    } else {
      wavy.app.status('Render stopped');
    }
  }
}
