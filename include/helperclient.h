#ifndef HELPERCLIENT_H
#define HELPERCLIENT_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVector>

class QDBusServiceWatcher;

class HelperClient : public QObject
{
    Q_OBJECT

public:
    explicit HelperClient(QObject *parent = nullptr);

    void installLocalPackages(const QStringList &paths);
    void installPackages(const QStringList &names);
    void removePackages(const QStringList &names, bool purge);
    void updateCache();
    void upgrade(bool distUpgrade);
    void autoRemove();
    void cancel();
    // 有请求在途（等待受理或执行中）时返回 true
    bool isBusy() const { return m_state != State::Idle; }

signals:
    // 事务已被助手接受；此后 Output/Finished 才会携带该 ID
    void accepted(const QString &transaction);
    // 请求被拒绝（未授权、忙、参数非法、服务不可用等），message 为可直接展示的文本
    void failed(const QString &message);
    void output(const QString &transaction, const QString &text);
    void finished(const QString &transaction, int exitCode, const QString &message);
    // 事务长时间没有任何输出，界面应提示用户可取消
    void stalled(const QString &transaction);
    void cancelRejected(const QString &message);

private slots:
    void onRemoteOutput(const QString &transaction, const QString &text);
    void onRemoteFinished(const QString &transaction, int exitCode, const QString &message);

private:
    // Idle: 无请求；Pending: 已发出请求、尚未拿到事务 ID；
    // Active: 已受理，等待 Finished
    enum class State { Idle, Pending, Active };

    void call(const QString &method, const QVariantList &arguments);
    void restartActivityTimers();
    void handleFinished(const QString &transaction, int exitCode, const QString &message);
    void bufferSignal(bool finished, const QString &transaction, int exitCode, const QString &text);
    void flushPendingSignals();

    struct PendingSignal {
        bool finished;
        QString transaction;
        int exitCode;
        QString text;
    };

    State m_state = State::Idle;
    QString m_transaction;
    QVector<PendingSignal> m_pendingSignals;
    QTimer m_stallTimer;
    // 受理后长时间完全没有信号时放弃跟踪，避免界面永久停留在忙碌状态
    QTimer m_lostTimer;
    QDBusServiceWatcher *m_serviceWatcher = nullptr;
    // 用于让助手退出后已经失效的在途调用不再产生第二次失败回调
    quint64 m_requestSerial = 0;
    // 订阅 Output/Finished 失败时直接拒绝请求，避免等待永远不会到达的信号
    bool m_signalsConnected = true;
    // Pending 阶段请求取消：拿到事务 ID 后立即发出 Cancel
    bool m_cancelWhenAccepted = false;
};

#endif // HELPERCLIENT_H
