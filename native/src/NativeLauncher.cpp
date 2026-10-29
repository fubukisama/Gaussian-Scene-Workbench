#include "NativeInstallation.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTranslator>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
  const QString applicationId = QString::fromLatin1(GSW_APP_USER_MODEL_ID);
  if (FAILED(SetCurrentProcessExplicitAppUserModelID(reinterpret_cast<LPCWSTR>(applicationId.utf16())))) return 4;
#endif
  QCoreApplication application(argc, argv);
  // Persistent settings identity must match existing installations across a display rename.
  QCoreApplication::setOrganizationName(QStringLiteral("GaussianSceneWorkbench"));
  QCoreApplication::setApplicationName(QString::fromLatin1("Gaussian Scene Workbench"));
  const QString language = QSettings().value(QStringLiteral("ui/language"), QStringLiteral("zh_CN")).toString();
  QTranslator catalog;
  if (QStringList{QStringLiteral("zh_CN"), QStringLiteral("en_US"), QStringLiteral("ja_JP")}.contains(language) &&
      catalog.load(QStringLiteral(":/i18n/workbench_%1.qm").arg(language))) application.installTranslator(&catalog);
  auto arguments = application.arguments().mid(1);
  if (arguments.size() == 2 && arguments.first() == QLatin1String("--read-shortcut")) {
    const auto info = gsw::readNativeShortcut(arguments.at(1));
    const QJsonObject record{{"valid", info.valid()}, {"target", info.target}, {"arguments", info.arguments},
        {"workingDirectory", info.workingDirectory}, {"description", info.description},
        {"showCommand", info.showCommand}, {"hotkey", info.hotkey}, {"error", info.error}};
    QTextStream(stdout) << QJsonDocument(record).toJson(QJsonDocument::Compact) << '\n';
    return info.valid() ? 0 : 2;
  }
  if (arguments.size() >= 2 && arguments.first() == QLatin1String("--write-shortcut")) {
    const QString path = arguments.at(1);
    const QString linkArguments = arguments.size() == 3 ? arguments.at(2) : QString();
    if (arguments.size() > 3) return 2;
    QString diagnostic;
    if (gsw::writeNativeLauncherShortcut(path, application.applicationFilePath(), linkArguments,
                                       QString::fromLatin1(GSW_APP_USER_MODEL_ID), &diagnostic)) return 0;
    QTextStream(stderr) << diagnostic << '\n';
    return 2;
  }
  QDir root(QCoreApplication::applicationDirPath());
  root.cdUp();
  if (!arguments.isEmpty() && arguments.first() == QLatin1String("--recover")) {
    QString error;
    if (arguments.size() == 1 && gsw::startNativeRecovery(root.absolutePath(), &error)) return 0;
    const auto title = QCoreApplication::translate("Workbench", "版本恢复");
    const auto text = gsw::nativeInstallationErrorText(QStringLiteral("managed_installation_recovery_unavailable"));
#ifdef Q_OS_WIN
    MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(text.utf16()),
        reinterpret_cast<LPCWSTR>(title.utf16()), MB_OK | MB_ICONERROR);
#else
    QTextStream(stderr) << title << ": " << text << '\n';
#endif
    return 2;
  }
  const bool planOnly = !arguments.isEmpty() && arguments.first() == QLatin1String("--print-launch-plan");
  if (planOnly) arguments.removeFirst();
  const bool diagnostics = !arguments.isEmpty() && arguments.first() == QLatin1String("--diagnostics");
  if (diagnostics) arguments.removeFirst();
  auto plan = gsw::resolveNativeInstallation(root.absolutePath(), diagnostics, arguments,
                                            QProcessEnvironment::systemEnvironment());
  if (planOnly) {
    // Never starts the application/GUI. Protocol 1 has no child; protocol 2
    // may invoke only the fixed headless read-only installed-version verifier.
    if (plan.valid()) (void)gsw::verifyNativeInstallationLaunch(plan, &plan.error);
    const QJsonObject record{{"valid", plan.valid()}, {"version", plan.version},
        {"program", plan.program}, {"workingDirectory", plan.workingDirectory}, {"error", plan.error}};
    QTextStream(stdout) << QJsonDocument(record).toJson(QJsonDocument::Compact) << '\n';
    return plan.valid() ? 0 : 2;
  }
  if (plan.valid()) {
    if (gsw::startNativeInstallation(plan, diagnostics, &plan.error)) return 0;
  }
  const QString title = plan.error == QLatin1String("managed_installation_start_unconfirmed")
      ? QCoreApplication::translate("Workbench", "启动状态未确认")
      : QCoreApplication::translate("Workbench", "无法启动软件");
  const QString text = gsw::nativeInstallationErrorText(plan.error);
#ifdef Q_OS_WIN
  MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(text.utf16()),
              reinterpret_cast<LPCWSTR>(title.utf16()), MB_OK | MB_ICONERROR);
#else
  QTextStream(stderr) << title << ": " << text << '\n';
#endif
  return 2;
}
