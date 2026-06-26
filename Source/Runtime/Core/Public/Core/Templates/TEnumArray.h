/*******************************************************************************
 * 文件: TEnumArray.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   枚举索引数组 — 以枚举值为索引的编译时固定数组
 *   类型安全地使用枚举值访问数组元素，避免硬编码整数索引
 *   用于渲染通道表、材质槽、输入绑定、状态映射表等场景
 *
 * 设计哲学:
 *   类型安全 — operator[] 只接受 EnumType，禁止整数索引
 *   编译时大小 — EnumCount 为模板参数，内嵌数组无堆分配
 *   枚举约束 — 枚举值须在 [0, EnumCount) 范围内
 *
 * 技术特性:
 *   - TEnumArray<EnumType, T, EnumCount>: 枚举索引数组
 *   - operator[]: 枚举索引访问
 *   - Fill: 全量填充
 *   - begin/end: 范围 for 支持
 *   - GetSize: 固定大小
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 枚举索引数组
/// @tparam EnumType 枚举类型 (须可转换为 SizeType)
/// @tparam T 元素类型
/// @tparam EnumCount 枚举值数量 (数组大小)
template<typename EnumType, typename T, SizeType EnumCount>
class TEnumArray
{
    static_assert(EnumCount > 0,
        "EnumCount must be > 0");

public:
    /// 默认构造 — 值初始化所有元素
    TEnumArray()
    {
        for (SizeType elemIdx = 0;
             elemIdx < EnumCount; ++elemIdx)
        {
            m_Data[elemIdx] = T{};
        }
    }

    /// 使用填充值构造
    explicit TEnumArray(const T& fillValue)
    {
        Fill(fillValue);
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD T& operator[](EnumType enumValue)
    {
        SizeType index =
            static_cast<SizeType>(enumValue);
        LIMX_ASSERT(index < EnumCount);
        return m_Data[index];
    }

    LIMX_NODISCARD const T& operator[](
        EnumType enumValue) const
    {
        SizeType index =
            static_cast<SizeType>(enumValue);
        LIMX_ASSERT(index < EnumCount);
        return m_Data[index];
    }

    LIMX_NODISCARD T* GetData() { return m_Data; }
    LIMX_NODISCARD const T* GetData() const
    {
        return m_Data;
    }

    // ========================================================================
    // 修改
    // ========================================================================

    /// 填充所有元素
    void Fill(const T& value)
    {
        for (SizeType elemIdx = 0;
             elemIdx < EnumCount; ++elemIdx)
        {
            m_Data[elemIdx] = value;
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD static constexpr SizeType GetSize()
    {
        return EnumCount;
    }

    /// 按枚举值查找 (线性扫描)
    LIMX_NODISCARD bool Contains(const T& value) const
    {
        for (SizeType elemIdx = 0;
             elemIdx < EnumCount; ++elemIdx)
        {
            if (m_Data[elemIdx] == value) return true;
        }
        return false;
    }

    // ========================================================================
    // 范围 for
    // ========================================================================

    LIMX_NODISCARD T* begin() { return m_Data; }
    LIMX_NODISCARD const T* begin() const
    {
        return m_Data;
    }
    LIMX_NODISCARD T* end()
    {
        return m_Data + EnumCount;
    }
    LIMX_NODISCARD const T* end() const
    {
        return m_Data + EnumCount;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const TEnumArray& other) const
    {
        for (SizeType elemIdx = 0;
             elemIdx < EnumCount; ++elemIdx)
        {
            if (m_Data[elemIdx] != other.m_Data[elemIdx])
                return false;
        }
        return true;
    }

    LIMX_NODISCARD bool operator!=(
        const TEnumArray& other) const
    {
        return !(*this == other);
    }

private:
    T m_Data[EnumCount];  ///< 内嵌元素数组
};

} // namespace Limx
