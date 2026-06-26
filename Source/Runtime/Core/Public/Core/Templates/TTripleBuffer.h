/*******************************************************************************
 * 文件: TTripleBuffer.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   三缓冲 — 无锁读写 + 中间态的三缓冲区模板
 *   写线程写入一个缓冲区，读线程读取另一个，第三个作为中间交换态
 *   比双缓冲更适合读写频率不对称的场景 (写快读慢或反之)
 *   用于渲染线程与逻辑线程数据交换、传感器数据流等场景
 *
 * 设计哲学:
 *   三份存储 — 写端、中间态、读端各持有独立缓冲区
 *   原子索引 — 使用原子操作交换索引，无锁实现
 *   写端发布 — 写完后将写缓冲与中间态交换
 *   读端获取 — 读前将中间态与读缓冲交换
 *
 * 技术特性:
 *   - TTripleBuffer<T>: 三缓冲容器
 *   - GetWrite: 获取写缓冲区
 *   - PublishWrite: 发布写缓冲到中间态
 *   - GetRead: 获取读缓冲区
 *   - ConsumeRead: 从中间态获取最新数据
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"

// MSVC 原子内联函数
extern "C" long _InterlockedExchange(long volatile*, long);
#pragma intrinsic(_InterlockedExchange)

namespace Limx
{

/// 三缓冲
/// @tparam T 缓冲区数据类型
template<typename T>
class TTripleBuffer
{
public:
    /// 默认构造
    TTripleBuffer()
        : m_WriteIndex(0)
        , m_MiddleIndex(1)
        , m_ReadIndex(2)
        , m_HasNewData(0)
    {
    }

    /// 从初始值构造
    explicit TTripleBuffer(const T& initialValue)
        : m_WriteIndex(0)
        , m_MiddleIndex(1)
        , m_ReadIndex(2)
        , m_HasNewData(0)
    {
        m_Buffers[0] = initialValue;
        m_Buffers[1] = initialValue;
        m_Buffers[2] = initialValue;
    }

    // 不可拷贝/移动 (含原子状态)
    TTripleBuffer(const TTripleBuffer&) = delete;
    TTripleBuffer& operator=(const TTripleBuffer&) = delete;
    TTripleBuffer(TTripleBuffer&&) = delete;
    TTripleBuffer& operator=(TTripleBuffer&&) = delete;

    // ========================================================================
    // 写端 (仅写线程调用)
    // ========================================================================

    /// 获取写缓冲区 (可写)
    LIMX_NODISCARD T& GetWrite()
    {
        return m_Buffers[m_WriteIndex];
    }

    /// 获取写缓冲区 (只读)
    LIMX_NODISCARD const T& GetWrite() const
    {
        return m_Buffers[m_WriteIndex];
    }

    /// 发布写缓冲 — 将写缓冲与中间态交换
    void PublishWrite()
    {
        // 交换写索引和中间索引
        long oldMiddle = _InterlockedExchange(
            reinterpret_cast<long volatile*>(&m_MiddleIndex),
            static_cast<long>(m_WriteIndex));
        m_WriteIndex = static_cast<Int32>(oldMiddle);

        // 标记有新数据
        _InterlockedExchange(
            reinterpret_cast<long volatile*>(&m_HasNewData),
            1);
    }

    // ========================================================================
    // 读端 (仅读线程调用)
    // ========================================================================

    /// 获取读缓冲区 (只读)
    LIMX_NODISCARD const T& GetRead() const
    {
        return m_Buffers[m_ReadIndex];
    }

    /// 获取读缓冲区 (可写, 慎用)
    LIMX_NODISCARD T& GetReadMutable()
    {
        return m_Buffers[m_ReadIndex];
    }

    /// 从中间态消费最新数据到读缓冲
    /// @return 是否有新数据被消费
    bool ConsumeRead()
    {
        // 检查是否有新数据
        long hadNew = _InterlockedExchange(
            reinterpret_cast<long volatile*>(&m_HasNewData),
            0);

        if (hadNew == 0) return false;

        // 交换读索引和中间索引
        long oldMiddle = _InterlockedExchange(
            reinterpret_cast<long volatile*>(&m_MiddleIndex),
            static_cast<long>(m_ReadIndex));
        m_ReadIndex = static_cast<Int32>(oldMiddle);

        return true;
    }

    /// 是否有未消费的新数据
    LIMX_NODISCARD bool HasNewData() const
    {
        return m_HasNewData != 0;
    }

private:
    T          m_Buffers[3];      ///< 三个缓冲区
    Int32      m_WriteIndex;      ///< 写缓冲区索引
    volatile long m_MiddleIndex;  ///< 中间态索引 (原子操作)
    Int32      m_ReadIndex;       ///< 读缓冲区索引
    volatile long m_HasNewData;   ///< 是否有新数据标志
};

} // namespace Limx
