# Manual viewport frame diagnostics / 手动视口抓帧 / 手動ビューポートフレーム診断

## 简体中文（zh_CN）

这是针对真实显示异常的诊断工具，不是已完成的修复，也不改变默认桌面快捷方式。
只有诊断启动器设置了进程环境变量 `GSW_RENDER_DIAGNOSTICS_DIR` 时，视口内的
`F12` 才启用抓帧。诊断共用点云、网格、3DGS 和 2DGS 的视口路径，不修改模型
或训练状态；不同模型类型只记录其实际经过的绘制阶段。

1. 保存当前工程，然后自行关闭正常版程序；启动器不会关闭已有窗口。
2. 使用下方原生启动器的 `--diagnostics` 参数。不要用正常版
   桌面快捷方式代替诊断启动器。模型与抓帧留在本地，不会上传。
3. 用相同文件、相同顺序导入并重现异常。也可打开保存的工程；若重开工程没有
   复现，请恢复原来的逐个导入步骤，而不是把正常画面当作复现成功。
4. 让程序保持前台，视口完全位于同一显示器内、无遮挡且未最小化。出现块状缺失
   或乱码后停止移动鼠标，在已有焦点的视口内按一次 `F12`，等待文件保存。
   如果视口尚未获得焦点，请先完成聚焦，再重新重现异常；不要为聚焦而点击
   已经出现异常的模型。抓帧可能短暂变慢，不要连续按键。
5. 告诉协作者抓帧目录中最新 `capture-*` 文件夹的 `manifest.json` 路径，
   以及异常发生前的操作。先提供路径即可，不必上传模型或图片。

抓帧顺序很重要：`presented-before.png` 先保存屏幕上实际看到的视口像素，之后
才读取已存在的视口帧缓冲到 `existing-before.png`，这一步不主动重绘。然后才
采集下一次正常绘制的清屏、网格、各模型、包围框和叠加绘制等阶段，最后保存
`presented-after.png`。下一帧可能恢复正常，不能拿它替代第一张故障画面。
请查看清单中的帧序号匹配、有效性、跳过和错误字段；存在文件不等于采集完整，
远端诊断设施检查通过也不代表此显示异常已复现或修复。

捕获文件可能包含模型画面、原始文件名、绝对路径及相机／绘制状态。它们只保存在
指定目录，启动器不自动打开或上传任何数据，也不更改持久环境变量或程序设置。
新版使用[托管安装目录](NATIVE_UPDATES.md)内的 `diagnostics/`（本机 E:），拒绝
穿过链接或目录联接的抓帧目录。请先自行保存并关闭已有实例；原生启动器不会结束
已有程序。结束后正常关闭诊断版；普通入口仍使用当前版本，但不启用 `F12` 诊断。

## English (en_US)

This is evidence collection for a real display defect, not a completed fix. The normal
desktop shortcut is unchanged. Viewport `F12` capture is enabled only when the manual
launcher supplies the process variable `GSW_RENDER_DIAGNOSTICS_DIR`. The shared
viewport path covers point clouds, meshes, 3DGS and 2DGS without changing model or
training state; each source records only the draw stages it actually uses.

1. Save the project, then manually close the normal application. The launcher never
   closes an existing window.
2. Use the native launcher's `--diagnostics` option below. Do not substitute
   the normal desktop shortcut. Models and captures remain local and are not uploaded.
3. Import the same files in the same order and reproduce the defect. Opening the saved
   project is also possible; if that does not reproduce it, repeat the original sequential
   import steps. A healthy image is not a successful reproduction.
4. Keep the application active, with the unobstructed, non-minimized viewport entirely
   on one monitor. When the missing blocks or corrupted image appears, stop moving the
   mouse, press `F12` once in the already-focused viewport, and wait for saving. If focus
   is elsewhere, focus first and then reproduce again; do not click the already-corrupted
   model just to focus. Capture can briefly slow rendering; do not press repeatedly.
5. Report the `manifest.json` path in the newest `capture-*` subdirectory and the actions
   immediately preceding the defect. A local path is enough; do not upload the assets.

Order matters: `presented-before.png` saves actual on-screen viewport pixels first.
Only then is the existing viewport framebuffer read into `existing-before.png`, without
requesting a new render. The next normal paint records clear, grid, individual models,
bounds and overlay stages, followed by `presented-after.png`. That later frame may
be healthy and must never replace the first defective image. Inspect the manifest's
serial-match, validity, skipped-stage and error fields. Files alone do not establish a
complete capture, and a remote capture-infrastructure pass does not prove that the
reported visual defect was reproduced or fixed.

Evidence can contain model pixels, original filenames, absolute paths and camera/draw
state. It is saved only under the chosen local directory; the launcher does not open or
upload it, or change persistent environment/settings. Prefer a writable non-system drive
(E: on this machine); a system-drive-only machine remains supported without additional
confirmation. The native launcher uses `diagnostics/` within the
[managed installation](NATIVE_UPDATES.md), rejecting links/junctions there. Save and manually
close existing instances first; the native launcher does not terminate them. Close the
diagnostic application normally when finished. Ordinary launch uses the current release
without enabling the F12 diagnostic.

## 日本語（ja_JP）

これは実際の表示不具合の証拠を収集する診断機能であり、修正完了版ではありません。
通常のデスクトップショートカットは変更しません。手動ランチャーがプロセス環境変数
`GSW_RENDER_DIAGNOSTICS_DIR` を指定した場合だけ、ビューポートの `F12` 収集が
有効になります。点群・メッシュ・3DGS・2DGS の共通ビューポートを対象とし、モデルや
学習状態は変更しません。各モデルでは実際に通過した描画段階だけを記録します。

1. プロジェクトを保存し、通常版を自分で終了してください。ランチャーが既存の
   ウィンドウを閉じることはありません。
2. 下記ネイティブランチャーの `--diagnostics` オプションを使用します。
   通常版のショートカットで代用しないでください。モデルと記録はローカルに保持し、
   アップロードしません。
3. 同じファイルを同じ順序でインポートし、不具合を再現します。保存済みプロジェクトを
   開く方法も利用できますが、再現しなければ元の順次インポート操作を繰り返してください。
   正常な画面は再現成功とは扱いません。
4. アプリを前面に保ち、ビューポート全体を一つのモニター内に収め、他のウィンドウで
   隠したり最小化したりしないでください。欠落や表示の乱れが出たらマウスを止め、
   既にフォーカスのあるビューポートで `F12` を一度押して保存を待ちます。フォーカスが
   別の場所にある場合は、先に合わせてから再現し直し、乱れたモデルをフォーカスのために
   クリックしないでください。一時的に描画が遅くなる場合があるため、連打しないでください。
5. 最新の `capture-*` フォルダー内の `manifest.json` のパスと、不具合直前の操作を
   協作者に伝えてください。ローカルパスだけでよく、モデルや画像のアップロードは不要です。

収集順序を区別してください。最初に `presented-before.png` へ実際の画面上の画素を
保存し、その後、再描画を要求せず既存のフレームバッファーを `existing-before.png` に
読み出します。続く通常の描画で、クリア、グリッド、各モデル、バウンディングボックス、
オーバーレイなどの段階を収集し、最後に `presented-after.png` を保存します。次の
フレームが正常に戻っても、最初の不具合画像の代わりにはできません。マニフェストの
フレーム番号の一致、有効性、省略、エラーを確認してください。ファイルの存在だけでは
完全な収集を意味せず、リモートの診断基盤テスト合格も本不具合の再現・修正の証明には
なりません。

記録にはモデルの画面、元のファイル名、絶対パス、カメラ・描画状態が含まれる場合が
あります。指定したローカルフォルダーにのみ保存し、自動表示・アップロードや永続的な
環境変数・設定の変更は行いません。書込み可能な非システムドライブ（本機では E:）を
優先し、システムドライブしかない場合は追加確認なしで利用できます。ネイティブ版は
[管理インストール先](NATIVE_UPDATES.md)内の `diagnostics/` を使用し、リンク・
ジャンクションを経由する記録先を拒否します。既存のアプリは事前に保存し、自分で
閉じてください。ランチャーは強制終了しません。診断版も通常どおり自分で閉じます。
通常起動は現行版を使用し、F12 診断を有効にしません。

## Manual command / 手动命令 / 手動コマンド

Use the verified managed native launcher. / 使用已验证的托管原生启动器。 /
検証済みの管理ネイティブランチャーを使用してください。

```powershell
& 'E:\Apps\Gaussian Scene Workbench\bin\Gaussian Scene Workbench Launcher.exe' --diagnostics
```

The old PowerShell script is retained only as a developer/manual legacy CLI. Do not
generate desktop links using hidden PowerShell or `ExecutionPolicy Bypass`. Native
application language selection continues to apply immediately. Raw legacy CLI output
remains English as documented in [LOCALIZATION.md](LOCALIZATION.md).

旧 PowerShell 脚本仅作为开发／手动兼容 CLI 保留，不再创建隐藏 PowerShell 或
`ExecutionPolicy Bypass` 的桌面链接。原始 CLI 输出按 [LOCALIZATION.md](LOCALIZATION.md)
保留英文，原生语言选择仍即时生效。

旧 PowerShell スクリプトは開発・手動互換 CLI としてのみ保持します。非表示の
PowerShell や `ExecutionPolicy Bypass` を使うデスクトップリンクは作成しません。
生の CLI 出力は [LOCALIZATION.md](LOCALIZATION.md) に従って英語のままです。
アプリの言語選択は引き続き即時反映されます。
