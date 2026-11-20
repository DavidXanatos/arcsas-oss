param([int]$Minutes = 8)
# Block-I/O stress against the srcraid RAID volume (D:) to force continuous
# dynamic FIB-slot allocation concurrent with maxView's mgmt IOCTLs. Write-through
# so writes really hit the controller. Logs iteration count + any error.
$ErrorActionPreference = "Continue"
$f   = "D:\srcstress.bin"
$log = "C:\Temp\stress_status.txt"
$buf = New-Object byte[] (16MB)
(New-Object Random).NextBytes($buf)
$tmp = New-Object byte[] (16MB)
$end = (Get-Date).AddMinutes($Minutes)
$n = 0
"START $(Get-Date -Format o)" | Set-Content $log
while ((Get-Date) -lt $end) {
  try {
    $fs = [IO.File]::Open($f, 'Create', 'Write', 'None')
    for ($i = 0; $i -lt 8; $i++) { $fs.Write($buf, 0, $buf.Length) }
    $fs.Flush($true)            # write-through to disk
    $fs.Close()
    $fs = [IO.File]::Open($f, 'Open', 'Read', 'None')
    while ($fs.Read($tmp, 0, $tmp.Length) -gt 0) { }
    $fs.Close()
    $n++
    if ($n % 10 -eq 0) { "ITER $n $(Get-Date -Format o)" | Set-Content $log }
  } catch {
    "ERROR $n $(Get-Date -Format o) :: $($_.Exception.Message)" | Set-Content $log
    Start-Sleep -Seconds 1
  }
}
Remove-Item $f -Force -ErrorAction SilentlyContinue
"DONE iters=$n $(Get-Date -Format o)" | Set-Content $log
