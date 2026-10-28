// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeUpdateUi.h"
#include "AppLanguage.h"
#include "NativeInstallation.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace gsw {
namespace {
QString text(const char *source) { return QCoreApplication::translate("Workbench", source); }
class UpdateController final : public QObject {
public:
  explicit UpdateController(QWidget *window) : QObject(window), mWindow(window) {
    setObjectName(QStringLiteral("nativeUpdateController"));
    QString applicationPath = QCoreApplication::applicationFilePath();
#ifdef GSW_UPDATER_UI_FIXTURE_BUILD
    // Compiled only into the distinct remote UI fixture target. The installed
    // application has no environment-based root/helper/verification override.
    const auto fixtureApplication = qEnvironmentVariable("GSW_UPDATE_UI_FIXTURE_APPLICATION");
    if (!fixtureApplication.isEmpty()) applicationPath = fixtureApplication;
#endif
    mRoot = managedInstallationForApplication(applicationPath);
    QDir version(QFileInfo(applicationPath).absolutePath());
    if (version.cdUp()) mRunningBuild = version.dirName();
    mPrepared = !mRoot.isEmpty() && QFileInfo(QDir(mRoot).filePath(QStringLiteral("updates/prepared.json"))).isFile();
    AppLanguage::onChanged(this, [this] { refresh(); });
    auto *timer = new QTimer(this);
    timer->setInterval(30 * 60 * 1000);
    connect(timer, &QTimer::timeout, this, [this] { automaticCheck(); });
    timer->start();
    QTimer::singleShot(30000, this, [this] { automaticCheck(); });
  }
  ~UpdateController() override {
    if (!mProcess) return;
    // Closing the app never kills training or interrupts an atomic activation.
    // Ask only the owned update helper to cancel, then leave it independent.
    mProcess->write("cancel\n"); mProcess->closeWriteChannel();
    disconnect(mProcess, nullptr, this, nullptr);
    mProcess->setParent(nullptr);
    connect(mProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), mProcess, &QObject::deleteLater);
  }
  void showDialog() {
    if (!mDialog) createDialog();
    refresh(); mDialog->show(); // Only an explicit Help action reaches here.
  }
private:
  QString channel() const {
    const auto value = QSettings().value(QStringLiteral("updates/channel"), QStringLiteral("stable")).toString();
    return value == QLatin1String("preview") ? value : QStringLiteral("stable");
  }
  void automaticCheck() {
    if (mProcess || !QSettings().value(QStringLiteral("updates/automaticCheck"), true).toBool()) return;
    const auto now = QDateTime::currentDateTimeUtc();
    if (now < QSettings().value(QStringLiteral("updates/deferredUntil")).toDateTime()) return;
    const auto previous = QSettings().value(QStringLiteral("updates/lastAttempt")).toDateTime();
    if (previous.isValid() && previous <= now && previous.secsTo(now) < 24 * 60 * 60) return;
    begin(QStringLiteral("--check"), true);
  }
  void begin(const QString &operation, bool background = false) {
    if (mProcess) return;
    mLastOperation = operation; mPhase = QStringLiteral("checking"); mCode.clear();
    mReceived = 0; mTotal = 0;
    if (operation == QLatin1String("--prepare")) mPrepared = false;
    mRequestChannel = channel(); mBuffer.clear(); mProtocolError = false;
    if (mRoot.isEmpty()) { mCode = QStringLiteral("BOOTSTRAP_REQUIRED"); mPhase = QStringLiteral("failed"); refresh(); return; }
    const auto helper = QDir(mRoot).filePath(QString::fromLatin1("bin/Gaussian Scene Workbench Update Helper.exe"));
    if (!QFileInfo(helper).isFile()) { mCode = QStringLiteral("BOOTSTRAP_REQUIRED"); mPhase = QStringLiteral("failed"); refresh(); return; }
    if (operation == QLatin1String("--check")) QSettings().setValue(QStringLiteral("updates/lastAttempt"), QDateTime::currentDateTimeUtc());
    auto *process = new QProcess(this);
    mProcess = process;
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
      mBuffer += process->readAllStandardOutput();
      if (mBuffer.size() > 4 * 1024 * 1024) { protocolFailure(); return; }
      for (auto end = mBuffer.indexOf('\n'); end >= 0; end = mBuffer.indexOf('\n')) {
        const auto line = mBuffer.left(end); mBuffer.remove(0, end + 1);
        QJsonParseError error;
        const auto record = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !record.isObject() ||
            record.object().value(QStringLiteral("schema")).toString() != QLatin1String("gsw.update_event/1")) { protocolFailure(); return; }
        if (mProtocolError) continue;
        const auto event = record.object();
        mPhase = event.value(QStringLiteral("phase")).toString();
        mCode = event.value(QStringLiteral("code")).toString();
        if (mPhase == QLatin1String("available")) mOffer = event.value(QStringLiteral("offer")).toObject();
        if (mPhase == QLatin1String("prepared")) mPrepared = true;
        if (mPhase == QLatin1String("activated")) mPrepared = false;
        if (event.contains(QStringLiteral("receivedBytes"))) {
          mReceived = event.value(QStringLiteral("receivedBytes")).toDouble();
          mTotal = event.value(QStringLiteral("totalBytes")).toDouble();
        }
        refresh();
      }
    });
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
      if (error != QProcess::FailedToStart) return;
      mProcess = nullptr; process->deleteLater(); mPhase = QStringLiteral("failed"); mCode = QStringLiteral("HELPER_START_FAILED"); refresh();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process, operation, background](int code, QProcess::ExitStatus exit) {
      if (mProcess != process) return;
      mProcess = nullptr; process->deleteLater();
      const auto expectedPhase = operation == QLatin1String("--check") ? QStringLiteral("available")
          : operation == QLatin1String("--prepare") ? QStringLiteral("prepared") : QStringLiteral("activated");
      if (exit == QProcess::NormalExit && code == 0 &&
          (!mBuffer.trimmed().isEmpty() || mPhase != expectedPhase ||
           (operation == QLatin1String("--check") && mOffer.value(QStringLiteral("buildId")).toString().isEmpty()))) {
        mProtocolError = true; mCode = QStringLiteral("UPDATE_PROTOCOL_INVALID");
      }
      if (exit != QProcess::NormalExit || code != 0 || mProtocolError) {
        mPhase = QStringLiteral("failed"); if (mCode.isEmpty()) mCode = QStringLiteral("HELPER_FAILED");
      }
      refresh();
      const auto skipped = QSettings().value(QStringLiteral("updates/skipped/") + mRequestChannel).toStringList();
      const bool automatic = background && QSettings().value(QStringLiteral("updates/automaticCheck"), true).toBool() &&
          QSettings().value(QStringLiteral("updates/prepareForNextLaunch"), false).toBool() && channel() == mRequestChannel;
      if (exit == QProcess::NormalExit && code == 0 && !mProtocolError && mPhase != QLatin1String("failed") && automatic) {
        if (operation == QLatin1String("--check") && mOffer.value(QStringLiteral("buildId")).toString() != mRunningBuild &&
            !skipped.contains(mOffer.value(QStringLiteral("buildId")).toString()))
          QTimer::singleShot(0, this, [this] { begin(QStringLiteral("--prepare"), true); });
        else if (operation == QLatin1String("--prepare") && mPrepared)
          QTimer::singleShot(0, this, [this] { begin(QStringLiteral("--apply-prepared"), true); });
      }
    });
    process->setProgram(helper); process->setArguments({operation, mRequestChannel});
    process->setWorkingDirectory(mRoot); process->start(); refresh();
  }
  void protocolFailure() {
    mProtocolError = true; mCode = QStringLiteral("UPDATE_PROTOCOL_INVALID"); mPhase = QStringLiteral("failed");
    mBuffer.clear(); if (mProcess) mProcess->write("cancel\n"); refresh();
  }
  void createDialog() {
    auto *dialog = new QDialog(mWindow); mDialog = dialog;
    dialog->setObjectName(QStringLiteral("nativeUpdateDialog")); dialog->resize(700, 550);
    AppLanguage::bind(dialog, "windowTitle", AppLanguage::source("软件更新"));
    auto *layout = new QVBoxLayout(dialog);
    mRunning = new QLabel(dialog); mRunning->setWordWrap(true); layout->addWidget(mRunning);
    mStatus = new QLabel(dialog); mStatus->setObjectName(QStringLiteral("nativeUpdateStatus")); mStatus->setWordWrap(true); layout->addWidget(mStatus);
    mProgress = new QProgressBar(dialog); mProgress->setRange(0, 1000); layout->addWidget(mProgress);
    mNotes = new QPlainTextEdit(dialog); mNotes->setObjectName(QStringLiteral("nativeUpdateNotes")); mNotes->setReadOnly(true); layout->addWidget(mNotes);
    auto *form = new QFormLayout;
    mChannel = new QComboBox(dialog); mChannel->addItems({QString(), QString()});
    mChannel->setObjectName(QStringLiteral("nativeUpdateChannel"));
    mChannel->setItemData(0, QStringLiteral("stable")); mChannel->setItemData(1, QStringLiteral("preview"));
    AppLanguage::bindComboItem(mChannel, 0, AppLanguage::source("稳定版"));
    AppLanguage::bindComboItem(mChannel, 1, AppLanguage::source("预览版"));
    mChannel->setCurrentIndex(channel() == QLatin1String("preview") ? 1 : 0);
    form->addRow(AppLanguage::text(new QLabel(dialog), AppLanguage::source("更新频道")), mChannel);
    layout->addLayout(form);
    connect(mChannel, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
      QSettings().setValue(QStringLiteral("updates/channel"), mChannel->currentData()); mOffer = {}; mPrepared = false; refresh();
    });
    auto *check = AppLanguage::text(new QCheckBox(dialog), AppLanguage::source("后台自动检查（不弹出窗口）"));
    check->setChecked(QSettings().value(QStringLiteral("updates/automaticCheck"), true).toBool());
    connect(check, &QCheckBox::toggled, this, [](bool value) { QSettings().setValue(QStringLiteral("updates/automaticCheck"), value); }); layout->addWidget(check);
    auto *prepare = AppLanguage::text(new QCheckBox(dialog), AppLanguage::source("自动下载并在下次启动时使用，不中断当前任务"));
    prepare->setChecked(QSettings().value(QStringLiteral("updates/prepareForNextLaunch"), false).toBool());
    connect(prepare, &QCheckBox::toggled, this, [](bool value) { QSettings().setValue(QStringLiteral("updates/prepareForNextLaunch"), value); }); layout->addWidget(prepare);
    auto *buttons = new QGridLayout; layout->addLayout(buttons);
    int buttonIndex = 0;
    const auto button = [dialog, buttons, &buttonIndex](const char *source) { auto *b = new QPushButton(dialog); AppLanguage::bind(b, "text", source); buttons->addWidget(b, buttonIndex / 3, buttonIndex % 3); ++buttonIndex; return b; };
    mCheck = button(AppLanguage::source("检查更新")); connect(mCheck, &QPushButton::clicked, this, [this] { begin(QStringLiteral("--check")); });
    mDownload = button(AppLanguage::source("下载更新")); connect(mDownload, &QPushButton::clicked, this, [this] { begin(QStringLiteral("--prepare")); });
    mApply = button(AppLanguage::source("下次启动使用此版本")); connect(mApply, &QPushButton::clicked, this, [this] { begin(QStringLiteral("--apply-prepared")); });
    mCancel = button(AppLanguage::source("取消下载")); connect(mCancel, &QPushButton::clicked, this, [this] { if (mProcess) { mProcess->write("cancel\n"); mPhase = QStringLiteral("cancelling"); refresh(); } });
    mRetry = button(AppLanguage::source("重试")); connect(mRetry, &QPushButton::clicked, this, [this] { begin(mLastOperation.isEmpty() ? QStringLiteral("--check") : mLastOperation); });
    mSkip = button(AppLanguage::source("跳过此构建")); connect(mSkip, &QPushButton::clicked, this, [this] {
      auto skipped = QSettings().value(QStringLiteral("updates/skipped/") + channel()).toStringList();
      skipped.removeAll(mOffer.value(QStringLiteral("buildId")).toString()); skipped.prepend(mOffer.value(QStringLiteral("buildId")).toString());
      while (skipped.size() > 100) skipped.removeLast(); QSettings().setValue(QStringLiteral("updates/skipped/") + channel(), skipped);
      mPhase = QStringLiteral("skipped"); refresh();
    });
    auto *later = button(AppLanguage::source("稍后提醒")); connect(later, &QPushButton::clicked, this, [this] {
      QSettings().setValue(QStringLiteral("updates/deferredUntil"), QDateTime::currentDateTimeUtc().addDays(1)); mDialog->hide();
    });
    auto *close = button(AppLanguage::source("关闭")); connect(close, &QPushButton::clicked, dialog, &QDialog::hide);
    mCheck->setObjectName(QStringLiteral("nativeUpdateCheck")); mDownload->setObjectName(QStringLiteral("nativeUpdateDownload"));
    mApply->setObjectName(QStringLiteral("nativeUpdateApply")); mCancel->setObjectName(QStringLiteral("nativeUpdateCancel"));
    mRetry->setObjectName(QStringLiteral("nativeUpdateRetry")); mSkip->setObjectName(QStringLiteral("nativeUpdateSkip"));
    later->setObjectName(QStringLiteral("nativeUpdateLater")); check->setObjectName(QStringLiteral("nativeUpdateAutomaticCheck"));
    prepare->setObjectName(QStringLiteral("nativeUpdateAutomaticPrepare"));
  }
  void refresh() {
    if (!mDialog) return;
    mRunning->setText(text(AppLanguage::source("正在运行的构建：%1")).arg(mRoot.isEmpty() ? text(AppLanguage::source("开发构建（未托管安装）")) : mRunningBuild));
    QString status;
    if (mCode == QLatin1String("NOT_CONFIGURED")) status = text(AppLanguage::source("尚未配置生产更新信任；不会接受未签名更新。"));
    else if (mCode == QLatin1String("BOOTSTRAP_REQUIRED") || mCode == QLatin1String("activation_bootstrap_migration_required")) status = text(AppLanguage::source("需要使用可信安装器升级启动组件；当前程序和工程未改变。"));
    else if (mPhase == QLatin1String("failed")) status = text(AppLanguage::source("更新未完成或尚未确认（%1）。当前窗口与工程不受影响，请重新检查更新状态。")).arg(mCode);
    else if (mPhase == QLatin1String("checking")) status = text(AppLanguage::source("正在检查已签名的更新信息…"));
    else if (mPhase == QLatin1String("downloading")) status = text(AppLanguage::source("正在下载：%1 / %2 MiB")).arg(mReceived / (1024 * 1024), 0, 'f', 1).arg(mTotal / (1024 * 1024), 0, 'f', 1);
    else if (mPhase == QLatin1String("verifying")) status = text(AppLanguage::source("正在验证签名和文件完整性…"));
    else if (mPhase == QLatin1String("staging")) status = text(AppLanguage::source("正在准备独立的新版本目录…"));
    else if (mPhase == QLatin1String("prepared")) status = text(AppLanguage::source("更新已下载并验证，可以准备下次启动。"));
    else if (mPhase == QLatin1String("activated")) status = text(AppLanguage::source("下次启动时生效；当前窗口和任务将继续运行。"));
    else if (mPhase == QLatin1String("cancelling")) status = text(AppLanguage::source("正在取消，保留可安全续传的数据…"));
    else if (mPhase == QLatin1String("skipped")) status = text(AppLanguage::source("自动更新已跳过此构建；仍可手动下载。"));
    else if (mOffer.value(QStringLiteral("buildId")).toString() == mRunningBuild && !mOffer.isEmpty()) status = text(AppLanguage::source("当前正在运行此构建。"));
    else if (!mOffer.isEmpty()) status = text(AppLanguage::source("可用版本：%1 · 构建 %2")).arg(mOffer.value(QStringLiteral("displayVersion")).toString(), mOffer.value(QStringLiteral("buildId")).toString());
    else status = text(AppLanguage::source("更新检查不影响当前工程和训练；不会自动重启软件。"));
    mStatus->setText(status);
    const auto notes = mOffer.value(QStringLiteral("releaseNotes")).toObject();
    const auto releaseText = notes.value(AppLanguage::current()).toString(notes.value(QStringLiteral("en_US")).toString());
    if (mNotes->toPlainText() != releaseText) mNotes->setPlainText(releaseText);
    const bool busy = mProcess != nullptr;
    mChannel->setEnabled(!busy); mCheck->setEnabled(!busy); mDownload->setEnabled(!busy && !mOffer.isEmpty() && mOffer.value(QStringLiteral("buildId")).toString() != mRunningBuild);
    mApply->setEnabled(!busy && mPrepared); mRetry->setEnabled(!busy && mPhase == QLatin1String("failed"));
    mCancel->setEnabled(busy && mPhase != QLatin1String("staging")); mSkip->setEnabled(!busy && !mOffer.isEmpty());
    if (mPhase == QLatin1String("downloading") && mTotal > 0) { mProgress->setRange(0, 1000); mProgress->setValue(int(qBound(0.0, mReceived / mTotal * 1000.0, 1000.0))); }
    else if (busy) mProgress->setRange(0, 0);
    else { mProgress->setRange(0, 1000); mProgress->setValue(mPhase == QLatin1String("activated") || mPrepared ? 1000 : 0); }
  }
  QPointer<QWidget> mWindow;
  QPointer<QDialog> mDialog;
  QPointer<QProcess> mProcess;
  QLabel *mRunning = nullptr, *mStatus = nullptr;
  QPlainTextEdit *mNotes = nullptr;
  QComboBox *mChannel = nullptr;
  QProgressBar *mProgress = nullptr;
  QPushButton *mCheck = nullptr, *mDownload = nullptr, *mApply = nullptr, *mCancel = nullptr, *mRetry = nullptr, *mSkip = nullptr;
  QString mRoot, mRunningBuild, mRequestChannel, mLastOperation, mPhase, mCode;
  QByteArray mBuffer;
  QJsonObject mOffer;
  double mReceived = 0, mTotal = 0;
  bool mPrepared = false, mProtocolError = false;
};
}
void installNativeUpdateUi(QMenu *helpMenu, QWidget *window) {
  if (!helpMenu || !window || window->findChild<QObject *>(QStringLiteral("nativeUpdateController"))) return;
  auto *controller = new UpdateController(window);
  auto *action = AppLanguage::text(helpMenu->addAction(QString()), AppLanguage::source("软件更新…"));
  action->setObjectName(QStringLiteral("nativeUpdateAction"));
  QObject::connect(action, &QAction::triggered, controller, [controller] { controller->showDialog(); });
}
} // namespace gsw
