#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTimer>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyle>

namespace {

// 复选框在单元格内水平居中；QTableWidget 默认始终把指示器画在左侧
class CenteredCheckBoxDelegate : public QStyledItemDelegate
{
public:
    explicit CenteredCheckBoxDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        // 背景/选中态交给样式绘制，复选框单独居中画
        opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        if (!index.data(Qt::CheckStateRole).isValid()) {
            return;
        }

        QStyleOptionViewItem checkOption = option;
        checkOption.state &= ~(QStyle::State_On | QStyle::State_Off | QStyle::State_NoChange);
        checkOption.state |= index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                ? QStyle::State_On : QStyle::State_Off;
        checkOption.rect = checkRect(option);
        style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &checkOption,
                             painter, opt.widget);
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        // 指示器已居中，默认的左侧命中区域不再适用
        if (event->type() == QEvent::MouseButtonRelease
                || event->type() == QEvent::MouseButtonDblClick) {
            if (!index.flags().testFlag(Qt::ItemIsUserCheckable)) {
                return false;
            }
            const QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() != Qt::LeftButton
                    || !checkRect(option).contains(mouseEvent->pos())) {
                return false;
            }
            const Qt::CheckState state =
                    static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
            model->setData(index, state == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                           Qt::CheckStateRole);
            return true;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    static QRect checkRect(const QStyleOptionViewItem &option)
    {
        QStyle *style = option.widget ? option.widget->style() : QApplication::style();
        const int width = style->pixelMetric(QStyle::PM_IndicatorWidth, &option, option.widget);
        const int height = style->pixelMetric(QStyle::PM_IndicatorHeight, &option, option.widget);
        return QRect(option.rect.x() + (option.rect.width() - width) / 2,
                     option.rect.y() + (option.rect.height() - height) / 2,
                     width, height);
    }
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , progressDialog(nullptr)
    , packageManager(new PackageManager(this))
{
    setupUI();
    setupConnections();
    
    setWindowTitle("DEB包管理器");
    setMinimumSize(1100, 640);
    resize(1200, 720);
    
    // 延迟检查系统命令，确保窗口完全显示
    QTimer::singleShot(100, this, [this]() {
        QString error = packageManager->getLastError();
        if (!error.isEmpty()) {
            QMessageBox::critical(this, "系统错误", error);
        } else {
            refreshPackageList();
        }
    });
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUI()
{
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    
    // 创建标签页控件
    tabWidget = new QTabWidget(this);
    
    // 第一页：已安装包管理
    QWidget *installedPage = new QWidget();
    QVBoxLayout *installedLayout = new QVBoxLayout(installedPage);
    
    // 搜索和刷新区域
    QHBoxLayout *searchLayout = new QHBoxLayout();
    QLabel *searchLabel = new QLabel("搜索本地包:");
    searchLabel->setStyleSheet("font-weight: bold;");
    searchLayout->addWidget(searchLabel);
    
    searchEdit = new QLineEdit();
    searchEdit->setPlaceholderText("输入包名搜索已安装的包...");
    // 按钮全部集中到顶部，搜索框不再抢占宽度
    searchEdit->setMaximumWidth(260);
    searchLayout->addWidget(searchEdit);
    
    searchButton = new QPushButton("搜索");
    searchLayout->addWidget(searchButton);
    
    refreshButton = new QPushButton("刷新列表");
    searchLayout->addWidget(refreshButton);
    
    batchInstallButton = new QPushButton("安装本地包");
    searchLayout->addWidget(batchInstallButton);
    
    removeButton = new QPushButton("卸载选中包");
    searchLayout->addWidget(removeButton);
    
    purgeConfigCheckbox = new QCheckBox("删除配置文件");
    searchLayout->addWidget(purgeConfigCheckbox);
    
    autoRemoveButton = new QPushButton("自动清理");
    searchLayout->addWidget(autoRemoveButton);
    
    searchLayout->addStretch();
    installedLayout->addLayout(searchLayout);
    
    // 包列表表格
    packageTable = new QTableWidget();
    packageTable->setColumnCount(4);
    packageTable->setHorizontalHeaderLabels(QStringList() << "选择" << "包名" << "版本" << "描述");
    packageTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    packageTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    packageTable->setContextMenuPolicy(Qt::CustomContextMenu);
    packageTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    packageTable->verticalHeader()->setDefaultAlignment(Qt::AlignCenter);
    packageTable->setItemDelegateForColumn(0, new CenteredCheckBoxDelegate(packageTable));
    packageTable->setSortingEnabled(true);
    installedLayout->addWidget(packageTable);
    
    // 状态区域
    progressBar = new QProgressBar();
    progressBar->setVisible(false);
    installedLayout->addWidget(progressBar);
    
    statusLabel = new QLabel("就绪");
    statusLabel->setObjectName("statusLabel");
    installedLayout->addWidget(statusLabel);
    
    // 添加标签页
    tabWidget->addTab(installedPage, "本地包管理");
    
    // 第二页：在线安装
    QWidget *onlinePage = new QWidget();
    QVBoxLayout *onlineLayout = new QVBoxLayout(onlinePage);
    
    // 搜索和刷新区域
    QHBoxLayout *onlineSearchLayout = new QHBoxLayout();
    QLabel *onlineSearchLabel = new QLabel("搜索在线包:");
    onlineSearchLabel->setStyleSheet("font-weight: bold;");
    onlineSearchLayout->addWidget(onlineSearchLabel);
    
    onlineSearchEdit = new QLineEdit();
    onlineSearchEdit->setPlaceholderText("输入包名搜索在线包...");
    onlineSearchEdit->setMaximumWidth(260);
    onlineSearchLayout->addWidget(onlineSearchEdit);
    
    onlineSearchButton = new QPushButton("搜索");
    onlineSearchLayout->addWidget(onlineSearchButton);
    
    installOnlineButton = new QPushButton("安装选中包");
    onlineSearchLayout->addWidget(installOnlineButton);
    
    refreshAptCacheButton = new QPushButton("更新缓存");
    onlineSearchLayout->addWidget(refreshAptCacheButton);
    
    // 将全部更新按钮和完全更新复选框移到顶部
    upgradeSystemButton = new QPushButton("全部更新");
    onlineSearchLayout->addWidget(upgradeSystemButton);
    
    distUpgradeCheckbox = new QCheckBox("依赖冲突时移除已安装包");
    onlineSearchLayout->addWidget(distUpgradeCheckbox);
    
    onlineSearchLayout->addStretch();
    onlineLayout->addLayout(onlineSearchLayout);
    
    // 包列表表格
    onlinePackageTable = new QTableWidget();
    onlinePackageTable->setColumnCount(5);
    onlinePackageTable->setHorizontalHeaderLabels(QStringList()
                                                  << "选择" << "包名" << "在线版本"
                                                  << "已安装版本" << "描述");
    onlinePackageTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    onlinePackageTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    onlinePackageTable->setContextMenuPolicy(Qt::CustomContextMenu);
    onlinePackageTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    // 两个版本列按文本长度自适应，避免长版本号被截断
    onlinePackageTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    onlinePackageTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    onlinePackageTable->verticalHeader()->setDefaultAlignment(Qt::AlignCenter);
    onlinePackageTable->setItemDelegateForColumn(0, new CenteredCheckBoxDelegate(onlinePackageTable));
    onlinePackageTable->setSortingEnabled(true);
    onlineLayout->addWidget(onlinePackageTable);
    
    // 添加系统更新提示信息
    QLabel *updateHintLabel = new QLabel("提示：「全部更新」升级所有已安装的包；勾选「依赖冲突时移除已安装包」后使用 dist-upgrade，"
                                         "会安装新的依赖并移除过时的包，可能会改变系统版本。");
    updateHintLabel->setWordWrap(true);
    updateHintLabel->setStyleSheet("font-size: 10px; color: #666;");
    onlineLayout->addWidget(updateHintLabel);
    
    tabWidget->addTab(onlinePage, "在线安装");
    
    mainLayout->addWidget(tabWidget);
}

void MainWindow::setupConnections()
{
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refreshPackageList);
    connect(searchButton, &QPushButton::clicked, this, &MainWindow::searchLocalPackages);
    connect(searchEdit, &QLineEdit::returnPressed, this, &MainWindow::searchLocalPackages);
    connect(removeButton, &QPushButton::clicked, this, &MainWindow::removeSelectedPackages);
    connect(batchInstallButton, &QPushButton::clicked, this, &MainWindow::installLocalPackages);
    connect(autoRemoveButton, &QPushButton::clicked, this, &MainWindow::autoRemovePackages);
    connect(packageTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::onPackageSelectionChanged);
    connect(packageTable, &QTableWidget::itemChanged, this, &MainWindow::onItemChanged);
    
    // 在线包管理连接
    connect(tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(onlineSearchButton, &QPushButton::clicked, this, &MainWindow::searchOnlinePackages);
    connect(onlineSearchEdit, &QLineEdit::returnPressed, this, &MainWindow::searchOnlinePackages);
    connect(refreshAptCacheButton, &QPushButton::clicked, this, &MainWindow::updateAptCache);
    connect(installOnlineButton, &QPushButton::clicked, this, &MainWindow::installSelectedOnlinePackages);
    connect(upgradeSystemButton, &QPushButton::clicked, this, &MainWindow::upgradeSystem);
    connect(onlinePackageTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::onOnlinePackageSelectionChanged);
    connect(onlinePackageTable, &QTableWidget::itemChanged, this, &MainWindow::onOnlineItemChanged);
    
    connect(packageManager, &PackageManager::operationStarted, this, &MainWindow::onOperationStarted);
    connect(packageManager, &PackageManager::operationProgress, this, &MainWindow::onOperationProgress);
    connect(packageManager, &PackageManager::operationOutput, this, [this](const QString &output) {
        if (progressDialog && progressDialog->isVisible()) {
            progressDialog->appendOutput(output);
        }
    });
    connect(packageManager, &PackageManager::operationFinished, this, &MainWindow::onOperationFinished);
    connect(packageManager, &PackageManager::operationStalled, this, &MainWindow::onOperationStalled);
    connect(packageManager, &PackageManager::cancelRejected, this, &MainWindow::onCancelRejected);
}

void MainWindow::refreshPackageList()
{
    showStatusMessage("正在加载已安装包列表...");
    packageManager->loadInstalledPackages();
}

void MainWindow::startBackgroundAptUpdate()
{
    // 等首屏渲染和已安装列表加载开始后再更新软件源
    QTimer::singleShot(1500, this, [this]() {
        m_backgroundUpdate = true;
        // 请求未被接受（已有操作在跑）时立即复位，避免之后误吞用户发起的更新
        if (!packageManager->updateAptCache()) {
            m_backgroundUpdate = false;
        }
    });
}

void MainWindow::searchLocalPackages()
{
    QString searchText = searchEdit->text().trimmed();
    if (searchText.isEmpty()) {
        refreshPackageList();
        return;
    }
    
    showStatusMessage(QString("正在搜索包: %1").arg(searchText));
    
    QList<DebPackage> filteredPackages;
    for (const DebPackage &pkg : allPackages) {
        // 界面承诺的是按包名搜索，匹配描述会把大量无关包带进结果
        if (pkg.name.contains(searchText, Qt::CaseInsensitive)) {
            filteredPackages.append(pkg);
        }
    }
    
    populatePackageTable(filteredPackages);
    showStatusMessage(QString("找到 %1 个匹配的包").arg(filteredPackages.size()));
}

void MainWindow::removeSelectedPackages()
{
    QStringList packageNames = getSelectedPackages();
    if (packageNames.isEmpty()) {
        QMessageBox::warning(this, "警告", "请选择要卸载的包");
        return;
    }
    
    bool purge = purgeConfigCheckbox->isChecked();
    QString confirmMsg = purge ? "确定要完全卸载以下包并删除配置文件吗？\n%1" : "确定要卸载以下包吗？\n%1";
    
    int result = QMessageBox::question(this, "确认卸载", 
                                     QString(confirmMsg).arg(packageNames.join("\n")),
                                     QMessageBox::Yes | QMessageBox::No);
    
    if (result == QMessageBox::Yes) {
        // 使用批量卸载方法，只请求一次权限
        packageManager->removeMultiplePackages(packageNames, purge);
    }
}

void MainWindow::installLocalPackages()
{
    QStringList filePaths = QFileDialog::getOpenFileNames(this, 
                                                        "选择要安装的DEB包", 
                                                        QDir::homePath(), 
                                                        "DEB包文件 (*.deb)");
    
    if (filePaths.isEmpty()) {
        return;
    }
    
    int result = QMessageBox::question(this, "确认安装", 
                                     QString("确定要安装以下DEB包吗？\n%1").arg(filePaths.join("\n")),
                                     QMessageBox::Yes | QMessageBox::No);
    
    if (result == QMessageBox::Yes) {
        // 使用批量安装方法，只请求一次权限
        packageManager->installMultiplePackages(filePaths);
    }
}

void MainWindow::autoRemovePackages()
{
    int result = QMessageBox::question(this, "确认自动清理", 
                                     "确定要执行自动清理吗？这将移除不再需要的依赖包。",
                                     QMessageBox::Yes | QMessageBox::No);
    
    if (result == QMessageBox::Yes) {
        packageManager->autoRemove();
    }
}

void MainWindow::populatePackageTable(const QList<DebPackage> &packages)
{
    // 逐行写入时若排序生效，行会在 setItem 之间被重排，导致各列错位
    const bool sorting = packageTable->isSortingEnabled();
    packageTable->setSortingEnabled(false);

    packageTable->setRowCount(0);
    
    for (int i = 0; i < packages.size(); ++i) {
        const DebPackage &pkg = packages[i];
        
        packageTable->insertRow(i);
        
        // 第一列：复选框
        QTableWidgetItem *checkItem = new QTableWidgetItem();
        checkItem->setCheckState(Qt::Unchecked);
        checkItem->setTextAlignment(Qt::AlignCenter);
        packageTable->setItem(i, 0, checkItem);
        
        // 第二列：包名
        QTableWidgetItem *nameItem = new QTableWidgetItem(pkg.name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        packageTable->setItem(i, 1, nameItem);
        
        // 第三列：版本
        QTableWidgetItem *versionItem = new QTableWidgetItem(pkg.version);
        versionItem->setFlags(versionItem->flags() & ~Qt::ItemIsEditable);
        packageTable->setItem(i, 2, versionItem);
        
        // 第四列：描述
        QTableWidgetItem *descItem = new QTableWidgetItem(pkg.description);
        descItem->setFlags(descItem->flags() & ~Qt::ItemIsEditable);
        packageTable->setItem(i, 3, descItem);
    }
    
    // 调整列宽
    packageTable->resizeColumnsToContents();
    packageTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

    packageTable->setSortingEnabled(sorting);
}

void MainWindow::onOperationStarted(PackageManager::OperationType type, const QString &message)
{
    // 加载已安装列表属于后台刷新，不占用用户操作的进度对话框
    if (type == PackageManager::OperationType::LoadInstalled) {
        showStatusMessage("加载中");
        return;
    }

    // 启动时的 apt update 与安装后的在线列表刷新都是静默后台操作
    const bool silent = (type == PackageManager::OperationType::UpdateCache && m_backgroundUpdate)
            || (type == PackageManager::OperationType::SearchOnline && m_silentOnlineSearch);
    if (silent) {
        showStatusMessage(message);
        return;
    }

    progressBar->setVisible(true);
    progressBar->setRange(0, 0); // 无限进度条
    setEnabled(false);
    showStatusMessage(message);

    // 释放上一次操作遗留的对话框，避免重复创建后泄漏
    delete progressDialog;
    progressDialog = new OperationProgressDialog(this);
    connect(progressDialog, &OperationProgressDialog::cancelRequested,
            this, &MainWindow::cancelCurrentOperation);
    progressDialog->setOperationName(message);
    progressDialog->show();
}

void MainWindow::onOperationProgress(PackageManager::OperationType type, const QString &message)
{
    if (type == PackageManager::OperationType::LoadInstalled) {
        return;
    }

    showStatusMessage(message);

    if (progressDialog && progressDialog->isVisible()) {
        progressDialog->setOperationName(message);
    }
}

void MainWindow::onOperationFinished(PackageManager::OperationType type, bool success,
                                     const QString &message)
{
    if (type == PackageManager::OperationType::LoadInstalled) {
        if (success) {
            allPackages = packageManager->getCurrentPackages();
            populatePackageTable(allPackages);
            showStatusMessage(QString("就绪 - 共加载 %1 个已安装包").arg(allPackages.size()), 0);

            // 在线安装已完成：本地列表更新后重新搜索一次，让“已安装”状态与实际一致
            if (m_refreshOnlineAfterInstall) {
                m_refreshOnlineAfterInstall = false;
                if (!m_lastOnlineSearchText.isEmpty()) {
                    m_silentOnlineSearch = true;
                    packageManager->searchOnlinePackages(m_lastOnlineSearchText);
                }
            }
        } else {
            showStatusMessage(QString("加载包列表失败: %1").arg(message));
        }
        return;
    }

    // 启动时的 apt update 在后台静默执行，不占用进度对话框和窗口可用性
    if (type == PackageManager::OperationType::UpdateCache && m_backgroundUpdate) {
        m_backgroundUpdate = false;
        showStatusMessage(success ? message
                                  : QString("后台更新软件源失败: %1").arg(message));
        return;
    }

    // 安装后的在线列表刷新同样是静默的，不覆盖安装对话框的结果
    const bool silentSearch = type == PackageManager::OperationType::SearchOnline
            && m_silentOnlineSearch;
    if (silentSearch) {
        m_silentOnlineSearch = false;
        if (!success) {
            showStatusMessage(QString("刷新在线列表失败: %1").arg(message));
            return;
        }
    } else {
        progressBar->setVisible(false);
        setEnabled(true);

        // 操作完成后，更新进度对话框状态，启用关闭按钮
        if (progressDialog && progressDialog->isVisible()) {
            progressDialog->setFinished(success, message);
        }

        if (!success) {
            showStatusMessage(QString("操作失败: %1").arg(message));
            return;
        }
    }

    if (type == PackageManager::OperationType::SearchOnline) {
        currentOnlinePackages = packageManager->getOnlineSearchResults();

        // 与本地列表相同：填充期间禁用排序，避免行被重排后列错位
        const bool sorting = onlinePackageTable->isSortingEnabled();
        onlinePackageTable->setSortingEnabled(false);

        // 清空表格
        onlinePackageTable->setRowCount(0);

        // 填充表格
        for (int i = 0; i < currentOnlinePackages.size(); ++i) {
            const DebPackage &pkg = currentOnlinePackages[i];

            onlinePackageTable->insertRow(i);

            // 第一列：复选框
            QTableWidgetItem *checkItem = new QTableWidgetItem();
            checkItem->setCheckState(Qt::Unchecked);
            checkItem->setTextAlignment(Qt::AlignCenter);
            onlinePackageTable->setItem(i, 0, checkItem);

            // 第二列：包名
            QTableWidgetItem *nameItem = new QTableWidgetItem(pkg.name);
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
            onlinePackageTable->setItem(i, 1, nameItem);

            // 第三列：在线版本
            QTableWidgetItem *onlineVersionItem = new QTableWidgetItem(pkg.version);
            onlineVersionItem->setFlags(onlineVersionItem->flags() & ~Qt::ItemIsEditable);
            onlineVersionItem->setTextAlignment(Qt::AlignCenter);
            onlinePackageTable->setItem(i, 2, onlineVersionItem);

            // 第四列：已安装版本，未安装时留空
            QString installedText;
            if (!pkg.installedVersion.isEmpty()) {
                installedText = pkg.installedVersion;
            } else if (pkg.installed) {
                installedText = "是";
            }
            QTableWidgetItem *installedItem = new QTableWidgetItem(installedText);
            installedItem->setFlags(installedItem->flags() & ~Qt::ItemIsEditable);
            installedItem->setTextAlignment(Qt::AlignCenter);
            onlinePackageTable->setItem(i, 3, installedItem);

            // 第五列：描述
            QTableWidgetItem *descItem = new QTableWidgetItem(pkg.description);
            descItem->setFlags(descItem->flags() & ~Qt::ItemIsEditable);
            onlinePackageTable->setItem(i, 4, descItem);
        }

        // 调整列宽
        onlinePackageTable->resizeColumnsToContents();
        onlinePackageTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);

        onlinePackageTable->setSortingEnabled(sorting);

        // 显示搜索结果数量
        showStatusMessage(QString("找到 %1 个匹配的包").arg(currentOnlinePackages.size()));
        return;
    }

    // 在线安装完成后，等待本地列表刷新结束再刷新在线列表的安装状态
    if (type == PackageManager::OperationType::InstallOnline) {
        m_refreshOnlineAfterInstall = true;
    }

    // 安装或卸载操作完成后需要刷新列表
    refreshPackageList();
}

void MainWindow::onOperationStalled(PackageManager::OperationType type)
{
    Q_UNUSED(type)

    if (progressDialog && progressDialog->isVisible()) {
        progressDialog->appendOutput(
                    QStringLiteral("\n[提示] 操作已长时间没有输出，如需终止请点击“取消操作”。"));
    }
    showStatusMessage("操作长时间没有输出，可点击“取消操作”终止");
}

void MainWindow::cancelCurrentOperation()
{
    if (progressDialog && progressDialog->isVisible()) {
        progressDialog->setOperationName("正在取消操作...");
    }
    showStatusMessage("正在取消操作...");
    packageManager->cancelOperation();
}

void MainWindow::onCancelRejected(const QString &message)
{
    showStatusMessage(QString("取消失败: %1").arg(message));
    if (progressDialog && progressDialog->isVisible()) {
        progressDialog->cancelFailed(message);
    }
}

void MainWindow::onItemChanged(QTableWidgetItem *item)
{
    if (!item) return;
    
    // 只有当修改的是第一列的复选框时才处理
    if (item->column() == 0) {
        // 禁用信号避免无限循环
        packageTable->blockSignals(true);
        
        int row = item->row();
        if (item->checkState() == Qt::Checked) {
            // 选中复选框时，将当前行添加到选择中
            for (int col = 1; col < packageTable->columnCount(); ++col) {
                QTableWidgetItem *tableItem = packageTable->item(row, col);
                if (tableItem) {
                    tableItem->setSelected(true);
                }
            }
        } else {
            // 取消复选框时，从选择中移除当前行
            for (int col = 1; col < packageTable->columnCount(); ++col) {
                QTableWidgetItem *tableItem = packageTable->item(row, col);
                if (tableItem) {
                    tableItem->setSelected(false);
                }
            }
        }
        
        // 恢复信号
        packageTable->blockSignals(false);
        
        // 更新状态信息
        onPackageSelectionChanged();
    }
}

void MainWindow::onPackageSelectionChanged()
{
    // 禁用信号避免无限循环
    packageTable->blockSignals(true);
    
    // 同步行选择和复选框状态
    for (int i = 0; i < packageTable->rowCount(); ++i) {
        QTableWidgetItem *checkItem = packageTable->item(i, 0);
        QTableWidgetItem *nameItem = packageTable->item(i, 1);
        
        if (checkItem && nameItem) {
            // 检查行是否被选中
            bool isSelected = nameItem->isSelected();
            checkItem->setCheckState(isSelected ? Qt::Checked : Qt::Unchecked);
        }
    }
    
    // 恢复信号
    packageTable->blockSignals(false);
    
    // 更新状态信息
    int selectedCount = 0;
    for (int i = 0; i < packageTable->rowCount(); ++i) {
        QTableWidgetItem *nameItem = packageTable->item(i, 1);
        if (nameItem && nameItem->isSelected()) {
            selectedCount++;
        }
    }
    showStatusMessage(QString("已选择 %1 个包").arg(selectedCount));
}

QStringList MainWindow::getSelectedPackages()
{
    QStringList selectedPackages;
    
    for (int i = 0; i < packageTable->rowCount(); ++i) {
        QTableWidgetItem *checkItem = packageTable->item(i, 0);
        if (checkItem && checkItem->checkState() == Qt::Checked) {
            QTableWidgetItem *nameItem = packageTable->item(i, 1);
            if (nameItem) {
                selectedPackages.append(nameItem->text());
            }
        }
    }
    
    return selectedPackages;
}



void MainWindow::onTabChanged(int index)
{
    // 当切换到在线安装页时，检查apt缓存
    if (index == 1) {
        if (!packageManager->checkAptCache()) {
            int result = QMessageBox::question(this, "apt缓存过期", 
                                            "apt缓存已过期或不存在，是否更新？",
                                            QMessageBox::Yes | QMessageBox::No);
            if (result == QMessageBox::Yes) {
                updateAptCache();
            }
        }
    } 
    // 当切换到本地包管理页时，延迟执行刷新，确保窗口渲染完成
    else if (index == 0) {
        QTimer::singleShot(100, this, [this]() {
            refreshPackageList();
        });
    }
}

void MainWindow::searchOnlinePackages()
{
    QString searchText = onlineSearchEdit->text().trimmed();
    if (searchText.isEmpty()) {
        QMessageBox::warning(this, "警告", "请输入搜索关键词");
        return;
    }

    // 记录关键词，安装完成后用它静默刷新一次列表
    m_lastOnlineSearchText = searchText;
    packageManager->searchOnlinePackages(searchText);
}

void MainWindow::installSelectedOnlinePackages()
{
    QStringList packageNames = getSelectedOnlinePackages();
    if (packageNames.isEmpty()) {
        QMessageBox::warning(this, "警告", "请选择要安装的包");
        return;
    }
    
    int result = QMessageBox::question(this, "确认安装", 
                                     QString("确定要安装以下包吗？\n%1").arg(packageNames.join("\n")),
                                     QMessageBox::Yes | QMessageBox::No);
    
    if (result == QMessageBox::Yes) {
        packageManager->installOnlinePackages(packageNames);
    }
}

void MainWindow::updateAptCache()
{
    if (!packageManager->updateAptCache()) {
        showStatusMessage(QString("更新失败: %1").arg(packageManager->getLastError()));
    }
}

void MainWindow::upgradeSystem()
{
    bool distUpgrade = distUpgradeCheckbox->isChecked();
    QString confirmMsg = distUpgrade
            ? "确定要执行全部更新（dist-upgrade）吗？依赖冲突时将移除已安装的包，可能会改变系统版本。"
            : "确定要执行全部更新（upgrade）吗？将升级所有已安装的包。";
    
    int result = QMessageBox::question(this, "确认更新", 
                                     confirmMsg,
                                     QMessageBox::Yes | QMessageBox::No);
    
    if (result == QMessageBox::Yes) {
        packageManager->upgradeSystem(distUpgrade);
    }
}

void MainWindow::onOnlinePackageSelectionChanged()
{
    // 同步行选择和复选框状态
    onlinePackageTable->blockSignals(true);
    
    for (int i = 0; i < onlinePackageTable->rowCount(); ++i) {
        QTableWidgetItem *checkItem = onlinePackageTable->item(i, 0);
        QTableWidgetItem *nameItem = onlinePackageTable->item(i, 1);
        
        if (checkItem && nameItem) {
            bool isSelected = nameItem->isSelected();
            checkItem->setCheckState(isSelected ? Qt::Checked : Qt::Unchecked);
        }
    }
    
    onlinePackageTable->blockSignals(false);
    
    // 更新状态信息
    int selectedCount = 0;
    for (int i = 0; i < onlinePackageTable->rowCount(); ++i) {
        QTableWidgetItem *nameItem = onlinePackageTable->item(i, 1);
        if (nameItem && nameItem->isSelected()) {
            selectedCount++;
        }
    }
    showStatusMessage(QString("已选择 %1 个在线包").arg(selectedCount));
}

void MainWindow::onOnlineItemChanged(QTableWidgetItem *item)
{
    if (!item) return;
    
    // 只有当修改的是第一列的复选框时才处理
    if (item->column() == 0) {
        // 禁用信号避免无限循环
        onlinePackageTable->blockSignals(true);
        
        int row = item->row();
        if (item->checkState() == Qt::Checked) {
            // 选中复选框时，将当前行添加到选择中
            for (int col = 1; col < onlinePackageTable->columnCount(); ++col) {
                QTableWidgetItem *tableItem = onlinePackageTable->item(row, col);
                if (tableItem) {
                    tableItem->setSelected(true);
                }
            }
        } else {
            // 取消复选框时，从选择中移除当前行
            for (int col = 1; col < onlinePackageTable->columnCount(); ++col) {
                QTableWidgetItem *tableItem = onlinePackageTable->item(row, col);
                if (tableItem) {
                    tableItem->setSelected(false);
                }
            }
        }
        
        // 恢复信号
        onlinePackageTable->blockSignals(false);
        
        // 更新状态信息
        onOnlinePackageSelectionChanged();
    }
}

QStringList MainWindow::getSelectedOnlinePackages()
{
    QStringList selectedPackages;
    
    for (int i = 0; i < onlinePackageTable->rowCount(); ++i) {
        QTableWidgetItem *checkItem = onlinePackageTable->item(i, 0);
        if (checkItem && checkItem->checkState() == Qt::Checked) {
            QTableWidgetItem *nameItem = onlinePackageTable->item(i, 1);
            if (nameItem) {
                selectedPackages.append(nameItem->text());
            }
        }
    }
    
    return selectedPackages;
}

void MainWindow::showStatusMessage(const QString &message, int timeout)
{
    statusLabel->setText(message);
    if (timeout > 0) {
        // 绑定上下文对象，窗口销毁时由 Qt 取消定时器，避免访问悬空 this
        QTimer::singleShot(timeout, this, [this]() {
            statusLabel->setText("就绪");
        });
    }
}