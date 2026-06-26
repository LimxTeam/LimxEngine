/*******************************************************************************
 * 文件: TCompressedPair.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   压缩对 — 利用空基类优化 (EBO) 的键值对
 *   当其中一个类型为空类 (如无状态分配器/比较器) 时
 *   自动通过继承消除该成员的内存占用
 *   用于容器内部存储分配器+数据、比较器+数据等场景
 *
 * 设计哲学:
 *   EBO — 空类型通过继承而非成员存储，节省内存
 *   透明访问 — First()/Second() 统一接口
 *   编译时选择 — 通过 TypeTraits 检测空类，自动选择存储策略
 *
 * 技术特性:
 *   - TCompressedPair<T1, T2>: 压缩对
 *   - First()/Second(): 访问两个元素
 *   - 空类型自动优化为零大小
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/TypeTraits/TypeTraits.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"

namespace Limx
{

namespace Private
{

/// 存储策略标签
/// 0: 两个都非空
/// 1: T1 为空
/// 2: T2 为空
/// 3: 两个都为空 (且类型不同)
template<typename T1, typename T2,
    bool T1Empty = __is_empty(T1),
    bool T2Empty = __is_empty(T2)>
struct TCompressedPairSelector
{
    static constexpr Int32 kValue = 0;
};

template<typename T1, typename T2>
struct TCompressedPairSelector<T1, T2, true, false>
{
    static constexpr Int32 kValue = 1;
};

template<typename T1, typename T2>
struct TCompressedPairSelector<T1, T2, false, true>
{
    static constexpr Int32 kValue = 2;
};

template<typename T1, typename T2>
struct TCompressedPairSelector<T1, T2, true, true>
{
    static constexpr Int32 kValue = 3;
};

/// 策略 0: 两个都非空 — 正常成员存储
template<typename T1, typename T2, Int32 Strategy>
class TCompressedPairImpl
{
public:
    TCompressedPairImpl() : m_First(), m_Second() {}

    TCompressedPairImpl(const T1& first, const T2& second)
        : m_First(first), m_Second(second) {}

    LIMX_NODISCARD T1& First() { return m_First; }
    LIMX_NODISCARD const T1& First() const
    {
        return m_First;
    }
    LIMX_NODISCARD T2& Second() { return m_Second; }
    LIMX_NODISCARD const T2& Second() const
    {
        return m_Second;
    }

private:
    T1 m_First;
    T2 m_Second;
};

/// 策略 1: T1 为空 — T1 通过继承 EBO
template<typename T1, typename T2>
class TCompressedPairImpl<T1, T2, 1> : private T1
{
public:
    TCompressedPairImpl() : T1(), m_Second() {}

    TCompressedPairImpl(const T1& first, const T2& second)
        : T1(first), m_Second(second) {}

    LIMX_NODISCARD T1& First()
    {
        return *static_cast<T1*>(this);
    }
    LIMX_NODISCARD const T1& First() const
    {
        return *static_cast<const T1*>(this);
    }
    LIMX_NODISCARD T2& Second() { return m_Second; }
    LIMX_NODISCARD const T2& Second() const
    {
        return m_Second;
    }

private:
    T2 m_Second;
};

/// 策略 2: T2 为空 — T2 通过继承 EBO
template<typename T1, typename T2>
class TCompressedPairImpl<T1, T2, 2> : private T2
{
public:
    TCompressedPairImpl() : T2(), m_First() {}

    TCompressedPairImpl(const T1& first, const T2& second)
        : T2(second), m_First(first) {}

    LIMX_NODISCARD T1& First() { return m_First; }
    LIMX_NODISCARD const T1& First() const
    {
        return m_First;
    }
    LIMX_NODISCARD T2& Second()
    {
        return *static_cast<T2*>(this);
    }
    LIMX_NODISCARD const T2& Second() const
    {
        return *static_cast<const T2*>(this);
    }

private:
    T1 m_First;
};

/// 策略 3: 两个都为空 — 双继承 EBO
template<typename T1, typename T2>
class TCompressedPairImpl<T1, T2, 3> : private T1, private T2
{
public:
    TCompressedPairImpl() : T1(), T2() {}

    TCompressedPairImpl(const T1& first, const T2& second)
        : T1(first), T2(second) {}

    LIMX_NODISCARD T1& First()
    {
        return *static_cast<T1*>(this);
    }
    LIMX_NODISCARD const T1& First() const
    {
        return *static_cast<const T1*>(this);
    }
    LIMX_NODISCARD T2& Second()
    {
        return *static_cast<T2*>(this);
    }
    LIMX_NODISCARD const T2& Second() const
    {
        return *static_cast<const T2*>(this);
    }
};

} // namespace Private

/// 压缩对 — 利用 EBO 的键值对
/// @tparam T1 第一个元素类型
/// @tparam T2 第二个元素类型
template<typename T1, typename T2>
class TCompressedPair
    : public Private::TCompressedPairImpl<T1, T2,
          Private::TCompressedPairSelector<T1, T2>::kValue>
{
    using Super = Private::TCompressedPairImpl<T1, T2,
        Private::TCompressedPairSelector<T1, T2>::kValue>;

public:
    TCompressedPair() : Super() {}

    TCompressedPair(const T1& first, const T2& second)
        : Super(first, second) {}
};

} // namespace Limx
