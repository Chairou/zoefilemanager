#ifndef SHORTCUTBAR_H
#define SHORTCUTBAR_H

// =============================================================================
// ShortcutBar —— 快捷管理器（Shortcut Manager）
//
// 概念区分（重要）：
//   - **快捷目录（Shortcut Folder）**：isFolder=true 的容器条目，用来组织
//     快捷方式。可以**嵌套快捷目录**，可以装本地/SFTP/SMB 任何叶子条目。
//     图标：📁 emoji（仅作为容器语义符号）。
//   - **本地目录（Local shortcut）**：type=="local" 的叶子条目，指向一个
//     真实的系统目录路径（绝对路径）。**不能嵌套**——它就是终点。
//     图标：QStyle::SP_DirIcon 系统目录图标（与左侧 DirectoryTree 一致）。
//   - **远程快捷方式**：type=="sftp" / "smb" 的叶子条目，指向远程 URL。
//     图标：🌐 / 🖥 emoji。
//
// 顶部 header 三件套：标题 + "新建快捷目录" + "新建快捷方式"
// 右键单条快捷方式：编辑 / 添加快捷目录 / 删除
// 右键快捷目录头：在此添加快捷方式 / 在此添加子快捷目录 / 重命名 / 删除
// 持久化：QSettings("WorkBuddy","ZoeFileManager") 下 "shortcuts" 数组
//   shortcuts[i] = { name, path, isFolder, expanded, type, username,
//                    password_obf, workgroup, children[] (递归嵌套) }
// 兼容性：旧版扁平 shortcuts[i] = {name,path} 自动识别（isFolder 缺省 = false）；
//         旧版只支持单层 children，新版改递归读写但向后兼容。
// =============================================================================

#include <QWidget>
#include <QVector>
#include <QString>

class QVBoxLayout;
class QPushButton;
class QLabel;

/// 单个条目：快捷方式（叶子）或快捷目录（容器）
struct ShortcutItem {
    QString name;
    QString type = "local";             // "local" / "sftp" / "smb"（仅 isFolder=false 时有意义）
    QString path;                       // local: 本地绝对路径；sftp/smb: 完整 URL
    // 远程协议可选凭证（仅 sftp/smb 用，未填即留空）。
    // 持久化时以 Base64 简单混淆写盘，*非强加密*；切勿在共享设备保存敏感账号。
    QString username;                   // sftp/smb 用
    QString password;                   // sftp/smb 用
    QString workgroup;                  // smb 专用（AD 域名，留空 = WORKGROUP）
    // SFTP 专用：私钥文件登录
    QString privateKeyPath;             // sftp 用：私钥文件路径（优先于密码认证）
    QString passphrase;                 // sftp 用：私钥的 passphrase（可选）
    bool    isFolder = false;
    bool    expanded = true;            // 仅 isFolder=true 时有效
    /// 仅 isFolder=true 时有效；可以同时包含叶子（local/sftp/smb）和嵌套快捷目录。
    QVector<ShortcutItem> children;
};

class ShortcutBar : public QWidget {
    Q_OBJECT

public:
    explicit ShortcutBar(QWidget* parent = nullptr);

    void loadFromSettings();
    void saveToSettings() const;

signals:
    void shortcutActivated(const QString& path);
    /// 完整版：除 path 外还带 type / username / password / workgroup，
    /// 让 MainWindow 能据此自动建立 SFTP / SMB 连接（含 DFS / 凭证复用）。
    /// shortcutActivated 与本信号同时触发，调用方按需选择监听。
    void shortcutActivatedFull(const ShortcutItem& item);

private slots:
    void onAddShortcutClicked();
    void onAddFolderClicked();
    void retranslate();

private:
    void rebuildButtons();
    /// 递归地把 items 渲染到 m_listLayout，使用 path 表示节点位置（path[0]
    /// = 顶层 index，path[1] = 第一层 child index，依此类推）。
    void renderItems(const QVector<ShortcutItem>& items,
                     const QVector<int>& parentPath, int indent);

    /// 通过路径找节点（返回 nullptr 表示无效路径）
    ShortcutItem*       itemAt(const QVector<int>& path);
    const ShortcutItem* itemAt(const QVector<int>& path) const;

    // ---- 顶层操作 ----
    void addTopLevel(const ShortcutItem& item);

    // ---- 通用增删改（按 path 定位，path 为空 = 顶层操作）----
    /// 删除 path 指向的条目（顶层或嵌套子项）；删快捷目录时连同所有后代一起删
    void removeAtPath(const QVector<int>& path);
    /// 编辑 path 指向的条目：folder 走改名；shortcut 走 AddShortcutDialog
    void editAtPath(const QVector<int>& path);
    /// 在 path 指向的快捷目录下追加一个 shortcut（path 为空 = 加到顶层）
    void addShortcutUnder(const QVector<int>& folderPath);
    /// 在 path 指向的快捷目录下追加一个子快捷目录（path 为空 = 加到顶层）
    void addFolderUnder(const QVector<int>& folderPath);
    /// 切换 path 指向的快捷目录的 expanded 状态
    void toggleExpandedAtPath(const QVector<int>& folderPath);

    // ---- 渲染 ----
    QPushButton* makeShortcutButton(const ShortcutItem& item, int indent,
                                    const QVector<int>& path);
    QPushButton* makeFolderButton(const ShortcutItem& item, int indent,
                                  const QVector<int>& path);

    QVBoxLayout* m_listLayout = nullptr;
    QPushButton* m_addShortcutBtn = nullptr;
    QPushButton* m_addFolderBtn = nullptr;
    QLabel*      m_title = nullptr;
    QVector<ShortcutItem> m_items;

    // 当前选中条目的"路径"（顶层 index + 各级 child index 链）。
    // 空 path = 没有选中。
    // 选中态由左/右键点击触发，rebuildButtons 时按 path 比对渲染。
    QVector<int> m_selectedPath;
    /// 同步衍生：当 m_selectedPath 指向 folder 自身时为 selected folder path；
    /// 当 m_selectedPath 指向 shortcut 时为其所在 folder path（"新增目标"）。
    /// 二者通过 selectedTargetFolderPath() 的逻辑统一推导，不再单独存。
    QVector<int> selectedTargetFolderPath() const;
};

#endif // SHORTCUTBAR_H
