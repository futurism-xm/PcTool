"""Fast evaluation harness failures; no downloaded model is required."""
import pathlib, tempfile, subprocess, struct, sys, shutil
exe=pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='pctool-ocr-eval-') as temporary:
    root=pathlib.Path(temporary);corpus=root/'corpus';corpus.mkdir()
    def run(tier,expected):
        p=subprocess.run([str(exe),str(root/'absent-models'),tier,str(corpus),str(root/'output'),'1'],capture_output=True,timeout=15)
        error=p.stderr.decode('utf-8',errors='replace')
        assert p.returncode!=0 and expected in error,(p.returncode,error)
    run('unknown','Unknown model tier')
    run('tiny','Empty corpus')
    (corpus/'fixture.bgra').write_bytes(struct.pack('<ii',128,128))
    run('tiny','Truncated fixture')
    (corpus/'fixture.bgra').write_bytes(struct.pack('<ii',128,128)+b'\xff'*128*128*4)
    run('tiny','det.onnx')
    isolated=root/'without-runtime';isolated.mkdir();shutil.copy2(exe,isolated/exe.name)
    exe=isolated/exe.name
    run('tiny','Missing or incompatible onnxruntime.dll')
print('Evaluation input validation and missing-model diagnostics PASS')
