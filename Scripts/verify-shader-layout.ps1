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
        # 必须精确到 include 指令本身。写成 'view_common\.h' 会把只在注释里
        # 提到它的着色器也算进来 (taa.frag 就是), 于是永远报"反射里没有这个
        # UBO"。同样的错在 FLightingUBO 那条上已经犯过一次 —— 标记是文本搜索,
        # 而注释也是文本。
        Marker = '#include\s+"view_common\.h"'
        # 5 → 4: depth_only.vert 在 GPU 驱动那天不再读 set 0 的 view/proj
        # (视图矩阵改走 push constant, 模型矩阵改走 set 3), 于是它不再 include
        # 这个头。
        MinShaders = 4
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

# storage buffer 走另一张表 —— 反射里它们在 storage_buffers 而不是
# uniform_buffers, 而且比的是**元素步长**不是块大小: 运行期定长数组的块
# 大小是 0, 只有 array_stride 是确定的。
#
# 步长其实比块大小更有力: 结构体里加一个字段、改一处对齐, 步长立刻不同,
# 而块大小对定长数组根本没有意义。
#
# 标记必须连 set/binding 一起写。同一个 C++ 结构体在不同着色器里绑在不同
# 位置 —— FLightData 在 pbr.frag 是 set2/binding5, 在 light_cull.comp 是
# set0/binding1。只匹配 'buffer LightBuffer' 的话, 两个文件都会被拿去和
# 同一组 set/binding 比, 于是恒红。
$storageStructs = @(
    @{
        Name    = 'FLightData'
        Header  = 'Source/Runtime/RenderCore/Public/RenderCore/Lighting/FLight.h'
        Set     = 2
        Binding = 5
        Marker  = 'set\s*=\s*2,\s*binding\s*=\s*5\)\s*readonly\s+buffer\s+LightBuffer'
        MinShaders = 1
    }
    @{
        Name    = 'FLightData'
        Header  = 'Source/Runtime/RenderCore/Public/RenderCore/Lighting/FLight.h'
        Set     = 0
        Binding = 1
        Marker  = 'set\s*=\s*0,\s*binding\s*=\s*1\)\s*readonly\s+buffer\s+LightBuffer'
        MinShaders = 1
    }
    @{
        Name    = 'FGpuDrawObject'
        Header  = 'Source/Runtime/RenderCore/Public/RenderCore/Culling/FGpuDraw.h'
        Set     = 3
        Binding = 0
        Marker  = 'set\s*=\s*3,\s*binding\s*=\s*0\)\s*readonly\s+buffer\s+ObjectBuffer'
        # 三个: pbr.vert / gbuffer.vert / depth_only.vert。深度预通道、前向
        # 通道、阴影通道读的必须是同一份逐物体数据 —— 前向的深度测试是 Equal,
        # 两处的模型矩阵差一点点整片几何就消失。
        MinShaders = 3
    }
    @{
        Name    = 'FGpuDrawObject'
        Header  = 'Source/Runtime/RenderCore/Public/RenderCore/Culling/FGpuDraw.h'
        Set     = 0
        Binding = 0
        Marker  = 'set\s*=\s*0,\s*binding\s*=\s*0\)\s*readonly\s+buffer\s+ObjectBuffer'
        MinShaders = 1
    }
    @{
        Name    = 'FSpotShadowData'
        Header  = 'Source/Runtime/RenderCore/Public/RenderCore/Lighting/FShadowAtlas.h'
        Set     = 2
        Binding = 9
        Marker  = 'set\s*=\s*2,\s*binding\s*=\s*9\)\s*readonly\s+buffer\s+SpotShadowBuffer'
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

$expected['FViewPushConstant'] =
    Get-ExpectedSize -Path $pushHeader -Name 'FViewPushConstant'

if ($expected['FViewPushConstant'] -lt 0) { exit 1 }

foreach ($s in $structs)
{
    $expected[$s.Name] = Get-ExpectedSize -Path $s.Header -Name $s.Name

    if ($expected[$s.Name] -lt 0) { exit 1 }
}

foreach ($s in $storageStructs)
{
    $expected[$s.Name] = Get-ExpectedSize -Path $s.Header -Name $s.Name

    if ($expected[$s.Name] -lt 0) { exit 1 }
}

$offenders   = New-Object System.Collections.ArrayList
$checkedPush = 0
$checkedByStruct = @{}

# 计算着色器也在内 —— FLightData 被 light_cull.comp 读, 而它此前完全没有
# 被这个脚本覆盖过。
$allShaders = Get-ChildItem -Path Shaders -Recurse -Include *.vert, *.frag, *.comp

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

    }
}


# ---- push constant ----
#
# 单独扫一遍全部着色器, 不挂在 UBO 那张表下面。
#
# 原来它挂在 FViewProjUBO 那一条上 (即"包含 view_common.h 的着色器"), 而
# GPU 驱动那天 depth_only.vert 不再读 set 0 的 view/proj、也就不再 include
# 那个头 —— 于是唯一还在用 push constant 的着色器落到了扫描范围之外, 计数
# 归零。那道下限闸把它拦成了失败, 但错误信息指的是"push constant 太少",
# 而真正的原因在两条判据的耦合上。
foreach ($shader in $allShaders)
{
    $source = Get-Content $shader.FullName -Raw

    # 判据必须精确到**声明**, 而且要认块名。
    #
    # 曾经用的是"源码里有没有 materialIndex"—— pbr.vert 与 gbuffer.vert 删掉
    # push constant 之后新增了一个叫 fragMaterialIndex 的 flat varying, 子串
    # 照样命中, 于是脚本去比一个根本不存在的 push constant。
    #
    # 认块名还排除了遗留的 triangle.vert: 它自有一个 64 字节的 PushConstants
    # 块 (只有 mat4 model), 与 FViewPushConstant 无关。把它们一起比会得到一个
    # 恒红的检查, 而恒红的检查最终会被人注释掉。
    if ($source -notmatch 'push_constant\)\s*uniform\s+ViewPushConstants')
    {
        continue
    }

    $relative = $shader.FullName.Substring((Get-Location).Path.Length + 1)
    $json = 'Binaries/' + $relative.Replace([char]92, [char]47) + '.json'

    if (-not (Test-Path $json))
    {
        [void]$offenders.Add("$($shader.Name): 找不到反射文件 $json")
        continue
    }

    $reflection = Get-Content $json -Raw | ConvertFrom-Json

    $checkedPush++

    if ($reflection.push_constants.size -ne $expected['FViewPushConstant'])
    {
        [void]$offenders.Add(
            "$($shader.Name): push constant 是 $($reflection.push_constants.size) 字节, C++ 侧 FViewPushConstant 是 $($expected['FViewPushConstant'])")
    }
}

# ---- storage buffer: 比元素步长 ----
#
# 计数用 "名字/set/binding" 作键而不是名字: FLightData 在两个着色器里绑在
# 不同位置, 只按名字计数的话, 其中一处一个都没查到也会被另一处的计数掩盖。
$checkedByStorage = @{}

foreach ($s in $storageStructs)
{
    $key = "$($s.Name)/$($s.Set)/$($s.Binding)"
    $checkedByStorage[$key] = 0

    $shaders = $allShaders |
               Where-Object { (Get-Content $_.FullName -Raw) -match $s.Marker }

    foreach ($shader in $shaders)
    {
        $relative = $shader.FullName.Substring((Get-Location).Path.Length + 1)
        $json = 'Binaries/' + $relative.Replace([char]92, [char]47) + '.json'

        if (-not (Test-Path $json))
        {
            [void]$offenders.Add("$($shader.Name): 找不到反射文件 $json")
            continue
        }

        $reflection = Get-Content $json -Raw | ConvertFrom-Json

        $ssbo = $reflection.storage_buffers |
                Where-Object { $_.set -eq $s.Set -and $_.binding -eq $s.Binding }

        if ($null -eq $ssbo)
        {
            [void]$offenders.Add(
                "$($shader.Name): 反射里没有 set $($s.Set) / binding $($s.Binding) 的 storage buffer ($($s.Name))")
            continue
        }

        $stride = $ssbo.members[0].array_stride

        $checkedByStorage[$key]++

        if ($stride -ne $expected[$s.Name])
        {
            [void]$offenders.Add(
                "$($shader.Name): set$($s.Set)/binding$($s.Binding) 元素步长 $stride 字节, C++ 侧 $($s.Name) 是 $($expected[$s.Name])")
        }
    }
}

foreach ($s in $storageStructs)
{
    $key = "$($s.Name)/$($s.Set)/$($s.Binding)"

    if ($checkedByStorage[$key] -lt $s.MinShaders)
    {
        Write-Host ("只检查到 {0} 个着色器的 {1} (set{2}/binding{3}, 预期至少 {4} 个) — 判定无效" -f `
            $checkedByStorage[$key], $s.Name, $s.Set, $s.Binding, $s.MinShaders) -ForegroundColor Red
        foreach ($offender in $offenders) { Write-Host "  $offender" }
        exit 1
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

# 下限从 3 降到 1。
#
# 曾经是 3 (depth_only.vert / gbuffer.vert / pbr.vert)。后两者在 GPU 驱动
# 那天把 push constant 整块删了 —— 逐物体数据搬进了 set 3 的 storage buffer,
# 因为间接绘制根本没有逐 draw 推送 push constant 这回事。
#
# 覆盖面并没有变窄: 上面新增的 storage buffer 那张表把 FGpuDrawObject 在
# 三个着色器里的元素步长都钉住了, 而那正是原来 push constant 承担的角色。
if ($checkedPush -lt 1) {
    Write-Host "只检查到 $checkedPush 个 push constant (预期至少 1 个) — 判定无效" -ForegroundColor Red
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

# storage buffer 也要出现在这一行。加了检查却不报出来的话, 它哪天悄悄退化成
# "一个着色器都没匹配到"就没人看得见 —— 而 MinShaders 那道闸只在计数为零时
# 才拦得住, 拦不住"本该三处只查了一处"。
$storageSummary = ($storageStructs | ForEach-Object {
    "{0}×{1}[s{2}b{3}]步长{4}B" -f `
        $checkedByStorage["$($_.Name)/$($_.Set)/$($_.Binding)"], `
        $_.Name, $_.Set, $_.Binding, $expected[$_.Name]
}) -join ', '

Write-Host "布局一致: $summary, push×$checkedPush=$($expected['FViewPushConstant'])B" -ForegroundColor Green
Write-Host "         $storageSummary" -ForegroundColor Green
exit 0
