# Gaussian PLY transform baking

The native **Export Model** dialog supports baking the active 3DGS or 2DGS object's translation, rotation and scale into a separate PLY. It reads complete source records and applies the source-index crop mask, never the viewport sample. Original files and optimizer checkpoints are not changed.

## Choosing the route

- An ordinary PLY without a camera-scale sidecar uses **Scene Coordinates (apply translation, rotation, scale)**.
- A PLY with a valid coordinate record uses the existing recorded-coordinate options and **Also apply the current model's scene transform**. This keeps model, cameras, crop, units and provenance bound together. This route accepts proper positive-scale similarities only; it does not permit nonuniform scaling or reflections of the recorded camera system. An invalid record cannot be bypassed through ordinary PLY baking.
- **Original Coordinates** retains the existing source-record preservation behavior. SPZ is still quantized/lossy and does not support transform baking.

## Attribute contract

Positions use source-coordinate double-precision calculations about the saved model pivot. Display recentering never enters the exported data. Known source/reference units are converted once according to the selected route; this is not CRS reprojection or independent scale validation.

The ordinary Gaussian PLY route rewrites the Gaussian's log-scales and wxyz quaternion so its transformed covariance is represented correctly. Two-scale 2DGS remains a two-scale surface Gaussian; no third scale or thickness is invented. Uniform scale adds its logarithm to the existing log-scales. DC-only nonuniform transforms use a double-precision one-sided Jacobi factorization of the transformed 3D covariance factor or 2D tangent factor.

Directional SH coefficients, in channel-major `f_rest_*` order, are transformed per band through degree 4. Rotations and conformal reflections remain within the same SH bands. The ordinary native implementation uses a deterministic real-SH change of basis with a 5-node Gauss-Legendre by 9-azimuth product rule, which is exact for these band products in real arithmetic. Storage rounding still follows the source property's float/double type.

Opacity and DC coefficients are not reactivated, clamped or quantized. All property names, types, order and unrelated custom values are retained. Custom fields are not guessed to be coordinates or physical tensors. For binary PLY, unrelated field bytes remain unchanged; for ASCII, their original numeric tokens remain unchanged. Baking recalculates affected values and is not a claim of whole-file byte identity.

## Strict-fidelity limits

An arbitrary nonuniform scaling changes the local viewing direction by a normalized inverse-affine mapping. A nonconstant finite-band SH function generally cannot express that mapping exactly at the same degree. The ordinary native exporter therefore rejects nonuniform scaling when an undeleted Gaussian has any nonzero directional SH coefficient. It does not fit an approximation, lower SH degree or discard appearance. Uniform scaling or original-coordinate export remains available. Deleted Gaussians do not cause this rejection.

Incomplete/malformed Gaussian schemas, unsupported SH layouts, non-finite or unrepresentable results fail before atomic publication. Cancellation or failure leaves an existing destination unchanged. The source-coordinate path does not require these baking-specific schema restrictions.

## Verification

`native/tests/ModelExportTests.cpp` exercises the public `exportModelFile` boundary with complete 3DGS and true two-scale 2DGS files, ASCII and both binary byte orders, cropping, double XYZ, custom attributes, SH, units, failure/cancellation and source-file protection. Recorded-coordinate tests launch the real native backend export process when its test Python/root are provided. Optional real-file tests must use explicitly selected input and separate output paths.

中文：普通高斯 PLY 选择“场景坐标”可烘焙变换；带相机尺度来源记录的模型使用原有记录路线及“同时应用当前模型的场景变换”，不能绕过来源验证。保留全部字段，2DGS 不增加厚度。非等比缩放遇到保留点的非零方向球谐系数会明确拒绝，不近似或丢弃外观。原件不变，烘焙后的相关数值会重算，不称逐字节无损。

日本語：通常の Gaussian PLY は「シーン座標」でトランスフォームをベイクできます。カメラ・スケールの出所記録があるモデルでは既存の記録付きルートと「現在のモデルのシーントランスフォームも適用」を使い、出所の検証を迂回しません。全フィールドを保持し、2DGS に厚みを追加しません。保持する点に非ゼロの方向依存 SH がある場合、不均等スケールは明示的に拒否し、外観を近似・破棄しません。元ファイルは変更せず、ベイク対象の数値は再計算されます。
