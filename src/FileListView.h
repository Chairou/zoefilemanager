#ifndef FILELISTVIEW_H
#define FILELISTVIEW_H

// =============================================================================
// FileListView —— 文件列表视图（QTableWidget 派生）
//
// 4 列：Name / Size / Modified / Permissions。
//
// 自定义功能：
//   1. **自带"."与"..":** 在 setEntries 时合成两条特殊行，分别代表当前目录
//      和父目录；".." 在 mount 根处不显示（避免越界到 sftp:// 上层）。
//   2. **自定义选择模式：** 关掉 QTableWidget 的内建选择（NoSelection），
//      自己按 Ctrl/Shift 在 mouseReleaseEvent 里实现行选择，避免单元格选择
//      与"行选择"语义冲突。
//   3. **拖动源：** 在已选中的行上按下 + 移动超阈值后启动 QDrag。同时附带
//      自定义 MIME（`application/x-zoe-fileentries`，支持 SFTP URL）和
//      标准 `text/uri-list`（外部 Finder 等可识别）。
//   4. **快捷键透传：** Cmd+C/X/V/A/Backspace 不让 QAbstractItemView 处理，
//      ignore 掉让 MainWindow 的全局 QShortcut 接管。
// =============================================================================

#include "Types.h"
#include <QTableWidget>
#include <QVector>
#include <QMenu>

class FileListView : public QTableWidget {
    Q_OBJECT

public:
    explicit FileListView(QWidget* parent = nullptr);

    /// 重设条目并重建表格。`currentPath` 用于合成 . / .. 行
    void setEntries(const QVector<FileEntry>& entries, const QString& currentPath);

    /// 返回当前选中的条目（不含 . / ..）
    QVector<FileEntry> getSelectedEntries() const;
    QVector<int> getSelectedIndices() const;
    void setSelectedIndices(const QVector<int>& indices);
    void selectAll() override;
    /// 按完整路径精确选中并滚动到可见。匹配不到时不动现有选择。
    /// 返回是否找到匹配项。
    bool selectByPath(const QString& path);
    const QVector<FileEntry>& entries() const { return m_entries; }

signals:
    void directoryActivated(const QString& path);  // 双击目录
    /// 双击的是已识别的压缩包（.zip / .tar.gz / .tgz / .tar.bz2 / .tbz /
    /// .tar.xz / .txz / .tar）—— 让上层在 app 内解压，而不是丢给系统打开。
    void archiveActivated(const QString& path);
    void selectionChanged_();                       // 后缀 _ 避免和基类同名
    void contextMenuRequested(const QPoint& pos);

protected:
    // 以下四个方法共同实现：自定义选择 + 拖动源
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onCellDoubleClicked(int row, int column);
    void onCustomContextMenu(const QPoint& pos);
    void onHeaderClicked(int logicalIndex);   // 表头点击 → 切换排序
    void retranslate();   // 语言切换时刷新表头文字

private:
    // 4 列的逻辑下标
    enum SortColumn { SortByName = 0, SortBySize = 1, SortByModified = 2, SortByPermissions = 3 };

    void setupColumns();
    QString formatSize(qint64 size) const;            // 1024 → "1.0 KB"
    QString getIconText(const FileEntry& entry) const;// FileType 转 emoji 前缀
    void updateRowSelection(int row, bool selected);
    void sortEntries(QVector<FileEntry>& entries) const;
    /// 用 m_rawEntries 重建可视行（拼上 . / ..，应用当前排序）
    void rebuildRows();

    QVector<FileEntry> m_entries;     // 包含 . 与 ..（界面上看到的全部）
    QVector<FileEntry> m_rawEntries;  // 不含 . 与 ..（重排序时直接复用）
    QString m_currentPath;
    int m_lastClickedRow = -1;        // Shift 范围选择的锚点

    int m_sortColumn = SortByName;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    // ---- 拖动源状态 ----
    // 按下时记录起点 + 当前 press 是否落在一个"可拖"的真实条目上。
    // 移动距离超过 QApplication::startDragDistance() 才启动 QDrag —— 防止
    // 单击就误触发拖动，破坏正常的"点一下选中"。
    //
    // 注意：本字段在 press 落在 *任何* 真实条目（非 . / ..）上时都会置位，
    // 不要求该行已被选中（v2 行为）。如果按下时该行未选中，press 处理里
    // 会先把它单独选上作为拖拽载荷。
    QPoint m_dragStartPos;
    bool   m_pressOnEntry = false;
};

#endif // FILELISTVIEW_H
