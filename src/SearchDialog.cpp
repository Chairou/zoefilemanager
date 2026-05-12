// =============================================================================
// SearchDialog.cpp —— 见 SearchDialog.h
// =============================================================================

#include "SearchDialog.h"
#include "RealFileSystem.h"
#include "I18n.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>

SearchDialog::SearchDialog(const QString& currentPath, QWidget* parent)
    : QDialog(parent)
{
    setMinimumSize(720, 500);
    resize(900, 600);

    auto* layout = new QVBoxLayout(this);

    auto* formLayout = new QFormLayout();
    // 让 field 列跟随对话框宽度铺满（默认是 FieldsStayAtSizeHint，会偏窄）
    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    formLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    formLayout->setHorizontalSpacing(8);

    m_queryInput = new QLineEdit();
    m_queryInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_queryInput->setMinimumHeight(28);
    m_searchLabel = new QLabel();
    formLayout->addRow(m_searchLabel, m_queryInput);

    m_pathInput = new QLineEdit(currentPath);
    m_pathInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_pathInput->setMinimumHeight(28);
    m_pathLabel = new QLabel();
    formLayout->addRow(m_pathLabel, m_pathInput);

    layout->addLayout(formLayout);

    auto* btnLayout = new QHBoxLayout();
    m_searchBtn = new QPushButton();
    m_searchBtn->setDefault(true);
    btnLayout->addStretch();
    btnLayout->addWidget(m_searchBtn);
    layout->addLayout(btnLayout);

    m_statusLabel = new QLabel();
    layout->addWidget(m_statusLabel);

    m_resultsList = new QListWidget();
    layout->addWidget(m_resultsList);

    connect(m_searchBtn, &QPushButton::clicked, this, &SearchDialog::performSearch);
    connect(m_queryInput, &QLineEdit::returnPressed, this, &SearchDialog::performSearch);
    connect(m_resultsList, &QListWidget::itemDoubleClicked, this, &SearchDialog::onResultDoubleClicked);

    connect(&I18n::instance(), &I18n::changed, this, &SearchDialog::retranslate);
    retranslate();

    m_queryInput->setFocus();
}

void SearchDialog::performSearch() {
    QString query = m_queryInput->text().trimmed();
    if (query.isEmpty()) return;

    QString basePath = m_pathInput->text().trimmed();
    if (basePath.isEmpty()) basePath = "/";

    auto results = RealFileSystem::instance().search(query, basePath);

    m_resultsList->clear();
    for (const auto& entry : results) {
        QString typeStr = entry.isDirectory ? "[DIR]" : "[FILE]";
        auto* item = new QListWidgetItem(typeStr + " " + entry.path);
        item->setData(Qt::UserRole, entry.isDirectory ? entry.path : entry.path.left(entry.path.lastIndexOf('/')));
        m_resultsList->addItem(item);
    }

    m_lastResultCount = static_cast<int>(results.size());
    const bool zh = (I18n::instance().current() == I18n::Lang::Zh);
    m_statusLabel->setText(
        QString(zh ? "找到 %1 个结果" : "Found %1 result(s)").arg(m_lastResultCount));
}

void SearchDialog::onResultDoubleClicked(QListWidgetItem* item) {
    QString path = item->data(Qt::UserRole).toString();
    emit navigateToPath(path);
    accept();
}

void SearchDialog::retranslate() {
    setWindowTitle(T("Search Files"));
    m_queryInput->setPlaceholderText(T("Enter search term..."));
    if (m_searchLabel) m_searchLabel->setText(T("Search:"));
    if (m_pathLabel)   m_pathLabel->setText(T("In path:"));
    m_searchBtn->setText(T("Search"));
    if (m_lastResultCount < 0) {
        m_statusLabel->setText(T("Enter a search term and click Search"));
    } else {
        const bool zh = (I18n::instance().current() == I18n::Lang::Zh);
        m_statusLabel->setText(
            QString(zh ? "找到 %1 个结果" : "Found %1 result(s)").arg(m_lastResultCount));
    }
}
