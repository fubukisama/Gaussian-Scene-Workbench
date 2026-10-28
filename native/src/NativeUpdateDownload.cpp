#include "NativeUpdateDownload.h"

#include <QAuthenticator>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace gsw {
namespace {
constexpr qint64 maximumArchiveLength = 4LL * 1024 * 1024 * 1024;

QString hostKey(const QString &host) {
  return QString::fromLatin1(QUrl::toAce(host)).toLower();
}

bool ordinaryLocalPath(const QString &path) {
  if (!QDir::isAbsolutePath(path) || path.startsWith(QLatin1String("//")) ||
      path.startsWith(QLatin1String("\\\\"))) return false;
  QString cursor = QDir::cleanPath(path);
  for (;;) {
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(cursor).utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
      return false;
    }
#else
    if (QFileInfo(cursor).isSymLink()) return false;
#endif
    const QString parent = QFileInfo(cursor).absolutePath();
    if (parent == cursor) return true;
    cursor = parent;
  }
}

bool safeDestination(const QString &path) {
  if (!ordinaryLocalPath(path) || QFileInfo(path).fileName().isEmpty()) return false;
  const QString portable = QDir::fromNativeSeparators(path);
  const auto parts = portable.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  for (qsizetype i = 0; i < parts.size(); ++i) {
    const auto &part = parts[i];
    if (part == QLatin1String(".") || part == QLatin1String("..") ||
        part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) return false;
    if (part.contains(QLatin1Char(':')) && !(i == 0 && part.size() == 2 && part[1] == QLatin1Char(':')))
      return false;
  }
  return !QFileInfo(path).isDir();
}

bool openOwnedPart(QFile &file, const QString &path, bool truncate) {
  if (!ordinaryLocalPath(path)) return false;
  file.setFileName(path);
#ifdef Q_OS_WIN
  const QString native = QDir::toNativeSeparators(QDir::cleanPath(path));
  HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  wchar_t finalName[32768]{};
  const DWORD finalLength = GetFinalPathNameByHandleW(handle, finalName, 32768, FILE_NAME_NORMALIZED);
  QString resolved = finalLength > 0 && finalLength < 32768 ? QString::fromWCharArray(finalName) : QString();
  if (resolved.startsWith(QLatin1String("\\\\?\\"))) resolved.remove(0, 4);
  const bool safe = GetFileInformationByHandle(handle, &info) && info.nNumberOfLinks == 1 &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
      resolved.compare(native, Qt::CaseInsensitive) == 0;
  if (!safe) { CloseHandle(handle); return false; }
  const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
  if (descriptor < 0) { CloseHandle(handle); return false; }
  if (!file.open(descriptor, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) {
    _close(descriptor); return false;
  }
#else
  if (!file.open(QIODevice::ReadWrite)) return false;
#endif
  if (truncate && !file.resize(0)) { file.close(); return false; }
  return file.seek(truncate ? 0 : file.size());
}

QString networkCode(QNetworkReply::NetworkError error) {
  switch (error) {
  case QNetworkReply::HostNotFoundError: return QStringLiteral("download_dns_failed");
  case QNetworkReply::TimeoutError: return QStringLiteral("download_timeout");
  case QNetworkReply::SslHandshakeFailedError: return QStringLiteral("download_tls_failed");
  case QNetworkReply::ProxyConnectionRefusedError:
  case QNetworkReply::ProxyConnectionClosedError:
  case QNetworkReply::ProxyNotFoundError:
  case QNetworkReply::ProxyTimeoutError:
  case QNetworkReply::ProxyAuthenticationRequiredError: return QStringLiteral("download_proxy_failed");
  case QNetworkReply::AuthenticationRequiredError: return QStringLiteral("download_authentication_forbidden");
  default: return QStringLiteral("download_network_error");
  }
}
} // namespace

class NativeUpdateDownload::Impl {
public:
  NativeUpdateDownload *q;
  QNetworkAccessManager manager;
  QTimer deadline;
  QPointer<QNetworkReply> reply;
  QFile part;
  std::unique_ptr<QSaveFile> output;
  QCryptographicHash digest{QCryptographicHash::Sha256};
  NativeUpdateDownloadRequest request;
  NativeUpdateDownloadPolicy policy;
  State state = State::Idle;
  bool active = false, finishing = false, headersAccepted = false, ownsPart = false;
  qint64 bytes = 0;
  quint64 generation = 0;
  int httpStatus = 0;

  explicit Impl(NativeUpdateDownload *owner) : q(owner), manager(owner), deadline(owner) {
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, q, [this] { finish(QStringLiteral("download_total_timeout")); });
    QObject::connect(&manager, &QNetworkAccessManager::authenticationRequired, q,
        [this](QNetworkReply *challenged, QAuthenticator *) {
          if (active && challenged == reply) finish(QStringLiteral("download_authentication_forbidden"));
        });
    QObject::connect(&manager, &QNetworkAccessManager::proxyAuthenticationRequired, q,
        [this](const QNetworkProxy &, QAuthenticator *) {
          if (active) finish(QStringLiteral("download_proxy_authentication_forbidden"));
        });
  }

  ~Impl() { shutdown(); }

  void shutdown() {
    active = false; ++generation; deadline.stop();
    if (reply) { QObject::disconnect(reply, nullptr, q, nullptr); reply->abort(); reply->deleteLater(); reply = nullptr; }
    QObject::disconnect(&manager, nullptr, q, nullptr);
    part.close();
    if (output) { output->cancelWriting(); output.reset(); }
  }

  bool transition(State next) {
    if (state == next) return true;
    state = next;
    const QPointer<NativeUpdateDownload> alive(q);
    emit q->stateChanged(next);
    return !alive.isNull();
  }

  bool emitProgress() {
    const QPointer<NativeUpdateDownload> alive(q);
    emit q->progress(bytes, request.expectedLength);
    return !alive.isNull();
  }

  bool approved(const QUrl &url) const {
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty() ||
        url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) || url.hasFragment()) return false;
    const auto host = hostKey(url.host());
    for (const auto &entry : policy.approvedHosts) if (host == hostKey(entry)) return true;
    return false;
  }

  void finish(const QString &code, bool cancelled = false) {
    if (!active || finishing) return;
    finishing = true;
    deadline.stop();
    if (reply) { QObject::disconnect(reply, nullptr, q, nullptr); reply->abort(); reply->deleteLater(); reply = nullptr; }
    part.close();
    if (output) { output->cancelWriting(); output.reset(); }
    const bool ok = code.isEmpty();
    const QString partPath = request.destinationPath + QStringLiteral(".part");
    if (ownsPart && ordinaryLocalPath(partPath)) QFile::remove(partPath);
    ownsPart = false;
    NativeUpdateDownloadResult result;
    result.ok = ok; result.cancelled = cancelled; result.code = code;
    result.filePath = ok ? request.destinationPath : QString();
    result.receivedBytes = bytes; result.httpStatus = httpStatus;
    const QPointer<NativeUpdateDownload> alive(q);
    if (!transition(ok ? State::Completed : cancelled ? State::Cancelled : State::Failed) || !alive) return;
    active = false; finishing = false;
    emit q->finished(result); // Nothing below may touch Impl: caller can destroy it.
  }

  void begin() {
    static const QRegularExpression sha(QStringLiteral("^[a-f0-9]{64}$"));
    if (policy.approvedHosts.isEmpty() || policy.maximumLength <= 0 || policy.maximumLength > maximumArchiveLength ||
        policy.inactivityTimeoutMs < 100 || policy.inactivityTimeoutMs > 600000 ||
        policy.totalTimeoutMs < 100 || policy.totalTimeoutMs > 3600000 ||
        policy.maximumAttempts < 1 || policy.maximumAttempts > 5 ||
        policy.maximumRedirects < 0 || policy.maximumRedirects > 5 ||
        policy.retryDelayMs < 0 || policy.retryDelayMs > 5000 ||
        !policy.sslConfiguration.backendConfiguration().isEmpty()) {
      finish(QStringLiteral("download_policy_invalid")); return;
    }
    if (!approved(request.url) || request.identity.isEmpty() || request.identity.toUtf8().size() > 1024 ||
        request.expectedLength <= 0 || request.expectedLength > policy.maximumLength || !sha.match(request.sha256).hasMatch()) {
      finish(QStringLiteral("download_request_invalid")); return;
    }
    if (!safeDestination(request.destinationPath) || !ordinaryLocalPath(request.destinationPath + QStringLiteral(".part"))) {
      finish(QStringLiteral("download_path_unsafe")); return;
    }
    if (!QDir().mkpath(QFileInfo(request.destinationPath).absolutePath())) {
      finish(QStringLiteral("download_prepare_failed")); return;
    }
    if (!QSslSocket::supportsSsl()) { finish(QStringLiteral("download_tls_unavailable")); return; }
    deadline.start(policy.totalTimeoutMs);
    QNetworkRequest wire(request.url);
    wire.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    wire.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    wire.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    wire.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    wire.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    wire.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    wire.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    wire.setRawHeader("Accept-Encoding", "identity");
    wire.setRawHeader("User-Agent", "GSW-Updater/1");
    wire.setRawHeader("Connection", "close");
    wire.setTransferTimeout(policy.inactivityTimeoutMs);
    auto tls = policy.sslConfiguration;
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    tls.setPrivateKey(QSslKey());
    tls.setLocalCertificateChain({});
    wire.setSslConfiguration(tls);
    if (!transition(State::Downloading) || !active) return;
    if (!emitProgress() || !active) return;
    reply = manager.get(wire);
    reply->setReadBufferSize(64 * 1024);
    auto *current = reply.data();
    QObject::connect(current, &QNetworkReply::metaDataChanged, q, [this, current] {
      if (active && reply == current) acceptHeaders();
    });
    QObject::connect(current, &QNetworkReply::readyRead, q, [this, current] {
      if (active && reply == current) consume();
    });
    QObject::connect(current, &QNetworkReply::sslErrors, q, [this, current](const QList<QSslError> &) {
      if (active && reply == current) finish(QStringLiteral("download_tls_failed"));
    });
    QObject::connect(current, &QNetworkReply::finished, q, [this, current] {
      if (active && reply == current) received();
    });
  }

  bool acceptHeaders() {
    if (!active || !reply) return false;
    if (headersAccepted) return true;
    httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus < 200) return false;
    if (httpStatus != 200) { finish(QStringLiteral("download_http_error")); return false; }
    if (!reply->attribute(QNetworkRequest::ConnectionEncryptedAttribute).toBool()) {
      finish(QStringLiteral("download_tls_failed")); return false;
    }
    const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
    if (!encoding.isEmpty() && encoding != "identity") {
      finish(QStringLiteral("download_content_encoding")); return false;
    }
    const auto length = reply->rawHeader("Content-Length").trimmed();
    bool goodLength = false;
    const qint64 declared = length.toLongLong(&goodLength);
    if (!length.isEmpty() && (!goodLength || declared != request.expectedLength)) {
      finish(QStringLiteral("download_length_mismatch")); return false;
    }
    if (!openOwnedPart(part, request.destinationPath + QStringLiteral(".part"), true)) {
      finish(QStringLiteral("download_write_failed")); return false;
    }
    ownsPart = true;
    headersAccepted = true;
    return true;
  }

  void consume() {
    if (!acceptHeaders()) return;
    while (active && reply && reply->bytesAvailable() > 0) {
      const QByteArray chunk = reply->read(64 * 1024);
      if (chunk.size() > request.expectedLength - bytes) { finish(QStringLiteral("download_length_mismatch")); return; }
      if (part.write(chunk) != chunk.size()) { finish(QStringLiteral("download_write_failed")); return; }
      bytes += chunk.size();
      if (!emitProgress()) return;
    }
  }

  void received() {
    const QPointer<NativeUpdateDownload> alive(q);
    consume();
    if (!alive) return;
    if (!active || !reply) return;
    if (reply->error() != QNetworkReply::NoError) { finish(networkCode(reply->error())); return; }
    if (!headersAccepted || bytes != request.expectedLength || !part.flush() || part.size() != bytes) {
      finish(QStringLiteral("download_length_mismatch")); return;
    }
    QObject::disconnect(reply, nullptr, q, nullptr); reply->deleteLater(); reply = nullptr;
    if (!part.seek(0)) { finish(QStringLiteral("download_read_failed")); return; }
    output = std::make_unique<QSaveFile>(request.destinationPath);
    output->setDirectWriteFallback(false);
    if (!output->open(QIODevice::WriteOnly)) { finish(QStringLiteral("download_write_failed")); return; }
    digest.reset();
    if (!transition(State::Verifying) || !active) return;
    scheduleVerify();
  }

  void scheduleVerify() {
    const auto token = generation;
    QTimer::singleShot(0, q, [this, token] {
      if (!active || token != generation) return;
      const QByteArray chunk = part.read(1024 * 1024);
      if (!chunk.isEmpty()) {
        digest.addData(chunk);
        if (output->write(chunk) != chunk.size()) { finish(QStringLiteral("download_write_failed")); return; }
        scheduleVerify(); return;
      }
      if (part.error() != QFileDevice::NoError || part.pos() != request.expectedLength) {
        finish(QStringLiteral("download_read_failed")); return;
      }
      if (QString::fromLatin1(digest.result().toHex()) != request.sha256) {
        finish(QStringLiteral("download_hash_mismatch")); return;
      }
      if (!safeDestination(request.destinationPath) || !output->commit()) {
        finish(QStringLiteral("download_publish_failed")); return;
      }
      output.reset();
      finish({});
    });
  }
};

NativeUpdateDownload::NativeUpdateDownload(QObject *parent) : QObject(parent), m(std::make_unique<Impl>(this)) {
  qRegisterMetaType<NativeUpdateDownloadResult>();
}
NativeUpdateDownload::~NativeUpdateDownload() = default;
bool NativeUpdateDownload::start(const NativeUpdateDownloadRequest &request, const NativeUpdateDownloadPolicy &policy) {
  if (m->active || m->finishing) return false;
  m->request = request; m->policy = policy; m->active = true;
  m->headersAccepted = false; m->ownsPart = false; m->bytes = 0; m->httpStatus = 0;
  const auto token = ++m->generation;
  QTimer::singleShot(0, this, [this, token] { if (m->active && token == m->generation) m->begin(); });
  return true;
}
void NativeUpdateDownload::cancel() { m->finish(QStringLiteral("download_cancelled"), true); }
NativeUpdateDownload::State NativeUpdateDownload::state() const { return m->state; }
qint64 NativeUpdateDownload::receivedBytes() const { return m->bytes; }
} // namespace gsw
