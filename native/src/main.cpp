#include "AppTheme.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSurfaceFormat>
#include <QTimer>

namespace {
QIcon createApplicationIcon() {
  QPixmap pixmap(128, 128);
  pixmap.fill(QColor(24, 27, 29));
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(77, 176, 158, 225));
  painter.drawEllipse(QRectF(22, 26, 66, 38));
  painter.setBrush(QColor(218, 169, 82, 220));
  painter.drawEllipse(QRectF(49, 50, 59, 35));
  painter.setBrush(QColor(105, 139, 211, 220));
  painter.drawEllipse(QRectF(29, 72, 54, 32));
  painter.setPen(QPen(QColor(241, 243, 244), 5.0));
  painter.setBrush(Qt::NoBrush);
  painter.drawEllipse(QRectF(17, 17, 94, 94));
  return QIcon(pixmap);
}
} // namespace

int main(int argc, char *argv[]) {
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

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
  QCoreApplication::setApplicationVersion(QStringLiteral(GSW_VERSION));
  application.setWindowIcon(createApplicationIcon());

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
  parser.addPositionalArgument(QStringLiteral("project"), QStringLiteral("Project file to open."), QStringLiteral("[project]"));
  parser.process(application);

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
