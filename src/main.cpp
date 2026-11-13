#include <QApplication>
#include <QTextCodec>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral(FDEBM_VERSION));
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));

    MainWindow w;
    w.show();
    // 启动后自动在后台更新一次 apt 缓存
    w.startBackgroundAptUpdate();

    return a.exec();
}
