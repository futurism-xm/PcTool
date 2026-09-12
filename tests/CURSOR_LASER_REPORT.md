# 录屏光标与独立激光验证（2026-09-11）

## 实现

- GIF / MP4 的准备和录制工具栏均提供单个光标按钮，无复选框。默认显示，隐藏后关闭并禁用激光，再次显示不会自动恢复激光；倒计时期间锁定，暂停期间可调整。
- 显示、隐藏、激光分别嵌入用户提供的 光标正常2.svg、光标不可用2.svg、光标.svg 的 path。共用抗锯齿绘制、15 DIP 图标和工具栏颜色，不依赖或打包外部 SVG。
- 标注工具、激光开关、设置面板目标独立。切换画笔／矩形／椭圆／箭头／文字、撤销、清空及 Esc 退出标注均保留激光；结束或放弃清理跟踪资源。
- 面板显示最近点击工具的设置，激光大小颜色独立，修改不影响已有文字草稿。切换光标或激光不提交、删除或改写文字。
- 两种编码器均遵守：隐藏时不输出系统指针或光点；激光启用时只输出光点；否则输出普通系统指针。GIF 原有 DPI 缩小、帧延迟及循环预览保持不变。

## 验证结果

Debug：PcTool、PcToolCaptureIntegration 构建通过，日志 `out/cursor-laser-build.log`。

| 测试 | 结果和范围 |
| --- | --- |
| toolbar_icons | 通过；含两个光标图标和新激光 SVG，三档 DPI 抗锯齿 |
| recording_annotation_alpha | 通过；标注透明合成 |
| capture_gif | 通过；1/15/30 FPS、96/144/192 DPI 输出尺寸、光点颜色与隐藏、预览暂停、保存和文件复制、隐藏启动预览 |
| capture_recording_cursor | 通过；解码 MP4 验证系统指针、激光替代、隐藏普通指针、隐藏激光和恢复，无重复箭头 |
| capture_recording_prepare | 通过；MP4 准备激光预览、光标禁用联动、重新显示不恢复激光、全屏工具栏拖动和取消 |
| capture_recording_ui | 通过；标注与激光独立、文字草稿保留、大小颜色、撤销／清除／Esc、暂停、输出合成和工具栏排除 |
| capture_recording_visual | 通过；三档 DPI、准备／倒计时／标注／暂停／文字及输入法消息 |
| capture_recording_styles | 通过；面板和字号列表、屏幕边缘及跨进程点击 |
| capture_recording_exit | 通过；截图进入录屏、准备／倒计时／暂停放弃和结束交付独立预览 |
| capture_recording_preview | 通过；MP4 预览回归 |
| capture_discard_ui | 通过；重复放弃、退出及受管文件清理 |
| capture_editor | 通过；截图编辑器回归 |

GIF 和 MP4 均通过相同的独立光标交互检查：逐个选用五种标注工具，编辑文字时切换光标／激光和修改激光样式，撤销、清除、Esc 及暂停恢复。检查实际光点窗口和文字控件，不只检查按钮状态。已生成并人工检查 `out/build/modular/*-independent-dpi96/144/192.png`、`*-cursor-hidden-dpi96/144/192.png` 等工具栏截图。

日志：`out/cursor-laser-tests.log`（11 项通过，旧截图点击坐标测试失败）、`out/cursor-laser-exit-tests.log`（修正坐标后通过）。另修正录屏 UI 测试原有固定第 1.6 秒取帧：新增交互检查使此时尚未完成标注，改在结束前稳定持有标注的阶段解码验证；没有放宽像素断言。截图入口测试沿用旧版按钮数量，已对齐当前包含清除和 GIF 的实际工具栏；未改截图生产逻辑。

复现（桌面测试顺序执行）：

```powershell
ctest --test-dir out/build/modular -R "^(toolbar_icons|recording_annotation_alpha|capture_gif|capture_recording_cursor|capture_recording_ui|capture_recording_prepare|capture_recording_preview|capture_recording_visual|capture_recording_styles|capture_recording_exit|capture_discard_ui|capture_editor)$" --output-on-failure
```

## 限制

- 实际显示器为 150% DPI；100%、200% 使用原生 DPI 消息及编码 DPI 参数模拟，未在真实混合 DPI／负坐标多显示器逐项测试。
- GIF 光标按钮、激光入口、滑块、全屏准备拖动／取消使用真实鼠标输入；工具组合和多数暂停状态通过原生窗口消息验证。没有把消息模拟称为全流程人工操作。
- 未重复测试长时间高分辨率压力、硬件设备切换、完整安装卸载、系统保存对话框人工取消及第三方粘贴；保存文件与剪贴板文件格式已验证。

## 发布

- Release 全部目标构建及 NSIS 安装包重建通过，日志 `out/cursor-laser-package.log`。
- Release 的 toolbar_icons、recording_annotation_alpha、recording_discard_files、module_boundaries 四项 CTest 全部通过，日志 `out/cursor-laser-release-tests.log`。
- 安装包：`out/installer/PcTool-0.1.0-x64-Setup.exe`；SHA256：`48CB67DC30FDE89AE88063407BEFCE16EE31B9C0584EE9EA79967BE31524D4F5`。
- 安装清单中的 PcTool.exe 与默认 Release 文件 SHA256 一致；未携带三个 SVG 源文件。
- 已启动 `cmake-build-release/PcTool.exe` 新版（本次启动 PID 3424）。启动清理期间用只读文件占用保护既有 1 个 GIF 缓存，启动前后 SHA256 相同。
