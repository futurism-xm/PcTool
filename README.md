# PcTool

一个基于 C++ / Win32 的 Windows 桌面工具，集成性能监控、截图与长截图、GIF 录制、屏幕录制、中英翻译、剪切板历史和 7-Zip 压缩解压。

通过任务栏或系统托盘访问常用功能，也可以在快捷键管理中设置自己的操作习惯。

## 功能

| 功能 | 说明 |
| --- | --- |
| 性能监控 | 显示 CPU、内存、上传与下载速度等信息，支持任务栏显示与托盘入口切换 |
| 截图与钉图 | 窗口识别、选区截图、画笔、形状、箭头、文字、马赛克等标注，支持钉图、复制与保存 |
| 长截图 | 手动双向滚动拼接、单击自动向下截取，支持整个选区或选区内单个竖向滚动区域 |
| GIF 录制 | 1～30 FPS、倒计时、暂停、标注、光标与激光指示，结束后循环预览 |
| 屏幕录制 | MP4 输出，支持系统声音、麦克风、暂停、标注、光标与激光指示 |
| 离线文字识别 | 使用 PaddleOCR 模型和 ONNX Runtime 在本机识别截图文字 |
| 中英翻译 | 输入、截图、划词三种入口，支持多个翻译源及独立结果展示 |
| 剪切板历史 | 查看本次运行期间的文本记录，快速选择和复制 |
| 7-Zip | 集成文件管理器、压缩与解压及资源管理器右键菜单 |

长截图通过图像匹配拼接，动态布局、重复图案或重叠不足时可能暂停；目前不支持横向或多个列表同时拼接。GIF 最多使用 256 色，高 DPI 下会按屏幕缩放比例降低输出分辨率，以减小文件体积。

截图翻译和文字识别按需加载本地 OCR 模型，识别结束后释放模型会话及推理缓存，避免大图识别后持续占用大量内存；识别期间的内存峰值仍随图片内容和大小变化。

## 安装与使用

系统要求：**Windows 10 2004（19041）及以上的 64 位 Windows，或 Windows 11**。录屏需要可用的图形设备及 Windows 媒体组件，受保护内容可能无法采集。

在本仓库的 **Releases** 页面获取已发布的 `PcTool-<版本>-x64-Setup.exe`，运行安装向导并选择安装位置。安装后创建桌面和开始菜单快捷方式；完整安装包包含 OCR 依赖及 7-Zip，不需要另装 Python 或外部编码器。

截图后可从工具栏进入长截图、文字识别、GIF 或屏幕录制。录制结束后进入预览：点击“完成”复制缓存文件供粘贴，点击下载并选择位置才另存文件。

### 默认快捷键

首次运行默认启用中英翻译和屏幕截图；升级后保留已经保存的开关选择。

| 操作 | 快捷键 |
| --- | --- |
| 输入翻译 | `Alt+Q` |
| 截图翻译 | `Alt+W` |
| 划词翻译 | `Alt+E` |
| 屏幕截图 | `Ctrl+Alt+A` |
| 剪切板历史 | `Alt+C` |

以上组合可以修改或清空。任务管理器、画图、7-Zip 和快捷键管理也支持绑定，默认未绑定。快捷键与其他软件冲突时，请在快捷键管理中调整。

### 配置翻译源

从“中英翻译”的子菜单打开“翻译源设置”，添加服务、填写参数并验证。当前支持：

| 服务 | 所需配置 |
| --- | --- |
| OpenAI 兼容接口 | API 基础地址、API Key、模型名称 |
| DeepL | Free / Pro 类型、API Key |
| 微软 Azure Translator | 订阅密钥、区域 |
| 百度通用翻译 | App ID、密钥 |

PcTool 不提供内置公共密钥或免费翻译额度，费用及可用性由所选服务决定。翻译会将提交的文字发送至配置的服务；**离线 OCR 不等于离线翻译**。含汉字的文本默认译为英语，其他文本译为简体中文。

### 数据与卸载

应用管理的配置与缓存直接位于安装目录，不再创建用户 SID 子文件夹：

```text
PcTool 安装目录/
├── PcTool.exe
├── Uninstall.exe
├── Data/                # 功能设置、快捷键、翻译源及 7-Zip 设置
└── Cache/<YYYY-MM-DD>/   # 按本地日期存放截图、GIF、录屏及临时文件
```

翻译密钥使用当前 Windows 用户的 DPAPI 加密。请勿公开 `Data` 或 `Cache`，也不要将其中的密钥配置直接用于其他用户或电脑。

升级时将当前用户原 SID 目录中的配置移到 `Data`，已有新位置文件不会被覆盖，冲突文件保留在旧目录。旧缓存保留原路径以兼容剪贴板引用，按相同的七天规则清理，清空后移除旧 SID 目录。安装器对 Data/Cache 的写入权限保持不变。

截图“完成”同时提供图片和 PNG 文件剪贴板格式；GIF、MP4 的“完成”复制缓存文件。自 v0.1.2 起，缓存按本地日期分文件夹，内部按 `Screenshots`、`Gif`、`Recordings` 和 `SevenZip` 分类。每次启动只清理日期距当天超过 7 天的缓存，恰好 7 天、当天和未来日期的文件夹保留；运行中不会定时清理。

清理保留当前剪贴板引用、正在使用或无法删除的文件，对应日期文件夹在清空后才删除；剪贴板暂时无法读取时跳过本次清理，下次启动重试。旧版未分日期的受管缓存按文件修改日期使用相同的 7 天规则，不迁移文件路径。系统剪贴板历史中的旧文件引用不保证长期有效。主动另存的文件位于用户选择的位置，不受缓存清理影响。

使用安装目录内的 `Uninstall.exe` 卸载。确认后先停止本次安装的全部任务，包括录屏、GIF、翻译和内置 7-Zip；任务未响应时会强制结束，确认进程实际退出后才删除文件，未完成结果可能不可用。卸载期间不能重新启动这些功能。

卸载清理受管文件、数据、快捷方式和本次安装的右键菜单等系统注册；如果之前独立安装过 7-Zip，则恢复其注册。必要时按资源管理器原来的用户、会话和权限恢复它，以释放扩展；恢复进程会在终止原进程前准备好。文件仍有短暂占用时等待重试，最终确实无法删除的文件才安排重启清理，并在详细记录中列出。用户自行放入的无关文件和主动另存的文件不在清理范围内。不要用直接删除文件夹代替卸载。

内置 7-Zip 设置读取或写入结束后即释放 hive；卸载先通知系统刷新扩展并等待正常卸载，仍有扩展占用时才重启对应资源管理器。重启会关闭受影响的文件夹窗口，PcTool 不记录或重新打开这些窗口，也不恢复其位置；桌面外壳仍按原用户身份恢复。清理未完成时保留卸载器，方便释放占用后重试。

## 从源码构建

需要 Windows、带 C++ 桌面开发工作负载及 Windows SDK 的 Visual Studio、CMake，以及 PowerShell。当前预设使用 **Visual Studio 2026（Visual Studio 18 2026 生成器）**；CMake 必须支持该生成器。请在 Visual Studio 的 **x64 Developer PowerShell** 中执行以下命令。

首次准备依赖需要联网，脚本会校验固定版本 OCR 下载文件的 SHA256。7-Zip 使用仓库内的源码构建，不需要 `Reference` 目录。

```powershell
# 在仓库根目录执行
powershell -ExecutionPolicy Bypass -File scripts/setup-ocr.ps1
powershell -ExecutionPolicy Bypass -File scripts/build-7zip.ps1
powershell -ExecutionPolicy Bypass -File scripts/setup-package-tools.ps1

cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug
cmake --build --preset vs2026-x64-release
```

主程序输出为 `out/build/vs2026-x64/Release/PcTool.exe`。运行时需保留同目录下部署的模型、运行库和模块，不能只复制主程序 EXE。

### 测试与安装包

```powershell
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
ctest --test-dir out/build/vs2026-x64 -C Release --output-on-failure

powershell -ExecutionPolicy Bypass -File scripts/build-installer.ps1 -BuildDirectory out/build/vs2026-x64
```

安装包输出到 `out/installer/`。构建产物通过 GitHub Releases 分发，不提交到源码仓库。

桌面集成测试可在配置时增加 `-DPCTOOL_ENABLE_DESKTOP_TESTS=ON`；需要可交互、未锁定的 Windows 桌面及相应音视频设备。部分开发评估脚本需要额外测试数据或运行环境，不属于普通构建前置条件。

### 目录结构

```text
src/          应用与各功能模块源码
Resources/    程序图标等构建资源
cmake/        模块清单与安装规则
scripts/      依赖准备、构建与打包脚本
packaging/    安装器与组件说明
third_party/  7-Zip 源码、OCR 依赖说明及许可证
tests/        原生测试、测试夹具与验证记录
docs/         架构与资源说明
```

架构说明见 [ARCHITECTURE.md](docs/ARCHITECTURE.md)，OCR 依赖见 [third_party/ocr/README.md](third_party/ocr/README.md)。

## 参考项目与致谢

感谢以下项目和产品提供的实现思路与交互参考：

| 项目 / 产品 | 在 PcTool 中的用途 |
| --- | --- |
| [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) | 性能监控功能参考 |
| [Gif123](https://github.com/aardio/Gif123) | GIF 录制功能参考 |
| [TTime](https://github.com/InkTimeRecord/TTime) | 翻译功能与配置交互参考 |
| [7-Zip](https://github.com/ip7z/7zip) | 集成源码，提供压缩、解压及文件管理能力 |
| QQ 截图 | 截图和长截图交互参考 |
| [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) | 离线文字识别模型 |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | 本地模型推理运行库 |
| [Simple Icons](https://github.com/simple-icons/simple-icons) | 翻译源标识，详见[图标来源说明](docs/TRANSLATION_ICONS.md) |

以上名称和商标属于各自权利人，列出参考来源不代表官方合作或背书。

## 许可证

PcTool 自有代码目前尚未声明统一的开源许可证；公开源码不代表已授予任意使用、修改及分发许可。

第三方组件遵循各自许可证：7-Zip 主要采用 LGPL，部分代码另有 BSD 等许可及 unRAR 限制；ONNX Runtime 使用 MIT，PaddleOCR 模型使用 Apache-2.0。具体以组件附带的许可证及通知为准，参见 [组件说明](packaging/license.txt)、[7-Zip 许可证](third_party/7zip/DOC/License.txt) 和 [OCR 许可证目录](third_party/ocr/licenses)。致谢不能替代相应的许可义务。

