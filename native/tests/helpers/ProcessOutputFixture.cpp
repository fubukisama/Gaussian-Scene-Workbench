#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdio>

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() >= 2 && arguments.at(1) == QStringLiteral("tree-child")) {
    QDeadlineTimer deadline(60000);
    while (!deadline.hasExpired()) {
      QThread::msleep(100);
    }
    return 0;
  }
  if (arguments.size() == 3 &&
      (arguments.at(1) == QStringLiteral("tree-parent") ||
       arguments.at(1) == QStringLiteral("tree-parent-exits"))) {
    qint64 childProcessId = 0;
    if (!QProcess::startDetached(application.applicationFilePath(),
                                 {QStringLiteral("tree-child")}, {},
                                 &childProcessId)) {
      return 6;
    }
    QFile ready(arguments.at(2));
    if (!ready.open(QIODevice::WriteOnly) ||
        ready.write(QByteArray::number(childProcessId)) <= 0) {
      return 7;
    }
    ready.close();
    if (arguments.at(1) == QStringLiteral("tree-parent-exits")) {
      QTimer::singleShot(300, &application, &QCoreApplication::quit);
    }
    return application.exec();
  }
  if (application.arguments().size() != 3) {
    return 2;
  }

#ifdef Q_OS_WIN
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  QFile output;
  if (!output.open(stdout, QIODevice::WriteOnly)) {
    return 3;
  }

  const QString readyPath = application.arguments().at(1);
  const QString releasePath = application.arguments().at(2);
  output.write("[worker-event] {\"version\":1,\"type\":\"status\",\"state\":\"run");
  output.flush();

  QFile ready(readyPath);
  if (!ready.open(QIODevice::WriteOnly) || ready.write("ready") != 5) {
    return 4;
  }
  ready.close();

  QDeadlineTimer deadline(10000);
  while (!QFileInfo::exists(releasePath) && !deadline.hasExpired()) {
    QThread::msleep(10);
  }
  if (!QFileInfo::exists(releasePath)) {
    return 5;
  }

  output.write("ning\",\"stage\":\"train\",\"progressPercent\":37}\nplain log\n");
  output.flush();
  return 0;
}
