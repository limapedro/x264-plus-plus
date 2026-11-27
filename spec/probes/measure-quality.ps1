# Black-box quality measurement probe.
# Generates deterministic YUV420p sources with ffmpeg, encodes them with the
# reference oracle, then measures decoded-vs-input PSNR/SSIM and file size.
# All media is written to $WorkDir (default: a temp directory outside the repo).
# No reference encoder source is read or used.

param(
    [string]$WorkDir = 'C:\Users\multi\AppData\Local\Temp\opencode\x264pp-quality',
    [string]$X264 = 'E:\__PROJECTS__\x264++\oracle\x264.exe',
    [string]$FFmpeg = 'C:\ffmpeg\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe'
)

# Native tools write progress to stderr; do not turn that into a terminating error.
$ErrorActionPreference = 'Continue'
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

function Get-Lavfi([string]$kind, [int]$w, [int]$h, [int]$fps) {
    switch ($kind) {
        'synthetic'  { return "testsrc2=size=${w}x${h}:rate=${fps}" }
        'natural'    { return "mandelbrot=size=${w}x${h}:rate=${fps}:maxiter=256:end_pts=10" }
        'noisy'      { return "testsrc2=size=${w}x${h}:rate=${fps},noise=alls=25:allf=t+u" }
        'static'     { return "color=c=0x303030:s=${w}x${h}:r=${fps},drawbox=x=0:y=0:w=iw/2:h=ih/2:color=0x808080:t=fill" }
        default      { throw "unknown content kind: $kind" }
    }
}

function Measure-One([string]$caseId, [int]$w, [int]$h, [int]$fps, [int]$frames,
                     [string]$kind, [string]$preset, [int]$crf, [int]$threads) {
    $base = Join-Path $WorkDir $caseId
    $yuv  = "$base.yuv"
    $enc  = "$base.264"
    $lavfi = Get-Lavfi $kind $w $h $fps
    if (-not (Test-Path $yuv)) {
        & $FFmpeg -hide_banner -loglevel error -y -f lavfi -i $lavfi `
            -frames:v $frames -pix_fmt yuv420p -f rawvideo $yuv 2>&1 | Out-Null
    }
    $null = & $X264 --input-res "${w}x${h}" --fps $fps --threads $threads `
        --preset $preset --crf $crf -o $enc $yuv 2>&1
    $bytes = (Get-Item $enc).Length

    $psnrOut = & $FFmpeg -hide_banner -loglevel info -y -i $enc `
        -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $yuv `
        -lavfi '[0:v][1:v]psnr' -f null - 2>&1 | Out-String
    $ssimOut = & $FFmpeg -hide_banner -loglevel info -y -i $enc `
        -f rawvideo -pixel_format yuv420p -video_size "${w}x${h}" -framerate $fps -i $yuv `
        -lavfi '[0:v][1:v]ssim' -f null - 2>&1 | Out-String

    $psnr = [regex]::Match($psnrOut, 'average:([0-9.]+|inf)').Groups[1].Value
    $ssim = [regex]::Match($ssimOut, 'All:([0-9.]+)').Groups[1].Value
    if (-not $psnr -or -not $ssim) {
        Write-Warning "parse failure for $caseId`nPSNR:$psnrOut`nSSIM:$ssimOut"
    }
    [pscustomobject]@{
        case = $caseId; w = $w; h = $h; frames = $frames; fps = $fps
        content = $kind; preset = $preset; crf = $crf; threads = $threads
        bytes = $bytes; psnr_db = $psnr; ssim_all = $ssim; yuv = $yuv; enc = $enc
    }
}

$cases = @(
    # id, w, h, fps, frames, content, preset, crf, threads
    ('c01', 320, 180, 30, 30, 'synthetic', 'medium',   18, 1),
    ('c02', 320, 180, 30, 30, 'synthetic', 'medium',   23, 1),
    ('c03', 320, 180, 30, 30, 'synthetic', 'medium',   28, 1),
    ('c04', 320, 180, 30, 30, 'synthetic', 'medium',   23, 4),
    ('c05', 640, 360, 30, 30, 'natural',   'medium',   23, 1),
    ('c06', 640, 360, 30, 30, 'natural',   'medium',   28, 1),
    ('c07', 640, 360, 30, 30, 'noisy',     'veryfast', 28, 1),
    ('c08', 320, 180, 30, 30, 'static',    'medium',   23, 1),
    ('c09', 1280, 720, 30, 10, 'synthetic','fast',     28, 4),
    ('c10', 640, 360, 30, 30, 'natural',   'medium',   23, 4),
    ('c11', 640, 360, 30, 30, 'noisy',     'veryfast', 28, 4),
    ('c12', 320, 180, 30, 30, 'synthetic', 'slow',     23, 1)
)

$rows = foreach ($c in $cases) {
    Measure-One -caseId $c[0] -w $c[1] -h $c[2] -fps $c[3] -frames $c[4] `
        -kind $c[5] -preset $c[6] -crf $c[7] -threads $c[8]
}
$rows | Export-Csv -Path (Join-Path $WorkDir 'quality.csv') -NoTypeInformation -Encoding utf8
$rows | Format-Table case, content, preset, crf, threads, bytes, psnr_db, ssim_all -AutoSize | Out-String -Width 200
