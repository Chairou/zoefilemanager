// =============================================================================
// SftpClient.cpp —— 见 SftpClient.h
//
// 本文件直接调用 libssh2 + 系统 BSD socket API（getaddrinfo、socket、
// non-blocking connect + select 超时）。把这部分包到本文件里，让上层
// (RemoteDialog / FileSystemRouter) 完全不感知 libssh2 / socket 细节。
// =============================================================================

#include "SftpClient.h"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <QFileInfo>
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QDir>

#include <algorithm>

// POSIX 网络/IO API
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

bool SftpClient::s_libInited = false;
bool SftpClient::s_shuttingDown = false;

void SftpClient::markShuttingDown() { s_shuttingDown = true; }
bool SftpClient::isShuttingDown()   { return s_shuttingDown; }

SftpClient::SftpClient(QObject* parent) : QObject(parent) {}

SftpClient::~SftpClient() {
    disconnect();   // RAII：始终干净拆掉会话
}

// 一次性初始化 libssh2 全局状态（线程不安全，但我们在 GUI 线程上调用，OK）。
bool SftpClient::initLibrary() {
    if (s_libInited) return true;
    int rc = libssh2_init(0);
    if (rc != 0) {
        setError(QString("libssh2_init failed: %1").arg(rc));
        return false;
    }
    s_libInited = true;
    return true;
}

// ---------------------------------------------------------------------------
// 非阻塞 TCP connect，支持超时控制。
//
// 流程：
//   1. getaddrinfo 解析（IPv4/IPv6 双栈）
//   2. 对每个候选 sockaddr 尝试 socket() + 非阻塞 connect()
//   3. 用 select() 等可写 / 超时
//   4. SO_ERROR 检查 connect 是否真成功
//   5. 第一个成功的就用，剩下不试
//   6. 设置 TCP_NODELAY 减小握手延迟
// ---------------------------------------------------------------------------
bool SftpClient::openSocket(const QString& host, int port, int timeoutMs) {
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    QByteArray hostUtf8 = host.toUtf8();
    QByteArray portStr  = QByteArray::number(port);
    struct addrinfo* res = nullptr;
    int gai = ::getaddrinfo(hostUtf8.constData(), portStr.constData(),
                            &hints, &res);
    if (gai != 0 || !res) {
        setError(QString("DNS lookup failed for %1: %2")
                 .arg(host, gai_strerror(gai)));
        return false;
    }

    int fd = -1;
    QString lastErr;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { lastErr = strerror(errno); continue; }

        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int cr = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (cr == 0) {
            ::fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno != EINPROGRESS) {
            lastErr = strerror(errno);
            ::close(fd); fd = -1;
            continue;
        }

        fd_set wset;
        FD_ZERO(&wset); FD_SET(fd, &wset);
        struct timeval tv;
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = ::select(fd + 1, nullptr, &wset, nullptr, &tv);
        if (sel <= 0) {
            lastErr = (sel == 0) ? "connect timeout" : strerror(errno);
            ::close(fd); fd = -1;
            continue;
        }
        int soerr = 0; socklen_t solen = sizeof(soerr);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &solen);
        if (soerr != 0) {
            lastErr = strerror(soerr);
            ::close(fd); fd = -1;
            continue;
        }
        ::fcntl(fd, F_SETFL, flags);
        break;
    }
    ::freeaddrinfo(res);

    if (fd < 0) {
        setError(QString("Cannot connect to %1:%2 - %3")
                 .arg(host).arg(port).arg(lastErr));
        return false;
    }

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    m_socket = fd;
    return true;
}

void SftpClient::closeSocket() {
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
}

QString SftpClient::sessionError() const {
    if (!m_session) return QStringLiteral("(no session)");
    char* msg = nullptr;
    int   len = 0;
    libssh2_session_last_error(m_session, &msg, &len, 0);
    return QString::fromUtf8(msg ? msg : "unknown libssh2 error", len);
}

void SftpClient::setError(const QString& msg) {
    m_lastError = msg;
}

// ---------------------------------------------------------------------------
// 完整登录流程：TCP → SSH 握手 → 取主机指纹 → 认证 → 可选 SFTP 通道。
// 任何阶段失败都会 disconnect() 清理已分配的资源，不留泄漏。
// ---------------------------------------------------------------------------
bool SftpClient::connectAndAuth(const RemoteConnection& conn,
                                bool openSftpChannel,
                                int timeoutMs) {
    disconnect();           // 重连前先把旧会话拆干净
    m_lastError.clear();
    m_fingerprint.clear();

    // 基本参数校验
    if (conn.host.isEmpty())     { setError("Host is empty");     return false; }
    if (conn.username.isEmpty()) { setError("Username is empty"); return false; }
    if (!initLibrary())                                            return false;
    if (!openSocket(conn.host, conn.port, timeoutMs))              return false;

    // 建立 libssh2 会话对象
    m_session = libssh2_session_init();
    if (!m_session) {
        setError("libssh2_session_init failed");
        closeSocket();
        return false;
    }
    // 走阻塞模式，简化 GUI 线程的同步语义
    libssh2_session_set_blocking(m_session, 1);

    // SSH 握手：协商版本、密钥交换、加密算法
    if (libssh2_session_handshake(m_session, m_socket) != 0) {
        setError(QString("SSH handshake failed: %1").arg(sessionError()));
        disconnect();
        return false;
    }

    // 抓主机公钥的 SHA256 指纹，用于在 UI 状态栏显示。未来若做
    // TOFU（首次信任 + 持久化）可以基于这个值做对比。
    const char* fp = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (fp) {
        QByteArray raw(fp, 32);
        m_fingerprint = "SHA256:" + QString::fromLatin1(raw.toBase64(
            QByteArray::Base64Option::OmitTrailingEquals));
    }

    // ---- 认证 ----
    // 策略：填了私钥就先试公钥；公钥失败且没填密码 → 失败；公钥失败但
    // 还填了密码 → 回退到密码认证；只填密码 → 直接走密码。
    bool authed = false;

    if (!conn.privateKeyPath.isEmpty()) {
        QFileInfo keyInfo(conn.privateKeyPath);
        if (!keyInfo.isReadable()) {
            setError(QString("Private key not readable: %1").arg(conn.privateKeyPath));
            disconnect();
            return false;
        }
        QByteArray user = conn.username.toUtf8();
        QByteArray key  = conn.privateKeyPath.toUtf8();
        QByteArray pass = conn.passphrase.toUtf8();
        // pubkey 文件传 nullptr 让 libssh2 自动在私钥旁边找 .pub
        int rc = libssh2_userauth_publickey_fromfile_ex(
            m_session, user.constData(), user.size(),
            nullptr,
            key.constData(),
            pass.isEmpty() ? nullptr : pass.constData());
        if (rc == 0) {
            authed = true;
        } else if (conn.password.isEmpty()) {
            // 用户只给了私钥，公钥认证失败就直接报错（不要静默回退）
            setError(QString("Public-key auth rejected: %1").arg(sessionError()));
            disconnect();
            return false;
        }
        // 否则继续往下走密码认证
    }

    if (!authed) {
        if (conn.password.isEmpty()) {
            setError("No credentials supplied (need password or private key)");
            disconnect();
            return false;
        }
        QByteArray user = conn.username.toUtf8();
        QByteArray pwd  = conn.password.toUtf8();
        int rc = libssh2_userauth_password_ex(
            m_session,
            user.constData(), user.size(),
            pwd.constData(),  pwd.size(),
            nullptr);
        if (rc != 0) {
            setError(QString("Password auth rejected: %1").arg(sessionError()));
            disconnect();
            return false;
        }
        authed = true;
    }

    if (!authed) {
        setError("Authentication failed (no method succeeded)");
        disconnect();
        return false;
    }

    const bool wantSftp = openSftpChannel
        && conn.protocol.compare("SFTP", Qt::CaseInsensitive) == 0;
    if (wantSftp) {
        m_sftp = libssh2_sftp_init(m_session);
        if (!m_sftp) {
            setError(QString("SFTP channel init failed: %1").arg(sessionError()));
            disconnect();
            return false;
        }
        // Smoke-test: open the root dir handle, then close it.
        LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(m_sftp, "/");
        if (!dir) {
            setError(QString("SFTP opendir(\"/\") failed: %1").arg(sessionError()));
            disconnect();
            return false;
        }
        libssh2_sftp_closedir(dir);
    }

    return true;
}

void SftpClient::disconnect() {
    // 全局 shutdown 标志（由 FileSystemRouter::~FileSystemRouter 设置）：
    // 此时 libcrypto/libssh2 可能已经被 dyld 卸载或 OpenSSL_cleanup 拆掉了，
    // 任何 libssh2 调用都会陷入 OpenSSL 然后在已释放的 RNG 锁上崩溃。
    // 进程都要退出了，礼貌的"再见包"也没必要发，直接关 fd 就走。
    if (s_shuttingDown) {
        m_sftp = nullptr;
        m_session = nullptr;
        closeSocket();
        return;
    }
    if (m_sftp) {
        libssh2_sftp_shutdown(m_sftp);
        m_sftp = nullptr;
    }
    if (m_session) {
        libssh2_session_disconnect(m_session, "Bye from Zoe File Manager");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    closeSocket();
}

// ---------------------------------------------------------------------------
// 远程文件系统操作 —— 全部基于 libssh2_sftp_*。
// 调用前必须保证 m_sftp != nullptr（已 connectAndAuth 且开了 SFTP 通道）。
// ---------------------------------------------------------------------------

// 文件类型启发式（与 RealFileSystem::guessFileType 等价，仅做 UI 装饰）
static FileType guessFileTypeFromName(const QString& name) {
    int dot = name.lastIndexOf('.');
    if (dot < 0) return FileType::Unknown;
    QString ext = name.mid(dot + 1).toLower();
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "pptx" ||
        ext == "xls" || ext == "xlsx") return FileType::Document;
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" ||
        ext == "svg" || ext == "bmp" || ext == "ico") return FileType::Image;
    if (ext == "html" || ext == "css" || ext == "js" || ext == "ts" ||
        ext == "py" || ext == "cpp" || ext == "h"  || ext == "java")
        return FileType::Code;
    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "rar" ||
        ext == "7z"  || ext == "dmg") return FileType::Archive;
    if (ext == "mp3" || ext == "flac" || ext == "wav" || ext == "ogg" ||
        ext == "aac") return FileType::Audio;
    if (ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov" ||
        ext == "wmv") return FileType::Video;
    if (ext == "txt" || ext == "md" || ext == "csv" || ext == "log")
        return FileType::Text;
    if (ext == "conf" || ext == "cfg" || ext == "ini" || ext == "yaml" ||
        ext == "yml" || ext == "json") return FileType::Config;
    if (ext == "sh"  || ext == "bash" || ext == "zsh") return FileType::Script;
    return FileType::Unknown;
}

static QString formatPosixPermissions(unsigned long mode) {
    // mode is POSIX st_mode. We only render rwx bits.
    QString r;
    r += (mode & 0400) ? 'r' : '-';
    r += (mode & 0200) ? 'w' : '-';
    r += (mode & 0100) ? 'x' : '-';
    r += (mode & 0040) ? 'r' : '-';
    r += (mode & 0020) ? 'w' : '-';
    r += (mode & 0010) ? 'x' : '-';
    r += (mode & 0004) ? 'r' : '-';
    r += (mode & 0002) ? 'w' : '-';
    r += (mode & 0001) ? 'x' : '-';
    return r;
}

static QString joinRemotePath(const QString& dir, const QString& name) {
    if (dir.isEmpty() || dir == "/") return "/" + name;
    if (dir.endsWith('/'))           return dir + name;
    return dir + "/" + name;
}

QVector<FileEntry> SftpClient::listDirectory(const QString& remotePath) {
    QVector<FileEntry> out;
    if (!m_sftp) { setError("SFTP channel not open"); return out; }

    QByteArray pathUtf8 = remotePath.isEmpty() ? QByteArray("/")
                                               : remotePath.toUtf8();

    // 打开目录句柄；服务器侧不存在或无权限时返回 NULL
    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(m_sftp, pathUtf8.constData());
    if (!dir) {
        setError(QString("opendir(%1) failed: %2")
                 .arg(remotePath, sessionError()));
        return out;
    }

    // readdir_ex 一次返回一项；nameBuf 是短名（不含路径），longEntry 是
    // ls -l 风格的长串（这里不用，仅为 API 强制要求）
    char                 nameBuf[1024];
    char                 longEntry[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (true) {
        int n = libssh2_sftp_readdir_ex(dir, nameBuf, sizeof(nameBuf),
                                        longEntry, sizeof(longEntry), &attrs);
        if (n <= 0) break;   // 0 = 读完，<0 = 出错（按读完处理，宽容）

        QString name = QString::fromUtf8(nameBuf, n);
        // 过滤 "." / ".."（服务器会包含这两项）；隐藏文件按全局开关
        if (name == "." || name == "..") continue;
        if (!m_showHidden && name.startsWith('.')) continue;

        FileEntry e;
        e.name = name;
        e.path = joinRemotePath(remotePath, name);
        // attrs.flags 标志位告诉我们哪些字段实际有效（SFTP 服务器可能不返回
        // 全部信息），逐字段判断后再读
        bool haveMode = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0;
        bool haveSize = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)         != 0;
        bool haveTime = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)    != 0;
        e.isDirectory = haveMode && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        e.size        = haveSize ? static_cast<qint64>(attrs.filesize)
                                 : (e.isDirectory ? 4096 : 0);
        e.modified    = haveTime
            ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(attrs.mtime))
            : QDateTime();
        e.permissions = haveMode ? formatPosixPermissions(attrs.permissions)
                                 : QStringLiteral("---------");
        e.type        = e.isDirectory ? FileType::Directory
                                      : guessFileTypeFromName(name);
        out.append(e);
    }

    libssh2_sftp_closedir(dir);

    // Sort: dirs first, then by name (case-insensitive)
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b){
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QStringList SftpClient::listSubdirectories(const QString& remotePath) {
    QStringList dirs;
    const auto entries = listDirectory(remotePath);
    for (const auto& e : entries) {
        if (e.isDirectory) dirs.append(e.path);
    }
    return dirs;
}

bool SftpClient::isDirectory(const QString& remotePath) {
    if (!m_sftp) return false;
    LIBSSH2_SFTP_ATTRIBUTES a;
    QByteArray p = remotePath.toUtf8();
    if (libssh2_sftp_stat(m_sftp, p.constData(), &a) != 0) return false;
    return (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
        && LIBSSH2_SFTP_S_ISDIR(a.permissions);
}

bool SftpClient::exists(const QString& remotePath) {
    if (!m_sftp) return false;
    LIBSSH2_SFTP_ATTRIBUTES a;
    QByteArray p = remotePath.toUtf8();
    return libssh2_sftp_stat(m_sftp, p.constData(), &a) == 0;
}

// ---------------------------------------------------------------------------
// 增/删/改 —— 都是 libssh2_sftp_* 的薄封装；失败时翻译错误码到 lastError()
// ---------------------------------------------------------------------------
bool SftpClient::removeFile(const QString& remotePath) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }
    QByteArray p = remotePath.toUtf8();
    int rc = libssh2_sftp_unlink(m_sftp, p.constData());
    if (rc != 0) {
        setError(QString("unlink(%1) failed: %2").arg(remotePath, sessionError()));
        return false;
    }
    return true;
}

bool SftpClient::removeEmptyDirectory(const QString& remotePath) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }
    QByteArray p = remotePath.toUtf8();
    int rc = libssh2_sftp_rmdir(m_sftp, p.constData());
    if (rc != 0) {
        setError(QString("rmdir(%1) failed: %2").arg(remotePath, sessionError()));
        return false;
    }
    return true;
}

bool SftpClient::removeRecursive(const QString& remotePath) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }

    // 先 stat 判断类型；不存在直接当成功（幂等）
    LIBSSH2_SFTP_ATTRIBUTES a;
    QByteArray p = remotePath.toUtf8();
    if (libssh2_sftp_stat(m_sftp, p.constData(), &a) != 0) {
        return true;  // 不存在视为已删
    }
    bool isDir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
              && LIBSSH2_SFTP_S_ISDIR(a.permissions);
    if (!isDir) {
        return removeFile(remotePath);
    }

    // 目录：临时关掉 hidden 过滤，确保 . 开头的项也能被列出来
    bool savedHidden = m_showHidden;
    m_showHidden = true;
    auto kids = listDirectory(remotePath);
    m_showHidden = savedHidden;

    for (const auto& e : kids) {
        // listDirectory 返回的 e.path 已经是 joinRemotePath(remotePath, name)
        if (!removeRecursive(e.path)) return false;
    }
    return removeEmptyDirectory(remotePath);
}

bool SftpClient::renamePath(const QString& oldRemotePath,
                            const QString& newRemotePath) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }
    QByteArray a = oldRemotePath.toUtf8();
    QByteArray b = newRemotePath.toUtf8();
    int rc = libssh2_sftp_rename(m_sftp, a.constData(), b.constData());
    if (rc != 0) {
        setError(QString("rename(%1 -> %2) failed: %3")
                 .arg(oldRemotePath, newRemotePath, sessionError()));
        return false;
    }
    return true;
}

bool SftpClient::makeDirectory(const QString& remotePath) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }
    QByteArray p = remotePath.toUtf8();
    // 0755（rwxr-xr-x）：和大多数 sftp/ssh 客户端默认一致
    long mode = LIBSSH2_SFTP_S_IRWXU
              | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP
              | LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH;
    int rc = libssh2_sftp_mkdir(m_sftp, p.constData(), mode);
    if (rc != 0) {
        setError(QString("mkdir(%1) failed: %2").arg(remotePath, sessionError()));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 同会话内 server-side 拷贝 —— SFTP 协议本身没有 server-side copy 原语，
// 所以只能 read 一边 write 另一边。带宽走的是 SSH 通道；好处是不必落地客户端
// 缓冲（按块流式）。失败时尝试清理半成品。
// ---------------------------------------------------------------------------
bool SftpClient::copyFile(const QString& srcRemotePath,
                          const QString& dstRemotePath) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }

    QByteArray srcUtf8 = srcRemotePath.toUtf8();
    QByteArray dstUtf8 = dstRemotePath.toUtf8();

    LIBSSH2_SFTP_HANDLE* in =
        libssh2_sftp_open(m_sftp, srcUtf8.constData(),
                          LIBSSH2_FXF_READ, 0);
    if (!in) {
        setError(QString("open(%1) for read failed: %2")
                 .arg(srcRemotePath, sessionError()));
        return false;
    }

    // 0644 默认权限；CREAT|WRITE|TRUNC = 覆盖式新建
    long mode = LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR
              | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH;
    LIBSSH2_SFTP_HANDLE* out =
        libssh2_sftp_open(m_sftp, dstUtf8.constData(),
                          LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                          mode);
    if (!out) {
        libssh2_sftp_close(in);
        setError(QString("open(%1) for write failed: %2")
                 .arg(dstRemotePath, sessionError()));
        return false;
    }

    constexpr int kBuf = 32 * 1024;  // 32KB —— SFTP 包通常 32K 上限友好
    QByteArray buf(kBuf, Qt::Uninitialized);
    bool ok = true;
    while (true) {
        ssize_t n = libssh2_sftp_read(in, buf.data(), buf.size());
        if (n == 0) break;
        if (n < 0) {
            setError(QString("read(%1) failed: %2")
                     .arg(srcRemotePath, sessionError()));
            ok = false;
            break;
        }
        const char* p = buf.constData();
        ssize_t left = n;
        while (left > 0) {
            ssize_t w = libssh2_sftp_write(out, p, left);
            if (w < 0) {
                setError(QString("write(%1) failed: %2")
                         .arg(dstRemotePath, sessionError()));
                ok = false;
                break;
            }
            left -= w;
            p    += w;
        }
        if (!ok) break;
    }

    libssh2_sftp_close(in);
    libssh2_sftp_close(out);

    if (!ok) {
        // 半成品收尾：能 unlink 就 unlink，失败也只能算了
        libssh2_sftp_unlink(m_sftp, dstUtf8.constData());
    }
    return ok;
}

bool SftpClient::copyRecursive(const QString& srcRemotePath,
                               const QString& dstRemotePath) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }

    LIBSSH2_SFTP_ATTRIBUTES a;
    QByteArray sp = srcRemotePath.toUtf8();
    if (libssh2_sftp_stat(m_sftp, sp.constData(), &a) != 0) {
        setError(QString("stat(%1) failed: %2")
                 .arg(srcRemotePath, sessionError()));
        return false;
    }
    bool isDir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
              && LIBSSH2_SFTP_S_ISDIR(a.permissions);
    if (!isDir) {
        return copyFile(srcRemotePath, dstRemotePath);
    }

    // 目录：自身不存在则创建（已存在则继续 merge）
    if (!exists(dstRemotePath)) {
        if (!makeDirectory(dstRemotePath)) return false;
    }

    bool savedHidden = m_showHidden;
    m_showHidden = true;
    auto kids = listDirectory(srcRemotePath);
    m_showHidden = savedHidden;

    for (const auto& e : kids) {
        // 拼出目标路径：srcRemotePath/<name> → dstRemotePath/<name>
        // listDirectory 的 e.path 已是绝对，取 name 即可
        QString dst = dstRemotePath;
        if (!dst.endsWith('/')) dst += '/';
        dst += e.name;
        if (!copyRecursive(e.path, dst)) return false;
    }
    return true;
}

// ===========================================================================
// 本地↔远程传输（uploadFile / uploadRecursive / downloadFile / downloadRecursive）
//
// 设计要点：
//   - 块大小 32 KB，与 copyFile 保持一致（对 SFTP 包尺寸友好）
//   - byte-level 进度回调：bytesDone 是"会话累计"，递归调用之间共享同一个
//     计数器（外部传入指针，递归内部直接累加，UI 那侧拿到的是平滑增长的曲线）
//   - bytesTotal 由外部预扫得到（localTotalBytes / remoteTotalBytes）。
//     UI 不必为单文件单独算 total，传 0 也能跑（进度退化为 busy 模式）
//   - progress 返回 false → 立刻返回 false，半成品文件尽量 unlink/QFile::remove
// ===========================================================================

qint64 SftpClient::localTotalBytes(const QString& localPath) {
    QFileInfo fi(localPath);
    if (!fi.exists()) return 0;
    if (fi.isFile()) return fi.size();
    if (!fi.isDir())  return 0;

    qint64 total = 0;
    QDir dir(localPath);
    const auto kids = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const auto& k : kids) {
        total += localTotalBytes(k.absoluteFilePath());
    }
    return total;
}

qint64 SftpClient::remoteTotalBytes(const QString& remotePath) {
    if (!m_sftp) return 0;
    LIBSSH2_SFTP_ATTRIBUTES a;
    QByteArray rp = remotePath.toUtf8();
    if (libssh2_sftp_stat(m_sftp, rp.constData(), &a) != 0) return 0;
    bool isDir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
              && LIBSSH2_SFTP_S_ISDIR(a.permissions);
    if (!isDir) {
        return (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (qint64)a.filesize : 0;
    }
    bool savedHidden = m_showHidden;
    m_showHidden = true;
    auto kids = listDirectory(remotePath);
    m_showHidden = savedHidden;

    qint64 total = 0;
    for (const auto& e : kids) {
        if (e.isDirectory) total += remoteTotalBytes(e.path);
        else               total += e.size;
    }
    return total;
}

bool SftpClient::uploadFile(const QString& localPath,
                            const QString& remotePath,
                            const TransferProgressFn& progress,
                            qint64 bytesTotal,
                            qint64* bytesDone) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }

    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        setError(QString("open local %1 for read failed: %2")
                 .arg(localPath, in.errorString()));
        return false;
    }

    // 0644 默认权限；CREAT|WRITE|TRUNC = 覆盖式新建
    QByteArray dstUtf8 = remotePath.toUtf8();
    long mode = LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR
              | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH;
    LIBSSH2_SFTP_HANDLE* out =
        libssh2_sftp_open(m_sftp, dstUtf8.constData(),
                          LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                          mode);
    if (!out) {
        setError(QString("sftp open(%1) for write failed: %2")
                 .arg(remotePath, sessionError()));
        return false;
    }

    const QString name = QFileInfo(localPath).fileName();
    constexpr int kBuf = 32 * 1024;
    QByteArray buf(kBuf, Qt::Uninitialized);
    bool ok = true;
    qint64 localDone = 0;
    while (true) {
        qint64 n = in.read(buf.data(), buf.size());
        if (n == 0) break;
        if (n < 0) {
            setError(QString("read local %1 failed: %2")
                     .arg(localPath, in.errorString()));
            ok = false;
            break;
        }
        const char* p = buf.constData();
        qint64 left = n;
        while (left > 0) {
            ssize_t w = libssh2_sftp_write(out, p, left);
            if (w < 0) {
                setError(QString("sftp write(%1) failed: %2")
                         .arg(remotePath, sessionError()));
                ok = false;
                break;
            }
            left -= w;
            p    += w;
        }
        if (!ok) break;

        localDone += n;
        if (bytesDone) *bytesDone += n;
        if (progress) {
            qint64 cur = bytesDone ? *bytesDone : localDone;
            if (!progress(name, cur, bytesTotal)) {
                setError("upload canceled by user");
                ok = false;
                break;
            }
        }
    }

    libssh2_sftp_close(out);
    in.close();

    if (!ok) {
        // 半成品收尾
        libssh2_sftp_unlink(m_sftp, dstUtf8.constData());
    }
    return ok;
}

bool SftpClient::uploadRecursive(const QString& localPath,
                                 const QString& remotePath,
                                 const TransferProgressFn& progress,
                                 qint64 bytesTotal,
                                 qint64* bytesDone) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }

    QFileInfo fi(localPath);
    if (!fi.exists()) {
        setError(QString("local %1 does not exist").arg(localPath));
        return false;
    }
    if (fi.isFile()) {
        return uploadFile(localPath, remotePath, progress, bytesTotal, bytesDone);
    }
    if (!fi.isDir()) {
        setError(QString("local %1 is neither file nor dir").arg(localPath));
        return false;
    }

    // 目录：远端不存在则创建（已存在则 merge）
    if (!exists(remotePath)) {
        if (!makeDirectory(remotePath)) return false;
    }

    QDir dir(localPath);
    const auto kids = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const auto& k : kids) {
        QString dst = remotePath;
        if (!dst.endsWith('/')) dst += '/';
        dst += k.fileName();
        if (!uploadRecursive(k.absoluteFilePath(), dst,
                             progress, bytesTotal, bytesDone)) {
            return false;
        }
    }
    return true;
}

bool SftpClient::downloadFile(const QString& remotePath,
                              const QString& localPath,
                              const TransferProgressFn& progress,
                              qint64 bytesTotal,
                              qint64* bytesDone) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }

    QByteArray srcUtf8 = remotePath.toUtf8();
    LIBSSH2_SFTP_HANDLE* in =
        libssh2_sftp_open(m_sftp, srcUtf8.constData(),
                          LIBSSH2_FXF_READ, 0);
    if (!in) {
        setError(QString("sftp open(%1) for read failed: %2")
                 .arg(remotePath, sessionError()));
        return false;
    }

    QFile out(localPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        libssh2_sftp_close(in);
        setError(QString("open local %1 for write failed: %2")
                 .arg(localPath, out.errorString()));
        return false;
    }

    const QString name = QFileInfo(remotePath).fileName();
    constexpr int kBuf = 32 * 1024;
    QByteArray buf(kBuf, Qt::Uninitialized);
    bool ok = true;
    qint64 localDone = 0;
    while (true) {
        ssize_t n = libssh2_sftp_read(in, buf.data(), buf.size());
        if (n == 0) break;
        if (n < 0) {
            setError(QString("sftp read(%1) failed: %2")
                     .arg(remotePath, sessionError()));
            ok = false;
            break;
        }
        qint64 written = out.write(buf.constData(), n);
        if (written != n) {
            setError(QString("write local %1 failed: %2")
                     .arg(localPath, out.errorString()));
            ok = false;
            break;
        }
        localDone += n;
        if (bytesDone) *bytesDone += n;
        if (progress) {
            qint64 cur = bytesDone ? *bytesDone : localDone;
            if (!progress(name, cur, bytesTotal)) {
                setError("download canceled by user");
                ok = false;
                break;
            }
        }
    }

    libssh2_sftp_close(in);
    out.close();

    if (!ok) {
        out.remove();   // 删半成品
    }
    return ok;
}

bool SftpClient::downloadRecursive(const QString& remotePath,
                                   const QString& localPath,
                                   const TransferProgressFn& progress,
                                   qint64 bytesTotal,
                                   qint64* bytesDone) {
    if (!m_sftp) { setError("SFTP channel not open"); return false; }

    LIBSSH2_SFTP_ATTRIBUTES a;
    QByteArray rp = remotePath.toUtf8();
    if (libssh2_sftp_stat(m_sftp, rp.constData(), &a) != 0) {
        setError(QString("sftp stat(%1) failed: %2")
                 .arg(remotePath, sessionError()));
        return false;
    }
    bool isDir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
              && LIBSSH2_SFTP_S_ISDIR(a.permissions);
    if (!isDir) {
        return downloadFile(remotePath, localPath, progress, bytesTotal, bytesDone);
    }

    // 目录：本地不存在则创建（已存在则 merge）
    QDir dir;
    if (!dir.exists(localPath)) {
        if (!dir.mkpath(localPath)) {
            setError(QString("mkpath local %1 failed").arg(localPath));
            return false;
        }
    }

    bool savedHidden = m_showHidden;
    m_showHidden = true;
    auto kids = listDirectory(remotePath);
    m_showHidden = savedHidden;

    for (const auto& e : kids) {
        QString dstLocal = localPath;
        if (!dstLocal.endsWith('/')) dstLocal += '/';
        dstLocal += e.name;
        if (!downloadRecursive(e.path, dstLocal,
                               progress, bytesTotal, bytesDone)) {
            return false;
        }
    }
    return true;
}
