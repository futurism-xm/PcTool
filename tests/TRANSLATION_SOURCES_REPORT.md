# 翻译源配置与真实接口接入验证

日期：2026-09-10。环境：当前 Windows x64 桌面、MSVC、Debug `out/build/modular`、默认 Release `cmake-build-release`。

## 实现范围

独立原生设置窗口支持 OpenAI 兼容、DeepL Free/Pro、Azure Translator 和百度通用翻译；没有默认服务、测试账号或公共密钥。菜单文字控制功能开关，箭头打开设置。浮窗只按顺序显示启用且已验证的源，无可用源时显示“请配置翻译源”。配置存入用户 LocalAppData，密钥使用 DPAPI，连接参数验证成功才保存。

## 自动化与窗口检查

| 用例 | 结果与证据 |
|---|---|
| 配置集合、同类多实例、稳定 ID、排序、启停、重启读取 | 通过：源存储和原生设置测试；未验证草稿不能恢复成启用状态 |
| 密钥存储 | 通过：DPAPI 往返、文件不包含测试密钥明文、原子替换 |
| 四种服务协议 | 通过：本机 Winsock HTTP 服务接收生产 WinHTTP 请求，检查认证头、模型、目标语言与返回解析 |
| 百度签名 | 通过：官方示例固定 App ID、密钥、salt 和 apple 的 MD5 签名向量 |
| OpenAI 流式 | 通过：分段 UTF-8、SSE 增量结果、100 次更新合并及独立源完成 |
| 错误与取消 | 通过：HTTP 401 验证失败保留旧配置、429 限流、无效响应、请求取消后及时释放、迟到回调丢弃 |
| 请求超时 | 通过：本机服务延迟 61 秒响应，生产 WinHTTP 请求在 60 秒总截止时间报告超时；不靠模拟异常替代 |
| 新源与表单 | 通过：四类选择卡片、添加草稿、验证保存、新源启用、重命名、取消验证、取消放弃修改、删除确认、关闭再打开 |
| 设置列表溢出 | 通过：多项源列表显示细滚动条，滚动后条目与选择正常 |
| 动态结果 | 通过：三个源显示、第三个结果只读及 Ctrl+A、长结果整体滚动、移除源销毁旧控件 |
| 空配置入口 | 通过：无占位源卡片，点击提示进入同一个设置窗口 |
| 菜单命中与消息 | 单独复测通过：文字和箭头分别命中，实际展开子菜单并派发设置消息 |
| 既有翻译交互 | 单独复测通过：空白 Alt+E、连续快捷键、跨进程复制、原生/浏览器取词、输入与结果选择、OCR、取消、卡片显隐及帧采样 |

RichEdit 的全选范围可能包含最后一个段落标记，新增测试比较实际选中文字，允许该标记，不以 `GetWindowTextLength` 与选择终点完全相等判断失败。

## 最终构建与回归

- Debug、Release 全目标构建成功。最终 Debug 相关 CTest **19/19 通过**，Release 已配置 CTest **12/12 通过**。
- Debug 同时回归截图核心、OCR 布局及结果窗口、剪贴板核心及原生窗口、菜单、录屏工具栏与视频预览、模块边界和压缩集成。未把未执行的其他桌面场景计入通过数。
- 日志：`out/translation-sources-final-debug-tests.log`、`out/translation-sources-final-release-tests.log`；独立翻译窗口复测日志为 `out/translation-sources-final-translation-ui-retest.log`。
- 安装包：`out/installer/PcTool-0.1.0-x64-Setup.exe`；默认程序：`cmake-build-release/PcTool.exe`。沿用原安装结构，新增接口只使用 Windows 系统库，没有额外服务运行时或模型文件。

## 视觉证据

`out/translation-sources-ui` 包含 `settings-empty-{96,144,192}.png`、`settings-filled-{96,144,192}.png`、`picker.png`、`settings-list-scroll.png`、`no-source-popup.png`、`three-source-results.png`。已检查圆角表单、绿色开关、细滚动条、空配置提示与三源长结果。

三档截图使用 WM_DPICHANGED 驱动 DPI 布局，属于模拟 100%、150%、200% DPI 检查；没有将它作为真实混合 DPI 跨屏实测。截图采用 PrintWindow，不能用它证明桌面遮挡、阴影或整机闪屏完全消失。

## 限制及未实测

- 四家真实服务的账号、额度及实际翻译质量均未实测：没有提供服务凭据。本机协议测试不能证明某个用户账号可用。配置窗口的“验证并保存”将实际发送可见测试短句“你好，世界。”。
- 真实系统代理／代理认证、TLS 中间代理、服务商不同地区的网络可达性未实测。
- 真实混合 DPI 多显示器及输入法候选窗专项测试未执行，保留原有输入法提交规则。
- 桌面批量回归曾出现菜单焦点、鼠标选字和真实 Alt+E 输入失败；同一断言的独立复测通过，未放宽输入测试。桌面程序的焦点可受运行环境影响，报告保留这些失败，不能宣称所有桌面场景稳定性已穷尽。桌面空白处的前台激活不可用时，既有测试使用原生空白窗口替代并输出 NOTE。

## 复现

```powershell
ctest --test-dir cmake-build-release --output-on-failure
ctest --test-dir out/build/modular -R '^translation_sources_(core|ui)$' --output-on-failure
ctest --test-dir out/build/modular -R '^translation_ui$' --output-on-failure
ctest --test-dir out/build/modular -R '^taskbar_menu_input$' --output-on-failure
```

原生桌面用例应在解锁桌面串行执行，避免用户输入或其他窗口测试争抢焦点。测试源只写入指定输出目录，不进入用户正式配置。

## 添加窗口样式更新（2026-09-10）

- 添加窗口改为圆角浅色浮窗、双列图标卡片、底部固定操作区；复用公共四周阴影。图标内嵌为矢量路径，已添加列表同步替换原字母占位图标，来源见 `docs/TRANSLATION_ICONS.md`。
- 每次打开默认选中第一个 OpenAI 兼容，立即启用绿色“添加”；选择有绿色描边、浅绿色底及勾选标记。键盘焦点采用独立的浅灰边框。关闭和 Esc 不创建草稿，重新打开恢复第一个选项。原生测试直接点击添加，验证创建的是 OpenAI 草稿。
- 原生测试覆盖选择不创建、确认添加、关闭、Esc、父窗口恢复、四类源添加以及 96/144/192 DPI 消息处理。截图：`out/translation-picker-ui/picker-selected-{96,144,192}.png`；三档仍属于模拟 DPI 检查。
- Debug、Release 全目标构建通过。Debug 相关 CTest 4/4 通过；Release 相关 CTest 3/3 及独立原生源窗口测试通过。日志：`out/translation-picker-debug-tests.log`、`out/translation-picker-release-tests.log`。此次仅更新视图、图标及添加确认交互，未更改翻译协议与凭据存储。

## 放弃修改重复提示修复（2026-09-10）

- 根因：旧 `Leave()` 在确认放弃后仅返回成功，没有将草稿和编辑框恢复为已保存值；打开添加窗口、确认添加等后续动作再次运行 `Dirty()` 时仍判为已修改。
- 修复：确认放弃后同步恢复草稿和所有连接字段，取消旧验证；点击继续编辑、× 或 Esc 保留当前修改。未写入或更改已保存的有效连接，不重新发起翻译。
- 放弃和删除确认使用独立圆角原生窗口，沿用添加窗口配色、阴影和按钮样式。默认焦点为继续编辑／取消；右上角 × 可点击，Esc 取消，Enter 执行当前焦点按钮。
- 新增原生回归：HTTP 401 失败后的未保存字段保留、继续编辑、实际按钮鼠标消息点击 ×、Esc 取消、确认放弃恢复表单及存储、关闭添加窗口后再次打开、选择并添加源不重复提示、删除确认。自动记录对话框次数，出现意外重复提示时取消并失败，不无限等待。
- 96/144/192 DPI 截图已检查：`out/translation-confirmation-ui/discard-confirmation-{96,144,192}.png`。这是消息驱动的模拟 DPI 布局检查，未新增真实混合 DPI 跨屏实测。
- Debug 和 Release 全目标构建通过。Debug 相关 CTest 3/3；Release 相关 CTest 2/2 及独立原生源窗口测试通过。日志：`out/translation-confirmation-debug-tests.log`、`out/translation-confirmation-release-tests.log`。

## 消费原操作与密钥图标更新（2026-09-10）

- 按最新交互要求，出现放弃修改确认框后，本次触发操作被消费。“放弃修改”只回滚表单，不再自动关闭设置、打开添加窗口、继续删除或切换源；再次点击才执行该操作。此前“确认后继续原操作”的行为已替换。
- 原生用例分别验证添加、关闭、删除、切换的首次操作不继续、第二次操作正常执行，且不重复询问放弃修改。删除自身仍需要独立的删除确认。
- 验证按钮和密钥按钮使用所在卡片的 `#ededed` 底色绘制圆角外部，并用缓冲画布一次性绘制，消除此前错误页面底色造成的白边。
- 密钥按钮改为用户所提供的两个 SVG path，支持显示／遮蔽及对应悬停提示；测试检查密码字符、提示标签和两种截图。路径位于 `source_icons.cpp`，不新增 SVG 运行时文件。
- Debug 全目标构建及相关 CTest 3/3 通过；Release 全目标构建、相关 CTest 2/2 及独立原生窗口测试通过。日志：`out/translation-eye-debug-tests.log`、`out/translation-eye-release-tests.log`；截图：`out/translation-eye-ui/eye-{masked,revealed}.png` 及三档 DPI 设置窗口截图。三档 DPI 仍为消息驱动模拟检查。

## 翻译结果卡片图标（2026-09-10）

- 浮窗源名称左侧复用设置窗口的 20 DIP 服务图标，名称保留 8 DIP 间距并在展开箭头之前省略长文本。
- 图标类型随源元数据传递到卡片，不依赖可修改的名称；图标绘制同步整体滚动坐标，不改变原文、结果控件及箭头命中区域。
- 已检查四类源的折叠卡片、三源展开及长结果滚动截图，包含消息驱动的 96/144/192 DPI 检查：`out/translation-result-icons-ui/result-source-icons-{96,144,192}.png`、`result-icons-scrolled.png`。
- Debug、Release 构建通过，Debug 相关 CTest 3/3、Release 相关 CTest 2/2 及原生源窗口测试通过。日志：`out/translation-result-icons-debug-tests.log`、`out/translation-result-icons-release-tests.log`。没有新增图标资源文件或正式翻译源配置。
