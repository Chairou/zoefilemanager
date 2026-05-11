#ifndef APPFONTS_H
#define APPFONTS_H

// =============================================================================
// AppFonts —— 应用级字号设置（菜单栏 + 文件/目录名）
//
// 两个独立可调字号：
//   - menuFontSize  : 工具栏、菜单栏、右键菜单文字字号
//   - itemFontSize  : 左侧目录树 + 双面板文件列表的条目字号
//
// 0（或负数）= 使用系统默认字号，不强制覆盖。
//
// 持久化：QSettings("WorkBuddy","ZoeFileManager") key:
//   ui/menuFontSize、ui/itemFontSize
//
// 语言切换时，如果当前菜单字号为 0（默认），中文模式下也会动态返回 15 作为
// "语言相关默认"——通过 effectiveMenuFontSize() 暴露给调用方。
// =============================================================================

#include <QObject>

class AppFonts : public QObject {
    Q_OBJECT
public:
    static AppFonts& instance();

    int menuFontSize() const { return m_menuFontSize; }   // 用户设置值（0 = 默认）
    int itemFontSize() const { return m_itemFontSize; }   // 用户设置值（0 = 默认）

    /// 生效字号：考虑"中文默认放大"规则。
    /// 若用户显式设置了 > 0 的值，直接用；否则中文时返回 15、英文时返回 0。
    int effectiveMenuFontSize() const;
    int effectiveItemFontSize() const;   // 0 = 使用控件默认字号

    void setMenuFontSize(int pt);
    void setItemFontSize(int pt);

signals:
    void changed();

private:
    AppFonts();
    int m_menuFontSize = 0;
    int m_itemFontSize = 0;
};

#endif // APPFONTS_H
