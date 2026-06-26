/*******************************************************************************
 * 文件: FColorGradient.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   颜色梯度 — 多关键帧颜色的线性插值
 *   支持任意数量的颜色控制点，按参数 t∈[0,1] 插值
 *   用于粒子系统颜色变化、热力图着色、编辑器色带等场景
 *
 * 设计哲学:
 *   有序关键帧 — 控制点按 t 值排序，二分查找插值区间
 *   线性插值 — 相邻关键帧间 LERP，首尾外夹持
 *   FLinearColor — 在线性色彩空间中插值，保证物理正确
 *
 * 技术特性:
 *   - FColorGradient: 颜色梯度
 *   - AddKey: 添加关键帧
 *   - Evaluate: 按 t 求值
 *   - GetKeyCount: 关键帧数
 *   - Clear: 清空
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FColor.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FColor.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 颜色梯度关键帧
struct FColorGradientKey
{
    Float32      Time;   ///< 参数 t∈[0,1]
    FLinearColor Color;  ///< 线性空间颜色

    FColorGradientKey()
        : Time(0.0f)
        , Color(0.0f, 0.0f, 0.0f, 1.0f)
    {
    }

    FColorGradientKey(Float32 time, const FLinearColor& color)
        : Time(time), Color(color)
    {
    }
};

/// 颜色梯度
class FColorGradient
{
public:
    FColorGradient() = default;

    // ========================================================================
    // 关键帧管理
    // ========================================================================

    /// 添加关键帧 (自动按 Time 排序插入)
    void AddKey(Float32 time, const FLinearColor& color)
    {
        FColorGradientKey key(time, color);

        // 查找插入位置 (保持排序)
        SizeType insertIndex = m_Keys.GetSize();
        for (SizeType keyIndex = 0;
             keyIndex < m_Keys.GetSize(); ++keyIndex)
        {
            if (m_Keys[keyIndex].Time > time)
            {
                insertIndex = keyIndex;
                break;
            }
        }

        // 在指定位置插入
        if (insertIndex == m_Keys.GetSize())
        {
            m_Keys.Add(key);
        }
        else
        {
            // 先追加一个空元素，然后后移
            m_Keys.Add(m_Keys[m_Keys.GetSize() - 1]);
            for (SizeType moveIndex = m_Keys.GetSize() - 2;
                 moveIndex > insertIndex; --moveIndex)
            {
                m_Keys[moveIndex] = m_Keys[moveIndex - 1];
            }
            m_Keys[insertIndex] = key;
        }
    }

    /// 清空所有关键帧
    void Clear() { m_Keys.Clear(); }

    /// 关键帧数
    LIMX_NODISCARD SizeType GetKeyCount() const
    {
        return m_Keys.GetSize();
    }

    /// 获取关键帧
    LIMX_NODISCARD const FColorGradientKey& GetKey(
        SizeType index) const
    {
        return m_Keys[index];
    }

    // ========================================================================
    // 求值
    // ========================================================================

    /// 按参数 t 求值
    /// @param time 参数值 (通常 0~1，但不强制)
    /// @return 插值后的颜色
    LIMX_NODISCARD FLinearColor Evaluate(Float32 time) const
    {
        if (m_Keys.GetSize() == 0)
        {
            return FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }

        if (m_Keys.GetSize() == 1)
        {
            return m_Keys[0].Color;
        }

        // 夹持到首尾范围
        if (time <= m_Keys[0].Time)
        {
            return m_Keys[0].Color;
        }

        SizeType lastIndex = m_Keys.GetSize() - 1;
        if (time >= m_Keys[lastIndex].Time)
        {
            return m_Keys[lastIndex].Color;
        }

        // 查找插值区间
        SizeType segmentIndex = 0;
        for (SizeType keyIndex = 0;
             keyIndex < lastIndex; ++keyIndex)
        {
            if (time >= m_Keys[keyIndex].Time &&
                time <= m_Keys[keyIndex + 1].Time)
            {
                segmentIndex = keyIndex;
                break;
            }
        }

        const FColorGradientKey& keyA = m_Keys[segmentIndex];
        const FColorGradientKey& keyB = m_Keys[segmentIndex + 1];

        // 计算区间内的归一化参数
        Float32 range = keyB.Time - keyA.Time;
        Float32 alpha = 0.0f;
        if (range > 1e-8f)
        {
            alpha = (time - keyA.Time) / range;
        }

        // 线性插值
        return LerpColor(keyA.Color, keyB.Color, alpha);
    }

private:
    /// 线性颜色插值
    LIMX_NODISCARD static FLinearColor LerpColor(
        const FLinearColor& a, const FLinearColor& b,
        Float32 alpha)
    {
        FLinearColor result;
        result.R = a.R + (b.R - a.R) * alpha;
        result.G = a.G + (b.G - a.G) * alpha;
        result.B = a.B + (b.B - a.B) * alpha;
        result.A = a.A + (b.A - a.A) * alpha;
        return result;
    }

    TArray<FColorGradientKey> m_Keys;  ///< 关键帧列表 (按 Time 排序)
};

} // namespace Limx
