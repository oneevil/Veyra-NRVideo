# SPDX-License-Identifier: GPL-3.0-only
# Checks the voice engine (src/tts) on Windows without building Veyra: downloads the models
# and the ONNX Runtime the app would download, compiles the two command-line tools with
# MSVC and speaks a line to a WAV file.
#
# Run from a "Developer PowerShell for VS 2022" (cl.exe on PATH), in the repository root:
#     powershell -ExecutionPolicy Bypass -File tools\voice\check-voice.ps1
# Everything goes to .\voice-check (about 600 MB; delete it afterwards).
param([string]$Work = "voice-check", [string]$Text = "Отойди от машины! Полиция уже едет, у нас 2 минуты.")
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path

function Fetch($url, $target) {
    if (Test-Path $target) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Write-Host "download $url"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$target.part"
    Move-Item "$target.part" $target
}

# ONNX Runtime: the same sherpa-onnx release that live speech recognition downloads.
$runtime = "$Work\runtime"
if (-not (Test-Path "$runtime\onnxruntime.dll")) {
    $archive = "$Work\sherpa.tar.bz2"
    Fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.2/sherpa-onnx-v2.0.2-win-x64-shared-MD-MinSizeRel-no-tts-lib.tar.bz2" $archive
    New-Item -ItemType Directory -Force -Path $runtime | Out-Null
    tar -xf $archive -C $runtime --strip-components 2 "sherpa-onnx-v2.0.2-win-x64-shared-MD-MinSizeRel-no-tts-lib/lib/onnxruntime.dll" "sherpa-onnx-v2.0.2-win-x64-shared-MD-MinSizeRel-no-tts-lib/lib/onnxruntime_providers_shared.dll"
}

# TeraTTSv2 at the commit Veyra pins (src/ui/VoiceModels.cpp).
$tera = "https://huggingface.co/TeraSpace/TeraTTSv2/resolve/4c3bf2eb99c148cb657af10bd3bf717befb714c8"
$files = @("unicode_indexer.json", "models/text_encoder.onnx", "models/duration_predictor.onnx",
           "models/sampler_distilled_cfg3_8step.onnx", "models/vocoder.onnx",
           "ruaccent/dictionary/accents.json.gz", "ruaccent/dictionary/yo_words.json.gz",
           "ruaccent/nn/nn_accent/model.onnx", "ruaccent/nn/nn_accent/vocab.txt", "ruaccent/nn/nn_accent/config.json")
foreach ($v in "ru_m5", "ru_f1", "ru_m1", "ru_f2", "eng_m4", "eng_f5", "eng_m3", "eng_f3") {
    $files += "styles/$v/style_ttl.npy", "styles/$v/style_dp.npy"
}
foreach ($f in $files) { Fetch "$tera/$f" "$Work\tera\$($f -replace '/', '\')" }

# The listening models (voices by the game's sound).
Fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx" "$Work\listen\silero_vad.onnx"
Fetch "https://huggingface.co/JaesungHuh/voice-gender-classifier/resolve/db1222153bd60337e900be22add7af180452adc0/model.safetensors" "$Work\listen\gender\model.safetensors"
Fetch "https://huggingface.co/JaesungHuh/voice-gender-classifier/resolve/db1222153bd60337e900be22add7af180452adc0/example1.wav" "$Work\listen\example1.wav"
Fetch "https://huggingface.co/JaesungHuh/voice-gender-classifier/resolve/db1222153bd60337e900be22add7af180452adc0/example2.wav" "$Work\listen\example2.wav"
Fetch "https://huggingface.co/Wespeaker/wespeaker-voxceleb-resnet221-LM/resolve/26732f43821f238bfb258c5ef54924a798e62118/voxceleb_resnet221_LM.onnx" "$Work\listen\speaker.onnx"

# Build the tools (no Qt, no CMake: plain cl.exe), with the objects in the work folder.
$repo = (Get-Location).Path
$flags = "/nologo", "/std:c++20", "/EHsc", "/O2", "/utf-8", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/DUNICODE", "/D_UNICODE"
$tools = @{
    "veyra_tts_cli.exe" = "tests\tts\TeraVoiceCli.cpp", "src\tts\Gzip.cpp", "src\tts\Numbers.cpp", "src\tts\Ort.cpp", "src\tts\RuStress.cpp", "src\tts\Tokenizers.cpp", "src\tts\TeraVoice.cpp", "src\tts\Narrators.cpp";
    "veyra_voiceid_cli.exe" = "tests\tts\VoiceIdCli.cpp", "src\tts\VoiceId.cpp", "src\tts\Features.cpp", "src\tts\Ort.cpp", "src\tts\Narrators.cpp";
    "veyra_tts_subtitle_tests.exe" = "tests\tts\SubtitleTextTests.cpp", "src\tts\SubtitleText.cpp"
}
Push-Location $Work
try {
    foreach ($tool in $tools.Keys) {
        $sources = $tools[$tool] | ForEach-Object { Join-Path $repo $_ }
        cl @flags "/Fe$tool" @sources
        if ($LASTEXITCODE) { throw "$tool did not compile" }
    }
} finally {
    Pop-Location
}

Write-Host "`n== subtitle cleanup tests"
& "$Work\veyra_tts_subtitle_tests.exe"

Write-Host "`n== speech"
& "$Work\veyra_tts_cli.exe" "$Work\tera" "$runtime\onnxruntime.dll" ru_m5 $Text "$Work\voice.wav"
Write-Host "listen to $Work\voice.wav"

Write-Host "`n== voices (example1 is a woman, example2 a man)"
$clips = @()
foreach ($name in "example1", "example2") {
    # 16 kHz mono 16-bit WAV -> raw float32, two halves each
    $bytes = [IO.File]::ReadAllBytes("$Work\listen\$name.wav")
    $count = ($bytes.Length - 44) / 2
    $half = [int]($count / 2)
    foreach ($part in 0, 1) {
        $out = New-Object byte[] ($half * 4)
        for ($i = 0; $i -lt $half; $i++) {
            $s = [BitConverter]::ToInt16($bytes, 44 + 2 * ($part * $half + $i)) / 32768.0
            [BitConverter]::GetBytes([single]$s).CopyTo($out, 4 * $i)
        }
        $clip = "$Work\listen\$name-$part.f32"
        [IO.File]::WriteAllBytes($clip, $out)
        $clips += $clip
    }
}
& "$Work\veyra_voiceid_cli.exe" "$runtime\onnxruntime.dll" "$Work\listen\silero_vad.onnx" "$Work\listen\gender" "$Work\listen\speaker.onnx" $clips[0] $clips[2] $clips[1] $clips[3]
