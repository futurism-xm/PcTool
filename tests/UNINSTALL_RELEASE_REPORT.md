# 卸载前释放占用测试报告

日期：2026-09-14；交付版本：0.1.2（重新打包）。

## 调整

- 卸载先结束本次安装的 PcTool 和内置 7-Zip，等待真实进程退出；未成功时停止文件删除，保留卸载重试入口。
- 撤销扩展注册后释放本次安装的 Explorer 扩展宿主，再复查所属进程，之后执行文件清理。
- 删除了“原 Explorer 为提升权限进程便跳过”的分支。复制宿主的原用户令牌，保留用户、会话及权限；不使用卸载器令牌替代。
- 先创建挂起的恢复进程，准备失败则不终止原宿主。原宿主退出后，如果 Windows 已恢复桌面，丢弃备用进程；否则恢复备用进程运行，并检查桌面窗口是否存活。
- 扩展模块枚举遇到 ERROR_BAD_LENGTH 时短暂重试，释放后再次检查是否仍加载扩展。
- 扩展辅助程序返回待处理状态不再直接设置 NSIS 重启标志；由文件删除后的实际结果确定。
- 删除失败使用全局共享的三秒等待重试预算，避免瞬时占用立即进入重启删除，也避免大量文件逐个等待。文件已被其他清理步骤删除时按成功处理。
- 持续的外部占用仍保留可靠的延期删除和具体文件路径记录。没有强行关闭无关进程的句柄，也没有结束其他应用或其他登录会话的桌面。

## 已执行

| 测试 | 结果 |
| --- | --- |
| Debug/Release 原生辅助程序及宿主测试构建 | 通过 |
| 测试进程真实加载 7-Zip DLL，删除时确实被占用 | 通过 |
| 恢复准备失败时原宿主仍运行 | 通过 |
| 终止宿主、确认退出、恢复原 SID/会话/权限，再立即删除 DLL | Debug/Release 通过，原进程 elevated=1 |
| 礼貌退出、未响应进程强制退出、多实例、同名其他安装保留 | 通过，共享退出耗时约 2.1 秒 |
| 进程退出后设置 hive 立即可删 | 通过 |
| 真实 PcTool 和内置 7-Zip 在卸载标记存在时拒绝启动 | 通过 |
| 文件真实独占后，在删除重试中释放 | 返回 0，文件删除，无重启队列条目 |
| 持续文件独占、持续加载 hive | 正确返回 3010，延期删除按文件至父目录登记 |
| 升级清单、旧 docs/tests、无关文件、路径越界、联接、部分失败及重试 | 通过 |
| 完整 NSIS 隔离安装、升级、带未响应任务卸载 | 两组通过，5.18 秒 / 4.74 秒，返回 0 |
| 无既有 7-Zip 注册 / 有既有第三方注册 | 隔离注册命名空间分别验证清理 / 恢复 |

完整 NSIS 测试检查了进程退出码、程序/DLL/模型/许可证/Data/Cache/卸载器等文件消失、测试快捷方式和注册项清理，以及无关文件保留；不只检查卸载返回值。

## 测试范围与复测

宿主恢复测试使用独立隐藏进程加载真实扩展 DLL，直接执行与 Explorer 相同的令牌复制、挂起创建、终止、恢复代码；没有重启当前 Explorer。未实际验证 Windows 桌面自动恢复分支、非提升权限 Explorer、远程/多登录会话以及系统重启后的延期删除执行。延期队列测试结束时只移除本测试专用目录的条目。

完整安装测试将注册和快捷方式重定向到隔离命名空间，没有卸载用户当前安装，也没有中断用户无关任务。

```powershell
cmake --build cmake-build-release --target PcToolUninstallHelper PcToolUninstallHostTests PcToolUninstallFixture
# 最后一个参数必须为新建的独立测试目录
./cmake-build-release/PcToolUninstallHostTests.exe ./cmake-build-release/PcToolUninstallFixture.exe ./out/archive-runtime/7-zip.dll ./out/new-host-fixture
./tests/uninstall_priority_tests.ps1 -Binaries ./cmake-build-release
./tests/uninstall_cleanup_tests.ps1
# 先构建最新安装包，再执行隔离安装测试
./tests/uninstall_installer_isolated.ps1 -Binaries ./cmake-build-release
```

本次日志：`out/uninstall-host-{debug,release}.log`、`out/uninstall-release-handles-{cleanup,priority,debug-priority,installer-test,package}.log`。日志及测试文件不进入安装载荷。

交付：`out/installer/PcTool-0.1.2-x64-Setup.exe`，包含本轮卸载辅助程序和清理脚本，保留之前的 OCR 内存修复及无 SID 层的七天日期缓存。

## 用户实际残留场景补充

用户反馈安装后未运行即卸载，留下 `D:\install\PcTool\Data` 和 `modules`。只读检查确认残留为 `7-zip.dll` 及 `Data/SevenZip/settings.hiv`、LOG1、LOG2；两个同会话 Explorer 进程（4332、7424）实际加载了这份 DLL，PcTool 没有运行。壳扩展会由 Explorer 加载，因此不要求先启动 PcTool。

在用户已授权释放资源管理器占用的范围内，使用本轮辅助程序对该残留路径执行释放。两个原进程退出，新的 Explorer 10824 未加载该扩展，四个残留文件均能独占只读打开；没有重启电脑。证据：`out/uninstall-live-release.log` 及 inspect 命令输出。旧卸载器已被删除，无法核对其二进制版本，不将当前释放成功等同于完整安装/卸载重现。

同时修复延期卸载的后续流程：文件尚未清理完时，保留卸载器、辅助程序、清理脚本、清单和卸载注册，释放占用后可以直接重试；不再销毁重试入口。隔离 NSIS 测试增加真实外部文件锁，验证第一次返回 3010 时重试文件仍在，释放后第二次卸载返回 0，受管文件与注册最终清理完成。测试仅移除其独立目录的延期队列记录，不改动用户原有队列。

测试方式也已修正：NSIS 默认自复制卸载器的外层进程可能先返回，不能用它的返回值替代实际清理结果。当前在安装目录外运行卸载器副本，传入 `_?=`，等待实际清理进程；分别断言持续占用为 3010、释放后重试为 0，并检查文件和注册。前文约五秒的旧测试结果不能作为实际卸载子进程退出码证据；最新证据为 `out/uninstall-residual-installer-test.log`。

新增 `inspect` 只读诊断，以及用于主程序已卸载、DLL 和 hive 仍残留场景的 `repair-shell` 扩展释放命令。两者均不会删除残留文件。

对用户目录残留文件的实际删除被自动审批拒绝（仅返回 blocked by policy），没有换用其他方式删除。实际占用已释放，但残留目录尚未由本轮工具删除。
