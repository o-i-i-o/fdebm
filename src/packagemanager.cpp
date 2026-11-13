#include "packagemanager.h"
#include "helperclient.h"
#include "programpaths.h"
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace
{
const int kSearchTimeoutMs = 60 * 1000;

QProcessEnvironment readOnlyEnvironment()
{
    // 固定 C locale，使 dpkg/apt 的输出格式稳定、与解析规则一致
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LC_MESSAGES"), QStringLiteral("C"));
    return environment;
}

// apt search 把关键词当作正则，界面这里做的是包名字面搜索：
// 转义元字符，避免 "g++"、"libc++" 等被当成量词/分组而返回无关结果。
QString escapeSearchPattern(const QString &term)
{
    static const QString specialCharacters =
            QStringLiteral("\\.^$*+?()[]{}|");
    QString escaped;
    escaped.reserve(term.size() * 2);
    for (const QChar character : term) {
        if (specialCharacters.contains(character)) {
            escaped += QLatin1Char('\\');
        }
        escaped += character;
    }
    return escaped;
}
}

PackageManager::PackageManager(QObject *parent)
    : QObject(parent)
    , m_lastError("")
    , m_helper(new HelperClient(this))
{
    connect(m_helper, &HelperClient::output, this,
            [this](const QString &, const QString &text) {
        emit operationOutput(text);
    });
    connect(m_helper, &HelperClient::finished, this,
            [this](const QString &, int exitCode, const QString &message) {
        const OperationType type = m_helperOperation;
        const QString successMessage = m_pendingSuccessMessage;
        m_pendingSuccessMessage.clear();

        if (exitCode == 0) {
            emit operationFinished(type, true, successMessage.isEmpty()
                                   ? QStringLiteral("操作完成") : successMessage);
        } else {
            m_lastError = message;
            emit operationFinished(type, false, message);
        }
    });
    connect(m_helper, &HelperClient::failed, this,
            [this](const QString &message) {
        const OperationType type = m_helperOperation;
        m_pendingSuccessMessage.clear();
        m_lastError = message;
        emit operationFinished(type, false, message);
    });
    connect(m_helper, &HelperClient::stalled, this,
            [this](const QString &) {
        emit operationStalled(m_helperOperation);
    });
    connect(m_helper, &HelperClient::cancelRejected, this,
            [this](const QString &message) {
        emit cancelRejected(message);
    });

    if (!checkSystemCommands()) {
        m_lastError = "系统命令不可用，请确保 apt 和 dpkg 已安装";
    }
}

PackageManager::~PackageManager()
{
}

void PackageManager::loadInstalledPackages()
{
    if (m_listProcess) {
        return;
    }
    if (m_dpkgPath.isEmpty()) {
        m_lastError = QStringLiteral("dpkg命令不可用");
        emit operationFinished(OperationType::LoadInstalled, false, m_lastError);
        return;
    }

    emit operationStarted(OperationType::LoadInstalled,
                          QStringLiteral("正在加载已安装包列表..."));

    QProcess *process = new QProcess(this);
    m_listProcess = process;

    QTimer *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process]() {
        if (process->state() != QProcess::NotRunning) {
            process->setProperty("timedOut", true);
            process->kill();
        }
    });
    timeout->start(10000);

    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_listProcess != process) {
            process->deleteLater();
            return;
        }
        m_listProcess = nullptr;

        const QByteArray errorOutput = process->readAllStandardError();
        const QByteArray output = process->readAllStandardOutput();
        const bool timedOut = process->property("timedOut").toBool();
        process->deleteLater();

        if (timedOut) {
            m_lastError = QStringLiteral("加载包列表超时");
            emit operationFinished(OperationType::LoadInstalled, false, m_lastError);
            return;
        }
        if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
            m_lastError = QString::fromLocal8Bit(errorOutput).trimmed();
            if (m_lastError.isEmpty()) {
                m_lastError = QStringLiteral("加载包列表失败");
            }
            emit operationFinished(OperationType::LoadInstalled, false, m_lastError);
            return;
        }

        parseInstalledPackages(QString::fromUtf8(output));
    });

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_listProcess != process) {
            return;
        }
        m_listProcess = nullptr;
        m_lastError = process->errorString();
        process->deleteLater();
        emit operationFinished(OperationType::LoadInstalled, false, m_lastError);
    });

    process->setProcessEnvironment(readOnlyEnvironment());
    process->start(m_dpkgPath, QStringList{QStringLiteral("-l")});
}

void PackageManager::parseInstalledPackages(const QString &output)
{
    currentPackages.clear();

    int count = 0;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        // 状态列第 1 位是期望动作，第 2 位为 i 表示已安装；
        // 这样 hi（被 hold 的已安装包）等状态也能加载
        if (line.size() < 2 || line.at(1) != QLatin1Char('i')) {
            continue;
        }

        const DebPackage pkg = parseDpkgLine(line);
        if (pkg.name.isEmpty()) {
            continue;
        }

        currentPackages.append(pkg);
        ++count;

        // 每处理100个包发送一次进度更新
        if (count % 100 == 0) {
            emit operationProgress(OperationType::LoadInstalled,
                                   QStringLiteral("已加载 %1 个包").arg(count));
        }
    }

    emit operationProgress(OperationType::LoadInstalled,
                           QStringLiteral("共加载 %1 个已安装包").arg(currentPackages.size()));
    emit operationFinished(OperationType::LoadInstalled, true,
                           QStringLiteral("成功加载 %1 个已安装包").arg(currentPackages.size()));
}

bool PackageManager::installPackage(const QString &packagePath)
{
    return installMultiplePackages(QStringList{packagePath});
}

bool PackageManager::removePackage(const QString &packageName, bool purge)
{
    if (packageName.isEmpty()) {
        m_lastError = "没有选择任何包";
        emit operationFinished(OperationType::Remove, false, m_lastError);
        return false;
    }

    emit operationStarted(OperationType::Remove, purge
                          ? QStringLiteral("正在完全卸载包并删除配置: %1").arg(packageName)
                          : QStringLiteral("正在卸载包: %1").arg(packageName));
    m_pendingSuccessMessage = purge
            ? QStringLiteral("包完全卸载并删除配置成功: %1").arg(packageName)
            : QStringLiteral("包卸载成功: %1").arg(packageName);

    m_helperOperation = OperationType::Remove;
    m_helper->removePackages(QStringList{packageName}, purge);
    return true;
}

bool PackageManager::removeMultiplePackages(const QStringList &packageNames, bool purge)
{
    if (packageNames.isEmpty()) {
        m_lastError = "没有选择任何包";
        emit operationFinished(OperationType::Remove, false, m_lastError);
        return false;
    }

    emit operationStarted(OperationType::Remove, purge
                          ? QStringLiteral("正在批量完全卸载 %1 个包并删除配置").arg(packageNames.size())
                          : QStringLiteral("正在批量卸载 %1 个包").arg(packageNames.size()));
    m_pendingSuccessMessage = purge
            ? QStringLiteral("所有 %1 个包完全卸载并删除配置成功").arg(packageNames.size())
            : QStringLiteral("所有 %1 个包卸载成功").arg(packageNames.size());

    m_helperOperation = OperationType::Remove;
    m_helper->removePackages(packageNames, purge);
    return true;
}

bool PackageManager::installMultiplePackages(const QStringList &packagePaths)
{
    if (packagePaths.isEmpty()) {
        m_lastError = "没有选择任何包文件";
        emit operationFinished(OperationType::InstallLocal, false, m_lastError);
        return false;
    }

    emit operationStarted(OperationType::InstallLocal,
                          QStringLiteral("正在批量安装 %1 个包").arg(packagePaths.size()));
    m_pendingSuccessMessage = QStringLiteral("所有 %1 个包安装成功").arg(packagePaths.size());

    m_helperOperation = OperationType::InstallLocal;
    m_helper->installLocalPackages(packagePaths);
    return true;
}

bool PackageManager::autoRemove()
{
    emit operationStarted(OperationType::AutoRemove, QStringLiteral("正在执行自动清理..."));
    m_pendingSuccessMessage = QStringLiteral("自动清理完成");

    m_helperOperation = OperationType::AutoRemove;
    m_helper->autoRemove();
    return true;
}

void PackageManager::cancelOperation()
{
    if (m_searchProcess) {
        if (m_searchAbortMessage.isEmpty()) {
            m_searchAbortMessage = QStringLiteral("搜索已取消");
        }
        m_searchProcess->kill();
        return;
    }
    m_helper->cancel();
}

bool PackageManager::checkAptCache()
{
    // 检查apt缓存是否存在，通过查看更新时间
    const QFileInfo cache(QStringLiteral("/var/cache/apt/pkgcache.bin"));
    if (!cache.exists()) {
        return false;
    }

    const qint64 cacheTime = cache.lastModified().toSecsSinceEpoch();
    if (cacheTime <= 0) {
        return false;
    }

    return (QDateTime::currentSecsSinceEpoch() - cacheTime) < 86400; // 24小时
}

bool PackageManager::updateAptCache()
{
    emit operationStarted(OperationType::UpdateCache, QStringLiteral("正在更新apt缓存..."));
    m_pendingSuccessMessage = QStringLiteral("apt缓存更新完成");

    m_helperOperation = OperationType::UpdateCache;
    m_helper->updateCache();
    return true;
}

bool PackageManager::searchOnlinePackages(const QString &searchTerm)
{
    if (searchTerm.isEmpty()) {
        m_lastError = "请输入搜索关键词";
        emit operationFinished(OperationType::SearchOnline, false, m_lastError);
        return false;
    }
    if (m_searchProcess) {
        m_lastError = "已有搜索正在进行，请稍候";
        emit operationFinished(OperationType::SearchOnline, false, m_lastError);
        return false;
    }
    if (m_aptPath.isEmpty()) {
        m_lastError = QStringLiteral("apt命令不可用");
        emit operationFinished(OperationType::SearchOnline, false, m_lastError);
        return false;
    }

    emit operationStarted(OperationType::SearchOnline,
                          QStringLiteral("正在搜索在线包: %1").arg(searchTerm));

    QProcess *process = new QProcess(this);
    m_searchProcess = process;
    m_searchAbortMessage.clear();

    QTimer::singleShot(kSearchTimeoutMs, process, [this, process]() {
        if (m_searchProcess != process) {
            return;
        }
        m_searchAbortMessage = QStringLiteral("搜索超时，已终止");
        process->kill();
    });

    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, process, searchTerm](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_searchProcess != process) {
            process->deleteLater();
            return;
        }
        m_searchProcess = nullptr;

        const QByteArray errorOutput = process->readAllStandardError();
        const QByteArray output = process->readAllStandardOutput();
        const QString abortMessage = m_searchAbortMessage;
        m_searchAbortMessage.clear();
        process->deleteLater();

        if (!abortMessage.isEmpty()) {
            m_lastError = abortMessage;
            emit operationFinished(OperationType::SearchOnline, false, abortMessage);
            return;
        }

        if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
            m_lastError = QString::fromLocal8Bit(errorOutput).trimmed();
            if (m_lastError.isEmpty()) {
                m_lastError = QStringLiteral("搜索包失败");
            }
            emit operationFinished(OperationType::SearchOnline, false, m_lastError);
            return;
        }

        parseAptSearchOutput(QString::fromUtf8(output));
    });

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_searchProcess != process) {
            return;
        }
        m_searchProcess = nullptr;
        m_searchAbortMessage.clear();
        m_lastError = process->errorString();
        process->deleteLater();
        emit operationFinished(OperationType::SearchOnline, false, m_lastError);
    });

    process->setProcessEnvironment(readOnlyEnvironment());
    // --names-only 只按包名匹配（默认是全文搜索，描述命中会带来大量无关结果）；
    // -- 之后的内容即使以 - 开头也按关键词处理，避免被当成 apt 选项。
    process->start(m_aptPath, QStringList{QStringLiteral("search"),
                                          QStringLiteral("--names-only"),
                                          QStringLiteral("--"),
                                          escapeSearchPattern(searchTerm)});
    return true;
}

void PackageManager::parseAptSearchOutput(const QString &output)
{
    onlineSearchResults.clear();

    // apt search 的头部提示随语言环境变化（C 为 Sorting/Full Text Search，
    // 中文为 正在排序/全文搜索），因此不依赖提示文本，只按行格式识别包。
    const QStringList lines = output.split(QLatin1Char('\n'));
    DebPackage currentPkg;
    bool hasCurrentPackage = false;

    const auto appendCurrent = [this, &currentPkg, &hasCurrentPackage]() {
        if (hasCurrentPackage && !currentPkg.name.isEmpty()) {
            onlineSearchResults.append(currentPkg);
        }
        hasCurrentPackage = false;
    };

    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) {
            appendCurrent();
            continue;
        }

        // 描述行以空白缩进（apt search 实际为两个空格）
        if (line.at(0).isSpace()) {
            if (hasCurrentPackage) {
                const QString text = line.trimmed();
                if (!text.isEmpty()) {
                    if (!currentPkg.description.isEmpty()) {
                        currentPkg.description += QLatin1Char(' ');
                    }
                    currentPkg.description += text;
                }
            }
            continue;
        }

        // 包信息行：形如 name/archive version arch [flags]
        const DebPackage parsed = parseAptSearchLine(line);
        if (parsed.name.isEmpty()) {
            // 头部提示或警告行，忽略
            continue;
        }

        appendCurrent();
        currentPkg = parsed;
        hasCurrentPackage = true;
    }
    appendCurrent();

    // apt search 已完成匹配，这里不再按关键词二次过滤，
    // 否则正则搜索等合法结果会被丢弃。
    updateInstalledStatus();

    emit operationFinished(OperationType::SearchOnline, true,
                           QStringLiteral("找到 %1 个匹配的包").arg(onlineSearchResults.size()));
}

void PackageManager::updateInstalledStatus()
{
    // dpkg 对多架构包给出 "name:arch"，而 apt search 通常只给 "name"，
    // 因此同时记录完整名称和去掉架构后的名称用于匹配
    QHash<QString, QString> installedVersions;
    QHash<QString, QString> installedBaseVersions;
    for (const DebPackage &pkg : currentPackages) {
        installedVersions.insert(pkg.name, pkg.version);
        const int colon = pkg.name.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            installedBaseVersions.insert(pkg.name.left(colon), pkg.version);
        }
    }

    for (DebPackage &pkg : onlineSearchResults) {
        QString installedVersion = installedVersions.value(pkg.name);
        if (installedVersion.isEmpty() && !pkg.name.contains(QLatin1Char(':'))) {
            installedVersion = installedBaseVersions.value(pkg.name);
        }
        pkg.installedVersion = installedVersion;
        pkg.installed = !installedVersion.isEmpty();
    }
}

bool PackageManager::installOnlinePackages(const QStringList &packageNames)
{
    if (packageNames.isEmpty()) {
        m_lastError = "没有选择任何包";
        emit operationFinished(OperationType::InstallOnline, false, m_lastError);
        return false;
    }

    emit operationStarted(OperationType::InstallOnline,
                          QStringLiteral("正在安装 %1 个包").arg(packageNames.size()));
    m_pendingSuccessMessage = QStringLiteral("所有 %1 个包安装成功").arg(packageNames.size());

    m_helperOperation = OperationType::InstallOnline;
    m_helper->installPackages(packageNames);
    return true;
}

bool PackageManager::upgradeSystem(bool distUpgrade)
{
    emit operationStarted(OperationType::Upgrade, distUpgrade
                          ? QStringLiteral("正在执行全部更新（dist-upgrade）...")
                          : QStringLiteral("正在执行全部更新（upgrade）..."));
    m_pendingSuccessMessage = distUpgrade ? QStringLiteral("全部更新完成（依赖冲突时移除已安装包）")
                                          : QStringLiteral("全部更新完成");

    m_helperOperation = OperationType::Upgrade;
    m_helper->upgrade(distUpgrade);
    return true;
}

DebPackage PackageManager::parseAptSearchLine(const QString &line)
{
    DebPackage pkg;
    pkg.installed = false;
    pkg.version = "";

    // apt search 实际输出格式示例（C 与中文环境的头部提示不同，包行一致）:
    // bash/stonking,now 5.3-3ubuntu1 amd64 [installed,automatic]
    //   GNU Bourne Again SHell

    if (line.isEmpty() || line.at(0).isSpace()) {
        return pkg;
    }

    // simplified() 已合并连续空白，按单空格切分即可，无需正则与跳过空项
    const QStringList parts = line.simplified().split(QLatin1Char(' '));
    if (parts.size() < 2) {
        return pkg;
    }

    // 包名必须带 "/存档" 后缀；头部提示与警告行没有该结构
    const QString fullName = parts[0];
    const int slashIndex = fullName.indexOf(QLatin1Char('/'));
    if (slashIndex <= 0) {
        return pkg;
    }

    pkg.name = fullName.left(slashIndex);
    pkg.version = parts[1];
    return pkg;
}

QString PackageManager::getLastError()
{
    return m_lastError;
}

DebPackage PackageManager::parseDpkgLine(const QString &line)
{
    DebPackage pkg;
    pkg.installed = true;

    // dpkg -l 输出格式: ii  package-name  version  architecture  description
    const QString simplified = line.simplified();
    if (simplified.isEmpty()) {
        return pkg;
    }
    const QStringList parts = simplified.split(QLatin1Char(' '));

    if (parts.size() >= 4) {
        // 多架构包在名称列带 ":arch" 后缀，需原样保留以便精确卸载
        pkg.name = parts[1];
        pkg.version = parts[2];

        // 第 4 列为架构，描述从第 5 列开始且可能包含空格
        QStringList descParts;
        for (int i = 4; i < parts.size(); ++i) {
            descParts.append(parts[i]);
        }
        pkg.description = descParts.join(QLatin1Char(' '));
    }

    return pkg;
}

bool PackageManager::checkSystemCommands()
{
    m_aptPath = ProgramPaths::findSystemExecutable(QStringLiteral("apt"));
    m_dpkgPath = ProgramPaths::findSystemExecutable(QStringLiteral("dpkg"));

    if (m_aptPath.isEmpty()) {
        m_lastError = "apt命令不可用";
        return false;
    }

    if (m_dpkgPath.isEmpty()) {
        m_lastError = "dpkg命令不可用";
        return false;
    }

    return true;
}
