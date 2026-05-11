// =============================================================================
// ShortcutBar.cpp —— 见 ShortcutBar.h
//
// 持久化与迁移逻辑见 loadFromSettings —— 老 key "DualPaneFileManager"
// 自动迁移到新 key "ZoeFileManager"，对老用户透明。
// =============================================================================

#include "ShortcutBar.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QDir>
#include <QFont>

namespace {
// Inline dialog that collects (name, path). If `name` is empty at accept,
// it auto-fills with the basename of the chosen path.
class AddShortcutDialog : public QDialog {
public:
    AddShortcutDialog(QWidget* parent, const QString& initialName = "",
                      const QString& initialPath = "")
        : QDialog(parent)
    {
        setWindowTitle(initialPath.isEmpty() ? "Add shortcut" : "Edit shortcut");
        setMinimumWidth(420);

        auto* layout = new QVBoxLayout(this);

        layout->addWidget(new QLabel("Name (optional, defaults to folder name):"));
        m_nameEdit = new QLineEdit(initialName);
        m_nameEdit->setPlaceholderText("e.g. Projects");
        layout->addWidget(m_nameEdit);

        layout->addSpacing(8);
        layout->addWidget(new QLabel("Directory:"));

        auto* pathRow = new QHBoxLayout();
        m_pathEdit = new QLineEdit(initialPath);
        m_pathEdit->setPlaceholderText(QDir::homePath());
        auto* browseBtn = new QPushButton("Browse...");
        pathRow->addWidget(m_pathEdit, 1);
        pathRow->addWidget(browseBtn);
        layout->addLayout(pathRow);

        connect(browseBtn, &QPushButton::clicked, this, [this]() {
            QString start = m_pathEdit->text().trimmed();
            if (start.isEmpty()) start = QDir::homePath();
            QString dir = QFileDialog::getExistingDirectory(this, "Select directory", start);
            if (!dir.isEmpty()) m_pathEdit->setText(dir);
        });

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addSpacing(8);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    QString name() const {
        QString n = m_nameEdit->text().trimmed();
        if (n.isEmpty()) {
            QString p = m_pathEdit->text().trimmed();
            if (p.endsWith('/') && p.length() > 1) p.chop(1);
            n = QFileInfo(p).fileName();
            if (n.isEmpty()) n = p;  // root
        }
        return n;
    }
    QString path() const { return m_pathEdit->text().trimmed(); }

private:
    QLineEdit* m_nameEdit;
    QLineEdit* m_pathEdit;
};
} // namespace

ShortcutBar::ShortcutBar(QWidget* parent)
    : QWidget(parent)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    // Header row: title + "add" button
    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(4);
    auto* title = new QLabel("SHORTCUTS");
    title->setStyleSheet(
        "color: #81A1C1;"        // Nord9 pale blue
        "font-size: 10px;"
        "font-weight: bold;"
        "letter-spacing: 1px;");

    m_addBtn = new QPushButton("+");
    m_addBtn->setFixedSize(22, 22);
    m_addBtn->setToolTip("Add shortcut");
    m_addBtn->setCursor(Qt::PointingHandCursor);
    m_addBtn->setFocusPolicy(Qt::NoFocus);
    m_addBtn->setFlat(true);
    // Make sure the "+" character is rendered and centered
    QFont addFont = m_addBtn->font();
    addFont.setPointSize(14);
    addFont.setBold(true);
    m_addBtn->setFont(addFont);
    m_addBtn->setStyleSheet(
        "QPushButton {"
        "  background: transparent;"
        "  color: #88C0D0;"
        "  border: 1px solid #4C566A;"
        "  border-radius: 11px;"
        "  padding: 0;"
        "  margin: 0;"
        "  text-align: center;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(136, 192, 208, 0.20);"
        "  border-color: #88C0D0;"
        "  color: #ECEFF4;"
        "}"
        "QPushButton:pressed {"
        "  background: #5E81AC;"
        "  color: #ECEFF4;"
        "}");

    headerRow->addWidget(title, 1);
    headerRow->addWidget(m_addBtn);

    rootLayout->addLayout(headerRow);

    // Scroll area for shortcut buttons
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: transparent; }");

    auto* scrollContent = new QWidget();
    scrollContent->setStyleSheet("background: transparent;");
    m_listLayout = new QVBoxLayout(scrollContent);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(2);
    m_listLayout->addStretch();

    scroll->setWidget(scrollContent);
    rootLayout->addWidget(scroll);

    connect(m_addBtn, &QPushButton::clicked, this, &ShortcutBar::onAddClicked);

    loadFromSettings();
}

QPushButton* ShortcutBar::makeShortcutButton(const ShortcutItem& item, int index) {
    auto* btn = new QPushButton(QString::fromUtf8("\xE2\xAD\x90 ") + item.name);
    btn->setToolTip(item.path);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(
        "QPushButton {"
        "  text-align: left;"
        "  padding: 5px 10px;"
        "  background: #3B4252;"        // Nord1
        "  color: #E5E9F0;"             // Nord5
        "  border: 1px solid #434C5E;"  // Nord2
        "  border-radius: 4px;"
        "}"
        "QPushButton:hover {"
        "  background: #434C5E;"
        "  color: #88C0D0;"
        "  border-color: #88C0D0;"
        "}"
        "QPushButton:pressed {"
        "  background: #5E81AC;"
        "  color: #ECEFF4;"
        "  border-color: #5E81AC;"
        "}");

    connect(btn, &QPushButton::clicked, this, [this, index]() {
        if (index < 0 || index >= m_items.size()) return;
        emit shortcutActivated(m_items[index].path);
    });

    connect(btn, &QPushButton::customContextMenuRequested, this,
            [this, btn, index](const QPoint& pos) {
        QMenu menu(btn);
        menu.addAction("Edit...", this, [this, index]() { editShortcut(index); });
        menu.addSeparator();
        menu.addAction("Remove", this, [this, index]() { removeShortcut(index); });
        menu.exec(btn->mapToGlobal(pos));
    });

    return btn;
}

void ShortcutBar::rebuildButtons() {
    // Remove all current widgets except the trailing stretch
    while (m_listLayout->count() > 0) {
        QLayoutItem* item = m_listLayout->takeAt(0);
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    for (int i = 0; i < m_items.size(); ++i) {
        m_listLayout->addWidget(makeShortcutButton(m_items[i], i));
    }
    m_listLayout->addStretch();
}

void ShortcutBar::addShortcut(const ShortcutItem& item) {
    m_items.append(item);
    saveToSettings();
    rebuildButtons();
}

void ShortcutBar::removeShortcut(int index) {
    if (index < 0 || index >= m_items.size()) return;
    auto ans = QMessageBox::question(this, "Remove shortcut",
        QString("Remove shortcut \"%1\"?").arg(m_items[index].name));
    if (ans != QMessageBox::Yes) return;
    m_items.removeAt(index);
    saveToSettings();
    rebuildButtons();
}

void ShortcutBar::editShortcut(int index) {
    if (index < 0 || index >= m_items.size()) return;
    AddShortcutDialog dlg(this, m_items[index].name, m_items[index].path);
    if (dlg.exec() != QDialog::Accepted) return;

    QString path = dlg.path();
    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        QMessageBox::warning(this, "Invalid directory",
            "Please choose an existing directory.");
        return;
    }
    m_items[index].name = dlg.name();
    m_items[index].path = path;
    saveToSettings();
    rebuildButtons();
}

void ShortcutBar::onAddClicked() {
    AddShortcutDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    QString path = dlg.path();
    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        QMessageBox::warning(this, "Invalid directory",
            "Please choose an existing directory.");
        return;
    }
    ShortcutItem item{ dlg.name(), path };
    addShortcut(item);
}

// QSettings keys: the application was renamed from "DualPaneFileManager"
// to "ZoeFileManager" in 0.8. We persist under the new name and, on first
// load, transparently migrate any data found under the old key so users
// don't lose their saved shortcuts.
namespace {
constexpr auto kSettingsOrg     = "WorkBuddy";
constexpr auto kSettingsApp     = "ZoeFileManager";
constexpr auto kLegacyAppKey    = "DualPaneFileManager";
}

void ShortcutBar::loadFromSettings() {
    QSettings s(kSettingsOrg, kSettingsApp);
    int size = s.beginReadArray("shortcuts");

    // First-run migration: if the new key is empty but the old key has
    // entries, copy them over before reading.
    if (size == 0) {
        s.endArray();
        QSettings legacy(kSettingsOrg, kLegacyAppKey);
        int legacySize = legacy.beginReadArray("shortcuts");
        if (legacySize > 0) {
            QVector<ShortcutItem> migrated;
            for (int i = 0; i < legacySize; ++i) {
                legacy.setArrayIndex(i);
                ShortcutItem item;
                item.name = legacy.value("name").toString();
                item.path = legacy.value("path").toString();
                if (!item.name.isEmpty() && !item.path.isEmpty()) {
                    migrated.append(item);
                }
            }
            legacy.endArray();
            // Persist under the new key so we only do this once.
            s.beginWriteArray("shortcuts", migrated.size());
            for (int i = 0; i < migrated.size(); ++i) {
                s.setArrayIndex(i);
                s.setValue("name", migrated[i].name);
                s.setValue("path", migrated[i].path);
            }
            s.endArray();
        } else {
            legacy.endArray();
        }
        // Re-open the read array so the regular load path below picks
        // up either the freshly-migrated data or stays empty.
        size = s.beginReadArray("shortcuts");
    }

    m_items.clear();
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        ShortcutItem item;
        item.name = s.value("name").toString();
        item.path = s.value("path").toString();
        if (!item.name.isEmpty() && !item.path.isEmpty()) {
            m_items.append(item);
        }
    }
    s.endArray();
    rebuildButtons();
}

void ShortcutBar::saveToSettings() const {
    QSettings s(kSettingsOrg, kSettingsApp);
    s.beginWriteArray("shortcuts", m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("name", m_items[i].name);
        s.setValue("path", m_items[i].path);
    }
    s.endArray();
}
