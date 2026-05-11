// =============================================================================
// AppFonts.cpp —— 见 AppFonts.h
// =============================================================================

#include "AppFonts.h"
#include "I18n.h"
#include <QSettings>

AppFonts& AppFonts::instance() {
    static AppFonts inst;
    return inst;
}

AppFonts::AppFonts() {
    QSettings s("WorkBuddy", "ZoeFileManager");
    m_menuFontSize = s.value("ui/menuFontSize", 0).toInt();
    m_itemFontSize = s.value("ui/itemFontSize", 0).toInt();
}

int AppFonts::effectiveMenuFontSize() const {
    if (m_menuFontSize > 0) return m_menuFontSize;
    // 未显式设置：中文默认 15pt，英文返回 0（= 用控件系统默认）
    return (I18n::instance().current() == I18n::Lang::Zh) ? 15 : 0;
}

int AppFonts::effectiveItemFontSize() const {
    if (m_itemFontSize > 0) return m_itemFontSize;
    return 0;   // 用控件自身默认
}

void AppFonts::setMenuFontSize(int pt) {
    if (pt == m_menuFontSize) return;
    m_menuFontSize = pt;
    QSettings s("WorkBuddy", "ZoeFileManager");
    s.setValue("ui/menuFontSize", pt);
    emit changed();
}

void AppFonts::setItemFontSize(int pt) {
    if (pt == m_itemFontSize) return;
    m_itemFontSize = pt;
    QSettings s("WorkBuddy", "ZoeFileManager");
    s.setValue("ui/itemFontSize", pt);
    emit changed();
}
