# Shared scene coordinates

## Coordinate contract

One scene uses one display reference. Each model keeps its source coordinates,
declared units/CRS, and its own editable translation, rotation and scale. The
first object in the persisted scene order supplies the scene reference; changing
the active or selected object does not change that reference. Camera clipping
limits and the minimum orthographic field of view use this same reference unit,
so switching source units cannot resize a close-up view. A collection in
the project tree is an organizational container, not a registration result.
Deleting the first object selects the next object in that order as the reference;
replacing the first object's source also changes its reference data. These are
explicit collection edits, not selection-only operations. When known and unknown
units are mixed, changing the reference cannot guarantee a physically meaningful
relationship between those models: unknown units remain unconverted raw values.

When both a model and the scene reference declare length units, the model is
converted to the reference units before display. If either side has unknown
units, numerical source coordinates are preserved without guessing metres or
fitting one model's bounds to another. The Properties panel's **Scene Coordinate
Relationship** summary and tooltip explain the current relationship. Matching
unit or CRS labels do not prove that independent reconstructions are registered.

The renderer may use local floating-point buffers and a precision-preserving
display shift. These are reversible display representations, not per-model
recentring, independent size normalization, a rewritten source file, or an
automatic registration. The scene reference is shared across point clouds,
Gaussian models, meshes, camera display and the grid. Focusing a model changes
the camera, not the model's source coordinates.
The isolated task-preview layer retains its own reference; observing a running
task does not assert that its reconstruction is registered to existing objects.

**Scene Base** places the reference plane beneath the collection, not beneath
whichever model is selected. **World Z=0** continues to use the shared zero plane.
The saved preference identifier remains `modelBase` for compatibility. Source
coordinates and original files are not rewritten by changing the reference
plane or the display language.

The reported scene-base elevation retains source double precision through the
local model transform and shared length-unit conversion. GPU float bounds are
not round-tripped into coordinate reports. Regression fixtures cover decimal
elevations in points, 3DGS, 2DGS and meshes, mixed metre/centimetre sources,
translated collection minima and active-object changes.

中文：场景底部高程在模型变换和共享长度单位换算中保留源数据双精度，不从 GPU
单精度范围反算坐标报告。回归覆盖点云、3DGS、2DGS 和网格的小数高程、米／厘米
混合单位、变换后的集合底部及活动对象切换。

日本語：シーン底面の高さは、モデルのトランスフォームと共通長さ単位への換算を
通して元データの倍精度を保持します。GPU の単精度の範囲から座標レポートへ
戻しません。点群、3DGS、2DGS、メッシュの小数の高さ、メートル／センチメートル
の混在、移動後のシーン底面、アクティブオブジェクトの切り替えを回帰検証します。

## Import, editing and limits

- **Append** preserves existing objects and adds the source coordinates to the
  shared scene. It explicitly does not auto-centre, fit scale or register models.
- **Replace Current** affects only the active object. Other objects and their
  user-authored transforms remain separate.
- User-authored transforms are retained on save/reopen. Display-reference
  conversion is not a reason to reset those transforms.
- Numeric translation and grid snapping use the shared scene unit. For example,
  with a metre reference, `G X 1` moves a centimetre source by 100 cm. Stored
  per-object translation and the source-coordinate inspector retain source units.
- Units can be converted only when declared. This change does not add CRS
  reprojection, surveyed control points, correspondence picking or ICP.
- Related generation outputs can share source coordinates when the generating
  pipeline preserves that frame. Similar names, a common folder or the same
  photographs are not proof: rerunning reconstruction can produce a different
  origin, orientation and scale.
- The shared presentation applies to 3DGS, 2DGS, point clouds, TSDF meshes,
  OpenMVS textures, SuGaR and GS2Mesh outputs. It does not alter training inputs,
  optimizer checkpoints or generation algorithms, and does not promise that
  independently produced models will overlap.

## Export units and coordinate reports

Original-coordinate export keeps source units. Scene-coordinate export applies
the model's editing transform and converts declared source/reference units into
the scene-reference unit. If either unit is unknown, the numeric conversion
factor stays one; this is not an inferred physical scale. GLB subsequently follows
its Y-up/metre convention, including its explicit one-unit-equals-one-metre
fallback for unknown output units. Gaussian PLY and SPZ remain restricted to
original source coordinates; this change does not add Gaussian transform baking.
Temporary display shifts are never baked into model files.
Source-coordinate PLY export embeds recognized units/CRS that depend on a source
`.prj` in the PLY header, leaving geometry and attributes unchanged; without
sidecar-dependent metadata, an otherwise unchanged untextured PLY remains byte-identical.
Scene-coordinate PLY export drops source CRS declarations when coordinates are
changed, and a conflicting or unreadable destination `.prj` prevents publication
without modifying existing files; sidecars are never copied or overwritten.

Coordinate/dimension reports keep the active file's source coordinates and units.
The shared reference plane elevation is converted back from scene-reference units
to the active source units in that report. The report does not claim that the
active source CRS and another model's CRS have been transformed or registered.

## Comparison with other software

### Reported project: source evidence

Read-only inspection of the two source bounds in the reported project found
dimensions of approximately `5.074219 × 2.476563 × 1.395462 u` for the mesh and
`19.361549 × 22.909439 × 10.470222 u` for the Gaussian model. Neither source
declares origin/reference metadata. These are source-coordinate extents, not
verified physical dimensions or a registration error measurement. The extents
alone cannot determine a correct scale, rotation or translation between the
models, and do not establish that the mesh was generated from the current
training reconstruction or its camera set. Existing user-authored transforms
must remain intact; no automatic alignment of these real files is claimed.

中文：当前报告工程的只读源范围检查得到：网格约为
`5.074219 × 2.476563 × 1.395462 u`，高斯模型约为
`19.361549 × 22.909439 × 10.470222 u`；两者均无原点／参考元数据。这只是原始
坐标范围，不是已确认的物理尺寸或配准误差，也不能证明网格与当前训练／相机组
属于同一次重建。不能据此自动缩放重叠，更不能覆盖用户已有的编辑变换。

日本語：報告されたプロジェクトの元の範囲を読み取り専用で調べると、メッシュは
約 `5.074219 × 2.476563 × 1.395462 u`、ガウシアンモデルは約
`19.361549 × 22.909439 × 10.470222 u` で、どちらにも原点／基準のメタデータが
ありません。これは元の座標範囲であり、確定した実寸や位置合わせ誤差では
ありません。同じ再構築やカメラ群から生成された証拠にもならないため、自動で
大きさを合わせたり、既存のユーザー変換を上書きしたりしません。

### Design references

| Software | Relevant distinction | GSW design consequence |
| --- | --- | --- |
| Blender | Global axes describe world space; Local axes describe each object's space. Collections organize objects. | Keep one scene frame and independent object transforms; collection membership is not alignment. |
| CloudCompare | Global Shift/Scale records the reversible mapping from original coordinates to precision-preserving working coordinates. A suitable prior shift can be reused for subsequent clouds. Registration is a separate operation. | Share the display reference and retain metadata; never hide source-coordinate differences by normalizing each model independently. |
| Metashape | A chunk has its own transform into world coordinates. Models from separate projects are explicitly aligned before chunks are merged. | Distinguish shared reconstruction coordinates from independently reconstructed or unregistered inputs. |

Official references:

- [Blender: Transform Orientations](https://docs.blender.org/manual/en/3.0/editors/3dview/controls/orientation.html)
- [Blender: Transform Properties](https://docs.blender.org/manual/en/4.2/scene_layout/object/properties/transforms.html)
- [CloudCompare: Global Shift and Scale](https://www.cloudcompare.org/doc/wiki/index.php/Global_Shift_and_Scale)
- [CloudCompare: Alignment and Registration](https://www.cloudcompare.org/doc/wiki/index.php/Alignment_and_Registration)
- [Metashape: Chunk transform and coordinate system](https://download.agisoft.com/metashape-java-api/latest/com/agisoft/metashape/Chunk.html)
- [Metashape: Manual chunk alignment](https://agisoft.freshdesk.com/support/solutions/articles/31000157701-manual-chunk-alignment-standard-edition-)

## 简体中文

同一场景使用共享显示参考，每个模型仍保留原始坐标、声明的单位／CRS 和独立的
编辑变换。参考来自工程保存顺序中的第一个对象，不随当前选中对象变化。双方
单位已声明时换算到参考单位；任一方单位未知时保留原始数值，不擅自当作米，也
不按包围盒把模型缩放到相同大小。“属性 → 场景坐标关系”及其提示说明当前关系。
单位或 CRS 一致不代表不同重建已经配准。
独立任务预览层保留自己的参考，观察运行中的任务不代表它已与原有对象配准。
删除首对象会改用下一个对象的参考；替换首对象源文件也会改变参考数据。
这些是明确的场景内容修改，不是切换选择。混合已知／未知单位时，更换参考不能
保证模型间的真实物理关系，未知单位仍保留原始数值。

“同时导入”不自动居中或配准；同图层、同目录、同照片和相似文件名都不是已配准
的证据。“场景底部”基准面使用整组对象的底部，不随选择跳到某一模型底部。
原文件和已保存的用户变换保留。本次不新增 CRS 重投影、控制点或 ICP 功能，不
修改 3DGS／2DGS 及网格生成算法或训练检查点。
数字移动和网格吸附使用共享场景单位。例如以米为参考时，`G X 1` 会将厘米模型
移动 100 cm；工程保存的逐对象位移和源坐标属性仍使用源单位。

导出“原始坐标”保留源单位；“场景坐标”应用模型变换，并在双方单位已声明时
换算到场景参考单位。任一单位未知时倍率保持 1，不猜测物理比例。GLB 随后再按
格式约定转换为 Y 向上／米制，未知输出单位使用明确的 1 单位 = 1 m 回退。
Gaussian PLY／SPZ 仍仅支持原始坐标；临时显示偏移不写入导出模型。
坐标／尺寸报告使用当前源文件单位，共享基准面高程也换算回当前源单位；这不代表
已完成 CRS 转换或配准。

## 日本語

同じシーンでは共通の表示基準を使用し、各モデルの元の座標、宣言済み単位／
CRS、個別の編集トランスフォームを保持します。基準はプロジェクトの保存順で
最初のオブジェクトとし、選択を変えても切り替えません。両方の単位が既知なら
基準単位へ換算し、どちらかが不明なら元の数値を保持します。メートルと推測
したり、バウンディングボックスに合わせて同じ大きさにしたりしません。
「プロパティ → シーン座標の関係」とツールチップで現在の関係を説明します。
単位や CRS の一致だけでは、別々の再構築の位置合わせは保証されません。
独立したタスクプレビューレイヤーは固有の基準を保ち、実行中タスクの表示は既存
オブジェクトとの位置合わせを保証しません。
最初のオブジェクトを削除すると次のオブジェクトが基準になり、最初のソースを
置き換えても基準データが変わります。これは選択変更ではなく明示的な内容編集
です。既知／不明の単位が混在する場合、基準を変えてもモデル間の物理的な関係は
保証できません。不明な単位は元の数値を保持します。

「同時にインポート」は自動的な中心合わせや位置合わせを行いません。同じ
レイヤー、フォルダー、写真、似た名前は位置合わせ済みの証拠ではありません。
「シーン底面」はオブジェクト全体の底面を使い、選択に応じて移動しません。
元ファイルと保存済みのユーザー変換を保持します。本変更は CRS 再投影、対応点、
ICP を追加せず、3DGS／2DGS、メッシュ生成処理、学習状態を変更しません。
数値移動とグリッドスナップは共通のシーン単位を使います。メートルが基準なら、
`G X 1` はセンチメートルのモデルを 100 cm 移動します。保存する各オブジェクトの
移動量と元座標のプロパティはソース単位を保持します。

「元の座標」のエクスポートはソース単位を保持します。「シーン座標」はモデル
変換を適用し、両側の単位が宣言済みならシーン基準単位に換算します。どちらか
が不明なら倍率を 1 に保ち、物理的な比率は推測しません。GLB はさらに形式の
規約に従って Y-up／メートルへ変換し、出力単位が不明なら明示された 1 単位 =
1 m の扱いを使います。Gaussian PLY／SPZ は引き続き元の座標に限定し、一時的な
表示シフトはモデルに書き出しません。座標／寸法レポートは現在のソース単位を
使い、共通基準面の高さもその単位へ戻します。CRS 変換や位置合わせを完了した
という意味ではありません。
