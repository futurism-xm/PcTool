# 圆角设置窗口、九项快捷键与翻译子菜单（2026-09-10）

## 本轮实现

- 翻译源设置和快捷键管理共用 `shared/ui/settings_frame`：无系统标题栏、浅色圆角外框、细边缘、四侧阴影、右上角关闭、标题区拖动及边缘缩放。透明阴影和边缘层不激活、不拦截鼠标；窗口及阴影按工作区约束尺寸和位置。
- 快捷键窗口默认 760×720 DIP，无左侧导航，九行白色绑定框及说明；绿色聚焦边框、清空入口、细圆角滚动条，固定底部恢复默认值与绿色保存。未保存关闭使用共用圆角确认框，禁止点击穿透。
- 存储保留 Hotkey0～4，追加 Hotkey5～8；界面顺序独立。所有组合可清空，空组合不注册、不查重、不显示菜单快捷键；恢复默认只改草稿，保存时检查有效性、重复和系统占用，失败保留原配置。
- 中英翻译主菜单仅显示名称、勾选及箭头。子菜单依次为输入翻译、截图翻译、划词翻译、分隔线、翻译源设置；绑定来自同一配置，间隔 5 DIP。
- 翻译开关只控制三项全局快捷键，不关闭已有窗口；未勾选仍可从菜单调用。菜单调用直接分发功能，划词记录原窗口 HWND 与进程身份并恢复来源，禁止使用其他进程的剪贴板结果。
- 菜单截图入口异步等待 300 ms，避开 Windows 菜单选中项淡出动画，再刷新合成桌面并采集。这个等待只用于菜单截图，Alt+E 未增加判空等待；新入口、隐藏和退出取消待执行采集，等待期间 Esc 可以取消。

## 编号用例与实际结果

| 编号 | 前置状态与操作 | 预期与实际结果 |
|---|---|---|
| HK01 | 默认配置打开设置，读取九项及顺序 | 通过：前五项默认组合正确，四个新增项为空 |
| HK02 | 重复绑定、非法组合后保存 | 通过：定位到错误行，不调用保存或覆盖配置 |
| HK03 | 修改划词键，恢复默认，再修改并模拟保存失败／重试 | 通过：恢复仅改草稿，失败保留，重试成功关闭 |
| HK04 | 九项全部清空后保存 | 通过：接受空组合；真实 Release 进程注册已全部释放 |
| HK05 | 改动后关闭，继续编辑，再关闭并放弃 | 通过：确认时父窗口禁用，取消保留草稿，放弃关闭；无取消按钮 |
| HK06 | 真实 SendInput 录入 Ctrl+Alt+Shift+F24，再按 Delete | 通过：长组合正确录入，Delete 清空，聚焦行滚动可见 |
| HK07 | 五项旧注册表配置加载；九项保存和全空重载 | 通过：旧索引含义及原绑定保留，新项默认空，九项及解绑可持久化 |
| HK08 | 外部进程占用新组合，真实 Release 窗口保存 | 通过：拒绝冲突；释放占用后保存；四个新增动作均已注册 |
| HK09 | 自定义剪切板／设置快捷键，重启再调用，清空所有绑定 | 通过：真实快捷键打开对应窗口，重启保留，解绑后注册释放；结束恢复用户原配置 |
| MN01 | 主菜单与箭头独立命中，展开翻译子菜单 | 通过：主菜单无快捷键串，三项入口及设置位置正确 |
| MN02 | 空绑定和 Ctrl+Alt+Shift+F24；96／144／192 DPI 测量 | 通过：空绑定无多余间隔，长组合完整，文字间隔为 5 DIP |
| MN03 | Notepad++ 独立实例、合成中英多行命令文本 | 通过：12 次 Alt+E、3 次自定义 Alt+R、10 次快捷键重复；松键顺序和按住重复均正确 |
| MN04 | 关闭全局翻译键后，Notepad++ 菜单调用 10 次 | 通过：其中 3 次真实点击共用子菜单，其余经统一菜单入口调用；来源及全文正确，每次只提交一次；错误 PID 被拒绝并提示 |
| MN05 | Edge 独立配置目录，本地 HTML 空白及可选文字 | 通过：各 10 次真实 Alt+E；实际子菜单划词通过。空白窗口立即可见，测试记录最高约 63 ms |
| MN06 | Windows 记事本独立测试文件，经子菜单取词并关闭来源 | 通过：文字完整填入；来源关闭后显示明确失败提示 |
| MN07 | 实际点击截图翻译子菜单，框选原生测试文本 | 通过：菜单已关闭、捕获已释放，OCR 结果为原测试文本，未混入菜单；仍走本地 OCR 与测试翻译源 |
| MN08 | 取消翻译勾选、已有弹窗、菜单输入翻译 | 通过：取消勾选不隐藏弹窗；关闭后仍能通过实际子菜单打开空输入窗口 |
| UI01 | 快捷键窗口三档 DPI，滚动到顶部／底部 | 通过：保存按钮固定、底部功能可达，圆角／阴影／文字已查看截图。DPI 通过消息模拟，未修改系统缩放 |
| UI02 | 翻译源设置三档 DPI、添加／验证／删除／排序／未保存确认 | 通过：主外框限制工作区，四侧阴影桌面截图通过目视检查；配置操作及点击消费回归通过 |
| RG01 | 翻译原文、只读结果、卡片展开收起、输入滚轮、取消／迟到结果 | 通过：完整 translation_ui 原生测试，包括三档 DPI 画面采样 |
| RG02 | 剪切板历史原生 UI、录屏预览、OCR 布局及共享绘制 | 通过：相关原生与单元测试通过 |
| RG03 | 任务栏／托盘真实 Release 切换与重启 | 通过：四次位置／重启检查；30 次切换最大 58.7167 ms，位置互斥、退出无残留注册 |

## 构建与复现

- Debug 全部构建成功：`out/build/modular`。
- Release 全部构建成功：`cmake-build-release`；该目录注册的 13 项 CTest 全部通过。
- Debug 本轮 16 项相关回归全部执行通过（初次有 6 项因未构建测试 EXE 而 Not Run，补全 Debug 构建后重跑这 6 项全部通过）；完整 translation_ui、translation_sources_core 另行通过。
- 安装包已重建：`out/installer/PcTool-0.1.0-x64-Setup.exe`。

在已配置的 MSVC x64 开发者环境中运行：

```powershell
cmake --build out/build/modular
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
ctest --test-dir out/build/modular -R 'hotkey_settings_ui|taskbar_menu_input|translation_notepadpp|translation_ui|translation_sources_ui' --output-on-failure
out/build/modular/PcToolHotkeyTests.exe out/rounded-settings-visual
pwsh -File tests/hotkey_registration_integration.ps1
pwsh -File tests/monitor_placement_integration.ps1
pwsh -File tests/monitor_switch_latency.ps1 -Cycles 30 -LimitMs 1000
pwsh -File scripts/build-installer.ps1 -SkipDependencyBuild
```

日志保存在 `out/rounded-settings-*.log`。快捷键窗口的三档 DPI 上下部截图在 `out/rounded-settings-visual`；翻译设置四侧阴影截图在 `out/build/modular/translation-sources-ui/settings-frame-*.png`，均使用合成背景。用例只向本地测试服务或注入的测试翻译源提交合成文本，未用用户文档调用真实供应商。

## 本轮发现及修正

- 实际菜单截图最初读到了“截图翻译 Alt+W”：逻辑关闭和 DwmFlush 不能消除仍在淡出的菜单选中项。新增异步等待后，原测试中英两行文本完整识别，回归通过。
- 菜单测试最初使用可见父窗口模拟应用隐藏控制器，菜单退出激活了测试父窗口并遮住测试截图。改为与应用一致的持久隐藏、无所有者控制器后再验证，避免把测试夹具焦点差异当成产品问题。
- 增加工作区尺寸硬限制，避免 DPI 推荐尺寸超过工作区后只修正位置、仍露出屏幕外。

## 未实测与边界

- 100%、150%、200% 为原生窗口 WM_DPICHANGED 模拟及桌面截图；未执行真实混合 DPI、多显示器负坐标跨屏拖动、系统显示缩放切换。
- 任务栏切换使用真实鼠标；托盘路径通过 Shell 回调入口测试。共用翻译子菜单已实际点击，但没有逐项完成真实托盘展开面板内点击图标后，在三个外部应用中的全部交叉组合，不能将其列为通过。
- 本轮未新增真实供应商付费请求测试，也未逐一实测新增的任务管理器、画图、7-Zip 启动快捷键；四项注册均验证，快捷键管理的实际启动已验证，其余复用已有功能分发。
- 系统桌面前台激活在测试环境中失败，使用原生空白窗口替代；不将其报告为桌面空白区实测通过。
- 未做安装向导的全新安装／卸载试验；已完成安装包构建，沿用已有可自定义位置的安装结构。
## 2026-09-10 后续修复：重绘残影与窗口打开期间的快捷键

- 以用户最新要求为准：设置窗口保持打开时全局快捷键继续有效，仅聚焦绑定框录入时暂停。点击空白、焦点移走、切换应用、Esc 或关闭都会恢复。保存检查短暂注销后恢复，防止自己的现有注册被误判为冲突。
- 将“窗口已打开”与“绑定框正在录入”拆成独立状态；设置窗口的消息循环只处理自己及子控件的对话框按键，不再干预其他功能窗口。
- 滚动／缩放从逐个 MoveWindow 并立即绘制，改为禁止复用旧像素、统一调整全部控件后完整重绘父子窗口；绑定框增加同级裁切。
- 重绘回归比较每次滚动／缩放后屏幕帧与完整刷新帧：原实现三档各 29/30 帧不一致；修复后三档各 0/30 帧不一致。测试针对过渡画面，而非只检查最终静态截图。100%／150%／200% 为模拟 DPI，真实混合 DPI 仍未实测。
- Debug 原生快捷键、任务栏布局／菜单、模块边界测试通过。Release 真实进程脚本验证：窗口打开时快捷键可调用剪切板；绑定框聚焦时同一组合只录入、不触发剪切板；Esc 后立即恢复；九项保存、冲突、重启及解绑回归通过。测试后恢复原有快捷键。
- 对比日志：`out/hotkey-repaint-before.log`、`out/hotkey-repaint-after.log`；三档截图：`out/hotkey-repaint-after`；真实进程日志：`out/hotkey-repaint-registration.log`。
## 2026-09-10 再次修复：原生边缘缩放与视口残片

- 共用 SettingsFrame 的 WM_NCCALCSIZE 在尺寸变化时要求完整重绘，顶层窗口禁止复制旧客户区像素；启用父子控件合成缓冲，缩放结束完整刷新。
- 视口上、下边缘不再绘制半截说明文字、功能名称或绑定框细条。绑定控件保留键盘导航，获得焦点时仍会滚动到可见位置。
- 新增真实 SendInput 边缘拖拽测试，监测 WM_ENTERSIZEMOVE，覆盖四边 × 三档模拟 DPI，确认进入 12 次原生缩放循环；拖动中及释放后共 36 帧与完整刷新帧一致，已查看截图。
- 旧程序的该项定时采样同样为 0/36 差异，因此不能声称精确复现了用户的每一次瞬时残影；本轮补强原生客户区重绘，并明确去掉截图中视口边缘的半截内容。DPI 为模拟，未执行真实混合 DPI 跨屏。
- 首次并发启动桌面用例造成同名测试窗口相互干扰，结果作废；终止测试进程后串行重新执行，原生拖拽、快捷键设置、翻译源设置、任务栏菜单及模块边界全部通过。真实 Release 快捷键脚本回归通过，窗口打开时快捷键仍可用。
- Debug／Release 构建通过。日志：out/native-resize-final-drag.log、out/native-resize-final-tests.log、out/native-resize-registration.log；截图：out/native-resize-after/drag-*.png。
- 复现命令：out/build/modular/PcToolHotkeyTests.exe out/native-resize-after drag。必须与其他桌面测试串行运行。

## 2026-09-10：HTTP 地址与左侧间距

- OpenAI 兼容地址允许远程、局域网、本机 HTTP/HTTPS；保留协议、端口及现有路径拼接规则。其他服务官方地址不变。
- 快捷键名称列从 144 DIP 缩为 88 DIP，绑定框及说明左移 56 DIP，保留右侧边界。
- Debug、Release 构建通过；Debug translation_sources_core、hotkey_settings_ui、translation_sources_ui、module_boundaries 共 4 项通过。HTTP 测试使用合成地址检查配置和请求构造，实际传输仅使用本机测试服务，没有访问用户服务。
- 96/144/192 模拟 DPI 原生窗口截图已检查；每档 30 次重排对照均无像素差异。真实混合 DPI 跨屏未实测。
- 日志：out/http-spacing-tests.log、out/http-spacing-visual.log；截图：out/http-spacing-visual。安装包已重建。

## 2026-09-10：快捷键管理独立窗口

快捷键管理增加与翻译源设置一致的 WS_EX_APPWINDOW，保留控制项导航及圆角外框，让 Windows 为其提供独立任务栏入口。原生测试确认 APPWINDOW 存在、非 TOOLWINDOW/CHILD；hotkey_settings_ui 和 taskbar_menu_input 均通过。此轮未自动检查 Explorer 任务栏实际按钮画面。Debug 构建通过；Release 随安装包重建。
日志：out/hotkey-independent-build.log、out/hotkey-independent-tests.log、out/hotkey-independent-installer.log。

## 2026-09-10：截图识别自身窗口

修复 FindWindowCallback 按进程排除全部 PcTool 窗口的问题。允许正常窗口参与自动选窗，排除当前截图层及本进程工具/透明辅助窗口（独立应用窗口除外）。
新增原生回归命令：PcToolCaptureIntegration.exe window-snap-test。合成同进程 APPWINDOW、透明阴影和实际截图覆盖层，确认命中目标边界，通过。未读取用户文档；未逐个真实设置窗口实测。Debug 构建和该测试通过，Release/安装包重建日志见 out/window-snap-installer.log。

## 2026-09-10：滚轮边界闪烁

Scroll 对限制后的偏移量先比较，位置不变则直接返回，不执行 Layout。包含到顶/到底后的继续滚动，以及未累积满一格的滚轮事件。
原生测试在上下边界各连续发送 30 次滚轮消息，覆盖主窗口和绑定框转发；观察窗口、视口、九个绑定框、底部按钮的 WM_PAINT 和 WM_WINDOWPOSCHANGING，结果均为 0。原有九项绑定、校验、保存及关闭确认测试通过。Debug 构建通过，Release 及安装包已重建。日志：out/hotkey-boundary-test.log。

## 2026-09-10：保存保留窗口及 7-zip 名称

保存成功更新初始绑定基线，保留窗口，退出按键录制状态并清除旧错误绘制；手动关闭已保存窗口不提示放弃修改。菜单、绑定列表及启动失败标题统一显示 7-zip，功能标识和注册表配置不变。
Debug 原生 hotkey_settings_ui、taskbar_menu_input 通过；实际应用快捷键注册/保存/重启集成脚本通过。Release 和安装包重建。日志 out/save-stay-*.log。

## 2026-09-10：GIF 按钮占位

按用户确认，本轮仅在截图工具栏 OCR 和屏幕录制之间添加 GIF 按钮，禁用录制操作，提示 GIF 录制（暂未开放）。提取用户 GIF图片.svg 的两条 path 并内嵌，使用现有 GDI+ SVG 路径解析和抗锯齿绘制，发布不携带 SVG 文件。
Debug 构建通过；图标测试在 96/144/192 DPI 通过（包含 GIF 墨迹和抗锯齿检查），200% 输出已人工查看。日志 out/gif-icon-test.log，截图 out/gif-icons-dpi*.png。Release 和安装包重建。

GIF 图标后续替换：改用用户 D:/download/GIF.svg 的单条路径，仍内嵌绘制。96/144/192 DPI 图标测试通过，200% 输出已检查；见 out/gif-replace-test.log 和 out/gif-replaced-dpi192.png。Debug/Release 与安装包重新构建。

## 2026-09-10：清除标注按钮

截图工具栏撤销右侧新增清除标注，无标注时禁用；清除标注列表并复位移动索引，保留截图选区。截图及录屏共享 ToolbarIcon::Clear，替换为用户删 除.svg 内嵌 path，不发布源 SVG。
Debug 构建通过；window-snap-test 增加按钮次序、空态禁用、清除及选区保持断言并通过。96/144/192 DPI 图标测试通过，200% 图标已查看。Release 与安装包重新构建。日志 out/clear-button-test.log、out/clear-icon-test.log。
