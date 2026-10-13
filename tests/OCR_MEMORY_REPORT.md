# 截图与 OCR 内存回归报告

日期：2026-09-14；版本：0.1.1；平台：本机 Windows x64。

## 原因与修复

- Alt+W 和截图工具栏文字识别共用本地 OCR。原实现的静态 `Engine` 长期保留 ONNX 模型会话及推理内存池，大图识别后内存池不会随请求释放。
- 改为在串行请求锁内创建请求级 Engine，请求返回或异常时销毁；等待锁期间已取消的请求不再加载模型。保留原模型、推理参数及正在推理时的取消机制。
- OCR 结果窗口关闭后，其 C++ 对象可能保留到下次工具调用。现在窗口销毁时释放预览位图、文档和源图引用；异步工作完成时也释放捕获的源图引用。工作线程仍持有自己需要的共享状态，关闭窗口不会提前释放正在使用的图像。
- 普通截图和翻译选区原有的关闭资源释放逻辑正常，本轮没有修改这两个采集组件的实现。
- 没有调用工作集清理 API；下降来自资源生命周期修复。ONNX DLL 仍按原设计保留加载。

## 测量方式与结果

使用独立 `PcToolCaptureIntegration` 进程、同一份合成中英文图片，交替识别 1280×800 和 2560×1440 图像。记录 Windows `PrivateUsage`、工作集和 GDI 对象数，不使用用户选区、不发送翻译请求。下表是私有内存，不是整个 PcTool 主进程的任务管理器截图。

| 场景 | 修复前 Release | 修复后 Release |
| --- | ---: | ---: |
| 第一次 OCR 结束 | 303.51 MiB | 14.03 MiB |
| 大图及后续请求结束 | 最高 568.85 MiB | 13.40～15.77 MiB |
| OCR 结果窗口关闭、对象仍保留 | 573.86 MiB | 16.24 MiB |
| 销毁上述对象后 | 569.90 MiB | 16.20 MiB |
| 识别中取消、对象仍保留 | 583.99 MiB | 17.17 MiB |

Release 四次识别及测量等待的总耗时：修复前 610～719 ms，修复后 718～828 ms（均包含 100 ms 的测量前等待）。请求级模型加载有一定成本；这是本机样本，不能作为所有图片的性能保证。推理执行期间仍会出现临时内存峰值，本次验证的是结束后是否释放。

Debug 相同内存回归通过，四次 OCR 结束时私有内存 13.51～16.46 MiB。

## 回归覆盖

- 普通截图真实选区窗口连续启动、取消 10 次：Release 第 5/10 次均为 2.70 MiB，GDI 均为 13，没有累积增长。
- Alt+W 使用的 TranslationCapture 选区组件连续启动、取消 10 次，结束时 2.79 MiB。
- 四次真实本地 OCR 推理、结果包含预期数字、不同图片大小切换、关闭后仍保留对象、识别中取消、并发请求中的取消均通过。
- 测试断言覆盖重复 OCR 内存增长、截图 GDI/内存增长，以及关闭和取消后保留对象与销毁对象的内存差异。
- Debug、Release 相关目标构建通过；两种配置的 `capture_core`、`ocr_layout`、`translation_core`、`translation_sources_core` 均通过（各 4/4）。

可重复执行：

```powershell
cmake --build cmake-build-release --target PcTool PcToolCaptureIntegration PcToolTranslationTests
./cmake-build-release/PcToolCaptureIntegration.exe memory-test enforce
ctest --test-dir cmake-build-release -R '^(capture_core|ocr_layout|translation_core|translation_sources_core)$' --output-on-failure
```

启用 `PCTOOL_ENABLE_DESKTOP_TESTS` 后，内存测试也注册为 CTest `capture_memory`；需要交互桌面及已部署的 OCR 模型和运行库。

开发目录中的证据日志（不随安装包发布）：

- `out/memory-before.log`：修复前基线。
- `out/memory-final.log`、`out/memory-debug.log`：修复后的 Release/Debug 内存测试。
- `out/memory-release-ctest.log`、`out/memory-debug-ctest.log`：相关核心回归结果。
- `out/memory-debug-build.log`、`out/memory-installer-build.log`：构建与打包结果。

## 交付与未实测范围

重新生成 `out/installer/PcTool-0.1.1-x64-Setup.exe`。文件清单中的 PcTool.exe SHA256 与本轮 Release 二进制一致，实际安装载荷不含 `docs`、`tests` 目录。未执行安装升级或替换当前运行的 `D:\install\PcTool\PcTool.exe`。

本轮没有以真实键盘完成主程序 Alt+W → 拖选 → 联网翻译的端到端测试；已验证该入口使用的选区组件和共用 OCR 推理路径。未覆盖所有截图编辑、长截图、录屏路径的内存，也未执行真实多显示器/混合 DPI、4K 极长文本压力测试。不能将本报告的独立进程数值当成完整主程序的固定内存上限。
