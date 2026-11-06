param([string]$Optimization = '-O2')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/hwvga-transfer'
New-Item -ItemType Directory -Force $out | Out-Null
function Extract-Function([string]$text, [string]$name) {
    $match = [regex]::Match($text, '(?m)^(?:static )?void ' + $name + '\([^\r\n]*\) \{')
    if (!$match.Success) { throw "Cannot locate $name" }
    $start = $match.Index
    $brace = $text.IndexOf('{', $start)
    $depth = 1
    $end = $brace + 1
    while ($depth -gt 0 -and $end -lt $text.Length) {
        if ($text[$end] -eq '{') { $depth++ }
        if ($text[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth -ne 0) { throw "Unbalanced $name" }
    return $text.Substring($start, $end - $start)
}
$hw = Get-Content (Join-Path $root 'efi/platform/hwvga.c') -Raw
$cpu = Get-Content (Join-Path $root 'efi/platform/cpuvmx.c') -Raw
$fetch = Extract-Function $hw 'hwvga_fetch'
$planar = Extract-Function $hw 'hwvga_store_planar'
$chain = Extract-Function $hw 'hwvga_store_chain4'
# Replace only the privileged aperture stores with a register-aware host bus.
# Fetch and hw_update are verbatim, not independently reimplemented algorithms.
$pointer = 'volatile uint8_t *vga\s*=\s*\(volatile uint8_t \*\)0xa0000;'
$planar = [regex]::Replace($planar, $pointer, '')
$chain = [regex]::Replace($chain, $pointer, '')
$old = 'vga[a]=src[a*4+p];'
if (!$planar.Contains($old)) { throw 'Planar store changed; review instrumentation' }
$planar = $planar.Replace($old, 'bus_write(a,src[a*4+p]);')
$old = 'vga[(a & 0xfffc) | a >> 14] = src[a];'
if (!$chain.Contains($old)) { throw 'Chain store changed; review instrumentation' }
$chain = $chain.Replace($old, 'bus_write((a & 0xfffc) | a >> 14,src[a]);')
$update = Extract-Function $cpu 'hw_update'
$proposal = $update.Replace('static void hw_update(', 'static void proposed_hw_update(')
if (!$proposal.Contains('hwvga_fetch(v);')) { throw 'Transition fetch changed' }
$proposal = $proposal.Replace('hwvga_fetch(v);', 'transition_fetch_proposal(v,want);')
$generated = "/* Generated from current production sources; host MMIO seam only. */`n$fetch`n$planar`n$chain`n$update`n$proposal`n"
[IO.File]::WriteAllText((Join-Path $out 'actual_transfer.h'), $generated)
$clang = 'C:/Program Files/LLVM/bin/clang.exe'
Push-Location $root
try {
    & $clang $Optimization -ffunction-sections -DBPP=32 -DWYSE_BUILD=1 -Ipc '-Wl,/opt:ref' tools/tests/hwvga_transfer.c -o work/hwvga-transfer/test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Host compile failed' }
    & ./work/hwvga-transfer/test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Host regression failed' }
} finally { Pop-Location }