#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

// =============================================================================
// SettingsDialog —— 应用设置（语言 + 字号）
//
// 设计：OK 后把用户选择写入 I18n / AppFonts 单例，各单例 emit changed()
// 信号后主窗口和各子控件自动 retranslate/重置字号。
// =============================================================================

#include <QDialog>
class QRadioButton;
class QSpinBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private slots:
    void onAccepted();
    void retranslate();

private:
    // 语言
    QRadioButton* m_enRadio = nullptr;
    QRadioButton* m_zhRadio = nullptr;

    // 字号
    QSpinBox* m_menuFontSpin = nullptr;   // 菜单栏/工具栏字号（0=默认）
    QSpinBox* m_itemFontSpin = nullptr;   // 文件/目录名字号（0=默认）

    // 分组标题 / 行标签（retranslate 用）
    class QGroupBox* m_langGroup = nullptr;
    class QGroupBox* m_fontGroup = nullptr;
    class QLabel*    m_menuFontLabel = nullptr;
    class QLabel*    m_itemFontLabel = nullptr;
    class QLabel*    m_fontHint = nullptr;
};

#endif // SETTINGSDIALOG_H
