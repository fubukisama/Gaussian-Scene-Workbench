#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <memory>
class QCloseEvent;
namespace gsw {
class TrainingBatchDialog final : public QDialog {
  Q_OBJECT
public:
  TrainingBatchDialog(const QString &python, const QString &backendRoot, const QString &workspaceRoot,
      const QJsonObject &currentDatasetDescriptor, const QProcessEnvironment &environment, QWidget *parent = nullptr);
  ~TrainingBatchDialog() override;
  [[nodiscard]] QJsonObject requestDocument() const;
  void setRequestDocument(const QJsonObject &request);
  [[nodiscard]] QJsonObject previewReport() const;
  [[nodiscard]] QString planPath() const;
  [[nodiscard]] bool isRunning() const;
  bool loadRequest(const QString &path);
  bool saveRequest(const QString &path);
  bool loadPlan(const QString &path);
public slots:
  void previewPlan();
  void createPlan();
  void cancelOperation();
  void startBatch();
  void resumeBatch();
  void refreshHistory();
signals:
  void runRequested(const QString &planPath, bool resume);
  void planningFinished(const QJsonObject &report);
protected:
  void reject() override;
  void closeEvent(QCloseEvent *event) override;
private:
  class State;
  std::unique_ptr<State> m;
  void invalidate();
  void refreshPresentation();
  void refreshEnabled();
  void startHelper(bool create);
  void readOutput(bool standardError);
  void finishHelper(int exitCode, bool normalExit);
  void fail(const char *source, const QString &detail = {});
};
}
