param(
    [Parameter(Mandatory = $true)][string]$Watcom,
    [string]$OutputDir = "work/nt5-opengl",
    [switch]$SingleBuffer
)

$ErrorActionPreference = "Stop"
$kit = (Resolve-Path -LiteralPath $Watcom).Path
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "../..")).Path
$source = Join-Path $repo "drivers/nt/opengl"
$archive = Join-Path $repo "drivers/windows9x/velocity9x/velocity9x-wysedos-g5k-src.zip"
$sourceHash = "75f77776c4f4c11d0327bcca7385cb065d4a5814a7320d2e4a00c70632a47aea"
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sourceHash) {
    throw "Velocity source archive hash differs from the pinned revision."
}
if ($OutputDir -notmatch '^work/[A-Za-z0-9_/-]+$' -or
    $OutputDir.EndsWith("/") -or $OutputDir.Contains("//")) {
    throw "Choose a new build directory under work/."
}
$build = Join-Path $repo $OutputDir
if (Test-Path -LiteralPath $build) {
    throw "Build directory already exists. Choose a separate output directory."
}
$velocity = Join-Path $build "velocity"
$null = New-Item -ItemType Directory -Path $velocity -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    foreach ($entry in $zip.Entries) {
        if (!$entry.Name -or $entry.FullName -notmatch
            '^velocity9x/(include/|src/opengl/|scripts/(lib/gl-dispatch|build-opengl-icd)\.ps1$)') {
            continue
        }
        $relative = $entry.FullName.Substring("velocity9x/".Length)
        $target = Join-Path $velocity $relative
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target)
    }
} finally {
    $zip.Dispose()
}
$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($name in @("gl_surface.c", "gl_surface.h", "gl_icd.c", "gl_state.c", "gl_state.h", "gl_get.c")) {
    $path = Join-Path $velocity "src/opengl/$name"
    $original = [IO.File]::ReadAllText($path, $utf8).Replace("`r`n", "`n")
    [IO.File]::WriteAllText($path, $original, $utf8)
}
$patches = @("gl_surface-nt5.patch", "gl_mip-nt5.patch", "gl_scene-nt5.patch")
if ($SingleBuffer) { $patches += "gl_single_buffer-nt5.patch" }
foreach ($patch in $patches) {
    & git -C $repo -c core.autocrlf=false apply --no-index "--directory=$OutputDir/velocity" (Join-Path $source $patch)
    if ($LASTEXITCODE) { throw "Could not apply $patch." }
}

$env:WATCOM = $kit
$env:INCLUDE = "$kit/h/nt/directx;$kit/h;$kit/h/nt"
. "$velocity/scripts/lib/gl-dispatch.ps1"
$null = Write-V9xGlDispatchHeader -RepoRoot $velocity -OutputDir $build

$sources = @(
    "gl_icd", "gl_surface", "gl_state", "gl_matrix", "gl_prim", "gl_texture",
    "gl_get", "gl_pixels", "gl_varray", "gl_attrib", "gl_light"
) | ForEach-Object { Join-Path $velocity "src/opengl/$_.c" }
$sources = @($sources) + (Join-Path $source "ntgl_d3d7.c")
$flags = @("-bt=nt", "-bd", "-zq", "-wx", "-we", "-zl", "-s", "-ox",
           "-i=$velocity/include", "-i=$build")
$objects = foreach ($file in $sources) {
    $object = Join-Path $build ([IO.Path]::GetFileNameWithoutExtension($file) + ".obj")
    & "$kit/binnt64/wcc386.exe" @flags "-fo=$object" $file | Out-Host
    if ($LASTEXITCODE) { throw "Open Watcom failed compiling $file." }
    $object
}

$dll = Join-Path $build "WGGLNT.DLL"
$link = Join-Path $build "wgglnt.lnk"
$lines = @(
    "format windows nt dll", "runtime windows=5.0", "option quiet",
    "option nodefaultlibs", "option start='_V9xGlEntry@12'",
    "alias '__DLLstart_'='_V9xGlEntry@12'", "option modname=WGGLNT", "name '$dll'"
)
$lines += $objects | ForEach-Object { "file '$_'" }
$recipe = [IO.File]::ReadAllText("$velocity/scripts/build-opengl-icd.ps1")
$exports = [regex]::Matches($recipe, "'(?<name>Drv\w+)=(?<symbol>_Drv\w+@\d+)'")
if ($exports.Count -ne 18) { throw "Unexpected number of OpenGL ICD exports." }
$lines += $exports | ForEach-Object {
    "export $($_.Groups['name'].Value)='$($_.Groups['symbol'].Value)'"
}
$lines += @(
    "library '$kit/lib386/nt/kernel32.lib'",
    "library '$kit/lib386/nt/user32.lib'",
    "library '$kit/lib386/nt/directx/dxguid.lib'"
)
$lines | Set-Content -LiteralPath $link -Encoding ascii
& "$kit/binnt64/wlink.exe" "@$link"
if ($LASTEXITCODE) { throw "Open Watcom failed linking the OpenGL ICD." }
Get-FileHash -LiteralPath $dll -Algorithm SHA256
