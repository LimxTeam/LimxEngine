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

# 要对照的每一个 C++ 结构体: 它的 static_assert 在哪个头文件里, 以及在
# SPIR-V 那侧对应哪个 (set, binding)。
#
# 这是一张表而不是几行写死的代码, 因为漏掉一个结构体不会有任何症状 ——
# FLightingUBO 就是这么漏了一整个周期的: verify-shader-layout 只查
# set 0/binding 0, 而 pbr.frag 里 `LightData lights[16]` 是个**字面量**,
# 不是宏也不是 include。C++ 侧把 kMaxLightCount 改掉时 static_assert 会
# 编译失败逼你改数字, 然后你改了数字, 着色器依然是 16 —— 那之后 UBO 里
# 所有字段整体错位: 相机位置读到光源数据、阴影矩阵读到垃圾、IBL 开关抽风,
# 零报错。
$structs = @(
    @{
        Name   = 'FViewProjUBO'
        Header = 'Source/Runtime/Renderer/Public/Renderer/Renderer/FRenderer.h'
        Set    = 0
        Binding = 0
        # 谁 include 了这个头就查谁
        Marker = 'view_common\.h'
        MinShaders = 5
    }
    @{
        Name   = 'FLightingUBO'
        Header = 'Source/Runtime/RenderCore/Public/RenderCore/Lighting/FLight.h'
        Set    = 2
        Binding = 0
        # 必须精确到声明本身。写成 'LightingUBO' 会把只在注释里提到它的
        # pbr.vert 也算进来, 于是永远报"反射里没有这个 UBO"。
        Marker = 'uniform\s+LightingUBO\s*\{'
        MinShaders = 1
    }
)

# push constant 单独一条 —— 它不是描述符, 反射里也在另一个字段上
$pushHeader = 'Source/Runtime/Renderer/Public/Renderer/Renderer/FRenderer.h'


function Get-ExpectedSize
{
    param([string]$Path, [string]$Name)

    if (-not (Test-Path $Path))
    {
        Write-Host "错误: 未找到 $Path" -ForegroundColor Red
        return -1
    }

    $text = Get-Content $Path -Raw

    if ($text -match "static_assert\(sizeof\($Name\)\s*==\s*(\d+)")
    {
        return [int]$Matches[1]
    }

    Write-Host "错误: 在 $Path 里找不到 $Name 的 static_assert" -ForegroundColor Red
    return -1
}

$expected = @{}

$expected['FModelPushConstant'] =
    Get-ExpectedSize -Path $pushHeader -Name 'FModelPushConstant'

if ($expected['FModelPushConstant'] -lt 0) { exit 1 }

foreach ($s in $structs)
{
    $expected[$s.Name] = Get-ExpectedSize -Path $s.Header -Name $s.Name

    if ($expected[$s.Name] -lt 0) { exit 1 }
}

$offenders   = New-Object System.Collections.ArrayList
$checkedPush = 0
$checkedByStruct = @{}

$allShaders = Get-ChildItem -Path Shaders -Recurse -Include *.vert, *.frag

foreach ($s in $structs)
{
    $checkedByStruct[$s.Name] = 0

    # 靠源码里的标记发现要检查哪些着色器 —— 将来加第六个着色器时不需要
    # 改这个脚本, 它自动被纳入。
    $shaders = $allShaders |
               Where-Object { (Get-Content $_.FullName -Raw) -match $s.Marker }

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
           Where-Object { $_.set -eq $s.Set -and $_.binding -eq $s.Binding }

    if ($null -eq $ubo) {
        [void]$offenders.Add(
            "$($shader.Name): 反射里没有 set $($s.Set) / binding $($s.Binding) 的 UBO ($($s.Name))")
        continue
    }

    $checkedByStruct[$s.Name]++

    if ($ubo.size -ne $expected[$s.Name]) {
        [void]$offenders.Add(
            "$($shader.Name): set$($s.Set)/binding$($s.Binding) 是 $($ubo.size) 字节, C++ 侧 $($s.Name) 是 $($expected[$s.Name])")
    }

    # push constant 的对照对象是 FModelPushConstant, 而不是"任何 push
    # constant"。triangle.vert 是遗留的演示管线, 它自有一个 64 字节的布局
    # (只有 mat4 model), 与 FModelPushConstant 无关 —— 把它们一起比会得到
    # 一个恒红的检查, 而恒红的检查最终会被人注释掉。
    #
    # 判据用源码里有没有 materialIndex: 那是 FModelPushConstant 相对于旧
    # 布局多出来的那个字段, 声明了它就是在用这个结构体。
    if ($s.Set -eq 0 -and (Get-Content $shader.FullName -Raw) -match 'materialIndex') {
        $checkedPush++

        if ($reflection.push_constants.size -ne $expected['FModelPushConstant']) {
            [void]$offenders.Add(
                "$($shader.Name): push constant 是 $($reflection.push_constants.size) 字节, C++ 侧 FModelPushConstant 是 $($expected['FModelPushConstant'])")
        }
    }
    }
}

# 一个都没查到就是"什么都没发生", 而那与"全部通过"在结果上无法区分。
# 反射文件缺失、目录改名、Where-Object 写错 —— 都会走到这里。
foreach ($s in $structs)
{
    if ($checkedByStruct[$s.Name] -lt $s.MinShaders)
    {
        Write-Host ("只检查到 {0} 个着色器的 {1} (预期至少 {2} 个) — 判定无效" -f `
            $checkedByStruct[$s.Name], $s.Name, $s.MinShaders) -ForegroundColor Red
        foreach ($offender in $offenders) { Write-Host "  $offender" }
        exit 1
    }
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

$summary = ($structs | ForEach-Object {
    "{0}×{1}={2}B" -f $checkedByStruct[$_.Name], $_.Name, $expected[$_.Name]
}) -join ', '

Write-Host "布局一致: $summary, push×$checkedPush=$($expected['FModelPushConstant'])B" -ForegroundColor Green
exit 0
