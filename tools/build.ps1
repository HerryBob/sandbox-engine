#Requires -Version 5.1
<#
.SYNOPSIS
    Build the engine with the Visual Studio environment loaded.

.DESCRIPTION
    build\Debug uses the Ninja generator, so ninja invokes cl.exe directly and
    relies on INCLUDE / LIB from vcvars64.bat. Running "cmake --build" from a
    plain shell fails with:
        fatal error C1083: cannot open include file: 'stddef.h'
        fatal error C1083: cannot open include file: 'atomic'
    This wrapper loads vcvars first, then builds.

    NOTE (sandbox): the build must be able to write build\Debug. When run from
    a confined shell, elevated permissions are required, otherwise ninja fails
    with "WriteFile(.ninja_lock): Permission denied".

.PARAMETER BuildDir
    Build directory. Default: <repo>\build\Debug

.PARAMETER Target
    Optional target name (e.g. builtin_blocks). Default: all.

.EXAMPLE
    .\tools\build.ps1
    .\tools\build.ps1 -Target sandbox_engine
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$VcVars = "E:\VS2026\VC\Auxiliary\Build\vcvars64.bat",
    [string]$Target = ""
)

$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build\Debug' }

if (-not (Test-Path $VcVars)) {
    Write-Host "[build] FAIL: vcvars not found: $VcVars" -ForegroundColor Red
    exit 1
}

$extra = ""
if ($Target) { $extra = ' --target ' + $Target }
$line = '"{0}" >nul && cmake --build "{1}"{2}' -f $VcVars, $BuildDir, $extra

& cmd.exe /c $line
$code = $LASTEXITCODE
if ($code -eq 0) {
    Write-Host "[build] OK" -ForegroundColor Green
} else {
    Write-Host "[build] FAIL ($code)" -ForegroundColor Red
}
exit $code
