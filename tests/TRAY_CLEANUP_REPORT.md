# 托盘残留图标修复（2026-09-10）

## 修改

- 新增 shared/ui/tray_icon.h，将注册状态区分为未知、存在、不存在。ADD 返回失败仍保留未知状态，尝试清理；DELETE 失败不直接宣称删除成功，通过图标身份查询确认，不存在或成功删除后才清空状态。
- 使用 NIF_GUID 和按程序路径稳定生成的 GUID，代替随进程 HWND 改变的身份。相同路径重启先清理旧 owner，再绑定新窗口；不同未签名构建路径使用不同 GUID，以兼容 Windows 的路径限制。
- 同一次注册结果不确定时先 MODIFY 检查并接管，避免盲目重复 ADD。Explorer 不响应时不调用可能长时间等待的托盘操作，保留状态供后续重试。
- 退出先分离任务栏子窗口，再处理托盘删除；失败后以定时器最多重试五次，最终销毁时再尝试一次，避免无限阻止退出。系统不可用时下次同路径启动仍能按同一 GUID 恢复。
- 保留上一轮切换顺序，失败时不在托盘状态未清理完毕的情况下重新附加 Explorer 子窗口。

## 已验证

- Debug：tray_icon_lifecycle、taskbar_layout、taskbar_menu_input、module_boundaries 全部通过。
- Release：tray_icon_lifecycle、taskbar_layout、module_boundaries 全部通过。
- 故障注入：重复显示不重复添加、删除失败仍保留状态、删除重试、ADD 实际成功但回包失败、失败 ADD 后清理、Explorer 暂时不可访问、新 owner 清理失败不误报注册成功、Explorer 重启恢复。
- 原生 PcToolTrayTests native：实际 Shell 注册，销毁旧 owner 且不删除以模拟残留，新 owner 清理并重新注册，删除后按 GUID 查询无注册。此项是窗口生命周期实验，不冒称真实系统崩溃实验。
- monitor_placement_integration.ps1：两种模式、重启恢复与进程退出后按 GUID 检查无残留注册通过；恢复测试前显示设置。
- 连续 30 次实际菜单正文点击切换通过，无双入口注册，最慢 48.1665 ms，原始数据 out/tray-cleanup-switch-final.json。
- Debug／Release 构建通过，安装包更新至 out/installer/PcTool-0.1.0-x64-Setup.exe。

未执行真实 Explorer 崩溃或系统关机断电；故障场景通过可注入 Shell 接口验证。注册查询和原生 owner 实验不等于逐个检查 Explorer 折叠菜单的所有历史缓存图像；旧版本使用临时 HWND 的已失效图标仍由 Explorer 刷新清除，不扫描或操作其他软件的托盘图标。
