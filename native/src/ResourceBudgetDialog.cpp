#include "ResourceBudgetDialog.h"
#include "AppLanguage.h"
#include "NativeViewport.h"
#include "ResourceBudget.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace gsw {
namespace {
QString memorySize(qint64 bytes) {
  constexpr qint64 MiB = 1024LL * 1024LL;
  constexpr qint64 GiB = MiB * 1024LL;
  if (bytes < GiB)
    return QStringLiteral("%1 MiB").arg(QLocale().toString(std::max(qint64(0), bytes) / double(MiB), 'f', 1));
  return QStringLiteral("%1 GiB").arg(QLocale().toString(bytes / double(GiB), 'f', 2));
}

QLabel *valueLabel(QWidget *parent, const char *name) {
  auto *label = new QLabel(parent);
  label->setObjectName(QString::fromLatin1(name));
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}

QLabel *captionLabel(QWidget *parent, const char *source) {
  auto *label = AppLanguage::text(new QLabel(parent), source);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  return label;
}
} // namespace

ResourceBudgetDialog::ResourceBudgetDialog(NativeViewport *viewport, QWidget *parent)
    : QDialog(parent), mViewport(viewport) {
  setObjectName(QStringLiteral("resourceBudgetDialog"));
  setModal(false);
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("资源预算"));
  setMinimumSize(360, 240);
  resize(680, 560);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(10);
  auto *scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("dialogBodyScroll"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *body = new QWidget(scroll);
  body->setObjectName(QStringLiteral("dialogBody"));
  auto *bodyLayout = new QVBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(10);
  scroll->setWidget(body);
  layout->addWidget(scroll, 1);

  auto *intro = AppLanguage::text(new QLabel(body), AppLanguage::source("预算设置即时生效并记住选择；所有网格与待加载网格数据共享同一份预算。此设置控制桌面端网格加载与显示，不修改训练配置，也不是全程序资源硬上限。"));
  intro->setTextFormat(Qt::PlainText);
  intro->setWordWrap(true);
  intro->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  bodyLayout->addWidget(intro);
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setHorizontalSpacing(12);
  form->setVerticalSpacing(8);
  bodyLayout->addLayout(form);
  mMode = new QComboBox(body);
  mMode->setObjectName(QStringLiteral("resourceBudgetMode"));
  mMode->setMinimumContentsLength(12);
  mMode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  mMode->addItem({}, static_cast<int>(ResourceBudgetPolicy::Mode::Automatic));
  mMode->addItem({}, static_cast<int>(ResourceBudgetPolicy::Mode::Manual));
  AppLanguage::bindComboItem(mMode, 0, AppLanguage::source("自动（激进）"));
  AppLanguage::bindComboItem(mMode, 1, AppLanguage::source("手动上限"));
  form->addRow(captionLabel(body, AppLanguage::source("预算模式")), mMode);
  const auto createLimit = [&](const char *name, const char *caption) {
    auto *spin = new QDoubleSpinBox(body);
    spin->setObjectName(QString::fromLatin1(name));
    spin->setDecimals(4);
    spin->setRange(0.0625, 128.0);
    spin->setSingleStep(0.25);
    spin->setSuffix(QStringLiteral(" GiB"));
    spin->setKeyboardTracking(false);
    AppLanguage::bind(spin, "accessibleName", caption);
    form->addRow(captionLabel(body, caption), spin);
    return spin;
  };
  mRamLimit = createLimit("resourceRamLimitGiB", AppLanguage::source("内存上限"));
  mGpuLimit = createLimit("resourceGpuLimitGiB", AppLanguage::source("显存上限"));
  const ResourceBudgetPolicy policy = viewport ? viewport->resourceBudgetPolicy() : ResourceBudgetPolicy{};
  mMode->setCurrentIndex(mMode->findData(static_cast<int>(policy.mode)));
  mRamLimit->setValue(policy.ramLimitMiB / 1024.0);
  mGpuLimit->setValue(policy.gpuLimitMiB / 1024.0);
  const bool manual = policy.mode == ResourceBudgetPolicy::Mode::Manual;
  mRamLimit->setEnabled(manual);
  mGpuLimit->setEnabled(manual);
  auto *explanation = AppLanguage::text(new QLabel(body), AppLanguage::source("自动模式使用当前可用资源的约 90%，至少保留 512 MiB 内存和 256 MiB 显存。手动上限不预分配内存；检测到的余量与硬件限制仍适用，检测失败时不保证安全余量。"));
  explanation->setTextFormat(Qt::PlainText);
  explanation->setWordWrap(true);
  explanation->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  bodyLayout->addWidget(explanation);

  auto *statusForm = new QFormLayout;
  statusForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  statusForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
  statusForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  statusForm->setHorizontalSpacing(12);
  statusForm->setVerticalSpacing(8);
  bodyLayout->addLayout(statusForm);
  mRamAvailable = valueLabel(body, "resourceRamAvailable");
  mGpuAvailableTitle = captionLabel(body, AppLanguage::source("显存检测不可用"));
  mGpuAvailable = valueLabel(body, "resourceGpuAvailable");
  mManagedUsage = valueLabel(body, "resourceManagedUsage");
  mReservedUsage = valueLabel(body, "resourceReservedUsage");
  mEffectiveBudget = valueLabel(body, "resourceEffectiveBudget");
  mMeshCounts = valueLabel(body, "resourceMeshCounts");
  mProbeDescription = valueLabel(body, "resourceProbeDescription");
  statusForm->addRow(captionLabel(body, AppLanguage::source("当前可用内存")), mRamAvailable);
  statusForm->addRow(mGpuAvailableTitle, mGpuAvailable);
  statusForm->addRow(captionLabel(body, AppLanguage::source("程序管理的模型占用（估算）")), mManagedUsage);
  statusForm->addRow(captionLabel(body, AppLanguage::source("加载预留")), mReservedUsage);
  statusForm->addRow(captionLabel(body, AppLanguage::source("当前生效预算")), mEffectiveBudget);
  statusForm->addRow(captionLabel(body, AppLanguage::source("网格状态")), mMeshCounts);
  bodyLayout->addWidget(mProbeDescription);
  bodyLayout->addStretch(1);
  auto *buttons = new QDialogButtonBox(this);
  auto *close = AppLanguage::text(buttons->addButton(QString(), QDialogButtonBox::RejectRole),
                                 AppLanguage::source("关闭"));
  close->setObjectName(QStringLiteral("resourceBudgetClose"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  connect(mMode, &QComboBox::currentIndexChanged, this, [this] { applyControls(); });
  connect(mRamLimit, &QDoubleSpinBox::valueChanged, this, [this] { applyControls(); });
  connect(mGpuLimit, &QDoubleSpinBox::valueChanged, this, [this] { applyControls(); });
  if (mViewport)
    connect(mViewport, &NativeViewport::resourceBudgetStatusChanged, this, [this] { refreshStatus(); });
  auto *refresh = new QTimer(this);
  refresh->setInterval(1000);
  connect(refresh, &QTimer::timeout, this, [this] { refreshStatus(); });
  refresh->start();
  AppLanguage::onChanged(this, [this] {
    mRamLimit->setLocale(QLocale());
    mGpuLimit->setLocale(QLocale());
    refreshStatus();
  });
  refreshStatus();
}

void ResourceBudgetDialog::applyControls() {
  if (!mViewport) return;
  ResourceBudgetPolicy policy;
  policy.mode = static_cast<ResourceBudgetPolicy::Mode>(mMode->currentData().toInt());
  policy.ramLimitMiB = qRound64(mRamLimit->value() * 1024.0);
  policy.gpuLimitMiB = qRound64(mGpuLimit->value() * 1024.0);
  mRamLimit->setEnabled(policy.mode == ResourceBudgetPolicy::Mode::Manual);
  mGpuLimit->setEnabled(policy.mode == ResourceBudgetPolicy::Mode::Manual);
  mViewport->setResourceBudgetPolicy(policy);
  const auto actual = mViewport->resourceBudgetPolicy();
  QSettings settings;
  settings.setValue(QStringLiteral("view/resourceBudgetMode"), actual.mode == ResourceBudgetPolicy::Mode::Manual
      ? QStringLiteral("manual") : QStringLiteral("auto"));
  settings.setValue(QStringLiteral("view/resourceRamLimitMiB"), actual.ramLimitMiB);
  settings.setValue(QStringLiteral("view/resourceGpuLimitMiB"), actual.gpuLimitMiB);
  refreshStatus();
}

void ResourceBudgetDialog::refreshStatus() {
  if (!mViewport) return;
  const auto status = mViewport->resourceBudgetStatus();
  const auto &snapshot = status.snapshot;
  const bool manual = mViewport->resourceBudgetPolicy().mode == ResourceBudgetPolicy::Mode::Manual;
  const auto unknown = QCoreApplication::translate("Workbench", "无法检测，使用安全回退");
  mRamAvailable->setText(snapshot.systemMemoryKnown
      ? QCoreApplication::translate("Workbench", "物理可用 %1 · 可提交 %2")
            .arg(memorySize(snapshot.physicalAvailableBytes), memorySize(snapshot.commitAvailableBytes))
      : unknown);
  switch (snapshot.gpuSource) {
  case GpuProbeSource::DxgiProcessBudget:
    AppLanguage::bind(mGpuAvailableTitle, "text", AppLanguage::source("本进程显存预算余量"));
    mGpuAvailable->setText(memorySize(snapshot.gpuAvailableBytes));
    mProbeDescription->setText(QCoreApplication::translate("Workbench", "DXGI：%1。显示的是本进程显存预算余量，不是设备总显存或全系统空闲显存。")
                                  .arg(snapshot.adapterName));
    break;
  case GpuProbeSource::NvxApproximate:
    AppLanguage::bind(mGpuAvailableTitle, "text", AppLanguage::source("近似可用显存"));
    mGpuAvailable->setText(memorySize(snapshot.gpuAvailableBytes));
    mProbeDescription->setText(QCoreApplication::translate("Workbench", "NVX：近似可用显存；其数值不是已预留的资源。"));
    break;
  case GpuProbeSource::Unavailable:
    AppLanguage::bind(mGpuAvailableTitle, "text", AppLanguage::source("显存检测不可用"));
    mGpuAvailable->setText(manual
        ? QCoreApplication::translate("Workbench", "无法检测（手动上限不是可用量）") : unknown);
    mProbeDescription->setText(manual
        ? QCoreApplication::translate("Workbench", "未检测到显存余量。手动上限不是测量值，无法保证保留安全余量。")
        : QCoreApplication::translate("Workbench", "未获得可靠的显存余量，使用安全回退预算。"));
    break;
  }
  const auto pair = [](qint64 ram, qint64 gpu) {
    return QCoreApplication::translate("Workbench", "内存 %1 · 显存 %2").arg(memorySize(ram), memorySize(gpu));
  };
  mManagedUsage->setText(pair(status.managedRamBytes, status.managedGpuBytes));
  mReservedUsage->setText(pair(status.reservedRamBytes, status.reservedGpuBytes));
  mEffectiveBudget->setText(pair(status.effectiveRamBudgetBytes, status.effectiveGpuBudgetBytes));
  mMeshCounts->setText(QCoreApplication::translate("Workbench", "常驻 %1 · 分页 %2 · 加载中 %3")
                          .arg(status.residentMeshCount).arg(status.pagedMeshCount).arg(status.loadingMeshCount));
}
} // namespace gsw
