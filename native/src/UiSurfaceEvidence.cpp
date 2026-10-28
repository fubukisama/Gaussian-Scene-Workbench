#include "UiSurfaceEvidence.h"

#include "AppLanguage.h"
#include "AppTheme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDebug>
#include <QDialog>
#include <QDockWidget>
#include <QDir>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontInfo>
#include <QGlyphRun>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPointer>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QStatusBar>
#include <QStringList>
#include <QTextEdit>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QVariant>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsw {
namespace {
constexpr int imageLimit = 160;
constexpr int observationLimit = 512;
constexpr int fontLimit = 256;
constexpr qint64 pixelLimit = 16LL * 1024 * 1024;
constexpr qint64 durationLimitMs = 10 * 60 * 1000;
constexpr int stableDelayMs = 80;

QString machineName(const QString &name) {
  static const QRegularExpression safe(QStringLiteral("^[A-Za-z_][A-Za-z0-9_:.-]{0,95}$"));
  return safe.match(name).hasMatch() ? name : QString();
}

QString surfaceRole(const QWidget *window) {
  if (qobject_cast<const QMessageBox *>(window)) return QStringLiteral("message-box");
  if (qobject_cast<const QFileDialog *>(window)) return QStringLiteral("file-dialog");
  if (qobject_cast<const QProgressDialog *>(window)) return QStringLiteral("progress-dialog");
  if (qobject_cast<const QMenu *>(window)) return QStringLiteral("menu");
  if (qobject_cast<const QDockWidget *>(window)) return QStringLiteral("floating-dock");
  if (qobject_cast<const QMainWindow *>(window)) return QStringLiteral("main-window");
  if (qobject_cast<const QDialog *>(window)) return QStringLiteral("dialog");
  return {};
}

QString widgetRole(const QWidget *widget) {
  const QString surface = surfaceRole(widget);
  if (!surface.isEmpty()) return surface;
  if (qobject_cast<const QLabel *>(widget)) return QStringLiteral("label");
  if (qobject_cast<const QAbstractButton *>(widget)) return QStringLiteral("button");
  if (qobject_cast<const QAbstractSpinBox *>(widget)) return QStringLiteral("numeric-field");
  if (qobject_cast<const QLineEdit *>(widget)) return QStringLiteral("text-field");
  if (qobject_cast<const QComboBox *>(widget)) return QStringLiteral("choice");
  if (qobject_cast<const QAbstractItemView *>(widget)) return QStringLiteral("item-view");
  if (qobject_cast<const QPlainTextEdit *>(widget)) return QStringLiteral("plain-text-editor");
  if (qobject_cast<const QTextEdit *>(widget)) return QStringLiteral("rich-text-editor");
  if (qobject_cast<const QMenuBar *>(widget)) return QStringLiteral("menu-bar");
  if (qobject_cast<const QToolBar *>(widget)) return QStringLiteral("tool-bar");
  if (qobject_cast<const QStatusBar *>(widget)) return QStringLiteral("status-bar");
  return QStringLiteral("widget-default");
}

QByteArray digest(const QJsonObject &object) {
  return QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact),
                                  QCryptographicHash::Sha256).toHex();
}

QString fileDigest(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

bool freshRoot(const QString &root) {
  const QFileInfo requested(root);
  if (!QDir::isAbsolutePath(root) || requested.exists() || requested.isSymLink() || requested.isJunction())
    return false;
  const QFileInfo parent(QFileInfo(root).absolutePath());
  if (!parent.exists() || !parent.isDir()) return false;
  for (QFileInfo cursor = parent;; cursor = QFileInfo(cursor.absolutePath())) {
    if (cursor.isSymLink() || cursor.isJunction()) return false;
    if (cursor.absoluteFilePath() == cursor.absolutePath()) break;
  }
  return true;
}

// QScreen::grabWindow reads screen pixels, including overlying windows. An
// exposed/active Qt window alone is insufficient. Check native z order first;
// do not capture the desktop or silently fall back to QWidget::grab.
#ifdef Q_OS_WIN
QString nativeCaptureGuard(WId id) {
  const HWND native = reinterpret_cast<HWND>(id);
  DWORD owner = 0, foregroundOwner = 0;
  if (!IsWindow(native) || !IsWindowVisible(native) || IsIconic(native))
    return QStringLiteral("native-window-not-visible");
  GetWindowThreadProcessId(native, &owner);
  GetWindowThreadProcessId(GetForegroundWindow(), &foregroundOwner);
  if (owner != GetCurrentProcessId() || foregroundOwner != owner)
    return QStringLiteral("not-owned-foreground-process");
  RECT client{};
  POINT origin{};
  if (!GetClientRect(native, &client) || !ClientToScreen(native, &origin))
    return QStringLiteral("native-client-unavailable");
  OffsetRect(&client, origin.x, origin.y);
  const RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
      GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
      GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
  if (client.right <= client.left || client.bottom <= client.top ||
      client.left < desktop.left || client.top < desktop.top ||
      client.right > desktop.right || client.bottom > desktop.bottom)
    return QStringLiteral("native-client-outside-desktop");
  // GetTopWindow/GW_HWNDNEXT explicitly traverse z order. Bound the walk because
  // native windows can be created/destroyed concurrently; an incomplete walk is
  // a refusal, not permission to read potentially obscured screen pixels.
  HWND other = GetTopWindow(nullptr);
  for (int visited = 0; other && visited < 4096; ++visited) {
    if (other == native) return {};
    if (IsWindowVisible(other) && !IsIconic(other)) {
      RECT bounds{}, intersection{};
      if (GetWindowRect(other, &bounds) && IntersectRect(&intersection, &bounds, &client))
        return QStringLiteral("screen-pixels-obscured");
    }
    other = GetWindow(other, GW_HWNDNEXT);
  }
  return QStringLiteral("native-z-order-unavailable");
}
#else
QString nativeCaptureGuard(WId) { return QStringLiteral("unsupported-native-platform"); }
#endif

class SurfaceEvidence final : public QObject {
public:
  SurfaceEvidence(QString root, QString commit, QString smoke, QApplication *application)
      : QObject(application), mRoot(std::move(root)), mCommit(std::move(commit)),
        mSmoke(std::move(smoke)) {
    setObjectName(QStringLiteral("gswUiSurfaceEvidenceAdapter"));
    mElapsed.start();
    application->installEventFilter(this);
    connect(application, &QCoreApplication::aboutToQuit, this, [this] {
      mFinished = true;
      for (auto it = mWindows.begin(); it != mWindows.end(); ++it) it->timer->stop();
      flush();
    });
    flush();
    for (QWidget *window : QApplication::topLevelWidgets()) schedule(window, QStringLiteral("install"));
  }

  bool ready() const { return !mStopped; }

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (mCapturing || mFinished || mStopped) return false;
    QString reason;
    switch (event->type()) {
    case QEvent::Show: reason = QStringLiteral("show"); break;
    case QEvent::Resize: reason = QStringLiteral("resize"); break;
    case QEvent::Move: reason = QStringLiteral("move"); break;
    case QEvent::LanguageChange: reason = QStringLiteral("language"); break;
    case QEvent::ThemeChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange: reason = QStringLiteral("theme"); break;
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange: reason = QStringLiteral("font"); break;
    case QEvent::DevicePixelRatioChange: reason = QStringLiteral("dpr"); break;
    case QEvent::LayoutRequest: reason = QStringLiteral("layout"); break;
    case QEvent::WindowActivate: reason = QStringLiteral("activation"); break;
    case QEvent::DynamicPropertyChange: {
      const auto *change = static_cast<QDynamicPropertyChangeEvent *>(event);
      if (change->propertyName() != "gswUiEvidenceState") return false;
      reason = QStringLiteral("synthetic-state");
      break;
    }
    case QEvent::Hide: {
      auto *window = qobject_cast<QWidget *>(object);
      auto it = mWindows.find(window);
      if (it != mWindows.end() && it->timer->isActive()) {
        it->timer->stop();
        observation(*it, QStringLiteral("skipped"), QStringLiteral("hidden-before-stable"));
      }
      return false;
    }
    default: return false;
    }
    if (auto *widget = qobject_cast<QWidget *>(object)) schedule(widget->window(), reason);
    else if (object == qApp)
      for (QWidget *window : QApplication::topLevelWidgets()) schedule(window, reason);
    return false;
  }

private:
  struct WindowState {
    QPointer<QWidget> window;
    QTimer *timer = nullptr;
    int id = 0;
    QString className;
    QString role;
    QSet<QString> reasons;
    QSet<QByteArray> recorded;
  };

  void schedule(QWidget *window, const QString &reason) {
    if (!window || !window->isWindow() || surfaceRole(window).isEmpty() ||
        mFinished || mStopped) return;
    auto it = mWindows.find(window);
    if (it == mWindows.end()) {
      if (mWindows.size() >= observationLimit) return;
      WindowState state;
      state.window = window;
      state.id = ++mNextWindow;
      state.className = machineName(QString::fromLatin1(window->metaObject()->className()));
      state.role = surfaceRole(window);
      state.timer = new QTimer(this);
      state.timer->setSingleShot(true);
      state.timer->setInterval(stableDelayMs);
      const int identity = state.id;
      connect(state.timer, &QTimer::timeout, this, [this, window, identity] {
        auto found = mWindows.find(window);
        if (found != mWindows.end() && found->id == identity) capture(*found);
      });
      connect(window, &QObject::destroyed, this, [this, window, identity] {
        auto found = mWindows.find(window);
        if (found == mWindows.end() || found->id != identity) return;
        if (found->timer->isActive() && !mFinished)
          observation(*found, QStringLiteral("skipped"), QStringLiteral("destroyed-before-stable"));
        found->timer->stop();
        found->timer->deleteLater();
        mWindows.erase(found);
      });
      it = mWindows.insert(window, std::move(state));
    }
    it->reasons.insert(reason);
    it->timer->start();
  }

  QJsonObject identity(const WindowState &state) const {
    QJsonObject result{{"windowInstance", state.id}, {"class", state.className},
        {"typedRole", state.role}, {"locale", AppLanguage::current()},
        {"theme", AppTheme::currentTheme() == UiTheme::Light ? "light" : "dark"}};
    if (QWidget *window = state.window.data()) {
      result.insert("objectName", machineName(window->objectName()));
      result.insert("logicalSize", QJsonArray{window->width(), window->height()});
      result.insert("widgetDpr", window->devicePixelRatioF());
      result.insert("syntheticState", machineName(window->property("gswUiEvidenceState").toString()));
    }
    return result;
  }

  void observation(WindowState &state, const QString &status, const QString &reason,
                   QJsonObject record = {}) {
    if (mRecords.size() >= observationLimit) { mStopped = true; flush(); return; }
    if (record.isEmpty()) record = identity(state);
    record.insert("status", status);
    record.insert("reason", reason);
    const QByteArray key = digest(record);
    if (state.recorded.contains(key)) return;
    state.recorded.insert(key);
    record.insert("elapsedMs", mElapsed.elapsed());
    QJsonArray reasons;
    QStringList sorted = state.reasons.values();
    sorted.sort();
    for (const QString &value : sorted) reasons.append(value);
    record.insert("triggerEvents", reasons);
    state.reasons.clear();
    mRecords.append(record);
    flush();
  }

  void capture(WindowState &state) {
    if (mFinished || mStopped) return;
    QWidget *window = state.window.data();
    if (!window) return;
    if (mElapsed.elapsed() > durationLimitMs || mImageCount >= imageLimit ||
        mRecords.size() >= observationLimit) {
      observation(state, QStringLiteral("skipped"), QStringLiteral("capture-limit-reached"));
      mStopped = true;
      flush();
      return;
    }
    QWindow *native = window->windowHandle();
    if (!window->isVisible() || window->isMinimized() || !native || !native->isExposed()) {
      observation(state, QStringLiteral("skipped"), QStringLiteral("not-exposed-native-window"));
      return;
    }
    const bool popup = qobject_cast<QMenu *>(window) && QApplication::activePopupWidget() == window;
    if (!popup && QApplication::activeWindow() != window && !window->isActiveWindow()) {
      observation(state, QStringLiteral("skipped"), QStringLiteral("not-active-owned-window"));
      return;
    }
    const WId id = window->effectiveWinId(); // Read only; never forces native creation.
    const QString guard = nativeCaptureGuard(id);
    if (!guard.isEmpty()) { observation(state, QStringLiteral("skipped"), guard); return; }
    QScreen *screen = native->screen();
    const qreal dpr = window->devicePixelRatioF();
    if (!screen || !std::isfinite(dpr) || dpr <= 0 || window->width() <= 0 || window->height() <= 0 ||
        double(window->width()) * window->height() * dpr * dpr > pixelLimit) {
      observation(state, QStringLiteral("skipped"), QStringLiteral("invalid-or-unbounded-capture-size"));
      return;
    }
    mCapturing = true;
    QJsonObject record = identity(state);
    const QJsonArray fonts = uiSurfaceFontEvidence(window);
    record.insert("visibleWidgetFontResolution", fonts);
    record.insert("fontRecordsMayBeTruncated", fonts.size() == fontLimit);
    record.insert("uiScalePercent", qApp->property("gswUiScalePercent").toInt());
    const QByteArray key = digest(record);
    if (mCaptured.contains(key)) { mCapturing = false; state.reasons.clear(); return; }
    // The id is nonzero and already belongs to an exposed native Qt window.
    const QPixmap pixels = screen->grabWindow(id, 0, 0, window->width(), window->height());
    mCapturing = false;
    const QString afterGuard = nativeCaptureGuard(id);
    if (!afterGuard.isEmpty()) {
      observation(state, QStringLiteral("skipped"), QStringLiteral("native-visibility-changed-during-grab"), record);
      return;
    }
    if (pixels.isNull() || qint64(pixels.width()) * pixels.height() > pixelLimit) {
      observation(state, QStringLiteral("failed"), QStringLiteral("native-grab-unavailable"), record);
      return;
    }
    const QString image = QStringLiteral("surface-%1.png").arg(mImageCount + 1, 4, 10, QLatin1Char('0'));
    const QString path = QDir(mRoot).filePath(image);
    QSaveFile file(path);
    if (QFileInfo::exists(path) || !file.open(QIODevice::WriteOnly) ||
        !pixels.save(&file, "PNG") || !file.commit()) {
      observation(state, QStringLiteral("failed"), QStringLiteral("png-write-failed"), record);
      return;
    }
    const QString sha = fileDigest(path);
    if (sha.isEmpty()) {
      observation(state, QStringLiteral("failed"), QStringLiteral("png-digest-failed"), record);
      return;
    }
    ++mImageCount;
    mCaptured.insert(key);
    record.insert("image", image);
    record.insert("sha256", sha);
    record.insert("captureMethod", "QScreen::grabWindow(owned existing native client id)");
    record.insert("occlusionCheck", "Win32-z-order-client-rect");
    record.insert("nativeWindowId", QString::number(quintptr(id), 16));
    record.insert("physicalImageSize", QJsonArray{pixels.width(), pixels.height()});
    record.insert("pixmapDpr", pixels.devicePixelRatio());
    record.insert("nativeWindowDpr", native->devicePixelRatio());
    record.insert("screenDpr", screen->devicePixelRatio());
    record.insert("screenLogicalDpi", screen->logicalDotsPerInch());
    record.insert("dimensionsMatchWidgetDpr",
        std::abs(pixels.width() - window->width() * dpr) <= 1 &&
        std::abs(pixels.height() - window->height() * dpr) <= 1);
#ifdef Q_OS_WIN
    record.insert("actualWindowsWindowDpi", int(GetDpiForWindow(reinterpret_cast<HWND>(id))));
#endif
    observation(state, QStringLiteral("captured"), QStringLiteral("stable-owned-surface"), record);
  }

  void flush() {
    const QJsonObject report{{"schema", "gsw.ui_surface_evidence/1"}, {"sourceCommit", mCommit},
        {"smoke", mSmoke}, {"platform", QGuiApplication::platformName()},
        {"finished", mFinished}, {"stoppedByLimit", mStopped}, {"imageCount", mImageCount},
        {"imageLimit", imageLimit}, {"observationLimit", observationLimit},
        {"fontLimitPerImage", fontLimit}, {"physicalPixelLimitPerImage", pixelLimit},
        {"durationLimitMs", durationLimitMs}, {"stableDelayMs", stableDelayMs},
        {"screenCaptureCanIncludeOcclusion", true},
        {"imagesDoNotReplacePublicBehaviorAssertions", true},
        {"customPainterFontRolesNotInWidgetDefaultProbe", true},
        {"nativeOsDialogsWithoutOwnedQWidgetNotObserved", true}, {"surfaces", mRecords}};
    QSaveFile output(QDir(mRoot).filePath(QStringLiteral("ui-surfaces.json")));
    const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
      mStopped = true;
      qWarning("UI_SURFACE_EVIDENCE: metadata-write-failed");
    }
  }

  QString mRoot, mCommit, mSmoke;
  QElapsedTimer mElapsed;
  QHash<QWidget *, WindowState> mWindows;
  QJsonArray mRecords;
  QSet<QByteArray> mCaptured;
  int mNextWindow = 0;
  int mImageCount = 0;
  bool mCapturing = false;
  bool mFinished = false;
  bool mStopped = false;
};
} // namespace

QJsonArray uiSurfaceFontEvidence(QWidget *window) {
  QJsonArray result;
  if (!window) return result;
  // Fixed QA data, not a user-facing or translatable message.
  const char32_t probe[]{0x4c, 0x61, 0x74, 0x69, 0x6e, 0x20, 0x4e2d, 0x6587,
      0x20, 0x65e5, 0x672c, 0x8a9e, 0x20, 0x30, 0x31, 0x32, 0x33};
  const QString text = QString::fromUcs4(probe, qsizetype(std::size(probe)));
  QList<QWidget *> widgets{window};
  widgets.append(window->findChildren<QWidget *>());
  for (QWidget *widget : widgets) {
    if (result.size() >= fontLimit) break;
    if (!widget->isVisible() || widget->window() != window) continue;
    const QFont requested = widget->font();
    const QFont deviceFont(requested, widget);
    const QFontInfo info(deviceFont);
    QTextLayout layout(text, deviceFont, widget);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) line.setLineWidth(2000);
    layout.endLayout();
    QJsonArray runs;
    for (const QGlyphRun &run : layout.glyphRuns()) {
      const auto glyphs = run.glyphIndexes();
      runs.append(QJsonObject{{"family", run.rawFont().familyName()}, {"style", run.rawFont().styleName()},
          {"glyphCount", glyphs.size()}, {"missingGlyphCount", glyphs.count(0)}});
    }
    const QPoint position = widget == window ? QPoint() : widget->mapTo(window, QPoint());
    result.append(QJsonObject{{"class", machineName(QString::fromLatin1(widget->metaObject()->className()))},
        {"name", machineName(widget->objectName())}, {"typedRole", widgetRole(widget)},
        {"fontRoleScope", "QWidget-default-not-custom-painter"},
        {"requestedFamily", requested.family()}, {"requestedPointSize", requested.pointSizeF()},
        {"requestedPixelSize", requested.pixelSize()}, {"resolvedFamily", info.family()},
        {"pointSize", info.pointSizeF()}, {"pixelSize", info.pixelSize()}, {"weight", int(info.weight())},
        {"exactMatch", info.exactMatch()}, {"enabled", widget->isEnabled()},
        {"logicalRect", QJsonArray{position.x(), position.y(), widget->width(), widget->height()}},
        {"logicalDpiX", widget->logicalDpiX()}, {"logicalDpiY", widget->logicalDpiY()},
        {"mixedScriptGlyphRuns", runs}});
  }
  return result;
}

bool installUiSurfaceEvidence(const QString &root, const QString &sourceCommit) {
  auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
  if (!application || QThread::currentThread() != application->thread() ||
      qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QGuiApplication::platformName() != QStringLiteral("windows") ||
      !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{40}$")).match(sourceCommit).hasMatch() ||
      application->findChild<QObject *>(QStringLiteral("gswUiSurfaceEvidenceAdapter"))) return false;
  const QString isolated = application->property("gswSmokeSettingsDirectory").toString();
  if (!QDir::isAbsolutePath(isolated) || !QFileInfo(isolated).isDir()) return false;
  QString smoke;
  const QRegularExpression smokeFlag(QStringLiteral("^--smoke-test(?:-[a-z0-9-]+)?$"));
  for (const QString &argument : application->arguments()) {
    if (smokeFlag.match(argument).hasMatch()) {
      smoke = argument;
      break;
    }
  }
  if (smoke.isEmpty() || !freshRoot(root) || !QDir().mkpath(root)) return false;
  auto *adapter = new SurfaceEvidence(QDir::cleanPath(root), sourceCommit.toLower(), smoke, application);
  if (!adapter->ready()) { delete adapter; return false; }
  qInfo("UI_SURFACE_EVIDENCE: installed; owned synthetic remote windows only");
  return true;
}
} // namespace gsw
