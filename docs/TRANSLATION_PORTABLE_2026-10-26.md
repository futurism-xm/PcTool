# 翻译源直接复制迁移（2026-10-26）

## 原因与修改

- P2：`source_config.cpp` 的 v1 `secret` 使用当前 Windows 用户的 DPAPI 加密；复制到另一台电脑后无法解密。配置位置又是 `Data/<SID>/Translation`，直接复制到 `Data/Translation` 不会被读取。
- 新配置为 v2 JSON，直接保存可读取的密钥，统一位置为 `Data/Translation/sources.json`，不依赖 Windows 加密密钥或 SID。配置 ID、顺序、四种服务的参数、验证和启用状态保留。仅支持 PcTool 相同格式，不能解释其他软件的任意配置格式。
- 按用户要求不兼容旧版加密配置：已移除 DPAPI 解密、旧 SID 目录回退读取及自动转换逻辑。仅接受新位置的 v2 文件；旧 v1 和未知版本提示不支持，不覆盖原件。旧 SID 目录不参与翻译源读取。
- 普通用户可读写共享 Translation；其他功能继续使用私有 SID 目录，程序和依赖保持只读。安装迁移排除该共享目录，升级不挪走文件，卸载仍将受管 Data 一并删除。
- 保存使用每次独立的临时文件，避免多个保存者竞争同一个 `sources.tmp`，保留刷新及原子替换逻辑。多个实例不提供实时配置同步，复制前退出程序。

## 使用方式

1. 安装新版并配置一次翻译源，生成 `Data/Translation`。
2. 退出两台电脑的 PcTool，备份目标已有翻译源，将旧电脑的 `Data/Translation` 复制覆盖到目标安装目录的 `Data` 下。
3. 启动新版 PcTool，读取复制后的配置。服务自身的密钥、余额、网络/IP 限制仍适用。

v2 文件含可读取的密钥，同一安装的本地用户共享配置，请勿公开或上传。不再读取、转换旧 v1 加密文件；误放旧版文件时，备份并移走旧 `sources.json` 后重新配置。

## 验证记录

- 修改前现有 Release 翻译核心、翻译源和模块边界测试 3/3 通过。
- 移除兼容后重新执行 Release 翻译存储、独立进程复制、翻译核心、四服务 HTTP 回环和模块边界回归，5/5 通过，结果见 `v2-only-release-final-tests.log`。
- 移除兼容后的 Debug 同组回归 5/5 通过（`v2-only-debug-final-tests.log`）；普通权限复制及保存复测通过。存储测试程序不再链接 Crypt32，已检查可执行文件无 DPAPI/Crypt32 导入。
- 新增测试覆盖中文和空格路径、全部字段、独立构造的 v2 文件、旧 SID 目录被忽略、v1/未知版本拒绝且文件不变、并发原子保存。测试不再生成或解密 DPAPI 配置。
- 使用实际中等完整性、管理员组禁用的 token runner 验证：安装迁移后，仅复制 Translation 的独立进程读取、保存及文件字节一致；原普通权限启动、设置、自启和 7-Zip 存储回归通过。
- PowerShell 语法和 `git diff --check` 通过。测试脚本曾遇到 Windows PowerShell 的 UTF-8/BOM 及 `Get-FileHash` 模块发现差异，已修复并复测通过。
- 回归发现中文 MSVC/Ninja 的显式头文件依赖遗漏了 `translation_source_tests.cpp`，删减 SourceStore 成员后测试复用了旧结构布局的对象文件，出现退出时崩溃。已为该文件补齐依赖并重建验证；没有通过保留废弃成员规避。
- 安装器逻辑沿用此前已通过的完整隔离回归：升级保留 Translation 文件、卸载删除 Data、外部 x86/x64 DLL 占用、立即重装、独立 7-Zip 恢复、无关文件保留及无占用删净根目录；生产注册快照不变，未新增重启删除队列。日志见 `out/translation-portable-20261026/isolated-package.log`。移除兼容后的最终安装包使用新暂存目录构建，136 个载荷文件、1454 个源码输入及安装包 SHA256 均校验一致（`v2-only-hash-check.log`）。

本次没有访问第二台真实电脑、没有使用真实服务密钥进行外网翻译验收。可移植性由独立构造的无 DPAPI 文件、跨目录/进程复制及普通权限读写验证；未把这些测试称为第二台电脑实测。构建保留现有编译告警和 NSIS 缺少 LegalCopyright 的告警，本次没有宣称性能提升。

## 安装包

- 最终文件：`out/installer/PcTool-0.1.2-x64-Setup.exe`
- BuildId：`20261026T022145553Z-730c2a606111`
- SHA256：`9148A6A8E64D414D3578FE8E969FAFC22034C2CED77821661C3B84B40698DA9C`
- 构建清单：`out/package-manifest/20261026T022145553Z-730c2a606111/`
- 测试证据：`out/translation-portable-20261026/`
