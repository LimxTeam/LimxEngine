# ============================================================
# benchmark.ps1 — 渲染吞吐基准
#
# 在同一份可执行文件上跑四种配置，量化视锥剔除与状态排序各自的收益。
# 四个配置只差命令行开关，场景规模、相机位姿、帧数完全一致，
# 因此差值可以直接归因到开关本身。
#
# 每次测量都会跳过前 --warmup 帧（默认 60）：首帧包含管线首次编译、
# 显存首次触碰、交换链首次呈现，耗时是稳态的数倍，混进平均值会让
# 任何对照失去意义。
#
# 用法:
#   powershell Scripts/benchmark.ps1                  # 默认 60×60，300 帧
#   powershell Scripts/benchmark.ps1 -Grid 80         # 更大规模
#   powershell Scripts/benchmark.ps1 -Frames 600      # 更长采样
#
# 退出码: 0 全部完成 | 1 存在运行失败
# ============================================================

[CmdletBinding()]
param(
    [int]$Grid   = 60,
    [int]$Frames = 300,
    [int]$Warmup = 60
)

$ErrorActionPreference = 'Continue'

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

$Exe = 'Binaries\Development\Win64\LimxLaunch.exe'
$Log = 'Logs\LimxEngine.log'

if (-not (Test-Path $Exe)) {
    Write-Host "错误: 未找到 $Exe，请先构建" -ForegroundColor Red
    exit 1
}

# 四种配置 —— 名称与开关一一对应
$Configurations = @(
    @{ Name = '基线 (剔除关 排序关)'; Args = '--no-cull --no-sort' },
    @{ Name = '仅剔除'.PadRight(0);   Args = '--no-sort' },
    @{ Name = '仅排序';               Args = '--no-cull' },
    @{ Name = '剔除 + 排序';          Args = '' }
)

$Results = @()
$Failed  = $false

Write-Host ''
Write-Host ('=' * 78)
Write-Host "  Limx Engine — 渲染吞吐基准  (网格 ${Grid}x${Grid}, 采样 $Frames 帧, 预热 $Warmup 帧)"
Write-Host ('=' * 78)

foreach ($config in $Configurations) {
    Remove-Item $Log -ErrorAction SilentlyContinue

    $argumentList = "--grid $Grid --frames $Frames --warmup $Warmup $($config.Args)"

    Write-Host ''
    Write-Host "  运行: $($config.Name)" -ForegroundColor Cyan

    $process = Start-Process -FilePath $Exe -ArgumentList $argumentList -PassThru
    $process.WaitForExit(300000) | Out-Null

    if (-not $process.HasExited) {
        $process.Kill()
        Write-Host '    超时' -ForegroundColor Red
        $Failed = $true
        continue
    }

    if (-not (Test-Path $Log)) {
        Write-Host '    未产生日志' -ForegroundColor Red
        $Failed = $true
        continue
    }

    $batchLine  = (Select-String -Path $Log -Pattern '\[基准\] 批次').Line
    $switchLine = (Select-String -Path $Log -Pattern '\[基准\] 状态切换').Line
    $timeLine   = (Select-String -Path $Log -Pattern '\[基准\] 帧耗时').Line

    if (-not $timeLine) {
        Write-Host '    日志中没有基准结果' -ForegroundColor Red
        $Failed = $true
        continue
    }

    # 从日志行里取数 —— 日志格式变化时这里会取到 0，随后在汇总里一眼看得出来
    $visible  = if ($batchLine  -match '可见 (\d+)')     { [int]$Matches[1] }    else { 0 }
    $switches = if ($switchLine -match '材质 (\d+) 次')  { [int]$Matches[1] }    else { 0 }
    $avgMs    = if ($timeLine   -match '平均 ([\d.]+) ms') { [double]$Matches[1] } else { 0 }
    $worstMs  = if ($timeLine   -match '最差 ([\d.]+) ms') { [double]$Matches[1] } else { 0 }

    $Results += [PSCustomObject]@{
        配置       = $config.Name
        可见批次   = $visible
        材质切换   = $switches
        平均耗时ms = [math]::Round($avgMs, 2)
        最差耗时ms = [math]::Round($worstMs, 2)
        帧率       = if ($avgMs -gt 0) { [math]::Round(1000.0 / $avgMs, 1) } else { 0 }
    }

    Write-Host "    平均 $([math]::Round($avgMs,2)) ms  可见 $visible  材质切换 $switches" -ForegroundColor Green
}

Write-Host ''
Write-Host ('=' * 78)
Write-Host '  汇总'
Write-Host ('=' * 78)

$Results | Format-Table -AutoSize

# 以基线为分母给出加速比 —— 绝对毫秒数依赖具体硬件, 比值才是可迁移的结论
if ($Results.Count -eq 4 -and $Results[0].平均耗时ms -gt 0) {
    $baseline = $Results[0].平均耗时ms

    Write-Host '  相对基线的加速比:'
    foreach ($row in $Results[1..3]) {
        if ($row.平均耗时ms -gt 0) {
            $speedup = [math]::Round($baseline / $row.平均耗时ms, 2)
            Write-Host "    $($row.配置): ${speedup}x"
        }
    }
    Write-Host ''
}

if ($Failed) { exit 1 }
exit 0
