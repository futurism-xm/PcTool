"""Freeze sources and truth before any model comparison. Pillow is tooling only."""
import pathlib, json, hashlib, struct
from PIL import Image, ImageDraw, ImageFont
ROOT=pathlib.Path(__file__).resolve().parents[1]
OUT=ROOT/'out/ocr-v6/corpus'
OUT.mkdir(parents=True,exist_ok=True)
if (OUT/'manifest.json').exists():
    raise SystemExit('Corpus already frozen; use existing manifest instead of regenerating.')
BASE=['文字识别：已完成窗口大小调整与功能栏修复。','中文与 English 混排，支持数字 0123456789。','CPU频率 3.39 GHz 内存 53% 上传 0.1 K/s','docker compose pull && docker compose up -d','https://example.com/api/v1?limit=100&offset=20','const std::wstring text = L"你好，世界！";']
EXTRA=[
 ['剪切板历史仅保留最近的100条文本，重复内容自动置顶。','拖动进度条可定位视频，暂停后继续播放。','长截图支持滚动查看，导出完整图片及全部标注。','录制结束后打开预览，点击完成复制视频文件。','字体大小：14、18、24；显示比例：100%、150%、200%。','Total: 1,024.50  Discount: 15%  Count: 007','if (count <= 100 && ready) { return result; }','路径 C:/Users/Public/Documents/report.txt'],
 ['系统声音默认开启，麦克风默认关闭。','鼠标悬停查看完整内容，按 Ctrl+A 全选文本。','网络下载速度 125.6 MB/s，延迟 28 ms。','请检查标点：逗号，句号。冒号：括号（测试）。','git status --short && git diff --stat','std::vector<int> values = {0, 1, 2, 3};','Error 404: Resource not found /api/v2/items','Version 3.7.0 | 2026-09-08 15:30:42'],
 ['屏幕录制预览窗口支持缩放、移动和最小化。','原图左侧显示识别框，右侧显示编号段落。','关闭软件后保留已完成的视频，取消录制不保存。','检索关键字：窗口、文字、图像、模型和识别结果。','Windows 10 / Windows 11 / AMD Ryzen 7 5800H','SELECT name, value FROM items WHERE id >= 10;','python tool.py --input image.png --output result.txt','0123456789 ABCDEFG abcdefg O0 Il1 rn m'],
]
manifest=[]
def save(name,image,lines,kind,source):
    image=image.convert('RGBA');image.save(OUT/(name+'.png'))
    raw=struct.pack('<ii',*image.size)+image.tobytes('raw','BGRA')
    (OUT/(name+'.bgra')).write_bytes(raw)
    truth='\n'.join(lines)
    (OUT/(name+'.truth.txt')).write_text(truth,'utf-8')
    manifest.append(dict(name=name,width=image.width,height=image.height,kind=kind,source=source,lines=lines,sha256=hashlib.sha256(raw).hexdigest(),truth_sha256=hashlib.sha256((OUT/(name+'.truth.txt')).read_bytes()).hexdigest()))
for size in (14,18,28):
    source=ROOT/f'cmake-build-release/ocr-comparison-{size}.png'
    save(f'00-original-{size}',Image.open(source),BASE,'baseline','Existing frozen GDI comparison fixture')
for i in range(27):
    size=(14,18,24)[i%3];dark=(i//3)%2;lines=BASE+EXTRA[i//9]
    image=Image.new('RGB',(1920,1080),(28,30,34) if dark else (255,255,255))
    draw=ImageDraw.Draw(image);font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc' if i%2==0 else 'C:/Windows/Fonts/simsun.ttc',size)
    for row,line in enumerate(lines):draw.text((25+(i%3)*15,25+row*58),line,font=font,fill=(230,230,230) if dark else (25,25,25))
    save(f'10-desktop-{i:02}',image,lines,'ordinary','Pillow rendered known text; font/contrast/size variants, not independent documents')
real=pathlib.Path('C:/Users/Administrator/AppData/Local/Temp/codex-clipboard-789b2f24-49cd-4d7a-8ac3-41ec0d8e1e95.png')
save('20-real-crop',Image.open(real).crop((20,328,780,426)),['已修复调整窗口大小时的边框、按钮和分隔线残影。','已通过三档 DPI 连续缩放测试、Debug / Release 构建及相关测试，新版已启动。'],'real','User-provided screenshot crop; manually transcribed; icons and unrelated areas excluded')
image=Image.new('RGB',(1200,3600),'white');tile=Image.open(ROOT/'cmake-build-release/ocr-comparison-28.png')
for i in range(10):image.paste(tile,(0,i*360))
save('30-long',image,BASE*10,'long','10 original blocks; checks tile ownership and duplicate lines')
save('40-blank',Image.new('RGB',(1920,1080),'white'),[],'blank','Empty image')
(OUT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),'utf-8')
print('Frozen',len(manifest),'images;',sum(sum(not c.isspace() for c in '\n'.join(s['lines'])) for s in manifest),'non-whitespace characters')
