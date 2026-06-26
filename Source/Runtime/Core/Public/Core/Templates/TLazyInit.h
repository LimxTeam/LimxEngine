/*******************************************************************************
 * 文件: TLazyInit.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   延迟初始化 — 首次访问时才构造对象的包装器
 *   避免全局/静态对象的构造顺序问题，按需创建
 *   用于单例延迟构造、可选子系统、昂贵资源按需加载等场景
 *
 * 设计哲学:
 *   按需构造 — 首次 Get() 时通过工厂函数创建对象
 *   内嵌存储 — 对象内嵌在 TAlignedBuffer 中，无堆分配
 *   显式重置 — 可手动销毁并在下次访问时重建
 *
 * 技术特性:
 *   - TLazyInit<T>: 延迟初始化包装器
 *   - Get: 获取对象引用 (首次调用时构造)
 *   - IsInitialized: 是否已初始化
 *   - Reset: 销毁对象
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

/// 延迟初始化包装器
/// @tparam T 被包装的对象类型
template<typename T>
class TLazyInit
{
public:
    TLazyInit()
        : m_IsInitialized(false)
    {
    }

    ~TLazyInit()
    {
        if (m_IsInitialized)
        {
            GetPtr()->~T();
        }
    }

    // 不可拷贝
    TLazyInit(const TLazyInit&) = delete;
    TLazyInit& operator=(const TLazyInit&) = delete;

    // 不可移动 (内嵌存储)
    TLazyInit(TLazyInit&&) = delete;
    TLazyInit& operator=(TLazyInit&&) = delete;

    // ========================================================================
    // 访问
    // ========================================================================

    /// 获取对象引用 (首次调用时默认构造)
    LIMX_NODISCARD T& Get()
    {
        if (!m_IsInitialized)
        {
            new (GetStorage()) T();
            m_IsInitialized = true;
        }
        return *GetPtr();
    }

    /// 获取对象引用 (只读)
    LIMX_NODISCARD const T& Get() const
    {
        LIMX_ASSERT(m_IsInitialized);
        return *GetPtr();
    }

    /// 获取指针 (未初始化则返回 nullptr)
    LIMX_NODISCARD T* TryGet()
    {
        return m_IsInitialized ? GetPtr() : nullptr;
    }

    LIMX_NODISCARD const T* TryGet() const
    {
        return m_IsInitialized ? GetPtr() : nullptr;
    }

    /// 解引用操作符
    LIMX_NODISCARD T& operator*() { return Get(); }
    LIMX_NODISCARD const T& operator*() const
    {
        return Get();
    }

    /// 箭头操作符
    LIMX_NODISCARD T* operator->() { return &Get(); }
    LIMX_NODISCARD const T* operator->() const
    {
        return &Get();
    }

    // ========================================================================
    // 显式初始化
    // ========================================================================

    /// 用指定参数显式初始化 (拷贝)
    void Initialize(const T& value)
    {
        if (m_IsInitialized)
        {
            GetPtr()->~T();
        }
        new (GetStorage()) T(value);
        m_IsInitialized = true;
    }

    /// 用指定参数显式初始化 (移动)
    void Initialize(T&& value)
    {
        if (m_IsInitialized)
        {
            GetPtr()->~T();
        }
        new (GetStorage()) T(static_cast<T&&>(value));
        m_IsInitialized = true;
    }

    // ========================================================================
    // 查询与重置
    // ========================================================================

    /// 是否已初始化
    LIMX_NODISCARD bool IsInitialized() const
    {
        return m_IsInitialized;
    }

    /// 销毁对象 (下次 Get() 会重新构造)
    void Reset()
    {
        if (m_IsInitialized)
        {
            GetPtr()->~T();
            m_IsInitialized = false;
        }
    }

private:
    void* GetStorage()
    {
        return static_cast<void*>(m_Storage);
    }

    T* GetPtr()
    {
        return reinterpret_cast<T*>(m_Storage);
    }

    const T* GetPtr() const
    {
        return reinterpret_cast<const T*>(m_Storage);
    }

    alignas(T) UInt8 m_Storage[sizeof(T)];  ///< 内嵌存储
    bool m_IsInitialized;                     ///< 初始化标志
};

} // namespace Limx
