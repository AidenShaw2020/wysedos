param([ValidateSet('-O0', '-O2')][string]$Optimization = '-O2', [int]$Triangles = 24)
# Gen8 encoder against IGT's kernels, then the Voodoo2 GPU back end's generated
# pixel shaders (on the Gen8 simulator) against the CPU renderer.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/voodoo2'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
try {
    $clang = 'C:/Program Files/LLVM/bin/clang.exe'
    & $clang -std=gnu11 $Optimization -UNDEBUG -Wall -Wextra tools/tests/gen8_asm.c efi/platform/gen8_asm.c -o "$out/gen8_asm.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Gen8 encoder test compile failed' }
    & "$out/gen8_asm.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Gen8 encoder test failed' }
    $coreSources = Get-ChildItem 'pc/voodoo/86box/*.c' | ForEach-Object { $_.FullName }
    & $clang -std=gnu11 $Optimization -UNDEBUG -Ipc -Ipc/voodoo/86box -Wno-deprecated-declarations tools/tests/voodoo2_gpu.c tools/tests/gen8_sim.c pc/voodoo2_gpu.c pc/voodoo2_gpu_test.c pc/voodoo2.c pc/pci.c efi/platform/gen8_asm.c @coreSources -o "$out/voodoo2_gpu.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Voodoo2 GPU test compile failed' }
    & "$out/voodoo2_gpu.exe" $Triangles
    if ($LASTEXITCODE -ne 0) { throw 'Voodoo2 GPU back end differs from the CPU renderer' }
} finally {
    Pop-Location
}
