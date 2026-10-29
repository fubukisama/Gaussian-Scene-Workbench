// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QString>

namespace gsw {
// Independent native availability/recovery entry. Opening or closing this
// window is not update admission, application health, or a rollback action.
class NativeRecoveryWindow final : public QDialog {
  Q_OBJECT
public:
  explicit NativeRecoveryWindow(const QString &installationRoot, QWidget *parent = nullptr);
};
} // namespace gsw
