#requires -Version 5
# Build + package + test-sign srcraid into .\package\ (self-signed test cert).
# The resulting package installs on a lab box with testsigning on + the test cert trusted.
param(
    [string]$WdkVer = "10.0.26100.0",
    [string]$Os     = "10_X64",           # catalog OS attribute (broad)
    [ValidateSet("Debug","Release")] [string]$Config = "Debug",
    [string]$CertThumbprint = "40b9479f1bee09a087e64a186db7edf9867a5f03"  # WDKTestCert
)
$ErrorActionPreference = "Stop"
# pwsh 7.3+ keeps manual arg quotes literal; Legacy restores 5.1 behaviour (no-op on 5.1).
$PSNativeCommandArgumentPassing = 'Legacy'
$root = $PSScriptRoot

# locate a WDK tool by exact version path, else highest version across all installed kits
function Resolve-KitTool([string]$exact, [string]$glob, [switch]$Optional) {
    if (Test-Path $exact) { return $exact }
    $hit = (Get-ChildItem $glob -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1).FullName
    if (-not $hit -and -not $Optional) { throw "WDK tool not found: $(Split-Path $exact -Leaf) (searched $exact and $glob)" }
    return $hit
}
$kbin   = "C:\Program Files (x86)\Windows Kits\10\bin"
$ktools = "C:\Program Files (x86)\Windows Kits\10\Tools"

& (Join-Path $root "build.ps1") -WdkVer $WdkVer -Config $Config
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$pkg = Join-Path $root "package"
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item (Join-Path $root "build\srcraid.sys") $pkg -Force
Copy-Item (Join-Path $root "srcraid.inf")       $pkg -Force
Copy-Item (Join-Path $root "install.bat")       $pkg -Force
Copy-Item (Join-Path $root "rollback.bat")      $pkg -Force
# devcon is a lab convenience for install.bat; optional (CI runners may not ship it)
$devcon = Resolve-KitTool "$ktools\$WdkVer\x64\devcon.exe" "$ktools\*\x64\devcon.exe" -Optional
if ($devcon) { Copy-Item $devcon $pkg -Force }
else { Write-Warning "devcon.exe not found - omitting from package (install via 'pnputil /add-driver srcraid.inf /install')" }

# stamp INF DriverVer with the just-built version (PnP ranks on this)
$build = [int]((Get-Content (Join-Path $root "buildnum.txt") -Raw).Trim())
$stampinf = Resolve-KitTool "$kbin\$WdkVer\x64\stampinf.exe" "$kbin\*\x64\stampinf.exe"
& $stampinf -f (Join-Path $pkg "srcraid.inf") -d "11/14/2026" -v "0.1.0.$build"
if ($LASTEXITCODE -ne 0) { throw "stampinf failed" }
Write-Host "[stampinf] DriverVer -> 0.1.0.$build" -ForegroundColor Cyan

$inf2cat  = Resolve-KitTool "$kbin\$WdkVer\x86\Inf2Cat.exe"  "$kbin\*\x86\Inf2Cat.exe"
$signtool = Resolve-KitTool "$kbin\$WdkVer\x64\signtool.exe" "$kbin\*\x64\signtool.exe"

Write-Host "[inf2cat] generating catalog ..." -ForegroundColor Cyan
& $inf2cat /driver:$pkg /os:$Os /verbose
if ($LASTEXITCODE -ne 0) { throw "inf2cat failed ($LASTEXITCODE)" }

# --- code-signing cert: prefer the supplied (WDK test) cert, else self-signed ---
$cert = $null
if ($CertThumbprint) {
    $cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $CertThumbprint } | Select-Object -First 1
    if ($cert) { Write-Host "[cert] using $($cert.Subject)" -ForegroundColor Cyan }
    else { Write-Warning "cert $CertThumbprint not found; falling back to self-signed" }
}
if (-not $cert) {
    $subject = "CN=srcraid Test Cert"
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
    if (-not $cert) {
        Write-Host "[cert] creating self-signed code-signing cert ..." -ForegroundColor Cyan
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
                    -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable `
                    -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(5)
    }
}
Export-Certificate -Cert $cert -FilePath (Join-Path $pkg "srcraid_test.cer") -Force | Out-Null

function Sign-File($path) {
    $args = @("sign","/v","/fd","sha256","/sha1",$cert.Thumbprint)
    # try to timestamp; proceed unsigned-timestamp if offline
    try {
        & $signtool @args "/tr" "http://timestamp.digicert.com" "/td" "sha256" $path
        if ($LASTEXITCODE -ne 0) { throw }
    } catch {
        Write-Warning "timestamp failed (offline?) - signing without timestamp"
        & $signtool @args $path
        if ($LASTEXITCODE -ne 0) { throw "signtool failed on $path" }
    }
}

Write-Host "[sign] srcraid.cat / srcraid.sys ..." -ForegroundColor Cyan
Sign-File (Join-Path $pkg "srcraid.cat")
Sign-File (Join-Path $pkg "srcraid.sys")

# Informational only: /pa verify fails on the build box until the test root is
# trusted (it will be, on the target, after importing srcraid_test.cer).
try { & $signtool verify /pa /c (Join-Path $pkg "srcraid.cat") (Join-Path $pkg "srcraid.sys") 2>&1 | Out-Host } catch {}
Write-Host "(verify above is expected to fail until the test cert is trusted on the target)" -ForegroundColor DarkGray

Write-Host "`nPACKAGE READY: $pkg" -ForegroundColor Green
Get-ChildItem $pkg | Select-Object Name, Length | Format-Table -AutoSize

# the informational /pa verify above leaves a non-zero $LASTEXITCODE (cert not yet trusted);
# reaching here means every guarded step succeeded, so exit clean for CI.
exit 0
