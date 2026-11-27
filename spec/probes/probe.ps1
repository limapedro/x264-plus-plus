# Black-box probe helper. Records stdout, stderr and exit code of one x264 invocation.
# Usage: .\probe.ps1 <name> <x264 args...>
# Outputs are written to $env:X264_PROBE_DIR (default: opencode temp dir).
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Cmd,
    [string]$StdinFile
)

$dir = $env:X264_PROBE_DIR
if (-not $dir) { $dir = 'C:\Users\multi\AppData\Local\Temp\opencode\x264-probes' }
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
$exe = 'E:\__PROJECTS__\x264++\oracle\x264.exe'

$XArgs = @($Cmd -split '\s+' | Where-Object { $_ -ne '' })

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
if (-not [string]::IsNullOrEmpty($StdinFile)) { $psi.RedirectStandardInput = $true }
function Quote([string]$s) {
    $s = $s -replace '(\\*)"', '$1$1\"'
    $s = $s -replace '(\\*)$', '$1$1'
    return '"' + $s + '"'
}
$psi.Arguments = (($XArgs | ForEach-Object { Quote $_ }) -join ' ')
$p = New-Object System.Diagnostics.Process
$p.StartInfo = $psi
[void]$p.Start()
if (-not [string]::IsNullOrEmpty($StdinFile)) {
    $bytes = [System.IO.File]::ReadAllBytes($StdinFile)
    $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $p.StandardInput.BaseStream.Close()
}
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()
$out = $outTask.GetAwaiter().GetResult()
$err = $errTask.GetAwaiter().GetResult()
$p.WaitForExit()
$code = $p.ExitCode
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText("$dir\$Name.out", $out, $utf8)
[System.IO.File]::WriteAllText("$dir\$Name.err", $err, $utf8)
[System.IO.File]::WriteAllText("$dir\$Name.code", "$code", $utf8)
Write-Output "=== $Name : exit=$code ==="
Write-Output '--- stdout ---'
($out -split "`r?`n") | Select-Object -First 40
Write-Output '--- stderr ---'
($err -split "`r?`n") | Select-Object -First 40
