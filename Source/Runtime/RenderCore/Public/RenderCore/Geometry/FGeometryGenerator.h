// ============================================================
// 文件名称：FGeometryGenerator.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：纯数据生成器 — 不持有 GPU 资源，仅输出 CPU 端顶点/索引
//          数组，由调用方 (FRenderer) 负责上传到 GPU。
// 功能描述：程序化基础几何体生成 — 立方体、平面、球体 (UV 球)。
//          输出交错布局顶点 (位置+法线+颜色+UV) 与 UInt16 索引数组。
// 技术特性：全部 static 方法，无状态，可在任意线程调用；
//          顶点布局与着色器 location 对应 (0=位置, 1=法线, 2=颜色, 3=UV)；
//          索引使用 UInt16 (最大 65535 顶点，基础几何体足够)。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ GenerateCube()           │ 生成单位立方体 (24顶点, 36索引)   │
// │ GeneratePlane()          │ 生成 XZ 平面 (细分可选)          │
// │ GenerateSphere()         │ 生成 UV 球体 (经纬细分可选)       │
//
// ── 结构体表 ──────────────────────────────────────────────────
// │ 结构体名                   │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ FMeshVertex              │ 顶点数据 (位置+法线+颜色+UV)      │
// │ FMeshData                │ 网格数据 (顶点数组+索引数组)       │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// │ 2026-04-07  │ LimxTeam  │ M0.3 添加 UV 坐标支持纹理采样   │
// ============================================================

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

namespace Limx
{

// ============================================================================
// FMeshVertex — 网格顶点布局 (位置 + 法线 + 颜色 + UV)
// ============================================================================

struct FMeshVertex
{
    FVector3 Position;   // location 0
    FVector3 Normal;     // location 1
    FVector3 Color;      // location 2
    FVector2 TexCoord;   // location 3
};

// ============================================================================
// FMeshData — 生成的网格数据 (CPU 端)
// ============================================================================

struct FMeshData
{
    TArray<FMeshVertex> Vertices;
    TArray<UInt16>      Indices;
};

// ============================================================================
// FGeometryGenerator — 程序化几何体生成器
// ============================================================================

class FGeometryGenerator
{
public:
    /// 生成单位立方体 (中心在原点, 边长 1.0)
    /// 24 顶点 (每面 4 个, 独立法线), 36 索引
    static FMeshData GenerateCube();

    /// 生成 XZ 平面 (中心在原点, Y=0)
    /// @param width      宽度 (X 方向)
    /// @param depth      深度 (Z 方向)
    /// @param subdivX    X 方向细分数
    /// @param subdivZ    Z 方向细分数
    static FMeshData GeneratePlane(Float32 width, Float32 depth,
                                    UInt32 subdivX = 1, UInt32 subdivZ = 1);

    /// 生成 UV 球体 (中心在原点)
    /// @param radius     半径
    /// @param slices     经度切分数 (水平)
    /// @param stacks     纬度切分数 (垂直)
    static FMeshData GenerateSphere(Float32 radius,
                                     UInt32 slices = 32, UInt32 stacks = 16);
};

} // namespace Limx
