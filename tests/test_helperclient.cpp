#include <QtTest>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QProcess>
#include <QSignalSpy>

#include "helperclient.h"

namespace
{
const char kService[] = "org.fdebm.Helper";
const char kPath[] = "/org/fdebm/Helper";

class FakeHelper : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.fdebm.Helper1")

public slots:
    QString UpdateCache()
    {
        return QStringLiteral("tx-test");
    }
};

struct DaemonGuard {
    qint64 pid = 0;
    ~DaemonGuard()
    {
        if (pid > 0) {
            QProcess::execute(QStringLiteral("kill"), QStringList{QString::number(pid)});
        }
    }
};
}

class HelperClientTest : public QObject
{
    Q_OBJECT

private slots:
    void helperExitFailsActiveTransaction();
};

void HelperClientTest::helperExitFailsActiveTransaction()
{
    QProcess daemon;
    daemon.start(QStringLiteral("dbus-daemon"),
                 QStringList{QStringLiteral("--session"), QStringLiteral("--fork"),
                             QStringLiteral("--print-address=1"), QStringLiteral("--print-pid=1")});
    if (!daemon.waitForFinished(5000) || daemon.exitCode() != 0) {
        QSKIP("需要 dbus-daemon");
    }

    const QList<QByteArray> lines = daemon.readAllStandardOutput().split('\n');
    if (lines.size() < 2) {
        QSKIP("无法启动私有 D-Bus");
    }

    DaemonGuard guard;
    bool ok = false;
    guard.pid = lines.at(1).trimmed().toLongLong(&ok);
    if (!ok) {
        QSKIP("无法读取 dbus-daemon 进程号");
    }
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", lines.at(0).trimmed());

    QDBusConnection bus = QDBusConnection::systemBus();
    QVERIFY(bus.isConnected());

    FakeHelper helper;
    QVERIFY(bus.registerObject(QLatin1String(kPath), &helper, QDBusConnection::ExportAllSlots));
    QVERIFY(bus.registerService(QLatin1String(kService)));

    HelperClient client;
    QSignalSpy accepted(&client, &HelperClient::accepted);
    QSignalSpy failed(&client, &HelperClient::failed);

    client.updateCache();
    QVERIFY(accepted.wait(5000));
    QCOMPARE(accepted.count(), 1);

    QVERIFY(bus.unregisterService(QLatin1String(kService)));
    QVERIFY(failed.wait(5000));
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.at(0).at(0).toString().contains(QStringLiteral("已退出")));

    // 助手退出时已经失效的调用不得再触发一次失败回调
    QTest::qWait(200);
    QCOMPARE(failed.count(), 1);

    // 状态已恢复：再次请求应真正发往总线，而不是被“已有操作”拦截
    QSignalSpy failedAgain(&client, &HelperClient::failed);
    client.updateCache();
    QVERIFY(failedAgain.wait(5000));
    QVERIFY(failedAgain.at(0).at(0).toString().contains(QStringLiteral("未运行")));

    bus.unregisterObject(QLatin1String(kPath));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    HelperClientTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_helperclient.moc"
