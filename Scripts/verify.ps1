# ============================================================
# verify.ps1 — 本地全量验证
#
# 与 .github/workflows/ci.yml 执行等价的检查，用于在提交前
# 本地复现 CI 结论。CI 尚未在真实环境跑通，本脚本是当前
# 唯一经过实测的验证入口。
#
# 用法:
#   pwsh Scripts/verify.ps1              # 增量构建 + 全部检查
#   pwsh Scripts/verify.ps1 -Rebuild     # 强制全量重建
#   pwsh Scripts/verify.ps1 -SkipTools   # 跳过 Rust 工具链构建
#
# 退出码: 0 全部通过 | 1 存在失败
# ============================================================

[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$SkipTools
)

$ErrorActionPreference = 'Continue'

# ------------------------------------------------------------
# 定位引擎根目录 — 脚本位于 <root>/Scripts
# ------------------------------------------------------------

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

$Script:StepIndex = 0
$Script:Failures = @()

function Write-Header {
    param([string]$Text)
    Write-Host ''
    Write-Host ('=' * 72)
    Write-Host "  $Text"
    Write-Host ('=' * 72)
}

# 执行一个验证步骤：失败只记录不中断，使一次运行暴露全部问题
function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Action
    )

    $Script:StepIndex++
    Write-Host ''
    Write-Host "[$Script:StepIndex] $Name" -ForegroundColor Cyan

    $output = & $Action 2>&1
    $exitCode = $LASTEXITCODE

    if ($null -eq $exitCode) { $exitCode = 0 }

    if ($exitCode -ne 0) {
        Write-Host "    失败 (退出码 $exitCode)" -ForegroundColor Red
        $output | Select-Object -Last 25 | ForEach-Object { Write-Host "    $_" }
        $Script:Failures += $Name
    }
    else {
        Write-Host "    通过" -ForegroundColor Green
    }
}

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
        foreach ($tool in @('lbt', 'lht', 'lsc')) {
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

# ------------------------------------------------------------
# 3. 源码规则
# ------------------------------------------------------------

Write-Header '源码规则'

Invoke-Step '零 STL / 零 CRT / 无裸 new-delete 检查' {
    .\Binaries\Tools\lbt.exe check --source-dir Source
}

Invoke-Step '模块配置校验' {
    .\Binaries\Tools\lbt.exe validate -s Source
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
    .\Binaries\Development\Win64\LimxCoreTests.exe
}

Invoke-Step 'RHITests' {
    .\Binaries\Development\Win64\LimxRHITests.exe
}

Invoke-Step 'AssetTests' {
    .\Binaries\Development\Win64\LimxAssetTests.exe
}

Invoke-Step 'EngineTests' {
    .\Binaries\Development\Win64\LimxEngineTests.exe
}

# 显存回收自检 —— 需要真实 GPU, 因此放在单元测试之后单独一步。
# 它验证的是"引用计数是否真的接通", 而这一点靠单元测试测不到:
# 泄漏只在 GPU 资源实际分配与释放时才成立。
Invoke-Step '显存回收自检' {
    .\Binaries\Development\Win64\LimxLaunch.exe --scene Content/TestScene/testscene.obj --reload-test
}

# IBL 白炉自检 —— 同样需要真实 GPU。
# 它用一个各方向辐射度恒为 1 的合成环境跑完整条 IBL 预计算链, 断言三条
# 解析可知的性质: 辐照度处处为 1、预滤波每一级都为 1、BRDF 表的 A+B 不
# 超过 1。这三条不依赖任何具体 HDRI, 因此不需要外部资产。
#
# 这条链上的错误几乎全是"看着差不多"的: 卷积系数差一个 π、mip 与粗糙度
# 的映射错位、归一化除错了分母 —— 画面上一律只表现为"环境光有点不对"。
Invoke-Step 'IBL 白炉自检' {
    .\Binaries\Development\Win64\LimxLaunch.exe --furnace-check
}

# ------------------------------------------------------------
# 汇总
# ------------------------------------------------------------

Write-Header '验证汇总'

if ($Script:Failures.Count -eq 0) {
    Write-Host "  全部 $Script:StepIndex 个步骤通过" -ForegroundColor Green
    Write-Host ''
    exit 0
}

Write-Host "  $($Script:Failures.Count)/$Script:StepIndex 个步骤失败:" -ForegroundColor Red
foreach ($failure in $Script:Failures) {
    Write-Host "    - $failure" -ForegroundColor Red
}
Write-Host ''
exit 1
