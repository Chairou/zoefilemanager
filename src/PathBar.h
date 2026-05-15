#ifndef PATHBAR_H
#define PATHBAR_H

// =============================================================================
// PathBar —— 路径栏（当前路径显示 + 可编辑模式）
//
// 两个页面切换：
//   1. 显示模式（默认）：**本地路径**以可点击面包屑形式展示每一段祖先目录，
//      点击任一段 emit pathSelected(absPrefix) → FilePanel::navigateTo 跳转；
//      **远程 URL (sftp://...)** 原样整条只读展示，不做拆分（避免语义歧义）。
//      SSOT 铁律不变：m_path 始终是文件视图 rootPath 的镜像，面包屑只是
//      对 m_path 的**展示**，所有跳转依旧绕道 FilePanel::navigateTo → syncPathUI
//      → setPath → rebuildDisplay 回来，不允许 PathBar 内部自改 m_path。
//   2. 编辑模式：变成 QLineEdit，用户可以直接输入路径（Tab 补全暂未做）。
//      Enter 提交，Esc/失焦取消。
// =============================================================================

#include <QWidget>
#include <QHBoxLayout>
#include <QPushButton>
#include <QString>

class QLabel;
class QTimer;
class QLineEdit;
class QStackedLayout;
class QScrollArea;

class PathBar : public QWidget {
    Q_OBJECT

public:
    explicit PathBar(QWidget* parent = nullptr);

    /// 设置当前显示路径（单一数据源：应由拥有者从"文件视图真实 rootPath"调用）。
    void setPath(const QString& path);
    QString path() const { return m_path; }

    // 切到/退出编辑模式（也响应 MainWindow 的 Ctrl+L 快捷键）
    void enterEditMode();
    void exitEditMode();

signals:
    /// 用户点击面包屑段要求跳转到某祖先目录；或在编辑框 Enter 了一个有效路径。
    /// FilePanel 应连到 navigateTo(path) 完成 SSOT 跳转。
    void pathSelected(const QString& path);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void rebuildDisplay();                          // 根据 m_path 重建面包屑
    void clearCrumbs();                             // 移除当前面包屑按钮/分隔符
    /// 顶层无边框小气泡。
    /// message 为空时，默认显示 "✓ Path copied"（向后兼容）。
    /// 调用方可自定义文字（如 "✓ 已复制：/Users/.../foo"）。
    void showCopiedToast(const QPoint& globalPos,
                         const QString& message = QString());
    void commitEditedPath();                        // 编辑模式 Enter 处理

    QString m_path;
    QStackedLayout* m_stack = nullptr;

    // ---- 只读显示页面 ----
    QWidget* m_displayPage = nullptr;
    QHBoxLayout* m_layout = nullptr;
    // 旧的"远程 URL fallback" QLabel，现在被面包屑替代 —— 隐藏保留，未来可清理
    QLabel* m_pathLabel = nullptr;
    // 面包屑容器（内部由 rebuildDisplay 每次重建）+ 水平滚动壳
    QWidget* m_crumbHost = nullptr;
    QHBoxLayout* m_crumbLayout = nullptr;
    QScrollArea* m_crumbScroll = nullptr;      // 横向滚动壳，容长路径不撑窗
    QPushButton* m_editBtn = nullptr;    // "✎" 按钮 —— 切换到编辑模式

    // ---- 编辑页面 ----
    QWidget* m_editPage = nullptr;
    QLineEdit* m_pathEdit = nullptr;

    // ---- 临时 "Path copied" toast ----
    QWidget* m_toast = nullptr;
    QTimer* m_toastTimer = nullptr;
};

#endif // PATHBAR_H
