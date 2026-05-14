# 代码库关键文件速查表

## 核心源文件列表

```
src/
├── Types.h                    # 全应用共享数据结构
│   └── RemoteConnection       # SFTP/SMB 连接配置
│   └── ShortcutItem (嵌在 ShortcutBar.h)
│   └── FileEntry
│   └── TabState
│
├── ShortcutBar.h/cpp         # 快捷管理器（1321 行）
│   ├── struct ShortcutItem   # 快捷方式/目录条目
│   ├── class ShortcutBar     # 快捷栏主类
│   └── class AddShortcutDialog (私有，嵌入 .cpp)
│
├── RemoteDialog.h/cpp        # 远程连接对话框
│   └── class RemoteDialog    # 连接 UI 和管理
│
├── SftpClient.h/cpp          # SFTP 客户端（libssh2）
│   └── class SftpClient
│
├── SmbClient.h/cpp           # SMB 客户端（libsmbclient）
│   └── class SmbClient
│
├── FileSystemRouter.h/cpp    # 文件系统路由层
│
├── MainWindow.h/cpp          # 主窗口（事件协调）
├── FilePanel.h/cpp           # 双面板视图
├── DirectoryTree.h/cpp       # 左侧目录树
├── FileListView.h/cpp        # 文件列表视图
├── PathBar.h/cpp             # 面包屑导航栏
│
└── 其他...
```

## 关键数据类型定义

### 1. RemoteConnection (Types.h:94-107)

```cpp
struct RemoteConnection {
    // 通用
    QString name;              // 连接别名
    QString protocol;          // "SFTP" / "SMB"
    QString host;
    int port = 22;
    QString username;
    QString password;
    
    // SFTP 专用
    QString privateKeyPath;    // 私钥路径
    QString passphrase;        // 私钥 passphrase
    QString fingerprint;       // 登录后回填 SHA256
    
    // SMB 专用
    QString share;             // 共享名
    QString workgroup;         // 域/工作组
};
```

### 2. ShortcutItem (ShortcutBar.h:36-49)

```cpp
struct ShortcutItem {
    QString name;              // 显示名
    QString type = "local";    // "local" / "sftp" / "smb"
    QString path;              // 路径或 URL
    
    // 远程凭证
    QString username;
    QString password;          // Base64 混淆
    QString workgroup;
    
    // 容器字段
    bool isFolder = false;
    bool expanded = true;
    QVector<ShortcutItem> children;  // 递归嵌套
};
```

### 3. FileEntry (Types.h:49-57)

```cpp
struct FileEntry {
    QString name;
    QString path;              // 本地路径或 sftp:// URL
    bool isDirectory = false;
    qint64 size = 0;
    QDateTime modified;
    QString permissions;       // POSIX 格式 "rwxr-xr-x"
    FileType type;
};
```

## 对话框 UI 结构

### AddShortcutDialog (ShortcutBar.cpp:86-500)

私有嵌入类，字段宽度统一 **200px**：

**Type Combo**（顶部，类型选择）
- "Local folder"
- "SFTP"
- "SMB"

**Local Mode 字段**
- Name (可选，自动补全)
- Directory + Browse 按钮

**SFTP Mode 字段**
- Host (主机或 IP)
- Port (默认 22)
- Username
- Password
- Path (服务器路径，如 /data/work)

**SMB Mode 字段**
- Host
- Share (共享名，如 Public)
- Sub-path (可选)
- Username (支持 DOMAIN\user)
- Password
- Workgroup (AD 域名，家用留空)

### RemoteDialog (RemoteDialog.cpp:19-133)

顶部：已保存连接列表 (QListWidget)
```
Production Server (deploy@prod.example.com:22)
Staging Server (admin@staging.example.com:22)
File Server (guest@fileserver.example.com:445)
```

表单：
- Protocol: SFTP / SMB
- Host
- Port
- Username
- Password
- Key Path (SFTP only，含 Browse 按钮)
- Passphrase (SFTP only)
- Share (SMB only)
- Workgroup (SMB only)

## 关键方法位置

### ShortcutBar 操作

| 操作 | 文件 | 行数 |
|------|------|------|
| 添加快捷方式 | ShortcutBar.cpp | 1099-1129 |
| 添加快捷目录 | ShortcutBar.cpp | 1131-1156 |
| 保存到 QSettings | ShortcutBar.cpp | 1250+ |
| 从 QSettings 加载 | ShortcutBar.cpp | 1200+ |
| 递归渲染 | ShortcutBar.cpp | 800+ |
| 按路径查找节点 | ShortcutBar.cpp | 950+ |

### RemoteDialog 操作

| 操作 | 文件 | 行数 |
|------|------|------|
| 协议切换 | RemoteDialog.cpp | 138-182 |
| 保存连接初始化 | RemoteDialog.cpp | 194-217 |
| 点击已保存条目 | RemoteDialog.cpp | 219-231 |
| 点击 Connect | RemoteDialog.cpp | 250+ |
| 浏览私钥 | RemoteDialog.cpp | 233-238 |

### SftpClient 操作

| 操作 | 文件 | 行数 |
|------|------|------|
| 连接 + 认证 | SftpClient.cpp | 200+ |
| 打开套接字 | SftpClient.cpp | 67-130 |
| 套接字超时 | SftpClient.cpp | 130+ |
| SSH 握手 | SftpClient.cpp | 170+ |
| 获取指纹 | SftpClient.cpp | 180+ |
| 公钥认证 | SftpClient.cpp | 190+ |
| 密码认证 | SftpClient.cpp | 210+ |

## 协议/格式规范

### 快捷方式 URL 格式

**Local**:
```
/Users/alice/Downloads
~/Projects/Code
```

**SFTP**:
```
sftp://user@host:22/data/work
sftp://host/path          (不含用户名)
sftp://host:2222/path     (非标准端口)
```

**SMB**:
```
smb://fileserver/share
smb://fileserver/share/subfolder
```

### QSettings 持久化路径

```
QSettings("WorkBuddy", "ZoeFileManager")
└── shortcuts[]
    ├── [0]
    │   ├── name: "My Projects"
    │   ├── type: "sftp"
    │   ├── path: "sftp://alice@example.com/data/work"
    │   ├── username: "alice"
    │   ├── password: <Base64 混淆>
    │   ├── isFolder: false
    │   └── expanded: true
    ├── [1]
    │   ├── name: "Folders"
    │   ├── isFolder: true
    │   ├── expanded: true
    │   └── children[]
    │       └── [0] { local 快捷方式 }
    └── ...
```

## 凭证处理

### 密码混淆

方式：Base64 编码（简单混淆，**非强加密**）
风险：不安全，仅适合非关键环境
存储：QSettings 本地文件（未加密）
警告：切勿在共享设备保存敏感账号

### 认证流程

**SFTP**:
```
1. 若 privateKeyPath 非空
   ├─ 尝试加载私钥 (支持 passphrase)
   ├─ 若成功 → 公钥认证
   └─ 若失败 + password 存在 → 回退密码认证
2. 否则 → 直接密码认证
3. 登录成功 → 捕获主机 SHA256 指纹
```

**SMB**:
```
1. 构造 smb:// URL
2. 通过 libsmbclient 认证回调提供凭证
3. 尝试 opendir(share) 验证连接
4. 支持：有用户名+密码、仅用户名、匿名访问
```

## 视觉样式

### 快捷方式图标与颜色

| 类型 | Emoji | 颜色 | 说明 |
|------|-------|------|------|
| 快捷目录 | 📁 | - | 合口文件夹（容器） |
| 本地快捷 | 📂 | #88C0D0 青色 | 开口文件夹（本地） |
| SFTP 快捷 | 🌐 | #A3BE8C 绿色 | 网络/远程 |
| SMB 快捷 | 🖥 | #B48EAD 紫色 | Windows 共享 |

### 字段宽度规范

所有对话框输入框统一：**200px**
- AddShortcutDialog 中： kFieldWidth = 200
- RemoteDialog 中： kFieldWidth = 200
- QLineEdit::setMinimumWidth(200)
- QLineEdit::setMaximumWidth(200)

### QFormLayout 配置

```cpp
m_form->setLabelAlignment(Qt::AlignLeft);
m_form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
m_form->setRowWrapPolicy(QFormLayout::DontWrapRows);
```

## 性能与线程

### GUI 线程阻塞设计

- 所有网络操作（SSH、SMB）在 GUI 线程同步调用
- 连接超时通常 < 1s（RemoteDialog 显示等待光标）
- 若需后台连接，可将 connectAndAuth 搬到 QThread

### 库初始化

```cpp
// SftpClient::initLibrary() 一次性调用
libssh2_init(0);        // 线程不安全，仅 GUI 线程

// 应用退出前
SftpClient::markShuttingDown();  // 标记全局 shutdown 旗
// 防止 libcrypto RNG 锁与 dyld unload 顺序问题
```

## 测试文件

```
tests/
├── test_sftp_client.cpp      # SFTP 客户端单元测试
├── test_router_sftp.cpp      # 文件系统路由 SFTP 测试
└── test_pathbar_routing.cpp  # 面包屑导航路由测试
```

## 常见修改点

### 添加新的连接类型

1. Types.h: RemoteConnection 添加新字段
2. AddShortcutDialog: 添加 UI 字段 + applyTypeUI() 逻辑
3. RemoteDialog: 添加对应的 UI + 协议切换逻辑
4. FileSystemRouter: 添加新协议的路由实现
5. 新建 XXXClient 类（如 SftpClient、SmbClient）

### 扩展快捷方式字段

1. ShortcutBar.h: ShortcutItem 添加成员
2. AddShortcutDialog: 新增输入控件
3. ShortcutBar::saveToSettings() / loadFromSettings() 更新序列化
4. 向后兼容：旧数据字段缺失时设默认值

### 修改对话框字段宽度

1. AddShortcutDialog: 修改 `kFieldWidth` 常量（第 373 行）
2. RemoteDialog: 修改 `kFieldWidth` 常量（第 47 行）

---

**生成时间**: 2026-05-14
**报告版本**: 1.0
**覆盖范围**: ShortcutBar、RemoteDialog、SFTP/SMB 连接、连接管理
