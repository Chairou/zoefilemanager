// =============================================================================
// MainWindow.cpp —— 见 MainWindow.h
//
// 本文件相对长（>1000 行），按"装配 → 工具栏 → 槽函数 → 主题 → 状态栏"
// 大致顺序排布。重点关注：
//   - 构造函数：UI 装配与信号-槽接线，应用退出 hook（aboutToQuit → router.clear）
//   - onPaste / onDropCopy / onDelete：进度对话框 + 远程边界守卫的样板
//   - onRemoteConnect / onRemoteDisconnect：把 SFTP 会话交给 FileSystemRouter
// =============================================================================

#include "MainWindow.h"
#include "SearchDialog.h"
#include "RemoteDialog.h"
#include "SftpClient.h"
#include "FileSystemRouter.h"
#include "ScriptDialog.h"
#include "RealFileSystem.h"
#include "AboutDialog.h"
#include <QApplication>
#include <QShortcut>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QClipboard>
#include <QToolButton>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProgressDialog>
#include <QProcess>
#include <QProxyStyle>
#include <QDesktopServices>
#include <QUrl>
#include <QEventLoop>
#include <QSettings>
#include <QCloseEvent>
#include <QInputDialog>
#include <QRegularExpression>
#include <QDateTime>
#include <QTimer>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QPainterPath>
#include <cmath>
#include "I18n.h"
#include "AppFonts.h"
#include "SettingsDialog.h"

// Proxy style 用来强制把 PM_ToolBarExtensionExtent（工具栏溢出按钮的宽度
// metric）增大。Qt 在 macOS 原生样式上只给 16~20px，导致我们自定义的 ">>"
// 文字按钮被 toolbar viewport 右侧裁切。覆盖这个 metric 是让 toolbar 真正
// 给溢出按钮预留足够空间的唯一规范方法（QSS min-width / setFixedSize 都
// 不会改变 toolbar 自己分配的可见宽度）。
class ToolbarExtStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int pixelMetric(PixelMetric pm, const QStyleOption* opt = nullptr,
                    const QWidget* widget = nullptr) const override {
        if (pm == PM_ToolBarExtensionExtent) {
            const int menuPt = AppFonts::instance().effectiveMenuFontSize();
            QFont f = QApplication::font();
            if (menuPt > 0) f.setPointSize(menuPt);
            f.setBold(true);
            const int textW = QFontMetrics(f).horizontalAdvance(QStringLiteral(">>"));
            return qMax(40, textW + 18);
        }
        return QProxyStyle::pixelMetric(pm, opt, widget);
    }
};

// ---------------------------------------------------------------------------
// 构造：组装整个 UI 树。顺序大致是：
//   1. 主分割器 + 左侧（DirectoryTree + ShortcutBar 上下叠放）+ 双面板
//   2. 接线：path/选择/激活/右键/拖拽 等所有面板信号 → 本类的槽
//   3. 工具栏 / 状态栏 / 快捷键 / 默认主题
//   4. 注册 aboutToQuit hook —— 在 Qt 主循环退出时主动清掉所有 SFTP 会话，
//      避免静态析构期 libssh2 撞到已卸载的 libcrypto（旧版崩溃 bug）
// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Zoe File Manager 0.8");
    resize(1400, 800);

    // Main splitter: Tree | Left Panel | Right Panel
    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(m_mainSplitter);

    m_dirTree = new DirectoryTree();
    m_shortcutBar = new ShortcutBar();

    // Left sidebar: ShortcutBar on top, DirectoryTree below (vertical splitter)
    auto* sidebarSplitter = new QSplitter(Qt::Vertical);
    sidebarSplitter->addWidget(m_shortcutBar);
    sidebarSplitter->addWidget(m_dirTree);
    sidebarSplitter->setStretchFactor(0, 0);
    sidebarSplitter->setStretchFactor(1, 1);
    sidebarSplitter->setSizes({140, 600});
    sidebarSplitter->setMinimumWidth(200);
    sidebarSplitter->setMaximumWidth(380);

    m_leftPanel = new FilePanel(PanelSide::Left);
    m_rightPanel = new FilePanel(PanelSide::Right);

    m_mainSplitter->addWidget(sidebarSplitter);
    m_mainSplitter->addWidget(m_leftPanel);
    m_mainSplitter->addWidget(m_rightPanel);
    m_mainSplitter->setSizes({240, 550, 550});

    setupToolbar();
    setupStatusBar();
    setupShortcuts();

    // Connect panel signals
    connect(m_leftPanel, &FilePanel::pathChanged, this, &MainWindow::onPanelPathChanged);
    connect(m_rightPanel, &FilePanel::pathChanged, this, &MainWindow::onPanelPathChanged);
    connect(m_leftPanel, &FilePanel::selectionChanged, this, &MainWindow::onPanelSelectionChanged);
    connect(m_rightPanel, &FilePanel::selectionChanged, this, &MainWindow::onPanelSelectionChanged);
    connect(m_leftPanel, &FilePanel::activated, this, &MainWindow::onPanelActivated);
    connect(m_rightPanel, &FilePanel::activated, this, &MainWindow::onPanelActivated);
    connect(m_leftPanel, &FilePanel::contextMenuRequested, this, &MainWindow::onContextMenu);
    connect(m_rightPanel, &FilePanel::contextMenuRequested, this, &MainWindow::onContextMenu);
    connect(m_leftPanel, &FilePanel::dropCopyRequested, this, &MainWindow::onDropCopy);
    connect(m_rightPanel, &FilePanel::dropCopyRequested, this, &MainWindow::onDropCopy);
    connect(m_leftPanel, &FilePanel::archiveActivated, this, &MainWindow::onExtractArchive);
    connect(m_rightPanel, &FilePanel::archiveActivated, this, &MainWindow::onExtractArchive);

    // Connect directory tree
    connect(m_dirTree, &DirectoryTree::directorySelected, this, &MainWindow::onDirectoryTreeSelected);

    // Connect shortcut bar — clicking a shortcut navigates the active panel
    connect(m_shortcutBar, &ShortcutBar::shortcutActivated, this,
            [this](const QString& path) {
        activePanel()->navigateTo(path);
    });

    // Initial highlights
    m_dirTree->highlightPath(m_leftPanel->currentPath(), PanelSide::Left);
    m_dirTree->highlightPath(m_rightPanel->currentPath(), PanelSide::Right);

    applyDarkTheme();
    updateStatusBar();

    // ---- 应用菜单栏：Help → About ----
    // 在 macOS 下，About 项会被自动归并到应用菜单（Apple 菜单旁那个），
    // 这是 Qt 平台集成的默认行为；Linux/Windows 上会留在 "Help" 菜单。
    // 这一项满足 LGPLv3 的 "appropriate copyright notice" 义务。
    {
        QMenu* helpMenu = menuBar()->addMenu(T("Help"));
        QAction* about = helpMenu->addAction(T("About Zoe File Manager"));
        // Qt 默认把 ApplicationRole 的 action 移到 macOS 应用菜单下
        about->setMenuRole(QAction::AboutRole);
        connect(about, &QAction::triggered, this, [this]() {
            AboutDialog dlg(this);
            dlg.exec();
        });
    }

    // Crucial: tear all SFTP / SSH sessions down while Qt, libssh2 and
    // libcrypto are still fully alive. If we wait for static destructors
    // at process exit, OpenSSL's RNG locks may have already been freed and
    // libssh2_session_free / libssh2_sftp_shutdown crash inside
    // pthread_rwlock_rdlock(NULL). aboutToQuit fires on the main thread
    // right before QCoreApplication returns from exec().
    connect(qApp, &QCoreApplication::aboutToQuit, this, []() {
        FileSystemRouter::instance().clear();
    });

    // 最后一步：从 QSettings 恢复用户上次关闭时的窗口几何、splitter 布局、
    // 左右面板路径以及活跃面板。必须放在所有子组件构造、信号连接、初始
    // navigateTo 完成之后；这样 loadSettings 里的 navigateTo / 高亮才能
    // 正确触达 panel 与 directory tree。
    loadSettings();
}

MainWindow::~MainWindow() {
    // 双保险：除了 aboutToQuit 之外再清一次。覆盖异常退出 / 提前关闭主窗口
    // 等绕过 aboutToQuit 的路径。clear() 是幂等的。
    FileSystemRouter::instance().clear();
}

// ---------------------------------------------------------------------------
// 持久化：把左右面板的当前路径、活跃面板、splitter 布局以及窗口几何
// 写入 QSettings，下次启动时通过 loadSettings 恢复。
//
// 存储位置（QSettings("WorkBuddy", "ZoeFileManager")）：
//   - panels/leftPath        左面板当前路径（绝对本地路径）
//   - panels/rightPath       右面板当前路径
//   - panels/activeSide      "Left" / "Right"
//   - mainSplitter/state     QSplitter::saveState() 的 byte array
//   - window/geometry        QWidget::saveGeometry() 的 byte array
//
// 路径有效性策略（loadSettings）：
//   - 远程 scheme（sftp://、ftp://）→ 回退到 home。本构建不持久化远程会话，
//     启动时也无法重建 SFTP 挂载，强行 navigate 会失败/卡死。
//   - 路径不存在或不是目录 → 回退到 home。
// ---------------------------------------------------------------------------

namespace {
// 判断字符串是否以远程 scheme 开头。本构建只识别 sftp/ftp，
// 但提前把 ssh/http(s) 也挡掉，省得未来加新协议时漏判。
bool isRemoteScheme(const QString& path) {
    static const char* const kSchemes[] = {
        "sftp://", "ftp://", "ftps://", "ssh://",
        "http://", "https://", "smb://"
    };
    for (const char* s : kSchemes) {
        if (path.startsWith(QLatin1String(s), Qt::CaseInsensitive)) return true;
    }
    return false;
}

// 把候选路径校验为"本地存在的目录"；不合格回退到 home。
QString sanitizeLocalDir(const QString& candidate) {
    if (candidate.isEmpty()) return QDir::homePath();
    if (isRemoteScheme(candidate)) return QDir::homePath();
    QFileInfo fi(candidate);
    if (!fi.exists() || !fi.isDir()) return QDir::homePath();
    return fi.absoluteFilePath();
}
} // namespace

void MainWindow::loadSettings() {
    QSettings settings("WorkBuddy", "ZoeFileManager");

    // ---- 窗口几何 / splitter 布局 ----
    // restoreGeometry / restoreState 在数据缺失或不兼容时返回 false，
    // 我们不做特殊处理，构造函数里已经 resize(1400, 800) + setSizes(...)
    // 提供了合理默认值。
    const QByteArray geom = settings.value("window/geometry").toByteArray();
    if (!geom.isEmpty()) restoreGeometry(geom);

    const QByteArray splitterState = settings.value("mainSplitter/state").toByteArray();
    if (!splitterState.isEmpty()) m_mainSplitter->restoreState(splitterState);

    // ---- 左右面板路径 ----
    const QString defaultPath = QDir::homePath();
    const QString leftRaw  = settings.value("panels/leftPath",  defaultPath).toString();
    const QString rightRaw = settings.value("panels/rightPath", defaultPath).toString();

    const QString leftPath  = sanitizeLocalDir(leftRaw);
    const QString rightPath = sanitizeLocalDir(rightRaw);

    m_leftPanel->navigateTo(leftPath);
    m_rightPanel->navigateTo(rightPath);

    // 同步左侧目录树高亮，对齐构造函数里的 "Initial highlights" 行为
    m_dirTree->highlightPath(leftPath,  PanelSide::Left);
    m_dirTree->highlightPath(rightPath, PanelSide::Right);

    // ---- 活跃面板 ----
    const QString activeStr = settings.value("panels/activeSide", "Left").toString();
    if (activeStr == QLatin1String("Right")) {
        m_activePane = PanelSide::Right;
        m_rightPanel->setFocus();
    } else {
        m_activePane = PanelSide::Left;
        m_leftPanel->setFocus();
    }

    updateStatusBar();
}

void MainWindow::saveSettings() {
    QSettings settings("WorkBuddy", "ZoeFileManager");

    // 面板当前路径：直接读取 FilePanel::currentPath()。注意这里不过滤
    // 远程路径——存了也无害（loadSettings 会回退到 home），而保留路径文本
    // 对调试有帮助。但为了下次启动恢复行为可预测，我们对远程路径明确改写
    // 为 home，避免把 sftp:// 写进配置。
    auto sanitizeForSave = [](const QString& p) -> QString {
        if (p.isEmpty() || isRemoteScheme(p)) return QDir::homePath();
        return p;
    };
    settings.setValue("panels/leftPath",  sanitizeForSave(m_leftPanel->currentPath()));
    settings.setValue("panels/rightPath", sanitizeForSave(m_rightPanel->currentPath()));
    settings.setValue("panels/activeSide",
        m_activePane == PanelSide::Left ? QStringLiteral("Left")
                                        : QStringLiteral("Right"));

    settings.setValue("mainSplitter/state", m_mainSplitter->saveState());
    settings.setValue("window/geometry",    saveGeometry());
}

// 关闭事件：先把状态落盘再走父类默认处理（接受关闭 → 进入 aboutToQuit）。
void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

// 辅助函数：绘制矢量图标（统一风格：1.6px 线宽，圆角，简洁几何）
// 底色：与目录树底色一致（暗主题 #2E3440 / 亮主题 #ffffff），
//       通过 m_isDarkTheme 标志位由调用方在主题切换时显式刷新。
// 描边色：在该底色上对比度合适的前景色（暗主题用浅色 #D8DEE9，亮主题用深灰 #424242）
static QIcon makeVectorIcon(std::function<void(QPainter&, int)> drawFunc, bool darkTheme, int size = 20) {
    const QColor bgColor   = darkTheme ? QColor("#2E3440") : QColor("#ffffff");
    const QColor lineColor = darkTheme ? QColor("#D8DEE9") : QColor("#424242");
    QPixmap pixmap(size, size);
    pixmap.fill(bgColor);  // 使用目录树底色作为图标底色
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setPen(QPen(lineColor, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    drawFunc(painter, size);
    return QIcon(pixmap);
}

// 剪切图标：剪刀形状
static QIcon makeCutIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cx = s / 2, cy = s / 2;
        // 两个圆环（剪刀手柄）
        p.drawEllipse(QPoint(cx - 4, cy - 3), 2, 2);
        p.drawEllipse(QPoint(cx - 4, cy + 3), 2, 2);
        // 剪刀刃
        p.drawLine(cx - 2, cy - 3, cx + 5, cy);
        p.drawLine(cx - 2, cy + 3, cx + 5, cy);
    }, darkTheme, size);
}

// 粘贴图标：剪贴板
static QIcon makePasteIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int m = s / 5;
        // 剪贴板外框
        p.drawRoundedRect(m, m + 2, s - 2*m, s - 2*m - 2, 2, 2);
        // 顶部夹子
        p.drawRect(s/2 - 2, m - 1, 4, 3);
    }, darkTheme, size);
}

// 全选图标：带勾的方框
static QIcon makeSelectAllIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int m = s / 5;
        // 方框
        p.drawRoundedRect(m, m, s - 2*m, s - 2*m, 2, 2);
        // 勾
        QPainterPath path;
        path.moveTo(m + 2, s/2);
        path.lineTo(s/2 - 1, s - m - 3);
        path.lineTo(s - m - 2, m + 3);
        p.drawPath(path);
    }, darkTheme, size);
}

// 文件夹图标：简化文件夹
static QIcon makeFolderIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int m = s / 5;
        // 文件夹主体
        QPainterPath path;
        path.moveTo(m, m + 3);
        path.lineTo(m, s - m);
        path.lineTo(s - m, s - m);
        path.lineTo(s - m, m + 3);
        // 文件夹标签
        path.lineTo(s/2 + 2, m + 3);
        path.lineTo(s/2, m);
        path.lineTo(m, m);
        path.closeSubpath();
        p.drawPath(path);
    }, darkTheme, size);
}

// 脚本图标：齿轮
static QIcon makeScriptIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cx = s / 2, cy = s / 2, r = s / 4;
        // 中心圆
        p.drawEllipse(QPoint(cx, cy), r - 2, r - 2);
        // 6 个齿
        for (int i = 0; i < 6; ++i) {
            double angle = i * M_PI / 3;
            int x1 = cx + (r - 1) * cos(angle);
            int y1 = cy + (r - 1) * sin(angle);
            int x2 = cx + (r + 3) * cos(angle);
            int y2 = cy + (r + 3) * sin(angle);
            p.drawLine(x1, y1, x2, y2);
        }
    }, darkTheme, size);
}

// 眼睛图标：显示/隐藏
static QIcon makeEyeIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cx = s / 2, cy = s / 2;
        // 眼睛轮廓（椭圆弧）
        QPainterPath path;
        path.moveTo(s/5, cy);
        path.quadTo(cx, cy - s/4, s*4/5, cy);
        path.quadTo(cx, cy + s/4, s/5, cy);
        p.drawPath(path);
        // 瞳孔
        p.setBrush(p.pen().color());
        p.drawEllipse(QPoint(cx, cy), 3, 3);
    }, darkTheme, size);
}

// 月亮图标：主题切换
static QIcon makeThemeIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cx = s / 2 + 1, cy = s / 2, r = s / 3;
        // 月牙（用两个圆的差集）
        QPainterPath outer, inner;
        outer.addEllipse(QPoint(cx, cy), r, r);
        inner.addEllipse(QPoint(cx + r/2, cy - r/4), r, r);
        p.fillPath(outer.subtracted(inner), QBrush(p.pen().color()));
    }, darkTheme, size);
}

// 箭头图标：导航（方向：0=左, 1=右, 2=上）
static QIcon makeArrowIcon(int direction, bool darkTheme, int size = 20) {
    return makeVectorIcon([direction](QPainter& p, int s) {
        int cx = s / 2, cy = s / 2;
        QPainterPath path;
        if (direction == 0) {  // 左
            path.moveTo(cx + 3, cy - 5);
            path.lineTo(cx - 3, cy);
            path.lineTo(cx + 3, cy + 5);
            p.drawPath(path);
            p.drawLine(cx - 3, cy, cx + 5, cy);
        } else if (direction == 1) {  // 右
            path.moveTo(cx - 3, cy - 5);
            path.lineTo(cx + 3, cy);
            path.lineTo(cx - 3, cy + 5);
            p.drawPath(path);
            p.drawLine(cx - 5, cy, cx + 3, cy);
        } else {  // 上
            path.moveTo(cx - 5, cy + 3);
            path.lineTo(cx, cy - 3);
            path.lineTo(cx + 5, cy + 3);
            p.drawPath(path);
            p.drawLine(cx, cy - 3, cx, cy + 5);
        }
    }, darkTheme, size);
}

// 刷新图标：环形箭头
static QIcon makeRefreshIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cx = s / 2, cy = s / 2, r = s / 3;
        // 3/4 圆弧
        QRectF rect(cx - r, cy - r, 2*r, 2*r);
        p.drawArc(rect, 30 * 16, 270 * 16);
        // 箭头（在弧的起点处）
        double a = 30 * M_PI / 180;
        int ax = cx + r * cos(a);
        int ay = cy - r * sin(a);
        p.drawLine(ax, ay, ax - 4, ay - 1);
        p.drawLine(ax, ay, ax - 1, ay + 4);
    }, darkTheme, size);
}

// 复制图标：两个重叠的方框
static QIcon makeCopyIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int m = s / 5;
        // 后面的方框
        p.drawRoundedRect(m, m, s - 2*m - 3, s - 2*m - 3, 1.5, 1.5);
        // 前面的方框
        p.drawRoundedRect(m + 3, m + 3, s - 2*m - 3, s - 2*m - 3, 1.5, 1.5);
    }, darkTheme, size);
}

// 删除图标：垃圾桶
static QIcon makeDeleteIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cx = s / 2;
        int top = s / 4 + 1;
        int bottom = s - s / 5;
        // 桶盖横线
        p.drawLine(cx - 6, top, cx + 6, top);
        // 提手
        p.drawLine(cx - 2, top - 2, cx + 2, top - 2);
        p.drawLine(cx - 2, top - 2, cx - 2, top);
        p.drawLine(cx + 2, top - 2, cx + 2, top);
        // 桶身（梯形）
        QPainterPath body;
        body.moveTo(cx - 5, top + 1);
        body.lineTo(cx - 4, bottom);
        body.lineTo(cx + 4, bottom);
        body.lineTo(cx + 5, top + 1);
        p.drawPath(body);
        // 桶身竖线
        p.drawLine(cx - 2, top + 3, cx - 2, bottom - 1);
        p.drawLine(cx + 2, top + 3, cx + 2, bottom - 1);
    }, darkTheme, size);
}

// 搜索图标：放大镜
static QIcon makeSearchIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cx = s / 2 - 2, cy = s / 2 - 2, r = s / 4;
        // 放大镜圆圈
        p.drawEllipse(QPoint(cx, cy), r, r);
        // 手柄
        p.drawLine(cx + r * 0.7, cy + r * 0.7, s - 3, s - 3);
    }, darkTheme, size);
}

// 连接图标：相连的链环
static QIcon makeConnectIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cy = s / 2;
        // 左链环
        p.drawRoundedRect(s/5 - 1, cy - 3, 7, 6, 3, 3);
        // 右链环
        p.drawRoundedRect(s - s/5 - 6, cy - 3, 7, 6, 3, 3);
        // 连接线
        p.drawLine(s/5 + 5, cy, s - s/5 - 5, cy);
    }, darkTheme, size);
}

// 断开图标：断开的链环
static QIcon makeDisconnectIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int cy = s / 2;
        // 左链环
        p.drawRoundedRect(s/5 - 1, cy - 3, 7, 6, 3, 3);
        // 右链环
        p.drawRoundedRect(s - s/5 - 6, cy - 3, 7, 6, 3, 3);
        // 断开标记（X）
        int mx = s / 2;
        p.drawLine(mx - 2, cy - 3, mx + 2, cy + 3);
        p.drawLine(mx + 2, cy - 3, mx - 2, cy + 3);
    }, darkTheme, size);
}

// 设置图标：滑块
static QIcon makeSettingsIcon(bool darkTheme, int size = 20) {
    return makeVectorIcon([](QPainter& p, int s) {
        int m = s / 5;
        int w = s - 2 * m;
        // 三条横线 + 滑块圆点
        int y1 = m + 1;
        int y2 = s / 2;
        int y3 = s - m - 1;
        p.drawLine(m, y1, m + w, y1);
        p.drawLine(m, y2, m + w, y2);
        p.drawLine(m, y3, m + w, y3);
        p.setBrush(p.pen().color());
        p.drawEllipse(QPoint(m + w * 0.7, y1), 2, 2);
        p.drawEllipse(QPoint(m + w * 0.3, y2), 2, 2);
        p.drawEllipse(QPoint(m + w * 0.6, y3), 2, 2);
    }, darkTheme, size);
}

// 工具栏：使用统一风格的矢量图标，文字作为 ToolTip 显示。
void MainWindow::setupToolbar() {
    auto* toolbar = addToolBar("Main");
    m_toolbar = toolbar;
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    // 导航按钮：使用自绘箭头图标
    m_backAction = toolbar->addAction(makeArrowIcon(0, m_isDarkTheme), "", this, &MainWindow::onBack);
    m_backAction->setToolTip(T("Back"));
    
    m_forwardAction = toolbar->addAction(makeArrowIcon(1, m_isDarkTheme), "", this, &MainWindow::onForward);
    m_forwardAction->setToolTip(T("Forward"));
    
    m_upAction = toolbar->addAction(makeArrowIcon(2, m_isDarkTheme), "", this, &MainWindow::onUp);
    m_upAction->setToolTip(T("Up"));
    
    m_refreshAction = toolbar->addAction(makeRefreshIcon(m_isDarkTheme), "", this, &MainWindow::onRefresh);
    m_refreshAction->setToolTip(T("Refresh"));

    // 按钮之间不再插入分隔符：用户要求按钮组紧贴无缝隙（2026-05-12 调整）
    // 文件操作：使用自定义矢量图标
    m_copyAction = toolbar->addAction(makeCopyIcon(m_isDarkTheme), "", this, &MainWindow::onCopy);
    m_copyAction->setToolTip(T("Copy"));
    
    m_cutAction = toolbar->addAction(makeCutIcon(m_isDarkTheme), "", this, &MainWindow::onCut);
    m_cutAction->setToolTip(T("Cut"));
    
    m_pasteAction = toolbar->addAction(makePasteIcon(m_isDarkTheme), "", this, &MainWindow::onPaste);
    m_pasteAction->setToolTip(T("Paste"));
    
    m_deleteAction = toolbar->addAction(makeDeleteIcon(m_isDarkTheme), "", this, &MainWindow::onDelete);
    m_deleteAction->setToolTip(T("Delete"));

    // 其他操作：使用自定义矢量图标
    m_selectAllAction = toolbar->addAction(makeSelectAllIcon(m_isDarkTheme), "", this, &MainWindow::onSelectAll);
    m_selectAllAction->setToolTip(T("Select All"));
    
    m_copyPathAction = toolbar->addAction(makeFolderIcon(m_isDarkTheme), "", this, &MainWindow::onCopyPath);
    m_copyPathAction->setToolTip(T("Copy Path"));
    
    m_runScriptAction = toolbar->addAction(makeScriptIcon(m_isDarkTheme), "", this, &MainWindow::onRunScript);
    m_runScriptAction->setToolTip(T("Script"));
    
    m_searchAction = toolbar->addAction(makeSearchIcon(m_isDarkTheme), "", this, &MainWindow::onSearch);
    m_searchAction->setToolTip(T("Search"));

    // 远程连接：使用自定义矢量图标
    m_remoteConnectAction = toolbar->addAction(makeConnectIcon(m_isDarkTheme), "", this, &MainWindow::onRemoteConnect);
    m_remoteConnectAction->setToolTip(T("Connect"));
    
    m_remoteDisconnectAction = toolbar->addAction(makeDisconnectIcon(m_isDarkTheme), "", this, &MainWindow::onRemoteDisconnect);
    m_remoteDisconnectAction->setToolTip(T("Disconnect"));
    m_remoteDisconnectAction->setEnabled(false);

    // 显示隐藏文件
    m_hiddenAction = toolbar->addAction(makeEyeIcon(m_isDarkTheme), "", this, &MainWindow::onToggleHidden);
    m_hiddenAction->setCheckable(true);
    m_hiddenAction->setChecked(RealFileSystem::instance().showHidden());
    m_hiddenAction->setToolTip(T("Show/Hide Hidden Files"));

    // 主题切换
    m_themeAction = toolbar->addAction(makeThemeIcon(m_isDarkTheme), "", this, &MainWindow::onToggleTheme);
    m_themeAction->setToolTip(m_isDarkTheme ? T("Theme: Dark") : T("Theme: Light"));

    // 推到右侧：Settings 按钮靠最右端显示
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    
    m_settingsAction = toolbar->addAction(makeSettingsIcon(m_isDarkTheme), "", this, &MainWindow::onOpenSettings);
    m_settingsAction->setToolTip(T("Settings"));
    // 语言切换时重新翻译整个 UI
    connect(&I18n::instance(), &I18n::changed,
            this, &MainWindow::retranslateUi);
    // 字号变化时刷新工具栏/菜单/面板/目录树字号
    connect(&AppFonts::instance(), &AppFonts::changed,
            this, &MainWindow::applyFonts);

    // 监听 toolbar 的事件，及时修正"溢出扩展按钮"（Qt 在首次溢出时才创建它）
    toolbar->installEventFilter(this);

    // 安装 proxy style：唯一可靠地让 toolbar 给溢出按钮分配足够宽度的方法。
    // 用 toolbar 当前的 baseStyle 派生，这样不会破坏原有的视觉风格。
    toolbar->setStyle(new ToolbarExtStyle(toolbar->style()));

    // 关键：对所有 QToolButton 启用 autoRaise，关闭 native frame。
    // 这是消除"按钮之间出现 1px 分隔线/边界感"最可靠的手段——
    // 仅靠 QSS `border: 0px` 无法压制 mac/fusion 的 PE_FrameButtonTool primitive。
    // setAutoRaise(true) 让 QToolButton 在非 hover/pressed 状态下完全不绘制 frame，
    // 由 QSS 全权接管按钮外观。需要在所有 action 已添加完成后再执行。
    for (auto* btn : toolbar->findChildren<QToolButton*>()) {
        btn->setAutoRaise(true);
    }

    // 同步 action tooltip → button tooltip。Qt 在 action text 为空且 toolbar
    // 被 QSS 样式化后，不会自动把 action->toolTip() 复制到内部 QToolButton，
    // 导致鼠标悬停无提示。此处显式兜底，保证每个按钮都有中文悬浮提示。
    syncToolbarButtonToolTips();
}

// 遍历 toolbar 上所有 QToolButton，把它们关联 QAction 的 toolTip 显式
// setToolTip 到 button 本身（若 button 当前 toolTip 为空或与 action 不一致）。
// 该函数需在：
//   1) setupToolbar() 装配完成后调用一次；
//   2) retranslateUi() 末尾调用（语言切换后刷新）；
//   3) eventFilter 的布局变化异步回调里调用（覆盖 Qt 后置创建的溢出按钮）。
void MainWindow::syncToolbarButtonToolTips() {
    if (!m_toolbar) return;
    for (auto* btn : m_toolbar->findChildren<QToolButton*>()) {
        QAction* act = btn->defaultAction();
        if (!act) continue;
        const QString tip = act->toolTip();
        if (!tip.isEmpty() && btn->toolTip() != tip) {
            btn->setToolTip(tip);
        }
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_toolbar &&
        (event->type() == QEvent::Resize ||
         event->type() == QEvent::LayoutRequest ||
         event->type() == QEvent::ChildAdded ||
         event->type() == QEvent::Show)) {
        // 异步修正：等 Qt 自己布局完毕再去找/改扩展按钮
        QTimer::singleShot(0, this, [this]() {
            if (!m_toolbar) return;
            // 防御：每次布局变化后都重新对所有 QToolButton（包括 Qt 自己后加的
            // 溢出扩展按钮）启用 autoRaise，确保它们都不绘制 native frame，
            // 按钮之间不再出现 1px 分隔线。
            for (auto* btn : m_toolbar->findChildren<QToolButton*>()) {
                btn->setAutoRaise(true);
            }
            // 布局变化后也同步一次 tooltip，覆盖 Qt 后置创建的溢出按钮、
            // 以及任何被替换/重建的 QToolButton。
            syncToolbarButtonToolTips();
            auto* ext = m_toolbar->findChild<QToolButton*>("qt_toolbar_ext_button");
            if (!ext) return;
            ext->setToolButtonStyle(Qt::ToolButtonTextOnly);
            if (ext->text() != QStringLiteral(">>")) ext->setText(">>");
            ext->setToolTip(T("More"));
            const int menuPt = AppFonts::instance().effectiveMenuFontSize();
            QFont ef = ext->font();
            if (menuPt > 0) ef.setPointSize(menuPt); else ef = QFont();
            ef.setBold(true);
            ext->setFont(ef);
            // 强制按钮宽度足够容纳 ">>"：用当前字号的 fontMetrics 计算文字宽度
            // 加左右内边距 16px，确保不被裁切。Qt 的扩展按钮默认宽度由
            // QStyle::PM_ToolBarExtensionExtent 决定，stylesheet min-width
            // 在某些样式下不被尊重，这里直接 setFixed 兜底。
            const int textW = QFontMetrics(ef).horizontalAdvance(">>");
            const int w = qMax(40, textW + 18);
            const int h = qMax(26, QFontMetrics(ef).height() + 10);
            ext->setFixedSize(w, h);
        });
    }
    return QMainWindow::eventFilter(watched, event);
}

// 主题切换时调用：按当前 palette 重建所有工具栏图标，
// 让图标颜色与新主题下的 UI 文字/按钮颜色一致。
void MainWindow::refreshToolbarIcons() {
    if (m_backAction)             m_backAction->setIcon(makeArrowIcon(0, m_isDarkTheme));
    if (m_forwardAction)          m_forwardAction->setIcon(makeArrowIcon(1, m_isDarkTheme));
    if (m_upAction)               m_upAction->setIcon(makeArrowIcon(2, m_isDarkTheme));
    if (m_refreshAction)          m_refreshAction->setIcon(makeRefreshIcon(m_isDarkTheme));
    if (m_copyAction)             m_copyAction->setIcon(makeCopyIcon(m_isDarkTheme));
    if (m_cutAction)              m_cutAction->setIcon(makeCutIcon(m_isDarkTheme));
    if (m_pasteAction)            m_pasteAction->setIcon(makePasteIcon(m_isDarkTheme));
    if (m_deleteAction)           m_deleteAction->setIcon(makeDeleteIcon(m_isDarkTheme));
    if (m_selectAllAction)        m_selectAllAction->setIcon(makeSelectAllIcon(m_isDarkTheme));
    if (m_copyPathAction)         m_copyPathAction->setIcon(makeFolderIcon(m_isDarkTheme));
    if (m_runScriptAction)        m_runScriptAction->setIcon(makeScriptIcon(m_isDarkTheme));
    if (m_searchAction)           m_searchAction->setIcon(makeSearchIcon(m_isDarkTheme));
    if (m_remoteConnectAction)    m_remoteConnectAction->setIcon(makeConnectIcon(m_isDarkTheme));
    if (m_remoteDisconnectAction) m_remoteDisconnectAction->setIcon(makeDisconnectIcon(m_isDarkTheme));
    if (m_hiddenAction)           m_hiddenAction->setIcon(makeEyeIcon(m_isDarkTheme));
    if (m_themeAction)            m_themeAction->setIcon(makeThemeIcon(m_isDarkTheme));
    if (m_settingsAction)         m_settingsAction->setIcon(makeSettingsIcon(m_isDarkTheme));
}

// 打开设置对话框（语言切换等）。OK 后 I18n 会 emit changed()，
// 进而触发 retranslateUi() 即时更新 UI。
void MainWindow::onOpenSettings() {
    SettingsDialog dlg(this);
    dlg.exec();
}

// 重新翻译所有可见 UI 文本。
// 只翻译主窗口自己负责装配的部分（工具栏 + Help 菜单 + 状态栏模板）。
// 右键菜单在弹出时每次重新构造，已用 T() 包裹，无需 retranslate。
// 各子对话框（Search/Remote/Script/About/Settings）自己负责 retranslate。
void MainWindow::retranslateUi() {
    // 工具栏按钮已改为图标显示，这里只更新 ToolTip 文本
    if (m_backAction)           m_backAction->setToolTip(T("Back"));
    if (m_forwardAction)        m_forwardAction->setToolTip(T("Forward"));
    if (m_upAction)             m_upAction->setToolTip(T("Up"));
    if (m_refreshAction)        m_refreshAction->setToolTip(T("Refresh"));
    if (m_copyAction)           m_copyAction->setToolTip(T("Copy"));
    if (m_cutAction)            m_cutAction->setToolTip(T("Cut"));
    if (m_pasteAction)          m_pasteAction->setToolTip(T("Paste"));
    if (m_deleteAction)         m_deleteAction->setToolTip(T("Delete"));
    if (m_selectAllAction)      m_selectAllAction->setToolTip(T("Select All"));
    if (m_copyPathAction)       m_copyPathAction->setToolTip(T("Copy Path"));
    if (m_runScriptAction)      m_runScriptAction->setToolTip(T("Script"));
    if (m_searchAction)         m_searchAction->setToolTip(T("Search"));
    if (m_remoteConnectAction)  m_remoteConnectAction->setToolTip(T("Connect"));
    if (m_remoteDisconnectAction) m_remoteDisconnectAction->setToolTip(T("Disconnect"));
    if (m_hiddenAction)         m_hiddenAction->setToolTip(T("Show/Hide Hidden Files"));
    if (m_themeAction) {
        m_themeAction->setToolTip(m_isDarkTheme ? T("Theme: Dark") : T("Theme: Light"));
    }
    if (m_settingsAction)       m_settingsAction->setToolTip(T("Settings"));

    // 字号同步与状态栏刷新
    applyFonts();
    updateStatusBar();
    // 语言切换后，重新把 action 的新 toolTip 同步到 QToolButton，
    // 否则首次打开时虽然 action->toolTip 被更新，但按钮层仍是旧值（或空）。
    syncToolbarButtonToolTips();
}

// 把 AppFonts 当前字号应用到工具栏 / 菜单栏 / 面板（FileListView）/ 目录树。
// 由 retranslateUi 和 AppFonts::changed 信号双重触发。
void MainWindow::applyFonts() {
    const int menuPt = AppFonts::instance().effectiveMenuFontSize();

    // ---- 工具栏：QToolBar::setFont 不会传给内部 QToolButton。
    //      必须遍历每个 action 的对应 widget 单独 setFont，且对 toolbar 自身
    //      也设一次（影响溢出按钮/spacer 等）。
    if (m_toolbar) {
        QFont f = m_toolbar->font();
        if (menuPt > 0) f.setPointSize(menuPt); else f = QFont();
        m_toolbar->setFont(f);
        const auto actions = m_toolbar->actions();
        for (QAction* a : actions) {
            if (QWidget* w = m_toolbar->widgetForAction(a)) {
                QFont wf = w->font();
                if (menuPt > 0) wf.setPointSize(menuPt); else wf = QFont();
                w->setFont(wf);
            }
        }
    }
    if (menuBar()) {
        QFont mf = menuBar()->font();
        if (menuPt > 0) mf.setPointSize(menuPt); else mf = QFont();
        menuBar()->setFont(mf);
    }

    const int itemPt = AppFonts::instance().effectiveItemFontSize();
    auto applyItemFont = [&](QWidget* w) {
        if (!w) return;
        QFont f = w->font();
        if (itemPt > 0) f.setPointSize(itemPt); else f = QFont();
        w->setFont(f);
    };
    // 文件面板（左/右）：FilePanel 自身没暴露 fileList，但 setFont 在
    // FilePanel 上 Qt 会传播给所有子部件 —— 包括 FileListView 表头与 cell。
    applyItemFont(m_leftPanel);
    applyItemFont(m_rightPanel);
    // 目录树
    applyItemFont(m_dirTree);

    // 工具栏溢出按钮（放不下的 actions 折叠到此处展开）：
    // QSS 的 qproperty-text 在某些 Qt 版本对它不生效（toolButtonStyle 是 IconOnly），
    // 这里直接抓到对象强制 TextOnly + ">>"。该按钮在 toolbar 首次溢出时由 Qt 创建，
    // 所以每次 applyFonts/调整大小都重新尝试一次。
    if (m_toolbar) {
        if (auto* ext = m_toolbar->findChild<QToolButton*>("qt_toolbar_ext_button")) {
            ext->setToolButtonStyle(Qt::ToolButtonTextOnly);
            ext->setText(">>");
            ext->setToolTip(T("More"));
            // 字号跟随菜单字号
            QFont ef = ext->font();
            if (menuPt > 0) ef.setPointSize(menuPt); else ef = QFont();
            ef.setBold(true);
            ext->setFont(ef);
            // 强制宽度足够，避免 macOS 原生样式只给极窄一条
            const int textW = QFontMetrics(ef).horizontalAdvance(">>");
            const int w = qMax(40, textW + 18);
            const int h = qMax(26, QFontMetrics(ef).height() + 10);
            ext->setFixedSize(w, h);
        }
    }
}

// 状态栏：四个 QLabel 平铺，分别显示左面板信息、右面板信息、剪贴板状态、活跃面板提示
void MainWindow::setupStatusBar() {
    m_statusLeft = new QLabel();
    m_statusRight = new QLabel();
    m_statusClipboard = new QLabel();
    m_statusPanel = new QLabel();
    m_statusClock = new QLabel();

    statusBar()->addWidget(m_statusLeft, 1);
    statusBar()->addWidget(m_statusRight, 1);
    statusBar()->addWidget(m_statusClipboard, 1);
    statusBar()->addPermanentWidget(m_statusPanel);
    // Permanent widgets 从右侧开始堆叠；最后 add 的会在最右。
    // 让时钟在最右下角。
    statusBar()->addPermanentWidget(m_statusClock);

    // 每秒刷新时钟；格式：yyyy-MM-dd HH:mm:ss
    auto updateClock = [this]() {
        if (m_statusClock) {
            m_statusClock->setText(
                QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        }
    };
    updateClock();  // 立即显示一次，避免启动后 1 秒内空白
    auto* clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, updateClock);
    clockTimer->start(1000);
}

// 全局快捷键：Cmd+C / Cmd+X / Cmd+V / Cmd+A / Backspace / Cmd+L 等。
// 注意：FileListView 主动 ignore 这些 keyEvent 让它们冒泡到这里，否则
// QAbstractItemView 默认的 Cmd+C/A 行为会抢走。
void MainWindow::setupShortcuts() {
    // ---- Clipboard operations ----
    // Qt's "Ctrl+X" is platform-translated to Cmd+X on macOS automatically,
    // but we ALSO register the explicit Meta+ modifier (Qt::Key_Meta == Cmd
    // on macOS) so the intent is obvious in the code and the shortcut keeps
    // working even if Qt's auto-translation is ever disabled.
    //
    // macOS: Cmd+C / Cmd+X / Cmd+V  (Qt::CTRL is Cmd on Mac)
    // Linux / Windows: Ctrl+C / Ctrl+X / Ctrl+V
    auto* scCopy = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_C), this);
    scCopy->setContext(Qt::WindowShortcut);
    connect(scCopy, &QShortcut::activated, this, &MainWindow::onCopy);

    auto* scCut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_X), this);
    scCut->setContext(Qt::WindowShortcut);
    connect(scCut, &QShortcut::activated, this, &MainWindow::onCut);

    auto* scPaste = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_V), this);
    scPaste->setContext(Qt::WindowShortcut);
    connect(scPaste, &QShortcut::activated, this, &MainWindow::onPaste);

    auto* scSelectAll = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_A), this);
    scSelectAll->setContext(Qt::WindowShortcut);
    connect(scSelectAll, &QShortcut::activated, this, &MainWindow::onSelectAll);

    auto* scDelete = new QShortcut(QKeySequence::Delete, this);
    connect(scDelete, &QShortcut::activated, this, &MainWindow::onDelete);

    auto* scBack = new QShortcut(QKeySequence("Alt+Left"), this);
    connect(scBack, &QShortcut::activated, this, &MainWindow::onBack);

    auto* scForward = new QShortcut(QKeySequence("Alt+Right"), this);
    connect(scForward, &QShortcut::activated, this, &MainWindow::onForward);

    auto* scUp = new QShortcut(QKeySequence("Alt+Up"), this);
    connect(scUp, &QShortcut::activated, this, &MainWindow::onUp);

    auto* scSearch = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(scSearch, &QShortcut::activated, this, &MainWindow::onSearch);

    auto* scScript = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_E), this);
    connect(scScript, &QShortcut::activated, this, &MainWindow::onRunScript);

    // Ctrl+L / Cmd+L — focus the active panel's path bar (browser-style)
    auto* scEditPath = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), this);
    connect(scEditPath, &QShortcut::activated, this, [this]() {
        if (auto* pb = activePanel()->currentPathBar()) {
            pb->enterEditMode();
        }
    });
}

// m_activePane 翻译为对应的 FilePanel 指针 —— 大多数槽（onCopy/onCut 等）
// 都基于 active panel 操作。
FilePanel* MainWindow::activePanel() const {
    return (m_activePane == PanelSide::Left) ? m_leftPanel : m_rightPanel;
}

// ----- 简单导航：直接转发给 active panel -----
void MainWindow::onBack() {
    activePanel()->goBack();
}

void MainWindow::onForward() {
    activePanel()->goForward();
}

void MainWindow::onUp() {
    activePanel()->goUp();
}

// Refresh 只刷当前面板的文件列表，**不**重建左侧目录树。
//
// 之前的版本在这里调了 buildTree()，但 buildTree() 会 clear() 整棵树再从
// 根重建，所有展开的节点被收起 —— 用户每按一次 Refresh 就要重新展开一遍，
// 体验很差。目录树只在"新增/删除目录"等结构性变化时才需要重建，那些操作
// （onDelete / onPaste / onToggleHidden 等）已经各自调了 buildTree()。
//
// 如果未来发现文件外部新增/删除的目录不同步，推荐做法是给 DirectoryTree
// 加一个"只刷新子节点但保留展开状态"的 refreshNode()，而不是全量 rebuild。
void MainWindow::onRefresh() {
    activePanel()->refresh();
}

// 复制：把当前选中条目记入应用内剪贴板。注意这不是系统剪贴板。
// 系统剪贴板（onCopyPath）用来复制路径文本到外部使用。
void MainWindow::onCopy() {
    auto selected = activePanel()->getSelectedEntries();
    if (selected.isEmpty()) return;

    ClipboardData clip;
    clip.entries = selected;
    clip.isCut = false;
    clip.sourcePath = activePanel()->currentPath();
    m_clipboard = clip;
    updateStatusBar();
}

// 剪切：与 onCopy 区别仅 isCut=true，粘贴成功后会删除源（在 onPaste 内处理）
void MainWindow::onCut() {
    auto selected = activePanel()->getSelectedEntries();
    if (selected.isEmpty()) return;

    ClipboardData clip;
    clip.entries = selected;
    clip.isCut = true;
    clip.sourcePath = activePanel()->currentPath();
    m_clipboard = clip;
    updateStatusBar();
}

// 粘贴：从应用内剪贴板把条目复制 / 移动到 active panel 当前目录。
//
// 关键决策：
//   - 跨本地↔远程：当前不支持，明确弹窗，避免 QFile::copy 在 sftp:// 路径上静默失败
//   - 进度推进按"叶子文件计数"而不是按顶层条目数 → 大目录进度条平滑
//   - 移动：先尝试 QFile::rename（同设备 O(1)），跨设备失败时回退到 copy+delete
//   - 用户取消：处理函数返回 false，打断循环但已完成的项保留（不回滚）
void MainWindow::onPaste() {
    if (!m_clipboard.has_value()) return;

    auto& fs = RealFileSystem::instance();
    QString destPath = activePanel()->currentPath();

    // Local<->remote transfers are not implemented yet - bail out loudly
    // instead of silently failing a QFile::copy on an "sftp://..." path.
    if (FileSystemRouter::instance().isRemote(destPath) ||
        FileSystemRouter::instance().isRemote(m_clipboard->sourcePath)) {
        QMessageBox::information(this, "Not supported yet",
            "Paste across local/remote is not supported in this build.");
        return;
    }
    const bool isCut = m_clipboard->isCut;
    const QString verbIng = isCut ? "Moving" : "Copying";
    const QString verbDone = isCut ? "Moved" : "Copied";

    // Pre-scan: total file count across all selected top-level entries.
    // This way progress scales with actual work, not with selection count.
    int totalFiles = 0;
    for (const auto& entry : m_clipboard->entries) {
        totalFiles += fs.countEntries(entry.path);
    }
    if (totalFiles <= 0) totalFiles = 1;  // guard div/zero

    QProgressDialog progress(QString("%1 to %2...").arg(verbIng, destPath),
                             "Cancel", 0, totalFiles, this);
    progress.setWindowTitle(isCut ? "Move" : "Copy");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    progress.setAutoClose(true);
    progress.setAutoReset(true);

    int processed = 0;
    int failedEntries = 0;
    bool canceled = false;

    auto progressFn = [&](const QString& currentPath, int count) -> bool {
        progress.setValue(count);
        QString name = QFileInfo(currentPath).fileName();
        progress.setLabelText(QString("%1: %2").arg(verbIng, name));
        QApplication::processEvents();
        return !progress.wasCanceled();
    };

    for (const auto& entry : m_clipboard->entries) {
        if (progress.wasCanceled()) { canceled = true; break; }

        if (isCut) {
            // Move: try QFile::rename first (fast, same filesystem). Fallback: copy+delete.
            QString destFull = destPath + "/" + QFileInfo(entry.path).fileName();
            if (QFile::rename(entry.path, destFull)) {
                int n = fs.countEntries(destFull);
                processed += n;
                progress.setValue(processed);
                progress.setLabelText(QString("Moved: %1").arg(entry.name));
                QApplication::processEvents();
            } else {
                // Cross-device move — copy then delete
                int before = processed;
                bool ok = fs.copyFileWithProgress(entry.path, destPath, progressFn, processed);
                if (!ok) {
                    if (progress.wasCanceled()) { canceled = true; break; }
                    failedEntries++;
                    processed = before + fs.countEntries(entry.path);  // advance past failed
                    continue;
                }
                int dummy = 0;
                fs.deleteDirectoryWithProgress(entry.path, nullptr, dummy);
            }
        } else {
            // Copy
            bool ok = fs.copyFileWithProgress(entry.path, destPath, progressFn, processed);
            if (!ok) {
                if (progress.wasCanceled()) { canceled = true; break; }
                failedEntries++;
                processed += fs.countEntries(entry.path);  // skip past failed count
            }
        }
    }
    progress.setValue(totalFiles);

    if (canceled) {
        statusBar()->showMessage(
            QString("%1 canceled").arg(verbDone), 3000);
    } else if (failedEntries == 0) {
        statusBar()->showMessage(
            QString("%1 %2 item(s) to %3")
                .arg(verbDone).arg(m_clipboard->entries.size()).arg(destPath),
            3000);
    } else {
        statusBar()->showMessage(
            QString("%1 with %2 failed item(s)").arg(verbDone).arg(failedEntries),
            3000);
    }

    if (isCut) {
        m_clipboard.reset();
    }
    activePanel()->refresh();
    updateStatusBar();
}

// 拖拽复制：FilePanel 在 dropEvent 里 emit dropCopyRequested 触发本槽。
// 与 onPaste 几乎对称（同样的远程边界守卫 + 进度对话框 + 失败计数），
// 只是固定 isCut=false（拖拽语义就是"复制过去"）。完成后刷新的是
// **目标侧** 面板而非 active panel —— 用户拖到非 active 那侧也会立刻看到。
void MainWindow::onDropCopy(const QStringList& sourcePaths,
                            const QString& destPath,
                            PanelSide destSide) {
    if (sourcePaths.isEmpty()) return;

    // Drag&drop across local <-> remote is not supported in this build,
    // mirroring onPaste's policy. Fail loud, not silent.
    if (FileSystemRouter::instance().isRemote(destPath)) {
        QMessageBox::information(this, "Not supported yet",
            "Dropping into a remote (SFTP) folder is not supported in this build.");
        return;
    }
    for (const QString& p : sourcePaths) {
        if (FileSystemRouter::instance().isRemote(p)) {
            QMessageBox::information(this, "Not supported yet",
                "Dropping remote files into a local folder is not supported in this build.");
            return;
        }
    }

    auto& fs = RealFileSystem::instance();

    // Pre-scan total work units so progress scales with leaf-file count.
    int totalFiles = 0;
    for (const QString& p : sourcePaths) {
        totalFiles += fs.countEntries(p);
    }
    if (totalFiles <= 0) totalFiles = 1;

    QProgressDialog progress(QString("Copying to %1...").arg(destPath),
                             "Cancel", 0, totalFiles, this);
    progress.setWindowTitle("Drop Copy");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    progress.setAutoClose(true);
    progress.setAutoReset(true);

    int processed = 0;
    int failed    = 0;
    bool canceled = false;

    auto progressFn = [&](const QString& currentPath, int count) -> bool {
        progress.setValue(count);
        progress.setLabelText(QString("Copying: %1")
            .arg(QFileInfo(currentPath).fileName()));
        QApplication::processEvents();
        return !progress.wasCanceled();
    };

    for (const QString& src : sourcePaths) {
        if (progress.wasCanceled()) { canceled = true; break; }
        bool ok = fs.copyFileWithProgress(src, destPath, progressFn, processed);
        if (!ok) {
            if (progress.wasCanceled()) { canceled = true; break; }
            failed++;
            processed += fs.countEntries(src);  // skip past failed entry
        }
    }
    progress.setValue(totalFiles);

    if (canceled) {
        statusBar()->showMessage("Drop copy canceled", 3000);
    } else if (failed == 0) {
        statusBar()->showMessage(
            QString("Copied %1 item(s) to %2").arg(sourcePaths.size()).arg(destPath),
            3000);
    } else {
        statusBar()->showMessage(
            QString("Copied with %1 failed item(s)").arg(failed), 3000);
    }

    // Refresh the *destination* panel (where the drop landed) so the new
    // entries become visible immediately.
    FilePanel* destPanel = (destSide == PanelSide::Left) ? m_leftPanel : m_rightPanel;
    destPanel->refresh();
    updateStatusBar();
}

// 删除：弹确认 + 进度对话框 + 失败计数。远程不支持。
// 注意 deleteDirectoryWithProgress 走自底向上递归，rmdir 要求空目录。
void MainWindow::onDelete() {
    auto selected = activePanel()->getSelectedEntries();
    if (selected.isEmpty()) return;

    // Guard: remote delete is not implemented in this build.
    for (const auto& e : selected) {
        if (FileSystemRouter::instance().isRemote(e.path)) {
            QMessageBox::information(this, "Not supported yet",
                "Deleting remote files is not supported in this build.");
            return;
        }
    }

    auto reply = QMessageBox::question(this, "Delete",
        QString("Delete %1 item(s)? This cannot be undone.").arg(selected.size()),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    auto& fs = RealFileSystem::instance();

    // Pre-scan: count every leaf file so progress scales with real work
    int totalFiles = 0;
    for (const auto& entry : selected) {
        totalFiles += fs.countEntries(entry.path);
    }
    if (totalFiles <= 0) totalFiles = 1;

    QProgressDialog progress(QString("Deleting %1 item(s)...").arg(selected.size()),
                             "Cancel", 0, totalFiles, this);
    progress.setWindowTitle("Delete");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    progress.setAutoClose(true);
    progress.setAutoReset(true);

    int processed = 0;
    int failedEntries = 0;
    bool canceled = false;

    auto progressFn = [&](const QString& currentPath, int count) -> bool {
        progress.setValue(count);
        QString name = QFileInfo(currentPath).fileName();
        progress.setLabelText(QString("Deleting: %1").arg(name));
        QApplication::processEvents();
        return !progress.wasCanceled();
    };

    for (const auto& entry : selected) {
        if (progress.wasCanceled()) { canceled = true; break; }

        bool ok = fs.deleteDirectoryWithProgress(entry.path, progressFn, processed);
        if (!ok) {
            if (progress.wasCanceled()) { canceled = true; break; }
            failedEntries++;
        }
    }
    progress.setValue(totalFiles);

    if (canceled) {
        statusBar()->showMessage("Delete canceled", 3000);
    } else if (failedEntries == 0) {
        statusBar()->showMessage(
            QString("Deleted %1 item(s)").arg(selected.size()), 3000);
    } else {
        statusBar()->showMessage(
            QString("Deleted with %1 failed item(s)").arg(failedEntries), 3000);
    }
    activePanel()->refresh();
}

void MainWindow::onSelectAll() {
    activePanel()->selectAll();
}

// 把当前路径写入"系统"剪贴板（QGuiApplication::clipboard），方便用户在
// 终端 / 浏览器 / 其它应用粘贴。区别于应用内 m_clipboard（那个存条目）。
void MainWindow::onCopyPath() {
    auto selected = activePanel()->getSelectedEntries();
    QString paths;
    if (selected.isEmpty()) {
        paths = activePanel()->currentPath();
    } else {
        QStringList pathList;
        for (const auto& e : selected) {
            pathList.append(e.path);
        }
        paths = pathList.join("\n");
    }
    QApplication::clipboard()->setText(paths);
    statusBar()->showMessage("Path copied to clipboard", 2000);
}

// 弹出 ScriptDialog 选脚本/参数运行（用 QProcess 异步执行）
void MainWindow::onRunScript() {
    auto selected = activePanel()->getSelectedEntries();
    ScriptDialog dlg(selected, this);
    dlg.exec();
}

// 弹出 SearchDialog 在 active panel 当前目录递归搜索；点击结果会
// navigateTo 该项的父目录。
void MainWindow::onSearch() {
    SearchDialog dlg(activePanel()->currentPath(), this);
    connect(&dlg, &SearchDialog::navigateToPath, this, [this](const QString& path) {
        activePanel()->navigateTo(path);
    });
    dlg.exec();
}

// ---------------------------------------------------------------------------
// 压缩为 zip：把 active panel 当前选中的条目（可包含目录）打成一个 .zip 放
// 到当前目录下。实现走系统 /usr/bin/zip（macOS 自带 Info-ZIP），跨平台解压
// 兼容性最好。
//
// 关键决策：
//   - 子进程异步：用 QProcess + QProgressDialog；stderr 出现 "  adding: <name>"
//     时推进度（按文件计数对照 RealFileSystem::countEntries 估的 totalFiles）。
//   - 取消：QProgressDialog::wasCanceled → kill QProcess → 删掉部分写入的 .zip。
//   - 工作目录：把 zip 进程 chdir 到 active panel 当前目录，然后用相对路径作
//     输入；这样 zip 包内的目录树是干净的（"foo/bar.txt" 而不是
//     "/Users/me/proj/foo/bar.txt"）。
//   - 命名冲突：目标 .zip 已存在则追加 "-2"、"-3"…
//   - 远程：本地 zip 命令操作不了 sftp:// 路径，明确弹窗拒绝。
// ---------------------------------------------------------------------------
void MainWindow::onCompressZip() {
    auto selected = activePanel()->getSelectedEntries();
    if (selected.isEmpty()) return;

    QString cwd = activePanel()->currentPath();

    // 远程边界守卫：源或目标任一在远端就拒绝
    if (FileSystemRouter::instance().isRemote(cwd)) {
        QMessageBox::information(this, "Not supported yet",
            "Compressing into a remote (SFTP) folder is not supported in this build.");
        return;
    }
    for (const auto& e : selected) {
        if (FileSystemRouter::instance().isRemote(e.path)) {
            QMessageBox::information(this, "Not supported yet",
                "Compressing remote files is not supported in this build.");
            return;
        }
    }

    // 决定输出文件名：单选用 basename，多选用 "Archive"，已存在则加后缀
    QString baseName;
    if (selected.size() == 1) {
        baseName = QFileInfo(selected.first().path).completeBaseName();
        if (baseName.isEmpty()) baseName = selected.first().name;
    } else {
        baseName = "Archive";
    }
    QString outPath = cwd + "/" + baseName + ".zip";
    int counter = 2;
    while (QFileInfo::exists(outPath)) {
        outPath = cwd + "/" + baseName + "-" + QString::number(counter++) + ".zip";
    }

    // 估算总文件数（zip 每加一个文件输出一行，作为进度刻度）
    auto& fs = RealFileSystem::instance();
    int totalFiles = 0;
    for (const auto& e : selected) totalFiles += fs.countEntries(e.path);
    if (totalFiles <= 0) totalFiles = 1;

    QProgressDialog progress(QString("Compressing to %1 ...")
                                 .arg(QFileInfo(outPath).fileName()),
                             "Cancel", 0, totalFiles, this);
    progress.setWindowTitle("Compress");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    progress.setAutoClose(true);
    progress.setAutoReset(true);

    // 装配 zip 命令：-r 递归，-q 关掉冗余输出，但用 -dg / -du 等选项打不开
    // "每文件一行"，所以这里改用 -v（verbose）的中间方案：去掉 -q，让 zip
    // 在 stderr 输出 "  adding: <name>"；我们扫这些行计数。
    QProcess proc;
    proc.setWorkingDirectory(cwd);
    proc.setProcessChannelMode(QProcess::MergedChannels);  // stdout/stderr 合并

    int processed = 0;
    bool canceled = false;
    QObject::connect(&proc, &QProcess::readyRead, &progress, [&]() {
        const QByteArray chunk = proc.readAll();
        // 每出现一行 "adding: " / "updating: " 就算前进一格
        int adds = chunk.count("\n  adding:")
                 + chunk.count("\n  updating:");
        // 处理首行（没有 leading newline 的情况）
        if (chunk.startsWith("  adding:") || chunk.startsWith("  updating:"))
            adds += 1;
        processed += adds;
        if (processed > totalFiles) processed = totalFiles;
        progress.setValue(processed);
    });

    QObject::connect(&progress, &QProgressDialog::canceled, &proc, [&]() {
        canceled = true;
        if (proc.state() != QProcess::NotRunning) proc.kill();
    });

    QStringList args{"-r", "-X", outPath};   // -X 不写入 macOS 扩展属性
    for (const auto& e : selected) {
        // 用相对路径，让 zip 内部的目录结构干净
        args << QFileInfo(e.path).fileName();
    }
    proc.start("/usr/bin/zip", args);
    if (!proc.waitForStarted(3000)) {
        QMessageBox::warning(this, "Compress failed",
            QString("Could not start /usr/bin/zip: %1").arg(proc.errorString()));
        return;
    }

    // 阻塞等待结束（同时让 progress 处理事件循环）
    while (proc.state() != QProcess::NotRunning) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        proc.waitForFinished(50);
    }
    progress.setValue(totalFiles);

    bool compressedOk = false;
    if (canceled) {
        // 删掉可能写了一半的输出文件
        QFile::remove(outPath);
        statusBar()->showMessage("Compression canceled", 3000);
    } else if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
        compressedOk = true;
        statusBar()->showMessage(
            QString("Created %1 (%2 item(s))")
                .arg(QFileInfo(outPath).fileName()).arg(selected.size()),
            4000);
    } else {
        QFile::remove(outPath);
        QString stderrText = QString::fromUtf8(proc.readAll());
        QMessageBox::warning(this, "Compress failed",
            QString("zip exited with code %1\n\n%2")
                .arg(proc.exitCode()).arg(stderrText.left(500)));
    }

    activePanel()->refresh();

    // 压缩成功 → 自动选中新生成的 .zip 文件，与解压完成后选中输出目录的
    // 行为对偶。selectByPath 找不到时无声跳过（用户可能切换了 active panel
    // 或导航走了），不会打扰用户。
    if (compressedOk) {
        if (auto* fl = activePanel()->currentFileList()) {
            fl->selectByPath(outPath);
        }
    }
    updateStatusBar();
}

// ---------------------------------------------------------------------------
// 解压：双击 .zip / .tar.gz / .tgz / .tar.bz2 / .tbz / .tar.xz / .txz / .tar
// 时调用。在压缩包所在目录创建 <basename>/ 子目录并解压进去（同名冲突追加
// "-2/-3..."），不让系统 Finder/Archive Utility 接管。
//
// 命令分发：
//   .zip                       → /usr/bin/unzip -q -o <archive> -d <outDir>
//   .tar / .tar.gz / .tgz /
//   .tar.bz2 / .tbz / .tbz2 /
//   .tar.xz / .txz             → /usr/bin/tar -xf <archive> -C <outDir>
//                                （tar 自动识别 gz/bz2/xz）
//
// 取消：QProgressDialog::canceled → kill 子进程 → 删半成品输出目录。
// 远程：远程压缩包不会触发本槽（FileListView 已过滤 sftp://）；保险起见
// 这里再守一道。
// ---------------------------------------------------------------------------
void MainWindow::onExtractArchive(const QString& archivePath, PanelSide side) {
    if (FileSystemRouter::instance().isRemote(archivePath)) {
        QMessageBox::information(this, "Not supported yet",
            "Extracting remote archives is not supported in this build.");
        return;
    }
    QFileInfo fi(archivePath);
    if (!fi.exists() || !fi.isFile()) return;

    // 推断 "干净" basename：去掉所有压缩链后缀
    //   foo.tar.gz  → foo
    //   bar.tgz     → bar
    //   baz.zip     → baz
    QString lower = fi.fileName().toLower();
    QString stem  = fi.fileName();
    auto stripIfMatch = [&](const QString& suf) {
        if (lower.endsWith(suf)) {
            stem  = stem.left(stem.size() - suf.size());
            lower = lower.left(lower.size() - suf.size());
            return true;
        }
        return false;
    };
    // 复合后缀优先（先剥 .gz/.bz2/.xz 再剥 .tar 不行，因为可能本就是 .tar）
    if (!stripIfMatch(".tar.gz")  && !stripIfMatch(".tgz")
     && !stripIfMatch(".tar.bz2") && !stripIfMatch(".tbz2")
     && !stripIfMatch(".tbz")
     && !stripIfMatch(".tar.xz")  && !stripIfMatch(".txz")
     && !stripIfMatch(".zip")
     && !stripIfMatch(".tar")) {
        // 不该走到这里 —— FileListView::isSupportedArchive 已经过滤
        return;
    }
    if (stem.isEmpty()) stem = "Extracted";

    const QString parentDir = fi.absolutePath();
    QString outDir = parentDir + "/" + stem;
    int counter = 2;
    while (QFileInfo::exists(outDir)) {
        outDir = parentDir + "/" + stem + "-" + QString::number(counter++);
    }
    if (!QDir().mkpath(outDir)) {
        QMessageBox::warning(this, "Extract failed",
            QString("Could not create output directory:\n%1").arg(outDir));
        return;
    }

    // 选解压命令
    QString program;
    QStringList args;
    if (lower.isEmpty()) lower = fi.fileName().toLower();   // safety
    const QString origLower = fi.fileName().toLower();
    if (origLower.endsWith(".zip")) {
        program = "/usr/bin/unzip";
        // -q 安静模式（仍会输出每个文件名到 stdout，便于推进度），
        // -o 覆盖已存在文件而不询问，-d 指定输出目录
        args << "-o" << archivePath << "-d" << outDir;
    } else {
        // tar 自带 gzip/bzip2/xz 自动识别（-x 即 detect-and-decompress）
        program = "/usr/bin/tar";
        args << "-xvf" << archivePath << "-C" << outDir;
        // -v 让 tar 每解一个文件输出一行到 stderr，便于推进度
    }

    // 进度估算：用压缩包大小做 progressMax 的代替（解出条目数事先不知道）。
    // 我们按"解压输出行数"推一个粗略进度；若行数远超估算，clamp 在 max-1。
    int totalRough = qMax(1, int(fi.size() / 1024)); // 每 KB 算 1 个 tick
    QProgressDialog progress(QString("Extracting %1 ...")
                                 .arg(fi.fileName()),
                             "Cancel", 0, totalRough, this);
    progress.setWindowTitle("Extract");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    progress.setAutoClose(true);
    progress.setAutoReset(true);

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);

    int ticks = 0;
    bool canceled = false;
    QObject::connect(&proc, &QProcess::readyRead, &progress, [&]() {
        const QByteArray chunk = proc.readAll();
        // 每行 ≈ 一个文件
        ticks += chunk.count('\n');
        int show = qMin(ticks * 4, totalRough - 1);   // 给 max-1 留点余量
        progress.setValue(show);
    });
    QObject::connect(&progress, &QProgressDialog::canceled, &proc, [&]() {
        canceled = true;
        if (proc.state() != QProcess::NotRunning) proc.kill();
    });

    proc.start(program, args);
    if (!proc.waitForStarted(3000)) {
        QMessageBox::warning(this, "Extract failed",
            QString("Could not start %1: %2").arg(program, proc.errorString()));
        QDir(outDir).removeRecursively();
        return;
    }
    while (proc.state() != QProcess::NotRunning) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        proc.waitForFinished(50);
    }
    progress.setValue(totalRough);

    bool extractedOk = false;
    if (canceled) {
        QDir(outDir).removeRecursively();
        statusBar()->showMessage("Extraction canceled", 3000);
    } else if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
        extractedOk = true;
        statusBar()->showMessage(
            QString("Extracted to %1").arg(QFileInfo(outDir).fileName()),
            4000);
    } else {
        QString stderrText = QString::fromUtf8(proc.readAll());
        QDir(outDir).removeRecursively();
        QMessageBox::warning(this, "Extract failed",
            QString("%1 exited with code %2\n\n%3")
                .arg(program).arg(proc.exitCode()).arg(stderrText.left(500)));
    }

    // 刷新解压发起的那一侧面板（不一定是 active）
    FilePanel* panel = (side == PanelSide::Left) ? m_leftPanel : m_rightPanel;
    panel->refresh();

    // 解压成功 → 自动选中新生成的输出目录，让用户立刻看到结果在哪。
    // 仅在面板当前还停留在压缩包所在目录时才选中（用户可能已经导航走了，
    // 那时 outDir 不在视图里，selectByPath 会无声失败也无害）。
    if (extractedOk) {
        if (auto* fl = panel->currentFileList()) {
            fl->selectByPath(outDir);
        }
    }
    updateStatusBar();
}

// 远程连接：弹 RemoteDialog → 用户填表 → SftpClient::connectAndAuth →
// 成功后 RemoteDialog::takeClient() 转交活会话给我们 → 挂到 router → 跳到 sftp://
// mountPrefix 同时也是 router 里的 key，断开时用它来 unmount。
void MainWindow::onRemoteConnect() {
    RemoteDialog dlg(this);
    bool succeeded = false;
    RemoteConnection accepted;
    connect(&dlg, &RemoteDialog::connectRequested, this,
            [&succeeded, &accepted](const RemoteConnection& conn) {
        succeeded = true;
        accepted = conn;
    });
    if (dlg.exec() != QDialog::Accepted || !succeeded) {
        return;  // user cancelled, or connect failed (dialog already showed why)
    }
    auto client = dlg.takeClient();
    if (!client) {
        statusBar()->showMessage("Internal error: no live session handed off", 3000);
        return;
    }

    // Build a mount prefix and register the live session with the router so
    // every panel / tree listing that walks into this URL goes over SFTP.
    const QString mount = QString("sftp://%1@%2:%3")
        .arg(accepted.username, accepted.host).arg(accepted.port);

    // If there's a stale mount at the same prefix (reconnect), drop it first
    // so the old session is torn down deterministically.
    if (!m_remoteMountPrefix.isEmpty()) {
        FileSystemRouter::instance().unmount(m_remoteMountPrefix);
    }
    FileSystemRouter::instance().mount(mount, std::move(client));
    m_remoteMountPrefix = mount;

    m_isRemoteConnected = true;
    // Navigate the active panel into the remote root. The router will
    // transparently serve the directory listing over SFTP.
    activePanel()->navigateTo(mount + "/");
    m_remoteConnectAction->setEnabled(false);
    m_remoteDisconnectAction->setEnabled(true);
    statusBar()->showMessage(
        QString("Connected: %1  (%2)")
            .arg(mount)
            .arg(accepted.fingerprint.isEmpty() ? "no fingerprint" : accepted.fingerprint),
        5000);
    updateStatusBar();
}

// 远程断开：先把双面板都导航回本地 home，再 unmount。
// 顺序很重要：unmount 会析构 SftpClient（disconnect SSH），如果先 unmount，
// 仍指向 sftp:// 的面板下一次 refresh 会撞到死指针。
void MainWindow::onRemoteDisconnect() {
    // Send panels home BEFORE tearing the mount down, otherwise any pending
    // refresh would hit a dead client.
    activePanel()->navigateTo(QDir::homePath());
    m_leftPanel->navigateTo(QDir::homePath());
    m_rightPanel->navigateTo(QDir::homePath());

    if (!m_remoteMountPrefix.isEmpty()) {
        FileSystemRouter::instance().unmount(m_remoteMountPrefix);
        m_remoteMountPrefix.clear();
    }

    m_isRemoteConnected = false;
    m_dirTree->buildTree();
    m_dirTree->highlightPath(m_leftPanel->currentPath(), PanelSide::Left);
    m_dirTree->highlightPath(m_rightPanel->currentPath(), PanelSide::Right);
    m_remoteConnectAction->setEnabled(true);
    m_remoteDisconnectAction->setEnabled(false);
    statusBar()->showMessage("Disconnected - returned to home directory", 3000);
    updateStatusBar();
}

void MainWindow::onToggleTheme() {
    m_isDarkTheme = !m_isDarkTheme;
    if (m_isDarkTheme) {
        applyDarkTheme();
        m_themeAction->setToolTip(T("Theme: Dark"));
    } else {
        applyLightTheme();
        m_themeAction->setToolTip(T("Theme: Light"));
    }
    // 主题切换后 palette 已变，重建图标让描边色跟随新主题
    refreshToolbarIcons();
}

// 隐藏文件开关：通过 router 一次性广播到本地+所有 SFTP 后端，再刷新两面板
void MainWindow::onToggleHidden() {
    bool show = m_hiddenAction->isChecked();
    // Propagate to every backend (local + every live SFTP mount) in one call.
    FileSystemRouter::instance().setShowHidden(show);

    // Refresh both panels so the listings reflect the new filter immediately
    m_leftPanel->refresh();
    m_rightPanel->refresh();

    // 增量同步左侧目录树：在保留展开/滚动/选中状态的前提下，让所有
    // *已加载* 节点的 children 跟随当前 showHidden 状态出现/消失。
    // 未加载节点不动，下次展开时 lazyPopulate 自然按当前 filter 拉取。
    // 不再使用 buildTree() —— 那会 clear() 整棵树并丢失所有用户上下文。
    m_dirTree->refreshHiddenVisibility();

    statusBar()->showMessage(show ? "Showing hidden files" : "Hiding hidden files", 2000);
}

// 面板路径变了：同步左侧目录树高亮 + 状态栏文字
void MainWindow::onPanelPathChanged(const QString& path, PanelSide side) {
    m_dirTree->highlightPath(path, side);
    // 仅当变化的是 active 面板时，把树滚动并居中到该项；
    // 否则只展开父级链（保持高亮可见，但不抢用户视野）。
    if (side == m_activePane) {
        m_dirTree->revealPath(path);
    } else {
        m_dirTree->expandToPath(path);
    }
    updateStatusBar();
}

// 面板内选中项变化 —— 刷新状态栏即可
void MainWindow::onPanelSelectionChanged(PanelSide /*side*/) {
    updateStatusBar();
}

// 哪个面板是 "active"（最近被点击/获焦）—— 决定快捷键和工具栏作用对象
void MainWindow::onPanelActivated(PanelSide side) {
    m_activePane = side;
    updateStatusBar();
    // 让左侧目录树跟随激活面板：展开+滚动+选中对应目录项。
    // revealPath 对远程/未在树中的路径静默跳过，且内部阻断信号，
    // 不会回调 directorySelected -> navigateTo，避免重入。
    if (m_dirTree) {
        m_dirTree->revealPath(activePanel()->currentPath());
    }
}

// 左侧目录树点击：导航 active panel 过去，并把目录树自身滚动到该项
// 让"双向联动"可见：点树 → active 面板跳转 + 两侧树高亮均由 pathChanged 回调更新。
void MainWindow::onDirectoryTreeSelected(const QString& path) {
    if (path.isEmpty()) return;
    // 让 active 面板跳转。FilePanel::navigateTo 会 emit pathChanged，
    // 继而 onPanelPathChanged 调用 highlightPath + expandToPath 同步树。
    activePanel()->navigateTo(path);
    // 显式把树滚动到被点项中心 + 聚焦高亮——确保视觉反馈明确（防止
    // 用户误以为"点了却没跳"：比如此前 active 面板和树上被点项完全同路径，
    // 或树因 setCurrentItem 行为未把视图居中）。
    if (m_dirTree) {
        m_dirTree->revealPath(path);
    }
    // 同时刷新对侧的高亮状态（pathChanged 回调只改动 active 一侧）。
    m_dirTree->highlightPath(activePanel()->currentPath(), m_activePane);
    PanelSide other = (m_activePane == PanelSide::Left) ? PanelSide::Right : PanelSide::Left;
    FilePanel* otherPanel = (other == PanelSide::Left) ? m_leftPanel : m_rightPanel;
    m_dirTree->highlightPath(otherPanel->currentPath(), other);
}

// ---------------------------------------------------------------------------
// "打开方式..." 子菜单的辅助函数（仅本翻译单元用）
//
// 设计选择：不引入 Objective-C++。在 macOS 上用纯命令行机制：
//   - 默认打开 → QDesktopServices::openUrl（走 Launch Services 默认绑定）
//   - 指定应用打开 → `open -a "<AppName>" <file>`，等同 Finder "用...打开"
//   - "选择其它应用..." → AppleScript `choose application` 弹标准选择器，
//     再通过 `open -a` 启动
//   - 候选列表 → 扫描 /Applications 与 ~/Applications 下的 .app，挑常见
//     编辑器/查看器（VS Code、Sublime、TextEdit、Preview、IntelliJ、
//     BBEdit、Atom 等），仅当这些 .app 实际存在时才展示
//
// 这种做法：零 Obj-C 依赖、跨用户应用目录、保留 macOS 原生选择对话框体验。
// 局限：候选列表不会自动按文件类型 narrow，但用户随时可走"选择其它应用..."
// 弹原生对话框做精确选择。
// ---------------------------------------------------------------------------
namespace {

/// 用系统默认应用打开 path（Finder/Launch Services / xdg-open / ShellExecute）
void openWithDefault(const QString& path) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

/// macOS 上：用指定 .app（绝对路径或应用显示名）打开 file
void openWithApp(const QString& appNameOrPath, const QString& filePath) {
    // open(1) 是 macOS 自带工具；-a 接应用名或 .app 路径都行
    QProcess::startDetached("/usr/bin/open",
        QStringList{ "-a", appNameOrPath, filePath });
}

/// 弹 macOS 原生"选择应用"对话框（AppleScript），用户挑完用其打开 file。
/// 用户取消则什么都不做。
void chooseAndOpen(const QString& filePath) {
    // AppleScript 的 'choose application as alias' 返回选中应用的 alias，
    // 取 POSIX path 即得到 .app 绝对路径
    const QString script = QStringLiteral(
        "set theApp to choose application as alias\n"
        "POSIX path of theApp");

    QProcess proc;
    proc.start("/usr/bin/osascript", QStringList{"-e", script});
    if (!proc.waitForFinished(60000)) {       // 用户可能要慢慢挑
        proc.kill();
        return;
    }
    if (proc.exitCode() != 0) return;          // 用户取消会非 0 退出
    QString appPath = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    if (appPath.isEmpty()) return;
    if (appPath.endsWith('/')) appPath.chop(1);  // POSIX path 末尾常带 /
    openWithApp(appPath, filePath);
}

/// 扫描 macOS 上常见的 .app 目录，挑出"常用编辑器/查看器"中实际安装的那些。
/// 返回 (显示名, .app 路径) 列表，按显示名字典序。
QVector<QPair<QString, QString>> commonInstalledApps() {
    // 候选清单：覆盖大多数开发者/普通用户的"打开方式"诉求
    static const char* const kCandidates[] = {
        "Visual Studio Code.app",
        "Sublime Text.app",
        "TextEdit.app",
        "Preview.app",
        "Xcode.app",
        "BBEdit.app",
        "Atom.app",
        "IntelliJ IDEA.app",
        "PyCharm.app",
        "CLion.app",
        "Cursor.app",
        "Zed.app",
        "MacVim.app",
        "Safari.app",
        "Google Chrome.app",
        "Firefox.app",
        "iTerm.app",
        "Terminal.app",
    };

    // 候选目录：系统级 + 用户级
    const QStringList searchRoots {
        QStringLiteral("/Applications"),
        QDir::homePath() + "/Applications",
    };

    QVector<QPair<QString, QString>> found;
    for (const char* name : kCandidates) {
        for (const QString& root : searchRoots) {
            QString full = root + "/" + QString::fromLatin1(name);
            if (QFileInfo::exists(full)) {
                QString display = QString::fromLatin1(name);
                display.chop(4);    // 去掉 ".app" 后缀
                found.append(qMakePair(display, full));
                break;              // 同名只收第一份
            }
        }
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });
    return found;
}

} // namespace

// 右键菜单：动态构建（Open / Copy / Cut / Paste / Delete / Copy Path / ...）。
// 项是否可用 / 是否出现根据当前选中状态决定。
//
// "Open With" 子菜单仅在恰好选中一个 *本地、非目录* 条目时显示 ——
// 远程 SFTP 路径无法用 macOS open(1) 启动；多选语义不清；目录用双击打开。
void MainWindow::onContextMenu(const QPoint& pos, PanelSide side) {
    m_activePane = side;

    QMenu menu(this);
    // 把右键菜单字号同步到全局 menu 字号（AppFonts），保持一致。
    {
        const int pt = AppFonts::instance().effectiveMenuFontSize();
        if (pt > 0) {
            QFont mf = menu.font();
            mf.setPointSize(pt);
            menu.setFont(mf);
        }
    }

    // ---- "Open With" / "Open" 子菜单（仅单选本地文件时展示） ----
    QVector<FileEntry> sel = activePanel()->getSelectedEntries();
    bool canOfferOpenWith = false;
    QString openTargetPath;
    if (sel.size() == 1) {
        const FileEntry& e = sel.first();
        if (!e.isDirectory && !FileSystemRouter::instance().isRemote(e.path)) {
            canOfferOpenWith = true;
            openTargetPath = e.path;
        }
    }

    if (canOfferOpenWith) {
        // "Open" 顶层项 = 双击行为（系统默认应用）
        menu.addAction(T("Open"), this, [openTargetPath]() {
            openWithDefault(openTargetPath);
        });

        // "Open With ▶" 子菜单：列出已安装的常用应用 + "其它..."
        QMenu* openWithMenu = menu.addMenu(T("Open With"));

        // Finder 风格的"默认应用"放在子菜单顶端
        openWithMenu->addAction(T("Default Application"), this,
            [openTargetPath]() { openWithDefault(openTargetPath); });
        openWithMenu->addSeparator();

        const auto apps = commonInstalledApps();
        for (const auto& app : apps) {
            const QString display = app.first;
            const QString appPath = app.second;
            openWithMenu->addAction(display, this,
                [appPath, openTargetPath]() {
                    openWithApp(appPath, openTargetPath);
                });
        }

        if (!apps.isEmpty()) openWithMenu->addSeparator();

        openWithMenu->addAction(T("Other Application…"), this,
            [openTargetPath]() { chooseAndOpen(openTargetPath); });

        menu.addSeparator();
    }

    // ---- "New File" / "New Folder" 顶端项 ----
    // 在当前 active panel 的 cwd 下创建文件/目录。远程路径走 information 弹窗
    // （由槽自身处理，这里直接把 cwd / isRemote 透传过去——避免 lambda 捕获
    // 时再算一次）。
    {
        const QString cwd = activePanel()->currentPath();
        const bool isRemote = FileSystemRouter::instance().isRemote(cwd);

        menu.addAction(T("New File…"), this, [this, cwd, isRemote]() {
            onCreateNewFile(cwd, isRemote);
        });
        menu.addAction(T("New Folder…"), this, [this, cwd, isRemote]() {
            onCreateNewFolder(cwd, isRemote);
        });
        menu.addSeparator();
    }

    // NOTE: "\tCtrl+X" in Qt QAction text is auto-translated to "⌘X" display
    // on macOS. The actual QShortcut bindings are registered in setupShortcuts()
    // using Qt::CTRL | Qt::Key_X (which maps to Cmd on macOS, Ctrl elsewhere).
    menu.addAction(TS("Copy\tCtrl+C"), this, &MainWindow::onCopy);
    menu.addAction(TS("Cut\tCtrl+X"), this, &MainWindow::onCut);
    menu.addAction(TS("Paste\tCtrl+V"), this, &MainWindow::onPaste);
    menu.addSeparator();
    // Rename 仅在恰好选中 1 项时启用（多选重命名语义不明确）。
    {
        QAction* renameAct = menu.addAction(T("Rename…"));
        if (sel.size() == 1) {
            const QString targetPath = sel.first().path;
            connect(renameAct, &QAction::triggered, this,
                [this, targetPath]() { onRenameEntry(targetPath); });
        } else {
            renameAct->setEnabled(false);
        }
    }
    menu.addAction(TS("Delete\tDel"), this, &MainWindow::onDelete);
    menu.addSeparator();
    menu.addAction(T("Copy Path"), this, &MainWindow::onCopyPath);
    menu.addAction(T("Copy File Name"), this, [this]() {
        auto selected = activePanel()->getSelectedEntries();
        if (!selected.isEmpty()) {
            QStringList names;
            for (const auto& e : selected) {
                names.append(e.name);
            }
            QApplication::clipboard()->setText(names.join("\n"));
            statusBar()->showMessage(T("File name(s) copied"), 2000);
        }
    });
    menu.addSeparator();

    // 仅当至少选了一项、且全部是本地路径时显示"压缩为 ZIP"
    bool hasLocalSelection = !sel.isEmpty();
    for (const auto& e : sel) {
        if (FileSystemRouter::instance().isRemote(e.path)) {
            hasLocalSelection = false; break;
        }
    }
    if (hasLocalSelection) {
        const QString label = (sel.size() == 1)
            ? QString(I18n::instance().current() == I18n::Lang::Zh
                      ? "压缩 \"%1\" 为 ZIP" : "Compress \"%1\" to ZIP").arg(sel.first().name)
            : QString(I18n::instance().current() == I18n::Lang::Zh
                      ? "压缩 %1 项为 ZIP" : "Compress %1 items to ZIP").arg(sel.size());
        menu.addAction(label, this, &MainWindow::onCompressZip);
        menu.addSeparator();
    }

    menu.addAction(TS("Run Script\tCtrl+E"), this, &MainWindow::onRunScript);

    menu.exec(pos);
}

// ---------------------------------------------------------------------------
// 新建文件 / 新建目录（右键菜单触发）
//
// 共同流程：
//   1. 远程目录直接拒绝（router 不支持远程写）
//   2. QInputDialog 取名，trim 后校验非法字符 (/ \ : * ? " < > |)
//   3. 已存在则自动追加 " (2)" / " (3)" …，文件保留扩展名
//   4. 实际创建（QFile::open / QDir::mkdir），失败弹 warning
//   5. 成功后 refresh() active panel + 状态栏 2s 提示
//
// 这里没有走 FileSystemRouter——目标本来就是本地路径（远程已被前置 return），
// 直接用 Qt 自己的 API 最直观；也避免给 router 增加不必要的接口。
// ---------------------------------------------------------------------------

namespace {
// 文件名非法字符（含 Windows + Unix 常见保留符）。前导 / 末尾空白由 trim 处理。
bool hasInvalidNameChars(const QString& name) {
    static const QRegularExpression kBad(R"([\\/:\*\?"<>\|])");
    return kBad.match(name).hasMatch();
}

// 解析 "foo.tar.gz" → ("foo", ".tar.gz")？不，QFileInfo 的 completeSuffix 太贪婪。
// 这里采用 Finder 风格：只剥离最后一个 "." 起的扩展名（"foo.tar.gz" → "foo.tar" + ".gz"）。
// 没有点的名字（包括纯隐藏文件如 ".env"）整体当作 base、扩展名为空。
QPair<QString, QString> splitBaseAndExt(const QString& fileName) {
    const int dot = fileName.lastIndexOf('.');
    if (dot <= 0 || dot == fileName.size() - 1) {
        // 没扩展名 / 以点开头（隐藏文件）/ 以点结尾——都不剥
        return { fileName, QString() };
    }
    return { fileName.left(dot), fileName.mid(dot) };
}

// 在 dir 下为 desired 找一个不冲突的名字；冲突则追加 " (2)" / " (3)" …
// isFile=true 时保留扩展名（"foo.txt" → "foo (2).txt"），否则整体追加。
QString uniqueName(const QString& dir, const QString& desired, bool isFile) {
    QDir d(dir);
    if (!QFileInfo::exists(d.filePath(desired))) return desired;

    QString base, ext;
    if (isFile) {
        auto pair = splitBaseAndExt(desired);
        base = pair.first;
        ext  = pair.second;
    } else {
        base = desired;
    }

    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QString("%1 (%2)%3").arg(base).arg(i).arg(ext);
        if (!QFileInfo::exists(d.filePath(candidate))) return candidate;
    }
    // 极端兜底：返回原名让创建步骤自己失败，避免死循环
    return desired;
}
} // namespace

void MainWindow::onCreateNewFile(const QString& dir, bool isRemote) {
    if (isRemote) {
        QMessageBox::information(this, T("Not Supported"),
            T("Creating files/folders on remote paths is not supported yet."));
        return;
    }

    bool ok = false;
    QString name = QInputDialog::getText(this,
        T("New File"), T("Name:"), QLineEdit::Normal,
        "untitled.txt", &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    if (hasInvalidNameChars(name)) {
        QMessageBox::warning(this, T("Invalid Name"),
            T("Name contains invalid characters."));
        return;
    }

    const QString finalName = uniqueName(dir, name, /*isFile=*/true);
    const QString full = QDir(dir).filePath(finalName);

    QFile f(full);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, T("Create Failed"),
            QString("Cannot create file: %1").arg(f.errorString()));
        return;
    }
    f.close();

    activePanel()->refresh();
    statusBar()->showMessage(
        QString(I18n::instance().current() == I18n::Lang::Zh
                ? "已创建：%1" : "Created: %1").arg(finalName), 2000);
}

void MainWindow::onCreateNewFolder(const QString& dir, bool isRemote) {
    if (isRemote) {
        QMessageBox::information(this, T("Not Supported"),
            T("Creating files/folders on remote paths is not supported yet."));
        return;
    }

    bool ok = false;
    QString name = QInputDialog::getText(this,
        T("New Folder"), T("Name:"), QLineEdit::Normal,
        "untitled folder", &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    if (hasInvalidNameChars(name)) {
        QMessageBox::warning(this, T("Invalid Name"),
            T("Name contains invalid characters."));
        return;
    }

    const QString finalName = uniqueName(dir, name, /*isFile=*/false);
    const QString full = QDir(dir).filePath(finalName);

    if (!QDir().mkdir(full)) {
        QMessageBox::warning(this, T("Create Failed"), T("Cannot create folder."));
        return;
    }

    activePanel()->refresh();
    statusBar()->showMessage(
        QString(I18n::instance().current() == I18n::Lang::Zh
                ? "已创建：%1" : "Created: %1").arg(finalName), 2000);
}

// 重命名：把 oldPath 改成用户输入的名字（同目录内）。
// - 远程（sftp:// 等）路径暂不支持
// - 非法字符检查复用 hasInvalidNameChars
// - 与已存在名冲突：提示覆盖 / 取消（只对文件提示覆盖；目录冲突直接拒绝避免误合并）
void MainWindow::onRenameEntry(const QString& oldPath) {
    if (oldPath.isEmpty()) return;

    if (FileSystemRouter::instance().isRemote(oldPath)) {
        QMessageBox::information(this, T("Not Supported"),
            T("Renaming on remote paths is not supported yet."));
        return;
    }

    QFileInfo fi(oldPath);
    if (!fi.exists()) {
        QMessageBox::warning(this, T("Rename Failed"), T("The item no longer exists."));
        activePanel()->refresh();
        return;
    }

    const QString oldName = fi.fileName();
    const QString dir = fi.absolutePath();
    const bool isDir = fi.isDir();

    bool ok = false;
    QString newName = QInputDialog::getText(this,
        isDir ? T("Rename Folder") : T("Rename File"),
        T("New name:"), QLineEdit::Normal,
        oldName, &ok).trimmed();
    if (!ok || newName.isEmpty()) return;
    if (newName == oldName) return;  // 无改动

    if (hasInvalidNameChars(newName)) {
        QMessageBox::warning(this, T("Invalid Name"),
            T("Name contains invalid characters."));
        return;
    }

    const QString newPath = QDir(dir).filePath(newName);
    const bool zh = (I18n::instance().current() == I18n::Lang::Zh);

    // 冲突处理
    if (QFileInfo::exists(newPath)) {
        if (isDir || QFileInfo(newPath).isDir()) {
            QMessageBox::warning(this, T("Rename Failed"),
                QString(zh ? "名为 \"%1\" 的文件或文件夹已存在。"
                           : "A folder or file named \"%1\" already exists.").arg(newName));
            return;
        }
        auto ret = QMessageBox::question(this, T("Overwrite?"),
            QString(zh ? "名为 \"%1\" 的文件已存在。\n是否覆盖？"
                       : "A file named \"%1\" already exists.\nOverwrite it?").arg(newName),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
        if (!QFile::remove(newPath)) {
            QMessageBox::warning(this, T("Rename Failed"),
                zh ? "无法删除已存在的文件以覆盖。"
                   : "Cannot remove the existing file to overwrite.");
            return;
        }
    }

    // 执行 rename：QFile::rename 对文件/目录都适用（调用 POSIX rename）
    if (!QFile::rename(oldPath, newPath)) {
        QMessageBox::warning(this, T("Rename Failed"),
            QString(zh ? "无法将 \"%1\" 重命名为 \"%2\"。"
                       : "Cannot rename \"%1\" to \"%2\".").arg(oldName, newName));
        return;
    }

    activePanel()->refresh();
    statusBar()->showMessage(
        QString(zh ? "已重命名：%1 → %2" : "Renamed: %1 → %2").arg(oldName, newName), 2000);
}

// 重新计算状态栏四个 label 的内容（条目数 / 选中数 / 剪贴板 / 活跃面板）
void MainWindow::updateStatusBar() {
    // Count folders and files in active panel
    auto* fl = activePanel()->currentFileList();
    if (!fl) return;

    const auto& entries = fl->entries();
    int folderCount = 0, fileCount = 0;
    qint64 totalSize = 0;
    for (const auto& e : entries) {
        if (e.name == "." || e.name == "..") continue;
        if (e.isDirectory) folderCount++;
        else {
            fileCount++;
            totalSize += e.size;
        }
    }

    QString sizeStr;
    if (totalSize < 1024) sizeStr = QString::number(totalSize) + " B";
    else if (totalSize < 1024 * 1024) sizeStr = QString::number(totalSize / 1024.0, 'f', 1) + " KB";
    else if (totalSize < 1024LL * 1024 * 1024) sizeStr = QString::number(totalSize / (1024.0 * 1024.0), 'f', 1) + " MB";
    else sizeStr = QString::number(totalSize / (1024.0 * 1024.0 * 1024.0), 'f', 1) + " GB";

    m_statusLeft->setText(QString("%1 %2, %3 %4 (%5)")
        .arg(folderCount).arg(T("folders"))
        .arg(fileCount).arg(T("files"))
        .arg(sizeStr));

    // Selection info
    auto selected = activePanel()->getSelectedEntries();
    if (!selected.isEmpty()) {
        qint64 selSize = 0;
        for (const auto& e : selected) selSize += e.size;
        QString selSizeStr;
        if (selSize < 1024) selSizeStr = QString::number(selSize) + " B";
        else if (selSize < 1024 * 1024) selSizeStr = QString::number(selSize / 1024.0, 'f', 1) + " KB";
        else selSizeStr = QString::number(selSize / (1024.0 * 1024.0), 'f', 1) + " MB";
        m_statusRight->setText(QString("%1 %2 %3, %4")
            .arg(T("Selected:")).arg(selected.size()).arg(T("item(s)")).arg(selSizeStr));
    } else {
        m_statusRight->setText("");
    }

    // Clipboard info
    if (m_clipboard.has_value()) {
        QString action = m_clipboard->isCut ? T("Cut") : T("Copy");
        m_statusClipboard->setText(QString("%1 %2 %3 %4")
            .arg(T("Clipboard:")).arg(action).arg(m_clipboard->entries.size()).arg(T("item(s)")));
    } else {
        m_statusClipboard->setText("");
    }

    // Active panel
    QString panelStr = QString("%1 %2")
        .arg(T("Panel:"))
        .arg(m_activePane == PanelSide::Left ? T("Left") : T("Right"));
    if (m_isRemoteConnected) panelStr += " [Remote]";
    m_statusPanel->setText(panelStr);
}

// 暗色主题：Nord 配色 (#2E3440 系列)。整片 stylesheet 注入到 QApplication，
// 所有子控件都会继承。修改时记得对照 applyLightTheme 保持对偶。
void MainWindow::applyDarkTheme() {
    // ===== Nord Theme palette =====
    // Polar Night:  #2E3440 / #3B4252 / #434C5E / #4C566A
    // Snow Storm:   #D8DEE9 / #E5E9F0 / #ECEFF4
    // Frost:        #8FBCBB / #88C0D0 / #81A1C1 / #5E81AC
    // Aurora:       #BF616A / #D08770 / #EBCB8B / #A3BE8C / #B48EAD
    QString qss = R"(
        QMainWindow, QWidget {
            background-color: #2E3440;
            color: #D8DEE9;
        }
        QSplitter::handle {
            background-color: #4C566A;
            width: 2px;
        }
        QSplitter::handle:hover {
            background-color: #88C0D0;
        }
        QToolBar {
            background: #2E3440;
            border: 0px;
            border-bottom: 1px solid #262B35;
            spacing: 0px;
            padding: 4px;
        }
        /* separator / handle 与工具栏同色（Nord Polar Night 0 #2E3440），
           避免默认深色条在纯色底上突兀 */
        /* 按钮之间的 separator：彻底干掉 —— 零宽零高零边距零边框，
           background 用 transparent 防止某些 Qt 版本仍渲染 1px 颜色条 */
        QToolBar::separator {
            background: transparent;
            width: 0px;
            height: 0px;
            margin: 0px;
            padding: 0px;
            border: 0px;
        }
        QToolBar::handle {
            background: #2E3440;
            border: 0px;
            width: 0px;
            margin: 0px;
            padding: 0px;
        }
        /* 按钮底色：与目录树背景 #2E3440 完全同色，按钮和工具栏融为一体；
           所有状态显式 border: 0px / outline: 0px（含四向独立声明，防止 cascade 复活），
           按钮矩形整块由实色填满，图标贴在按钮底色上，杜绝任何边框/抗锯齿混色
           露出底层 toolbar 色，更杜绝按钮之间出现 1px 分隔线。 */
        QToolBar QToolButton {
            background: #2E3440;
            border: 0px solid transparent;
            border-left: 0px;
            border-right: 0px;
            border-top: 0px;
            border-bottom: 0px;
            border-radius: 0px;
            outline: 0px;
            margin: 0px;
            padding: 6px 12px;
            color: #E5E9F0;
        }
        QToolBar QToolButton:focus {
            outline: 0px;
            border: 0px;
            border-left: 0px;
            border-right: 0px;
            border-top: 0px;
            border-bottom: 0px;
            background: #2E3440;
        }
        QToolBar QToolButton:hover {
            background: #3B5168;
            border: 0px;
            border-left: 0px;
            border-right: 0px;
            border-top: 0px;
            border-bottom: 0px;
            outline: 0px;
            color: #88C0D0;
        }
        QToolBar QToolButton:pressed {
            background: #46627E;
            border: 0px;
            border-left: 0px;
            border-right: 0px;
            border-top: 0px;
            border-bottom: 0px;
            outline: 0px;
        }
        QToolBar QToolButton:checked {
            background: #3D5959;
            border: 0px;
            border-left: 0px;
            border-right: 0px;
            border-top: 0px;
            border-bottom: 0px;
            outline: 0px;
            color: #8FBCBB;
        }
        /* 工具栏溢出按钮（"放不下的按钮"展开入口）：
           显式给一个清晰的 ">>" 标识，让用户在窄窗口/大字号下也能找到。 */
        QToolBar QToolButton#qt_toolbar_ext_button {
            qproperty-text: ">>";
            background: #2E3440;
            border: 0px;
            outline: 0px;
            min-width: 40px;
            min-height: 26px;
            padding: 2px 8px;
            font-weight: bold;
            color: #88C0D0;
        }
        QToolBar QToolButton#qt_toolbar_ext_button:hover {
            background: #3B5168;
            border: 0px;
            outline: 0px;
            color: #ECEFF4;
        }
        QTabWidget::pane {
            border: 1px solid #434C5E;
            background-color: #2E3440;
            top: -1px;
        }
        QTabWidget::tab-bar {
            alignment: left;
            left: 0;
        }
        QTabBar {
            alignment: left;
            qproperty-drawBase: 0;
        }
        QTabBar::tab {
            background-color: #3B4252;
            color: #81A1C1;
            padding: 6px 14px;
            border: 1px solid #434C5E;
            border-bottom: none;
            border-top-left-radius: 4px;
            border-top-right-radius: 4px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background-color: #434C5E;
            color: #88C0D0;
            border-bottom: 2px solid #88C0D0;
            font-weight: bold;
        }
        QTabBar::tab:hover:!selected {
            background-color: #434C5E;
            color: #E5E9F0;
        }
        QTreeWidget, QTableWidget {
            background-color: #2E3440;
            color: #D8DEE9;
            border: 1px solid #434C5E;
            alternate-background-color: #353B48;
            gridline-color: #3B4252;
            selection-background-color: rgba(136, 192, 208, 0.25);
            outline: 0;
        }
        QTreeWidget::item, QTableWidget::item {
            padding: 3px 4px;
            border: none;
        }
        QTreeWidget::item:hover, QTableWidget::item:hover {
            background-color: rgba(136, 192, 208, 0.10);
        }
        QTreeWidget::item:selected, QTableWidget::item:selected {
            background-color: rgba(136, 192, 208, 0.28);
            color: #ECEFF4;
        }
        QTreeWidget::branch:has-siblings:!adjoins-item,
        QTreeWidget::branch:has-siblings:adjoins-item,
        QTreeWidget::branch:!has-children:!has-siblings:adjoins-item {
            border-image: none;
        }
        QHeaderView::section {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #434C5E, stop:1 #3B4252);
            color: #88C0D0;
            border: none;
            border-right: 1px solid #4C566A;
            border-bottom: 1px solid #4C566A;
            padding: 5px 8px;
            font-weight: bold;
        }
        QStatusBar {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3B4252, stop:1 #2E3440);
            color: #88C0D0;
            border-top: 1px solid #5E81AC;
            padding: 2px;
        }
        QStatusBar QLabel {
            color: #D8DEE9;
            padding: 0 8px;
        }
        QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QComboBox {
            background-color: #3B4252;
            color: #ECEFF4;
            border: 1px solid #434C5E;
            border-radius: 4px;
            padding: 4px 8px;
            selection-background-color: #5E81AC;
            selection-color: #ECEFF4;
        }
        QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {
            border-color: #88C0D0;
            background-color: #434C5E;
        }
        QComboBox::drop-down {
            border: none;
            width: 20px;
        }
        QComboBox QAbstractItemView {
            background: #3B4252;
            color: #E5E9F0;
            selection-background-color: #5E81AC;
            border: 1px solid #4C566A;
        }
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #434C5E, stop:1 #3B4252);
            color: #E5E9F0;
            border: 1px solid #4C566A;
            border-radius: 4px;
            padding: 6px 16px;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #4C566A, stop:1 #434C5E);
            border-color: #88C0D0;
            color: #88C0D0;
        }
        QPushButton:pressed {
            background: #5E81AC;
            color: #ECEFF4;
        }
        QPushButton:default {
            border: 2px solid #88C0D0;
        }
        QPushButton:disabled {
            background: #3B4252;
            color: #4C566A;
            border-color: #3B4252;
        }
        QGroupBox {
            color: #88C0D0;
            border: 1px solid #434C5E;
            border-radius: 4px;
            margin-top: 8px;
            padding-top: 16px;
            font-weight: bold;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 4px;
            color: #88C0D0;
        }
        QMenuBar {
            background-color: #3B4252;
            color: #E5E9F0;
            border-bottom: 1px solid #4C566A;
        }
        QMenuBar::item:selected {
            background: #434C5E;
            color: #88C0D0;
        }
        QMenu {
            background-color: #3B4252;
            color: #E5E9F0;
            border: 1px solid #4C566A;
            border-radius: 4px;
            padding: 4px;
        }
        QMenu::item {
            padding: 6px 24px;
            border-radius: 3px;
        }
        QMenu::item:selected {
            background: #5E81AC;
            color: #ECEFF4;
        }
        QMenu::separator {
            height: 1px;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 transparent, stop:0.5 #4C566A, stop:1 transparent);
            margin: 4px 8px;
        }
        QScrollBar:vertical {
            background: #2E3440;
            width: 10px;
            border: none;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: #434C5E;
            border-radius: 5px;
            min-height: 20px;
        }
        QScrollBar::handle:vertical:hover {
            background: #5E81AC;
        }
        QScrollBar::handle:vertical:pressed {
            background: #88C0D0;
        }
        QScrollBar:horizontal {
            background: #2E3440;
            height: 10px;
            border: none;
            margin: 0;
        }
        QScrollBar::handle:horizontal {
            background: #434C5E;
            border-radius: 5px;
            min-width: 20px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #5E81AC;
        }
        QScrollBar::handle:horizontal:pressed {
            background: #88C0D0;
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            height: 0px;
            width: 0px;
        }
        QScrollBar::add-page, QScrollBar::sub-page {
            background: transparent;
        }
        QListWidget {
            background-color: #2E3440;
            color: #D8DEE9;
            border: 1px solid #434C5E;
            outline: 0;
        }
        QListWidget::item:hover {
            background-color: rgba(136, 192, 208, 0.10);
        }
        QListWidget::item:selected {
            background-color: rgba(136, 192, 208, 0.28);
            color: #ECEFF4;
        }
        QDialog {
            background-color: #2E3440;
            color: #D8DEE9;
        }
        QLabel {
            color: #D8DEE9;
            background: transparent;
        }
        QCheckBox, QRadioButton {
            color: #D8DEE9;
            spacing: 6px;
        }
        QCheckBox::indicator, QRadioButton::indicator {
            width: 14px;
            height: 14px;
        }
        QCheckBox::indicator:unchecked {
            background: #3B4252;
            border: 1px solid #4C566A;
            border-radius: 3px;
        }
        QCheckBox::indicator:checked {
            background: #88C0D0;
            border: 1px solid #88C0D0;
            border-radius: 3px;
        }
        QToolTip {
            background-color: #3B4252;
            color: #ECEFF4;
            border: 1px solid #88C0D0;
            padding: 4px 6px;
        }
        QProgressBar {
            background-color: #3B4252;
            color: #ECEFF4;
            border: 1px solid #4C566A;
            border-radius: 4px;
            text-align: center;
            padding: 1px;
        }
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #5E81AC, stop:1 #88C0D0);
            border-radius: 3px;
        }
        QProgressDialog {
            background-color: #2E3440;
        }
        QProgressDialog QLabel {
            color: #E5E9F0;
            padding: 4px 0;
        }
    )";
    qApp->setStyleSheet(qss);
}

// 亮色主题：与 applyDarkTheme 对偶
void MainWindow::applyLightTheme() {
    QString qss = R"(
        QMainWindow, QWidget {
            background-color: #f5f5f5;
            color: #333333;
        }
        QSplitter::handle {
            background-color: #1976d2;
            width: 2px;
        }
        QToolBar {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffffff, stop:1 #f0f4f8);
            border-bottom: 2px solid #1976d2;
            spacing: 4px;
            padding: 4px;
        }
        QToolBar QToolButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 4px 8px;
            color: #333333;
        }
        QToolBar QToolButton:hover {
            background: rgba(25, 118, 210, 0.1);
            border-color: #1976d2;
            color: #1976d2;
        }
        QToolBar QToolButton:pressed {
            background: rgba(25, 118, 210, 0.2);
        }
        /* 工具栏溢出按钮（"放不下的按钮"展开入口） */
        QToolBar QToolButton#qt_toolbar_ext_button {
            qproperty-text: ">>";
            min-width: 40px;
            min-height: 26px;
            padding: 2px 8px;
            font-weight: bold;
            color: #1976d2;
        }
        QToolBar QToolButton#qt_toolbar_ext_button:hover {
            background: rgba(25, 118, 210, 0.15);
        }
        QTabWidget::pane {
            border: 1px solid #e0e0e0;
            background-color: #ffffff;
        }
        QTabBar::tab {
            background-color: #f5f5f5;
            color: #666666;
            padding: 6px 12px;
            border: 1px solid #e0e0e0;
            border-bottom: none;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background-color: #ffffff;
            color: #1976d2;
            border-bottom: 2px solid #1976d2;
            font-weight: bold;
        }
        QTabBar::tab:hover {
            background-color: rgba(25, 118, 210, 0.05);
            color: #333333;
        }
        QTreeWidget, QTableWidget {
            background-color: #ffffff;
            color: #333333;
            border: 1px solid #e0e0e0;
            alternate-background-color: #fafafa;
            gridline-color: #e0e0e0;
        }
        QTreeWidget::item:hover, QTableWidget::item:hover {
            background-color: rgba(25, 118, 210, 0.05);
        }
        QTreeWidget::item:selected, QTableWidget::item:selected {
            background-color: rgba(25, 118, 210, 0.15);
            border-left: 3px solid #1976d2;
            padding-left: 4px;
            color: #1976d2;
        }
        QHeaderView::section {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffffff, stop:1 #f0f4f8);
            color: #1976d2;
            border: 1px solid #e0e0e0;
            padding: 4px 8px;
            font-weight: bold;
        }
        QStatusBar {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ffffff, stop:1 #f0f4f8);
            color: #1976d2;
            border-top: 2px solid #1976d2;
            padding: 2px;
        }
        QStatusBar QLabel {
            color: #555555;
            padding: 0 8px;
        }
        QLineEdit, QTextEdit, QSpinBox, QComboBox {
            background-color: #ffffff;
            color: #333333;
            border: 1px solid #e0e0e0;
            border-radius: 4px;
            padding: 4px 8px;
        }
        QLineEdit:focus, QTextEdit:focus {
            border-color: #1976d2;
            background-color: #f8fbff;
        }
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffffff, stop:1 #f5f5f5);
            color: #333333;
            border: 1px solid #e0e0e0;
            border-radius: 4px;
            padding: 6px 16px;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #f0f4f8, stop:1 #e3f2fd);
            border-color: #1976d2;
            color: #1976d2;
        }
        QPushButton:pressed {
            background-color: rgba(25, 118, 210, 0.2);
        }
        QPushButton:default {
            border: 2px solid #1976d2;
        }
        QGroupBox {
            color: #1976d2;
            border: 1px solid #e0e0e0;
            border-radius: 4px;
            margin-top: 8px;
            padding-top: 16px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 4px;
            color: #1976d2;
        }
        QMenu {
            background-color: #ffffff;
            color: #333333;
            border: 1px solid #1976d2;
            border-radius: 4px;
        }
        QMenu::item {
            padding: 6px 24px;
        }
        QMenu::item:selected {
            background-color: rgba(25, 118, 210, 0.15);
            color: #1976d2;
        }
        QMenu::separator {
            height: 1px;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 transparent, stop:0.5 #1976d2, stop:1 transparent);
            margin: 4px 8px;
        }
        QScrollBar:vertical {
            background: #f5f5f5;
            width: 10px;
            border: none;
        }
        QScrollBar::handle:vertical {
            background: #cccccc;
            border-radius: 5px;
            min-height: 20px;
        }
        QScrollBar::handle:vertical:hover {
            background: #1976d2;
        }
        QScrollBar:horizontal {
            background: #f5f5f5;
            height: 10px;
            border: none;
        }
        QScrollBar::handle:horizontal {
            background: #cccccc;
            border-radius: 5px;
            min-width: 20px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #1976d2;
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            height: 0px;
            width: 0px;
        }
        QListWidget {
            background-color: #ffffff;
            color: #333333;
            border: 1px solid #e0e0e0;
        }
        QListWidget::item:hover {
            background-color: rgba(25, 118, 210, 0.05);
        }
        QListWidget::item:selected {
            background-color: rgba(25, 118, 210, 0.15);
            color: #1976d2;
        }
        QDialog {
            background-color: #f5f5f5;
        }
        QLabel {
            color: #333333;
        }
    )";
    qApp->setStyleSheet(qss);
}
