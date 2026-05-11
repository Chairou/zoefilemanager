#ifndef FILESYSTEMROUTER_H
#define FILESYSTEMROUTER_H

// =============================================================================
// FileSystemRouter —— 文件系统"路径前缀"路由器
//
// 角色：让 FilePanel / DirectoryTree 等 UI 层只关心路径字符串，路径背后是
//       本地磁盘还是 SFTP 远端，由这里透明决定。
//
// 路由规则（纯字符串前缀匹配）：
//   - 路径 == 某个已 mount 前缀，或以 "<mountPrefix>/" 开头 → 走 SFTP 后端
//   - 其它 → 走 RealFileSystem（本地）
//
// 典型 mountPrefix："sftp://user@host:22"；UI 层显示给用户的远程路径形如
// "sftp://user@host:22/pub/example"。
//
// 生命周期：mount() 接管 SftpClient 所有权，unmount()/clear() 释放并清理
// 会话。析构时为了规避静态析构顺序陷阱（libcrypto 可能已被 dyld 卸载），
// 不再走 SftpClient::disconnect 的完整清理路径，仅设置全局 shutdown 标志
// 让 SftpClient 析构降级为 close socket（详见 SftpClient::markShuttingDown）。
//
// 线程：当前所有调用都在 GUI 主线程；本类不做加锁（如果未来要后台线程
// 列目录，需把 m_mounts 加锁或换实现）。
// =============================================================================

#include "Types.h"
#include <QString>
#include <QVector>
#include <QStringList>
#include <memory>
#include <string>
#include <unordered_map>

class SftpClient;

class FileSystemRouter {
public:
    /// 全局单例（Meyer's singleton）
    static FileSystemRouter& instance();

    /**
     * 把已认证好的 SFTP 会话挂到给定前缀下。
     *
     * @param mountPrefix 例如 "sftp://demo@test.rebex.net:22"，不带末尾 '/'
     * @param client      已 connectAndAuth 成功的 SftpClient（接管所有权）
     *
     * 若同前缀已有挂载，会被替换（旧会话析构）。
     */
    void mount(const QString& mountPrefix, std::unique_ptr<SftpClient> client);

    /// 拆除指定前缀的挂载（未知前缀也安全）；触发 SftpClient 析构 →
    /// libssh2_session_disconnect + free。
    void unmount(const QString& mountPrefix);

    /**
     * 一次性拆除所有挂载。**必须**在应用退出路径上主动调用
     * （MainWindow 析构 / QCoreApplication::aboutToQuit），那时 libssh2 和
     * libcrypto 还活着。否则等到静态析构时 OpenSSL 的 RNG 锁可能已被
     * cleanup，libssh2_session_free 内部 RAND_bytes_ex 会 SIGSEGV。
     */
    void clear();

    /// 路径是否落在某个已挂载前缀下（即"是否远程"）
    bool isRemote(const QString& path) const;

    /// 找出 path 命中的 mount 前缀；本地路径返回空串
    QString mountFor(const QString& path) const;

    // ----- 与 RealFileSystem 等价的对外 API（只覆盖 UI 用到的子集）-----
    QVector<FileEntry> listDirectory(const QString& path);
    QStringList        getSubDirectories(const QString& path);
    bool               isDirectory(const QString& path);
    bool               exists(const QString& path);
    QString            normalizePath(const QString& path) const;

    /// 把"显示隐藏文件"开关广播到本地与所有远端后端
    void setShowHidden(bool show);

private:
    FileSystemRouter() = default;
    ~FileSystemRouter();

    /// 路由解析结果。client==nullptr 表示路径走本地；否则 remotePath 是
    /// 把 mount 前缀从原 path 中剥掉后的"服务器侧绝对路径"。
    struct Resolved { SftpClient* client; QString remotePath; QString mount; };
    Resolved resolve(const QString& path) const;

    // 用 std::unordered_map 而不是 QHash —— QHash 的 COW 实现要求 value 可拷贝，
    // 但 std::unique_ptr<SftpClient> 不可拷贝，会导致编译错误。
    std::unordered_map<std::string, std::unique_ptr<SftpClient>> m_mounts;
};

#endif // FILESYSTEMROUTER_H
