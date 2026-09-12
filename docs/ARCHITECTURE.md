# PcTool 模块边界

| 目录 | 职责 |
|---|---|
| `src/app` | 入口、配置、应用消息、会话互斥和窗口生命周期协调 |
| `src/monitor` | 采样、任务栏显示及功能菜单 |
| `src/hotkeys` | 快捷键查看、编辑与校验界面；通过保存回调交由应用层持久化和注册 |
| `src/clipboard` | 100 条文本历史、剪贴板监听、列表与详情窗口 |
| `src/system_tools` | 系统画图启动适配器 |
| `src/archive` | 定位和启动程序自带的原版 7-Zip，处理缺失组件与启动失败 |
| `src/capture` | 桌面截图、钉图与静态图片编辑窗口 |
| `src/long_capture` | 稳定帧采集、垂直拼接、分块结果及会话界面 |
| `src/ocr` | 本地模型推理、结构化结果、分块合并、结果布局及窗口 |
| `src/translation` | 翻译输入浮窗、三组快捷取词、独立截图选区、源配置及四种翻译服务适配器 |
| `src/recording` | 音视频采集编码、录制标注交互、时间线、结果预览 |
| `src/shared/image` | 原图、标注数据、视口坐标转换 |
| `src/shared/annotation` | 标注样式、文字度量、矢量绘制及透明图层合成 |
| `src/shared/ui` | 图标、DPI、公共工具窗体与控件 |
| `src/shared/platform` | Windows 图像／采集平台适配 |
| `src/shared/async` | 取消与过期结果控制 |
| `third_party/7zip` | 原版 26.03 完整源码，独立构建，无产品改名或功能裁剪 |
| `cmake`、`scripts`、`packaging` | 源码清单、依赖构建、部署与安装卸载 |

应用层通过工厂和结果回调协调窗口。共享层不引用 `app`、`capture`、`ocr` 等具体窗口，边界约束由 `module_boundaries` 测试保护。截图与图片编辑仍在 `capture` 模块内部共享原有编辑状态与命中逻辑，本轮未重写这些交互。

原来的 `capture_core` 拆为共享图像、拼接、OCR 合并、录屏时间线及取消状态。原来的 `capture_tools.cpp` 中长截图窗口和采集任务移到 `long_capture`，仅保留应用协调。画图启动从剪贴板模块移出。

共享标注不再构造 `ScreenshotOverlay`。截图和录屏共同调用独立 `AnnotationPainter`，沿用原来的抗锯齿路径、文字度量和换行实现；`AnnotationRenderer` 管理自己的 GDI+ 生命周期及录屏图层合成。马赛克采样保留在图片编辑模块。

7-Zip 通过独立 EXE 和 DLL 组成运行模块，位于 `modules/archive`，不搜索系统安装路径。独立进程可在 PcTool 退出后继续压缩任务，也保持原版扩展、帮助和选项兼容。压缩模块不依赖截图、剪贴板或 OCR；安装器统一部署并管理系统集成。

源码清单位于 `cmake/PcToolModules.cmake`。新增模块头文件必须列入该清单，兼顾当前中文 MSVC/Ninja 环境的显式头文件依赖。安装清单位于 `cmake/PcToolInstall.cmake`，安装脚本由暂存目录生成精确文件清单，卸载不递归删除用户选择的目录。

翻译源目录与 DPAPI 配置存储在 `source_config`，原生设置视图在 `source_settings`，WinHTTP 传输、服务协议和 SSE 解析在 `source_provider`。`translation_submission` 负责按稳定源 ID 调度、取消和合并增量结果；`translation_controller` 管理浮窗、动态只读控件和应用入口。服务回调只持有带请求编号的邮箱，不持有窗口指针。网络请求只由验证或翻译提交发起，设置变化只刷新源目录并取消旧请求，不重新发送输入。
