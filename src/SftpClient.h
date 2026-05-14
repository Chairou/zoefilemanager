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
#include <functional>

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

    // ----- 远程文件操作（增/删/改） -----
    /**
     * 删除单个文件（不含目录）。语义对应 sftp_unlink。
     */
    bool               removeFile(const QString& remotePath);
    /**
     * 删除空目录。语义对应 sftp_rmdir。
     */
    bool               removeEmptyDirectory(const QString& remotePath);
    /**
     * 递归删除：文件直接 unlink；目录则递归清空再 rmdir。
     * 任意叶子失败即整体失败（已删除的不会回滚）。
     */
    bool               removeRecursive(const QString& remotePath);
    /**
     * POSIX rename：跨目录移动 / 改名都行；目标已存在的语义由服务器决定
     * （多数实现会失败，需上层先 unlink）。
     */
    bool               renamePath(const QString& oldRemotePath,
                                  const QString& newRemotePath);
    /**
     * 创建目录（一级；mode=0755）。父目录不存在时失败。
     */
    bool               makeDirectory(const QString& remotePath);

    // ----- 远程↔远程拷贝（同会话内，server 侧 read+write） -----
    /**
     * 在同一会话内把 src 复制到 dst（覆盖式）。dst 若已存在会被覆盖。
     * 仅文件；目录请用 copyRecursive。
     */
    bool               copyFile(const QString& srcRemotePath,
                                const QString& dstRemotePath);
    /**
     * 递归拷贝（目标父目录必须存在，自身不存在则创建）。
     */
    bool               copyRecursive(const QString& srcRemotePath,
                                     const QString& dstRemotePath);

    // ----- 本地↔远程传输 -----
    /**
     * 进度回调签名（byte-level）：
     *   currentName  当前正在传输的项目（用于 UI label）
     *   bytesDone    本次会话累计已传字节数
     *   bytesTotal   预扫得到的总字节数（>=0；0 表示未知，UI 应回退到 busy 模式）
     * 返回 false 表示用户取消传输（实现方应尽快中止并返回 false）。
     */
    using TransferProgressFn =
        std::function<bool(const QString& currentName,
                           qint64 bytesDone, qint64 bytesTotal)>;

    /**
     * 把本地文件上传到远端（覆盖式）。仅文件；目录请用 uploadRecursive。
     * progress 可为空。
     */
    bool               uploadFile(const QString& localPath,
                                  const QString& remotePath,
                                  const TransferProgressFn& progress = {},
                                  qint64 bytesTotal = 0,
                                  qint64* bytesDone = nullptr);
    /**
     * 递归上传：本地目录 → 远端目录；本地文件 → 远端文件。
     * 远端目标父目录必须存在；自身不存在则创建。
     */
    bool               uploadRecursive(const QString& localPath,
                                       const QString& remotePath,
                                       const TransferProgressFn& progress = {},
                                       qint64 bytesTotal = 0,
                                       qint64* bytesDone = nullptr);
    /**
     * 把远端文件下载到本地（覆盖式）。仅文件；目录请用 downloadRecursive。
     */
    bool               downloadFile(const QString& remotePath,
                                    const QString& localPath,
                                    const TransferProgressFn& progress = {},
                                    qint64 bytesTotal = 0,
                                    qint64* bytesDone = nullptr);
    /**
     * 递归下载：远端目录 → 本地目录；远端文件 → 本地文件。
     */
    bool               downloadRecursive(const QString& remotePath,
                                         const QString& localPath,
                                         const TransferProgressFn& progress = {},
                                         qint64 bytesTotal = 0,
                                         qint64* bytesDone = nullptr);

    // ----- 预扫工具：计算总字节数（用于驱动进度条上限） -----
    /// 本地文件/目录总字节数（递归加和；目录元数据不计；不可读条目跳过）
    static qint64 localTotalBytes(const QString& localPath);
    /// 远端文件/目录总字节数（递归 listDirectory 累加；隐藏文件计入）
    qint64 remoteTotalBytes(const QString& remotePath);

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
