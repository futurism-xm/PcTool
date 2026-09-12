# 卸载优先终止与清理验证

日期：2026-09-12。

## 实现

- 卸载与升级分开：卸载不再调用等待封装最多 120 秒的普通退出流程。原生辅助程序设置安装级标记后通知所属窗口退出，共享 2 秒宽限，再按实际进程句柄强制终止未退出的进程并等待退出。
- 按规范化映像路径识别本次安装的 PcTool、7z、7zG、7zFM；重新核对终止句柄的映像，排除同名外部程序。控制台窗口不发送关闭消息，避免影响共享控制台中的其他任务。
- PcTool 所有启动入口、三个内置 7-Zip 程序、壳扩展类工厂、现有菜单及执行命令均检查卸载标记。安装完成后清除标记。
- 先恢复或撤销本次安装的 32/64 位壳扩展注册，再检查加载者。只重启当前交互会话中确实加载本次扩展、且可取得非提升用户令牌的 Explorer；使用原用户令牌恢复，不启动管理员桌面。其他会话或无法安全恢复的情况保留重启清理路径。
- 版本化受管清单在升级时原子合并；保留旧版本文件、旧开发报告路径和确定的旧 DLL 替换文件清理信息。不递归删除安装目录中的任意内容，不跟随重解析点到外部目录。
- 数据、程序文件清理按独立步骤汇总错误；失败保留卸载器、辅助程序、清单及注册备份供重试。3010 表示等待重启，不能算作已经没有残留。无关文件导致目录非空不会单独要求重启。

## 已执行

| 检查 | 结果 |
| --- | --- |
| Debug、Release 主程序及原生辅助程序构建 | 通过 |
| 修改后的 7-Zip 可执行程序、x64/x86 壳扩展构建 | 通过 |
| 正常退出 + 两个无响应进程 + 外部同名进程 | 通过；Release 约 2.06 秒，Debug 约 2.08 秒；核对正常退出码 0、强制结束码 1602 |
| 真实 RegLoadAppKey hive 占用 | 所属进程退出后立即清理，无需重启 |
| PcTool 和三个真实 7-Zip 程序的卸载期启动 | 均返回 1618，没有启动新任务 |
| 已加载的真实 7-Zip COM 扩展 | 标记后不能取得新工厂、创建新实例、生成菜单或执行现有菜单命令 |
| 受管清单跨升级合并、旧文件清理 | 通过；无关文件保留 |
| 越界清单、目录联接 | 越界拒绝；外部文件保留 |
| 部分失败及主程序已移除后的重试 | 保留恢复信息，修正问题后可继续清理 |
| 独占文件、仍加载的 hive、重启队列 | 返回 3010，核对原生 NULL 目标及文件先于父目录的顺序；释放占用后可立即删除 |
| Debug / Release 相关 CTest | 各 3/3：module_boundaries、archive_integration、archive_shell |

完整 NSIS 流程使用生产脚本生成隔离测试安装器，仅替换注册表命名空间、快捷方式目的地及禁止触及开发程序的普通升级退出调用。实际执行安装、覆盖升级、运行持有 hive 的无响应进程、卸载；分别验证没有独立 7-Zip、预先存在独立 7-Zip 注册两种情况。

两种情况均通过：卸载耗时约 6.13 秒和 4.76 秒；核对主程序、辅助程序、卸载器、Data/Cache、模块、模型、许可证及测试快捷方式不存在，两个注册表视图均正确删除或恢复，额外文件保留。不是仅检查可见窗口或卸载退出码。

## 复现与证据

在提升权限的开发测试终端运行：

```powershell
powershell -ExecutionPolicy Bypass -File tests/uninstall_priority_tests.ps1 -Binaries cmake-build-release
powershell -ExecutionPolicy Bypass -File tests/uninstall_cleanup_tests.ps1
# 先生成安装包，供以下测试复用暂存目录与清单
powershell -ExecutionPolicy Bypass -File tests/uninstall_installer_isolated.ps1 -Binaries cmake-build-release
```

- Release 进程及重试：`out/uninstall-priority-tests/438d6d30b4d84e5a9e91573a221a8f24`。
- Debug 进程测试：`out/uninstall-priority-tests/48feaaf1bf70471ea2f1142259b02e58`。
- 完整隔离安装：`out/uninstall-installer-tests/db981c3d8acf4b999dcca72d34e21ab4`。
- 占用清理：`out/uninstall-cleanup-tests/f164f11674af4b0d94f282ba8135e6a1`。
- 打包记录：`out/uninstall-final-package.log`；确认清单包含辅助程序及固定版本 7-Zip 源码，不包含 docs/tests 目录。
- 安装包 SHA256：`8A8CABAFD216A4D2AFEABFD18FC51A41CEE78E3EF952150A8883C14D47A6AD54`。

## 未实测与边界

未重启用户桌面的 Explorer、未重启电脑验证启动阶段删除；未执行真实多用户会话恢复。当前其他会话的 Explorer 占用使用等待重启的路径。

未逐项操作实际录屏、GIF、暂停、预览、OCR、联网翻译和正在写入用户目的地的压缩/解压任务后卸载；已验证共享的进程终止机制、真实 hive 释放、原生入口和实际安装流程，不能将其等同于所有业务场景的人工实测。

未卸载或覆盖用户现有安装，未删除用户文件。现有安装必须使用新版安装包覆盖安装以更新卸载器；只替换旧版清理脚本不能获得完整的新卸载流程。
