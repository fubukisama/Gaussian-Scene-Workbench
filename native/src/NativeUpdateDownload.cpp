#include "NativeUpdateDownload.h"
#include <QTimer>

namespace gsw {
class NativeUpdateDownload::Impl {
public:
  State state = State::Idle;
  bool active = false;
  qint64 bytes = 0;
};
NativeUpdateDownload::NativeUpdateDownload(QObject *parent) : QObject(parent), m(std::make_unique<Impl>()) {
  qRegisterMetaType<NativeUpdateDownloadResult>();
}
NativeUpdateDownload::~NativeUpdateDownload() = default;
bool NativeUpdateDownload::start(const NativeUpdateDownloadRequest &, const NativeUpdateDownloadPolicy &) {
  if (m->active) return false;
  m->active = true;
  QTimer::singleShot(0, this, [this] {
    m->active = false;
    m->state = State::Failed;
    emit stateChanged(m->state);
    NativeUpdateDownloadResult result; result.code = QStringLiteral("download_not_implemented");
    emit finished(result);
  });
  return true;
}
void NativeUpdateDownload::cancel() {}
NativeUpdateDownload::State NativeUpdateDownload::state() const { return m->state; }
qint64 NativeUpdateDownload::receivedBytes() const { return m->bytes; }
} // namespace gsw
