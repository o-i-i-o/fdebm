#include "helperclient.h"
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QVariant>

namespace
{
const char kService[] = "org.fdebm.Helper";
const char kPath[] = "/org/fdebm/Helper";
const char kInterface[] = "org.fdebm.Helper1";

// 助手等待 polkit 最长 120 秒，客户端需更宽限
const int kCallTimeoutMs = 150000;
// 事务受理后若长时间没有任何输出，提示用户可取消
const int kStallTimeoutMs = 10 * 60 * 1000;
// 拿到事务 ID 之前最多缓冲的信号数；超出部分直接丢弃
const int kMaxPendingSignals = 256;

QString friendlyError(const QString &name, const QString &message)
{
    QString detail = message.trimmed();
    if (detail.isEmpty()) {
        detail = QStringLiteral("未知错误");
    }

    if (name == QLatin1String("org.fdebm.Helper1.Error.NotAuthorized")) {
        return QStringLiteral("未获得授权：%1").arg(detail);
    }
    if (name == QLatin1String("org.fdebm.Helper1.Error.Busy")) {
        return QStringLiteral("已有操作正在进行，请稍后再试");
    }
    if (name == QLatin1String("org.fdebm.Helper1.Error.InvalidArgument")) {
        return QStringLiteral("参数无效：%1").arg(detail);
    }
    if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
        || name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")) {
        return QStringLiteral("包管理助手服务未运行，请检查 fdebm-helperd 是否已启动");
    }
    if (name == QLatin1String("org.freedesktop.DBus.Error.NoReply")) {
        return QStringLiteral("等待助手响应超时：%1").arg(detail);
    }
    if (name == QLatin1String("org.freedesktop.DBus.Error.Disconnected")) {
        return QStringLiteral("与系统总线的连接已断开");
    }
    return detail;
}
}

HelperClient::HelperClient(QObject *parent)
    : QObject(parent)
{
    m_stallTimer.setSingleShot(true);
    m_stallTimer.setInterval(kStallTimeoutMs);
    connect(&m_stallTimer, &QTimer::timeout, this, [this]() {
        if (m_state != State::Active || m_transaction.isEmpty()) {
            return;
        }
        emit stalled(m_transaction);
        // 保持提示周期，事务一直没有输出会反复通知
        m_stallTimer.start();
    });

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
                     QStringLiteral("Output"), this, SLOT(onRemoteOutput(QString,QString)))) {
        qWarning() << "订阅助手 Output 信号失败:" << bus.lastError().message();
    }
    if (!bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
                     QStringLiteral("Finished"), this, SLOT(onRemoteFinished(QString,int,QString)))) {
        qWarning() << "订阅助手 Finished 信号失败:" << bus.lastError().message();
    }

    // 助手崩溃或被 systemd 重启后不会再有 Finished，必须主动收尾，否则界面会永久停在忙碌状态
    m_serviceWatcher = new QDBusServiceWatcher(
                QLatin1String(kService), bus,
                QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &oldOwner, const QString &newOwner) {
        if (oldOwner.isEmpty() || !newOwner.isEmpty() || m_state == State::Idle) {
            return;
        }

        ++m_requestSerial;
        m_state = State::Idle;
        m_transaction.clear();
        m_pendingSignals.clear();
        m_stallTimer.stop();
        emit failed(QStringLiteral("包管理助手服务已退出，操作状态未知，请重新打开程序确认系统状态"));
    });
}

void HelperClient::installLocalPackages(const QStringList &paths)
{
    call(QStringLiteral("InstallLocalPackages"), QVariantList{QVariant::fromValue(paths)});
}

void HelperClient::installPackages(const QStringList &names)
{
    call(QStringLiteral("InstallPackages"), QVariantList{QVariant::fromValue(names)});
}

void HelperClient::removePackages(const QStringList &names, bool purge)
{
    call(QStringLiteral("RemovePackages"), QVariantList{QVariant::fromValue(names), purge});
}

void HelperClient::updateCache()
{
    call(QStringLiteral("UpdateCache"), QVariantList());
}

void HelperClient::upgrade(bool distUpgrade)
{
    call(QStringLiteral("Upgrade"), QVariantList{distUpgrade});
}

void HelperClient::autoRemove()
{
    call(QStringLiteral("AutoRemove"), QVariantList());
}

void HelperClient::cancel()
{
    if (m_state != State::Active || m_transaction.isEmpty()) {
        emit cancelRejected(QStringLiteral("没有正在进行的操作"));
        return;
    }

    QDBusMessage request = QDBusMessage::createMethodCall(QLatin1String(kService),
                                                          QLatin1String(kPath),
                                                          QLatin1String(kInterface),
                                                          QStringLiteral("Cancel"));
    request.setArguments(QVariantList{m_transaction});

    auto *watcher = new QDBusPendingCallWatcher(
                QDBusConnection::systemBus().asyncCall(request, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *self) {
        self->deleteLater();

        const QDBusMessage reply = self->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            qWarning() << "取消请求失败:" << reply.errorName() << reply.errorMessage();
            emit cancelRejected(friendlyError(reply.errorName(), reply.errorMessage()));
        }
        // 取消受理后仍以 Finished 信号作为事务终点
    });
}

void HelperClient::call(const QString &method, const QVariantList &arguments)
{
    if (m_state != State::Idle) {
        emit failed(QStringLiteral("已有操作正在进行，请稍后再试"));
        return;
    }

    m_state = State::Pending;
    m_pendingSignals.clear();
    m_stallTimer.stop();
    const quint64 serial = ++m_requestSerial;

    QDBusMessage request = QDBusMessage::createMethodCall(QLatin1String(kService),
                                                          QLatin1String(kPath),
                                                          QLatin1String(kInterface),
                                                          method);
    request.setArguments(arguments);

    QDBusPendingCall pending = QDBusConnection::systemBus().asyncCall(request, kCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, serial](QDBusPendingCallWatcher *self) {
        self->deleteLater();

        // 助手已退出时该调用已被服务所有者变化处理过，忽略迟到的错误回复
        if (serial != m_requestSerial) {
            return;
        }

        const QDBusMessage reply = self->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            m_state = State::Idle;
            m_transaction.clear();
            m_pendingSignals.clear();
            qWarning() << "助手请求失败:" << reply.errorName() << reply.errorMessage();
            emit failed(friendlyError(reply.errorName(), reply.errorMessage()));
            return;
        }

        const QString transaction = reply.arguments().value(0).toString();
        if (transaction.isEmpty()) {
            m_state = State::Idle;
            m_transaction.clear();
            m_pendingSignals.clear();
            emit failed(QStringLiteral("助手返回了空的事务标识"));
            return;
        }

        m_transaction = transaction;
        m_state = State::Active;
        m_stallTimer.start();
        emit accepted(transaction);
        flushPendingSignals();
    });
}

void HelperClient::restartStallTimer()
{
    if (m_state == State::Active && !m_transaction.isEmpty()) {
        m_stallTimer.start();
    }
}

void HelperClient::bufferSignal(bool finished, const QString &transaction,
                                int exitCode, const QString &text)
{
    // 拿到事务 ID 之前无法判断信号归属，先缓冲并在受理后按 ID 过滤
    if (m_pendingSignals.size() >= kMaxPendingSignals) {
        return;
    }
    m_pendingSignals.append(PendingSignal{finished, transaction, exitCode, text});
}

void HelperClient::flushPendingSignals()
{
    const QString transaction = m_transaction;
    const QVector<PendingSignal> buffered = m_pendingSignals;
    m_pendingSignals.clear();

    for (const PendingSignal &signal : buffered) {
        if (m_state != State::Active || signal.transaction != transaction) {
            continue;
        }
        if (signal.finished) {
            handleFinished(signal.transaction, signal.exitCode, signal.text);
            continue;
        }
        restartStallTimer();
        emit output(signal.transaction, signal.text);
    }
}

void HelperClient::onRemoteOutput(const QString &transaction, const QString &text)
{
    if (m_state == State::Pending) {
        bufferSignal(false, transaction, 0, text);
        return;
    }
    if (m_state != State::Active || transaction != m_transaction) {
        // 事务已结束或不属于当前事务的迟到输出，直接丢弃
        return;
    }

    restartStallTimer();
    emit output(transaction, text);
}

void HelperClient::onRemoteFinished(const QString &transaction, int exitCode, const QString &message)
{
    if (m_state == State::Pending) {
        bufferSignal(true, transaction, exitCode, message);
        return;
    }
    if (m_state != State::Active || transaction != m_transaction) {
        qWarning() << "忽略迟到或不属于当前事务的 Finished:" << transaction;
        return;
    }

    handleFinished(transaction, exitCode, message);
}

void HelperClient::handleFinished(const QString &transaction, int exitCode, const QString &message)
{
    m_transaction.clear();
    m_state = State::Idle;
    m_stallTimer.stop();
    emit finished(transaction, exitCode, message);
}
