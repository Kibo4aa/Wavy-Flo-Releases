# Camera, 3D layers and Empty

- Use **Layer > Add Camera** to create a perspective camera. Position X/Y/Z,
  Rotation (Z), Rotation X/Y and Zoom are animatable in Transform.
- Use **Layer > 3D Layer** on a selected text, shape, image, video or Empty.
  This exposes Position Z, Rotation X/Y, Scale Z and Anchor Z.
  A cube toggle in each timeline layer row, beside MB, also switches 3D on/off.
  Camera layers always remain 3D.
- Position, Scale, Anchor Point and Rotation display grouped X/Y/Z values for
  3D layers. Right-click the property and toggle **Separate Dimensions** to
  split or recombine its axes. The toggle preserves animation curves and is
  saved with the project. For 2D layers, Rotation has one axis and other groups
  have X/Y only.
- Use **Layer > Add Empty** for a non-rendering parent controller. Choose it
  in another layer's Parent/Link selector. Enable 3D on the Empty to control Z
  and X/Y rotation. Parenting preserves the child's world transform.
- **Anchor Point** in Transform controls the local pivot. X/Y are grouped;
  press **A** to reveal them. Coordinates are relative to the layer's center,
  so (0, 0) preserves existing projects. Anchor properties support keyframes.
- The topmost enabled Camera within its in/out points is active. With no active
  camera, a default perspective camera preserves Z=0 framing. Positive Z moves
  away from that default camera. Ordinary 2D layers ignore the camera.
- Empty guides are visible only in the composition viewer. Camera and Empty
  never appear in exported frames. Select cameras through the timeline.

The renderer projects flat layer planes (2.5D), on both CPU and GPU paths. It
does not implement mesh geometry, lighting, extruded text, depth of field or
per-pixel intersections between planes. Contiguous 3D groups are sorted by
their center depth; 2D layers retain their stack boundaries. Planes crossing
the near plane are hidden to avoid an unbounded projection. Inline text editing
uses Qt's proxy editor and still needs interactive validation under perspective.

Build and runtime tests have not been executed for this change, at the user's
request. After building, `Wavy-Flo.exe --3d-self-test` writes
`3d-self-test.txt`; CTest exposes the same checks as `scene_3d`. GPU comparisons
include perspective and explicit-camera cases via `--gpu-self-test`.
