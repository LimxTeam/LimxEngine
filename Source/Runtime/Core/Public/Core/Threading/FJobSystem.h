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

    /// 递减并返回**递减后**的剩余值
    ///
    /// 调用方靠它判断"我是不是最后一个" —— 返回 0 即代表本次递减把计数
    /// 清空了, 收尾工作该由这一个调用者来做。
    ///
    /// 原先这里在 TAtomic::Decrement() 之后又减了一次 1。而 TAtomic 的
    /// Decrement 底层是 _InterlockedDecrement, 本就返回递减后的值 ——
    /// 多减的这一次让返回值比真实剩余数少 1。后果是把 N 个作业的完成回调
    /// 提前到第 N-1 个作业结束时触发, 最后一个还在跑。
    ///
    /// 而 IsComplete() 直接读计数, 并不受影响 —— 于是两个接口对"完成"
    /// 的判断彼此矛盾, 具体表现取决于调用方用了哪一个。
    LIMX_NODISCARD Int32 Decrement()
    {
        return m_Count.Decrement();
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

            // 按**值**捕获迭代体, 不能按引用。
            //
            // body 是一个引用参数, 而 Create 返回的批次通常比调用点活得久 ——
            // 最常见的写法就是直接传一个临时 lambda:
            //     FParallelFor::Create(n, 64, [](SizeType b, SizeType e){ ... });
            // 那个临时对象在整条语句结束时就析构了, 按引用捕获的话, 批次里
            // 每个作业都握着一根悬垂引用, 而它们要到之后才被执行。
            //
            // 这种错误不会在创建时报任何问题, 只会在执行时读到已释放的内存 ——
            // 小对象上往往还"碰巧能跑", 换个分配器或加点负载才崩。
            FJob job;
            job.EntryPoint = [body, rangeBegin, rangeEnd]()
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
