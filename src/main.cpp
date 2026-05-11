// =============================================================================
// main.cpp —— 应用入口
//
// 极简：构造 QApplication、设置应用名（影响 macOS Dock / 菜单栏 / About 对话框
// 显示的标题），创建 MainWindow、show、进入事件循环。
//
// 不做：命令行解析、单实例锁、自定义崩溃处理（如需，应在这里挂上去）。
// =============================================================================

#include "MainWindow.h"
#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // 应用展示名 —— macOS 菜单栏左上角、Dock 标签、关于对话框都会读这个。
    // 注意：ShortcutBar 透明地把旧 QSettings key
    // ("WorkBuddy/DualPaneFileManager") 迁移到新名 "ZoeFileManager"，
    // 所以即便我们改了 displayName，老用户的快捷方式不会丢。
    app.setApplicationName("Zoe File Manager");
    app.setApplicationDisplayName("Zoe File Manager");
    app.setApplicationVersion("0.8");

    MainWindow window;
    window.show();

    return app.exec();
}
