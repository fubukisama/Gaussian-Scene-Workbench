#include "InitializationEditor.h"
#include "AppLanguage.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <array>
#include <cmath>

namespace gsw {
namespace {
QString translated(const char *source) { return QCoreApplication::translate("Workbench", source); }
QString jsonText(const QJsonValue &value) {
  const auto bytes = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(bytes.mid(1, bytes.size() - 2));
}
QJsonValue jsonValue(const QString &text, bool *valid = nullptr) {
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson((QLatin1Char('[') + text + QLatin1Char(']')).toUtf8(), &error);
  const bool ok = error.error == QJsonParseError::NoError && document.isArray() && document.array().size() == 1;
  if (valid) *valid = ok;
  return ok ? document.array().at(0) : QJsonValue(text);
}
bool finiteNumber(const QString &text, double *value = nullptr) {
  bool ok = false; const double number = QLocale::c().toDouble(text.trimmed(), &ok);
  if (value) *value = number;
  return ok && std::isfinite(number);
}
QString numberText(const QJsonValue &value) {
  return value.isDouble() ? QString::number(value.toDouble(), 'g', 17) : value.isNull() || value.isUndefined() ? QString() : value.toVariant().toString();
}
}
class InitializationEditor::State {
public:
  QJsonObject original;
  bool loading = false, allowPreserve = false, pathEdited = false;
  QString frame = QStringLiteral("source"), directory;
  QComboBox *kind = nullptr, *frames = nullptr;
  QLineEdit *count = nullptr, *seed = nullptr, *path = nullptr, *priors = nullptr;
  QPlainTextEdit *provenance = nullptr;
  std::array<QLineEdit *, 6> bounds{};
  QPushButton *browse = nullptr;
  QLabel *warning = nullptr, *hint = nullptr;
  const char *frameWarning = nullptr;
};

InitializationEditor::InitializationEditor(const bool allowPreserve, QWidget *parent)
    : QWidget(parent), m(std::make_unique<State>()) {
  m->allowPreserve = allowPreserve;
  setObjectName(QStringLiteral("initializationEditor"));
  setProperty("gswLayoutContainer", true);
  auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
  auto *form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows); layout->addLayout(form);
  const auto row = [&](const char *caption, QWidget *widget) {
    auto *label = AppLanguage::text(new QLabel(this), caption);
    label->setBuddy(widget);
    form->addRow(label, widget);
  };
  m->kind = new QComboBox(this); m->kind->setObjectName(QStringLiteral("initializationKind"));
  const auto addKind = [&](const QString &id, const char *caption) {
    m->kind->addItem(QString(), QVariant(id)); AppLanguage::bindComboItem(m->kind, m->kind->count() - 1, caption);
  };
  if (allowPreserve) addKind(QStringLiteral("preserve"), AppLanguage::source("保留来源初始化；未声明则随机初始化"));
  addKind(QStringLiteral("random"), AppLanguage::source("随机初始化（无几何先验）"));
  addKind(QStringLiteral("external_point_cloud"), AppLanguage::source("外部点云初始化（含几何先验）"));
  addKind(QStringLiteral("sfm_sparse"), AppLanguage::source("SfM 稀疏点初始化"));
  row(AppLanguage::source("初始化来源"), m->kind);
  m->frames = new QComboBox(this); m->frames->setObjectName(QStringLiteral("initializationFrame"));
  m->frames->addItem(QString(), QVariant(QStringLiteral("source")));
  m->frames->addItem(QString(), QVariant(QStringLiteral("canonical")));
  AppLanguage::bindComboItem(m->frames, 0, AppLanguage::source("源坐标（沿用源单位）"));
  AppLanguage::bindComboItem(m->frames, 1, AppLanguage::source("规范坐标（已记录的尺度）"));
  row(AppLanguage::source("输入坐标框架"), m->frames);
  auto *frameHint = AppLanguage::text(new QLabel(this), AppLanguage::source("训练坐标仅由已记录变换派生，不能作为输入框架标签。"));
  frameHint->setWordWrap(true); layout->addWidget(frameHint);
  m->count = new QLineEdit(this); m->count->setObjectName(QStringLiteral("initializationCount")); m->count->setMaxLength(16);
  m->seed = new QLineEdit(this); m->seed->setObjectName(QStringLiteral("initializationSeed")); m->seed->setMaxLength(16);
  row(AppLanguage::source("初始化点数"), m->count); row(AppLanguage::source("初始化随机种子（uint32）"), m->seed);
  auto *pathRow = new QWidget(this); pathRow->setProperty("gswLayoutContainer", true);
  auto *pathLayout = new QHBoxLayout(pathRow); pathLayout->setContentsMargins(0,0,0,0);
  m->path = new QLineEdit(pathRow); m->path->setObjectName(QStringLiteral("initializationPath"));
  m->browse = AppLanguage::text(new QPushButton(pathRow), AppLanguage::source("选择文件…"));
  pathLayout->addWidget(m->path, 1); pathLayout->addWidget(m->browse); row(AppLanguage::source("初始化点云"), pathRow);
  for (int bound = 0; bound < 2; ++bound) {
    auto *box = new QWidget(this); box->setProperty("gswLayoutContainer", true);
    auto *values = new QHBoxLayout(box); values->setContentsMargins(0,0,0,0);
    for (int axis = 0; axis < 3; ++axis) {
      auto *input = new QLineEdit(box); input->setObjectName(QStringLiteral("initializationBound%1%2").arg(bound).arg(axis));
      input->setPlaceholderText(QString::fromLatin1("XYZ" + axis, 1));
      m->bounds[bound * 3 + axis] = input; values->addWidget(input);
    }
    row(bound == 0 ? AppLanguage::source("粗范围最小值") : AppLanguage::source("粗范围最大值"), box);
  }
  auto *clear = AppLanguage::text(new QPushButton(this), AppLanguage::source("清空范围与点云选择"));
  clear->setObjectName(QStringLiteral("initializationClearCoordinates")); layout->addWidget(clear);
  m->priors = new QLineEdit(this); m->priors->setObjectName(QStringLiteral("initializationPriors"));
  row(AppLanguage::source("先验声明（JSON 数组）"), m->priors);
  m->provenance = new QPlainTextEdit(this); m->provenance->setObjectName(QStringLiteral("initializationProvenance"));
  m->provenance->setMinimumHeight(60); m->provenance->setMaximumHeight(130);
  row(AppLanguage::source("初始化来源记录（JSON）"), m->provenance);
  m->hint = AppLanguage::text(new QLabel(this), AppLanguage::source("相机位置不能唯一确定物体深度。粗范围是声明或假设，不是已测表面；随机点保存为文件也不构成几何先验。"));
  m->hint->setWordWrap(true); layout->addWidget(m->hint);
  m->warning = new QLabel(this); m->warning->setObjectName(QStringLiteral("initializationValidation"));
  m->warning->setWordWrap(true); m->warning->setTextFormat(Qt::PlainText); layout->addWidget(m->warning);
  const auto changed = [this] { if (m->loading) return; refresh(); emit initializationChanged(); };
  connect(m->kind, &QComboBox::currentIndexChanged, this, changed);
  connect(m->count, &QLineEdit::textChanged, this, changed); connect(m->seed, &QLineEdit::textChanged, this, changed);
  connect(m->priors, &QLineEdit::textChanged, this, changed); connect(m->provenance, &QPlainTextEdit::textChanged, this, changed);
  for (auto *input : m->bounds) connect(input, &QLineEdit::textChanged, this, changed);
  connect(m->path, &QLineEdit::textChanged, this, [this, changed] { if (!m->loading) m->pathEdited = true; changed(); });
  connect(m->frames, &QComboBox::currentIndexChanged, this, [this, changed] {
    if (m->loading) return;
    bool hasCoordinates = !m->path->text().trimmed().isEmpty();
    for (auto *input : m->bounds) hasCoordinates |= !input->text().trimmed().isEmpty();
    if (hasCoordinates) {
      const QSignalBlocker blocked(m->frames); m->frames->setCurrentIndex(m->frames->findData(m->frame));
      m->frameWarning = AppLanguage::source("坐标框架已保留；请先清空范围和点云选择，再明确输入新框架中的坐标。"); refresh(); return;
    }
    m->frame = m->frames->currentData().toString(); m->frameWarning = nullptr; changed();
  });
  connect(clear, &QPushButton::clicked, this, [this] {
    m->loading = true; for (auto *input : m->bounds) input->clear(); m->path->clear(); m->loading = false;
    m->pathEdited = true; m->frameWarning = nullptr; refresh(); emit initializationChanged();
  });
  connect(m->browse, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, translated(AppLanguage::source("选择初始化点云")), m->directory,
        translated(AppLanguage::source("PLY 点云 (*.ply);;所有文件 (*)")));
    if (path.isEmpty()) return;
    const QString relative = QDir(m->directory).relativeFilePath(path);
    if (m->directory.isEmpty() || relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) {
      m->frameWarning = AppLanguage::source("初始化点云必须位于所选相机数据目录内；请选择合法来源副本，原文件不会被移动。"); refresh(); return;
    }
    m->path->setText(QDir::fromNativeSeparators(relative));
  });
  AppLanguage::onChanged(this, [this] { refresh(); });
  setInitialization(allowPreserve ? QJsonObject{{"kind","preserve"}} :
      QJsonObject{{"kind","random"},{"frame","source"},{"count",10000},{"seed",0},{"priors",QJsonArray{}}});
}
InitializationEditor::~InitializationEditor() = default;
void InitializationEditor::setDatasetDirectory(const QString &directory) { m->directory = directory; }
void InitializationEditor::setInitialization(const QJsonObject &value) {
  m->loading = true; m->original = value; m->pathEdited = false; m->frameWarning = nullptr;
  m->kind->setCurrentIndex(m->kind->findData(value.value(QStringLiteral("kind")).toString(QStringLiteral("random"))));
  m->frame = value.value(QStringLiteral("frame")).toString(QStringLiteral("source")); m->frames->setCurrentIndex(m->frames->findData(m->frame));
  m->count->setText(numberText(value.value(QStringLiteral("count")))); m->seed->setText(numberText(value.value(QStringLiteral("seed"))));
  m->path->setText(value.value(QStringLiteral("path")).toString());
  const auto bounds = value.value(QStringLiteral("bounds")).toArray();
  for (int i = 0; i < 6; ++i) {
    const auto vector = bounds.size() == 2 ? bounds.at(i / 3).toArray() : QJsonArray{};
    m->bounds[i]->setText(vector.size() == 3 ? numberText(vector.at(i % 3)) : QString());
  }
  m->priors->setText(value.contains(QStringLiteral("priors")) ? jsonText(value.value(QStringLiteral("priors"))) : QStringLiteral("[]"));
  m->provenance->setPlainText(value.contains(QStringLiteral("provenance")) ? jsonText(value.value(QStringLiteral("provenance"))) : QString());
  m->loading = false; refresh(); emit initializationChanged();
}
QJsonObject InitializationEditor::initialization() const {
  auto result = m->original;
  const QString kind = m->kind->currentIndex() < 0 ? result.value(QStringLiteral("kind")).toString() : m->kind->currentData().toString();
  result.insert(QStringLiteral("kind"), kind);
  if (kind == QStringLiteral("preserve")) return result;
  result.insert(QStringLiteral("frame"), m->frame);
  if (kind == QStringLiteral("random")) {
    for (const auto &[key, input] : {std::pair{QStringLiteral("count"),m->count},std::pair{QStringLiteral("seed"),m->seed}}) {
      if (input->text().trimmed().isEmpty()) { if (!result.value(key).isNull()) result.remove(key); }
      else result.insert(key, jsonValue(input->text().trimmed()));
    }
    QJsonArray bounds; bool supplied = false;
    for (int side = 0; side < 2; ++side) {
      QJsonArray vector;
      for (int axis = 0; axis < 3; ++axis) { const auto text = m->bounds[side * 3 + axis]->text().trimmed(); supplied |= !text.isEmpty(); vector.append(jsonValue(text)); }
      bounds.append(vector);
    }
    if (supplied) result.insert(QStringLiteral("bounds"), bounds);
    else if (!result.value(QStringLiteral("bounds")).isNull()) result.remove(QStringLiteral("bounds"));
    if (m->original.value(QStringLiteral("kind")) != QStringLiteral("random") || m->pathEdited) {
      result.remove(QStringLiteral("path")); result.remove(QStringLiteral("sha256")); result.remove(QStringLiteral("format"));
    }
  } else {
    result.insert(QStringLiteral("path"), QDir::fromNativeSeparators(m->path->text().trimmed()));
    if (m->pathEdited) {
      for (const auto &key : {QStringLiteral("sha256"),QStringLiteral("point_bounds"),QStringLiteral("count")}) result.remove(key);
      if (QFileInfo(m->path->text()).suffix().compare(QStringLiteral("ply"), Qt::CaseInsensitive) == 0) result.insert(QStringLiteral("format"), QStringLiteral("ply"));
    }
  }
  const auto priorText = m->priors->text().trimmed();
  if (m->original.contains(QStringLiteral("priors")) || priorText != QStringLiteral("[]")) result.insert(QStringLiteral("priors"), jsonValue(priorText));
  const auto provenanceText = m->provenance->toPlainText().trimmed();
  if (!provenanceText.isEmpty()) result.insert(QStringLiteral("provenance"), jsonValue(provenanceText)); else result.remove(QStringLiteral("provenance"));
  return result;
}
QString InitializationEditor::validationError() const {
  const QString kind = m->kind->currentData().toString();
  if (kind == QStringLiteral("preserve") && m->allowPreserve) return {};
  if (kind != QStringLiteral("random") && kind != QStringLiteral("external_point_cloud") && kind != QStringLiteral("sfm_sparse"))
    return translated(AppLanguage::source("初始化来源不受支持；未替换为随机点或重求解相机。"));
  if (m->frame != QStringLiteral("source") && m->frame != QStringLiteral("canonical"))
    return translated(AppLanguage::source("初始化仅接受源坐标或规范坐标；训练坐标必须通过已记录变换派生。"));
  if (kind == QStringLiteral("random")) {
    const auto integer = [](const QString &text, quint64 low, quint64 high) {
      if (text.trimmed().isEmpty()) return true;
      for (const QChar character : text.trimmed()) if (character < QLatin1Char('0') || character > QLatin1Char('9')) return false;
      bool ok = false; const auto value = text.trimmed().toULongLong(&ok);
      return ok && !text.contains(QLatin1Char('-')) && value >= low && value <= high;
    };
    if (!integer(m->count->text(), 4, 10000000)) return translated(AppLanguage::source("随机初始化点数必须是 4 到 10000000 的整数。"));
    if (!integer(m->seed->text(), 0, 4294967295ULL)) return translated(AppLanguage::source("初始化随机种子必须是 0 到 4294967295 的整数；不会截断或取模。"));
    bool any = false; for (auto *input : m->bounds) any |= !input->text().trimmed().isEmpty();
    if (any) for (int axis = 0; axis < 3; ++axis) {
      double low = 0, high = 0;
      if (!finiteNumber(m->bounds[axis]->text(), &low) || !finiteNumber(m->bounds[axis + 3]->text(), &high) || low >= high)
        return translated(AppLanguage::source("粗范围需要六个有限数值，且每轴最小值必须小于最大值。"));
    }
  } else if (m->path->text().trimmed().isEmpty()) return translated(AppLanguage::source("请明确选择初始化点云；不会用评价网格或空路径代替。"));
  bool valid = false; const auto priors = jsonValue(m->priors->text(), &valid);
  if (!valid || !priors.isArray()) return translated(AppLanguage::source("先验声明必须是字符串 JSON 数组。"));
  for (const auto &prior : priors.toArray()) {
    if (!prior.isString()) return translated(AppLanguage::source("先验声明必须是字符串 JSON 数组。"));
    if (prior == QStringLiteral("evaluation_geometry") || prior == QStringLiteral("ground_truth") || prior == QStringLiteral("test_geometry"))
      return translated(AppLanguage::source("评价几何、真实深度或测试参考不能作为无先验初始化输入。"));
  }
  if (!m->provenance->toPlainText().trimmed().isEmpty()) { jsonValue(m->provenance->toPlainText(), &valid); if (!valid) return translated(AppLanguage::source("初始化来源记录不是有效 JSON；原记录尚未丢弃。")); }
  return {};
}
void InitializationEditor::refresh() {
  const QString kind = m->kind->currentData().toString(); const bool random = kind == QStringLiteral("random"), preserve = kind == QStringLiteral("preserve");
  m->count->setEnabled(random); m->seed->setEnabled(random); m->path->setEnabled(!random && !preserve); m->browse->setEnabled(!random && !preserve);
  m->frames->setEnabled(!preserve); for (auto *input : m->bounds) input->setEnabled(random);
  m->priors->setEnabled(!preserve); m->provenance->setEnabled(!preserve);
  m->warning->setText(m->frameWarning ? translated(m->frameWarning) : validationError());
}
}
