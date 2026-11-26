#Requires -Version 5.1
<#
.SYNOPSIS
    冒烟基线：构建 → 运行 → 抓指标 → 写入 / 比对 bench\baseline.txt

.DESCRIPTION
    重构规则 0。所有重构步骤的"与上一次比对"都靠这个脚本。
      无参       → 跑一次并写入 bench\baseline.txt（建基线）
      -Compare   → 跑一次并与基线逐项比对，输出 PASS/FAIL（不一致 → 退出码 1）
                   首次不通过会隔 2 秒自动复测一次（真实回归会两次都失败，
                   而"构建刚结束"造成的机器负载抖动通常只影响第一次）。

    指标全部从引擎日志解析（不需要引擎提供内部计数接口）：
      selftest          8 项自检通过的条数（应恒为 8）
      tps_physics       最后一条 [物理] 的平均 tps（稳定，容差 ±0.1）
      tps_business      最后一条 [业务] 的平均 tps（15s 时仍在爬升，容差 ±0.5）
      blocks            初始世界：方块=N
      chunks            流式预热：chunk=N（非空 chunk 数）
      warmup_*          流式预热：请求列 / 已生成 / 已应用
      mods              加载 N 个模组

    -NoBuild 时会校验 exe 不比源码旧（否则跑的是旧 exe，指标毫无意义）。

.PARAMETER Seconds
    运行时长（秒），默认 16。之后程序被优雅关闭（CloseMainWindow），
    关闭路径才打印"初始世界"与卸载日志 —— 所以不能太小。

.PARAMETER NoBuild
    跳过构建（会先校验 exe 不比源码旧）。

.PARAMETER Compare
    与基线比对，而不是写入。

.EXAMPLE
    .\tools\smoke.ps1                 # 建基线
    .\tools\smoke.ps1 -Compare        # 重构后比对
    .\tools\smoke.ps1 -Compare -NoBuild
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$BaselineFile = "",
    [int]$Seconds = 16,
    [switch]$NoBuild,
    [switch]$Compare
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir)     { $BuildDir     = Join-Path $repo 'build\Debug' }
if (-not $BaselineFile) { $BaselineFile = Join-Path $repo 'bench\baseline.txt' }

# 容差：0 = 必须完全一致
$tolerance = [ordered]@{
    'selftest'         = 0
    'tps_physics'      = 0.1
    'tps_business'     = 0.5
    'blocks'           = 0
    'chunks'           = 0
    'warmup_requested' = 0
    'warmup_generated' = 0
    'warmup_applied'   = 0
    'mods'             = 0
}

# ---- 1. 构建 ----
if (-not $NoBuild) {
    Write-Host "[smoke] 构建 $BuildDir ..."
    & (Join-Path $PSScriptRoot 'build.ps1') -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[smoke] FAIL：构建失败" -ForegroundColor Red
        exit 1
    }
}

$exe = Join-Path $BuildDir 'sandbox_engine.exe'
if (-not (Test-Path $exe)) {
    Write-Host "[smoke] FAIL：找不到 $exe（先构建，或传 -BuildDir）" -ForegroundColor Red
    exit 1
}

# -NoBuild 时必须确认可执行文件不比源码旧：否则跑的是旧 exe，指标"PASS"毫无意义。
if ($NoBuild) {
    $srcDirs = @((Join-Path $repo 'src'), (Join-Path $repo 'modapi'),
                 (Join-Path $repo 'mods_src'), (Join-Path $repo 'shaders'))
    $srcDirs = $srcDirs | Where-Object { Test-Path $_ }
    $newest = Get-ChildItem $srcDirs -Recurse -File -Include *.cpp,*.hpp,*.h,*.vert,*.frag |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newest -and (Get-Item $exe).LastWriteTime -lt $newest.LastWriteTime) {
        Write-Host ("[smoke] FAIL：exe 比源码旧（最新源：{0}，{1}）——先构建，或去掉 -NoBuild" -f `
                    $newest.Name, $newest.LastWriteTime) -ForegroundColor Red
        exit 1
    }
}

# ---- 2~3. 运行 + 抓指标（可重复调用）----
function Invoke-Measure {
    # 日志留在工作区内（AGENTS.md：不要在 E:\MinecraftGame2 之外写文件）
    $logDir = Join-Path $repo 'build\smoke-logs'
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
    $log = Join-Path $logDir ('smoke_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.log')
    $err = $log + '.err'

    # --no-save：冒烟/基线必须跑"纯净世界"。否则玩家存档（他挖过/建过的世界）会让
    # blocks/chunks 指标偏离基线 —— 那不是回归，是"你玩过"，基线就失去意义了。
    $proc = Start-Process -FilePath $exe -WorkingDirectory $BuildDir `
            -ArgumentList '--no-save' `
            -RedirectStandardOutput $log -RedirectStandardError $err -PassThru
    Start-Sleep -Seconds $Seconds
    if (-not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        $proc.WaitForExit(10000) | Out-Null
    }
    if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }

    if (-not (Test-Path $log)) { return $null }

    $lines = Get-Content $log -Encoding UTF8
    $m = @{}

    $m['selftest'] = ($lines | Select-String -Pattern '自检通过' -SimpleMatch).Count

    $phys = $lines | Select-String -Pattern '\[物理\]' | Select-Object -Last 1
    if ($phys -and $phys.Line -match '平均tps=([0-9.]+)') { $m['tps_physics'] = [double]$matches[1] }

    $biz = $lines | Select-String -Pattern '\[业务\]' | Select-Object -Last 1
    if ($biz -and $biz.Line -match '平均tps=([0-9.]+)') { $m['tps_business'] = [double]$matches[1] }

    $world = $lines | Select-String -Pattern '初始世界：方块=(\d+)' | Select-Object -Last 1
    if ($world) { $m['blocks'] = [int]$world.Matches[0].Groups[1].Value }

    $warm = $lines | Select-String -Pattern '流式预热：请求列=(\d+) 已生成=(\d+) 已应用=(\d+) 方块=(\d+) .*chunk=(\d+)' | Select-Object -Last 1
    if ($warm) {
        $m['warmup_requested'] = [int]$warm.Matches[0].Groups[1].Value
        $m['warmup_generated'] = [int]$warm.Matches[0].Groups[2].Value
        $m['warmup_applied']   = [int]$warm.Matches[0].Groups[3].Value
        if (-not $m.ContainsKey('blocks')) { $m['blocks'] = [int]$warm.Matches[0].Groups[4].Value }
        $m['chunks'] = [int]$warm.Matches[0].Groups[5].Value
    }

    $mods = $lines | Select-String -Pattern '加载 (\d+) 个模组' | Select-Object -Last 1
    if ($mods) { $m['mods'] = [int]$mods.Matches[0].Groups[1].Value }

    Write-Host "[smoke] 日志：$log"
    Write-Host "[smoke] 本次指标："
    foreach ($k in $tolerance.Keys) {
        if ($m.ContainsKey($k)) { Write-Host ('        {0}={1}' -f $k, $m[$k]) }
    }
    return $m
}

# 比对并打印表格；返回超差项数
function Compare-Metrics($m) {
    $base = @{}
    foreach ($line in (Get-Content $BaselineFile -Encoding UTF8)) {
        if ($line -match '^\s*#' -or $line -notmatch '=') { continue }
        $kv = $line -split '=', 2
        $base[$kv[0].Trim()] = $kv[1].Trim()
    }

    $fail = 0
    Write-Host ''
    Write-Host ('{0,-17} {1,12} {2,12} {3,10}  {4}' -f 'metric', 'baseline', 'now', 'diff', 'result')
    Write-Host ('-' * 66)
    foreach ($k in $tolerance.Keys) {
        $b = if ($base.ContainsKey($k)) { [double]$base[$k] } else { $null }
        $n = if ($m.ContainsKey($k)) { [double]$m[$k] } else { $null }
        $ok = ($null -ne $b) -and ($null -ne $n) -and ([math]::Abs($n - $b) -le $tolerance[$k])
        if (-not $ok) { $fail++ }
        $d = if (($null -ne $b) -and ($null -ne $n)) { [math]::Round($n - $b, 4) } else { '-' }
        $bs = if ($null -ne $b) { $b } else { '缺' }
        $ns = if ($null -ne $n) { $n } else { '缺' }
        Write-Host ('{0,-17} {1,12} {2,12} {3,10}  {4}' -f $k, $bs, $ns, $d, $(if ($ok) { 'PASS' } else { 'FAIL' })) `
            -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
    }
    Write-Host ('-' * 66)
    return $fail
}

# ---- 4. 建基线（无比对）----
if (-not $Compare) {
    $m = Invoke-Measure
    $missing = @($tolerance.Keys | Where-Object { -not $m.ContainsKey($_) })
    if ($missing.Count -gt 0) {
        Write-Host ("[smoke] FAIL：缺少指标 {0}（常见原因：崩溃 / 未正常退出 / 预热超时）" -f ($missing -join ',')) -ForegroundColor Red
        exit 1
    }
    $dir = Split-Path -Parent $BaselineFile
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $out = @('# smoke baseline')
    foreach ($k in $tolerance.Keys) { $out += ('{0}={1}' -f $k, $m[$k]) }
    Set-Content -Path $BaselineFile -Value $out -Encoding UTF8
    Write-Host "[smoke] 已写入基线：$BaselineFile" -ForegroundColor Green
    exit 0
}

# ---- 5. 比对（首次失败自动复测一次）----
if (-not (Test-Path $BaselineFile)) {
    Write-Host "[smoke] FAIL：基线不存在（先不带 -Compare 跑一次）" -ForegroundColor Red
    exit 1
}

$maxAttempts = 2
$fail = 1
for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
    if ($attempt -gt 1) {
        Write-Host "[smoke] 第 $($attempt - 1) 次未通过，2 秒后复测（负载抖动 / 真实回归都能区分）..." -ForegroundColor Yellow
        Start-Sleep -Seconds 2
    }
    $m = Invoke-Measure
    $missing = @($tolerance.Keys | Where-Object { -not $m.ContainsKey($_) })
    if ($missing.Count -gt 0) {
        Write-Host ("[smoke] 缺少指标 {0}（崩溃 / 未正常退出 / 预热超时）" -f ($missing -join ',')) -ForegroundColor Red
        $fail = 1
        continue
    }
    $fail = Compare-Metrics $m
    if ($fail -eq 0) {
        Write-Host '[smoke] PASS（全部指标在容差内）' -ForegroundColor Green
        exit 0
    }
}

Write-Host "[smoke] FAIL：$fail 项超出容差（已复测 $maxAttempts 次）" -ForegroundColor Red
exit 1
