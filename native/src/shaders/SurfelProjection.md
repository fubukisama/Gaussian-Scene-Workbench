# Independently implemented 2D Gaussian projection

`SurfelProjection.glsl` is new Gaussian Scene Workbench code licensed under
GPL-3.0-or-later. It is an implementation of published mathematics, not a
relicensing or translation of the previously embedded 2DGS rasterizer code.
The author of this replacement did not read or copy the restricted embedded
shader or the upstream rasterizer implementation during this work.

## Primary sources

- Huang, Yu, Chen, Geiger and Gao, *2D Gaussian Splatting for Geometrically
  Accurate Radiance Fields*, SIGGRAPH 2024,
  [arXiv:2403.17888v1](https://arxiv.org/html/2403.17888v1), section 4.1,
  equations (4)-(6); section 4.2, equations (7)-(11).
- [Official project page](https://surfsplatting.github.io/) identifies the
  paper and its authors.

The arXiv paper is marked CC BY 4.0. That publication license and the licenses
of the authors' separate software are distinct from the license of this new
implementation. No third-party software source is included here. Attribution
above credits the method; the algebra, numerical policy and GLSL expression
below were derived for this application.

## Contract and derivation

Write `s = (u,v,1)`. The three input rows define a homography:

```
x = dot(Tu,s) / dot(Tw,s)
y = dot(Tv,s) / dot(Tw,s)
```

This follows the paper's local tangent-plane parameterization and homogeneous
projection. The Gaussian exponent is `-(u*u + v*v)/2`. The visible support used
for rasterization is the local disk `u*u + v*v <= 9` (three standard deviations).
The positive-camera-depth convention is separate from the denominator: under
orthographic projection `dot(Tw,s)` is constant, not a camera distance.

### Bounding intervals from a conic tangent equation

For one projected axis let numerator row be `n`, denominator row be `w`. The
level set for a screen coordinate `q` is a line in the local disk:

```
(n.xy - q*w.xy) dot (u,v) + n.z - q*w.z = 0.
```

The endpoint of its projected interval is a tangent to the radius-three circle.
The tangent condition is therefore

```
(n.z - q*w.z)^2 = 9 * |n.xy - q*w.xy|^2.
```

For numerical stability normalize all rows by positive `w.z`, let
`h = w.xy/w.z`, `c = n.z/w.z`, `d = n.xy/w.z - c*h`, and use the shifted
coordinate `t = q-c`. This gives the quadratic

```
A*t^2 + B*t + C = 0
A = 1 - 9*dot(h,h)
B = 18*dot(d,h)
C = -9*dot(d,d).
```

The disk has a finite front-facing projection only if
`1 - 3*length(h) > 0`. A small floating-point safety margin rejects a disk
touching or crossing the projective horizon rather than producing infinite
geometry. The shader rescales `d` before the quadratic solve, uses the stable
root `r = -(B + sign(B)*sqrt(B*B-4*A*C))/2` with the companion root `C/r`, and
handles `d=0` explicitly. It solves each axis independently, so no matrix
inverse or ellipse orientation decomposition is needed. A planar disk that
projects to a line may still have finite bounds; its singular intersection is
rejected at fragment evaluation.

Each interval receives a small outward floating-point allowance, proportional
to the screen-coordinate magnitude and inversely proportional to `A`. The
near-horizon conditioning term prevents a rounded bound from trimming the
support; ordinary viewport-scale bounds expand by much less than one pixel.

The two axis intervals give the AABB. The paper's screen-space filter has
standard deviation `sqrt(2)/2`; its three-sigma support is `sqrt(4.5)` pixels.
Filter support must also be included in the rasterized AABB.

### Intersection as a two-by-two linear system

For pixel `(x,y)`, cross-multiplication gives

```
a = Tu - x*Tw
b = Tv - y*Tw
a.x*u + a.y*v = -a.z
b.x*u + b.y*v = -b.z.
```

The implementation solves this system by its two-by-two determinant. It first
forms the equations by subtraction, then normalizes each equation to avoid
scale-dependent singularity checks. Subtracting a screen-center term only
after common rescaling can create a small cancellation residual, which becomes
enormous relative to an extremely small tangent coefficient. Direct finite
subtraction preserves exact center intersections. If finite inputs overflow
the multiply/subtract, a common-scaled path is used instead. The implementation
splits that common division into two divisions by the square root of the scale,
so GPU reciprocal division does not flush a subnormal reciprocal to zero when
the scale is near the largest finite float. It
rejects non-finite or ill-conditioned systems, and checks that the recovered
homogeneous denominator is finite and positive. It uses a determinant solve,
not a three-vector cross product. This is the same geometric intersection
described by the paper, expressed directly as a linear solve.

### Low-pass exponent and fragment depth

The maximum of two Gaussian values is the exponential of the smaller squared
distance. With the paper's filter variance of one half, this becomes

```
power = -0.5 * min(dot(uv,uv), 2*dot(pixel-filterCenter,pixel-filterCenter)).
```

For depth, the input `cameraDepth` is an affine row of the tangent plane in
positive camera-depth coordinates. Let `z = dot(cameraDepth,(u,v,1))`.
OpenGL eye coordinates use `eyeZ=-z`. The shader computes clip `z` and `w`
from the projection matrix's depth rows, divides to obtain NDC depth, and
maps `[-1,1]` to `[0,1]`. It accepts conventional perspective and orthographic
matrices whose clip depth depends only on eye `z` and the homogeneous
constant. Invalid, behind-camera, out-of-clip-range or non-finite results
return `-1`; they must be discarded, not clamped into a visible plane.

## Independently computed fixture values

These are algebraic expectations for tests of the actual public shader/viewport
seam, not a claim that a CPU calculation exercises GPU code.
The listed extents are exact geometric values before the outward floating-point
allowance (approximately 0.00044 pixels for the orthographic fixture).

| Case | Tu; Tv; Tw | Expected bounds center | Expected half extent |
| --- | --- | --- | --- |
| Orthographic | `(10,0,400); (0,20,300); (0,0,1)` | `(400,300)` | `(30,60)` |
| Tilted perspective | `(50,0,400); (30,20,300); (0.1,0,1)` | `(390.10989011,300)` | `(32.96703297,62.89709020)` |
| Horizon crossing | `(170,0,400); (120,20,300); (0.4,0,1)` | `false` | not used |

- Orthographic pixel `(420,280)` intersects at `uv=(2,-1)` with denominator 1.
- Tilted pixel `(409.09090909,336.36363636)` intersects at `uv=(1,2)` with
  denominator 1.1.
- With near plane 1 and far plane 11, a point at positive camera depth 6 has
  OpenGL fragment depth `11/12` for a conventional perspective matrix and
  `1/2` for a conventional orthographic matrix.
- `uv=(10,0)` and pixel-filter offset `(0.5,0)` give `power=-0.25`; the filter
  branch, not the local disk branch, determines that value.

## Verification ledger

- 2026-10-14: Read project `AGENTS.md` and `CONTEXT.md`; selected parent plan
  `.planning/gizmo-shaft-20261014`. Parent owns shared plan/integration/tests.
- 2026-10-14: Consulted the official project page and the public arXiv paper
  equations only. No restricted shader/rasterizer code was consulted.
- A seeded numerical reference check covered 2,000 finite homographies and
  721 samples of each radius-three circle boundary, plus the filter support.
  The double-precision intervals contained all sampled projected points.
  A separately translated float32 design check showed subpixel rounding
  undercoverage up to 0.00473 pixels for large near-horizon bounds before
  the numerical allowance was added. The shader now expands intervals
  outward for that conditioning; all 2,000 by 721 samples were contained
  in the repeated float32 design check. The orthographic fixture's computed
  half extents were approximately `(30.000427,60.000374)`.
- A float32 design check of the determinant solve recovered both supplied
  intersections with maximum local-coordinate error below `1e-4`, under
  common homography scales `1e-12`, `1` and `1e12`. It also rejected an
  exactly singular two-equation system and a negative denominator. These
  checks are not actual GLSL execution and do not replace the integration gate.
- The parent's actual public GLSL-module test was reproduced on a standalone
  OpenGL 3.3 float FBO on the NVIDIA GeForce RTX 4080 Laptop GPU. Seventeen
  cases passed, while a tiny tangent at the center produced spurious local
  coordinates around `(8940697,3352761.25)`. This established the rescaling
  cancellation issue described above. The intersection subtraction-order fix
  made the unchanged test pass all 18 cases, with the tiny-center result
  exactly `(0,0)` and denominator 1. No singularity thresholds were changed.
- One further actual-GPU fixture uses finite rows with magnitude `1e38`,
  where the raw subtraction overflows but the homography is still solvable.
  The initial single-division fallback returned invalid. Splitting its common
  normalization into two square-root divisions made it return exactly
  `uv=(-4,-4)` and a finite positive denominator approximately `1e38`.
  The permanent public GLSL-module harness now passes all 19 cases.
- A temporary in-memory test insertion initially matched both result prints,
  causing recursion in the harness. No test file was changed by that attempt;
  targeting the unique final summary marker corrected the insertion.
- Public viewport/image/depth integration remains a separate parent gate.
  No visible GUI was launched here.
- Read-only command issue: `rg` does not expand a Windows path glob supplied
  as an argument; subsequent inspections use concrete paths.
- Runtime probe: the existing D-drive Python has NumPy but not PySide6, so
  it cannot supply a Python/Qt offscreen shader-compilation harness. A bounded
  executable discovery encountered an inaccessible WinSAT cache directory;
  no file or permissions were modified.
