#ifndef HELPERSERVICE_H
#define HELPERSERVICE_H

#include <QDBusContext>
#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <functional>

#include "polkitauthorizer.h"

class AptBackend;

class HelperService : public QObject, public QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.fdebm.Helper1")

public:
    explicit HelperService(AptBackend *backend, QObject *parent = nullptr);

public slots:
    QString InstallLocalPackages(const QStringList &paths);
    QString InstallPackages(const QStringList &names);
    QString RemovePackages(const QStringList &names, bool purge);
    QString UpdateCache();
    QString Upgrade(bool distUpgrade);
    QString AutoRemove();
    bool Cancel(const QString &transaction);

signals:
    // 仅用于接口声明；实际发送走 sendSignal() 的单播路径
    void Output(const QString &transaction, const QString &text);
    void Finished(const QString &transaction, int exitCode, const QString &message);

private:
    void handleRequest(const QString &action, const QString &validationError,
                       const std::function<QString()> &startOperation);
    void sendError(const QDBusMessage &call, const QString &name, const QString &message);
    void onBackendOutput(const QString &transaction, const QString &text);
    void onBackendFinished(const QString &transaction, int exitCode, const QString &message);
    void sendSignal(const QString &member, const QVariantList &arguments, const QString &destination);

    AptBackend *m_backend;
    PolkitAuthorizer m_authorizer;
    QHash<QString, QString> m_destinations;
    bool m_authorizing = false;
};

#endif // HELPERSERVICE_H
