#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <memory>

class QCloseEvent;
namespace gsw {
// The dialog owns only short-lived CPU inspection helpers, never trainers.
class ExistingTrainingDialog final : public QDialog {
  Q_OBJECT
public:
  ExistingTrainingDialog(const QString &python, const QString &backendRoot,
      const QProcessEnvironment &environment, const QString &initialJobStore,
      QWidget *parent=nullptr);
  ~ExistingTrainingDialog() override;
  [[nodiscard]] QJsonObject currentReport() const;
  [[nodiscard]] bool isConnected() const;
  [[nodiscard]] bool isBusy() const;
  bool connectBatch(const QString &planPath);
public slots:
  void refreshJobs();
  void connectSelectedJob();
  void disconnectJob();
  void pauseJob();
  void stopJob();
signals:
  void resumeRequested(const QString &configurationPath,const QString &outputDirectory);
  void resultRequested(const QString &outputDirectory);
  void resumeBatchRequested(const QString &planPath);
  void reportUpdated(const QJsonObject &report);
protected:
  void reject() override;
  void closeEvent(QCloseEvent *event) override;
private:
  class State;
  std::unique_ptr<State> m;
  void refreshPresentation();
  void refreshEnabled();
  void startHelper(const QString &operation);
  void finishHelper(int exitCode,bool normalExit);
  void readHelperOutput(bool errors);
  void cancelOwnedHelper();
  void requestCommand(const QString &command);
};
}
