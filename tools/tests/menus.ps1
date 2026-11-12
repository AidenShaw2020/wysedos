param([ValidateSet('-O0', '-O2')][string]$Optimization = '-O2', [switch]$Sanitize)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/menus'
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
    & 'C:/Program Files/LLVM/bin/clang.exe' -std=gnu11 $Optimization @extra -UNDEBUG -DCIRCLE_BUILD=1 -Itools/tests/menus_host_include -Ipc -Iefi/platform -Wno-deprecated-declarations tools/tests/menus.c pc/settingsui.c pc/diskui.c pc/config_save.c pc/osd.c pc/font8x16.c -o "$out/test$Optimization$suffix.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Menu host compile failed' }
    & "$out/test$Optimization$suffix.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Menu regression failed' }
} finally {
    $env:PATH = $oldPath
    Pop-Location
}
