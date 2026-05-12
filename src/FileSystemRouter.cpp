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
