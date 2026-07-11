#pragma once

#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace gsw {

class ProcessSupervisor final : public QObject {
  Q_OBJECT

public:
  explicit ProcessSupervisor(QObject *parent = nullptr);

  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] QString activeTask() const;
  bool start(const QString &taskName, const QString &program, const QStringList &arguments,
             const QString &workingDirectory = {},
             const QProcessEnvironment &environment = {},
             bool acceptsCancelCommand = false);
  void stop();

signals:
  void taskStarted(const QString &taskName);
  void outputReady(const QString &text);
  void taskFinished(const QString &taskName, int exitCode, bool succeeded);
  void runningChanged(bool running);

private:
  void drainOutput();

  QProcess mProcess;
  QString mActiveTask;
  bool mAcceptsCancelCommand = false;
};

} // namespace gsw
