// =============================================================================
// DirectoryTree.cpp —— 见 DirectoryTree.h
//
// 设计要点：单/双击歧义消解 + 懒加载。"Loading..." 占位符是为了让目录
// 项显示展开三角，但避免预读取所有孙子节点。
// =============================================================================

#include "DirectoryTree.h"
#include "RealFileSystem.h"
#include <QHeaderView>
#include <QDir>
#include <QSignalBlocker>
#include <QAbstractItemView>

DirectoryTree::DirectoryTree(QWidget* parent)
    : QTreeWidget(parent)
{
    setHeaderLabel("Directories");
    setAnimated(true);
    setIndentation(16);
    setExpandsOnDoubleClick(false);
    header()->setVisible(false);

    // 滚动条：水平/垂直均 AsNeeded —— 视口放得下时隐藏，放不下时出现。
    // macOS 原生样式下表现为 overlay（滚动时才浮现），符合"平时不显示"预期。
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 按像素滚动比按 item 滚动更平滑，横向尤其明显。
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 默认 QTreeWidget 在列宽不足时会裁剪文字（"...")而不触发水平滚动。
    // 让 header 按内容尺寸伸展，使超长文件名真正把内容宽度撑大，
    // 从而在需要时让横向滚动条可用。
    header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    header()->setStretchLastSection(false);
    // 关闭文字省略，让完整文件名参与内容宽度计算。
    setTextElideMode(Qt::ElideNone);

    // 200ms protection timer for click disambiguation
    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    m_clickTimer->setInterval(200);
    connect(m_clickTimer, &QTimer::timeout, this, &DirectoryTree::onClickTimeout);

    connect(this, &QTreeWidget::itemClicked, this, &DirectoryTree::onItemClicked);
    connect(this, &QTreeWidget::itemDoubleClicked, this, &DirectoryTree::onItemDoubleClicked);
    connect(this, &QTreeWidget::itemExpanded, this, &DirectoryTree::onItemExpanded);

    buildTree();
}

void DirectoryTree::buildTree() {
    clear();
    m_pathToItem.clear();

    auto* rootItem = new QTreeWidgetItem(this);
    rootItem->setText(0, "/");
    rootItem->setData(0, ROLE_PATH, "/");
    rootItem->setData(0, ROLE_LOADED, false);
    rootItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    m_pathToItem["/"] = rootItem;

    // Populate root and first 2 levels
    populateChildren(rootItem, "/", 0);
    setLoaded(rootItem, true);
    rootItem->setExpanded(true);
}

void DirectoryTree::populateChildren(QTreeWidgetItem* parentItem, const QString& parentPath, int depth) {
    auto& fs = RealFileSystem::instance();
    QStringList subdirs = fs.getSubDirectories(parentPath);

    for (const auto& dirPath : subdirs) {
        QString name = dirPath.mid(dirPath.lastIndexOf('/') + 1);
        auto* item = new QTreeWidgetItem(parentItem);
        item->setText(0, name);
        item->setData(0, ROLE_PATH, dirPath);
        item->setData(0, ROLE_LOADED, false);
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        m_pathToItem[dirPath] = item;

        if (depth < MAX_INITIAL_DEPTH - 1) {
            // Recursively populate within depth limit
            populateChildren(item, dirPath, depth + 1);
            setLoaded(item, true);
        } else {
            // Add a placeholder child so the expand arrow shows
            QStringList childDirs = fs.getSubDirectories(dirPath);
            if (!childDirs.isEmpty()) {
                auto* placeholder = new QTreeWidgetItem(item);
                placeholder->setText(0, "Loading...");
            }
        }
    }
}

void DirectoryTree::lazyPopulate(QTreeWidgetItem* item) {
    if (isLoaded(item)) return;

    // Remove placeholder children
    while (item->childCount() > 0) {
        delete item->takeChild(0);
    }

    QString path = getPathForItem(item);
    auto& fs = RealFileSystem::instance();
    QStringList subdirs = fs.getSubDirectories(path);

    for (const auto& dirPath : subdirs) {
        QString name = dirPath.mid(dirPath.lastIndexOf('/') + 1);
        auto* childItem = new QTreeWidgetItem(item);
        childItem->setText(0, name);
        childItem->setData(0, ROLE_PATH, dirPath);
        childItem->setData(0, ROLE_LOADED, false);
        childItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        m_pathToItem[dirPath] = childItem;

        // Add placeholder if this directory has subdirectories
        QStringList grandchildren = fs.getSubDirectories(dirPath);
        if (!grandchildren.isEmpty()) {
            auto* placeholder = new QTreeWidgetItem(childItem);
            placeholder->setText(0, "Loading...");
        }
    }

    setLoaded(item, true);
}

void DirectoryTree::onItemExpanded(QTreeWidgetItem* item) {
    lazyPopulate(item);
}

QString DirectoryTree::getPathForItem(QTreeWidgetItem* item) const {
    return item->data(0, ROLE_PATH).toString();
}

bool DirectoryTree::isLoaded(QTreeWidgetItem* item) const {
    return item->data(0, ROLE_LOADED).toBool();
}

void DirectoryTree::setLoaded(QTreeWidgetItem* item, bool loaded) {
    item->setData(0, ROLE_LOADED, loaded);
}

void DirectoryTree::onItemClicked(QTreeWidgetItem* item, int /*column*/) {
    // After a doubleClick, Qt fires itemClicked again for the 2nd release —
    // ignore that ghost click so we don't navigate after a double-click.
    if (m_doubleClickGuard) {
        m_doubleClickGuard = false;
        return;
    }

    QString path = getPathForItem(item);
    if (path.isEmpty()) return;

    // Defer navigation by 200ms so a double-click within that window can
    // cancel it (double-click should JUST toggle expand/collapse, not navigate).
    // Also snapshot the expand state so the double-click handler can toggle
    // relative to the state BEFORE any signal-chain expansion happened.
    m_pendingItem = item;
    m_lastClickedItem = item;
    m_lastClickedExpandedSnapshot = item->isExpanded();
    m_clickTimer->start();
}

void DirectoryTree::onItemDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) return;

    // Cancel the deferred single-click navigation
    m_clickTimer->stop();
    m_pendingItem = nullptr;
    m_doubleClickGuard = true;  // suppress the ghost itemClicked from 2nd release

    // Toggle expand state relative to whatever it was at first click.
    bool wasExpandedAtFirstClick =
        (item == m_lastClickedItem) ? m_lastClickedExpandedSnapshot : item->isExpanded();
    item->setExpanded(!wasExpandedAtFirstClick);

    m_lastClickedItem = nullptr;
}

void DirectoryTree::onClickTimeout() {
    // 200ms passed without a second click — emit navigation now.
    if (!m_pendingItem) return;
    QTreeWidgetItem* item = m_pendingItem;
    m_pendingItem = nullptr;

    QString path = getPathForItem(item);
    if (path.isEmpty()) return;

    emit directorySelected(path);
}

void DirectoryTree::highlightPath(const QString& path, PanelSide side) {
    // Clear previous highlight for this side
    clearHighlight(side);

    if (side == PanelSide::Left) {
        m_leftHighlight = path;
    } else {
        m_rightHighlight = path;
    }

    // Apply highlight
    if (m_pathToItem.contains(path)) {
        QTreeWidgetItem* item = m_pathToItem[path];
        QColor color = (side == PanelSide::Left) ? m_leftColor : m_rightColor;
        item->setForeground(0, QBrush(color));
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
    }
}

void DirectoryTree::clearHighlight(PanelSide side) {
    QString& highlight = (side == PanelSide::Left) ? m_leftHighlight : m_rightHighlight;
    if (!highlight.isEmpty() && m_pathToItem.contains(highlight)) {
        QTreeWidgetItem* item = m_pathToItem[highlight];
        item->setForeground(0, QBrush());
        QFont font = item->font(0);
        font.setBold(false);
        item->setFont(0, font);
    }
    highlight.clear();
}

void DirectoryTree::expandToPath(const QString& path) {
    // Expand all ancestors, lazy-loading as needed
    QStringList parts = path.split('/', Qt::SkipEmptyParts);
    QString current = "/";

    if (m_pathToItem.contains(current)) {
        lazyPopulate(m_pathToItem[current]);
        m_pathToItem[current]->setExpanded(true);
    }

    for (const auto& part : parts) {
        current = (current == "/") ? "/" + part : current + "/" + part;
        if (m_pathToItem.contains(current)) {
            lazyPopulate(m_pathToItem[current]);
            m_pathToItem[current]->setExpanded(true);
        } else {
            // Path not yet in tree, stop
            break;
        }
    }
}

// 展开到 path 对应项 + 滚动到视图中心 + setCurrentItem。
// 用于面板激活时让左侧树跟随定位。
// 用 QSignalBlocker 暂时阻断本树信号，防止 setCurrentItem 触发 itemClicked
// 进而回调 navigateTo 形成重入。
void DirectoryTree::revealPath(const QString& path) {
    if (path.isEmpty()) return;
    // 远程/非本地 scheme：本树仅管本地文件系统，直接跳过。
    if (path.startsWith("sftp://") || path.startsWith("ftp://") ||
        path.startsWith("ssh://")  || path.startsWith("smb://") ||
        path.startsWith("http://") || path.startsWith("https://")) {
        return;
    }
    expandToPath(path);
    auto it = m_pathToItem.find(path);
    if (it == m_pathToItem.end()) return;
    QTreeWidgetItem* item = it.value();

    QSignalBlocker blocker(this);
    setCurrentItem(item);
    scrollToItem(item, QAbstractItemView::PositionAtCenter);
}
