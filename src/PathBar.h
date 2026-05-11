#ifndef PATHBAR_H
#define PATHBAR_H

// =============================================================================
// PathBar —— 路径栏（面包屑 + 可编辑模式）
//
// 两个页面切换：
//   1. 面包屑模式（默认）：把当前路径切成按钮："/ > Users > foo > bar"
//      点击 root 跳到根；点击中间段跳到祖先；同时把目标路径复制到系统剪贴板
//      并显示一秒钟的"已复制"小气泡（toast）。
//   2. 编辑模式：变成 QLineEdit，用户可以直接输入路径（Tab 补全暂未做）。
//      Enter 提交，Esc/失焦取消。
//
// 远程 URL 处理（关键）：
//   - 路径 sftp://user@host:22/pub/example 会被识别为：root 显示为
//     "sftp://user@host:22/"，后面用斜杠拆出 "pub" / "example"。
//   - 普通 split('/') 会把 "sftp:" 和 "user@host:22" 拆成奇怪的按钮，
//     必须先问 FileSystemRouter::mountFor() 把挂载前缀整体作"根"处理。
// =============================================================================

#include <QWidget>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVector>
#include <QString>

class QLabel;
class QTimer;
class QLineEdit;
class QStackedLayout;

class PathBar : public QWidget {
    Q_OBJECT

public:
    explicit PathBar(QWidget* parent = nullptr);

    void setPath(const QString& path);
    QString path() const { return m_path; }

    // 切到/退出编辑模式（也响应 MainWindow 的 Ctrl+L 快捷键）
    void enterEditMode();
    void exitEditMode();

signals:
    /// 用户点了某个面包屑按钮，或在编辑框 Enter 了一个有效路径
    void pathSelected(const QString& path);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void rebuildButtons();          // 根据 m_path 重建面包屑按钮
    void showCopiedToast(const QPoint& globalPos);  // 顶层无边框小气泡
    void commitEditedPath();        // 编辑模式 Enter 处理

    QString m_path;
    QStackedLayout* m_stack = nullptr;

    // ---- 面包屑页面 ----
    QWidget* m_breadcrumbPage = nullptr;
    QHBoxLayout* m_layout = nullptr;
    QVector<QPushButton*> m_buttons;
    QPushButton* m_editBtn = nullptr;   // "✎" 按钮 —— 切换到编辑模式

    // ---- 编辑页面 ----
    QWidget* m_editPage = nullptr;
    QLineEdit* m_pathEdit = nullptr;

    // ---- 临时 "Path copied" toast ----
    QWidget* m_toast = nullptr;
    QTimer* m_toastTimer = nullptr;
};

#endif // PATHBAR_H
