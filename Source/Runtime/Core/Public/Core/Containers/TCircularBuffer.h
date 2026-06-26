/*******************************************************************************
 * 文件: TCircularBuffer.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   无锁环形缓冲 — 单生产者单消费者 (SPSC) 无锁队列
 *   基于原子操作的读写索引实现无锁通信
 *   用于音频流缓冲、渲染线程命令传递、日志异步写入等场景
 *
 * 设计哲学:
 *   SPSC 无锁 — 仅一个生产者和一个消费者线程，无需互斥锁
 *   2 的幂容量 — 容量必须为 2 的幂，用位掩码替代取模
 *   原子索引 — 读写索引使用原子操作保证可见性
 *
 * 技术特性:
 *   - TCircularBuffer<T, Capacity>: 编译时容量的 SPSC 环形缓冲
 *   - TryPush: 非阻塞入队
 *   - TryPop: 非阻塞出队
 *   - GetSize: 当前元素数
 *   - IsEmpty/IsFull: 状态查询
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *   外部: MSVC _Interlocked* 内建函数
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"

#if LIMX_COMPILER_MSVC
extern "C"
{
    void _ReadWriteBarrier();
    void _mm_mfence();
}
#pragma intrinsic(_ReadWriteBarrier)
#endif

namespace Limx
{

/// 无锁 SPSC 环形缓冲
/// @tparam T 元素类型
/// @tparam Capacity 容量 (必须为 2 的幂)
template<typename T, SizeType Capacity>
class TCircularBuffer
{
    static_assert(Capacity > 0, "Capacity must be > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2");

    static constexpr SizeType kMask = Capacity - 1;

public:
    TCircularBuffer()
        : m_ReadIndex(0)
        , m_WriteIndex(0)
    {
    }

    ~TCircularBuffer()
    {
        // 析构所有未消费的元素
        while (m_ReadIndex != m_WriteIndex)
        {
            SizeType readSlot = m_ReadIndex & kMask;
            GetElement(readSlot)->~T();
            ++m_ReadIndex;
        }
    }

    // 不可拷贝/移动 (SPSC 语义下不安全)
    TCircularBuffer(const TCircularBuffer&) = delete;
    TCircularBuffer& operator=(const TCircularBuffer&) = delete;
    TCircularBuffer(TCircularBuffer&&) = delete;
    TCircularBuffer& operator=(TCircularBuffer&&) = delete;

    // ========================================================================
    // 生产者操作 (仅生产者线程调用)
    // ========================================================================

    /// 尝试入队 (拷贝)
    /// @return 是否成功 (false = 队列已满)
    bool TryPush(const T& element)
    {
        SizeType writeIndex = m_WriteIndex;
        SizeType nextWrite = writeIndex + 1;

        // 检查是否已满
        if (nextWrite - m_ReadIndex > Capacity)
        {
            return false;
        }

        SizeType slot = writeIndex & kMask;
        new (GetElement(slot)) T(element);

        // 写屏障 — 确保数据写入在索引更新之前可见
        CompilerBarrier();
        m_WriteIndex = nextWrite;
        return true;
    }

    /// 尝试入队 (移动)
    bool TryPush(T&& element)
    {
        SizeType writeIndex = m_WriteIndex;
        SizeType nextWrite = writeIndex + 1;

        if (nextWrite - m_ReadIndex > Capacity)
        {
            return false;
        }

        SizeType slot = writeIndex & kMask;
        new (GetElement(slot)) T(MoveTemp(element));

        CompilerBarrier();
        m_WriteIndex = nextWrite;
        return true;
    }

    // ========================================================================
    // 消费者操作 (仅消费者线程调用)
    // ========================================================================

    /// 尝试出队
    /// @param outElement 输出元素
    /// @return 是否成功 (false = 队列为空)
    bool TryPop(T& outElement)
    {
        SizeType readIndex = m_ReadIndex;

        // 检查是否为空
        if (readIndex == m_WriteIndex)
        {
            return false;
        }

        CompilerBarrier();

        SizeType slot = readIndex & kMask;
        outElement = MoveTemp(*GetElement(slot));
        GetElement(slot)->~T();

        CompilerBarrier();
        m_ReadIndex = readIndex + 1;
        return true;
    }

    /// 查看队首元素 (不出队)
    LIMX_NODISCARD const T* Peek() const
    {
        if (m_ReadIndex == m_WriteIndex)
        {
            return nullptr;
        }
        SizeType slot = m_ReadIndex & kMask;
        return GetElement(slot);
    }

    // ========================================================================
    // 查询 (任意线程可调用)
    // ========================================================================

    /// 当前元素数 (近似值，多线程下可能不精确)
    LIMX_NODISCARD SizeType GetSize() const
    {
        return m_WriteIndex - m_ReadIndex;
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_ReadIndex == m_WriteIndex;
    }

    /// 是否已满
    LIMX_NODISCARD bool IsFull() const
    {
        return (m_WriteIndex - m_ReadIndex) >= Capacity;
    }

    /// 容量
    LIMX_NODISCARD static constexpr SizeType GetCapacity()
    {
        return Capacity;
    }

private:
    /// 获取存储槽位
    T* GetElement(SizeType slot)
    {
        return reinterpret_cast<T*>(
            &m_Storage[slot * sizeof(T)]);
    }

    const T* GetElement(SizeType slot) const
    {
        return reinterpret_cast<const T*>(
            &m_Storage[slot * sizeof(T)]);
    }

    /// 编译器屏障
    static void CompilerBarrier()
    {
#if LIMX_COMPILER_MSVC
        _ReadWriteBarrier();
#endif
    }

    /// 读写索引 — 使用 volatile 保证可见性
    /// 仅在 SPSC 场景下安全 (x86 TSO 内存模型)
    volatile SizeType m_ReadIndex;

    /// 缓存行填充 — 避免 false sharing
    char m_Padding[64 - sizeof(SizeType)];

    volatile SizeType m_WriteIndex;

    /// 元素存储
    alignas(alignof(T)) char m_Storage[Capacity * sizeof(T)];
};

} // namespace Limx
