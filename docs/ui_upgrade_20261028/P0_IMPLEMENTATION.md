# Navigation coverage implementation candidate

Status: IMPLEMENTED / REMOTE_ACCEPTANCE_PENDING. This is not a whole-product
completion or a claim about reconstruction quality.

The frozen real-window baseline is run40641681307 at4f1dbb8:20cases,
14edge failures. The independent oracle and performance evaluator's12contracts
each passed remote run40648965098. Thresholds and original failures are retained.

The candidate keeps the same Blender-adapted logical axis projections, ordering,
colors, circles, labels, buttons and hit targets. Only the composition adapter
changes: Qt's raster paint engine covers those vectors at the **actual native
physical pixel resolution** in a navigation-sized premultiplied-alpha QImage,
then composites that image at a physical-pixel-aligned origin. There is no
whole-viewport supersampling, scene rescaling, new timer or reduced model detail.
This coverage adapter is GSW code using Qt APIs, **not** a claimed transplant of
Blender's GPU coverage shader. Existing Blender/Lucide notices remain unchanged.

One per-viewport cache includes exact geometry/display state, language, font,
actual DPR and logical font DPI. Theme, hover/press, camera/projection, layout,
font or locale changes cannot reuse an unrelated tile. Old pixel storage is
released before replacement. Native image upload requests and conservative CPU
working storage are bounded by8MiB; requested bytes are not measured GPU VRAM
residency. No custom GL resource or context-owned handle is introduced.
Unsupported painter composition/opacity/world transforms, invalid dimensions
or allocation/paint-engine failure fall back to the original vector adapter;
they do not draw an empty or stale control. The real diagnostic receipt names
the selected path, raster and destination paint engines, cache generation and
resource accounting separately.

Remote gates: unchanged20-case physical-pixel oracle; existing real navigation
and camera contracts in all3languages; native-DPR raster module contracts;
20theme/resize cycles plus instant language cache invalidation; three paired
fresh-process performance rounds against pre-AA common-observer commit
`b0e9fae5c53156b74e247e9c56ab3d0bd1e4a0ff`.
The fixed viewport is established before the first observed paint, and every
raw cold/warm sample carries its size/DPR. Performance recordings never capture
screenshots/read framebuffer or query GL during measured/idle windows.

Local work was static source/translation validation and compiler/linker checks
only. No local app, GUI, automated test, installer or training was executed.
User NVIDIA hardware, real cross-monitor migration, arbitrary RGB/alpha
background feature, later shell/gizmo/forms and installed delivery remain
separate gates; they cannot be inferred from this implementation.
