#ifndef REALFILESYSTEM_H
#define REALFILESYSTEM_H

// =============================================================================
// RealFileSystem —— 本地文件系统的薄封装（基于 QDir / QFile）
//
// 设计目的：
//   1. 把 Qt 文件 API 收口到一个地方，方便单元测试 / mock。
//   2. 提供"按文件计数"的进度回调版本（QProgressDialog 友好）。
//   3. 与 SftpClient 通过 FileSystemRouter 共享相同的"列目录"语义。
//
// 单例：用 Meyer's singleton（线程安全 since C++11），读不到设置文件，
//       完全无状态，仅作为命名空间使用。
//
// 注意：这里只处理本地路径。任何 sftp:// URL 应在调用方先经
//       FileSystemRouter::isRemote() 路由到 SftpClient 处理。
// =============================================================================

#include "Types.h"
#include <QVector>
#include <QString>
#include <QFile>
#include <functional>

class RealFileSystem {
public:
    /**
     * 进度回调函数签名 —— 每处理完一个文件被调用一次。
     *   currentPath : 刚刚（或正在）处理的文件绝对路径
     *   processed   : 至今为止已处理的文件总数（含 currentPath）
     *   返回值      : true=继续，false=请求中止操作
     *
     * 实现方应快速返回，不阻塞（典型实现是更新 QProgressDialog 并
     * 检查 wasCanceled()）。
     */
    using ProgressFn = std::function<bool(const QString& currentPath, int processed)>;

    /// 全局单例访问点
    static RealFileSystem& instance();

    // ----- 读取 -----
    /// 列目录（不含 . 与 ..），按"目录优先 + 名字大小写不敏感"排序
    QVector<FileEntry> listDirectory(const QString& path) const;
    bool isDirectory(const QString& path) const;
    bool exists(const QString& path) const;
    /// 仅返回子目录的绝对路径列表（DirectoryTree 用）
    QStringList getSubDirectories(const QString& path) const;
    /// 简单的递归名称匹配搜索（深度<=5，最多1000条），上层应给用户选项
    QVector<FileEntry> search(const QString& query, const QString& basePath) const;
    /// 路径规整（QDir::cleanPath 的薄封装）
    QString normalizePath(const QString& path) const;

    /**
     * 统计 path 下"叶子文件"的总数（空目录算 1）。
     * 用于 QProgressDialog::setMaximum —— 进度按文件数推进而不是按
     * 顶层条目数推进，复制大目录时进度条更平滑、更准确。
     */
    int countEntries(const QString& path) const;

    // ----- 简单文件操作（无进度、不可取消）-----
    bool copyFile(const QString& source, const QString& dest) const;
    bool moveFile(const QString& source, const QString& dest) const;
    bool deleteFile(const QString& path) const;
    bool deleteDirectory(const QString& path) const;

    // ----- 进度+可取消版本 -----
    /**
     * 复制 source 到 dest 目录下（最终目标是 dest/<basename(source)>）。
     * `progress` 可为空；返回 false 表示要么被取消、要么遇到错误。
     * `counter` 是"累计已处理文件数"的引用 —— 上层在多次调用之间
     * 共享同一个计数器，进度条才能从头到尾连续推进。
     */
    bool copyFileWithProgress(const QString& source, const QString& dest,
                              const ProgressFn& progress, int& counter) const;
    /// 递归删除目录（带进度），同上语义
    bool deleteDirectoryWithProgress(const QString& path,
                                     const ProgressFn& progress, int& counter) const;

    // ----- 全局开关 -----
    /// 是否在列目录与子目录枚举中包含"."开头的隐藏文件
    void setShowHidden(bool show) { m_showHidden = show; }
    bool showHidden() const { return m_showHidden; }

private:
    RealFileSystem() = default;
    /// 根据扩展名给出 FileType（仅用于 UI 装饰）
    FileType guessFileType(const QString& name) const;
    /// QFile::Permissions → "rwxr-xr-x" 风格字符串
    QString formatPermissions(QFile::Permissions perms) const;

    bool m_showHidden = false;
};

#endif // REALFILESYSTEM_H
