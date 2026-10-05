#pragma once

#include <QCheckBox>
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionFocusRect>
#include <algorithm>
#include <limits>

namespace gsw {

// A normal checkbox's minimum width is its entire single-line caption. Keep
// the original Qt button (text, shortcuts, accessibility and state) but let
// long localized captions wrap within a responsive form.
class WrappingCheckBox final : public QCheckBox {
public:
  explicit WrappingCheckBox(QWidget *parent = nullptr)
      : QCheckBox(parent) {
    configureSizePolicy();
  }

  WrappingCheckBox(const QString &text, QWidget *parent = nullptr)
      : QCheckBox(text, parent) {
    configureSizePolicy();
  }

  QSize sizeHint() const override {
    const QSize natural = QCheckBox::sizeHint();
    return QSize(natural.width(), heightForWidth(width()));
  }

  QSize minimumSizeHint() const override {
    QStyleOptionButton option;
    initStyleOption(&option);
    const int indicator = style()->pixelMetric(QStyle::PM_IndicatorWidth, &option, this);
    const int spacing = style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, &option, this);
    return QSize(indicator + spacing + 4 * fontMetrics().averageCharWidth(),
                 QCheckBox::sizeHint().height());
  }

  int heightForWidth(int width) const override {
    const QSize natural = QCheckBox::sizeHint();
    QStyleOptionButton option;
    initStyleOption(&option);
    option.rect = QRect(0, 0, std::max(1, width), natural.height());
    const QRect contents = style()->subElementRect(QStyle::SE_CheckBoxContents, &option, this);
    const int textHeight = fontMetrics().boundingRect(
        QRect(0, 0, std::max(1, contents.width()), std::numeric_limits<int>::max() / 8),
        textFlags(), text()).height();
    const int padding = std::max(0, natural.height() - fontMetrics().height());
    return std::max(natural.height(), textHeight + padding);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    QStyleOptionButton option;
    initStyleOption(&option);
    QRect contents = style()->subElementRect(QStyle::SE_CheckBoxContents, &option, this);
    const int firstLineHeight = std::max(fontMetrics().height(),
        style()->pixelMetric(QStyle::PM_IndicatorHeight, &option, this));
    option.rect.setHeight(firstLineHeight);
    option.text.clear();
    option.state &= ~QStyle::State_HasFocus;
    style()->drawControl(QStyle::CE_CheckBox, &option, &painter, this);
    contents.setTop(std::max(contents.top(), (firstLineHeight - fontMetrics().height()) / 2));
    style()->drawItemText(&painter, contents, textFlags(), palette(), isEnabled(),
                         text(), QPalette::WindowText);
    if (hasFocus()) {
      QStyleOptionFocusRect focus;
      focus.initFrom(this);
      focus.rect = rect().adjusted(0, 0, -1, -1);
      focus.backgroundColor = palette().color(QPalette::Window);
      style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
    }
  }

  bool hitButton(const QPoint &position) const override {
    return rect().contains(position);
  }

  void resizeEvent(QResizeEvent *event) override {
    QCheckBox::resizeEvent(event);
    // Do not invalidate again for height-only changes requested by the form.
    if (event->oldSize().width() != event->size().width()) updateGeometry();
  }

  void changeEvent(QEvent *event) override {
    QCheckBox::changeEvent(event);
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange ||
        event->type() == QEvent::LanguageChange) {
      updateGeometry();
      update();
    }
  }

private:
  void configureSizePolicy() {
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
  }

  int textFlags() const {
    const int mnemonic = style()->styleHint(QStyle::SH_UnderlineShortcut, nullptr, this)
        ? Qt::TextShowMnemonic : Qt::TextHideMnemonic;
    return Qt::AlignLeading | Qt::AlignTop | Qt::TextWordWrap | mnemonic;
  }
};

} // namespace gsw
