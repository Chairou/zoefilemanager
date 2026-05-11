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

FileSystemRouter& FileSystemRouter::instance() {
    static FileSystemRouter inst;
    return inst;
}

// ---------------------------------------------------------------------------
// 析构 —— 只在进程退出时被静态终结器调用。
//
// 关键：进入这里时 libssh2 / libcrypto 可能已经被 dyld 卸载了
// （C++ 静态析构顺序 vs dylib 卸载顺序在 macOS 上不保证）。
// 此时调用 libssh2_session_free / libssh2_sftp_shutdown 会陷入 OpenSSL，
// 触发 pthread_rwlock_rdlock(NULL) → SIGSEGV（曾在崩溃报告中实际发生）。
//
// 所以这里只翻一个全局 shutdown 标志，让 SftpClient 析构降级为
// 仅 close socket，不再调任何 libssh2 API。完整的"优雅再见"必须由
// 应用调用 clear() 主动完成（在 Qt 还活着的时候）。
// ---------------------------------------------------------------------------
FileSystemRouter::~FileSystemRouter() {
    SftpClient::markShuttingDown();
    m_mounts.clear();
}

// 安全的拆挂载路径：MainWindow 析构 / aboutToQuit 调用，此时
// libssh2/libcrypto 还活着，unique_ptr 析构链会走完整流程。
void FileSystemRouter::clear() {
    m_mounts.clear();
}

void FileSystemRouter::mount(const QString& mountPrefix,
                             std::unique_ptr<SftpClient> client) {
    // 同前缀已挂会被替换 —— 老的 unique_ptr 立刻析构，干净拆掉旧会话
    m_mounts[mountPrefix.toStdString()] = std::move(client);
}

void FileSystemRouter::unmount(const QString& mountPrefix) {
    m_mounts.erase(mountPrefix.toStdString());
}

bool FileSystemRouter::isRemote(const QString& path) const {
    return resolve(path).client != nullptr;
}

QString FileSystemRouter::mountFor(const QString& path) const {
    return resolve(path).mount;
}

// ---------------------------------------------------------------------------
// 路径解析 —— 找出最长匹配的挂载前缀，并把"服务器侧绝对路径"剥出来。
//
// 输入示例 / 输出示例：
//   "/Users/foo"                      → {nullptr, "/Users/foo", ""}
//   "sftp://demo@h:22/"               → {client*, "/",          "sftp://demo@h:22"}
//   "sftp://demo@h:22/pub/example"    → {client*, "/pub/example", "sftp://demo@h:22"}
// ---------------------------------------------------------------------------
FileSystemRouter::Resolved FileSystemRouter::resolve(const QString& path) const {
    // 最长前缀匹配；挂载数量极小（实际就 0 或 1 个），线性扫描没问题。
    QString      bestMount;
    SftpClient*  bestClient = nullptr;
    for (auto it = m_mounts.begin(); it != m_mounts.end(); ++it) {
        const QString prefix = QString::fromStdString(it->first);
        if (path == prefix || path.startsWith(prefix + "/")) {
            if (prefix.size() > bestMount.size()) {
                bestMount  = prefix;
                bestClient = it->second.get();
            }
        }
    }
    if (!bestClient) return {nullptr, path, QString()};

    // 剥前缀，确保 remote 至少是 "/"
    QString remote = path.mid(bestMount.size());
    if (remote.isEmpty()) remote = "/";
    if (!remote.startsWith('/')) remote.prepend('/');
    return {bestClient, remote, bestMount};
}

// ---------------------------------------------------------------------------
// 列目录 —— 远程后端返回的 FileEntry 里 path 是"服务器侧绝对路径"，
// 这里再加回 mount 前缀，让 UI 看到的路径形式始终是一致的"sftp://...全路径"。
// ---------------------------------------------------------------------------
QVector<FileEntry> FileSystemRouter::listDirectory(const QString& path) {
    Resolved r = resolve(path);
    if (r.client) {
        auto entries = r.client->listDirectory(r.remotePath);
        for (auto& e : entries) {
            e.path = r.mount + e.path;     // 还原成 "sftp://user@host:22/foo"
        }
        return entries;
    }
    return RealFileSystem::instance().listDirectory(path);
}

QStringList FileSystemRouter::getSubDirectories(const QString& path) {
    Resolved r = resolve(path);
    if (r.client) {
        QStringList remoteDirs = r.client->listSubdirectories(r.remotePath);
        QStringList out;
        out.reserve(remoteDirs.size());
        for (const auto& d : remoteDirs) out.append(r.mount + d);
        return out;
    }
    return RealFileSystem::instance().getSubDirectories(path);
}

bool FileSystemRouter::isDirectory(const QString& path) {
    Resolved r = resolve(path);
    if (r.client) return r.client->isDirectory(r.remotePath);
    return RealFileSystem::instance().isDirectory(path);
}

bool FileSystemRouter::exists(const QString& path) {
    Resolved r = resolve(path);
    if (r.client) return r.client->exists(r.remotePath);
    return RealFileSystem::instance().exists(path);
}

// 路径规整：本地用 QDir::cleanPath；远程则只折叠重复的 "//"，
// 不能用 QDir::cleanPath 因为它会把 "sftp://..." 折成 "sftp:/..."（破坏 URL）。
QString FileSystemRouter::normalizePath(const QString& path) const {
    Resolved r = resolve(path);
    if (r.client) {
        QString remote = r.remotePath;
        while (remote.contains("//")) remote.replace("//", "/");
        if (remote.isEmpty()) remote = "/";
        return r.mount + remote;
    }
    return RealFileSystem::instance().normalizePath(path);
}

// 隐藏文件开关广播到所有后端，UI 只需调用一次本方法
void FileSystemRouter::setShowHidden(bool show) {
    RealFileSystem::instance().setShowHidden(show);
    for (auto it = m_mounts.begin(); it != m_mounts.end(); ++it) {
        it->second->setShowHidden(show);
    }
}
