#ifndef SEARCHDIALOG_H
#define SEARCHDIALOG_H

// =============================================================================
// SearchDialog —— "在当前目录下搜索文件名"对话框
//
// 极简：QLineEdit 输入查询词 → 点 Search 触发 RealFileSystem::search
// → 结果列在 QListWidget 里，双击结果发 navigateToPath 信号让调用方
// 把面板导航过去（实际是导航到结果项的父目录）。
//
// 局限：当前 search 是同步的、单线程的；查询大量文件时会卡 UI。
// 真要做工程级搜索可以后续改成 QtConcurrent + 实时增量结果。
// =============================================================================

#include "Types.h"
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>

class SearchDialog : public QDialog {
    Q_OBJECT

public:
    /// `currentPath` 决定搜索根；用户可在对话框里改
    explicit SearchDialog(const QString& currentPath, QWidget* parent = nullptr);

signals:
    /// 双击结果项时发出，路径已经是结果项所在的目录
    void navigateToPath(const QString& path);

private slots:
    void performSearch();
    void onResultDoubleClicked(QListWidgetItem* item);

private:
    QLineEdit* m_queryInput;
    QLineEdit* m_pathInput;
    QPushButton* m_searchBtn;
    QListWidget* m_resultsList;
    QLabel* m_statusLabel;
};

#endif // SEARCHDIALOG_H
