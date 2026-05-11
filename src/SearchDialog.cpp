// =============================================================================
// SearchDialog.cpp —— 见 SearchDialog.h
// =============================================================================

#include "SearchDialog.h"
#include "RealFileSystem.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>

SearchDialog::SearchDialog(const QString& currentPath, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Search Files");
    setMinimumSize(500, 400);

    auto* layout = new QVBoxLayout(this);

    auto* formLayout = new QFormLayout();

    m_queryInput = new QLineEdit();
    m_queryInput->setPlaceholderText("Enter search term...");
    formLayout->addRow("Search:", m_queryInput);

    m_pathInput = new QLineEdit(currentPath);
    formLayout->addRow("In path:", m_pathInput);

    layout->addLayout(formLayout);

    auto* btnLayout = new QHBoxLayout();
    m_searchBtn = new QPushButton("Search");
    m_searchBtn->setDefault(true);
    btnLayout->addStretch();
    btnLayout->addWidget(m_searchBtn);
    layout->addLayout(btnLayout);

    m_statusLabel = new QLabel("Enter a search term and click Search");
    layout->addWidget(m_statusLabel);

    m_resultsList = new QListWidget();
    layout->addWidget(m_resultsList);

    connect(m_searchBtn, &QPushButton::clicked, this, &SearchDialog::performSearch);
    connect(m_queryInput, &QLineEdit::returnPressed, this, &SearchDialog::performSearch);
    connect(m_resultsList, &QListWidget::itemDoubleClicked, this, &SearchDialog::onResultDoubleClicked);

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

    m_statusLabel->setText(QString("Found %1 result(s)").arg(results.size()));
}

void SearchDialog::onResultDoubleClicked(QListWidgetItem* item) {
    QString path = item->data(Qt::UserRole).toString();
    emit navigateToPath(path);
    accept();
}
