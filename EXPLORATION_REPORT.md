# ZoeFileManager 代码库探索报告

## 项目概览
- **位置**: `/Users/chairou/code/cpp/zoefilemanager`
- **语言**: C++/Qt
- **主要功能**: 双面板文件管理器，支持本地、SFTP 和 SMB 远程连接

---

## 1. 快捷管理器 (ShortcutBar) 实现

### 文件位置
- **头文件**: `/Users/chairou/code/cpp/zoefilemanager/src/ShortcutBar.h` (120 行)
- **实现**: `/Users/chairou/code/cpp/zoefilemanager/src/ShortcutBar.cpp` (1321 行)

### 核心数据结构：ShortcutItem

```cpp
struct ShortcutItem {
    QString name;
    QString type = "local";           // "local" / "sftp" / "smb"
    QString path;                     // 本地路径或远程 URL
    
    // 远程凭证（仅 sftp/smb 使用）
    QString username;                 
    QString password;                 // Base64 简单混淆，非强加密
    QString workgroup;                // SMB 专用（AD 域名）
    
    bool    isFolder = false;         // false=叶子，true=容器
    bool    expanded = true;          // 仅 folder 时有效
    QVector<ShortcutItem> children;   // 递归嵌套，支持任意深度
};
```

### 概念区分

1. **快捷目录** (isFolder=true)
   - 容器条目，用于组织快捷方式
   - 可嵌套快捷目录
   - 可同时装本地/SFTP/SMB 任何叶子条目
   - 图标：📁 emoji

2. **本地快捷方式** (type="local", isFolder=false)
   - 叶子条目，指向真实系统目录
   - 绝对路径
   - 不能嵌套
   - 图标：📂 emoji + 青色 (#88C0D0)

3. **远程快捷方式** (type="sftp"/"smb", isFolder=false)
   - 叶子条目，指向远程 URL
   - SFTP 图标：🌐 + 绿色 (#A3BE8C)
   - SMB 图标：🖥 + 紫色 (#B48EAD)

### 关键类定义

```cpp
class ShortcutBar : public QWidget {
public:
    explicit ShortcutBar(QWidget* parent = nullptr);
    
    void loadFromSettings();
    void saveToSettings() const;
    
signals:
    void shortcutActivated(const QString& path);
    void shortcutActivatedFull(const ShortcutItem& item);
    
private slots:
    void onAddShortcutClicked();
    void onAddFolderClicked();
    void retranslate();
    
private:
    // 递归渲染
    void renderItems(const QVector<ShortcutItem>& items,
                     const QVector<int>& parentPath, int indent);
    
    // 通用增删改（按 path 定位）
    ShortcutItem*       itemAt(const QVector<int>& path);
    void removeAtPath(const QVector<int>& path);
    void editAtPath(const QVector<int>& path);
    void addShortcutUnder(const QVector<int>& folderPath);
    void addFolderUnder(const QVector<int>& folderPath);
    void toggleExpandedAtPath(const QVector<int>& folderPath);
    
    // 按钮渲染
    QPushButton* makeShortcutButton(const ShortcutItem& item, int indent,
                                    const QVector<int>& path);
    QPushButton* makeFolderButton(const ShortcutItem& item, int indent,
                                  const QVector<int>& path);
    
    QVBoxLayout* m_listLayout = nullptr;
    QPushButton* m_addShortcutBtn = nullptr;
    QPushButton* m_addFolderBtn = nullptr;
    QVector<ShortcutItem> m_items;
    QVector<int> m_selectedPath;
};
```

### 持久化机制

使用 QSettings：
```
QSettings("WorkBuddy", "ZoeFileManager") 
    → "shortcuts" 数组
    
shortcuts[i] = {
    name, path, type, isFolder, expanded,
    username, password_obf, workgroup,
    children[] (递归)
}
```

### 顶部 Header 布局
- 标题标签
- "新建快捷目录" 按钮 (m_addFolderBtn)
- "新建快捷方式" 按钮 (m_addShortcutBtn)

### 右键菜单
- **单条快捷方式**: 编辑 / 添加快捷目录 / 删除
- **快捷目录头**: 在此添加快捷方式 / 在此添加子快捷目录 / 重命名 / 删除

---

## 2. 添加快捷方式对话框 (AddShortcutDialog)

### 位置
**嵌入** `/Users/chairou/code/cpp/zoefilemanager/src/ShortcutBar.cpp` 第 86-500 行

### 核心设计

**私有类** AddShortcutDialog: public QDialog

```cpp
class AddShortcutDialog : public QDialog {
    // 类型选择（顶部固定）
    QComboBox*   m_typeCombo;          // "Local folder" / "SFTP" / "SMB"
    
    // 通用字段
    QLineEdit*   m_nameEdit;           // 快捷方式显示名（可选，留空自动取最后一段）
    
    // Local 类型字段
    QLineEdit*   m_localPathEdit;      // 本地目录绝对路径
    QPushButton* m_browseBtn;          // 文件夹选择按钮
    QWidget*     m_localPathRowWidget;
    
    // SFTP 类型字段
    QLineEdit*   m_hostEdit;           // 主机名或 IP
    QSpinBox*    m_portSpin;           // 端口（默认 22）
    QLineEdit*   m_userEdit;           // 用户名
    QLineEdit*   m_pwdEdit;            // 密码（密码混淆保存）
    QLineEdit*   m_remotePathEdit;     // 服务器路径（/data/work）
    
    // SMB 类型字段
    QLineEdit*   m_smbHostEdit;        // SMB 主机
    QLineEdit*   m_smbShareEdit;       // 共享名（如 "Public"）
    QLineEdit*   m_smbSubpathEdit;     // 共享内子路径（可选）
    QLineEdit*   m_smbUserEdit;        // 用户名（支持 DOMAIN\\user）
    QLineEdit*   m_smbPwdEdit;         // 密码
    QLineEdit*   m_smbWorkgroupEdit;   // 工作组/AD 域名
    
    QFormLayout* m_form;
    QLabel*      m_hintLabel;          // 提示文本
    
public:
    QString type() const;              // 当前选择的类型
    QString name() const;              // 自动补全名字
    QString path() const;              // 拼成完整 URL
    QString username() const;          // 按类型取凭证
    QString password() const;
    QString workgroup() const;
    void setCredentials(...);          // 编辑模式下回填凭证
    
private:
    void applyTypeUI();                // 根据类型切换字段显示/隐藏
    void prefillFromPath(...);         // 编辑模式：URL 反解回字段
    void setRowVisible(...);           // 控制表单行的显示/隐藏
};
```

### 字段宽度规范
- 所有输入框固定宽度：**200px**
- 与 RemoteDialog 对齐，视觉风格一致

### 类型切换逻辑

**Local Mode**:
- 显示: Directory + Browse 按钮
- 隐藏: SFTP、SMB 所有字段

**SFTP Mode**:
- 显示: Host, Port, Username, Password, Path
- 隐藏: SMB 字段
- 端口默认 22，若当前 445 → 改为 22

**SMB Mode**:
- 显示: Host, Share, Sub-path, Username, Password, Workgroup
- 隐藏: SFTP 字段
- 端口默认 445，若当前 22 → 改为 445

### 调用流程

1. `ShortcutBar::onAddShortcutClicked()` 或 `onAddFolderClicked()`
2. 获取"智能落点" folderPath（选中 folder 或其子项所在 folder）
3. `ShortcutBar::addShortcutUnder(folderPath)`
4. 创建 `AddShortcutDialog` 对话框 `exec()`
5. 用户填表 → 点 OK
6. 验证路径（`validatePath()`）
7. 创建 `ShortcutItem` 子对象
8. 插入 `level->append(child)`
9. 调用 `saveToSettings()` + `rebuildButtons()`

### 关键方法

```cpp
// 返回拼接的 URL（path() 调用它）
QString path() const {
    const QString t = type();
    if (t == "local") {
        // 返回本地绝对路径，支持 ~ 展开
        return expandedPath;
    }
    if (t == "sftp") {
        // 拼成: sftp://[user@]host[:port]/path
        return url;
    }
    // smb
    // 拼成: smb://host/share[/sub-path]
    return url;
}

void prefillFromPath(const QString& type, const QString& path) {
    // 编辑时，URL 反解回各字段
    if (type == "local") {
        m_localPathEdit->setText(path);
    } else if (type == "sftp" && path.startsWith("sftp://")) {
        // 解析 sftp://[user@]host:port/path
    } else if (type == "smb" && path.startsWith("smb://")) {
        // 解析 smb://host/share[/sub]
    }
}

void applyTypeUI() {
    // 根据 m_typeCombo 当前选项
    // 调用 setRowVisible() 显示/隐藏相应行
}
```

---

## 3. SFTP 连接相关代码

### SftpClient

**头文件**: `/Users/chairou/code/cpp/zoefilemanager/src/SftpClient.h` (100 行)
**实现**: `/Users/chairou/code/cpp/zoefilemanager/src/SftpClient.cpp` (600+ 行)

#### 核心接口

```cpp
class SftpClient : public QObject {
    // 使用 libssh2 库实现
    
    bool connectAndAuth(const RemoteConnection& conn,
                        bool openSftpChannel = true,
                        int timeoutMs = 8000);
    
    void disconnect();
    bool isConnected() const { return m_session != nullptr; }
    QString lastError() const { return m_lastError; }
    QString hostFingerprintSha256() const { return m_fingerprint; }
    
    // 远程文件系统操作
    QVector<FileEntry> listDirectory(const QString& remotePath);
    QStringList        listSubdirectories(const QString& remotePath);
    bool               isDirectory(const QString& remotePath);
    bool               exists(const QString& remotePath);
    
    void setShowHidden(bool show) { m_showHidden = show; }
    bool showHidden() const { return m_showHidden; }
    
private:
    int                m_socket    = -1;
    LIBSSH2_SESSION*   m_session   = nullptr;
    LIBSSH2_SFTP*      m_sftp      = nullptr;
    QString            m_lastError;
    QString            m_fingerprint;  // SHA256（登录成功后回填）
    bool               m_showHidden = false;
};
```

#### 认证策略（RemoteConnection 内部）

1. **优先尝试公钥认证** (若 `privateKeyPath` 非空)
   - 支持 passphrase 保护的私钥
   
2. **回退到密码认证** (否则或公钥失败)
   - 使用 `password` 字段
   
3. **指纹验证**
   - 登录成功后将主机 SHA256 指纹写回 `RemoteConnection.fingerprint`
   - 用于在 UI 展示，未来可作 TOFU 信任锚

#### 连接流程

```
1. getaddrinfo 解析主机名 (IPv4/IPv6 双栈)
2. 非阻塞 TCP connect + select 超时
3. SSH 握手 (libssh2_session_handshake)
4. 拿主机指纹 SHA256
5. 认证: 优先 publickey → 否则 password
6. 打开 SFTP 通道 (opendir("/") 验证)
```

#### 线程模型
- **同步阻塞调用**（GUI 线程）
- 登录耗时 < 1s 一般可接受
- 若需长连接可搬到 `QThread`

---

## 4. 远程连接对话框 (RemoteDialog)

### 文件位置
- **头文件**: `/Users/chairou/code/cpp/zoefilemanager/src/RemoteDialog.h` (103 行)
- **实现**: `/Users/chairou/code/cpp/zoefilemanager/src/RemoteDialog.cpp` (400+ 行)

### 核心类定义

```cpp
class RemoteDialog : public QDialog {
    Q_OBJECT
    
public:
    explicit RemoteDialog(QWidget* parent = nullptr);
    ~RemoteDialog() override;
    
    RemoteConnection getConnection() const;
    std::unique_ptr<SftpClient> takeClient();    // SFTP 成功时有效
    std::unique_ptr<SmbClient>  takeSmbClient(); // SMB 成功时有效
    
signals:
    void connectRequested(const RemoteConnection& conn);  // 真实登录成功后发出
    
private slots:
    void onSavedConnectionClicked(QListWidgetItem* item);
    void onConnectClicked();
    void onBrowseKey();
    void onProtocolChanged(int index);
    void retranslate();
    
private:
    void setupSavedConnections();
    void setBusy(bool busy);
    
    // --- 控件 ---
    QGroupBox*   m_savedGroup;
    QGroupBox*   m_formGroup;
    QFormLayout* m_formLayout;
    QListWidget* m_savedList;
    QComboBox*   m_protocolCombo;    // "SFTP" / "SMB"
    QLineEdit*   m_hostInput;
    QSpinBox*    m_portSpin;         // 范围 1-65535
    QLineEdit*   m_usernameInput;
    QLineEdit*   m_passwordInput;    // Password Echo Mode
    QLineEdit*   m_keyPathInput;
    QPushButton* m_browseKeyBtn;
    QLineEdit*   m_passphraseInput;  // 私钥 passphrase
    QLineEdit*   m_shareInput;       // SMB only
    QLineEdit*   m_workgroupInput;   // SMB only
    QWidget*     m_keyRowWidget;     // SFTP 时可见、SMB 时隐藏
    QPushButton* m_connectBtn;
    QPushButton* m_cancelBtn;
    QLabel*      m_statusLabel;
    
    // --- 状态 ---
    QVector<RemoteConnection> m_savedConnections;
    RemoteConnection          m_lastConnected;
    std::unique_ptr<SftpClient> m_client;
    std::unique_ptr<SmbClient>  m_smbClient;
    bool m_cursorOverridden = false;  // 光标 push/pop 守门
};
```

### 字段对齐规范
- **通用宽度**: 200px（所有输入框都对齐）
- **QFormLayout 配置**:
  ```cpp
  m_formLayout->setLabelAlignment(Qt::AlignLeft);
  m_formLayout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  m_formLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
  ```

### 协议切换行为

**SFTP → SMB**:
- 隐藏字段: keyPathInput, passphraseInput
- 显示字段: shareInput, workgroupInput
- 端口默认改为 445（若当前为 22）

**SMB → SFTP**:
- 隐藏字段: shareInput, workgroupInput
- 显示字段: keyPathInput, passphraseInput
- 端口默认改为 22（若当前为 445）

### 已保存连接示例

```cpp
void RemoteDialog::setupSavedConnections() {
    RemoteConnection prod;
    prod.name = "Production Server"; prod.protocol = "SFTP";
    prod.host = "prod.example.com"; prod.port = 22; prod.username = "deploy";
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
    
    // 填充 m_savedList 显示
}
```

### 连接流程

```
1. 用户点 Connect 按钮 (onConnectClicked)
2. setBusy(true) → 禁用所有输入 + 显示等待光标
3. 调 SftpClient::connectAndAuth() 或 SmbClient::connectAndAuth()
4. 成功 → accept()；失败 → 弹 QMessageBox，用户继续尝试
5. MainWindow 用 takeClient() 拿走活会话，挂进 FileSystemRouter
```

### 密码/凭证处理

- **密码字段**: `QLineEdit::Password` Echo Mode（隐藏输入）
- **持久化**: Base64 简单混淆（非强加密）
- **警告**: 切勿在共享设备保存敏感账号

---

## 5. 连接配置数据结构 (RemoteConnection)

### 定义位置
**文件**: `/Users/chairou/code/cpp/zoefilemanager/src/Types.h` (第 94-107 行)

### 完整结构

```cpp
struct RemoteConnection {
    // 通用字段
    QString name;                   // 用户给这条连接起的别名（保存连接时用）
    QString protocol;               // "SFTP" / "SMB"
    QString host;
    int port = 22;
    QString username;
    QString password;               // Base64 简单混淆
    
    // SFTP 专用字段
    QString privateKeyPath;         // 可选：私钥文件路径，给了就优先用公钥
    QString passphrase;             // 可选：私钥的 passphrase
    QString fingerprint;            // 登录成功后回填（SHA256）
    
    // SMB 专用字段
    QString share;                  // SMB 共享名（如 "Public"）
    QString workgroup;              // 可选：Windows 工作组/域名
};
```

### 字段说明

| 字段 | 类型 | 默认值 | 说明 | 适用协议 |
|------|------|--------|------|---------|
| name | QString | - | 连接别名 | 通用 |
| protocol | QString | - | "SFTP" / "SMB" | 通用 |
| host | QString | - | 主机名或 IP | 通用 |
| port | int | 22 | TCP 端口 | 通用 |
| username | QString | - | 登录用户名 | 通用 |
| password | QString | - | 登录密码 | 通用 |
| privateKeyPath | QString | - | 私钥路径 | SFTP only |
| passphrase | QString | - | 私钥 passphrase | SFTP only |
| fingerprint | QString | - | 主机 SHA256 指纹 | 通用（回填） |
| share | QString | - | SMB 共享名 | SMB only |
| workgroup | QString | - | Windows 域/工作组 | SMB only |

### 用途

1. **RemoteDialog**: 收集用户输入，构造 RemoteConnection
2. **SftpClient / SmbClient**: 消费此结构，执行真实连接
3. **ShortcutItem**: 嵌入凭证信息，用于快捷方式的自动连接
4. **FileSystemRouter**: 管理活连接池

---

## 6. 连接管理实现

### 1. RemoteDialog 连接管理

**已保存连接列表** (m_savedList)
- QListWidget 显示 3 条内置示例（无真实密码）
- 点击条目 → 自动填充表单
- 格式: `"Connection Name (user@host:port)"`

**最后连接** (m_lastConnected)
- 记录最后一次登录成功的 RemoteConnection
- 用于恢复上次连接参数

### 2. 活会话管理

```cpp
// 在 RemoteDialog 中
std::unique_ptr<SftpClient> m_client;      // SFTP 活会话
std::unique_ptr<SmbClient>  m_smbClient;   // SMB 活会话

// 对话框 accept 后，调用方通过 takeClient() / takeSmbClient() 
// 使用 move 语义获取所有权
std::unique_ptr<SftpClient> takeClient();
std::unique_ptr<SmbClient>  takeSmbClient();
```

### 3. 快捷方式中的连接复用

```cpp
// ShortcutItem 内嵌的凭证字段
QString username;
QString password;
QString workgroup;

// 当用户点快捷方式时，ShortcutBar 发信号 shortcutActivatedFull(item)
// MainWindow 据此自动发起连接
```

---

## 7. 相关文件映射表

| 文件 | 行数 | 功能 |
|------|------|------|
| Types.h | 119 | RemoteConnection 结构、FileEntry、TabState 等共享类型 |
| ShortcutBar.h | 120 | ShortcutItem、ShortcutBar 类定义 |
| ShortcutBar.cpp | 1321 | AddShortcutDialog 嵌入实现 + ShortcutBar 完整逻辑 |
| RemoteDialog.h | 103 | RemoteDialog 类定义 |
| RemoteDialog.cpp | 400+ | 对话框 UI 初始化、协议切换、保存连接管理 |
| SftpClient.h | 100 | SftpClient 类定义 + libssh2 接口 |
| SftpClient.cpp | 600+ | libssh2 集成、套接字管理、认证逻辑 |
| SmbClient.h | 67 | SmbClient 类定义 + libsmbclient 接口 |
| SmbClient.cpp | 500+ | libsmbclient 集成、鉴权回调 |
| FileSystemRouter.h/cpp | - | 活连接池管理、protocol 路由 |
| MainWindow.h/cpp | - | 对话框显示、事件协调 |

---

## 8. 关键设计要点

### 递归快捷目录
- 支持任意深度嵌套 (children[] 递归)
- 容器 (isFolder=true) 和叶子 (isFolder=false) 混合存储

### 类型统一性
- AddShortcutDialog 和 RemoteDialog 使用相同字段宽度 (200px)
- QFormLayout 对齐策略一致
- 视觉风格协调

### 凭证安全
- 密码采用 Base64 简单混淆（非强加密）⚠️
- 切勿在共享设备保存敏感账号
- 支持私钥认证 (SFTP)、匿名访问 (SMB)

### TOFU 信任模式
- 首次连接捕获主机 SHA256 指纹
- 后续用指纹验证身份（未来可启用）

### 线程安全
- 所有网络操作在 GUI 线程（阻塞）
- 库初始化一次性完成 (SftpClient::initLibrary)
- 应用退出时标记全局 shutdown 旗（防止 libcrypto cleanup 顺序问题）

---

## 9. 快速查找指南

| 需求 | 文件位置 | 行数 |
|------|---------|------|
| 添加快捷方式对话框 | ShortcutBar.cpp | 86-500 |
| ShortcutItem 定义 | ShortcutBar.h | 36-49 |
| RemoteConnection 定义 | Types.h | 94-107 |
| SFTP 认证逻辑 | SftpClient.cpp | 200+ |
| 协议切换 UI | RemoteDialog.cpp | 138-182 |
| 保存连接列表 | RemoteDialog.cpp | 194-217 |
| 快捷方式持久化 | ShortcutBar.cpp | 1200+ |

