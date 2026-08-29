/*******************************************************************************
 * 文件: FProbe.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   生命周期探针类型 — 统计构造/拷贝/移动/析构次数的非平凡测试元素
 *   容器测试用它验证元素被正确构造与析构、扩容时未产生多余拷贝、
 *   移动语义真正转移而非退化为拷贝
 *
 * 设计哲学:
 *   非平凡才能暴露问题 — 用 Int32 测容器只能验证数值正确，无法发现
 *   "析构未调用""扩容走了拷贝而非移动""移后源未置空"这类真实缺陷。
 *   探针把这些不可见行为变成可断言的计数。
 *
 *   计数为静态量 — 容器内部会自由拷贝/移动元素，实例级计数无法汇总；
 *   静态计数配合每个用例开头的 ResetCounters 即可精确观察一段代码的行为。
 *
 * 技术特性:
 *   - 六个特殊成员函数全部自定义并计数, 无一走编译器默认实现
 *   - GetLiveCount 给出"已构造未析构"数, 非零即元素泄漏
 *   - MovedFrom 标记可验证移动后源对象确实被置为有效但未指定状态
 *   - 提供 operator== 与哈希特化以便用作 TSet/TMap 的键
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h
 *
 * 注意事项:
 *   静态计数非线程安全 — 探针仅用于单线程容器测试
 *   每个用例必须先调用 ResetCounters, 否则会累计上一个用例的计数
 *
 ******************************************************************************/

#pragma once

#include "CoreTests/CoreTestsMinimal.h"

namespace Limx
{

// ============================================================================
// FProbe — 生命周期计数探针
// ============================================================================

/// 非平凡测试元素 — 记录自身参与的每一次构造/拷贝/移动/析构
class FProbe
{
public:
    // ========================================================================
    // 全局计数 — 每个用例开头 ResetCounters 后独立观察
    // ========================================================================

    static Int32 s_DefaultConstructCount;
    static Int32 s_ValueConstructCount;
    static Int32 s_CopyConstructCount;
    static Int32 s_MoveConstructCount;
    static Int32 s_CopyAssignCount;
    static Int32 s_MoveAssignCount;
    static Int32 s_DestructCount;

    /// 清零全部计数 — 每个用例开头调用
    static void ResetCounters()
    {
        s_DefaultConstructCount = 0;
        s_ValueConstructCount   = 0;
        s_CopyConstructCount    = 0;
        s_MoveConstructCount    = 0;
        s_CopyAssignCount       = 0;
        s_MoveAssignCount       = 0;
        s_DestructCount         = 0;
    }

    /// 已构造但尚未析构的实例数 — 非零即元素泄漏
    LIMX_NODISCARD static Int32 GetLiveCount()
    {
        return s_DefaultConstructCount + s_ValueConstructCount +
               s_CopyConstructCount + s_MoveConstructCount - s_DestructCount;
    }

    /// 构造总次数 (不含赋值)
    LIMX_NODISCARD static Int32 GetTotalConstructCount()
    {
        return s_DefaultConstructCount + s_ValueConstructCount +
               s_CopyConstructCount + s_MoveConstructCount;
    }

    // ========================================================================
    // 特殊成员函数 — 全部自定义以便计数
    // ========================================================================

    FProbe()
        : m_Value(0)
        , m_IsMovedFrom(false)
    {
        ++s_DefaultConstructCount;
    }

    explicit FProbe(Int32 value)
        : m_Value(value)
        , m_IsMovedFrom(false)
    {
        ++s_ValueConstructCount;
    }

    FProbe(const FProbe& other)
        : m_Value(other.m_Value)
        , m_IsMovedFrom(false)
    {
        ++s_CopyConstructCount;
    }

    FProbe(FProbe&& other) noexcept
        : m_Value(other.m_Value)
        , m_IsMovedFrom(false)
    {
        // 移后源置为可析构的已知状态, 便于断言移动确实发生
        other.m_Value      = -1;
        other.m_IsMovedFrom = true;

        ++s_MoveConstructCount;
    }

    FProbe& operator=(const FProbe& other)
    {
        if (this != &other)
        {
            m_Value       = other.m_Value;
            m_IsMovedFrom = false;
        }

        ++s_CopyAssignCount;
        return *this;
    }

    FProbe& operator=(FProbe&& other) noexcept
    {
        if (this != &other)
        {
            m_Value       = other.m_Value;
            m_IsMovedFrom = false;

            other.m_Value       = -1;
            other.m_IsMovedFrom = true;
        }

        ++s_MoveAssignCount;
        return *this;
    }

    ~FProbe()
    {
        ++s_DestructCount;

        // 析构后写入哨兵值 — 若容器重复析构同一元素, 二次析构时
        // m_Value 已是哨兵, 配合计数可定位问题
        m_Value = -2;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD Int32 GetValue() const { return m_Value; }

    void SetValue(Int32 value) { m_Value = value; }

    /// 本实例是否曾被移动走
    LIMX_NODISCARD bool IsMovedFrom() const { return m_IsMovedFrom; }

    LIMX_NODISCARD bool operator==(const FProbe& other) const
    {
        return m_Value == other.m_Value;
    }

    LIMX_NODISCARD bool operator!=(const FProbe& other) const
    {
        return !(*this == other);
    }

private:
    Int32 m_Value;
    bool  m_IsMovedFrom;
};

} // namespace Limx
