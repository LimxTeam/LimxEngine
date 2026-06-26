/*******************************************************************************
 * 文件: FRect.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   2D 矩形 — 轴对齐矩形 (位置+尺寸)
 *   提供相交检测、包含判断、合并、裁剪等常用操作
 *   用于 UI 布局、2D 碰撞、视口裁剪、纹理区域等场景
 *
 * 设计哲学:
 *   位置+尺寸 — 以左上角 (X, Y) 和宽高 (Width, Height) 表示
 *   值类型 — 轻量 POD 风格，可自由拷贝
 *   静态工厂 — 提供 FromMinMax/FromCenterExtent 等便捷构造
 *
 * 技术特性:
 *   - FRect: 2D 轴对齐矩形
 *   - Contains: 点/矩形包含
 *   - Intersects: 相交检测
 *   - Intersection: 求交集矩形
 *   - Union: 求并集矩形
 *   - Expand/Shrink: 膨胀/收缩
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FMath.h,
 *          Core/Math/FVector.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"

namespace Limx
{

/// 2D 轴对齐矩形
struct FRect
{
    Float32 X;       ///< 左上角 X
    Float32 Y;       ///< 左上角 Y
    Float32 Width;   ///< 宽度
    Float32 Height;  ///< 高度

    // ========================================================================
    // 构造
    // ========================================================================

    FRect()
        : X(0.0f), Y(0.0f), Width(0.0f), Height(0.0f)
    {
    }

    FRect(Float32 x, Float32 y, Float32 width, Float32 height)
        : X(x), Y(y), Width(width), Height(height)
    {
    }

    /// 从最小/最大点构造
    LIMX_NODISCARD static FRect FromMinMax(
        Float32 minX, Float32 minY,
        Float32 maxX, Float32 maxY)
    {
        return FRect(minX, minY, maxX - minX, maxY - minY);
    }

    /// 从中心和半尺寸构造
    LIMX_NODISCARD static FRect FromCenterExtent(
        Float32 centerX, Float32 centerY,
        Float32 halfWidth, Float32 halfHeight)
    {
        return FRect(centerX - halfWidth,
                     centerY - halfHeight,
                     halfWidth * 2.0f,
                     halfHeight * 2.0f);
    }

    /// 从 FVector2 位置和尺寸构造
    LIMX_NODISCARD static FRect FromPositionSize(
        const FVector2& position, const FVector2& size)
    {
        return FRect(position.X, position.Y, size.X, size.Y);
    }

    // ========================================================================
    // 边界查询
    // ========================================================================

    LIMX_NODISCARD Float32 GetLeft() const { return X; }
    LIMX_NODISCARD Float32 GetTop() const { return Y; }
    LIMX_NODISCARD Float32 GetRight() const
    {
        return X + Width;
    }
    LIMX_NODISCARD Float32 GetBottom() const
    {
        return Y + Height;
    }

    LIMX_NODISCARD FVector2 GetPosition() const
    {
        return FVector2(X, Y);
    }

    LIMX_NODISCARD FVector2 GetSize() const
    {
        return FVector2(Width, Height);
    }

    LIMX_NODISCARD FVector2 GetCenter() const
    {
        return FVector2(X + Width * 0.5f,
                        Y + Height * 0.5f);
    }

    LIMX_NODISCARD FVector2 GetMin() const
    {
        return FVector2(X, Y);
    }

    LIMX_NODISCARD FVector2 GetMax() const
    {
        return FVector2(X + Width, Y + Height);
    }

    LIMX_NODISCARD Float32 GetArea() const
    {
        return Width * Height;
    }

    LIMX_NODISCARD Float32 GetPerimeter() const
    {
        return 2.0f * (Width + Height);
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return Width <= 0.0f || Height <= 0.0f;
    }

    // ========================================================================
    // 包含判断
    // ========================================================================

    /// 点是否在矩形内
    LIMX_NODISCARD bool ContainsPoint(
        Float32 px, Float32 py) const
    {
        return px >= X && px <= X + Width &&
               py >= Y && py <= Y + Height;
    }

    LIMX_NODISCARD bool ContainsPoint(
        const FVector2& point) const
    {
        return ContainsPoint(point.X, point.Y);
    }

    /// 另一个矩形是否完全包含在内
    LIMX_NODISCARD bool ContainsRect(const FRect& other) const
    {
        return other.X >= X &&
               other.Y >= Y &&
               other.GetRight() <= GetRight() &&
               other.GetBottom() <= GetBottom();
    }

    // ========================================================================
    // 相交
    // ========================================================================

    /// 是否与另一个矩形相交
    LIMX_NODISCARD bool Intersects(const FRect& other) const
    {
        return X < other.GetRight() &&
               GetRight() > other.X &&
               Y < other.GetBottom() &&
               GetBottom() > other.Y;
    }

    /// 求交集矩形 (不相交返回空矩形)
    LIMX_NODISCARD FRect Intersection(
        const FRect& other) const
    {
        Float32 newLeft = FMath::Max(X, other.X);
        Float32 newTop = FMath::Max(Y, other.Y);
        Float32 newRight = FMath::Min(
            GetRight(), other.GetRight());
        Float32 newBottom = FMath::Min(
            GetBottom(), other.GetBottom());

        if (newRight <= newLeft || newBottom <= newTop)
        {
            return FRect();
        }

        return FRect(newLeft, newTop,
                     newRight - newLeft,
                     newBottom - newTop);
    }

    // ========================================================================
    // 合并
    // ========================================================================

    /// 求并集矩形 (包含两个矩形的最小矩形)
    LIMX_NODISCARD FRect Union(const FRect& other) const
    {
        if (IsEmpty()) return other;
        if (other.IsEmpty()) return *this;

        Float32 newLeft = FMath::Min(X, other.X);
        Float32 newTop = FMath::Min(Y, other.Y);
        Float32 newRight = FMath::Max(
            GetRight(), other.GetRight());
        Float32 newBottom = FMath::Max(
            GetBottom(), other.GetBottom());

        return FRect(newLeft, newTop,
                     newRight - newLeft,
                     newBottom - newTop);
    }

    // ========================================================================
    // 变换
    // ========================================================================

    /// 膨胀 (四边各扩展 amount)
    LIMX_NODISCARD FRect Expand(Float32 amount) const
    {
        return FRect(X - amount, Y - amount,
                     Width + amount * 2.0f,
                     Height + amount * 2.0f);
    }

    /// 收缩
    LIMX_NODISCARD FRect Shrink(Float32 amount) const
    {
        return Expand(-amount);
    }

    /// 平移
    LIMX_NODISCARD FRect Translate(
        Float32 dx, Float32 dy) const
    {
        return FRect(X + dx, Y + dy, Width, Height);
    }

    LIMX_NODISCARD FRect Translate(
        const FVector2& offset) const
    {
        return Translate(offset.X, offset.Y);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(const FRect& other) const
    {
        return FMath::Abs(X - other.X) < 1e-6f &&
               FMath::Abs(Y - other.Y) < 1e-6f &&
               FMath::Abs(Width - other.Width) < 1e-6f &&
               FMath::Abs(Height - other.Height) < 1e-6f;
    }

    LIMX_NODISCARD bool operator!=(const FRect& other) const
    {
        return !(*this == other);
    }
};

} // namespace Limx
