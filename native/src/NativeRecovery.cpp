// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeRecovery.h"
#include "AppLanguage.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace gsw {
NativeRecoveryWindow::NativeRecoveryWindow(const QString &installationRoot, QWidget *parent)
    : QDialog(parent) {
  setObjectName(QStringLiteral("nativeRecoveryWindow"));
  setAttribute(Qt::WA_ShowWithoutActivating);
  setModal(false);
  resize(640, 230);
  AppLanguage::bind(this, "windowTitle", "版本恢复");
  auto *layout = new QVBoxLayout(this);
  auto *description = AppLanguage::text(new QLabel(this),
      "当前未选择任何恢复版本。关闭此窗口不会更改安装、工程或训练。");
  description->setObjectName(QStringLiteral("recoveryDescription"));
  description->setWordWrap(true);
  description->setTextFormat(Qt::PlainText);
  layout->addWidget(description);
  auto *location = new QLabel(this);
  location->setObjectName(QStringLiteral("recoveryInstallation"));
  location->setWordWrap(true);
  location->setTextFormat(Qt::PlainText);
  const auto refreshLocation = [location, installationRoot] {
    location->setText(QCoreApplication::translate("Workbench", "安装位置：%1").arg(installationRoot));
  };
  AppLanguage::onChanged(location, refreshLocation);
  refreshLocation();
  layout->addWidget(location);
  auto *form = new QFormLayout;
  auto *languageLabel = AppLanguage::text(new QLabel(this), "语言设置");
  auto *language = new QComboBox(this);
  language->setObjectName(QStringLiteral("recoveryLanguage"));
  for (const auto &id : AppLanguage::supported()) language->addItem(AppLanguage::displayName(id), id);
  const auto refreshLanguage = [language] {
    const QSignalBlocker blocker(language);
    language->setCurrentIndex(language->findData(AppLanguage::current()));
  };
  AppLanguage::onChanged(language, refreshLanguage);
  refreshLanguage();
  connect(language, &QComboBox::currentIndexChanged, this, [language, refreshLanguage](int index) {
    if (index >= 0 && !AppLanguage::apply(language->itemData(index).toString())) refreshLanguage();
  });
  form->addRow(languageLabel, language);
  layout->addLayout(form);
  layout->addStretch();
  auto *buttons = new QDialogButtonBox(this);
  auto *close = buttons->addButton(QDialogButtonBox::Close);
  close->setObjectName(QStringLiteral("recoveryClose"));
  AppLanguage::bind(close, "text", "关闭恢复窗口");
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  layout->addWidget(buttons);
  // No automatic worker, network check, pointer publication, application
  // launch or retry loop. Trusted selection/rollback is a separate seam.
}
} // namespace gsw
