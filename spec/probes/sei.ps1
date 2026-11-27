# Extract the x264 SEI "options:" payload from a raw Annex B .264 file.
param([Parameter(Mandatory = $true)][string]$Path)
$b = [System.IO.File]::ReadAllBytes($Path)
$s = [System.Text.Encoding]::ASCII.GetString($b)
$m = [regex]::Match($s, 'x264 - core [^\x00]*')
if ($m.Success) { $m.Value } else { Write-Output '(no x264 SEI string found)' }
