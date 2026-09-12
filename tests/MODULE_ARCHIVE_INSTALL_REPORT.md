# 模块重构、7-Zip 与安装包验证

日期：2026-09-08。环境：Windows 10 x64，Ryzen 7 5800H，约 16 GB 内存，VS 2026/MSVC 14.51，交互桌面可用。未使用其他已安装的 7-Zip 运行库。

## 实现范围

- 原有源码按功能迁移，拆分混合的 capture_core 和长截图会话；录屏共享标注移除对截图窗口的反向依赖。重构前备份位于 `out/refactor-backup/before-module-refactor.zip`。
- 原版 7-Zip 26.03 的文件管理器、GUI、控制台、完整格式 DLL、64/32 位 Shell 扩展及两种 SFX 均从固定源码编译。保持原版界面和功能，PcTool 仅提供入口与统一部署。
- 安装包支持目录选择、统一卸载、升级、原版右键注册及既有注册值备份；不强制更改文件关联。每次从全新暂存目录打包，卸载按文件清单移除，保留用户文件。

## 已执行及结果

| 用例 | 结果 | 证据／内容 |
|---|---|---|
| Debug 编译 | 通过 | `out/modular-final-build.log` |
| Release 编译 | 通过 | 默认 `cmake-build-release`，`out/release-build.log` |
| 7 项既有基础 CTest | 全部通过 | 图像、拼接、OCR、剪贴板、任务栏、标注透明度、图标和放弃录屏文件清理 |
| 23 项既有桌面集成测试 | 全部通过 | `out/modular-tests.log`；钉图交互、剪贴板、菜单输入、录屏标注／激光／声音／结束／放弃／预览、OCR、长图与编辑器 |
| 模块边界 | 通过 | `module_boundaries`，共享模块及压缩／系统工具无反向功能依赖 |
| 压缩功能 | 通过 | `archive_integration`：7z、ZIP、TAR、WIM 往返；中文与空格路径；原文 SHA256；密码加密与错误密码拒绝；分卷；更新、删除；SFX |
| 原版右键扩展 | 通过 | `archive_shell`：加载源码构建的 DLL，经 COM 接口初始化真实文件并生成菜单 |
| 安装与原版窗口 | 通过 | `out/installer-tests.log`；自定义中文／空格目录，`PcTool --archive` 启动模块内 7zFM，中文原版窗口正常打开 |
| 覆盖升级、占用扩展 | 通过 | 将 Shell DLL 映射到进程后升级，安装器更换新 DLL，旧映射文件安排重启清理 |
| 卸载保留数据 | 通过 | 测试目录中的无关文件、视频目录保留；开发版启动项不变；64/32 位 Shell 注册恢复到安装前状态 |
| 打包依赖 | 通过 | 包含 VC++ 运行库、OCR、帮助语言及源码许可证；不含测试程序和用户录屏；OCR 版本文件与源依赖 SHA256 相同 |

首轮新增压缩脚本在 Windows PowerShell 下出现工具命令发现及预期错误输出处理问题，已改为 .NET 哈希并显式处理错误密码的非零退出码，重跑通过。首轮安装测试的等待方式误等到文件管理器退出，已改为仅等待 PcTool 启动器进程，重跑通过。这些失败没有作为通过结果计入。

`out/build/modular` 启用桌面用例后共 33 项；默认 Release 关闭桌面用例，共 10 项。默认测试不更改系统安装；安装测试需要管理员环境且无已安装正式版 PcTool，使用独立测试目录。

## 来源与复现

- 源码：`Reference/7zip-main` 的固定副本 `third_party/7zip`；版本见 `C/7zVersion.h`，26.03 / 2026-09-03。
- 仅语言／帮助取自 `https://7-zip.org/a/7z2603-x64.exe`，SHA256 `0859C524B8A63551848F0C246ABDDCB1D0B7B656B0FBFE879F8D85E61A9E6EDD`。未执行其安装程序或使用其编译二进制替代源码构建。
- NSIS 3.12 便携编译器 ZIP：SHA256 `56581F90DB321581C5381193D796FFFCF2D24B2F8FED2160A6C6A3BAA67F2C4F`，来源和校验逻辑见 `scripts/setup-package-tools.ps1`。
- 安装文件 SHA256 清单位于 `out/package-manifest/files.json`；随包源码及许可证见 `licenses/7zip-source.zip` 与 `modules/archive`。

```powershell
# VS x64 Developer PowerShell
powershell -ExecutionPolicy Bypass -File scripts/build-7zip.ps1
powershell -ExecutionPolicy Bypass -File scripts/setup-package-tools.ps1
cmake --build cmake-build-release -j 4
ctest --test-dir cmake-build-release --output-on-failure
powershell -ExecutionPolicy Bypass -File scripts/build-installer.ps1 -SkipDependencyBuild
# 管理员测试会话，无正式安装版时：
powershell -ExecutionPolicy Bypass -File tests/installer_integration.ps1 -Setup D:/projects/c++/PcTool/out/installer/PcTool-0.1.0-x64-Setup.exe
```

## 限制与未执行项目

- 本轮未遍历 7-Zip 所有格式／算法及全部原版设置组合，完整功能来自未裁剪的上游源码，不把抽样测试等同于全格式认证。
- 已验证 Shell 注册和原版 COM 菜单生成；未在 Windows 11“显示更多选项”及实际 32 位文件管理器中实测。32 位 DLL 已编译并验证注册路径。
- UI 回归中的 100%／150%／200% 图像来自已有原生测试的 DPI 场景；没有声称实测真实混合 DPI 多显示器。
- 未实际重启电脑验证延迟删除；被映射的旧 DLL 已安排由 Windows 重启清理。未模拟断电、系统磁盘耗尽及不同用户的 UAC 凭据。
- 当前环境没有独立安装的旧版 7-Zip；验证了安装前无注册时的恢复，真实多版本并存及其他软件后续改写注册仍需专门环境测试。
- 安装包尚未配置代码签名。应用设置和用户生成文件按设计保留，卸载后保留内容的目录可能仍存在。
