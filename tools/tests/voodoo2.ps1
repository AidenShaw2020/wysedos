param([ValidateSet('-O0', '-O2')][string]$Optimization = '-O2', [switch]$Sanitize)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/voodoo2'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
$oldPath = $env:PATH
try {
    $extra = @()
    $suffix = ''
    if ($Sanitize) {
        $extra = @('-fsanitize=address', '-g', '-fno-omit-frame-pointer')
        $suffix = '-asan'
        $resource = (& 'C:/Program Files/LLVM/bin/clang.exe' -print-resource-dir).Trim()
        if ($LASTEXITCODE -ne 0) { throw 'Clang resource lookup failed' }
        $env:PATH = (Join-Path $resource 'lib/windows') + ';' + $env:PATH
    }
    $coreSources = Get-ChildItem 'pc/voodoo/86box/*.c' | ForEach-Object { $_.FullName }
    & 'C:/Program Files/LLVM/bin/clang.exe' -std=gnu11 $Optimization @extra -UNDEBUG -Ipc -Ipc/voodoo/86box -Wno-deprecated-declarations tools/tests/voodoo2.c pc/voodoo2.c pc/voodoo2_gpu.c efi/platform/gen8_asm.c pc/pci.c efi/platform/intel_gpu.c @coreSources -o "$out/test$Optimization$suffix.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Voodoo2 host compile failed' }
    & "$out/test$Optimization$suffix.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Voodoo2 regression failed' }
} finally {
    $env:PATH = $oldPath
    Pop-Location
}
