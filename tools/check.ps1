#Requires -Version 5.1
<#
.SYNOPSIS
    一键验证：构建 + 冒烟比对（重构 / 修 bug 的每个步骤都用它）

.DESCRIPTION
    把「构建」和「冒烟」合成一条命令，减少来回调用次数。
    smoke.ps1 自己会调 build.ps1，所以这里只是给出一套固定参数 + 一行结论。

        .\tools\check.ps1               构建 + 对比基线（冒烟 16 秒）
        .\tools\check.ps1 -Fast         构建 + 对比基线（冒烟 8 秒；纯注释/改名够用）
        .\tools\check.ps1 -NoBuild      跳过构建（会校验 exe 不比源码旧）
        .\tools\check.ps1 -Baseline     构建 + 重写基线（判定"行为变化是可接受的"时用）
        .\tools\check.ps1 -Quiet        只打印一行结论（适合连续多步）

    退出码：0 = 通过；1 = 构建失败 / 比对超差 / 缺指标

.EXAMPLE
    .\tools\check.ps1 -Fast
    纯注释改动后的快速验证（省一半冒烟时间）

.EXAMPLE
    .\tools\check.ps1 -Baseline
    改了行为且确认是对的（例如接入 Jolt 后物理数值变化）→ 重写基线
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [int]$Seconds = 0,
    [switch]$Fast,
    [switch]$NoBuild,
    [switch]$Baseline,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build\Debug' }
if ($Seconds -le 0) { $Seconds = if ($Fast) { 8 } else { 16 } }

$t0 = Get-Date
$smokeArgs = @{ BuildDir = $BuildDir; Seconds = $Seconds }
if ($NoBuild)  { $smokeArgs['NoBuild'] = $true }
if (-not $Baseline) { $smokeArgs['Compare'] = $true }

if ($Quiet) {
    $raw = & (Join-Path $PSScriptRoot 'smoke.ps1') @smokeArgs *>&1
} else {
    & (Join-Path $PSScriptRoot 'smoke.ps1') @smokeArgs
}
$code = $LASTEXITCODE
$secs = [int]((Get-Date) - $t0).TotalSeconds

$verdict = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
$mode = if ($Baseline) { '重写基线' } else { '对比基线' }
Write-Host ("[check] {0}  {1}s  {2}  （冒烟 {3}s）" -f $verdict, $secs, $mode, $Seconds) `
    -ForegroundColor $(if ($code -eq 0) { 'Green' } else { 'Red' })

if ($Quiet -and $code -ne 0) {
    # 失败时把关键行打出来，否则只看到 FAIL 不知所措
    $raw | Select-String -Pattern 'FAIL|error|缺|超差' | Select-Object -Last 10 | ForEach-Object { "  " + $_.Line }
}

exit $code
