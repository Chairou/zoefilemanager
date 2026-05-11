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

PathBar::PathBar(QWidget* parent)
    : QWidget(parent)
{
    // Overall bar height: just tall enough for one line of text.
    setFixedHeight(22);

    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->setSpacing(0);

    // --- Display page --------------------------------------------------
    m_displayPage = new QWidget();
    m_layout = new QHBoxLayout(m_displayPage);
    m_layout->setContentsMargins(4, 0, 4, 0);
    m_layout->setSpacing(2);
    m_layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Double-click on empty area of the display row enters edit mode
    m_displayPage->installEventFilter(this);

    // 面包屑容器（本地路径用）
    m_crumbHost = new QWidget();
    m_crumbLayout = new QHBoxLayout(m_crumbHost);
    m_crumbLayout->setContentsMargins(0, 0, 0, 0);
    m_crumbLayout->setSpacing(0);
    m_crumbLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_crumbHost->installEventFilter(this);

    // 远程 URL fallback label（点击复制）
    m_pathLabel = new QPushButton("");
    m_pathLabel->setFlat(true);
    m_pathLabel->setCursor(Qt::PointingHandCursor);
    m_pathLabel->setFocusPolicy(Qt::NoFocus);
    m_pathLabel->setMaximumHeight(20);
    m_pathLabel->setMinimumHeight(20);
    m_pathLabel->setToolTip(T("Click to copy current path"));
    m_pathLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_pathLabel->setStyleSheet(
        "QPushButton {"
        "  padding: 0 6px;"
        "  margin: 0;"
        "  min-height: 20px;"
        "  max-height: 20px;"
        "  border: 1px solid transparent;"
        "  border-radius: 3px;"
        "  background: transparent;"
        "  color: #D8DEE9;"
        "  text-align: left;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(255,255,255,0.08);"
        "  border-color: rgba(136,192,208,0.35);"
        "}"
        "QPushButton:pressed { background: rgba(255,255,255,0.14); }"
        "QPushButton:focus { outline: none; }");
    connect(m_pathLabel, &QPushButton::clicked, this, [this]() {
        if (m_path.isEmpty()) return;
        QApplication::clipboard()->setText(m_path);
        QPoint pos = m_pathLabel->mapToGlobal(
            QPoint(m_pathLabel->width() / 2, m_pathLabel->height()));
        showCopiedToast(pos);
    });

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

    // 面包屑容器占据 1 份伸展空间；远程 URL 路径 label 也占 1 份（同时只显示其一）
    m_layout->addWidget(m_crumbHost, 1);
    m_layout->addWidget(m_pathLabel, 1);
    m_layout->addWidget(m_editBtn, 0, Qt::AlignVCenter);

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
    if ((obj == m_displayPage || obj == m_crumbHost)
        && event->type() == QEvent::MouseButtonDblClick) {
        enterEditMode();
        return true;
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
    // 单一数据源：把 m_path 映射为面包屑（本地）或整条 label（远程 URL）。
    // 绝不在此方法内修改 m_path。所有跳转通过 emit pathSelected。
    if (!m_crumbHost || !m_pathLabel) return;

    clearCrumbs();

    // 远程 URL 特判：sftp://... 原样整条只读显示，不做拆分。
    const bool isRemote = m_path.contains("://");
    if (isRemote) {
        m_crumbHost->hide();
        m_pathLabel->show();
        m_pathLabel->setText(m_path);
        return;
    }
    m_pathLabel->hide();
    m_crumbHost->show();

    // 本地路径：解析为 ["/", "a", "b", "c"]
    // 例：m_path = "/Users/chairou/code" → segs = ["Users","chairou","code"]
    QStringList segs = m_path.split('/', Qt::SkipEmptyParts);

    // 工具函数：创建一个面包屑按钮
    auto makeCrumb = [this](const QString& text, const QString& absPath) -> QToolButton* {
        auto* btn = new QToolButton(m_crumbHost);
        btn->setObjectName("crumbBtn");
        btn->setText(text);
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        btn->setProperty("absPath", absPath);
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
        connect(btn, &QToolButton::clicked, this, [this, absPath]() {
            // SSOT：不自改 m_path，请求 FilePanel::navigateTo
            emit pathSelected(absPath);
        });
        return btn;
    };

    // 工具函数：创建一个 "/" 分隔符
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

    // 右侧留白伸展（让面包屑靠左紧排，剩余空间交给 ✎ 按钮之前的空白）
    m_crumbLayout->addStretch(1);
}

void PathBar::showCopiedToast(const QPoint& globalPos) {
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
