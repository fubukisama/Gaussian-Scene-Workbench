#include "NativeUpdateDownload.h"
#include "NativeUpdateLocalPath.h"

#include <QAuthenticator>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <QStorageInfo>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace gsw {
namespace {
constexpr qint64 maximumArchiveLength = 4LL * 1024 * 1024 * 1024;
// Match activation's retained operating margin. Downloads temporarily hold
// both the owned part and QSaveFile's final verified copy, not just one ZIP.
constexpr qint64 downloadFreeSpaceReserve = 512LL * 1024 * 1024;

QString hostKey(const QString &host) {
  return QString::fromLatin1(QUrl::toAce(host)).toLower();
}

bool ordinaryLocalPath(const QString &path) {
  if (!updatepaths::logicalLocalPath(path)) return false;
  QString cursor = QDir::cleanPath(path);
  for (;;) {
#ifdef Q_OS_WIN
    const auto api = updatepaths::nativeApiPath(cursor);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(api.utf16()));
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
  const QString api = updatepaths::nativeApiPath(path);
  HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(api.utf16()), GENERIC_READ | GENERIC_WRITE,
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

bool strongEtag(const QByteArray &tag) {
  if (tag.size() < 2 || tag.size() > 1024 || tag.front() != '"' || tag.back() != '"') return false;
  for (qsizetype i = 1; i + 1 < tag.size(); ++i)
    if (uchar(tag[i]) < 0x21 || uchar(tag[i]) > 0x7e || tag[i] == '"') return false;
  return true;
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
  bool ownsResume = false, resumeValid = false, resumed = false;
  QByteArray entityTag;
  QUrl activeUrl;
  qint64 bytes = 0, requestOffset = 0;
  quint64 generation = 0, responseSerial = 0;
  int httpStatus = 0, redirects = 0, attempts = 1;

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
    if (host.isEmpty()) return false;
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
    const QString resumePath = request.destinationPath + QStringLiteral(".resume.json");
    const bool retain = !ok && resumeValid && strongEtag(entityTag) &&
        ordinaryLocalPath(partPath) && QFileInfo(partPath).size() > 0 &&
        QFileInfo(partPath).size() <= request.expectedLength;
    if (!retain && ownsPart && ordinaryLocalPath(partPath)) QFile::remove(partPath);
    if (!retain && ownsResume && ordinaryLocalPath(resumePath)) QFile::remove(resumePath);
    ownsPart = false;
    ownsResume = false;
    NativeUpdateDownloadResult result;
    result.ok = ok; result.cancelled = cancelled; result.code = code;
    result.filePath = ok ? request.destinationPath : QString();
    result.receivedBytes = bytes; result.httpStatus = httpStatus; result.resumed = resumed;
    const QPointer<NativeUpdateDownload> alive(q);
    if (!transition(ok ? State::Completed : cancelled ? State::Cancelled : State::Failed) || !alive) return;
    active = false; finishing = false;
    emit q->finished(result); // Nothing below may touch Impl: caller can destroy it.
  }

  bool readResume() {
    const QString partPath = request.destinationPath + QStringLiteral(".part");
    const QString resumePath = request.destinationPath + QStringLiteral(".resume.json");
    if (!ordinaryLocalPath(partPath) || !ordinaryLocalPath(resumePath)) {
      finish(QStringLiteral("download_path_unsafe")); return false;
    }
    QFile stateFile(resumePath);
    QJsonObject saved;
    if (stateFile.open(QIODevice::ReadOnly) && stateFile.size() <= 16384) {
      QJsonParseError error;
      const auto doc = QJsonDocument::fromJson(stateFile.readAll(), &error);
      if (error.error == QJsonParseError::NoError && doc.isObject()) saved = doc.object();
    }
    stateFile.close();
    const QSet<QString> fields{QStringLiteral("schema"), QStringLiteral("identity"), QStringLiteral("length"),
        QStringLiteral("sha256"), QStringLiteral("etag")};
    const auto keys = saved.keys();
    const auto tag = saved.value(QStringLiteral("etag")).toString().toLatin1();
    const bool bound = QSet<QString>(keys.cbegin(), keys.cend()) == fields &&
        saved.value(QStringLiteral("schema")).toString() == QLatin1String("gsw.native_update_resume/1") &&
        saved.value(QStringLiteral("identity")).toString() == request.identity &&
        saved.value(QStringLiteral("length")).isDouble() &&
        saved.value(QStringLiteral("length")).toDouble() == double(request.expectedLength) &&
        saved.value(QStringLiteral("sha256")).toString() == request.sha256 && strongEtag(tag);
    if (QFileInfo::exists(partPath)) {
      if (!openOwnedPart(part, partPath, false)) {
        finish(QStringLiteral("download_path_unsafe")); return false;
      }
      ownsPart = true;
    }
    if (bound && part.isOpen() && part.size() > 0 && part.size() <= request.expectedLength) {
      ownsResume = true; resumeValid = true; entityTag = tag; bytes = part.size();
      return true;
    }
    // These are the helper's documented owned cache siblings, never a trust
    // high-water/root store. Unbound/corrupt resume bytes may be discarded.
    part.close();
    if (ownsPart) QFile::remove(partPath);
    if (QFileInfo::exists(resumePath)) { ownsResume = true; QFile::remove(resumePath); }
    ownsPart = false; ownsResume = false; resumeValid = false;
    entityTag.clear(); bytes = 0;
    return true;
  }

  bool saveResume(const QByteArray &tag) {
    resumeValid = false;
    const QString path = request.destinationPath + QStringLiteral(".resume.json");
    if (!ordinaryLocalPath(path)) { finish(QStringLiteral("download_path_unsafe")); return false; }
    ownsResume = true;
    entityTag = strongEtag(tag) ? tag : QByteArray();
    if (entityTag.isEmpty()) { QFile::remove(path); return true; }
    const QJsonObject record{{QStringLiteral("schema"), QStringLiteral("gsw.native_update_resume/1")},
        {QStringLiteral("identity"), request.identity}, {QStringLiteral("length"), request.expectedLength},
        {QStringLiteral("sha256"), request.sha256}, {QStringLiteral("etag"), QString::fromLatin1(entityTag)}};
    QSaveFile file(path); file.setDirectWriteFallback(false);
    const auto raw = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(raw) != raw.size() || !file.commit()) {
      finish(QStringLiteral("download_resume_write_failed")); return false;
    }
    resumeValid = true;
    return true;
  }

  void begin() {
    static const QRegularExpression sha(QStringLiteral("^[a-f0-9]{64}$"));
    if (policy.approvedHosts.isEmpty() || policy.approvedHosts.size() > 32 ||
        policy.maximumLength <= 0 || policy.maximumLength > maximumArchiveLength ||
        policy.inactivityTimeoutMs < 100 || policy.inactivityTimeoutMs > 600000 ||
        policy.totalTimeoutMs < 100 || policy.totalTimeoutMs > 3600000 ||
        policy.maximumAttempts < 1 || policy.maximumAttempts > 5 ||
        policy.maximumRedirects < 0 || policy.maximumRedirects > 3 ||
        policy.retryDelayMs < 0 || policy.retryDelayMs > 5000 ||
        !policy.sslConfiguration.backendConfiguration().isEmpty()) {
      finish(QStringLiteral("download_policy_invalid")); return;
    }
    for (const auto &host : policy.approvedHosts) {
      if (host.isEmpty() || host != host.trimmed() || hostKey(host).isEmpty() ||
          host.contains(QRegularExpression(QStringLiteral("[/@:*\\\\]")))) {
        finish(QStringLiteral("download_policy_invalid")); return;
      }
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
    if (!readResume()) return;
    // All ancestors have already been checked as ordinary local paths. On
    // Windows their drive root is the same volume and avoids a long-path API
    // limit in a storage query for a deeply nested Unicode cache directory.
#ifdef Q_OS_WIN
    const auto storagePath = QDir::fromNativeSeparators(request.destinationPath).left(3);
#else
    const auto storagePath = QFileInfo(request.destinationPath).absolutePath();
#endif
    QStorageInfo storage(storagePath); storage.refresh();
    const qint64 peakAdditional = (request.expectedLength - bytes) + request.expectedLength;
    if (!storage.isValid() || !storage.isReady() || storage.isReadOnly() ||
        storage.bytesAvailable() < peakAdditional + downloadFreeSpaceReserve) {
      finish(QStringLiteral("download_insufficient_space")); return;
    }
    if (bytes == request.expectedLength) {
      resumed = true;
      beginVerify();
      return;
    }
    issueRequest(request.url);
  }

  void issueRequest(const QUrl &url) {
    if (!active) return;
    if (!approved(url)) { finish(QStringLiteral("download_redirect_forbidden")); return; }
    activeUrl = url;
    headersAccepted = false;
    httpStatus = 0;
    requestOffset = bytes;
    QNetworkRequest wire(url);
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
    if (requestOffset > 0) {
      wire.setRawHeader("Range", "bytes=" + QByteArray::number(requestOffset) + '-');
      wire.setRawHeader("If-Range", entityTag);
    }
    wire.setTransferTimeout(policy.inactivityTimeoutMs);
    auto tls = policy.sslConfiguration;
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    tls.setPrivateKey(QSslKey());
    tls.setLocalCertificateChain({});
    wire.setSslConfiguration(tls);
    const auto token = generation;
    if (!transition(State::Downloading) || !active || token != generation) return;
    if (!emitProgress() || !active || token != generation) return;
    reply = manager.get(wire);
    reply->setReadBufferSize(64 * 1024);
    auto *current = reply.data();
    const auto serial = ++responseSerial;
    QObject::connect(current, &QNetworkReply::metaDataChanged, q, [this, current, serial] {
      if (active && reply == current && responseSerial == serial) acceptHeaders();
    });
    QObject::connect(current, &QNetworkReply::readyRead, q, [this, current, serial] {
      if (active && reply == current && responseSerial == serial) consume();
    });
    QObject::connect(current, &QNetworkReply::sslErrors, q, [this, current, serial](const QList<QSslError> &) {
      if (active && reply == current && responseSerial == serial) finish(QStringLiteral("download_tls_failed"));
    });
    QObject::connect(current, &QNetworkReply::finished, q, [this, current, serial] {
      if (active && reply == current && responseSerial == serial) received();
    });
  }

  void followRedirect() {
    if (redirects >= policy.maximumRedirects) { finish(QStringLiteral("download_redirect_limit")); return; }
    const auto location = reply->rawHeader("Location");
    if (location.isEmpty() || location.size() > 8192) { finish(QStringLiteral("download_redirect_invalid")); return; }
    const auto relative = QUrl::fromEncoded(location, QUrl::StrictMode);
    if (!relative.isValid()) { finish(QStringLiteral("download_redirect_invalid")); return; }
    const auto target = activeUrl.resolved(relative);
    if (!approved(target)) { finish(QStringLiteral("download_redirect_forbidden")); return; }
    ++redirects;
    QObject::disconnect(reply, nullptr, q, nullptr);
    auto *previous = reply.data(); reply = nullptr;
    const auto serial = ++responseSerial;
    previous->abort(); previous->deleteLater();
    const auto token = generation;
    // Rebuild a new identity-encoded request after the old signal completes.
    // No cookies, credentials or response-controlled headers are forwarded.
    QTimer::singleShot(0, q, [this, target, token, serial] {
      if (active && generation == token && responseSerial == serial) issueRequest(target);
    });
  }

  void retryLater(const QString &exhaustedCode) {
    if (attempts >= policy.maximumAttempts) { finish(exhaustedCode); return; }
    if (part.isOpen() && !part.flush()) { finish(QStringLiteral("download_write_failed")); return; }
    // Only the original signed identity/length/hash plus a strong validator
    // can keep an already-owned prefix across a retry. A response body is not
    // accepted until its next 200/206 headers independently pass validation.
    if (bytes > 0 && (!resumeValid || !strongEtag(entityTag))) {
      part.close();
      const auto partPath = request.destinationPath + QStringLiteral(".part");
      const auto resumePath = request.destinationPath + QStringLiteral(".resume.json");
      if ((ownsPart && !ordinaryLocalPath(partPath)) || (ownsResume && !ordinaryLocalPath(resumePath))) {
        finish(QStringLiteral("download_path_unsafe")); return;
      }
      if ((ownsPart && QFileInfo::exists(partPath) && !QFile::remove(partPath)) ||
          (ownsResume && QFileInfo::exists(resumePath) && !QFile::remove(resumePath))) {
        finish(QStringLiteral("download_write_failed")); return;
      }
      ownsPart = false; ownsResume = false; resumeValid = false;
      entityTag.clear(); bytes = 0; resumed = false;
    }
    if (reply) {
      QObject::disconnect(reply, nullptr, q, nullptr);
      auto *previous = reply.data(); reply = nullptr;
      previous->abort(); previous->deleteLater();
    }
    const auto serial = ++responseSerial;
    const auto token = generation;
    const auto target = activeUrl;
    ++attempts;
    const int delay = qMin(5000, policy.retryDelayMs * (1 << (attempts - 2)));
    if (!transition(State::RetryWait) || !active || generation != token) return;
    QTimer::singleShot(delay, q, [this, target, token, serial] {
      if (!active || generation != token || responseSerial != serial) return;
      if (!deadline.isActive() || deadline.remainingTime() <= 0) {
        finish(QStringLiteral("download_total_timeout")); return;
      }
      issueRequest(target); // The original total deadline is never restarted.
    });
  }

  bool acceptHeaders() {
    if (!active || !reply) return false;
    if (headersAccepted) return true;
    httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus < 200) return false;
    if (!reply->attribute(QNetworkRequest::ConnectionEncryptedAttribute).toBool()) {
      finish(QStringLiteral("download_tls_failed")); return false;
    }
    if (httpStatus == 301 || httpStatus == 302 || httpStatus == 303 || httpStatus == 307 || httpStatus == 308) {
      followRedirect(); return false;
    }
    if (httpStatus == 503) { retryLater(QStringLiteral("download_http_error")); return false; }
    if (httpStatus != 200 && httpStatus != 206) { finish(QStringLiteral("download_http_error")); return false; }
    const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
    if (!encoding.isEmpty() && encoding != "identity") {
      finish(QStringLiteral("download_content_encoding")); return false;
    }
    if (httpStatus == 206) {
      const auto rawRange = reply->rawHeader("Content-Range").trimmed();
      static const QRegularExpression range(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$"));
      const auto match = range.match(QString::fromLatin1(rawRange));
      bool a = false, b = false, c = false;
      const qint64 start = match.captured(1).toLongLong(&a);
      const qint64 end = match.captured(2).toLongLong(&b);
      const qint64 total = match.captured(3).toLongLong(&c);
      if (!requestOffset || !match.hasMatch() || !a || !b || !c || start != requestOffset ||
          end != request.expectedLength - 1 || total != request.expectedLength ||
          reply->rawHeader("ETag").trimmed() != entityTag) {
        resumeValid = false;
        // Never consume an unusable 206 body or splice it into the owned
        // prefix. The ordinary bounded retry discards that now-unbound cache
        // and reissues the same signed identity from zero under its deadline.
        retryLater(QStringLiteral("download_resume_rejected")); return false;
      }
    }
    const auto length = reply->rawHeader("Content-Length").trimmed();
    bool goodLength = false;
    const qint64 declared = length.toLongLong(&goodLength);
    const qint64 responseLength = request.expectedLength - (httpStatus == 206 ? requestOffset : 0);
    if (!length.isEmpty() && (!goodLength || declared != responseLength)) {
      finish(QStringLiteral("download_length_mismatch")); return false;
    }
    if (httpStatus == 200) {
      part.close();
      if (!openOwnedPart(part, request.destinationPath + QStringLiteral(".part"), true)) {
        finish(QStringLiteral("download_write_failed")); return false;
      }
      ownsPart = true; bytes = 0; resumed = false;
    } else {
      resumed = true;
    }
    if (!saveResume(reply->rawHeader("ETag").trimmed())) return false;
    headersAccepted = true;
    if (!emitProgress()) return false;
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
    const auto token = generation;
    const auto serial = responseSerial;
    consume();
    if (!alive) return;
    if (token != generation || serial != responseSerial) return;
    if (!active || !reply) return;
    if (reply->error() == QNetworkReply::RemoteHostClosedError) {
      retryLater(networkCode(reply->error())); return;
    }
    if (reply->error() != QNetworkReply::NoError) { finish(networkCode(reply->error())); return; }
    if (!headersAccepted || bytes != request.expectedLength || !part.flush() || part.size() != bytes) {
      finish(QStringLiteral("download_length_mismatch")); return;
    }
    QObject::disconnect(reply, nullptr, q, nullptr); reply->deleteLater(); reply = nullptr;
    beginVerify();
  }

  void beginVerify() {
    const auto token = generation;
    if (!part.seek(0)) { finish(QStringLiteral("download_read_failed")); return; }
    output = std::make_unique<QSaveFile>(request.destinationPath);
    output->setDirectWriteFallback(false);
    if (!output->open(QIODevice::WriteOnly)) { finish(QStringLiteral("download_write_failed")); return; }
    digest.reset();
    if (!transition(State::Verifying) || !active || token != generation) return;
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
        resumeValid = false;
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
  m->ownsResume = false; m->resumeValid = false; m->resumed = false; m->requestOffset = 0; m->entityTag.clear();
  m->redirects = 0; m->attempts = 1; m->activeUrl = {};
  const auto token = ++m->generation;
  QTimer::singleShot(0, this, [this, token] { if (m->active && token == m->generation) m->begin(); });
  return true;
}
void NativeUpdateDownload::cancel() { m->finish(QStringLiteral("download_cancelled"), true); }
NativeUpdateDownload::State NativeUpdateDownload::state() const { return m->state; }
qint64 NativeUpdateDownload::receivedBytes() const { return m->bytes; }
} // namespace gsw
