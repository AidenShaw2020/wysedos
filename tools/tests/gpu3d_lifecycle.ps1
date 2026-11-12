param([ValidateSet('-O0', '-O2')][string]$Optimization = '-O2')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/gpu3d-lifecycle'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
try {
    $clang = 'C:/Program Files/LLVM/bin/clang.exe'
    & $clang -std=gnu11 $Optimization -UNDEBUG -Wall -Wextra -Werror tools/tests/gpu3d_lifecycle.c efi/platform/gen8_asm.c -o "$out/gpu3d_lifecycle.exe"
    if ($LASTEXITCODE -ne 0) { throw 'GPU lifecycle compile failed' }
    & "$out/gpu3d_lifecycle.exe"
    if ($LASTEXITCODE -ne 0) { throw 'GPU lifecycle test failed' }
} finally { Pop-Location }
