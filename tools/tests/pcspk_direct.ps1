param([string]$Optimization = '-O2')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/pcspk-direct'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
try {
    & 'C:/Program Files/LLVM/bin/clang.exe' $Optimization -UNDEBUG -DWYSE_BUILD -DSOUND_FREQUENCY=48000 -Iefi/platform -Iefi/include tools/tests/pcspk_direct.c -o work/pcspk-direct/test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Host compile failed' }
    & ./work/pcspk-direct/test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Direct PC speaker regression failed' }
} finally { Pop-Location }