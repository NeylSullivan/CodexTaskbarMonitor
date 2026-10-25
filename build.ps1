# Builds CodexTaskbar.exe with MSVC (Visual Studio 2019/2022).
# Static CRT (/MT), no external dependencies: one self-contained exe.
#
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1
# Tests:  powershell -ExecutionPolicy Bypass -File build.ps1 -Test

param([switch]$Test)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$candidates = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat'
)
$vcvars = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    Write-Error "vcvars64.bat not found. Install Visual Studio 2019/2022 with the 'Desktop development with C++' workload."
}

New-Item -ItemType Directory -Force "$root\build" | Out-Null

function Invoke-Vc([string]$Command, [string]$What) {
    cmd /c "call `"$vcvars`" >nul && $Command"
    if ($LASTEXITCODE -ne 0) { Write-Error "step failed: $What (exit $LASTEXITCODE)" }
}

Invoke-Vc "pushd `"$root\src`" && rc /nologo /fo `"$root\build\app.res`" app.rc && popd" "resource compile"

Invoke-Vc ("cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE " +
           "src\CodexTaskbar.cpp build\app.res /link /SUBSYSTEM:WINDOWS " +
           "/MANIFEST:EMBED /MANIFESTINPUT:src\app.manifest /OUT:build\CodexTaskbar.exe " +
           "user32.lib gdi32.lib shell32.lib advapi32.lib winhttp.lib winmm.lib") "compile+link"

$exe = Get-Item "$root\build\CodexTaskbar.exe"
Write-Output ("built: {0} ({1:N0} bytes)" -f $exe.FullName, $exe.Length)
Write-Output ("description: {0}" -f $exe.VersionInfo.FileDescription)

if ($Test) {
    Invoke-Vc ("cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE " +
               "tests\UsageTests.cpp /Fo:build\UsageTests.obj /Fe:build\UsageTests.exe " +
               "/link /SUBSYSTEM:CONSOLE") "compile tests"
    & "$root\build\UsageTests.exe"
    if ($LASTEXITCODE -ne 0) { Write-Error "usage tests failed (exit $LASTEXITCODE)" }
    Invoke-Vc ("cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE " +
               "tests\TooltipTests.cpp build\app.res /Fo:build\TooltipTests.obj /Fe:build\TooltipTests.exe " +
               "/link /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /MANIFESTINPUT:src\app.manifest") "compile tooltip tests"
    & "$root\build\TooltipTests.exe"
    if ($LASTEXITCODE -ne 0) { Write-Error "tooltip tests failed (exit $LASTEXITCODE)" }
}
