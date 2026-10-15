#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <memory>

class QCloseEvent;

namespace gsw {

class QualityMeasurementDialog final : public QDialog {
  Q_OBJECT
public:
  QualityMeasurementDialog(const QString &python, const QString &backendRoot,
                           const QString &workspaceRoot,
                           const QProcessEnvironment &environment,
                           QWidget *parent = nullptr);
  ~QualityMeasurementDialog() override;
  void setManifestPath(const QString &path);
  void setPairedFolders(const QString &reference, const QString &render, const QString &mask = {});
  [[nodiscard]] QString manifestPath() const;
  [[nodiscard]] QString reportPath() const;
  [[nodiscard]] QJsonObject measurementReport() const;
  [[nodiscard]] bool isRunning() const;

public slots:
  void startMeasurement();
  void cancelMeasurement();
  void refreshFolderPairs();

signals:
  void measurementFinished(const QJsonObject &report);
  void reportChosen(const QString &path, bool useAsBaseline);

protected:
  void reject() override;
  void closeEvent(QCloseEvent *event) override;

private:
  class State;
  std::unique_ptr<State> m;
  void refreshPresentation();
  void refreshProgress();
  void refreshEnabled();
  void readOutput(bool standardError);
  void finishProcess(int exitCode, bool normalExit);
  void fail(const char *source, const QString &detail = {});
  void selectReport(bool useAsBaseline);
  void inputsChanged();
  void refreshPairs();
};

} // namespace gsw
