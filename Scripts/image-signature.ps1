# ============================================================
# image-signature.ps1 — 把一张 PPM 压成可提交的"图像签名", 并与基线比对
#
# 为什么不直接比整张图: 1280×720 的 PPM 是 2.7 MB, 每次改动都提交一张的话
# 仓库会迅速膨胀, 而且逐像素相等对 GPU 渲染来说过于严苛 —— 驱动版本、
# 光栅化的绑定规则、浮点收缩都会让个别像素差 1。
#
# 签名是 32×18 的平均池化 (每格 40×40 像素), 1728 字节。它对以下改动**极其
# 敏感**: 亮度整体偏移、颜色通道错位、光源丢失、阴影跑位、材质参数错乱 ——
# 也就是"UBO 字段整体错位"这类改动的全部可观测后果。而它对单像素级的驱动
# 差异不敏感, 因为 1600 个像素平均之后那些差异被抹平了。
#
# 用法:
#   image-signature.ps1 -Ppm <图> -Write <签名文件>        # 生成基线
#   image-signature.ps1 -Ppm <图> -Baseline <签名文件>     # 比对
#
# 退出码: 0 = 一致或已写入, 1 = 不一致或出错
# ============================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Ppm,

    # 生成基线签名文件
    [string]$Write = '',

    # 与已有基线比对
    [string]$Baseline = '',

    # 每格平均值允许的最大偏差 (0-255)
    #
    # 2 是给驱动侧的浮点差异留的余量。一个 40×40 格子里 1600 个像素平均
    # 之后, 单像素差 1 只会让格子均值动 1/1600 —— 也就是说 2 已经宽到能
    # 容下整格系统性偏移 1 的情况, 而任何真实的着色改动都远超它。
    [int]$Tolerance = 2
)

$ErrorActionPreference = 'Continue'

if (-not (Test-Path $Ppm))
{
    Write-Host "错误: 未找到 $Ppm" -ForegroundColor Red
    exit 1
}

if ($Write -eq '' -and $Baseline -eq '')
{
    Write-Host '错误: 必须给出 -Write 或 -Baseline 之一' -ForegroundColor Red
    exit 1
}

$GridX = 32
$GridY = 18

# ---- 读 PPM (P6, 二进制) ----
$bytes = [System.IO.File]::ReadAllBytes($Ppm)

# 头是三行文本: "P6\n<w> <h>\n255\n"。逐字节找第三个换行, 不用文本解码 ——
# 载荷是二进制, 按文本读会被编码转换破坏。
$newlines = 0
$offset   = 0

for ($i = 0; $i -lt $bytes.Length -and $newlines -lt 3; $i++)
{
    if ($bytes[$i] -eq 10)
    {
        $newlines++
        $offset = $i + 1
    }
}

if ($newlines -lt 3)
{
    Write-Host "错误: $Ppm 不是合法的 P6 PPM (头不完整)" -ForegroundColor Red
    exit 1
}

$headerText = [System.Text.Encoding]::ASCII.GetString($bytes, 0, $offset)
$lines = $headerText -split "`n"

if ($lines[0].Trim() -ne 'P6')
{
    Write-Host "错误: $Ppm 不是 P6 格式 (magic = $($lines[0].Trim()))" -ForegroundColor Red
    exit 1
}

$dims = $lines[1].Trim() -split '\s+'
$width  = [int]$dims[0]
$height = [int]$dims[1]

$expectedBytes = $offset + $width * $height * 3

if ($bytes.Length -lt $expectedBytes)
{
    Write-Host ("错误: $Ppm 载荷不足 — 声明 {0}x{1} 需要 {2} 字节, 实际 {3}" -f `
        $width, $height, $expectedBytes, $bytes.Length) -ForegroundColor Red
    exit 1
}

# ---- 平均池化 ----
$sums   = New-Object 'int[]' ($GridX * $GridY * 3)
$counts = New-Object 'int[]' ($GridX * $GridY)

for ($y = 0; $y -lt $height; $y++)
{
    $gy = [int]([double]$y * $GridY / $height)
    if ($gy -ge $GridY) { $gy = $GridY - 1 }

    $rowBase = $offset + $y * $width * 3

    for ($x = 0; $x -lt $width; $x++)
    {
        $gx = [int]([double]$x * $GridX / $width)
        if ($gx -ge $GridX) { $gx = $GridX - 1 }

        $cell = $gy * $GridX + $gx
        $p    = $rowBase + $x * 3

        $sums[$cell * 3]     += $bytes[$p]
        $sums[$cell * 3 + 1] += $bytes[$p + 1]
        $sums[$cell * 3 + 2] += $bytes[$p + 2]
        $counts[$cell]++
    }
}

$signature = New-Object 'int[]' ($GridX * $GridY * 3)

for ($cell = 0; $cell -lt $GridX * $GridY; $cell++)
{
    if ($counts[$cell] -eq 0) { continue }

    for ($ch = 0; $ch -lt 3; $ch++)
    {
        $signature[$cell * 3 + $ch] =
            [int][math]::Round($sums[$cell * 3 + $ch] / $counts[$cell])
    }
}

# ---- 写基线 ----
if ($Write -ne '')
{
    $text = "# LimxEngine 图像签名 ${GridX}x${GridY} 平均池化`n"
    $text += "# 源: $width x $height`n"

    for ($cell = 0; $cell -lt $GridX * $GridY; $cell++)
    {
        $text += ("{0} {1} {2}`n" -f $signature[$cell * 3],
                  $signature[$cell * 3 + 1], $signature[$cell * 3 + 2])
    }

    [System.IO.File]::WriteAllText($Write, $text)
    Write-Host "签名已写入 $Write (${GridX}x${GridY})" -ForegroundColor Green
    exit 0
}

# ---- 与基线比对 ----
if (-not (Test-Path $Baseline))
{
    Write-Host "错误: 未找到基线 $Baseline" -ForegroundColor Red
    exit 1
}

$baseLines = Get-Content $Baseline | Where-Object { $_ -notmatch '^\s*#' -and $_.Trim() -ne '' }

if ($baseLines.Count -ne $GridX * $GridY)
{
    Write-Host ("错误: 基线有 {0} 格, 期望 {1} —— 网格尺寸不一致" -f `
        $baseLines.Count, ($GridX * $GridY)) -ForegroundColor Red
    exit 1
}

$worst      = 0
$worstCell  = -1
$overCount  = 0

for ($cell = 0; $cell -lt $GridX * $GridY; $cell++)
{
    $parts = $baseLines[$cell].Trim() -split '\s+'

    for ($ch = 0; $ch -lt 3; $ch++)
    {
        $diff = [math]::Abs($signature[$cell * 3 + $ch] - [int]$parts[$ch])

        if ($diff -gt $worst)
        {
            $worst     = $diff
            $worstCell = $cell
        }

        if ($diff -gt $Tolerance) { $overCount++ }
    }
}

if ($overCount -gt 0)
{
    $cx = $worstCell % $GridX
    $cy = [int][math]::Floor($worstCell / $GridX)

    Write-Host ("图像与基线不一致: {0} 个通道超出容差 {1}, 最大偏差 {2} (格 {3},{4})" -f `
        $overCount, $Tolerance, $worst, $cx, $cy) -ForegroundColor Red
    Write-Host "  基线: $Baseline" -ForegroundColor DarkGray
    Write-Host "  实测: $Ppm" -ForegroundColor DarkGray
    exit 1
}

Write-Host ("图像与基线一致 (最大偏差 {0}, 容差 {1})" -f $worst, $Tolerance) -ForegroundColor Green
exit 0
