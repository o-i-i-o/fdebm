#include "operationprogressdialog.h"
#include <QMessageBox>
#include <QScrollBar>
#include <QTextCursor>

OperationProgressDialog::OperationProgressDialog(QWidget *parent)
    : QDialog(parent)
    , isFinished(false)
{
    setWindowTitle("操作进度");
    setMinimumSize(600, 400);
    setModal(true);

    // 创建布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // 操作名称标签
    operationLabel = new QLabel("正在执行操作...");
    operationLabel->setStyleSheet("font-weight: bold;");
    mainLayout->addWidget(operationLabel);

    // 输出文本框
    outputTextEdit = new QTextEdit();
    outputTextEdit->setReadOnly(true);
    outputTextEdit->setFontFamily("Monospace");
    outputTextEdit->setLineWrapMode(QTextEdit::NoWrap);
    mainLayout->addWidget(outputTextEdit);

    // 按钮布局
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    // 关闭按钮：运行中为“取消操作”，结束后变为“关闭”
    closeButton = new QPushButton("取消操作");
    connect(closeButton, &QPushButton::clicked, this, &OperationProgressDialog::requestCancel);
    buttonLayout->addWidget(closeButton);

    mainLayout->addLayout(buttonLayout);
}

OperationProgressDialog::~OperationProgressDialog()
{
}

void OperationProgressDialog::setOperationName(const QString &name)
{
    operationLabel->setText(name);
}

void OperationProgressDialog::appendOutput(const QString &output)
{
    // apt/dpkg 输出按纯文本插入，避免其中的 <...> 被当作富文本解析
    QTextCursor cursor(outputTextEdit->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(output + QLatin1Char('\n'));

    // 自动滚动到底部
    QScrollBar *scrollBar = outputTextEdit->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void OperationProgressDialog::setFinished(bool success, const QString &message)
{
    Q_UNUSED(success)

    isFinished = true;

    appendOutput("\n" + message);
    closeButton->setText("关闭");
    closeButton->setEnabled(true);
    closeButton->setFocus();
}

void OperationProgressDialog::requestCancel()
{
    if (isFinished) {
        close();
        return;
    }
    if (isCancelling) {
        return;
    }

    const int result = QMessageBox::question(
                this, "取消操作",
                "确定要取消当前操作吗？\n"
                "取消会终止正在运行的 apt 进程，系统可能停留在部分完成的状态。",
                QMessageBox::Yes | QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }

    isCancelling = true;
    closeButton->setEnabled(false);
    closeButton->setText("正在取消...");
    emit cancelRequested();
}

void OperationProgressDialog::cancelFailed(const QString &message)
{
    appendOutput("\n取消失败: " + message);
    isCancelling = false;
    closeButton->setText("取消操作");
    closeButton->setEnabled(true);
}

void OperationProgressDialog::closeEvent(QCloseEvent *event)
{
    if (isFinished) {
        event->accept();
        return;
    }

    // 关闭窗口等同请求取消；事务结束前不关闭对话框，避免丢失输出
    event->ignore();
    requestCancel();
}