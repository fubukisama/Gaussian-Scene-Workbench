#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <memory>

class QCloseEvent;
namespace gsw {
class ScaleConstraintsDialog final : public QDialog {
  Q_OBJECT
public:
  ScaleConstraintsDialog(const QString &python, const QString &backendRoot,
      const QString &workspaceRoot, const QString &sourceDatasetPath,
      const QString &expectedIdentity, const QProcessEnvironment &environment,
      QWidget *parent = nullptr);
  ScaleConstraintsDialog(const QString &python, const QString &backendRoot,
      const QString &workspaceRoot, const QJsonObject &sourceDescriptor,
      const QProcessEnvironment &environment, QWidget *parent = nullptr);
  ~ScaleConstraintsDialog() override;
  [[nodiscard]] QJsonObject constraintDocument() const;
  void setConstraintDocument(const QJsonObject &document);
  [[nodiscard]] QJsonObject solutionReport() const;
  [[nodiscard]] QString resultDatasetPath() const;
  [[nodiscard]] QJsonObject resultDatasetInfo() const;
  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] QJsonObject sourceDescriptor() const;
  [[nodiscard]] QString resultVersionPath() const;
  bool loadConstraints(const QString &path);
  bool saveConstraints(const QString &path);
  bool saveReport(const QString &path);
public slots:
  void startEstimate();
  void cancelOperation();
  void createDatasetVersion();
  void inspectSource();
signals:
  void estimateFinished(const QJsonObject &report);
  void datasetVersionCreated(const QString &path, const QJsonObject &summary);
  void versionCreated(const QString &kind, const QString &artifactPath, const QJsonObject &summary);
protected:
  void reject() override;
  void closeEvent(QCloseEvent *event) override;
private:
  class State;
  std::unique_ptr<State> m;
  void refreshPresentation();
  void refreshEnabled();
  void invalidate();
  void startOperation(bool createVersion, bool inspect = false);
  void readOutput(bool standardError);
  void finishProcess(int exitCode, bool normalExit);
  void fail(const char *source, const QString &detail = {});
};
} // namespace gsw
