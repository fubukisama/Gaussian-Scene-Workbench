#include "SceneEditModel.h"

#include <algorithm>

namespace gsw {

void SceneEditModel::reset(const qsizetype pointCount) {
  mSelected = QBitArray(std::max<qsizetype>(0, pointCount), false);
  mDeleted = QBitArray(std::max<qsizetype>(0, pointCount), false);
  mSelectedCount = 0;
  mDeletedCount = 0;
  mUndoStack.clear();
  mRedoStack.clear();
  mRevision = 0;
  mExportedRevision = 0;
}

void SceneEditModel::applySelection(const QVector<quint32> &indices,
                                    const SelectionOperation operation) {
  if (operation == SelectionOperation::Replace) {
    clearSelection();
  }

  const bool selected = operation != SelectionOperation::Subtract;
  for (const quint32 sourceIndex : indices) {
    const qsizetype index = static_cast<qsizetype>(sourceIndex);
    if (index < 0 || index >= mSelected.size() || mDeleted.testBit(index)) {
      continue;
    }
    setSelected(index, selected);
  }
}

void SceneEditModel::clearSelection() {
  mSelected.fill(false);
  mSelectedCount = 0;
}

void SceneEditModel::invertSelection() {
  for (qsizetype index = 0; index < mSelected.size(); ++index) {
    setSelected(index, !mDeleted.testBit(index) && !mSelected.testBit(index));
  }
}

qsizetype SceneEditModel::deleteSelection() {
  if (mSelectedCount == 0) {
    return 0;
  }

  DeleteCommand command;
  command.indices.reserve(mSelectedCount);
  for (qsizetype index = 0; index < mSelected.size(); ++index) {
    if (!mSelected.testBit(index) || mDeleted.testBit(index)) {
      continue;
    }
    command.indices.append(static_cast<quint32>(index));
    setSelected(index, false);
    setDeleted(index, true);
  }

  if (command.indices.isEmpty()) {
    return 0;
  }
  if (mUndoStack.size() >= MaximumHistoryEntries) {
    mUndoStack.removeFirst();
  }
  const qsizetype deleted = command.indices.size();
  mUndoStack.append(std::move(command));
  mRedoStack.clear();
  ++mRevision;
  return deleted;
}

qsizetype SceneEditModel::undo() {
  if (mUndoStack.isEmpty()) {
    return 0;
  }
  DeleteCommand command = mUndoStack.takeLast();
  for (const quint32 sourceIndex : command.indices) {
    const qsizetype index = static_cast<qsizetype>(sourceIndex);
    setDeleted(index, false);
    setSelected(index, true);
  }
  const qsizetype restored = command.indices.size();
  mRedoStack.append(std::move(command));
  ++mRevision;
  return restored;
}

qsizetype SceneEditModel::redo() {
  if (mRedoStack.isEmpty()) {
    return 0;
  }
  DeleteCommand command = mRedoStack.takeLast();
  for (const quint32 sourceIndex : command.indices) {
    const qsizetype index = static_cast<qsizetype>(sourceIndex);
    setSelected(index, false);
    setDeleted(index, true);
  }
  const qsizetype deleted = command.indices.size();
  mUndoStack.append(std::move(command));
  ++mRevision;
  return deleted;
}

void SceneEditModel::markExported() { mExportedRevision = mRevision; }

qsizetype SceneEditModel::pointCount() const { return mSelected.size(); }

qsizetype SceneEditModel::selectedCount() const { return mSelectedCount; }

qsizetype SceneEditModel::deletedCount() const { return mDeletedCount; }

bool SceneEditModel::canUndo() const { return !mUndoStack.isEmpty(); }

bool SceneEditModel::canRedo() const { return !mRedoStack.isEmpty(); }

bool SceneEditModel::hasUnsavedChanges() const {
  return mDeletedCount > 0 && mRevision != mExportedRevision;
}

const QBitArray &SceneEditModel::selectedBits() const { return mSelected; }

const QBitArray &SceneEditModel::deletedBits() const { return mDeleted; }

void SceneEditModel::setSelected(const qsizetype index, const bool selected) {
  const bool current = mSelected.testBit(index);
  if (current == selected) {
    return;
  }
  mSelected.setBit(index, selected);
  mSelectedCount += selected ? 1 : -1;
}

void SceneEditModel::setDeleted(const qsizetype index, const bool deleted) {
  const bool current = mDeleted.testBit(index);
  if (current == deleted) {
    return;
  }
  mDeleted.setBit(index, deleted);
  mDeletedCount += deleted ? 1 : -1;
}

} // namespace gsw
