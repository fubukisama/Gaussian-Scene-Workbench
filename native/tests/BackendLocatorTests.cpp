#include "BackendLocator.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {
class ScopedEnvironment final {
public:
  explicit ScopedEnvironment(const char *name)
      : mName(name), mWasSet(qEnvironmentVariableIsSet(name)),
        mValue(qgetenv(name)) {}
  ~ScopedEnvironment() {
    if (mWasSet) {
      qputenv(mName.constData(), mValue);
    } else {
      qunsetenv(mName.constData());
    }
  }

private:
  QByteArray mName;
  bool mWasSet;
  QByteArray mValue;
};

bool touchFile(const QString &path) {
  const QFileInfo info(path);
  if (!QDir().mkpath(info.absolutePath())) {
    return false;
  }
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
}
} // namespace

class BackendLocatorTests final : public QObject {
  Q_OBJECT

private slots:
  void ignoresRepositoryMarkersInTheWorkingDirectory();
  void rejectsBackendWithoutVideoExtractionHelper();
  void rejectsBackendWithoutImportPreflight();
  void locatesCompletePackagedBackendAboveBinDirectory();
  void prefersConfiguredGaussianEnvironmentAndSupportsGsPrefix();
  void locatesGaussianEnvironmentOnRepositoryVolume();
  void doesNotTreatArbitraryPathPythonAsGaussianEnvironment();
};

void BackendLocatorTests::ignoresRepositoryMarkersInTheWorkingDirectory() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  const QString applicationDirectory = root.filePath(QStringLiteral("trusted/app/bin"));
  const QString untrustedDirectory = root.filePath(QStringLiteral("opened-project"));
  QVERIFY(QDir().mkpath(applicationDirectory));
  QVERIFY(touchFile(QDir(untrustedDirectory).filePath(QStringLiteral("native/worker/gsw_worker.py"))));
  QVERIFY(touchFile(QDir(untrustedDirectory).filePath(QStringLiteral("native/worker/import_preflight.py"))));
  QVERIFY(touchFile(QDir(untrustedDirectory).filePath(QStringLiteral("crop_editor/server.py"))));
  QVERIFY(touchFile(QDir(untrustedDirectory).filePath(QStringLiteral("crop_editor/video_extract.py"))));
  QVERIFY(touchFile(QDir(untrustedDirectory).filePath(QStringLiteral("scripts/check_3dgs_env.ps1"))));
  QVERIFY(touchFile(QDir(untrustedDirectory).filePath(QStringLiteral("gaussian-splatting/train.py"))));

  const QString originalWorkingDirectory = QDir::currentPath();
  QVERIFY(QDir::setCurrent(untrustedDirectory));
  const QString located = gsw::BackendLocator::findRepositoryRoot(applicationDirectory);
  QVERIFY(QDir::setCurrent(originalWorkingDirectory));

  QVERIFY(located.isEmpty());
}

void BackendLocatorTests::rejectsBackendWithoutVideoExtractionHelper() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  QVERIFY(touchFile(root.filePath(QStringLiteral("native/worker/gsw_worker.py"))));
  QVERIFY(touchFile(root.filePath(QStringLiteral("native/worker/import_preflight.py"))));
  QVERIFY(touchFile(root.filePath(QStringLiteral("crop_editor/server.py"))));
  QVERIFY(touchFile(root.filePath(QStringLiteral("scripts/check_3dgs_env.ps1"))));
  QVERIFY(touchFile(root.filePath(QStringLiteral("gaussian-splatting/train.py"))));

  QVERIFY(gsw::BackendLocator::findRepositoryRoot(root.path(), root.path()).isEmpty());
}

void BackendLocatorTests::rejectsBackendWithoutImportPreflight() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  QVERIFY(touchFile(root.filePath(QStringLiteral("native/worker/gsw_worker.py"))));
  QVERIFY(touchFile(root.filePath(QStringLiteral("crop_editor/server.py"))));
  QVERIFY(touchFile(root.filePath(QStringLiteral("crop_editor/video_extract.py"))));
  QVERIFY(touchFile(root.filePath(QStringLiteral("scripts/check_3dgs_env.ps1"))));
  QVERIFY(touchFile(root.filePath(QStringLiteral("gaussian-splatting/train.py"))));

  QVERIFY(gsw::BackendLocator::findRepositoryRoot(root.path(), root.path()).isEmpty());
}

void BackendLocatorTests::locatesCompletePackagedBackendAboveBinDirectory() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString applicationDirectory = root.filePath(QStringLiteral("bin"));
  QVERIFY(QDir().mkpath(applicationDirectory));
  const QStringList markers = {
      QStringLiteral("native/worker/gsw_worker.py"),
      QStringLiteral("native/worker/import_preflight.py"),
      QStringLiteral("crop_editor/server.py"),
      QStringLiteral("crop_editor/video_extract.py"),
      QStringLiteral("scripts/check_3dgs_env.ps1"),
      QStringLiteral("gaussian-splatting/train.py"),
  };
  for (const QString &marker : markers) {
    QVERIFY(touchFile(root.filePath(marker)));
  }

  QCOMPARE(gsw::BackendLocator::findRepositoryRoot(applicationDirectory),
           QDir::cleanPath(root.path()));
}

void BackendLocatorTests::prefersConfiguredGaussianEnvironmentAndSupportsGsPrefix() {
  ScopedEnvironment gaussianGuard("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  ScopedEnvironment gsGuard("GS_CONDA_PREFIX");
  ScopedEnvironment condaGuard("CONDA_PREFIX");
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString preferred = root.filePath(QStringLiteral("preferred"));
  const QString fallback = root.filePath(QStringLiteral("fallback"));
  QVERIFY(touchFile(QDir(preferred).filePath(QStringLiteral("python.exe"))));
  QVERIFY(touchFile(QDir(fallback).filePath(QStringLiteral("python.exe"))));
  qputenv("GAUSSIAN_SPLATTING_CONDA_PREFIX", preferred.toUtf8());
  qputenv("GS_CONDA_PREFIX", fallback.toUtf8());
  qunsetenv("CONDA_PREFIX");

  QCOMPARE(gsw::BackendLocator::findGaussianPython(root.path()),
           QDir::toNativeSeparators(QDir(preferred).filePath(QStringLiteral("python.exe"))));

  qunsetenv("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  QCOMPARE(gsw::BackendLocator::findGaussianPython(root.path()),
           QDir::toNativeSeparators(QDir(fallback).filePath(QStringLiteral("python.exe"))));
}

void BackendLocatorTests::locatesGaussianEnvironmentOnRepositoryVolume() {
  ScopedEnvironment gaussianGuard("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  ScopedEnvironment gsGuard("GS_CONDA_PREFIX");
  ScopedEnvironment condaGuard("CONDA_PREFIX");
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir fixture(temporary.path());
  const QString volumeRoot = fixture.filePath(QStringLiteral("data-volume"));
  const QString repositoryRoot =
      QDir(volumeRoot).filePath(QStringLiteral("apps/gsw"));
  const QString expectedPython = QDir(volumeRoot).filePath(
      QStringLiteral("conda/envs/gaussian_splatting/python.exe"));
  QVERIFY(QDir().mkpath(repositoryRoot));
  QVERIFY(touchFile(expectedPython));
  qunsetenv("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  qunsetenv("GS_CONDA_PREFIX");
  qunsetenv("CONDA_PREFIX");

  QCOMPARE(gsw::BackendLocator::findGaussianPython(repositoryRoot,
                                                    {volumeRoot}),
           QDir::toNativeSeparators(expectedPython));
}

void BackendLocatorTests::doesNotTreatArbitraryPathPythonAsGaussianEnvironment() {
  ScopedEnvironment gaussianGuard("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  ScopedEnvironment gsGuard("GS_CONDA_PREFIX");
  ScopedEnvironment condaGuard("CONDA_PREFIX");
  ScopedEnvironment pathGuard("PATH");
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString arbitraryPython = root.filePath(QStringLiteral("unrelated/python.exe"));
  QVERIFY(touchFile(arbitraryPython));
  qunsetenv("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  qunsetenv("GS_CONDA_PREFIX");
  qunsetenv("CONDA_PREFIX");
  qputenv("PATH", QFileInfo(arbitraryPython).absolutePath().toUtf8());

  QVERIFY(gsw::BackendLocator::findGaussianPython(root.path()) !=
          QDir::toNativeSeparators(arbitraryPython));
}

QTEST_GUILESS_MAIN(BackendLocatorTests)

#include "BackendLocatorTests.moc"
