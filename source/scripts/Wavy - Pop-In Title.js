// Wavy-Flo script, in Wavy-Flo's own API (wavy): a title that pops in at the
// playhead, settles, and glows. File > Scripts > Wavy - Pop-In Title.
// The whole run is one step Undo takes back.
//
// Frames are the Main's; scale and opacity are fractions (1 = 100 %).
var comp = wavy.project.active;
var words = wavy.prompt('Title text', 'Wavy-Flo', 'Pop-In Title');
if (words !== null && words !== '') {
  var at = Math.round(wavy.project.frame);
  var title = comp.addText(words, { name: 'Title - ' + words, fontSize: 140 });
  title.position = [comp.width / 2, comp.height / 2];

  // Scale: 0 -> 115 % -> 100 %, eased into the overshoot and out of it.
  title.scale.setAt(at, [0, 0]);
  title.scale.setAt(at + 8, [1.15, 1.15]);
  title.scale.setAt(at + 14, [1, 1]);
  title.scale.ease(at + 8, 'easeInOut');
  title.scale.ease(at + 14, 'easeIn');

  // Fades in over the first 6 frames.
  title.opacity.setAt(at, 0);
  title.opacity.setAt(at + 6, 1);

  title.addEffect('Glow');
  wavy.project.addMarker(at + 14, words);
  title.select();
  wavy.log('Pop-In Title: "' + words + '" at frame ' + at);
}
