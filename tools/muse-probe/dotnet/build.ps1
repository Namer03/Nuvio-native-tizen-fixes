# build.ps1 — build NvProbe.dll (netcoreapp3.1) on this PC and collect the
# artifacts for deployment to the TV. Run from this folder:
#     powershell -ExecutionPolicy Bypass -File .\build.ps1
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Join-Path $here "NvProbe\NvProbe.csproj"
$out  = Join-Path $here "out"

$dn = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dn) {
  Write-Host "No .NET SDK found on this PC." -ForegroundColor Yellow
  Write-Host "Install any recent SDK (it can target netcoreapp3.1), then re-run:"
  Write-Host "  winget install Microsoft.DotNet.SDK.8"
  Write-Host "  or download: https://dotnet.microsoft.com/download"
  exit 1
}
Write-Host ("dotnet SDK: " + (dotnet --version))
dotnet build $proj -c Release -f netcoreapp3.1 -o $out
Write-Host ""
Write-Host ("artifacts in: " + $out) -ForegroundColor Green
Get-ChildItem (Join-Path $out "NvProbe.*") | ForEach-Object { Write-Host ("  {0}  {1} bytes" -f $_.Name, $_.Length) }
Write-Host ""
Write-Host "Next: tell Claude the build is done; it will package these into a TV deploy script."
