// =============================================================================
// I18n.cpp —— 见 I18n.h
//
// 词典在本文件末尾集中维护。key=英文原文；值=中文译文。
// 新增翻译条目时：往 kZh 表里加 key→译文即可。
// =============================================================================

#include "I18n.h"
#include <QSettings>
#include <QLocale>
#include <QHash>

namespace {
// 中文词典：key 必须与源码里 T("...") 的 key 完全一致（英文原文）。
// 英文语言直接返回 key，无需词典。
const QHash<QString, QString>& zhDict() {
    static const QHash<QString, QString> kZh = {
        // ---- 工具栏 ----
        {"\xE2\x86\x90 Back",     "\xE2\x86\x90 \xE5\x90\x8E\xE9\x80\x80"},
        {"\xE2\x86\x92 Fwd",      "\xE2\x86\x92 \xE5\x89\x8D\xE8\xBF\x9B"},
        {"\xE2\x86\x91 Up",       "\xE2\x86\x91 \xE4\xB8\x8A\xE4\xB8\x80\xE7\xBA\xA7"},
        {"\xE2\x9F\xB3 Refresh",  "\xE2\x9F\xB3 \xE5\x88\xB7\xE6\x96\xB0"},
        {"Copy",                   "复制"},
        {"Cut",                    "剪切"},
        {"Paste",                  "粘贴"},
        {"Delete",                 "删除"},
        {"Select All",             "全选"},
        {"Copy Path",              "复制路径"},
        {"Copy File Name",         "复制文件名"},
        {"Script",                 "脚本"},
        {"Search",                 "搜索"},
        {"Connect",                "连接"},
        {"Disconnect",             "断开连接"},
        {"\xE2\x97\x89 Hidden",    "\xE2\x97\x89 \xE9\x9A\x90\xE8\x97\x8F"},
        {"Theme: Dark",            "主题：深色"},
        {"Theme: Light",           "主题：浅色"},

        // ---- 菜单 ----
        {"Settings",               "设置"},
        {"Settings…",              "设置…"},
        {"Help",                   "帮助"},
        {"About",                  "关于"},
        {"About Zoe File Manager", "关于 Zoe 文件管理器"},
        {"Language",               "语言"},
        {"English",                "English"},
        {"中文 (Simplified)",       "中文（简体）"},
        {"Font Size",              "字体大小"},
        {"Menu / Toolbar:",        "菜单 / 工具栏："},
        {"File / Folder name:",    "文件 / 文件夹名："},
        {"0 = use default; recommended 13–18 pt.",
            "0 = 使用默认；建议 13–18 pt。"},
        {"More",                   "更多"},

        // ---- 右键菜单 ----
        {"Open",                    "打开"},
        {"Open With",               "打开方式"},
        {"Default Application",     "默认应用"},
        {"Other Application…",      "其它应用…"},
        {"New File…",               "新建文件…"},
        {"New Folder…",             "新建文件夹…"},
        {"Rename…",                 "重命名…"},
        {"Run Script",              "运行脚本"},

        // ---- 对话框标题 / 按钮 ----
        {"New File",                "新建文件"},
        {"New Folder",              "新建文件夹"},
        {"Rename File",             "重命名文件"},
        {"Rename Folder",           "重命名文件夹"},
        {"Name:",                   "名称："},
        {"New name:",               "新名称："},
        {"Invalid Name",            "名称无效"},
        {"Name contains invalid characters.", "名称包含非法字符。"},
        {"Create Failed",           "创建失败"},
        {"Cannot create folder.",   "无法创建文件夹。"},
        {"Rename Failed",           "重命名失败"},
        {"The item no longer exists.", "该条目已不存在。"},
        {"Overwrite?",              "是否覆盖？"},
        {"Not Supported",           "暂不支持"},
        {"Creating files/folders on remote paths is not supported yet.",
            "远程路径暂不支持创建文件/文件夹。"},
        {"Renaming on remote paths is not supported yet.",
            "远程路径暂不支持重命名。"},

        // ---- 状态栏模板（格式化前的纯文本片段） ----
        {"Panel:",                  "面板："},
        {"Left",                    "左"},
        {"Right",                   "右"},
        {"Clipboard:",              "剪贴板："},
        {"Selected:",               "已选中："},
        {"folders",                 "个文件夹"},
        {"files",                   "个文件"},
        {"item(s)",                 "项"},
        {"File name(s) copied",     "文件名已复制"},

        // ---- 快捷栏 / 搜索 / 远程等通用 ----
        {"Search…",                 "搜索…"},
        {"Remote Connect",          "远程连接"},
        {"Remote Disconnect",       "断开远程"},

        // ---- SearchDialog ----
        {"Search Files",            "搜索文件"},
        {"Search:",                 "搜索："},
        {"In path:",                "在路径："},
        {"Enter search term...",    "输入要搜索的关键字..."},
        {"Enter a search term and click Search", "输入搜索关键字后点击\"搜索\""},

        // ---- ScriptDialog ----
        {"Run Script",              "运行脚本"},
        {"WARNING: Commands will be executed on your real filesystem!",
            "警告：命令将在真实文件系统上执行！"},
        {"Selected Files",          "已选文件"},
        {"Command",                 "命令"},
        {"Output",                  "输出"},
        {"Quick commands:",         "快捷命令："},
        {"Enter command to execute...", "输入要执行的命令..."},
        {"Run",                     "运行"},
        {"Close",                   "关闭"},
        {"-- Select a quick command --", "-- 选择快捷命令 --"},
        {"(no files selected)",     "（未选择文件）"},

        // ---- AboutDialog ----
        {"All notices",             "全部声明"},
        {"(license text will appear below)", "（许可证文本将在下方显示）"},

        // ---- RemoteDialog ----
        {"Remote Connection",       "远程连接"},
        {"Saved Connections",       "已保存的连接"},
        {"Connection Details",      "连接信息"},
        {"Protocol:",               "协议："},
        {"Host:",                   "主机："},
        {"Port:",                   "端口："},
        {"Username:",               "用户名："},
        {"Password:",               "密码："},
        {"Private key:",            "私钥："},
        {"Key passphrase:",         "私钥口令："},
        {"hostname or IP",          "主机名或 IP"},
        {"username",                "用户名"},
        {"leave empty if using a private key", "使用私钥时可留空"},
        {"optional, e.g. ~/.ssh/id_rsa", "可选，例如 ~/.ssh/id_rsa"},
        {"only if the key is encrypted", "仅在私钥被加密时填写"},
        {"Browse…",                 "浏览…"},
        {"Browse...",               "浏览..."},
        {"Cancel",                  "取消"},
        {"Ready.",                  "就绪。"},
        {"Select private key",      "选择私钥文件"},
        {"Missing host",            "缺少主机"},
        {"Please enter a host name or IP.", "请输入主机名或 IP 地址。"},
        {"Missing user",            "缺少用户名"},
        {"Please enter a username.", "请输入用户名。"},
        {"Missing credentials",     "缺少凭证"},
        {"Provide a password, a private key, or both.",
            "请提供密码、私钥或两者。"},
        {"Connecting to %1@%2:%3 …", "正在连接 %1@%2:%3 …"},
        {"Connection failed",        "连接失败"},
        {"✓ Connected. Host key %1", "✓ 已连接。主机密钥 %1"},
        {"(unknown)",                "（未知）"},

        // ---- ShortcutBar ----
        {"SHORTCUTS",               "快捷栏"},
        {"Add shortcut",            "添加快捷方式"},
        {"Edit shortcut",           "编辑快捷方式"},
        {"Name (optional, defaults to folder name):",
            "名称（可选，默认为目录名）："},
        {"e.g. Projects",           "例如：Projects"},
        {"Directory:",              "目录："},
        {"Select directory",        "选择目录"},
        {"Edit...",                 "编辑..."},
        {"Remove",                  "移除"},
        {"Remove shortcut",         "移除快捷方式"},
        {"Invalid directory",       "无效目录"},
        {"Please choose an existing directory.", "请选择一个存在的目录。"},

        // ---- PathBar ----
        {"Enter path and press Enter (Esc to cancel)",
            "输入路径后回车确认（Esc 取消）"},
        {"Edit path (double-click empty area, or Ctrl+L)",
            "编辑路径（双击空白区域，或 Ctrl+L）"},
        // Toast: ✓ Path copied
        {"✓ Path copied",            "✓ 路径已复制"},

        // ---- FileListView 表头 ----
        {"Name",                    "名称"},
        {"Size",                    "大小"},
        {"Modified",                "修改时间"},
        {"Permissions",             "权限"},

        // ---- RemoteDialog ----
        {"Remote Connection",       "远程连接"},
        {"Saved Connections",       "已保存的连接"},
        {"Connection Details",      "连接信息"},
        {"Protocol:",               "协议："},
        {"Host:",                   "主机："},
        {"Port:",                   "端口："},
        {"Username:",               "用户名："},
        {"Password:",               "密码："},
        {"Private key:",            "私钥："},
        {"Key passphrase:",         "私钥口令："},
        {"hostname or IP",          "主机名或 IP"},
        {"username",                "用户名"},
        {"leave empty if using a private key", "若使用私钥请留空"},
        {"optional, e.g. ~/.ssh/id_rsa",       "可选，例如 ~/.ssh/id_rsa"},
        {"only if the key is encrypted",       "仅当私钥已加密时填写"},
        {"Browse…",                 "浏览…"},
        {"Cancel",                  "取消"},
        {"Ready.",                  "就绪。"},
        {"Select private key",      "选择私钥"},
        {"Missing host",            "缺少主机"},
        {"Please enter a host name or IP.", "请输入主机名或 IP。"},
        {"Missing user",            "缺少用户名"},
        {"Please enter a username.","请输入用户名。"},
        {"Missing credentials",     "缺少凭据"},
        {"Provide a password, a private key, or both.", "请输入密码、私钥或两者。"},
        {"Connecting to %1@%2:%3 …","正在连接 %1@%2:%3 …"},
        {"Connection failed",       "连接失败"},
        {"✓ Connected. Host key %1","✓ 已连接。主机密钥 %1"},
        {"(unknown)",               "（未知）"},
    };
    return kZh;
}

QString kSettingsKey = QStringLiteral("ui/language");
} // namespace

I18n& I18n::instance() {
    static I18n inst;
    return inst;
}

I18n::I18n() {
    // 从 QSettings 读取；缺失则跟随系统 locale。
    QSettings s("WorkBuddy", "ZoeFileManager");
    QString saved = s.value(kSettingsKey).toString();
    if (saved == "zh") {
        m_current = Lang::Zh;
    } else if (saved == "en") {
        m_current = Lang::En;
    } else {
        // 首次启动：跟随系统语言
        auto lang = QLocale::system().language();
        m_current = (lang == QLocale::Chinese) ? Lang::Zh : Lang::En;
    }
}

void I18n::setCurrent(Lang lang) {
    if (lang == m_current) return;
    m_current = lang;
    QSettings s("WorkBuddy", "ZoeFileManager");
    s.setValue(kSettingsKey, lang == Lang::Zh ? "zh" : "en");
    emit changed();
}

QString I18n::tr(const QString& key) const {
    if (m_current == Lang::En) return key;
    const auto& d = zhDict();
    auto it = d.find(key);
    if (it == d.end()) return key;
    return it.value();
}

QString I18n::trWithShortcut(const QString& keyWithTab) const {
    int tab = keyWithTab.indexOf('\t');
    if (tab < 0) return tr(keyWithTab);
    QString head = keyWithTab.left(tab);
    QString tail = keyWithTab.mid(tab);  // 含 \t 和后面的快捷键
    return tr(head) + tail;
}
