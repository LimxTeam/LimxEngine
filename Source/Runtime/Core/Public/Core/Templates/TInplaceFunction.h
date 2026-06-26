/*******************************************************************************
 * 文件: TInplaceFunction.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   固定大小可调用对象 — 无堆分配的类型擦除函数包装
 *   将可调用对象存储在内联缓冲区中，超出大小则编译时报错
 *   用于性能敏感路径的回调、渲染管线回调、中断处理等场景
 *
 * 设计哲学:
 *   零堆分配 — 所有存储均在对象内部的固定缓冲区中
 *   编译时约束 — 可调用对象超过缓冲区大小时 static_assert 失败
 *   接口兼容 — 与 TFunction 接口一致，可互相替换
 *
 * 技术特性:
 *   - TInplaceFunction<Signature, BufferSize>: 固定缓冲区函数包装
 *   - operator(): 调用
 *   - operator bool: 有效性检查
 *   - 默认缓冲区 64 字节
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

/// 前向声明 — 主模板不定义
template<typename Signature, SizeType BufferSize = 64>
class TInplaceFunction;

/// TInplaceFunction 偏特化 — 匹配函数签名 ReturnType(Args...)
template<typename ReturnType, typename... Args, SizeType BufferSize>
class TInplaceFunction<ReturnType(Args...), BufferSize>
{
    /// 虚函数表 — 类型擦除操作
    struct FVTable
    {
        ReturnType (*Invoke)(void*, Args...);
        void (*Destroy)(void*);
        void (*MoveConstruct)(void* dest, void* source);
    };

    /// 具体类型的虚函数表实例
    template<typename Callable>
    struct TCallableVTable
    {
        static ReturnType Invoke(void* storage, Args... args)
        {
            return (*static_cast<Callable*>(storage))(
                static_cast<Args&&>(args)...);
        }

        static void Destroy(void* storage)
        {
            static_cast<Callable*>(storage)->~Callable();
        }

        static void MoveConstruct(void* dest, void* source)
        {
            new (dest) Callable(
                MoveTemp(*static_cast<Callable*>(source)));
        }

        static constexpr FVTable kVTable = {
            &Invoke, &Destroy, &MoveConstruct
        };
    };

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 空函数
    TInplaceFunction()
        : m_VTable(nullptr)
    {
    }

    /// 从可调用对象构造
    template<typename Callable>
    TInplaceFunction(Callable callable)
        : m_VTable(nullptr)
    {
        static_assert(sizeof(Callable) <= BufferSize,
            "Callable exceeds TInplaceFunction buffer size");
        static_assert(alignof(Callable) <= alignof(
            decltype(m_Storage)),
            "Callable alignment exceeds buffer alignment");

        new (&m_Storage) Callable(MoveTemp(callable));
        m_VTable = &TCallableVTable<Callable>::kVTable;
    }

    /// 空指针构造
    TInplaceFunction(decltype(nullptr))
        : m_VTable(nullptr)
    {
    }

    ~TInplaceFunction()
    {
        if (m_VTable != nullptr)
        {
            m_VTable->Destroy(&m_Storage);
        }
    }

    /// 移动构造
    TInplaceFunction(TInplaceFunction&& other) noexcept
        : m_VTable(other.m_VTable)
    {
        if (m_VTable != nullptr)
        {
            m_VTable->MoveConstruct(&m_Storage, &other.m_Storage);
            other.m_VTable->Destroy(&other.m_Storage);
            other.m_VTable = nullptr;
        }
    }

    /// 移动赋值
    TInplaceFunction& operator=(TInplaceFunction&& other) noexcept
    {
        if (this != &other)
        {
            if (m_VTable != nullptr)
            {
                m_VTable->Destroy(&m_Storage);
            }

            m_VTable = other.m_VTable;
            if (m_VTable != nullptr)
            {
                m_VTable->MoveConstruct(
                    &m_Storage, &other.m_Storage);
                other.m_VTable->Destroy(&other.m_Storage);
                other.m_VTable = nullptr;
            }
        }
        return *this;
    }

    /// 空指针赋值
    TInplaceFunction& operator=(decltype(nullptr))
    {
        if (m_VTable != nullptr)
        {
            m_VTable->Destroy(&m_Storage);
            m_VTable = nullptr;
        }
        return *this;
    }

    // 不可拷贝
    TInplaceFunction(const TInplaceFunction&) = delete;
    TInplaceFunction& operator=(const TInplaceFunction&) = delete;

    // ========================================================================
    // 调用
    // ========================================================================

    /// 调用
    ReturnType operator()(Args... args) const
    {
        LIMX_ASSERT(m_VTable != nullptr);
        return m_VTable->Invoke(
            const_cast<void*>(
                static_cast<const void*>(&m_Storage)),
            static_cast<Args&&>(args)...);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 是否有效
    LIMX_NODISCARD explicit operator bool() const
    {
        return m_VTable != nullptr;
    }

    /// 缓冲区大小
    LIMX_NODISCARD static constexpr SizeType GetBufferSize()
    {
        return BufferSize;
    }

private:
    const FVTable* m_VTable;  ///< 虚函数表指针

    /// 内联存储缓冲区
    alignas(16) char m_Storage[BufferSize];
};

} // namespace Limx
