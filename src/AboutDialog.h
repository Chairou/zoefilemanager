#ifndef ABOUTDIALOG_H
#define ABOUTDIALOG_H

// =============================================================================
// AboutDialog —— "关于本应用"对话框
//
// 职责：
//   1. 展示应用名 / 版本 / Copyright
//   2. 醒目的"This software uses Qt 6 under the LGPL v3" 致谢（满足 LGPL
//      第 4 条 "appropriate copyright notice" 义务）
//   3. 列出第三方依赖：Qt / libssh2 / OpenSSL；点击对应按钮在底部 QTextEdit
//      里加载 Contents/Resources/licenses/<name>.txt 全文
//
// 与商用合规相关的核心承诺都集中在这里，文案改动（比如年份、版本号）
// 只动一处。许可证全文由 CMakeLists 在 build 时复制进 bundle，**不**
// 通过 qrc 资源系统嵌入二进制 —— 用户拿到 bundle 后可以肉眼检查
// licenses/ 目录，符合"显式可见"的精神。
// =============================================================================

#include <QDialog>

class QTextEdit;
class QLabel;

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);

private slots:
    /// 把 Contents/Resources/licenses/<name> 的内容载到 m_licenseView。
    /// 找不到文件时显示一段提示文字，不抛异常。
    void showLicenseFile(const QString& fileName);
    void retranslate();

private:
    QString licensesDir() const;     // 计算 bundle 内 licenses/ 绝对路径

    QTextEdit* m_licenseView = nullptr;
    QLabel*    m_currentLabel = nullptr;
    QLabel*    m_subLabel = nullptr;         // "Version X / Copyright..."
    QLabel*    m_ackLabel = nullptr;         // LGPL 致谢大段 HTML
    QString    m_currentLicenseFile;         // retranslate 时还原 "Showing: xxx"
};

#endif // ABOUTDIALOG_H
