/*******************************************************************************
 * 文件: TArray.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   动态数组容器 — 替代 std::vector 的零 STL 依赖实现
 *   支持连续内存存储、动态扩容、随机访问、范围 for 循环
 *   通过分配器接口管理内存，默认使用 DefaultAllocator
 *
 * 设计哲学:
 *   数据导向 — 连续内存布局，缓存友好
 *   零 STL 依赖 — 不包含任何标准库头文件
 *   所有权明确 — TArray 拥有其中所有元素的所有权
 *   异常安全 — 不使用异常，所有错误通过断言捕获
 *   POD 优化 — 对算术类型/指针/枚举使用 memcpy 代替逐元素操作
 *
 * 技术特性:
 *   - 动态扩容策略: 2 倍增长 (摊还 O(1) 追加)
 *   - 支持: Add, Insert, RemoveAt, RemoveSwap, Find, Contains
 *   - 范围 for: begin()/end() 迭代器
 *   - 移动语义: 移动构造/赋值转移所有权，零拷贝
 *   - 分配器注入: 构造时传入 IAllocator 引用
 *
 * 依赖关系:
 *   内部: Core/CoreTypes.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h
 *
 * 注意事项:
 *   TArray 不是线程安全的 — 并发读写需外部同步
 *   Remove 操作不保证尾部元素析构后内存清零
 *   Capacity 扩容后不会自动收缩
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 动态数组 — 连续内存、动态扩容、类型安全
/// @tparam T 元素类型
template<typename T>
class TArray
{
public:
    // ========================================================================
    // 类型别名
    // ========================================================================

    using ElementType   = T;
    using SizeType      = Limx::SizeType;
    using Iterator      = T*;
    using ConstIterator = const T*;

    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 空数组，使用默认分配器
    TArray()
        : m_Data(nullptr)
        , m_Size(0)
        , m_Capacity(0)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    /// 指定分配器构造
    explicit TArray(IAllocator& allocator)
        : m_Data(nullptr)
        , m_Size(0)
        , m_Capacity(0)
        , m_Allocator(&allocator)
    {
    }

    /// 指定初始容量构造 (不构造元素)
    explicit TArray(SizeType initialCapacity,
                    IAllocator& allocator = GetDefaultAllocator())
        : m_Data(nullptr)
        , m_Size(0)
        , m_Capacity(0)
        , m_Allocator(&allocator)
    {
        Reserve(initialCapacity);
    }

    /// 拷贝构造
    TArray(const TArray& other)
        : m_Data(nullptr)
        , m_Size(0)
        , m_Capacity(0)
        , m_Allocator(other.m_Allocator)
    {
        if (other.m_Size > 0)
        {
            Reserve(other.m_Size);
            Memory::CopyConstructItems(m_Data, other.m_Data, other.m_Size);
            m_Size = other.m_Size;
        }
    }

    /// 移动构造 — 转移所有权，零拷贝
    TArray(TArray&& other) noexcept
        : m_Data(other.m_Data)
        , m_Size(other.m_Size)
        , m_Capacity(other.m_Capacity)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Data = nullptr;
        other.m_Size = 0;
        other.m_Capacity = 0;
    }

    /// 析构 — 析构所有元素并释放内存
    ~TArray()
    {
        Clear();
        DeallocateBuffer();
    }

    // ========================================================================
    // 赋值运算符
    // ========================================================================

    /// 拷贝赋值
    TArray& operator=(const TArray& other)
    {
        if (this != &other)
        {
            Clear();
            if (other.m_Size > 0)
            {
                Reserve(other.m_Size);
                Memory::CopyConstructItems(m_Data, other.m_Data, other.m_Size);
                m_Size = other.m_Size;
            }
        }
        return *this;
    }

    /// 移动赋值 — 转移所有权
    TArray& operator=(TArray&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            DeallocateBuffer();

            m_Data = other.m_Data;
            m_Size = other.m_Size;
            m_Capacity = other.m_Capacity;
            m_Allocator = other.m_Allocator;

            other.m_Data = nullptr;
            other.m_Size = 0;
            other.m_Capacity = 0;
        }
        return *this;
    }

    // ========================================================================
    // 元素访问
    // ========================================================================

    /// 下标访问 (不检查越界)
    LIMX_NODISCARD FORCEINLINE T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        return m_Data[index];
    }

    LIMX_NODISCARD FORCEINLINE const T& operator[](SizeType index) const
    {
        LIMX_ASSERT(index < m_Size);
        return m_Data[index];
    }

    /// 首元素
    LIMX_NODISCARD FORCEINLINE T& First()
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Data[0];
    }

    LIMX_NODISCARD FORCEINLINE const T& First() const
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Data[0];
    }

    /// 末元素
    LIMX_NODISCARD FORCEINLINE T& Last()
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Data[m_Size - 1];
    }

    LIMX_NODISCARD FORCEINLINE const T& Last() const
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Data[m_Size - 1];
    }

    /// 原始数据指针
    LIMX_NODISCARD FORCEINLINE T* GetData() { return m_Data; }
    LIMX_NODISCARD FORCEINLINE const T* GetData() const { return m_Data; }

    // ========================================================================
    // 容量与大小
    // ========================================================================

    /// 当前元素数量
    LIMX_NODISCARD FORCEINLINE SizeType GetSize() const { return m_Size; }

    /// 当前已分配容量 (元素数)
    LIMX_NODISCARD FORCEINLINE SizeType GetCapacity() const { return m_Capacity; }

    /// 是否为空
    LIMX_NODISCARD FORCEINLINE bool IsEmpty() const { return m_Size == 0; }

    /// 已用内存字节数
    LIMX_NODISCARD FORCEINLINE SizeType GetAllocatedSize() const
    {
        return m_Capacity * sizeof(T);
    }

    // ========================================================================
    // 容量管理
    // ========================================================================

    /// 预分配至少 newCapacity 个元素的空间
    /// 不改变 Size，不构造新元素
    void Reserve(SizeType newCapacity)
    {
        if (newCapacity <= m_Capacity)
        {
            return;
        }

        T* newData = AllocateBuffer(newCapacity);

        if (m_Data && m_Size > 0)
        {
            Memory::MoveConstructItems(newData, m_Data, m_Size);
            Memory::DestructItems(m_Data, m_Size);
        }

        DeallocateBuffer();
        m_Data = newData;
        m_Capacity = newCapacity;
    }

    /// 收缩容量至当前 Size — 释放多余内存
    void Shrink()
    {
        if (m_Size == m_Capacity)
        {
            return;
        }

        if (m_Size == 0)
        {
            DeallocateBuffer();
            m_Data = nullptr;
            m_Capacity = 0;
            return;
        }

        T* newData = AllocateBuffer(m_Size);
        Memory::MoveConstructItems(newData, m_Data, m_Size);
        Memory::DestructItems(m_Data, m_Size);
        DeallocateBuffer();
        m_Data = newData;
        m_Capacity = m_Size;
    }

    // ========================================================================
    // 元素添加
    // ========================================================================

    /// 在末尾追加元素 (拷贝)
    /// @return 新元素的索引
    SizeType Add(const T& element)
    {
        // 自引用安全: element 可能指向本数组内部 (例如 values.Add(values[0])
        // 或 LZ77 反向引用那样从已写入区域回读)。扩容会释放旧缓冲区，
        // 使传入的引用悬垂，随后的拷贝构造读到的是已释放内存。
        // 因此需要扩容时先把值搬到临时对象，再从临时对象移入新位置。
        if (m_Size >= m_Capacity)
        {
            T temporary(element);

            EnsureCapacity(m_Size + 1);
            new (m_Data + m_Size) T(MoveTemp(temporary));
        }
        else
        {
            new (m_Data + m_Size) T(element);
        }

        return m_Size++;
    }

    /// 在末尾追加元素 (移动)
    SizeType Add(T&& element)
    {
        // 同 Add(const T&): 右值引用同样可能绑定到本数组内部的元素
        if (m_Size >= m_Capacity)
        {
            T temporary(MoveTemp(element));

            EnsureCapacity(m_Size + 1);
            new (m_Data + m_Size) T(MoveTemp(temporary));
        }
        else
        {
            new (m_Data + m_Size) T(MoveTemp(element));
        }

        return m_Size++;
    }

    /// 在末尾原地构造元素
    template<typename... Args>
    T& Emplace(Args&&... args)
    {
        EnsureCapacity(m_Size + 1);
        T* location = m_Data + m_Size;
        new (location) T(Forward<Args>(args)...);
        ++m_Size;
        return *location;
    }

    /// 在指定位置插入元素 (拷贝)
    void Insert(SizeType index, const T& element)
    {
        LIMX_ASSERT(index <= m_Size);
        EnsureCapacity(m_Size + 1);

        if (index < m_Size)
        {
            // 后移现有元素
            Memory::RelocateItemsBackward(
                m_Data + index + 1, m_Data + index, m_Size - index);
        }

        new (m_Data + index) T(element);
        ++m_Size;
    }

    /// 在指定位置插入元素 (移动)
    void Insert(SizeType index, T&& element)
    {
        LIMX_ASSERT(index <= m_Size);
        EnsureCapacity(m_Size + 1);

        if (index < m_Size)
        {
            Memory::RelocateItemsBackward(
                m_Data + index + 1, m_Data + index, m_Size - index);
        }

        new (m_Data + index) T(MoveTemp(element));
        ++m_Size;
    }

    /// 追加另一个数组的所有元素
    void Append(const TArray& other)
    {
        if (other.m_Size > 0)
        {
            EnsureCapacity(m_Size + other.m_Size);
            Memory::CopyConstructItems(m_Data + m_Size, other.m_Data, other.m_Size);
            m_Size += other.m_Size;
        }
    }

    // ========================================================================
    // 元素移除
    // ========================================================================

    /// 移除指定索引的元素 — 保持顺序，O(n)
    void RemoveAt(SizeType index)
    {
        LIMX_ASSERT(index < m_Size);

        Memory::DestructItems(m_Data + index, 1);

        SizeType moveCount = m_Size - index - 1;
        if (moveCount > 0)
        {
            Memory::MoveConstructItems(
                m_Data + index, m_Data + index + 1, moveCount);
            Memory::DestructItems(m_Data + index + 1, moveCount);
        }

        --m_Size;
    }

    /// 移除指定索引的元素 — 用末尾元素填充，O(1)，不保持顺序
    void RemoveAtSwap(SizeType index)
    {
        LIMX_ASSERT(index < m_Size);

        Memory::DestructItems(m_Data + index, 1);

        --m_Size;
        if (index != m_Size)
        {
            // 将末尾元素移到被删除的位置
            Memory::MoveConstructItems(m_Data + index, m_Data + m_Size, 1);
            Memory::DestructItems(m_Data + m_Size, 1);
        }
    }

    /// 移除末尾元素
    void RemoveLast()
    {
        LIMX_ASSERT(m_Size > 0);
        --m_Size;
        Memory::DestructItems(m_Data + m_Size, 1);
    }

    /// 清空所有元素 — 析构所有元素但不释放内存
    void Clear()
    {
        if (m_Size > 0)
        {
            Memory::DestructItems(m_Data, m_Size);
            m_Size = 0;
        }
    }

    /// 重置 — 析构所有元素并释放内存
    void Reset()
    {
        Clear();
        DeallocateBuffer();
        m_Data = nullptr;
        m_Capacity = 0;
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 线性查找 — 返回首个匹配元素的索引，未找到返回 kSizeTypeMax
    LIMX_NODISCARD SizeType Find(const T& element) const
    {
        for (SizeType index = 0; index < m_Size; ++index)
        {
            if (m_Data[index] == element)
            {
                return index;
            }
        }
        return kSizeTypeMax;
    }

    /// 是否包含指定元素
    LIMX_NODISCARD bool Contains(const T& element) const
    {
        return Find(element) != kSizeTypeMax;
    }

    // ========================================================================
    // 迭代器 — 支持范围 for 循环
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE Iterator begin() { return m_Data; }
    LIMX_NODISCARD FORCEINLINE Iterator end() { return m_Data + m_Size; }
    LIMX_NODISCARD FORCEINLINE ConstIterator begin() const { return m_Data; }
    LIMX_NODISCARD FORCEINLINE ConstIterator end() const { return m_Data + m_Size; }

    // ========================================================================
    // 调整大小
    // ========================================================================

    /// 调整大小 — 新增元素默认构造，多余元素析构
    void SetSize(SizeType newSize)
    {
        if (newSize > m_Size)
        {
            EnsureCapacity(newSize);
            Memory::DefaultConstructItems(m_Data + m_Size, newSize - m_Size);
        }
        else if (newSize < m_Size)
        {
            Memory::DestructItems(m_Data + newSize, m_Size - newSize);
        }
        m_Size = newSize;
    }

    /// 调整大小 — 新增元素用指定值填充
    void SetSize(SizeType newSize, const T& fillValue)
    {
        if (newSize > m_Size)
        {
            EnsureCapacity(newSize);
            for (SizeType index = m_Size; index < newSize; ++index)
            {
                new (m_Data + index) T(fillValue);
            }
        }
        else if (newSize < m_Size)
        {
            Memory::DestructItems(m_Data + newSize, m_Size - newSize);
        }
        m_Size = newSize;
    }

private:
    // ========================================================================
    // 内部辅助
    // ========================================================================

    /// 分配指定数量元素的原始缓冲区
    LIMX_NODISCARD T* AllocateBuffer(SizeType elementCount)
    {
        SizeType alignment = alignof(T) > kDefaultAlignment
            ? alignof(T) : kDefaultAlignment;
        return static_cast<T*>(m_Allocator->Allocate(
            elementCount * sizeof(T), alignment));
    }

    /// 释放当前缓冲区
    void DeallocateBuffer()
    {
        if (m_Data)
        {
            m_Allocator->Deallocate(m_Data);
            m_Data = nullptr;
        }
    }

    /// 确保容量至少为 requiredCapacity — 不足时按 2 倍扩容
    void EnsureCapacity(SizeType requiredCapacity)
    {
        if (requiredCapacity <= m_Capacity)
        {
            return;
        }

        // 2 倍扩容策略，最小 8 个元素
        SizeType newCapacity = m_Capacity > 0 ? m_Capacity * 2 : 8;
        while (newCapacity < requiredCapacity)
        {
            newCapacity *= 2;
        }

        Reserve(newCapacity);
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    T*          m_Data;       ///< 连续内存缓冲区
    SizeType    m_Size;       ///< 当前元素数量
    SizeType    m_Capacity;   ///< 已分配容量 (元素数)
    IAllocator* m_Allocator;  ///< 内存分配器 (非拥有引用)
};

} // namespace Limx
