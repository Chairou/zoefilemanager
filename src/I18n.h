#ifndef I18N_H
#define I18N_H

// =============================================================================
// I18n —— 极简国际化工具（中/英双语）
//
// 为什么不用 Qt 的 tr() + QTranslator/.ts/.qm？
//   - 项目原有代码里所有字符串都是硬编码英文，不走 tr()；
//   - Qt 标准流程需要 lupdate/lrelease 生成 .ts/.qm，再通过 CMake
//     打包进 bundle，改动面较大；
//   - 我们的词典规模不大（~100 条），用代码词典直接查表最直接。
//
// 用法：
//   1) 获取当前语言：I18n::instance().current()
//   2) 切换语言：I18n::instance().setCurrent(Lang::Zh)  -> emit changed()
//   3) 取翻译：I18n::t("Key")  —— 若词典中存在该 key，用当前语言版本；
//      否则回退到 key 本身（通常 key 就是英文原文）。
//
// 约定：
//   - key 就是英文原文（保持与现有代码一致），避免大量 #define。
//   - 词典里只放【需要翻译】的 UI 文字；按钮/状态栏/菜单/右键等。
//   - "\tCtrl+C" 等快捷键后缀不作为 key 参与查表，调用方自己拼接。
//
// 存储：QSettings("WorkBuddy","ZoeFileManager") key="ui/language"，
//        值为 "zh" / "en"。首次启动跟随系统 locale。
// =============================================================================

#include <QObject>
#include <QString>

class I18n : public QObject {
    Q_OBJECT
public:
    enum class Lang { En, Zh };

    static I18n& instance();

    Lang current() const { return m_current; }
    void setCurrent(Lang lang);

    // 翻译：若词典中 key 不存在，返回 key 本身。
    QString tr(const QString& key) const;
    // 快捷：带快捷键后缀（e.g. "Copy\tCtrl+C"）——保留后缀，翻译前半。
    QString trWithShortcut(const QString& keyWithTab) const;

signals:
    void changed();

private:
    I18n();
    Lang m_current = Lang::En;
};

// 全局便捷函数。
inline QString T(const QString& key) { return I18n::instance().tr(key); }
inline QString TS(const QString& k)  { return I18n::instance().trWithShortcut(k); }

#endif // I18N_H
