#ifndef REMOTEDIALOG_H
#define REMOTEDIALOG_H

// =============================================================================
// RemoteDialog —— SFTP / SSH 远程连接对话框
//
// 用法：MainWindow 弹这个对话框，用户填表并点 Connect 后：
//   1. 本对话框同步调 SftpClient::connectAndAuth（GUI 线程阻塞，但有等待
//      光标 + 状态 label 提示）
//   2. 成功才 accept()；失败弹 QMessageBox 让用户改参数继续尝试
//   3. MainWindow 用 takeClient() 把活会话拿走，挂进 FileSystemRouter
//
// 字段：协议 / 主机 / 端口 / 用户名 / 密码 / 私钥路径 / passphrase
// （任选密码或私钥；都填的话先尝试公钥，失败时回退到密码 —— 详见 SftpClient）。
//
// 已知坑（已修）：早期版本在 setBusy(true/false) 里同时压两次
// QApplication::setOverrideCursor 但只 restore 一次，导致登录后 Arrow 光标
// 锁死。现在用 m_cursorOverridden 状态位严格保证 push/pop 配对。
// =============================================================================

#include "Types.h"
#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QVector>
#include <memory>

class SftpClient;

class RemoteDialog : public QDialog {
    Q_OBJECT

public:
    explicit RemoteDialog(QWidget* parent = nullptr);
    ~RemoteDialog() override;

    /// 如果对话框已 accept，返回当时成功登录的连接配置；否则返回当前界面值
    RemoteConnection getConnection() const;

    /// 把已登录成功的活会话交给调用方（move 语义）。
    /// 调用方负责保管、最终 disconnect。
    std::unique_ptr<SftpClient> takeClient();

signals:
    /// 仅在真实登录成功后发出（与"用户点 Connect 但失败"严格区分）
    void connectRequested(const RemoteConnection& conn);

private slots:
    void onSavedConnectionClicked(QListWidgetItem* item);  // 点已保存条目 → 填表
    void onConnectClicked();                                // 校验 + 真实拨号
    void onBrowseKey();                                     // 弹文件选择器选私钥

private:
    void setupSavedConnections();   // 内置几条示例（不含密码）
    /**
     * 锁/解锁所有输入控件，并管理"忙等"鼠标光标。
     * 必须严格成对调用（true 一次 + false 一次），否则会光标卡死。
     */
    void setBusy(bool busy);

    // ---- 控件 ----
    QListWidget* m_savedList;
    QComboBox*   m_protocolCombo;
    QLineEdit*   m_hostInput;
    QSpinBox*    m_portSpin;
    QLineEdit*   m_usernameInput;
    QLineEdit*   m_passwordInput;
    QLineEdit*   m_keyPathInput;
    QPushButton* m_browseKeyBtn;
    QLineEdit*   m_passphraseInput;
    QPushButton* m_connectBtn;
    QPushButton* m_cancelBtn;
    QLabel*      m_statusLabel;          // 显示进度/错误/指纹

    QVector<RemoteConnection> m_savedConnections;     // 内置示例
    RemoteConnection          m_lastConnected;        // 登录成功的那次
    std::unique_ptr<SftpClient> m_client;             // 活会话，等 takeClient
    bool                      m_cursorOverridden = false;  // 光标 push/pop 守门
};

#endif // REMOTEDIALOG_H
