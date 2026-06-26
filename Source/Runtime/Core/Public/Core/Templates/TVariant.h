/*******************************************************************************
 * 文件: TVariant.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   类型安全联合体 — 替代 std::variant 的零 STL 依赖实现
 *   在固定内存中存储多种类型中的一种，运行时追踪当前活跃类型
 *   用于事件参数、属性值、解析结果等需要异构类型的场景
 *
 * 设计哲学:
 *   编译时类型列表 — 参数包指定可存储的类型集合
 *   运行时类型索引 — UInt32 追踪当前活跃类型
 *   就地存储 — 对齐的 char 数组，无堆分配
 *   值语义 — 支持拷贝/移动/析构
 *
 * 技术特性:
 *   - 存储: alignas(MaxAlign) char[MaxSize] 内联缓冲区
 *   - Set<T>: 就地构造指定类型
 *   - Get<T>: 获取引用 (类型不匹配则断言)
 *   - Is<T>: 查询当前是否为指定类型
 *   - GetIndex: 获取当前活跃类型索引
 *   - Visit: 类型安全访问 (编译时分发)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h, Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

// ============================================================================
// 编译时辅助: 参数包查询
// ============================================================================

namespace VariantDetail
{
    /// 参数包中最大 sizeof
    template<typename T, typename... Rest>
    struct MaxSizeOf
    {
        static constexpr SizeType Value =
            sizeof(T) > MaxSizeOf<Rest...>::Value
                ? sizeof(T) : MaxSizeOf<Rest...>::Value;
    };

    template<typename T>
    struct MaxSizeOf<T>
    {
        static constexpr SizeType Value = sizeof(T);
    };

    /// 参数包中最大 alignof
    template<typename T, typename... Rest>
    struct MaxAlignOf
    {
        static constexpr SizeType Value =
            alignof(T) > MaxAlignOf<Rest...>::Value
                ? alignof(T) : MaxAlignOf<Rest...>::Value;
    };

    template<typename T>
    struct MaxAlignOf<T>
    {
        static constexpr SizeType Value = alignof(T);
    };

    /// 查找类型在参数包中的索引 (编译时)
    template<typename Target, typename First, typename... Rest>
    struct IndexOf
    {
        static constexpr UInt32 Value =
            IsSame<Target, First>::Value
                ? 0 : 1 + IndexOf<Target, Rest...>::Value;
    };

    template<typename Target, typename Last>
    struct IndexOf<Target, Last>
    {
        static constexpr UInt32 Value =
            IsSame<Target, Last>::Value ? 0 : 1;
    };

    /// 参数包大小
    template<typename... Types>
    struct PackSize
    {
        static constexpr UInt32 Value = sizeof...(Types);
    };

    /// 按索引获取参数包中的类型
    template<UInt32 Index, typename First, typename... Rest>
    struct TypeAt
    {
        using Type = typename TypeAt<Index - 1, Rest...>::Type;
    };

    template<typename First, typename... Rest>
    struct TypeAt<0, First, Rest...>
    {
        using Type = First;
    };

    /// 无效索引标记
    static constexpr UInt32 kInvalidIndex = 0xFFFFFFFF;

    // ========================================================================
    // 析构/拷贝/移动分发表 (运行时按索引分发)
    // ========================================================================

    /// 析构分发
    template<typename T>
    void DestroyImpl(void* storage)
    {
        static_cast<T*>(storage)->~T();
    }

    /// 拷贝构造分发
    template<typename T>
    void CopyConstructImpl(void* dst, const void* src)
    {
        new (dst) T(*static_cast<const T*>(src));
    }

    /// 移动构造分发
    template<typename T>
    void MoveConstructImpl(void* dst, void* src)
    {
        new (dst) T(MoveTemp(*static_cast<T*>(src)));
    }

    /// 函数指针类型
    using DestroyFn = void (*)(void*);
    using CopyConstructFn = void (*)(void*, const void*);
    using MoveConstructFn = void (*)(void*, void*);

} // namespace VariantDetail

// ============================================================================
// TVariant — 类型安全联合体
// ============================================================================

/// 类型安全联合体
/// @tparam Types 可存储的类型列表
template<typename... Types>
class TVariant
{
    static constexpr SizeType kStorageSize =
        VariantDetail::MaxSizeOf<Types...>::Value;
    static constexpr SizeType kStorageAlign =
        VariantDetail::MaxAlignOf<Types...>::Value;
    static constexpr UInt32 kTypeCount =
        VariantDetail::PackSize<Types...>::Value;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 空 (无活跃类型)
    TVariant()
        : m_TypeIndex(VariantDetail::kInvalidIndex)
    {
    }

    /// 从值构造
    template<typename T>
    TVariant(const T& value)
        : m_TypeIndex(VariantDetail::kInvalidIndex)
    {
        Set<T>(value);
    }

    /// 从右值构造
    template<typename T>
    TVariant(T&& value)
        : m_TypeIndex(VariantDetail::kInvalidIndex)
    {
        EmplaceMove<T>(MoveTemp(value));
    }

    /// 拷贝构造
    TVariant(const TVariant& other)
        : m_TypeIndex(VariantDetail::kInvalidIndex)
    {
        if (other.IsValid())
        {
            static constexpr VariantDetail::CopyConstructFn
                kCopyTable[] = { &VariantDetail::CopyConstructImpl<Types>... };
            kCopyTable[other.m_TypeIndex](&m_Storage, &other.m_Storage);
            m_TypeIndex = other.m_TypeIndex;
        }
    }

    /// 移动构造
    TVariant(TVariant&& other) noexcept
        : m_TypeIndex(VariantDetail::kInvalidIndex)
    {
        if (other.IsValid())
        {
            static constexpr VariantDetail::MoveConstructFn
                kMoveTable[] = { &VariantDetail::MoveConstructImpl<Types>... };
            kMoveTable[other.m_TypeIndex](&m_Storage, &other.m_Storage);
            m_TypeIndex = other.m_TypeIndex;
            other.Reset();
        }
    }

    ~TVariant()
    {
        Reset();
    }

    /// 拷贝赋值
    TVariant& operator=(const TVariant& other)
    {
        if (this != &other)
        {
            Reset();
            if (other.IsValid())
            {
                static constexpr VariantDetail::CopyConstructFn
                    kCopyTable[] = {
                        &VariantDetail::CopyConstructImpl<Types>... };
                kCopyTable[other.m_TypeIndex](
                    &m_Storage, &other.m_Storage);
                m_TypeIndex = other.m_TypeIndex;
            }
        }
        return *this;
    }

    /// 移动赋值
    TVariant& operator=(TVariant&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            if (other.IsValid())
            {
                static constexpr VariantDetail::MoveConstructFn
                    kMoveTable[] = {
                        &VariantDetail::MoveConstructImpl<Types>... };
                kMoveTable[other.m_TypeIndex](
                    &m_Storage, &other.m_Storage);
                m_TypeIndex = other.m_TypeIndex;
                other.Reset();
            }
        }
        return *this;
    }

    // ========================================================================
    // 类型操作
    // ========================================================================

    /// 设置值 (拷贝)
    template<typename T>
    void Set(const T& value)
    {
        Reset();
        new (&m_Storage) T(value);
        m_TypeIndex = VariantDetail::IndexOf<T, Types...>::Value;
    }

    /// 就地构造 (移动)
    template<typename T>
    void EmplaceMove(T&& value)
    {
        Reset();
        new (&m_Storage) T(MoveTemp(value));
        m_TypeIndex = VariantDetail::IndexOf<
            typename RemoveReferenceT<T>::Type, Types...>::Value;
    }

    /// 获取引用 (类型安全)
    template<typename T>
    LIMX_NODISCARD T& Get()
    {
        constexpr UInt32 index =
            VariantDetail::IndexOf<T, Types...>::Value;
        LIMX_ASSERT(m_TypeIndex == index);
        return *reinterpret_cast<T*>(&m_Storage);
    }

    template<typename T>
    LIMX_NODISCARD const T& Get() const
    {
        constexpr UInt32 index =
            VariantDetail::IndexOf<T, Types...>::Value;
        LIMX_ASSERT(m_TypeIndex == index);
        return *reinterpret_cast<const T*>(&m_Storage);
    }

    /// 安全获取指针 (类型不匹配返回 nullptr)
    template<typename T>
    LIMX_NODISCARD T* TryGet()
    {
        constexpr UInt32 index =
            VariantDetail::IndexOf<T, Types...>::Value;
        if (m_TypeIndex == index)
        {
            return reinterpret_cast<T*>(&m_Storage);
        }
        return nullptr;
    }

    template<typename T>
    LIMX_NODISCARD const T* TryGet() const
    {
        constexpr UInt32 index =
            VariantDetail::IndexOf<T, Types...>::Value;
        if (m_TypeIndex == index)
        {
            return reinterpret_cast<const T*>(&m_Storage);
        }
        return nullptr;
    }

    /// 查询当前是否为指定类型
    template<typename T>
    LIMX_NODISCARD bool Is() const
    {
        constexpr UInt32 index =
            VariantDetail::IndexOf<T, Types...>::Value;
        return m_TypeIndex == index;
    }

    /// 获取当前活跃类型索引
    LIMX_NODISCARD UInt32 GetIndex() const { return m_TypeIndex; }

    /// 是否有有效值
    LIMX_NODISCARD bool IsValid() const
    {
        return m_TypeIndex != VariantDetail::kInvalidIndex;
    }

    /// 重置为空
    void Reset()
    {
        if (IsValid())
        {
            static constexpr VariantDetail::DestroyFn
                kDestroyTable[] = {
                    &VariantDetail::DestroyImpl<Types>... };
            kDestroyTable[m_TypeIndex](&m_Storage);
            m_TypeIndex = VariantDetail::kInvalidIndex;
        }
    }

private:
    alignas(kStorageAlign) char m_Storage[kStorageSize];
    UInt32 m_TypeIndex;
};

} // namespace Limx
