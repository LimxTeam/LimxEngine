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

    # 所有自检一律隐藏窗口。
    #
    # 一轮验证要启动引擎二十多次, 每次弹一个 1280x720 的窗口 —— 整个屏幕
    # 闪个不停。--hidden 只是不 ShowWindow: 交换链、渲染、回读一切照旧,
    # 判据量到的数字与可见时逐位相同 (实测)。
    #
    # 不走"离屏渲染"那条路是刻意的: 那需要一条不带交换链的独立路径, 而那条
    # 路径与真实渲染的差别恰恰是判据最不该引入的东西。
    $Arguments = "$Arguments --hidden"

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
# 光追深度 vs 光栅化深度 (逐像素) —— 需要真实 GPU。
#
# 上一条验的是"加速结构这套机制对不对", 用的是手搭的方片。这一条验的是
# **加速结构里装的是不是屏幕上那个场景**: 真实网格的顶点跨度、子网格的索引
# 偏移、每个物体的变换、以及渲染对象列表与 BLAS 的对应关系。
#
# 光栅器与 BVH 是两条彻底独立的路径 —— 一条按三角形扫描填深度, 一条按射线
# 遍历树。它们对同一个像素给出同一个深度, 才说明两边看的是同一个场景。
#
# 容差按**深度缓冲区在这一像素上能分辨多细**算, 而不是一个固定的相对数 ——
# 透视深度在远处压缩得极厉害, 同一个 float32 最低位在近处代表微米、在远处
# 代表米。固定的相对容差等于对不同近远平面的场景用了完全不同的严格程度,
# 实测差十倍。
#
# 两个场景是互补的:
#   综合场景  33 个物体 + 天空背景 + 蒙版材质, 覆盖 919234 像素, 不符 19 个
#   OBJ 场景  6 个子网格**共用一对缓冲区**, 索引字节偏移 0/768/840/912/984/1080
#             —— 只有它能验到"子网格的索引偏移"这条路径
# 双边上采样在深度不连续处到底有没有起作用 —— 需要真实 GPU。
#
# 这一条曾经被撤下来过: 它挑"深度不连续像素"用的深度回读, 在前向通道的
# StoreOp 修好之前读到的是未初始化显存, 挑出来的 157095 个"不连续像素"
# 大多是垃圾与有效数据的边界。修好之后真实的数字是 7236 个。
#
# 统计量取渗色像素计数 (半分辨率与全分辨率相差超过 0.2 的) 而不是均值 ——
# 渗色本来就是少数像素上的大偏差。实测双边 919 / 纯双线性 1361 / 加权反向
# 1801, 而均值只差 27% 与 77%。
#
# 已知盲点写在代码里: 这条判据分得开"上采样太糊", 分不开"太锐"。
Invoke-Step 'AO 双边上采样 (深度不连续处的渗色)' -RequiresGpu {
    Invoke-Engine '--showcase --gpu-driven --taa --bloom --clustered --gtao --frames 20 --warmup 5 --ao-edge-check'
}

# 光追反射 —— 需要真实 GPU。两个场景是互补的。
#
# 光追反射与屏幕空间反射的差别不在画质, 在**缺口看不看得见**: SSR 反射不出
# 相机背后的、被挡住的、视野外的东西, 而画面上只是"那里没反射", 与"那里本来
# 就不该有反射"长得一样。
#
# 代价是命中之后要自己把顶点、法线、材质取回来 —— 光栅化那条路上由固定功能
# 硬件做的插值, 这里得手写。手写的每一步都可能错位, 所以判据不看颜色, 看四个
# 能逐像素对的原始量。
#
# 墙角场景 (地面反射墙, 有解析值):
#     命中距离  与 -P.z/R.z 比      实测 225280 像素全对, 最大误差 0.000154
#     材质下标  与墙的 bindless 下标比  一个都不错 (地面 0 / 墙 1)
#     命中法线  与墙的法线 (0,0,1) 比   最大误差**恰好 0**
#     位置自洽  由 t 算的命中点 == 由取回顶点插的命中点, 最大残差 0.000003
#
# OBJ 场景 (六个子网格共用一对缓冲区, 索引字节偏移 0/768/840/912/984/1080):
#     只跑位置自洽 —— 它与场景无关。43931 个命中像素, 最大残差 0.000001。
#     这是唯一能验到"索引地址漏加字节偏移"的场景。
#
# 变异 9/10, 唯一逃逸需要"有对象被跳过"的场景, 写在代码里。
Invoke-Step '光追反射 (墙角场景, 四个量对解析值)' -RequiresGpu {
    Invoke-Engine '--corner-scene --clustered --frames 8 --warmup 3 --rt-reflection-check'
}

Invoke-Step '光追反射 (OBJ 子网格, 位置自洽)' -RequiresGpu {
    Invoke-Engine '--scene Content/TestScene/testscene.obj --clustered --frames 8 --warmup 3 --rt-reflection-self'
}

# 光追环境光遮蔽 —— 需要真实 GPU。
#
# GTAO 那条判据只能验"随半径增大朝 0.5 单调收敛" —— 屏幕空间的近似没有解析解
# 可对。光追 AO 有闭式解: 地面 y=0、墙 z=0、离墙 d、半径 R, 令 c=d/R,
#
#     遮蔽率 = (1-c²)/2 - (2/π)∫_c^1 s·arcsin(c/s) ds
#
# c=0 时正好 0.5 (半个半球被挡住), c>=1 时正好 1。这个式子在 Python 里用
# 四十万次余弦加权采样独立验过, 七个 c 值上最大差 7.6e-4。
#
# 实测 (墙角场景, 256 个 Hammersley 样本, 两个半径各跑一次):
#
#     半径 0.8   有遮蔽区 39236 像素   有符号误差 -0.000083  绝对误差 0.0035
#     半径 2.0   有遮蔽区 116036 像素  有符号误差 -0.000124  绝对误差 0.0034
#
# 绝对误差 0.0034 与 R8 的量化步长 1/255=0.0039 同量级 —— **逐像素已经压到
# 输出纹理的精度极限**。
#
# 统计只取 d<R 的那一段: c>=1 的地方闭式解恒为 1, 而地面在视野里铺得很远,
# 半径 0.8 时八成以上的像素落在那一段。拿全体平均判等于把信号稀释五倍。
# 这一条现在连半分辨率一起验 —— 见代码里的两条子判据:
#   偶数像素上半分辨率与全分辨率必须**逐位相同** (半分辨率不是"把深度降采样
#     再解", 而是每隔一个像素解一次, 于是它是全分辨率结果的严格子集);
#   上采样之后整幅图仍要过同一条闭式解判据 (实测绝对误差 0.0027, 比全分辨率
#     的 0.0035 还小 —— 双边平滑把蒙特卡洛噪声抹掉了一部分)。
Invoke-Step '光追 AO (闭式解 + 半分辨率逐位一致)' -RequiresGpu {
    Invoke-Engine '--corner-scene --clustered --frames 8 --warmup 3 --rt-ao-check'
}

# 光追阴影 —— 需要真实 GPU。
#
# 与 --shadow-check 问的是同一件事、用同一条扫描线、同一组解析常量, 差别只在
# 输入: 一张是着色后的画面, 一张是光追的可见度掩码。于是量出来的差就只能是
# 阴影本身的差, 不是测量方法的差。
#
# 实测 (阴影场景, 1280x720, 一个像素 0.00487 世界单位):
#     光追      误差 0.00153 / 0.00191    容差 0.00974 (两个像素)
#     阴影贴图  误差 0.01419 / 0.01294    容差 0.07273
#
# 同一个解析值, 光追准八倍 —— 因为阴影贴图的容差里塞着深度偏置与图集分辨率,
# 而光追这两样都没有。容差一旦放宽到阴影贴图那个量级, 这条判据就不再是在
# 验光追了。
#
# 变异 8/10, 两条逃逸的成因写在代码里 (其中一条是理论上就该逃的)。
Invoke-Step '光追阴影 (边界落在相似三角形的解析位置上)' -RequiresGpu {
    Invoke-Engine '--shadow-scene --shadow-lights 2 --clustered --frames 8 --warmup 3 --rt-shadow-check'
}

Invoke-Step '光追深度 (综合场景, 与光栅化逐像素比对)' -RequiresGpu {
    Invoke-Engine '--showcase --clustered --frames 8 --warmup 3 --rt-depth-check'
}

Invoke-Step '光追深度 (OBJ 子网格, 与光栅化逐像素比对)' -RequiresGpu {
    Invoke-Engine '--scene Content/TestScene/testscene.obj --clustered --frames 8 --warmup 3 --rt-depth-check'
}

Invoke-Step '光追加速结构 (GPU 遍历与 CPU 解析解逐条比对)' -RequiresGpu {
    Invoke-Engine '--scene Content/TestScene/testscene.obj --frames 5 --warmup 2 --rt-check'
}

# 几何表按源对象下标索引 —— 需要真实 GPU。
#
# 补的是 Day 5 记录在案的那条逃逸: 几何表写成 table[实例序号] 还是
# table[源对象下标], 只在**有对象被跳过**时才分得开, 而三个测试场景一个都
# 不跳过 (墙角 2/2、综合 33/33、OBJ 3/3)。判据没覆盖到它, 不是判据不够严,
# 是场景里没有那件事。
#
# 所以这条判据自己造那件事: 取真实场景的对象列表, 在中间插一个没有三角形
# 的对象, 单独建一份加速结构, 把几何表读回来逐条比对。带两条元判据 ——
# 那个对象真的被跳过了, 而且跳过点之后至少有一对相邻对象是可分的。
#
# 变异"table[实例序号]"实测退出码 26, 不符 5 条。
Invoke-Step '光追几何表 (跳过对象之后仍按源下标索引)' -RequiresGpu {
    Invoke-Engine '--showcase --clustered --frames 4 --warmup 2 --rt-geometry-table-check'
}

# 光追的图有没有到达画面 —— 需要真实 GPU。
#
# 前六天的光追判据全是**旁路判据**: 把通道产出的图读回来与解析值比。那证明
# 了"图算得对", 没证明"画面用了这张图"。中间隔着描述符槽位、UBO 位域、
# 着色器里那个 if、半透明护栏 —— 任何一处断了, 旁路判据照样满分。
#
# 三条判据都不看颜色, 只看因果: AO 只能变暗 (实测变亮 0 个像素); AO 暗的
# 地方画面得跟着变 (实测 82.9%, 阈值 20%); 反射改到的像素比例要落在区间内
# (实测 3.7%, 区间 0.5%..25% —— 只卡下限的话"无条件加到每个像素上"能通过)。
#
# 五条变异全红: AO 分支断开 / AO 反着乘 / rtFlags 位序对调 / 反射不看
# 粗糙度 / 反射分支断开。
Invoke-Step '光追混合 (产出的图确实到达画面)' -RequiresGpu {
    Invoke-Engine '--showcase --gpu-driven --clustered --frames 6 --warmup 3 --rt-hybrid-check'
}

# 双边上采样在深度不连续处不渗色 —— 需要真实 GPU。
#
# 补的是 Day 7 扫描里的一条逃逸: 把双边权重去掉 (退化成双线性), 墙角场景上
# 所有判据全绿 —— 因为那个场景几乎没有深度不连续, 而双边加权的全部作用都在
# 不连续处。三个变体在墙角场景上的绝对误差是 0.002621 / 0.002621 / 0.002612,
# 前两个连小数点后六位都相同。
#
# 所以跑综合场景, 而且**限定在跨越深度不连续的像素上**再取均值:
#     双边 (正确)   0.006633
#     退化成双线性   0.019440   <- 要红
# 阈值 0.012。整幅的最大差不行 (噪声主导, 三个变体都接近 1), 整幅的均值也
# 不行 (不连续像素只占百分之几, 摊平就没了)。
Invoke-Step '光追 AO 上采样 (深度不连续处不渗色)' -RequiresGpu {
    Invoke-Engine '--showcase --gpu-driven --clustered --frames 6 --warmup 3 --rt-ao-upsample-check'
}

# meshlet 切分 —— 纯 CPU, 但仍标 RequiresGpu, 因为它跑在完整的引擎启动流程里。
#
# 判的是一件纯组合的事: 展开全部 meshlet 得到的三角形集合, 与原始索引数组
# 是不是同一个多重集 (含绕序)。下游 (剔除、可见性缓冲、材质解析) 全都假定
# meshlet 就是原网格的一个划分 —— 少一个三角形是"模型上有个洞", 多一个是
# Z 冲突, 而两者都可能只在某个视角下才看得见。
#
# 十条判据: 多重集相同 / 上限 / 局部索引 / 全局下标 / 包围球包得住 /
# 包围球不比自身包围盒的半对角线大 / 法线锥包得住 / 哨兵值合法 /
# 平均三角形数 / 顶点复用率。
#
# 最后两条是**质量**判据: 一个三角形一个 meshlet 满足前面每一条, 而它把
# 顶点数据放大三倍、把剔除粒度缩到没有意义。
#
# 三个输入: 球体 (连通、规则、法线处处不同)、立方体 (六个面互不共享顶点,
# 验"退化搜索"与"法线锥必须标记无效")、OBJ 测试网格 (真实资产, 顶点顺序
# 由导出器决定)。
#
# 变异 10/10。
Invoke-Step 'Meshlet 切分 (展开后与原三角形集合逐个相同)' -RequiresGpu {
    Invoke-Engine '--showcase --frames 3 --warmup 1 --meshlet-check'
}

# 两级 meshlet 剔除 —— 需要真实 GPU。
#
# 判的是"GPU 剔出来的可见集合与 CPU 参考实现逐条相同"。参考实现逐字照抄
# meshlet_common.h 里那三个函数 —— 各写各的话, 比的是"两个实现一不一样",
# 而两个实现可以一起错。
#
# 六条判据: 集合相同 / 计数器自洽 / 第一级保守 (它剔掉的实例, 逐 meshlet
# 判据也必须剔掉) / 背面剔除单调 / **分支覆盖** / 剔除真的剔掉了东西。
#
# 分支覆盖那一条是 Day 9 第一轮变异扫描的直接产物: 十二条变异里有四条逃逸,
# 成因全是"那个分支在这个视角下根本没被走到" —— 而一致性判据对没走到的
# 分支毫无约束。现在六个视锥平面各自独自剔掉过东西、两条 early-out 各自
# 被走到过, 少一条判据就红。
#
# 判据自己造条件: 六组相机配置 (窄视场 / 只视锥 / 宽视场 / 远平面拉近 /
# 近平面推远 / 相机放进某个包围球)。综合场景也为此加了两个物体 ——
# 一个均匀缩放的立方体 (法线锥无效) 与一个椭球 (非均匀缩放但锥有效)。
#
# 变异 12/12。
Invoke-Step 'Meshlet 剔除 (两级与 CPU 参考实现逐条相同)' -RequiresGpu {
    Invoke-Engine '--showcase --frames 6 --warmup 2 --meshlet-cull-check'
}

# meshlet 光栅化 —— 需要真实 GPU 且需要网格着色器。
#
# 补的是 Day 9 明写下来的欠账: 那一天的判据全是数值判据, 而数值判据**证明
# 不了画面**。这一天 meshlet 真的被光栅化了, 于是可以问画面。
#
# 四条判据:
#   网格着色器路径与计算展开回退路径画出的深度**逐位相同** (不留容差);
#   每个像素上 meshlet 路径的深度 >= 经典深度预通道的 (前者的三角形集合
#     是后者的子集 —— 只画不透明批次);
#   恰好相等的像素比例 —— 场景里没有蒙版材质时要求**处处相同**;
#   网格着色器声明的输出上限容得下构建器实际产出的最大 meshlet。
#
# 跑**两个**场景:
#   墙角  没有蒙版材质, 所以"处处逐位相同"那一档真的会跑 (实测
#         921600/921600);
#   综合  两个网格 (逐实例基址非零)、多实例, 所以"基址漏加"这一类缺陷
#         才显形 —— 而它正是这条判据当场抓到的那个。
#
# 变异 12/12。
# 光栅化的同一次执行还写出一张**可见性缓冲区** (R32_UINT): 每个像素记
# (可见记录槽位, 三角形序号) + 1, 0 表示这里没有几何体。加一是为了让 0
# 空出来当空值 —— 0 本身是合法编号 (第 0 条记录的第 0 个三角形), 而清除
# 颜色附件走的是浮点通道, R32_UINT 上只有 0.0f 的位模式恰好是整数 0。
#
# 判据比的是编号**解出来**的三元组 (实例, meshlet, 三角形), 不是编号本身:
# 槽位来自原子追加, 顺序每帧都不同 —— 实测直接比编号有 817036 个像素不同,
# 而它们画的是同一批三角形。
#
# 外加一条覆盖一致性: 编号为空的像素上深度必须是 1.0, 反过来也一样。
# 两者是同一次光栅化的两个附件, 覆盖范围只能相同。
Invoke-Step 'Meshlet 光栅化 (墙角: 与经典深度处处逐位相同)' -RequiresGpu {
    Invoke-Engine '--corner-scene --frames 8 --warmup 3 --meshlet-depth-check'
}

Invoke-Step 'Meshlet 光栅化 (综合: 两条路径逐位相同)' -RequiresGpu {
    Invoke-Engine '--showcase --frames 8 --warmup 3 --meshlet-depth-check'
}

# 材质解析 —— 需要真实 GPU。
#
# 可见性缓冲区上一个像素只有一个数。解析把它展开回"这个像素上是什么":
# 槽位 -> 可见记录 -> (实例, meshlet); 三角形序号 -> 三个顶点; 顶点 + 像素
# 中心 -> 重心坐标 -> 插值出来的属性。这条链上任何一环错了, 画面上都是
# "某处的着色不对", 而光栅化阶段一点痕迹都没有。
#
# 四条判据, 每条盯一段链:
#   重算的深度与光栅器写的逐像素吻合 (实测 0 个像素超过 64 ULP, 最大 15.6)
#     —— 一条判据钉住整条解码链: 编号、可见记录、顶点、重心坐标;
#   法线与经典 G-Buffer 吻合 (实测平均夹角 0.0026 度, 最大 0.39);
#   插值出来的世界坐标投回屏幕落在这个像素上 (实测最大偏离 0.0005 像素)
#     —— 这一条与场景无关, 而且只有透视校正的权重满足它;
#   材质下标按**源对象下标**查而不是实例序号 (Day 5 光追几何表的同一个坑)。
#
# 第三条是判据逼出来的: 只比法线的话"属性不做透视校正"逃逸 —— 综合场景里
# 法线变化大的三角形 (球) 都很小, 跨深度大的三角形 (地面) 三个顶点的法线
# 又相同, 于是任何权重插出来都一样。而世界坐标在每个三角形上都随位置变。
#
# 变异 11/11。
Invoke-Step 'Meshlet 材质解析 (重算的深度/法线/世界坐标都对得上)' -RequiresGpu {
    Invoke-Engine '--showcase --frames 8 --warmup 3 --meshlet-resolve-check'
}

# 网格简化 (QEM 边坍缩) —— 纯 CPU, 不需要 GPU。
#
# 虚拟几何的 LOD 从这里起步。判据里最要紧的一条是**误差是真实偏差的上界**:
# 把每一个原始顶点到简化后表面的距离量出来取最大, 必须不超过简化器报出来的
# 那个数。LOD 选择靠把这个误差投到屏幕上与阈值比来决定画哪一层 —— 误差报
# 小了, 该换层的时候不换; 相邻两块因此选了不同层而边界对不上, 就是裂缝。
#
# 这一条当场顶出过三版错的实现: 拿二次误差开方当误差 (面积加权之后量纲都
# 不对, 报小二十倍)、拿"顶点滑了多远"当误差 (平面上真实偏差 0 而报 1.92)、
# 只在局部三角形里量 (点沿着表面滑走之后量的是到边的距离, 平面上差 32000
# 倍)。现在三个测试网格上都是精确值 (1.00 倍)。
#
# 另外五条: 真的简化了 (不然"什么都不做"对上面那条满分)、不退化、没有三角形
# 与脚下的原始表面法线反向 (那是"鳍")、细条三角形不许变多、闭合流形保持、
# 同样输入两遍结果逐位相同 (第三天要拿它建 DAG)。
#
# 变异 7/8 (第 8 条的逃逸原因已测: 逐步翻转检查被"锚住最初法线"那条盖住)。
Invoke-Step '网格简化 (QEM: 误差是真实偏差的上界)' {
    Invoke-Engine '--frames 1 --warmup 0 --mesh-simplify-check'
}

# 规模: 压力场景上的 meshlet 光栅化 —— 需要真实 GPU。
#
# 与综合场景那一条同一条判据 (meshlet 路径与经典深度逐位相同), 但场景换成
# 1601 个物体的压力网格。换场景不是为了"更大", 是为了**换几何**: 压力场景
# 里的球只有 16x12 段, 每个 meshlet 跨的曲率比综合场景大得多。
#
# 第十四天正是这一条量出法线锥存错了 —— 存进去的是半角余弦而背面剔除要拿
# 半角正弦比。半角小于 45 度时那个错只表现为漏剔 (保守), 越过 45 度就翻成
# 错剔。综合场景里的球分段密, 半角一直在 45 度以内, 于是 Day 9 的剔除判据、
# Day 10 的逐位相同、Day 12 的解析判据全绿; 换到这里立刻有 1490 个像素画的
# 是背后的东西。
Invoke-Step 'Meshlet 光栅化 (压力场景: 与经典深度逐位相同)' -RequiresGpu {
    Invoke-Engine '--grid 40 --frames 8 --warmup 3 --meshlet-depth-check'
}

# 规模: 逐物体缓冲区装不下时不许索引到界外 —— 需要真实 GPU。
#
# 逐物体缓冲区 (模型矩阵 + 材质下标) 定容, 分三段: 相机 / 投射体 / 半透明。
# 场景大到三段合计超容量时后面的段被截断 —— 截断本身没得选, 要命的是绘制
# 那一侧照着**列表长度**走: 各 Pass 把列表下标当 firstInstance 传进去, 而
# 着色器拿 gl_InstanceIndex 直接索引那个缓冲区。列表比写进去的条目长时,
# 后面那些物体索引到的是缓冲区之外, 读出来的"材质下标"是垃圾, 下一步要去
# 索引 bindless 材质表 —— GPU 读非法地址, **设备丢失**。
#
# 这个缺陷第十四天就撞上了 (16130 个物体必然丢设备), 但当时只查到"是 CPU
# 侧的上传写触发的"就没往下走。第十五天开了 VK_EXT_device_fault, 驱动一句
# 话说清: READ_INVALID, 而那个地址不属于任何一个活着的缓冲区。
#
# 判据盯的是不变式而不是症状: **发出去的最大逐物体下标必须小于写进去的
# 条目数**。拿"会不会丢设备"当判据是不行的 —— 越界读到的内存是不是已映射
# 取决于堆布局, 实测同一个场景有时丢设备有时安然无恙。
#
# 场景里有方向光 + 投影聚光灯 + 一小撮半透明, 三条绘制路径 (级联阴影、
# 阴影图集、前向半透明) 都走得到。变异 5/5。
Invoke-Step 'GPU 剔除溢出 (截断之后不许索引到界外)' -RequiresGpu {
    Invoke-Engine '--grid 127 --frames 8 --warmup 3 --gpu-cull-overflow-check'
}

# 规模: 容量溢出必须响 —— 需要真实 GPU。
#
# 可见表与待定表都是定容的, 超出容量的条目在着色器里直接丢掉。丢掉本身没得
# 选 (总不能越界写), 问题在于第一版**一个字都不说**: 画面上少一块, 日志里
# 干干净净, 而且只在场景大到一定程度才出现。
#
# 判据自己造条件: 靠场景规模走不到溢出 (可见表按 262144 条开的), 所以它把
# 容量压到可见数的四分之一, 逼那条路径走一遍, 再验三件事 —— 报得出来、
# 写进去的那部分仍然是好的、容量恢复之后标志不粘着。
Invoke-Step 'Meshlet 规模 (容量溢出必须报出来)' -RequiresGpu {
    Invoke-Engine '--grid 100 --frames 10 --warmup 3 --meshlet-scale-check'
}

# Hi-Z 两阶段遮挡剔除 —— 需要真实 GPU。
#
# 遮挡剔除是纯粹的优化, 所以判据的主干很直接: 开关它, 深度与可见性必须
# 逐像素相同。但这条判据自己**造条件** —— 默认视角下几乎没有遮挡, 而且
# 相机不动, 上一帧的金字塔永远是对的, 于是第二阶段整个删掉都不会红。
#
# 判据做了三件事来把该走的路径走到:
#
#   把相机摆到量出来最有遮挡的位置 (0,1,10), 一半以上的 meshlet 被剔;
#   制造一次视角跳变 —— 第一阶段拿到的是上一个视角的金字塔, 结论必然错,
#     两阶段要在同一帧里把它补回来;
#   再摆一次到柱子内部 —— 那里包围球穿近平面, 投影会失败, 而那时"存疑
#     不剔"是画面正确的唯一依据。
#
# 除了画面, 还有一层直接盯着遮挡测试本身的判据 (hiz_probe.comp): 310 个
# 包围球喂进 MeshletProjectSphere 与 MeshletHizMaxDepth, 逐个验四条单向
# 不等式 —— 矩形包得住真实投影且不许松、最近深度不大于球面真实最小深度、
# 金字塔查到的最大值不小于第 0 级真值、结论与那两个数自洽。
#
# 这一层非有不可: 两阶段是**自纠错**的, 第一阶段错剔了第二阶段就补回来,
# 于是画面判据对遮挡测试本身的错误几乎是瞎的 —— 十一条变异里它只红了三条。
# 探针加上去之后是 11/12 (第 12 条构造上不可达)。
#
# 探针当场量出两个真缺陷: 四角近似包不住透视投影的球 (最多缺 977 像素),
# "球面上离相机最近的点"不是深度最小的点 (最多偏 0.0076)。两个都只让第一
# 阶段多剔, 都被第二阶段补了回去, 画面上从来没露过头。改成切线精确解之后,
# 第一阶段剔 94 个而第二阶段只需补 25 个 (原来是 102 / 46)。
Invoke-Step 'Meshlet 遮挡剔除 (两阶段 Hi-Z, 开关画面逐像素相同)' -RequiresGpu {
    Invoke-Engine '--showcase --frames 12 --warmup 4 --meshlet-occlusion-check'
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
