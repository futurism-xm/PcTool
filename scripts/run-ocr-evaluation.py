"""Serial, fresh-process model comparison. Source/truth manifest must be frozen."""
import pathlib, subprocess, json, hashlib, shutil, csv, statistics, math, difflib, argparse
ROOT=pathlib.Path(__file__).resolve().parents[1];ASSETS=ROOT/'out/ocr-v6'
parser=argparse.ArgumentParser();parser.add_argument('--output-dir',type=pathlib.Path,default=ASSETS)
OUT=parser.parse_args().output_dir.resolve();OUT.mkdir(parents=True,exist_ok=True);CORPUS=ASSETS/'corpus'
EXE=ROOT/'out/build/ocr-eval/PcToolOcrBenchmark.exe'
run_path=OUT/'run-metadata.json'
fingerprints={'exe_sha256':hashlib.sha256(EXE.read_bytes()).hexdigest(),'runtime_sha256':hashlib.sha256(EXE.with_name('onnxruntime.dll').read_bytes()).hexdigest(),'corpus_manifest_sha256':hashlib.sha256((CORPUS/'manifest.json').read_bytes()).hexdigest()}
if run_path.exists():
    saved=json.loads(run_path.read_text('utf-8'))
    if any(saved.get(k)!=v for k,v in fingerprints.items()):raise SystemExit('Binary/runtime/corpus changed: use a fresh evaluation results directory.')
else:run_path.write_text(json.dumps(fingerprints,indent=2),'utf-8')
pins=json.loads((ROOT/'scripts/ocr-v6-models.json').read_text('utf-8'))
for tier in ('tiny','small','medium'):
    for kind in ('det','rec'):
        expected=pins[f'PaddlePaddle/PP-OCRv6_{tier}_{kind}_onnx']['files']['inference.onnx']['sha256']
        assert hashlib.sha256((ASSETS/tier/(kind+'.onnx')).read_bytes()).hexdigest()==expected
manifest=json.loads((CORPUS/'manifest.json').read_text('utf-8'))
for item in manifest:
    assert hashlib.sha256((CORPUS/(item['name']+'.bgra')).read_bytes()).hexdigest()==item['sha256']
    assert hashlib.sha256((CORPUS/(item['name']+'.truth.txt')).read_bytes()).hexdigest()==item['truth_sha256']
cold=OUT/'cold-corpus';cold.mkdir(exist_ok=True)
shutil.copy2(CORPUS/'10-desktop-00.bgra',cold/'10-desktop-00.bgra')
summary={}
def compact(s):return ''.join(s.split())
def distance(a,b):
    row=list(range(len(b)+1))
    for i,ca in enumerate(a,1):
        previous=row[0];row[0]=i
        for j,cb in enumerate(b,1):
            old=row[j];row[j]=min(row[j]+1,row[j-1]+1,previous+(ca!=cb));previous=old
    return row[-1]
def percentile(values,p=.95):return sorted(values)[max(0,math.ceil(len(values)*p)-1)]
for tier in ('v5','tiny','small','medium'):
    # The production directory now contains v6 tiny. Never label it as a v5 baseline.
    model=ROOT/'out/ocr-downloads' if tier=='v5' else ASSETS/tier
    if tier=='v5':
        for filename,expected in [('det.onnx','4d97c44a20d30a81aad087d6a396b08f786c4635742afc391f6621f5c6ae78ae'),('rec.onnx','5825fc7ebf84ae7a412be049820b4d86d77620f204a041697b0494669b1742c5')]:
            assert hashlib.sha256((model/filename).read_bytes()).hexdigest()==expected,'Original v5 baseline unavailable'
    results=OUT/'results'/tier;results.mkdir(parents=True,exist_ok=True)
    failure=None
    for name,source,repeats in [(f'cold-{i}',cold,1) for i in range(5)]+[('warm',CORPUS,5)]:
        target=results/name
        if (target/'checks.txt').exists():continue
        print(tier,name,flush=True)
        with (results/(name+'.log')).open('w',encoding='utf-8') as log:
            proc=subprocess.run([str(EXE),str(model),tier,str(source),str(target),str(repeats)],stdout=log,stderr=subprocess.STDOUT,timeout=2400)
        if proc.returncode:
            failure=(results/(name+'.log')).read_text('utf-8');break
    if failure:
        summary[tier]={'failure':failure};(OUT/'summary.json').write_text(json.dumps(summary,indent=2,ensure_ascii=False),'utf-8');continue
    rows=list(csv.DictReader((results/'warm/metrics.csv').open()))
    ordinary=[float(r['total_ms']) for r in rows if r['sample'].startswith('10-')]
    coldrows=[next(csv.DictReader((results/f'cold-{i}/metrics.csv').open())) for i in range(5)]
    per_sample=[];errors=chars=missing=duplicates=0
    for item in manifest:
        name=item['name'];truth='\n'.join(item['lines']);actual=(results/'warm'/(name+'-0.txt')).read_text('utf-8')
        e=distance(compact(truth),compact(actual));n=len(compact(truth));errors+=e;chars+=n
        actual_lines=[compact(s) for s in actual.splitlines() if compact(s)];expected=[compact(s) for s in item['lines']]
        # Greedy fuzzy line matching; >= 0.5 similarity counts as detected, not necessarily correct.
        available=set(range(len(actual_lines)));miss=0
        for line in expected:
            candidates=[(difflib.SequenceMatcher(None,line,actual_lines[j],autojunk=False).ratio(),j) for j in available]
            if candidates and max(candidates)[0]>=.5:available.remove(max(candidates)[1])
            else:miss+=1
        dup=sum(any(difflib.SequenceMatcher(None,actual_lines[j],line,autojunk=False).ratio()>=.8 for line in expected) for j in available)
        missing+=miss;duplicates+=dup
        per_sample.append(dict(name=name,kind=item['kind'],errors=e,chars=n,missing=miss,duplicates=dup,whitespace_reference=sum(c.isspace() for c in truth),whitespace_actual=sum(c.isspace() for c in actual),truth=truth,actual=actual))
    first=rows[0];last=rows[-1]
    summary[tier]=dict(errors=errors,chars=chars,cer=errors/chars,missing_lines=missing,duplicate_lines=duplicates,
        ordinary_median_ms=statistics.median(ordinary),ordinary_p95_ms=percentile(ordinary),cold_median_ms=statistics.median(float(r['total_ms']) for r in coldrows),cold_p95_ms=percentile([float(r['total_ms']) for r in coldrows]),
        ordinary_detect_median_ms=statistics.median(float(r['detect_ms']) for r in rows if r['sample'].startswith('10-')),
        ordinary_recognize_median_ms=statistics.median(float(r['recognize_ms']) for r in rows if r['sample'].startswith('10-')),
        long_median_ms=statistics.median(float(r['total_ms']) for r in rows if r['sample']=='30-long'),
        baseline_working_bytes=int(first['baseline_working_bytes']),baseline_private_bytes=int(first['baseline_private_bytes']),
        loaded_working_bytes=int(first['loaded_working_bytes']),loaded_private_bytes=int(first['loaded_private_bytes']),
        peak_working_bytes=max(int(r['peak_working_bytes']) for r in rows),peak_private_bytes=max(int(r['peak_private_bytes']) for r in rows),
        final_working_bytes=int(last['working_bytes']),final_private_bytes=int(last['private_bytes']),model_bytes=sum(p.stat().st_size for p in model.glob('*.onnx')),samples=per_sample)
    (OUT/'summary.json').write_text(json.dumps(summary,indent=2,ensure_ascii=False),'utf-8')
    print(tier,'CER',errors/chars,'P95',percentile(ordinary),flush=True)
