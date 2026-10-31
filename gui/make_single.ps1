param(
	[switch]$SkipDeploy
)

# stderr от windeployqt/enigmavbconsole нам не мешает, ловим только коды выхода
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $here "dist"
$evb = "P:\Pickle_temp\enigma\pack_odol_gui.evb"
$console = "V:\pickle\tech\enigmavb\enigmavbconsole.exe"
$out = Join-Path $here "..\odol2mlod-gui_single.exe"

if (-not $SkipDeploy) {
	$env:PATH = "P:\Qt\Tools\mingw1310_64\bin;P:\Qt\6.11.1\mingw_64\bin;" + $env:PATH
	New-Item -ItemType Directory -Force -Path $dist | Out-Null
	Copy-Item (Join-Path $here "odol2mlod-gui.exe") (Join-Path $dist "odol2mlod-gui.exe") -Force
	& "P:\Qt\6.11.1\mingw_64\bin\windeployqt.exe" (Join-Path $dist "odol2mlod-gui.exe") | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }
	# рядом с окном нужен сам конвертер, его кладёт пост-сборка CMake
	Copy-Item (Join-Path $here "odol2mlod.exe") (Join-Path $dist "odol2mlod.exe") -Force
}

& (Join-Path $here "pack_single.ps1") -Dist $dist -InputExe (Join-Path $dist "odol2mlod-gui.exe") -OutputExe $out -OutEvb $evb
& $console $evb
if ($LASTEXITCODE -ne 0) { throw "enigmavbconsole failed" }
"OK: $out ($((Get-Item $out).Length) bytes)"
