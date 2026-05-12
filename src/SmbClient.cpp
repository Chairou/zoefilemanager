// =============================================================================
// SmbClient.cpp —— 见 SmbClient.h
//
// 参考资料：
//   - man smbclient（库 API 部分）
//   - libsmbclient.h 注释
//   - Samba 源码 examples/libsmbclient/
// =============================================================================

#include "SmbClient.h"

#include <libsmbclient.h>
#include <cstring>
#include <cerrno>
#include <QHash>
#include <QMutex>
#include <QFileInfo>
#include <QDateTime>
#include <QUrl>
#include <QStringDecoder>
#include <QDebug>
#include <QLoggingCategory>

// 用专门的日志分类，便于按需打开/关闭：
//   QT_LOGGING_RULES="zoe.smb.debug=true" ./ZoeFileManager
Q_LOGGING_CATEGORY(lcSmb, "zoe.smb", QtWarningMsg)

// libsmbclient 返回的文件名 bytes 的编码取决于 server 的 dos charset 配置：
// - 大多数现代 Samba/Windows SMB2+ 返回 UTF-8
// - 老服务器或简体中文 Windows 可能返回 GBK / CP936
// 这里"UTF-8 优先、失败回退 GBK"做容错。
static QString decodeSmbName(const char* raw) {
    if (!raw) return QString();
    const QByteArray bytes(raw);
    // 先尝试 UTF-8：用 QStringDecoder 严格模式（碰到非法字节序列会 setError）
    QStringDecoder utf8Dec(QStringDecoder::Utf8,
                           QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInvalidToNull);
    QString s = utf8Dec.decode(bytes);
    if (!s.contains(QChar(0))) {
        // 进一步启发式：纯 ASCII 一定 OK；含非 ASCII 字节且 UTF-8 解码后含
        // 大量"未识别"字符则视为非 UTF-8
        bool allAscii = true;
        for (unsigned char c : bytes) {
            if (c >= 0x80) { allAscii = false; break; }
        }
        if (allAscii) return s;
        // 非 ASCII 但 UTF-8 解出没有 \0：很可能是合法 UTF-8
        // （UTF-8 自描述强，误判率低）
        return s;
    }
    // UTF-8 解码失败 → 尝试 GBK
    QStringDecoder gbkDec("GBK");
    if (gbkDec.isValid()) {
        QString g = gbkDec.decode(bytes);
        if (!g.isEmpty()) return g;
    }
    // 都不行：退回 fromLocal8Bit
    return QString::fromLocal8Bit(raw);
}

// 把"人类填写"的 share 路径片段做 percent-encoding：
//   "文体协会"        → "%E4%B8%87%E4%BD%93%E5%8D%8F%E4%BC%9A"
//   "share/sub a"     → "share/sub%20a"
// libsmbclient 的 URL 解析器对非 ASCII 与空格不稳健，需要预先编码。
// 我们按 '/' 拆段，对每段单独 QUrl::toPercentEncoding，避免把分隔符也编码掉。
static QString encodeSmbPath(const QString& raw) {
    QString trimmed = raw;
    while (trimmed.startsWith('/')) trimmed.remove(0, 1);
    while (trimmed.endsWith('/'))   trimmed.chop(1);
    if (trimmed.isEmpty()) return trimmed;
    const auto parts = trimmed.split('/', Qt::SkipEmptyParts);
    QStringList out;
    out.reserve(parts.size());
    for (const auto& p : parts) {
        // 保留 ASCII 字母/数字/常见 URL 安全字符；其它都 percent-encode
        out << QString::fromUtf8(QUrl::toPercentEncoding(p));
    }
    return out.join('/');
}

// 把 errno → 更可诊断的字符串。libsmbclient 经常用 EAGAIN(11) 表示
// "鉴权失败 / 协商失败"，原始 strerror 文案"Resource temporarily unavailable"
// 对用户毫无信息量，这里做一次翻译。
static QString smbStrError(int e) {
    switch (e) {
    case EAGAIN:        return "Resource temporarily unavailable (常见原因：用户名/密码错误、服务器拒绝匿名访问、或 SMB 协议协商失败)";
    case EACCES:        return "Permission denied (鉴权被拒绝；请检查用户名/密码/共享权限)";
    case ECONNREFUSED:  return "Connection refused (端口未开放或被防火墙拦截)";
    case EHOSTUNREACH:  return "Host unreachable (无法到达主机)";
    case ETIMEDOUT:     return "Timed out (连接超时)";
    case ENOENT:        return "No such file or share (共享名不存在)";
    case EINVAL:        return "Invalid argument (URL 或参数格式错误)";
    case ENODEV:        return "No such device (服务器未启用 SMB 服务)";
    default:            return QString::fromLocal8Bit(strerror(e))
                            + QString(" (errno=%1)").arg(e);
    }
}

// ---- 静态凭证仓库：libsmbclient 回调不带 user-data 指针，我们用 ctx 地址
// 做 key 维护一张全局表。 ----
static QHash<SMBCCTX*, SmbClient::Creds>& credsStore() {
    static QHash<SMBCCTX*, SmbClient::Creds> s;
    return s;
}
static QMutex& credsMutex() {
    static QMutex m;
    return m;
}

SmbClient::Creds& SmbClient::credsFor(SMBCCTX* ctx) {
    QMutexLocker lock(&credsMutex());
    return credsStore()[ctx];
}

void SmbClient::clearCreds(SMBCCTX* ctx) {
    QMutexLocker lock(&credsMutex());
    credsStore().remove(ctx);
}

// libsmbclient 回调：在需要凭证时被调用，把结果写回 wg/un/pw 缓冲区。
void SmbClient::authCallback(SMBCCTX* ctx,
                             const char* /*srv*/, const char* /*shr*/,
                             char* wg, int wglen,
                             char* un, int unlen,
                             char* pw, int pwlen) {
    QMutexLocker lock(&credsMutex());
    auto it = credsStore().find(ctx);
    if (it == credsStore().end()) return;
    const Creds& c = it.value();

    auto copyTo = [](char* buf, int buflen, const QByteArray& src) {
        int n = qMin(buflen - 1, static_cast<int>(src.size()));
        if (n < 0) n = 0;
        memcpy(buf, src.constData(), n);
        buf[n] = '\0';
    };
    copyTo(wg, wglen, c.workgroup.toUtf8());
    copyTo(un, unlen, c.user.toUtf8());
    copyTo(pw, pwlen, c.password.toUtf8());
}

SmbClient::SmbClient() = default;

SmbClient::~SmbClient() {
    disconnect();
}

bool SmbClient::connectAndAuth(const RemoteConnection& conn) {
    // 重建 context
    if (m_ctx) disconnect();

    SMBCCTX* ctx = smbc_new_context();
    if (!ctx) {
        m_lastError = "smbc_new_context failed";
        return false;
    }

    // 先记录凭证，再 init（init 内部可能就需要 auth 回调）
    // workgroup 必须非空：未填时退回 WORKGROUP（libsmbclient 期望非空字串）
    QString wg = conn.workgroup.isEmpty() ? QStringLiteral("WORKGROUP") : conn.workgroup;
    {
        QMutexLocker lock(&credsMutex());
        credsStore()[ctx] = Creds{
            /*user*/      conn.username,
            /*workgroup*/ wg,
            /*password*/  conn.password
        };
    }
    smbc_setFunctionAuthDataWithContext(ctx, &SmbClient::authCallback);
    smbc_setDebug(ctx, 0);
    smbc_setOptionUseKerberos(ctx, 0);
    smbc_setOptionFallbackAfterKerberos(ctx, 1);
    // 不要在每次重新连接同一 server 时丢弃 tree connection ——
    // 这样首次 opendir(share) 建立的认证 session 可以在后续 opendir(share/path) 时复用，
    // 避免企业 AD 域服务器对"新 session"再次要求匿名 setup 而触发 LOGON_FAILURE。
    smbc_setOptionOneSharePerServer(ctx, 0);
    smbc_setOptionUseCCache(ctx, 0);

    if (!smbc_init_context(ctx)) {
        m_lastError = "smbc_init_context failed";
        clearCreds(ctx);
        smbc_free_context(ctx, 1);
        return false;
    }
    smbc_set_context(ctx);
    m_ctx = ctx;

    // libsmbclient 的认证有一个微妙陷阱：如果 share 字段带子路径（比如
    // "tfs/文体协会/腾讯电影协会-影音博物馆"），直接 opendir 完整 URL 会让
    // libsmbclient 在内部对该路径所在的 share 做一次新的 session setup，
    // 而那次 setup 在某些 AD 服务器上不会复用我们的 user/password 凭证，
    // 导致 "NT_STATUS_LOGON_FAILURE / Could not resolve ..."。
    //
    // 解决：把 share 拆成 [first_segment, rest]，先 opendir 到 share 根
    // (smb://host/<first>) 建立认证好的 tree connection，再 opendir 完整路径
    // 复用同一个 connection。
    QString firstShare = conn.share;
    QString restPath;
    {
        QString trimmed = conn.share;
        while (trimmed.startsWith('/')) trimmed.remove(0, 1);
        const int slash = trimmed.indexOf('/');
        if (slash >= 0) {
            firstShare = trimmed.left(slash);
            restPath   = trimmed.mid(slash + 1);
        }
    }

    // 第 1 步：opendir 到 share 根，用 user/password 完成 session setup
    QString rootUrl = buildUrl(conn.host, firstShare);
    errno = 0;
    int fd = smbc_opendir(rootUrl.toUtf8().constData());
    if (fd < 0) {
        const int e = errno;
        m_lastError = QString("opendir %1 failed: %2").arg(rootUrl, smbStrError(e));
        disconnect();
        return false;
    }
    smbc_closedir(fd);

    // 第 2 步：若用户填了子路径，验证它也能进，复用已建立的 session
    if (!restPath.isEmpty()) {
        QString fullUrl = buildUrl(conn.host, firstShare, restPath);
        errno = 0;
        int fd2 = smbc_opendir(fullUrl.toUtf8().constData());
        if (fd2 < 0) {
            const int e = errno;
            m_lastError = QString("opendir %1 failed: %2").arg(fullUrl, smbStrError(e));
            disconnect();
            return false;
        }
        smbc_closedir(fd2);
    }

    m_lastError.clear();
    return true;
}

QVector<FileEntry> SmbClient::listDirectory(const QString& url) {
    QVector<FileEntry> out;
    if (!m_ctx) {
        m_lastError = "not connected";
        return out;
    }

    // 上游传来的 URL 可能已是 percent-encoded（我们 listDirectory 返回的
    // FileEntry.path 用的就是 server 原生字节编码后 percent-encode），也可能
    // 是裸 UTF-8（用户从 PathBar 手动输入）。
    //
    // 处理策略：检查每个 path 段是否含 '%' —— 若含则视为已编码、原样透传；
    // 若不含则按 UTF-8 字节 percent-encode（这是用户输入路径的常见情形，
    // 至少能让 server 是 UTF-8 编码的场景能跑通）。
    //
    // 关键：**不要** decode 再 encode —— decode 出的字节序列未必是 UTF-8
    // （server 可能是 GBK），用 fromPercentEncoding 拿到字节后再 toUtf8 会
    // 把原始字节按 UTF-8 错误重编，导致子目录路径与 server 端实际名字对不上。
    QString safeUrl = url;
    if (safeUrl.startsWith("smb://")) {
        QString rest = safeUrl.mid(QStringLiteral("smb://").size());
        int slash = rest.indexOf('/');
        QString host = (slash >= 0) ? rest.left(slash) : rest;
        QString path = (slash >= 0) ? rest.mid(slash + 1) : QString();
        QStringList parts;
        for (const auto& p : path.split('/', Qt::SkipEmptyParts)) {
            if (p.contains('%')) {
                // 已编码，保留
                parts << p;
            } else {
                // 裸 UTF-8 字符，按 UTF-8 字节 percent-encode
                parts << QString::fromLatin1(QUrl::toPercentEncoding(p.toUtf8()));
            }
        }
        safeUrl = "smb://" + host + (parts.isEmpty() ? "" : "/" + parts.join('/'));
    }

    qCDebug(lcSmb) << "listDirectory: input url =" << url
                   << "safeUrl =" << safeUrl;

    errno = 0;
    int fd = smbc_opendir(safeUrl.toUtf8().constData());
    if (fd < 0) {
        const int e = errno;
        m_lastError = QString("opendir %1 failed: %2").arg(safeUrl, smbStrError(e));
        qCWarning(lcSmb) << "opendir failed:" << m_lastError;
        return out;
    }

    int rawCount = 0;
    struct smbc_dirent* de = nullptr;
    while ((de = smbc_readdir(fd)) != nullptr) {
        ++rawCount;
        QString name = decodeSmbName(de->name);
        qCDebug(lcSmb) << "  dirent[" << rawCount << "] type=" << de->smbc_type
                       << "namelen=" << de->namelen
                       << "name=" << name
                       << "rawHex=" << QByteArray(de->name, de->namelen).toHex();
        if (name == "." || name == "..") continue;

        FileEntry e;
        e.name = name;
        // 构造 item URL：父 URL（已编码）+ 子名编码。
        //
        // 关键：percent-encode 必须用 dirent->name 的**原始字节**，而不是
        // 重新 UTF-8 编码 QString。原因：server 传过来的字节流是 server 的
        // dos charset（可能是 GBK），libsmbclient 后续 opendir 比较时也用
        // 这个原始字节序列。如果我们 decode 成 Unicode 再用 UTF-8 重编，
        // 字面就和 server 上的实际名字不匹配，进入子目录会得到空内容。
        const QByteArray rawEncoded = QUrl::toPercentEncoding(
            QByteArray(de->name));
        QString itemUrl = safeUrl;
        if (!itemUrl.endsWith('/')) itemUrl += '/';
        itemUrl += QString::fromLatin1(rawEncoded);
        e.path = itemUrl;

        // 尝试 stat 拿 size/mtime/isDir
        struct stat st{};
        if (smbc_stat(itemUrl.toUtf8().constData(), &st) == 0) {
            e.isDirectory = S_ISDIR(st.st_mode);
            e.size = st.st_size;
            e.modified = QDateTime::fromSecsSinceEpoch(st.st_mtime);
            // 权限字符串
            char perm[11];
            perm[0]  = e.isDirectory ? 'd' : '-';
            perm[1]  = (st.st_mode & S_IRUSR) ? 'r' : '-';
            perm[2]  = (st.st_mode & S_IWUSR) ? 'w' : '-';
            perm[3]  = (st.st_mode & S_IXUSR) ? 'x' : '-';
            perm[4]  = (st.st_mode & S_IRGRP) ? 'r' : '-';
            perm[5]  = (st.st_mode & S_IWGRP) ? 'w' : '-';
            perm[6]  = (st.st_mode & S_IXGRP) ? 'x' : '-';
            perm[7]  = (st.st_mode & S_IROTH) ? 'r' : '-';
            perm[8]  = (st.st_mode & S_IWOTH) ? 'w' : '-';
            perm[9]  = (st.st_mode & S_IXOTH) ? 'x' : '-';
            perm[10] = '\0';
            e.permissions = QString::fromLatin1(perm);
        } else {
            // stat 失败，但 dirent 类型码可用
            e.isDirectory = (de->smbc_type == SMBC_DIR
                             || de->smbc_type == SMBC_FILE_SHARE
                             || de->smbc_type == SMBC_SERVER
                             || de->smbc_type == SMBC_WORKGROUP);
            e.size = 0;
        }
        out.append(e);
    }
    smbc_closedir(fd);
    qCDebug(lcSmb) << "listDirectory: total raw dirents =" << rawCount
                   << "returned entries =" << out.size();
    return out;
}

void SmbClient::disconnect() {
    if (!m_ctx) return;
    clearCreds(m_ctx);
    smbc_free_context(m_ctx, /*shutdown_ctx=*/1);
    m_ctx = nullptr;
}

QString SmbClient::buildUrl(const QString& host, const QString& share,
                            const QString& path) {
    QString url = "smb://";
    url += host;   // host 通常是 ASCII，无需编码
    if (!share.isEmpty()) {
        url += '/';
        url += encodeSmbPath(share);   // 支持多级路径如 "tfs/中文/子目录"
    }
    if (!path.isEmpty()) {
        if (!url.endsWith('/')) url += '/';
        url += encodeSmbPath(path);
    }
    return url;
}
