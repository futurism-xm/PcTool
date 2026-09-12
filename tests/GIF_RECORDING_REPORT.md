# GIF 录制验证报告（2026-09-11）

## 实现

- 截图 GIF 入口进入录制准备；默认 15 FPS（1～30），开始后锁定。
- 光标默认开启，激光默认关闭；取消光标关闭并禁用激光，重新勾选只恢复普通光标。
- 复用会话、激光大小颜色、标注、暂停、放弃、倒计时及拖动；GIF 无音频。SVG path 编译入程序。
- WIC 逐帧量化编码，有限当前/待写帧缓冲，不积攒整段录制；编码不足时减少采样，按实际累计时间计算帧延迟。
- NETSCAPE2.0 循环扩展，每帧完整、不透明、处置为保留；独立 GDI+ GIF 解码预览。
- 文件复制共用 MP4 的 CF_HDROP / Preferred DropEffect。缓存位于 Cache/<SID>/Gif；下载临时写入后原子替换目标。

## 已执行

| 用例 | 前置/步骤/预期 | 结果 |
|---|---|---|
| G01 | 合成窗口，1/15/30 FPS 各约 2.2 秒，检查尺寸、帧数与延迟 | 通过；首轮 3/33/66 帧，223/218/221 cs，321×181 |
| G02 | 15 FPS 绿色激光，前半段开光标、后半段关闭，检查编码像素 | 通过；颜色正确，关闭后光点消失 |
| G03 | 检查 GIF 循环扩展 | 通过，NETSCAPE2.0 |
| G04 | 预览自动播放、暂停、下载、完成复制文件 | 通过，完成关闭且剪贴板含 CF_HDROP |
| G05 | 准备时关闭光标后启用激光，再重新开启 | 通过，禁用无面板，开启显示面板 |
| G06 | 开始、倒计时、录制、关闭光标、暂停恢复、结束 | 通过，返回 GIF，关闭光标收起激光面板 |
| G07 | 注入 96/144/192 DPI 消息生成准备工具栏截图 | 通过并检查截图；模拟 DPI |
| G08 | 未开始及录制中放弃 | 通过，受管临时文件删除 |
| G09 | 不存在的目标父目录 | 通过，有错误、无成功路径 |
| G10 | 真实 SendInput 拖动滑块至 1/30、快速连续切换光标、点击激光 | 通过；发现并修复双击消息造成第二次点击丢失 |
| G11 | 全屏 GIF 准备，真实鼠标拖动工具栏、点击取消 | 通过，移动后可点击，鼠标捕获释放 |
| R01 | 普通录屏准备、录屏 UI、预览 | 通过 |
| R02 | 标注透明合成、放弃文件处理、图标、模块边界 | 通过 |

回归日志：out/gif-final-regression.log（8 项通过）。Debug 构建：out/gif-build.log。
追加原生输入测试：out/gif-native-tests.log（通过，13.76 秒），滑块截图 gif-test-slider1.png、gif-test-slider30.png 已检查。

## 复现

运行 `ctest --test-dir out/build/modular -R "^(capture_gif|capture_recording_ui|capture_recording_preview|capture_recording_prepare|recording_annotation_alpha|recording_discard_files|toolbar_icons|module_boundaries)$" --output-on-failure`。

图片和 GIF 位于 out/build/modular，前缀 gif-test：prepare-dpi96/144/192.png、recording.png、preview.png、1/15/30.gif。

## 未实测与限制

- GIF 光标/激光、滑块及全屏准备拖动/取消已使用真实鼠标输入；其余多数状态通过原生窗口消息调用。全屏 GIF 视频输出、屏幕边缘拖动和各方向选区调整尚未逐项实测。
- 实际混合 DPI、负坐标多显示器、长时间高分辨率压力和峰值内存未实测。
- 下载已通过 SaveTo 写入实际 GIF 验证；系统保存对话框的人工选择/取消未实测。
- 剪贴板格式及文件已验证，第三方聊天程序粘贴未实测。
- 启动清理复用受管缓存实现，本轮未重测系统重启、文件占用及完整安装/卸载。
- GIF 最多 256 色；实际采样帧率可能低于设置值，输出保留实际时长。

Release 构建及安装包已完成：out/gif-package.log。最终安装包 SHA256：1BE246BFED178D9E0C75F66731146A4BA0DE9669D707F99EC3D593B8D797AF45。双击修复后的普通录屏回归：out/gif-last-regression.log（2 项通过）。

## 高 DPI 自动缩小（2026-09-11）
- 新增选区所在显示器 DPI 自动读取，在开始时固定输出尺寸，最低按 96 DPI，尺寸四舍五入且至少 1 像素。标注和光标合成后通过 WIC Fant 插值整体缩小，再量化编码。
- 模拟 96/144/192 DPI 编码验证通过，321×181 分别输出 321×181、214×121、161×91；缩放后的激光颜色及位置检查通过。现有 G01 在本次测试中按上述三种尺寸执行。
- 当前实际显示器的自动 DPI 输出检查通过。真实混合 DPI 跨屏未实测。
- Debug 构建、capture_gif 和 capture_recording_ui 均通过，日志 out/gif-dpi-build.log、out/gif-dpi-tests.log。
- 高 DPI 版本 Release 构建与安装包已完成（out/gif-dpi-package.log），新版安装包 SHA256：72037BDF586FDC75DB97CFE422C9D13B50DBB992AAA4BCD67501E5AA6238B116。

## 结束后预览被隐藏修复（2026-09-11）
- 根因：后台启动携带 STARTF_USESHOWWINDOW/SW_HIDE，首次普通窗口的 ShowWindow(SW_SHOW) 被启动参数覆盖。实际发现 GIF 文件存在、GIF 预览 HWND 存在但 IsWindowVisible 为 false。
- 与 MP4 预览一致，改用 SetWindowPos(SWP_SHOWWINDOW) 明确显示，并请求前台激活。
- 补充真实应用 CaptureTools.Record 回调链与鼠标结束按钮测试，断言预览可见、窗口中心命中预览、另存及完成控件可见；增加隐藏启动子进程测试。capture_gif 通过，18.62 秒，日志 out/gif-preview-fix-tests.log。
- 修复版 Debug、Release 构建、GIF 测试及 MP4 预览回归均通过；安装包已重建（out/gif-preview-fix-package.log）。更新重启期间通过只读占用保护保留已有 GIF 缓存文件。

## 光标图标与独立激光更新（2026-09-11）

GIF 和 MP4 共用光标图标开关，激光不再与标注工具互斥，Esc 退出标注不关闭激光。复选框及旧互斥说明以本节为准。实现、12 项回归结果、模拟 DPI 与未实测范围见 [CURSOR_LASER_REPORT.md](CURSOR_LASER_REPORT.md)。

## 帧率滑块抗锯齿（2026-09-11）

- 将原先 GDI Ellipse 硬边滑块替换为 GDI+ 抗锯齿填充圆，使用浮点 DPI 半径，轨道同步采用圆角抗锯齿线；沿用原大小、颜色、帧率范围和命中逻辑。
- 已检查 96/144/192 DPI 准备工具栏截图（gif-test-prepare-dpi*.png），圆形滑块边缘平滑。DPI 使用窗口消息模拟，实际显示器为 150%。
- capture_gif 通过（19.59 秒）：含真实鼠标拖动滑块到 1/30 FPS、倒计时、录制、预览、文件复制及下载。日志 out/gif-slider-tests.log。
- Debug 构建日志 out/gif-slider-build.log；Release 与安装包日志 out/gif-slider-package.log。
