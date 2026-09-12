# Chromium／Electron 长截图滚轮兼容修复

日期：2026-09-12。用户提供 Codex 聊天窗口截图，反馈向上截取一直提示“未检测到可滚动内容”。

## 根因与对照证据

Chromium 的 `RerouteMouseWheel` 在处理滚轮时调用 `WindowFromPoint(lParam)`。它发现该点是其他进程的窗口、且不支持滚轮重定向时，会丢弃消息。PcTool 的输入保护层覆盖选区，普通原生控件接受定向投递，而 Chromium 把保护层视作无关窗口。这解释了之前原生列表测试通过、Electron 仍不滚动的差异。

已使用独立 Electron 40.0.0 窗口、合成中英文列表复现，未操作用户文档或向外发送正文。对照构建仅禁用本次新增的 Chromium 投递路径，保持其他代码一致：实际列表位置从 600 到 600，DOM 收到的滚轮数为 0，PcTool 显示“未检测到可滚动内容”。记录：`out/scroll-electron-control-native.log`。

参考 Chromium 上游源码：

- <https://github.com/chromium/chromium/blob/main/ui/base/win/mouse_wheel_util.cc>
- <https://github.com/chromium/chromium/blob/main/ui/views/win/hwnd_message_handler.cc>

## 修复

- 对 `Chrome_WidgetWin_*` 来源采用有界同步投递。在保护层中仅临时让出滚轮坐标的一个像素用于命中检测，重新校验该点仍属于原来源，完成投递后立即恢复完整保护层。
- 同步消息超时为 100 ms，不自动重发未知结果。该短暂投递期间拦截选区内的鼠标移动，点击继续由原钩子消费，避免借投递间隙触发源页面悬停或点击。程序不移动用户光标、不隐藏整层、不模拟用户点击。
- 普通原生窗口继续使用原来的异步投递。最小 350 ms 步进和画面稳定检查保留；锁定列表后，即使鼠标在选区其他位置，仍向原列表投递。
- 修复到顶／到底通知尚未被 UI 定时器处理时吞掉下一次滚轮的竞态：先处理旧动作完成，再登记新动作；边界处重新单击启动同样先处理旧通知。

连续真实文字测试另发现两处拼接误判，并一并修复：

- 分隔线或文本行离开视口边缘时，旧代码仅凭两行变成背景色就判定布局缩小。现在要求连续边缘带的证据；匹配范围因空白行变短时，对遗漏边缘按同一位移双向校验，再决定是否保持已锁定区域。真实布局缩小仍有拒绝测试。
- 重复的句子后半部分不能单独给出唯一位移，旧检测可能将一行分成滚动列和固定列。现在使用已确认位移验证相邻文字列及背景连接，整行一起拼接；保留静态边界及独立滚动条排除逻辑。
- 首步位移较大时，首行的部分字形可能未进入最初的变化范围。沿已确认位移从另一帧验证上下边缘，恢复真实正文边界，避免把首行上半部分当成固定内容。

## 验证

- 新增 `tests/electron_scroll_fixture.cjs`：独立真实 Electron 页面，包含固定侧栏、悬停区域及一个内部滚动列表。DOM 滚动位置、滚轮／移动／点击计数仅用于测试断言，产品不读取 DOM。
- 修复后第一步由 600 滚至 550，实际收到一个滚轮事件，长图高度由 740 增至 790；页面鼠标移动、点击计数均未增加。
- 连续上下交替至少 12 次；向上到顶并重复提示；在选区侧栏单击，自动恢复已采集下端并继续到列表底部；再次向下滚动重复提示。全过程检查实际 DOM 位置与拼接坐标一致、光标未被程序挪动、页面未收到额外悬停或点击。
- 结束交接编辑，输出 900×2380 图像。将其中 450×2000 的完整正文逐像素对照 Canvas 原始 BGRA：66 行中英文、数字、相同句子后缀，无漏行、重行或文字列错位。已检查完整 PNG。
- 增加 GDI 中文密集文字的连续向上与整行逐像素验证；核心测试继续覆盖重复图案拒绝、真实布局变化、多个列表选择、滚动条排除和容量限制。
- 增加首步强制移动 100 像素的真实 Electron 用例，后续仍使用普通滚轮；这是对页面大步响应的测试模拟，不修改产品步长。该场景曾在 Release 暴露首行裁切，修复后整幅正文逐像素通过：`out/scroll-electron-large-fixed-native.log`。
- Debug Electron 完整像素通过记录：`out/scroll-electron-debug-pixels2-native.log`。早期文字失败和对照日志均保留。测试遥测文件使用共享删除读取，避免 Windows 文件原子替换被读者短暂占用导致测试进程退出。
- 最终 Debug／Release 构建成功：`out/scroll-electron-debug-build.log`、`out/scroll-electron-release-build.log`。Debug 相关 CTest 6/6 通过（核心、双向／局部滚动、QQ 长截图、实时采集、会话及模块边界）：`out/scroll-electron-debug-final-ctest.log`。
- Release CTest 4/4 通过（核心、普通 Electron、100 像素首步 Electron、模块边界）：`out/scroll-electron-release-final-ctest.log`。两项实际页面测试都核对全部正文像素；原始日志和图像在 `cmake-build-release/electron-scroll-*`、`cmake-build-release/electron-scroll-large-*`。
- Release QQ 长截图与会话回归通过：`out/scroll-electron-release-qq-long-test.log`、`out/scroll-electron-release-session-test.log`，覆盖编辑、复制 PNG／图片、保存取消／重试及退出清理。
- 已启动默认 `cmake-build-release/PcTool.exe`，进程及哈希记录见 `out/scroll-electron-launch.log`；启动期间保护已有缓存，用户参考录屏保持原哈希。安装包为 `out/installer/PcTool-0.1.0-x64-Setup.exe`，构建和清单核对见 `out/scroll-electron-package.log`、`out/scroll-electron-package-verification.log`。

## 重跑

仅测试需要单独的 Electron 运行时；本轮为 Electron 40.0.0 / Chromium 144.0.7559.60，下载包按 Electron npm 包的校验值验证。通过 CMake 的 `PCTOOL_ELECTRON_TEST_EXECUTABLE` 显式指定测试运行时路径后，可运行 `ctest --test-dir <构建目录> -R capture_electron_scroll --output-on-failure`，包含普通与较大首步两项测试。测试需要可交互桌面；不指定运行时则不注册。

测试版本写入对应输出前缀的 `-versions.json`。运行时及其配置均留在 `out`，不增加 PcTool 发布依赖，不随安装包携带。

## 未实测

本轮两次尝试通过 computer-use 读取用户 Codex 窗口，都报 `SetIsBorderRequired failed: 不支持此接口 (0x80004002)`；没有使用过期坐标继续操作，因此不能把独立 Electron 实测写成 Codex 当前聊天页实测。真实混合 DPI 未实测；原生回归的 100%、150%、200% DPI 为消息模拟。其他 Chromium 版本、框架特例及复杂动态布局仍需按实际环境验证。
