#pragma once

#include <QObject>
#include <QMetaType>
#include <QSslConfiguration>
#include <QStringList>
#include <QUrl>
#include <memory>

namespace gsw {

// The caller supplies these values only after authenticating signed metadata.
// A completed download proves byte integrity, never publisher authorization.
struct NativeUpdateDownloadRequest final {
  QUrl url;
  QString destinationPath;
  QString identity;
  qint64 expectedLength = 0;
  QString sha256;
};

struct NativeUpdateDownloadPolicy final {
  QStringList approvedHosts; // Exact DNS/ACE host names; no wildcard fallback.
  qint64 maximumLength = 4LL * 1024 * 1024 * 1024;
  int inactivityTimeoutMs = 30000;
  int totalTimeoutMs = 30 * 60 * 1000;
  int maximumAttempts = 3;
  int maximumRedirects = 3;
  int retryDelayMs = 500;
  // Trusted in-process configuration, not a public CLI/request field. Tests
  // can add their fixture CA; peer verification is always forced on.
  QSslConfiguration sslConfiguration = QSslConfiguration::defaultConfiguration();
};

struct NativeUpdateDownloadResult final {
  bool ok = false;
  bool cancelled = false;
  QString code; // Stable machine code; never a network error string or URL.
  QString filePath;
  qint64 receivedBytes = 0;
  int httpStatus = 0;
  bool resumed = false;
};

class NativeUpdateDownload final : public QObject {
  Q_OBJECT
public:
  enum class State { Idle, Downloading, RetryWait, Verifying, Completed, Cancelled, Failed };
  Q_ENUM(State)

  explicit NativeUpdateDownload(QObject *parent = nullptr);
  ~NativeUpdateDownload() override;

  // Returns false only while another request is active. Accepted requests
  // produce exactly one finished signal, including invalid input/cancellation.
  // Files are owned siblings: destination + ".part" and ".resume.json".
  // Existing destination bytes change only after complete length/hash checks.
  // The helper holds the common installation lock and derives this owned
  // cache destination; this module does not create a competing install lock.
  bool start(const NativeUpdateDownloadRequest &, const NativeUpdateDownloadPolicy &);
  void cancel();
  [[nodiscard]] State state() const;
  [[nodiscard]] qint64 receivedBytes() const;

signals:
  void stateChanged(gsw::NativeUpdateDownload::State state);
  void progress(qint64 receivedBytes, qint64 totalBytes);
  void finished(const gsw::NativeUpdateDownloadResult &result);

private:
  class Impl;
  std::unique_ptr<Impl> m;
};

} // namespace gsw

Q_DECLARE_METATYPE(gsw::NativeUpdateDownloadResult)
