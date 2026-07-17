#include "AppTheme.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QSurfaceFormat>
#include <QTimer>

#ifdef _WIN32
#include <shobjidl.h>

#include <cwchar>

#define GSW_WIDEN_IMPL(value) L##value
#define GSW_WIDEN(value) GSW_WIDEN_IMPL(value)

namespace {
bool configureWindowsApplicationIdentity() {
  constexpr const wchar_t *expectedId = GSW_WIDEN(GSW_APP_USER_MODEL_ID);
  if (FAILED(SetCurrentProcessExplicitAppUserModelID(expectedId))) {
    return false;
  }

  PWSTR actualId = nullptr;
  const HRESULT result = GetCurrentProcessExplicitAppUserModelID(&actualId);
  const bool matches = SUCCEEDED(result) && actualId != nullptr &&
                       std::wcscmp(actualId, expectedId) == 0;
  CoTaskMemFree(actualId);
  return matches;
}
} // namespace

#undef GSW_WIDEN
#undef GSW_WIDEN_IMPL
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
  if (!configureWindowsApplicationIdentity()) {
    return 4;
  }
#endif

  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  if (qEnvironmentVariableIntValue("GSW_USE_NON_NATIVE_DIALOGS") != 0) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  }

  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  format.setSamples(4);
  format.setSwapInterval(1);
  QSurfaceFormat::setDefaultFormat(format);

  QApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("GaussianSceneWorkbench"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("github.com/fubukisama"));
  QCoreApplication::setApplicationName(QStringLiteral("Gaussian Scene Workbench"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("Gaussian Scene Workbench Native"));
  QCoreApplication::setApplicationVersion(QStringLiteral(GSW_VERSION));
  const QIcon applicationIcon(QStringLiteral(":/icons/gsw-app-icon.png"));
  if (applicationIcon.isNull()) {
    qCritical() << "Failed to load the embedded application icon.";
    return 3;
  }
  application.setWindowIcon(applicationIcon);

  const int scalePercent = gsw::AppTheme::loadScalePercent(QGuiApplication::primaryScreen());
  gsw::AppTheme::apply(application, scalePercent, false);

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("Gaussian Scene Workbench native desktop application"));
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption projectOption(
      {QStringLiteral("p"), QStringLiteral("project")},
      QStringLiteral("Open a .gsw.json project file."), QStringLiteral("file"));
  parser.addOption(projectOption);
  QCommandLineOption smokeTestOption(
      QStringLiteral("smoke-test"),
      QStringLiteral("Launch the main window briefly, then exit successfully."));
  parser.addOption(smokeTestOption);
  QCommandLineOption mediaSourceOption(
      QStringLiteral("media-source"),
      QStringLiteral("Pre-populate the media import dialog with a file or directory. "
                     "May be specified more than once."),
      QStringLiteral("path"));
  parser.addOption(mediaSourceOption);
  parser.addPositionalArgument(QStringLiteral("project"), QStringLiteral("Project file to open."), QStringLiteral("[project]"));
  parser.process(application);

  application.setProperty("gswInitialMediaSources",
                          parser.values(mediaSourceOption));
  gsw::MainWindow window;
  QString projectPath = parser.value(projectOption);
  const bool smokeTest = parser.isSet(smokeTestOption);
  if (projectPath.isEmpty() && !parser.positionalArguments().isEmpty()) {
    projectPath = parser.positionalArguments().first();
  }
  if (!projectPath.isEmpty()) {
    window.openProjectFile(QFileInfo(projectPath).absoluteFilePath());
  }
  window.show();
  bool smokeTestCompleted = !smokeTest;
  if (smokeTest) {
    QTimer::singleShot(250, &application, [&application, &window, &smokeTestCompleted]() {
      smokeTestCompleted = window.isVisible();
      application.exit(smokeTestCompleted ? 0 : 2);
    });
  }
  const int exitCode = application.exec();
  return smokeTestCompleted ? exitCode : 2;
}
