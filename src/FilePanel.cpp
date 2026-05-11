// =============================================================================
// FilePanel.cpp —— 见 FilePanel.h
//
// 实现要点：
//   - 构造里手工搭"tab 标题栏 + content 栈"代替 QTabWidget（macOS 原生样式问题）
//   - createTab / navigateTo / goBack 等都委托给 FileSystemRouter 列目录
//   - 拖放部分：FilePanel 作为 drop target；FileListView 作为 drag source
//     （FileListView 故意不 setAcceptDrops，让 drag event 冒泡到本组件）
// =============================================================================

#include "FilePanel.h"
#include "FileSystemRouter.h"
#include <QMouseEvent>
#include <QEvent>
#include <QDir>
#include <QSizePolicy>
#include <QHBoxLayout>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QStringList>

// 同应用 drag-and-drop 的私有 MIME 类型，用于"线分隔的路径列表"，UTF-8。
// 同时也支持 SFTP URL（标准 text/uri-list 不能表示 sftp://）。
static constexpr const char* kZoeMime = "application/x-zoe-fileentries";

FilePanel::FilePanel(PanelSide side, QWidget* parent)
    : QWidget(parent)
    , m_side(side)
    , m_addTabBtn(new QPushButton("+"))
{
    // Drop target: enables dragEnter/dragMove/drop events on this widget
    // (and via propagation, anything inside it that doesn't claim the
    // event for itself). FileListView intentionally does NOT call
    // setAcceptDrops(true), so its drag events bubble up to us.
    setAcceptDrops(true);

    // Left panel: Nord Aurora yellow (#EBCB8B) ; Right panel: Nord Aurora green
    // ROLLBACK: 原配色为天蓝 #4FC3F7 (QColor(79, 195, 247))。如需回滚把
    // 下一行换回：
    //   m_accentColor = (side == PanelSide::Left) ? QColor(79, 195, 247) : QColor(163, 190, 140);
    m_accentColor = (side == PanelSide::Left) ? QColor(235, 203, 139) : QColor(163, 190, 140);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- Tab header row: [QTabBar][stretch][+ button] ----
    // Custom arrangement — completely bypasses QTabWidget to avoid macOS
    // native-style centering of tabs.
    auto* headerRow = new QWidget();
    auto* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);
    headerLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_tabBar = new QTabBar();
    m_tabBar->setTabsClosable(true);
    m_tabBar->setMovable(true);
    m_tabBar->setExpanding(false);          // don't stretch tabs
    m_tabBar->setDocumentMode(true);        // flat look, bypass native rounded tabs
    m_tabBar->setUsesScrollButtons(true);
    m_tabBar->setElideMode(Qt::ElideMiddle);
    m_tabBar->setDrawBase(false);
    m_tabBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_tabBar->setStyleSheet(
        "QTabBar { alignment: left; border: none; background: transparent; }"
        "QTabBar::tab {"
        "  background-color: #3B4252;"
        "  color: #81A1C1;"
        "  padding: 6px 14px;"
        "  border: 1px solid #434C5E;"
        "  border-bottom: none;"
        "  border-top-left-radius: 4px;"
        "  border-top-right-radius: 4px;"
        "  margin-right: 2px;"
        "}"
        "QTabBar::tab:selected {"
        "  background-color: #434C5E;"
        "  color: #88C0D0;"
        "  font-weight: bold;"
        "}"
        "QTabBar::tab:hover:!selected {"
        "  background-color: #434C5E;"
        "  color: #E5E9F0;"
        "}");

    // "+" add-tab button — reuse Nord styling
    m_addTabBtn->setFixedSize(22, 22);
    m_addTabBtn->setFlat(true);
    m_addTabBtn->setCursor(Qt::PointingHandCursor);
    m_addTabBtn->setFocusPolicy(Qt::NoFocus);
    m_addTabBtn->setToolTip("New tab");
    m_addTabBtn->setStyleSheet(
        "QPushButton {"
        "  background: transparent;"
        "  color: #88C0D0;"
        "  border: 1px solid #4C566A;"
        "  border-radius: 11px;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "  padding: 0;"
        "  margin: 0;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(136, 192, 208, 0.20);"
        "  border-color: #88C0D0;"
        "  color: #ECEFF4;"
        "}"
        "QPushButton:pressed {"
        "  background: #5E81AC;"
        "  color: #ECEFF4;"
        "  border-color: #5E81AC;"
        "}");

    headerLayout->addWidget(m_tabBar, 0, Qt::AlignLeft | Qt::AlignBottom);
    headerLayout->addStretch(1);
    // Small spacer so the + button isn't glued to the right edge
    headerLayout->addSpacing(4);
    headerLayout->addWidget(m_addTabBtn, 0, Qt::AlignVCenter);
    headerLayout->addSpacing(6);

    // ---- Content area: QStackedWidget (replaces QTabWidget's pane) ----
    m_stack = new QStackedWidget();
    m_stack->setStyleSheet(
        "QStackedWidget { border-top: 1px solid #434C5E; background: #2E3440; }"
    );

    layout->addWidget(headerRow);
    layout->addWidget(m_stack, 1);

    connect(m_tabBar, &QTabBar::currentChanged, this, &FilePanel::onTabChanged);
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &FilePanel::onCloseTab);
    // Double-click a tab to close it (only if more than one tab remains)
    connect(m_tabBar, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index < 0) return;
        onCloseTab(index);
    });
    connect(m_tabBar, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from < 0 || to < 0 || from >= m_tabs.size() || to >= m_tabs.size()) return;
        m_tabs.move(from, to);
        // Keep stacked widget in sync
        QWidget* w = m_stack->widget(from);
        m_stack->removeWidget(w);
        m_stack->insertWidget(to, w);
    });
    connect(m_addTabBtn, &QPushButton::clicked, this, &FilePanel::onAddTab);

    // Create initial tab
    createTab(QDir::homePath());
    updateTabColor();
}

void FilePanel::createTab(const QString& path) {
    TabData tab;

    tab.container = new QWidget();
    auto* containerLayout = new QVBoxLayout(tab.container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    tab.pathBar = new PathBar();
    tab.fileList = new FileListView();

    containerLayout->addWidget(tab.pathBar);
    containerLayout->addWidget(tab.fileList);

    tab.state.currentPath = path;

    connect(tab.pathBar, &PathBar::pathSelected, this, &FilePanel::onPathSelected);
    connect(tab.fileList, &FileListView::directoryActivated, this, &FilePanel::onDirectoryActivated);
    connect(tab.fileList, &FileListView::selectionChanged_, this, &FilePanel::onFileSelectionChanged);
    connect(tab.fileList, &FileListView::contextMenuRequested, this, [this](const QPoint& pos) {
        emit contextMenuRequested(pos, m_side);
    });
    // 双击压缩包：透传到 MainWindow 让其在 app 内解压（不走系统 Finder）
    connect(tab.fileList, &FileListView::archiveActivated, this, [this](const QString& path) {
        emit archiveActivated(path, m_side);
    });

    // Install event filter so any mouse press / focus-in on the tab's widgets
    // marks this panel as the active one (so the directory tree targets it).
    tab.container->installEventFilter(this);
    tab.pathBar->installEventFilter(this);
    tab.fileList->installEventFilter(this);
    tab.fileList->viewport()->installEventFilter(this);

    // Load entries
    auto entries = FileSystemRouter::instance().listDirectory(path);
    tab.fileList->setEntries(entries, path);
    tab.pathBar->setPath(path);

    // Get directory name for tab title
    QString title = path;
    if (path != "/") {
        title = path.mid(path.lastIndexOf('/') + 1);
    }

    int index = m_stack->addWidget(tab.container);
    m_tabBar->insertTab(index, title);
    m_tabs.append(tab);
    m_tabBar->setCurrentIndex(index);
}

void FilePanel::navigateTo(const QString& path) {
    if (m_tabs.isEmpty()) return;
    auto& tab = currentTab();

    // Add to back history
    tab.state.backHistory.append(tab.state.currentPath);
    tab.state.forwardHistory.clear();
    tab.state.currentPath = path;
    tab.state.selectedIndices.clear();

    // Update view
    auto entries = FileSystemRouter::instance().listDirectory(path);
    tab.fileList->setEntries(entries, path);
    tab.pathBar->setPath(path);

    // Update tab title
    QString title = path;
    if (path != "/") {
        title = path.mid(path.lastIndexOf('/') + 1);
    }
    m_tabBar->setTabText(m_tabBar->currentIndex(), title);

    emit pathChanged(path, m_side);
}

void FilePanel::goBack() {
    if (m_tabs.isEmpty()) return;
    auto& tab = currentTab();
    if (tab.state.backHistory.isEmpty()) return;

    tab.state.forwardHistory.append(tab.state.currentPath);
    QString prevPath = tab.state.backHistory.takeLast();
    tab.state.currentPath = prevPath;

    auto entries = FileSystemRouter::instance().listDirectory(prevPath);
    tab.fileList->setEntries(entries, prevPath);
    tab.pathBar->setPath(prevPath);

    QString title = prevPath;
    if (prevPath != "/") title = prevPath.mid(prevPath.lastIndexOf('/') + 1);
    m_tabBar->setTabText(m_tabBar->currentIndex(), title);

    emit pathChanged(prevPath, m_side);
}

void FilePanel::goForward() {
    if (m_tabs.isEmpty()) return;
    auto& tab = currentTab();
    if (tab.state.forwardHistory.isEmpty()) return;

    tab.state.backHistory.append(tab.state.currentPath);
    QString nextPath = tab.state.forwardHistory.takeLast();
    tab.state.currentPath = nextPath;

    auto entries = FileSystemRouter::instance().listDirectory(nextPath);
    tab.fileList->setEntries(entries, nextPath);
    tab.pathBar->setPath(nextPath);

    QString title = nextPath;
    if (nextPath != "/") title = nextPath.mid(nextPath.lastIndexOf('/') + 1);
    m_tabBar->setTabText(m_tabBar->currentIndex(), title);

    emit pathChanged(nextPath, m_side);
}

void FilePanel::goUp() {
    QString path = currentPath();
    if (path == "/") return;

    // For remote mounts, the "root" looks like "sftp://user@host:22/". Going
    // up from there must stop at the mount root, not walk into the URL bits.
    QString mount = FileSystemRouter::instance().mountFor(path);
    if (!mount.isEmpty()) {
        QString mountRoot = mount + "/";
        if (path == mountRoot || path == mount) return;
    }

    QString parent = path.left(path.lastIndexOf('/'));
    if (parent.isEmpty()) parent = "/";
    // Keep the trailing slash for remote roots so the router still matches.
    if (!mount.isEmpty() && parent == mount) parent += "/";
    navigateTo(parent);
}

void FilePanel::refresh() {
    if (m_tabs.isEmpty()) return;
    auto& tab = currentTab();
    auto entries = FileSystemRouter::instance().listDirectory(tab.state.currentPath);
    tab.fileList->setEntries(entries, tab.state.currentPath);
}

QString FilePanel::currentPath() const {
    if (m_tabs.isEmpty()) return "/";
    return currentTab().state.currentPath;
}

FileListView* FilePanel::currentFileList() const {
    if (m_tabs.isEmpty()) return nullptr;
    return currentTab().fileList;
}

PathBar* FilePanel::currentPathBar() const {
    if (m_tabs.isEmpty()) return nullptr;
    return currentTab().pathBar;
}

QVector<FileEntry> FilePanel::getSelectedEntries() const {
    if (auto* fl = currentFileList()) {
        return fl->getSelectedEntries();
    }
    return {};
}

void FilePanel::selectAll() {
    if (auto* fl = currentFileList()) {
        fl->selectAll();
    }
}

bool FilePanel::canGoBack() const {
    if (m_tabs.isEmpty()) return false;
    return !currentTab().state.backHistory.isEmpty();
}

bool FilePanel::canGoForward() const {
    if (m_tabs.isEmpty()) return false;
    return !currentTab().state.forwardHistory.isEmpty();
}

void FilePanel::onTabChanged(int index) {
    if (m_tabs.isEmpty() || index < 0 || index >= m_tabs.size()) return;
    // Sync the stacked widget to the current tab
    m_stack->setCurrentIndex(index);
    emit pathChanged(currentPath(), m_side);
    emit activated(m_side);
    updateTabColor();
}

void FilePanel::onAddTab() {
    createTab(QDir::homePath());
    updateTabColor();
}

void FilePanel::onCloseTab(int index) {
    if (m_tabs.size() <= 1) return;  // Don't close last tab
    if (index < 0 || index >= m_tabs.size()) return;

    QWidget* w = m_stack->widget(index);
    m_stack->removeWidget(w);
    delete w;

    m_tabBar->removeTab(index);
    m_tabs.removeAt(index);
    updateTabColor();
}

void FilePanel::onPathSelected(const QString& path) {
    emit activated(m_side);
    navigateTo(path);
}

void FilePanel::onDirectoryActivated(const QString& path) {
    emit activated(m_side);
    navigateTo(path);
}

void FilePanel::onFileSelectionChanged() {
    emit activated(m_side);
    emit selectionChanged(m_side);
}

FilePanel::TabData& FilePanel::currentTab() {
    return m_tabs[m_tabBar->currentIndex()];
}

const FilePanel::TabData& FilePanel::currentTab() const {
    return m_tabs[m_tabBar->currentIndex()];
}

void FilePanel::updateTabColor() {
    // Highlight the selected tab with panel accent color (cyan for left, green for right)
    QString color = m_accentColor.name();
    m_tabBar->setStyleSheet(QString(
        "QTabBar { alignment: left; border: none; background: transparent; }"
        "QTabBar::tab {"
        "  background-color: #3B4252;"
        "  color: #81A1C1;"
        "  padding: 6px 14px;"
        "  border: 1px solid #434C5E;"
        "  border-bottom: none;"
        "  border-top-left-radius: 4px;"
        "  border-top-right-radius: 4px;"
        "  margin-right: 2px;"
        "}"
        "QTabBar::tab:selected {"
        "  background-color: #434C5E;"
        "  color: %1;"
        "  border-bottom: 2px solid %1;"
        "  font-weight: bold;"
        "}"
        "QTabBar::tab:hover:!selected {"
        "  background-color: #434C5E;"
        "  color: #E5E9F0;"
        "}"
    ).arg(color));
}

bool FilePanel::eventFilter(QObject* /*obj*/, QEvent* event) {
    // Mark this panel as active whenever the user presses a mouse button
    // or focuses any child widget. Does NOT swallow the event.
    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::FocusIn:
            emit activated(m_side);
            break;
        default:
            break;
    }
    return false;  // always pass the event through
}

// ---------------------------------------------------------------------------
// Drag & drop: receive files dragged from the *other* panel and ask
// MainWindow to copy them into our current directory.
//
// Acceptance policy:
//   - Source must come from this app (custom mime type) OR be plain
//     file:// URLs (so Finder drops also work for free).
//   - Same-panel drops are ignored (no point copying onto self).
// ---------------------------------------------------------------------------

static QStringList extractDropPaths(const QMimeData* mime) {
    QStringList paths;
    if (!mime) return paths;
    if (mime->hasFormat(kZoeMime)) {
        const QByteArray raw = mime->data(kZoeMime);
        for (const QString& line : QString::fromUtf8(raw).split('\n', Qt::SkipEmptyParts)) {
            paths << line;
        }
    } else if (mime->hasUrls()) {
        for (const QUrl& u : mime->urls()) {
            if (u.isLocalFile()) paths << u.toLocalFile();
        }
    }
    return paths;
}

void FilePanel::dragEnterEvent(QDragEnterEvent* event) {
    QStringList paths = extractDropPaths(event->mimeData());
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    // If every dragged path's *parent* equals our currentPath, this is a
    // self-drop - reject so the user gets a "no-drop" cursor.
    bool allFromHere = true;
    QString here = currentPath();
    for (const QString& p : paths) {
        QString parent = p.left(p.lastIndexOf('/'));
        if (parent.isEmpty()) parent = "/";
        if (parent != here) { allFromHere = false; break; }
    }
    if (allFromHere) { event->ignore(); return; }

    event->setDropAction(Qt::CopyAction);
    event->acceptProposedAction();
    // Visual cue: a bright accent border on the panel while a drag hovers.
    setStyleSheet(QString(
        "FilePanel { border: 2px solid %1; border-radius: 4px; }"
    ).arg(m_accentColor.name()));
}

void FilePanel::dragMoveEvent(QDragMoveEvent* event) {
    if (!extractDropPaths(event->mimeData()).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void FilePanel::dragLeaveEvent(QDragLeaveEvent* /*event*/) {
    setStyleSheet("");
}

void FilePanel::dropEvent(QDropEvent* event) {
    setStyleSheet("");
    QStringList paths = extractDropPaths(event->mimeData());
    if (paths.isEmpty()) { event->ignore(); return; }

    const QString dest = currentPath();
    // Filter out anything whose parent already equals the destination
    // (defensive: same-panel partial drag).
    QStringList real;
    for (const QString& p : paths) {
        QString parent = p.left(p.lastIndexOf('/'));
        if (parent.isEmpty()) parent = "/";
        if (parent != dest) real << p;
    }
    if (real.isEmpty()) { event->ignore(); return; }

    event->setDropAction(Qt::CopyAction);
    event->acceptProposedAction();
    emit dropCopyRequested(real, dest, m_side);
}
