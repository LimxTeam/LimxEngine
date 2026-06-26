/*******************************************************************************
 * 文件: FInterpCurve.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   插值曲线 — 关键帧驱动的值动画曲线
 *   支持多种插值模式: 阶梯、线性、三次 Hermite
 *   用于属性动画、时间轴编辑器、粒子参数曲线等场景
 *
 * 设计哲学:
 *   关键帧 — 每个关键帧包含时间、值、切线和插值模式
 *   多模式 — 同一曲线内不同段可使用不同插值方式
 *   Float32 — 单精度浮点值曲线，复合类型可用多条曲线组合
 *
 * 技术特性:
 *   - EInterpMode: 插值模式枚举 (Step/Linear/Cubic)
 *   - FInterpKey: 关键帧
 *   - FInterpCurve: 插值曲线
 *   - AddKey: 添加关键帧
 *   - Evaluate: 按时间求值
 *   - AutoComputeTangents: 自动计算切线
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FMath.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 插值模式
enum class EInterpMode : UInt8
{
    Step   = 0, ///< 阶梯 — 保持前一帧值直到下一帧
    Linear = 1, ///< 线性 — 两帧间线性插值
    Cubic  = 2  ///< 三次 Hermite — 使用切线的平滑插值
};

/// 插值关键帧
struct FInterpKey
{
    Float32     Time;         ///< 时间
    Float32     Value;        ///< 值
    Float32     ArriveTan;    ///< 到达切线
    Float32     LeaveTan;     ///< 离开切线
    EInterpMode InterpMode;   ///< 插值模式

    FInterpKey()
        : Time(0.0f)
        , Value(0.0f)
        , ArriveTan(0.0f)
        , LeaveTan(0.0f)
        , InterpMode(EInterpMode::Linear)
    {
    }

    FInterpKey(Float32 time, Float32 value,
               EInterpMode mode = EInterpMode::Linear)
        : Time(time)
        , Value(value)
        , ArriveTan(0.0f)
        , LeaveTan(0.0f)
        , InterpMode(mode)
    {
    }
};

/// 插值曲线
class FInterpCurve
{
public:
    FInterpCurve() = default;

    // ========================================================================
    // 关键帧管理
    // ========================================================================

    /// 添加关键帧 (自动按时间排序插入)
    SizeType AddKey(const FInterpKey& key)
    {
        SizeType insertIndex = m_Keys.GetSize();
        for (SizeType keyIndex = 0;
             keyIndex < m_Keys.GetSize(); ++keyIndex)
        {
            if (m_Keys[keyIndex].Time > key.Time)
            {
                insertIndex = keyIndex;
                break;
            }
        }

        if (insertIndex == m_Keys.GetSize())
        {
            m_Keys.Add(key);
        }
        else
        {
            m_Keys.Add(m_Keys[m_Keys.GetSize() - 1]);
            for (SizeType moveIdx = m_Keys.GetSize() - 2;
                 moveIdx > insertIndex; --moveIdx)
            {
                m_Keys[moveIdx] = m_Keys[moveIdx - 1];
            }
            m_Keys[insertIndex] = key;
        }

        return insertIndex;
    }

    /// 便捷添加
    SizeType AddKey(Float32 time, Float32 value,
                    EInterpMode mode = EInterpMode::Linear)
    {
        return AddKey(FInterpKey(time, value, mode));
    }

    /// 移除关键帧
    void RemoveKeyAt(SizeType index)
    {
        m_Keys.RemoveAt(index);
    }

    /// 清空
    void Clear() { m_Keys.Clear(); }

    /// 关键帧数
    LIMX_NODISCARD SizeType GetKeyCount() const
    {
        return m_Keys.GetSize();
    }

    /// 获取关键帧 (只读)
    LIMX_NODISCARD const FInterpKey& GetKey(
        SizeType index) const
    {
        return m_Keys[index];
    }

    /// 获取关键帧 (可写)
    LIMX_NODISCARD FInterpKey& GetKey(SizeType index)
    {
        return m_Keys[index];
    }

    // ========================================================================
    // 求值
    // ========================================================================

    /// 按时间求值
    LIMX_NODISCARD Float32 Evaluate(Float32 time) const
    {
        if (m_Keys.GetSize() == 0) return 0.0f;
        if (m_Keys.GetSize() == 1) return m_Keys[0].Value;

        // 夹持到首尾范围
        if (time <= m_Keys[0].Time)
        {
            return m_Keys[0].Value;
        }

        SizeType lastIdx = m_Keys.GetSize() - 1;
        if (time >= m_Keys[lastIdx].Time)
        {
            return m_Keys[lastIdx].Value;
        }

        // 查找区间
        SizeType segIdx = 0;
        for (SizeType keyIdx = 0;
             keyIdx < lastIdx; ++keyIdx)
        {
            if (time >= m_Keys[keyIdx].Time &&
                time <= m_Keys[keyIdx + 1].Time)
            {
                segIdx = keyIdx;
                break;
            }
        }

        const FInterpKey& keyA = m_Keys[segIdx];
        const FInterpKey& keyB = m_Keys[segIdx + 1];

        Float32 range = keyB.Time - keyA.Time;
        if (range < 1e-8f) return keyA.Value;

        Float32 alpha = (time - keyA.Time) / range;

        switch (keyA.InterpMode)
        {
        case EInterpMode::Step:
            return keyA.Value;

        case EInterpMode::Linear:
            return keyA.Value +
                   (keyB.Value - keyA.Value) * alpha;

        case EInterpMode::Cubic:
            return HermiteInterp(
                keyA.Value, keyA.LeaveTan * range,
                keyB.Value, keyB.ArriveTan * range,
                alpha);

        default:
            return keyA.Value;
        }
    }

    // ========================================================================
    // 切线计算
    // ========================================================================

    /// 自动计算所有关键帧的切线 (Catmull-Rom 风格)
    void AutoComputeTangents()
    {
        for (SizeType keyIdx = 0;
             keyIdx < m_Keys.GetSize(); ++keyIdx)
        {
            if (m_Keys[keyIdx].InterpMode !=
                EInterpMode::Cubic)
            {
                continue;
            }

            Float32 tangent = 0.0f;

            if (keyIdx == 0)
            {
                // 首帧 — 使用前向差分
                if (m_Keys.GetSize() > 1)
                {
                    Float32 dt = m_Keys[1].Time -
                                 m_Keys[0].Time;
                    if (dt > 1e-8f)
                    {
                        tangent = (m_Keys[1].Value -
                                   m_Keys[0].Value) / dt;
                    }
                }
            }
            else if (keyIdx == m_Keys.GetSize() - 1)
            {
                // 末帧 — 使用后向差分
                SizeType prev = keyIdx - 1;
                Float32 dt = m_Keys[keyIdx].Time -
                             m_Keys[prev].Time;
                if (dt > 1e-8f)
                {
                    tangent = (m_Keys[keyIdx].Value -
                               m_Keys[prev].Value) / dt;
                }
            }
            else
            {
                // 中间帧 — 使用中心差分
                SizeType prev = keyIdx - 1;
                SizeType next = keyIdx + 1;
                Float32 dt = m_Keys[next].Time -
                             m_Keys[prev].Time;
                if (dt > 1e-8f)
                {
                    tangent = (m_Keys[next].Value -
                               m_Keys[prev].Value) / dt;
                }
            }

            m_Keys[keyIdx].ArriveTan = tangent;
            m_Keys[keyIdx].LeaveTan = tangent;
        }
    }

    // ========================================================================
    // 时间范围
    // ========================================================================

    /// 曲线起始时间
    LIMX_NODISCARD Float32 GetStartTime() const
    {
        if (m_Keys.GetSize() == 0) return 0.0f;
        return m_Keys[0].Time;
    }

    /// 曲线结束时间
    LIMX_NODISCARD Float32 GetEndTime() const
    {
        if (m_Keys.GetSize() == 0) return 0.0f;
        return m_Keys[m_Keys.GetSize() - 1].Time;
    }

    /// 曲线时长
    LIMX_NODISCARD Float32 GetDuration() const
    {
        return GetEndTime() - GetStartTime();
    }

private:
    /// 三次 Hermite 插值
    /// h00(t) = 2t³ - 3t² + 1
    /// h10(t) = t³ - 2t² + t
    /// h01(t) = -2t³ + 3t²
    /// h11(t) = t³ - t²
    static Float32 HermiteInterp(
        Float32 p0, Float32 m0,
        Float32 p1, Float32 m1, Float32 t)
    {
        Float32 tt = t * t;
        Float32 ttt = tt * t;

        Float32 h00 = 2.0f * ttt - 3.0f * tt + 1.0f;
        Float32 h10 = ttt - 2.0f * tt + t;
        Float32 h01 = -2.0f * ttt + 3.0f * tt;
        Float32 h11 = ttt - tt;

        return h00 * p0 + h10 * m0 +
               h01 * p1 + h11 * m1;
    }

    TArray<FInterpKey> m_Keys;  ///< 关键帧列表 (按时间排序)
};

} // namespace Limx
