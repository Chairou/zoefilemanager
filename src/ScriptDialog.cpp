// =============================================================================
// ScriptDialog.cpp —— 见 ScriptDialog.h
// =============================================================================

#include "ScriptDialog.h"
#include "I18n.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QProcess>

ScriptDialog::ScriptDialog(const QVector<FileEntry>& selectedFiles, QWidget* parent)
    : QDialog(parent)
    , m_selectedFiles(selectedFiles)
{
    setMinimumSize(600, 500);

    auto* layout = new QVBoxLayout(this);

    // Warning label
    m_warningLabel = new QLabel();
    m_warningLabel->setStyleSheet("QLabel { color: #ff6b6b; font-weight: bold; padding: 4px; }");
    layout->addWidget(m_warningLabel);

    // Selected files group
    m_filesGroup = new QGroupBox();
    auto* filesLayout = new QVBoxLayout(m_filesGroup);
    m_filesList = new QListWidget();
    m_filesList->setMaximumHeight(100);
    for (const auto& f : selectedFiles) {
        m_filesList->addItem(f.path);
    }
    if (selectedFiles.isEmpty()) {
        m_filesList->addItem(T("(no files selected)"));
    }
    filesLayout->addWidget(m_filesList);
    layout->addWidget(m_filesGroup);

    // Quick commands
    m_cmdGroup = new QGroupBox();
    auto* cmdLayout = new QVBoxLayout(m_cmdGroup);

    auto* quickLayout = new QHBoxLayout();
    m_quickLabel = new QLabel();
    quickLayout->addWidget(m_quickLabel);
    m_quickCommands = new QComboBox();
    setupQuickCommands();
    quickLayout->addWidget(m_quickCommands, 1);
    cmdLayout->addLayout(quickLayout);

    auto* inputLayout = new QHBoxLayout();
    m_commandInput = new QLineEdit();
    inputLayout->addWidget(m_commandInput, 1);
    m_runBtn = new QPushButton();
    m_runBtn->setDefault(true);
    inputLayout->addWidget(m_runBtn);
    cmdLayout->addLayout(inputLayout);

    layout->addWidget(m_cmdGroup);

    // Output area
    m_outputGroup = new QGroupBox();
    auto* outputLayout = new QVBoxLayout(m_outputGroup);
    m_outputArea = new QTextEdit();
    m_outputArea->setReadOnly(true);
    m_outputArea->setFont(QFont("Courier", 11));
    outputLayout->addWidget(m_outputArea);
    layout->addWidget(m_outputGroup);

    // Close button
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_closeBtn = new QPushButton();
    btnLayout->addWidget(m_closeBtn);
    layout->addLayout(btnLayout);

    connect(m_quickCommands, QOverload<int>::of(&QComboBox::activated), this, &ScriptDialog::onQuickCommandSelected);
    connect(m_runBtn, &QPushButton::clicked, this, &ScriptDialog::onRunClicked);
    connect(m_commandInput, &QLineEdit::returnPressed, this, &ScriptDialog::onRunClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    connect(&I18n::instance(), &I18n::changed, this, &ScriptDialog::retranslate);
    retranslate();
}

void ScriptDialog::setupQuickCommands() {
    m_commands = {"ls -la", "file", "wc -l", "md5", "tar -czf archive.tar.gz", "chmod 755"};
    m_quickCommands->addItem(T("-- Select a quick command --"));
    for (const auto& cmd : m_commands) {
        m_quickCommands->addItem(cmd);
    }
}

void ScriptDialog::onQuickCommandSelected(int index) {
    if (index > 0 && index <= m_commands.size()) {
        m_commandInput->setText(m_commands[index - 1]);
    }
}

void ScriptDialog::onRunClicked() {
    QString command = m_commandInput->text().trimmed();
    if (command.isEmpty()) return;

    // Build the full command with selected files as arguments
    QString fullCommand = command;
    if (!m_selectedFiles.isEmpty()) {
        for (const auto& f : m_selectedFiles) {
            fullCommand += " " + QString("\"%1\"").arg(f.path);
        }
    }

    m_outputArea->append(QString("$ %1").arg(fullCommand));

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start("/bin/sh", QStringList() << "-c" << fullCommand);

    const bool zh = (I18n::instance().current() == I18n::Lang::Zh);

    if (!process.waitForStarted(5000)) {
        m_outputArea->append(zh ? "错误：无法启动进程\n" : "Error: Failed to start process\n");
        return;
    }

    if (!process.waitForFinished(5000)) {
        process.kill();
        m_outputArea->append(zh ? "错误：命令超时（5 秒限制）\n"
                                : "Error: Command timed out (5 second limit)\n");
        return;
    }

    QString output = QString::fromUtf8(process.readAll());
    if (output.isEmpty() && process.exitCode() != 0) {
        m_outputArea->append(QString(zh ? "进程退出，代码 %1\n"
                                        : "Process exited with code %1\n").arg(process.exitCode()));
    } else {
        m_outputArea->append(output + "\n");
    }
}

void ScriptDialog::retranslate() {
    setWindowTitle(T("Run Script"));
    if (m_warningLabel)
        m_warningLabel->setText(T("WARNING: Commands will be executed on your real filesystem!"));
    if (m_filesGroup)  m_filesGroup->setTitle(T("Selected Files"));
    if (m_cmdGroup)    m_cmdGroup->setTitle(T("Command"));
    if (m_outputGroup) m_outputGroup->setTitle(T("Output"));
    if (m_quickLabel)  m_quickLabel->setText(T("Quick commands:"));
    if (m_commandInput) m_commandInput->setPlaceholderText(T("Enter command to execute..."));
    if (m_runBtn)      m_runBtn->setText(T("Run"));
    if (m_closeBtn)    m_closeBtn->setText(T("Close"));
    if (m_quickCommands && m_quickCommands->count() > 0) {
        m_quickCommands->setItemText(0, T("-- Select a quick command --"));
    }
}
