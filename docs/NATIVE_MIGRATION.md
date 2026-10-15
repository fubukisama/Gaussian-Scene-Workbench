# Native backend migration / 原生后端迁移 / ネイティブバックエンドへの移行

Updated: 2026-10-15. The default `main` branch now contains the native desktop application; it is the sole active development and installation entry point.

## English

The shared Python workflow code formerly located under `crop_editor/` now lives
in `native/backend/compute.py`. Dataset import, COLMAP reconstruction, 3DGS/2DGS,
mesh generation, texturing, path validation and job control remain compute
functions invoked by supervised native workers. The module has no HTTP server
or browser startup entry point.

The retired interface has been removed from this branch: HTML/CSS/JavaScript
assets, browser-only tests and package manifests, the Electron shell, local HTTP
routes and old desktop launch/release scripts. Optional Node-based conversion is
not part of the native package; native SPZ uses the compiled C++ codec. Removing
the interface does not add unsupported native features such as SOG export.

The current structure is:

```text
native/src/                 Qt UI, viewport and native process supervision
native/worker/              Worker entry points and training-state adapters
native/backend/compute.py   Shared dataset/reconstruction/training/mesh services
native/backend/tests/       Direct backend and security regression tests
gaussian-splatting/         Separate research training implementation
training_kit/               Compute helpers
scripts/                   Native build, runtime and validation tools
```

The worker and packaged backend must come from the same revision. Source-tree
development may explicitly select a trusted checkout with `GSW_BACKEND_ROOT`;
an arbitrary current directory is not an implicit backend. A package includes
the backend it uses, the corresponding native source and the applicable licenses.

Existing native `.gsw.json` projects, model files and full-state checkpoints are
data, not retired application code. Their format and trust checks remain in
place. Existing external runtime environment variables remain the configuration
route described in [runtime setup](WINDOWS_NATIVE_RUNTIME.md); this cleanup does
not automatically convert an Electron runtime JSON file into native settings.
Back up and retain earlier projects and outputs when switching installations.
Native checkpoint compatibility remains subject to backend, input and settings
validation; a PLY warm start is not a full optimizer-state resume.

Older application versions remain available in Git history and existing tags.
This branch cleanup does not rewrite history or remove research assets. It also
does not establish complete old-interface feature parity: use the current
[native matrix](NATIVE_PARITY.md), [pipeline gates](GENERATION_PIPELINES.md) and
[training limits](TRAINING_PERFORMANCE.md).

Validation covers the migrated direct backend, native workers, three-language
desktop behavior and a staged native package without the retired folders.
Real CUDA and optional mesh-runtime checks remain separate capability gates;
the source relocation alone is not proof of their availability on every PC.

## 简体中文

默认 `main` 分支现为原生桌面端，是唯一活跃开发与安装入口。原 `crop_editor/` 中的共享计算逻辑迁入 `native/backend/compute.py`，保留数据集
导入、COLMAP、3DGS/2DGS、网格、纹理、路径校验和任务控制，由原生 worker 直接
调用。模块不再提供 HTTP 服务或浏览器启动入口。

本分支移除旧 HTML/CSS/JavaScript、浏览器专用测试与依赖清单、Electron 外壳、
HTTP 路由及旧启动/发行脚本。原生 SPZ 使用 C++ 编解码器，不依赖旧 Node 转换器。
删除旧界面不代表补齐 SOG 等尚未支持的原生能力。

worker 与安装包后端应来自同一提交。源码开发可显式通过 `GSW_BACKEND_ROOT`
选择可信仓库，不把当前工作目录自动当作后端。安装包保留匹配的后端、原生对应
源码和许可证。上方目录表列出新的职责边界。

已有原生 `.gsw.json`、模型和完整状态检查点属于数据，格式与信任检查保持。
独立运行环境继续使用[环境说明](WINDOWS_NATIVE_RUNTIME.md)中的变量；本轮不会
把 Electron 的运行环境 JSON 自动转换为原生设置。切换安装版时保留并备份已有
工程与成果；续训仍验证后端、输入和参数，PLY 热启动不等于完整优化器状态恢复。

历史版本保留在 Git 历史和现有标签中，本次不改写历史、不删除研究资产。
旧界面退役不等于所有功能已移植，能力以[原生矩阵](NATIVE_PARITY.md)、
[管线验收](GENERATION_PIPELINES.md)和[训练限制](TRAINING_PERFORMANCE.md)为准。
验收覆盖迁移后的直接后端、原生 worker、三语言界面及无旧目录的安装包；
真实 CUDA 与可选网格运行库仍分别验证。

## 日本語

既定の `main` はネイティブデスクトップ版となり、唯一の開発・導入入口です。従来の `crop_editor/` 内にあった共通処理は `native/backend/compute.py` に移しました。
データセット取込、COLMAP、3DGS/2DGS、メッシュ、テクスチャ、パス検証、タスク
制御をネイティブ worker が直接呼び出します。HTTP サーバーやブラウザーの
起動入口は提供しません。

この分岐から旧 HTML/CSS/JavaScript、ブラウザー専用テストと依存一覧、Electron、
HTTP ルート、旧起動・配布スクリプトを削除します。ネイティブ SPZ は C++ の
コーデックを使用し、旧 Node 変換器に依存しません。旧 UI の削除で SOG などの
未対応機能が追加されるわけではありません。

worker と同梱バックエンドは同じコミットを使います。ソース開発では
`GSW_BACKEND_ROOT` で信頼するリポジトリを明示でき、作業ディレクトリを
暗黙のバックエンドとして扱いません。パッケージは対応するバックエンド・
ネイティブソース・ライセンスを保持します。役割ごとの配置は上記の表の通りです。

既存のネイティブ `.gsw.json`、モデル、完全状態チェックポイントはデータであり、
形式と信頼性の検証を維持します。外部環境は引き続き[実行環境](WINDOWS_NATIVE_RUNTIME.md)
にある変数で指定し、Electron のランタイム JSON を自動でネイティブ設定へ変換
する機能は追加しません。切替時は既存のプロジェクトと成果を保管・バックアップ
してください。再開には方式・入力・設定の検証が必要で、PLY ウォームスタートは
完全な最適化器状態の再開ではありません。

過去の版は Git 履歴と既存タグに残し、研究資産は削除しません。旧 UI の廃止は
全機能移植済みという意味ではなく、[ネイティブ機能表](NATIVE_PARITY.md)、
[パイプライン検証](GENERATION_PIPELINES.md)、[学習の制限](TRAINING_PERFORMANCE.md)
が対応範囲を示します。移行後の直接バックエンド、worker、三言語 UI、旧ディレクトリ
のないパッケージを検査し、実 CUDA と追加メッシュ環境は別途検証します。
