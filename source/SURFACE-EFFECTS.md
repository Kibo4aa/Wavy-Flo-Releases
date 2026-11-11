# Surface effects

Wavy-Flo's built-in pixel stack contains Fill, Gradient, Tint, Stroke, Drop
Shadow, Glow and Gaussian Blur. Drag an effect header above or below another
header to reorder it. Every effect consumes the result directly above it; for
example, Stroke followed by Glow glows the stroked result. Hide, Delete, undo
and project save/load use the same ordered stack.

## Tint

Effect List > Stylize > Tint mixes incoming RGB toward a chosen colour by its
Amount while preserving alpha. Tint below Stroke also colours the stroke. Tint
above Stroke leaves the later stroke colour intact. CPU composition uses the
parallel pixel loop and GPU composition includes Tint in the layer's single GPU
submission, so it does not require an SDK plugin or a CPU/GPU round trip.

## Gradient and Stroke

Gradient supports Linear/Radial modes, two colours, angle and amount. Its local
coordinates follow the layer bounds. Stroke supports Inside, Outside and Center
over the rendered alpha silhouette of text, shapes and footage. Static text can
use its vector-outline fast path; other content uses a cached Euclidean distance
field.

The effects use float intermediates for 8/16/32 bpc. Parameters currently stay
static in Effect Setting; SDK plugin numeric parameters can be animated in the
timeline.

`--surface-effects-self-test` checks ordered pairs, Tint, stroke coverage,
gradient direction, Fill to Glow colour, and CPU/GPU processing. The complete
17-test suite passed on 2026-09-12.
