#ifndef SCRIPTDIALOG_H
#define SCRIPTDIALOG_H

// =============================================================================
// ScriptDialog —— "对选中文件运行脚本/命令"对话框
//
// 用法：
//   1. 上方列出 MainWindow 传进来的当前选中条目（只读 QListWidget）
//   2. 中间是"快捷命令"下拉（chmod/chown/file/...）+ 命令输入框
//   3. 选了快捷命令会自动填入模板（占位符 {file} 由 onRunClicked 替换为实际文件）
//   4. 下方 QTextEdit 显示 QProcess 的 stdout / stderr 实时输出
//
// 注意：直接调用系统 shell（QProcess::start("/bin/sh", {"-c", cmd})），
// 用户输入未做转义；脚本场景默认信任用户。
// =============================================================================

#include "Types.h"
#include <QDialog>
#include <QLineEdit>
#include <QTextEdit>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QVector>

class ScriptDialog : public QDialog {
    Q_OBJECT

public:
    explicit ScriptDialog(const QVector<FileEntry>& selectedFiles, QWidget* parent = nullptr);

private slots:
    void onQuickCommandSelected(int index);  // 切换下拉项 → 自动填模板
    void onRunClicked();                     // 替换 {file} 占位符 + QProcess 执行

private:
    void setupQuickCommands();   // 内置一组常用命令

    QListWidget* m_filesList;
    QComboBox* m_quickCommands;
    QLineEdit* m_commandInput;
    QTextEdit* m_outputArea;
    QPushButton* m_runBtn;
    QVector<FileEntry> m_selectedFiles;
    QStringList m_commands;     // 与 m_quickCommands 索引一一对应的命令模板
};

#endif // SCRIPTDIALOG_H
