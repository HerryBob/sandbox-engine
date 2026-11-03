# 生成 HUD 位图字体图集 -> assets/textures/font/ascii.png
#
# 布局（与 src/render/hud/HudFont.hpp 的常量必须一致）：
#   格子   10 x 16 px
#   网格   16 列 x 6 行 = 96 格 -> 图集 160 x 96 px
#   格 0   纯白块（HUD 背景面板采样它，alpha 混合时用）
#   格 1..95  ASCII 32..126（95 个可打印字符），字符 c 的格序号 = c - 31
#
# 为什么离线生成、产物入库：
#   引擎运行时不依赖系统字体（换机器、换字体都不会让 HUD 变形），
#   也避免把有版权的字体文件放进仓库 —— 这里只是用本机 Consolas 栅格化一次。
#   要换字体/字号/加中文，重跑本脚本即可，C++ 侧只改格子尺寸常量。
#
# 用法：pwsh -File tools\gen_font_atlas.ps1

Add-Type -AssemblyName System.Drawing

$CW = 10      # 格子宽
$CH = 16      # 格子高
$COLS = 16    # 列数
$ROWS = 6     # 行数
$W = $CW * $COLS
$H = $CH * $ROWS

$outDir = Join-Path $PSScriptRoot "..\assets\textures\font"
if (-not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
$outPath = Join-Path $outDir "ascii.png"

$bmp = New-Object System.Drawing.Bitmap -ArgumentList $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(255, 0, 0, 0))
# 单色栅格化：不做抗锯齿，保证每个像素非黑即白（NEAREST 采样时最锐）
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
$font = New-Object System.Drawing.Font -ArgumentList "Consolas", 13, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
$sf = [System.Drawing.StringFormat]::GenericTypographic
$white = [System.Drawing.Brushes]::White

# 格 0：纯白（渲染 HUD 背板时采样这一格，覆盖率恒为 1）
$g.FillRectangle($white, 0, 0, $CW, $CH)

for ($i = 0; $i -lt 95; $i++) {
    $c = [char](32 + $i)
    if ($c -eq [char]32) { continue }   # 空格：留黑，不画
    $idx = $i + 1
    $col = $idx % $COLS
    $row = [math]::Floor($idx / $COLS)
    # 单字符量宽高后居中，避免宽字符（W、@、%）压到邻格
    $box = $g.MeasureString([string]$c, $font, [System.Drawing.PointF]::new(0, 0), $sf)
    $xo = [math]::Round((($CW - $box.Width) / 2))
    $yo = [math]::Round((($CH - $box.Height) / 2)) + 1
    $g.DrawString([string]$c, $font, $white, [float]($col * $CW + $xo), [float]($row * $CH + $yo), $sf)
}

$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

Write-Output ("generated: " + $outPath + "  (" + $W + "x" + $H + ")")
