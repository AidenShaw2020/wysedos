param([ValidateSet('-O0', '-O2')][string]$Optimization = '-O2')
# The Intel GPU render channel (pc/wgpu.c) against a fake engine.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/wgpu'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
try {
    $clang = 'C:/Program Files/LLVM/bin/clang.exe'
    & $clang -std=gnu11 $Optimization -UNDEBUG -Wall -Wextra -Werror -Ipc -c tools/tests/wgpu.c -o "$out/wgpu_test.o"
    if ($LASTEXITCODE -ne 0) { throw 'WGPU test compile failed' }
    & $clang -std=gnu11 $Optimization -UNDEBUG -Wall -Wextra -Werror -Ipc -c pc/wgpu.c -o "$out/wgpu.o"
    if ($LASTEXITCODE -ne 0) { throw 'WGPU compile failed' }
    & $clang -std=gnu11 $Optimization -Ipc -Wno-everything -c pc/pci.c -o "$out/pci.o"
    if ($LASTEXITCODE -ne 0) { throw 'PCI compile failed' }
    & $clang "$out/wgpu_test.o" "$out/wgpu.o" "$out/pci.o" -o "$out/wgpu.exe"
    if ($LASTEXITCODE -ne 0) { throw 'WGPU test link failed' }
    & "$out/wgpu.exe"
    if ($LASTEXITCODE -ne 0) { throw 'WGPU render channel test failed' }
} finally {
    Pop-Location
}
