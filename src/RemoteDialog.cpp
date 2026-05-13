// =============================================================================
// RemoteDialog.cpp —— 见 RemoteDialog.h
// =============================================================================

#include "RemoteDialog.h"
#include "SftpClient.h"
#include "SmbClient.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QDir>

RemoteDialog::RemoteDialog(QWidget* parent)
    : QDialog(parent)
{
    setMinimumSize(560, 540);

    auto* layout = new QVBoxLayout(this);

    // ---- Saved connections ----
    m_savedGroup      = new QGroupBox();
    auto* savedLayout = new QVBoxLayout(m_savedGroup);
    m_savedList = new QListWidget();
    m_savedList->setMaximumHeight(120);
    savedLayout->addWidget(m_savedList);
    layout->addWidget(m_savedGroup);

    setupSavedConnections();

    // ---- Connection form ----
    m_formGroup  = new QGroupBox();
    m_formLayout = new QFormLayout(m_formGroup);
    // UI: 用户要求所有输入框左对齐 —— 标签靠左、整表靠左、不换行
    m_formLayout->setLabelAlignment(Qt::AlignLeft);
    m_formLayout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_formLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);

    m_protocolCombo = new QComboBox();
    m_protocolCombo->addItems({"SFTP", "SMB"});  // 去掉 SSH；SMB 是文件共享
    m_protocolCombo->setMinimumWidth(200);  // UI: 协议下拉框宽度 ×2
    m_formLayout->addRow(QString(), m_protocolCombo);

    m_hostInput = new QLineEdit();
    m_formLayout->addRow(QString(), m_hostInput);

    m_portSpin = new QSpinBox();
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_formLayout->addRow(QString(), m_portSpin);

    m_usernameInput = new QLineEdit();
    m_formLayout->addRow(QString(), m_usernameInput);

    m_passwordInput = new QLineEdit();
    m_passwordInput->setEchoMode(QLineEdit::Password);
    m_formLayout->addRow(QString(), m_passwordInput);

    auto* keyRow      = new QHBoxLayout();
    m_keyPathInput    = new QLineEdit();
    m_browseKeyBtn    = new QPushButton();
    keyRow->addWidget(m_keyPathInput);
    keyRow->addWidget(m_browseKeyBtn);
    // 用一个 QWidget 容器包裹 keyRow，方便根据协议整行 show/hide
    m_keyRowWidget = new QWidget();
    m_keyRowWidget->setLayout(keyRow);
    m_formLayout->addRow(QString(), m_keyRowWidget);

    m_passphraseInput = new QLineEdit();
    m_passphraseInput->setEchoMode(QLineEdit::Password);
    m_formLayout->addRow(QString(), m_passphraseInput);

    // ---- SMB 字段：Share / Workgroup ----
    m_shareInput = new QLineEdit();
    m_formLayout->addRow(QString(), m_shareInput);
    m_workgroupInput = new QLineEdit();
    m_formLayout->addRow(QString(), m_workgroupInput);

    layout->addWidget(m_formGroup);

    // ---- Status line ----
    m_statusLabel = new QLabel();
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet("color: #888;");
    layout->addWidget(m_statusLabel);

    // ---- Buttons ----
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_cancelBtn = new QPushButton();
    m_connectBtn = new QPushButton();
    m_connectBtn->setDefault(true);
    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_connectBtn);
    layout->addLayout(btnLayout);

    connect(m_savedList,   &QListWidget::itemDoubleClicked, this, &RemoteDialog::onSavedConnectionClicked);
    connect(m_savedList,   &QListWidget::itemClicked,        this, &RemoteDialog::onSavedConnectionClicked);
    connect(m_connectBtn,  &QPushButton::clicked,            this, &RemoteDialog::onConnectClicked);
    connect(m_cancelBtn,   &QPushButton::clicked,            this, &QDialog::reject);
    connect(m_browseKeyBtn,&QPushButton::clicked,            this, &RemoteDialog::onBrowseKey);
    connect(m_protocolCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RemoteDialog::onProtocolChanged);

    connect(&I18n::instance(), &I18n::changed, this, &RemoteDialog::retranslate);
    retranslate();
    onProtocolChanged(m_protocolCombo->currentIndex());   // 初始化字段可见性+端口
}

// 协议切换：SFTP 隐藏 Share/Workgroup 字段，显示私钥/passphrase 字段，
// 默认端口 22；SMB 相反：显示 Share/Workgroup，隐藏私钥/passphrase，默认端口 445。
void RemoteDialog::onProtocolChanged(int /*index*/) {
    const QString proto = m_protocolCombo->currentText();
    const bool isSmb = (proto == "SMB");

    auto showRow = [this](QWidget* field, bool visible) {
        if (!field) return;
        field->setVisible(visible);
        if (auto* lbl = m_formLayout->labelForField(field)) lbl->setVisible(visible);
    };

    // 私钥/passphrase 仅 SFTP 需要
    showRow(m_keyRowWidget,    !isSmb);
    showRow(m_passphraseInput, !isSmb);

    // Share/Workgroup 仅 SMB 需要
    showRow(m_shareInput,     isSmb);
    showRow(m_workgroupInput, isSmb);

    // Password 字段的占位提示按协议切换 —— SMB 不支持私钥认证，
    // 默认情境就是"填密码或匿名/guest"，不应再显示 SFTP 风格的"若使用私钥请留空"。
    if (m_passwordInput) {
        m_passwordInput->setPlaceholderText(isSmb
            ? T("password (leave empty for guest)")
            : T("leave empty if using a private key"));
    }
    // Username placeholder：SMB 在企业 AD 域里常常要 DOMAIN\user 或 user@domain
    if (m_usernameInput) {
        m_usernameInput->setPlaceholderText(isSmb
            ? T("username (try DOMAIN\\\\user or user@domain in AD environments)")
            : T("username"));
    }
    // Workgroup placeholder：企业域时填大写域名，家用留空
    if (m_workgroupInput) {
        m_workgroupInput->setPlaceholderText(
            T("home/SOHO: leave empty; corporate AD: enter your domain (e.g. TENCENT)"));
    }

    // 协议对应默认端口（仅当当前端口仍是上一协议的默认值时才替换，避免
    // 覆盖用户手动输入的自定义端口）
    if (isSmb && (m_portSpin->value() == 22 || m_portSpin->value() == 0)) {
        m_portSpin->setValue(445);
    } else if (!isSmb && (m_portSpin->value() == 445)) {
        m_portSpin->setValue(22);
    }
}

RemoteDialog::~RemoteDialog() {
    // Belt-and-suspenders: if the dialog dies while still in "busy" state
    // (e.g. parent window closed mid-connect), pop the override cursor so
    // the app doesn't keep a hourglass forever.
    if (m_cursorOverridden) {
        QApplication::restoreOverrideCursor();
        m_cursorOverridden = false;
    }
}

void RemoteDialog::setupSavedConnections() {
    // These are *templates* only - no auto-connect. Real credentials are
    // never stored in the binary.
    RemoteConnection prod;
    prod.name = "Production Server"; prod.protocol = "SFTP";
    prod.host = "prod.example.com";  prod.port = 22; prod.username = "deploy";
    m_savedConnections.append(prod);

    RemoteConnection staging;
    staging.name = "Staging Server"; staging.protocol = "SFTP";
    staging.host = "staging.example.com"; staging.port = 22; staging.username = "admin";
    m_savedConnections.append(staging);

    RemoteConnection dev;
    dev.name = "File Server"; dev.protocol = "SMB";
    dev.host = "fileserver.example.com"; dev.port = 445; dev.username = "guest";
    dev.share = "Public";
    m_savedConnections.append(dev);

    for (const auto& conn : m_savedConnections) {
        m_savedList->addItem(QString("%1 (%2@%3:%4)")
            .arg(conn.name, conn.username, conn.host).arg(conn.port));
    }
}

void RemoteDialog::onSavedConnectionClicked(QListWidgetItem* item) {
    int index = m_savedList->row(item);
    if (index < 0 || index >= m_savedConnections.size()) return;

    const auto& conn = m_savedConnections[index];
    m_protocolCombo->setCurrentText(conn.protocol);
    m_hostInput->setText(conn.host);
    m_portSpin->setValue(conn.port);
    m_usernameInput->setText(conn.username);
    m_passwordInput->clear();
    if (m_shareInput)     m_shareInput->setText(conn.share);
    if (m_workgroupInput) m_workgroupInput->setText(conn.workgroup);
}

void RemoteDialog::onBrowseKey() {
    QString start = QDir::homePath() + "/.ssh";
    if (!QDir(start).exists()) start = QDir::homePath();
    QString path = QFileDialog::getOpenFileName(this, T("Select private key"), start);
    if (!path.isEmpty()) m_keyPathInput->setText(path);
}

void RemoteDialog::retranslate() {
    setWindowTitle(T("Remote Connection"));
    m_savedGroup->setTitle(T("Saved Connections"));
    m_formGroup->setTitle(T("Connection Details"));

    // FormLayout 行标签：通过 labelForField 拿到 QLabel*，重新 setText
    auto setFormLabel = [this](QWidget* field, const QString& text) {
        if (auto* w = m_formLayout->labelForField(field)) {
            if (auto* lbl = qobject_cast<QLabel*>(w)) lbl->setText(text);
        }
    };
    setFormLabel(m_protocolCombo,   T("Protocol:"));
    setFormLabel(m_hostInput,       T("Host:"));
    setFormLabel(m_portSpin,        T("Port:"));
    setFormLabel(m_usernameInput,   T("Username:"));
    setFormLabel(m_passwordInput,   T("Password:"));
    setFormLabel(m_passphraseInput, T("Key passphrase:"));
    if (m_shareInput)     setFormLabel(m_shareInput,     T("Share:"));
    if (m_workgroupInput) setFormLabel(m_workgroupInput, T("Workgroup:"));
    // keyRow 是 QWidget 容器：找它的 label
    if (m_keyRowWidget) setFormLabel(m_keyRowWidget, T("Private key:"));

    m_hostInput->setPlaceholderText(T("hostname or IP"));
    m_usernameInput->setPlaceholderText(T("username"));
    m_passwordInput->setPlaceholderText(T("leave empty if using a private key"));
    m_keyPathInput->setPlaceholderText(T("optional, e.g. ~/.ssh/id_rsa"));
    m_passphraseInput->setPlaceholderText(T("only if the key is encrypted"));
    if (m_shareInput)     m_shareInput->setPlaceholderText(T("share name, e.g. Public"));
    if (m_workgroupInput) m_workgroupInput->setPlaceholderText(T("optional Windows workgroup/domain"));

    m_browseKeyBtn->setText(T("Browse…"));
    m_cancelBtn->setText(T("Cancel"));
    m_connectBtn->setText(T("Connect"));

    if (m_statusIsReady) {
        m_statusLabel->setText(T("Ready."));
    }

    // 语言切换可能把"按协议定制"的占位符（密码框）覆盖回 SFTP 文案，
    // 这里再触发一次协议联动复位。
    if (m_protocolCombo) onProtocolChanged(m_protocolCombo->currentIndex());
}

void RemoteDialog::setBusy(bool busy) {
    m_connectBtn->setEnabled(!busy);
    m_cancelBtn->setEnabled(!busy);
    m_protocolCombo->setEnabled(!busy);
    m_hostInput->setEnabled(!busy);
    m_portSpin->setEnabled(!busy);
    m_usernameInput->setEnabled(!busy);
    m_passwordInput->setEnabled(!busy);
    m_keyPathInput->setEnabled(!busy);
    m_browseKeyBtn->setEnabled(!busy);
    m_passphraseInput->setEnabled(!busy);

    // Cursor: push exactly once on busy=true, pop exactly once on busy=false.
    // The previous implementation pushed WaitCursor on entry AND pushed
    // ArrowCursor on exit before restoring -> the Arrow was left on the
    // override stack and the app's cursor stayed frozen as an arrow even
    // over widgets that should show IBeam / PointingHand. `m_cursorOverridden`
    // guards against double-push/double-pop on accidental re-entry.
    if (busy && !m_cursorOverridden) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_cursorOverridden = true;
    } else if (!busy && m_cursorOverridden) {
        QApplication::restoreOverrideCursor();
        m_cursorOverridden = false;
    }
}

void RemoteDialog::onConnectClicked() {
    RemoteConnection conn;
    conn.protocol       = m_protocolCombo->currentText();
    conn.host           = m_hostInput->text().trimmed();
    conn.port           = m_portSpin->value();
    conn.username       = m_usernameInput->text().trimmed();
    conn.password       = m_passwordInput->text();
    conn.privateKeyPath = m_keyPathInput->text().trimmed();
    conn.passphrase     = m_passphraseInput->text();
    if (m_shareInput)     conn.share     = m_shareInput->text().trimmed();
    if (m_workgroupInput) conn.workgroup = m_workgroupInput->text().trimmed();

    // Expand a leading "~" so users can type "~/.ssh/id_rsa".
    if (conn.privateKeyPath.startsWith("~/")) {
        conn.privateKeyPath = QDir::homePath() + conn.privateKeyPath.mid(1);
    }

    if (conn.host.isEmpty()) {
        QMessageBox::warning(this, T("Missing host"), T("Please enter a host name or IP."));
        m_hostInput->setFocus(); return;
    }
    if (conn.username.isEmpty()) {
        QMessageBox::warning(this, T("Missing user"), T("Please enter a username."));
        m_usernameInput->setFocus(); return;
    }

    // SMB 分支：用 libsmbclient 真实连接
    if (conn.protocol == "SMB") {
        if (conn.share.isEmpty()) {
            QMessageBox::warning(this, T("Missing share"),
                T("Please enter the SMB share name (e.g. Public)."));
            if (m_shareInput) m_shareInput->setFocus();
            return;
        }

        setBusy(true);
        m_statusIsReady = false;
        m_statusLabel->setStyleSheet("color: #888;");
        m_statusLabel->setText(T("Connecting to %1@%2:%3 …")
            .arg(conn.username, conn.host).arg(conn.port));
        QApplication::processEvents();

        auto smb = std::make_unique<SmbClient>();
        bool smbOk = smb->connectAndAuth(conn);
        setBusy(false);

        if (!smbOk) {
            m_statusLabel->setStyleSheet("color: #c0392b;");
            m_statusLabel->setText("✗ " + smb->lastError());
            QMessageBox::critical(this, T("Connection failed"), smb->lastError());
            return;
        }

        if (conn.name.isEmpty()) {
            conn.name = QString("%1@%2/%3").arg(conn.username, conn.host, conn.share);
        }
        m_lastConnected = conn;
        m_smbClient = std::move(smb);
        m_statusLabel->setStyleSheet("color: #27ae60;");
        m_statusLabel->setText(QString("✓ Connected: smb://%1/%2")
            .arg(conn.host, conn.share));

        emit connectRequested(conn);
        accept();
        return;
    }

    // 以下是 SFTP 分支
    if (conn.password.isEmpty() && conn.privateKeyPath.isEmpty()) {
        QMessageBox::warning(this, T("Missing credentials"),
            T("Provide a password, a private key, or both."));
        m_passwordInput->setFocus(); return;
    }

    setBusy(true);
    m_statusIsReady = false;
    m_statusLabel->setStyleSheet("color: #888;");
    m_statusLabel->setText(T("Connecting to %1@%2:%3 …")
        .arg(conn.username, conn.host).arg(conn.port));
    QApplication::processEvents();

    auto client = std::make_unique<SftpClient>();
    bool ok = client->connectAndAuth(conn, /*openSftpChannel=*/true);
    setBusy(false);

    if (!ok) {
        m_statusLabel->setStyleSheet("color: #c0392b;");
        m_statusLabel->setText("✗ " + client->lastError());
        QMessageBox::critical(this, T("Connection failed"), client->lastError());
        return;
    }

    conn.fingerprint = client->hostFingerprintSha256();
    if (conn.name.isEmpty()) {
        conn.name = QString("%1@%2").arg(conn.username, conn.host);
    }
    m_lastConnected = conn;
    m_client = std::move(client);

    m_statusLabel->setStyleSheet("color: #27ae60;");
    m_statusLabel->setText(T("✓ Connected. Host key %1")
        .arg(conn.fingerprint.isEmpty() ? T("(unknown)") : conn.fingerprint));

    emit connectRequested(conn);
    accept();
}

RemoteConnection RemoteDialog::getConnection() const {
    if (!m_lastConnected.host.isEmpty()) return m_lastConnected;
    RemoteConnection conn;
    conn.protocol       = m_protocolCombo->currentText();
    conn.host           = m_hostInput->text();
    conn.port           = m_portSpin->value();
    conn.username       = m_usernameInput->text();
    conn.password       = m_passwordInput->text();
    conn.privateKeyPath = m_keyPathInput->text();
    conn.passphrase     = m_passphraseInput->text();
    return conn;
}

std::unique_ptr<SftpClient> RemoteDialog::takeClient() {
    return std::move(m_client);
}

std::unique_ptr<SmbClient> RemoteDialog::takeSmbClient() {
    return std::move(m_smbClient);
}
