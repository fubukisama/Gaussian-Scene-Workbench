// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeRecovery.h"
#include "NativeInstallation.h"
#include "AppLanguage.h"
#include <QApplication>
#include <QCoreApplication>
#include <QTextStream>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
  const auto applicationId = QString::fromLatin1(GSW_APP_USER_MODEL_ID);
  if (FAILED(SetCurrentProcessExplicitAppUserModelID(reinterpret_cast<LPCWSTR>(applicationId.utf16())))) return 4;
#endif
  QApplication application(argc, argv);
  if (application.arguments().size() != 1) return 2; // No root/feed/key/unsafe CLI.
#ifdef GSW_UPDATER_FIXTURE_BUILD
  QCoreApplication::setOrganizationName(QStringLiteral("GSWUpdateFixture"));
  QCoreApplication::setApplicationName(QString::fromLatin1("GSW Update Fixture Recovery"));
#else
  QCoreApplication::setOrganizationName(QStringLiteral("GaussianSceneWorkbench"));
  QCoreApplication::setApplicationName(QStringLiteral("Gaussian Scene Workbench"));
#endif
  const auto root = gsw::nativeRecoveryInstallationForApplication(QCoreApplication::applicationFilePath());
  if (root.isEmpty()) { QTextStream(stderr) << "recovery_installation_invalid\n"; return 2; }
  if (!gsw::AppLanguage::initialize()) { QTextStream(stderr) << "recovery_language_unavailable\n"; return 3; }
  gsw::NativeRecoveryWindow window(root);
  window.show(); // Explicit recovery request only; never raise/activate/topmost.
  return application.exec();
}
