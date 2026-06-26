/*******************************************************************************
 * 文件: TDoubleBuffer.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   双缓冲 — 读写分离的双缓冲区模板
 *   一个缓冲区供读取，另一个供写入，通过交换实现无锁切换
 *   用于渲染帧数据、音频缓冲、状态快照等生产-消费场景
 *
 * 设计哲学:
 *   双份存储 — 始终持有两份数据副本
 *   交换而非拷贝 — Swap 仅切换索引，O(1) 操作
 *   读写分离 — 读端和写端可并发访问不同缓冲区
 *
 * 技术特性:
 *   - TDoubleBuffer<T>: 双缓冲容器
 *   - GetRead: 获取当前读缓冲区
 *   - GetWrite: 获取当前写缓冲区
 *   - Swap: 交换读写缓冲区
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

namespace Limx
{

/// 双缓冲
/// @tparam T 缓冲区数据类型
template<typename T>
class TDoubleBuffer
{
public:
    /// 默认构造
    TDoubleBuffer()
        : m_ReadIndex(0)
    {
    }

    /// 从初始值构造 (两个缓冲区均初始化为同一值)
    explicit TDoubleBuffer(const T& initialValue)
        : m_ReadIndex(0)
    {
        m_Buffers[0] = initialValue;
        m_Buffers[1] = initialValue;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 获取当前读缓冲区 (只读)
    LIMX_NODISCARD const T& GetRead() const
    {
        return m_Buffers[m_ReadIndex];
    }

    /// 获取当前写缓冲区 (可写)
    LIMX_NODISCARD T& GetWrite()
    {
        return m_Buffers[1 - m_ReadIndex];
    }

    /// 获取当前写缓冲区 (只读)
    LIMX_NODISCARD const T& GetWrite() const
    {
        return m_Buffers[1 - m_ReadIndex];
    }

    /// 获取当前读缓冲区 (可写, 慎用)
    LIMX_NODISCARD T& GetReadMutable()
    {
        return m_Buffers[m_ReadIndex];
    }

    // ========================================================================
    // 交换
    // ========================================================================

    /// 交换读写缓冲区
    void Swap()
    {
        m_ReadIndex = 1 - m_ReadIndex;
    }

    /// 交换并获取新的读缓冲区
    LIMX_NODISCARD const T& SwapAndGetRead()
    {
        Swap();
        return GetRead();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前读索引
    LIMX_NODISCARD SizeType GetReadIndex() const
    {
        return static_cast<SizeType>(m_ReadIndex);
    }

    /// 当前写索引
    LIMX_NODISCARD SizeType GetWriteIndex() const
    {
        return static_cast<SizeType>(1 - m_ReadIndex);
    }

private:
    T    m_Buffers[2];  ///< 双缓冲区
    Int32 m_ReadIndex;  ///< 当前读缓冲区索引 (0 或 1)
};

} // namespace Limx
