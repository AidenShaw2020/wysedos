param([ValidateSet('-O0', '-O2')][string]$Optimization = '-O2')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/vga-vbe-snapshot'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
try {
    & 'C:/Program Files/LLVM/bin/clang.exe' -std=gnu11 $Optimization -Wno-unknown-pragmas -ffunction-sections -fdata-sections tools/tests/vga_vbe_snapshot.c '-Wl,/OPT:REF' -o "$out/test$Optimization.exe"
    if ($LASTEXITCODE -ne 0) { throw 'VBE host compile failed' }
    & "$out/test$Optimization.exe"
    if ($LASTEXITCODE -ne 0) { throw 'VBE capacity/scanout regression failed' }
} finally {
    Pop-Location
}
