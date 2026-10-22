#include "TrainingRequestStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const QString root = qEnvironmentVariable("GSW_TEST_ROOT", QDir::tempPath());
  if (!QDir().mkpath(root)) return 2;
  QTemporaryDir fixture(QDir(root).filePath(QStringLiteral("training-store-XXXXXX")));
  if (!fixture.isValid()) return 2;
  const QString workspace = fixture.filePath(QStringLiteral("workspace"));
  const QString dataset = QDir(workspace).filePath(QStringLiteral("datasets/owned"));
  if (!QDir().mkpath(dataset)) return 2;
  const QJsonObject request{{QStringLiteral("schema"), QStringLiteral("gsw.training_dialog_configuration/1")},
                           {QStringLiteral("iterations"), 17000},
                           {QStringLiteral("quality"), QStringLiteral("quick")}};
  QString error;
  const gsw::TrainingRequestStore store(workspace, dataset);
  const bool saved = store.save(request, &error);
  const gsw::TrainingRequestStore reopened(workspace, dataset);
  bool passed = saved && reopened.load(&error) == request && error.isEmpty();
  std::printf("Training request store %s: accepted request survives project-store reopen\n",
              passed ? "PASS" : "FAIL");
  const auto check = [&passed](const bool value, const char *label) {
    passed = passed && value;
    std::printf("Training request store %s: %s\n", value ? "PASS" : "FAIL", label);
  };
  check(gsw::TrainingRequestStore(workspace, QDir(workspace).filePath(QStringLiteral("datasets/other")))
            .load(&error).isEmpty() && error.isEmpty(),
        "another dataset never inherits manual training values");
  const QString otherWorkspace = fixture.filePath(QStringLiteral("other-workspace"));
  check(gsw::TrainingRequestStore(otherWorkspace, dataset).load(&error).isEmpty() && error.isEmpty(),
        "another project never inherits a dataset request");
  auto otherRequest = request;
  otherRequest.insert(QStringLiteral("iterations"), 23000);
  gsw::TrainingRequestStore canonical(workspace, dataset, QStringLiteral("owned-camera-identity"));
  check(canonical.save(otherRequest, &error) && error.isEmpty(),
        "canonical request saves under its explicit identity");
  check(gsw::TrainingRequestStore(workspace, fixture.filePath(QStringLiteral("relocated-camera")),
          QStringLiteral("owned-camera-identity")).load(&error) == otherRequest && error.isEmpty(),
        "same canonical identity remains reusable after source relocation");
  check(gsw::TrainingRequestStore(workspace, dataset, QStringLiteral("different-camera-identity"))
            .load(&error).isEmpty() && error.isEmpty(),
        "different camera identity cannot reuse a request implicitly");
  check(reopened.load(&error) == request && error.isEmpty(),
        "saving canonical values does not overwrite the ordinary dataset scope");
  auto unsafe = request;
  unsafe.insert(QStringLiteral("outputScene"), QStringLiteral("existing-result"));
  check(!store.save(unsafe, &error) && !error.isEmpty(),
        "restoration never stores an output-overwrite target");
  check(store.load(&error) == request && error.isEmpty(),
        "invalid save retains the last accepted request");
  return passed ? 0 : 1;
}
