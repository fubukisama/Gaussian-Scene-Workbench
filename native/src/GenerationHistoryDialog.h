#pragma once

#include "GenerationHistoryStore.h"

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace gsw {

class GenerationHistoryDialog final : public QDialog {
  Q_OBJECT
public:
  enum class Action { None, ViewResult, Resume };
  explicit GenerationHistoryDialog(const QString &projectRoot, QWidget *parent = nullptr);
  [[nodiscard]] Action selectedAction() const { return mAction; }
  [[nodiscard]] GenerationExperiment selectedRecord() const;
  void setResumeAllowed(bool allowed);

private:
  void refreshPresentation();
  void refreshSelection();
  void choose(Action action);
  GenerationHistoryStore mStore;
  QList<GenerationExperiment> mRecords;
  QString mLoadError;
  QTreeWidget *mList = nullptr;
  QLabel *mDetails = nullptr;
  QLabel *mNotice = nullptr;
  QPlainTextEdit *mParameters = nullptr;
  QPushButton *mViewResult = nullptr;
  QPushButton *mResume = nullptr;
  Action mAction = Action::None;
  bool mResumeAllowed = true;
};

} // namespace gsw
