/*******************************************************************************
 * 文件: FPolygon2D.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   2D 多边形 — 顶点列表定义的平面多边形
 *   提供面积、周长、点包含测试、质心、凸性判断等操作
 *   用于 UI 碰撞区域、2D 物理形状、地图区域标注等场景
 *
 * 设计哲学:
 *   顶点列表 — 按顺序连接的顶点序列，隐式闭合
 *   Shoelace 公式 — 计算有符号面积 (正=逆时针, 负=顺时针)
 *   光线投射 — 点包含测试使用奇偶规则
 *
 * 技术特性:
 *   - FPolygon2D: 2D 多边形
 *   - AddVertex: 添加顶点
 *   - GetArea: 面积 (Shoelace)
 *   - GetPerimeter: 周长
 *   - ContainsPoint: 点包含测试 (射线法)
 *   - GetCentroid: 质心
 *   - IsConvex: 凸性判断
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FVector.h,
 *          Core/Math/FMath.h, Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 2D 多边形
class FPolygon2D
{
public:
    FPolygon2D() = default;

    // ========================================================================
    // 顶点管理
    // ========================================================================

    /// 添加顶点
    void AddVertex(const FVector2& vertex)
    {
        m_Vertices.Add(vertex);
    }

    /// 添加顶点 (坐标)
    void AddVertex(Float32 x, Float32 y)
    {
        m_Vertices.Add(FVector2(x, y));
    }

    /// 设置顶点
    void SetVertex(SizeType index, const FVector2& vertex)
    {
        m_Vertices[index] = vertex;
    }

    /// 获取顶点
    LIMX_NODISCARD const FVector2& GetVertex(
        SizeType index) const
    {
        return m_Vertices[index];
    }

    /// 顶点数
    LIMX_NODISCARD SizeType GetVertexCount() const
    {
        return m_Vertices.GetSize();
    }

    /// 清空
    void Clear() { m_Vertices.Clear(); }

    /// 预分配
    void Reserve(SizeType capacity)
    {
        m_Vertices.Reserve(capacity);
    }

    // ========================================================================
    // 几何属性
    // ========================================================================

    /// 有符号面积 (Shoelace 公式)
    /// 正值=逆时针, 负值=顺时针
    LIMX_NODISCARD Float32 GetSignedArea() const
    {
        SizeType vertexCount = m_Vertices.GetSize();
        if (vertexCount < 3) return 0.0f;

        Float32 area = 0.0f;
        for (SizeType vertIdx = 0;
             vertIdx < vertexCount; ++vertIdx)
        {
            SizeType nextIdx =
                (vertIdx + 1) % vertexCount;
            area += m_Vertices[vertIdx].X *
                    m_Vertices[nextIdx].Y;
            area -= m_Vertices[nextIdx].X *
                    m_Vertices[vertIdx].Y;
        }
        return area * 0.5f;
    }

    /// 面积 (绝对值)
    LIMX_NODISCARD Float32 GetArea() const
    {
        return FMath::Abs(GetSignedArea());
    }

    /// 周长
    LIMX_NODISCARD Float32 GetPerimeter() const
    {
        SizeType vertexCount = m_Vertices.GetSize();
        if (vertexCount < 2) return 0.0f;

        Float32 perimeter = 0.0f;
        for (SizeType vertIdx = 0;
             vertIdx < vertexCount; ++vertIdx)
        {
            SizeType nextIdx =
                (vertIdx + 1) % vertexCount;
            perimeter +=
                (m_Vertices[nextIdx] -
                 m_Vertices[vertIdx]).Length();
        }
        return perimeter;
    }

    /// 质心
    LIMX_NODISCARD FVector2 GetCentroid() const
    {
        SizeType vertexCount = m_Vertices.GetSize();
        if (vertexCount == 0) return FVector2();
        if (vertexCount == 1) return m_Vertices[0];
        if (vertexCount == 2)
        {
            return (m_Vertices[0] + m_Vertices[1]) * 0.5f;
        }

        Float32 cx = 0.0f;
        Float32 cy = 0.0f;
        Float32 signedArea = 0.0f;

        for (SizeType vertIdx = 0;
             vertIdx < vertexCount; ++vertIdx)
        {
            SizeType nextIdx =
                (vertIdx + 1) % vertexCount;
            Float32 cross =
                m_Vertices[vertIdx].X *
                    m_Vertices[nextIdx].Y -
                m_Vertices[nextIdx].X *
                    m_Vertices[vertIdx].Y;
            signedArea += cross;
            cx += (m_Vertices[vertIdx].X +
                   m_Vertices[nextIdx].X) * cross;
            cy += (m_Vertices[vertIdx].Y +
                   m_Vertices[nextIdx].Y) * cross;
        }

        signedArea *= 0.5f;
        if (FMath::Abs(signedArea) < 1e-8f)
        {
            return FVector2();
        }

        Float32 factor = 1.0f / (6.0f * signedArea);
        return FVector2(cx * factor, cy * factor);
    }

    // ========================================================================
    // 包含测试
    // ========================================================================

    /// 点包含测试 (射线法 — 奇偶规则)
    LIMX_NODISCARD bool ContainsPoint(
        const FVector2& point) const
    {
        SizeType vertexCount = m_Vertices.GetSize();
        if (vertexCount < 3) return false;

        bool inside = false;
        SizeType prevIdx = vertexCount - 1;

        for (SizeType vertIdx = 0;
             vertIdx < vertexCount; ++vertIdx)
        {
            const FVector2& vi = m_Vertices[vertIdx];
            const FVector2& vj = m_Vertices[prevIdx];

            if ((vi.Y > point.Y) != (vj.Y > point.Y))
            {
                Float32 intersectX =
                    (vj.X - vi.X) *
                    (point.Y - vi.Y) /
                    (vj.Y - vi.Y) + vi.X;

                if (point.X < intersectX)
                {
                    inside = !inside;
                }
            }

            prevIdx = vertIdx;
        }

        return inside;
    }

    // ========================================================================
    // 凸性判断
    // ========================================================================

    /// 是否为凸多边形
    LIMX_NODISCARD bool IsConvex() const
    {
        SizeType vertexCount = m_Vertices.GetSize();
        if (vertexCount < 3) return false;

        bool hasPositive = false;
        bool hasNegative = false;

        for (SizeType vertIdx = 0;
             vertIdx < vertexCount; ++vertIdx)
        {
            SizeType nextIdx =
                (vertIdx + 1) % vertexCount;
            SizeType nextNextIdx =
                (vertIdx + 2) % vertexCount;

            FVector2 edge1 =
                m_Vertices[nextIdx] - m_Vertices[vertIdx];
            FVector2 edge2 =
                m_Vertices[nextNextIdx] -
                m_Vertices[nextIdx];

            // 2D 叉积 (z 分量)
            Float32 cross =
                edge1.X * edge2.Y - edge1.Y * edge2.X;

            if (cross > 0.0f) hasPositive = true;
            if (cross < 0.0f) hasNegative = true;

            if (hasPositive && hasNegative) return false;
        }

        return true;
    }

    /// 是否为顺时针绕向
    LIMX_NODISCARD bool IsClockwise() const
    {
        return GetSignedArea() < 0.0f;
    }

    /// 反转顶点顺序
    void Reverse()
    {
        SizeType vertexCount = m_Vertices.GetSize();
        for (SizeType swapIdx = 0;
             swapIdx < vertexCount / 2; ++swapIdx)
        {
            SizeType mirrorIdx =
                vertexCount - 1 - swapIdx;
            FVector2 temp = m_Vertices[swapIdx];
            m_Vertices[swapIdx] = m_Vertices[mirrorIdx];
            m_Vertices[mirrorIdx] = temp;
        }
    }

private:
    TArray<FVector2> m_Vertices;  ///< 顶点列表
};

} // namespace Limx
