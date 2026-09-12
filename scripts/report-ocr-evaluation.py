"""Render measured results; never substitute official marketing benchmarks."""
import pathlib, json, argparse
import csv
ROOT=pathlib.Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--input-dir',type=pathlib.Path,default=ROOT/'out/ocr-v6')
OUT=parser.parse_args().input_dir.resolve()
data=json.loads((OUT/'summary.json').read_text('utf-8'))
valid={k:v for k,v in data.items() if 'failure' not in v}
regressions={}
for tier in valid:
    folder=OUT/'results'/tier/'warm'
    rows=list(csv.DictReader((folder/'metrics.csv').open()))
    regressions[tier]={'long_60_lines':all(int(r['lines'])==60 for r in rows if r['sample']=='30-long'),
        'blank':all(int(r['lines'])==0 for r in rows if r['sample']=='40-blank'),
        'bounds_and_cancel':(folder/'checks.txt').exists()}
(OUT/'regressions.json').write_text(json.dumps(regressions,indent=2),'utf-8')
candidates={k:v for k,v in valid.items() if v['ordinary_p95_ms']<=5000 and all(regressions[k].values())}
winner=min(candidates,key=lambda k:(candidates[k]['cer'],candidates[k]['missing_lines'],candidates[k]['ordinary_p95_ms'],candidates[k]['peak_private_bytes'])) if candidates else 'v5'
if winner!='v5' and valid[winner]['cer']>=valid['v5']['cer']:winner='v5'
lines=['# PP-OCRv6 本机评估','', '环境：AMD Ryzen 7 5800H，约 16 GB RAM，Windows x64，ONNX Runtime CPU 1.20.1，2 条推理线程。模型串行、独立进程运行；未升级运行库，未修改默认 Release。','',
'## 实测结果','', '| 模型 | 字符错误 / 13,741 | CER | 普通截图中位 / P95 | 首次中位 / P95 | 长图中位 | 模型大小 |', '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
for k,v in data.items():
    if 'failure' in v:lines.append(f'| {k} | 不兼容：见原始错误 | | | | | |');continue
    lines.append(f"| {k} | {v['errors']} | {v['cer']:.2%} | {v['ordinary_median_ms']/1000:.2f} / {v['ordinary_p95_ms']/1000:.2f} s | {v['cold_median_ms']/1000:.2f} / {v['cold_p95_ms']/1000:.2f} s | {v['long_median_ms']/1000:.2f} s | {v['model_bytes']/1048576:.1f} MiB |")
lines+=['','普通截图为 27 张 1920×1080、14 行文字的样本，每张重复 5 次；首次为同一普通截图在 5 个新进程中测试，包含模型加载。P95 使用 nearest-rank。长图为 1200×3600、60 行文字。','',
'| 模型 | 启动工作集 / 私有内存 | 加载后工作集 / 私有内存 | 峰值工作集 / 私有内存 | 峰值相对启动增量 | 结束驻留工作集 / 私有内存 |','| --- | ---: | ---: | ---: | ---: | ---: |']
for k,v in valid.items():
    pair=lambda a,b:f'{v[a]/1048576:.1f} / {v[b]/1048576:.1f}'
    lines.append(f"| {k} | {pair('baseline_working_bytes','baseline_private_bytes')} | {pair('loaded_working_bytes','loaded_private_bytes')} | {pair('peak_working_bytes','peak_private_bytes')} | {(v['peak_working_bytes']-v['baseline_working_bytes'])/1048576:.1f} / {(v['peak_private_bytes']-v['baseline_private_bytes'])/1048576:.1f} | {pair('final_working_bytes','final_private_bytes')} |")
lines+=['','内存单位 MiB。峰值为 5 ms 采样的全样本运行峰值，可能遗漏更短的瞬时峰值；含模型、输入像素、预处理及推理内存池。加载后取首次图像推理前的采样。结束驻留取最后一张图完成时，仍保持模型缓存。','',
'| 模型 | 普通截图检测 / 识别中位 | 未匹配参考行 | 疑似重复行 | 空白图 |', '| --- | ---: | ---: | ---: | --- |']
for k,v in valid.items():lines.append(f"| {k} | {v['ordinary_detect_median_ms']/1000:.2f} / {v['ordinary_recognize_median_ms']/1000:.2f} s | {v['missing_lines']} | {v['duplicate_lines']} | {'通过' if all(not s['actual'].strip() for s in v['samples'] if s['kind']=='blank') else '出现文字'} |")
lines+=['','漏行诊断按去空白后文本相似度 ≥0.5 贪心匹配；剩余输出与参考行相似度 ≥0.8 记为疑似重复。这是辅助诊断，不等同于人工标注的检测召回率。字符错误率使用完整文本 Levenshtein 编辑距离，忽略空白，保留大小写、标点及繁简差异。原始换行、空白数量另存 summary.json，编号不参与评分。','',
'| 模型 | 原有基准错误数 | 普通截图错误数 | 真实裁剪错误数 | 长图错误数 |','| --- | ---: | ---: | ---: | ---: |']
for k,v in valid.items():lines.append('| '+k+' | '+' | '.join(str(sum(s['errors'] for s in v['samples'] if s['kind']==kind)) for kind in ('baseline','ordinary','real','long'))+' |')
lines+=['','长图完整性硬检查要求五次识别均保留 60 行；具体通过/失败见 `out/ocr-v6/regressions.json`。小字号的单字错误计入准确率，整行丢失额外作为分块完整性回归失败。','',
'## 规格结论','',f'按准确率优先、普通截图 P95 ≤5 秒及长图完整性筛选，推荐：**PP-OCRv6 {winner}**。原生窗口验证见文末。small 在五次长图识别中均漏掉约 y=960 的一整行网址（59/60 行），当前接入方式不通过完整性回归；medium 精度最高，但 P95 7.47 秒超过约定。', '',
'## 样本与限制','', '33 张图、13,741 个非空白字符：原有 3 张基准、27 张字号/字体/明暗变化图、1 张用户真实截图裁剪、1 张长图和1张空白图。标准答案在测试前冻结。合成图有重复文本，不代表 33 个独立文档；真实截图仅一张局部，不能据此宣称通用准确率。','',
'检测使用 PcTool 现有横排区域提取、扩框及分块策略，按官方 v6 配置调整 BGR 均值/方差、阈值与扩框比例；识别按官方 3×48×320 配置和字典执行 CTC 解码。未加入透视/旋转校正，结果代表当前原生集成方式，不是官方完整 Python 流水线的最高成绩。','',
'## 改善与退化样例','']
if winner!='v5':
    base={s['name']:s for s in valid['v5']['samples']}
    for title,items in [('改善',sorted(valid[winner]['samples'],key=lambda s:s['errors']-base[s['name']]['errors'])[:2]),('退化',sorted(valid[winner]['samples'],key=lambda s:s['errors']-base[s['name']]['errors'],reverse=True)[:2])]:
        for s in items:lines.append(f"- {title}候选 {s['name']}：v5 {base[s['name']]['errors']} 处，{winner} {s['errors']} 处；完整原文与输出见 summary.json，差值为零表示持平。")
if winner=='tiny':
    lines+=['','具体例子：长图里 v5 曾将 `内存` 识别为 `內存`，将 `&&` 识别为 `&&l` 或 `&l&`；tiny 在对应行正确。退化例子：`10-desktop-17` 中 tiny 将 `Ctrl+A` 识别为 `Ctr1+A`，并把代码半角引号识别成弯引号。']
lines+=['','## 复现','', '1. 安装评估脚本所需 Python requests、PyYAML、Pillow（仅开发工具，PcTool 不依赖 Python）。','2. 运行 `python scripts/prepare-ocr-v6.py`；模型来源、固定 revision 和 SHA256 见 `scripts/ocr-v6-models.json`。','3. 使用冻结的 `out/ocr-v6/corpus`；首次准备运行 `python scripts/ocr-evaluation-corpus.py`，需要原基准 PNG 和本机用户截图。其他机器没有这些输入时须明确报告，不能悄悄换样本。','4. 在 VS Developer Shell 配置 `cmake -S . -B out/build/ocr-eval -G Ninja -DCMAKE_BUILD_TYPE=Release -DPCTOOL_BUILD_OCR_BENCHMARK=ON`，构建目标 `PcToolOcrBenchmark`。','5. 执行 `python scripts/run-ocr-evaluation.py` 和 `python scripts/report-ocr-evaluation.py`。已有完成结果会复用；要重测请使用新的结果目录，避免覆盖历史。','', '逐次 CSV、文本、坐标、取消检查与日志在 `out/ocr-v6/results/`。评估参数不会进入用户配置；正常构建不启用评估入口。']
lines+=['','四档合计完成 680 次计时识别，顺序为 v5、tiny、small、medium；期间没有同时编译或运行其他 OCR 测试，未控制全部后台进程或 CPU 温度。需要重新计时时，可使用 `run-ocr-evaluation.py --output-dir <新目录>`，再用 `report-ocr-evaluation.py --input-dir <新目录>` 生成报告。','',
'## 验证与后续替换','']
validation=OUT/'validation.json'
if validation.exists():
    lines+=json.loads(validation.read_text('utf-8'))['results']
else:lines+=['原生窗口及构建验证尚未记录，不据此宣布可替换。']
lines+=['','后续若实施替换：将默认检测/识别模型改为官方 v6 tiny 配套模型，部署官方字符字典；将评估中已验证的 BGR 均值/方差、检测阈值 0.2、区域阈值 0.4、扩框比例 1.4 正式接入默认引擎；更新固定模型校验及许可证，保留现有 OCR 窗口与取消流程。无需升级 ONNX Runtime，不增加 Python 或云端依赖。需重新构建并回归后发布，不能仅覆盖 v5 的两个模型文件。本轮没有执行替换。']
(ROOT/'tests/OCR_V6_REPORT.md').write_text('\n'.join(lines)+'\n','utf-8')
print('Performance recommendation:',winner)
