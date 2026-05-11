// =============================================================================
// RemoteDialog.cpp —— 见 RemoteDialog.h
// =============================================================================

#include "RemoteDialog.h"
#include "SftpClient.h"

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
    setWindowTitle("Remote Connection");
    setMinimumSize(560, 540);

    auto* layout = new QVBoxLayout(this);

    // ---- Saved connections ----
    auto* savedGroup  = new QGroupBox("Saved Connections");
    auto* savedLayout = new QVBoxLayout(savedGroup);
    m_savedList = new QListWidget();
    m_savedList->setMaximumHeight(120);
    savedLayout->addWidget(m_savedList);
    layout->addWidget(savedGroup);

    setupSavedConnections();

    // ---- Connection form ----
    auto* formGroup  = new QGroupBox("Connection Details");
    auto* formLayout = new QFormLayout(formGroup);

    m_protocolCombo = new QComboBox();
    m_protocolCombo->addItems({"SFTP", "SSH"});  // SFTP first - it's the data path
    formLayout->addRow("Protocol:", m_protocolCombo);

    m_hostInput = new QLineEdit();
    m_hostInput->setPlaceholderText("hostname or IP");
    formLayout->addRow("Host:", m_hostInput);

    m_portSpin = new QSpinBox();
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    formLayout->addRow("Port:", m_portSpin);

    m_usernameInput = new QLineEdit();
    m_usernameInput->setPlaceholderText("username");
    formLayout->addRow("Username:", m_usernameInput);

    m_passwordInput = new QLineEdit();
    m_passwordInput->setEchoMode(QLineEdit::Password);
    m_passwordInput->setPlaceholderText("leave empty if using a private key");
    formLayout->addRow("Password:", m_passwordInput);

    auto* keyRow      = new QHBoxLayout();
    m_keyPathInput    = new QLineEdit();
    m_keyPathInput->setPlaceholderText("optional, e.g. ~/.ssh/id_rsa");
    m_browseKeyBtn    = new QPushButton("Browse…");
    keyRow->addWidget(m_keyPathInput);
    keyRow->addWidget(m_browseKeyBtn);
    formLayout->addRow("Private key:", keyRow);

    m_passphraseInput = new QLineEdit();
    m_passphraseInput->setEchoMode(QLineEdit::Password);
    m_passphraseInput->setPlaceholderText("only if the key is encrypted");
    formLayout->addRow("Key passphrase:", m_passphraseInput);

    layout->addWidget(formGroup);

    // ---- Status line ----
    m_statusLabel = new QLabel("Ready.");
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet("color: #888;");
    layout->addWidget(m_statusLabel);

    // ---- Buttons ----
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_cancelBtn = new QPushButton("Cancel");
    m_connectBtn = new QPushButton("Connect");
    m_connectBtn->setDefault(true);
    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_connectBtn);
    layout->addLayout(btnLayout);

    connect(m_savedList,   &QListWidget::itemDoubleClicked, this, &RemoteDialog::onSavedConnectionClicked);
    connect(m_savedList,   &QListWidget::itemClicked,        this, &RemoteDialog::onSavedConnectionClicked);
    connect(m_connectBtn,  &QPushButton::clicked,            this, &RemoteDialog::onConnectClicked);
    connect(m_cancelBtn,   &QPushButton::clicked,            this, &QDialog::reject);
    connect(m_browseKeyBtn,&QPushButton::clicked,            this, &RemoteDialog::onBrowseKey);
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
    dev.name = "Dev Server"; dev.protocol = "SSH";
    dev.host = "dev.example.com"; dev.port = 2222; dev.username = "developer";
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
}

void RemoteDialog::onBrowseKey() {
    QString start = QDir::homePath() + "/.ssh";
    if (!QDir(start).exists()) start = QDir::homePath();
    QString path = QFileDialog::getOpenFileName(this, "Select private key", start);
    if (!path.isEmpty()) m_keyPathInput->setText(path);
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

    // Expand a leading "~" so users can type "~/.ssh/id_rsa".
    if (conn.privateKeyPath.startsWith("~/")) {
        conn.privateKeyPath = QDir::homePath() + conn.privateKeyPath.mid(1);
    }

    if (conn.host.isEmpty()) {
        QMessageBox::warning(this, "Missing host", "Please enter a host name or IP.");
        m_hostInput->setFocus(); return;
    }
    if (conn.username.isEmpty()) {
        QMessageBox::warning(this, "Missing user", "Please enter a username.");
        m_usernameInput->setFocus(); return;
    }
    if (conn.password.isEmpty() && conn.privateKeyPath.isEmpty()) {
        QMessageBox::warning(this, "Missing credentials",
            "Provide a password, a private key, or both.");
        m_passwordInput->setFocus(); return;
    }

    setBusy(true);
    m_statusLabel->setStyleSheet("color: #888;");
    m_statusLabel->setText(QString("Connecting to %1@%2:%3 …")
        .arg(conn.username, conn.host).arg(conn.port));
    QApplication::processEvents();

    auto client = std::make_unique<SftpClient>();
    bool ok = client->connectAndAuth(conn, /*openSftpChannel=*/true);
    setBusy(false);

    if (!ok) {
        m_statusLabel->setStyleSheet("color: #c0392b;");
        m_statusLabel->setText("✗ " + client->lastError());
        QMessageBox::critical(this, "Connection failed", client->lastError());
        return;
    }

    conn.fingerprint = client->hostFingerprintSha256();
    if (conn.name.isEmpty()) {
        conn.name = QString("%1@%2").arg(conn.username, conn.host);
    }
    m_lastConnected = conn;
    m_client = std::move(client);

    m_statusLabel->setStyleSheet("color: #27ae60;");
    m_statusLabel->setText(QString("✓ Connected. Host key %1")
        .arg(conn.fingerprint.isEmpty() ? "(unknown)" : conn.fingerprint));

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
