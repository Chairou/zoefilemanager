// =============================================================================
// RealFileSystem.cpp —— 见 RealFileSystem.h 的总体说明
//
// 本文件实现"本地文件系统访问"的薄封装。所有路径都假定是本地绝对路径；
// SFTP URL 不应到达这里（FileSystemRouter 会先路由）。
// =============================================================================

#include "RealFileSystem.h"
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QFile>

// ----- Meyer's singleton（C++11 起线程安全的局部静态对象）-----
RealFileSystem& RealFileSystem::instance() {
    static RealFileSystem inst;
    return inst;
}

// ---------------------------------------------------------------------------
// 列目录：返回目录里的所有条目（不含 . 与 ..）
//   - 排序：目录在前，按文件名大小写不敏感
//   - 隐藏文件：默认不显示，由 m_showHidden 控制
//   - 目录大小：约定填 4096（仅作 UI 显示，不代表真实块大小）
// ---------------------------------------------------------------------------
QVector<FileEntry> RealFileSystem::listDirectory(const QString& path) const {
    QVector<FileEntry> entries;
    QString normalized = normalizePath(path);

    QDir dir(normalized);
    if (!dir.exists()) {
        return entries;
    }

    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (m_showHidden) {
        filters |= QDir::Hidden;
    }
    dir.setFilter(filters);
    dir.setSorting(QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    QFileInfoList infoList = dir.entryInfoList();
    for (const QFileInfo& info : infoList) {
        // 双保险：即便 filter 没拦住（不同平台 QDir::Hidden 行为略有差异），
        // 这里再过一次 dot-files。
        if (!m_showHidden && info.fileName().startsWith('.')) continue;

        FileEntry entry;
        entry.name = info.fileName();
        entry.path = info.absoluteFilePath();
        entry.isDirectory = info.isDir();
        entry.size = info.isDir() ? 4096 : info.size();
        entry.modified = info.lastModified();
        entry.permissions = formatPermissions(info.permissions());
        entry.type = info.isDir() ? FileType::Directory : guessFileType(info.fileName());
        entries.append(entry);
    }

    return entries;
}

bool RealFileSystem::isDirectory(const QString& path) const {
    return QFileInfo(path).isDir();
}

bool RealFileSystem::exists(const QString& path) const {
    return QFileInfo::exists(path);
}

// ---------------------------------------------------------------------------
// 仅枚举子目录（DirectoryTree 懒加载用）。
// 与 listDirectory 区别：这里只返回目录的绝对路径字符串列表，不构造
// FileEntry，少做了一次结构体填充，少分配。
// ---------------------------------------------------------------------------
QStringList RealFileSystem::getSubDirectories(const QString& path) const {
    QStringList dirs;
    QString normalized = normalizePath(path);

    QDir dir(normalized);
    if (!dir.exists()) {
        return dirs;
    }

    QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot;
    if (m_showHidden) {
        filters |= QDir::Hidden;
    }
    dir.setFilter(filters);
    dir.setSorting(QDir::Name | QDir::IgnoreCase);

    QFileInfoList infoList = dir.entryInfoList();
    for (const QFileInfo& info : infoList) {
        if (!m_showHidden && info.fileName().startsWith('.')) continue;
        dirs.append(info.absoluteFilePath());
    }

    return dirs;
}

// ---------------------------------------------------------------------------
// 简单递归搜索：基于文件名子串匹配（大小写不敏感）。
// 限制：深度 <= 5 层、最多 1000 条命中 —— 防止符号链接环 / 大目录卡死。
// 真正生产级搜索应该用 mdfind / locate / 自家索引，这只是 SearchDialog 的兜底。
// ---------------------------------------------------------------------------
QVector<FileEntry> RealFileSystem::search(const QString& query, const QString& basePath) const {
    QVector<FileEntry> results;
    QString normalized = normalizePath(basePath);

    if (!QDir(normalized).exists()) {
        return results;
    }

    QDirIterator it(normalized, QDir::AllEntries | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    int count = 0;
    int depth = 0;

    while (it.hasNext() && count < 1000) {
        it.next();
        QFileInfo info = it.fileInfo();

        // 通过相对路径里 '/' 的数量推断深度，避免符号链接造成的无限递归
        QString relativePath = info.absoluteFilePath().mid(normalized.length());
        depth = relativePath.count('/');
        if (depth > 5) continue;

        if (info.fileName().contains(query, Qt::CaseInsensitive)) {
            FileEntry entry;
            entry.name = info.fileName();
            entry.path = info.absoluteFilePath();
            entry.isDirectory = info.isDir();
            entry.size = info.isDir() ? 4096 : info.size();
            entry.modified = info.lastModified();
            entry.permissions = formatPermissions(info.permissions());
            entry.type = info.isDir() ? FileType::Directory : guessFileType(info.fileName());
            results.append(entry);
            count++;
        }
    }

    return results;
}

QString RealFileSystem::normalizePath(const QString& path) const {
    if (path.isEmpty()) return "/";
    return QDir::cleanPath(path);
}

// ---------------------------------------------------------------------------
// 简单复制（不带进度，不可取消）—— 仅用于内部辅助 / 兼容老调用点。
// 上层 UI 复制走 copyFileWithProgress 才能给用户进度反馈。
// ---------------------------------------------------------------------------
bool RealFileSystem::copyFile(const QString& source, const QString& dest) const {
    QFileInfo sourceInfo(source);
    if (sourceInfo.isDir()) {
        // 递归拷贝目录：先在 dest 下创建同名目录，再逐项复制
        QDir sourceDir(source);
        QString destDirPath = dest + "/" + sourceInfo.fileName();
        QDir destDir(destDirPath);
        if (!destDir.exists()) {
            QDir().mkpath(destDirPath);
        }

        QFileInfoList entries = sourceDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
        for (const QFileInfo& entry : entries) {
            if (entry.isDir()) {
                if (!copyFile(entry.absoluteFilePath(), destDirPath)) {
                    return false;
                }
            } else {
                QString destFile = destDirPath + "/" + entry.fileName();
                if (!QFile::copy(entry.absoluteFilePath(), destFile)) {
                    return false;
                }
            }
        }
        return true;
    } else {
        QString destPath = dest + "/" + sourceInfo.fileName();
        return QFile::copy(source, destPath);
    }
}

bool RealFileSystem::moveFile(const QString& source, const QString& dest) const {
    // QFile::rename 在同设备上是 O(1)（仅改 inode 链），跨设备会失败 —— 上层
    // 应在失败时退化为 copy + delete（onPaste 走的就是这个分支）。
    QFileInfo sourceInfo(source);
    QString destPath = dest + "/" + sourceInfo.fileName();
    return QFile::rename(source, destPath);
}

bool RealFileSystem::deleteFile(const QString& path) const {
    return QFile::remove(path);
}

bool RealFileSystem::deleteDirectory(const QString& path) const {
    return QDir(path).removeRecursively();
}

// 文件类型启发式判断 —— 仅用于决定 UI 图标，不参与权限/操作判断
FileType RealFileSystem::guessFileType(const QString& name) const {
    QString ext = QFileInfo(name).suffix().toLower();
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "pptx" || ext == "xls" || ext == "xlsx")
        return FileType::Document;
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "svg" || ext == "bmp" || ext == "ico")
        return FileType::Image;
    if (ext == "html" || ext == "css" || ext == "js" || ext == "ts" || ext == "py" || ext == "cpp" || ext == "h" || ext == "java")
        return FileType::Code;
    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "rar" || ext == "7z" || ext == "dmg")
        return FileType::Archive;
    if (ext == "mp3" || ext == "flac" || ext == "wav" || ext == "ogg" || ext == "aac")
        return FileType::Audio;
    if (ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov" || ext == "wmv")
        return FileType::Video;
    if (ext == "txt" || ext == "md" || ext == "csv" || ext == "log")
        return FileType::Text;
    if (ext == "conf" || ext == "cfg" || ext == "ini" || ext == "yaml" || ext == "yml" || ext == "json")
        return FileType::Config;
    if (ext == "sh" || ext == "bash" || ext == "zsh")
        return FileType::Script;
    return FileType::Unknown;
}

// QFile::Permissions 9 个 bit 翻译为 "rwxr-xr-x" 风格字符串
QString RealFileSystem::formatPermissions(QFile::Permissions perms) const {
    QString result;
    result += (perms & QFile::ReadOwner)  ? 'r' : '-';
    result += (perms & QFile::WriteOwner) ? 'w' : '-';
    result += (perms & QFile::ExeOwner)   ? 'x' : '-';
    result += (perms & QFile::ReadGroup)  ? 'r' : '-';
    result += (perms & QFile::WriteGroup) ? 'w' : '-';
    result += (perms & QFile::ExeGroup)   ? 'x' : '-';
    result += (perms & QFile::ReadOther)  ? 'r' : '-';
    result += (perms & QFile::WriteOther) ? 'w' : '-';
    result += (perms & QFile::ExeOther)   ? 'x' : '-';
    return result;
}

// ---------------------------------------------------------------------------
// 递归统计叶子文件数 —— 用于设置 QProgressDialog 的 maximum。
// 约定：空目录算 1 个工作单元（mkdir 也是要花时间的）。
// 文件/符号链接也是 1 个单元，目录不直接计数（只递归其子项）。
// ---------------------------------------------------------------------------
int RealFileSystem::countEntries(const QString& path) const {
    QFileInfo info(path);
    if (!info.exists()) return 0;
    if (info.isFile() || info.isSymLink()) return 1;

    int count = 0;
    QDir dir(path);
    QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    if (entries.isEmpty()) return 1;  // 空目录算 1 个工作单元
    for (const QFileInfo& e : entries) {
        if (e.isDir() && !e.isSymLink()) {
            count += countEntries(e.absoluteFilePath());
        } else {
            count += 1;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// 带进度+可取消的复制
//   - source 可以是文件、目录、或符号链接
//   - dest 是目标"父目录"，最终拷贝到 dest/<basename(source)>
//   - 每完成一个叶子文件 counter++，调用 progress(...) 让上层更新 UI
//   - progress 返回 false 表示用户点了取消，立即返回 false 中止
//
// 设计上不一次性收集所有文件再循环：递归即时复制 + 即时计数，省内存，
// 大目录也不会"列完才开始动"。
// ---------------------------------------------------------------------------
bool RealFileSystem::copyFileWithProgress(const QString& source, const QString& dest,
                                          const ProgressFn& progress, int& counter) const {
    // 进入每一项前先让 UI 喘一口气，并检查是否被取消
    if (progress && !progress(source, counter)) return false;

    QFileInfo sourceInfo(source);
    if (!sourceInfo.exists()) return false;

    if (sourceInfo.isDir() && !sourceInfo.isSymLink()) {
        // 目录：先确保目标父目录下有同名子目录
        QString destDirPath = dest + "/" + sourceInfo.fileName();
        QDir destDir(destDirPath);
        if (!destDir.exists() && !QDir().mkpath(destDirPath)) {
            return false;
        }

        QDir sourceDir(source);
        QFileInfoList entries = sourceDir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);

        if (entries.isEmpty()) {
            // 空目录：自身也算 1 个工作单元，让进度条不会因为大量空目录"卡住"
            counter++;
            if (progress && !progress(source, counter)) return false;
            return true;
        }

        for (const QFileInfo& entry : entries) {
            if (entry.isDir() && !entry.isSymLink()) {
                if (!copyFileWithProgress(entry.absoluteFilePath(), destDirPath,
                                          progress, counter)) {
                    return false;  // 取消或失败
                }
            } else {
                QString destFile = destDirPath + "/" + entry.fileName();
                if (!QFile::copy(entry.absoluteFilePath(), destFile)) {
                    return false;
                }
                counter++;
                if (progress && !progress(entry.absoluteFilePath(), counter)) {
                    return false;
                }
            }
        }
        return true;
    } else {
        // 单文件（或符号链接，按文件复制）
        QString destPath = dest + "/" + sourceInfo.fileName();
        if (!QFile::copy(source, destPath)) return false;
        counter++;
        if (progress && !progress(source, counter)) return false;
        return true;
    }
}

// ---------------------------------------------------------------------------
// 带进度的递归删除 —— 自底向上。
// 顺序保证：必须先删完所有子项，再删空目录本身（POSIX rmdir 要求空）。
// ---------------------------------------------------------------------------
bool RealFileSystem::deleteDirectoryWithProgress(const QString& path,
                                                 const ProgressFn& progress,
                                                 int& counter) const {
    QFileInfo info(path);
    if (!info.exists()) return true;

    if (progress && !progress(path, counter)) return false;

    if (info.isFile() || info.isSymLink()) {
        bool ok = QFile::remove(path);
        if (ok) {
            counter++;
            if (progress && !progress(path, counter)) return false;
        }
        return ok;
    }

    // 目录：先递归删内容
    QDir dir(path);
    QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);

    if (entries.isEmpty()) {
        // 空目录直接删
        bool ok = QDir().rmdir(path);
        if (ok) {
            counter++;
            if (progress && !progress(path, counter)) return false;
        }
        return ok;
    }

    for (const QFileInfo& e : entries) {
        if (e.isDir() && !e.isSymLink()) {
            if (!deleteDirectoryWithProgress(e.absoluteFilePath(), progress, counter)) {
                return false;
            }
        } else {
            if (!QFile::remove(e.absoluteFilePath())) return false;
            counter++;
            if (progress && !progress(e.absoluteFilePath(), counter)) return false;
        }
    }

    // 内容删完了再删自己 —— 这里 *不* 累加 counter，因为上面的"空目录"分支
    // 已经为这一情形负责计数；走到这里说明此目录原来非空，子项已经计过。
    return QDir().rmdir(path);
}
