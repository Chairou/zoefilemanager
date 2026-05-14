// =============================================================================
// FileSystemRouter.cpp —— 见 FileSystemRouter.h
//
// 实现细节：
//   - 内部 map 用 std::string 而非 QString —— std::unordered_map 对 QString
//     缺乏 std::hash 特化，简单起见统一用 std::string；entry 不多，转换成本可忽略。
//   - resolve() 走"最长前缀匹配"，理论上支持嵌套挂载；实际上同时只会有一个
//     SFTP 连接，但代码保留扩展空间。
// =============================================================================

#include "FileSystemRouter.h"
#include "RealFileSystem.h"
#include "SftpClient.h"
#include "SmbClient.h"
#include <QFile>
#include <QFileInfo>

FileSystemRouter& FileSystemRouter::instance() {
    static FileSystemRouter inst;
    return inst;
}

FileSystemRouter::~FileSystemRouter() {
    SftpClient::markShuttingDown();
    m_mounts.clear();
    m_smbMounts.clear();
}

void FileSystemRouter::clear() {
    m_mounts.clear();
    m_smbMounts.clear();
}

void FileSystemRouter::mount(const QString& mountPrefix,
                             std::unique_ptr<SftpClient> client) {
    m_mounts[mountPrefix.toStdString()] = std::move(client);
}

void FileSystemRouter::mountSmb(const QString& mountPrefix,
                                std::unique_ptr<SmbClient> client) {
    m_smbMounts[mountPrefix.toStdString()] = std::move(client);
}

void FileSystemRouter::unmount(const QString& mountPrefix) {
    m_mounts.erase(mountPrefix.toStdString());
    m_smbMounts.erase(mountPrefix.toStdString());
}

bool FileSystemRouter::isRemote(const QString& path) const {
    Resolved r = resolve(path);
    return r.sftp != nullptr || r.smb != nullptr;
}

QString FileSystemRouter::mountFor(const QString& path) const {
    return resolve(path).mount;
}

// ---------------------------------------------------------------------------
// 路径解析 —— SFTP 挂载剥前缀后是服务器侧路径；
// SMB 挂载保留完整 URL，因为 libsmbclient API 直接吃 "smb://host/share/..."。
// ---------------------------------------------------------------------------
FileSystemRouter::Resolved FileSystemRouter::resolve(const QString& path) const {
    QString      bestMount;
    SftpClient*  bestSftp  = nullptr;
    SmbClient*   bestSmb   = nullptr;

    for (auto it = m_mounts.begin(); it != m_mounts.end(); ++it) {
        const QString prefix = QString::fromStdString(it->first);
        if (path == prefix || path.startsWith(prefix + "/")) {
            if (prefix.size() > bestMount.size()) {
                bestMount = prefix;
                bestSftp  = it->second.get();
                bestSmb   = nullptr;
            }
        }
    }
    for (auto it = m_smbMounts.begin(); it != m_smbMounts.end(); ++it) {
        const QString prefix = QString::fromStdString(it->first);
        if (path == prefix || path.startsWith(prefix + "/")) {
            if (prefix.size() > bestMount.size()) {
                bestMount = prefix;
                bestSmb   = it->second.get();
                bestSftp  = nullptr;
            }
        }
    }

    if (!bestSftp && !bestSmb) return {nullptr, nullptr, path, QString()};

    if (bestSftp) {
        QString remote = path.mid(bestMount.size());
        if (remote.isEmpty()) remote = "/";
        if (!remote.startsWith('/')) remote.prepend('/');
        return {bestSftp, nullptr, remote, bestMount};
    }
    // SMB：保留完整 URL 作为 remotePath
    return {nullptr, bestSmb, path, bestMount};
}

// ---------------------------------------------------------------------------
// 列目录
// ---------------------------------------------------------------------------
QVector<FileEntry> FileSystemRouter::listDirectory(const QString& path) {
    Resolved r = resolve(path);
    if (r.sftp) {
        auto entries = r.sftp->listDirectory(r.remotePath);
        for (auto& e : entries) {
            e.path = r.mount + e.path;
        }
        return entries;
    }
    if (r.smb) {
        // SMB 的 listDirectory 已返回完整 smb:// URL，无需拼接
        return r.smb->listDirectory(r.remotePath);
    }
    return RealFileSystem::instance().listDirectory(path);
}

QStringList FileSystemRouter::getSubDirectories(const QString& path) {
    Resolved r = resolve(path);
    if (r.sftp) {
        QStringList remoteDirs = r.sftp->listSubdirectories(r.remotePath);
        QStringList out;
        out.reserve(remoteDirs.size());
        for (const auto& d : remoteDirs) out.append(r.mount + d);
        return out;
    }
    if (r.smb) {
        // 简单做法：调 listDirectory 然后过滤目录
        auto entries = r.smb->listDirectory(r.remotePath);
        QStringList out;
        for (const auto& e : entries) {
            if (e.isDirectory) out.append(e.path);
        }
        return out;
    }
    return RealFileSystem::instance().getSubDirectories(path);
}

bool FileSystemRouter::isDirectory(const QString& path) {
    Resolved r = resolve(path);
    if (r.sftp) return r.sftp->isDirectory(r.remotePath);
    if (r.smb) {
        // 通过父目录 listDirectory + 名称匹配判断（避免给 SmbClient 加新方法）
        int slash = path.lastIndexOf('/');
        if (slash < 0) return false;
        QString parent = path.left(slash);
        QString name   = path.mid(slash + 1);
        auto entries = r.smb->listDirectory(parent);
        for (const auto& e : entries) {
            if (e.name == name) return e.isDirectory;
        }
        return false;
    }
    return RealFileSystem::instance().isDirectory(path);
}

bool FileSystemRouter::exists(const QString& path) {
    Resolved r = resolve(path);
    if (r.sftp) return r.sftp->exists(r.remotePath);
    if (r.smb) {
        // 同 isDirectory 思路
        if (path == r.mount) return true;   // 挂载根本身视为存在
        int slash = path.lastIndexOf('/');
        if (slash < 0) return false;
        QString parent = path.left(slash);
        QString name   = path.mid(slash + 1);
        auto entries = r.smb->listDirectory(parent);
        for (const auto& e : entries) {
            if (e.name == name) return true;
        }
        return false;
    }
    return RealFileSystem::instance().exists(path);
}

QString FileSystemRouter::normalizePath(const QString& path) const {
    Resolved r = resolve(path);
    if (r.sftp) {
        QString remote = r.remotePath;
        while (remote.contains("//")) remote.replace("//", "/");
        if (remote.isEmpty()) remote = "/";
        return r.mount + remote;
    }
    if (r.smb) {
        // SMB 路径不能破坏 smb:// 这种双斜杠头
        QString rest = path.mid(QStringLiteral("smb://").size());
        while (rest.contains("//")) rest.replace("//", "/");
        return "smb://" + rest;
    }
    return RealFileSystem::instance().normalizePath(path);
}

void FileSystemRouter::setShowHidden(bool show) {
    RealFileSystem::instance().setShowHidden(show);
    for (auto it = m_mounts.begin(); it != m_mounts.end(); ++it) {
        it->second->setShowHidden(show);
    }
    // SmbClient 暂未提供 setShowHidden（隐藏文件在 SMB 里少见）；预留扩展。
}

// ---------------------------------------------------------------------------
// 文件操作派发
// ---------------------------------------------------------------------------
bool FileSystemRouter::sameMount(const QString& a, const QString& b) const {
    return resolve(a).mount == resolve(b).mount;
}

bool FileSystemRouter::removePath(const QString& path) {
    Resolved r = resolve(path);
    if (r.sftp) {
        bool ok = r.sftp->removeRecursive(r.remotePath);
        if (!ok) m_lastError = r.sftp->lastError();
        return ok;
    }
    if (r.smb) {
        m_lastError = QStringLiteral("SMB delete is not implemented yet");
        return false;
    }
    // 本地：用 RealFileSystem 的递归删
    int dummy = 0;
    bool ok = RealFileSystem::instance().deleteDirectoryWithProgress(path, nullptr, dummy);
    if (!ok) m_lastError = QStringLiteral("local delete failed: ") + path;
    return ok;
}

bool FileSystemRouter::renamePath(const QString& oldPath, const QString& newPath) {
    if (!sameMount(oldPath, newPath)) {
        m_lastError = QStringLiteral("rename across different mounts is not supported");
        return false;
    }
    Resolved ro = resolve(oldPath);
    Resolved rn = resolve(newPath);
    if (ro.sftp) {
        bool ok = ro.sftp->renamePath(ro.remotePath, rn.remotePath);
        if (!ok) m_lastError = ro.sftp->lastError();
        return ok;
    }
    if (ro.smb) {
        m_lastError = QStringLiteral("SMB rename is not implemented yet");
        return false;
    }
    // 本地：先尝试 QFile::rename（同设备 O(1)）；失败留给上层做 copy+delete 的回退
    if (QFile::rename(oldPath, newPath)) return true;
    m_lastError = QStringLiteral("local rename failed: ") + oldPath;
    return false;
}

bool FileSystemRouter::copyPath(const QString& srcPath, const QString& dstPath) {
    if (!sameMount(srcPath, dstPath)) {
        m_lastError = QStringLiteral("copy across different mounts is not supported");
        return false;
    }
    Resolved rs = resolve(srcPath);
    Resolved rd = resolve(dstPath);
    if (rs.sftp) {
        bool ok = rs.sftp->copyRecursive(rs.remotePath, rd.remotePath);
        if (!ok) m_lastError = rs.sftp->lastError();
        return ok;
    }
    if (rs.smb) {
        m_lastError = QStringLiteral("SMB copy is not implemented yet");
        return false;
    }
    // 本地：用 RealFileSystem（不带进度回调）
    int dummy = 0;
    QString destDir = QFileInfo(dstPath).absolutePath();
    bool ok = RealFileSystem::instance().copyFileWithProgress(srcPath, destDir, nullptr, dummy);
    if (!ok) m_lastError = QStringLiteral("local copy failed: ") + srcPath;
    return ok;
}

// ---------------------------------------------------------------------------
// 跨边界传输（local ↔ sftp）。SMB 暂不支持。
// ---------------------------------------------------------------------------
bool FileSystemRouter::transferAcross(const QString& srcPath,
                                      const QString& dstPath,
                                      const SftpClient::TransferProgressFn& progress,
                                      qint64 bytesTotal,
                                      qint64* bytesDone) {
    Resolved rs = resolve(srcPath);
    Resolved rd = resolve(dstPath);

    // SMB 任一侧：暂不支持
    if (rs.smb || rd.smb) {
        m_lastError = QStringLiteral("SMB transfer is not implemented yet");
        return false;
    }

    const bool srcRemote = (rs.sftp != nullptr);
    const bool dstRemote = (rd.sftp != nullptr);

    if (srcRemote && dstRemote) {
        m_lastError = QStringLiteral("transferAcross: both sides remote — use copyPath instead");
        return false;
    }
    if (!srcRemote && !dstRemote) {
        m_lastError = QStringLiteral("transferAcross: both sides local — use RealFileSystem instead");
        return false;
    }

    if (!srcRemote && dstRemote) {
        // 上传：local → sftp
        bool ok = rd.sftp->uploadRecursive(srcPath, rd.remotePath,
                                           progress, bytesTotal, bytesDone);
        if (!ok) m_lastError = rd.sftp->lastError();
        return ok;
    } else {
        // 下载：sftp → local
        bool ok = rs.sftp->downloadRecursive(rs.remotePath, dstPath,
                                             progress, bytesTotal, bytesDone);
        if (!ok) m_lastError = rs.sftp->lastError();
        return ok;
    }
}

qint64 FileSystemRouter::totalBytes(const QString& path) {
    Resolved r = resolve(path);
    if (r.sftp) return r.sftp->remoteTotalBytes(r.remotePath);
    if (r.smb)  return 0;     // SMB 暂未实现
    return SftpClient::localTotalBytes(path);
}
