#pragma once

#include "DatasetImportPlan.h"

#include <QDialog>
#include <QString>
#include <QStringList>

#include <optional>

class QCheckBox;
class QDoubleSpinBox;
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
                      bool createsProject,
                      QWidget *parent = nullptr);

  [[nodiscard]] DatasetImportRequest request() const;
  [[nodiscard]] const std::optional<DatasetImportPlan> &validatedPlan() const;

protected:
  void accept() override;

private:
  void addFiles();
  void addDirectory();
  void removeSelected();
  void clearSources();
  void appendSourcePaths(const QStringList &paths);
  void refreshSummary();

  QString mInitialDirectory;
  QLineEdit *mSceneName = nullptr;
  QListWidget *mSourceList = nullptr;
  QDoubleSpinBox *mFramesPerSecond = nullptr;
  QCheckBox *mOverwrite = nullptr;
  QLabel *mSummary = nullptr;
  QPushButton *mRemoveButton = nullptr;
  QPushButton *mClearButton = nullptr;
  QPushButton *mImportButton = nullptr;
  std::optional<DatasetImportPlan> mValidatedPlan;
};

} // namespace gsw
