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
#include <QScrollBar>
#include <QRect>
#include <QFileInfo>

// 把外部传入的 path 规范化成 m_pathToItem 里使用的 key 形式：
//   - 展开开头的 ~/
//   - 去掉尾随的 '/'（根 "/" 除外）
//   - 折叠重复的 '/'
// 不会做符号链接解析（避免阻塞）；树本身按 RealFileSystem 列出的路径建索引，
// 一般已经是规范的，问题主要出在外部输入（快捷方式、用户手填等）。
static QString normalizeTreeKey(const QString& raw) {
    if (raw.isEmpty()) return raw;
    QString p = raw;
    if (p.startsWith("~/")) p = QDir::homePath() + p.mid(1);
    while (p.contains("//")) p.replace("//", "/");
    if (p.size() > 1 && p.endsWith('/')) p.chop(1);
    return p;
}
#include <QTimer>
#include <QFontMetrics>
#include <QTreeWidgetItemIterator>
#include <QSet>

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

    // 初始按 m_activeSide（默认 Left=黄）应用选择条配色，避免出现全局蓝。
    applyActiveAccentStyle();
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

void DirectoryTree::highlightPath(const QString& rawPath, PanelSide side) {
    const QString path = normalizeTreeKey(rawPath);
    // Clear previous highlight for this side
    clearHighlight(side);

    if (side == PanelSide::Left) {
        m_leftHighlight = path;
    } else {
        m_rightHighlight = path;
    }

    // Apply highlight。规则：左右两侧的目标若是同一项，
    // 按 m_activeSide 取色（active tab 的颜色优先），保证目录树与 tab 颜色一致；
    // 否则各自按本侧颜色着色。
    if (m_pathToItem.contains(path)) {
        QTreeWidgetItem* item = m_pathToItem[path];
        const bool sameAsOther = (side == PanelSide::Left)
            ? (m_rightHighlight == path)
            : (m_leftHighlight == path);
        PanelSide colorSide = side;
        if (sameAsOther) colorSide = m_activeSide;  // 同一项 → 取 active 侧颜色
        QColor color = (colorSide == PanelSide::Left) ? m_leftColor : m_rightColor;
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
        // 若该项仍被另一侧持有，则不要清掉颜色，只是把本侧 highlight key 置空，
        // 然后按对侧颜色重染（避免单侧 clear 把"另一侧仍指向该项"的高亮也擦掉）。
        const PanelSide other = (side == PanelSide::Left) ? PanelSide::Right : PanelSide::Left;
        const QString& otherKey = (other == PanelSide::Left) ? m_leftHighlight : m_rightHighlight;
        if (otherKey == highlight) {
            QColor color = (other == PanelSide::Left) ? m_leftColor : m_rightColor;
            item->setForeground(0, QBrush(color));
            // bold 保持
        } else {
            item->setForeground(0, QBrush());
            QFont font = item->font(0);
            font.setBold(false);
            item->setFont(0, font);
        }
    }
    highlight.clear();
}

void DirectoryTree::setActiveSide(PanelSide side) {
    if (m_activeSide == side) return;
    m_activeSide = side;
    // 当左右指向同一项时，按新 active 侧颜色重新染该项。
    if (!m_leftHighlight.isEmpty() && m_leftHighlight == m_rightHighlight
        && m_pathToItem.contains(m_leftHighlight)) {
        QTreeWidgetItem* item = m_pathToItem[m_leftHighlight];
        QColor color = (side == PanelSide::Left) ? m_leftColor : m_rightColor;
        item->setForeground(0, QBrush(color));
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
    }
    // 让"选择条"的背景色跟随激活侧（黄/绿），与 tab 颜色保持一致
    applyActiveAccentStyle();
}

// =============================================================================
// applyActiveAccentStyle —— 按 m_activeSide 刷新选择条背景色
//
// 背景：MainWindow 全局样式表把 QTreeWidget::item:selected 的背景固定成
// Nord 蓝（rgba(136,192,208,0.28)）。而我们希望目录树的"选择条"颜色与
// 当前激活的 tab 同色（左=黄 #EBCB8B / 右=绿 #A3BE8C）。
//
// 做法：在本控件局部 setStyleSheet —— Qt 局部样式优先于全局，把
// item:selected / item:selected:!active（失焦时仍保持选中色）覆盖。
// 用 rgba(0.28) 透明度匹配原视觉密度，避免太刺眼。
// =============================================================================
void DirectoryTree::applyActiveAccentStyle() {
    QColor base = (m_activeSide == PanelSide::Left) ? m_leftColor : m_rightColor;
    // 选择条背景：accent 色 + 28% 透明度（与原蓝色密度一致）
    QString selBg = QString("rgba(%1, %2, %3, 0.28)")
                        .arg(base.red()).arg(base.green()).arg(base.blue());
    // hover：accent 色 + 10% 透明度
    QString hoverBg = QString("rgba(%1, %2, %3, 0.10)")
                          .arg(base.red()).arg(base.green()).arg(base.blue());

    setStyleSheet(QString(
        "QTreeWidget { background-color: #2E3440; color: #D8DEE9; border: 1px solid #434C5E; outline: 0; }"
        "QTreeWidget::item { padding: 3px 4px; border: none; }"
        "QTreeWidget::item:hover { background-color: %1; }"
        "QTreeWidget::item:selected { background-color: %2; color: #ECEFF4; }"
        "QTreeWidget::item:selected:!active { background-color: %2; color: #ECEFF4; }"
    ).arg(hoverBg, selBg));
}

void DirectoryTree::expandToPath(const QString& rawPath) {
    const QString path = normalizeTreeKey(rawPath);
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
void DirectoryTree::revealPath(const QString& rawPath) {
    if (rawPath.isEmpty()) return;
    // 远程/非本地 scheme：本树仅管本地文件系统，直接跳过。
    if (rawPath.startsWith("sftp://") || rawPath.startsWith("ftp://") ||
        rawPath.startsWith("ssh://")  || rawPath.startsWith("smb://") ||
        rawPath.startsWith("http://") || rawPath.startsWith("https://")) {
        return;
    }
    const QString path = normalizeTreeKey(rawPath);
    expandToPath(path);
    auto it = m_pathToItem.find(path);
    if (it == m_pathToItem.end()) return;
    QTreeWidgetItem* item = it.value();

    QSignalBlocker blocker(this);
    setCurrentItem(item);
    scrollToItem(item, QAbstractItemView::PositionAtCenter);

    // 水平 + 垂直居中（延迟一轮事件循环）
    //
    // 顺序非常关键：
    //   1) 先 scrollToItem(PositionAtCenter) —— 它会做垂直居中，**同时**
    //      还会做一次水平的 "auto ensure-visible"，把项拉到视口靠左可见。
    //      这一步必须最先做，否则随后我们设置的水平 value 会被它覆盖。
    //   2) 再手动 setValue 修正水平滚动 —— 水平 setValue 只影响水平位置，
    //      不会改变垂直位置，所以不会破坏步骤 1 的垂直居中。
    //
    // 为什么放在 singleShot(0) 里：scrollToItem 触发水平滚动条
    // AsNeeded 的出现/隐藏会改变 viewport 高度 → 触发布局。延迟到下一轮
    // 事件循环可以让布局稳定，visualRect / hbar->maximum() 返回最终值。
    //
    // **重要：lambda 里不可以捕获 QTreeWidgetItem* —— 那个 item 可能在
    // 下一帧执行前因 lazyPopulate / rebuild / 节点删除等原因被销毁，
    // 触发 EXC_BAD_ACCESS（曾在崩溃报告中 indexFromItem(野指针) → SIGSEGV）。
    // 改捕 path 字符串，回调里通过 m_pathToItem.find(path) 重新查找当前
    // 仍存活的 item；找不到就静默返回。**
    QTimer::singleShot(0, this, [this, path]() {
        auto it = m_pathToItem.find(path);
        if (it == m_pathToItem.end()) return;
        QTreeWidgetItem* freshItem = it.value();
        if (!freshItem) return;

        QModelIndex idx = indexFromItem(freshItem, 0);
        if (!idx.isValid()) return;

        // 第 1 步：再次强制垂直居中（+ Qt 内部的水平可见调整）。
        scrollToItem(freshItem, QAbstractItemView::PositionAtCenter);

        // 第 2 步：用 setValue 覆盖 Qt 的水平定位，做真·水平居中。
        auto* hbar = horizontalScrollBar();
        if (!hbar || hbar->maximum() <= 0) return;

        QRect vr = visualRect(idx);
        const int textStartX = vr.left();
        const int textPixWidth =
            fontMetrics().horizontalAdvance(freshItem->text(0)) + 24;
        const int itemCenterInViewport = textStartX + textPixWidth / 2;
        const int viewportW = viewport()->width();
        const int delta = itemCenterInViewport - viewportW / 2;
        const int newVal = qBound(hbar->minimum(),
                                  hbar->value() + delta,
                                  hbar->maximum());
        hbar->setValue(newVal);
    });
}

// =============================================================================
// refreshHiddenVisibility ——切换 Show/Hide 隐藏文件后的"增量同步"
//
// 目标：
//   - 展开/滚动/选中状态完全保持
//   - 已加载节点的 children 跟随当前 showHidden 状态增删
//   - 未加载节点不动（下次展开时 lazyPopulate 会按当前 filter 拉取）
//
// 算法：
//   1) 阻塞信号 + 暂停重绘，记录 vScroll/hScroll 与 currentItem 的 path
//   2) 遍历整棵树（仅处理 isLoaded=true 的节点）：
//        - 取该节点的 path，调 fs.getSubDirectories(path) 拿当前 filter
//          下应有的 children 路径集合 newSet
//        - 当前 children 中对应 path 集合 oldSet
//        - 删除 (oldSet - newSet) 的节点，并清理 m_pathToItem 映射；
//          被删节点是当前选中或祖先 → 选中转移到最近的可见祖先
//        - 插入 (newSet - oldSet) 的节点；插入位置按 newSet 的顺序
//          （fs.getSubDirectories 已排序）保持有序
//   3) 恢复 currentItem 与滚动位置
//
// 注意：getSubDirectories 已返回完整绝对路径列表，且其内部读取
// m_showHidden，调用方无需额外传 filter。
// =============================================================================
void DirectoryTree::refreshHiddenVisibility() {
    QSignalBlocker blocker(this);
    setUpdatesEnabled(false);

    // 记录滚动位置 & 当前选中
    auto* vbar = verticalScrollBar();
    auto* hbar = horizontalScrollBar();
    const int savedV = vbar ? vbar->value() : 0;
    const int savedH = hbar ? hbar->value() : 0;

    QTreeWidgetItem* curItem = currentItem();
    QString savedCurPath = curItem ? getPathForItem(curItem) : QString();

    auto& fs = RealFileSystem::instance();

    // 把所有已加载节点先收集到一个列表（避免边遍历边改）
    QList<QTreeWidgetItem*> loadedNodes;
    {
        QTreeWidgetItemIterator it(this);
        while (*it) {
            QTreeWidgetItem* node = *it;
            if (isLoaded(node)) {
                loadedNodes.append(node);
            }
            ++it;
        }
    }

    for (QTreeWidgetItem* parentItem : loadedNodes) {
        const QString parentPath = getPathForItem(parentItem);
        if (parentPath.isEmpty()) continue;

        // 当前 filter 下该节点应有的子目录路径列表（已排序）
        const QStringList desired = fs.getSubDirectories(parentPath);
        QSet<QString> desiredSet(desired.begin(), desired.end());

        // 现有 children 的 path 集合（跳过 "Loading..." 占位符）
        QMap<QString, QTreeWidgetItem*> existing;
        for (int i = 0; i < parentItem->childCount(); ++i) {
            QTreeWidgetItem* child = parentItem->child(i);
            QString cp = getPathForItem(child);
            if (cp.isEmpty()) continue;  // placeholder
            existing[cp] = child;
        }

        // ---- 1) 删除 existing - desired ----
        for (auto eit = existing.begin(); eit != existing.end(); ++eit) {
            if (desiredSet.contains(eit.key())) continue;

            QTreeWidgetItem* toRemove = eit.value();

            // 若被删节点本身或其后代是当前选中 → 把选中转到最近可见祖先
            if (curItem && (curItem == toRemove ||
                            isAncestorOf_(toRemove, curItem))) {
                curItem = parentItem;
                savedCurPath = parentPath;
            }

            // 清理 m_pathToItem（包括所有后代）
            removeFromPathMap_(toRemove);

            // 从树中移除并 delete
            int idx = parentItem->indexOfChild(toRemove);
            if (idx >= 0) {
                delete parentItem->takeChild(idx);
            }
        }

        // ---- 2) 插入 desired - existing（按 desired 顺序，保证排序一致）----
        for (int i = 0; i < desired.size(); ++i) {
            const QString& dirPath = desired[i];
            if (existing.contains(dirPath)) continue;  // 已有，跳过

            QString name = dirPath.mid(dirPath.lastIndexOf('/') + 1);
            auto* item = new QTreeWidgetItem();
            item->setText(0, name);
            item->setData(0, ROLE_PATH, dirPath);
            item->setData(0, ROLE_LOADED, false);
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            m_pathToItem[dirPath] = item;

            // 添加占位符让展开三角显示
            QStringList grandchildren = fs.getSubDirectories(dirPath);
            if (!grandchildren.isEmpty()) {
                auto* placeholder = new QTreeWidgetItem(item);
                placeholder->setText(0, "Loading...");
            }

            // 插入到正确位置（按 desired 顺序）
            // 需要找到 desired[i] 在 parentItem 当前 children 中应处的位置
            int insertAt = parentItem->childCount();  // 默认插到末尾
            for (int j = 0; j < parentItem->childCount(); ++j) {
                QString cp = getPathForItem(parentItem->child(j));
                if (cp.isEmpty()) continue;  // placeholder
                // 找到第一个 desired 序号比当前更大的位置
                int otherIdx = desired.indexOf(cp);
                if (otherIdx > i) {
                    insertAt = j;
                    break;
                }
            }
            parentItem->insertChild(insertAt, item);
        }
    }

    // ---- 3) 恢复高亮（防止增量过程中样式被擦除）----
    // m_pathToItem 可能因删除而失效；highlightPath 内部会判断存在性。
    if (!m_leftHighlight.isEmpty()) {
        QString tmp = m_leftHighlight;
        m_leftHighlight.clear();   // 强制 highlightPath 重新应用
        highlightPath(tmp, PanelSide::Left);
    }
    if (!m_rightHighlight.isEmpty()) {
        QString tmp = m_rightHighlight;
        m_rightHighlight.clear();
        highlightPath(tmp, PanelSide::Right);
    }

    // ---- 4) 恢复 currentItem ----
    if (!savedCurPath.isEmpty()) {
        auto it = m_pathToItem.find(savedCurPath);
        if (it != m_pathToItem.end()) {
            setCurrentItem(it.value());
        }
    }

    // ---- 5) 恢复滚动位置 ----
    if (vbar) vbar->setValue(savedV);
    if (hbar) hbar->setValue(savedH);

    setUpdatesEnabled(true);
}

// 辅助：判断 ancestor 是否为 descendant 的（直接或间接）父节点
bool DirectoryTree::isAncestorOf_(QTreeWidgetItem* ancestor,
                                  QTreeWidgetItem* descendant) const {
    if (!ancestor || !descendant) return false;
    QTreeWidgetItem* p = descendant->parent();
    while (p) {
        if (p == ancestor) return true;
        p = p->parent();
    }
    return false;
}

// 辅助：从 m_pathToItem 中移除 node 自身及其所有后代条目（不 delete 节点，
// 仅清理映射），用于删除子树前防止悬挂指针。
void DirectoryTree::removeFromPathMap_(QTreeWidgetItem* node) {
    if (!node) return;
    QString path = getPathForItem(node);
    if (!path.isEmpty()) {
        auto it = m_pathToItem.find(path);
        if (it != m_pathToItem.end() && it.value() == node) {
            m_pathToItem.erase(it);
        }
    }
    for (int i = 0; i < node->childCount(); ++i) {
        removeFromPathMap_(node->child(i));
    }
}
