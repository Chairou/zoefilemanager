#ifndef SHORTCUTBAR_H
#define SHORTCUTBAR_H

// =============================================================================
// ShortcutBar —— 快捷管理器（Shortcut Manager）
//
// 支持两类条目：
//   - 快捷方式（Shortcut）：name + path，点击 → emit shortcutActivated(path)
//   - 目录（Folder）：name + 一组子快捷方式 + 展开/折叠状态
//                    （为保持简单，目录不能再嵌套目录，仅一层）
//
// 顶部 header 三件套：标题 + "新建目录" + "新建快捷方式"
// 右键单条快捷方式：编辑 / 删除
// 右键目录头：重命名 / 在此目录添加快捷方式 / 删除整个目录（含其下条目，需确认）
//
// 持久化：QSettings("WorkBuddy","ZoeFileManager") 下 "shortcuts" 数组
//   shortcuts[i] = { name, path, isFolder, expanded, children[] }
//   children[j]  = { name, path }  （只能是快捷方式）
// 兼容性：旧版扁平 shortcuts[i] = {name,path} 自动识别（isFolder 缺省 = false）；
//         同时保留旧应用名 "DualPaneFileManager" 的一次性迁移逻辑。
// =============================================================================

#include <QWidget>
#include <QVector>
#include <QString>

class QVBoxLayout;
class QPushButton;
class QLabel;

/// 单个条目：快捷方式或目录
struct ShortcutItem {
    QString name;
    QString type = "local";             // "local" / "sftp" / "smb"（仅 isFolder=false 时有意义）
    QString path;                       // local: 本地绝对路径；sftp/smb: 完整 URL
    // 远程协议可选凭证（仅 sftp/smb 用，未填即留空）。
    // 持久化时以 Base64 简单混淆写盘，*非强加密*；切勿在共享设备保存敏感账号。
    QString username;                   // sftp/smb 用（SFTP 也存进 username 而非 URL，避免与 URL 解析耦合）
    QString password;                   // sftp/smb 用
    QString workgroup;                  // smb 专用（AD 域名，留空 = WORKGROUP）
    bool    isFolder = false;
    bool    expanded = true;            // 仅 isFolder=true 时有效
    QVector<ShortcutItem> children;     // 仅 isFolder=true 时有效（其子项必为快捷方式）
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

    // ---- 顶层操作 ----
    void addTopLevel(const ShortcutItem& item);
    void removeTopLevel(int index);
    void editTopLevel(int index);             // shortcut 走目录选择，folder 走改名
    void toggleFolderExpanded(int index);

    // ---- 目录子项操作 ----
    void addChildToFolder(int folderIndex);
    void removeChildFromFolder(int folderIndex, int childIndex);
    void editChildOfFolder(int folderIndex, int childIndex);

    // ---- 渲染 ----
    QPushButton* makeShortcutButton(const ShortcutItem& item, int indent,
                                    int folderIndex /*-1=top*/, int idxInParent);
    QPushButton* makeFolderButton(const ShortcutItem& item, int folderIndex);

    QVBoxLayout* m_listLayout = nullptr;
    QPushButton* m_addShortcutBtn = nullptr;
    QPushButton* m_addFolderBtn = nullptr;
    QLabel*      m_title = nullptr;
    QVector<ShortcutItem> m_items;
};

#endif // SHORTCUTBAR_H
