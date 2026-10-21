// Non-displayed public dialog contracts. No MainWindow or training connection
// consumes runRequested: these checks use only the real CPU plan helper.
#include "AppLanguage.h"
#include "TrainingBatchSmokeTest.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const QString testRoot=qEnvironmentVariable("GSW_TEST_ROOT");
  if(testRoot.isEmpty()||!QDir::isAbsolutePath(testRoot)||!QDir().mkpath(testRoot)){
    std::fprintf(stderr,"An explicit writable GSW_TEST_ROOT is required.\n");return 2;
  }
  qputenv("TEMP",testRoot.toUtf8());qputenv("TMP",testRoot.toUtf8());
  QApplication app(argc,argv);
  app.setOrganizationName(QStringLiteral("GSW-owned-batch-ui-contract"));
  app.setApplicationName(QStringLiteral("GSW-owned-batch-ui-contract"));
  const auto arguments=app.arguments();
  if(arguments.size()!=4){
    std::fprintf(stderr,"usage: TrainingBatchUiTests <repository> <python> <locale>\n");return 2;
  }
  const QString repository=QFileInfo(arguments.at(1)).canonicalFilePath();
  const QString python=QFileInfo(arguments.at(2)).canonicalFilePath();
  const QString locale=arguments.at(3);
  if(repository.isEmpty()||!QFileInfo(repository).isDir()||python.isEmpty()||!QFileInfo(python).isFile())return 2;
  qputenv("GSW_BACKEND_ROOT",repository.toUtf8());
  qputenv("GSW_BATCH_TEST_PYTHON",python.toUtf8());
  qputenv("PYTHONUTF8","1");qputenv("PYTHONDONTWRITEBYTECODE","1");
  QTemporaryDir temporary(QDir(testRoot).filePath(QStringLiteral("batch-ui-contract-XXXXXX")));
  if(!temporary.isValid())return 2;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
  if(!gsw::AppLanguage::initialize(locale))return 3;
  const bool passed=gsw::runTrainingBatchSmokeTest();
  std::fprintf(stderr,"Training batch UI contracts: %s (%s); no MainWindow or GPU training.\n",
      passed?"PASS":"FAIL",qPrintable(locale));
  return passed?0:1;
}
