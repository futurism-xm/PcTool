# 扩展释放与文件夹窗口恢复

> 历史报告：文件夹记录和恢复功能已按用户最新要求移除，相关测试目标也已删除。以下恢复窗口的行为及命令不再适用于当前版本；扩展正常释放和必要时重启宿主的改进仍保留。最新验证见 `UNINSTALL_NO_FOLDER_RESTORE_REPORT.md`。

日期：2026-09-14。版本仍为 0.1.2，重新构建安装包。

## 行为

- 7-Zip 设置根 hive 改为每次注册表操作持有，子键关闭后不再保留静态根句柄。缓存租约在 DLL 卸载时释放。保留设置持久化和卸载期间拒绝新任务的行为。
- 修正扩展 `LockServer` 计数，`DllCanUnloadNow` 原子读取引用数，由 COM 决定何时卸载；不从外部强行卸载 Explorer 中的 DLL。
- 卸载先发送关联刷新通知，并给正常扩展卸载约 1.8 秒的观察机会。已经没有扩展加载者时直接继续清理，不重启 Explorer；仍占用时才使用原有宿主恢复机制。
- 重启前通过 Shell 接口记录文件夹 PIDL、所属进程和窗口位置。只恢复实际退出进程中的文件夹，已经存在的相同目录不重复打开，安装目录及子目录不恢复。
- 恢复通过独立 Shell 浏览器窗口导航到原 PIDL，尽量恢复窗口位置。信息仅在内存中保存，日志不输出文件夹路径。

## 已验证

| 项目 | 结果 |
| --- | --- |
| 主程序所需辅助程序、恢复测试 Debug / Release 构建 | 通过 |
| 真实 7-Zip COM 菜单对象及 DLL 仍活着时，hive 可独占读取 | 通过 |
| COM LockServer 保持 DLL、平衡释放后 DllCanUnloadNow 返回 S_OK | 通过 |
| 原工厂、菜单和缓存命令的卸载标记拦截 | 通过 |
| 设置 hive 写入、关闭后释放、再次进程读取 | 通过 |
| 文件夹恢复：两个真实 Explorer 测试窗口关闭后恢复目录及窗口位置 | Debug / Release 通过 |
| 独立第三个对照文件夹始终保留，重复恢复不增加窗口 | Debug / Release 通过 |
| 原令牌宿主恢复、准备失败不结束原宿主 | 通过 |
| 隔离 NSIS 安装、升级、持续占用保留重试工具、释放后再次卸载 | 有/无旧 7-Zip 注册两组通过 |
| 包内辅助程序及 8 个 7-Zip 模块哈希 | 与本次构建一致 |
| 安装载荷不含 docs/tests | 通过 |

测试使用独立创建的三个文件夹窗口，只关闭测试创建的窗口，没有重启用户 Explorer 或关闭其无关窗口。完整安装测试继续使用隔离注册命名空间，并等待真正执行清理的卸载进程。

## 已知限制

- 正常卸载 DLL 的时间由 Explorer/COM 控制。引用仍在时需要重启兜底；本轮没有在真实 Explorer 中验证它一定会在 1.8 秒内自行卸载。
- 宿主重启和真实文件夹恢复分别实测，本轮未把用户真实 Explorer 重启与多个窗口恢复组合执行。
- Windows 11 标签页分组、导航历史、窗口内选择状态不保证恢复；Shell 未暴露的窗口或失效的网络/虚拟目录可能无法恢复。恢复目标是接口可枚举的文件夹位置和窗口位置。

日志：`out/uninstall-folders-shell-test.log`、`out/uninstall-folder-restore-{release,debug}-final.log`、`out/uninstall-folders-storage*.log`、`out/uninstall-folders-host-test.log`、`out/uninstall-folders-installer-test.log`、`out/uninstall-folders-package.log`。

复测：

```powershell
cmake --build cmake-build-release --target PcToolExplorerFolderTests PcToolArchiveShellTests PcToolUninstallHostTests
# 使用新的独立输出目录，测试会打开并关闭自己的三个文件夹窗口
./cmake-build-release/PcToolExplorerFolderTests.exe ./out/new-folder-restore-fixture
```

交付：`out/installer/PcTool-0.1.2-x64-Setup.exe`。旧的已加载扩展需通过升级及必要的首次释放退出，之后才能使用新扩展的句柄生命周期。
