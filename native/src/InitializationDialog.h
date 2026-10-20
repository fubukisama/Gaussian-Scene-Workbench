#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <memory>
class QCloseEvent;
namespace gsw {
class InitializationDialog final : public QDialog {
  Q_OBJECT
public:
  InitializationDialog(const QString &python, const QString &backendRoot,
      const QString &workspaceRoot, const QString &datasetPath, const QString &expectedIdentity,
      const QJsonObject &initialization, const QProcessEnvironment &environment, QWidget *parent = nullptr);
  ~InitializationDialog() override;
  [[nodiscard]] QJsonObject requestDocument() const;
  bool setRequestDocument(const QJsonObject &request);
  [[nodiscard]] QJsonObject report() const;
  [[nodiscard]] bool isRunning() const;
  bool loadRequest(const QString &path);
  bool saveRequest(const QString &path);
  bool saveReport(const QString &path);
public slots:
  void startInspection();
  void requestSuggestion();
  void applySuggestion();
  void createVersion();
  void cancelOperation();
signals:
  void inspectionFinished(const QJsonObject &report);
  void versionCreated(const QString &datasetPath, const QJsonObject &summary);
protected:
  void reject() override;
  void closeEvent(QCloseEvent *) override;
private:
  class State; std::unique_ptr<State> m;
  void invalidate();
  void startOperation(const QString &operation);
  void finishProcess(int exitCode, bool normalExit);
  void readOutput(bool standardError);
  void refresh();
  void fail(const char *source, const QString &detail = {});
};
}
