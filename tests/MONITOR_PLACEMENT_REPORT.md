# 性能监控显示位置（2026-09-10）

- 勾选显示任务栏监控，取消勾选改为 PcTool 托盘图标；托盘复用功能菜单，性能监控正文切换位置，右侧箭头继续选择指标。
- `HKCU\Software\PcTool\MonitorInTaskbar` 保存选择；不存在时沿用任务栏模式。任务栏恢复失败时保留托盘入口并重试，退出删除图标。
- Debug：taskbar_layout、taskbar_menu_input、module_boundaries 全部通过。包含默认值、隔离注册表保存／重载、菜单正文与箭头命中和勾选状态。
- Release：构建通过；taskbar_layout、module_boundaries 通过；手动运行 PcToolTaskbarTests.exe menu-test 通过（该构建目录未注册桌面 CTest）。
- `tests/monitor_placement_integration.ps1` 使用真实 Release 进程依次验证任务栏、托盘、托盘重启、任务栏恢复，四次通过；每次检查任务栏窗口与托盘图标互斥，结束恢复原设置。
- 初版集成检查使用 Shell_NotifyIconGetRect，托盘折叠后两次检查失败。改用零标志 NIM_MODIFY 检查图标注册、不修改字段后通过；退出等待改为等待进程结束。
- 安装包构建通过：out/installer/PcTool-0.1.0-x64-Setup.exe。
- 未实测：真实 Explorer 崩溃重启、真实多显示器混合 DPI；未将自动恢复代码核查算作交互实测。布局测试覆盖模拟 96／144／192 DPI。
