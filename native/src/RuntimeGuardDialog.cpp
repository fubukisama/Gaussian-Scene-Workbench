#include "RuntimeGuardDialog.h"
#include "AppLanguage.h"
#include "WrappingCheckBox.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

namespace gsw {
RuntimeGuardDialog::RuntimeGuardDialog(const QJsonObject &guard, QWidget *parent)
    : QDialog(parent), mOriginal(guard.isEmpty() ? QJsonObject{
        {"enabled",false},{"mode","pause"},{"usage_fraction",0.9},
        {"minimum_spare_mib",512},{"max_consecutive_denials",3}} : guard) {
  setObjectName(QStringLiteral("runtimeGuardDialog"));
  AppLanguage::bind(this,"windowTitle",AppLanguage::source("续训显存保护设置"));
  setMinimumSize(340,240); resize(620,390);
  const QSet<QString> allowed{QStringLiteral("enabled"),QStringLiteral("mode"),QStringLiteral("usage_fraction"),
      QStringLiteral("minimum_spare_mib"),QStringLiteral("max_consecutive_denials")};
  for(auto it=mOriginal.begin();it!=mOriginal.end();++it) if(!allowed.contains(it.key()))mValidInput=false;
  const auto integer=[this](const char *key,const int low,const int high){const auto v=mOriginal.value(QLatin1String(key));
    return v.isDouble()&&std::isfinite(v.toDouble())&&std::floor(v.toDouble())==v.toDouble()&&v.toDouble()>=low&&v.toDouble()<=high;};
  const auto fraction=mOriginal.value(QStringLiteral("usage_fraction"));
  mValidInput=mValidInput&&mOriginal.value(QStringLiteral("enabled")).isBool()&&
      QStringList{QStringLiteral("pause"),QStringLiteral("defer")}.contains(mOriginal.value(QStringLiteral("mode")).toString())&&
      fraction.isDouble()&&std::isfinite(fraction.toDouble())&&fraction.toDouble()>=0.5&&fraction.toDouble()<=1.0&&
      integer("minimum_spare_mib",0,1048576)&&integer("max_consecutive_denials",1,1000000);
  auto *layout=new QVBoxLayout(this);
  auto *notice=AppLanguage::text(new QLabel(this),AppLanguage::source(
      "这些设置仅在完整状态续训时生效，不热切换当前训练，也不启动任务。相机、尺度、随机状态、优化器与执行优化保持不变。"));
  notice->setWordWrap(true);layout->addWidget(notice);
  auto *form=new QFormLayout;form->setRowWrapPolicy(QFormLayout::WrapLongRows);layout->addLayout(form);
  mEnabled=AppLanguage::text(new WrappingCheckBox(this),AppLanguage::source("启用显存增密保护（独立于性能优化）"));
  mEnabled->setObjectName(QStringLiteral("resumeResourceGuard"));mEnabled->setChecked(mOriginal.value("enabled").toBool());form->addRow(mEnabled);
  mMode=new QComboBox(this);mMode->setObjectName(QStringLiteral("resumeGuardMode"));
  mMode->addItem(QString(),QStringLiteral("pause"));AppLanguage::bindComboItem(mMode,0,AppLanguage::source("质量优先：保存完整状态并安全暂停"));
  mMode->addItem(QString(),QStringLiteral("defer"));AppLanguage::bindComboItem(mMode,1,AppLanguage::source("继续训练：暂缓增密并提示质量风险"));
  mMode->setCurrentIndex(mMode->findData(mOriginal.value("mode").toString()));
  form->addRow(QCoreApplication::translate("Workbench","显存持续不足时"),mMode);
  AppLanguage::text(qobject_cast<QLabel*>(form->labelForField(mMode)),AppLanguage::source("显存持续不足时"));
  mUsage=new QDoubleSpinBox(this);mUsage->setObjectName(QStringLiteral("resumeGuardMemoryPercent"));mUsage->setRange(50.0,100.0);
  mUsage->setDecimals(8);mUsage->setSuffix(QStringLiteral("%"));mUsage->setValue(fraction.toDouble()*100.0);
  connect(mUsage,&QDoubleSpinBox::valueChanged,this,[this]{mUsageEdited=true;});
  form->addRow(QCoreApplication::translate("Workbench","训练显存目标"),mUsage);
  AppLanguage::text(qobject_cast<QLabel*>(form->labelForField(mUsage)),AppLanguage::source("训练显存目标"));
  mSpare=new QSpinBox(this);mSpare->setObjectName(QStringLiteral("resumeGuardMinimumSpareMiB"));mSpare->setRange(0,1048576);
  mSpare->setSuffix(QStringLiteral(" MiB"));mSpare->setValue(mOriginal.value("minimum_spare_mib").toInt());
  form->addRow(QCoreApplication::translate("Workbench","最低空闲显存"),mSpare);
  AppLanguage::text(qobject_cast<QLabel*>(form->labelForField(mSpare)),AppLanguage::source("最低空闲显存"));
  mDenials=new QSpinBox(this);mDenials->setObjectName(QStringLiteral("resumeGuardMaxConsecutiveDenials"));mDenials->setRange(1,1000000);
  mDenials->setValue(mOriginal.value("max_consecutive_denials").toInt());
  form->addRow(QCoreApplication::translate("Workbench","连续拒绝次数"),mDenials);
  AppLanguage::text(qobject_cast<QLabel*>(form->labelForField(mDenials)),AppLanguage::source("连续拒绝次数"));
  auto *warning=AppLanguage::text(new QLabel(this),AppLanguage::source(
      "关闭保护或将目标设为 100%、余量设为零，都不能保证不会出现 CUDA 内存不足；手动暂停与完整状态保存仍可使用。"));
  warning->setWordWrap(true);layout->addWidget(warning);
  auto *error=AppLanguage::text(new QLabel(this),AppLanguage::source("输入的显存保护配置无效；未修改原配置，请返回检查来源。"));
  error->setObjectName(QStringLiteral("resumeGuardInputError"));error->setWordWrap(true);error->setVisible(!mValidInput);layout->addWidget(error);
  auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);
  AppLanguage::text(buttons->button(QDialogButtonBox::Ok),AppLanguage::source("采用续训保护设置"));
  buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("resumeGuardAccept"));
  AppLanguage::text(buttons->button(QDialogButtonBox::Cancel),AppLanguage::source("取消"));
  buttons->button(QDialogButtonBox::Ok)->setEnabled(mValidInput);layout->addWidget(buttons);
  connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
  const auto refresh=[this]{const QList<QWidget*> controls{mMode,mUsage,mSpare,mDenials};for(auto *w:controls)w->setEnabled(mValidInput&&mEnabled->isChecked());mEnabled->setEnabled(mValidInput);};
  connect(mEnabled,&QCheckBox::toggled,this,refresh);refresh();
}
QJsonObject RuntimeGuardDialog::configuration() const {
  if(!mValidInput)return mOriginal;
  auto result=mOriginal;
  result.insert(QStringLiteral("enabled"),mEnabled->isChecked());
  result.insert(QStringLiteral("mode"),mMode->currentData().toString());
  if(mUsageEdited)result.insert(QStringLiteral("usage_fraction"),mUsage->value()/100.0);
  result.insert(QStringLiteral("minimum_spare_mib"),mSpare->value());
  result.insert(QStringLiteral("max_consecutive_denials"),mDenials->value());
  return result;
}
}
