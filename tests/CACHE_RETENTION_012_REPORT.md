# v0.1.2 日期缓存与七天清理测试报告

日期：2026-09-14。测试在独立安装目录夹具内执行，没有清理用户当前安装的缓存。

## 实现行为

- 新文件进入 `Cache/YYYY-MM-DD/<类型>/`，使用创建缓存路径时的本地日期；类型为 Screenshots、Recordings、Gif、SevenZip。配置直接存入 `Data`，两者均不再创建 SID 子目录。文件名仍使用唯一标识，不改变图片/文件剪贴板格式和下载行为。
- 主实例启动时清理日期差严格大于 7 的目录。例如 2026-09-14 启动时，2026-09-07 保留，2026-09-06 及更早可清理；不是按小时计时，也不在运行中周期清理。
- 日期按日历计算，跨月、跨年和闰年不会依赖固定月份天数。未来日期和非法/未知文件夹名称保守保留。
- 旧版 Screenshots、Recordings、Gif、SevenZip 目录按文件最后修改的本地日期判断七天期限，不移动文件、不改变已有剪贴板路径。
- 当前用户旧 SID 缓存路径也按同样规则清理，清空后移除旧 SID 目录；不会清理其他用户的旧 SID 目录。旧 SID 配置合并到 Data，保留原有字节和 DPAPI 密文，不覆盖新位置已有文件；发生冲突时保留旧文件。7-Zip 单独启动也可迁移其旧设置 hive。
- 当前剪贴板引用、文件独占访问失败、7-Zip 活动租约继续受保护。删除失败和受保护文件留到下次启动重试；日期目录仅在清空后删除。剪贴板无法打开时跳过本次清理；不清空剪贴板。
- 遍历不进入重解析点。用户明确选择的另存位置不参与清理。
- 内置 7-Zip 每次请求临时路径时重新读取本地日期，并为该进程用过的每个日期保留租约。更新了构建脚本：本地存储头文件变化时重新编译其直接引用对象，避免 nmake 使用旧的存储逻辑。

## 已执行验证

| 项目 | Debug | Release |
| --- | --- | --- |
| 主程序及存储测试构建 | 通过 | 通过 |
| 按当天日期、类型创建唯一路径 | 通过 | 通过 |
| 七天保留、八天清理、清空后删除日期目录 | 通过 | 通过 |
| 当天、未来、非法日期、未知目录保留 | 通过 | 通过 |
| 闰日合法性、跨月/跨年日期差 | 通过 | 通过 |
| 旧版缓存按修改日期清理，近期文件保留 | 通过 | 通过 |
| 文件剪贴板引用、文件占用、7-Zip 租约保留及释放后重试 | 通过 | 通过 |
| 独立窗口线程持有剪贴板时跳过清理 | 通过 | 通过 |
| 到目录外的符号链接不被跟随 | 通过 | 通过 |
| 本地配置保存、失败不覆盖、7-Zip hive 重启读取 | 通过 | 通过 |
| 独立目录 ACL、受管目录卸载清理、无关文件保留 | 通过 | 通过 |
| capture_core、ocr_layout、translation_core | 3/3 | 3/3 |

移除 SID 层后补充的专项测试：首次运行不创建 SID 目录、旧设置及文件字节保留、冲突不覆盖、旧 SID 文件剪贴板引用保护、七天边界及清空后移除 SID、7-Zip 单独启动迁移旧 hive 并读回原值。新路径下重新验证安装目录权限与受管目录卸载。

7-Zip x64 组件及 x86 资源管理器扩展重新构建成功；不是仅复制之前的二进制。

复测命令（在 VS x64 Developer PowerShell 中）：

```powershell
cmake --build cmake-build-release --target PcToolStorageTests PcToolArchiveStorageTests
./tests/run_storage_tests.ps1 -Binaries ./cmake-build-release -Output ./out/cache-retention-check
ctest --test-dir cmake-build-release -R '^(capture_core|ocr_layout|translation_core)$' --output-on-failure
```

日志位于开发目录 `out/cache-012-storage-{debug,release}.log`、`out/cache-012-{debug,release}-ctest.log`、`out/cache-012-debug-build.log`、`out/cache-012-7zip-build.log` 和 `out/cache-012-installer-build.log`，不随安装包发布。

移除 SID 层的后续日志使用 `out/flat-storage-*` 前缀；主程序和安装包保持 v0.1.2，重新构建全部受影响的 7-Zip 模块。

## 交付与范围

主程序资源、CMake 项目版本、安装器文件版本及卸载注册版本统一为 0.1.2；安装器为 `out/installer/PcTool-0.1.2-x64-Setup.exe`。保留上一轮 OCR 内存修复。安装载荷继续排除 docs/tests。

日期边界通过传入固定的本地日期及构造文件时间测试，没有修改系统时钟。未真实等待八天或跨午夜，不将这些模拟用例视为长时间运行实测；没有安装/卸载当前用户的正式版本。本轮没有重复执行截图、GIF、MP4 的完整 GUI 录制/另存流程，也没有在 Explorer 中重新加载壳扩展做交互测试。
