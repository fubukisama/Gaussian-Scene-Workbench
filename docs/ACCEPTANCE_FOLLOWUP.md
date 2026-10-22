# Native acceptance follow-up (0.3.7)

## Windows generated paths

Explicit canonical-camera → solve preparation now uses an exclusively owned,
same-parent short staging directory and short train-image names. Full view IDs,
original image paths, SHA256, and excluded validation/test IDs remain in the
provenance. Existing solve versions retain their original mappings on resume.
Locked-camera inputs never take this solver path implicitly.

The complete known COLMAP layout is planned before cache cleanup, copying or
external execution. It uses short per-attempt directory tokens while retaining
the full task identity, and retries retain previous previews. Generated file and
directory paths are checked in UTF-16 units against conservative legacy Win32
budgets (259/247 excluding NUL). This is an external-tool compatibility boundary,
not general unlimited Windows path support or a registry-policy change. The
snapshot bound follows the actual size_t timestamp format in the supported
[COLMAP source](https://github.com/colmap/colmap/blob/bd1fcf6/src/colmap/controllers/incremental_pipeline.cc#L102-L114).

The final portable solve dataset is separately planned and copied into a
same-volume, identity-owned stage. Image/provenance and actual tree hashes are
verified before publishing. Explicit replacement keeps the previous output as
a backup, with a transaction journal and rollback on publication failures. Old
models and checkpoints are not deleted to make the copy succeed. Backups and
interrupted journals are retained; generic automatic crash/orphan collection is
not implemented. Truly unsupported deep paths fail before costly processing.
Managed projects can be saved to a shorter root; external datasets must instead
be copied to a shorter location and relinked, preserving originals.

## Preset ownership and saved requests

Selecting a quality preset previews its effects; **Apply Quality Preset** is the
explicit action that changes iterations, applicable resolution ratio, and the
backend's preset densification/optimizer profile. Manual values are not reset
by selecting a pending preset, changing backend, language, VRAM protection or
execution settings. Preserve-input-size and locked-camera pixel policy are not
overridden by preset application; locked input still supports explicitly chosen
manual scaling with full-intrinsic adaptation.

Accepted editable requests are saved per project and dataset identity, without
input paths, output names, overwrite authority or camera provenance. Cancelling
the settings dialog does not commit changes. If later environment admission or
launch fails, the already accepted preferences remain available for retry.
When a saved request came from another camera mode, reopening restores its
independent manual parameters but retains the current input's camera/pixel
policy. It never silently selects solve or starts SfM. Invalid requests are
rejected atomically, not clamped into a different experiment.

## Evidence and delivery limits

Focused CPU, actual offscreen Qt controls and real CPU COLMAP file handoff are
separate from GPU convergence, full SfM, physical surface accuracy and the full
cloud suite. Existing unrelated long-running training proofs are not repeated
merely to increase test counts. The C2 5 cm nominal cube remains synthetic;
source-declared units and coordinate round trips are not independent physical
measurement. A distance used to fit scale cannot also be its independent check.

Local acceptance/delivery and final exact-commit GitHub acceptance are separate.
The CI build observer preserves the original command, exit status, 75-minute
deadline and complete tests/scans. It retains only numeric process/object
progress and safe source basenames, not environments or full command lines.
This instrumentation is not a claim that the CodeQL build bottleneck is fixed.

## 简体中文

规范相机显式转为 solve 时采用受保护短名 staging 和短图像名，完整视图身份、
哈希、来源与训练／验证／测试划分仍保留。不会让 locked 输入静默运行 SfM。
COLMAP 与最终工作副本的实际路径在复制、清理旧缓存和启动外部处理前检查。
超出外部工具兼容范围会明确拒绝，不擅改 Windows 策略。输出先复制校验，再
备份旧结果并发布，失败回滚；旧模型与检查点备份不自动删除，强杀后的通用
孤立文件自动清理仍未提供。外部数据路径过长须复制到短目录再重新关联，
仅另存工程并不会搬动外部数据。

选择质量预设只预览，明确“应用质量预设”才改变相应参数。手动值不会因语言、
后端、显存保护或执行设置改变而重置；锁定相机和保留尺寸策略继续生效。
接受的参数按工程和数据身份保存，不复用输入／输出路径或覆盖权限。重开时
恢复独立参数，但不自动采用旧 solve 或更改当前像素策略。取消设置对话框不保存。

本地关键验收后可先交付，全量云端检查独立跟进；短程、坐标往返、5 cm 合成
方块或外观代理均不能证明实物精度。同一距离不能同时用于定尺度与独立检核。
CI 进度采样不减测试或扫描，不延长原期限，也不冒称已经解决编译瓶颈。

## 日本語

正規カメラ入力を明示的にsolveへ渡す場合、所有権を確認した短いstaging名と
学習画像名を使い、完全なview ID・hash・元画像・分割を来歴に保持します。
locked入力を暗黙に再求解しません。COLMAPと最終作業副本の実パスを、旧cache
整理や外部処理の前に検査します。Windows方針は変更せず、未対応の深さは
明確に拒否します。出力は先にコピー・検証し、旧結果をバックアップして
公開、失敗時はrollbackします。モデル・checkpointの旧backupは削除しません。
強制終了後の汎用orphan自動回収は未提供です。外部参照が長い場合は短い場所
へコピーして再関連付けし、工程Save Asだけで解決したとは扱いません。

品質の選択は変更内容のプレビューで、「品質プリセットを適用」だけが対象値を
変更します。手動値は言語・backend・VRAM保護・実行設定の変更で初期化しません。
固定カメラ・サイズ維持の方針を保ち、明示manual縮小は完全Kと連動します。
受理した設定は工程とdataset identityごとに保存し、入力／出力先や上書き権限を
復元しません。再表示時も現在の相機・画素方針を維持し、旧solveを自動選択しません。
設定dialogの取消は保存しません。

ローカル受入れと配布、最終cloud検収を分けます。短程・座標往復・公称5cmの
合成入力・外観代理は実物精度の証明ではありません。尺度fitと独立checkに
同一距離を重複使用しません。CIの観測追加はtest/scan/期限を維持するもので、
ビルド根本原因の修正や速度改善が実証済みという意味ではありません。
