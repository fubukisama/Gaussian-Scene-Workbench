#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>

class QPlainTextEdit;

namespace gsw {

// Session-only presentation data. This is not a persistent job or a resume record.
// All mutation belongs to the object's GUI thread.
class TaskRecord final : public QObject {
  Q_OBJECT
public:
  static constexpr qsizetype MaximumLogCharacters = 512 * 1024;

  explicit TaskRecord(const QString &name = {}, const QString &outputDirectory = {},
                      QObject *parent = nullptr);

  const QString &name() const { return mName; }
  const QString &state() const { return mState; }
  const QDateTime &startedAt() const { return mStartedAt; }
  const QDateTime &finishedAt() const { return mFinishedAt; }
  const QString &outputDirectory() const { return mOutputDirectory; }
  const QString &logText() const { return mLogText; }
  bool logTruncated() const { return mLogTruncated; }

  void setName(const QString &name);
  void setState(const QString &state);
  void setOutputDirectory(const QString &directory);
  void finish(const QString &state);
  void appendLog(const QString &text);
  bool remapManagedOutputDirectory(const QString &oldRoot, const QString &newRoot);

  QString stateLabel() const;
  QString diagnosticSummary() const;
  bool exportLog(const QString &path, QString *errorMessage = nullptr) const;

  // Append without stealing selection or scroll position when follow is false.
  // Optional trimming is in QPlainTextEdit document characters, not raw-log bytes.
  static void appendToLogView(QPlainTextEdit *view, const QString &text, bool follow,
                             qsizetype leadingCharactersToRemove = 0);

signals:
  void changed();
  // A large incoming chunk is reduced to the retained tail before this signal.
  // removedCharacters refers to the previous raw log's UTF-16 code units.
  void logChanged(const QString &appendedText, qsizetype removedCharacters);

private:
  void assertGuiThread() const;
  void refreshOutputIdentity();
  QString mName;
  QString mState = QStringLiteral("starting");
  QDateTime mStartedAt;
  QDateTime mFinishedAt;
  QString mOutputDirectory;
  QString mCanonicalOutputDirectory;
  QHash<QString, QString> mCanonicalAncestors;
  QString mLogText;
  bool mLogTruncated = false;
};

} // namespace gsw
