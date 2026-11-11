// Wavy-Flo script: a kinetic title at the playhead -- its words pop in one
// after another, float while it stays, and fly out at random at the end.
// Three Text Animations, stacked; each can be opened and changed in
// Properties > Text Animation afterwards.
var comp = wavy.project.active;
var words = wavy.prompt('Title text', 'Wavy-Flo moves', 'Kinetic Title');
if (words !== null && words !== '') {
  var at = Math.round(wavy.app.frame);
  var stay = 60; // frames it holds before going out
  var title = comp.addText(words, { name: 'Kinetic - ' + words, fontSize: 120 });
  title.position = [comp.width / 2, comp.height / 2];
  title.start = at;

  // In: word after word, from nothing, with a little overshoot of its own.
  title.addTextAnimator({ preset: 'Word Pop', from: at, duration: 24, delay: 6, name: 'Words In' });

  // While it stays: a gentle float.
  title.addTextAnimator({ preset: 'Wiggle Float', name: 'Float', wiggleSpeed: 1.2, wigglePosition: 6 });

  // Out: letters scatter upward in a random order and fade.
  var out = title.addTextAnimator({ name: 'Scatter Out', x: 0, y: 0, opacity: 100, separateOut: true,
                                    outY: -320, outRotation: 60, outScale: 60, outOpacity: 0, outDelay: 1,
                                    order: 'random', mode: 'random' });
  out.animateOut(at + 24 + stay, at + 24 + stay + 14, 'easeIn');
  title.duration = 24 + stay + 30;

  title.select();
  wavy.log('Kinetic Title: "' + words + '" from frame ' + at);
}
