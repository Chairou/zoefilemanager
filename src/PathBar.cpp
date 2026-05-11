// =============================================================================
// PathBar.cpp —— 见 PathBar.h
//
// 重点函数 rebuildButtons：决定面包屑如何拆分。对 SFTP URL 必须先把整个
// "sftp://user@host:22" 当作一个不可拆的"根"，否则按 '/' 一拆就乱。
// =============================================================================

#include "PathBar.h"
#include "FileSystemRouter.h"
#include <QLabel>
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

PathBar::PathBar(QWidget* parent)
    : QWidget(parent)
{
    // Overall bar height: just tall enough for one line of text.
    // 22px gives comfortable breathing room for most font sizes without
    // feeling chunky. QLineEdit/QPushButton honor this via their own minHeight.
    setFixedHeight(22);

    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->setSpacing(0);

    // --- Breadcrumb page ----------------------------------------------
    m_breadcrumbPage = new QWidget();
    m_layout = new QHBoxLayout(m_breadcrumbPage);
    m_layout->setContentsMargins(4, 0, 4, 0);
    m_layout->setSpacing(1);
    m_layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Double-click on empty area of the breadcrumb row enters edit mode
    m_breadcrumbPage->installEventFilter(this);

    // --- Edit page ----------------------------------------------------
    m_editPage = new QWidget();
    auto* editLayout = new QHBoxLayout(m_editPage);
    editLayout->setContentsMargins(4, 0, 4, 0);
    editLayout->setSpacing(2);
    m_pathEdit = new QLineEdit();
    m_pathEdit->setPlaceholderText("Enter path and press Enter (Esc to cancel)");
    m_pathEdit->setFixedHeight(20);
    m_pathEdit->setStyleSheet(
        "QLineEdit { padding: 0 4px; margin: 0; border: 1px solid #434C5E;"
        "  border-radius: 3px; background: #3B4252; color: #ECEFF4; }");
    m_pathEdit->installEventFilter(this);
    editLayout->addWidget(m_pathEdit, 1);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, &PathBar::commitEditedPath);

    m_stack->addWidget(m_breadcrumbPage);
    m_stack->addWidget(m_editPage);
    m_stack->setCurrentWidget(m_breadcrumbPage);

    setPath("/");
}

void PathBar::setPath(const QString& path) {
    m_path = path;
    rebuildButtons();
    // If we were in edit mode, stay in edit mode but refresh the text;
    // otherwise the breadcrumb gets rebuilt automatically above.
    if (m_pathEdit) m_pathEdit->setText(path);
}

void PathBar::enterEditMode() {
    m_pathEdit->setText(m_path);
    m_pathEdit->selectAll();
    m_stack->setCurrentWidget(m_editPage);
    m_pathEdit->setFocus();
}

void PathBar::exitEditMode() {
    m_stack->setCurrentWidget(m_breadcrumbPage);
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
    // Double-click on empty breadcrumb area → enter edit mode
    if (obj == m_breadcrumbPage && event->type() == QEvent::MouseButtonDblClick) {
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

void PathBar::rebuildButtons() {
    // Clear existing buttons
    for (auto* btn : m_buttons) {
        m_layout->removeWidget(btn);
        delete btn;
    }
    m_buttons.clear();
    m_editBtn = nullptr;

    // Remove any leftover non-widget items (spacers/stretches)
    while (QLayoutItem* item = m_layout->takeAt(0)) {
        if (!item->widget()) {
            delete item;
        } else {
            m_layout->addItem(item);
            break;
        }
    }

    const QString btnStyle =
        "QPushButton {"
        "  padding: 0 6px;"
        "  margin: 0;"
        "  min-height: 20px;"
        "  max-height: 20px;"
        "  border: 1px solid transparent;"
        "  border-radius: 3px;"
        "  background: transparent;"
        "  text-align: left;"
        "}"
        "QPushButton:hover { background: rgba(255,255,255,0.10); }"
        "QPushButton:pressed { background: rgba(255,255,255,0.18); }"
        "QPushButton:focus { outline: none; }";

    // ---- Figure out whether this path lives under a mounted prefix
    // (e.g. "sftp://demo@test.rebex.net:22") and split it accordingly.
    //
    //   localRoot   : the path clickable as the "root" breadcrumb
    //                 ("/" for local, "sftp://user@host:22/" for remote)
    //   rootLabel   : what to display on that root button
    //   relPath     : the part of m_path *inside* the root, e.g. "/pub/foo"
    //
    // This is the fix for a bug where remote URLs were split on every "/"
    // and each crumb emitted a non-routable path like "/sftp:/user@host:22/pub".
    QString mountPrefix = FileSystemRouter::instance().mountFor(m_path);
    QString localRoot, rootLabel, relPath;
    if (!mountPrefix.isEmpty()) {
        localRoot = mountPrefix + "/";
        rootLabel = mountPrefix + "/";           // shows "sftp://user@host:22/"
        relPath   = m_path.mid(mountPrefix.size());
        if (relPath.startsWith('/')) relPath = relPath.mid(1);
    } else {
        localRoot = "/";
        rootLabel = "/";
        relPath   = (m_path == "/") ? QString() : m_path.mid(1);  // drop leading '/'
    }

    // Root button
    auto* rootBtn = new QPushButton(rootLabel);
    rootBtn->setFlat(true);
    rootBtn->setCursor(Qt::PointingHandCursor);
    rootBtn->setMaximumHeight(20);
    rootBtn->setFocusPolicy(Qt::NoFocus);
    rootBtn->setStyleSheet(btnStyle);
    const QString rootTarget = localRoot;
    connect(rootBtn, &QPushButton::clicked, this, [this, rootBtn, rootTarget]() {
        QApplication::clipboard()->setText(rootTarget);
        QPoint pos = rootBtn->mapToGlobal(QPoint(rootBtn->width() / 2, rootBtn->height()));
        showCopiedToast(pos);
        emit pathSelected(rootTarget);
    });
    m_layout->addWidget(rootBtn);
    m_buttons.append(rootBtn);

    if (!relPath.isEmpty()) {
        QStringList parts = relPath.split('/', Qt::SkipEmptyParts);
        QString accumulated = localRoot;
        if (accumulated.endsWith('/')) accumulated.chop(1);  // strip trailing '/'
        for (int i = 0; i < parts.size(); ++i) {
            accumulated += "/" + parts[i];

            auto* separator = new QPushButton(">");
            separator->setFlat(true);
            separator->setEnabled(false);
            separator->setMaximumHeight(20);
            separator->setMaximumWidth(14);
            separator->setFocusPolicy(Qt::NoFocus);
            separator->setStyleSheet(
                "QPushButton {"
                "  padding: 0;"
                "  margin: 0;"
                "  min-height: 20px;"
                "  max-height: 20px;"
                "  border: none;"
                "  background: transparent;"
                "  color: #888;"
                "}");
            m_layout->addWidget(separator);
            m_buttons.append(separator);

            auto* btn = new QPushButton(parts[i]);
            btn->setFlat(true);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setMaximumHeight(20);
            btn->setFocusPolicy(Qt::NoFocus);
            btn->setStyleSheet(btnStyle);

            QString targetPath = accumulated;
            connect(btn, &QPushButton::clicked, this, [this, btn, targetPath]() {
                QApplication::clipboard()->setText(targetPath);
                QPoint pos = btn->mapToGlobal(QPoint(btn->width() / 2, btn->height()));
                showCopiedToast(pos);
                emit pathSelected(targetPath);
            });
            m_layout->addWidget(btn);
            m_buttons.append(btn);
        }
    }

    m_layout->addStretch();

    // Trailing "edit" button — click to switch to text-input mode
    m_editBtn = new QPushButton(QString::fromUtf8("\xE2\x9C\x8E"));  // ✎
    m_editBtn->setToolTip("Edit path (double-click empty area, or Ctrl+L)");
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
    m_layout->addWidget(m_editBtn);
    m_buttons.append(m_editBtn);
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
        auto* label = new QLabel(QString::fromUtf8("\xE2\x9C\x93 \xE8\xB7\xAF\xE5\xBE\x84\xE5\xB7\xB2\xE5\xA4\x8D\xE5\x88\xB6"), m_toast);
        // UTF-8 "✓ 路径已复制"
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
