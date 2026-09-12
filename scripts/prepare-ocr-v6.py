"""Download official, revision-pinned evaluation models; never modifies the release."""
import hashlib, json, pathlib, requests, yaml
ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/ocr-v6'
OUT.mkdir(parents=True, exist_ok=True)
manifest_path = OUT / 'models.json'
pinned = ROOT / 'scripts/ocr-v6-models.json'
manifest = json.loads(pinned.read_text('utf-8')) if pinned.exists() else (json.loads(manifest_path.read_text('utf-8')) if manifest_path.exists() else {})
for tier in ('tiny', 'small', 'medium'):
    folder = OUT / tier
    folder.mkdir(exist_ok=True)
    for kind in ('det', 'rec'):
        repo = f'PaddlePaddle/PP-OCRv6_{tier}_{kind}_onnx'
        if repo not in manifest:
            response = requests.get(f'https://huggingface.co/api/models/{repo}?blobs=true', timeout=60)
            response.raise_for_status()
            data = response.json()
            manifest[repo] = {'revision': data['sha'], 'files': {}, 'license': 'Apache-2.0'}
            for entry in data['siblings']:
                if entry['rfilename'] in ('inference.onnx', 'inference.yml', 'README.md'):
                    manifest[repo]['files'][entry['rfilename']] = {'sha256': entry.get('lfs', {}).get('sha256')}
            manifest_path.write_text(json.dumps(manifest, indent=2), 'utf-8')
        record = manifest[repo]
        for name, info in record['files'].items():
            target = folder / (f'{kind}.onnx' if name.endswith('.onnx') else f'{kind}-{name}')
            url = f'https://huggingface.co/{repo}/resolve/{record["revision"]}/{name}'
            info['url'] = url
            if not target.exists() or (info['sha256'] and hashlib.sha256(target.read_bytes()).hexdigest() != info['sha256']):
                print('Download', tier, kind, name, flush=True)
                response = requests.get(url, timeout=180)
                response.raise_for_status()
                digest = hashlib.sha256(response.content).hexdigest()
                if info['sha256'] and digest != info['sha256']:
                    raise RuntimeError('Model checksum mismatch')
                target.write_bytes(response.content)
            info.update(sha256=hashlib.sha256(target.read_bytes()).hexdigest(), bytes=target.stat().st_size)
        manifest_path.write_text(json.dumps(manifest, indent=2), 'utf-8')
    det = yaml.safe_load((folder/'det-inference.yml').read_text('utf-8'))
    rec = yaml.safe_load((folder/'rec-inference.yml').read_text('utf-8'))
    assert rec['PostProcess']['name'] == 'CTCLabelDecode'
    assert rec['PreProcess']['transform_ops'][0]['DecodeImage']['img_mode'] == 'BGR'
    assert rec['PreProcess']['transform_ops'][2]['RecResizeImg']['image_shape'] == [3, 48, 320]
    assert det['PostProcess']['thresh'] == .2 and det['PostProcess']['unclip_ratio'] == 1.4
    assert det['PostProcess']['box_thresh'] == (.4 if tier == 'tiny' else .45)
    norm=next(op['NormalizeImage'] for op in det['PreProcess']['transform_ops'] if 'NormalizeImage' in op)
    assert norm['mean'] == [.485,.456,.406] and norm['std'] == [.229,.224,.225]
    (folder/'characters.txt').write_text('\n'.join(rec['PostProcess']['character_dict'])+'\n', 'utf-8')
    print(tier, det['PostProcess'], flush=True)
print('Pinned provenance:', manifest_path)
