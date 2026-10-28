# Local developer publication / 本地开发发布 / ローカル開発用の発行

## 简体中文

`GSW Native Developer Publisher.exe` 是独立构建的开发工具，不随公开应用、启动器或更新
Helper 的运行包分发。唯一写命令明确为 `--publish-unsigned-local`，每次响应都包含
`admission=local-developer-unsigned` 和 `publisherAuthenticated=false`。完整 ZIP、文件清单、
对应源码及许可校验证明本地输入一致，不能证明发布者签名，不能代替 TUF 接纳或完整产品验收。

工具使用与公开更新相同的 `NativeActivationCore`：整次操作只持有一个 `.publish.lock`，
发布不可变版本槽并原子切换三字段 `current.json`。重复提交先重新验证已安装全文件；
指针损坏可用原完整 ZIP 和 Offer 重新提交恢复。旧版本不删除、覆盖或启动，工程、照片、
训练数据和设置不变。任何 `.update-trust-anchor` 或 `updates/trust` 痕迹（包括损坏内容、
目录或链接）都拒绝进入此未签名路径；工具不迁移或重置信任状态。

仅私有开发接纳可使用经过 `bootstrap.files` 完整校验的旧 protocol 1 引导；公开在线安装和
历史回退仍要求 protocol 2。PowerShell 仅保留首次可信引导复制和显式快捷方式编排；首次引导
短锁释放后才调用原生发布器，不复制版本槽，也不写当前指针。旧 `PackageRoot`、`FilesManifest`、
`AcceptanceProof` 仅用于首次引导；缺少新 `Archive`、`Offer`、`DeveloperPublisher` 时在写入前拒绝。
`ActivateVersion` 也必须提供版本匹配的原完整 ZIP 和 Offer，不把本地未签名凭据当作历史信任。

## English

`GSW Native Developer Publisher.exe` is built separately and is never shipped in the
public application, launcher or update-helper runtime package. Its only write command
is explicitly named `--publish-unsigned-local`. Every response includes
`admission=local-developer-unsigned` and `publisherAuthenticated=false`. Complete ZIP,
payload, corresponding-source and license checks establish local consistency, not a
publisher signature, TUF admission or full product acceptance.

The tool shares `NativeActivationCore` with the public updater: one `.publish.lock`
owns the complete operation, an immutable slot is published, and the three-field
`current.json` is atomically replaced. Retries independently revalidate an existing
slot. Resubmit the original whole ZIP and Offer to recover a malformed pointer. No
old version is removed, overwritten or started, and no project, photo, training data
or setting is changed. Any `.update-trust-anchor` or `updates/trust` trace, including
corrupt content, directories or links, rejects unsigned admission. Trust is never
migrated or reset by this tool.

Only the private developer admission can use a legacy protocol-1 bootstrap after
verifying every `bootstrap.files` row and the exact bootstrap file set. Public online
installation and historical rollback still require protocol 2. PowerShell retains
only first trusted-bootstrap copying and explicit shortcut orchestration. Its short
bootstrap lock is released before the native publisher acquires the shared lock;
PowerShell never copies a version slot or writes the launch pointer. Legacy
`PackageRoot`, `FilesManifest` and `AcceptanceProof` inputs serve the first bootstrap
only. Missing `Archive`, `Offer` or `DeveloperPublisher` rejects before any write.
`ActivateVersion` also requires the version-matching original whole ZIP and Offer;
a local unsigned receipt is not historical publisher trust.

## 日本語

`GSW Native Developer Publisher.exe` は別途ビルドする開発専用ツールであり、公開アプリ・
ランチャー・更新 Helper の実行パッケージには含めません。唯一の書込みコマンドは
`--publish-unsigned-local` です。応答には常に `admission=local-developer-unsigned` と
`publisherAuthenticated=false` が含まれます。完全 ZIP・全ファイル・対応ソース・ライセンスの
検証はローカル入力の整合性を示すものであり、発行元の署名、TUF の受入れ、製品全体の受入検証
を示しません。

公開更新と同じ `NativeActivationCore` を使い、一つの `.publish.lock` で操作全体を保護し、
変更しないバージョンスロットと三項目の `current.json` の原子的切替を管理します。再実行時も
既存スロット全体を再検証します。破損したポインターは元の完全 ZIP と Offer の再投入で復旧
できます。旧版を削除・上書き・起動せず、プロジェクト・写真・学習データ・設定を変更しません。
`.update-trust-anchor` または `updates/trust` の痕跡は、破損・ディレクトリー・リンクも含めて
未署名経路を拒否します。このツールは信頼状態の移行や初期化を行いません。

私有の開発用受入れだけが、`bootstrap.files` の全項目と実ファイル集合を検証した旧 protocol 1
を使用できます。公開オンラインインストールと履歴ロールバックは protocol 2 のままです。
PowerShell は初回の信頼済みブートストラップ配置と明示的なショートカット処理だけを担当し、
短い初回用ロックを解放してから原生発行ツールを呼びます。バージョンスロットやポインターは
書き込みません。旧 `PackageRoot`・`FilesManifest`・`AcceptanceProof` は初回用のみです。
新しい `Archive`・`Offer`・`DeveloperPublisher` がなければ書込み前に拒否します。
`ActivateVersion` にも対象版の元の完全 ZIP と Offer が必要であり、未署名のローカル記録を
発行元に対する履歴上の信頼として扱いません。

## Commands / 命令 / コマンド

```powershell
# Build-only on a development device; all automated tests run on remote CI.
cmake -S native/updater-developer -B D:/CodexBuild/gsw-developer-publisher
cmake --build D:/CodexBuild/gsw-developer-publisher --config Release `
  --target gsw_native_developer_publisher

# Prepare whole ZIP/source/license inputs through the developer packager first.
# $localOffer is a gsw.update_offer/1 JSON descriptor for these exact ZIP bytes.
& $developerPublisher --publish-unsigned-local $developerRoot $wholeZip $localOffer

# Optional first bootstrap and explicit entry-point migration.
./scripts/publish_native_installation.ps1 -InstallRoot $developerRoot `
  -Archive $wholeZip -Offer $localOffer -DeveloperPublisher $developerPublisher `
  -PackageRoot $verifiedPackage -FilesManifest $freshManifest `
  -AcceptanceProof $fullRemoteAcceptance -SynchronizeShortcuts

# Re-activate/recover only by revalidating the original whole release.
./scripts/publish_native_installation.ps1 -InstallRoot $developerRoot `
  -ActivateVersion $previousBuildId -Archive $originalWholeZip `
  -Offer $originalOffer -DeveloperPublisher $developerPublisher

# Remote Windows runner only, never on a user installation.
python -I -B scripts/test_native_developer_publisher.py --remote-fixture-only `
  --publisher $developerPublisher --work-root D:/owned-developer-fixtures `
  --evidence D:/developer-publication-evidence
```

The machine JSON schema is `gsw.native_developer_publication/1`, with `ok`, `code`,
optional `buildId`, and the fixed unsigned-admission fields above. Exit codes are 0
(activated) or 2 (rejected). No production signing key, Release, feed or public root
is created. / 退出码为 0（激活）或 2（拒绝），不创建生产密钥、Release、feed 或公开根。 /
終了コードは 0（切替成功）または 2（拒否）です。運用用署名鍵・Release・feed・公開ルートは
作成しません。
