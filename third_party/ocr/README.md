# Offline OCR dependencies

Run `powershell -ExecutionPolicy Bypass -File scripts/setup-ocr.ps1` from the project root before the first build. Downloads occur only during this explicit setup step. The script pins and verifies SHA256 for every binary archive/model and reuses verified cached downloads.

- Inference: Microsoft ONNX Runtime CPU 1.20.1, Windows x64, official release archive (MIT; bundled third-party notices).
- Models: official PaddlePaddle PP-OCRv6 tiny detection and recognition ONNX models (Apache-2.0).
- Fixed official repository revisions and SHA256 checksums are in `scripts/ocr-v6-models.json`; `scripts/setup-ocr.ps1` selects the tiny pair. The dictionary is extracted from that exact recognition model's official `inference.yml` and preserved in `scripts/ocr-v6-tiny-characters.txt` with a pinned checksum.
- PcTool implements native C++ preprocessing, horizontal DB region extraction and CTC decoding. It does not embed the RapidOCR Python package or require Python, OpenCV, a GPU or a cloud service.

CMake deploys `onnxruntime.dll`, `ocr_models/det.onnx`, `ocr_models/rec.onnx`, `ocr_models/characters.txt`, `ocr_models/MODEL_VERSION.txt`, and `ocr_licenses/` beside the executable. Ship all of these together. Runtime and models add about 17 MiB (uncompressed), plus licenses. The DLL is loaded from the executable directory only, on first recognition. Missing dependencies produce an OCR error instead of silently using the previous engine.

To check the model family, exact published revisions, file hashes and sources, read the executable-adjacent `ocr_models/MODEL_VERSION.txt`. The OCR result window title remains `文字识别 · PcTool`.

This integration is intended for horizontal desktop text. Rotated/vertical text and photographed documents with perspective are not corrected. Tiny, blurred or unusual glyphs can still be misrecognized. Windows OCR remains available internally for compatibility and comparison tests, not as the application's default engine.
