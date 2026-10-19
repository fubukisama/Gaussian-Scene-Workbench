#pragma once

#include "DatasetImportPlan.h"

#include <QDialog>
#include <QString>
#include <QStringList>

#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace gsw {

class DatasetImportDialog final : public QDialog {
  Q_OBJECT

public:
  DatasetImportDialog(const QString &initialDirectory,
                      const QString &suggestedSceneName,
                      const QStringList &initialSourcePaths,
                      const QString &projectRoot,
                      bool unsavedProject,
                      QWidget *parent = nullptr);

  [[nodiscard]] DatasetImportRequest request() const;
  [[nodiscard]] const std::optional<DatasetImportPlan> &validatedPlan() const;
  // Opens the same native transactional importer in explicit camera mode.
  // The request remains unvalidated until the shared worker adapter runs.
  void setCameraInput(const QJsonObject &input);

protected:
  void accept() override;
  void changeEvent(QEvent *event) override;

private:
  void addFiles();
  void addDirectory();
  void removeSelected();
  void clearSources();
  void appendSourcePaths(const QStringList &paths);
  void refreshSummary();
  void refreshIcons();
  void refreshInputMode();
  void chooseCameraSource(bool directory);

  QString mInitialDirectory;
  QLineEdit *mSceneName = nullptr;
  QListWidget *mSourceList = nullptr;
  QDoubleSpinBox *mFramesPerSecond = nullptr;
  QCheckBox *mOverwrite = nullptr;
  QLabel *mSummary = nullptr;
  QPushButton *mAddFilesButton = nullptr;
  QPushButton *mAddDirectoryButton = nullptr;
  QPushButton *mRemoveButton = nullptr;
  QPushButton *mClearButton = nullptr;
  QPushButton *mImportButton = nullptr;
  QComboBox *mInputMode = nullptr;
  QComboBox *mCameraAdapter = nullptr;
  QLineEdit *mCameraSource = nullptr;
  QComboBox *mCameraUnit = nullptr;
  QLineEdit *mScaleEvidence = nullptr;
  QComboBox *mNormalization = nullptr;
  QComboBox *mInitialization = nullptr;
  QLineEdit *mInitializationPath = nullptr;
  QLineEdit *mBoundsMinimum = nullptr;
  QLineEdit *mBoundsMaximum = nullptr;
  QWidget *mCameraPanel = nullptr;
  QWidget *mMediaActions = nullptr;
  QLabel *mSourceTitle = nullptr;
  QJsonObject mCameraInputTemplate;
  std::optional<DatasetImportPlan> mValidatedPlan;
};

} // namespace gsw
