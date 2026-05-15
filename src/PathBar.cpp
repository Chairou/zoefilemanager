// =============================================================================
// PathBar.cpp —— 见 PathBar.h
//
// 2026-05 第二轮重构：在保持 SSOT 铁律的前提下，恢复**可点击面包屑**：
//   - 本地路径 "/a/b/c" 拆分为 [根"/", "a", "b", "c"] 四段按钮 + 三个 "/" 分隔符
//   - 点击段按钮 → emit pathSelected(absPrefix) → FilePanel::navigateTo 跳转
//   - 远程 URL (sftp://user@host:22/...) 保持整条只读展示（避免误拆 "://"）
//   - 整条路径复制：双击空白区域进入编辑模式；或点 ✎；或 Ctrl+L
// =============================================================================

#include "PathBar.h"
#include "FileSystemRouter.h"
#include "I18n.h"
#include <QLabel>
#include <QToolButton>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QLineEdit>
#include <QStackedLayout>
#include <QKeyEvent>
#include <QFileInfo>
#include <QDir>
#include <QStringList>
#include <QMouseEvent>
#include <QByteArray>
#include <QScrollArea>
#include <QScrollBar>

namespace {
// 远程 URL 反解：把 percent-encoded 中文还原成可读字符串用于"显示"。
// 注意：返回值仅用于显示/tooltip，绝不能写回 m_path（会破坏 router 路由）。
QString humanizeRemoteUrl(const QString& url) {
    const int sep = url.indexOf("://");
    if (sep < 0) return url;
    const QString scheme = url.left(sep + 3);          // "smb://" / "sftp://"
    const QString rest = url.mid(sep + 3);
    // QByteArray::fromPercentEncoding 默认按 UTF-8 解码，正合中文需要
    const QByteArray decoded = QByteArray::fromPercentEncoding(rest.toUtf8());
    return scheme + QString::fromUtf8(decoded);
}

// 反解单个路径段：`%E6%96%87...` → `文体协会`。
// 输入保持原样（unchanged）直接作为 absPath 参与跳转；返回值仅用于按钮显示文字。
QString humanizeSegment(const QString& seg) {
    if (!seg.contains('%')) return seg;
    const QByteArray decoded = QByteArray::fromPercentEncoding(seg.toUtf8());
    const QString asUtf8 = QString::fromUtf8(decoded);
    return asUtf8.isEmpty() ? seg : asUtf8;
}
} // namespace

PathBar::PathBar(QWidget* parent)
    : QWidget(parent)
{
    // 路径栏高度：面包屑 ScrollArea 单行 22px + 横向滚动条 6px + 余量 = 30px
    setMinimumHeight(30);
    setMaximumHeight(32);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->setSpacing(0);

    // --- Display page --------------------------------------------------
    m_displayPage = new QWidget();
    m_layout = new QHBoxLayout(m_displayPage);
    m_layout->setContentsMargins(4, 0, 4, 0);
    m_layout->setSpacing(2);
    m_layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // Double-click on empty area of the display row enters edit mode
    m_displayPage->installEventFilter(this);

    // 面包屑容器（本地 & 远程统一使用）
    m_crumbHost = new QWidget();
    m_crumbLayout = new QHBoxLayout(m_crumbHost);
    m_crumbLayout->setContentsMargins(0, 0, 0, 0);
    m_crumbLayout->setSpacing(0);
    m_crumbLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_crumbHost->installEventFilter(this);

    // 把面包屑容器放进一个水平 ScrollArea，超长路径横向滚动，不撑大窗口。
    // 外观 frameless，竖向绝不出现滚动条。
    m_crumbScroll = new QScrollArea();
    m_crumbScroll->setObjectName("crumbScroll");
    m_crumbScroll->setFrameShape(QFrame::NoFrame);
    m_crumbScroll->setWidgetResizable(true);
    m_crumbScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_crumbScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_crumbScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_crumbScroll->setMinimumHeight(22);
    m_crumbScroll->setMaximumHeight(28);     // 单行 22px + 横向滚动条空间
    m_crumbScroll->setStyleSheet(
        "QScrollArea#crumbScroll { background: transparent; border: 0; }"
        "QScrollArea#crumbScroll > QWidget > QWidget { background: transparent; }"
        "QScrollBar:horizontal { height: 6px; background: transparent; }"
        "QScrollBar::handle:horizontal {"
        "  background: rgba(136,192,208,0.35);"
        "  min-width: 24px; border-radius: 3px; }"
        "QScrollBar::handle:horizontal:hover { background: rgba(136,192,208,0.60); }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        "  width: 0; background: transparent; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
        "  background: transparent; }");
    m_crumbScroll->setWidget(m_crumbHost);
    m_crumbScroll->viewport()->installEventFilter(this); // 双击空白进入编辑

    // 远程 URL fallback label（QLabel + wordWrap 支持折行；中文反解显示）
    // - 单击：复制原始（percent-encoded）m_path 到剪贴板
    // - 双击：进入编辑模式
    // 用 QLabel 而不是 QPushButton —— QPushButton 不原生支持 word-wrap。
    m_pathLabel = new QLabel("");
    m_pathLabel->setObjectName("pathLabelRemote");
    m_pathLabel->setWordWrap(true);
    m_pathLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_pathLabel->setCursor(Qt::PointingHandCursor);
    m_pathLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_pathLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_pathLabel->setStyleSheet(
        "QLabel#pathLabelRemote {"
        "  padding: 1px 6px;"
        "  margin: 0;"
        "  border: 1px solid transparent;"
        "  border-radius: 3px;"
        "  background: transparent;"
        "  color: #D8DEE9;"
        "}"
        "QLabel#pathLabelRemote:hover {"
        "  background: rgba(255,255,255,0.06);"
        "  border-color: rgba(136,192,208,0.30);"
        "}");
    m_pathLabel->installEventFilter(this);

    m_editBtn = new QPushButton(QString::fromUtf8("\xE2\x9C\x8E"));  // ✎
    m_editBtn->setToolTip(T("Edit path (double-click empty area, or Ctrl+L)"));
    m_editBtn->setFlat(true);
    m_editBtn->setCursor(Qt::PointingHandCursor);
    m_editBtn->setFocusPolicy(Qt::NoFocus);
    m_editBtn->setFixedSize(18, 18);
    m_editBtn->setStyleSheet(
        "QPushButton {"
        "  padding: 0;"
        "  margin: 0;"
        "  border: 1px solid transparent;"
        "  border-radius: 3px;"
        "  background: transparent;"
        "  color: #81A1C1;"
        "  font-size: 11px;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(136, 192, 208, 0.18);"
        "  color: #88C0D0;"
        "  border-color: #88C0D0;"
        "}"
        "QPushButton:pressed {"
        "  background: #5E81AC;"
        "  color: #ECEFF4;"
        "}");
    connect(m_editBtn, &QPushButton::clicked, this, &PathBar::enterEditMode);

    // 面包屑 ScrollArea 占据 1 份伸展空间。
    // 旧的 m_pathLabel（远程 URL wordWrap 长 Label）现在被面包屑替代，
    // 默认隐藏保留对象，避免别处潜在依赖；未来可以清理。
    m_layout->addWidget(m_crumbScroll, 1);
    m_layout->addWidget(m_pathLabel, 1);
    m_pathLabel->hide();
    // ✎ 按钮折行后顶端对齐，避免跑到中间
    m_layout->addWidget(m_editBtn, 0, Qt::AlignTop);

    // --- Edit page ----------------------------------------------------
    m_editPage = new QWidget();
    auto* editLayout = new QHBoxLayout(m_editPage);
    editLayout->setContentsMargins(4, 0, 4, 0);
    editLayout->setSpacing(2);
    m_pathEdit = new QLineEdit();
    m_pathEdit->setPlaceholderText(T("Enter path and press Enter (Esc to cancel)"));
    m_pathEdit->setFixedHeight(20);
    m_pathEdit->setStyleSheet(
        "QLineEdit { padding: 0 4px; margin: 0; border: 1px solid #434C5E;"
        "  border-radius: 3px; background: #3B4252; color: #ECEFF4; }");
    m_pathEdit->installEventFilter(this);
    editLayout->addWidget(m_pathEdit, 1);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, &PathBar::commitEditedPath);

    m_stack->addWidget(m_displayPage);
    m_stack->addWidget(m_editPage);
    m_stack->setCurrentWidget(m_displayPage);

    setPath("/");
}

void PathBar::setPath(const QString& path) {
    m_path = path;
    rebuildDisplay();
    // If we were in edit mode, keep edit mode but refresh the text;
    // otherwise the display label gets refreshed above.
    if (m_pathEdit) m_pathEdit->setText(path);
}

void PathBar::enterEditMode() {
    m_pathEdit->setText(m_path);
    m_pathEdit->selectAll();
    m_stack->setCurrentWidget(m_editPage);
    m_pathEdit->setFocus();
}

void PathBar::exitEditMode() {
    m_stack->setCurrentWidget(m_displayPage);
}

void PathBar::commitEditedPath() {
    QString target = m_pathEdit->text().trimmed();
    if (target.isEmpty()) { exitEditMode(); return; }

    // Expand leading ~ to home
    if (target.startsWith("~/")) {
        target = QDir::homePath() + target.mid(1);
    } else if (target == "~") {
        target = QDir::homePath();
    }

    // Validate through the router so remote URLs (sftp://...) work too.
    // QDir::cleanPath mangles double-slashes in URLs (collapses "sftp://"
    // to "sftp:/"), so only clean LOCAL paths.
    auto& router = FileSystemRouter::instance();
    const bool isRemote = router.isRemote(target) ||
                          target.startsWith("sftp://");
    if (!isRemote) {
        target = QDir::cleanPath(target);
    } else {
        target = router.normalizePath(target);
    }

    if (!router.exists(target) || !router.isDirectory(target)) {
        // Flash red border briefly to indicate invalid path, stay in edit mode
        m_pathEdit->setStyleSheet("QLineEdit { border: 1px solid #BF616A; }");
        QTimer::singleShot(1200, this, [this]() {
            if (m_pathEdit) m_pathEdit->setStyleSheet("");
        });
        return;
    }

    exitEditMode();
    emit pathSelected(target);
}

bool PathBar::eventFilter(QObject* obj, QEvent* event) {
    // Double-click on empty display area → enter edit mode
    const bool isScrollViewport =
        (m_crumbScroll && obj == m_crumbScroll->viewport());
    if ((obj == m_displayPage || obj == m_crumbHost || isScrollViewport)
        && event->type() == QEvent::MouseButtonDblClick) {
        enterEditMode();
        return true;
    }
    // 远程 URL label：单击复制原始 m_path（保留 percent-encoded，粘贴可路由）；
    //                  双击进入编辑模式
    if (obj == m_pathLabel) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            enterEditMode();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && !m_path.isEmpty()) {
                QApplication::clipboard()->setText(m_path);
                QPoint pos = m_pathLabel->mapToGlobal(
                    QPoint(m_pathLabel->width() / 2, m_pathLabel->height()));
                showCopiedToast(pos);
                return true;
            }
        }
    }
    // Esc in edit mode → cancel
    if (obj == m_pathEdit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            exitEditMode();
            return true;
        }
    }
    // Focus leaving the edit field (user clicked elsewhere) → exit edit mode
    // without committing. Same effect as Esc.
    if (obj == m_pathEdit && event->type() == QEvent::FocusOut) {
        if (m_stack && m_stack->currentWidget() == m_editPage) {
            exitEditMode();
        }
    }
    return QWidget::eventFilter(obj, event);
}

void PathBar::clearCrumbs() {
    if (!m_crumbLayout) return;
    // 删除 m_crumbHost 内所有子 widget（按钮 + 分隔符 label）
    while (QLayoutItem* item = m_crumbLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
}

void PathBar::rebuildDisplay() {
    // 单一数据源：把 m_path 映射为面包屑（本地 & 远程统一）。
    // 绝不在此方法内修改 m_path。所有跳转通过 emit pathSelected。
    if (!m_crumbHost) return;

    clearCrumbs();

    // 工具：创建一个面包屑按钮（display 文本；absPath 原样用于跳转）
    auto makeCrumb = [this](const QString& text, const QString& absPath,
                            const QString& tooltip = QString()) -> QToolButton* {
        auto* btn = new QToolButton(m_crumbHost);
        btn->setObjectName("crumbBtn");
        btn->setText(text);
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        btn->setProperty("absPath", absPath);
        if (!tooltip.isEmpty()) btn->setToolTip(tooltip);
        btn->setStyleSheet(
            "QToolButton#crumbBtn {"
            "  background: transparent;"
            "  border: 0px;"
            "  padding: 0 6px;"
            "  min-height: 20px;"
            "  max-height: 20px;"
            "  color: #ECEFF4;"
            "}"
            "QToolButton#crumbBtn:hover {"
            "  color: #88C0D0;"
            "  text-decoration: underline;"
            "}"
            "QToolButton#crumbBtn:pressed {"
            "  color: #5E81AC;"
            "}");
        connect(btn, &QToolButton::clicked, this, [this, btn, absPath]() {
            // 1) 复制路径到剪贴板（保留原始 percent-encoded 字面，
            //    粘贴回路径栏可直接被 router 路由）
            QApplication::clipboard()->setText(absPath);

            // 2) 在按钮下方弹出 toast，提示具体复制了什么路径
            //    远程 URL 用 humanizeRemoteUrl 反解 percent-encoded 中文做提示，
            //    本地路径直接显示。
            QString shown = absPath;
            if (absPath.contains("://")) {
                shown = humanizeRemoteUrl(absPath);
            }
            // 太长截断，避免 toast 撑得过宽
            if (shown.size() > 80) {
                shown = shown.left(38) + QString::fromUtf8("…")
                            + shown.right(38);
            }
            const QString msg = QString::fromUtf8("✓ ") + T("Copied: ") + shown;
            const QPoint anchor = btn->mapToGlobal(
                QPoint(btn->width() / 2, btn->height()));
            showCopiedToast(anchor, msg);

            // 3) SSOT：不自改 m_path，请求 FilePanel::navigateTo
            emit pathSelected(absPath);
        });
        return btn;
    };

    // 工具：分隔符
    auto makeSep = [this]() -> QLabel* {
        auto* sep = new QLabel("/", m_crumbHost);
        sep->setObjectName("crumbSep");
        sep->setStyleSheet(
            "QLabel#crumbSep {"
            "  color: #4C566A;"
            "  padding: 0 2px;"
            "  background: transparent;"
            "}");
        return sep;
    };

    const int schemeSep = m_path.indexOf("://");
    const bool isRemote = schemeSep >= 0;

    if (isRemote) {
        // 远程 URL 面包屑拆分示例：
        //   m_path = "smb://tencent.com/tfs/%E6%96%87%E4%BD%93%E5%8D%8F%E4%BC%9A/%E8%85%BE%E8%AE%AF..."
        //   段 0 (host root) : display="smb://tencent.com"  absPath="smb://tencent.com"
        //   段 1             : display="tfs"                absPath="smb://tencent.com/tfs"
        //   段 2             : display="文体协会"            absPath="smb://tencent.com/tfs/%E6%96%87%E4%BD%93%E5%8D%8F%E4%BC%9A"
        //   段 3             : display="腾讯电影协会-…"      absPath="smb://tencent.com/tfs/.../%E8%85%BE..."
        // 关键：每段 absPath **保留原始 percent-encoded 字面**，供 router 路由。
        const QString scheme = m_path.left(schemeSep + 3);    // "smb://"
        const QString afterScheme = m_path.mid(schemeSep + 3); // "tencent.com/tfs/%E6.../..."
        const int firstSlash = afterScheme.indexOf('/');
        const QString host = (firstSlash < 0) ? afterScheme
                                              : afterScheme.left(firstSlash);
        const QString afterHost = (firstSlash < 0) ? QString()
                                                   : afterScheme.mid(firstSlash + 1);

        // 第一个按钮：scheme + host（点击回到 host 根，列 share 列表）
        const QString hostAbs = scheme + host;
        m_crumbLayout->addWidget(makeCrumb(scheme + host, hostAbs, hostAbs));

        // 剩余段：按 '/' 拆，逐段累计 absPath
        const QStringList segs = afterHost.split('/', Qt::SkipEmptyParts);
        QString accum = hostAbs;   // 不带尾斜杠
        for (int i = 0; i < segs.size(); ++i) {
            accum += "/" + segs[i];               // 保留 percent-encoded 原样
            m_crumbLayout->addWidget(makeSep());
            const QString display = humanizeSegment(segs[i]);
            m_crumbLayout->addWidget(makeCrumb(display, accum, accum));
        }

        m_crumbLayout->addStretch(1);

        // 折行/滚动后自适应高度；同时自动滚到最右侧让用户看到当前目录
        if (m_crumbScroll) {
            QTimer::singleShot(0, this, [this]() {
                if (!m_crumbScroll) return;
                auto* sb = m_crumbScroll->horizontalScrollBar();
                if (sb) sb->setValue(sb->maximum());
            });
        }
        updateGeometry();
        return;
    }

    // 本地路径：解析为 ["/", "a", "b", "c"]
    // 例：m_path = "/Users/chairou/code" → segs = ["Users","chairou","code"]
    const QStringList segs = m_path.split('/', Qt::SkipEmptyParts);

    // 根 "/" 按钮（始终在最左，点击回到根）
    m_crumbLayout->addWidget(makeCrumb("/", "/"));

    // 若 m_path 非根，继续追加每一段
    QString accum;
    for (int i = 0; i < segs.size(); ++i) {
        accum += "/" + segs[i];
        // 段与段之间（包括根与第一段之间）插入分隔符
        // 但"根按钮文字本身是 /"，所以段前不再加分隔符以免双斜杠视觉重复
        if (i > 0) m_crumbLayout->addWidget(makeSep());
        m_crumbLayout->addWidget(makeCrumb(segs[i], accum));
    }

    // 右侧留白伸展（让面包屑靠左紧排）
    m_crumbLayout->addStretch(1);

    if (m_crumbScroll) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_crumbScroll) return;
            auto* sb = m_crumbScroll->horizontalScrollBar();
            if (sb) sb->setValue(sb->maximum());
        });
    }
    updateGeometry();
}

void PathBar::showCopiedToast(const QPoint& globalPos, const QString& message) {
    // Lazily create a reusable frameless popup widget
    if (!m_toast) {
        m_toast = new QWidget(nullptr,
            Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_toast->setAttribute(Qt::WA_ShowWithoutActivating);
        m_toast->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_toast->setStyleSheet(
            "QWidget {"
            "  background: rgba(59, 66, 82, 235);"       // Nord1
            "  border: 1px solid #88C0D0;"               // Nord8
            "  border-radius: 6px;"
            "}"
            "QLabel {"
            "  color: #ECEFF4;"                          // Nord6
            "  background: transparent;"
            "  border: none;"
            "  padding: 6px 12px;"
            "  font-weight: 500;"
            "}");

        auto* layout = new QHBoxLayout(m_toast);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(T("✓ Path copied"), m_toast);
        label->setObjectName("toastLabel");
        layout->addWidget(label);

        m_toastTimer = new QTimer(this);
        m_toastTimer->setSingleShot(true);
        m_toastTimer->setInterval(2000);
        connect(m_toastTimer, &QTimer::timeout, this, [this]() {
            if (m_toast) m_toast->hide();
        });
    }

    // 更新 toast 文字（默认 "✓ Path copied"，调用方可覆盖如 "✓ 已复制：/foo/bar"）
    if (auto* label = m_toast->findChild<QLabel*>("toastLabel")) {
        label->setText(message.isEmpty() ? T("✓ Path copied") : message);
    }
    m_toast->adjustSize();
    int x = globalPos.x() - m_toast->width() / 2;
    int y = globalPos.y() + 4;

    if (QScreen* screen = QGuiApplication::screenAt(globalPos)) {
        QRect geo = screen->availableGeometry();
        if (x < geo.left() + 4) x = geo.left() + 4;
        if (x + m_toast->width() > geo.right() - 4) x = geo.right() - 4 - m_toast->width();
        if (y + m_toast->height() > geo.bottom() - 4) {
            y = globalPos.y() - m_toast->height() - 24;
        }
    }

    m_toast->move(x, y);
    m_toast->show();
    m_toast->raise();

    m_toastTimer->start();
}
