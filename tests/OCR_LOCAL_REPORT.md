# Local OCR evaluation — 2026-09-08

Windows x64 CPU, PP-OCRv5 mobile ONNX models, ONNX Runtime 1.20.1. Actual local inference, no image upload. The reproducible fixture is `tests/ocr_comparison.cpp`; run `PcToolCaptureIntegration.exe ocr-compare <absolute-output-prefix>`.

The fixture renders six known lines at 14, 18 and 28 pixels using Microsoft YaHei UI: Chinese UI text, Chinese/English mixed text, monitoring numbers, shell commands, a URL and C++ code. Levenshtein edit distance excludes whitespace but preserves case and punctuation. Each size has 188 reference characters.

| Font pixels | Windows OCR edits | PP-OCRv5 edits | PP-OCRv5 time (Debug) |
| --- | ---: | ---: | ---: |
| 14 | 32 | 2 | 1141 ms |
| 18 | 20 | 1 | 907 ms |
| 28 | 6 | 2 | 953 ms |
| Total | 58 / 564 | 5 / 564 | |

Character error rate on this synthetic corpus decreased from 10.3% to 0.9%; this is not a general accuracy guarantee. Remaining mistakes include a simplified/traditional glyph substitution and an extra character near `&&`. Initial recognition includes lazy model startup. Long images take longer because each detected line is recognized separately.

Passed: blank image, original coordinate bounds, ten repeated blocks crossing tile boundaries without lost/duplicated command lines, and cancellation of active inference within two seconds. Existing OCR layout, result-window interaction (including long results), and screenshot source lifecycle tests also passed with the local engine as default.

Delivery validation: Debug and default Release builds succeeded. Release CTest passed all 7 registered core tests; native `ocr-ui-test` and `ocr-source-test` passed in both configurations. Release comparison reproduced the same 58 versus 5 edits. Isolated deployment probes without the runtime and without models both returned explicit dependency errors; the normal deployment was not modified. The dependency setup script was executed successfully using checksum-verified cached downloads.

Not evaluated as an accuracy corpus: handwriting, rotated/vertical text, perspective photographs, all application fonts, or all languages. Detection currently uses axis-aligned regions without orientation correction. Test outputs include source PNGs and both engines' full text for review; see the build directory's `ocr-comparison*.png` and `ocr-comparison.txt`.
