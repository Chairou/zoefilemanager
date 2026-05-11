#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// =============================================================================
// MainWindow —— 应用主窗口（QMainWindow 派生）
//
// 视觉布局（QSplitter 横向 3 列）：
//   ┌───────────────────────────────────────────────────────┐
//   │ Toolbar (Back/Forward/Up/Refresh/Copy/Cut/Paste/...) │
//   ├──────────────┬──────────────────────┬──────────────┤
//   │ DirectoryTree│ FilePanel(Left)      │ FilePanel(R) │
//   │ ShortcutBar  │  - tabs              │  - tabs      │
//   │              │  - PathBar           │  - PathBar   │
//   │              │  - FileListView      │  - FileListV │
//   ├──────────────┴──────────────────────┴──────────────┤
//   │ StatusBar: counts, clipboard, active panel          │
//   └───────────────────────────────────────────────────────┘
//
// 主要职责：
//   1. 编排 UI 组件（toolbar / 树 / 双面板 / 状态栏 / 主题 / 快捷键）。
//   2. 把"业务动作"集中在槽里：复制/剪切/粘贴/删除/搜索/远程连接/拖拽复制 等。
//   3. 跨面板/跨组件协调：哪个面板是 active；剪贴板状态；远程挂载状态。
//
// 不做：
//   - 真正的文件 I/O（委托给 RealFileSystem / FileSystemRouter）
//   - 单个面板内的导航历史（FilePanel 自己管）
//   - SSH/SFTP 协议（委托给 SftpClient）
// =============================================================================

#include "Types.h"
#include "FilePanel.h"
#include "DirectoryTree.h"
#include "ShortcutBar.h"
#include <QMainWindow>
#include <QSplitter>
#include <QToolBar>
#include <QStatusBar>
#include <QAction>
#include <QLabel>
#include <QCloseEvent>
#include <optional>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    // 拦截关闭事件以持久化窗口/面板状态（左右面板路径、活跃面板、splitter、几何）
    void closeEvent(QCloseEvent* event) override;

private slots:
    // ----- 工具栏动作 -----
    void onBack();         // 后退（active panel 的历史栈）
    void onForward();      // 前进
    void onUp();           // 到父目录
    void onRefresh();      // 重新列当前目录
    void onCopy();         // 把选中条目放入应用内剪贴板（isCut=false）
    void onCut();           // 同上但 isCut=true（粘贴后会删源）
    void onPaste();        // 从剪贴板粘贴到 active panel 当前目录
    void onDelete();       // 删除选中条目（带确认+进度）
    void onSelectAll();
    void onCopyPath();     // 把当前路径复制到系统剪贴板
    void onRunScript();    // 弹出 ScriptDialog 选脚本/参数运行
    void onSearch();       // 弹出 SearchDialog 在当前目录搜索
    void onCompressZip();  // 把选中条目压成 zip 到当前目录
    /// 双击压缩包：在 archivePath 所在目录创建 <basename>/ 并解压进去，
    /// 完成后刷新对应侧面板。
    void onExtractArchive(const QString& archivePath, PanelSide side);
    void onRemoteConnect();    // 弹出 RemoteDialog；成功后把 SFTP 挂到 router
    void onRemoteDisconnect(); // 拆挂载，回到 home
    void onToggleTheme();      // 暗/亮主题切换
    void onToggleHidden();     // 显示/隐藏点文件

    // ----- 右键菜单：新建文件 / 新建目录 -----
    // dir 为目标目录（active panel 的 currentPath）；isRemote 为 router.isRemote(dir)。
    // 远程暂不支持，弹 information 后 return。
    void onCreateNewFile(const QString& dir, bool isRemote);
    void onCreateNewFolder(const QString& dir, bool isRemote);

    // ----- 来自子组件的回调 -----
    void onPanelPathChanged(const QString& path, PanelSide side);
    void onPanelSelectionChanged(PanelSide side);
    void onPanelActivated(PanelSide side);          // 哪个面板被点击/获焦
    void onDirectoryTreeSelected(const QString& path);
    void onContextMenu(const QPoint& pos, PanelSide side);

    /**
     * 拖拽复制：FilePanel 通过 dropCopyRequested 信号请求把
     * sourcePaths 复制进 destPath；本槽负责弹进度对话框 + 实际复制
     * + 校验本地↔远程的边界（远程暂不支持）+ 完成后刷新目标面板。
     */
    void onDropCopy(const QStringList& sourcePaths,
                    const QString& destPath,
                    PanelSide destSide);

private:
    // ----- 装配 -----
    void setupToolbar();
    void setupStatusBar();
    void setupShortcuts();    // 全局快捷键（Cmd+C/X/V/A/Backspace/...）
    void applyDarkTheme();
    void applyLightTheme();
    void updateStatusBar();   // 重算左右面板条目数 / 剪贴板提示
    FilePanel* activePanel() const;   // m_activePane 翻译为对应指针

    // ----- 持久化（QSettings: WorkBuddy / ZoeFileManager） -----
    void loadSettings();      // 启动时读取并恢复左右面板路径、活跃面板、几何
    void saveSettings();      // 关闭时写入当前左右面板路径、活跃面板、几何

    // ----- 子组件 -----
    QSplitter* m_mainSplitter;
    DirectoryTree* m_dirTree;
    ShortcutBar* m_shortcutBar;
    FilePanel* m_leftPanel;
    FilePanel* m_rightPanel;
    PanelSide m_activePane = PanelSide::Left;

    // ----- 工具栏动作 -----
    QAction* m_backAction;
    QAction* m_forwardAction;
    QAction* m_upAction;
    QAction* m_refreshAction;
    QAction* m_copyAction;
    QAction* m_cutAction;
    QAction* m_pasteAction;
    QAction* m_deleteAction;
    QAction* m_selectAllAction;
    QAction* m_copyPathAction;
    QAction* m_runScriptAction;
    QAction* m_searchAction;
    QAction* m_remoteConnectAction;
    QAction* m_remoteDisconnectAction;
    QAction* m_themeAction;
    QAction* m_hiddenAction;

    // ----- 状态栏标签 -----
    QLabel* m_statusLeft;
    QLabel* m_statusRight;
    QLabel* m_statusClipboard;
    QLabel* m_statusPanel;

    // ----- 应用状态 -----
    std::optional<ClipboardData> m_clipboard;       // 应用内剪贴板（与系统剪贴板独立）
    bool m_isDarkTheme = true;
    bool m_isRemoteConnected = false;
    QString m_remoteMountPrefix;  // 例如 "sftp://demo@test.rebex.net:22"，
                                  // 也是 FileSystemRouter 里的挂载键
};

#endif // MAINWINDOW_H
