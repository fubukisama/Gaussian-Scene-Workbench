# Gaussian Scene Workbench

**Gaussian Scene Workbench** is a Windows desktop research workbench for Gaussian Splatting, photogrammetry, point clouds and meshes.

中文：**高斯场景研究工作台** · 日本語：**ガウスシーン研究ワークベンチ**

[English](#native-desktop-preview) · [简体中文](#简体中文) · [日本語](#日本語)

## Native Desktop Preview

- Version: `0.3.1-native-preview`
- Documentation updated: `2026-10-14` — not a new release-version announcement.
- Active branch: [`agent/native-desktop-0.3`](https://github.com/fubukisama/Gaussian-Scene-Workbench/tree/agent/native-desktop-0.3)
- Interface: Qt 6.8 / C++20 / native OpenGL. The desktop UI does not run HTML, Electron, Node.js, a browser engine or a local web server.

This README describes the **native desktop branch**, not the legacy HTML/Electron application. Native development remains a preview; a workflow entry does not imply complete legacy feature parity.

### Current capabilities

- **Projects and media:** untitled projects, photo/folder/video import, external dataset links, portable `.gsw.json` projects and managed data, Save As/recovery, and separate dataset/reconstruction/scene/task-history cleanup.
- **Reconstruction and training:** guarded COLMAP reconstruction, 3DGS/2DGS training, input/runtime preflight, reconstruction reuse explanations, progress, logs and cancellation. CUDA training needs a compatible external environment.
- **Safe pause/resume:** compatible native 3DGS/2DGS jobs save full optimizer state at an iteration boundary. Resume validates inputs, settings and checkpoint contents and requires explicit trust confirmation. PLY previews and warm starts are not full-state resume.
- **Training performance and quality:** bounded exact-pixel image staging, VRAM-aware growth admission and quality-first full-state pause, optional numerically verified MIT fused L1/SSIM, and truthful whole-step/stage/memory monitoring. Experimental Faster-GS is capability-gated, not a replacement for default densification or 2DGS. See[training performance](docs/TRAINING_PERFORMANCE.md).
- **Independent experiments:** new jobs preserve existing models and transforms, use a separate task-preview layer, and retain individual settings, results and available resume entries. Pause Preview freezes observation, not training. Reference-model display caches can yield resources without deleting project objects.
- **Native visualization:** Gaussian, 2DGS surfel, point-cloud and triangle-mesh display, Gaussian SH controls, camera trajectories, perspective/orthographic navigation and an adaptive infinite grid. FPS measures completed viewport-frame cadence with 60-frame smoothing, not GPU execution time or the inverse of CPU submission time.
- **Shared coordinates and editing:** shared scene units/reference, preserved source coordinates, reversible display shifts, coordinate reports, multi-object selection, Move/Rotate/Scale/Transform, Global/Local orientation, numeric input, snapping and undo/redo. The fixed source-base grid does not lock models to the plane; independent reconstructions are not automatically registered.
- **Large models and export:** complete mesh loading when estimated RAM/GPU costs fit, disk-backed paging otherwise, live automatic/aggressive or manual budgets, textured PLY display, full-source point/Gaussian selection and cropping where supported, and PLY/SPZ plus mesh export subject to format-specific limits.
- **Desktop UI:** immediate Simplified Chinese/English/Japanese switching, light/dark and local-time automatic appearance, 90–150% scaling, unified vector icons, responsive edit toolbars, remembered file/folder history, persistent panels and save/exit confirmation with worker cleanup.

Mesh-generation routes include bounded/unbounded TSDF, SuGaR and GS2Mesh, with optional OpenMVS photo texturing. Runtimes, weights and verification gates are backend-specific; mesh stages do **not** provide full optimizer-state resume. See the [pipeline matrix](docs/GENERATION_PIPELINES.md) and [mesh guide](docs/MESH_GENERATION.md).

### Download and run on Windows

1. In the [Native Windows workflow](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml), select a **successful run for this branch and the desired commit**, then download its artifact. Artifacts expire; a newer source commit does not guarantee a ready binary. Alternatively, build from source below.
2. Extract the **complete package** to a writable directory and launch `bin/Gaussian Scene Workbench.exe`. Keep the Qt DLLs, plugins and packaged backend files; do not copy only the EXE.
3. Verify the external runtime before reconstruction/training, then add media or import PLY/SPZ. See [Windows runtime setup](docs/WINDOWS_NATIVE_RUNTIME.md) and [COLMAP setup](docs/COLMAP_SETUP.md).

Prefer a non-system drive when available, such as `D:\Apps\GSW`. C:-only devices automatically use the existing writable workspace/build/package location. Windows Application Control may require approved signed binaries/runtimes; the app does not disable or bypass that policy.

The old Electron `0.2.4` release and `Setup Gaussian Scene Workbench.cmd` are **not** the native preview installation instructions.

### Build the native desktop

From this branch's checkout, with the dependencies in [native/README.md](native/README.md):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 -Configuration Release -Package
```

The default runnable directory is `native/dist/Gaussian-Scene-Workbench-0.3.1-native-preview-win-x64/`, with the executable under `bin/`. Use `-BuildDirectory` and `-PackageDirectoryName` for custom or side-by-side locations. Save and exit normally before switching an active installation/desktop shortcut. CUDA/PyTorch training dependencies are separate from the Qt desktop build.

Translation and native regression commands:

```powershell
python -B scripts/native_i18n.py
python -B scripts/test_native_i18n.py
ctest --test-dir native/build -C Release --output-on-failure
```

Replace `native/build` for custom builds. GUI tests require the configured Qt/OpenGL runtime; optional CUDA integration gates are documented separately.

### Important boundaries

- The OpenGL preview is not the upstream CUDA tile renderer. GPU timing queries and 2DGS shared-GPU-memory live preview remain separate limitations; checkpoint-based previews are available where supported.
- Mesh budgets are estimates, not a hard quota for all Gaussian buffers, other processes or training allocations. Geometry paging does not page texture atlases.
- Full-state resume applies to compatible native 3DGS/2DGS checkpoints, not arbitrary external PTH files or mesh stages. Experiment metadata persists; task logs and historical metric curves remain session-only.
- Do not assume legacy PSNR/evaluation, asset-management or all HTML features are implemented in the native interface.

### Documentation and source layout

- [Native build and implementation](native/README.md)
- [Windows runtimes](docs/WINDOWS_NATIVE_RUNTIME.md) · [App Control/signing](docs/WINDOWS_APPLICATION_CONTROL.md)
- [Generation pipelines](docs/GENERATION_PIPELINES.md) · [Training resume and experiment history](docs/TRAINING_RESUME.md)
- [Scene coordinates](docs/SCENE_COORDINATES.md) · [Multiple models](docs/MULTI_SCENE_IMPORT.md)
- [Resource budgets](docs/RESOURCE_BUDGETS.md) · [Mesh generation](docs/MESH_GENERATION.md)
- [Icon system](docs/ICON_SYSTEM.md) · [Localization](docs/LOCALIZATION.md)
- [Migration](docs/NATIVE_MIGRATION.md) · [Parity gates](docs/NATIVE_PARITY.md)

`native/` contains the desktop UI, OpenGL renderer, workers and tests; `docs/` and `scripts/` contain documentation, build/runtime and validation tools; `training_kit/` and `gaussian-splatting/` contain compute helpers and training sources. `crop_editor/` and `resources/app/` retain legacy web/Electron code and shared backend helpers, not a browser dependency of the native UI.

Generated datasets, outputs, environments, caches, builds and packages are excluded from Git. Legacy interface development belongs to [`main`](https://github.com/fubukisama/Gaussian-Scene-Workbench/tree/main), not this desktop branch.

### License and intended use

The original native desktop main program is **GPL-3.0-or-later** from the 2026-10-14 licensing change; its scope and exceptions are in [native/LICENSE.md](native/LICENSE.md), with the complete GPL text in [COPYING](COPYING). The Blender v4.5.0 cone/cube geometry port retains upstream notices; existing GSW drag, constraint and undo logic is retained rather than importing Blender's complete operator system. Earlier MIT grants and original code outside this scope remain under [LICENSE](LICENSE). Third-party training, CUDA, codec, Qt and icon components retain their own licenses. Graphdeco research components still have non-commercial research/evaluation restrictions unless separate permission is obtained. The GPL main-program grant does not grant commercial rights to every backend. See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## 简体中文

**高斯场景研究工作台**在 `agent/native-desktop-0.3` 分支开发原生桌面预览版，版本为 `0.3.1-native-preview`。本页文档更新日期为 **2026-10-14**，不代表另行发布新版本。Qt 6.8/C++20/OpenGL 界面不运行 HTML、Electron、Node.js、浏览器引擎或本地网页服务器。

### 当前功能

- **工程与素材：**未命名工程，照片/文件夹/视频导入，外部数据集关联，保存/另存为、恢复，以及数据集、重建、场景、任务历史的分别清理。
- **重建与训练：**COLMAP、3DGS/2DGS、数据与环境预检、复用重建原因、进度、日志和取消。CUDA 训练需要兼容的独立运行环境。
- **训练性能与质量：**有上限的无损图像缓存与传输、显存增密保护、质量优先的完整状态暂停、经过数值验证的可选MIT联合L1/SSIM，以及整步/阶段/显存监测。Fast-GS实验路径按能力回退，不代替默认增密或2DGS。见[训练性能](docs/TRAINING_PERFORMANCE.md)。
- **完整进度保存：**原生 3DGS/2DGS 在安全迭代边界保存完整状态并暂停，校验输入、参数和可信检查点后续训；PLY 预览/热启动不等于完整优化器状态恢复。
- **独立实验：**新任务保留原模型及变换，使用独立预览，分别记录参数、成果和续训入口。“暂停预览”不暂停训练；可让出参考模型显示缓存而不删除对象。
- **可视化：**高斯、2DGS surfel、点云、纹理网格、球谐、相机轨迹、透视/正交导航及无限自适应网格。FPS 是实际完成的视口帧节奏，采用 60 帧平滑，不是 GPU 执行耗时或 CPU 提交耗时的倒数。
- **坐标与编辑：**共享场景单位/参考系、源坐标、可逆显示偏移、坐标报告、多对象选择、移动/旋转/缩放/组合工具、全局/局部方向、数字输入、吸附、撤销重做。固定基准面不限制 Z 移动，也不自动配准独立重建模型。
- **大模型与导出：**网格按实际内存/显存余量自适应加载，预算足够时完整常驻、不足时分页；支持即时激进自动/手动预算、纹理 PLY、受支持数据的完整源点选择/裁剪、PLY/SPZ 和网格导出。
- **桌面界面：**中英日即时切换，深浅色与本地时间自动切换，90–150% 缩放，统一矢量图标，小视口工具栏适配，常用文件/文件夹记忆，持久面板及保存/退出确认。

网格生成含有界/无界 TSDF、SuGaR、GS2Mesh 和可选 OpenMVS 照片纹理阶段。环境、权重及验收状态各不相同，网格阶段没有完整优化器续训；见[管线矩阵](docs/GENERATION_PIPELINES.md)和[网格指南](docs/MESH_GENERATION.md)。

### 下载、安装与构建

1. 在 [Native Windows workflow](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml) 选择对应分支/提交的**成功运行**并下载 artifact；有保存时限，并非每个新提交都有可用安装包。
2. 完整解压后运行 `bin/Gaussian Scene Workbench.exe`；保留 Qt DLL、插件和后端文件，不要只复制 EXE。
3. 重建/训练前按[运行环境](docs/WINDOWS_NATIVE_RUNTIME.md)与[COLMAP](docs/COLMAP_SETUP.md)说明验证环境；已有 PLY/SPZ 模型可直接导入。

有非系统盘时优先采用，例如 `D:\Apps\GSW`；只有 C: 时自动使用现有可写目录，不因缺少其他盘符而阻塞。企业应用控制需要获准的签名/运行库，软件不绕过安全策略。旧 Electron `0.2.4` 安装器不是原生版的安装方式。

源码构建使用上方 `scripts\build_native.ps1 -Configuration Release -Package`，默认输出到 `native/dist/Gaussian-Scene-Workbench-0.3.1-native-preview-win-x64/`；完整依赖与验证见 [native/README.md](native/README.md)。

### 能力边界与许可

当前仍是预览版，不把旧 HTML 的 PSNR、通用资产管理等功能视为已迁移。OpenGL 预览不等于上游 CUDA 分块渲染器；网格预算不是全程序/训练的硬配额，纹理未分页；2DGS 共享 GPU 实时预览仍有独立限制。实验记录可持久保存，任务日志和历史曲线仍仅保留当前会话。

自 2026-10-14 本次许可变更起，原生桌面主程序的原创代码采用 **GPL-3.0-or-later**；范围及例外见 [native/LICENSE.md](native/LICENSE.md)，完整 GPL 文本见 [COPYING](COPYING)。实际移植 Blender v4.5.0 的圆锥/立方体几何并保留上游声明，但不是复制完整 Blender 操作器系统；拖动、约束和撤销仍由 GSW 处理。此前授予的 MIT 权利及此范围以外的原创代码保留 [LICENSE](LICENSE) 条款。第三方训练、CUDA、编解码、Qt、图标仍遵循各自许可；Graphdeco 相关组件仍有非商业研究/评估限制，不因主程序 GPL 而获得商用权。见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。

## 日本語

**ガウスシーン研究ワークベンチ**のネイティブ版は `agent/native-desktop-0.3` で開発中です。バージョンは `0.3.1-native-preview`、本ページの文書更新日は **2026-10-14** です。新しい製品バージョンの公開日を意味しません。Qt 6.8/C++20/OpenGL を使い、HTML、Electron、Node.js、ブラウザーエンジン、ローカル Web サーバーを実行しません。

### 現在の機能

- **プロジェクトと素材：**未保存プロジェクト、写真/フォルダー/動画追加、外部データセット、保存/名前を付けて保存、復元、データセット・再構築・シーン・履歴の個別整理。
- **再構築と学習：**COLMAP、3DGS/2DGS、入力と環境の事前確認、再構築を再利用する理由、進捗、ログ、キャンセル。CUDA 学習には互換性のある外部環境が必要です。
- **学習の性能と品質：**上限付き可逆画像転送、GPUメモリ増密保護、品質優先の完全状態一時停止、数値検証済みの任意MIT融合L1/SSIM、全体/区間/メモリ測定。実験的Fast-GSは互換性で判定し、既定の増密や2DGSを置き換えません。[学習の性能](docs/TRAINING_PERFORMANCE.md)を参照してください。
- **完全な状態からの再開：**ネイティブ 3DGS/2DGS は安全な反復境界で状態を保存して一時停止し、入力・設定・信頼できるチェックポイントを検証して再開します。PLY プレビューやウォームスタートは完全な最適化器状態の復元ではありません。
- **実験の分離：**既存モデル/変換と独立したタスクプレビューを保持し、設定・成果・再開入口を個別に保存します。「プレビューを一時停止」は学習を止めません。表示キャッシュを解放しても参照モデルを削除しません。
- **表示：**ガウシアン、2DGS surfel、点群、テクスチャ付きメッシュ、SH、カメラ軌跡、透視/平行投影、無限適応グリッド。FPS は実際に完了したフレームの間隔を 60 フレームで平滑化した値で、GPU 実行時間や CPU 送信時間の逆数ではありません。
- **座標と編集：**共通単位/基準、元の座標、可逆な表示シフト、座標レポート、複数選択、移動/回転/スケール/複合ツール、グローバル/ローカル、数値入力、スナップ、取り消し/やり直し。固定基準面は Z 移動を拘束せず、別々に再構築したモデルの位置合わせを自動では行いません。
- **大規模モデルと出力：**実際の RAM/GPU メモリ余量に応じ、収まるメッシュは完全常駐、収まらない場合はページングします。積極的な自動/手動予算、テクスチャ付き PLY、対応データの全ソース点選択/クロップ、PLY/SPZ とメッシュ出力を備えます。
- **UI：**中英日の即時切替、ライト/ダークと現地時刻による自動切替、90–150% スケール、統一ベクターアイコン、小さなビューのツールバー、よく使うファイル/フォルダー履歴、パネル状態、保存/終了確認。

メッシュ生成は有界/無界 TSDF、SuGaR、GS2Mesh と任意の OpenMVS 写真テクスチャ工程を含みます。環境・重み・検証状況は方式ごとに異なり、完全な最適化器状態からの再開には対応しません。[パイプライン表](docs/GENERATION_PIPELINES.md)と[メッシュガイド](docs/MESH_GENERATION.md)を参照してください。

### 入手・起動・ビルド

1. [Native Windows workflow](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml) で対象ブランチ/コミットの**成功した実行**を選び artifact を取得します。保存期限があり、最新ソースのバイナリーが常に存在するとは限りません。
2. 全体を展開し `bin/Gaussian Scene Workbench.exe` を実行します。Qt DLL、プラグイン、バックエンドを保持し、EXE だけをコピーしないでください。
3. 学習/再構築前に[実行環境](docs/WINDOWS_NATIVE_RUNTIME.md)と[COLMAP](docs/COLMAP_SETUP.md)を確認します。既存 PLY/SPZ は直接読み込めます。

別ドライブが使える場合は `D:\Apps\GSW` などを優先し、C: のみでは既存の書き込み可能な場所を使います。別ドライブがないだけでは処理を止めません。Windows Application Control には承認された署名/ランタイムが必要で、ポリシーを回避しません。旧 Electron `0.2.4` の導入手順とは別です。

ビルドは上記 `scripts\build_native.ps1 -Configuration Release -Package`、既定の出力先は `native/dist/Gaussian-Scene-Workbench-0.3.1-native-preview-win-x64/` です。依存関係と検証は [native/README.md](native/README.md) を参照してください。

### 制限とライセンス

本版はプレビューで、旧 HTML 版の PSNR、汎用アセット管理などが移植済みとは限りません。OpenGL は上流 CUDA タイルレンダラーと同一ではなく、メッシュ予算は全体/学習の強制的な上限ではありません。テクスチャはページングせず、2DGS の共有 GPU ライブプレビューにも制限があります。実験記録は保存されますが、ログと過去の指標曲線はセッション内のみです。

2026-10-14 の本変更から、独自のネイティブ主プログラムは **GPL-3.0-or-later** です。範囲と例外は [native/LICENSE.md](native/LICENSE.md)、全 GPL 本文は [COPYING](COPYING) にあります。Blender v4.5.0 の円錐/立方体形状を実際に移植して元の通知を保持しますが、操作器システム全体の移植ではありません。ドラッグ・制約・取り消しは GSW が処理します。過去の MIT の権利と対象外の独自コードは [LICENSE](LICENSE) の条件を維持します。学習・CUDA・コーデック・Qt・アイコンは各ライセンスに従い、Graphdeco 関連部の非商用研究/評価制限も維持します。GPL 主プログラムは全バックエンドの商用許諾を意味しません。[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) を参照してください。
