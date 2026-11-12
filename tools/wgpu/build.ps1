# Builds the DOS test of the render channel: WGPUTEST.EXE (+ DOS4GW.EXE) in
# work/wgpu/dos. Needs Open Watcom (the 2026-08-01 build: $env:WATCOM or
# work/ow2-0801) and clang for the shader table.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/wgpu/dos'
New-Item -ItemType Directory -Force $out | Out-Null
$watcom = if ($env:WATCOM) { $env:WATCOM } else { Join-Path $root 'work/ow2-0801' }
Push-Location $root
try {
    & 'C:/Program Files/LLVM/bin/clang.exe' -std=gnu11 -O2 -Wall -Wextra -Werror -Iefi/platform tools/wgpu/genkernel.c efi/platform/gen8_asm.c -o "$out/genkernel.exe"
    if ($LASTEXITCODE -ne 0) { throw 'genkernel compile failed' }
    & "$out/genkernel.exe" | Set-Content -Encoding Ascii "$out/wgpu_kernel.h"
    $env:WATCOM = $watcom
    $env:INCLUDE = Join-Path $watcom 'h'
    $env:PATH = (Join-Path $watcom 'binnt64') + ';' + (Join-Path $watcom 'binnt') + ';' + $env:PATH
    Push-Location $out
    try {
        & wcl386 '-q' '-bt=dos' '-l=dos4g' '-za99' '-ox' '-wx' '-we' "-i=$root/efi/platform" "-i=$root/pc" "-i=$out" '-fe=WGPUTEST.EXE' "$root/tools/wgpu/wgputest.c"
        if ($LASTEXITCODE -ne 0) { throw 'WGPUTEST compile failed' }
    } finally { Pop-Location }
    Copy-Item (Join-Path $watcom 'binw/dos4gw.exe') (Join-Path $out 'DOS4GW.EXE') -Force
    Get-Item (Join-Path $out 'WGPUTEST.EXE')
} finally {
    Pop-Location
}
