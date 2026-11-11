# Shapes, Pen and Mask tools

## The shape model

The timeline and project file use one **Shape Layer** type. Rectangle, ellipse,
polygon, star and an explicit Bezier path are generators/content owned by that
layer, rather than separate layer types. Each Shape Layer keeps its own fill and
stroke colours. Projects saved with the older rectangle/circle/polygon/star
types migrate automatically when opened.

Nothing is terminal. Layer > Break Shape Open turns a generator into the points
it was drawing, curves and handles intact, and from that moment the points drive
the shape and the old parameters do not. It freezes the shape at the current
frame, so any animation of Width, Height, Roundness or the radii is spent when
you do it.

The generator parameters live under **Shape Setup** in the timeline.

## Drawing with the shape tools

Hold **Shift** while dragging a Rectangle or Ellipse to hold both dimensions
equal, which is what makes a circle round and a rectangle square. It is applied
after snapping, so the shape stays the shape even when an edge has caught.

**Snapping** catches a drag on the edges and middle of the composition and on
the edges and middle of every other layer visible at that frame. It applies both
to drawing a new shape and to moving a layer; when moving, it is the layer own
edges that catch, not the pointer. A layer never catches on itself. The reach is
eight pixels of pointer travel rather than eight composition units, so the pull
feels the same however far the viewer is zoomed. A line shows what was caught,
because a drag stopping short with no explanation reads as a fault.

Hold **Ctrl** to suspend snapping for one drag. The Snap box beside Grid in the
viewer turns it off altogether and is remembered between sessions.

## Generator parameters

Rectangle and Ellipse tools create editable parametric paths. Layer > Add Shape
Layer contains the four generators. Expand Shape Setup > Rectangle/Ellipse/
Star/Polygon Path in the timeline to edit and keyframe the available parameters:
Width, Height, Roundness (pixels), Points, Outer Radius, Inner Radius and Path
Rotation. Polygon has at least three sides; Star supports one to 1000 points.
A one-point star is a degenerate line and needs a stroke to be visible.
Point counts round to integers during animation. Inner Radius is limited to
Outer Radius while rendering. Roundness is limited by neighboring edge lengths.

For a circle from a rectangle, make Width and Height equal and set Roundness
to at least half that size. Layer > Shape Appearance controls fill and stroke
for parametric shapes as well. Existing rectangle/ellipse projects retain their
previous default dimensions.

Use the Pen toolbar tool to create a vector shape. Click to add corners; press
and drag to create symmetric Bezier handles. Click the first point to close a
filled shape. Enter finishes an open stroked path. Backspace removes the last
draft point; Escape or switching tools cancels the draft. A completed path is
one undoable operation.

Select a shape and choose Layer > Shape Appearance for fill, stroke color,
stroke width and closed/open path settings. Shapes retain vector geometry
under scaling and can use the existing 3D and parenting transforms.

Select a content layer before choosing Mask. Draw and close the path, or press
Enter after three points. Masks use layer-local coordinates and follow layer
transforms. Layer > Masks contains Enabled, Invert and Delete for each mask.
Multiple enabled masks intersect. Masks clip the final layer including its
effects. Camera and Empty cannot be masked.

This initial implementation does not yet include editing vertices after a path
is committed, path keyframes, mask feather/expansion, or Boolean shape operators.
Drawing Bezier masks through a perspective view approximates the inverse curve
by transforming control points. For precise curves, draw masks in 2D before
enabling 3D.

Automated checks cover generator geometry, legacy migration, independent shape
colour, clipping, inverted masks and project persistence.
