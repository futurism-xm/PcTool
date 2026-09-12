$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cache = Join-Path $projectRoot 'out/ocr-downloads'
$destination = Join-Path $projectRoot 'third_party/ocr'
New-Item -ItemType Directory -Force $cache, "$destination/include", "$destination/models", "$destination/licenses" | Out-Null

function Get-VerifiedFile([string]$url, [string]$path, [string]$sha256) {
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $sha256) {
        Invoke-WebRequest -Uri $url -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $sha256) {
        throw "SHA256 mismatch: $path. Dependency was not installed."
    }
}

Get-VerifiedFile 'https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip' "$cache/onnxruntime.zip" '78D447051E48BD2E1E778BBA378BEC4ECE11191C9E538CF7B2C4A4565E8F5581'
$pins = Get-Content -LiteralPath "$PSScriptRoot/ocr-v6-models.json" -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($kind in @('det', 'rec')) {
    $entry = $pins."PaddlePaddle/PP-OCRv6_tiny_${kind}_onnx".files.'inference.onnx'
    Get-VerifiedFile $entry.url "$cache/v6-tiny-$kind.onnx" $entry.sha256
}
$dictionary = Join-Path $PSScriptRoot 'ocr-v6-tiny-characters.txt'
if ((Get-FileHash -LiteralPath $dictionary -Algorithm SHA256).Hash -ne '2AF150BAB777D86FA1CE821F6A37EABEA90F1BB99AB9C41F031262E892A747BC') {
    throw 'PP-OCRv6 tiny character dictionary checksum mismatch.'
}
Expand-Archive -LiteralPath "$cache/onnxruntime.zip" -DestinationPath $cache -Force
$runtime = Join-Path $cache 'onnxruntime-win-x64-1.20.1'
Copy-Item "$runtime/include/*" "$destination/include" -Force
Copy-Item "$runtime/lib/onnxruntime.dll" $destination -Force
Copy-Item "$runtime/LICENSE" "$destination/licenses/ONNXRuntime-LICENSE.txt" -Force
Copy-Item "$runtime/ThirdPartyNotices.txt" "$destination/licenses/ONNXRuntime-ThirdPartyNotices.txt" -Force
Copy-Item "$cache/v6-tiny-det.onnx" "$destination/models/det.onnx" -Force
Copy-Item "$cache/v6-tiny-rec.onnx" "$destination/models/rec.onnx" -Force
Copy-Item -LiteralPath $dictionary -Destination "$destination/models/characters.txt" -Force
$version = @('Model: PP-OCRv6 tiny', 'Publisher: PaddlePaddle / PaddleOCR', 'Format: ONNX; inference: offline CPU', 'Runtime: ONNX Runtime 1.20.1 (Windows x64)', 'License: Apache-2.0 (models), MIT (runtime)', '')
foreach ($kind in @('det', 'rec')) {
    $model = $pins."PaddlePaddle/PP-OCRv6_tiny_${kind}_onnx"
    $version += @("$kind model: PP-OCRv6_tiny_$kind", "Revision: $($model.revision)", "SHA256: $($model.files.'inference.onnx'.sha256)", "Source: $($model.files.'inference.onnx'.url)", '')
}
$version += @('Character dictionary: extracted from the pinned official recognition inference.yml', 'characters.txt SHA256: 2af150bab777d86fa1ce821f6a37eabea90f1bb99ab9c41f031262e892a747bc')
[IO.File]::WriteAllLines("$destination/models/MODEL_VERSION.txt", $version, [Text.UTF8Encoding]::new($false))
Write-Output 'Offline OCR dependencies installed (Windows x64). CMake packages them beside both executables.'
