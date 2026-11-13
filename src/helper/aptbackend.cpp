#include "aptbackend.h"
#include "localpackages.h"
#include "programpaths.h"
#include "proxyconfig.h"
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QProcessEnvironment>
#include <QTemporaryFile>
#include <QTimer>
#include <QtConcurrent>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
QString systemError()
{
    return QString::fromLocal8Bit(std::strerror(errno));
}

void removeFiles(const QStringList &paths)
{
    for (const QString &path : paths) {
        QFile::remove(path);
    }
}

bool copyFileContents(int sourceFd, QTemporaryFile *target, QString *error)
{
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t readBytes = ::read(sourceFd, buffer, sizeof(buffer));
        if (readBytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = QStringLiteral("读取安装包失败：%1").arg(systemError());
            return false;
        }
        if (readBytes == 0) {
            break;
        }
        qint64 offset = 0;
        while (offset < readBytes) {
            const qint64 written = target->write(buffer + offset, readBytes - offset);
            if (written <= 0) {
                *error = QStringLiteral("写入暂存文件失败：%1").arg(target->errorString());
                return false;
            }
            offset += written;
        }
    }
    if (!target->flush()) {
        *error = QStringLiteral("写入暂存文件失败：%1").arg(target->errorString());
        return false;
    }
    return true;
}

// 只做 fd → 暂存文件的拷贝，在工作线程执行；不读写任何 AptBackend 成员
bool copyToStaging(const LocalPackageSet &packages, const QString &stagingDir,
                   QStringList *staged, QString *error)
{
    for (const LocalPackageSet::File &source : packages.files()) {
        QTemporaryFile target(QDir(stagingDir).filePath(QStringLiteral("deb-XXXXXX.deb")));
        target.setAutoRemove(false);
        if (!target.open()) {
            *error = QStringLiteral("无法创建暂存文件：%1").arg(target.errorString());
            removeFiles(*staged);
            staged->clear();
            return false;
        }
        target.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        // 只从授权前打开的 fd 读取，路径后续如何变化都不会换掉内容来源
        QString copyError;
        const bool copied = copyFileContents(source.fd, &target, &copyError);
        const qint64 stagedSize = target.size();
        const QString stagedPath = target.fileName();
        target.close();

        if (!copied) {
            QFile::remove(stagedPath);
            *error = copyError;
            removeFiles(*staged);
            staged->clear();
            return false;
        }
        if (quint64(stagedSize) != source.size) {
            QFile::remove(stagedPath);
            *error = QStringLiteral("%1 在读取过程中发生变化，已拒绝安装").arg(source.path);
            removeFiles(*staged);
            staged->clear();
            return false;
        }

        staged->append(stagedPath);
    }
    return true;
}
}

AptBackend::AptBackend(QObject *parent)
    : QObject(parent)
{
}

void AptBackend::setDryRun(bool dryRun)
{
    m_dryRun = dryRun;
}

void AptBackend::setDryRunDelayMs(int delayMs)
{
    if (delayMs >= 0) {
        m_dryRunDelayMs = delayMs;
    }
}

void AptBackend::setStagingDir(const QString &dir)
{
    if (!dir.isEmpty()) {
        m_stagingDir = dir;
    }
}

bool AptBackend::ensureStagingDir(QString *error)
{
    if (!QDir().mkpath(m_stagingDir)) {
        *error = QStringLiteral("无法创建暂存目录 %1").arg(m_stagingDir);
        return false;
    }

    const QByteArray encoded = QFile::encodeName(m_stagingDir);
    struct stat info;
    // lstat 不跟随符号链接：路径本身被替换为链接时直接拒绝
    if (::lstat(encoded.constData(), &info) != 0) {
        *error = QStringLiteral("无法访问暂存目录 %1：%2").arg(m_stagingDir, systemError());
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        *error = QStringLiteral("暂存路径 %1 不是目录").arg(m_stagingDir);
        return false;
    }

    const int fd = ::open(encoded.constData(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        *error = QStringLiteral("无法打开暂存目录 %1：%2").arg(m_stagingDir, systemError());
        return false;
    }

    struct stat opened;
    if (::fstat(fd, &opened) != 0) {
        *error = QStringLiteral("无法读取暂存目录 %1：%2").arg(m_stagingDir, systemError());
        ::close(fd);
        return false;
    }
    // 检查与打开必须是同一个目录，防止中间被替换
    if (opened.st_dev != info.st_dev || opened.st_ino != info.st_ino) {
        *error = QStringLiteral("暂存目录 %1 在检查期间被替换").arg(m_stagingDir);
        ::close(fd);
        return false;
    }
    // 只信任由当前身份独占的目录，避免其他用户预置文件或替换目录
    if (opened.st_uid != ::geteuid()) {
        *error = QStringLiteral("暂存目录 %1 的所有者不正确").arg(m_stagingDir);
        ::close(fd);
        return false;
    }
    if ((opened.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        *error = QStringLiteral("暂存目录 %1 允许其他用户写入，已拒绝使用").arg(m_stagingDir);
        ::close(fd);
        return false;
    }
    if (::fchmod(fd, S_IRWXU) != 0) {
        *error = QStringLiteral("无法设置暂存目录 %1 的权限：%2").arg(m_stagingDir, systemError());
        ::close(fd);
        return false;
    }

    ::close(fd);
    return true;
}

bool AptBackend::verifyLocalPackages(const LocalPackageSet &packages, QString *error)
{
    const QVector<LocalPackageSet::File> &sources = packages.files();
    if (sources.isEmpty()) {
        *error = QStringLiteral("没有可安装的本地安装包");
        return false;
    }

    for (const LocalPackageSet::File &source : sources) {
        const QString &path = source.path;
        struct stat info;

        // fd 指向授权前校验过的 inode，fstat 反映的是同一对象的最新状态
        if (::fstat(source.fd, &info) != 0) {
            *error = QStringLiteral("无法读取 %1 的信息：%2").arg(path, systemError());
            return false;
        }
        if (!S_ISREG(info.st_mode)) {
            *error = QStringLiteral("%1 不是普通文件").arg(path);
            return false;
        }
        if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            *error = QStringLiteral("%1 在授权期间允许其他用户写入，已拒绝安装").arg(path);
            return false;
        }
        if (info.st_size <= 0 || quint64(info.st_size) != source.size) {
            *error = QStringLiteral("%1 在授权期间发生变化，已拒绝安装").arg(path);
            return false;
        }

        // 路径若被删除、替换或改成符号链接，即使 fd 内容安全也应 fail-closed
        struct stat current;
        if (::lstat(QFile::encodeName(path).constData(), &current) != 0
                || !S_ISREG(current.st_mode)
                || quint64(current.st_dev) != source.device
                || quint64(current.st_ino) != source.inode) {
            *error = QStringLiteral("%1 在授权期间被替换，已拒绝安装").arg(path);
            return false;
        }
    }
    return true;
}

void AptBackend::clearStagingDirectory()
{
    QDir dir(m_stagingDir);
    if (!dir.exists()) {
        return;
    }
    const QStringList leftovers = dir.entryList(QStringList{QStringLiteral("deb-*.deb")},
                                                QDir::Files | QDir::Hidden | QDir::System);
    for (const QString &name : leftovers) {
        QFile::remove(dir.filePath(name));
    }
}

void AptBackend::cleanupStagedFiles()
{
    removeFiles(m_stagedFiles);
    m_stagedFiles.clear();
}

bool AptBackend::isBusy() const
{
    return m_process != nullptr || !m_transaction.isEmpty();
}

QString AptBackend::installLocalPackages(const std::shared_ptr<LocalPackageSet> &packages)
{
    m_lastError.clear();
    if (isBusy() || !packages) {
        return QString();
    }

    // 预检只做元数据访问，必须在回复事务 ID 前完成：
    // 授权期间被替换/放宽权限的包仍以方法错误立即拒绝
    QString error;
    if (!ensureStagingDir(&error)) {
        m_lastError = error;
        return QString();
    }
    if (!verifyLocalPackages(*packages, &error)) {
        m_lastError = error;
        return QString();
    }
    // 目录已确认安全，可清理上次异常退出遗留的暂存文件
    clearStagingDirectory();

    // 事务先分配并立即回复，耗时的大文件拷贝随后在工作线程进行，
    // 不会占用 D-Bus 主线程，也不会超过客户端的调用超时
    const QString transaction = QStringLiteral("tx-%1").arg(++m_counter);
    m_transaction = transaction;
    m_cancelRequested = false;
    m_preparing = true;

    const QString stagingDir = m_stagingDir;
    QFutureWatcher<StageResult> *watcher = new QFutureWatcher<StageResult>(this);
    connect(watcher, &QFutureWatcher<StageResult>::finished, this,
            [this, watcher, transaction]() {
        watcher->deleteLater();
        onStagingFinished(transaction, watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([packages, stagingDir]() {
        StageResult result;
        copyToStaging(*packages, stagingDir, &result.staged, &result.error);
        return result;
    }));

    return transaction;
}

void AptBackend::onStagingFinished(const QString &transaction, const StageResult &result)
{
    if (transaction != m_transaction) {
        removeFiles(result.staged);
        return;
    }
    m_preparing = false;

    if (!result.error.isEmpty()) {
        removeFiles(result.staged);
        m_transaction.clear();
        m_cancelRequested = false;
        m_lastError = result.error;
        emit finished(transaction, -1, result.error);
        return;
    }

    m_stagedFiles = result.staged;

    if (m_cancelRequested) {
        m_transaction.clear();
        m_cancelRequested = false;
        cleanupStagedFiles();
        emit finished(transaction, -1, QStringLiteral("操作已取消"));
        return;
    }

    QStringList arguments{QStringLiteral("install"), QStringLiteral("-y"), QStringLiteral("--")};
    arguments += m_stagedFiles;

    if (m_dryRun) {
        // 推迟到事件循环，确保 HelperService 已登记本事务的接收方
        QTimer::singleShot(0, this, [this, transaction, arguments]() {
            if (m_transaction != transaction) {
                return;
            }
            emit output(transaction,
                        QStringLiteral("[dry-run] %1 %2")
                        .arg(QStringLiteral("apt"), arguments.join(QLatin1Char(' '))));
        });
        // 保留一个事务窗口，便于联调时观察单播信号、Busy 拒绝与取消
        QTimer::singleShot(m_dryRunDelayMs, this, [this, transaction]() {
            if (m_transaction != transaction) {
                return;
            }
            emit output(transaction, QStringLiteral("[dry-run] 未执行任何命令"));
            finish(transaction, 0, QStringLiteral("dry-run 完成"));
        });
        return;
    }

    if (!startProcess(QStringLiteral("apt"), arguments)) {
        const QString processError = m_lastError;
        m_transaction.clear();
        m_cancelRequested = false;
        cleanupStagedFiles();
        emit finished(transaction, -1, processError);
    }
}

QString AptBackend::installPackages(const QStringList &names)
{
    QStringList arguments{QStringLiteral("install"), QStringLiteral("-y"), QStringLiteral("--")};
    arguments += names;
    return begin(QStringLiteral("apt"), arguments);
}

QString AptBackend::removePackages(const QStringList &names, bool purge)
{
    QStringList arguments{purge ? QStringLiteral("purge") : QStringLiteral("remove"),
                          QStringLiteral("-y"), QStringLiteral("--")};
    arguments += names;
    return begin(QStringLiteral("apt"), arguments);
}

QString AptBackend::updateCache()
{
    return begin(QStringLiteral("apt"), QStringList{QStringLiteral("update")});
}

QString AptBackend::upgrade(bool distUpgrade)
{
    return begin(QStringLiteral("apt"),
                 QStringList{distUpgrade ? QStringLiteral("dist-upgrade") : QStringLiteral("upgrade"),
                             QStringLiteral("-y")});
}

QString AptBackend::autoRemove()
{
    return begin(QStringLiteral("apt"),
                 QStringList{QStringLiteral("autoremove"), QStringLiteral("-y")});
}

bool AptBackend::cancel(const QString &transaction)
{
    if (transaction.isEmpty() || transaction != m_transaction) {
        return false;
    }

    if (m_preparing) {
        // 暂存拷贝仍在工作线程，标记后由 onStagingFinished 收尾
        m_cancelRequested = true;
        return true;
    }

    if (!m_process) {
        // dry-run 没有真实进程，直接结束事务；已排队的模拟输出会因事务
        // 标识不匹配而自行失效
        finish(transaction, -1, QStringLiteral("操作已取消"));
        return true;
    }

    m_cancelRequested = true;
    QProcess *process = m_process;
    process->terminate();

    // apt 可能阻塞在 dpkg 或锁等待中，宽限期后强制结束
    QTimer::singleShot(5000, this, [this, process]() {
        if (m_process == process && process->state() != QProcess::NotRunning) {
            process->kill();
        }
    });
    return true;
}

QString AptBackend::begin(const QString &program, const QStringList &arguments)
{
    if (isBusy()) {
        return QString();
    }

    const QString transaction = QStringLiteral("tx-%1").arg(++m_counter);
    m_transaction = transaction;
    m_cancelRequested = false;

    if (m_dryRun) {
        // 推迟到事件循环，确保 HelperService 已登记本事务的接收方
        QTimer::singleShot(0, this, [this, transaction, program, arguments]() {
            if (m_transaction != transaction) {
                return;
            }
            emit output(transaction,
                        QStringLiteral("[dry-run] %1 %2").arg(program, arguments.join(QLatin1Char(' '))));
        });
        // 保留一个事务窗口，便于联调时观察单播信号、Busy 拒绝与取消
        QTimer::singleShot(m_dryRunDelayMs, this, [this, transaction]() {
            if (m_transaction != transaction) {
                return;
            }
            emit output(transaction, QStringLiteral("[dry-run] 未执行任何命令"));
            finish(transaction, 0, QStringLiteral("dry-run 完成"));
        });
        return transaction;
    }

    if (!startProcess(program, arguments)) {
        m_transaction.clear();
        return QString();
    }

    return transaction;
}

bool AptBackend::startProcess(const QString &program, const QStringList &arguments)
{
    // 特权进程不继承调用方 PATH，程序必须从固定目录解析为绝对路径
    const QString programPath = ProgramPaths::findSystemExecutable(program);
    if (programPath.isEmpty()) {
        m_lastError = QStringLiteral("找不到系统命令 %1").arg(program);
        return false;
    }

    m_process = new QProcess(this);

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/sbin:/usr/bin:/sbin:/bin"));
    environment.insert(QStringLiteral("HOME"), QStringLiteral("/root"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    environment.insert(QStringLiteral("DEBIAN_FRONTEND"), QStringLiteral("noninteractive"));
    environment.insert(QStringLiteral("APT_LISTCHANGES_FRONTEND"), QStringLiteral("none"));
    // 登录会话的代理环境不进入特权进程；仅从 root 可写的固定配置转发白名单变量
    ProxyConfig::applyFromFile(QStringLiteral("/etc/fdebm/proxy.conf"), &environment);
    m_process->setProcessEnvironment(environment);
    m_process->setWorkingDirectory(QStringLiteral("/"));
    m_process->setStandardInputFile(QProcess::nullDevice());

    connect(m_process, &QProcess::readyReadStandardOutput, this, &AptBackend::readStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &AptBackend::readStandardError);
    connect(m_process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &AptBackend::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &AptBackend::onProcessError);
    m_process->start(programPath, arguments);

    return true;
}

void AptBackend::readStandardOutput()
{
    if (!m_process) {
        return;
    }
    const QString text = QString::fromLocal8Bit(m_process->readAllStandardOutput());
    if (!text.isEmpty()) {
        emit output(m_transaction, text);
    }
}

void AptBackend::readStandardError()
{
    if (!m_process) {
        return;
    }
    const QString text = QString::fromLocal8Bit(m_process->readAllStandardError());
    if (!text.isEmpty()) {
        emit output(m_transaction, text);
    }
}

void AptBackend::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_process) {
        return;
    }

    readStandardOutput();
    readStandardError();

    const QString transaction = m_transaction;
    QString message;
    if (exitCode == 0 && exitStatus == QProcess::NormalExit) {
        message = QStringLiteral("操作完成");
    } else if (m_cancelRequested) {
        message = QStringLiteral("操作已取消");
    } else {
        message = QStringLiteral("操作失败（退出码 %1）").arg(exitCode);
    }

    QProcess *process = m_process;
    m_process = nullptr;
    process->deleteLater();

    finish(transaction, exitCode, message);
}

void AptBackend::onProcessError(QProcess::ProcessError error)
{
    if (!m_process || error != QProcess::FailedToStart) {
        return;
    }

    const QString transaction = m_transaction;
    const QString message = m_process->errorString();

    QProcess *process = m_process;
    m_process = nullptr;
    process->deleteLater();

    finish(transaction, -1, message);
}

void AptBackend::finish(const QString &transaction, int exitCode, const QString &message)
{
    m_transaction.clear();
    m_cancelRequested = false;
    m_preparing = false;
    cleanupStagedFiles();
    emit finished(transaction, exitCode, message);
}
