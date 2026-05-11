// =============================================================================
// AboutDialog.cpp —— 见 AboutDialog.h
// =============================================================================

#include "AboutDialog.h"
#include "I18n.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QTextStream>
#include <QVBoxLayout>

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setMinimumSize(640, 540);

    auto* root = new QVBoxLayout(this);

    // ---- 应用标题 + 版本 ----
    auto* title = new QLabel("Zoe File Manager");
    QFont tf = title->font();
    tf.setPointSize(tf.pointSize() + 6);
    tf.setBold(true);
    title->setFont(tf);
    root->addWidget(title);

    m_subLabel = new QLabel();
    m_subLabel->setStyleSheet("color: #888;");
    root->addWidget(m_subLabel);

    // ---- LGPL 致谢（满足 LGPL v3 第 4(d) 条 attribution 要求） ----
    // 注意：法律文本保留英文，仅翻译最后一段的解释性说明。
    m_ackLabel = new QLabel();
    m_ackLabel->setWordWrap(true);
    m_ackLabel->setOpenExternalLinks(true);
    m_ackLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    root->addWidget(m_ackLabel);

    // ---- License 全文查看区 ----
    auto* btnRow = new QHBoxLayout();
    m_currentLabel = new QLabel();
    m_currentLabel->setStyleSheet("color: #888;");
    btnRow->addWidget(m_currentLabel, 1);

    auto addBtn = [&](const QString& text, const QString& file) {
        auto* b = new QPushButton(text);
        connect(b, &QPushButton::clicked, this,
                [this, file]() { showLicenseFile(file); });
        btnRow->addWidget(b);
    };
    addBtn("LGPL-3.0",     "LGPL-3.0.txt");
    addBtn("libssh2",      "libssh2.txt");
    addBtn("OpenSSL",      "OpenSSL.txt");
    addBtn(T("All notices"),  "THIRD_PARTY_LICENSES.txt");
    root->addLayout(btnRow);

    m_licenseView = new QTextEdit();
    m_licenseView->setReadOnly(true);
    m_licenseView->setFontFamily("Menlo");
    m_licenseView->setStyleSheet(
        "QTextEdit { background: #2E3440; color: #D8DEE9; "
        "  border: 1px solid #434C5E; border-radius: 4px; padding: 6px; }");
    root->addWidget(m_licenseView, 1);

    // 默认载入 LGPL —— 它是和 Qt 商用合规最相关的一份
    showLicenseFile("LGPL-3.0.txt");

    // 关闭按钮
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    root->addWidget(bb);

    connect(&I18n::instance(), &I18n::changed, this, &AboutDialog::retranslate);
    retranslate();
}

QString AboutDialog::licensesDir() const {
    // .app/Contents/Resources/licenses/ —— Qt 提供的 applicationDirPath()
    // 在 bundle 模式下指向 .app/Contents/MacOS/，所以从那里上一级到 Resources。
    QDir d(QCoreApplication::applicationDirPath());
    d.cdUp();                         // → .app/Contents
    if (d.cd("Resources/licenses")) {
        return d.absolutePath();
    }
    // 开发期 / flat 二进制启动：licenses/ 直接在源码树里
    QDir source(QCoreApplication::applicationDirPath());
    if (source.cd("../licenses")) return source.absolutePath();
    if (source.cd("../../licenses")) return source.absolutePath();
    return QString();
}

void AboutDialog::showLicenseFile(const QString& fileName) {
    m_currentLicenseFile = fileName;
    const bool zh = (I18n::instance().current() == I18n::Lang::Zh);
    const QString dir = licensesDir();
    if (dir.isEmpty()) {
        m_licenseView->setPlainText(zh
            ? "本构建中未找到 license 文件。应位于 .app bundle 的 "
              "Contents/Resources/licenses/ 下。"
            : "License files were not found in this build. They should "
              "live under Contents/Resources/licenses/ inside the .app bundle.");
        m_currentLabel->setText(QString(zh ? "（缺失：%1）" : "(missing: %1)").arg(fileName));
        return;
    }
    const QString full = dir + "/" + fileName;
    QFile f(full);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_licenseView->setPlainText(
            QString(zh ? "无法打开 license 文件：\n%1"
                       : "Could not open license file:\n%1").arg(full));
        m_currentLabel->setText(QString(zh ? "（错误：%1）" : "(error: %1)").arg(fileName));
        return;
    }
    QTextStream ts(&f);
    m_licenseView->setPlainText(ts.readAll());
    m_currentLabel->setText(QString(zh ? "正在显示：%1" : "Showing: %1").arg(fileName));
}

void AboutDialog::retranslate() {
    setWindowTitle(T("About Zoe File Manager"));
    const bool zh = (I18n::instance().current() == I18n::Lang::Zh);
    const QString ver = QCoreApplication::applicationVersion();
    if (m_subLabel) {
        m_subLabel->setText(QString(zh ? "版本 %1\n\n版权所有 (c) 2026 WorkBuddy"
                                       : "Version %1\n\nCopyright (c) 2026 WorkBuddy").arg(ver));
    }
    if (m_ackLabel) {
        // 法律声明的前两段保持英文确保合规；末段"下方查看/可替换 Qt 框架"翻译。
        m_ackLabel->setText(zh
            ? "<p>本软件使用 <b>Qt 6</b> "
              "(<a href=\"https://www.qt.io\">https://www.qt.io</a>)，遵循 "
              "<b>GNU Lesser General Public License, version 3 (LGPL-3.0)</b>。"
              "Qt 版权归 The Qt Company Ltd. 及其他贡献者所有。</p>"
              "<p>本软件同时使用第三方依赖 <b>libssh2</b>（BSD-3-Clause）和 "
              "<b>OpenSSL</b>（Apache-2.0）。</p>"
              "<p>所有许可证全文可在下方查看，也位于应用 bundle 的 "
              "<code>Contents/Resources/licenses/</code> 目录下。"
              "终端用户可通过覆盖 <code>Contents/Frameworks/</code> 下的文件"
              "替换内置的 Qt 框架为任何兼容的 Qt 6 构建。</p>"
            : "<p>This software uses <b>Qt 6</b> "
              "(<a href=\"https://www.qt.io\">https://www.qt.io</a>) under the "
              "<b>GNU Lesser General Public License, version 3 (LGPL-3.0)</b>. "
              "Qt is Copyright (C) The Qt Company Ltd. and other contributors.</p>"
              "<p>This software also uses "
              "<b>libssh2</b> (BSD-3-Clause) and <b>OpenSSL</b> (Apache-2.0) "
              "as third-party dependencies.</p>"
              "<p>The full text of every license is available below and inside "
              "the application bundle under "
              "<code>Contents/Resources/licenses/</code>. "
              "End users are entitled to replace the bundled Qt frameworks with "
              "any compatible build of Qt 6 by overwriting the files under "
              "<code>Contents/Frameworks/</code>.</p>");
    }
    // 重新刷一次 current license label（文件未变，只是语言变了）
    if (m_currentLabel) {
        if (m_currentLicenseFile.isEmpty()) {
            m_currentLabel->setText(T("(license text will appear below)"));
        } else {
            m_currentLabel->setText(
                QString(zh ? "正在显示：%1" : "Showing: %1").arg(m_currentLicenseFile));
        }
    }
}
