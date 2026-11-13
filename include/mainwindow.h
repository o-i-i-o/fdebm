#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTableWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QProgressBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QTabWidget>
#include <QCheckBox>
#include <QGroupBox>
#include <QPointer>
#include "packagemanager.h"
#include "operationprogressdialog.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 启动后在后台静默执行一次 apt update，不弹出进度对话框
    void startBackgroundAptUpdate();

private slots:
    void refreshPackageList();
    void searchLocalPackages();
    void removeSelectedPackages();
    void installLocalPackages();
    void autoRemovePackages();
    void onOperationStarted(PackageManager::OperationType type, const QString &message);
    void onOperationProgress(PackageManager::OperationType type, const QString &message);
    void onOperationFinished(PackageManager::OperationType type, bool success, const QString &message);
    void onOperationStalled(PackageManager::OperationType type);
    void cancelCurrentOperation();
    void onCancelRejected(const QString &message);
    void onPackageSelectionChanged();
    void onItemChanged(QTableWidgetItem *item);
    QStringList getSelectedPackages();
    
    // 在线包管理槽函数
    void searchOnlinePackages();
    void installSelectedOnlinePackages();
    void updateAptCache();
    void upgradeSystem();
    void onOnlinePackageSelectionChanged();
    void onOnlineItemChanged(QTableWidgetItem *item);
    QStringList getSelectedOnlinePackages();
    void onTabChanged(int index);

private:
    void setupUI();
    void setupConnections();
    void populatePackageTable(const QList<DebPackage> &packages);
    void showStatusMessage(const QString &message, int timeout = 5000);
    
    QTabWidget *tabWidget;
    QTableWidget *packageTable;
    QLineEdit *searchEdit;
    QPushButton *searchButton;
    QPushButton *refreshButton;
    QPushButton *removeButton;
    QPushButton *batchInstallButton;
    QPushButton *autoRemoveButton;
    QCheckBox *purgeConfigCheckbox;
    QLabel *statusLabel;
    QProgressBar *progressBar;
    
    // 在线包管理组件
    QLineEdit *onlineSearchEdit;
    QPushButton *onlineSearchButton;
    QPushButton *refreshAptCacheButton;
    QPushButton *installOnlineButton;
    QPushButton *upgradeSystemButton;
    QCheckBox *distUpgradeCheckbox;
    QTableWidget *onlinePackageTable;
    QList<DebPackage> currentOnlinePackages;
    
    // 操作进度对话框
    QPointer<OperationProgressDialog> progressDialog;
    
    PackageManager *packageManager;
    QList<DebPackage> allPackages;

    // 后台静默执行的 apt update，不弹进度对话框、不禁用窗口
    bool m_backgroundUpdate = false;
    // 在线安装成功后静默重跑上次搜索，刷新“已安装”状态
    bool m_silentOnlineSearch = false;
    bool m_refreshOnlineAfterInstall = false;
    QString m_lastOnlineSearchText;
};

#endif
