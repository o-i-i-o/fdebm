#include "helperservice.h"
#include "aptbackend.h"
#include "localpackages.h"
#include "packageargs.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDebug>
#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <memory>

namespace
{
const char kErrorBusy[] = "org.fdebm.Helper1.Error.Busy";
const char kErrorNotAuthorized[] = "org.fdebm.Helper1.Error.NotAuthorized";
const char kErrorInvalidArgument[] = "org.fdebm.Helper1.Error.InvalidArgument";

// 工作线程的校验结果；packages 持有授权前打开的 fd，跨线程传递只共享所有权
struct LocalPackageRequestResult
{
    QString error;
    std::shared_ptr<LocalPackageSet> packages;
};

// /proc/<pid>/stat 的第 22 个字段，用于消除 PID 复用导致的身份歧义
bool readProcessStartTime(quint32 pid, quint64 *startTime)
{
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray data = file.readAll();
    const int closeParen = data.lastIndexOf(')');
    if (closeParen < 0 || closeParen + 2 >= data.size()) {
        return false;
    }

    // ')' 之后从第 3 个字段开始，starttime 是第 22 个字段，即这里的下标 19
    const QList<QByteArray> fields = data.mid(closeParen + 2).split(' ');
    if (fields.size() < 20) {
        return false;
    }

    bool ok = false;
    const quint64 value = fields.at(19).toULongLong(&ok);
    if (!ok) {
        return false;
    }

    *startTime = value;
    return true;
}
}

HelperService::HelperService(AptBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_authorizer(this)
{
    connect(m_backend, &AptBackend::output, this, &HelperService::onBackendOutput);
    connect(m_backend, &AptBackend::finished, this, &HelperService::onBackendFinished);
}

QString HelperService::InstallLocalPackages(const QStringList &paths)
{
    if (!calledFromDBus()) {
        return QString();
    }

    const QDBusMessage call = message();
    const QString sender = call.service();

    if (m_backend->isBusy() || m_pendingSenders.contains(sender)) {
        sendErrorReply(QLatin1String(kErrorBusy), QStringLiteral("已有操作正在进行"));
        return QString();
    }

    // 校验与打开 fd 需要读磁盘，放到工作线程执行，避免未授权请求阻塞 D-Bus 主线程
    setDelayedReply(true);

    QFutureWatcher<LocalPackageRequestResult> *watcher =
            new QFutureWatcher<LocalPackageRequestResult>(this);
    connect(watcher, &QFutureWatcher<LocalPackageRequestResult>::finished, this,
            [this, watcher, call, sender]() {
        watcher->deleteLater();
        const LocalPackageRequestResult result = watcher->result();

        // 授权前仍持有已校验的 fd（防 TOCTOU），但差异原因会暴露 root 可见的
        // 文件存在性/权限，因此只写日志，对外统一返回同一条错误
        if (!result.error.isEmpty()) {
            qWarning() << "拒绝本地安装请求:" << result.error;
            sendError(call, QLatin1String(kErrorInvalidArgument),
                      QStringLiteral("本地安装包校验失败（详情见系统日志）"));
            return;
        }

        // C++11 不支持 init-capture，先取出再按值捕获
        const std::shared_ptr<LocalPackageSet> packages = result.packages;
        handleAuthorized(call, sender, QStringLiteral("org.fdebm.helper.install"),
                         [this, packages]() {
            return m_backend->installLocalPackages(packages);
        });
    });
    watcher->setFuture(QtConcurrent::run([paths]() {
        LocalPackageRequestResult result;

        QStringList canonicalPaths;
        result.error = PackageArgs::validateLocalDebPaths(paths, &canonicalPaths);
        if (!result.error.isEmpty()) {
            return result;
        }

        // 授权前打开并持有描述符，授权结束后只从这些 fd 暂存，
        // 期间路径被替换为其他普通文件也不会改变实际内容来源
        std::shared_ptr<LocalPackageSet> packages = std::make_shared<LocalPackageSet>();
        packages->open(canonicalPaths, &result.error);
        result.packages = packages;
        return result;
    }));

    return QString();
}

QString HelperService::InstallPackages(const QStringList &names)
{
    const QString error = PackageArgs::validatePackageNames(names);
    handleRequest(QStringLiteral("org.fdebm.helper.install"), error,
                  [this, names]() {
        return m_backend->installPackages(names);
    });
    return QString();
}

QString HelperService::RemovePackages(const QStringList &names, bool purge)
{
    const QString error = PackageArgs::validatePackageNames(names);
    handleRequest(QStringLiteral("org.fdebm.helper.remove"), error,
                  [this, names, purge]() {
        return m_backend->removePackages(names, purge);
    });
    return QString();
}

QString HelperService::UpdateCache()
{
    handleRequest(QStringLiteral("org.fdebm.helper.update-cache"), QString(),
                  [this]() {
        return m_backend->updateCache();
    });
    return QString();
}

QString HelperService::Upgrade(bool distUpgrade)
{
    handleRequest(QStringLiteral("org.fdebm.helper.upgrade"), QString(),
                  [this, distUpgrade]() {
        return m_backend->upgrade(distUpgrade);
    });
    return QString();
}

QString HelperService::AutoRemove()
{
    handleRequest(QStringLiteral("org.fdebm.helper.remove"), QString(),
                  [this]() {
        return m_backend->autoRemove();
    });
    return QString();
}

bool HelperService::Cancel(const QString &transaction)
{
    if (!calledFromDBus()) {
        return false;
    }

    if (transaction.isEmpty()) {
        sendErrorReply(QLatin1String(kErrorInvalidArgument), QStringLiteral("事务标识为空"));
        return false;
    }

    const QString owner = m_destinations.value(transaction);
    if (owner.isEmpty()) {
        sendErrorReply(QLatin1String(kErrorInvalidArgument), QStringLiteral("事务不存在或已结束"));
        return false;
    }
    // 取消只是终止自己的能力，不涉及提权，但必须限制为事务发起方
    if (owner != message().service()) {
        sendErrorReply(QLatin1String(kErrorNotAuthorized), QStringLiteral("无权取消该事务"));
        return false;
    }

    if (!m_backend->cancel(transaction)) {
        sendErrorReply(QLatin1String(kErrorInvalidArgument), QStringLiteral("事务不存在或已结束"));
        return false;
    }
    return true;
}

void HelperService::handleRequest(const QString &action, const QString &validationError,
                                  const std::function<QString()> &startOperation)
{
    if (!calledFromDBus()) {
        return;
    }

    if (!validationError.isEmpty()) {
        sendErrorReply(QLatin1String(kErrorInvalidArgument), validationError);
        return;
    }

    const QDBusMessage call = message();
    setDelayedReply(true);
    handleAuthorized(call, call.service(), action, startOperation);
}

void HelperService::handleAuthorized(const QDBusMessage &call, const QString &sender,
                                     const QString &action,
                                     const std::function<QString()> &startOperation)
{
    if (sender.isEmpty()) {
        sendError(call, QLatin1String(kErrorNotAuthorized), QStringLiteral("无法确定调用方身份"));
        return;
    }

    // 同一调用方只保留一个待授权请求，避免重复弹窗；不同用户可并行授权，
    // 任何一个 polkit 回调丢失都不会永久阻塞其他调用方
    if (m_pendingSenders.contains(sender)) {
        sendError(call, QLatin1String(kErrorBusy), QStringLiteral("已有请求正在等待授权"));
        return;
    }
    if (m_backend->isBusy()) {
        sendError(call, QLatin1String(kErrorBusy), QStringLiteral("已有操作正在进行"));
        return;
    }

    QDBusConnectionInterface *busInterface = QDBusConnection::systemBus().interface();
    const uint pid = busInterface ? busInterface->servicePid(sender) : 0u;
    quint64 startTime = 0;
    if (pid == 0 || !readProcessStartTime(pid, &startTime)) {
        sendError(call, QLatin1String(kErrorNotAuthorized), QStringLiteral("无法确定调用方进程身份"));
        return;
    }

    m_pendingSenders.insert(sender);

    m_authorizer.check(action, pid, startTime,
                       [this, call, sender, startOperation](bool authorized, const QString &error) {
        m_pendingSenders.remove(sender);

        if (!authorized) {
            sendError(call, QLatin1String(kErrorNotAuthorized),
                      error.isEmpty() ? QStringLiteral("未获得授权") : error);
            return;
        }

        // 等待授权期间可能已有另一个请求启动了事务
        if (m_backend->isBusy()) {
            sendError(call, QLatin1String(kErrorBusy), QStringLiteral("已有操作正在进行"));
            return;
        }

        m_backend->clearLastError();
        const QString transaction = startOperation();
        if (transaction.isEmpty()) {
            const QString backendError = m_backend->lastError();
            if (!backendError.isEmpty()) {
                sendError(call, QLatin1String(kErrorInvalidArgument), backendError);
            } else {
                sendError(call, QLatin1String(kErrorBusy), QStringLiteral("已有操作正在进行"));
            }
            return;
        }

        m_destinations.insert(transaction, sender);
        QDBusConnection::systemBus().send(call.createReply(QVariantList{transaction}));
    });
}

void HelperService::sendError(const QDBusMessage &call, const QString &name, const QString &message)
{
    QDBusConnection::systemBus().send(call.createErrorReply(name, message));
}

void HelperService::onBackendOutput(const QString &transaction, const QString &text)
{
    sendSignal(QStringLiteral("Output"),
               QVariantList{transaction, text},
               m_destinations.value(transaction));
}

void HelperService::onBackendFinished(const QString &transaction, int exitCode, const QString &message)
{
    sendSignal(QStringLiteral("Finished"),
               QVariantList{transaction, exitCode, message},
               m_destinations.value(transaction));
    m_destinations.remove(transaction);
}

void HelperService::sendSignal(const QString &member, const QVariantList &arguments,
                               const QString &destination)
{
    if (destination.isEmpty()) {
        qWarning() << "跳过无接收方的信号:" << member;
        return;
    }

    QDBusMessage signal = QDBusMessage::createTargetedSignal(destination,
                                                             QStringLiteral("/org/fdebm/Helper"),
                                                             QStringLiteral("org.fdebm.Helper1"),
                                                             member);
    signal.setArguments(arguments);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.send(signal)) {
        qWarning() << "发送信号失败:" << member << bus.lastError().message();
    }
}
