#ifndef DIRECTORYTREE_H
#define DIRECTORYTREE_H

// =============================================================================
// DirectoryTree —— 左侧目录树（QTreeWidget 派生）
//
// 行为约定：
//   1. 单击 → 200ms 后导航 active panel 到该目录（不展开/折叠）
//   2. 双击 → 仅切换展开/折叠状态，不导航
//   3. 区分 1/2 的关键：单击启动 200ms 定时器，期间发生第二次按下就
//      取消定时器并改走双击逻辑（"click disambiguation"）。
//   4. 懒加载：初始构建只展开 2 层，更深的目录在 itemExpanded 时按需读取。
//      避免一开始就 walk 整个文件系统（在 / 下能有几千项）。
//   5. 高亮：左面板和右面板各高亮一项（蓝/绿色）反映当前目录。
//
// 不做：远程目录树。当前 DirectoryTree 仅显示本地文件系统；连接 SFTP 后
// 树本身不变，浏览体验完全依赖右侧 FilePanel + PathBar。
// =============================================================================

#include "Types.h"
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QString>
#include <QMap>
#include <QTimer>

class DirectoryTree : public QTreeWidget {
    Q_OBJECT

public:
    explicit DirectoryTree(QWidget* parent = nullptr);

    void buildTree();    // 清空 + 从根 "/" 重新构建初始两层
    void highlightPath(const QString& path, PanelSide side);   // 蓝/绿色高亮
    void clearHighlight(PanelSide side);

    /// 按当前 m_activeSide 的 accent（左黄/右绿）刷新 QTreeWidget::item:selected
    /// 的背景色——保证目录树"选择条"颜色与激活 tab 颜色一致。
    void applyActiveAccentStyle();
    void expandToPath(const QString& path);   // 把所有祖先节点展开（懒加载触发）

    /// 切换 active 面板时调用：当左右高亮指向同一项时，会按 active 侧颜色
    /// 重新着色（避免被另一侧颜色"覆盖"，让目录树颜色与 active tab 颜色一致）。
    void setActiveSide(PanelSide side);

    /// 面板激活时调用：展开到 path 对应项、滚动到视图中心、setCurrentItem。
    /// 远程/不在树中的路径静默跳过。不会触发 directorySelected 信号。
    void revealPath(const QString& path);

    /// 切换 Show/Hide 隐藏文件后调用：在不破坏展开/滚动/选中状态的前提下，
    /// 对所有"已加载"节点的 children 做增量同步（隐藏目录条目按当前
    /// RealFileSystem::showHidden() 出现或消失）。未加载节点不动 ——
    /// 用户下次展开时 lazyPopulate 会按当前 filter 自然加载。
    void refreshHiddenVisibility();

signals:
    void directorySelected(const QString& path);

private slots:
    void onItemClicked(QTreeWidgetItem* item, int column);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onItemExpanded(QTreeWidgetItem* item);    // 懒加载触发点
    void onClickTimeout();                         // 200ms 后真正发出 directorySelected

private:
    void populateChildren(QTreeWidgetItem* parentItem, const QString& parentPath, int depth);
    void lazyPopulate(QTreeWidgetItem* item);  // 把 "Loading..." 占位符替换为真实子项
    QString getPathForItem(QTreeWidgetItem* item) const;
    bool isLoaded(QTreeWidgetItem* item) const;
    void setLoaded(QTreeWidgetItem* item, bool loaded);

    // 增量刷新辅助（仅 refreshHiddenVisibility 使用）
    bool isAncestorOf_(QTreeWidgetItem* ancestor, QTreeWidgetItem* descendant) const;
    void removeFromPathMap_(QTreeWidgetItem* node);

    // QTreeWidgetItem::data 用的 role
    static const int ROLE_PATH = Qt::UserRole;
    static const int ROLE_LOADED = Qt::UserRole + 1;
    static const int MAX_INITIAL_DEPTH = 2;        // 初始展开层数

    QMap<QString, QTreeWidgetItem*> m_pathToItem;  // 反查：路径 → 树节点
    QString m_leftHighlight;
    QString m_rightHighlight;
    PanelSide m_activeSide = PanelSide::Left;  // 决定左右指向同一项时显示谁的颜色
    // ROLLBACK: 左侧原为天蓝 #4FC3F7 (QColor(79, 195, 247))。如需回滚把
    // 下一行换回：QColor m_leftColor{79, 195, 247};
    QColor m_leftColor{235, 203, 139};   // #EBCB8B 左面板 Nord aurora 黄
    QColor m_rightColor{163, 190, 140};  // #A3BE8C 右面板 Nord aurora 绿

    // 单/双击歧义消解：200ms 定时器
    QTimer* m_clickTimer = nullptr;
    QTreeWidgetItem* m_pendingItem = nullptr;
    bool m_doubleClickGuard = false;  // 双击后的"幽灵第二次 itemClicked"屏蔽

    // 单击瞬间记录展开状态，让后续双击的 toggle 操作基于"单击前"的状态
    // （而不是被信号链改了之后的状态），保证双击 = 单纯翻转展开。
    QTreeWidgetItem* m_lastClickedItem = nullptr;
    bool m_lastClickedExpandedSnapshot = false;
};

#endif // DIRECTORYTREE_H
