# Builds and runs the standalone PackFileSystem verification harness.
param(
    [string]$Pack = "$PSScriptRoot\..\..\generated_scene\transparent-machines.caustica",
    [string]$AssetsRoot = "$PSScriptRoot\..\..\Assets"
)

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$outDir = Join-Path $env:TEMP 'caustica_pack_fs_test'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'pack_filesystem_test.exe'
$repoRoot = Resolve-Path "$PSScriptRoot\..\.."

$cmd = @"
call "$vcvars" >nul
cl /nologo /EHsc /std:c++20 /W3 /I"$repoRoot\caustica\caustica\include" ^
  "$repoRoot\support\tests\pack_filesystem_standalone.cpp" ^
  "$repoRoot\caustica\caustica\src\core\vfs\PackFileSystem.cpp" ^
  /Fe:"$exe" /Fo:"$outDir\\" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
"$exe" "$Pack" "$AssetsRoot"
"@

$bat = Join-Path $outDir 'build_and_run.cmd'
Set-Content -Path $bat -Value $cmd -Encoding ASCII
& cmd /c $bat
exit $LASTEXITCODE
