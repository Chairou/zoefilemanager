// =============================================================================
// ScriptDialog.cpp —— 见 ScriptDialog.h
// =============================================================================

#include "ScriptDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QProcess>

ScriptDialog::ScriptDialog(const QVector<FileEntry>& selectedFiles, QWidget* parent)
    : QDialog(parent)
    , m_selectedFiles(selectedFiles)
{
    setWindowTitle("Run Script");
    setMinimumSize(600, 500);

    auto* layout = new QVBoxLayout(this);

    // Warning label
    auto* warningLabel = new QLabel("WARNING: Commands will be executed on your real filesystem!");
    warningLabel->setStyleSheet("QLabel { color: #ff6b6b; font-weight: bold; padding: 4px; }");
    layout->addWidget(warningLabel);

    // Selected files group
    auto* filesGroup = new QGroupBox("Selected Files");
    auto* filesLayout = new QVBoxLayout(filesGroup);
    m_filesList = new QListWidget();
    m_filesList->setMaximumHeight(100);
    for (const auto& f : selectedFiles) {
        m_filesList->addItem(f.path);
    }
    if (selectedFiles.isEmpty()) {
        m_filesList->addItem("(no files selected)");
    }
    filesLayout->addWidget(m_filesList);
    layout->addWidget(filesGroup);

    // Quick commands
    auto* cmdGroup = new QGroupBox("Command");
    auto* cmdLayout = new QVBoxLayout(cmdGroup);

    auto* quickLayout = new QHBoxLayout();
    quickLayout->addWidget(new QLabel("Quick commands:"));
    m_quickCommands = new QComboBox();
    setupQuickCommands();
    quickLayout->addWidget(m_quickCommands, 1);
    cmdLayout->addLayout(quickLayout);

    auto* inputLayout = new QHBoxLayout();
    m_commandInput = new QLineEdit();
    m_commandInput->setPlaceholderText("Enter command to execute...");
    inputLayout->addWidget(m_commandInput, 1);
    m_runBtn = new QPushButton("Run");
    m_runBtn->setDefault(true);
    inputLayout->addWidget(m_runBtn);
    cmdLayout->addLayout(inputLayout);

    layout->addWidget(cmdGroup);

    // Output area
    auto* outputGroup = new QGroupBox("Output");
    auto* outputLayout = new QVBoxLayout(outputGroup);
    m_outputArea = new QTextEdit();
    m_outputArea->setReadOnly(true);
    m_outputArea->setFont(QFont("Courier", 11));
    outputLayout->addWidget(m_outputArea);
    layout->addWidget(outputGroup);

    // Close button
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto* closeBtn = new QPushButton("Close");
    btnLayout->addWidget(closeBtn);
    layout->addLayout(btnLayout);

    connect(m_quickCommands, QOverload<int>::of(&QComboBox::activated), this, &ScriptDialog::onQuickCommandSelected);
    connect(m_runBtn, &QPushButton::clicked, this, &ScriptDialog::onRunClicked);
    connect(m_commandInput, &QLineEdit::returnPressed, this, &ScriptDialog::onRunClicked);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}

void ScriptDialog::setupQuickCommands() {
    m_commands = {"ls -la", "file", "wc -l", "md5", "tar -czf archive.tar.gz", "chmod 755"};
    m_quickCommands->addItem("-- Select a quick command --");
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

    if (!process.waitForStarted(5000)) {
        m_outputArea->append("Error: Failed to start process\n");
        return;
    }

    if (!process.waitForFinished(5000)) {
        process.kill();
        m_outputArea->append("Error: Command timed out (5 second limit)\n");
        return;
    }

    QString output = QString::fromUtf8(process.readAll());
    if (output.isEmpty() && process.exitCode() != 0) {
        m_outputArea->append(QString("Process exited with code %1\n").arg(process.exitCode()));
    } else {
        m_outputArea->append(output + "\n");
    }
}
