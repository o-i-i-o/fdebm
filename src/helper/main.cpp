#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>
#include "aptbackend.h"
#include "helperservice.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("fdebm-helperd"));
    QCoreApplication::setApplicationVersion(QStringLiteral(FDEBM_VERSION));

    const QStringList arguments = app.arguments();
    const bool dryRun = arguments.contains(QStringLiteral("--dry-run"));

    QString stagingDir;
    const int stagingIndex = arguments.indexOf(QStringLiteral("--staging-dir"));
    if (stagingIndex >= 0) {
        if (stagingIndex + 1 >= arguments.size()
            || arguments.at(stagingIndex + 1).startsWith(QLatin1String("--"))) {
            qCritical() << "--staging-dir 需要一个目录参数";
            return 1;
        }
        stagingDir = arguments.at(stagingIndex + 1);
        if (!stagingDir.startsWith(QLatin1Char('/'))) {
            qCritical() << "--staging-dir 必须是绝对路径";
            return 1;
        }
    }

    // 仅用于 dry-run 联调：模拟事务持续时长，便于测试取消与 watchdog
    int dryRunDelayMs = 300;
    const int delayIndex = arguments.indexOf(QStringLiteral("--dry-run-delay-ms"));
    if (delayIndex >= 0) {
        bool ok = false;
        const int value = delayIndex + 1 < arguments.size()
                ? arguments.at(delayIndex + 1).toInt(&ok) : 0;
        if (!ok || value < 0 || value > 60000) {
            qCritical() << "--dry-run-delay-ms 需要 0 到 60000 之间的毫秒数";
            return 1;
        }
        dryRunDelayMs = value;
    }

    AptBackend backend;
    backend.setDryRun(dryRun);
    backend.setDryRunDelayMs(dryRunDelayMs);
    backend.setStagingDir(stagingDir);

    HelperService service(&backend);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCritical() << "无法连接系统总线:" << bus.lastError().message();
        return 1;
    }
    if (!bus.registerObject(QStringLiteral("/org/fdebm/Helper"), &service,
                            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        qCritical() << "注册 D-Bus 对象失败:" << bus.lastError().message();
        return 1;
    }
    if (!bus.registerService(QStringLiteral("org.fdebm.Helper"))) {
        qCritical() << "注册 D-Bus 服务名失败:" << bus.lastError().message();
        return 1;
    }

    if (dryRun) {
        qInfo() << "fdebm-helperd 以 dry-run 模式运行，不执行真实命令";
    }

    return app.exec();
}
