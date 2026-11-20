#requires -Version 5
# Build srcraid.sys (Storport miniport) with WDK 10.0.26100 km headers/libs.
# Usage: .\build.ps1   (optionally -WdkVer 10.0.26100.0 -Config Debug)
param(
    [string]$WdkVer = "10.0.26100.0",
    [ValidateSet("Debug","Release")] [string]$Config = "Debug"
)
$ErrorActionPreference = "Stop"

# CI runs this under PowerShell 7 (pwsh). Since 7.3 the default native-argument passing
# ('Standard') keeps our manually-quoted  "/OUT:`"$path`""  quotes LITERAL, so link.exe is
# asked to create a file whose name contains quote characters -> fatal LNK1104. 'Legacy'
# restores Windows PowerShell 5.1 behaviour (quotes consumed as delimiters). This variable
# exists only on pwsh 7.2+; assigning it on Windows PowerShell 5.1 is a harmless no-op.
$PSNativeCommandArgumentPassing = 'Legacy'

$root   = $PSScriptRoot
$srcDir = Join-Path $root "src"
$outDir = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# --- locate MSVC ---
$vs    = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022" -Directory | Select-Object -First 1
$msvc  = Get-ChildItem (Join-Path $vs.FullName "VC\Tools\MSVC") -Directory | Sort-Object Name -Descending | Select-Object -First 1
$cl    = Join-Path $msvc.FullName "bin\Hostx64\x64\cl.exe"
$link  = Join-Path $msvc.FullName "bin\Hostx64\x64\link.exe"
$msvcInc = Join-Path $msvc.FullName "include"
$msvcLib = Join-Path $msvc.FullName "lib\x64"

# --- WDK paths ---
$kitInc = "C:\Program Files (x86)\Windows Kits\10\Include\$WdkVer"
$kitLib = "C:\Program Files (x86)\Windows Kits\10\Lib\$WdkVer\km\x64"
$incKm     = Join-Path $kitInc "km"
$incShared = Join-Path $kitInc "shared"
$incCrt    = Join-Path $kitInc "km\crt"

foreach ($p in @($cl,$link,$incKm,$incShared,$kitLib)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}

# --- resource compiler (version resource) ---
$rc    = "C:\Program Files (x86)\Windows Kits\10\bin\$WdkVer\x64\rc.exe"
if (-not (Test-Path $rc)) { $rc = (Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\rc.exe" | Sort-Object FullName -Descending | Select-Object -First 1).FullName }
$incUm = Join-Path $kitInc "um"

# --- auto-increment build number (last field of 0.1.0.N) ---
$verFile = Join-Path $root "buildnum.txt"
$build = 0
if (Test-Path $verFile) { $build = [int]((Get-Content $verFile -Raw).Trim()) }
$build++
Set-Content -Path $verFile -Value $build -NoNewline
$script:SrcBuildNum = $build
Write-Host "Version: 0.1.0.$build" -ForegroundColor Yellow

$dbg = if ($Config -eq "Debug") { @("/Od","/D_DEBUG","/DDBG=1") } else { @("/O2","/DNDEBUG") }

$common = @(
    "/nologo","/c","/W4","/WX-","/Zi","/FC","/kernel","/GS","/Gy","/wd4324","/wd4152"
    "/D_AMD64_","/DAMD64","/D_WIN64","/DVER_BUILD=$build"
) + $dbg + @(
    "/I`"$incCrt`"","/I`"$incKm`"","/I`"$incShared`"","/I`"$msvcInc`""
    "/Fd`"$outDir\srcraid.pdb`""
)

$objs = @()
foreach ($c in (Get-ChildItem (Join-Path $srcDir '*.c') | Sort-Object Name)) {
    $obj = Join-Path $outDir ($c.BaseName + ".obj")
    Write-Host "[cl] $($c.Name)" -ForegroundColor Cyan
    & $cl @common "/Fo`"$obj`"" "`"$($c.FullName)`""
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($c.Name) ($LASTEXITCODE)" }
    $objs += $obj
}

# --- compile version resource ---
$res = Join-Path $outDir "srcraid.res"
Write-Host "[rc] srcraid.rc" -ForegroundColor Cyan
& $rc /nologo /dVER_BUILD=$build "/fo$res" "/I$incUm" "/I$incShared" "/I$msvcInc" "$srcDir\srcraid.rc"
if ($LASTEXITCODE -ne 0) { throw "rc failed ($LASTEXITCODE)" }

# --- resolve kernel import libs to absolute paths (robust across partial/split WDK layouts) ---
# The GitHub runner can expose a WDK version that has storport.lib (so version-detect picks it)
# but is missing a sibling like BufferOverflowFastFailK.lib; search every installed Lib version
# per-lib and bind the absolute path so link can't LNK1104 on a lib that exists *somewhere*.
$kitLibRoot = "C:\Program Files (x86)\Windows Kits\10\Lib"
$umLib = Join-Path $kitLibRoot "$WdkVer\um\x64"
$reqLibs = @("storport.lib","ntoskrnl.lib","hal.lib","BufferOverflowFastFailK.lib")
$libAbs = @()
foreach ($l in $reqLibs) {
    $hit = @((Join-Path $kitLib $l),(Join-Path $umLib $l)) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $hit) {
        $hit = (Get-ChildItem "$kitLibRoot\*\km\x64\$l","$kitLibRoot\*\um\x64\$l" -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending | Select-Object -First 1).FullName
    }
    if (-not $hit) { throw "link lib not found: $l (searched $kitLib, $umLib, and all WDK Lib versions under $kitLibRoot)" }
    $libAbs += $hit
}
Write-Host "[libs] $(($libAbs | ForEach-Object { Split-Path $_ -Parent } | Select-Object -Unique) -join '; ')" -ForegroundColor DarkGray

$linkArgs = @(
    "/nologo","/DRIVER","/SUBSYSTEM:NATIVE,10.0","/ENTRY:GsDriverEntry","/MACHINE:X64"
    "/NODEFAULTLIB","/INTEGRITYCHECK","/RELEASE","/OPT:REF,ICF","/INCREMENTAL:NO","/DEBUG"
    "/LIBPATH:`"$kitLib`"","/LIBPATH:`"$umLib`"","/LIBPATH:`"$msvcLib`""
    "/OUT:`"$outDir\srcraid.sys`"","/PDB:`"$outDir\srcraid.pdb`"","/MAP:`"$outDir\srcraid.map`""
) + ($libAbs | ForEach-Object { "`"$_`"" }) + ($objs | ForEach-Object { "`"$_`"" }) + @("`"$res`"")

Write-Host "[link] linking srcraid.sys ..." -ForegroundColor Cyan
& $link @linkArgs
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }

Write-Host "`nBUILD OK:" -ForegroundColor Green
Get-Item "$outDir\srcraid.sys" | Select-Object FullName, Length, LastWriteTime | Format-List
