# Managed native updates / 原生版本更新 / ネイティブ版の更新

## 简体中文

桌面、开始菜单、任务栏统一使用固定的原生启动器。已验证的新版本放入独立的
`versions/<version>/`，完成全文件校验后才原子切换 `current.json`。下一次启动直接
进入新版，不必重新固定图标。运行中的版本继续工作，不强制重启；旧版本不删除，
工程、素材、设置和训练状态不搬迁、不覆盖。此入口对 3DGS、2DGS、点云、网格及
其他生成链路一致，不改变训练算法，也不将预览或 PLY 热启动宣称为优化器续训。

本机托管目录：`E:\Apps\Gaussian Scene Workbench`。其他机器可选可写的非系统盘；
只有系统盘时支持现有 C: 目录。启动器和最初的 Qt 引导运行库保留在 `bin/`，
各版本自带自己的运行库。普通版本更新不覆盖引导程序；更换引导协议需要单独迁移。
原有安装向导仍使用固定安装目录，无须套用此版本目录结构。

下方托管发布命令不是运行中热更新，也不负责后台联网下载或定时拉取 Git。新增的
“帮助 → 软件更新…”是独立联网前端：每日静默检查，自动下载及准备下次启动需主动开启。
生产信任或可信引导组件尚未配置时会明确报错，不能当作“已是最新”；接入界面不等于
已批准公开更新源、签名或引导迁移。发布者先确认对应提交的完整远端 CI、相关验收及
包文件通过，再执行下方命令。局部验收或等待中的 CI 不能发布。
SHA-256 防止文件错配，不等同于发布者数字签名；不要对来源不明的包生成验收凭证。
失败会保留当前版本及记录；回退只切换到已安装并重新校验通过的版本。

快捷方式迁移前备份原链接，保留参数、热键、窗口状态和说明；目标、工作目录、图标及
应用标识统一到固定入口。不取消固定、不重排任务栏，不重启资源管理器、不结束软件。
不会恢复回收站中被安全软件标记的旧诊断链接，也不设置杀毒白名单。

入口迁移只需执行一次。之后的常规更新只发布版本，不再扫描或重写快捷方式。
`-SynchronizeShortcuts` 用于首次迁移或显式修复；如其他软件的无效链接使扫描失败，
可用 `-ShortcutDirectories` 限定已识别的原生入口目录，保留失败记录和原链接。

手动诊断使用固定启动器的 `--diagnostics` 参数，抓帧仍须手动按 F12，保存在安装根目录
下的 `diagnostics/`，不上传。普通启动清除仅用于该子进程的诊断变量。安装错误提示
使用当前保存的中／英／日语言，应用内语言切换仍立即生效。

协议 2 的普通启动通过固定、无窗口的原生 helper 离线核对已接纳的可信历史、撤销记录及
完整版本文件，并持安装锁直到创建指定版本的进程；不联网、不回写信任台账或高水位，
不自动选择另一版本。离线校验只能拒绝本机已知的撤销，不能发现尚未同步的新撤销。
创建进程不等于通过应用健康验收。`--print-launch-plan` 在协议 1 下仍不创建子进程；
协议 2 下只运行只读校验 helper，不启动应用或 GUI。引导协议迁移仍须另行验证和部署。

## English

Desktop, Start menu and taskbar entry points use one stable native launcher. Each verified
release has an immutable `versions/<version>/` directory. Only after all payload hashes
pass does publication atomically replace `current.json`. The next launch uses the new
release without repinning. A running release is not restarted or overwritten, old releases
remain available for rollback, and projects, assets, settings and training state are untouched.
The entry point is shared by 3DGS, 2DGS, point clouds, meshes and all generation pipelines;
it changes no training algorithm or optimizer-state resume capability.

The local managed root is `E:\Apps\Gaussian Scene Workbench`. Prefer a writable non-system
drive elsewhere; existing C: locations are supported on system-drive-only devices. The
bootstrap and its initial Qt runtime remain in `bin/`; each release owns its own runtime.
Ordinary updates do not replace the bootstrap. A bootstrap-protocol migration is separate.
The classic setup wizard retains its existing fixed-directory installation adapter.

The managed publisher below is not live patching or a network downloader/Git poller. The
new Help → Software Updates entry is a separate online front end: checks are silent and
daily, while automatic download/next-launch preparation is opt-in. Missing production trust
or a trusted bootstrap produces an explicit error, never an up-to-date claim. UI integration
does not approve public feeds, signing or bootstrap migration. The publisher must first
verify full remote CI, relevant acceptance and the exact-source package. Scoped or pending
acceptance is insufficient. SHA-256 binds file integrity, not publisher authenticity;
never manufacture acceptance evidence for an untrusted package. Failure retains the active
release and a record; rollback selects and revalidates an already-installed release.

Migration backs up each recognized native shortcut and retains arguments, hotkey, window
state and description. Target, working directory, icon and application identity use the
stable entry. It does not unpin, reorder the taskbar, restart Explorer or close applications.
It does not restore a quarantined/deleted diagnostic link or change antivirus trust settings.

Entry-point migration is a one-time operation. Ordinary updates publish a version without
rescanning or rewriting shortcuts. Use `-SynchronizeShortcuts` only for initial migration or
explicit repair; if an unreadable unrelated link stops a scan, restrict it with
`-ShortcutDirectories` to identified native entry-point folders and retain the failure record.

Manual capture uses the native launcher's `--diagnostics` option; F12 remains opt-in and saves
locally under the managed root's `diagnostics/`, without uploads. Ordinary launch removes the
diagnostic variable for that child only. Launcher errors use the saved Chinese/English/Japanese
language; in-application language switching remains immediate.

Protocol 2 ordinary startup uses the fixed headless native helper to inspect accepted trust
history, known revocations and the complete installed slot offline, holding the installation
lock through creation of the selected application process. It does not fetch metadata, write
the trust ledger/high-water marks or choose another version. Offline inspection cannot learn
revocations that have not been synchronized. Process creation is not application-health
acceptance. `--print-launch-plan` still creates no child under protocol 1; under protocol 2
it runs only the read-only verification helper, never the application or a GUI. Bootstrap
protocol migration remains a separate verified deployment.

## 日本語

デスクトップ・スタートメニュー・タスクバーは共通の固定ネイティブランチャーを使用します。
検証済みの各バージョンを変更しない `versions/<version>/` に配置し、全ファイルのハッシュ
確認後に `current.json` を原子的に切り替えます。次回起動から新バージョンになり、再度の
ピン留めは不要です。起動中の旧版は終了・上書きせず、ロールバック用に残します。
プロジェクト・素材・設定・学習状態は移動・上書きしません。3DGS・2DGS・点群・メッシュ・
その他の生成経路で共通です。学習方式や最適化状態の再開機能を変更するものではありません。

本機の管理先は `E:\Apps\Gaussian Scene Workbench` です。他の端末でも書込み可能な
非システムドライブを優先し、C: のみの端末では既存ディレクトリーを利用できます。
ランチャーと初期 Qt ランタイムは `bin/` に保持し、各バージョンは独自のランタイムを
持ちます。通常更新でブートストラップを置換せず、プロトコル変更は別途移行します。
従来のセットアップウィザードは固定インストール先を使用する方式のままです。

下記の管理型発行コマンドはホットパッチやネットワークダウンローダー、Git の定期取得では
ありません。新しい「ヘルプ → ソフトウェア更新…」は独立したオンライン UI です。確認は
毎日サイレントに行い、自動ダウンロードと次回起動の準備は明示的な有効化が必要です。
本番の信頼設定や信頼できるブートストラップが未設定なら明確に失敗し、「最新版」とは
表示しません。UI の接続は公開配信元・署名・ブートストラップ移行の承認ではありません。
発行前に対応コミットの完全なリモート CI・関連受入検証・パッケージを確認します。
限定検証や実行中の CI では発行できません。SHA-256 はファイル整合性であり、発行元の署名ではありません。
不明な配布物に受入証跡を作成しないでください。失敗時は現行版と記録を保持します。
ロールバックはインストール済みの版を再検証して選択する操作です。

対象のネイティブショートカットは移行前にバックアップし、引数・ホットキー・表示状態・
説明を保持します。起動先・作業先・アイコン・アプリ識別子は固定入口に統一します。
ピン留め解除・並べ替え、Explorer の再起動、アプリ終了は行いません。削除・隔離された
旧診断リンクを復元せず、ウイルス対策の信頼設定も変更しません。

入口の移行は一度だけです。通常更新ではバージョンを発行し、ショートカットの再走査・
再書込みは行いません。`-SynchronizeShortcuts` は初回移行または明示的な修復に使用します。
他アプリの読取り不能なリンクで停止した場合は、失敗記録と元リンクを保持し、
`-ShortcutDirectories` で確認済みのネイティブ入口フォルダーに対象を限定できます。

診断はネイティブランチャーの `--diagnostics` で手動起動します。F12 による収集先は
管理ルート内の `diagnostics/` で、自動アップロードしません。通常起動では子プロセスの
診断変数のみ解除します。エラー表示は保存済みの中・英・日言語を使用し、アプリ内の
言語変更は引き続き即時反映されます。

プロトコル 2 の通常起動は、固定の非表示ネイティブ helper で受入済みの信頼履歴・既知の
撤回情報・インストール済み全ファイルをオフライン検証し、指定版のプロセスを作成するまで
インストールロックを保持します。通信、信頼台帳や高水位の書換え、別版の自動選択は
行いません。未同期の新しい撤回は検出できません。プロセス作成はアプリの健全性受入を
意味しません。`--print-launch-plan` はプロトコル 1 では子プロセスを作らず、プロトコル 2
では読取り専用の検証 helper のみを実行し、アプリや GUI は起動しません。
ブートストラップのプロトコル移行は引き続き別途の検証・配備が必要です。

## Publisher commands / 发布命令 / 発行コマンド

Developer CLI messages remain English. / 开发 CLI 原始输出保留英文。 /
開発 CLI の生出力は英語です。

```powershell
# Exact-source full remote acceptance, not a scoped test claim.
./scripts/write_native_release_manifest.ps1 -PackageRoot $verifiedPackage `
  -SourceCommit $commit -AcceptanceProof $fullAcceptance -OutputPath $freshManifest
./scripts/publish_native_installation.ps1 -InstallRoot 'E:\Apps\Gaussian Scene Workbench' `
  -PackageRoot $verifiedPackage -FilesManifest $freshManifest -AcceptanceProof $fullAcceptance
# One-time migration/repair only: append -SynchronizeShortcuts, optionally
# -ShortcutDirectories $identifiedNativeShortcutFolders.
# Roll back without deleting either slot or closing a running application.
./scripts/publish_native_installation.ps1 -InstallRoot 'E:\Apps\Gaussian Scene Workbench' `
  -ActivateVersion $previousInstalledVersion
# Manual capture: native executable and arguments, no shell execution-policy override.
& 'E:\Apps\Gaussian Scene Workbench\bin\Gaussian Scene Workbench Launcher.exe' --diagnostics
```

Evidence: `update-records/<id>/publication.json`, shortcut backups and `current-before.json`.
A partial entry-point failure is explicitly recorded in `publication-failed.json` with the
actual activation status. Remote gates include `native_managed_installation`,
`scripts/test_native_installation.ps1`, Native Managed Updates and the full Native Windows run.
