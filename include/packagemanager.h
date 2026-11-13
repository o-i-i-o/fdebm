#ifndef PACKAGEMANAGER_H
#define PACKAGEMANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>

class HelperClient;
class QProcess;

struct DebPackage {
    QString name;
    QString version;
    QString description;
    bool installed;
    // 在线搜索结果中记录本机已安装的版本，未安装时为空
    QString installedVersion;
};

class PackageManager : public QObject
{
    Q_OBJECT

public:
    enum class OperationType {
        LoadInstalled,
        InstallLocal,
        InstallOnline,
        Remove,
        UpdateCache,
        Upgrade,
        AutoRemove,
        SearchOnline
    };
    Q_ENUM(OperationType)

    explicit PackageManager(QObject *parent = nullptr);
    ~PackageManager();

    void loadInstalledPackages();
    bool installPackage(const QString &packagePath);
    bool removePackage(const QString &packageName, bool purge = false);
    bool installMultiplePackages(const QStringList &packagePaths);
    bool removeMultiplePackages(const QStringList &packageNames, bool purge = false);
    bool autoRemove();
    void cancelOperation();
    
    // 在线包管理方法
    bool checkAptCache();
    bool updateAptCache();
    bool searchOnlinePackages(const QString &searchTerm);
    bool installOnlinePackages(const QStringList &packageNames);
    bool upgradeSystem(bool distUpgrade);
    
    QString getLastError();
    QList<DebPackage> getCurrentPackages() const { return currentPackages; }
    QList<DebPackage> getOnlineSearchResults() const { return onlineSearchResults; }

signals:
    void operationStarted(OperationType type, const QString &message);
    void operationProgress(OperationType type, const QString &message);
    void operationOutput(const QString &output);
    void operationFinished(OperationType type, bool success, const QString &message);
    // 已受理的事务长时间没有输出，界面应提示用户可以取消
    void operationStalled(OperationType type);
    void cancelRejected(const QString &message);

private:
    DebPackage parseDpkgLine(const QString &line);
    DebPackage parseAptSearchLine(const QString &line);
    void parseInstalledPackages(const QString &output);
    void parseAptSearchOutput(const QString &output);
    void updateInstalledStatus();
    bool checkSystemCommands();
    
    QString m_lastError;
    // 助手的 finished/failed 回调不带操作类型，这里记录最近一次发起的助手请求
    OperationType m_helperOperation = OperationType::LoadInstalled;
    QList<DebPackage> currentPackages;
    QList<DebPackage> onlineSearchResults;
    HelperClient *m_helper;
    QProcess *m_listProcess = nullptr;
    QProcess *m_searchProcess = nullptr;
    // 非空表示搜索被用户取消或超时终止，完成回调据此给出准确提示
    QString m_searchAbortMessage;
    QString m_pendingSuccessMessage;
    // 只使用固定目录解析出的绝对路径，不依赖进程 PATH
    QString m_aptPath;
    QString m_dpkgPath;
};

#endif