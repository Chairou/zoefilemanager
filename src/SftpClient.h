#ifndef SFTPCLIENT_H
#define SFTPCLIENT_H

// =============================================================================
// SftpClient —— libssh2 的 C++/Qt 薄封装，提供"真实"的 SSH/SFTP 登录
//
// 流程（synchronous，阻塞调用）：
//   1. 解析主机名（getaddrinfo，IPv4/IPv6 双栈）
//   2. 非阻塞 TCP connect + select 超时
//   3. SSH 握手（libssh2_session_handshake）
//   4. 拿主机指纹 SHA256（用于在 UI 上展示，未来可作 TOFU 信任锚）
//   5. 认证：优先 publickey（如给了私钥），否则 password
//   6. 若协议是 SFTP，打开 SFTP 通道并以 opendir("/") 验通
//
// RAII：析构会自动 disconnect()，调多次 disconnect() 安全。
//
// 线程：libssh2 是阻塞的。当前所有调用走 GUI 线程；登录耗时 < 1s 一般可以接受，
// 不行的话可以把 connect 搬到 QThread —— public API 已经按这个方向设计了
// （都是同步可重入的方法，没有信号槽依赖）。
// =============================================================================

#include "Types.h"
#include <QObject>
#include <QString>
#include <QVector>

// 前向声明 libssh2 不透明类型，避免在本头文件暴露 libssh2.h（让上层
// include path 不必带 libssh2 头目录）。
struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_SFTP    LIBSSH2_SFTP;

class SftpClient : public QObject {
public:
    explicit SftpClient(QObject* parent = nullptr);
    ~SftpClient() override;

    /**
     * 登录 + 认证（同步，阻塞）。
     *
     * @param conn             连接配置（host/port/credentials/...）
     * @param openSftpChannel  true 且 protocol=="SFTP" 时，会打开 SFTP 通道
     *                         并以 opendir("/") 验证可用
     * @param timeoutMs        TCP 连接超时（ms）；handshake 之后的阻塞时间
     *                         由 libssh2 自己控制，不计入此值
     * @return                 true=登录成功；false=失败，lastError() 给出原因
     */
    bool connectAndAuth(const RemoteConnection& conn,
                        bool openSftpChannel = true,
                        int timeoutMs = 8000);

    /// 关闭会话。多次调用安全；处于"全局 shutdown"状态时降级为只关 socket。
    void disconnect();

    bool isConnected() const { return m_session != nullptr; }
    QString lastError() const { return m_lastError; }
    QString hostFingerprintSha256() const { return m_fingerprint; }

    /**
     * 进程退出前由应用 hook 调用（见 FileSystemRouter::~FileSystemRouter）。
     * 一旦置位，本对象析构时 disconnect() 不再调任何 libssh2 API，仅 close
     * 原始 socket。这是为了规避静态析构 vs dyld 卸载顺序导致的 SIGSEGV：
     * libcrypto 的 RNG 锁可能已先被 cleanup，libssh2 内部的 RAND_bytes_ex
     * 会在 pthread_rwlock_rdlock(NULL) 上崩溃。
     */
    static void markShuttingDown();
    static bool isShuttingDown();

    // ----- 远程文件系统操作（阻塞，需 SFTP 通道已打开）-----
    // remotePath 是服务器侧绝对路径（"/", "/home/demo" 等）。
    // 失败时返回空容器/false 并设置 lastError()；通道未打开时静默返回 false，不抛异常。
    QVector<FileEntry> listDirectory(const QString& remotePath);
    QStringList        listSubdirectories(const QString& remotePath);
    bool               isDirectory(const QString& remotePath);
    bool               exists(const QString& remotePath);

    /// 是否在 listDirectory 输出中包含 "." 开头的隐藏项
    void setShowHidden(bool show) { m_showHidden = show; }
    bool showHidden() const { return m_showHidden; }

private:
    bool initLibrary();                                           // libssh2_init 一次性
    bool openSocket(const QString& host, int port, int timeoutMs);// 非阻塞 connect + 超时
    void closeSocket();
    void setError(const QString& msg);
    QString sessionError() const;                                 // 翻译 libssh2 最近错误

    int                m_socket    = -1;        // 原始 TCP fd，-1 == 关闭
    LIBSSH2_SESSION*   m_session   = nullptr;
    LIBSSH2_SFTP*      m_sftp      = nullptr;
    QString            m_lastError;
    QString            m_fingerprint;
    bool               m_showHidden = false;

    static bool        s_libInited;     // libssh2_init 是否已调过
    static bool        s_shuttingDown;  // 全局 shutdown 标志
};

#endif // SFTPCLIENT_H
