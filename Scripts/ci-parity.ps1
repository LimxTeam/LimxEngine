# ============================================================
# ci-parity.ps1 — 校验 verify.ps1 与 ci.yml 没有分歧
#
# 两边都要跑的命令必须逐字相同。这一点靠人记不住:
#
#   第五阶段 Day 1 实测发现, verify.ps1 跑的是
#     lbt.exe validate -s Source
#   而 ci.yml 跑的是
#     lbt.exe validate -s Source --strict
#
#   于是本地 16 步全绿, 而 CI 的第二步会直接失败 —— 这类分歧在真正触发
#   CI 之前完全看不见, 而 CI 触发一次要几分钟。
#
# 本脚本从两个文件里各自抽出工具调用命令行, 做集合比对。它不需要 GPU,
# 不需要网络, 因此属于无 GPU 层。
#
# 退出码: 0 一致 | 1 存在分歧
# ============================================================

[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

$VerifyPath = Join-Path $RootDir 'Scripts\verify.ps1'
$CiPath     = Join-Path $RootDir '.github\workflows\ci.yml'

foreach ($p in @($VerifyPath, $CiPath)) {
    if (-not (Test-Path $p)) {
        Write-Host "错误: 未找到 $p" -ForegroundColor Red
        exit 1
    }
}

# ------------------------------------------------------------
# 抽取工具调用
#
# 只看 Binaries\Tools\ 与 Binaries\Development\ 下的调用 —— 那些是两边
# 共有的部分。GitHub Action (checkout / cache / upload-artifact) 与
# PowerShell 的控制流各自独有, 不参与比对。
# ------------------------------------------------------------

function Get-ToolCommands {
    param([string]$Path)

    $commands = @()

    foreach ($line in (Get-Content $Path)) {
        # 统一分隔符与前后缀, 使两个文件的写法可比
        if ($line -notmatch 'Binaries[\\/](Tools|Development)') { continue }

        $text = $line.Trim()

        # 去掉 YAML 的 "run: " 前缀与 PowerShell 的前导点斜杠
        $text = $text -replace '^\s*run:\s*', ''
        $text = $text -replace '^\.[\\/]', ''
        $text = $text -replace '[\\/]', '/'

        # 去掉行尾注释与反引号续行
        $text = $text -replace '\s+#.*$', ''
        $text = $text -replace '\s*`$', ''

        if ($text.Length -gt 0) {
            $commands += $text
        }
    }

    return $commands
}

$verifyCmds = Get-ToolCommands $VerifyPath
$ciCmds     = Get-ToolCommands $CiPath

Write-Host ''
Write-Host ('=' * 72)
Write-Host '  CI 等价性检查'
Write-Host ('=' * 72)
Write-Host ''

# ------------------------------------------------------------
# 比对
#
# 只报"同一个工具、同一个子命令, 但参数不同"的情形。两边命令集合不完全
# 相同是正常的 —— CI 分作业跑, verify 顺序跑, 且 GPU 层只在其中一边。
# 真正危险的是同一件事两边参数不一样。
# ------------------------------------------------------------

function Get-CommandKey {
    param([string]$Command)

    $parts = $Command -split '\s+'

    if ($parts.Count -eq 0) { return $null }

    $exe = Split-Path -Leaf $parts[0]

    # 子命令: 第一个不以 - 开头的后续词
    $sub = ''
    for ($i = 1; $i -lt $parts.Count; $i++) {
        if ($parts[$i] -notmatch '^-') { $sub = $parts[$i]; break }
    }

    return "$exe $sub".Trim()
}

$verifyMap = @{}
foreach ($c in $verifyCmds) {
    $k = Get-CommandKey $c
    if ($k) { $verifyMap[$k] = $c }
}

$mismatches = @()

foreach ($c in $ciCmds) {
    $k = Get-CommandKey $c
    if (-not $k -or -not $verifyMap.ContainsKey($k)) { continue }

    if ($verifyMap[$k] -ne $c) {
        $mismatches += [PSCustomObject]@{
            命令   = $k
            verify = $verifyMap[$k]
            ci     = $c
        }
    }
}

if ($mismatches.Count -eq 0) {
    Write-Host "  两边共有的 $($verifyMap.Count) 条工具命令参数一致" -ForegroundColor Green
    Write-Host ''
    exit 0
}

Write-Host "  发现 $($mismatches.Count) 处分歧:" -ForegroundColor Red
Write-Host ''

foreach ($m in $mismatches) {
    Write-Host "    $($m.命令)" -ForegroundColor Yellow
    Write-Host "      verify.ps1: $($m.verify)"
    Write-Host "      ci.yml    : $($m.ci)"
    Write-Host ''
}

Write-Host '  两边跑同一件事时参数必须逐字相同, 否则本地全绿而 CI 失败。'
Write-Host ''
exit 1
