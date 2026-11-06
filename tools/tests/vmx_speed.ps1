param([string]$Optimization = '-O2')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root 'work/vmx-speed'
New-Item -ItemType Directory -Force $out | Out-Null
$source = Get-Content (Join-Path $root 'efi/platform/cpuvmx.c') -Raw
$match = [regex]::Match($source, '(?ms)^static bool cycle_budget_ready\([^)]*\) \{.*?^\}')
if (!$match.Success) { throw 'Cannot locate actual cycle budget helper' }
$pmu = [regex]::Match($source, '(?ms)^static bool configure_guest_cycles\([^)]*\) \{.*?^\}')
if (!$pmu.Success) { throw 'Cannot locate actual guest-only PMU configuration' }
[IO.File]::WriteAllText((Join-Path $out 'actual_budget.h'), $pmu.Value + "`n" + $match.Value)
Push-Location $root
try {
    & 'C:/Program Files/LLVM/bin/clang.exe' $Optimization tools/tests/vmx_speed.c -o work/vmx-speed/test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Host compile failed' }
    & ./work/vmx-speed/test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Cycle budget regression failed' }
    & ./work/vmx-speed/test.exe --no-cycle-limit
    if ($LASTEXITCODE -ne 1) { throw 'Negative control did not reproduce short-loop overflow' }
    'PASS expected negative control: missing cycle limiter fails the same timing criterion'
    & ./work/vmx-speed/test.exe --host-cycle-accounting
    if ($LASTEXITCODE -ne 1) { throw 'Negative control did not charge host work incorrectly' }
    'PASS expected negative control: host VM-exit work must not be charged to the guest'
    & ./work/vmx-speed/test.exe --host-instruction-accounting
    if ($LASTEXITCODE -ne 1) { throw 'Negative control did not charge host instructions incorrectly' }
    'PASS expected negative control: host instructions must not be charged to the guest'
} finally { Pop-Location }