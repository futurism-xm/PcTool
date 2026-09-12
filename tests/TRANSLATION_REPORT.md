## 2026-09-10 原文区域与外层滚轮隔离

- 按最新交互要求取消原文滚到边界后转交外层的行为。原文卡片（含留白与内部滑块）始终消费其区域内的滚轮；到顶、到底、短原文无溢出时都不移动整体。鼠标移到卡片外才滚动外层，切换区域时清除剩余半格增量。
- 原生回归在 96／144／192 DPI 消息环境下检查输入上下边界不带动外层、短输入不带动外层、鼠标位于外部滑块时仍能移动整体；保留原文正常滚动、滑块同步、真实滚轮注入、文字选区与结果滚动测试。
- Debug 原生 CTest、Release 原生套件及两档构建通过。Release 首次在既有 `manual entry` 桌面断言失败，独立重跑全套通过，未修改该断言；结果图位于 `out/wheel-isolation-release-ui/`。DPI 为消息模拟，未实测真实混合 DPI 显示器。本节替代下方历史报告中的“边界交接”交互要求。

## 2026-09-10 Notepad++ Alt+E 取词失败

- 使用 Notepad++ 8.9.7 独立进程、独立设置目录及合成文本复现，未读取或修改用户当前打开的文档。先通过 Scintilla 选区范围确认 263 字节已选中，再发送真实 Alt+E。旧实现快速按放成功，按住 150ms 后先释放 Alt 的用例失败，未出现来源剪贴板更新：`out/selection-fix/before-valid.trace` 与 `before-valid/npp-trial-1.png`。
- 仅增加自然松键等待后同一用例仍失败。`focus.trace` 显示发送复制时来源 `guiFlags=4`（GUI_INMENUMODE），说明 Alt 松键进入菜单模式，Ctrl+C 未由编辑器完成。修复为立即不激活显示浮窗，等待触发键自然释放，让来源处理松键消息；复制前使用有界 WM_CANCELMODE 退出来源菜单状态，再清空剪贴板并发送一次 Ctrl+C。调度让出不用于判空，不恢复 1.5 秒空结果等待。
- 复制接收状态在发送前建立；取消通知使用独立取词请求编号。触发键重复和模拟事件不取消请求，新输入、隐藏、关闭及新请求仍使旧接收失效。来源关闭及来源校验失败显示具体原因。诊断记录阶段、窗口、进程、序列号与事件计数，不记录正文或密钥。
- Debug／Release 独立 Notepad++ 测试通过：12 次真实快捷键覆盖快按、两种松键顺序、按住和重复按下 E，另从翻译窗口连续重新取词 10 次；每次完整填入且测试服务恰好提交一次，警告清除、结果显示。`debug-npp/`、`release-npp/` 保存结果画面；`release-npp.trace` 记录菜单退出路径。
- Debug／Release 完整原生 `ui` 套件通过，回归浏览器、记事本、空白弹窗、延迟复制、用户编辑、剪贴板占用、外来剪贴板拒绝、截图 OCR、滚轮和只读结果。Release 空白原生观测 47ms，浏览器空白 10 次最大观测 46ms；不是跨设备性能保证。外国来源测试改为等待本次实际发送后再写剪贴板，避免新的异步发送流程把测试写入当成复制前的旧内容清空。
- Debug 相关 CTest 3/3（核心、Notepad++、模块边界），Release 相关 CTest 2/2 通过；两档构建通过。可设置 `PCTOOL_TEST_NOTEPADPP` 将真实 Notepad++ 用例加入桌面 CTest，或运行 `PcToolTranslationTests.exe npp <输出目录> <notepad++.exe路径>`；运行前退出 PcTool 避免占用快捷键。
- 限制：所有翻译请求使用注入的测试服务，不访问真实账号，也未执行合成文本中的命令。真实 Windows 桌面前台激活在本环境不可用，使用原生空白窗口替代；96／144／192 DPI 为消息模拟，非真实混合显示器。最初测试误点中新建按钮导致空白文档的尝试不计入产品失败证据，以 `before-valid` 已验证选区的用例为准。

## 2026-09-10 翻译原文滚轮与自绘滚动条联动

- 修复前新增原生用例复现 `input wheel down did not scroll text`：输入框不显示系统滚动条，却仍交给 RichEdit 默认滚轮处理；自绘内部滚动条收到的滚轮又直接转到外层。
- 现在统一按鼠标位置路由输入框、输入卡片留白和内部滑块的滚轮，通过 `EM_SETSCROLLPOS` 滚动并同步自绘滑块。长原文优先消耗滚动，到边界后滚动整体；保留细小滚轮增量并读取 Windows 滚轮行数／整页设置，不改写文本或选区。
- Debug 和 Release 原生窗口套件通过：96／144／192 DPI 消息环境下覆盖上下滚动、半格增量累积、滑块位置同步、留白与滑块上的滚轮、上下边界交接、原文选区保持。96 DPI 额外使用 `SendInput(MOUSEEVENTF_WHEEL)` 检查实际桌面消息路由。
- Debug 相关 CTest（`translation_core`、`module_boundaries`）2/2 通过；两档构建通过。完整原生测试回归快捷键、浏览器及记事本取词、只读选字、展开重绘与生命周期。首次扩展套件在既有 `mouse result selection` 桌面断言失败，独立重跑及 Release 均通过，未删改该断言。
- 证据：`out/input-wheel-debug-ui/`、`out/input-wheel-release-ui/` 下的 `input-native-wheel.png` 与 `input-wheel-{96,144,192}.png`；构建日志前缀为 `out/input-wheel-`。复现命令：`PcToolTranslationTests.exe ui <输出目录>`（先退出 PcTool，避免快捷键冲突）。
- 限制：高 DPI 为消息模拟，未实测真实混合 DPI 显示器；半格滚轮通过消息注入，未实测物理精密触控板。未操作用户截图中的具体页面，也未访问真实翻译服务。系统整页及零行设置有实现分支，本轮未修改用户系统设置进行实测。

## 当前状态：2026-09-09 取消语音播放

按用户要求移除翻译窗口原文和结果的播放按钮、Windows SAPI 朗读服务及链接依赖，并收紧原文区域底部留白。未修改 OCR 模型或录屏音频功能。

验证：Debug／Release 构建、两档核心测试及原生翻译窗口测试通过；原生测试覆盖三档 DPI 消息、箭头展开、只读复制、长文本滚动、真实浏览器 Alt+E 和记事本取词。三档展开采样均未检测到变色帧。真实桌面前台激活不可用，仍沿用测试输出中的限制说明。

清理：已删除评估脚本、评估 C++ 源码和 SAPI 服务文件。自动执行检查拒绝了递归删除及逐文件删除，`out/tts-evaluation` 仍保留 1,541 个文件、462,096,358 字节（约 440.7 MiB）；没有将这些评估文件加入正式程序或安装包。评估进程已结束，临时防火墙规则已移除。

下文中朗读／语音相关内容是历史记录，不代表当前功能。当前回归测试改为确认播放控件不存在，并继续检查箭头展开、只读结果、输入、取词与窗口生命周期。

﻿# 中英翻译验证

日期：2026-09-09。环境：Windows 10 x64、VS 2026、Ryzen 7 5800H，交互桌面解锁。默认 OCR 仍是 PP-OCRv6 tiny。

## 快捷键清空与空内容不弹窗（2026-09-09）

Alt+Q 每次清空原文、结果、旧任务和滚动位置，再显示空白输入窗。Alt+W／Alt+E 先隐藏并清空旧窗口，只在取得本次非空白文字后填入、显示和提交；OCR 识别阶段不打开翻译窗口。无选区、剪贴板没有更新、非文本、空白图、普通空格及全角空格均不显示翻译窗。新请求仍使用取消标记及请求编号，过期 OCR 不得重新弹窗。

最终验证：Debug 翻译测试 2/2、Release 核心 11/11、Release 原生翻译测试通过。测试涵盖重新打开清空、空白截图隐藏、OCR 完成前隐藏、有效 OCR 与真实记事本取词、无选区／非文本／纯空白不弹窗、前台变化取消及过期回调。纯空白用例通过系统 WM_SETTEXT 跨进程消息设置独立测试应用，普通 SetWindowText 不能用于跨进程子控件设置。

证据：`out/translation-entry-build.log`、`translation-entry-tests.log`、`translation-entry-release-build.log`、`translation-entry-release-tests.log`、`translation-entry-release-ui.log`。Release 测试期间 Alt+Q 被外部程序占用，调用行为通过控制器入口验证，不能当作成功注册该全局快捷键。语音缺失、真实混合 DPI 和真实输入法候选窗限制沿用此前报告。

本节替代此前保留上次输入、OCR 阶段显示窗口、空白／失败保留原文的行为。

## 无边框、居中与默认宽度更新（2026-09-09）

- 默认宽度改为 TTime 源码 StoreService 中的 450 DIP。每次 Alt+Q/W/E 调出恢复默认宽度，在鼠标所在显示器工作区中央显示；内容自动增高后保持居中，手动拖动／缩放后保持用户位置，下一次调用再居中。
- 移除 WS_THICKFRAME、系统非客户区绘制和 DWM 非客户区渲染，拖动与边缘缩放改用自身鼠标捕获逻辑。保留四周柔和阴影；Esc 取消拖动恢复起始位置，捕获丢失释放拖动状态。
- 底部常驻状态行及其高度预留已删除。状态仍供失败处理使用，错误通过跟踪提示窗临时显示约 4.5 秒，不改变正文和窗体尺寸。

验证：Debug 翻译 CTest 2/2、Release 基础 CTest 11/11、Release 原生翻译测试通过。原生测试检查居中偏差不超过 1 px、默认宽度、自定义拖动捕获与位移、Esc 恢复、无边框缩放、重复调用重置宽度、置顶失焦保留，以及错误提示前后窗体几何不变。既有内容自适应、滚动、OCR／取词、只读结果和取消测试继续通过。

证据：`out/translation-frame-build.log`、`translation-frame-tests.log`、`translation-frame-release-build.log`、`translation-frame-release-tests.log`、`translation-frame-release-ui.log`。桌面截图 `translation-frame-release-ui/frame-drag.png`、`frame-inactive.png` 验证拖动／失焦无硬边框；三档 DPI 图片由消息模拟生成。真实混合 DPI 跨屏和真实输入法候选窗仍未实测；系统语音资源缺失，未声称实际朗读通过。

以下为历史变更记录；其中此前“保留宽度／鼠标附近定位／底部状态行”的行为已由本节替代。

## 内容自适应与统一滚动更新（2026-09-09）

原文使用原生 EDIT 实际折行数，在 2～10 行之间自适应，超过 10 行显示输入区内部滚动条。展开结果按完整正文高度排列，没有内部结果滚动条；整个正文区域放入独立裁切视口，顶部图钉固定，长内容使用最右侧统一滚动条。默认自适应高度不超过 720 DIP 及显示器工作区，手动调整高度仅在本次打开期间保留，再次快捷键调出恢复自适应并保留宽度。

布局不再按窗口比例分配空白，不再强制提交后放大到 500 DIP。原生文本控件保持原对象，普通重排不重新写入文本，保留选区及整体阅读位置；输入滚动条和外侧滚动条预留固定宽度，避免显隐改变折行。新提交及新取词回到整体内容顶部。

| 用例 | 实际结果与证据 |
|---|---|
| AD-01 原文高度 | 0／1／2 行保持最小两行，10／11 行高度相同，仅 11 行出现内部滚动条；长网址折行通过 |
| AD-02 短结果 | 一行待配置内容高度收紧；卡片折叠与再次展开通过，复制按钮仍不存在 |
| AD-03 长结果 | 80／120 行测试专用结果可通过整体滚动查看，末尾及第二张卡片可达，不出现结果内部滚动条 |
| AD-04 原生鼠标 | 使用 SendInput 实测原生滑块拖动、结果拖选到视口边缘后的整体自动滚动；结果控件滚轮传递通过 |
| AD-05 重排保持 | 调整宽度后原文选区、整体滚动位置保留；手动高度在本次打开保持，下次快捷键调出收紧 |
| AD-06 原有交互 | 只读选字、Ctrl+A/C、禁止编辑、Shift+Enter、输入法组合消息保护、取词／OCR 自动提交、取消及过期回调通过 |
| AD-07 DPI 与图片 | 96／144／192 DPI 消息模拟生成短内容、展开结果、长结果顶部／底部截图，已检查布局和裁切；`out/translation-adaptive-release-ui/` |
| 构建与回归 | Debug、Release 构建通过；Debug 相关回归 6/6、最终翻译测试 2/2、Release 核心 11/11、Release 原生翻译测试通过 |

证据：`out/translation-adaptive-build.log`、`translation-adaptive-regression.log`、`translation-adaptive-tests.log`、`translation-adaptive-release-build.log`、`translation-adaptive-release-tests.log`、`translation-adaptive-release-ui.log`。

早期原生滑块测试因同线程等待松开事件而超时；改为独立输入线程，并通过 GetScrollBarInfo 读取实际滑块位置后通过。模拟 DPI 下原生箭头尺寸不能用控件宽度替代。没有将早期失败或超时计为通过。

边界：真实混合 DPI／负坐标跨屏、真实中文输入法候选窗仍未实测；系统语音数据缺失，仅验证错误反馈和停止生命周期，未声称实际朗读通过。本轮没有接入翻译服务，长结果均为测试程序专用文本，不会出现在正式程序。

## 浮窗样式与提交交互更新（2026-09-09）

- 浮窗由独立透明阴影窗提供四周柔和阴影，阴影不激活、不拦截鼠标；空心直立／实心倾斜图钉区分置顶状态。输入提示相对 EDIT 实际格式矩形向右留 4 DIP，避免与光标重叠。
- Enter 提交、Shift+Enter 换行；截图 OCR 和真实 Ctrl+C 取词成功后走统一提交。两张卡片自动展开，也可独立折叠。未配置翻译源只显示“翻译源待配置”，不联网、不生成模拟译文。
- 结果使用只读 EDIT，支持鼠标选字、Ctrl+A/C 和长文本滚动；复制按钮已移除，键盘复制保留。未配置源的朗读按钮禁用。输入改变清空结果，晚到的源回调不能覆盖新输入或访问已销毁窗口。
- `translation_submission.h` 管理源状态、取消和线程安全结果邮箱；`translation_view` 管理卡片几何和阴影，控制器协调原生窗口及三种入口。

| 检查 | 实际结果与证据 |
|---|---|
| Debug 构建及全量回归 | 35/35 通过，含 24 项桌面回归；`out/translation-style-build.log`、`out/translation-style-regression.log` |
| Release 构建及核心回归 | 11/11 通过；`out/translation-style-release-build.log`、`out/translation-style-release-tests.log` |
| Release 原生翻译窗口 | 通过；`out/translation-style-release-ui.log`，涵盖回车、Shift+Enter、自动提交、复制按钮移除、结果鼠标选区、Ctrl+A/C、禁止修改、长结果滚动、独立折叠、编辑后清空 |
| 异步状态 | 测试专用延迟源验证正常完成、待配置、新提交淘汰旧回调、取消及销毁后回调；测试源和测试文本没有注册到正式程序 |
| 四周阴影 | 已检查桌面合成截图 `out/translation-style-release-ui/shadow-white.png`、`shadow-dark.png`；原生测试检查透明鼠标样式及可见状态 |
| DPI 和图钉 | 96/144/192 DPI 消息模拟生成 `popup-*`、`placeholder-*`、`pinned-*`、`expanded-*`，已检查留白、箭头、图钉、提示文字与光标间距；不是三套真实显示器环境 |
| 真实取词与 OCR | 独立进程、Windows 记事本、截图 OCR 自动提交通过；原有空白、失败保留、取消及过期结果测试保留 |

结果区域注入长文本仅用于测试原生只读控件；没有将其当成真实翻译服务成功。输入法开始／结束组合与 Enter 的消息处理已测，真实中文候选窗操作仍未实测。真实混合 DPI、跨负坐标显示器及高权限程序取词仍未实测。系统语音数据文件缺失，验证了明确错误提示及停止生命周期，未声称实际朗读通过。

复现新增交互：`cmake-build-release/PcToolTranslationTests.exe ui out/translation-style-release-ui`。最新测试和初版报告分开保留，以下为入口功能初版记录。

## 入口功能初版


提供 Alt+Q 输入、Alt+W 截图 OCR、Alt+E 实际 Ctrl+C 取词、图钉、多行编辑、复制和 Windows 本地朗读。没有注册翻译服务，没有联网请求，也没有模拟译文。翻译源卡片显示待配置。

翻译代码位于 `src/translation`。后台 OCR 只持有输入图像和共享取消标记，结果由 UI 定时收取；修改文本、隐藏窗口或启动新请求会使旧结果失效。共享 `FreezeDesktop` 提供干净的冻结桌面，不引用截图窗口私有状态。

## 已执行

| 检查 | 结果与证据 |
|---|---|
| Debug 构建 | 通过；`out/translation-build.log` |
| Debug 全量 CTest | 35/35 通过；`out/translation-regression-final.log`，包含 24 项桌面回归 |
| Release 构建与基础 CTest | 构建通过，11/11 测试通过；`out/translation-release-build.log`、`out/translation-release-tests.log` |
| Release 原生翻译测试 | 通过；`out/translation-release-ui.log` |
| 浮窗与控件 | 输入保留、Esc 隐藏、图钉置顶／失焦、复制、DPI 布局通过；100%／150%／200% 图像位于 `out/translation-release-ui/popup-*.png`，已检查布局及绘制 |
| Alt+E 跨进程 | 独立原生 EDIT 进程与真实 Windows 记事本均通过 Ctrl+C 取词；`notepad-result.png` 展示实际结果 |
| 剪贴板历史 | 正常监听收到复制文本，历史窗口截图包含本次内容；复用现有去重及置顶实现 |
| 剪贴板异常 | 无选区时不读旧内容、非文本复制被拒绝、前台改变取消，均通过 |
| 截图 OCR | 真实桌面选区裁切、直接填入翻译窗通过；干净输入为 `clean-crop.png`，覆盖层图像为 `capture-selection.png` |
| OCR 取消生命周期 | 修改输入不被旧结果覆盖、窗口隐藏后不重新弹出、空白图提示且保留输入，均通过 |
| 选区操作 | 方向／边界和负坐标数学测试、极小选区不提交、Esc 与右键取消通过 |
| 朗读 | 使用系统 SAPI；停止及退出释放通过。本机 Huihui（804）及 Zira（409）有注册记录但语音数据文件缺失，已实测明确的缺少资源提示；未把错误当作成功播放 |
| 快捷键 | 开关注销验证通过；早期 Debug 运行遇到外部 Alt+Q 占用并保留其他入口，最新 Release 测试 Alt+Q 注册／注销通过 |
| 既有功能 | 截图、钉图、长图、OCR、录屏标注、声音、结束／放弃／预览、剪贴板、任务栏和压缩模块的适用测试通过 |

## 测试中修复的问题

- 多行 EDIT 的 WM_SETTEXT 不发送 EN_CHANGE，最初的取消逻辑漏掉此类文本替换。现在同时监测外部 WM_SETTEXT，内部 OCR 提交受 changing 标记保护。
- 跨进程测试最初只调用 SetForegroundWindow，受到 Windows 前台锁限制。现在通过真实鼠标点击激活独立测试窗口，产品仍在发送 Ctrl+C 前核对原前台，不强行抢回用户切换后的窗口。
- 录屏回归原先将桌面任意进程的标准对话框视为自己的“另存为”。测试已限制为当前测试进程的可见对话框，重跑及最终全量回归通过。
- 输入区无溢出时隐藏滚动条，避免空白的灰色滚动槽；长错误提示可悬停查看全文。
- 朗读视觉检查发现 SAPI 返回 0x8004503A；系统注册的中英文 LangDataPath 对应文件均不存在。现在检查已注册语音的数据资源并给出安装／修复提示，同时显式绑定当前系统默认播放设备，避免旧音频设备配置干扰。

## 限制与未实测项目

- 未接入实际翻译源，不能输出译文，这是本轮确认的范围。
- 三档 DPI 使用原生窗口 DPI 场景和截图验证；没有将其当作真实混合 DPI 多显示器测试。真实跨显示器负坐标拖动未实测，负坐标几何已测。
- 使用系统原生 Unicode 多行 EDIT，中文文本、换行及复制已实测；中文输入法候选框与实际拼音组合输入尚未做人工逐项验证。
- 本机现有语音资源缺失，缺少资源提示已经实测；未修改系统组件。正常语音数据环境下的音频回采及人工听音验收仍未执行。
- 权限高于 PcTool 的程序可能拒绝 SendInput，已提供错误提示，但本轮未通过改变用户程序权限进行实测。应用不会自动提权。
- 桌面测试会临时启动并关闭自己的测试窗口和记事本，修改剪贴板；不会编辑用户已打开的文件。

## 复现

```powershell
# VS x64 Developer PowerShell
cmake --build cmake-build-release -j 4
ctest --test-dir cmake-build-release --output-on-failure
cmake-build-release/PcToolTranslationTests.exe ui out/translation-release-ui
# 开启 PCTOOL_ENABLE_DESKTOP_TESTS 的构建目录可运行全部桌面用例：
ctest --test-dir out/build/modular --output-on-failure
powershell -ExecutionPolicy Bypass -File scripts/build-installer.ps1 -SkipDependencyBuild
```


## 2026-09-09 空内容提示（替代此前空内容隐藏规则）

- Alt+W 空白 OCR、Alt+E 无选区、新非文本剪贴板、Unicode 纯空白：显示清空后的翻译窗口，结果折叠，顶部橙色圆角提示“识别内容为空”。有效内容仍自动提交。
- 原生测试覆盖空结果窗口可见、旧文字清除、警告控件可见、3 秒后隐藏、前台变化取消、迟到 OCR 不重新弹窗；跨进程复制及真实记事本取词通过。
- 96/144/192 DPI 消息场景生成 empty-warning-*.png；视觉检查修正提示与输入视口交叠，以及 150% DPI 四舍五入导致末字换行的问题。此为模拟 DPI，不代表真实混合 DPI 多显示器实测。
- TTime 0.9.2 源码 Input.vue 的 translateFun 空分支清空输入、收起结果、调用 ElMessageExtend.warning；messageExtend 延迟 100ms 调用 Element Plus，并设 8px 圆角。本实现参考行为，未复制其清空/恢复剪贴板策略。
- 正在运行的 TTime 为 0.9.15。参考窗口实测受桌面采集 SetIsBorderRequired 0x80004002 阻断；空白截图、无选区、纯空白及有效文本四项运行版对照实验未完成，不能用源码核查替代。
- Alt+Q 被外部程序占用，测试通过控制器入口调用覆盖行为；本轮未改变外部快捷键。真实混合 DPI、权限不足、剪贴板长期占用以及 OCR 依赖缺失的完整桌面故障注入未执行；保留对应具体错误分支。
- 构建与验证记录：out/translation-empty-debug-build.log、out/translation-empty-debug-tests.log、out/translation-empty-release-build.log、out/translation-empty-release-tests.log、out/translation-empty-release-ui.log。界面图位于 out/translation-empty-release-ui。


## 2026-09-09 Alt+E 响应延迟

- 无新剪贴板内容的正常等待由 1500ms 降为 300ms；有新文本立即处理，剪贴板被占用单独保留 1500ms 重试。触发键释放等待和前台校验保留。
- 增加无选区弹窗 700ms 内出现的原生断言，同时核对输入为空、提示准确、未导入旧剪贴板。Debug 本机实测 375ms（包含轮询与窗口显示）；跨进程有效文本及真实记事本取词回归通过。不是任意应用的速度保证，极慢复制应用可能需重试。
- 日志：out/translation-latency-debug-tests.log、out/translation-latency-release-ui.log。Alt+Q 外部占用限制沿用上节。


## 2026-09-09 按 TTime 取词流程重做（替代上一节 300ms 超时方案）

- 已核对 Reference/TTime-main/src/main/service/GlobalShortcutEvent.ts 的 translateChoice/getSelectedText，以及 StoreService.ts 的默认配置：600ms，分两次各 300ms 等待。本机运行版目录 userDataConfig/config.json 的 translateChoiceDelay 也是 600；只读取了该配置项。
- 主动释放修饰键、暂存并清空剪贴板文本、等待、Ctrl+C 按下、等待、释放并读取、恢复原文本、显示结果。取消保留前台检查和旧请求过滤；重复 Alt+E 忽略。未移植与本次响应流程无关的驼峰/下划线转换。
- 新增成功与空选区恢复剪贴板断言，以及复制中切换前台后的原文恢复和模拟按键释放检查。源码/配置核查不等于运行版 TTime 的快捷键实测，运行版截图接口阻断限制仍适用。
- 与 TTime 相同，仅恢复 Unicode 文本；非文本格式不保留。剪贴板历史会正常接收复制和恢复事件，恢复的旧文字可能重新置顶。
- 记录：out/translation-ttime-debug-tests.log、out/translation-ttime-release-tests.log、out/translation-ttime-release-ui.log、out/translation-ttime-installer-build.log。

- 最终 Debug 翻译 2/2、Release CTest 11/11 通过；Release 原生测试第二次运行通过，空选区显示实测 704ms（含两段 300ms 等待、轮询和窗口显示）。首次运行在未改动的 borderless resize 坐标用例失败，原始日志保留为 out/translation-ttime-release-ui-first-attempt.log；未确定偶发原因，不将重跑通过解释为该缩放问题已修复。安装包已重建，默认 Release 已启动。


## 2026-09-09 Alt+E 立即清空与复制（替代 TTime 双等待方案）

- 删除备份、恢复与两段 300ms 延时；快捷键调用中立即 EmptyClipboard 和 SendInput 完整 Ctrl+C。窗口直接显示，不激活以免把尚在队列中的 Ctrl+C 导向翻译框；空内容提示立即可见，真实复制文本到达后填入并聚焦。
- Windows 复制为异步：不再将首个空读取认定为最终结果。保留请求有效期间的剪贴板检查（最长 1.5 秒），该期限只限制迟到结果接收，不延迟弹窗；用户修改输入、点击输入或切换应用后旧结果失效。错误占用另行重试。未使用任意应用全局即时复制的假设。
- 原生测试验证快捷键处理返回时窗口已经可见、成功后保留新剪贴板文字、空选区清空剪贴板、不恢复旧内容、重复快捷键、取消及无遗留模拟按键。Debug 首轮成功版本空选区约 62ms；包含窗口排版与两次重复调用。真实记事本与独立进程选区复制通过。
- 初版“下一次 UI 轮询直接读空并抢焦点”未通过跨进程取词测试，已用上述非激活显示和异步接收修复。
- 日志：out/translation-immediate-debug-build.log、out/translation-immediate-debug-tests.log、out/translation-immediate-release-build.log、out/translation-immediate-release-tests.log、out/translation-immediate-release-ui.log。真实 TTime 对照和多显示器限制沿用前述记录；本轮行为以用户最新要求为准。


## 2026-09-09 Alt+E 空白、重复调用与错误提示闪烁修复

- 取消取词入口强制 Hide；非激活弹窗使用 HWND_TOP，避免空白位置调用后窗口被其他普通窗口遮住。将 Explorer 的桌面宿主切换视作同一桌面来源，避免误取消。
- 翻译窗口在前台时，重复 Alt+E 对本次已取得的原文重新提交；不复制自己的输入框、不切换外部焦点、不隐藏弹窗。回到外部应用则清空上次取词上下文并重新 Ctrl+C。Alt+Q/W 清除重复取词上下文。
- 取词期间不显示“识别内容为空”；确有空结果或异步取词结束仍无新内容时才显示。窗口仍立即显示，等待只影响空结果判定。输入区点击可取消待处理复制，防止覆盖输入。
- 新增连续 5 次重新提交的 WM_SHOWWINDOW 隐藏次数断言，以及普通/延迟复制全过程不得出现空内容提示的检查；新增空白原生窗口、重复空结果和桌面空白入口测试。日志明确记录桌面是否成功激活，不以普通窗口代替真实桌面测试通过。
- 日志：out/translation-repeat-debug-build.log、out/translation-repeat-debug-tests.log、out/translation-repeat-release-build.log、out/translation-repeat-release-tests.log、out/translation-repeat-release-ui.log。


## 2026-09-09 事件驱动即时警告（当前行为，替代此前判空方案）

- 删除无剪贴板更新时的 1.5 秒判空分支；立即显示空警告，允许有效复制稍晚返回时短暂出现。WM_CLIPBOARDUPDATE 接收新结果；计时器仅用于访问占用/焦点转移失败的有界重试，不再轮询等待空结果。
- 每次请求注册剪贴板监听与轻量键鼠取消钩子，使用请求编号丢弃过期取消消息；忽略带标记的模拟输入与触发键释放。成功、取消、隐藏及退出注销监听和钩子。
- 校验剪贴板所有者 PID 与来源 PID，拒绝其他进程内容；无法归属的剪贴板所有者也拒绝。跨进程代理写剪贴板的特殊应用可能无法取词，不宣称覆盖所有应用。
- 重复入口保存来源窗口及 PID，回到来源重新复制，而非重用缓存文字。原生测试源窗口补上正常的 WM_SETFOCUS 子编辑框焦点恢复，使窗口重新激活后 Ctrl+C 能到达编辑器。
- 连续 10 次原来源重新复制通过；新增立即提示断言、有效结果清除提示、迟到文字不覆盖用户输入、外部剪贴板写入拒绝、剪贴板暂时占用但窗口立即显示的检查，并采集含窗口外围的 empty-visible-desktop.png。
- Debug 首轮通过版本空入口约 78ms。真实桌面激活仍受系统前台限制，测试日志分别记录桌面测试或未执行，不能把原生空白测试窗口等同于真实桌面实测。
- 日志：out/translation-events-debug-build.log、out/translation-events-debug-tests.log、out/translation-events-release-build.log、out/translation-events-release-tests.log、out/translation-events-release-ui.log、out/translation-events-installer-build.log。

- 最终 Debug 翻译 2/2、Release CTest 11/11、Release 原生测试通过；正常记事本来源关闭后的明确提示、待取词期间关闭功能后的监听清理均已验证。Release 空入口本机约 62ms，含屏幕截图已检查；不是任意应用/桌面的响应保证。
- 补跑曾出现一次既有截图 OCR 用例失败，保存于 out/translation-events-debug-tests-ocr-failure.log；退出正在运行的 PcTool 后重跑通过，未确定偶发根因，不宣称修复了独立 OCR 问题。安装包已更新，默认 Release 已启动。


## 2026-09-09 修复 Alt+E 内部点击消失及焦点竞争

- 根因：非激活浮窗在收到鼠标按下时尚不是前台窗口，旧判断把它当作外部点击；使用稍后的 GetCursorPos 还会把拖动/移动后的坐标误当作按下位置。
- 低级鼠标钩子现在用 MSLLHOOKSTRUCT.pt 的按下位置和 WindowFromPoint/IsChild 固定命中结果。内部只取消复制监听；外部且未钉住才隐藏。取消已排队时不再读取或重试旧复制。
- WM_ACTIVATE 失焦改为投递一次消息，在焦点切换完成后核对真实前台状态；原来源激活期间和短暂空前台不误隐藏。没有新增判空延迟。
- Debug 原生测试通过真实鼠标事件验证输入框聚焦/编辑、扬声器、图钉、卡片展开、钉住外部点击保留及未钉住外部点击收起。
- 额外检查 Alt+E 注册可用性，连续 10 次实际 SendInput Alt+E 通过 WM_HOTKEY 路由，要求每次剪贴板序列变化、窗口显示、新文字填入，以及随后无失焦消息将其隐藏。不是只调用 HandleHotkey 的测试。若其他软件占用快捷键，日志会明确跳过该项。
- 记录：out/translation-click-debug-build.log、out/translation-click-debug-tests.log、out/translation-click-release-build.log、out/translation-click-release-tests.log、out/translation-click-release-ui.log；实际点击后的屏幕截图为 inside-click-visible.png。真实桌面激活限制沿用前述说明。


## 2026-09-09 应用页面空白 Alt+E 与 TTime 样式对齐（当前版本）

### 修复与实现

- 将取词 revision 与窗口 visibilityRevision 分开。取消复制接收不再调用 Hide；未激活浮窗忽略来源窗口的失焦消息，旧失焦消息不能影响新调用。浮窗先临时置于前方且不激活，实际激活后按图钉状态恢复正常层级。
- 输入监听移到独立消息线程。低级钩子仅记录输入序列并投递带调用编号的消息，不访问外部窗口、不执行布局和文件读取；窗口命中判断在 UI 线程进行。明确的外部点击、切换应用、Esc 收起窗口，内部点击只终止旧取词，避免迟到内容覆盖输入。测试额外让 UI 线程忙碌 1.2 秒，验证输入钩子继续工作。
- 每次 Alt+E 只执行一次被动显示，不在 Ctrl+C 发出后再次重置调用编号，防止使已经排队的取消消息失效。不增加空结果等待；剪贴板访问失败仍单独采用有界重试。
- 主体默认 430 DIP，外部阴影单独绘制。参考 TTime 0.9.2 的 450 像素窗口减左右各 10 像素外边距，以及用户提供的 0.9.15 运行版截图；不宣称两个版本完全一致。
- 原生 RichEdit 使用 14 DIP 正文、21 DIP 固定行距、#606266 文字、#e5e5e5 输入背景和 #ededed 结果背景。统一文本替换后的格式；DPI 重排使用 TOM 暂停格式操作的撤销记录，保留用户文本撤销。粘贴只接收 Unicode 文本，不引入富文本格式。
- 输入与整体视口复用 SlimScrollbar：6 DIP、3 DIP 圆角、#c3c3c3 滑块、无箭头，轨道与父背景一致；仅溢出时出现且持续可见。输入最多 10 行，整体视口最多 720 DIP／当前工作区。拖动、滚轮、选择自动滚动及首行位置均使用实际控件坐标验证。
- 修复视觉检查发现的长原文改短后首行裁切：同步重新应用行距并约束 RichEdit 的实际滚动偏移，不只重置滚动条位置。

### 编号用例与实际结果

| 用例 | 入口、步骤与预期 | 结果 |
|---|---|---|
| REF-01 | Edge 本地页面非编辑空白区域，无文字选区，实际注册 Alt+E 连续 10 次；即时显示空提示，旧文字不导入 | 通过；最终 Debug、Release 轮次各 10 次，最大观测延迟均 63ms |
| REF-02 | 检查空弹窗中央的实际最上层窗口，并采集包含周围背景的截图 | 通过；不是仅检查 IsWindowVisible |
| REF-03 | 同一 Edge 页面选择真实文本，连续 Alt+E 10 次，系统 Ctrl+C 产生剪贴板文本并自动填入提交 | 通过；测试页面仅提供选择／清除选区辅助键，不替换或模拟复制处理 |
| REF-04 | 独立原生 EDIT 来源连续 10 次真实 Alt+E；真实 Windows 记事本取词及关闭来源后再次调用 | 通过 |
| REF-05 | 内部输入、图钉、朗读、卡片点击；外部点击；钉住后外部点击；非激活空窗口直接按 Esc | 通过 |
| REF-06 | UI 忙碌 1.2 秒期间实际发送鼠标点击，恢复后仍正确收起 | 通过 |
| REF-07 | 旧失焦消息、延迟复制后编辑、外部进程写剪贴板、剪贴板短暂占用、关闭功能及退出 | 通过；旧结果不覆盖输入、不自行重新弹窗 |
| REF-08 | 空输入及 1／2／10／11 行、长网址、长原文；短文收紧、超过 10 行显示内部细滚动条 | 通过 |
| REF-09 | 96／144／192 DPI 消息测试：输入两行实际坐标差应为 21 DIP，首行不裁切；输入滑块宽 6 DIP且可拖动 | 通过；已检查各档导出的图像 |
| REF-10 | 长结果整体滚动、真实鼠标拖动滑块、结果拖选到边缘自动滚动、末条可达及缩放保持位置 | 通过 |
| REF-11 | 输入法组合消息、Enter／Shift+Enter、只读结果选择与复制、纯文本粘贴、DPI 重排后撤销 | 通过；输入法候选窗实测见下面限制 |
| REF-12 | 原截图裁切、OCR 自动填入、空白 OCR、取消与迟到 OCR 结果 | 通过 |

### 环境与未执行项

- 浏览器为 Microsoft Edge 152.0.4191.66，使用测试专属 profile 和本地 HTML 页面，不访问网页服务或用户已有浏览器数据。BrowserCase 测试记录真实 WM_HOTKEY、系统剪贴板、窗口层级及屏幕截图。
- **用户图中的 Codex 页面未做自动交互实测**；本轮是同类 Chromium 浏览器页面空白／非编辑区域的替代验证，不能计为该应用实测。
- 实际 Windows 桌面激活在本环境仍未成功，测试明确记录未执行；不以浏览器或原生空白窗口替代桌面实测结论。
- 100%／150%／200% 为发送对应 DPI 消息并检查控件及渲染图；真实混合 DPI、负坐标副屏及跨屏移动未实测。
- 中文候选窗未实测，已测试输入法组合消息期间的 Enter 防误提交。系统语音资源不可用，缺失语音提示与停止逻辑通过，未验证可听播报。
- 未执行 TTime 0.9.15 运行版的实时并排操作，本轮视觉依据为用户截图与本地源码、无敏感字段的字号／宽度配置核查。

### 构建、日志和复现

- Debug、Release 完整构建通过；Debug 非桌面 CTest 11/11、翻译 CTest 2/2；Release 默认 CTest 11/11、独立翻译原生桌面测试通过。
- 构建与测试日志：`out/translation-reference-debug-build.log`、`out/translation-reference-debug-tests.log`、`out/translation-reference-debug-core-tests.log`、`out/translation-reference-release-build.log`、`out/translation-reference-release-tests.log`、`out/translation-reference-release-ui.log`。
- 原生测试复现：`cmake-build-release/PcToolTranslationTests.exe ui D:/projects/c++/PcTool/out/translation-reference-release-ui`。运行前退出 PcTool，避免占用 Alt+E；测试运行期间不要操作键鼠。若快捷键被其他程序占用，测试会记录跳过真实快捷键部分。
- 关键截图在 `out/translation-reference-release-ui/`：`browser-blank-0.png`、`browser-blank-9.png`、`browser-selection.png`、`input-long-96.png`、`input-long-144.png`、`input-long-192.png`、`long-top-144.png`、`long-bottom-144.png`、`before-select.png`。
- 可选诊断：启动进程前设置 `PCTOOL_TRANSLATION_TRACE` 为日志绝对路径，记录调用／请求编号、时刻、前台句柄、显示状态及隐藏原因，不记录原文或剪贴板正文。默认不启用日志。
- 安装包已重建：`out/installer/PcTool-0.1.0-x64-Setup.exe`；构建日志 `out/translation-reference-installer-build.log`。默认程序位于 `cmake-build-release/PcTool.exe`。


## 2026-09-09 焦点框与右侧留白修正

- 删除翻译按钮的 DrawFocusRect，保留键盘焦点、Tab、图钉状态和悬停反馈；检查失焦钉住画面，不再出现系统虚线框。
- 内容视口使用窗体完整宽度，卡片左右均为 12 DIP；外部滚动条在右侧已有留白中绘制，距窗体边缘 1 DIP。取消原来的额外 8 DIP 空槽。
- 输入文字区域与内部滚动条分别排版，取消重复扣减的滚动条宽度；内部滚动条距卡片右缘 3 DIP，文字与滑块间隔 2 DIP。显示和隐藏滚动条时，文字排版宽度不变。
- 首轮实际鼠标测试发现全宽视口遮挡滚动条命中，且右侧缩放带会拦截拖动；已让滚动条保持在视口之上，并让其区域优先返回 HTCLIENT，其余窗口边缘仍可缩放。不能只凭 PrintWindow 的图像判断鼠标命中正确。
- 新增到现有原生用例的断言：有无内部滚动条时编辑区宽度相同、内容视口与窗体等宽、内外滚动条右缘距离、WindowFromPoint 实际命中外部滚动条。真实拖动、滚轮、首行位置、底部可达、窗口缩放和 Alt+E 流程回归通过。
- Debug、Release 构建通过；Debug 翻译 CTest 2/2，Release 默认 CTest 11/11，Release 独立原生测试通过。安装包重建完成。
- 已检查 96／144／192 DPI 消息生成的图像；真实混合 DPI 环境仍未实测。关键画面：`out/translation-spacing-release-ui/placeholder-144.png`、`input-long-144.png`、`long-top-144.png`、`frame-inactive.png`。
- 日志前缀为 `out/translation-spacing-`，包含两档构建、增量构建、Debug／Release 测试、Release 原生测试与安装包构建日志；最初失败的命中诊断保留在 `out/translation-spacing-debug-ui.log`。

## 2026-09-09 Alt+E 空内容窗口恢复与重复调用

- 用户未能回忆发生问题的具体应用和窗口状态，本轮未宣称复现该未知场景。新增测试实际复现最小化窗口不恢复：修复前日志 `out/translation-always-before.log` 报 `blank Alt+E did not restore minimized popup`。
- 显示时显式恢复普通窗口，避免仅设置 SWP_SHOWWINDOW 后仍保留最小化状态。未由用户点击激活的空结果窗口不因临时 WM_ACTIVATE 消息进入失焦关闭逻辑；真实内部点击后仍恢复正常焦点处理。
- 回归又发现 Esc 同时经低级监听和原始键盘消息处理，可能关闭紧接着重新打开的输入框。现在通过带调用编号的监听消息处理一次并消费该按下事件；拖动中 Esc 仍取消拖动。诊断增加窗口关闭、输入框 Esc 和监听 Esc 原因，不记录正文。
- 新增首次真实 Alt+E（窗口尚未创建）、最小化恢复、隐藏／关闭／重复状态循环 12 次、临时激活消息和窗口中央真实最上层命中检查。首次 Esc 后使用真实 Alt+Q 验证重新打开，避免只调用内部接口；保留此回归用例。
- 最终 Debug 翻译 CTest 2/2 通过（34.22 秒）；Release 原生测试通过，默认 CTest 11/11 通过。浏览器空白及选中文本各连续真实 Alt+E 10 次通过；最终 Release 空白浏览器调用最大观测延迟 62ms，原生空内容 63ms，均未增加判空等待。
- 已检查包含窗口外围的 `out/translation-always-release-ui/cold-blank.png`、`blank-restored.png`：空窗口、识别内容为空提示和折叠卡片可见。两档完整构建及安装包重建通过。
- 日志前缀为 `out/translation-always-`，包含构建、Debug／Release 测试、Release 原生测试、诊断和安装包日志。复现命令：`cmake-build-release/PcToolTranslationTests.exe ui D:/projects/c++/PcTool/out/translation-always-release-ui`，运行前退出 PcTool，避免占用快捷键。
- 限制沿用前述记录：真实 Windows 桌面激活不可用；Edge 本地页面是替代应用测试；未自动操作用户截图中的 Codex 页面。DPI 为 96／144／192 消息与图像验证，非真实混合 DPI；真实输入法候选窗与可听语音未实测。不能将这些替代验证表述为所有应用／所有状态均已实测。

## 2026-09-09 翻译源展开重绘与朗读图标

- 展开／收起卡片不再触发窗口重新居中；仍约束在工作区内。移除视口移动时的同步旧布局绘制，测量阶段禁止重绘，排版完成后统一使窗口及子控件失效。内容视口采用子窗口合成绘制，避免 RichEdit、卡片和按钮分别更新时露出中间画面。
- 结果朗读按钮绘制所在卡片的同一背景（包含标题悬停色），不绘制独立按钮底色或悬停方块。图标使用输入朗读按钮的 RGB(73,77,82)，未配置源仍保持禁用，不改变朗读条件。
- 原生用例在 96／144／192 DPI 消息环境下各连续展开／收起 30 次，验证窗口左上角稳定、结果显隐正确及输入选区保持；生成 card-hover-*.png、card-normal-*.png。静态图像用于检查底色和图标，不将其单独作为动态无闪烁证明。
- Debug、Release 构建通过；Debug 翻译 CTest 2/2，Release 默认 CTest 11/11、Release 翻译原生测试通过。回归 Alt+E、复制、滚动、输入法组合消息及 OCR 入口。日志前缀为 out/translation-card-，关键图像位于 out/translation-card-release-ui/。
- 安装包已重建。真实混合 DPI、真实输入法候选窗以及用户当前页面的自动操作未实测；本轮未操作 Codex 窗口。

## 2026-09-09 根据用户录屏修复整窗空白中间帧

- 用户视频 `Recording-20260909-160548-25837578.mp4` 为 30 fps。解码第 98、99、100 帧显示：约 3.27 秒处，翻译窗主体短暂消失、露出后方页面，阴影仍在，下一帧恢复。证据保留在 `out/translation-flicker/frame-098.png`、`frame-099.png`、`frame-100.png`。这证明上一轮“已修复闪烁”的结论不足，静态截图和位置断言遗漏了中间帧。
- 新增独立线程连续读取桌面上本应保持灰色的输入区，同时记录窗口命中、可见性和层级；在主线程展开／收起 30 次期间检测异常像素。修复前 144 DPI 用例抓到 4/63 次异常采样，颜色变为白色、窗口仍可见，自动断言失败。日志 `out/translation-flicker/before-test.log` 和 `before/toggle-samples-144.csv`。
- 将 WS_EX_COMPOSITED 从内部视口移到顶层翻译窗口，使外层背景、输入区、按钮和结果控件在同一个窗口树内合成绘制。保留先前的图标样式和位置稳定修复，没有改变输入、取词或翻译功能。
- 修复后同一用例：Debug 96／144／192 DPI 分别 0/68、0/77、0/77 异常采样；Release 分别 0/67、0/60、0/73。每档执行 30 次展开／收起。这里统计的是连续桌面采样次数，不宣称覆盖显示器每个刷新帧。
- Debug、Release 完整构建通过；两档原生翻译测试通过，Release CTest 11/11。原有输入选区、只读结果、滚动、快捷键、OCR、取消和生命周期回归通过。日志位于 `out/translation-flicker/`，含 before／after／release 测试与安装包构建记录。
- 保留环境限制：DPI 测试为 96／144／192 消息模拟，并非真实混合 DPI 显示器；用户视频作为失败证据读取，未自动操作其中的 Codex 页面。用户设备的全部应用组合及每个物理刷新帧未穷举测试。

## 2026-09-09 仅箭头切换翻译源展开状态

- 原先整行标题参与展开命中，禁用喇叭的鼠标消息转发给父窗口时也会折叠。PopupLayout 现在单独定义箭头按钮区域，绘制和鼠标命中共用该区域；标题、空白区域及喇叭均不切换展开状态。
- 新增消息级和真实鼠标检查：标题点击保持折叠、箭头点击展开、禁用喇叭点击保持展开、喇叭命令不折叠、箭头可独立关闭第一张卡片而保留第二张。原本要求点击标题展开的旧用例同步更新。
- Debug、Release 构建通过；最终 Debug 翻译 CTest 2/2 通过（39.03 秒），Release 默认 CTest 11/11、原生翻译测试通过。三个 DPI 档位连续桌面采样未发现输入区空白。
- 保留初轮失败记录：一次鼠标选字断言失败、一次旧的标题点击展开断言失败、一次首次快捷键超时；旧交互断言已更新，最终两档原生测试通过，不删除失败日志。日志前缀为 out/translation-arrow-，最终 Debug 日志为 translation-arrow-debug-tests-verified.log。
- README 和安装包已更新。DPI、真实输入法候选窗等环境限制沿用前述报告。
