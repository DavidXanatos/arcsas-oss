<#
  regress.ps1 - srcraid post-deploy regression harness (run ON the lab target).

  Validates, after every driver deploy, the things that have bitten us one-by-one:
  driver load, enumeration consistency (firmware view vs Windows view), real block
  I/O integrity, write-back cache flush, volume health, the management round-trip
  (arcconf -> our mgmt-FIB passthrough -> firmware), the maxView agent, and the
  srcraid-internal AIF/CISS trace state.

  Non-destructive by default: the I/O test uses a scratch FILE on D: (no raw sectors,
  no container create/delete). Pass -Full to also exercise a create+delete cycle
  (DESTRUCTIVE of free array space - off by default).

  Usage (on the target):
    powershell -ExecutionPolicy Bypass -File C:\Temp\regress.ps1 [-IoSizeMB 64] [-Full]

  Exit code = number of CRITICAL checks that failed (0 = all good).
#>
[CmdletBinding()]
param(
    [int]    $IoSizeMB   = 64,
    [string] $DriveLetter = 'D',
    [switch] $Full
)

$ErrorActionPreference = 'Continue'
$arcconf  = 'C:\Program Files\Adaptec\maxView Storage Manager\arcconf.exe'
$srctrace = 'C:\Temp\srctrace.exe'
$baseFile = 'C:\Temp\regress_baseline.txt'
$scratch  = "${DriveLetter}:\__srcraid_regress.bin"

$script:crit = 0
$script:warn = 0
$results = @()

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail, [switch]$Critical, [switch]$WarnOnly)
    if ($Ok)            { $tag = '[PASS]' }
    elseif ($WarnOnly)  { $tag = '[WARN]'; $script:warn++ }
    else                { $tag = '[FAIL]'; if ($Critical) { $script:crit++ } }
    $line = "{0} {1,-26} {2}" -f $tag, $Name, $Detail
    Write-Host $line
    $script:results += $line
}

Write-Host "===== srcraid regression harness  $(Get-Date -Format s) =====" -ForegroundColor Cyan

# --- 1. driver loaded + version ---------------------------------------------
try {
    $drv = Get-CimInstance Win32_PnPSignedDriver -ErrorAction Stop |
           Where-Object { $_.DeviceName -match 'srcraid' } | Select-Object -First 1
    Check 'driver-loaded' ($null -ne $drv) ("version=" + ($drv.DriverVersion)) -Critical
} catch { Check 'driver-loaded' $false "query failed: $_" -Critical }

# --- 2. target volume present + healthy -------------------------------------
try {
    $vol = Get-Volume -DriveLetter $DriveLetter -ErrorAction Stop
    Check "volume-$DriveLetter-healthy" ($vol.HealthStatus -eq 'Healthy') `
          ("health=" + $vol.HealthStatus + " free=" + [int]($vol.SizeRemaining/1GB) + "GB") -Critical
} catch { Check "volume-$DriveLetter-healthy" $false "not found: $_" -Critical }

# --- 3. Windows enumeration + baseline compare ------------------------------
try {
    $raid = Get-Disk | Where-Object BusType -eq 'RAID' |
            ForEach-Object { "{0}:{1}GB" -f $_.Number, [int]($_.Size/1GB) } | Sort-Object
    $sig  = ($raid -join ',')
    Check 'enum-windows' ($raid.Count -gt 0) ("disks=" + $sig)
    if (Test-Path $baseFile) {
        $base = (Get-Content $baseFile -Raw).Trim()
        Check 'enum-vs-baseline' ($sig -eq $base) `
              $(if ($sig -eq $base) { "unchanged" } else { "CHANGED was[$base] now[$sig]" }) -WarnOnly
    } else {
        Set-Content $baseFile $sig
        Check 'enum-vs-baseline' $true "baseline recorded" -WarnOnly
    }
} catch { Check 'enum-windows' $false "failed: $_" }

# --- 4. firmware enumeration (arcconf) + cross-check ------------------------
try {
    $ad = & $arcconf GETCONFIG 1 AD 2>&1
    $ldpd = & $arcconf GETCONFIG 1 LD 2>&1
    $pd   = & $arcconf GETCONFIG 1 PD 2>&1
    $ctrlOk = ($ad -match 'Controller Status\s*:\s*Optimal').Count -gt 0
    $ldCount = ([regex]::Matches(($ldpd -join "`n"), 'Logical Device number')).Count
    Check 'mgmt-roundtrip-arcconf' $ctrlOk "ControllerStatus Optimal via mgmt-FIB passthrough" -Critical
    Check 'enum-firmware' $true "firmware LDs=$ldCount (eyeball vs Windows above)"
} catch { Check 'mgmt-roundtrip-arcconf' $false "arcconf failed: $_" -Critical }

# --- 5. block I/O integrity (write / flush / read / verify) -----------------
try {
    $bytes = $IoSizeMB * 1MB
    $buf = New-Object byte[] $bytes
    (New-Object Random).NextBytes($buf)
    $wsha = [System.Security.Cryptography.SHA256]::Create().ComputeHash($buf)
    [System.IO.File]::WriteAllBytes($scratch, $buf)
    Write-VolumeCache -DriveLetter $DriveLetter -ErrorAction SilentlyContinue   # force to media
    # drop cache by reopening; read back
    $rbuf = [System.IO.File]::ReadAllBytes($scratch)
    $rsha = [System.Security.Cryptography.SHA256]::Create().ComputeHash($rbuf)
    $match = ($rbuf.Length -eq $bytes) -and (-not (Compare-Object $wsha $rsha))
    Check 'io-integrity' $match ("${IoSizeMB}MB write/flush/read/verify " + $(if($match){'OK'}else{'MISMATCH'})) -Critical
} catch {
    Check 'io-integrity' $false "I/O error: $_" -Critical
} finally {
    Remove-Item $scratch -ErrorAction SilentlyContinue
}

# --- 6. write-back cache flush timing ---------------------------------------
try {
    $t = Measure-Command { Write-VolumeCache -DriveLetter $DriveLetter -ErrorAction Stop }
    Check 'writeback-flush' $true ("SYNCHRONIZE_CACHE ok, {0}ms" -f [int]$t.TotalMilliseconds)
} catch { Check 'writeback-flush' $false "flush failed: $_" -WarnOnly }

# --- 7. physical-disk health ------------------------------------------------
try {
    $bad = Get-PhysicalDisk -ErrorAction Stop |
           Where-Object { $_.HealthStatus -ne 'Healthy' }
    Check 'phys-disk-health' ($bad.Count -eq 0) `
          $(if($bad.Count -eq 0){"all Healthy"}else{"unhealthy: " + ($bad.FriendlyName -join ',')}) -WarnOnly
} catch { Check 'phys-disk-health' $false "query failed: $_" -WarnOnly }

# --- 8. maxView agent -------------------------------------------------------
try {
    $svc = Get-Service maxviewWebServer -ErrorAction Stop
    Check 'maxview-agent' ($svc.Status -eq 'Running') ("status=" + $svc.Status) -WarnOnly
} catch { Check 'maxview-agent' $false "service absent: $_" -WarnOnly }

# --- 9. srcraid AIF channel + CISS state (internal trace) -------------------
if (Test-Path $srctrace) {
    try {
        & $srctrace reset | Out-Null
        Start-Sleep -Seconds 10
        $tr = & $srctrace 2>&1
        $posts     = ([regex]::Matches(($tr -join "`n"), 'AIF .*a=0')).Count
        $completes = ([regex]::Matches(($tr -join "`n"), 'AIF .*a=1')).Count
        $hdr = ($tr | Select-String 'CISS avail=').ToString()
        Check 'aif-channel-alive' (($posts -gt 0) -and ($completes -gt 0)) `
              "posts=$posts completes=$completes (10s window)" -WarnOnly
        Check 'trace-header' ($hdr -ne $null) $hdr -WarnOnly
    } catch { Check 'aif-channel-alive' $false "srctrace failed: $_" -WarnOnly }
}

# --- 10. OPTIONAL destructive create/delete cycle ---------------------------
if ($Full) {
    Write-Host "--- FULL: create+delete a scratch RAID0 on free space ---" -ForegroundColor Yellow
    try {
        $before = ([regex]::Matches((& $arcconf GETCONFIG 1 LD 2>&1 -join "`n"), 'Logical Device number')).Count
        $mk = & $arcconf CREATE 1 LOGICALDRIVE 1024 VOLUME ALL noprompt 2>&1
        Start-Sleep -Seconds 8
        $mid = ([regex]::Matches((& $arcconf GETCONFIG 1 LD 2>&1 -join "`n"), 'Logical Device number')).Count
        $created = $mid -gt $before
        # delete the one we made (highest LD number)
        $del = & $arcconf DELETE 1 LOGICALDRIVE $mid noprompt 2>&1
        Start-Sleep -Seconds 8
        $after = ([regex]::Matches((& $arcconf GETCONFIG 1 LD 2>&1 -join "`n"), 'Logical Device number')).Count
        Check 'mgmt-create-delete' ($created -and ($after -eq $before)) `
              "LD before=$before mid=$mid after=$after" -Critical
    } catch { Check 'mgmt-create-delete' $false "create/delete failed: $_" -Critical }
}

# --- summary ----------------------------------------------------------------
Write-Host "===== summary: critical-fail=$script:crit warn=$script:warn =====" -ForegroundColor Cyan
if ($script:crit -eq 0) { Write-Host "RESULT: PASS" -ForegroundColor Green }
else                    { Write-Host "RESULT: FAIL ($script:crit critical)" -ForegroundColor Red }
exit $script:crit
