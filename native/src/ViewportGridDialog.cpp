#include "ViewportGridDialog.h"
#include "AppLanguage.h"
#include "SceneCoordinates.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

namespace gsw {
namespace {
using DisplayUnit = ViewportGridSettings::DisplayUnit;
using SpacingMode = ViewportGridSettings::SpacingMode;

QString text(const char *source) {
  return QCoreApplication::translate("Workbench", source);
}

QLabel *label(QWidget *parent, const char *source = nullptr) {
  auto *result = new QLabel(parent);
  result->setTextFormat(Qt::PlainText);
  result->setWordWrap(true);
  result->setMinimumWidth(0);
  result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  if (source) AppLanguage::bind(result, "text", source);
  return result;
}

double metresPerDisplayUnit(DisplayUnit unit) {
  switch (unit) {
  case DisplayUnit::Millimetres: return 0.001;
  case DisplayUnit::Centimetres: return 0.01;
  case DisplayUnit::Metres: return 1.0;
  case DisplayUnit::Scene: break;
  }
  return 0.0;
}

// Avoid an overflowing intermediate a*b when the final a*b/c is representable.
// This also works on Windows where long double has double precision/range.
std::optional<double> positiveProductRatio(double a, double b, double c) {
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) ||
      a <= 0.0 || b <= 0.0 || c <= 0.0) return std::nullopt;
  int ea = 0, eb = 0, ec = 0;
  const double ma = std::frexp(a, &ea);
  const double mb = std::frexp(b, &eb);
  const double mc = std::frexp(c, &ec);
  const double value = std::ldexp((ma * mb) / mc, ea + eb - ec);
  return std::isfinite(value) && value > 0.0
      ? std::optional<double>(value) : std::nullopt;
}

std::optional<double> shownSpacing(double raw, DisplayUnit unit,
                                  const std::optional<double> &metresPerCoordinate) {
  if (unit == DisplayUnit::Scene)
    return std::isfinite(raw) && raw > 0.0 ? std::optional<double>(raw) : std::nullopt;
  if (!metresPerCoordinate) return std::nullopt;
  return positiveProductRatio(raw, *metresPerCoordinate, metresPerDisplayUnit(unit));
}

QString formatNumber(double value) {
  return QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest);
}
} // namespace

class ViewportGridDialog::State final {
public:
  ViewportGridSettings accepted;
  SceneCoordinateInfo coordinates;
  std::optional<double> metresPerCoordinate;
  bool initialValid = false;
  bool densityEdited = false;
  bool displayUnitEdited = false;
  double originalDensity = 1.0;
  double cachedRawSpacing = 1.0;
  QString lastFormattedSpacing;
  DisplayUnit currentUnit = DisplayUnit::Scene;
  bool unitRepresentationError = false;
  QComboBox *mode = nullptr;
  QLineEdit *spacing = nullptr;
  QComboBox *subdivisions = nullptr;
  QDoubleSpinBox *density = nullptr;
  QComboBox *unit = nullptr;
  QCheckBox *gridVisible = nullptr;
  QCheckBox *axesVisible = nullptr;
  QLabel *provenance = nullptr;
  QLabel *unknownUnits = nullptr;
  QLabel *validation = nullptr;
  QPushButton *apply = nullptr;
};

ViewportGridDialog::ViewportGridDialog(const ViewportGridSettings &source,
    const SceneCoordinateInfo &coordinates, QWidget *parent)
    : QDialog(parent), m(std::make_unique<State>()) {
  m->accepted = source;
  m->coordinates = coordinates;
  m->metresPerCoordinate = sceneMetresPerCoordinate(coordinates);
  m->initialValid = source.isValid();
  m->originalDensity = source.density;
  m->cachedRawSpacing = source.majorSpacing;
  m->currentUnit = m->metresPerCoordinate ? source.displayUnit : DisplayUnit::Scene;
  auto initialShown = shownSpacing(source.majorSpacing, m->currentUnit, m->metresPerCoordinate);
  if (!initialShown) {
    m->currentUnit = DisplayUnit::Scene;
    initialShown = shownSpacing(source.majorSpacing, m->currentUnit, m->metresPerCoordinate);
  }

  setObjectName(QStringLiteral("viewportGridDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("网格设置"));
  setMinimumSize(340, 260);
  resize(590, 510);
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
  bodyLayout->addWidget(label(body, AppLanguage::source("仅调整视口网格与单位显示，不修改模型坐标、相机或训练尺度；来源尺度声明不等于独立精度验证。")));

  m->gridVisible = AppLanguage::text(new QCheckBox(body), AppLanguage::source("显示网格"));
  m->gridVisible->setObjectName(QStringLiteral("gridVisibleCheck"));
  m->gridVisible->setChecked(source.gridVisible);
  bodyLayout->addWidget(m->gridVisible);
  m->axesVisible = AppLanguage::text(new QCheckBox(body), AppLanguage::source("显示坐标轴"));
  m->axesVisible->setObjectName(QStringLiteral("gridAxesVisibleCheck"));
  m->axesVisible->setChecked(source.axesVisible);
  bodyLayout->addWidget(m->axesVisible);

  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setHorizontalSpacing(12);
  form->setVerticalSpacing(8);
  bodyLayout->addLayout(form);
  const auto caption = [&](const char *sourceText, QWidget *control) {
    auto *title = label(body, sourceText);
    title->setBuddy(control);
    AppLanguage::bind(control, "accessibleName", sourceText);
    return title;
  };

  m->mode = new QComboBox(body);
  m->mode->setObjectName(QStringLiteral("gridSpacingMode"));
  m->mode->addItem({}, static_cast<int>(SpacingMode::Automatic));
  m->mode->addItem({}, static_cast<int>(SpacingMode::Fixed));
  AppLanguage::bindComboItem(m->mode, 0, AppLanguage::source("自动（随缩放）"));
  AppLanguage::bindComboItem(m->mode, 1, AppLanguage::source("固定间距"));
  m->mode->setCurrentIndex(m->mode->findData(static_cast<int>(source.spacingMode)));
  form->addRow(caption(AppLanguage::source("间距模式"), m->mode), m->mode);

  m->spacing = new QLineEdit(body);
  m->spacing->setObjectName(QStringLiteral("gridMajorSpacing"));
  auto locale = QLocale::c();
  locale.setNumberOptions(locale.numberOptions() | QLocale::RejectGroupSeparator);
  auto *validator = new QDoubleValidator(0.0, std::numeric_limits<double>::max(), -1, m->spacing);
  validator->setNotation(QDoubleValidator::ScientificNotation);
  validator->setLocale(locale);
  m->spacing->setValidator(validator);
  m->lastFormattedSpacing = initialShown ? formatNumber(*initialShown) : QString{};
  m->spacing->setText(m->lastFormattedSpacing);
  AppLanguage::bind(m->spacing, "toolTip", AppLanguage::source("输入正数，支持科学计数法，例如 1e-3；数值使用所选显示单位。"));
  form->addRow(caption(AppLanguage::source("主网格间距"), m->spacing), m->spacing);

  m->subdivisions = new QComboBox(body);
  m->subdivisions->setObjectName(QStringLiteral("gridSubdivisions"));
  AppLanguage::bind(m->subdivisions, "toolTip", AppLanguage::source("主格分成多少小格；1 表示不显示细格。"));
  for (const int count : {1, 2, 5, 10})
    m->subdivisions->addItem(QString::number(count), count);
  m->subdivisions->setCurrentIndex(m->subdivisions->findData(source.subdivisions));
  form->addRow(caption(AppLanguage::source("细分数"), m->subdivisions), m->subdivisions);

  m->density = new QDoubleSpinBox(body);
  m->density->setObjectName(QStringLiteral("gridDensity"));
  m->density->setDecimals(2);
  m->density->setRange(0.25, 4.0);
  m->density->setSingleStep(0.25);
  m->density->setSuffix(QStringLiteral(" ×"));
  m->density->setLocale(locale);
  m->density->setKeyboardTracking(false);
  if (std::isfinite(source.density)) m->density->setValue(source.density);
  AppLanguage::bind(m->density, "toolTip", AppLanguage::source("仅在自动模式下调整网格显示密度，不改变场景单位或模型尺度。"));
  form->addRow(caption(AppLanguage::source("自动网格密度"), m->density), m->density);

  m->unit = new QComboBox(body);
  m->unit->setObjectName(QStringLiteral("gridDisplayUnit"));
  m->unit->addItem({}, static_cast<int>(DisplayUnit::Scene));
  m->unit->addItem({}, static_cast<int>(DisplayUnit::Millimetres));
  m->unit->addItem({}, static_cast<int>(DisplayUnit::Centimetres));
  m->unit->addItem({}, static_cast<int>(DisplayUnit::Metres));
  AppLanguage::bindComboItem(m->unit, 1, AppLanguage::source("毫米（mm）"));
  AppLanguage::bindComboItem(m->unit, 2, AppLanguage::source("厘米（cm）"));
  AppLanguage::bindComboItem(m->unit, 3, AppLanguage::source("米（m）"));
  if (!m->metresPerCoordinate) {
    if (auto *model = qobject_cast<QStandardItemModel *>(m->unit->model())) {
      for (int i = 1; i < m->unit->count(); ++i)
        if (auto *item = model->item(i)) item->setEnabled(false);
    }
  }
  m->unit->setCurrentIndex(m->unit->findData(static_cast<int>(m->currentUnit)));
  form->addRow(caption(AppLanguage::source("显示单位"), m->unit), m->unit);

  m->provenance = label(body);
  m->provenance->setObjectName(QStringLiteral("gridUnitProvenance"));
  m->provenance->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  bodyLayout->addWidget(m->provenance);
  m->unknownUnits = label(body, AppLanguage::source("未声明物理尺度：仅使用原始单位 u，不能换算为 mm、cm 或 m。"));
  m->unknownUnits->setVisible(!m->metresPerCoordinate.has_value());
  bodyLayout->addWidget(m->unknownUnits);
  m->validation = label(body);
  m->validation->setObjectName(QStringLiteral("gridValidation"));
  bodyLayout->addWidget(m->validation);
  bodyLayout->addStretch(1);

  auto *buttons = new QDialogButtonBox(this);
  m->apply = AppLanguage::text(buttons->addButton(QString{}, QDialogButtonBox::AcceptRole),
                              AppLanguage::source("确定"));
  m->apply->setObjectName(QStringLiteral("gridApplyButton"));
  m->apply->setDefault(true);
  auto *cancel = AppLanguage::text(buttons->addButton(QString{}, QDialogButtonBox::RejectRole),
                                  AppLanguage::source("取消"));
  cancel->setObjectName(QStringLiteral("gridCancelButton"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &ViewportGridDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m->mode, &QComboBox::currentIndexChanged, this, [this] { refreshValidation(); });
  connect(m->subdivisions, &QComboBox::currentIndexChanged, this, [this] { refreshValidation(); });
  connect(m->spacing, &QLineEdit::textChanged, this, [this] {
    m->unitRepresentationError = false;
    refreshValidation();
  });
  connect(m->density, &QDoubleSpinBox::valueChanged, this, [this] {
    m->densityEdited = true;
    refreshValidation();
  });
  connect(m->unit, &QComboBox::currentIndexChanged, this, [this] { changeDisplayUnit(); });
  AppLanguage::onChanged(this, [this] { refreshPresentation(); });
  refreshPresentation();
}

ViewportGridDialog::~ViewportGridDialog() = default;

ViewportGridSettings ViewportGridDialog::settings() const {
  return m->accepted;
}

std::optional<double> ViewportGridDialog::rawSpacing() const {
  if (!m->spacing->hasAcceptableInput()) return std::nullopt;
  if (m->spacing->text() == m->lastFormattedSpacing &&
      std::isfinite(m->cachedRawSpacing) && m->cachedRawSpacing > 0.0)
    return m->cachedRawSpacing;
  bool ok = false;
  const double shown = m->spacing->validator()->locale().toDouble(m->spacing->text(), &ok);
  if (!ok || !std::isfinite(shown) || shown <= 0.0) return std::nullopt;
  if (m->currentUnit == DisplayUnit::Scene) return shown;
  if (!m->metresPerCoordinate) return std::nullopt;
  return positiveProductRatio(shown, metresPerDisplayUnit(m->currentUnit), *m->metresPerCoordinate);
}

std::optional<ViewportGridSettings> ViewportGridDialog::readControls() const {
  if (!m->initialValid || m->mode->currentIndex() < 0 ||
      m->subdivisions->currentIndex() < 0 || m->unit->currentIndex() < 0)
    return std::nullopt;
  const auto spacing = rawSpacing();
  if (!spacing) return std::nullopt;
  ViewportGridSettings result;
  result.spacingMode = static_cast<SpacingMode>(m->mode->currentData().toInt());
  // A temporarily unknown physical scale (or an unrepresentable conversion)
  // changes only the form's effective display to u. Do not erase a persisted
  // physical-unit preference until the user actually chooses another unit.
  result.displayUnit = m->displayUnitEdited ? m->currentUnit : m->accepted.displayUnit;
  result.majorSpacing = *spacing;
  result.subdivisions = m->subdivisions->currentData().toInt();
  // Opening a bounded spin control must not silently round a persisted value.
  result.density = m->densityEdited ? m->density->value() : m->originalDensity;
  result.gridVisible = m->gridVisible->isChecked();
  result.axesVisible = m->axesVisible->isChecked();
  return result.isValid() ? std::optional<ViewportGridSettings>(result) : std::nullopt;
}

void ViewportGridDialog::refreshPresentation() {
  const QSignalBlocker unitBlocker(m->unit);
  QString symbol = QStringLiteral("u");
  if (m->metresPerCoordinate && m->coordinates.unit != SceneLengthUnit::Unknown) {
    const double nominal = m->coordinates.unit == SceneLengthUnit::Millimetres ? 0.001 :
        m->coordinates.unit == SceneLengthUnit::Centimetres ? 0.01 : 1.0;
    if (std::abs(*m->metresPerCoordinate / nominal - 1.0) < 1e-10)
      symbol = sceneLengthUnitSymbol(m->coordinates.unit);
  }
  m->unit->setItemText(0, text(AppLanguage::source("场景单位（%1）")).arg(symbol));
  m->provenance->setText(text(AppLanguage::source("尺度来源：%1"))
      .arg(sceneLengthUnitDescription(m->coordinates)));
  refreshValidation();
}

void ViewportGridDialog::refreshValidation() {
  const bool automatic = m->mode->currentData().toInt() == static_cast<int>(SpacingMode::Automatic);
  m->spacing->setEnabled(!automatic);
  m->subdivisions->setEnabled(!automatic);
  m->density->setEnabled(automatic);
  const bool valid = readControls().has_value();
  m->apply->setEnabled(valid);
  QString message;
  if (!m->initialValid)
    message = text(AppLanguage::source("输入网格设置无效，无法应用。"));
  else if (!rawSpacing())
    message = text(AppLanguage::source("请输入有限的正数作为主网格间距。"));
  else if (m->unitRepresentationError)
    message = text(AppLanguage::source("当前间距无法用所选单位表示，请改用场景单位或调整间距。"));
  else if (!valid)
    message = text(AppLanguage::source("输入网格设置无效，无法应用。"));
  m->validation->setText(message);
}

void ViewportGridDialog::changeDisplayUnit() {
  if (m->unit->currentIndex() < 0) return;
  const auto requested = static_cast<DisplayUnit>(m->unit->currentData().toInt());
  if (requested == m->currentUnit) return;
  const auto raw = rawSpacing();
  const auto shown = raw ? shownSpacing(*raw, requested, m->metresPerCoordinate) : std::nullopt;
  if (!shown) {
    const QSignalBlocker unitBlocker(m->unit);
    m->unit->setCurrentIndex(m->unit->findData(static_cast<int>(m->currentUnit)));
    m->unitRepresentationError = raw.has_value();
    refreshValidation();
    return;
  }
  m->cachedRawSpacing = *raw;
  m->currentUnit = requested;
  m->displayUnitEdited = true;
  m->lastFormattedSpacing = formatNumber(*shown);
  m->unitRepresentationError = false;
  const QSignalBlocker spacingBlocker(m->spacing);
  m->spacing->setText(m->lastFormattedSpacing);
  refreshValidation();
}

void ViewportGridDialog::accept() {
  const auto value = readControls();
  if (!value) {
    refreshValidation();
    return;
  }
  m->accepted = *value;
  QDialog::accept();
}

} // namespace gsw
