#ifndef SHORTCUTBAR_H
#define SHORTCUTBAR_H

// =============================================================================
// ShortcutBar —— 用户自定义"快捷目录"栏（左侧 DirectoryTree 下方）
//
// 行为：
//   - 每个快捷方式是 (name, path) 元组；点击 → 发 shortcutActivated 信号
//   - 用户可点 "+" 添加（弹对话框选目录 + 起名）
//   - 右键单条快捷方式可编辑或删除
//   - 持久化：QSettings("WorkBuddy","ZoeFileManager") 下的 "shortcuts" 数组
//   - 兼容性迁移：发现新 key 为空但旧 key ("DualPaneFileManager") 有数据
//     时自动复制过去并写回新 key，老用户改名升级不丢配置
// =============================================================================

#include <QWidget>
#include <QVector>
#include <QString>

class QVBoxLayout;
class QScrollArea;
class QPushButton;

/// 单个快捷方式：显示名 + 绝对路径
struct ShortcutItem {
    QString name;
    QString path;
};

class ShortcutBar : public QWidget {
    Q_OBJECT

public:
    explicit ShortcutBar(QWidget* parent = nullptr);

    /// 从 QSettings 装载（含旧 key 一次性迁移逻辑）
    void loadFromSettings();
    /// 写回 QSettings（仅写新 key "ZoeFileManager"）
    void saveToSettings() const;

signals:
    /// 用户点击某条快捷方式 —— MainWindow 把 active panel 导航过去
    void shortcutActivated(const QString& path);

private slots:
    void onAddClicked();

private:
    void rebuildButtons();
    void addShortcut(const ShortcutItem& item);
    void removeShortcut(int index);
    void editShortcut(int index);
    QPushButton* makeShortcutButton(const ShortcutItem& item, int index);

    QVBoxLayout* m_listLayout = nullptr;  // 装快捷按钮的纵向布局
    QPushButton* m_addBtn = nullptr;
    QVector<ShortcutItem> m_items;
};

#endif // SHORTCUTBAR_H
