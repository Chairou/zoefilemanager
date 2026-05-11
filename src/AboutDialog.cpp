// =============================================================================
// AboutDialog.cpp —— 见 AboutDialog.h
// =============================================================================

#include "AboutDialog.h"

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
    setWindowTitle("About Zoe File Manager");
    setMinimumSize(640, 540);

    auto* root = new QVBoxLayout(this);

    // ---- 应用标题 + 版本 ----
    auto* title = new QLabel("Zoe File Manager");
    QFont tf = title->font();
    tf.setPointSize(tf.pointSize() + 6);
    tf.setBold(true);
    title->setFont(tf);
    root->addWidget(title);

    const QString ver = QCoreApplication::applicationVersion();
    auto* sub = new QLabel(QString("Version %1\n\nCopyright (c) 2026 WorkBuddy").arg(ver));
    sub->setStyleSheet("color: #888;");
    root->addWidget(sub);

    // ---- LGPL 致谢（满足 LGPL v3 第 4(d) 条 attribution 要求） ----
    auto* ack = new QLabel(
        "<p>This software uses <b>Qt 6</b> "
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
    ack->setWordWrap(true);
    ack->setOpenExternalLinks(true);
    ack->setTextInteractionFlags(Qt::TextBrowserInteraction);
    root->addWidget(ack);

    // ---- License 全文查看区 ----
    auto* btnRow = new QHBoxLayout();
    m_currentLabel = new QLabel("(license text will appear below)");
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
    addBtn("All notices",  "THIRD_PARTY_LICENSES.txt");
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
    const QString dir = licensesDir();
    if (dir.isEmpty()) {
        m_licenseView->setPlainText(
            "License files were not found in this build. They should "
            "live under Contents/Resources/licenses/ inside the .app bundle.");
        m_currentLabel->setText(QString("(missing: %1)").arg(fileName));
        return;
    }
    const QString full = dir + "/" + fileName;
    QFile f(full);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_licenseView->setPlainText(
            QString("Could not open license file:\n%1").arg(full));
        m_currentLabel->setText(QString("(error: %1)").arg(fileName));
        return;
    }
    QTextStream ts(&f);
    m_licenseView->setPlainText(ts.readAll());
    m_currentLabel->setText(QString("Showing: %1").arg(fileName));
}
