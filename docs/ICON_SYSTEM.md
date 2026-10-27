# Native desktop icon system / 桌面端图标系统 / デスクトップのアイコン体系

## 简体中文

桌面端的应用自有命令使用统一的单色 SVG 图标。移动、旋转、缩放采用明确的三维操作符号；文件、数据集、场景、任务、日志使用不同的语义图标。主工具栏、编辑工具条、视口导航、工程树、任务详情、实验档案、媒体来源列表和常用文件历史共享同一套资源与色彩规则。常用文件历史与媒体来源列表中的图标由应用提供；文件浏览器本身的盘符、系统位置、文件类型图标仍由 Qt／Windows 文件系统提供器负责。

共享图标引擎在普通（Normal）及选中行／菜单（Selected）状态使用当前主题的前景色，在禁用（Disabled）状态使用禁用色。按钮启用选中（On）状态在浅色主题使用强调色，在深色主题使用高对比度前景色；显式强调图标使用强调色。主工具栏的选中工具采用柔和底色，浅色主题表现为浅蓝底与蓝色图标，深色主题保持明亮图标；视口编辑工具条通过单独的自绘逻辑采用蓝底与白色选中图标，两种主题均保持该配对。图标可随深浅主题即时刷新。SVG 按实际显示尺寸和设备像素比绘制；对话框中的图标尺寸同时跟随界面缩放和文字高度。文字说明、快捷键、工具提示和无障碍名称继续使用三语翻译。切换主题、语言或缩放只刷新显示，不改变模型、选择或任务状态。

参考 Blender 的清晰工具符号与悬停提示、CloudCompare 的按用途分组工具栏，以及 Metashape 的观察／模型编辑操作区分。程序使用 Lucide 图标，而没有复制这些软件的图标文件、品牌标识或界面源码。Windows 标题栏的最小化、最大化、关闭按钮保持系统实现；普通表单的文字按钮和 Qt 标准消息图标保留其语义。

## English

Application-owned commands use one monochrome SVG icon family. Move, Rotate and Scale have explicit 3D symbols; files, datasets, scenes, tasks and logs have distinct semantic symbols. The main toolbar, editing strip, viewport navigation, project tree, task details, experiment history, media sources and frequent-file history share the same artwork and colour rules. The application's history and media rows use these icons, while Qt/Windows filesystem providers retain drive, shell-location and file-type artwork inside the file browser.

The shared icon engine uses the theme foreground in Normal and selected row/menu (Selected) states, and the disabled foreground in Disabled state. Checked buttons (On) use the accent colour in the light theme and a high-contrast foreground in the dark theme; explicitly accented icons use the accent colour. Checked main-toolbar tools have a soft background, with blue icons on pale blue in the light theme and bright icons in the dark theme. The separately painted viewport editing strip pairs a blue selected background with white icons in both themes. Appearance changes refresh icons immediately. SVGs render at their requested logical size and device pixel ratio; dialog icon sizes also follow UI scaling and font height. Captions, shortcuts, tooltips and accessible names retain the existing three-language localization. Appearance, language and scale changes update presentation without changing models, selection or task state.

Design references are Blender's tool symbols and hover explanations, CloudCompare's purpose-grouped toolbars and Metashape's separation of navigation from object editing. Lucide supplies the artwork; no Blender, CloudCompare or Metashape icon files, branding or UI implementation are copied. Windows owns minimize/maximize/close caption controls. Ordinary form text buttons and Qt's standard message symbols keep their established meaning.

## 日本語

アプリケーション独自の操作には統一した単色 SVG アイコンを使います。移動・回転・スケールには明確な 3D 操作記号を使い、ファイル・データセット・シーン・タスク・ログには異なる意味の記号を使います。主ツールバー、編集ツールバー、ビューポートのナビゲーション、プロジェクトツリー、タスク詳細、実験履歴、メディアの参照元、よく使うファイルの履歴は同じ図形と配色規則を共有します。履歴とメディアの行はアプリのアイコンを使い、ファイルブラウザー内のドライブ・システムの場所・ファイル種類の図形は Qt／Windows のファイルシステムプロバイダーに従います。

共通のアイコンエンジンは通常（Normal）と行・メニューの選択（Selected）にテーマの前景色を使い、無効（Disabled）には無効時の前景色を使います。チェックされたボタン（On）はライトテーマでアクセント色、ダークテーマで高コントラストの前景色を使い、明示的な強調アイコンはアクセント色を使います。主ツールバーの選択ツールには柔らかな背景色を使い、ライトテーマでは淡い青の背景に青いアイコン、ダークテーマでは明るいアイコンを表示します。ビューポートの編集ツールバーは独自の描画処理で、両テーマとも青い選択背景と白いアイコンを組み合わせます。ライト／ダーク切替で直ちに更新します。SVG は指定した論理サイズとデバイスピクセル比で描画し、ダイアログ内のアイコンサイズは UI スケールと文字の高さにも追従します。ラベル、ショートカット、ツールチップ、アクセシビリティ名は既存の 3 言語翻訳を保持します。テーマ・言語・スケールの変更は表示だけを更新し、モデル・選択・タスク状態を変更しません。

Blender の明確なツール記号とホバー説明、CloudCompare の用途別ツールバー、Metashape のナビゲーションとモデル編集の区別を参考にしています。図形には Lucide を使い、各ソフトウェアのアイコンファイル、ブランド記号、UI 実装はコピーしていません。タイトルバーの最小化・最大化・閉じる操作は Windows の実装を保持します。通常のフォームの文字ボタンと Qt の標準メッセージ記号は既存の意味を保ちます。

## Mapping / 对应关系 / 対応

| Purpose / 用途 / 用途 | Shared identity / 共用标识 / 共通 ID | Lucide asset |
| --- | --- | --- |
| Move / 移动 / 移動 | `Move` | `move-3d` |
| Rotate / 旋转 / 回転 | `Rotate` | `rotate-3d` |
| Scale / 缩放 / スケール | `Scale` | `scaling` |
| Global / 全局 / グローバル | `Global` | `globe` |
| Local / 局部 / ローカル | `Local` | `box` |
| Folder / 文件夹 / フォルダー | `Folder` | `folder` |
| File / 文件 / ファイル | `File` | `file` |
| Copy / 复制 / コピー | `Copy` | `copy` |
| Remove from list / 从列表移除 / リストから除外 | `Remove` | `minus` |
| Delete / 删除 / 削除 | `Delete` | `trash` |
| Full Screen / 全屏 / 全画面 | `FullScreen` | `maximize-2` |
| Exit Full Screen / 退出全屏 / 全画面を終了 | `Restore` | `minimize-2` |

The authoritative full mapping is `native/src/WorkbenchIcons.cpp`.
完整映射以 `native/src/WorkbenchIcons.cpp` 为准。
完全な対応表は `native/src/WorkbenchIcons.cpp` を参照してください。

## Provenance and references / 来源与参考 / 出典と参考

- Artwork: [Lucide icons at pinned commit `70562c1ee1c4fdcf736fe97bc893fb8511927934`](https://github.com/lucide-icons/lucide/tree/70562c1ee1c4fdcf736fe97bc893fb8511927934/icons). Vendored license: `native/third_party/lucide/LICENSE`, ISC plus MIT for Feather-derived icons. Original application code keeps its existing MIT license.
- [Blender 4.5 Tool System](https://docs.blender.org/manual/en/4.5/interface/tool_system.html) and [Viewport Gizmos](https://docs.blender.org/manual/en/4.5/editors/3dview/display/gizmo.html): tool identities, active-tool indication and hover explanations. Blender source licensing remains [GPL](https://www.blender.org/about/license/); no source or artwork was imported.
- [CloudCompare Toolbars and Icons](https://cloudcompare.org/doc/wiki/index.php/Toolbars_and_icons): grouping by purpose and separate view commands. CloudCompare source licensing remains [GPL 2 or later](https://github.com/CloudCompare/CloudCompare/blob/master/license.txt); no source or artwork was imported.
- [Agisoft Metashape 2.3 Standard Manual](https://www.agisoft.com/pdf/metashape_2_3_en.pdf), Appendix A: model-view presentation and navigation tools. The manual is a design reference; proprietary assets and code were not imported.

This icon presentation is shared by point clouds, 3DGS, 2DGS, reconstruction and all supported mesh-generation workflows. It introduces no backend or processing-state change.
该图标显示统一覆盖点云、3DGS、2DGS、重建及所有支持的网格生成流程，不改变后端或处理状态。
このアイコン表示は点群・3DGS・2DGS・再構築・対応するすべてのメッシュ生成で共通し、バックエンドや処理状態を変更しません。

## Quiet list and panel chrome / 列表与面板减线 / リストとパネルの装飾線

普通树、列表及表格项使用明确的透明、无边框样式，避免 Windows 11 原生样式在模型名称与“眼睛”等列之间自动绘制贯穿空单元格的竖线。选中与悬停仍有独立反馈，交替行底色保留；真正的数据表格格线由各表自身控制，不随装饰线一起关闭。标签页用现有选中强调及底色区分，不再增加竖隔栏；属性面板分节沿用字重与留白，不重复画下划线。输入框、键盘焦点、弹出层边界和真实可拖动分隔区保持原规则。三语即时切换只刷新显示，不改变选择、可见性、相机或任务。

Normal tree, list and table items explicitly use transparent, borderless chrome so the Windows 11 native style cannot add column rules through empty cells, including the model-name/eye boundary. Selection and hover feedback and alternating row surfaces remain. Real table grids remain controlled by each table, independently of decorative item rules. Tabs retain the existing active accent and surfaces without extra vertical partitions; inspector headings use their existing weight and spacing without duplicate underlines. Input boundaries, keyboard focus, popup edges and real draggable separators retain their contracts. Immediate three-language switching changes presentation only, not selection, visibility, cameras or jobs.

通常のツリー・リスト・表の項目には透明で枠のない装飾を明示し、Windows 11 の標準スタイルがモデル名と可視性アイコンの間など、空のセルにも縦線を描かないようにします。選択・ホバーの表示と交互の行背景は維持し、実際の表の罫線は各表で独立に制御します。タブは既存の選択アクセントと背景で区別し、余分な縦区切りを付けません。プロパティーの見出しは既存の字重と余白を使い、下線を重ねません。入力欄、キーボードフォーカス、ポップアップの境界、実際にドラッグできる区切りは維持します。三言語の即時切替は表示だけを更新し、選択・可視性・カメラ・タスクは変更しません。
