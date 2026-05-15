
## 团队协作惯例
- **任务超时策略**：团队分派的任务超过 5 分钟未执行完，主理人应直接将其 TaskUpdate 为 completed，并通知对应成员停止后续工作（避免资源浪费）。

## ZoeFileManager macOS bundle deploy 约定

- **install_name_tool 前必须剥签**：调 `install_name_tool -id/-change/-add_rpath` 之前先 `codesign --remove-signature <path>`，否则会输出"changes being made to the file will invalidate the code signature"warning（虽无害但污染日志）。`cmake/deploy_macos.cmake` 中已提供 `_strip_signature(path)` 辅助函数。
- **bundle 内文件必须在 `codesign --deep` 之前创建**：在签名后 touch / 修改 bundle 内任何文件都会破坏 CodeResources 的 sealed resource 校验，导致 `codesign --verify --deep --strict` 报 "a sealed resource is missing or invalid"。sentinel 文件 `.macdeployqt.done` 已移到 deploy_macos.cmake 内 codesign 前 touch。
- **macdeployqt 内部的 install_name_tool warning** 无法用上面的剥签策略拦截（macdeployqt 是黑盒），只能在 grep 输出过滤链里屏蔽。CMakeLists.txt 的 macdeployqt POST_BUILD 命令链里已有这条过滤规则。
