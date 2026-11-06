param([ValidateSet('-O0','-O2')][string]$Optimization = '-O2')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$clang = 'C:/Program Files/LLVM/bin/clang.exe'
$out = Join-Path $root 'work/i386-bios-rowfill'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
try {
    Write-Output "Compiler: $clang $Optimization; pc/i386.c is linked unmodified"
    Get-FileHash pc/i386.c,disks/386/vgabios.bin,tools/tests/i386_bios_rowfill.c -Algorithm SHA256 | Format-List
    & $clang $Optimization -std=gnu11 -DWYSE_BUILD=1 -DCIRCLE_PC_NO_UART=1 -Itools/tests/i386_host_include -Iefi/platform -Ipc pc/i386.c tools/tests/i386_bios_rowfill.c -o "work/i386-bios-rowfill/test$Optimization.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Actual interpreter host compile failed' }
    & "./work/i386-bios-rowfill/test$Optimization.exe" disks/386/vgabios.bin
    if ($LASTEXITCODE -ne 0) { throw 'Actual interpreter regression failed' }
    foreach ($control in @('--drop-after-first-row','--corrupt-handoff')) {
        & "./work/i386-bios-rowfill/test$Optimization.exe" disks/386/vgabios.bin $control
        if ($LASTEXITCODE -ne 1) { throw "Negative control was not rejected: $control" }
    }
    Write-Output 'PASS: both injected negative controls rejected with exit 1'
} finally { Pop-Location }