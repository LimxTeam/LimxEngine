/*******************************************************************************
 * 文件: TSpan.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   非拥有连续内存视图 — 替代 std::span 的零 STL 依赖实现
 *   轻量引用类型，仅存储指针 + 长度，不管理内存生命周期
 *   用于函数参数传递连续缓冲区，避免拷贝和模板膨胀
 *
 * 设计哲学:
 *   非拥有 — 不分配/释放内存，仅引用外部数据
 *   零开销 — sizeof(TSpan) = 2 * sizeof(void*)，可安全按值传递
 *   隐式转换 — 从 TArray、C 数组、初始化列表等隐式构造
 *
 * 技术特性:
 *   - 存储: T* + SizeType (16 字节 on x64)
 *   - 构造: 从指针+长度、TArray、C 数组
 *   - 访问: operator[], GetData(), GetSize(), Front(), Back()
 *   - 切片: Subspan(), First(), Last()
 *   - 迭代: begin()/end() range-based for 支持
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

// TArray 前向声明
template<typename T> class TArray;

/// 非拥有连续内存视图
/// @tparam T 元素类型
template<typename T>
class TSpan
{
public:
    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 空视图
    constexpr TSpan() : m_Data(nullptr), m_Size(0) {}

    /// 从指针 + 长度构造
    constexpr TSpan(T* data, SizeType size)
        : m_Data(data), m_Size(size) {}

    /// 从 C 数组构造
    template<SizeType N>
    constexpr TSpan(T (&array)[N])
        : m_Data(array), m_Size(N) {}

    /// 从 TArray 构造
    TSpan(TArray<T>& array)
        : m_Data(array.GetData()), m_Size(array.GetSize()) {}

    /// 从 const TArray 构造 (仅限 const T 的 Span)
    TSpan(const TArray<typename RemoveConstT<T>>& array)
        : m_Data(array.GetData()), m_Size(array.GetSize()) {}

    // ========================================================================
    // 元素访问
    // ========================================================================

    LIMX_NODISCARD constexpr T& operator[](SizeType index) const
    {
        LIMX_ASSERT(index < m_Size);
        return m_Data[index];
    }

    LIMX_NODISCARD constexpr T* GetData() const { return m_Data; }
    LIMX_NODISCARD constexpr SizeType GetSize() const { return m_Size; }
    LIMX_NODISCARD constexpr bool IsEmpty() const { return m_Size == 0; }

    /// 获取字节大小
    LIMX_NODISCARD constexpr SizeType GetSizeBytes() const
    {
        return m_Size * sizeof(T);
    }

    LIMX_NODISCARD constexpr T& Front() const
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Data[0];
    }

    LIMX_NODISCARD constexpr T& Back() const
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Data[m_Size - 1];
    }

    // ========================================================================
    // 切片
    // ========================================================================

    /// 子视图 — 从 offset 开始取 count 个元素
    LIMX_NODISCARD constexpr TSpan Subspan(SizeType offset,
                                            SizeType count) const
    {
        LIMX_ASSERT(offset + count <= m_Size);
        return TSpan(m_Data + offset, count);
    }

    /// 子视图 — 从 offset 到末尾
    LIMX_NODISCARD constexpr TSpan Subspan(SizeType offset) const
    {
        LIMX_ASSERT(offset <= m_Size);
        return TSpan(m_Data + offset, m_Size - offset);
    }

    /// 前 N 个元素
    LIMX_NODISCARD constexpr TSpan First(SizeType count) const
    {
        LIMX_ASSERT(count <= m_Size);
        return TSpan(m_Data, count);
    }

    /// 后 N 个元素
    LIMX_NODISCARD constexpr TSpan Last(SizeType count) const
    {
        LIMX_ASSERT(count <= m_Size);
        return TSpan(m_Data + m_Size - count, count);
    }

    // ========================================================================
    // 迭代器
    // ========================================================================

    LIMX_NODISCARD constexpr T* begin() const { return m_Data; }
    LIMX_NODISCARD constexpr T* end() const { return m_Data + m_Size; }

private:
    T*       m_Data;
    SizeType m_Size;
};

/// 便捷工厂 — 从指针+长度创建
template<typename T>
LIMX_NODISCARD constexpr TSpan<T> MakeSpan(T* data, SizeType size)
{
    return TSpan<T>(data, size);
}

/// 便捷工厂 — 从 C 数组创建
template<typename T, SizeType N>
LIMX_NODISCARD constexpr TSpan<T> MakeSpan(T (&array)[N])
{
    return TSpan<T>(array, N);
}

} // namespace Limx
