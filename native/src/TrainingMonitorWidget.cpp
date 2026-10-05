#include "AppLanguage.h"
#include "AppTheme.h"
#include <QCoreApplication>
#include "TrainingMonitorWidget.h"
#include <QJsonArray>

#include <QFontMetrics>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace gsw {

namespace {

QString formattedDuration(const double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) {
    return QStringLiteral("-");
  }
  const qint64 rounded = static_cast<qint64>(std::round(seconds));
  const qint64 hours = rounded / 3600;
  const qint64 minutes = (rounded % 3600) / 60;
  const qint64 remainder = rounded % 60;
  if (hours > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(hours)
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remainder, 2, 10, QLatin1Char('0'));
  }
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(remainder, 2, 10, QLatin1Char('0'));
}

QString stageLabel(const QString &stage) {
  if (stage == QStringLiteral("paused")) return QCoreApplication::translate("Workbench", "已暂停");
  if (stage == QStringLiteral("queued")) {
    return QCoreApplication::translate("Workbench", "排队中");
  }
  if (stage == QStringLiteral("prepare") ||
      stage == QStringLiteral("preparing")) {
    return QCoreApplication::translate("Workbench", "准备数据");
  }
  if (stage == QStringLiteral("colmap")) {
    return QCoreApplication::translate("Workbench", "相机解算 · 稀疏点云生成");
  }
  if (stage == QStringLiteral("train")) {
    return QCoreApplication::translate("Workbench", "训练中");
  }
  if (stage == QStringLiteral("finalizing")) {
    return QCoreApplication::translate("Workbench", "发布模型");
  }
  return stage.isEmpty() ? QCoreApplication::translate("Workbench", "运行中") : stage;
}

QLabel *metricValue(QWidget *parent) {
  auto *label = new QLabel(QStringLiteral("-"), parent);
  QFont font = label->font();
  font.setBold(true);
  label->setFont(font);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QLabel *metricCaption(const char *source, QLabel *value) {
  auto *captionLabel = AppLanguage::text(new QLabel(value->parentWidget()), source);
  captionLabel->setObjectName(QStringLiteral("mutedLabel"));
  captionLabel->setWordWrap(true);
  return captionLabel;
}

} // namespace

class TrainingCurvesWidget final : public QWidget {
public:
  explicit TrainingCurvesWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("trainingCurves"));
    setMinimumHeight(fontMetrics().height() * 6 + 32);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  void setSamples(const QVector<TrainingSample> &samples) {
    mSamples = samples;
    update();
  }

protected:
  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
      setMinimumHeight(fontMetrics().height() * 6 + 32);
      updateGeometry();
    }
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::StyleChange) {
      // Theme changes repaint existing samples; they never modify telemetry.
      update();
    }
  }

  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool light = AppTheme::currentTheme() == UiTheme::Light;
    const QColor canvas = light ? QColor(250, 252, 253) : QColor(15, 18, 20);
    const QColor grid = light ? QColor(211, 221, 225) : QColor(47, 53, 57);
    const QColor lossColor = light ? QColor(17, 114, 95) : QColor(91, 199, 170);
    const QColor psnrColor = light ? QColor(144, 90, 9) : QColor(226, 181, 91);
    const QColor waitingColor = light ? QColor(93, 103, 111) : QColor(126, 134, 139);
    painter.fillRect(rect(), canvas);

    QFont chartFont = font();
    if (chartFont.pointSizeF() > 0.0) {
      chartFont.setPointSizeF(std::max(7.0, chartFont.pointSizeF() * 0.82));
    }
    painter.setFont(chartFont);
    const QFontMetrics metrics(chartFont);
    const int labelHeight = metrics.height();
    const QRectF plot = QRectF(rect()).adjusted(12.0, labelHeight + 10.0,
                                                  -12.0, -10.0);
    if (plot.width() < 10.0 || plot.height() < 10.0) {
      return;
    }

    painter.setPen(grid);
    for (int line = 0; line <= 4; ++line) {
      const qreal y = plot.top() + plot.height() * line / 4.0;
      painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    painter.setPen(lossColor);
    painter.drawText(QPointF(plot.left(), labelHeight), QCoreApplication::translate("Workbench", "Loss"));
    const QString psnrLabel = QStringLiteral("PSNR");
    painter.setPen(psnrColor);
    painter.drawText(QPointF(plot.right() - metrics.horizontalAdvance(psnrLabel),
                             labelHeight),
                     psnrLabel);

    if (mSamples.size() < 2) {
      painter.setPen(waitingColor);
      const QString waiting = QCoreApplication::translate("Workbench", "等待训练采样…");
      painter.drawText(plot, Qt::AlignCenter, waiting);
      return;
    }

    const int firstIteration = mSamples.front().iteration;
    const int lastIteration = std::max(mSamples.back().iteration,
                                       firstIteration + 1);
    drawSeries(painter, plot, lossColor, firstIteration, lastIteration,
               [](const TrainingSample &sample) { return sample.loss; });
    drawSeries(painter, plot, psnrColor, firstIteration, lastIteration,
               [](const TrainingSample &sample) { return sample.psnr; });
  }

private:
  template <typename ValueAccessor>
  void drawSeries(QPainter &painter, const QRectF &plot, const QColor &color,
                  const int firstIteration, const int lastIteration,
                  ValueAccessor valueFor) {
    std::optional<double> minimum;
    std::optional<double> maximum;
    for (const TrainingSample &sample : mSamples) {
      const std::optional<double> value = valueFor(sample);
      if (!value.has_value()) {
        continue;
      }
      minimum = minimum.has_value() ? std::min(*minimum, *value) : *value;
      maximum = maximum.has_value() ? std::max(*maximum, *value) : *value;
    }
    if (!minimum.has_value() || !maximum.has_value()) {
      return;
    }
    double range = *maximum - *minimum;
    if (range < 1e-9) {
      range = std::max(std::abs(*maximum) * 0.1, 1e-3);
      *minimum -= range * 0.5;
      *maximum += range * 0.5;
    }

    QPainterPath path;
    bool started = false;
    for (const TrainingSample &sample : mSamples) {
      const std::optional<double> value = valueFor(sample);
      if (!value.has_value()) {
        continue;
      }
      const qreal x = plot.left() +
                      plot.width() * (sample.iteration - firstIteration) /
                          static_cast<double>(lastIteration - firstIteration);
      const qreal y = plot.bottom() -
                      plot.height() * (*value - *minimum) /
                          (*maximum - *minimum);
      if (!started) {
        path.moveTo(x, y);
        started = true;
      } else {
        path.lineTo(x, y);
      }
    }
    QPen pen(color, 1.8);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.drawPath(path);
  }

  QVector<TrainingSample> mSamples;
};

TrainingMonitorWidget::TrainingMonitorWidget(QWidget *parent) : QWidget(parent) {
  setMinimumSize(0, 0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSizeConstraint(QLayout::SetNoConstraint);
  mScrollArea = new QScrollArea(this);
  mScrollArea->setObjectName(QStringLiteral("trainingMonitorScroll"));
  mScrollArea->setFrameShape(QFrame::NoFrame);
  mScrollArea->setMinimumSize(0, 0);
  mScrollArea->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  mScrollArea->setWidgetResizable(true);
  AppLanguage::bind(mScrollArea, "accessibleName", AppLanguage::source("训练监视内容"));
  AppLanguage::bind(mScrollArea, "accessibleDescription", AppLanguage::source("缩小面板时可滚动查看全部信息；指标会随宽度自动换行。"));
  outer->addWidget(mScrollArea);
  auto *content = new QWidget(mScrollArea);
  content->setObjectName(QStringLiteral("trainingMonitorContent"));
  auto *layout = new QVBoxLayout(content);
  layout->setSizeConstraint(QLayout::SetMinimumSize);
  layout->setContentsMargins(10, 8, 10, 8);
  layout->setSpacing(6);

  auto *heading = new QHBoxLayout();
  mTitle = AppLanguage::text(new QLabel(QCoreApplication::translate("Workbench", "尚未开始训练"), content), AppLanguage::source("尚未开始训练"));
  mTitle->setObjectName(QStringLiteral("trainingTaskTitle"));
  mTitle->setTextFormat(Qt::PlainText);
  mTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  mTitle->setMinimumWidth(0);
  QFont titleFont = mTitle->font();
  titleFont.setBold(true);
  mTitle->setFont(titleFont);
  mState = AppLanguage::text(new QLabel(QCoreApplication::translate("Workbench", "空闲"), content), AppLanguage::source("空闲"));
  mState->setTextFormat(Qt::PlainText);
  mState->setObjectName(QStringLiteral("statusWarn"));
  heading->addWidget(mTitle, 1);
  heading->addWidget(mState);
  layout->addLayout(heading);

  mProgress = new QProgressBar(content);
  mProgress->setRange(0, 100);
  mProgress->setValue(0);
  mProgress->setTextVisible(true);
  layout->addWidget(mProgress);
  mReconstructionQuality = new QLabel(content);
  mReconstructionQuality->setObjectName(QStringLiteral("reconstructionQualityLabel"));
  mReconstructionQuality->setWordWrap(true);
  mReconstructionQuality->hide();
  layout->addWidget(mReconstructionQuality);

  mEffectiveParameters = new QLabel(content);
  mEffectiveParameters->setObjectName(QStringLiteral("trainingEffectiveParameters"));
  mEffectiveParameters->setWordWrap(true);
  mEffectiveParameters->setTextFormat(Qt::PlainText);
  mEffectiveParameters->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mEffectiveParameters->hide();
  layout->addWidget(mEffectiveParameters);

  mMetricsGrid = new QGridLayout();
  mMetricsGrid->setHorizontalSpacing(18);
  mMetricsGrid->setVerticalSpacing(2);
  mIteration = metricValue(content);
  mLoss = metricValue(content);
  mPsnr = metricValue(content);
  mGaussianCount = metricValue(content);
  mSpeed = metricValue(content);
  mElapsed = metricValue(content);
  mRemaining = metricValue(content);
  mMetricValues = {mIteration, mLoss, mPsnr, mGaussianCount, mSpeed, mElapsed, mRemaining};
  mMetricCaptions = {metricCaption(AppLanguage::source("迭代"), mIteration),
      metricCaption(AppLanguage::source("Loss"), mLoss),
      metricCaption(AppLanguage::source("训练 PSNR"), mPsnr),
      metricCaption(AppLanguage::source("高斯数量"), mGaussianCount),
      metricCaption(AppLanguage::source("速度"), mSpeed),
      metricCaption(AppLanguage::source("已用时"), mElapsed),
      metricCaption(AppLanguage::source("预计剩余"), mRemaining)};
  mPrimitiveCountCaption = mMetricCaptions[3];
  for (int index = 0; index < 7; ++index) {
    mMetricValues[index]->setObjectName(QStringLiteral("trainingMetricValue%1").arg(index));
    mMetricValues[index]->setTextFormat(Qt::PlainText);
  }
  layout->addLayout(mMetricsGrid);

  mDensityWarning = new QLabel(content);
  mDensityWarning->setObjectName(QStringLiteral("densityGuardWarning"));
  mDensityWarning->setWordWrap(true);
  mDensityWarning->hide();
  layout->addWidget(mDensityWarning);

  mCurves = new TrainingCurvesWidget(content);
  layout->addWidget(mCurves, 1);
  mScrollArea->setWidget(content);
  mScrollArea->viewport()->installEventFilter(this);
  mTitle->installEventFilter(this);
  relayoutMetrics();
  AppLanguage::onChanged(this, [this]() { retranslateStatus(); });
}

bool TrainingMonitorWidget::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::FontChange) {
    // QSS can notify related widgets separately. Measure after font/style
    // propagation, not against a mixture of old and new font metrics.
    QTimer::singleShot(0, this, [this] { relayoutMetrics(); refreshTaskTitle(); });
  } else if (event->type() == QEvent::Resize) {
    if (watched == mScrollArea->viewport()) relayoutMetrics();
    if (watched == mTitle) refreshTaskTitle();
  }
  return QWidget::eventFilter(watched, event);
}

void TrainingMonitorWidget::refreshTaskTitle() {
  const QString full = mHasTraining ? mTaskTitle
      : QCoreApplication::translate("Workbench", "尚未开始训练");
  QString singleLine = full;
  for (const QChar separator : {QChar('\r'), QChar('\n'), QChar('\t'), QChar(0x2028), QChar(0x2029)})
    singleLine.replace(separator, QLatin1Char(' '));
  mTitle->setText(mTitle->fontMetrics().elidedText(singleLine, Qt::ElideMiddle,
                                                 std::max(0, mTitle->width())));
  // Qt tooltips detect HTML automatically. Escape legal markup in user names
  // so the complete name is displayed literally, never interpreted as HTML.
  mTitle->setToolTip(Qt::convertFromPlainText(full));
}

void TrainingMonitorWidget::relayoutMetrics() {
  const QString iterationWidth = QStringLiteral("%1 / %1").arg(QLocale().toString(200000));
  int tileWidth = mIteration->fontMetrics().horizontalAdvance(iterationWidth);
  for (int index = 0; index < 7; ++index) {
    tileWidth = std::max(tileWidth, mMetricCaptions[index]->fontMetrics().horizontalAdvance(mMetricCaptions[index]->text()));
    tileWidth = std::max(tileWidth, mMetricValues[index]->fontMetrics().horizontalAdvance(mMetricValues[index]->text()));
  }
  const int spacing = mMetricsGrid->horizontalSpacing();
  const int available = std::max(0, mScrollArea->viewport()->width() - 20);
  const int columns = std::clamp((available + spacing) / std::max(1, tileWidth + spacing), 1, 7);
  if (columns == mMetricColumns) return;
  mMetricColumns = columns;
  setProperty("metricColumns", columns);
  for (int index = 0; index < 7; ++index) {
    mMetricsGrid->removeWidget(mMetricCaptions[index]);
    mMetricsGrid->removeWidget(mMetricValues[index]);
    mMetricsGrid->setColumnStretch(index, index < columns ? 1 : 0);
  }
  for (int index = 0; index < 7; ++index) {
    const int row = (index / columns) * 2;
    const int column = index % columns;
    mMetricsGrid->addWidget(mMetricCaptions[index], row, column);
    mMetricsGrid->addWidget(mMetricValues[index], row + 1, column);
  }
}

void TrainingMonitorWidget::retranslateStatus() {
  if (mHasTraining) mTitle->setText(mTaskTitle);
  if (mFinished) {
    mState->setText(mSucceeded ? QCoreApplication::translate("Workbench", "已完成")
        : mPaused ? QCoreApplication::translate("Workbench", "已暂停")
        : mCancelled ? QCoreApplication::translate("Workbench", "已取消")
                     : QCoreApplication::translate("Workbench", "失败"));
  } else if (mHasTraining) {
    mState->setText(mLastStage.isEmpty() ? QCoreApplication::translate("Workbench", "启动中") : stageLabel(mLastStage));
  }
  mPrimitiveCountCaption->setText(mSparsePreview ? QCoreApplication::translate("Workbench", "稀疏点数")
                                                : QCoreApplication::translate("Workbench", "高斯数量"));
  // Repaint existing samples; never ingest a status twice or reset telemetry.
  refreshMetrics();
}

void TrainingMonitorWidget::beginTraining(const QString &taskName,
                                          const QString &backend,
                                          const int expectedIterations) {
  mTelemetry.reset(expectedIterations);
  mHasTraining = true;
  mFinished = false;
  mSparsePreview = false;
  mLastStage.clear();
  mDensityGuardIteration = 0;
  mDensityGuardDeferred = 0;
  mReconstructionStatus = {};
  mTrainingSummary = {};
  mTaskTitle = QStringLiteral("%1 · %2").arg(backend.toUpper(), taskName);
  mTitle->setText(mTaskTitle);
  mState->setText(QCoreApplication::translate("Workbench", "启动中"));
  mPrimitiveCountCaption->setText(QCoreApplication::translate("Workbench", "高斯数量"));
  mProgress->setValue(0);
  refreshMetrics();
}

void TrainingMonitorWidget::updateStatus(const WorkerStatus &status) {
  mTelemetry.ingest(status);
  if (!status.trainingSummary.isEmpty() &&
      (mTrainingSummary.value(QStringLiteral("phase")).toString() != QStringLiteral("loaded") ||
       status.trainingSummary.value(QStringLiteral("phase")).toString() == QStringLiteral("loaded")))
    mTrainingSummary = status.trainingSummary;
  mLastStage = status.stage;
  if (!status.reconstructionQuality.isEmpty() || !status.generationIssue.isEmpty()) {
    mReconstructionStatus = status;
  }
  if (status.densityGuardIteration.has_value() && status.densityGuardDeferred.has_value()) {
    mDensityGuardIteration = *status.densityGuardIteration;
    mDensityGuardDeferred = *status.densityGuardDeferred;
  }
  mSparsePreview = status.previewKind == QStringLiteral("colmap_sparse") || status.stage == QStringLiteral("colmap");
  mState->setText(stageLabel(status.stage));
  mPrimitiveCountCaption->setText(
      status.previewKind == QStringLiteral("colmap_sparse") ||
              status.stage == QStringLiteral("colmap")
          ? QCoreApplication::translate("Workbench", "稀疏点数")
          : QCoreApplication::translate("Workbench", "高斯数量"));
  if (status.progressPercent.has_value()) {
    mProgress->setValue(status.progressPercent.value());
  } else if (mTelemetry.iteration().has_value() &&
             mTelemetry.expectedIterations() > 0) {
    mProgress->setValue(std::clamp(
        static_cast<int>(std::round(
            mTelemetry.iteration().value() * 100.0 /
            mTelemetry.expectedIterations())),
        0, 100));
  }
  refreshMetrics();
}

void TrainingMonitorWidget::finishTraining(const bool succeeded,
                                           const bool cancelled, const bool paused) {
  mFinished = true;
  mSucceeded = succeeded;
  mCancelled = cancelled;
  mPaused = paused;
  mState->setText(succeeded ? QCoreApplication::translate("Workbench", "已完成")
                  : paused ? QCoreApplication::translate("Workbench", "已暂停")
                  : cancelled ? QCoreApplication::translate("Workbench", "已取消")
                              : QCoreApplication::translate("Workbench", "失败"));
  if (succeeded) {
    mProgress->setValue(100);
  }
  refreshMetrics();
}

const TrainingTelemetry &TrainingMonitorWidget::telemetry() const {
  return mTelemetry;
}

void TrainingMonitorWidget::refreshMetrics() {
  const QLocale locale;
  mEffectiveParameters->setVisible(!mTrainingSummary.isEmpty());
  if (!mTrainingSummary.isEmpty()) {
    const auto &summary = mTrainingSummary;
    const QString optimizer = summary.value(QStringLiteral("optimizer")).toString() == QStringLiteral("sparse_adam")
        ? QCoreApplication::translate("Workbench", "稀疏 Adam") : QStringLiteral("Adam");
    const int resolution = summary.value(QStringLiteral("resolution")).toInt();
    const QString resolutionText = (resolution == 1 || resolution == 2 || resolution == 4 || resolution == 8)
        ? QCoreApplication::translate("Workbench", "1/%1 分辨率").arg(resolution)
        : QCoreApplication::translate("Workbench", "目标宽度 %1 px").arg(locale.toString(resolution));
    QString text = QCoreApplication::translate("Workbench", "生效参数：%1 · %2 次迭代 · %3 · 优化器 %4。")
        .arg(summary.value(QStringLiteral("backend")).toString().toUpper(),
             locale.toString(summary.value(QStringLiteral("iterations")).toInt()),
             resolutionText, optimizer);
    if (summary.value(QStringLiteral("phase")).toString() == QStringLiteral("loaded")) {
      QStringList dimensions;
      for (const QJsonValue &value : summary.value(QStringLiteral("trainDimensions")).toArray()) {
        const QJsonArray size = value.toArray();
        dimensions.append(QCoreApplication::translate("Workbench", "%1 × %2 px（%3 张）")
            .arg(locale.toString(size.at(0).toInt()), locale.toString(size.at(1).toInt()),
                 locale.toString(size.at(2).toInt())));
      }
      const int extra = summary.value(QStringLiteral("trainDimensionKinds")).toInt() - dimensions.size();
      if (extra > 0) dimensions.append(QCoreApplication::translate("Workbench", "另有 %1 种尺寸").arg(locale.toString(extra)));
      text += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "实际训练图像：%1 张 · %2 · 总计 %3 MP。")
          .arg(locale.toString(summary.value(QStringLiteral("trainImageCount")).toInt()),
               dimensions.join(QStringLiteral(" · ")),
               locale.toString(summary.value(QStringLiteral("trainPixels")).toDouble() / 1000000.0, 'f', 2));
    } else {
      text += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "实际训练图像尺寸将在相机加载后确认。");
    }
    mEffectiveParameters->setText(text);
    QString details = QCoreApplication::translate("Workbench", "增密截止：%1 · 间隔：%2 · 梯度阈值：%3")
        .arg(locale.toString(summary.value(QStringLiteral("densifyUntil")).toInt()),
             locale.toString(summary.value(QStringLiteral("densificationInterval")).toInt()),
             locale.toString(summary.value(QStringLiteral("densifyGradient")).toDouble(), 'g', 6));
    if (summary.value(QStringLiteral("backend")).toString() == QStringLiteral("3dgs")) {
      const auto enabled = [](bool value) {
        return value ? QCoreApplication::translate("Workbench", "启用") : QCoreApplication::translate("Workbench", "禁用");
      };
      details += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "抗锯齿：%1 · 曝光补偿：%2")
          .arg(enabled(summary.value(QStringLiteral("antialiasing")).toBool()),
               enabled(summary.value(QStringLiteral("exposureCompensation")).toBool()));
    } else {
      details += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "深度混合比例：%1")
          .arg(locale.toString(summary.value(QStringLiteral("depthRatio")).toDouble(), 'g', 6));
    }
    mEffectiveParameters->setToolTip(text + QLatin1Char('\n') + details);
  }
  const WorkerStatus &quality = mReconstructionStatus;
  mReconstructionQuality->setVisible(!quality.reconstructionQuality.isEmpty() || !quality.generationIssue.isEmpty());
  QString message;
  if (quality.generationIssue == QStringLiteral("source_frames_missing")) {
    message = QCoreApplication::translate("Workbench", "原始帧无法完整恢复。请重新导入原始照片或视频；已保留当前模型，不会使用缺失照片的子集继续训练。");
  } else if (!quality.reconstructionQuality.isEmpty()) {
    const QString phase = quality.reconstructionQuality == QStringLiteral("repairing")
        ? QCoreApplication::translate("Workbench", "有限重试中")
        : quality.reconstructionQuality == QStringLiteral("rejected")
        ? QCoreApplication::translate("Workbench", "质量检查未通过 · 已阻止训练")
        : quality.reconstructionQuality == QStringLiteral("partial")
        ? QCoreApplication::translate("Workbench", "部分相机注册 · 覆盖不足")
        : QCoreApplication::translate("Workbench", "最低质量检查通过");
    message = QCoreApplication::translate("Workbench", "重建检查：相机 %1/%2 · 有效稀疏点 %3 · %4。高斯数量和训练 PSNR 不代表新视角质量。")
        .arg(locale.toString(quality.reconstructionViews.value_or(0)),
             locale.toString(quality.reconstructionInputs.value_or(0)),
             locale.toString(quality.reconstructionPoints.value_or(0)), phase);
  }
  mReconstructionQuality->setText(message);
  mDensityWarning->setVisible(mDensityGuardDeferred > 0);
  if (mDensityGuardDeferred > 0) {
    mDensityWarning->setText(QCoreApplication::translate("Workbench",
        "过度裁剪保护：第 %1 次迭代暂缓删除 %2 个高斯。请检查拍摄覆盖与重建尺度；数量不代表几何质量。")
        .arg(locale.toString(mDensityGuardIteration), locale.toString(mDensityGuardDeferred)));
  }
  const int total = mTelemetry.expectedIterations();
  mIteration->setText(mTelemetry.iteration().has_value()
                          ? QStringLiteral("%1 / %2")
                                .arg(locale.toString(*mTelemetry.iteration()))
                                .arg(total > 0 ? locale.toString(total)
                                               : QStringLiteral("-"))
                          : QStringLiteral("- / %1")
                                .arg(total > 0 ? locale.toString(total)
                                               : QStringLiteral("-")));
  mLoss->setText(mTelemetry.loss().has_value()
                     ? locale.toString(*mTelemetry.loss(), 'g', 6)
                     : QStringLiteral("-"));
  mPsnr->setText(mTelemetry.psnr().has_value()
                     ? QStringLiteral("%1 dB").arg(
                           locale.toString(*mTelemetry.psnr(), 'f', 2))
                     : QStringLiteral("-"));
  mGaussianCount->setText(
      mTelemetry.gaussianCount().has_value()
          ? locale.toString(*mTelemetry.gaussianCount())
          : QStringLiteral("-"));
  if (mTelemetry.iterationMilliseconds().has_value() &&
      *mTelemetry.iterationMilliseconds() > 0.0) {
    const double milliseconds = *mTelemetry.iterationMilliseconds();
    mSpeed->setText(QStringLiteral("%1 it/s")
                        .arg(locale.toString(1000.0 / milliseconds, 'f', 1)));
    if (mTelemetry.iteration().has_value() && total > 0) {
      const int remaining = std::max(total - *mTelemetry.iteration(), 0);
      mRemaining->setText(
          formattedDuration(remaining * milliseconds / 1000.0));
    } else {
      mRemaining->setText(QStringLiteral("-"));
    }
  } else {
    mSpeed->setText(QStringLiteral("-"));
    mRemaining->setText(QStringLiteral("-"));
  }
  mElapsed->setText(mTelemetry.elapsedSeconds().has_value()
                        ? formattedDuration(*mTelemetry.elapsedSeconds())
                        : QStringLiteral("-"));
  if (mFinished) mRemaining->setText(QStringLiteral("-"));
  mCurves->setSamples(mTelemetry.samples());
  relayoutMetrics();
  refreshTaskTitle();
}

} // namespace gsw
