# Edge-on object rotation

## Failure and cause

With a horizontal camera, the Z rotation ring collapses to a line. Both direct
ring dragging and modal `R`, `Z` entered an active transform but kept returning
zero degrees. The old constrained-rotation branch relied exclusively on two
ray/plane intersections. Parallel rays returned no intersection, leaving the
angle at its initialized zero. Almost parallel rays were also poorly conditioned.

The native scene smoke reproduced this before the fix on resident/paged point
and mesh fixtures: `active true`, `pitch 0`, `angle 0` for both input paths.
The existing free-view rotation/trackball tests did not cover this degeneracy.

## Calculation

`axisRotationDragDegrees` in `ModelInteraction` now centralizes the constrained
mouse calculation, using the actual world-space constraint axis, including
rotated local axes:

- Well-facing planes retain the previous signed ray/plane angle.
- For near-edge-on planes (`abs(dot(axis, viewDirection)) < 0.2`) the front-facing
  ring tangent is projected onto the screen. Drag distance along that tangent,
  divided by the gesture's screen-space radius, gives the rotation in radians.
- This mapping is stable above/below the horizontal view and does not divide by
  the vanishing plane denominator. Orthographic and perspective cameras use the
  same projection-aware calculation. Direct handles freeze sensitivity from the
  displayed ring radius; modal rotation retains its existing radius.
- Snapping, precision modifiers, numeric input, quaternion composition, cancel,
  persistence, and undo/redo remain downstream of angle calculation. View
  orbit/navigation and free trackball rotation are unchanged.

## Regression coverage

- Math tests cover all three axes, rotated local frames, both projections,
  exact and near-edge-on views, reversed/zero/orthogonal drags, and multi-turn
  linear drag continuity. Face-on clockwise/counterclockwise angles are preserved.
- Native scene smokes exercise an actual Z ring press/drag and modal `R`, `Z`
  at horizontal views in both projection modes. They verify nonzero Z rotation,
  Ctrl snapping, Shift precision, numeric 45 degrees, cancellation, commit,
  undo, and redo on the existing point and mesh fixtures.
- The GUI regression was added and observed failing before the implementation
  changed. Combining that real event path with the extracted math seam prevents
  a successful isolated calculation from masking a broken interaction route.

Hardware review captures and logs are generated under
`native/build-unity-gizmo/edge-on-rotation-qa/`.
