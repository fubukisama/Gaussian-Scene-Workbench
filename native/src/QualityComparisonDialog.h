#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QPointer>
#include <array>
#include <memory>

class QCheckBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QProcess;
class QPushButton;
class QTableWidget;
class QTemporaryDir;
class QTimer;

namespace gsw {
class QualityMeasurementDialog;

class QualityComparisonDialog final : public QDialog {
  Q_OBJECT
public:
  QualityComparisonDialog(const QString &python, const QString &backendRoot,
                          const QString &workspaceRoot,
                          const QProcessEnvironment &environment,
                          QWidget *parent = nullptr);
  ~QualityComparisonDialog() override;
  void setReportPaths(const QString &baseline, const QString &candidate);
  void setContract(const QJsonObject &contract);
  [[nodiscard]] QJsonObject contract() const;
  [[nodiscard]] QJsonObject comparisonReport() const;
  [[nodiscard]] bool isRunning() const;
  bool saveComparisonReport(const QString &path);

public slots:
  void startComparison();
  void cancelComparison();
  void showMeasurement();

signals:
  void comparisonFinished(const QJsonObject &report);

protected:
  void reject() override;
  void closeEvent(QCloseEvent *event) override;

private:
  struct Limit {
    QCheckBox *enabled = nullptr;
    QLineEdit *value = nullptr;
  };
  struct Metric {
    QString identifier;
    const char *caption = nullptr;
    std::array<Limit, 3> limits;
  };
  void refreshPresentation();
  void refreshEnabled();
  void chooseReport(QLineEdit *target);
  void readOutput(bool standardError);
  void finishProcess(int exitCode, bool normalExit);
  void fail(const char *message, const QString &detail = {});
  QJsonObject buildContract(bool *valid) const;

  QString mPython;
  QString mBackendRoot;
  QString mWorkspaceRoot;
  QProcessEnvironment mEnvironment;
  std::array<Metric, 6> mMetrics;
  QLineEdit *mBaseline = nullptr;
  QLineEdit *mCandidate = nullptr;
  QCheckBox *mImageRequired = nullptr;
  QCheckBox *mGeometryRequired = nullptr;
  QWidget *mInputs = nullptr;
  QTableWidget *mLimits = nullptr;
  QTableWidget *mResults = nullptr;
  QTableWidget *mChanges = nullptr;
  QLabel *mSummary = nullptr;
  QLabel *mProtocol = nullptr;
  QPushButton *mCompare = nullptr;
  QPushButton *mMeasure = nullptr;
  QPushButton *mSave = nullptr;
  QPushButton *mCancel = nullptr;
  QProcess *mProcess = nullptr;
  QTimer *mTimeout = nullptr;
  std::unique_ptr<QTemporaryDir> mScratch;
  QJsonObject mReport;
  QByteArray mOutput;
  QByteArray mErrors;
  QString mProtocolIdentifier;
  const char *mFailure = nullptr;
  QString mFailureDetail;
  const char *mSaveMessage = nullptr;
  QString mSaveDetail;
  bool mBusy = false;
  bool mCancelled = false;
  bool mClosePending = false;
  quint64 mRunId = 0;
  QPointer<QualityMeasurementDialog> mMeasurementDialog;
};

} // namespace gsw
