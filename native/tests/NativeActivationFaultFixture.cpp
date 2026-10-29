// SPDX-License-Identifier: GPL-3.0-or-later
// Remote-only console fixture. This target is never installed or packaged.
#include "NativeActivationCore.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>
#include <cstdlib>

#ifndef GSW_ACTIVATION_FAULT_FIXTURE
#error This source must only be built as the separate fault-injection fixture.
#endif

namespace {
QString requestedCheckpoint;
void record(const QJsonObject &value) {
  QTextStream stream(stdout);
  stream << QJsonDocument(value).toJson(QJsonDocument::Compact) << '\n';
  stream.flush();
}
}

// NativeActivationCore calls this only in its separately compiled fixture copy.
// The parent must really terminate this process; reaching this marker is not a
// simulated crash or a constructed on-disk state. A bounded self-exit prevents
// an abandoned test from leaving a permanent process behind.
void gswActivationFixtureCheckpoint(const char *checkpoint) {
  if (requestedCheckpoint != QString::fromLatin1(checkpoint)) return;
  record({{"checkpoint", QString::fromLatin1(checkpoint)},
          {"pid", QCoreApplication::applicationPid()}, {"testOnly", true}});
  QThread::sleep(30);
  std::_Exit(85);
}

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  const auto arguments = application.arguments();
  if (arguments.size() == 2 && arguments.at(1) == QLatin1String("--slot-probe")) {
    record({{"probeReady", true}, {"pid", QCoreApplication::applicationPid()},
            {"testOnly", true}, {"formalGswAcceptance", false}});
    return 0;
  }
  if (arguments.size() != 6 || arguments.at(1) != QLatin1String("--publish-fixture")) return 64;
  requestedCheckpoint = arguments.at(5);
  if (!QStringList{QStringLiteral("-"), QStringLiteral("extract-file-written"),
          QStringLiteral("slot-published"), QStringLiteral("pointer-before"),
          QStringLiteral("pointer-after")}.contains(requestedCheckpoint)) return 64;
  QFile offer(arguments.at(4));
  if (!offer.open(QIODevice::ReadOnly) || offer.size() > 4 * 1024 * 1024) return 65;
  gsw::NativeInstallationLock lock(arguments.at(2));
  if (!lock.locked()) {
    record({{"ok", false}, {"code", lock.error()}, {"testOnly", true}});
    return 2;
  }
  const auto result = gsw::NativeDeveloperPublisher::publish(arguments.at(2),
      arguments.at(3), offer.readAll(), lock);
  record({{"ok", result.ok}, {"code", result.code}, {"buildId", result.buildId},
          {"testOnly", true}, {"publisherAuthenticated", false}});
  return result.ok ? 0 : 2;
}
