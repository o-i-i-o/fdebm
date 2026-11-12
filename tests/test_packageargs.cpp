#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include "localpackages.h"
#include "packageargs.h"
#include "programpaths.h"
#include "proxyconfig.h"

namespace
{
int g_failures = 0;

void check(bool condition, const QString &what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", qPrintable(what));
        ++g_failures;
    }
}

void checkError(const QString &actual, const QString &needle, const QString &what)
{
    check(!actual.isEmpty() && actual.contains(needle),
          QStringLiteral("%1（实际: %2）").arg(what, actual));
}

bool writeFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write("dummy\n");
    return file.flush();
}

bool writeText(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(content) != content.size()) {
        return false;
    }
    return file.flush();
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ---- 包名校验 ----
    const QStringList validNames{
        QStringLiteral("libc6"),
        QStringLiteral("libc6:amd64"),
        QStringLiteral("g++-12"),
        QStringLiteral("python3.11"),
        QStringLiteral("0ad"),
    };
    for (const QString &name : validNames) {
        check(PackageArgs::validatePackageNames(QStringList{name}).isEmpty(),
              QStringLiteral("合法包名被拒绝: %1").arg(name));
    }

    const QStringList invalidNames{
        QStringLiteral("-oDPkg::Pre-Install-Pkgs"),
        QStringLiteral("bad name"),
        QStringLiteral("bad;rm"),
        QStringLiteral("Upper"),
        QStringLiteral("libc6:amd64:extra"),
        QString(),
        QString(201, QLatin1Char('a')),
    };
    for (const QString &name : invalidNames) {
        checkError(PackageArgs::validatePackageNames(QStringList{name}),
                   QStringLiteral("非法包名"),
                   QStringLiteral("非法包名未被拒绝: %1").arg(name.left(32)));
    }

    checkError(PackageArgs::validatePackageNames(QStringList{}),
               QStringLiteral("未选择任何软件包"),
               QStringLiteral("空包名列表"));

    QStringList tooManyNames;
    for (int i = 0; i < 201; ++i) {
        tooManyNames.append(QStringLiteral("pkg%1").arg(i));
    }
    checkError(PackageArgs::validatePackageNames(tooManyNames),
               QStringLiteral("最多处理 200"),
               QStringLiteral("包名数量上限"));

    // ---- 本地 deb 路径校验 ----
    QTemporaryDir temp;
    check(temp.isValid(), QStringLiteral("无法创建临时目录"));
    QDir dir(temp.path());

    const QString okPath = dir.filePath(QStringLiteral("ok.deb"));
    check(writeFile(okPath), QStringLiteral("无法创建测试文件"));
    // 显式设为 0644，避免测试环境 umask 带来组可写
    check(QFile::setPermissions(okPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ReadGroup | QFileDevice::ReadOther),
          QStringLiteral("无法设置 ok.deb 权限"));
    QStringList canonical;
    check(PackageArgs::validateLocalDebPaths(QStringList{okPath}, &canonical).isEmpty(),
          QStringLiteral("合法 deb 被拒绝"));
    check(canonical.size() == 1
          && QFileInfo(canonical.first()).fileName() == QStringLiteral("ok.deb"),
          QStringLiteral("合法 deb 未返回规范化路径"));

    check(PackageArgs::validateLocalDebPaths(QStringList{okPath}, nullptr).isEmpty(),
          QStringLiteral("允许省略 canonicalPaths 输出参数"));

    const QString linkPath = dir.filePath(QStringLiteral("link.deb"));
    check(QFile::link(okPath, linkPath), QStringLiteral("无法创建符号链接"));
    canonical.clear();
    check(PackageArgs::validateLocalDebPaths(QStringList{linkPath}, &canonical).isEmpty()
          && canonical.size() == 1
          && QFileInfo(canonical.first()).canonicalFilePath()
             == QFileInfo(okPath).canonicalFilePath(),
          QStringLiteral("符号链接未解析到目标文件"));

    const QString txtPath = dir.filePath(QStringLiteral("pkg.txt"));
    check(writeFile(txtPath), QStringLiteral("无法创建 txt 测试文件"));
    checkError(PackageArgs::validateLocalDebPaths(QStringList{txtPath}, nullptr),
               QStringLiteral("不是 .deb 文件"),
               QStringLiteral("非 deb 后缀未被拒绝"));

    const QString worldPath = dir.filePath(QStringLiteral("world.deb"));
    check(writeFile(worldPath), QStringLiteral("无法创建 world.deb"));
    check(QFile::setPermissions(worldPath,
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                | QFileDevice::ReadOther | QFileDevice::WriteOther),
          QStringLiteral("无法设置文件权限"));
    checkError(PackageArgs::validateLocalDebPaths(QStringList{worldPath}, nullptr),
               QStringLiteral("文件权限不安全"),
               QStringLiteral("组/其他用户可写的 deb 未被拒绝"));

    const QString dirDebPath = dir.filePath(QStringLiteral("dir.deb"));
    check(QDir().mkdir(dirDebPath), QStringLiteral("无法创建目录"));
    checkError(PackageArgs::validateLocalDebPaths(QStringList{dirDebPath}, nullptr),
               QStringLiteral("不是普通文件"),
               QStringLiteral("目录未被拒绝"));

    checkError(PackageArgs::validateLocalDebPaths(
                       QStringList{dir.filePath(QStringLiteral("missing.deb"))}, nullptr),
               QStringLiteral("文件不存在"),
               QStringLiteral("不存在的路径未被拒绝"));

    checkError(PackageArgs::validateLocalDebPaths(QStringList{QStringLiteral("pkg.deb")},
                                                  nullptr),
               QStringLiteral("必须是绝对路径"),
               QStringLiteral("相对路径未被拒绝"));

    checkError(PackageArgs::validateLocalDebPaths(QStringList{QStringLiteral("")}, nullptr),
               QStringLiteral("必须是绝对路径"),
               QStringLiteral("空路径未被拒绝"));

    checkError(PackageArgs::validateLocalDebPaths(
                       QStringList{QStringLiteral("/tmp/with%1null.deb").arg(QLatin1Char('\0'))},
                       nullptr),
               QStringLiteral("必须是绝对路径"),
               QStringLiteral("含 NUL 的路径未被拒绝"));

    checkError(PackageArgs::validateLocalDebPaths(QStringList{}, nullptr),
               QStringLiteral("未选择任何 DEB 文件"),
               QStringLiteral("空路径列表"));

    QStringList tooManyPaths;
    for (int i = 0; i < 201; ++i) {
        tooManyPaths.append(QStringLiteral("/nonexistent/pkg%1.deb").arg(i));
    }
    checkError(PackageArgs::validateLocalDebPaths(tooManyPaths, nullptr),
               QStringLiteral("最多处理 200"),
               QStringLiteral("路径数量上限"));

    // ---- 系统命令路径解析 ----
    const QString aptPath = ProgramPaths::findSystemExecutable(QStringLiteral("apt"));
    if (!aptPath.isEmpty()) {
        check(QFileInfo(aptPath).isAbsolute(), QStringLiteral("apt 未解析为绝对路径"));
        check(QFileInfo(aptPath).isExecutable(), QStringLiteral("apt 路径不可执行"));
    }
    check(ProgramPaths::findSystemExecutable(QString()).isEmpty(),
          QStringLiteral("空命令名不应被解析"));
    check(ProgramPaths::findSystemExecutable(QStringLiteral("bin/sh")).isEmpty(),
          QStringLiteral("带相对斜杠的命令名不应被解析"));
    check(ProgramPaths::findSystemExecutable(QStringLiteral("/bin/sh")).isEmpty(),
          QStringLiteral("绝对路径输入不应被直接接受"));

    // 调用方 PATH 中的同名程序不得被采用
    QTemporaryDir fakeBin;
    check(fakeBin.isValid(), QStringLiteral("无法创建伪造 PATH 目录"));
    const QString fakeProgram =
            QDir(fakeBin.path()).filePath(QStringLiteral("fdebm-test-probe"));
    check(writeFile(fakeProgram), QStringLiteral("无法创建伪造程序"));
    check(QFile::setPermissions(fakeProgram, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                             | QFileDevice::ExeOwner),
          QStringLiteral("无法设置伪造程序权限"));
    const QByteArray originalPath = qgetenv("PATH");
    qputenv("PATH", fakeBin.path().toLocal8Bit() + ':' + originalPath);
    check(ProgramPaths::findSystemExecutable(QStringLiteral("fdebm-test-probe")).isEmpty(),
          QStringLiteral("PATH 中的同名程序不应被采用"));
    qputenv("PATH", originalPath);

    // ---- 代理配置读取 ----
    const QString proxyPath = dir.filePath(QStringLiteral("proxy.conf"));
    check(writeText(proxyPath,
                    "http_proxy=http://127.0.0.1:3128\n"
                    "HTTPS_PROXY=\"http://proxy.example:8080\"\n"
                    "no_proxy=localhost,127.0.0.1\n"
                    "PATH=/evil\n"
                    "not a pair\n"
                    "ftp_proxy=http://bad\x01host\n"
                    "http_proxy=\n"),
          QStringLiteral("无法创建代理配置文件"));
    check(QFile::setPermissions(proxyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                           | QFileDevice::ReadGroup | QFileDevice::ReadOther),
          QStringLiteral("无法设置代理配置权限"));

    QProcessEnvironment proxyEnv;
    ProxyConfig::applyFromFile(proxyPath, &proxyEnv);
    check(proxyEnv.value(QStringLiteral("http_proxy")) == QStringLiteral("http://127.0.0.1:3128"),
          QStringLiteral("合法代理变量未生效"));
    check(proxyEnv.value(QStringLiteral("HTTPS_PROXY")) == QStringLiteral("http://proxy.example:8080"),
          QStringLiteral("带引号的代理值未解析"));
    check(proxyEnv.value(QStringLiteral("no_proxy")) == QStringLiteral("localhost,127.0.0.1"),
          QStringLiteral("no_proxy 未生效"));
    check(!proxyEnv.contains(QStringLiteral("PATH")),
          QStringLiteral("非白名单变量不应被转发"));
    check(!proxyEnv.contains(QStringLiteral("ftp_proxy")),
          QStringLiteral("含控制字符的代理值不应被转发"));

    const QString unsafeProxyPath = dir.filePath(QStringLiteral("proxy-unsafe.conf"));
    check(writeText(unsafeProxyPath, "http_proxy=http://evil.example:1\n"),
          QStringLiteral("无法创建不安全代理配置"));
    check(QFile::setPermissions(unsafeProxyPath,
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                | QFileDevice::ReadOther | QFileDevice::WriteOther),
          QStringLiteral("无法设置不安全代理配置权限"));
    QProcessEnvironment unsafeProxyEnv;
    ProxyConfig::applyFromFile(unsafeProxyPath, &unsafeProxyEnv);
    check(unsafeProxyEnv.isEmpty(), QStringLiteral("组/其他用户可写的代理配置不应被读取"));

    QProcessEnvironment missingProxyEnv;
    ProxyConfig::applyFromFile(dir.filePath(QStringLiteral("missing-proxy.conf")),
                               &missingProxyEnv);
    check(missingProxyEnv.isEmpty(), QStringLiteral("不存在的代理配置不应报错"));

    // ---- 授权前本地安装包打开校验 ----
    LocalPackageSet debSet;
    QString setError;

    const QString emptyDeb = dir.filePath(QStringLiteral("empty.deb"));
    check(writeText(emptyDeb, QByteArray()), QStringLiteral("无法创建空 deb"));
    check(QFile::setPermissions(emptyDeb, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                            | QFileDevice::ReadGroup | QFileDevice::ReadOther),
          QStringLiteral("无法设置空 deb 权限"));
    check(!debSet.open(QStringList{emptyDeb}, &setError)
          && setError.contains(QStringLiteral("空文件")),
          QStringLiteral("空 deb 未被拒绝: %1").arg(setError));

    const QString fifoDeb = dir.filePath(QStringLiteral("fifo.deb"));
    check(::mkfifo(QFile::encodeName(fifoDeb).constData(), 0644) == 0,
          QStringLiteral("无法创建 FIFO"));
    setError.clear();
    check(!debSet.open(QStringList{fifoDeb}, &setError)
          && setError.contains(QStringLiteral("不是普通文件")),
          QStringLiteral("FIFO 未被拒绝: %1").arg(setError));

    const QString bigDeb = dir.filePath(QStringLiteral("big.deb"));
    {
        QFile big(bigDeb);
        check(big.open(QIODevice::WriteOnly), QStringLiteral("无法创建超大 deb"));
        check(big.resize(4LL * 1024 * 1024 * 1024 + 1),
              QStringLiteral("无法扩展超大 deb"));
    }
    check(QFile::setPermissions(bigDeb, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                          | QFileDevice::ReadGroup | QFileDevice::ReadOther),
          QStringLiteral("无法设置超大 deb 权限"));
    setError.clear();
    check(!debSet.open(QStringList{bigDeb}, &setError)
          && setError.contains(QStringLiteral("超过单个安装包大小上限")),
          QStringLiteral("超大 deb 未被拒绝: %1").arg(setError));

    const QString symlinkDeb = dir.filePath(QStringLiteral("symlink.deb"));
    check(QFile::link(okPath, symlinkDeb), QStringLiteral("无法创建 deb 符号链接"));
    setError.clear();
    check(!debSet.open(QStringList{symlinkDeb}, &setError)
          && setError.contains(QStringLiteral("无法打开")),
          QStringLiteral("符号链接未被拒绝: %1").arg(setError));

    setError.clear();
    check(debSet.open(QStringList{okPath}, &setError),
          QStringLiteral("合法 deb 无法打开: %1").arg(setError));

    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d 项\n", g_failures);
        return 1;
    }
    std::printf("ALL OK\n");
    return 0;
}
