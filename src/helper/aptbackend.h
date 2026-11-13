#ifndef APTBACKEND_H
#define APTBACKEND_H

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <memory>

class LocalPackageSet;

// 工作线程的暂存结果，仅包含路径与错误，不触碰主线程状态
struct StageResult
{
    QStringList staged;
    QString error;
};

class AptBackend : public QObject
{
    Q_OBJECT

public:
    explicit AptBackend(QObject *parent = nullptr);

    void setDryRun(bool dryRun);
    void setDryRunDelayMs(int delayMs);
    void setStagingDir(const QString &dir);
    bool isBusy() const;
    QString lastError() const { return m_lastError; }
    void clearLastError() { m_lastError.clear(); }

    QString installLocalPackages(const std::shared_ptr<LocalPackageSet> &packages);
    QString installPackages(const QStringList &names);
    QString removePackages(const QStringList &names, bool purge);
    QString updateCache();
    QString upgrade(bool distUpgrade);
    QString autoRemove();
    bool cancel(const QString &transaction);

signals:
    void output(const QString &transaction, const QString &text);
    void finished(const QString &transaction, int exitCode, const QString &message);

private:
    QString begin(const QString &program, const QStringList &arguments);
    bool startProcess(const QString &program, const QStringList &arguments);
    bool ensureStagingDir(QString *error);
    bool verifyLocalPackages(const LocalPackageSet &packages, QString *error);
    void onStagingFinished(const QString &transaction, const StageResult &result);
    void clearStagingDirectory();
    void cleanupStagedFiles();
    void readStandardOutput();
    void readStandardError();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void finish(const QString &transaction, int exitCode, const QString &message);

    QProcess *m_process = nullptr;
    QString m_transaction;
    QString m_stagingDir = QStringLiteral("/var/lib/fdebm/staging");
    QStringList m_stagedFiles;
    QString m_lastError;
    int m_counter = 0;
    bool m_dryRun = false;
    int m_dryRunDelayMs = 300;
    bool m_cancelRequested = false;
    // 暂存拷贝正在工作线程进行，取消时先标记，由 onStagingFinished 收尾
    bool m_preparing = false;
};

#endif // APTBACKEND_H
