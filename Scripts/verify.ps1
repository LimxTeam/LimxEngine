# ============================================================
# verify.ps1 — 本地全量验证
#
# 与 .github/workflows/ci.yml 执行等价的检查，用于在提交前
# 本地复现 CI 结论。本脚本是唯一经过实测的验证入口。
#
# ── 两层 ────────────────────────────────────────────────────
#
# 步骤分成两层, 由 Invoke-Step 的 -RequiresGpu 标记区分:
#
#   无 GPU 层  工具链、着色器编译、源码规则、C++ 构建、单元测试。
#              这些只需要 CPU 与 Vulkan SDK (头文件与 glslang),
#              普通 CI 运行器就能跑。
#
#   需 GPU 层  显存回收自检、IBL 白炉自检、导入基准。
#              它们要创建真实的 VkDevice 并提交命令 —— 云端运行器
#              通常只有软件光栅化器(甚至没有), 跑不了。
#
# 分层的意义在于: 没有 GPU 的环境里, 无 GPU 层仍然是**全绿即可信**
# 的结论, 而不是"有几步失败但据说没关系"。后者用不了几次就会被当成
# 噪声忽略, 于是整套验证一起失效。
#
# 用法:
#   pwsh Scripts/verify.ps1              # 全部 (本机有 GPU 时)
#   pwsh Scripts/verify.ps1 -SkipGpu     # 只跑无 GPU 层
#   pwsh Scripts/verify.ps1 -OnlyGpu     # 只跑需 GPU 层
#   pwsh Scripts/verify.ps1 -Rebuild     # 强制全量重建
#   pwsh Scripts/verify.ps1 -SkipTools   # 跳过 Rust 工具链构建
#
# 退出码: 0 全部通过 | 1 存在失败
# ============================================================

[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$SkipTools,

    # 只跑无 GPU 层 —— CI 的普通运行器用这个
    [switch]$SkipGpu,

    # 只跑需 GPU 层 —— 有真实显卡的机器上补跑
    [switch]$OnlyGpu
)

if ($SkipGpu -and $OnlyGpu) {
    Write-Host '错误: -SkipGpu 与 -OnlyGpu 互斥' -ForegroundColor Red
    exit 1
}

$ErrorActionPreference = 'Continue'

# ------------------------------------------------------------
# 定位引擎根目录 — 脚本位于 <root>/Scripts
# ------------------------------------------------------------

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

$Script:StepIndex = 0
$Script:Failures = @()
$Script:Skipped  = @()

function Write-Header {
    param([string]$Text)
    Write-Host ''
    Write-Host ('=' * 72)
    Write-Host "  $Text"
    Write-Host ('=' * 72)
}

# 执行一个验证步骤：失败只记录不中断，使一次运行暴露全部问题
#
# -RequiresGpu 标记那些必须有真实显卡才能跑的步骤。层的归属写在步骤
# 本身而不是脚本外面, 是为了让 verify.ps1 与 ci.yml 不会各记一份而
# 悄悄漂移 —— 加一个新步骤时, 它属于哪一层是当场就要回答的问题。
function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Action,
        [switch]$RequiresGpu
    )

    if ($RequiresGpu -and $SkipGpu) {
        $Script:Skipped += "$Name (需 GPU)"
        return
    }

    if (-not $RequiresGpu -and $OnlyGpu) {
        return
    }

    $Script:StepIndex++
    Write-Host ''
    Write-Host "[$Script:StepIndex] $Name" -ForegroundColor Cyan

    # 先清空 —— $LASTEXITCODE 只由本机可执行文件写入, 不清空的话读到的
    # 可能是**上一步**留下的值。
    #
    # 而"清空之后仍然是 null"恰恰意味着这一步根本没有跑起来任何程序:
    # 可执行文件不存在时 PowerShell 抛的是非终止错误, 在
    # $ErrorActionPreference='Continue' 下被 2>&1 吞进 $output, 而
    # $LASTEXITCODE 一动不动。
    #
    # 实测: 把本脚本放进一个完全没有 Binaries/ 目录的树里跑 -SkipTools,
    # 十六步全绿、退出 0。而 Binaries/ 是 gitignore 的 —— 也就是说任何
    # 新克隆或 git clean 之后的树都会这样。
    $global:LASTEXITCODE = $null

    $output = & $Action 2>&1
    $exitCode = $LASTEXITCODE

    if ($null -eq $exitCode)
    {
        Write-Host '    失败 (命令未执行 — 可执行文件缺失?)' -ForegroundColor Red
        $output | Select-Object -Last 25 | ForEach-Object { Write-Host "    $_" }
        $Script:Failures += $Name
        return
    }

    if ($exitCode -ne 0) {
        Write-Host "    失败 (退出码 $exitCode)" -ForegroundColor Red
        $output | Select-Object -Last 25 | ForEach-Object { Write-Host "    $_" }
        $Script:Failures += $Name
    }
    else {
        Write-Host "    通过" -ForegroundColor Green
    }
}

# ------------------------------------------------------------
# 运行引擎可执行文件
#
# LimxLaunch.exe 是 GUI 子系统程序 (PE Subsystem = 2)。PowerShell 的调用
# 运算符 & 对 GUI 程序**不等待也不回填 $LASTEXITCODE** —— 它启动进程后
# 立刻返回, $LASTEXITCODE 保持未设置。
#
# 后果是: Invoke-Step 把未设置当成 0, 于是每一个跑 LimxLaunch 的步骤都
# 无条件打印"通过"。显存回收自检与白炉自检因此从来没有真正验证过任何
# 东西 —— 哪怕进程立刻崩溃, 这一步照样是绿的。
#
# 更隐蔽的连带后果: 进程还在后台跑, 下一步就开始了。白炉自检持有日志
# 文件 (FILE_SHARE_READ), 紧接着的基准脚本删不掉它, 于是基准读到的是
# 上一步留下的陈旧日志, 报"日志中没有结果"。
#
# 单元测试与 lbt 都是控制台程序 (Subsystem = 3), & 对它们的行为是对的,
# 所以这个坑只在引擎这几步上。
function Invoke-Engine {
    param([string]$Arguments)

    $stdout = Join-Path $env:TEMP 'limx_verify_stdout.txt'
    $stderr = Join-Path $env:TEMP 'limx_verify_stderr.txt'

    # 可执行文件不存在时 Start-Process 抛非终止错误, $process 留在 $null,
    # 于是 $process.ExitCode 也是 $null —— 回填给 $LASTEXITCODE 之后就成了
    # "未设置", 而 Invoke-Step 曾把未设置当成 0。
    #
    # 这是上面那个 GUI 子系统 bug 的残留分支: -Wait -PassThru 解决了"不等待",
    # 没有解决"根本没启动"。
    if (-not (Test-Path $EngineExe))
    {
        Write-Host "    引擎可执行文件不存在: $EngineExe" -ForegroundColor Red
        $global:LASTEXITCODE = 127
        return
    }

    $process = Start-Process -FilePath $EngineExe -ArgumentList $Arguments `
        -WorkingDirectory $RootDir -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr

    if ($null -eq $process)
    {
        Write-Host '    进程未能启动' -ForegroundColor Red
        $global:LASTEXITCODE = 127
        return
    }

    foreach ($file in @($stdout, $stderr)) {
        if (Test-Path $file) {
            Get-Content $file
            Remove-Item $file -ErrorAction SilentlyContinue
        }
    }

    # Invoke-Step 读的是 $LASTEXITCODE, 这里显式回填
    $global:LASTEXITCODE = $process.ExitCode
}

$EngineExe = Join-Path $RootDir 'Binaries\Development\Win64\LimxLaunch.exe'

Write-Header 'Limx Engine — 本地验证'
Write-Host "  根目录: $RootDir"
Write-Host "  VULKAN_SDK: $env:VULKAN_SDK"

if (-not $env:VULKAN_SDK) {
    Write-Host '  错误: 未设置 VULKAN_SDK 环境变量' -ForegroundColor Red
    exit 1
}

# ------------------------------------------------------------
# 1. Rust 工具链
# ------------------------------------------------------------

if (-not $SkipTools) {
    Write-Header 'Rust 工具链'

    Invoke-Step 'cargo check' {
        Push-Location Programs
        cargo check --workspace --all-targets
        Pop-Location
    }

    Invoke-Step 'cargo test' {
        Push-Location Programs
        cargo test --workspace
        Pop-Location
    }

    Invoke-Step 'cargo build --release' {
        Push-Location Programs
        cargo build --workspace --release
        Pop-Location
    }

    Invoke-Step '发布工具到 Binaries/Tools' {
        foreach ($tool in @('lbt', 'lht', 'lsc', 'lat')) {
            $source = "Programs/target/release/$tool.exe"
            if (Test-Path $source) {
                Copy-Item $source "Binaries/Tools/$tool.exe" -Force
            }
        }
        $global:LASTEXITCODE = 0
    }
}

# ------------------------------------------------------------
# 2. 着色器
# ------------------------------------------------------------

Write-Header '着色器'

Invoke-Step '编译 SPIR-V (Vulkan 1.4)' {
    .\Binaries\Tools\lsc.exe compile-all -s Shaders -o Binaries/Shaders --vulkan-version 1.4 -O
}

# 严格校验 —— 编译通过不等于没有警告。
#
# 这一步此前只在 ci.yml 里有, 本地从不执行, 而它的参数用法本身就是错的
# (validate 当时只接受单个文件, 传目录直接 os error 5)。CI 的着色器作业
# 因此一直是红的, 而本地十七步全绿 —— 没人会去看一个长期红着的 CI。
Invoke-Step '着色器严格校验' {
    .\Binaries\Tools\lsc.exe validate -s Shaders --vulkan-version 1.4 --strict
}

# 引擎的 FMatrix 是行主序; GLSL 默认按列主序解读 uniform 里的 mat, 不加
# row_major 等于把矩阵整体转置。后果是所有顶点被变换到裁剪体外 —— 画面只剩
# 清屏色, 而校验层与编译器都不会报错。这一步把该约定变成可检查的规则。
Invoke-Step '矩阵存储序约定 (uniform/push_constant 必须 row_major)' {
    $offenders = New-Object System.Collections.ArrayList

    # 用 foreach 语句而非 ForEach-Object —— 后者的脚本块在子作用域中执行,
    # 其中的 "+=" 会先读外层变量再在本地新建同名变量, 结果是外层永远为空,
    # 检查恒为"通过"。这类静默失效的检查比没有检查更危险。
    $shaderFiles = Get-ChildItem -Path Shaders -Recurse -Include *.vert, *.frag, *.comp, *.geom

    foreach ($file in $shaderFiles) {
        $lines = Get-Content $file.FullName

        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i]

            # 只看带块体的 uniform / push_constant 声明行;
            # 独立 uniform (sampler 等) 不含矩阵成员
            $isBlock = ($line -match 'layout\s*\(.*\)\s*uniform\b') -or
                       ($line -match 'layout\s*\(.*push_constant.*\)')

            if (-not $isBlock) { continue }
            if ($line -notmatch '\{\s*$') { continue }

            $hasMatrix = $false
            for ($j = $i + 1; $j -lt $lines.Count; $j++) {
                if ($lines[$j] -match '^\s*\}') { break }
                if ($lines[$j] -match '^\s*mat[234]') { $hasMatrix = $true }
            }

            if ($hasMatrix -and $line -notmatch 'row_major') {
                [void]$offenders.Add(("{0}:{1}: {2}" -f $file.Name, ($i + 1), $line.Trim()))
            }
        }
    }

    if ($offenders.Count -gt 0) {
        Write-Host '    含矩阵成员的块缺少 row_major 限定符:' -ForegroundColor Red
        foreach ($offender in $offenders) { Write-Host "      $offender" }
        $global:LASTEXITCODE = 1
    }
    else {
        $global:LASTEXITCODE = 0
    }
}

# C++ 侧的 static_assert 与 SPIR-V 反射必须对上。
#
# 两侧各自都有保障: C++ 的 static_assert 钉住结构体大小, 着色器统一从
# view_common.h 取声明。缺的是**跨语言那一跳** —— 有人改了 C++ 结构体
# 却没改 GLSL 头 (或反过来) 时, 两边各自都编得过, 只有画面会错, 而且
# 错法是矩阵整体错位、全黑无报错。
#
# 期望值从 static_assert 里读, 不写死在这个脚本里 —— 写死的话它自己
# 就会与 C++ 侧漂移, 而漂移之后它照样报"通过"。
Invoke-Step 'C++ 结构体与 SPIR-V 布局一致' {
    powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/verify-shader-layout.ps1
}

# ------------------------------------------------------------
# 3. 源码规则
# ------------------------------------------------------------

Write-Header '源码规则'

Invoke-Step '零 STL / 零 CRT / 无裸 new-delete 检查' {
    .\Binaries\Tools\lbt.exe check --source-dir Source
}

# --strict 必须与 ci.yml 保持一致。
#
# 此前本地不带 --strict 而 CI 带, 于是本地 16 步全绿而 CI 的第二步会直接
# 失败 —— 这类分歧在真正触发 CI 之前完全看不见。凡是两边都要跑的命令,
# 参数就必须逐字相同。
Invoke-Step '模块配置校验' {
    .\Binaries\Tools\lbt.exe validate -s Source --strict
}

# 校验本脚本与 ci.yml 没有分歧 —— 不需要 GPU, 也不需要网络。
#
# 放在这里而不是只在 CI 里跑: 分歧是在本地改动时产生的, 越早报越好。
# 等到 CI 触发才发现, 一个来回要几分钟。
Invoke-Step 'CI 等价性' {
    powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/ci-parity.ps1
}

# ------------------------------------------------------------
# 4. 构建
# ------------------------------------------------------------

Write-Header 'C++ 构建'

Invoke-Step $(if ($Rebuild) { '全量重建' } else { '增量构建' }) {
    if ($Rebuild) {
        .\Binaries\Tools\lbt.exe build -s Source -c development --skip-shaders --rebuild
    }
    else {
        .\Binaries\Tools\lbt.exe build -s Source -c development --skip-shaders
    }
}

# ------------------------------------------------------------
# 5. 单元测试
# ------------------------------------------------------------

Write-Header '单元测试'

Invoke-Step 'CoreTests' {
    .\Binaries\Development\Win64\LimxCoreTests.exe --min-cases 406
}

Invoke-Step 'RHITests' {
    .\Binaries\Development\Win64\LimxRHITests.exe --min-cases 58
}

Invoke-Step 'AssetTests' {
    .\Binaries\Development\Win64\LimxAssetTests.exe --min-cases 200
}

# 引擎层测试逐套件跑 —— 与 ci.yml 保持一致。
#
# CI 那边拆开是因为作业日志需要 admin 权限才能读, 而步骤成败是公开的;
# 本地拆开则是为了两边命令逐字相同。
#
# 展开写而不是 foreach 循环: 循环里命令行是 "--suite $suite", 文本上与
# ci.yml 的 "--suite BlendMode" 对不上, ci-parity.ps1 会把五条都报成
# "CI 独有"。这条检查抓到过我自己 —— 它就该抓得到。
# 启动探针 —— 与 ci.yml 保持一致, 见那边的说明
Invoke-Step '引擎层测试 · 启动探针' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --list
}

Invoke-Step '引擎层测试 · BlendMode' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite BlendMode
}

Invoke-Step '引擎层测试 · CascadeSplit' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite CascadeSplit
}

Invoke-Step '引擎层测试 · GeometryWinding' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite GeometryWinding
}

Invoke-Step '引擎层测试 · LSpatialTrait' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite LSpatialTrait
}

Invoke-Step '引擎层测试 · TranslucentSort' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite TranslucentSort
}

Invoke-Step '引擎层测试 · ClusterGrid' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite ClusterGrid
}

Invoke-Step '引擎层测试 · CompressedTextureFormat' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite CompressedTextureFormat
}

Invoke-Step '引擎层测试 · Octahedral' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite Octahedral
}

Invoke-Step '引擎层测试 · ShadowAtlas' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --suite ShadowAtlas
}

# 用例数下限 —— 防的是"套件悄悄消失"。
#
# 上面每一条都是手写的套件名。新加一个套件而忘了补一行, 它就永远不会被执行,
# 而"没执行"与"全过"在退出码上完全一样。ClusterGrid、Octahedral、ShadowAtlas
# 三个就是这么漏掉的 —— 注册了整整一个周期, 一次都没跑过。
#
# 判据放在二进制里而不是脚本里: 脚本要判断就得解析输出文本, 而解析失败会
# 退化成"没发现问题"。
Invoke-Step '引擎层测试 · 用例数下限' {
    .\Binaries\Development\Win64\LimxEngineTests.exe --min-cases 85
}

# 显存回收自检 —— 需要真实 GPU, 因此放在单元测试之后单独一步。
# 它验证的是"引用计数是否真的接通", 而这一点靠单元测试测不到:
# 泄漏只在 GPU 资源实际分配与释放时才成立。
Invoke-Step '显存回收自检' -RequiresGpu {
    Invoke-Engine '--scene Content/TestScene/testscene.obj --reload-test'
}

# IBL 白炉自检 —— 同样需要真实 GPU。
# 它用一个各方向辐射度恒为 1 的合成环境跑完整条 IBL 预计算链, 断言三条
# 解析可知的性质: 辐照度处处为 1、预滤波每一级都为 1、BRDF 表的 A+B 不
# 超过 1。这三条不依赖任何具体 HDRI, 因此不需要外部资产。
#
# 这条链上的错误几乎全是"看着差不多"的: 卷积系数差一个 π、mip 与粗糙度
# 的映射错位、归一化除错了分母 —— 画面上一律只表现为"环境光有点不对"。
Invoke-Step 'IBL 白炉自检' -RequiresGpu {
    Invoke-Engine '--furnace-check'
}

# G-Buffer 自检 —— 需要真实 GPU。
#
# 速度矢量的正确性没有任何 CPU 侧的办法可验: 它是"本帧裁剪坐标减上一帧
# 裁剪坐标", 而两者都只存在于 GPU 上。错法又特别安静 —— 上一帧矩阵保存
# 晚了一步, 速度就恒为零; 画面完全正常, 只有 TAA 接上以后才表现为拖影。
#
# 三阶段: 静止建立基准 → 只转偏航角 (速度必须逐像素等于 CPU 预测值) →
# 保持不动 (速度必须精确为零)。顺序不能换, 详见 RunGBufferChecks 的注释。
Invoke-Step 'G-Buffer 自检 (法线编码 + 速度矢量)' -RequiresGpu {
    Invoke-Engine '--frames 20 --warmup 5 --gbuffer-check'
}

# 同一套自检再跑一遍, 这次开抖动。
#
# 两次都要跑, 因为两边验的不是同一件事: 关抖动时验的是速度本身;
# 开抖动时验的是"速度里没有混进抖动" —— 相机静止下速度依然精确为零。
# 只跑其中一个的话, 另一边的失效方式完全没有覆盖。
#
# 开抖动那次还多验一条正向对照: 相机不变的两帧覆盖掩码必须有差异。
# 没有它, "--taa 是个空开关"会让上面每一项都完美通过。
Invoke-Step 'G-Buffer 自检 (开 TAA 抖动)' -RequiresGpu {
    Invoke-Engine '--frames 20 --warmup 5 --gbuffer-check --taa'
}

# 泛光自检 —— 需要真实 GPU。
#
# 判据是**点扩散函数**: 一个接近点的光源经过降采样-升采样链之后应当得到径向
# 对称、单调衰减、明显扩散的光晕。
#
# 为什么非要量它: 这条链最典型的缺陷是半纹素偏移 —— 核的采样坐标算错半个
# 纹素, 每一级都把图像往同一个方向挪一点, 六级累积下来光晕整体偏离光源好几
# 个像素。而画面上那仍然是"一团发光的东西", 没人看得出来。
#
# 光源必须接近一个点。第一版用了 0.4 单位的方块 (半分辨率下 60 像素宽), 而
# 那让整套判据失效: 大光源的"泛光"主要由它自己的形状决定 —— 实测把降采样
# 退化成单点采样、或整条升采样链跳过, 判据仍然全绿。
Invoke-Step '泛光自检 (点扩散函数)' -RequiresGpu {
    Invoke-Engine '--bloom-scene --bloom --frames 20 --warmup 5 --bloom-check'
}

# GTAO 自检 —— 需要真实 GPU。
#
# 90 度凹角处余弦加权的可见度解析值是 0.5, 但那是"搜索半径 → ∞"的极限。
# 判据因此不是"等于 0.5", 而是**随半径增大朝 0.5 单调收敛** —— 那是这个算法
# 的物理签名, 而写错的实现 (法线没转视空间、角度约定反了、地平线取错方向)
# 不会有: 它们要么恒为 1, 要么与半径无关, 要么往反方向走。
#
# 用专门的墙角场景 (两个 20x20 平面成直角)。多一个物体解析值就不再成立,
# 而"解析值不成立"与"实现算错了"在结果上无法区分。
Invoke-Step 'GTAO 自检 (墙角的解析收敛)' -RequiresGpu {
    Invoke-Engine '--corner-scene --gtao --frames 20 --warmup 5 --ao-check'
}

# 半分辨率 GTAO 也要过同一条解析判据 —— 它是个近似, 但必须仍然收敛到 0.5。
Invoke-Step 'GTAO 自检 (半分辨率下的解析收敛)' -RequiresGpu {
    Invoke-Engine '--corner-scene --gtao --gtao-half --frames 20 --warmup 5 --ao-check'
}

# 半分辨率与全分辨率的逐像素比对。
#
# 三条判据里最要紧的是"平均差**不能为零**": 一个把 --gtao-half 忽略掉的实现
# 会得到两张完全相同的图, 而"差异要小"那一条对它满分通过 —— 于是"没生效"与
# "完美无损"在判据上无法区分。
#
# 容差 0.025 是从墙角场景量出来的 (正确实现给 0.014394), 与场景有关: 阴影
# 场景上正确实现就有 0.0373。所以这一条只在墙角场景上跑。
Invoke-Step 'GTAO 自检 (半分辨率与全分辨率逐像素比对)' -RequiresGpu {
    Invoke-Engine '--corner-scene --gtao --frames 20 --warmup 5 --ao-half-check'
}

# GPU 驱动绘制自检 —— 需要真实 GPU。
#
# 判据是"GPU 驱动路径与逐物体绘制画出来的东西**完全一样**"。这一条比"看起来
# 对"强得多: GPU 驱动错了的表现高度趋同 —— 剔除多剔了几个 (少几个物体)、
# firstInstance 没接上 (整个场景挤在一个变换上)、分组的起点算错 (某一组画成了
# 另一组的几何)。三种都不崩、不报错。
#
# 能这样比的前提是两条路径走的是**同一份着色器代码**: 顶点着色器只有一条路径,
# 总是从 set 3 取模型矩阵; 逐物体绘制那条只是把物体下标经 firstInstance 传进去。
#
# 另外两条判据防的是"判据本身失效": 可见数必须真的少于总数 (否则一个什么都
# 不做的剔除也能通过), 组数必须真的少于物体数 (否则分组没起作用而画面依然正确)。
#
# 用 --grid 24 而不是演示场景: 要有物体在视锥外, 剔除才有东西可剔。
Invoke-Step 'GPU 驱动绘制自检 (与逐物体绘制逐像素比对)' -RequiresGpu {
    Invoke-Engine '--grid 24 --frames 20 --warmup 5 --gpu-driven-check'
}

# 聚光灯阴影自检 —— 需要真实 GPU。
#
# 阴影这类缺陷的表现高度趋同: 图集块偏移算错、矩阵没转置、绘制的视口与采样的
# UV 不一致、深度偏移过大 —— 在画面上都是"影子位置不对"或"影子没了", 而人的
# 第一反应永远是去调 bias。
#
# 判据因此是**解析的**: 灯在 (0,0,6), 薄板在 z=3, 墙在 z=0, 影子边界由相似
# 三角形唯一确定为 板半宽 × 6/(6-3)。实测与解析值差 0.8%, 而那 0.8% 有确切的
# 来源 (法线偏移把接收点推离墙面 0.027 单位)。
#
# 灯必须在**相机轴上** —— 这样遮挡物在画面上的像一定落在它自己影子之外
# (灯离板子近, 放大率更大), 影子边界才不会被自己的遮挡物挡住。
#
# 两盏灯而非一盏: 只有一块的话, "块偏移算错"会退化成"偏到了图集的空白区",
# 而空白区深度是 1.0 恰好判为无遮挡 —— 与"这盏灯没有影子"在画面上一模一样。
Invoke-Step '聚光灯阴影自检 (相似三角形的解析边界)' -RequiresGpu {
    Invoke-Engine '--shadow-scene --frames 20 --warmup 5 --shadow-check'
}

# 同一套判据, 但用满 64 块。
#
# 两个规模抓的不是同一类东西。两块时它们都落在图集的左上角, 而那一片恰好被
# 上一个 Pass 留下的裁剪矩形罩着 —— "图集 Pass 忘了设裁剪矩形"这类缺陷完全
# 暴露不出来 (实测把那行注释掉, 两块的版本退出码仍是 0)。
#
# 64 块时被测的两盏在第 62/63 块, 纹素 x 是 3072 与 3584, 远在交换链宽度之外,
# 同一个变异立刻被抓住。
Invoke-Step '聚光灯阴影自检 (用满 64 块图集)' -RequiresGpu {
    Invoke-Engine '--shadow-scene --shadow-lights 64 --frames 20 --warmup 5 --shadow-check'
}

# 点光源立方体阴影自检 —— 需要真实 GPU。
#
# 两条判据抓的是不同的东西:
#   影子边界落在相似三角形算出的位置上 —— 验六个面的矩阵、块的连续分配、
#     以及采样的整条链路;
#   影子横跨立方体的面边界时**不能断** —— 验选面的规则与 C++ 侧算矩阵时的
#     编号一致。断开处离两条边界都很远, 边界本身仍然正确, 所以第一条判据
#     发现不了它。
#
# 灯刻意放得离墙很近 (2 单位), 于是墙上 |x| 超过 2 的地方主轴就翻面 ——
# 影子必然跨过面边界。灯放远一点的话可见范围内根本不跨面, 整条选面逻辑
# 就无从判定, 而判据照样全绿。
Invoke-Step '点光源立方体阴影自检 (跨面不断 + 解析边界)' -RequiresGpu {
    Invoke-Engine '--shadow-scene --point-shadow --frames 20 --warmup 5 --shadow-check'
}

# TAA 自检 —— 需要真实 GPU。
#
# TAA 最危险的失效方式是**看起来正常但什么都没做**: 裁剪范围取小了历史每帧
# 都被拉回当前值, 或者重投影全部落在屏幕外历史一律被拒。两种情况下画面都完全
# 正常, 只是锯齿还在 —— 而那要对着屏幕看边缘才发现。
#
# 判据是数值的: 抖动 N 帧的均匀平均就是超采样真值, TAA 的输出必须比其中任何
# 单帧都显著更接近它。一个"什么都没做"的 TAA 输出等于单帧, 这一条直接不成立。
Invoke-Step 'TAA 自检 (与多帧平均比对)' -RequiresGpu {
    Invoke-Engine '--frames 20 --warmup 5 --taa-check'
}

# 分簇光照自检 —— 需要真实 GPU。
#
# 两条判据覆盖的是完全不同的失效方式:
#   --cluster-check    回读簇表, 与 FClusterGrid.h 的 CPU 参照逐簇比对。
#                      验的是簇表本身对不对。
#   --light-cull-check 同一帧、同一个着色器、只翻转分簇开关, 两次画面必须
#                      逐像素一致。验的是片段着色器**用对了**那张表 ——
#                      切片映射、屏幕分块、索引区间的读取。
#
# 簇表全对而片段着色器查错簇时, 前一条全绿。反过来也一样。
Invoke-Step '分簇光照自检 (簇表 + 逐像素等价)' -RequiresGpu {
    Invoke-Engine '--light-grid 12 --frames 20 --warmup 5 --cluster-check --light-cull-check'
}

# 图像回归 —— 需要真实 GPU。
#
# 前面所有的 GPU 自检验的都是**数值性质** (速度为零、法线朝向相机、能量
# 守恒), 没有一条能回答"画面看起来还对不对"。而着色路径最常见的回归恰恰
# 是数值上完全合法、只是结果变了: UBO 字段错位、材质参数读错、某盏光丢了。
#
# 比的不是整张图, 是 32x18 平均池化的签名 (1728 字节, 可提交)。逐像素相等
# 对 GPU 渲染过于严苛 —— 驱动版本、光栅化绑定规则、浮点收缩都会让个别像素
# 差 1。而一个 40x40 的格子平均之后, 那些差异被抹平, 真实的着色改动却不会。
#
# **局限**: 基线是在特定 GPU 与驱动上生成的。换硬件需要重新生成基线, 那
# 一步必须由人确认画面确实正确, 不能自动覆盖 —— 自动覆盖等于这个检查永远
# 通过。生成命令写在 Content/Baselines/README.md 里。
Invoke-Step '图像回归 (演示场景)' -RequiresGpu {
    $shot = Join-Path $env:TEMP 'limx-verify-demo.ppm'

    Invoke-Engine "--frames 20 --warmup 5 --screenshot $shot"

    if ($global:LASTEXITCODE -ne 0)
    {
        return
    }

    powershell -NoProfile -ExecutionPolicy Bypass `
        -File Scripts/image-signature.ps1 `
        -Ppm $shot -Baseline Content/Baselines/demo-scene.sig
}

# 综合场景自检 —— 需要真实 GPU。
#
# 前面每条判据各用一个**最小场景**: 墙角只有两块平面, 泛光只有一个方块,
# 阴影只有一堵墙加一块板。那是刻意的 —— 多一样东西, 解析判据就不再成立。
#
# 但那也留下一个空白: 没有任何一个场景**同时**跑全部子系统。而子系统之间会
# 互相影响 —— 分簇决定哪些光参与着色, 阴影图集的块下标存在光源数据里,
# GPU 驱动的逐物体缓冲区被四个通道共用, TAA 的历史依赖速度矢量。任何一处
# 对不上, 单独的最小场景都发现不了。
#
# 这一条问的因此是另一件事: 不是"这个数对不对", 而是**"这个子系统到底跑没
# 跑"**。每一条都带"够不够判"的元判据 —— 场景里没有对应的东西时直接判失败,
# 而不是悄悄通过。
Invoke-Step '综合场景自检 (每个子系统都留下痕迹)' -RequiresGpu {
    Invoke-Engine '--showcase --gpu-driven --gtao --gtao-half --taa --bloom --clustered --frames 20 --warmup 5 --showcase-check'
}

# 光追加速结构 (GPU 遍历 vs CPU 解析解) —— 需要真实 GPU。
#
# 两套彼此独立的实现撞同一个数: GPU 那套走驱动的 BVH, CPU 那套是引擎自己的
# Moller-Trumbore 逐三角形求交。它们连算法都不是一回事, 所以不存在"照着对方
# 调到一致"这条路。
#
# 判据本身没法看画面 —— 加速结构建错了不会崩、不会报错, 只是所有射线都不命中
# 或者命中错地方, 而那在画面上与"这里本来就没东西"完全一样。
#
# 场景刻意摆成能分辨对错的样子: 有命中也有落空, 五个实例里有一个被另一个完全
# 挡住 (它一次都不该被命中), 有一个绕 Y 轴转过 45 度; 几何体后面还接了一段
# "越界就会读到"的哨兵三角形, 摆在更近处 —— 任何多建了三角形的实现都会让命中
# 距离整体前移 0.5, 而那是容差的五千倍。
#
# 设备不支持光追时这一条判**失败**而不是跳过: 判通过的话它在任何不支持的机器上
# 都是空的, 而换一台机器正是"不支持"最常见的来源。
Invoke-Step '光追加速结构 (GPU 遍历与 CPU 解析解逐条比对)' -RequiresGpu {
    Invoke-Engine '--scene Content/TestScene/testscene.obj --frames 5 --warmup 2 --rt-check'
}

# 图像回归 (综合场景) —— 需要真实 GPU。
#
# 与演示场景那一条同一个机制, 但覆盖面完全不同: 演示场景只有一盏方向光与
# 几个物体, 而这个场景里三种阴影、四类材质、全套后处理同时在跑。着色路径上
# 任何一处"数值合法但结果变了"的回归, 这里比那里更容易撞上。
Invoke-Step '图像回归 (综合场景)' -RequiresGpu {
    $shot = Join-Path $env:TEMP 'limx-verify-showcase.ppm'

    Invoke-Engine "--showcase --gpu-driven --gtao --gtao-half --taa --bloom --clustered --frames 20 --warmup 5 --screenshot $shot"

    if ($global:LASTEXITCODE -ne 0)
    {
        return
    }

    powershell -NoProfile -ExecutionPolicy Bypass `
        -File Scripts/image-signature.ps1 `
        -Ppm $shot -Baseline Content/Baselines/showcase-scene.sig
}

# 交换链重建自检 —— 需要真实 GPU。
#
# OnResize 平时只有窗口缩放时才走, 而自动化里没有任何东西会改窗口尺寸。
# 这条路径因此长期不被覆盖: 第五阶段 Day 4 把 OnResize 的 8 个位置参数
# 重构成结构体之后, "构建通过 + 画面没变"看着像验过了 —— 而画面没变恰恰
# 是因为那段代码根本没执行。
#
# 强制重建 8 次, 靠验证层与逐像素比对确认资源重建正确。
Invoke-Step '交换链重建自检' -RequiresGpu {
    Invoke-Engine '--scene Content/TestScene/testscene.obj --frames 40 --warmup 5 --resize-test 5'
}

# 资产导入回归哨兵 —— 需要真实 GPU (要建设备并上传纹理)。
#
# 导入是一次性成本, 不出现在任何逐帧数字里, 也不会让任何单元测试变红。
# 第四周把 Sponza 的导入从 1.9 s 压到 0.84 s, 其中最大的一笔是一个反复
# Reserve 造成的重分配问题 —— 那类退化极容易被无意改回去, 且悄无声息。
#
# 只跑导入基准那一段 (-SkipImport 的反面), 逐帧基准另有其用途, 不必
# 每次验证都跑三百帧。
Invoke-Step '资产导入回归哨兵' -RequiresGpu {
    powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/benchmark.ps1 `
        -Grid 20 -Frames 20 -Warmup 5 -ImportRuns 3
}

# ------------------------------------------------------------
# 汇总
# ------------------------------------------------------------

Write-Header '验证汇总'

# 跳过的步骤必须列出来。
#
# 无 GPU 层全绿是一个可信的结论, 但它是"该层全绿", 不是"全部通过" ——
# 把这两者混为一谈, 迟早会有人拿着一份没跑过白炉自检的绿色结果去发版。
if ($Script:Skipped.Count -gt 0) {
    Write-Host "  已跳过 $($Script:Skipped.Count) 个步骤 (本次只跑无 GPU 层):" -ForegroundColor Yellow
    foreach ($skipped in $Script:Skipped) {
        Write-Host "    - $skipped" -ForegroundColor Yellow
    }
    Write-Host ''
}

if ($Script:Failures.Count -eq 0) {
    if ($Script:Skipped.Count -gt 0) {
        Write-Host "  无 GPU 层 $Script:StepIndex 个步骤全部通过" -ForegroundColor Green
        Write-Host '  (需 GPU 的步骤尚未验证 — 在有显卡的机器上跑 -OnlyGpu 补齐)' -ForegroundColor Yellow
    }
    else {
        Write-Host "  全部 $Script:StepIndex 个步骤通过" -ForegroundColor Green
    }

    Write-Host ''
    exit 0
}

Write-Host "  $($Script:Failures.Count)/$Script:StepIndex 个步骤失败:" -ForegroundColor Red
foreach ($failure in $Script:Failures) {
    Write-Host "    - $failure" -ForegroundColor Red
}
Write-Host ''
exit 1
