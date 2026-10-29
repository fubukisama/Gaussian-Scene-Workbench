// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeRecovery.h"
#include "AppLanguage.h"
#include "NativeActivationFiles.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace gsw {
namespace {
bool keys(const QJsonObject &object, const QSet<QString> &expected) {
  const auto names = object.keys();
  return QSet<QString>(names.cbegin(), names.cend()) == expected;
}
bool safeId(const QString &id) {
  static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$"));
  return expression.match(id).hasMatch();
}
bool safeCode(const QString &code) {
  static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_.-]{1,128}$"));
  return expression.match(code).hasMatch();
}
QString helperName() {
#ifdef GSW_UPDATER_FIXTURE_BUILD
  return QString::fromLatin1("GSW Update Fixture Helper.exe");
#else
  return QString::fromLatin1("Gaussian Scene Workbench Update Helper.exe");
#endif
}
}

struct NativeRecoveryWindow::Impl {
  enum class Operation { List, Restore };
  enum class Status { Loading, Ready, Empty, Restoring, Failed, Restored, Unconfirmed, Unavailable, NotConfigured };
  NativeRecoveryWindow *q;
  QString root, selected, code;
  QListWidget *list = nullptr;
  QLabel *statusLabel = nullptr;
  QPushButton *refresh = nullptr, *restore = nullptr;
  QProcess process;
  QTimer deadline;
  QFile heldHelper;
  QByteArray pending;
  qint64 outputSize = 0, errorSize = 0;
  bool busy = false, closeRequested = false, protocolError = false, terminalSeen = false, catalogReady = false;
  Operation operation = Operation::List;
  Status status = Status::Loading;
  QJsonObject terminal;

  Impl(NativeRecoveryWindow *window, QString installation) : q(window), root(QDir::cleanPath(installation)) {
    process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *parameters) {
      parameters->flags |= CREATE_NO_WINDOW;
    });
#endif
    QObject::connect(&process, &QProcess::readyReadStandardOutput, q, [this] { readOutput(); });
    QObject::connect(&process, &QProcess::readyReadStandardError, q, [this] {
      errorSize += process.readAllStandardError().size();
      if (errorSize > 4 * 1024 * 1024) protocolError = true;
    });
    QObject::connect(&process, &QProcess::errorOccurred, q, [this](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart && busy) {
        deadline.stop(); busy = false; heldHelper.close(); code = QStringLiteral("RECOVERY_HELPER_UNAVAILABLE");
        status = Status::Unavailable; catalogReady = false; update(); finishClose();
      } else if (error != QProcess::UnknownError) protocolError = true;
    });
    QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), q,
        [this](int exitCode, QProcess::ExitStatus exitStatus) { finished(exitCode, exitStatus); });
    deadline.setSingleShot(true); deadline.setInterval(120000);
    QObject::connect(&deadline, &QTimer::timeout, q, [this] {
      // No automatic retry or process termination after a lost/late response.
      code = QStringLiteral("RECOVERY_HELPER_TIMEOUT"); status = Status::Unconfirmed; update();
    });
  }
  ~Impl() { process.disconnect(q); deadline.disconnect(q); }

  void update() {
    const auto tr = [](const char *source) { return QCoreApplication::translate("Workbench", source); };
    QString text;
    if (closeRequested) text = tr("正在安全结束当前验证或恢复操作，完成后关闭此窗口。");
    else switch (status) {
      case Status::Loading: text = tr("正在验证已接受的本地版本…"); break;
      case Status::Ready: text = tr("请选择一个已接受且未撤回的本地版本。"); break;
      case Status::Empty: text = tr("没有可恢复的受信本地版本。现有工程和设置未更改。"); break;
      case Status::Restoring: text = tr("正在重新验证并切换选中版本…"); break;
      case Status::Failed: text = tr("恢复未完成（%1）。请检查当前版本；工程和设置未更改。").arg(code); break;
      case Status::Restored: text = tr("选中版本将在下次启动时使用；未启动程序，也未关闭当前任务。"); break;
      case Status::Unconfirmed: text = tr("恢复状态尚未确认（%1）。程序可能已切换；请勿重复操作，工程和设置未更改。").arg(code); break;
      case Status::Unavailable: text = tr("无法运行恢复校验组件（%1）。未尝试切换版本。").arg(code); break;
      case Status::NotConfigured: text = tr("尚未配置生产更新信任；不会接受未签名更新。"); break;
    }
    statusLabel->setText(text);
    refresh->setEnabled(!busy && !closeRequested);
    list->setEnabled(!busy && !closeRequested && catalogReady);
    restore->setEnabled(!busy && !closeRequested && catalogReady && list->currentItem() &&
        safeId(list->currentItem()->data(Qt::UserRole).toString()));
  }
  void start(Operation requested, const QString &id = {}) {
    if (busy || closeRequested || (requested == Operation::Restore && !safeId(id))) return;
    operation = requested; selected = id;
    if (requested == Operation::List) catalogReady = false;
    pending.clear(); outputSize = errorSize = 0; terminal = {}; terminalSeen = false; protocolError = false; code.clear();
    const auto program = QDir(root).filePath(QStringLiteral("bin/") + helperName());
    heldHelper.close();
    try { activation::heldRead(heldHelper, program); }
    catch (...) { code = QStringLiteral("RECOVERY_HELPER_UNAVAILABLE"); status = Status::Unavailable; catalogReady = false; update(); return; }
    process.setProgram(program); process.setWorkingDirectory(QFileInfo(program).absolutePath());
    process.setArguments(requested == Operation::List
        ? QStringList{QStringLiteral("--list-recovery"), QStringLiteral("stable")}
        : QStringList{QStringLiteral("--rollback"), QStringLiteral("stable"), id});
    busy = true; status = requested == Operation::List ? Status::Loading : Status::Restoring; update();
    deadline.start(); process.start(); process.closeWriteChannel();
  }
  void readOutput() {
    const auto chunk = process.readAllStandardOutput(); outputSize += chunk.size();
    if (outputSize > 4 * 1024 * 1024) { protocolError = true; pending.clear(); return; }
    pending += chunk;
    while (pending.contains('\n')) {
      const auto end = pending.indexOf('\n'); auto line = pending.left(end); pending.remove(0, end + 1);
      if (line.endsWith('\r')) line.chop(1);
      QJsonParseError error; const auto document = QJsonDocument::fromJson(line, &error);
      if (error.error != QJsonParseError::NoError || !document.isObject() ||
          document.toJson(QJsonDocument::Compact) != line || terminalSeen) { protocolError = true; continue; }
      const auto record = document.object();
      if (record.value("schema") != QLatin1String("gsw.update_event/1")) { protocolError = true; continue; }
      const auto phase = record.value("phase").toString();
      if (operation == Operation::Restore && phase == QLatin1String("verifying") && keys(record, {"schema", "phase"})) continue;
      if ((phase == QLatin1String("failed") && keys(record, {"schema", "phase", "code"}) && safeCode(record.value("code").toString())) ||
          (operation == Operation::List && phase == QLatin1String("recovery_candidates") &&
            keys(record, {"schema", "phase", "code", "candidates"}) && record.value("code") == QLatin1String("ACCEPTED_HISTORY")) ||
          (operation == Operation::Restore && phase == QLatin1String("rolled_back") &&
            keys(record, {"schema", "phase", "code", "buildId"}) && record.value("code") == QLatin1String("NEXT_LAUNCH") &&
            record.value("buildId") == selected)) { terminal = record; terminalSeen = true; }
      else protocolError = true;
    }
  }
  void finished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (!busy) return;
    readOutput(); errorSize += process.readAllStandardError().size(); deadline.stop(); busy = false; heldHelper.close();
    const bool valid = !protocolError && pending.isEmpty() && terminalSeen && errorSize <= 4 * 1024 * 1024 &&
        exitStatus == QProcess::NormalExit;
    if (!valid) { code = QStringLiteral("RECOVERY_HELPER_UNCONFIRMED"); status = Status::Unconfirmed; catalogReady = false; }
    else if (terminal.value("phase") == QLatin1String("failed") && exitCode == 2) {
      code = terminal.value("code").toString(); status = code == QLatin1String("NOT_CONFIGURED") ? Status::NotConfigured : Status::Failed;
      if (code == QLatin1String("NOT_CONFIGURED") || code == QLatin1String("BUILD_REVOKED")) catalogReady = false;
    } else if (exitCode != 0 || terminal.value("phase") == QLatin1String("failed")) {
      code = QStringLiteral("RECOVERY_HELPER_UNCONFIRMED"); status = Status::Unconfirmed; catalogReady = false;
    } else if (operation == Operation::Restore) status = Status::Restored;
    else {
      const auto value = terminal.value("candidates"); const auto candidates = value.toArray();
      bool validList = value.isArray() && candidates.size() <= 1024; QSet<QString> seen;
      for (const auto &candidate : candidates) {
        const auto item = candidate.toObject(); const auto id = item.value("buildId").toString();
        if (!candidate.isObject() || !keys(item, {"buildId", "displayVersion"}) || !safeId(id) ||
            !item.value("displayVersion").isString() || item.value("displayVersion").toString().toUtf8().size() > 2048 ||
            item.value("displayVersion").toString().contains(QLatin1String("://")) ||
            seen.contains(id.toCaseFolded())) validList = false;
        seen.insert(id.toCaseFolded());
      }
      if (!validList) { code = QStringLiteral("RECOVERY_HELPER_UNCONFIRMED"); status = Status::Unconfirmed; catalogReady = false; }
      else {
        const auto previous = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear();
        for (const auto &candidate : candidates) {
          const auto item = candidate.toObject();
          const auto id = item.value("buildId").toString(), version = item.value("displayVersion").toString();
          auto *row = new QListWidgetItem(version == id || version.isEmpty() ? id : version + QString::fromUtf8(" · ") + id, list);
          row->setData(Qt::UserRole, id); if (id == previous) list->setCurrentItem(row);
        }
        catalogReady = true; status = list->count() ? Status::Ready : Status::Empty;
      }
    }
    update(); finishClose();
  }
  void confirmRestore() {
    if (busy || !list->currentItem()) return;
    const auto id = list->currentItem()->data(Qt::UserRole).toString(); if (!safeId(id)) return;
    QMessageBox confirmation(QMessageBox::Question, QCoreApplication::translate("Workbench", "版本恢复"),
        QCoreApplication::translate("Workbench", "确认在下次启动时使用构建 %1？当前窗口和任务不会被关闭。").arg(id),
        QMessageBox::Yes | QMessageBox::Cancel, q);
    confirmation.setObjectName(QStringLiteral("recoveryConfirm")); confirmation.setTextFormat(Qt::PlainText);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    if (confirmation.exec() == QMessageBox::Yes) start(Operation::Restore, id);
  }
  void finishClose() { if (closeRequested && !busy) { closeRequested = false; q->close(); } }
  bool deferClose() { if (!busy) return false; closeRequested = true; update(); return true; }
};

NativeRecoveryWindow::NativeRecoveryWindow(const QString &installationRoot, QWidget *parent)
    : QDialog(parent), m(std::make_unique<Impl>(this, installationRoot)) {
  setObjectName(QStringLiteral("nativeRecoveryWindow")); setAttribute(Qt::WA_ShowWithoutActivating); setModal(false); resize(680, 440);
  AppLanguage::bind(this, "windowTitle", "版本恢复");
  auto *layout = new QVBoxLayout(this);
  auto *description = AppLanguage::text(new QLabel(this),
      "仅可恢复已接受且未撤回、文件完整的本地版本；切换仅影响下次启动，不会关闭当前窗口或任务。");
  description->setObjectName(QStringLiteral("recoveryDescription")); description->setWordWrap(true);
  description->setTextFormat(Qt::PlainText); layout->addWidget(description);
  auto *location = new QLabel(this); location->setObjectName(QStringLiteral("recoveryInstallation"));
  location->setWordWrap(true); location->setTextFormat(Qt::PlainText);
  const auto refreshLocation = [location, installationRoot] {
    location->setText(QCoreApplication::translate("Workbench", "安装位置：%1").arg(installationRoot));
  };
  AppLanguage::onChanged(location, refreshLocation); refreshLocation(); layout->addWidget(location);
  layout->addWidget(AppLanguage::text(new QLabel(this), "可恢复的已验证版本"));
  m->list = new QListWidget(this); m->list->setObjectName(QStringLiteral("recoveryCandidates"));
  m->list->setSelectionMode(QAbstractItemView::SingleSelection); layout->addWidget(m->list);
  m->statusLabel = new QLabel(this); m->statusLabel->setObjectName(QStringLiteral("recoveryStatus"));
  m->statusLabel->setWordWrap(true); m->statusLabel->setTextFormat(Qt::PlainText); layout->addWidget(m->statusLabel);
  auto *form = new QFormLayout; auto *languageLabel = AppLanguage::text(new QLabel(this), "语言设置");
  auto *language = new QComboBox(this); language->setObjectName(QStringLiteral("recoveryLanguage"));
  for (const auto &id : AppLanguage::supported()) language->addItem(AppLanguage::displayName(id), id);
  const auto refreshLanguage = [language] {
    const QSignalBlocker blocker(language); language->setCurrentIndex(language->findData(AppLanguage::current()));
  };
  AppLanguage::onChanged(language, refreshLanguage); refreshLanguage();
  connect(language, &QComboBox::currentIndexChanged, this, [language, refreshLanguage](int index) {
    if (index >= 0 && !AppLanguage::apply(language->itemData(index).toString())) refreshLanguage();
  });
  form->addRow(languageLabel, language); layout->addLayout(form);
  auto *buttons = new QDialogButtonBox(this);
  m->refresh = AppLanguage::text(new QPushButton(this), "重新验证版本"); m->refresh->setObjectName(QStringLiteral("recoveryRefresh"));
  m->restore = AppLanguage::text(new QPushButton(this), "恢复选中版本"); m->restore->setObjectName(QStringLiteral("recoveryRestore"));
  buttons->addButton(m->refresh, QDialogButtonBox::ActionRole); buttons->addButton(m->restore, QDialogButtonBox::ActionRole);
  auto *close = buttons->addButton(QDialogButtonBox::Close); close->setObjectName(QStringLiteral("recoveryClose"));
  AppLanguage::bind(close, "text", "关闭恢复窗口"); connect(close, &QPushButton::clicked, this, &QWidget::close);
  connect(m->refresh, &QPushButton::clicked, this, [this] { m->start(Impl::Operation::List); });
  connect(m->restore, &QPushButton::clicked, this, [this] { m->confirmRestore(); });
  connect(m->list, &QListWidget::currentItemChanged, this, [this] { m->update(); });
  layout->addWidget(buttons); AppLanguage::onChanged(this, [this] { m->update(); }); m->update();
  QTimer::singleShot(0, this, [this] { m->start(Impl::Operation::List); });
}
NativeRecoveryWindow::~NativeRecoveryWindow() = default;
void NativeRecoveryWindow::reject() { if (!m->deferClose()) QDialog::reject(); }
void NativeRecoveryWindow::closeEvent(QCloseEvent *event) {
  if (m->deferClose()) { event->ignore(); return; }
  QDialog::closeEvent(event);
}
} // namespace gsw
