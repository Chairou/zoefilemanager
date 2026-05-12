#ifndef SMBCLIENT_H
#define SMBCLIENT_H

// =============================================================================
// SmbClient —— 对 libsmbclient 的轻量封装
//
// 职责范围（MVP）：
//   - 初始化 SMB 上下文（smbc_new_context + smbc_init_context）
//   - 用回调方式提供 user/workgroup/password（而不是全局 smbc_setUsername）
//   - 建立"连接"：其实 libsmbclient 是按需连接，没有 connectAndAuth 等价 API；
//     我们在 connectAndAuth() 里试着 opendir 根 share 当作验证
//   - 目录浏览、stat、读文件（后续可扩展写/删/建目录）
//
// 线程模型：GUI 线程阻塞调用，和 SftpClient 一致。
// =============================================================================

#include "Types.h"
#include <QString>
#include <QVector>
#include <memory>

struct _SMBCCTX;
typedef struct _SMBCCTX SMBCCTX;

class SmbClient {
public:
    SmbClient();
    ~SmbClient();

    /// 初始化 + 鉴权验证。成功返回 true。
    /// 实际做法：用 conn 参数构造 smb://host/share URL，opendir 看看能否打开。
    bool connectAndAuth(const RemoteConnection& conn);

    /// 已连接后：列出某个 smb:// URL 下的条目。path 形如
    /// "smb://host/share" 或 "smb://host/share/subdir"。
    QVector<FileEntry> listDirectory(const QString& url);

    /// 断开并释放上下文。析构时会自动调，显式 disconnect 可用于"手工清掉"。
    void disconnect();

    QString lastError() const { return m_lastError; }
    bool isConnected() const { return m_ctx != nullptr; }

    /// 构造 smb:// URL 的工具：smb://<host>[/share][/path]
    static QString buildUrl(const QString& host, const QString& share,
                            const QString& path = QString());

    // 鉴权凭证（必须在头文件公开以便 .cpp 的静态工具函数可用）。
    struct Creds { QString user, workgroup, password; };

private:
    static void authCallback(SMBCCTX* ctx,
                             const char* srv, const char* shr,
                             char* wg, int wglen,
                             char* un, int unlen,
                             char* pw, int pwlen);

    SMBCCTX* m_ctx = nullptr;
    QString  m_lastError;

    // 鉴权信息存放在单例映射里（ctx ptr → creds），供静态回调使用。
    // 这是 libsmbclient 唯一规范的方式——它不带 user-data 指针。
    static Creds& credsFor(SMBCCTX* ctx);
    static void   clearCreds(SMBCCTX* ctx);
};

#endif // SMBCLIENT_H
