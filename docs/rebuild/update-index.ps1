# Regenerate docs/rebuild/INDEX.md from the stashed source tree and current docs.
# Dirty-side tooling only: reads the reference tree for filenames, never copies source text.
$ErrorActionPreference = 'Stop'
$src = 'E:\_x264_dirty_stash\x264-source'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $PSScriptRoot 'INDEX.md'

$files = Get-ChildItem $src -Recurse -File | Where-Object {
    $_.Extension -in '.c', '.h', '.asm', '.S', '.cl', '.pl', '.sh', '.py', '.txt' -or
    $_.Name -in 'configure', 'Makefile', 'version.sh'
} | ForEach-Object { $_.FullName.Substring($src.Length + 1).Replace('\', '/') } | Sort-Object

$done = 0
$lines = @(
    '# Rebuild specification index',
    '',
    'One document per source file, same tree, `.md` appended. Regenerate with `pwsh docs/rebuild/update-index.ps1`.',
    '',
    '| Source file | Specification | Status | Lines |',
    '| --- | --- | --- | ---: |'
)
foreach ($f in $files) {
    $doc = "$f.md"
    $docPath = Join-Path $PSScriptRoot $doc
    if (Test-Path $docPath) {
        $n = (Get-Content $docPath | Measure-Object -Line).Lines
        $status = if ($n -lt 60) { 'thin' } else { 'done' }
        if ($status -eq 'done') { $done++ }
        $lines += "| ``$f`` | [$doc]($doc) | $status | $n |"
    } else {
        $lines += "| ``$f`` | $doc | pending | 0 |"
    }
}
$lines += ''
$lines += "**Progress: $done / $($files.Count) documents complete (thin docs need expansion).**"
Set-Content -Path $out -Value $lines -Encoding UTF8
Write-Output "INDEX.md: $done / $($files.Count) complete"
