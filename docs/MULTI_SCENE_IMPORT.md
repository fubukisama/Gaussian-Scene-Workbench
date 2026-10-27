# 多对象导入

通过“导入 PLY 场景/网格”选择文件。空场景直接导入；已有对象时，弹窗提供：

- **同时导入**（默认）：保留已有对象，在同一视口加入新对象。
- **覆盖当前**：只替换当前选中的对象，其余对象保留。
- **取消**：不改变现有对象。

这两种导入方式都不会删除或改写原始 PLY。工程树的“场景”节点列出全部对象；点击列表条目或画面里的实际模型可切换当前对象。单选时位移、旋转、缩放只影响该对象，多选时作用于整组。裁剪和导出仍针对当前单个对象。切换对象前，尚未导出的裁剪删除仍要求导出、放弃或取消；放弃会明确恢复当前对象的点数据。

点云、Mesh 和高斯模型共用视口，相机保持统一。各对象有独立的数据、变换、GPU 缓冲、纹理和磁盘分页状态；同一坐标系中的大坐标数据按各自的局部显示偏移映射到当前场景坐标，不会因为分别导入而都被强制移动到零点。点击多个重叠模型时，按实际图元深度选择较近的对象；包围盒内部的空白不作为多对象命中区域。

## 同时选中与整组变换

列表选择参考 [CloudCompare 的 DB tree 操作说明](https://www.cloudcompare.org/doc/wiki/index.php/DB_tree)。这里选的是完整模型对象，不是模型内部的点。

- 工程树：单击替换选择，Ctrl 单击增减，Shift 连续范围选择。
- 视口：单击实际模型替换选择，Ctrl 单击增减，Shift 单击追加；Ctrl+A 全选已加载模型。空白单击或 Esc 清除选择。
- 选中的模型分别高亮，共用一个操作器。G 整体移动，R 整体旋转，R R 轨迹球旋转，S 整体等比缩放；F / 查找模型框定整组选中对象。
- 共同中心取所有选中模型变换后包围范围的中心。旋转和缩放同时更新成员的位置与姿态/大小，保持组内相对关系，而不是各自绕自身中心转。移动/旋转可使用全局轴或当前模型的局部轴。
- 多选缩放只提供等比缩放，避免对不同朝向的模型引入无法由现有 TRS 表达的剪切；工具栏明确显示“等比缩放”。缩放到支持边界时限制共同倍率，不分别截断模型比例。
- Enter / 单击确认；Esc / 右键取消整个操作。一次撤销或重做作用于整组，即使随后切换了选择也能还原原来那组。导入、替换或外部改写变换时清除旧变换历史，防止历史记录误作用于不同数据。
- 多选时禁用点裁剪；需要裁剪时先单选一个模型。属性栏注明选中数量，并明确数值属于当前模型；导出与覆盖仍只针对当前对象，不合并或批量覆盖。

## 保存与兼容

工程增加 `sceneCollection` 字段，保存对象 ID、路径、源单位位移、旋转、缩放及当前对象 ID。原有单场景字段继续反映当前对象；旧工程没有该字段时自动作为单对象打开。请使用支持多对象的新版本继续编辑多对象工程，旧版本不会保留新增的集合字段。

普通保存、另存为、未命名工程恢复和工程快照都包含完整集合。整组变换先验证全部成员，再一次性更新工程，不会只保存部分成员。选中集合与撤销记录属于当前会话，重开工程恢复当前对象而不恢复整组选中状态。托管目录迁移会重定位全部对象路径，外部文件仍按原有规则链接，不会因选择“同时导入”自动复制。清除场景关联只移除当前对象，不删除源文件；其他对象仍引用某个托管数据集时，会阻止删除该数据集。

## 当前边界

- 本次改造针对 PLY 模型对象，不是多套照片数据集同时训练、跨 CRS 重投影或点级跨对象联合裁剪；尚未提供视口矩形框选对象、任意自定义组中心及批量导出。
- 不透明点/网格之间使用共享深度缓冲。高斯模型保留各自内部深度排序，并按对象从后向前混合；彼此穿插的透明高斯仍不具备跨对象、逐高斯的全局透明排序。
- 各对象分别保留现有编辑能力；磁盘分页的大点云和 Mesh 仍按既有规则只读。
- “同时导入”保留原始位置与尺度，不自动拼接、配准或统一不同文件声明的单位。不同坐标系或单位的数据需先对齐。

## 验证

### 隐藏模型时保持观察画布

中文：眼睛按钮只改变模型可见性。工程仍保留模型对象时，即使全部隐藏，渲染／选择／编辑工具栏也保留原有布局占位；不因工具栏折叠改变视口尺寸和投影比例。固定网格以保留对象的原始范围为基准，不因隐藏对象改用可见子集或退回零平面；仍与属性中记录的基准一致。隐藏对象仍不能移动、旋转或缩放，已有操作可用性约束不变。恢复可见性不重新导入、不重设相机，也不修改源文件或模型变换。空工程或移除全部对象属于不同的布局状态。

English: Eye buttons change model visibility only. While imported objects remain in the project, the render/selection/edit toolbar row keeps its layout space even when every model is hidden, so collapsing controls cannot resize the viewport or change its projection aspect. The fixed grid uses retained objects' original bounds, not just visible objects or a zero-plane fallback when hidden, and stays consistent with the recorded reference. Hidden objects remain unavailable for move, rotate and scale; existing action eligibility is unchanged. Revealing an object does not reimport it, reset the camera, or modify its source file or transform. An empty project or removal of all objects is a separate layout state.

日本語：目のボタンはモデルの表示／非表示だけを変更します。プロジェクトにモデルオブジェクトが残っている場合、すべて非表示でも描画・選択・編集ツールバーの領域を維持し、折りたたみによるビューポート寸法や投影比率の変化を防ぎます。固定グリッドは保持中オブジェクトの元の範囲を基準とし、非表示によって可視部分やゼロ平面へ切り替えず、記録された基準との一致を維持します。非表示オブジェクトの移動・回転・拡大縮小は引き続き無効で、既存の操作条件は変えません。再表示で再インポートやカメラのリセットを行わず、元ファイルやモデル変換も変更しません。空のプロジェクトや全オブジェクトの削除は別のレイアウト状態です。

### 混合模型的渲染状态隔离

中文：切换活动模型不再为了空的移除列表切换或释放 OpenGL 上下文。普通点云、网格和 GPU 预览的拾取使用独立帧缓冲，并恢复调用方上下文、表面、读写帧缓冲、纹理／渲染缓冲、像素缓冲和属性 divisor。像素读回使用明确的紧密行布局，深度清除为 1，再恢复调用方的原始设置；输入回调不会为尚未显示的数据申请上传。静态 3DGS／2DGS 的只读几何拾取保持不变，不修改源数据、显示精度或模型变换。后台检查不代替录屏所示前台异常的实际复测。

English: Activating a model no longer changes or releases an OpenGL context for an empty removal list. Point-cloud, mesh and GPU-preview picking uses an isolated framebuffer and restores the caller's context, surface, read/draw framebuffers, texture/renderbuffer, pixel buffers and attribute divisor. Readback uses an explicit tightly packed layout and clear depth 1 before restoring the original caller settings; input callbacks do not admit uploads for undisplayed data. Static 3DGS/2DGS read-only geometry picking remains unchanged, without altering source data, display precision or model transforms. Background acceptance does not replace an interactive recheck of the recorded foreground anomaly.

日本語：アクティブモデルの切り替えでは、削除対象がない場合に OpenGL コンテキストを切り替えたり解放したりしません。点群・メッシュ・GPU プレビューの選択判定は独立したフレームバッファーを使用し、呼び出し元のコンテキスト、サーフェス、読み取り／描画 FBO、テクスチャ／レンダーバッファー、ピクセルバッファー、属性 divisor を復元します。読み出し時には密な行配置と深度クリア値 1 を明示し、終了後に元の設定へ戻します。入力処理で未表示データのアップロードを要求しません。静的な 3DGS／2DGS の読み取り専用幾何判定、元データ、表示精度、モデル変換は変更しません。バックグラウンド検査だけでは、録画の前面表示で発生した異常の再確認を代替できません。

`native_multi_scene_import` 使用真实导入弹窗验证追加、替换和取消；检查同帧中的两个 Mesh、实际几何 Ctrl 点选、工程树 Ctrl/Shift 选择、共同中心旋转/缩放、整组移动/取消/撤销/重做、未选中对象隔离、保存重开、快速替换的异步失效处理，以及采用不同局部偏移的大坐标点云与高斯模型同时显示。`workspace_document` 覆盖多对象保存、另存为、托管迁移和单对象清除，以及批量变换原子提交与无效成员整组拒绝，`recovery_store` 覆盖集合恢复记录。

`native_multi_gaussian_isolation` additionally covers ordinary points, mesh, SH3 3DGS and SH1 two-scale 2DGS with unequal counts, resident/compatibility/mixed renderer configurations and two camera poses. Actual tree and viewport clicks must preserve deliberately non-default caller GL state and each visible source's rendered pixels, transforms and file hash.
