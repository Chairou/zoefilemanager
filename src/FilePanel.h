#ifndef FILEPANEL_H
#define FILEPANEL_H

// =============================================================================
// FilePanel —— 单侧文件浏览面板（左/右各一份）
//
// 内部组成：
//   ┌────────────────────────────────────────────┐
//   │ [tab1][tab2]...[+]                         │  ← 自定义 QTabBar
//   ├────────────────────────────────────────────┤
//   │ PathBar (面包屑/编辑模式)                   │  ← 每 tab 一份
//   ├────────────────────────────────────────────┤
//   │ FileListView (QTableWidget 派生)            │  ← 每 tab 一份
//   └────────────────────────────────────────────┘
//
// 关键设计：
//   1. 每个 tab 持一份 TabData（FileListView + PathBar + TabState 历史栈），
//      切 tab 走 QStackedWidget，避免每次重建视图。
//   2. **不用 QTabWidget**，因为 macOS 原生样式强制 tab 居中，无法靠左。
//      自己摆 QTabBar + QStackedWidget 是最干净的解决方法。
//   3. 拖放：本组件作为 drop target（setAcceptDrops(true)），不直接执行
//      复制，而是 emit dropCopyRequested 让 MainWindow 处理（统一进度对话框）。
//   4. 事件过滤：在 createTab 里给 fileList/pathBar/container 都装了 eventFilter，
//      任何鼠标按键 / 焦点变化都会 emit activated(side)，让 MainWindow 知道
//      "现在用户在玩这一侧"。
// =============================================================================

#include "Types.h"
#include "FileListView.h"
#include "PathBar.h"
#include <QWidget>
#include <QTabBar>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QVector>

class FilePanel : public QWidget {
    Q_OBJECT

public:
    explicit FilePanel(PanelSide side, QWidget* parent = nullptr);

    // 导航：navigateTo 把 currentPath 压进 backHistory，清空 forwardHistory
    void navigateTo(const QString& path);
    void goBack();          // 取出 backHistory 顶；当前压入 forwardHistory
    void goForward();
    void goUp();            // 到父目录；远程 mount 根处会停住，不越界
    void refresh();         // 重新列当前目录（不动历史）

    QString currentPath() const;
    PanelSide side() const { return m_side; }
    FileListView* currentFileList() const;
    PathBar* currentPathBar() const;
    QVector<FileEntry> getSelectedEntries() const;
    void selectAll();

    bool canGoBack() const;
    bool canGoForward() const;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    // ---- 拖放（drop target）----
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

signals:
    void pathChanged(const QString& path, PanelSide side);
    void selectionChanged(PanelSide side);
    void activated(PanelSide side);             // 任何鼠标/焦点交互
    void contextMenuRequested(const QPoint& pos, PanelSide side);
    /**
     * 用户双击了一个已识别的压缩包（zip/tar.gz/...）。
     * MainWindow 接到后会在压缩包所在目录下创建同名子目录并解压进去，
     * 完成后刷新本侧面板。
     */
    void archiveActivated(const QString& archivePath, PanelSide side);
    /**
     * 用户拖了一组文件丢到本面板上 —— 请求 MainWindow 执行复制。
     * 不直接在这里做拷贝，是为了集中处理进度对话框、远程边界守卫等。
     */
    void dropCopyRequested(const QStringList& sourcePaths,
                           const QString& destPath,
                           PanelSide destSide);

private slots:
    void onTabChanged(int index);
    void onAddTab();
    void onCloseTab(int index);
    void onPathSelected(const QString& path);       // PathBar 面包屑/Enter
    void onDirectoryActivated(const QString& path); // 双击目录
    void onFileSelectionChanged();

private:
    /// 单 tab 的所有状态打包
    struct TabData {
        FileListView* fileList;
        PathBar* pathBar;
        QWidget* container;     // tab 内容容器，加进 QStackedWidget
        TabState state;         // 当前路径 + 前进/后退历史
    };

    void createTab(const QString& path);
    void updateTabColor();      // 根据 m_side 重新染色当前 tab 高亮
    TabData& currentTab();
    const TabData& currentTab() const;

    // ----- 单一数据源：路径 UI 同步入口 -----
    // 在视图 rootPath 切换完成后调用，把 tab 标题、路径栏文本统一对齐到
    // `canonicalPath`（即文件视图的真实 rootPath）。任何导航路径（navigateTo /
    // goBack / goForward / createTab / onTabChanged）都必须通过它收尾，
    // 避免三处 UI 出现分叉。
    void syncPathUI(int tabIndex, const QString& canonicalPath);
    // 从绝对路径派生 tab 标题：当前目录的 basename；根目录则显示 "/"。
    // 对尾随斜杠、远程挂载根做了归一化。
    static QString titleFromPath(const QString& path);

    PanelSide m_side;
    // 自定义"tab 标题栏 + 内容栈"，详见类头注释
    QTabBar* m_tabBar = nullptr;
    QStackedWidget* m_stack = nullptr;
    QPushButton* m_addTabBtn;
    QVector<TabData> m_tabs;
    QColor m_accentColor;       // 左:#4FC3F7，右:Nord Aurora 绿
};

#endif // FILEPANEL_H
