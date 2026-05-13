// =============================================================================
// ShortcutBar.cpp —— 快捷管理器
//
// 数据结构（递归嵌套）：
//   m_items[] : 顶层条目，每条可以是 shortcut（叶子）或 folder（容器）
//   folder.children[] : folder 的子项；可同时装叶子（local/sftp/smb）和
//                       嵌套的子 folder。深度无硬上限。
//
// 持久化（QSettings，递归数组）：
//   shortcuts[i] = { name, path, type, isFolder, expanded,
//                    username, password_obf, workgroup,    // 叶子专属
//                    children[]                            // folder 专属，递归同结构
//   }
//   兼容性：
//     - isFolder 缺省视为 false；
//     - type 缺省按 path 前缀推断（sftp:// → sftp，smb:// → smb，否则 local）；
//     - 旧版本仅支持 children 单层无嵌套的数据，被新代码当作"folder 但不再
//       含子 folder"读入，向后兼容。
// =============================================================================

#include "ShortcutBar.h"
#include "I18n.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QCursor>
#include <QStyle>
#include <QApplication>
#include <QMessageBox>
#include <QByteArray>
#include <QSettings>
#include <QDir>
#include <QFont>

namespace {

// ---- 三种类型的视觉表示 ----
// emoji 选择原则：在大多数中文 macOS 字体下都能正确显示彩色 emoji。
//   local : 📁 蓝绿（Nord8 #88C0D0）—— 与 macOS Finder 习惯一致
//   sftp  : 🌐 绿色（Nord14 #A3BE8C）—— 强调"网络/远程主机"
//   smb   : 🖥️ 紫色（Nord15 #B48EAD）—— 强调"Windows/共享磁盘"
// 类型 → 视觉样式的小映射表。
// 设计意图（重要）：
//   local : 📂 emoji + 青色（Nord8 #88C0D0）—— 用开口文件夹 emoji，
//           与"快捷目录"📁（合口）形成视觉对比，让用户一眼区分
//           "本地真实目录（叶子）" vs "用来组织条目的快捷目录（容器）"
//   sftp  : 🌐 emoji + 绿色（Nord14 #A3BE8C）—— 强调"网络/远程主机"
//   smb   : 🖥 emoji + 紫色（Nord15 #B48EAD）—— 强调"Windows/共享磁盘"
// 三种类型统一走「prefix + emoji + name」文本渲染路径，
// 保证 indent 缩进的文本起点完全一致（避免 setIcon 把文本整体右推）。
struct TypeStyle {
    QString iconUtf8;       // 全部走 emoji 文本（不再使用 setIcon）
    QString accent;         // hover 时的强调色 + 选中态左侧 3px 边色
};

TypeStyle styleFor(const QString& type) {
    if (type == "sftp") return { QString::fromUtf8("\xF0\x9F\x8C\x90"), "#A3BE8C" };  // 🌐
    if (type == "smb")  return { QString::fromUtf8("\xF0\x9F\x96\xA5"), "#B48EAD" };  // 🖥
    /* local */          return { QString::fromUtf8("\xF0\x9F\x93\x82"), "#88C0D0" };  // 📂
}

// 统一的添加 / 编辑对话框：(type, name, path-or-url)
//
// 设计：
//   - 类型选择固定显示在最上面
//   - "Local" 模式：显示 Directory + Browse 按钮
//   - "SFTP" 模式：拆成 Host / Port / Username / Path 四个字段
//                  （与 RemoteDialog 的"连接信息"字段一致）
//   - "SMB"  模式：拆成 Host / Share / Path / Workgroup 四个字段
//   - 字段宽度统一 200 px，与 RemoteDialog 对齐，视觉风格一致
//   - 切换类型时只 show/hide 对应行；编辑已有快捷方式时按 URL 反解回填
//   - 对外暴露 path()：将多个字段拼成 sftp:// / smb:// URL，保持持久化
//     格式不变（向后兼容旧版本读取的代码）
class AddShortcutDialog : public QDialog {
public:
    AddShortcutDialog(QWidget* parent,
                      const QString& initialType = "local",
                      const QString& initialName = "",
                      const QString& initialPath = "")
        : QDialog(parent)
    {
        setWindowTitle(initialPath.isEmpty() ? T("Add shortcut") : T("Edit shortcut"));
        setMinimumWidth(460);

        auto* root = new QVBoxLayout(this);

        // ---- Type combo（顶部，独立一行） ----
        auto* typeRow = new QHBoxLayout();
        typeRow->setContentsMargins(0, 0, 0, 0);
        auto* typeLabel = new QLabel(T("Type:"));
        m_typeCombo = new QComboBox();
        m_typeCombo->addItem(T("Local folder"), "local");
        m_typeCombo->addItem("SFTP",            "sftp");
        m_typeCombo->addItem("SMB",             "smb");
        for (int i = 0; i < m_typeCombo->count(); ++i) {
            if (m_typeCombo->itemData(i).toString() == initialType) {
                m_typeCombo->setCurrentIndex(i);
                break;
            }
        }
        m_typeCombo->setMinimumWidth(kFieldWidth);
        m_typeCombo->setMaximumWidth(kFieldWidth);
        typeRow->addWidget(typeLabel);
        typeRow->addWidget(m_typeCombo);
        typeRow->addStretch();
        root->addLayout(typeRow);

        root->addSpacing(6);

        // ---- Name（所有类型共用） ----
        m_nameEdit = new QLineEdit(initialName);
        fixWidth(m_nameEdit);
        m_nameEdit->setPlaceholderText(T("optional, defaults to last path segment"));
        m_nameEdit->setToolTip(T(
            "快捷方式显示名（可选）。\n"
            "留空时会自动取路径的最后一段（如目录名 / 文件夹名）。"));

        // ---- Local: Directory + Browse ----
        m_localPathEdit = new QLineEdit();
        m_browseBtn = new QPushButton(T("Browse..."));
        auto* localPathRow = new QHBoxLayout();
        localPathRow->setContentsMargins(0, 0, 0, 0);
        localPathRow->setSpacing(4);
        m_browseBtn->setFixedWidth(72);
        localPathRow->addWidget(m_localPathEdit);
        localPathRow->addWidget(m_browseBtn);
        m_localPathRowWidget = new QWidget();
        m_localPathRowWidget->setLayout(localPathRow);
        fixWidth(m_localPathRowWidget);
        m_localPathEdit->setPlaceholderText(QDir::homePath());
        m_localPathEdit->setToolTip(T(
            "本地目录的绝对路径。\n"
            "示例：/Users/me/Projects、~/Downloads（会自动展开 ~）\n"
            "可点击右侧 \"浏览\" 按钮选择。"));

        // ---- SFTP fields ----
        m_hostEdit = new QLineEdit();
        fixWidth(m_hostEdit);
        m_hostEdit->setPlaceholderText(T("hostname or IP"));
        m_hostEdit->setToolTip(T(
            "SFTP 服务器主机名或 IP 地址。\n"
            "示例：example.com、192.168.1.10"));

        m_portSpin = new QSpinBox();
        m_portSpin->setRange(1, 65535);
        m_portSpin->setValue(22);
        fixWidth(m_portSpin);
        m_portSpin->setToolTip(T(
            "TCP 端口。\n"
            "SFTP 默认 22，SMB 默认 445。"));

        m_userEdit = new QLineEdit();
        fixWidth(m_userEdit);
        m_userEdit->setPlaceholderText(T("username"));
        m_userEdit->setToolTip(T(
            "登录用户名。\n"
            "SFTP：普通用户名（如 alice）"));

        m_pwdEdit = new QLineEdit();
        m_pwdEdit->setEchoMode(QLineEdit::Password);
        fixWidth(m_pwdEdit);
        m_pwdEdit->setPlaceholderText(T("password (leave empty if using a key)"));
        m_pwdEdit->setToolTip(T(
            "登录密码（可选）。\n"
            "• 留空表示走私钥 / 系统 keychain 登录。\n"
            "• 已填写时以本地混淆形式保存（非强加密），\n"
            "  请勿在共享设备保存敏感账号。"));

        m_remotePathEdit = new QLineEdit();
        fixWidth(m_remotePathEdit);
        m_remotePathEdit->setPlaceholderText("/data/work");
        m_remotePathEdit->setToolTip(T(
            "服务器上的目标路径（绝对路径，以 \"/\" 开头）。\n"
            "示例：/data/work、/home/alice/code"));

        // ---- SMB fields ----
        m_smbHostEdit = new QLineEdit();
        fixWidth(m_smbHostEdit);
        m_smbHostEdit->setPlaceholderText(T("hostname or IP"));
        m_smbHostEdit->setToolTip(T(
            "SMB 服务器主机名或 IP（也可填域名）。\n"
            "示例：fileserver、tencent.com"));

        m_smbShareEdit = new QLineEdit();
        fixWidth(m_smbShareEdit);
        m_smbShareEdit->setPlaceholderText(T("share name, e.g. Public"));
        m_smbShareEdit->setToolTip(T(
            "SMB 共享名（顶级共享）。\n"
            "示例：Public、tfs"));

        m_smbSubpathEdit = new QLineEdit();
        fixWidth(m_smbSubpathEdit);
        m_smbSubpathEdit->setPlaceholderText(T("optional sub-path inside the share"));
        m_smbSubpathEdit->setToolTip(T(
            "共享内的子路径（可选），分隔符使用 \"/\"。\n"
            "示例：文体协会/腾讯电影协会-影音博物馆"));

        m_smbUserEdit = new QLineEdit();
        fixWidth(m_smbUserEdit);
        m_smbUserEdit->setPlaceholderText(T("username (or DOMAIN\\\\user)"));
        m_smbUserEdit->setToolTip(T(
            "登录用户名（可选，留空则尝试匿名 / Guest）。\n"
            "• 普通：alice\n"
            "• 企业 AD：可写 alice，配合下方 Workgroup=TENCENT；\n"
            "  也可以直接 DOMAIN\\\\user 形式（如 TENCENT\\\\alice）。"));

        m_smbPwdEdit = new QLineEdit();
        m_smbPwdEdit->setEchoMode(QLineEdit::Password);
        fixWidth(m_smbPwdEdit);
        m_smbPwdEdit->setPlaceholderText(T("password (leave empty for guest)"));
        m_smbPwdEdit->setToolTip(T(
            "登录密码（可选）。\n"
            "• 留空表示尝试匿名 / Guest 访问。\n"
            "• 已填写时以本地混淆形式保存（非强加密），\n"
            "  请勿在共享设备保存敏感账号。"));

        m_smbWorkgroupEdit = new QLineEdit();
        fixWidth(m_smbWorkgroupEdit);
        m_smbWorkgroupEdit->setPlaceholderText(
            T("home/SOHO: leave empty; corporate AD: enter your domain (e.g. TENCENT)"));
        m_smbWorkgroupEdit->setToolTip(T(
            "Windows 工作组 / AD 域名（建议大写）。\n"
            "• 家用 / SOHO：留空（默认 WORKGROUP）\n"
            "• 企业 AD：填写域名，如 TENCENT"));

        // ---- 把字段塞进 QFormLayout，与 RemoteDialog 一致 ----
        m_form = new QFormLayout();
        m_form->setLabelAlignment(Qt::AlignLeft);
        m_form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_form->setRowWrapPolicy(QFormLayout::DontWrapRows);

        m_form->addRow(new QLabel(T("Name:")), m_nameEdit);
        m_form->addRow(new QLabel(T("Directory:")), m_localPathRowWidget);
        m_form->addRow(new QLabel(T("Host:")),     m_hostEdit);
        m_form->addRow(new QLabel(T("Port:")),     m_portSpin);
        m_form->addRow(new QLabel(T("Username:")), m_userEdit);
        m_form->addRow(new QLabel(T("Password:")), m_pwdEdit);
        m_form->addRow(new QLabel(T("Path:")),     m_remotePathEdit);
        m_form->addRow(new QLabel(T("Host:")),     m_smbHostEdit);
        m_form->addRow(new QLabel(T("Share:")),    m_smbShareEdit);
        m_form->addRow(new QLabel(T("Sub-path:")), m_smbSubpathEdit);
        m_form->addRow(new QLabel(T("Username:")), m_smbUserEdit);
        m_form->addRow(new QLabel(T("Password:")), m_smbPwdEdit);
        m_form->addRow(new QLabel(T("Workgroup:")),m_smbWorkgroupEdit);

        root->addLayout(m_form);

        m_hintLabel = new QLabel();
        m_hintLabel->setStyleSheet("color: #888; font-size: 11px;");
        m_hintLabel->setWordWrap(true);
        root->addWidget(m_hintLabel);

        // 反解 initialPath
        prefillFromPath(initialType, initialPath);

        connect(m_browseBtn, &QPushButton::clicked, this, [this]() {
            QString start = m_localPathEdit->text().trimmed();
            if (start.startsWith("~/")) start = QDir::homePath() + start.mid(1);
            if (start.isEmpty() || !QFileInfo(start).isDir()) start = QDir::homePath();
            QString dir = QFileDialog::getExistingDirectory(this, T("Select directory"), start);
            if (!dir.isEmpty()) m_localPathEdit->setText(dir);
        });

        connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { applyTypeUI(); });

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        root->addSpacing(8);
        root->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        applyTypeUI();
    }

    QString type() const { return m_typeCombo->currentData().toString(); }

    // 编辑模式下，外部用此把已有凭证回填进 dialog（密码与用户名不写入 path URL，
    // 因此与 path 分离）。在构造之后、exec() 之前调用即可。
    void setCredentials(const QString& username,
                        const QString& password,
                        const QString& workgroup) {
        m_userEdit   ->setText(username);
        m_pwdEdit    ->setText(password);
        m_smbUserEdit->setText(username);
        m_smbPwdEdit ->setText(password);
        m_smbWorkgroupEdit->setText(workgroup);
    }

    // 取出用户填写的凭证（按当前 type 区分输入框）
    QString username() const {
        const QString t = type();
        if (t == "sftp") return m_userEdit   ->text().trimmed();
        if (t == "smb")  return m_smbUserEdit->text().trimmed();
        return QString();
    }
    QString password() const {
        const QString t = type();
        if (t == "sftp") return m_pwdEdit   ->text();   // 密码不 trim，避免误删尾空格
        if (t == "smb")  return m_smbPwdEdit->text();
        return QString();
    }
    QString workgroup() const {
        return (type() == "smb") ? m_smbWorkgroupEdit->text().trimmed() : QString();
    }

    QString name() const {
        QString n = m_nameEdit->text().trimmed();
        if (!n.isEmpty()) return n;
        const QString p = path();
        if (type() == "local") {
            QString last = QFileInfo(p).fileName();
            return last.isEmpty() ? p : last;
        }
        // remote: 取 URL 的最后一段
        int slash = p.lastIndexOf('/');
        int proto = p.indexOf("://");
        if (slash > proto + 2) {
            QString last = p.mid(slash + 1);
            if (!last.isEmpty()) return last;
        }
        return p;
    }

    // 把字段拼成统一的 path 形式：
    //   local: 直接是绝对路径
    //   sftp : sftp://user@host:port/path
    //   smb  : smb://host/share[/sub] （workgroup 暂未编入 URL，保留供后续）
    QString path() const {
        const QString t = type();
        if (t == "local") {
            QString p = m_localPathEdit->text().trimmed();
            if (p.startsWith("~/")) p = QDir::homePath() + p.mid(1);
            return p;
        }
        if (t == "sftp") {
            QString user = m_userEdit->text().trimmed();
            QString host = m_hostEdit->text().trimmed();
            int     port = m_portSpin->value();
            QString p    = m_remotePathEdit->text().trimmed();
            if (host.isEmpty()) return QString();
            if (!p.startsWith('/')) p.prepend('/');
            QString url = "sftp://";
            if (!user.isEmpty()) url += user + "@";
            url += host;
            if (port != 22) url += ":" + QString::number(port);
            url += p;
            return url;
        }
        // smb
        QString host  = m_smbHostEdit->text().trimmed();
        QString share = sanitizeShare(m_smbShareEdit->text());
        QString sub   = sanitizeShare(m_smbSubpathEdit->text());
        if (host.isEmpty() || share.isEmpty()) return QString();
        QString url = "smb://" + host + "/" + share;
        if (!sub.isEmpty()) url += "/" + sub;
        return url;
    }

private:
    static constexpr int kFieldWidth = 200;

    static void fixWidth(QWidget* w) {
        w->setMinimumWidth(kFieldWidth);
        w->setMaximumWidth(kFieldWidth);
    }

    // 清理用户误填的反斜杠 / 空白段（与 RemoteDialog::onConnectClicked 同策略）
    static QString sanitizeShare(const QString& raw) {
        QString s = raw.trimmed();
        s.replace('\\', '/');
        while (s.startsWith('/')) s.remove(0, 1);
        while (s.endsWith('/'))   s.chop(1);
        // 折叠多余 //
        while (s.contains("//")) s.replace("//", "/");
        return s;
    }

    void setRowVisible(QWidget* field, bool visible) {
        if (!field) return;
        field->setVisible(visible);
        if (auto* lbl = m_form->labelForField(field)) lbl->setVisible(visible);
    }

    void applyTypeUI() {
        const QString t = type();
        const bool local = (t == "local");
        const bool sftp  = (t == "sftp");
        const bool smb   = (t == "smb");

        setRowVisible(m_localPathRowWidget, local);

        setRowVisible(m_hostEdit,        sftp);
        setRowVisible(m_portSpin,        sftp);
        setRowVisible(m_userEdit,        sftp);
        setRowVisible(m_pwdEdit,         sftp);
        setRowVisible(m_remotePathEdit,  sftp);

        setRowVisible(m_smbHostEdit,      smb);
        setRowVisible(m_smbShareEdit,     smb);
        setRowVisible(m_smbSubpathEdit,   smb);
        setRowVisible(m_smbUserEdit,      smb);
        setRowVisible(m_smbPwdEdit,       smb);
        setRowVisible(m_smbWorkgroupEdit, smb);

        if (sftp && m_portSpin->value() == 445) m_portSpin->setValue(22);

        if (local) {
            m_hintLabel->setText(T("Pick any local folder you want quick access to."));
        } else if (sftp) {
            m_hintLabel->setText(T(
                "Saves as: sftp://user@host[:port]/path\n"
                "Activate the connection first via Remote → Connect…"));
        } else {
            m_hintLabel->setText(T(
                "Saves as: smb://host/share[/sub-path]\n"
                "Activate the connection first via Remote → Connect…"));
        }
    }

    // 把已有 URL 反解回字段（编辑模式）
    void prefillFromPath(const QString& type, const QString& path) {
        if (path.isEmpty()) return;
        if (type == "local") {
            m_localPathEdit->setText(path);
            return;
        }
        if (type == "sftp" && path.startsWith("sftp://", Qt::CaseInsensitive)) {
            QString rest = path.mid(QString("sftp://").size());
            QString userHost, p;
            int slash = rest.indexOf('/');
            if (slash >= 0) { userHost = rest.left(slash); p = rest.mid(slash); }
            else            { userHost = rest;             p = "/";             }

            QString user, hostPort = userHost;
            int at = userHost.indexOf('@');
            if (at >= 0) {
                user = userHost.left(at);
                hostPort = userHost.mid(at + 1);
            }
            QString host = hostPort;
            int port = 22;
            int colon = hostPort.lastIndexOf(':');
            if (colon >= 0) {
                host = hostPort.left(colon);
                bool ok = false;
                int v = hostPort.mid(colon + 1).toInt(&ok);
                if (ok) port = v;
            }
            m_userEdit->setText(user);
            m_hostEdit->setText(host);
            m_portSpin->setValue(port);
            m_remotePathEdit->setText(p);
            return;
        }
        if (type == "smb" && path.startsWith("smb://", Qt::CaseInsensitive)) {
            QString rest = path.mid(QString("smb://").size());
            int slash1 = rest.indexOf('/');
            QString host = (slash1 >= 0) ? rest.left(slash1) : rest;
            QString tail = (slash1 >= 0) ? rest.mid(slash1 + 1) : QString();
            int slash2 = tail.indexOf('/');
            QString share = (slash2 >= 0) ? tail.left(slash2) : tail;
            QString sub   = (slash2 >= 0) ? tail.mid(slash2 + 1) : QString();
            m_smbHostEdit->setText(host);
            m_smbShareEdit->setText(share);
            m_smbSubpathEdit->setText(sub);
            return;
        }
        // 未识别协议，保守地放回 local 输入框，至少不丢数据
        m_localPathEdit->setText(path);
    }

    QFormLayout* m_form;
    QComboBox*   m_typeCombo;
    QLineEdit*   m_nameEdit;

    // local
    QWidget*     m_localPathRowWidget;
    QLineEdit*   m_localPathEdit;
    QPushButton* m_browseBtn;

    // sftp
    QLineEdit*   m_hostEdit;
    QSpinBox*    m_portSpin;
    QLineEdit*   m_userEdit;
    QLineEdit*   m_pwdEdit;
    QLineEdit*   m_remotePathEdit;

    // smb
    QLineEdit*   m_smbHostEdit;
    QLineEdit*   m_smbShareEdit;
    QLineEdit*   m_smbSubpathEdit;
    QLineEdit*   m_smbUserEdit;
    QLineEdit*   m_smbPwdEdit;
    QLineEdit*   m_smbWorkgroupEdit;

    QLabel*      m_hintLabel;
};

// 圆形小图标按钮的样式工厂（"+" / "📁+" 都用同一组样式）
QString makeRoundIconBtnStyle() {
    return QStringLiteral(
        "QPushButton {"
        "  background: transparent;"
        "  color: #88C0D0;"
        "  border: 1px solid #4C566A;"
        "  border-radius: 11px;"
        "  padding: 0;"
        "  margin: 0;"
        "  text-align: center;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(136, 192, 208, 0.20);"
        "  border-color: #88C0D0;"
        "  color: #ECEFF4;"
        "}"
        "QPushButton:pressed {"
        "  background: #5E81AC;"
        "  color: #ECEFF4;"
        "}");
}

// 目录头按钮样式（与 shortcut 区分）
QString makeFolderBtnStyle() {
    return QStringLiteral(
        "QPushButton {"
        "  text-align: left;"
        "  padding: 5px 10px;"
        "  background: #434C5E;"        // Nord2，比 shortcut 略亮
        "  color: #ECEFF4;"
        "  border: 1px solid #4C566A;"
        "  border-radius: 4px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background: #4C566A;"
        "  color: #88C0D0;"
        "  border-color: #88C0D0;"
        "}"
        "QPushButton:pressed {"
        "  background: #5E81AC;"
        "  color: #ECEFF4;"
        "}");
}

QString makeShortcutBtnStyle() {   // 已废弃（按 type 动态生成）；保留以备后用
    return QStringLiteral(
        "QPushButton {"
        "  text-align: left;"
        "  padding: 5px 10px;"
        "  background: #3B4252;"
        "  color: #E5E9F0;"
        "  border: 1px solid #434C5E;"
        "  border-radius: 4px;"
        "}");
}

// 按 type 校验 path/URL 合法性
//   local : 必须是已存在的本地目录
//   sftp  : 必须以 "sftp://" 开头
//   smb   : 必须以 "smb://" 开头
// 返回值 true=合法，false=非法（同时把错误提示放到 errOut）
bool validatePath(const QString& type, const QString& path, QString* errOut) {
    if (path.isEmpty()) {
        if (errOut) *errOut = T("Please enter a path or URL.");
        return false;
    }
    if (type == "local") {
        if (!QFileInfo(path).isDir()) {
            if (errOut) *errOut = T("Please choose an existing directory.");
            return false;
        }
        return true;
    }
    if (type == "sftp") {
        if (!path.startsWith("sftp://", Qt::CaseInsensitive)) {
            if (errOut) *errOut = T("SFTP URL must start with sftp://");
            return false;
        }
        return true;
    }
    if (type == "smb") {
        if (!path.startsWith("smb://", Qt::CaseInsensitive)) {
            if (errOut) *errOut = T("SMB URL must start with smb://");
            return false;
        }
        return true;
    }
    if (errOut) *errOut = T("Unknown shortcut type.");
    return false;
}

// 给 QMenu 应用统一的 Nord 暗色不透明样式。
// 不设此样式时，macOS 暗色主题下默认 vibrancy 会让菜单半透明、文字与桌面
// 互相干扰看不清；这里显式覆盖背景色 + 边框 + hover/separator，使其完全
// 不透明，并与 ShortcutBar 视觉风格一致。
static void applyMenuStyle(QMenu* m) {
    if (!m) return;
    // 关键：关闭 Mac 原生 vibrancy，强制 Qt 自绘背景，否则 stylesheet 的
    // background 会被半透明窗体合成穿透。
    m->setAttribute(Qt::WA_TranslucentBackground, false);
    m->setStyleSheet(
        "QMenu {"
        "  background: #2E3440;"          // Nord Polar Night
        "  color: #E5E9F0;"
        "  border: 1px solid #4C566A;"
        "  padding: 4px 0px;"
        "}"
        "QMenu::item {"
        "  background: transparent;"
        "  padding: 6px 18px;"
        "}"
        "QMenu::item:selected {"
        "  background: #5E81AC;"
        "  color: #ECEFF4;"
        "}"
        "QMenu::item:disabled {"
        "  color: #6C7280;"
        "}"
        "QMenu::separator {"
        "  height: 1px;"
        "  background: #434C5E;"
        "  margin: 4px 8px;"
        "}"
    );
}

} // namespace

// =============================================================================

ShortcutBar::ShortcutBar(QWidget* parent)
    : QWidget(parent)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    // ---- Header：标题 + 新建目录按钮 + 新建快捷方式按钮 ----
    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(4);

    m_title = new QLabel(T("SHORTCUTS"));
    m_title->setStyleSheet(
        "color: #81A1C1;"
        "font-size: 10px;"
        "font-weight: bold;"
        "letter-spacing: 1px;");

    // 新建"目录"按钮：用文件夹 emoji + "+"，比 "+" 更直观
    m_addFolderBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x93\x81"));   // 📁
    m_addFolderBtn->setFixedSize(22, 22);
    m_addFolderBtn->setCursor(Qt::PointingHandCursor);
    m_addFolderBtn->setFocusPolicy(Qt::NoFocus);
    m_addFolderBtn->setFlat(true);
    {
        QFont f = m_addFolderBtn->font();
        f.setPointSize(10);
        m_addFolderBtn->setFont(f);
    }
    m_addFolderBtn->setStyleSheet(makeRoundIconBtnStyle());

    // 新建"快捷方式"按钮（保留原 "+"）
    m_addShortcutBtn = new QPushButton("+");
    m_addShortcutBtn->setFixedSize(22, 22);
    m_addShortcutBtn->setCursor(Qt::PointingHandCursor);
    m_addShortcutBtn->setFocusPolicy(Qt::NoFocus);
    m_addShortcutBtn->setFlat(true);
    {
        QFont f = m_addShortcutBtn->font();
        f.setPointSize(14);
        f.setBold(true);
        m_addShortcutBtn->setFont(f);
    }
    m_addShortcutBtn->setStyleSheet(makeRoundIconBtnStyle());

    headerRow->addWidget(m_title, 1);
    headerRow->addWidget(m_addFolderBtn);
    headerRow->addWidget(m_addShortcutBtn);

    rootLayout->addLayout(headerRow);

    // ---- 列表（带滚动） ----
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: transparent; }");

    auto* scrollContent = new QWidget();
    scrollContent->setStyleSheet("background: transparent;");
    m_listLayout = new QVBoxLayout(scrollContent);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(2);
    m_listLayout->addStretch();

    scroll->setWidget(scrollContent);
    rootLayout->addWidget(scroll);

    connect(m_addShortcutBtn, &QPushButton::clicked, this, &ShortcutBar::onAddShortcutClicked);
    connect(m_addFolderBtn,   &QPushButton::clicked, this, &ShortcutBar::onAddFolderClicked);

    loadFromSettings();
    retranslate();   // 设置 tooltip / 标题（loadFromSettings 之后再做以拿到最新 i18n）

    connect(&I18n::instance(), &I18n::changed, this, &ShortcutBar::retranslate);
}

void ShortcutBar::retranslate() {
    if (m_title)          m_title->setText(T("SHORTCUTS"));
    if (m_addShortcutBtn) m_addShortcutBtn->setToolTip(T("Add shortcut"));
    if (m_addFolderBtn)   m_addFolderBtn->setToolTip(T("Add folder"));
}

// =============================================================================
// 渲染
// =============================================================================

QPushButton* ShortcutBar::makeShortcutButton(const ShortcutItem& item, int indent,
                                             const QVector<int>& path) {
    QString prefix;
    for (int i = 0; i < indent; ++i) prefix += QStringLiteral("    ");

    const TypeStyle ts = styleFor(item.type);
    // 三种类型统一：prefix + emoji + name，保证 indent 文本起点对齐
    QString label = prefix + ts.iconUtf8 + " " + item.name;
    auto* btn = new QPushButton(label);
    btn->setToolTip(item.path);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    btn->setFocusPolicy(Qt::NoFocus);

    // 选中态判断：m_selectedPath（QVector<int>）与当前 path 完全相等
    const bool selected = (m_selectedPath == path);

    if (selected) {
        btn->setStyleSheet(QString(
            "QPushButton {"
            "  text-align: left;"
            "  padding: 5px 10px;"
            "  background: #5E81AC;"
            "  color: #ECEFF4;"
            "  border: 1px solid #5E81AC;"
            "  border-left: 3px solid %1;"
            "  border-radius: 4px;"
            "}"
            "QPushButton:hover {"
            "  background: #6A8FBF;"
            "  border-color: #6A8FBF;"
            "  border-left: 3px solid %1;"
            "}"
            "QPushButton:pressed {"
            "  background: #4C6E96;"
            "  border-color: #4C6E96;"
            "  border-left: 3px solid %1;"
            "}").arg(ts.accent));
    } else {
        btn->setStyleSheet(QString(
            "QPushButton {"
            "  text-align: left;"
            "  padding: 5px 10px;"
            "  background: #3B4252;"
            "  color: #E5E9F0;"
            "  border: 1px solid #434C5E;"
            "  border-radius: 4px;"
            "}"
            "QPushButton:hover {"
            "  background: #434C5E;"
            "  color: %1;"
            "  border-color: %1;"
            "}"
            "QPushButton:pressed {"
            "  background: #5E81AC;"
            "  color: #ECEFF4;"
            "  border-color: #5E81AC;"
            "}").arg(ts.accent));
    }

    QString sPath = item.path;
    ShortcutItem snapshot = item;
    QVector<int> nodePath = path;
    connect(btn, &QPushButton::clicked, this, [this, sPath, snapshot, nodePath]() {
        m_selectedPath = nodePath;
        rebuildButtons();
        emit shortcutActivated(sPath);
        emit shortcutActivatedFull(snapshot);
    });

    connect(btn, &QPushButton::customContextMenuRequested, this,
            [this, btn, nodePath](const QPoint& pos) {
        const bool needRebuild = (m_selectedPath != nodePath);
        auto buildMenu = [this, nodePath](QWidget* parent) {
            auto* m = new QMenu(parent);
            applyMenuStyle(m);
            m->addAction(T("Edit..."), this, [this, nodePath]() {
                editAtPath(nodePath);
            });
            // shortcut 自身不能装东西，但允许就近添加：在它所在 folder（path 去掉末位）下添加
            QVector<int> parentFolder = nodePath;
            if (!parentFolder.isEmpty()) parentFolder.removeLast();
            m->addAction(T("Add shortcut here..."), this, [this, parentFolder]() {
                addShortcutUnder(parentFolder);
            });
            m->addAction(T("Add shortcut folder..."), this, [this, parentFolder]() {
                addFolderUnder(parentFolder);
            });
            m->addSeparator();
            m->addAction(T("Remove"), this, [this, nodePath]() {
                removeAtPath(nodePath);
            });
            return m;
        };
        if (needRebuild) {
            m_selectedPath = nodePath;
            rebuildButtons();
            QMenu* menu = buildMenu(this);
            menu->exec(QCursor::pos());
            menu->deleteLater();
            return;
        }
        QMenu* menu = buildMenu(btn);
        menu->exec(btn->mapToGlobal(pos));
        menu->deleteLater();
    });

    return btn;
}

QPushButton* ShortcutBar::makeFolderButton(const ShortcutItem& item, int indent,
                                           const QVector<int>& path) {
    QString prefix;
    for (int i = 0; i < indent; ++i) prefix += QStringLiteral("    ");

    const QString arrow = item.expanded
        ? QString::fromUtf8("\xE2\x96\xBC")    // ▼
        : QString::fromUtf8("\xE2\x96\xB6");   // ▶
    const QString folderIcon = QString::fromUtf8("\xF0\x9F\x93\x81"); // 📁
    auto* btn = new QPushButton(prefix + arrow + " " + folderIcon + " " + item.name +
                                QString("  (%1)").arg(item.children.size()));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    btn->setFocusPolicy(Qt::NoFocus);

    // folder 自身高亮 OR 作为"选中的 shortcut 所在 folder"高亮（就近反馈"新增会落到这里"）
    const QVector<int> targetFolder = selectedTargetFolderPath();
    const bool selected = (m_selectedPath == path) || (targetFolder == path);

    if (selected) {
        btn->setStyleSheet(
            "QPushButton {"
            "  text-align: left;"
            "  padding: 6px 10px;"
            "  background: #5E81AC;"
            "  color: #ECEFF4;"
            "  border: 1px solid #5E81AC;"
            "  border-left: 3px solid #EBCB8B;"
            "  border-radius: 4px;"
            "  font-weight: bold;"
            "}"
            "QPushButton:hover {"
            "  background: #6A8FBF;"
            "  border-color: #6A8FBF;"
            "  border-left: 3px solid #EBCB8B;"
            "}"
        );
    } else {
        btn->setStyleSheet(makeFolderBtnStyle());
    }

    QVector<int> nodePath = path;
    connect(btn, &QPushButton::clicked, this, [this, nodePath]() {
        // 单击：选中 folder + toggle expanded
        m_selectedPath = nodePath;
        toggleExpandedAtPath(nodePath);   // 内部 saveToSettings + rebuildButtons
    });

    connect(btn, &QPushButton::customContextMenuRequested, this,
            [this, btn, nodePath](const QPoint& pos) {
        const bool needRebuild = (m_selectedPath != nodePath);
        auto buildMenu = [this, nodePath](QWidget* parent) {
            auto* m = new QMenu(parent);
            applyMenuStyle(m);
            m->addAction(T("Add shortcut here..."), this, [this, nodePath]() {
                addShortcutUnder(nodePath);
            });
            m->addAction(T("Add shortcut folder..."), this, [this, nodePath]() {
                addFolderUnder(nodePath);   // 在该 folder 下添加子快捷目录（嵌套）
            });
            m->addAction(T("Rename folder..."), this, [this, nodePath]() {
                editAtPath(nodePath);
            });
            m->addSeparator();
            m->addAction(T("Remove folder"), this, [this, nodePath]() {
                removeAtPath(nodePath);
            });
            return m;
        };
        if (needRebuild) {
            m_selectedPath = nodePath;
            rebuildButtons();
            QMenu* menu = buildMenu(this);
            menu->exec(QCursor::pos());
            menu->deleteLater();
            return;
        }
        QMenu* menu = buildMenu(btn);
        menu->exec(btn->mapToGlobal(pos));
        menu->deleteLater();
    });

    return btn;
}

void ShortcutBar::renderItems(const QVector<ShortcutItem>& items,
                              const QVector<int>& parentPath, int indent) {
    for (int i = 0; i < items.size(); ++i) {
        QVector<int> path = parentPath;
        path.append(i);
        const auto& it = items[i];
        if (it.isFolder) {
            m_listLayout->addWidget(makeFolderButton(it, indent, path));
            if (it.expanded) {
                renderItems(it.children, path, indent + 1);
            }
        } else {
            m_listLayout->addWidget(makeShortcutButton(it, indent, path));
        }
    }
}

void ShortcutBar::rebuildButtons() {
    while (m_listLayout->count() > 0) {
        QLayoutItem* item = m_listLayout->takeAt(0);
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    renderItems(m_items, /*parentPath*/{}, /*indent*/0);
    m_listLayout->addStretch();
}

// =============================================================================
// 路径辅助
// =============================================================================

ShortcutItem* ShortcutBar::itemAt(const QVector<int>& path) {
    if (path.isEmpty()) return nullptr;
    QVector<ShortcutItem>* level = &m_items;
    ShortcutItem* node = nullptr;
    for (int depth = 0; depth < path.size(); ++depth) {
        const int idx = path[depth];
        if (idx < 0 || idx >= level->size()) return nullptr;
        node = &(*level)[idx];
        if (depth == path.size() - 1) return node;
        if (!node->isFolder) return nullptr;   // 中间路径必须是 folder
        level = &node->children;
    }
    return node;
}

const ShortcutItem* ShortcutBar::itemAt(const QVector<int>& path) const {
    return const_cast<ShortcutBar*>(this)->itemAt(path);
}

QVector<int> ShortcutBar::selectedTargetFolderPath() const {
    if (m_selectedPath.isEmpty()) return {};
    const ShortcutItem* sel = itemAt(m_selectedPath);
    if (!sel) return {};
    if (sel->isFolder) return m_selectedPath;
    // 选中的是 shortcut → 返回它所在 folder 路径（去掉末位 index）
    QVector<int> p = m_selectedPath;
    p.removeLast();
    return p;
}

// =============================================================================
// 顶层操作
// =============================================================================

void ShortcutBar::addTopLevel(const ShortcutItem& item) {
    m_items.append(item);
    saveToSettings();
    rebuildButtons();
}

// =============================================================================
// 通用增删改（按 path 定位）
// =============================================================================

void ShortcutBar::removeAtPath(const QVector<int>& path) {
    if (path.isEmpty()) return;
    // 先确认存在
    const ShortcutItem* node = itemAt(path);
    if (!node) return;

    const bool zh = (I18n::instance().current() == I18n::Lang::Zh);
    QString msg;
    if (node->isFolder) {
        // 递归统计后代叶子数（仅作提示用，不严格）
        std::function<int(const ShortcutItem&)> countLeaves = [&](const ShortcutItem& it) -> int {
            if (!it.isFolder) return 1;
            int n = 0;
            for (const auto& c : it.children) n += countLeaves(c);
            return n;
        };
        const int leaves = countLeaves(*node);
        msg = (zh
            ? QString("是否移除快捷目录 \"%1\"？该目录及其下 %2 个条目将一并删除。")
            : QString("Remove shortcut folder \"%1\"? It and %2 entries inside will be removed."))
              .arg(node->name).arg(leaves);
    } else {
        msg = (zh ? QString("是否移除快捷方式 \"%1\"？")
                  : QString("Remove shortcut \"%1\"?")).arg(node->name);
    }
    auto ans = QMessageBox::question(this,
        node->isFolder ? T("Remove folder") : T("Remove shortcut"), msg);
    if (ans != QMessageBox::Yes) return;

    // 找到父级 vector 然后 removeAt
    QVector<int> parentPath = path;
    parentPath.removeLast();
    QVector<ShortcutItem>* parentVec = nullptr;
    if (parentPath.isEmpty()) {
        parentVec = &m_items;
    } else {
        ShortcutItem* parentNode = itemAt(parentPath);
        if (!parentNode || !parentNode->isFolder) return;
        parentVec = &parentNode->children;
    }
    const int idx = path.last();
    if (idx < 0 || idx >= parentVec->size()) return;
    parentVec->removeAt(idx);

    // 清理选中态：若选中条目是被删的或其后代，则清空；否则若被删条目在前面 → 同级索引前移
    if (!m_selectedPath.isEmpty()) {
        // 判断 m_selectedPath 是否以 path 为前缀（含相等）
        bool isAncestorOrSelf = (m_selectedPath.size() >= path.size());
        for (int i = 0; isAncestorOrSelf && i < path.size(); ++i) {
            if (m_selectedPath[i] != path[i]) isAncestorOrSelf = false;
        }
        if (isAncestorOrSelf) {
            m_selectedPath.clear();
        } else if (m_selectedPath.size() >= path.size()
                   && std::equal(parentPath.begin(), parentPath.end(), m_selectedPath.begin())
                   && m_selectedPath[parentPath.size()] > idx) {
            // 同父级，被删项在前 → 索引前移
            --m_selectedPath[parentPath.size()];
        }
    }

    saveToSettings();
    rebuildButtons();
}

void ShortcutBar::editAtPath(const QVector<int>& path) {
    ShortcutItem* node = itemAt(path);
    if (!node) return;

    if (node->isFolder) {
        bool ok = false;
        QString name = QInputDialog::getText(this, T("Rename folder"),
                                             T("Folder name:"), QLineEdit::Normal,
                                             node->name, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        node->name = name;
        saveToSettings();
        rebuildButtons();
    } else {
        AddShortcutDialog dlg(this, node->type, node->name, node->path);
        dlg.setCredentials(node->username, node->password, node->workgroup);
        if (dlg.exec() != QDialog::Accepted) return;
        QString err;
        if (!validatePath(dlg.type(), dlg.path(), &err)) {
            QMessageBox::warning(this, T("Invalid path"), err);
            return;
        }
        node->type      = dlg.type();
        node->name      = dlg.name();
        node->path      = dlg.path();
        node->username  = dlg.username();
        node->password  = dlg.password();
        node->workgroup = dlg.workgroup();
        saveToSettings();
        rebuildButtons();
    }
}

void ShortcutBar::addShortcutUnder(const QVector<int>& folderPath) {
    // folderPath 为空 → 顶层；否则必须指向一个快捷目录
    QVector<ShortcutItem>* level = nullptr;
    ShortcutItem* folderNode = nullptr;
    if (folderPath.isEmpty()) {
        level = &m_items;
    } else {
        folderNode = itemAt(folderPath);
        if (!folderNode || !folderNode->isFolder) return;
        level = &folderNode->children;
    }
    AddShortcutDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    QString err;
    if (!validatePath(dlg.type(), dlg.path(), &err)) {
        QMessageBox::warning(this, T("Invalid path"), err);
        return;
    }
    ShortcutItem child;
    child.type      = dlg.type();
    child.name      = dlg.name();
    child.path      = dlg.path();
    child.username  = dlg.username();
    child.password  = dlg.password();
    child.workgroup = dlg.workgroup();
    child.isFolder  = false;
    level->append(child);
    if (folderNode) folderNode->expanded = true;
    saveToSettings();
    rebuildButtons();
}

void ShortcutBar::addFolderUnder(const QVector<int>& folderPath) {
    QVector<ShortcutItem>* level = nullptr;
    ShortcutItem* folderNode = nullptr;
    if (folderPath.isEmpty()) {
        level = &m_items;
    } else {
        folderNode = itemAt(folderPath);
        if (!folderNode || !folderNode->isFolder) return;
        level = &folderNode->children;
    }
    bool ok = false;
    const bool zh = (I18n::instance().current() == I18n::Lang::Zh);
    QString name = QInputDialog::getText(this, T("Add folder"),
                                         T("Folder name:"), QLineEdit::Normal,
                                         zh ? "新快捷目录" : "New shortcut folder",
                                         &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    ShortcutItem item;
    item.name = name;
    item.isFolder = true;
    item.expanded = true;
    level->append(item);
    if (folderNode) folderNode->expanded = true;
    saveToSettings();
    rebuildButtons();
}

void ShortcutBar::toggleExpandedAtPath(const QVector<int>& folderPath) {
    ShortcutItem* node = itemAt(folderPath);
    if (!node || !node->isFolder) return;
    node->expanded = !node->expanded;
    saveToSettings();
    rebuildButtons();
}

// =============================================================================
// "+" 按钮
// =============================================================================

void ShortcutBar::onAddShortcutClicked() {
    // 智能落点：选中态指向某个 folder（含选中其子 shortcut 时回退到所在 folder）
    QVector<int> target = selectedTargetFolderPath();
    addShortcutUnder(target);
}

void ShortcutBar::onAddFolderClicked() {
    // 智能落点：与新增 shortcut 一致——可在选中 folder 下嵌套创建子快捷目录
    QVector<int> target = selectedTargetFolderPath();
    addFolderUnder(target);
}

// =============================================================================
// 持久化
// =============================================================================

namespace {
constexpr auto kSettingsOrg     = "WorkBuddy";
constexpr auto kSettingsApp     = "ZoeFileManager";
constexpr auto kLegacyAppKey    = "DualPaneFileManager";

// 密码混淆：XOR + Base64，仅防止 INI 文件被肉眼直读，**不是强加密**。
// 若将来要升级到 Keychain / DPAPI，这两个函数可以保留接口、内部替换实现。
constexpr quint8 kObfuscateKey[] = {0x5A, 0x71, 0x3F, 0x86, 0x12, 0xC4, 0x9E, 0x47};

QString obfuscate(const QString& plain) {
    if (plain.isEmpty()) return QString();
    QByteArray bytes = plain.toUtf8();
    for (int i = 0; i < bytes.size(); ++i) {
        bytes[i] = bytes[i] ^ kObfuscateKey[i % sizeof(kObfuscateKey)];
    }
    return QString::fromLatin1(bytes.toBase64());
}

QString deobfuscate(const QString& obf) {
    if (obf.isEmpty()) return QString();
    QByteArray bytes = QByteArray::fromBase64(obf.toLatin1());
    for (int i = 0; i < bytes.size(); ++i) {
        bytes[i] = bytes[i] ^ kObfuscateKey[i % sizeof(kObfuscateKey)];
    }
    return QString::fromUtf8(bytes);
}
} // namespace

// 旧版本数据无 "type" 字段时，按 path 前缀推断
static QString inferTypeFromPath(const QString& path) {
    if (path.startsWith("sftp://", Qt::CaseInsensitive)) return "sftp";
    if (path.startsWith("smb://",  Qt::CaseInsensitive)) return "smb";
    return "local";
}

// 递归读取一个 QSettings 数组（已 beginReadArray）→ QVector<ShortcutItem>
// 调用约定：函数内部用 setArrayIndex 切到第 i 项；不负责 beginReadArray /
// endArray 本身——那个由调用者负责（顶层用 "shortcuts"，子层用 "children"）。
static QVector<ShortcutItem> readArrayRecursive(QSettings& s, int size) {
    QVector<ShortcutItem> items;
    items.reserve(size);
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        ShortcutItem item;
        item.name     = s.value("name").toString();
        item.path     = s.value("path").toString();
        item.isFolder = s.value("isFolder", false).toBool();
        item.expanded = s.value("expanded", true).toBool();
        item.type     = s.value("type", inferTypeFromPath(item.path)).toString();
        item.username = s.value("username").toString();
        item.password = deobfuscate(s.value("password_obf").toString());
        item.workgroup= s.value("workgroup").toString();

        if (item.isFolder) {
            const int childCount = s.beginReadArray("children");
            item.children = readArrayRecursive(s, childCount);
            s.endArray();
            if (!item.name.isEmpty()) items.append(item);
        } else {
            if (!item.name.isEmpty() && !item.path.isEmpty()) {
                items.append(item);
            }
        }
    }
    return items;
}

// 递归写一个 QVector<ShortcutItem> 到当前 QSettings 数组（已 beginWriteArray）。
// 与 readArrayRecursive 对称：begin/end 由调用者负责。
static void writeArrayRecursive(QSettings& s, const QVector<ShortcutItem>& items) {
    for (int i = 0; i < items.size(); ++i) {
        s.setArrayIndex(i);
        const auto& it = items[i];
        s.setValue("name", it.name);
        s.setValue("path", it.path);
        s.setValue("type", it.type);
        s.setValue("isFolder", it.isFolder);
        s.setValue("expanded", it.expanded);
        if (!it.isFolder) {
            // 凭证仅在快捷方式（叶子）上才有意义；密码混淆后写盘。
            s.setValue("username",     it.username);
            s.setValue("password_obf", obfuscate(it.password));
            s.setValue("workgroup",    it.workgroup);
        } else {
            // folder 写 children 子数组（即使为空也写，便于 reload 时清晰）
            s.beginWriteArray("children", it.children.size());
            writeArrayRecursive(s, it.children);
            s.endArray();
        }
    }
}

void ShortcutBar::loadFromSettings() {
    QSettings s(kSettingsOrg, kSettingsApp);
    int size = s.beginReadArray("shortcuts");

    // First-run migration: 旧应用名 → 新应用名
    if (size == 0) {
        s.endArray();
        QSettings legacy(kSettingsOrg, kLegacyAppKey);
        int legacySize = legacy.beginReadArray("shortcuts");
        if (legacySize > 0) {
            QVector<ShortcutItem> migrated;
            for (int i = 0; i < legacySize; ++i) {
                legacy.setArrayIndex(i);
                ShortcutItem item;
                item.name = legacy.value("name").toString();
                item.path = legacy.value("path").toString();
                item.isFolder = false;
                item.type = "local";   // 老数据全是本地路径
                if (!item.name.isEmpty() && !item.path.isEmpty()) {
                    migrated.append(item);
                }
            }
            legacy.endArray();
            s.beginWriteArray("shortcuts", migrated.size());
            writeArrayRecursive(s, migrated);
            s.endArray();
        } else {
            legacy.endArray();
        }
        size = s.beginReadArray("shortcuts");
    }

    m_items = readArrayRecursive(s, size);
    s.endArray();
    rebuildButtons();
}

void ShortcutBar::saveToSettings() const {
    QSettings s(kSettingsOrg, kSettingsApp);
    s.remove("shortcuts");
    s.beginWriteArray("shortcuts", m_items.size());
    writeArrayRecursive(s, m_items);
    s.endArray();
}
