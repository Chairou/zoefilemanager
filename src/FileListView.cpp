// =============================================================================
// FileListView.cpp —— 见 FileListView.h
// =============================================================================

#include "FileListView.h"
#include "FileSystemRouter.h"
#include "I18n.h"
#include <QHeaderView>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFont>
#include <QSet>
#include <QDesktopServices>
#include <QUrl>
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPalette>
#include <algorithm>

// 同应用 drag 标识本应用自己产生的拖拽数据，让 drop 端可以保留 SFTP URL 等
// 标准 file:// URL 装不下的格式。格式：UTF-8、每行一条绝对路径或 sftp:// URL。
static constexpr const char* kZoeMime = "application/x-zoe-fileentries";

FileListView::FileListView(QWidget* parent)
    : QTableWidget(parent)
{
    setupColumns();
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setShowGrid(false);
    setAlternatingRowColors(true);
    verticalHeader()->setVisible(false);
    verticalHeader()->setDefaultSectionSize(25);  // 28 - 10% → 25
    verticalHeader()->setMinimumSectionSize(20);
    // 关闭"最后一列拉满"，让总列宽是各列的实际设置宽之和；
    // 这样当视口窄于总列宽时会触发水平滚动条，反之隐藏。
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);

    // 滚动条：AsNeeded —— 平时不显示，内容/列宽 > 视口时自动出现。
    // macOS 下自动走系统 overlay 样式。
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 关闭 Name 列的省略号，让长文件名真实撑大内容宽度；
    // 这样横向滚动条才有意义可触发。
    setTextElideMode(Qt::ElideNone);
    setWordWrap(false);

    // We drive sorting ourselves (to keep . / .. / directory-first rules).
    // Don't enable QTableWidget's built-in sort — just use the header's
    // sort indicator and handle section clicks.
    setSortingEnabled(false);
    horizontalHeader()->setSectionsClickable(true);
    horizontalHeader()->setSortIndicatorShown(true);
    horizontalHeader()->setSortIndicator(m_sortColumn, m_sortOrder);

    setFocusPolicy(Qt::StrongFocus);

    connect(this, &QTableWidget::cellDoubleClicked, this, &FileListView::onCellDoubleClicked);
    connect(this, &QTableWidget::customContextMenuRequested, this, &FileListView::onCustomContextMenu);
    connect(horizontalHeader(), &QHeaderView::sectionClicked, this, &FileListView::onHeaderClicked);

    connect(&I18n::instance(), &I18n::changed, this, &FileListView::retranslate);
}

void FileListView::retranslate() {
    setHorizontalHeaderLabels({T("Name"), T("Size"), T("Modified"), T("Permissions")});
}

void FileListView::setupColumns() {
    setColumnCount(4);
    setHorizontalHeaderLabels({T("Name"), T("Size"), T("Modified"), T("Permissions")});
    horizontalHeader()->resizeSection(0, 300);
    horizontalHeader()->resizeSection(1, 100);
    horizontalHeader()->resizeSection(2, 150);
    horizontalHeader()->resizeSection(3, 120);
}

void FileListView::setEntries(const QVector<FileEntry>& entries, const QString& currentPath) {
    m_currentPath = currentPath;
    m_lastClickedRow = -1;

    // Store raw entries (without synthetic . / ..) so we can re-sort without re-fetching
    m_rawEntries = entries;

    rebuildRows();
}

void FileListView::sortEntries(QVector<FileEntry>& entries) const {
    auto cmp = [this](const FileEntry& a, const FileEntry& b) -> bool {
        bool asc = (m_sortOrder == Qt::AscendingOrder);
        int diff = 0;

        switch (m_sortColumn) {
            case SortByName:
                diff = QString::compare(a.name, b.name, Qt::CaseInsensitive);
                break;
            case SortBySize:
                if (a.size < b.size) diff = -1;
                else if (a.size > b.size) diff = 1;
                else diff = QString::compare(a.name, b.name, Qt::CaseInsensitive);
                break;
            case SortByModified:
                if (a.modified < b.modified) diff = -1;
                else if (a.modified > b.modified) diff = 1;
                else diff = QString::compare(a.name, b.name, Qt::CaseInsensitive);
                break;
            case SortByPermissions:
                diff = QString::compare(a.permissions, b.permissions);
                if (diff == 0) diff = QString::compare(a.name, b.name, Qt::CaseInsensitive);
                break;
        }
        return asc ? (diff < 0) : (diff > 0);
    };

    std::sort(entries.begin(), entries.end(), cmp);
}

void FileListView::rebuildRows() {
    // Preserve selection by remembering currently-selected paths
    QSet<QString> selectedPaths;
    for (int i = 0; i < rowCount(); ++i) {
        if (item(i, 0) && item(i, 0)->isSelected() && i < m_entries.size()) {
            selectedPaths.insert(m_entries[i].path);
        }
    }

    m_entries.clear();

    // Split into directories and files; each group is sorted independently
    // so directories always come before files (familiar file-manager UX).
    QVector<FileEntry> dirs, files;
    for (const auto& e : m_rawEntries) {
        if (e.isDirectory) dirs.append(e);
        else files.append(e);
    }
    sortEntries(dirs);
    sortEntries(files);

    // "." is always first; ".." (if applicable) second
    FileEntry dotEntry;
    dotEntry.name = ".";
    dotEntry.path = m_currentPath;
    dotEntry.isDirectory = true;
    dotEntry.type = FileType::Directory;
    m_entries.append(dotEntry);

    if (m_currentPath != "/") {
        // For remote mounts ("sftp://user@host:22/..."), ".." must stop at
        // the mount root - going further up would walk into the URL scheme
        // bits and produce a non-routable path.
        QString mount = FileSystemRouter::instance().mountFor(m_currentPath);
        QString mountRoot = mount.isEmpty() ? QString() : (mount + "/");
        const bool atMountRoot = !mount.isEmpty() &&
                                 (m_currentPath == mountRoot ||
                                  m_currentPath == mount);
        if (!atMountRoot) {
            FileEntry dotDotEntry;
            dotDotEntry.name = "..";
            QString parent = m_currentPath.left(m_currentPath.lastIndexOf('/'));
            if (parent.isEmpty()) parent = "/";
            // If parent equals the bare mount prefix, normalize it back to
            // "<mount>/" so the router still routes it to SFTP.
            if (!mount.isEmpty() && parent == mount) parent += "/";
            dotDotEntry.path = parent;
            dotDotEntry.isDirectory = true;
            dotDotEntry.type = FileType::Directory;
            m_entries.append(dotDotEntry);
        }
    }

    m_entries.append(dirs);
    m_entries.append(files);

    // Populate table
    setRowCount(m_entries.size());
    for (int i = 0; i < m_entries.size(); ++i) {
        const auto& entry = m_entries[i];

        auto* nameItem = new QTableWidgetItem(getIconText(entry) + " " + entry.name);
        nameItem->setData(Qt::UserRole, i);

        // Color coding by file type — Nord palette
        QColor nameColor;
        if (entry.isDirectory) {
            nameColor = QColor("#88C0D0"); // Nord8 frost — directories
            // 目录名颜色已经与文件区分（Nord8 frost vs 各种暖色），不再额外
            // 加粗，与普通文件保持同样的字重，列表整体更平。
        } else {
            switch (entry.type) {
                case FileType::Image:    nameColor = QColor("#B48EAD"); break; // Nord15 purple
                case FileType::Code:     nameColor = QColor("#A3BE8C"); break; // Nord14 green
                case FileType::Script:   nameColor = QColor("#A3BE8C"); break; // Nord14 green
                case FileType::Archive:  nameColor = QColor("#D08770"); break; // Nord12 orange
                case FileType::Audio:    nameColor = QColor("#B48EAD"); break; // Nord15 purple
                case FileType::Video:    nameColor = QColor("#BF616A"); break; // Nord11 red
                case FileType::Document: nameColor = QColor("#81A1C1"); break; // Nord9 pale-blue
                case FileType::Text:     nameColor = QColor("#E5E9F0"); break; // Nord5 snow
                case FileType::Config:   nameColor = QColor("#EBCB8B"); break; // Nord13 yellow
                default:                 nameColor = QColor("#D8DEE9"); break; // Nord4 snow
            }
        }
        nameItem->setForeground(QBrush(nameColor));
        setItem(i, 0, nameItem);

        auto* sizeItem = new QTableWidgetItem(entry.isDirectory ? "<DIR>" : formatSize(entry.size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (entry.isDirectory) {
            sizeItem->setForeground(QBrush(QColor("#88C0D0")));
        } else {
            sizeItem->setForeground(QBrush(QColor("#A3BE8C"))); // Nord green for sizes
        }
        setItem(i, 1, sizeItem);

        auto* dateItem = new QTableWidgetItem(entry.modified.isValid() ? entry.modified.toString("yyyy-MM-dd HH:mm") : "");
        dateItem->setForeground(QBrush(QColor("#81A1C1"))); // Nord9 pale blue for dates
        setItem(i, 2, dateItem);

        auto* permItem = new QTableWidgetItem(entry.permissions);
        permItem->setForeground(QBrush(QColor("#4C566A"))); // Nord3 muted
        setItem(i, 3, permItem);

        // Restore selection for this row if it was previously selected
        if (selectedPaths.contains(entry.path) && entry.name != "." && entry.name != "..") {
            updateRowSelection(i, true);
        }
    }
}

void FileListView::onHeaderClicked(int logicalIndex) {
    if (logicalIndex < 0 || logicalIndex > 3) return;

    if (logicalIndex == m_sortColumn) {
        // Same column — toggle order
        m_sortOrder = (m_sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
    } else {
        // New column — default to ascending
        m_sortColumn = logicalIndex;
        m_sortOrder = Qt::AscendingOrder;
    }

    horizontalHeader()->setSortIndicator(m_sortColumn, m_sortOrder);
    rebuildRows();
}

// 判定 path 是否是我们支持的"app 内解压"格式。仅按文件名后缀判，不嗅探内容
// （足够实用，且与 onArchiveExtract 的命令分发一致）。
static bool isSupportedArchive(const QString& path) {
    const QString p = path.toLower();
    return p.endsWith(".zip")
        || p.endsWith(".tar")
        || p.endsWith(".tar.gz") || p.endsWith(".tgz")
        || p.endsWith(".tar.bz2") || p.endsWith(".tbz") || p.endsWith(".tbz2")
        || p.endsWith(".tar.xz")  || p.endsWith(".txz");
}

void FileListView::onCellDoubleClicked(int row, int /*column*/) {
    if (row < 0 || row >= m_entries.size()) return;
    const auto& entry = m_entries[row];

    if (entry.isDirectory) {
        emit directoryActivated(entry.path);
    } else if (isSupportedArchive(entry.path)
               && !entry.path.startsWith("sftp://")) {
        // 已识别压缩包 + 本地路径 → 走 app 内解压（MainWindow 处理）
        // 远程压缩包暂不支持，落到下面的 openUrl 也没意义；这里直接把 SFTP
        // 排除掉以保持行为明确。
        emit archiveActivated(entry.path);
    } else {
        // 其它文件：交给系统默认应用（macOS Launch Services / Linux xdg-open
        // / Windows ShellExecute 等）
        QDesktopServices::openUrl(QUrl::fromLocalFile(entry.path));
    }
}

void FileListView::mousePressEvent(QMouseEvent* event) {
    // 记录起点 + 判定本次 press 是否可能开启拖拽。
    //
    // 行为（v2）：只要 press 落在一个"真实条目"（非 . / ..）上就允许拖拽 ——
    // 不要求该行已被选中。如果落在未选中行上，立刻把它单独选上作为拖拽
    // 载荷，让用户视觉上"看到自己拖了什么"，且 QDrag 的 mime 数据/pixmap
    // 正确显示。带 Ctrl/Shift 的修饰键 press 维持原 mouseReleaseEvent 里
    // 的多选语义，不要在这里干预。
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        m_pressOnEntry = false;

        int row = rowAt(event->pos().y());
        if (row >= 0 && row < m_entries.size()) {
            const auto& e = m_entries[row];
            const bool isSynthetic = (e.name == "." || e.name == "..");
            if (!isSynthetic) {
                m_pressOnEntry = true;

                const bool ctrl  = event->modifiers() & Qt::ControlModifier;
                const bool shift = event->modifiers() & Qt::ShiftModifier;
                const bool isSelected = item(row, 0) && item(row, 0)->isSelected();

                // 如果 press 落在未选中的行上、且没按 Ctrl/Shift（那两种是
                // 多选/范围选 的修饰键，让 mouseReleaseEvent 处理），就把
                // 这一行单独选上作为拖拽载荷。
                if (!isSelected && !ctrl && !shift) {
                    for (int i = 0; i < rowCount(); ++i) {
                        updateRowSelection(i, i == row);
                    }
                    m_lastClickedRow = row;
                    emit selectionChanged_();
                }
            }
        }
    }
    // 透传给基类，保持 QTableWidget 的焦点/锚点/单元格激活等内建行为
    QTableWidget::mousePressEvent(event);
}

void FileListView::mouseMoveEvent(QMouseEvent* event) {
    // 没按左键，或本次 press 不是从可拖条目开始 —— 走基类默认（用于
    // 鼠标悬浮等行为，本表暂未启用但保留通路）。
    if (!(event->buttons() & Qt::LeftButton) || !m_pressOnEntry) {
        QTableWidget::mouseMoveEvent(event);
        return;
    }
    // 移动距离阈值：低于 startDragDistance 视作"还在原地"，让纯单击不至于
    // 触发拖拽 → 破坏单击选中。
    if ((event->pos() - m_dragStartPos).manhattanLength()
            < QApplication::startDragDistance()) {
        QTableWidget::mouseMoveEvent(event);
        return;
    }

    QVector<FileEntry> selected = getSelectedEntries();
    if (selected.isEmpty()) {
        // 理论上 mousePressEvent 已经把 press 那一行选中了；走到这里说明
        // press 落点变了或被其它逻辑清空，保险起见放弃拖拽。
        QTableWidget::mouseMoveEvent(event);
        return;
    }

    // Build mime data: a same-app custom payload (line-delimited paths,
    // works for both local paths and "sftp://..." URLs) AND a standard
    // text/uri-list so dragging into Finder / other apps works for the
    // local case.
    auto* mime = new QMimeData;
    QStringList lines;
    QList<QUrl> urls;
    for (const auto& e : selected) {
        lines << e.path;
        if (!e.path.startsWith("sftp://")) {
            urls << QUrl::fromLocalFile(e.path);
        }
    }
    mime->setData(kZoeMime, lines.join('\n').toUtf8());
    if (!urls.isEmpty()) mime->setUrls(urls);

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);

    // Tiny pixmap label "<N> items" so the drag has a visual handle.
    const QString label = (selected.size() == 1)
        ? selected.first().name
        : QString("%1 items").arg(selected.size());
    QFontMetrics fm(font());
    QSize sz(fm.horizontalAdvance(label) + 24, fm.height() + 10);
    QPixmap pm(sz);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(59, 66, 82, 230));
        p.setPen(QColor(136, 192, 208));
        p.drawRoundedRect(pm.rect().adjusted(0, 0, -1, -1), 4, 4);
        p.setPen(QColor(236, 239, 244));
        p.drawText(pm.rect(), Qt::AlignCenter, label);
    }
    drag->setPixmap(pm);
    drag->setHotSpot(QPoint(pm.width() / 2, pm.height() / 2));

    // 默认动作 Copy（匹配用户需求：跨面板拖拽 = 复制）。Move 也声明出来
    // 让接收方自己选；本应用 dropEvent 始终走 CopyAction。
    drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
    m_pressOnEntry = false;  // 重置；下一次 press 重新判定
}

void FileListView::mouseReleaseEvent(QMouseEvent* event) {
    int row = rowAt(event->pos().y());
    if (row < 0) {
        QTableWidget::mouseReleaseEvent(event);
        return;
    }

    bool ctrl = event->modifiers() & Qt::ControlModifier;
    bool shift = event->modifiers() & Qt::ShiftModifier;

    if (shift && m_lastClickedRow >= 0) {
        // Range select
        int start = qMin(m_lastClickedRow, row);
        int end = qMax(m_lastClickedRow, row);
        // Clear previous selection first
        for (int i = 0; i < rowCount(); ++i) {
            updateRowSelection(i, false);
        }
        for (int i = start; i <= end; ++i) {
            updateRowSelection(i, true);
        }
    } else if (ctrl) {
        // Toggle selection
        bool currentlySelected = item(row, 0) && item(row, 0)->isSelected();
        updateRowSelection(row, !currentlySelected);
        m_lastClickedRow = row;
    } else {
        // Single select - clear others
        for (int i = 0; i < rowCount(); ++i) {
            updateRowSelection(i, i == row);
        }
        m_lastClickedRow = row;
    }

    emit selectionChanged_();
    QTableWidget::mouseReleaseEvent(event);
}

void FileListView::keyPressEvent(QKeyEvent* event) {
    // Let the top-level QShortcuts in MainWindow handle Copy / Cut / Paste /
    // SelectAll / Delete — the base class QAbstractItemView would otherwise
    // swallow these (esp. Cmd+C/Cmd+A on macOS) for its own built-in behaviors.
    if (event->matches(QKeySequence::Copy) ||
        event->matches(QKeySequence::Cut) ||
        event->matches(QKeySequence::Paste) ||
        event->matches(QKeySequence::SelectAll) ||
        event->matches(QKeySequence::Delete)) {
        event->ignore();
        return;
    }
    QTableWidget::keyPressEvent(event);
}

void FileListView::updateRowSelection(int row, bool selected) {
    for (int col = 0; col < columnCount(); ++col) {
        if (auto* it = item(row, col)) {
            it->setSelected(selected);
        }
    }
}

// ---------------------------------------------------------------------------
// 激活高亮：active 时把 selected 行底色改为 accent（带 alpha），
// 非 active 时让 selected 行视觉上完全等同于未选中行（清洁视图）。
//
// 实现走 stylesheet 而非 QPalette：QPalette 在 macOS 系统主题下经常被原生
// 样式覆盖；项目其余处也都是 stylesheet 路线，与之保持一致更稳。
//
// 注意：本表 setSelectionMode(NoSelection) + 自管 isSelected()，所以
// QTableWidget::item:selected 是我们自己 setSelected(true) 触发的，
// stylesheet 选择器照常生效。
//
// 非激活方案：把 :selected 规则的 background-color 设成 transparent，
// 并显式 reset color/font-weight 等可能被全局样式带上的属性。文字色不
// 写死成某个值，让每行 setForeground 设的"文件类型色"（目录蓝/代码绿/
// 配置黄等）原样透出 —— 这样选中行和未选中行外观完全一致，达到"清洁
// 窗口"效果。注意 :hover 也要保持透明，避免鼠标在非激活面板上掠过时
// 闪出蓝色 hover 条。
// ---------------------------------------------------------------------------
void FileListView::setActiveHighlight(bool active, const QColor& accent) {
    m_activeHighlight = active;
    m_activeAccent = accent;

    // -----------------------------------------------------------------------
    // 关键：MainWindow 全局 stylesheet 里 `QTableWidget::item:selected` 用了
    // rgba(136,192,208,0.28) 硬编码 Frost 蓝，并且还有属性级
    // `selection-background-color: rgba(136,192,208,0.25)`。
    //
    // 之前用同样的 `QTableWidget::item:selected` 选择器去覆盖，**特异性相同**，
    // 在 Qt 的 stylesheet 级联里全局规则可能赢；同时 QPalette::Highlight 还
    // 是另一条独立通道，stylesheet 的 :selected 也未必能阻止 QStyle 走 palette。
    //
    // 三管齐下确保非激活面板真的没有任何选中视觉：
    //   1) 选择器升级为 `FileListView::item:selected` —— 类名级特异性高于
    //      `QTableWidget::item:selected`，必赢全局规则。
    //   2) 同时写属性级 `selection-background-color` / `selection-color`，
    //      压住全局表里的同名属性。
    //   3) 直接改 QPalette::Highlight，覆盖 macOS 原生 QStyle 绘制路径。
    // -----------------------------------------------------------------------
    QString css;
    QPalette pal = palette();
    if (active && accent.isValid()) {
        // 28% 不透明度的 accent 色
        QColor sel = accent;
        sel.setAlphaF(0.28);
        css = QString(
            "FileListView {"
            "  selection-background-color: rgba(%1, %2, %3, %4);"
            "  selection-color: #ECEFF4;"
            "}"
            "FileListView::item:selected {"
            "  background-color: rgba(%1, %2, %3, %4);"
            "  color: #ECEFF4;"
            "}"
            "FileListView::item:selected:!active {"
            "  background-color: rgba(%1, %2, %3, %4);"
            "  color: #ECEFF4;"
            "}")
            .arg(sel.red()).arg(sel.green()).arg(sel.blue()).arg(sel.alpha());
        pal.setColor(QPalette::Highlight, sel);
        pal.setColor(QPalette::HighlightedText, QColor("#ECEFF4"));
        // macOS 失焦态用的 Inactive group 也要同色，避免切窗口后回到默认蓝
        pal.setColor(QPalette::Inactive, QPalette::Highlight, sel);
        pal.setColor(QPalette::Inactive, QPalette::HighlightedText, QColor("#ECEFF4"));
    } else {
        // 非 active：完全清掉选中视觉。background 透明 + 文字色不写死，让
        // setForeground 设的"文件类型色"原样透出。hover 也压成透明。
        css = QStringLiteral(
            "FileListView {"
            "  selection-background-color: transparent;"
            "}"
            "FileListView::item:selected {"
            "  background-color: transparent;"
            "}"
            "FileListView::item:selected:!active {"
            "  background-color: transparent;"
            "}"
            "FileListView::item:hover {"
            "  background-color: transparent;"
            "}");
        // QPalette 把 Highlight 设成透明（alpha=0），从根上不让 QStyle 画蓝条
        QColor transp(0, 0, 0, 0);
        pal.setColor(QPalette::Highlight, transp);
        pal.setColor(QPalette::Inactive, QPalette::Highlight, transp);
        // HighlightedText 设回普通文字色，避免选中时强制变白覆盖文件类型色
        pal.setColor(QPalette::HighlightedText, QColor("#D8DEE9"));
        pal.setColor(QPalette::Inactive, QPalette::HighlightedText, QColor("#D8DEE9"));
    }
    setPalette(pal);
    setStyleSheet(css);
    // 强制 viewport 重绘，避免 QStyle 缓存导致延迟更新
    if (viewport()) viewport()->update();
}

QVector<FileEntry> FileListView::getSelectedEntries() const {
    QVector<FileEntry> result;
    for (int i = 0; i < rowCount(); ++i) {
        if (item(i, 0) && item(i, 0)->isSelected()) {
            if (i < m_entries.size()) {
                const auto& entry = m_entries[i];
                if (entry.name != "." && entry.name != "..") {
                    result.append(entry);
                }
            }
        }
    }
    return result;
}

QVector<int> FileListView::getSelectedIndices() const {
    QVector<int> result;
    for (int i = 0; i < rowCount(); ++i) {
        if (item(i, 0) && item(i, 0)->isSelected()) {
            result.append(i);
        }
    }
    return result;
}

void FileListView::setSelectedIndices(const QVector<int>& indices) {
    for (int i = 0; i < rowCount(); ++i) {
        updateRowSelection(i, indices.contains(i));
    }
}

void FileListView::selectAll() {
    for (int i = 0; i < rowCount(); ++i) {
        updateRowSelection(i, true);
    }
    emit selectionChanged_();
}

// 按完整路径定位并选中。命中则替换为单选 + 滚动可见 + emit
// selectionChanged_；未命中则什么都不做（保留现有选择）。
//
// 用例：解压 / 新建 / 重命名 等操作完成后，让 UI 自动把光标"落"在新生成
// 的那一项上，与 macOS Finder 行为对齐。
bool FileListView::selectByPath(const QString& path) {
    if (path.isEmpty()) return false;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].path != path) continue;
        // 单选这一行
        for (int r = 0; r < rowCount(); ++r) {
            updateRowSelection(r, r == i);
        }
        m_lastClickedRow = i;
        // 滚动到可见，并把当前单元格指向它（带键盘光标，便于继续按方向键操作）
        if (auto* it = item(i, 0)) {
            scrollToItem(it, QAbstractItemView::PositionAtCenter);
            setCurrentItem(it);
        }
        emit selectionChanged_();
        return true;
    }
    return false;
}

void FileListView::onCustomContextMenu(const QPoint& pos) {
    // 体验对齐 macOS Finder：右键直接落在某行 → 自动把该行选上作为右键
    // 操作的对象，无须用户预先点选。这样 "Open With / Copy / Delete" 等
    // 菜单项立刻就以右键命中的那行为对象生效。
    //
    // 规则：
    //   - 右键命中合成行（"." / ".."）→ 不动选择，菜单按空白上下文显示
    //   - 右键命中已选中的行 → 不动选择（保留多选，菜单对整组生效）
    //   - 右键命中未选中的真实行 → 替换为只选该行，emit selectionChanged_
    //   - 右键命中空白 → 不动选择
    int row = rowAt(pos.y());
    if (row >= 0 && row < m_entries.size()) {
        const auto& e = m_entries[row];
        const bool isSynthetic = (e.name == "." || e.name == "..");
        const bool isSelected  = item(row, 0) && item(row, 0)->isSelected();
        if (!isSynthetic && !isSelected) {
            for (int i = 0; i < rowCount(); ++i) {
                updateRowSelection(i, i == row);
            }
            m_lastClickedRow = row;
            emit selectionChanged_();
        }
    }
    emit contextMenuRequested(mapToGlobal(pos));
}

QString FileListView::formatSize(qint64 size) const {
    if (size < 1024) return QString::number(size) + " B";
    if (size < 1024 * 1024) return QString::number(size / 1024.0, 'f', 1) + " KB";
    if (size < 1024LL * 1024 * 1024) return QString::number(size / (1024.0 * 1024.0), 'f', 1) + " MB";
    return QString::number(size / (1024.0 * 1024.0 * 1024.0), 'f', 1) + " GB";
}

QString FileListView::getIconText(const FileEntry& entry) const {
    if (entry.isDirectory) return "\xF0\x9F\x93\x81"; // folder emoji
    switch (entry.type) {
        case FileType::Document: return "\xF0\x9F\x93\x84"; // page
        case FileType::Image:    return "\xF0\x9F\x96\xBC"; // framed picture
        case FileType::Code:     return "\xF0\x9F\x92\xBB"; // computer
        case FileType::Archive:  return "\xF0\x9F\x93\xA6"; // package
        case FileType::Audio:    return "\xF0\x9F\x8E\xB5"; // musical note
        case FileType::Video:    return "\xF0\x9F\x8E\xAC"; // clapper
        case FileType::Text:     return "\xF0\x9F\x93\x9D"; // memo
        case FileType::Config:   return "\xE2\x9A\x99";     // gear
        case FileType::Script:   return "\xF0\x9F\x94\xA7"; // wrench
        default:                 return "\xF0\x9F\x93\x84"; // page
    }
}
