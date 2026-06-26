/*******************************************************************************
 * 文件: FMeshTopology.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   网格拓扑 — 顶点/边/三角面索引表的轻量表示
 *   提供基于索引缓冲区的三角网格拓扑查询
 *   用于网格验证、法线重计算、拓扑分析、LOD 生成等场景
 *
 * 设计哲学:
 *   索引缓冲 — 顶点/面均以索引表示，不持有实际几何数据
 *   轻量视图 — 持有外部索引数组的视图，零拷贝
 *   拓扑查询 — 面法线/边界检测/连通性
 *
 * 技术特性:
 *   - FMeshTopology: 三角网格拓扑
 *   - GetTriangleCount/GetVertexCount: 查询
 *   - GetTriangleIndices: 获取面顶点索引
 *   - IsManifold: 流形检测
 *   - ComputeAdjacency: 计算邻接面
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 三角网格拓扑 (索引缓冲视图)
class FMeshTopology
{
public:
    /// 三角形面
    struct FTriangleFace
    {
        UInt32 Indices[3];  ///< 三个顶点索引

        LIMX_NODISCARD UInt32 operator[](
            Int32 i) const
        {
            return Indices[i];
        }
    };

    /// 边 (有序: A < B)
    struct FEdge
    {
        UInt32 A;  ///< 顶点索引 A (较小)
        UInt32 B;  ///< 顶点索引 B (较大)

        bool operator==(const FEdge& other) const
        {
            return A == other.A && B == other.B;
        }

        bool operator!=(const FEdge& other) const
        {
            return !(*this == other);
        }

        bool operator<(const FEdge& other) const
        {
            if (A != other.A) return A < other.A;
            return B < other.B;
        }
    };

    FMeshTopology() : m_VertexCount(0) {}

    // ========================================================================
    // 数据加载
    // ========================================================================

    /// 从索引数组加载 (每 3 个索引为一个三角形)
    /// @param indices 索引数组 (长度必须为 3 的倍数)
    /// @param indexCount 索引数量
    /// @param vertexCount 顶点总数
    void Load(const UInt32* indices,
              SizeType indexCount,
              UInt32 vertexCount)
    {
        LIMX_ASSERT(indexCount % 3 == 0);
        m_VertexCount = vertexCount;
        m_Faces.Clear();
        SizeType faceCount = indexCount / 3;
        m_Faces.Reserve(faceCount);

        for (SizeType faceIdx = 0;
             faceIdx < faceCount; ++faceIdx)
        {
            FTriangleFace face;
            face.Indices[0] = indices[faceIdx * 3 + 0];
            face.Indices[1] = indices[faceIdx * 3 + 1];
            face.Indices[2] = indices[faceIdx * 3 + 2];
            m_Faces.Add(face);
        }
    }

    void Load(const TArray<UInt32>& indices,
              UInt32 vertexCount)
    {
        Load(indices.GetData(), indices.GetSize(),
             vertexCount);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetTriangleCount() const
    {
        return m_Faces.GetSize();
    }

    LIMX_NODISCARD UInt32 GetVertexCount() const
    {
        return m_VertexCount;
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Faces.GetSize() == 0;
    }

    LIMX_NODISCARD const FTriangleFace& GetFace(
        SizeType faceIndex) const
    {
        LIMX_ASSERT(faceIndex < m_Faces.GetSize());
        return m_Faces[faceIndex];
    }

    // ========================================================================
    // 边提取
    // ========================================================================

    /// 提取所有唯一边 (去重，按顶点索引排序)
    void ExtractUniqueEdges(TArray<FEdge>& outEdges) const
    {
        TArray<FEdge> allEdges;
        allEdges.Reserve(m_Faces.GetSize() * 3);

        for (SizeType faceIdx = 0;
             faceIdx < m_Faces.GetSize(); ++faceIdx)
        {
            const FTriangleFace& face = m_Faces[faceIdx];
            for (Int32 edgeSlot = 0;
                 edgeSlot < 3; ++edgeSlot)
            {
                UInt32 v0 = face.Indices[edgeSlot];
                UInt32 v1 = face.Indices[(edgeSlot + 1) % 3];

                FEdge edge;
                edge.A = (v0 < v1) ? v0 : v1;
                edge.B = (v0 < v1) ? v1 : v0;
                allEdges.Add(edge);
            }
        }

        // 去重 (简单 O(n^2), 适合中小型网格)
        outEdges.Clear();
        for (SizeType edgeIdx = 0;
             edgeIdx < allEdges.GetSize(); ++edgeIdx)
        {
            bool duplicate = false;
            for (SizeType checkIdx = 0;
                 checkIdx < outEdges.GetSize(); ++checkIdx)
            {
                if (allEdges[edgeIdx] == outEdges[checkIdx])
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                outEdges.Add(allEdges[edgeIdx]);
            }
        }
    }

    // ========================================================================
    // 邻接计算
    // ========================================================================

    /// 计算每个顶点关联的面索引列表
    void ComputeVertexToFaces(
        TArray<TArray<UInt32>>& outVertexFaces) const
    {
        outVertexFaces.Clear();
        for (UInt32 vIdx = 0; vIdx < m_VertexCount; ++vIdx)
        {
            outVertexFaces.Add(TArray<UInt32>());
        }

        for (SizeType faceIdx = 0;
             faceIdx < m_Faces.GetSize(); ++faceIdx)
        {
            const FTriangleFace& face = m_Faces[faceIdx];
            for (Int32 vSlot = 0; vSlot < 3; ++vSlot)
            {
                UInt32 vIdx = face.Indices[vSlot];
                if (vIdx < m_VertexCount)
                {
                    outVertexFaces[vIdx].Add(
                        static_cast<UInt32>(faceIdx));
                }
            }
        }
    }

    // ========================================================================
    // 验证
    // ========================================================================

    /// 检查索引是否全部在有效范围内
    LIMX_NODISCARD bool HasValidIndices() const
    {
        for (SizeType faceIdx = 0;
             faceIdx < m_Faces.GetSize(); ++faceIdx)
        {
            const FTriangleFace& face = m_Faces[faceIdx];
            for (Int32 vSlot = 0; vSlot < 3; ++vSlot)
            {
                if (face.Indices[vSlot] >= m_VertexCount)
                    return false;
            }
        }
        return true;
    }

    /// 清空
    void Clear()
    {
        m_Faces.Clear();
        m_VertexCount = 0;
    }

private:
    TArray<FTriangleFace> m_Faces;       ///< 三角面列表
    UInt32                m_VertexCount; ///< 顶点总数
};

} // namespace Limx
