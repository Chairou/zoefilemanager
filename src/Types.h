#ifndef TYPES_H
#define TYPES_H

// =============================================================================
// Types.h —— 全应用共享的数据结构与枚举定义
//
// 这里集中放置 UI 层、文件系统层、SFTP 层、剪贴板等模块都要互相传递的
// "纯数据" 类型。原则：
//   1. 只放 POD / 简单结构体，不写业务逻辑（避免循环依赖）。
//   2. 字段尽量提供合理默认值，避免"忘记初始化"导致的未定义行为。
//   3. 跨模块传递 FileEntry 时使用值语义（QVector 浅拷贝代价低）。
// =============================================================================

#include <QString>
#include <QDateTime>
#include <QVector>
#include <optional>

/**
 * 文件类型分类 —— 用于决定列表视图里显示哪种小图标 / 颜色。
 *
 * 通过文件扩展名启发式推断（见 RealFileSystem::guessFileType），
 * 不读取 magic bytes，只是 UI 层装饰用，不参与权限/操作判断。
 */
enum class FileType {
    Directory,   // 目录
    Document,    // 办公文档：pdf/doc/docx/pptx/xls/xlsx
    Image,       // 图片：jpg/png/gif/svg/bmp/ico ...
    Code,        // 源码：cpp/h/py/ts/js/html/css ...
    Archive,     // 压缩包：zip/tar/gz/rar/7z/dmg
    Audio,       // 音频：mp3/flac/wav/ogg/aac
    Video,       // 视频：mp4/avi/mkv/mov/wmv
    Text,        // 纯文本：txt/md/csv/log
    Config,      // 配置：conf/cfg/ini/yaml/yml/json
    Script,      // shell 脚本：sh/bash/zsh
    Binary,      // 二进制可执行（保留，目前未启发式归类）
    Unknown      // 未识别 —— 默认箱
};

/**
 * 单个文件/目录条目 —— 列表显示与剪贴板操作的最小单位。
 *
 * `path` 既可能是本地绝对路径（"/Users/foo/bar"）也可能是 SFTP URL
 * （"sftp://user@host:22/pub/file"），由 FileSystemRouter 根据前缀路由。
 *
 * `permissions` 是 9 位 rwx 字符串（POSIX 风格，"rwxr-xr-x"），便于
 * 直接展示给用户，避免每次重新解析 mode_t。
 */
struct FileEntry {
    QString name;                            // 显示名（不含目录前缀）
    QString path;                            // 绝对路径或 sftp:// URL
    bool isDirectory = false;
    qint64 size = 0;                         // 文件字节数；目录约定填 4096
    QDateTime modified;                      // 最后修改时间（远程可能为空）
    QString permissions = "rwxr-xr-x";       // POSIX 风格权限字串
    FileType type = FileType::Unknown;
};

/**
 * 单个标签页（tab）的浏览状态 —— FilePanel 在每个 tab 上各持一份。
 *
 * back/forward 历史按时间顺序保存路径字符串；前进/后退操作就是把当前
 * 路径压入对方栈、再从本方栈弹出最新值。selectedIndices 用于在
 * tab 切换后恢复行选中（暂未启用，预留）。
 */
struct TabState {
    QString currentPath;
    QStringList backHistory;
    QStringList forwardHistory;
    QVector<int> selectedIndices;
};

/**
 * 应用内"剪贴板" —— 与系统剪贴板独立。
 *
 * 由 MainWindow::onCopy / onCut 写入，由 onPaste 读取并清空（仅 cut）。
 * 不直接持有临时副本，只记录源路径列表 + 操作类型。
 */
struct ClipboardData {
    QVector<FileEntry> entries;     // 被复制/剪切的条目
    bool isCut = false;             // true=剪切（粘贴后删除源），false=复制
    QString sourcePath;             // 操作发起时的源目录（用于状态栏显示）
};

/**
 * SFTP 连接配置 —— RemoteDialog 收集，SftpClient 消费。
 *
 * 认证策略（见 SftpClient::connectAndAuth）：
 *   1. 若 privateKeyPath 非空 → 优先尝试公钥认证；
 *   2. 否则（或公钥认证失败但同时填了 password）→ 回退到密码认证。
 * fingerprint 字段在登录成功后写回（SHA256，用于在状态栏展示
 * 主机指纹，未来可作 TOFU 信任锚）。
 */
struct RemoteConnection {
    QString name;                   // 用户给这条连接起的别名（保存连接时用）
    QString protocol;               // "SFTP" / "SMB"
    QString host;
    int port = 22;
    QString username;
    QString password;
    QString privateKeyPath;         // 可选：私钥文件路径，给了就优先用公钥
    QString passphrase;             // 可选：私钥的 passphrase
    QString fingerprint;            // 登录成功后回填（SHA256）
    // ---- SMB 专用 ----
    QString share;                  // SMB 共享名（如 "Public"）。SFTP 忽略。
    QString workgroup;              // 可选：Windows 工作组/域名。SFTP 忽略。
};

/**
 * 双面板的"哪一侧" —— 大量代码用它判断左右
 * （状态栏文案、面包屑跳转、拖放高亮颜色等）。
 */
enum class PanelSide {
    Left,
    Right
};

#endif // TYPES_H
