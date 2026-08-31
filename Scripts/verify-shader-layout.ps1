# ============================================================
# verify-shader-layout.ps1 — C++ 结构体与 SPIR-V 布局的一致性检查
#
# 两侧各自都已有保障: C++ 的 static_assert 钉住结构体大小, 五个顶点着色器
# 统一从 Shaders/Builtin/view_common.h 取 UBO 声明。缺的是**跨语言那一跳**
# —— 有人改了 C++ 结构体却没改 GLSL 头 (或反过来) 时, 两边各自都编得过,
# 只有画面会错, 而且错法是矩阵整体错位、全黑无报错。std140 不会因为着色器
# 少声明一个字段就报错, 它只按自己声明的偏移去读。
#
# 期望值从 static_assert 里读, 不写死在本脚本里 —— 写死的话它自己就会与
# C++ 侧漂移, 而漂移之后它照样报"通过"。
#
# 数据源是 lsc 产出的反射 JSON。那份反射在 2026-08-31 之前是坏的 (size 恒
# 为 0), 修好之后才能作为判据 —— 用坏的反射做这个检查等于什么都没检查。
#
# 退出码: 0 = 一致, 1 = 不一致或判定无效
# ============================================================

[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

$header = 'Source/Runtime/Renderer/Public/Renderer/Renderer/FRenderer.h'

if (-not (Test-Path $header)) {
    Write-Host "错误: 未找到 $header" -ForegroundColor Red
    exit 1
}

$headerText = Get-Content $header -Raw

# 期望值来自 C++ 侧唯一的真相源
$expected = @{}

foreach ($name in @('FViewProjUBO', 'FModelPushConstant')) {
    if ($headerText -match "static_assert\(sizeof\($name\)\s*==\s*(\d+)") {
        $expected[$name] = [int]$Matches[1]
    }
    else {
        Write-Host "错误: 在 $header 里找不到 $name 的 static_assert" -ForegroundColor Red
        exit 1
    }
}

# 用 view_common.h 的引用关系发现要检查哪些着色器 —— 将来加第六个着色器时
# 不需要改这个脚本, 它自动被纳入。
$shaders = Get-ChildItem -Path Shaders -Recurse -Include *.vert, *.frag |
           Where-Object { (Get-Content $_.FullName -Raw) -match 'view_common\.h' }

$checkedUbo  = 0
$checkedPush = 0
$offenders   = New-Object System.Collections.ArrayList

foreach ($shader in $shaders) {
    $relative = $shader.FullName.Substring((Get-Location).Path.Length + 1)

    # 用 .NET 的 Replace 而不是 -replace: 后者第一个参数是正则, 反斜杠要写
    # 成两个, 而那层转义在经手任何生成脚本时极易被吃掉。吃掉之后模式变成
    # 一个孤立的反斜杠, 正则报错 —— 而报错落在循环体里, 表现是"一个着色器
    # 都没查到", 而不是一条明确的错误。
    $json = 'Binaries/' + $relative.Replace([char]92, [char]47) + '.json'

    if (-not (Test-Path $json)) {
        [void]$offenders.Add("$($shader.Name): 找不到反射文件 $json")
        continue
    }

    $reflection = Get-Content $json -Raw | ConvertFrom-Json

    $ubo = $reflection.uniform_buffers |
           Where-Object { $_.set -eq 0 -and $_.binding -eq 0 }

    if ($null -eq $ubo) {
        [void]$offenders.Add("$($shader.Name): 反射里没有 set 0 / binding 0 的 UBO")
        continue
    }

    $checkedUbo++

    if ($ubo.size -ne $expected['FViewProjUBO']) {
        [void]$offenders.Add(
            "$($shader.Name): set0/binding0 是 $($ubo.size) 字节, C++ 侧 FViewProjUBO 是 $($expected['FViewProjUBO'])")
    }

    # push constant 的对照对象是 FModelPushConstant, 而不是"任何 push
    # constant"。triangle.vert 是遗留的演示管线, 它自有一个 64 字节的布局
    # (只有 mat4 model), 与 FModelPushConstant 无关 —— 把它们一起比会得到
    # 一个恒红的检查, 而恒红的检查最终会被人注释掉。
    #
    # 判据用源码里有没有 materialIndex: 那是 FModelPushConstant 相对于旧
    # 布局多出来的那个字段, 声明了它就是在用这个结构体。
    if ((Get-Content $shader.FullName -Raw) -match 'materialIndex') {
        $checkedPush++

        if ($reflection.push_constants.size -ne $expected['FModelPushConstant']) {
            [void]$offenders.Add(
                "$($shader.Name): push constant 是 $($reflection.push_constants.size) 字节, C++ 侧 FModelPushConstant 是 $($expected['FModelPushConstant'])")
        }
    }
}

# 一个都没查到就是"什么都没发生", 而那与"全部通过"在结果上无法区分。
# 反射文件缺失、目录改名、Where-Object 写错 —— 都会走到这里。
if ($checkedUbo -lt 5) {
    Write-Host "只检查到 $checkedUbo 个着色器的 UBO (预期至少 5 个) — 判定无效" -ForegroundColor Red
    foreach ($offender in $offenders) { Write-Host "  $offender" }
    exit 1
}

if ($checkedPush -lt 3) {
    Write-Host "只检查到 $checkedPush 个 push constant (预期至少 3 个) — 判定无效" -ForegroundColor Red
    foreach ($offender in $offenders) { Write-Host "  $offender" }
    exit 1
}

if ($offenders.Count -gt 0) {
    Write-Host 'C++ 结构体与 SPIR-V 布局不一致:' -ForegroundColor Red
    foreach ($offender in $offenders) { Write-Host "  $offender" }
    exit 1
}

Write-Host "$checkedUbo 个 UBO / $checkedPush 个 push constant 全部对上 (UBO $($expected['FViewProjUBO']) 字节, push $($expected['FModelPushConstant']) 字节)" -ForegroundColor Green
exit 0
