#pragma once

#include <QBitArray>
#include <QVector>

namespace gsw {

enum class SelectionOperation {
  Replace,
  Add,
  Subtract,
};

class SceneEditModel final {
public:
  void reset(qsizetype pointCount);
  void applySelection(const QVector<quint32> &indices, SelectionOperation operation);
  void clearSelection();
  void invertSelection();

  [[nodiscard]] qsizetype deleteSelection();
  [[nodiscard]] qsizetype undo();
  [[nodiscard]] qsizetype redo();

  void markExported();

  [[nodiscard]] qsizetype pointCount() const;
  [[nodiscard]] qsizetype selectedCount() const;
  [[nodiscard]] qsizetype deletedCount() const;
  [[nodiscard]] bool canUndo() const;
  [[nodiscard]] bool canRedo() const;
  [[nodiscard]] bool hasUnsavedChanges() const;
  [[nodiscard]] const QBitArray &selectedBits() const;
  [[nodiscard]] const QBitArray &deletedBits() const;

private:
  struct DeleteCommand {
    QVector<quint32> indices;
  };

  static constexpr qsizetype MaximumHistoryEntries = 32;

  void setSelected(qsizetype index, bool selected);
  void setDeleted(qsizetype index, bool deleted);

  QBitArray mSelected;
  QBitArray mDeleted;
  qsizetype mSelectedCount = 0;
  qsizetype mDeletedCount = 0;
  QVector<DeleteCommand> mUndoStack;
  QVector<DeleteCommand> mRedoStack;
  quint64 mRevision = 0;
  quint64 mExportedRevision = 0;
};

} // namespace gsw
