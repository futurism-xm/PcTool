# 安装目录统一存储测试报告（2026-09-11）

## 实现

- 共享 app_storage 接口提供基于 EXE 位置的 SID 数据目录、唯一缓存文件、原子写入和安全启动清理。功能设置不再读写旧 Software/PcTool；不迁移或删除旧版本用户数据。
- 截图同时提供 CF_BITMAP 与 CF_HDROP；录屏输出和 partial 文件在 Cache。下载仍使用用户选择路径。GIF 仅预留目录类别。
- 7-zip 实际构建源码使用 RegLoadAppKey 的本地 hive 保存偏好，模块自身定位安装根；MyGetTempPath 改为本地 Cache/SevenZip，不修改 Explorer 环境。活动进程使用缓存租约。
- 安装器配置 Data/Cache 的安装用户、SYSTEM、Administrators 权限。卸载前检查 archive 进程，删除受管数据（不跟随链接），占用文件安排重启并提示；保留额外文件，原有系统注册恢复机制保留。

## 已执行

| 用例 | 结果 |
|---|---|
| EXE 根目录定位、不受工作目录改变影响 | 通过 |
| settings.ini 保存及重启读取；只读文件保存失败且原配置保留 | 通过 |
| 当前用户 SID 隔离路径 | 通过 |
| 缓存剪贴板引用保留、占用保留、过期删除 | 通过 |
| 缓存目录链接不越界清理 | 通过 |
| 安装 Data/Cache ACL 和受管删除、无关文件保留 | 通过（隔离普通目录，Windows PowerShell） |
| 截图 CF_HDROP 文件存在且同时提供 CF_BITMAP | 通过（合成窗口） |
| 录屏预览和丢弃文件回归 | 通过 |
| 翻译源持久化、DPAPI、协议测试及配置窗口 | 通过 |
| 快捷键本地文件保存、冲突、重启、释放注册 | 通过 |
| 7-zip 私有 hive 跨进程读写 | 通过 |
| 实际 7zFM 启动/关闭、原生 shell COM 菜单填充 | 通过（隔离模块副本，生成本地 hive） |
| Debug、Release 构建 | 通过 |

日志：out/storage-*.log。相关原生用例已加入 installation_storage CTest；run_storage_tests.ps1 使用随机隔离目录，不操作旧用户配置。一次快捷键 UI 用例在并行构建期间失败，独立复跑通过；Windows PowerShell Set-Acl 模块加载问题已改用 .NET ACL 接口修复，重跑通过。

## 未实测与边界

- 未在干净虚拟机中完成整套安装/升级/卸载与 Program Files 的真实普通用户登录验证；已执行安装器使用的权限/清理逻辑，实际系统注册恢复沿用现有机制。
- 未实测跨 Windows 用户登录、卸载占用文件的重启删除、第三方应用实际 Ctrl+V、Explorer 菜单命令执行后的完整压缩/解压任务。文件粘贴已验证格式与缓存文件，不能将其写成所有应用粘贴实测通过。
- 不迁移旧开发版本目录；这些历史数据继续留在原处。Windows 维护的注册、DPAPI 系统数据和系统临时记录不宣称全部留在安装目录。
- 本轮没有实现 GIF 录制。

## 2026-09-11：全屏录制工具栏命中

全屏选区的输入层是工具栏的 owned popup，Windows 会将其保持在 owner 之上。原先再次置顶工具栏不能可靠解决鼠标拦截。
现在输入层在工具栏及其可见附属控件范围返回 HTTRANSPARENT，让同线程下方控件响应鼠标；准备选区与录制标注共用此规则。现有边缘拖动继续工作并限制在显示器内。

验证：capture_recording_prepare 增加全屏区域的真实 SendInput 拖动、释放捕获和点击取消断言；capture_recording_ui 覆盖录制标注、音频按钮、工具面板、点击透传和结束预览，两项通过。修正录制测试遗留的 LOCALAPPDATA 路径检查，改为安装目录缓存。全屏准备阶段已用真实鼠标测试，完整全屏录制视频和真实跨显示器拖动未实测。Debug/Release 构建通过，安装包重建。日志 out/fullscreen-bar-final-tests.log。
