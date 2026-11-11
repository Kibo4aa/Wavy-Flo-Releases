// Wavy-Flo script: the colour of the frame at the playhead, as a fill layer
// at the bottom of the composition shown -- a background that matches.
var comp = wavy.project.active;
var frame = wavy.image.render(wavy.app.frame, 160);
var average = frame.average(); // [r, g, b, a], 0..255
frame.free();
var fill = comp.addFill([average[0] / 255, average[1] / 255, average[2] / 255, 1], { name: 'Frame Colour' });
fill.moveToBottom();
wavy.log('Frame Colour: ' + average.slice(0, 3).map(Math.round).join(', '));
