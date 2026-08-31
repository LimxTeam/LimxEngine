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
#   powershell Scripts/benchmark.ps1 -SkipImport      # 只跑渲染那半段
#
# 脚本分两段: 前半段量渲染吞吐 (逐帧), 后半段量资产导入 (一次性)。
# 两段都是回归哨兵 —— 超出预算就以 1 退出。
#
# 逐帧那一段同时报 CPU 帧时与 GPU 帧时。两者的比值是判断瓶颈在哪一侧的
# 唯一依据: 第五阶段 Day 1 实测 60x60 网格下 CPU 16.0 ms 而 GPU 1.7 ms,
# 也就是说显卡有近十倍余量闲着, 而此前四周所有"优化"量到的都是 CPU 侧。
# 没有这一列, 很容易把 CPU 瓶颈的改善说成"渲染变快了"。
#
# 退出码: 0 全部完成 | 1 存在运行失败或导入超预算
# ============================================================

[CmdletBinding()]
param(
    [int]$Grid   = 60,
    [int]$Frames = 300,
    [int]$Warmup = 60,

    # 环境贴图路径 —— 缺失时自动跳过 IBL 那一项
    [string]$HdriPath = 'Content/HDRI/bloem_train_track_clear_2k.hdr',

    # ---- 导入基准 ----
    [string]$ImportScene = 'Content/Sponza/Sponza.gltf',
    [int]$ImportRuns     = 5,
    [switch]$SkipImport,

    # 导入耗时预算 (ms)。0 表示只报告不判定。
    #
    # 默认值是基线的三倍 —— 见下方 $ImportBaseline 的说明。
    [double]$ImportBudgetMs = 0,

    # 未埋点 GPU 时间的上限 (%)
    #
    # 整帧是一对独立的时间戳, 不是各 Pass 相加, 所以两者的差额是真实测量
    # 而非恒等式。Day 1 实测干净状态下为 0.7%~1.9%; 故意让一个 Pass 不计时
    # 会跳到 35.8%。取 5% 作上限, 既容得下驱动侧的零碎开销, 又能抓住整个
    # Pass 级别的漏埋。
    [double]$UnaccountedBudgetPct = 5.0
)

$ErrorActionPreference = 'Continue'

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

# 路径一律取绝对值, 并给 Start-Process 显式指定工作目录。
#
# Set-Location 只改 PowerShell 的当前位置, 不改进程的工作目录, 而
# Start-Process 用的是后者 —— 相对路径于是按"启动这个 PowerShell 的那个
# 进程的目录"解析。脚本从命令行直接跑时两者恰好一致, 从 verify.ps1 里
# 嵌套调用时就未必, 而症状极具误导性: 子进程退出码 0, 日志却还是上一步
# 留下的陈旧文件, 报出来是"日志中没有基准结果"。
$Exe = Join-Path $RootDir 'Binaries\Development\Win64\LimxLaunch.exe'
$Log = Join-Path $RootDir 'Logs\LimxEngine.log'

if (-not (Test-Path $Exe)) {
    Write-Host "错误: 未找到 $Exe，请先构建" -ForegroundColor Red
    exit 1
}

# 五种配置 —— 名称与开关一一对应
#
# 最后一项在完整开关的基础上再加环境光照。它与第四项只差一个 --hdri,
# 因此两者的差值就是 IBL 的逐帧成本 —— 那是三次立方体贴图采样加一次
# 查找表采样, 应当只有零点几毫秒。一旦这个差值明显变大, 说明预滤波
# 的 mip 选取或采样器配置出了问题, 而那种退化在画面上几乎看不出来。
$Configurations = @(
    @{ Name = '基线 (剔除关 排序关)'; Args = '--no-cull --no-sort' },
    @{ Name = '仅剔除'.PadRight(0);   Args = '--no-sort' },
    @{ Name = '仅排序';               Args = '--no-cull' },
    @{ Name = '剔除 + 排序';          Args = '' },
    @{ Name = '剔除 + 排序 + IBL';    Args = "--hdri $HdriPath" }
)

$Results = @()
$Failed  = $false

Write-Host ''
Write-Host ('=' * 78)
Write-Host "  Limx Engine — 渲染吞吐基准  (网格 ${Grid}x${Grid}, 采样 $Frames 帧, 预热 $Warmup 帧)"
Write-Host ('=' * 78)

foreach ($config in $Configurations) {
    # HDRI 是下载来的大文件, 不入库 —— 缺了就跳过那一项而非整体失败
    if ($config.Args -like '*--hdri*' -and -not (Test-Path $HdriPath)) {
        Write-Host ''
        Write-Host "  跳过: $($config.Name) — 未找到 $HdriPath" -ForegroundColor Yellow
        continue
    }

    Remove-Item $Log -ErrorAction SilentlyContinue

    $argumentList = "--grid $Grid --frames $Frames --warmup $Warmup $($config.Args)"

    Write-Host ''
    Write-Host "  运行: $($config.Name)" -ForegroundColor Cyan

    $process = Start-Process -FilePath $Exe -ArgumentList $argumentList `
        -WorkingDirectory $RootDir -PassThru
    $process.WaitForExit(300000) | Out-Null

    if (-not $process.HasExited) {
        $process.Kill()
        Write-Host '    超时' -ForegroundColor Red
        $Failed = $true
        continue
    }

    # 先看退出码再看日志。
    #
    # 反过来的话, 进程根本没跑起来时读到的是上一次留下的陈旧日志, 报出
    # 来的是"日志里没有结果" —— 那句话把人引向日志格式, 而真正的原因是
    # 进程压根没运行。
    if ($process.ExitCode -ne 0) {
        Write-Host "    进程退出码 $($process.ExitCode)" -ForegroundColor Red
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

    # GPU 侧
    $gpuLine = (Select-String -Path $Log -Pattern '\[基准\] GPU 整帧').Line

    $gpuMs        = if ($gpuLine -match 'GPU 整帧 ([\d.]+) ms')  { [double]$Matches[1] } else { 0 }
    $unaccountedPct = if ($gpuLine -match '未埋点 [\d.]+ ms \(([\d.]+)%') { [double]$Matches[1] } else { -1 }

    # 逐 Pass 明细 —— 打印出来供人看, 不参与判定
    $passLines = (Select-String -Path $Log -Pattern '\[基准\] GPU Pass').Line

    $Results += [PSCustomObject]@{
        配置       = $config.Name
        可见批次   = $visible
        材质切换   = $switches
        CPUms      = [math]::Round($avgMs, 2)
        GPUms      = [math]::Round($gpuMs, 2)
        瓶颈       = if ($gpuMs -gt 0 -and $avgMs -gt 0) {
                         if ($gpuMs -gt $avgMs * 0.8) { 'GPU' } else { 'CPU' }
                     } else { '?' }
        帧率       = if ($avgMs -gt 0) { [math]::Round(1000.0 / $avgMs, 1) } else { 0 }
    }

    Write-Host "    CPU $([math]::Round($avgMs,2)) ms | GPU $([math]::Round($gpuMs,2)) ms  可见 $visible  材质切换 $switches" -ForegroundColor Green

    foreach ($p in $passLines) {
        if ($p -match 'GPU Pass (.+?) — ([\d.]+) ms \(([\d.]+)%') {
            Write-Host ("      {0,-18} {1,7:N3} ms  {2,5:N1}%" -f $Matches[1], [double]$Matches[2], [double]$Matches[3])
        }
    }

    # 未埋点比例 —— 新增 Pass 忘了计时时这里会跳起来。
    #
    # 判定放在这里而不是只打印: 漏埋的表现是某个 Pass 的时间凭空消失,
    # 不报错、不崩溃, 而逐 Pass 表看起来依然完整。
    if ($unaccountedPct -lt 0) {
        # 取不到就是失败, 不是警告。
        #
        # 这一项存在的意义是"抓住某个 Pass 漏了计时"。而 GPU 计时整个取不
        # 到时, 它连一个数都没有 —— 那是比"某个 Pass 漏埋"更严重的情况,
        # 却被写成了黄字警告放行。也就是说这道闸门在最该拦的时候自己关掉
        # 了自己。
        #
        # 真实触发路径不止一条: 查询池创建失败、设备不支持时间戳、日志格式
        # 改动导致正则不匹配。任何一条都会让整个 GPU 侧判定静默失效。
        Write-Host '      未取到 GPU 计时 — 无法判定是否有 Pass 漏埋' -ForegroundColor Red
        $Failed = $true
    }
    elseif ($unaccountedPct -gt $UnaccountedBudgetPct) {
        Write-Host ("      未埋点 {0:N1}% 超出上限 {1:N1}% — 可能有 Pass 漏了计时" -f `
            $unaccountedPct, $UnaccountedBudgetPct) -ForegroundColor Red
        $Failed = $true
    }
    else {
        Write-Host ("      未埋点 {0:N1}%" -f $unaccountedPct) -ForegroundColor DarkGray
    }
}

Write-Host ''
Write-Host ('=' * 78)
Write-Host '  汇总'
Write-Host ('=' * 78)

$Results | Format-Table -AutoSize

# 以基线为分母给出加速比 —— 绝对毫秒数依赖具体硬件, 比值才是可迁移的结论
#
# 条数不写死: 加了第五个配置 (IBL) 之后这里曾经还写着 -eq 4, 结果只要
# HDRI 存在整段加速比就静悄悄不打印了。
if ($Results.Count -ge 2 -and $Results[0].CPUms -gt 0) {
    $baseline = $Results[0].CPUms

    Write-Host '  相对基线的加速比 (CPU 帧时):'
    foreach ($row in $Results[1..($Results.Count - 1)]) {
        if ($row.CPUms -gt 0) {
            $speedup = [math]::Round($baseline / $row.CPUms, 2)
            Write-Host "    $($row.配置): ${speedup}x"
        }
    }
    Write-Host ''

    # 明确标注这个加速比是 CPU 侧的。
    #
    # 上面每一行的"瓶颈"列若为 CPU, 就说明 GPU 一直在等 —— 那时这个倍数
    # 衡量的是提交端的开销下降, 与着色器快慢无关。把它说成"渲染变快了"
    # 是这一列存在的理由。
    $gpuBound = @($Results | Where-Object { $_.瓶颈 -eq 'GPU' }).Count
    Write-Host "  $gpuBound/$($Results.Count) 个配置为 GPU 受限"
    Write-Host ''
}

# ============================================================
#  第二段 — 资产导入基准 (回归哨兵)
# ============================================================
#
# 为什么要单独量导入: 它是一次性成本, 不出现在任何逐帧数字里, 因此
# 逐帧基准再绿也盖不住它的退化。第四周把 Sponza 的导入从 1.9 s 压到
# 0.84 s, 而其中最大的一笔 (图元装配 283 ms → 26 ms) 是一个反复
# Reserve 导致的重分配问题 —— 那类退化极容易被无意改回去, 且不会
# 让任何测试变红。所以给它一个能盯住的数字。

if (-not $SkipImport) {

    if (-not (Test-Path $ImportScene)) {
        Write-Host ''
        Write-Host "  跳过导入基准 — 未找到 $ImportScene" -ForegroundColor Yellow
    }
    else {
        Write-Host ('=' * 78)
        Write-Host "  资产导入基准  ($ImportScene, $ImportRuns 次取中位数)"
        Write-Host ('=' * 78)

        $importRows = @()

        for ($i = 1; $i -le $ImportRuns; $i++) {
            # 两次运行之间必须留间隔。
            #
            # 这不是保守起见: 背靠背跑的时候, 上一次进程的退出 (释放几百
            # MiB 显存、销毁设备) 会与下一次的启动重叠, 把上传那一项从
            # 100 ms 推到 1000 ms 以上。这个假象在第四周骗过两次, 每次
            # 都得出了"某项优化引起退化"的错误结论。
            if ($i -gt 1) { Start-Sleep -Seconds 5 }

            Remove-Item $Log -ErrorAction SilentlyContinue

            $p = Start-Process -FilePath $Exe -PassThru `
                -WorkingDirectory $RootDir -ArgumentList `
                "--scene $ImportScene --frames 3 --warmup 1"
            $p.WaitForExit(300000) | Out-Null

            if (-not $p.HasExited) {
                $p.Kill()
                Write-Host "    第 $i 次: 超时" -ForegroundColor Red
                $Failed = $true
                continue
            }

            # 退出码优先 —— 理由同渲染段
            if ($p.ExitCode -ne 0) {
                Write-Host "    第 $i 次: 进程退出码 $($p.ExitCode)" -ForegroundColor Red
                $Failed = $true
                continue
            }

            if (-not (Test-Path $Log)) {
                Write-Host "    第 $i 次: 未产生日志" -ForegroundColor Red
                $Failed = $true
                continue
            }

            $totalLine = (Select-String -Path $Log -Pattern '资产导入完成').Line
            $partLine  = (Select-String -Path $Log -Pattern '分项 — 解析').Line

            if (-not $totalLine -or -not $partLine) {
                Write-Host "    第 $i 次: 日志中没有导入结果" -ForegroundColor Red
                $Failed = $true
                continue
            }

            $importRows += [PSCustomObject]@{
                总计 = if ($totalLine -match '耗时 ([\d.]+) ms')     { [double]$Matches[1] } else { 0 }
                解析 = if ($partLine  -match '解析 ([\d.]+) ms')     { [double]$Matches[1] } else { 0 }
                解码 = if ($partLine  -match '纹理解码 ([\d.]+) ms') { [double]$Matches[1] } else { 0 }
                上传 = if ($partLine  -match '纹理上传 ([\d.]+) ms') { [double]$Matches[1] } else { 0 }
            }

            Write-Host ("    第 {0} 次: 总 {1,7:N1} ms | 解析 {2,6:N1} | 解码 {3,6:N1} | 上传 {4,6:N1}" -f `
                $i, $importRows[-1].总计, $importRows[-1].解析,
                $importRows[-1].解码, $importRows[-1].上传)
        }

        if ($importRows.Count -eq 0) {
            Write-Host '  导入基准无有效样本' -ForegroundColor Red
            $Failed = $true
        }
        else {
            # 取中位数而非平均: 偶发的一次调度抖动能把平均值拉高几百毫秒,
            # 而中位数不受单个离群点影响。
            function Get-Median([double[]]$Values) {
                $sorted = $Values | Sort-Object
                return $sorted[[int]([math]::Floor($sorted.Count / 2))]
            }

            $medTotal  = Get-Median ($importRows.总计)
            $medParse  = Get-Median ($importRows.解析)
            $medDecode = Get-Median ($importRows.解码)
            $medUpload = Get-Median ($importRows.上传)

            Write-Host ''
            Write-Host ("  中位数: 总 {0:N1} ms | 解析 {1:N1} | 解码 {2:N1} | 上传 {3:N1}" -f `
                $medTotal, $medParse, $medDecode, $medUpload) -ForegroundColor Green

            # ---- 判定 ----
            #
            # 基线是 2026-08-30 在一台 16 逻辑核的 Windows 机器上测得的
            # 中位数。绝对毫秒随硬件浮动, 所以预算取三倍而非一倍 —— 目标
            # 是抓住"数量级退化" (那次 Reserve 问题是 8 倍), 而不是在慢
            # 一点的机器上误报。机器差异更大时用 -ImportBudgetMs 覆盖,
            # 传 0 则只报告不判定。
            $ImportBaseline = 840.0

            $budget = if ($ImportBudgetMs -gt 0) { $ImportBudgetMs }
                      else { $ImportBaseline * 3.0 }

            if ($medTotal -gt $budget) {
                Write-Host ("  导入超出预算: {0:N1} ms > {1:N1} ms" -f $medTotal, $budget) `
                    -ForegroundColor Red
                Write-Host '  (若这台机器本就慢于基线, 用 -ImportBudgetMs 调整预算)'
                $Failed = $true
            }
            else {
                Write-Host ("  预算 {0:N1} ms — 通过" -f $budget) -ForegroundColor Green
            }
        }

        Write-Host ''
    }
}

if ($Failed) { exit 1 }
exit 0
