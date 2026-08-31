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
# 退出码: 0 = 已逐条比对且一致 | 1 = 未能得出"一致"这个结论
#
# 注意 0 的含义是"比对做完了且没有分歧", 不是"没有报出分歧"。两者的差别
# 就是下面 $RequiredKeys 那道闸门存在的理由 —— 见该处注释。
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

        # 只认真正的可执行文件调用 —— 必须出现 .exe。
        #
        # 否则 YAML 的 path:/name: 与 PowerShell 的变量赋值 (两者都可能
        # 带 Binaries 路径) 会被当成命令报出来。这条限制也意味着: 想被
        # 比对的命令必须写全 .exe 后缀, 这在两个文件里本来就是惯例。
        if ($line -notmatch '\.exe') { continue }

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

$ciMap = @{}
foreach ($c in $ciCmds) {
    $k = Get-CommandKey $c
    if ($k) { $ciMap[$k] = $c }
}

# ------------------------------------------------------------
# 抽取有效性闸门 —— 必须在比对之前
#
# 下面的比对本质上是求交集。而空集与任何集合的交集都是空集: 抽取一旦落空,
# "零处分歧"就会照常成立, 脚本退出 0。也就是说这个脚本最危险的失败形态不是
# 报错, 是**一件都没比对却报告一致** —— 判据(有没有分歧)依赖一个在失败路径
# 上取不到的值(抽出来的命令), 而取不到时默认落在"通过"那一侧。
#
# 抽取规则要求每行同时满足两条: 路径匹配 Binaries[\/](Tools|Development),
# 且含字面量 .exe。ci.yml 只要换一种写法就同时不满足 —— 路径写成
# ${{ env.TOOLS }}/lbt、或者用 working-directory 加裸命令名, 都会让 lbt 那
# 几条一条也抽不出来。实测: 指向一份 ${{ env.TOOLS }}/lbt 写法、且去掉了
# validate --strict 的 ci.yml, 修改前的脚本退出 0, 还打印"16 条命令参数
# 一致" —— 而 --strict 分歧正是这个脚本存在的全部理由。
#
# 判据不用"抽到的条数够不够"这种阈值: 阈值只能证明抽到了东西, 证明不了抽到
# 的是不是该抽的那几条 —— 上面那份 ci.yml 里 lsc 和测试程序仍是字面路径,
# 条数看着不少, 而 lbt 三条全丢了。所以改成点名: 下面这几条是两边必须都跑
# 的调用, 少一条只有两种可能, 要么抽取规则跟文件写法脱节了, 要么这条命令真
# 的从某一边消失了 —— 两种都必须红, 都不能算作"没有分歧"。
# ------------------------------------------------------------

$RequiredKeys = @(
    'lbt.exe check'
    'lbt.exe validate'
    'lbt.exe build'
    'lsc.exe compile-all'
    'lsc.exe validate'
)

$absent = @()
foreach ($rk in $RequiredKeys) {
    $inVerify = $verifyMap.ContainsKey($rk)
    $inCi     = $ciMap.ContainsKey($rk)
    if ($inVerify -and $inCi) { continue }

    $absent += [PSCustomObject]@{
        命令   = $rk
        verify = $inVerify
        ci     = $inCi
    }
}

if ($absent.Count -gt 0) {
    Write-Host '  本次没有比对成立的依据: 应当两边都出现的命令没有全部抽到。' -ForegroundColor Red
    Write-Host ''
    # 用 @() 包一层: 抽取结果为空时 Get-ToolCommands 返回的是 $null 而不是空
    # 数组, 直接取 .Count 拿不到 0 —— 而 0 恰好是这里最需要打印出来的那个数。
    Write-Host ("    verify.ps1 抽到 {0} 条, ci.yml 抽到 {1} 条" -f @($verifyCmds).Count, @($ciCmds).Count)
    Write-Host ''
    foreach ($a in $absent) {
        $vMark = if ($a.verify) { '有' } else { '缺' }
        $cMark = if ($a.ci)     { '有' } else { '缺' }
        Write-Host ("    {0,-20} verify.ps1: {1}   ci.yml: {2}" -f $a.命令, $vMark, $cMark) -ForegroundColor Yellow
    }
    Write-Host ''
    Write-Host '  可能原因二选一:'
    Write-Host '    1. 该命令真的从某一边被删了 —— 那边从此不跑这一步, 直接修文件。'
    Write-Host '    2. 写法变了, Get-ToolCommands 抽不出来 —— 例如路径改成了'
    Write-Host '       ${{ env.TOOLS }}/lbt, 或者改用 working-directory 加裸命令名。'
    Write-Host '       此时必须同步改抽取规则, 否则这个脚本从此只会空转报绿。'
    Write-Host ''
    exit 1
}

# 真正逐字比对过的对数。报告里必须用这个数, 不能用 $verifyMap.Count ——
# 后者是"verify.ps1 一侧抽出了多少条", 跟比对了多少对没有关系: ci.yml 一条
# 都没抽到时它照样是 16, 于是一个让人放心的数字底下是零次比对。
$compared = 0
$mismatches = @()

foreach ($c in $ciCmds) {
    $k = Get-CommandKey $c
    if (-not $k -or -not $verifyMap.ContainsKey($k)) { continue }

    $compared++

    if ($verifyMap[$k] -ne $c) {
        $mismatches += [PSCustomObject]@{
            命令   = $k
            verify = $verifyMap[$k]
            ci     = $c
        }
    }
}

# ------------------------------------------------------------
# CI 独有的命令
#
# 上面只比对两边都有的命令。但"CI 跑了而 verify 没跑"同样危险 —— 那部分
# 在本地永远不会被执行, 出问题只能等 CI 告诉你。
#
# 实测踩过: ci.yml 有 `lsc validate -s Shaders --strict` 而 verify.ps1 只有
# compile-all。那条命令因为参数用法就是错的 (validate 当时只接受单个文件,
# 传目录直接 os error 5), CI 的着色器作业一直是红的, 而本地十七步全绿。
# ------------------------------------------------------------

$onlyInCi = @()

foreach ($c in $ciCmds) {
    $k = Get-CommandKey $c
    if (-not $k) { continue }

    # 这些是 CI 环境独有的, 本地不适用
    if ($k -match 'verify\.ps1|ci-parity\.ps1') { continue }

    if (-not $verifyMap.ContainsKey($k)) {
        $onlyInCi += $c
    }
}

if ($mismatches.Count -eq 0 -and $onlyInCi.Count -eq 0) {
    Write-Host "  逐字比对了 $compared 对工具命令, 参数一致" -ForegroundColor Green
    Write-Host ''
    exit 0
}

if ($onlyInCi.Count -gt 0) {
    Write-Host "  $($onlyInCi.Count) 条命令只在 ci.yml 里出现, 本地从不执行:" -ForegroundColor Red
    Write-Host ''
    foreach ($c in $onlyInCi) {
        Write-Host "    $c" -ForegroundColor Yellow
    }
    Write-Host ''
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
