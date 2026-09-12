# QQ 长截图交互改造验证

日期：2026-09-11。

## 参考依据

用户提供 `{4686F4D9-A630-4B44-8AA8-7EFBEDC1005E}.mp4`（40.9 秒）展示 QQ 长截图。参考原选区滚动采集、暗色外围、选区外缩略图、尺寸提示、编辑／下载／取消／完成及后续可滚动长图编辑。独立实现，不引入 QQ 文件或资源。现场 QQ 自动化采集此前报 `0x80004002`，本次不声称完成 QQ 直接操作实测。

## 实现

- 轻量覆盖窗口和紧凑工具栏替换独立大型采集窗口；提示“滚动鼠标轴或单击，开始截长图”。
- 手动滚轮、单击自动滚动、再次单击暂停；自动滚动只向原来源发送滚轮，鼠标离开／来源失焦／内容不再变化时停止。自动注入使用专用标记，不吞掉正常手动滚轮。
- DXGI 采集稳定帧，沿用固定页眉页脚和可靠重叠拼接算法；显示层全部设置采集排除。
- 编辑模式无系统标题栏，暗色外围、尺寸标签、完整宽度及细滚动条，复用标注、OCR 和样式面板。
- 完成复制缓存 PNG 的 CF_HDROP 和 CF_BITMAP；下载才另存。保存取消、复制失败后可重试。最终处理防止模态文件对话框引发定时器重入。

## 已执行

Debug 构建成功，`out/qq-long-debug-tests.log` 中 12/12 项 CTest 通过：capture_core、recording_annotation_alpha、toolbar_icons、capture_gif、capture_recording_prepare、capture_ocr、capture_editor、capture_qq_long、capture_live_scroll、capture_sessions、capture_recording_preview、module_boundaries。

新增 `capture_qq_long` 在独立原生合成页面使用真实 SendInput：

- 轮滚由源窗口接收；单击启动自动滚动，源窗口未收到点击；第二次单击暂停。
- 拼接结果逐像素比较：无漏行、重复行、覆盖层，固定页眉与页脚保留一份。
- 编辑移交保留完整尺寸；长图内部滚轮有效，未出现系统滚动条；滚动后的标注坐标正确，清除及撤销正确。
- 另存取消不关闭编辑窗口；完成关闭并提供真实存在的 PNG 文件及位图剪贴板格式。
- 采集完成后取消下载，再次完成仍成功；右键和 Esc 取消均释放会话。
- 全屏选择时控件位于工作区内，缩略图避开工具栏，实际鼠标可点击取消。
- 通过 96／144／192 DPI 消息生成并检查采集／编辑桌面截图；测试图像及代码仅在测试目标中。

## 限制

DPI 为同一物理显示器的消息模拟，未测试真实混合 DPI 跨屏。未对每个浏览器的平滑滚动、无限列表、受保护窗口逐一实测；只有可靠重叠会追加。安装／卸载未在另一台隔离电脑重新实测。用户录屏保留在原路径，并在 `out/qq-long-reference/qq-reference.mp4` 留存副本，避免以后按既有规则清理缓存时丢失参考；不加入安装包。

## Release 与最终复查

- Debug、Release 最终构建均成功。外围使用固定黑色；遮罩为缩略图留出透明区域；完成按钮复用完整图标文字，消除重复绘制。
- 最后一次 `capture_qq_long` 在 Debug CTest 与 Release 原生程序均通过；新增桌面缩略图与独立绘制图逐像素差异检查，确保预览不被遮罩压暗。日志：`out/qq-long-debug-visual-final.log`、`out/qq-long-release-visual-final.log`。
- Release 默认未启用 desktop CTest 注册，相关 4/4 CTest 通过（`out/qq-long-release-tests.log`）。另直接执行 Release 原生用例：qq-long-test、long-test、session-test、editor-test、prepare-test、preview-test、gif-test 均通过，详见 `out/qq-long-release-native.log`。
- Release OCR 批次首次运行出现 90 秒超时；单独重跑通过（`out/qq-long-release-ocr.log`），尚未证明首次超时根因。OCR 实现未修改；不将超时隐去或计为首次通过。
- 已查看最终 100%／150%／200% DPI 的原生桌面截图；强制 DPI 切换期间可能触发无法匹配提示，但合成结果像素检查仍通过。真实跨屏 DPI 与所有应用的连续动画页面仍需后续实际使用验证。
- 安装包输出位置：`out/installer/PcTool-0.1.0-x64-Setup.exe`。参考视频原始 SHA-256：`06D6B58CAF0F94D9BBB0AB43A898ED2186B91A61703F55E7B372284F01F85253`；启动新版时保留原缓存文件，避免本轮工作清除用户视频。


## 交付确认

最终安装包生成成功：14,100,534 字节，SHA-256 `35D5CA80FD3BE02290B2EB9DAFC02F1BBAC22A6942EA54B34B1AF4A982F9BC4C`。默认 `cmake-build-release/PcTool.exe` 已启动，确认进程存活（PID 12360）。本次启动保护了 17 个已有缓存文件，参考视频原路径哈希与改造前一致；另外保存了不受缓存清理影响的参考副本。
