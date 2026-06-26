/*******************************************************************************
 * 文件: FJobSystem.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   作业系统 — 轻量级任务提交与依赖管理框架
 *   支持提交作业、设置依赖关系、等待完成
 *   用于并行化渲染准备、物理计算、资源加载等 CPU 密集任务
 *
 * 设计哲学:
 *   作业描述 — FJob 描述一个可执行的工作单元
 *   计数器依赖 — FJobCounter 原子计数器表示前置条件
 *   无线程池 — 仅定义作业接口和依赖模型，线程池由 FTaskGraph 提供
 *
 * 技术特性:
 *   - FJob: 作业描述 (回调 + 优先级 + 依赖计数器)
 *   - FJobCounter: 原子计数器 (依赖追踪)
 *   - FJobBatch: 批量作业提交
 *   - 优先级: High/Normal/Low
 *
 * 依赖关系:
 *   内部: Core/Threading/FAtomic.h, Core/Templates/TFunction.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Threading/FAtomic.h"
#include "Core/Templates/TFunction.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 作业优先级
enum class JobPriority : UInt8
{
    High   = 0,  ///< 高优先级 (帧关键路径)
    Normal = 1,  ///< 普通优先级 (默认)
    Low    = 2   ///< 低优先级 (后台任务)
};

/// 作业计数器 — 原子计数器，用于依赖追踪
class FJobCounter
{
public:
    FJobCounter() { m_Count.Store(0); }
    explicit FJobCounter(Int32 initialCount)
    {
        m_Count.Store(initialCount);
    }

    /// 递增
    void Increment() { m_Count.Increment(); }

    /// 递减并返回新值
    LIMX_NODISCARD Int32 Decrement()
    {
        return m_Count.Decrement() - 1;
    }

    /// 当前值
    LIMX_NODISCARD Int32 GetValue() const
    {
        return m_Count.Load();
    }

    /// 是否已完成 (值为 0)
    LIMX_NODISCARD bool IsComplete() const
    {
        return m_Count.Load() == 0;
    }

    /// 重置
    void Reset(Int32 value = 0) { m_Count.Store(value); }

private:
    TAtomic<Int32> m_Count;
};

/// 作业描述 — 一个可执行的工作单元
struct FJob
{
    TFunction<void()>  EntryPoint;    ///< 作业入口函数
    JobPriority        Priority;      ///< 优先级
    FJobCounter*       DependsOn;     ///< 前置依赖计数器 (可选)
    FJobCounter*       SignalOnComplete; ///< 完成时递减的计数器 (可选)

    FJob()
        : Priority(JobPriority::Normal)
        , DependsOn(nullptr)
        , SignalOnComplete(nullptr)
    {
    }

    FJob(TFunction<void()> entryPoint,
         JobPriority priority = JobPriority::Normal)
        : EntryPoint(MoveTemp(entryPoint))
        , Priority(priority)
        , DependsOn(nullptr)
        , SignalOnComplete(nullptr)
    {
    }

    /// 是否就绪 (无依赖或依赖已满足)
    LIMX_NODISCARD bool IsReady() const
    {
        return !DependsOn || DependsOn->IsComplete();
    }
};

/// 作业批次 — 收集多个作业统一提交
class FJobBatch
{
public:
    FJobBatch() = default;
    ~FJobBatch() = default;

    /// 添加作业
    void Add(FJob job)
    {
        m_Jobs.Add(MoveTemp(job));
    }

    /// 添加简单作业 (仅回调)
    void Add(TFunction<void()> entryPoint,
             JobPriority priority = JobPriority::Normal)
    {
        m_Jobs.Add(FJob(MoveTemp(entryPoint), priority));
    }

    /// 设置所有作业完成时递减的计数器
    void SetCompletionCounter(FJobCounter* counter)
    {
        for (SizeType index = 0;
             index < m_Jobs.GetSize(); ++index)
        {
            m_Jobs[index].SignalOnComplete = counter;
        }
    }

    /// 设置所有作业的前置依赖
    void SetDependency(FJobCounter* dependency)
    {
        for (SizeType index = 0;
             index < m_Jobs.GetSize(); ++index)
        {
            m_Jobs[index].DependsOn = dependency;
        }
    }

    /// 获取作业数组
    LIMX_NODISCARD TArray<FJob>& GetJobs() { return m_Jobs; }
    LIMX_NODISCARD const TArray<FJob>& GetJobs() const
    {
        return m_Jobs;
    }

    /// 作业数量
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Jobs.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Jobs.IsEmpty();
    }

    /// 清空
    void Clear() { m_Jobs.Clear(); }

private:
    TArray<FJob> m_Jobs;
};

/// 并行 For 辅助 — 将范围 [0, count) 拆分为多个作业
class FParallelFor
{
public:
    /// 创建并行 For 作业批次
    /// @param count       总迭代次数
    /// @param batchSize   每个作业处理的迭代数
    /// @param body        迭代体 (参数: 起始索引, 结束索引)
    /// @param completion  完成计数器 (可选)
    LIMX_NODISCARD static FJobBatch Create(
        SizeType count,
        SizeType batchSize,
        const TFunction<void(SizeType, SizeType)>& body,
        FJobCounter* completion = nullptr)
    {
        FJobBatch batch;

        if (count == 0) return batch;
        if (batchSize == 0) batchSize = 1;

        SizeType batchCount = (count + batchSize - 1) / batchSize;

        if (completion)
        {
            completion->Reset(static_cast<Int32>(batchCount));
        }

        for (SizeType batchIndex = 0;
             batchIndex < batchCount; ++batchIndex)
        {
            SizeType rangeBegin = batchIndex * batchSize;
            SizeType rangeEnd = rangeBegin + batchSize;
            if (rangeEnd > count) rangeEnd = count;

            // 捕获 body 引用和范围
            FJob job;
            job.EntryPoint = [&body, rangeBegin, rangeEnd]()
            {
                body(rangeBegin, rangeEnd);
            };
            job.Priority = JobPriority::Normal;
            job.SignalOnComplete = completion;

            batch.Add(MoveTemp(job));
        }

        return batch;
    }
};

} // namespace Limx
