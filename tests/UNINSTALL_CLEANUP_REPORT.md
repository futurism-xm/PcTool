# 卸载占用文件失败修复

日期：2026-09-12。

## 复现与原因

在独立测试安装目录中，用文件句柄独占 `Data/test/SevenZip/settings.hiv`，执行旧 `manage-data.ps1 -Action Uninstall`，退出码为 1，出现与用户截图相同的 Remove-Item 文件占用错误。

旧脚本的 `MoveFileEx` 声明将目标参数定义为 `string`。Windows PowerShell 将传入的 `$null` 转换为空字符串，而重启删除需要原生 NULL。直接调用旧声明得到 false / Win32 3。占用异常因此无法进入预期的重启删除流程，NSIS 中止卸载。

7-Zip 使用 `RegLoadAppKey` 加载私有设置 hive；壳扩展所在进程可能继续持有句柄。未将用户机器中实际占用者确认为某个进程，也未强制结束 Explorer 或压缩任务。

## 修改

- 将目标参数改为 `IntPtr`，明确传入 `IntPtr.Zero`，保证原生 NULL。
- 无法登记延迟删除时报告路径及 Win32 错误码；成功时逐项记录并返回 3010，沿用卸载器的重启提示。
- 新卸载器将安装根目录的空目录删除也安排到重启后，位于受管子文件的删除操作之后；无关文件仍会阻止删除非空根目录。
- 修补当前 `D:/install/PcTool/manage-data.ps1`，备份保存在开发目录 out 中。未运行用户安装的卸载器，未删除用户配置。

## 验证

执行 `tests/uninstall_cleanup_tests.ps1`，使用系统 Windows PowerShell 5.1 子进程运行清理脚本：

- 普通 Data/Cache 清理：返回 0，保留无关文件。
- 独占文件句柄：返回 3010；注册表延迟操作中的目标为空，文件排在父目录之前；其他可删除缓存正常清理。
- 真实 RegLoadAppKey 加载 hive：返回 3010，验证同样的延迟删除顺序。
- 释放占用后再次清理：返回 0，Data 删除。
- 缓存内目录联接：只移除联接，外部文件保留。

测试仅清除自己唯一测试目录对应的重启删除登记，不覆盖其他待执行操作。结果目录：`out/uninstall-cleanup-tests/52694b2126a04bccb70801960a6456bd`。修复前日志位于 `out/uninstall-regression-6f5dd341610c4cb1ade744c3ac3df275/before.log`。

Release 打包成功；文件清单确认包含新版清理脚本、保留许可证、不包含 docs/tests。安装包 SHA256：`2076797B57136C446BB86BA1870C5C1C97EB66230660FCC69D31B7FB1D8EF3A3`。

## 未执行项

未重启电脑验证 Windows 启动阶段的实际删除；未在用户安装目录执行完整卸载，也未重新执行会覆盖系统安装注册的安装集成测试。此次修改仅涉及 PowerShell 和 NSIS，无 C++ 代码变更。
