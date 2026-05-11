// =============================================================================
// SettingsDialog.cpp —— 见 SettingsDialog.h
// =============================================================================

#include "SettingsDialog.h"
#include "I18n.h"
#include "AppFonts.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QLabel>

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    auto* layout = new QVBoxLayout(this);

    // ---- 语言 ----
    m_langGroup = new QGroupBox(this);
    auto* langLayout = new QVBoxLayout(m_langGroup);
    m_enRadio = new QRadioButton(m_langGroup);
    m_zhRadio = new QRadioButton(m_langGroup);
    langLayout->addWidget(m_enRadio);
    langLayout->addWidget(m_zhRadio);
    layout->addWidget(m_langGroup);

    if (I18n::instance().current() == I18n::Lang::Zh) {
        m_zhRadio->setChecked(true);
    } else {
        m_enRadio->setChecked(true);
    }

    // ---- 字号 ----
    m_fontGroup = new QGroupBox(this);
    auto* fontLayout = new QFormLayout(m_fontGroup);

    auto& f = AppFonts::instance();

    m_menuFontSpin = new QSpinBox(m_fontGroup);
    m_menuFontSpin->setRange(0, 30);     // 0 表示"使用默认"
    m_menuFontSpin->setSuffix(" pt");
    m_menuFontSpin->setSpecialValueText("Default");
    m_menuFontSpin->setValue(f.menuFontSize());
    m_menuFontLabel = new QLabel();
    fontLayout->addRow(m_menuFontLabel, m_menuFontSpin);

    m_itemFontSpin = new QSpinBox(m_fontGroup);
    m_itemFontSpin->setRange(0, 30);
    m_itemFontSpin->setSuffix(" pt");
    m_itemFontSpin->setSpecialValueText("Default");
    m_itemFontSpin->setValue(f.itemFontSize());
    m_itemFontLabel = new QLabel();
    fontLayout->addRow(m_itemFontLabel, m_itemFontSpin);

    m_fontHint = new QLabel();
    m_fontHint->setStyleSheet("color: #888;");
    m_fontHint->setWordWrap(true);
    fontLayout->addRow(m_fontHint);

    layout->addWidget(m_fontGroup);

    // ---- 按钮 ----
    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(btns);
    connect(btns, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);

    retranslate();
    setMinimumWidth(380);
}

void SettingsDialog::onAccepted() {
    if (m_zhRadio->isChecked()) {
        I18n::instance().setCurrent(I18n::Lang::Zh);
    } else {
        I18n::instance().setCurrent(I18n::Lang::En);
    }
    AppFonts::instance().setMenuFontSize(m_menuFontSpin->value());
    AppFonts::instance().setItemFontSize(m_itemFontSpin->value());
    accept();
}

void SettingsDialog::retranslate() {
    setWindowTitle(T("Settings"));
    if (m_langGroup) m_langGroup->setTitle(T("Language"));
    if (m_enRadio)   m_enRadio->setText(T("English"));
    if (m_zhRadio)   m_zhRadio->setText(T("中文 (Simplified)"));
    if (m_fontGroup) m_fontGroup->setTitle(T("Font Size"));
    if (m_menuFontLabel) m_menuFontLabel->setText(T("Menu / Toolbar:"));
    if (m_itemFontLabel) m_itemFontLabel->setText(T("File / Folder name:"));
    if (m_fontHint) m_fontHint->setText(T("0 = use default; recommended 13–18 pt."));
    const QString defaultText = (I18n::instance().current() == I18n::Lang::Zh)
                                ? "默认" : "Default";
    if (m_menuFontSpin) m_menuFontSpin->setSpecialValueText(defaultText);
    if (m_itemFontSpin) m_itemFontSpin->setSpecialValueText(defaultText);
}
