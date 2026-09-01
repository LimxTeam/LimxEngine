// ============================================================
// 文件名称：FRayTracingScene.cpp
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：跳过一个对象永远要留下痕迹。几何体无效时不建 BLAS 是对的，
//          但"全部都被跳过了"与"场景是空的"必须分得开 —— 后者会让任何
//          光追判据在一棵空树上满分通过，而空树不崩不报错，画面上与
//          "这里本来就没东西"一模一样。
// 功能描述：从渲染对象列表构建并维护光追加速结构 — 逐对象 BLAS、单个
//          TLAS、每帧刷新实例变换与重建 TLAS。
// 技术特性：BLAS 与 TLAS 分开重建 (几何体不变时只重建 TLAS)；实例的
//          CustomIndex 存的是**源对象下标**而不是实例序号，跳过之后
//          两者不再相等。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ Initialize()                   │ 建 TLAS                   │
// │ Shutdown()                     │ 销毁全部加速结构           │
// │ DestroyBlas()                  │ 只销毁 BLAS               │
// │ RebuildGeometry()              │ 按对象列表重建全部 BLAS    │
// │ UpdateInstances()              │ 刷新实例变换              │
// │ RecordBuild()                  │ 录构建命令                │
// ============================================================

#include "Renderer/RayTracing/FRayTracingScene.h"

#include "Renderer/Renderer/FRenderer.h"

#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

// ============================================================================
// 生命周期
// ============================================================================

ERHIResult FRayTracingScene::Initialize(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    if (!device->IsRayTracingSupported())
    {
        // 不是错误 —— 调用方会据此走不带光追的路径。但要说出来, 否则
        // "这台机器没有光追"与"光追代码没接上"在日志里分不开。
        LIMX_LOG(LogRenderer, Display,
                 "[光追场景] 设备不支持光线追踪 — 不建加速结构");
        return ERHIResult::ErrorIncompatibleDriver;
    }

    m_Device = device;

    FRHITlasDesc desc;
    desc.MaxInstanceCount = kMaxInstances;
    desc.PreferFastTrace  = true;
    desc.DebugName        = "SceneTlas";

    const ERHIResult result = device->CreateTopLevelAS(desc, m_Tlas);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[光追场景] TLAS 创建失败");
        m_Device = nullptr;
        return result;
    }

    // 几何表 —— 主机可写, 因为它随 BLAS 一起重建
    {
        FRHIBufferDesc tableDesc;
        tableDesc.Size        = GetGeometryTableBytes();
        tableDesc.Usage       = EBufferUsage::StorageBuffer;
        tableDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        tableDesc.DebugName   = "RayTracingGeometryTable";

        const ERHIResult tableResult =
            device->CreateBuffer(tableDesc, m_GeometryTable);

        if (!IsRHISuccess(tableResult))
        {
            LIMX_LOG(LogRenderer, Error, "[光追场景] 几何表创建失败");
            device->DestroyAccelStruct(m_Tlas);
            m_Device = nullptr;
            return tableResult;
        }

        // 整块清零。
        //
        // 被跳过的对象在表里留一个全零的条目, 而全零的设备地址在着色器里
        // 取值会立刻崩 —— 那比读到一块随机内存好: 后者给出的是"材质随机
        // 串位"这种要查几天的现象。
        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(m_GeometryTable, &mapped)) &&
            mapped != nullptr)
        {
            Memory::MemZero(mapped,
                            static_cast<SizeType>(GetGeometryTableBytes()));
            device->UnmapBuffer(m_GeometryTable);
        }
    }

    LIMX_LOG(LogRenderer, Display,
             "[光追场景] TLAS 就绪 — 实例数上限 {}", kMaxInstances);

    return ERHIResult::Success;
}

void FRayTracingScene::DestroyBlas()
{
    if (m_Device == nullptr)
    {
        return;
    }

    for (SizeType i = 0; i < m_Blas.GetSize(); ++i)
    {
        m_Device->DestroyAccelStruct(m_Blas[i]);
    }

    m_Blas.Clear();
    m_SourceIndices.Clear();
    m_InstanceCount = 0;
}

void FRayTracingScene::Shutdown()
{
    DestroyBlas();

    if (m_Device != nullptr)
    {
        m_Device->DestroyBuffer(m_GeometryTable);

        if (m_Tlas.IsValid())
        {
            m_Device->DestroyAccelStruct(m_Tlas);
        }
    }

    m_Device = nullptr;
    m_SkippedCount = 0;
}

// ============================================================================
// 几何体
// ============================================================================

UInt64 FRayTracingScene::ComputeGeometrySignature(
    const TArray<FRenderObject>& objects)
{
    // FNV-1a 64 位。
    //
    // 只混入决定 BLAS 形状的字段 —— 变换不在内, 因为物体移动只需要重建
    // TLAS。把变换也混进来的话每帧都会重建全部 BLAS, 那是几十倍的开销
    // 而画面完全一样, 属于"慢得看不出原因"的那类问题。
    //
    // 反过来漏掉一个决定形状的字段更糟: 换了网格而签名不变, 于是光追里
    // 的形状停留在上一个场景。所以这里逐个列全, 加字段时必须同步加进来。
    UInt64 hash = 14695981039346656037ULL;

    const auto mix = [&hash](UInt64 value)
    {
        for (UInt32 byte = 0; byte < 8; ++byte)
        {
            hash ^= (value >> (byte * 8)) & 0xFFu;
            hash *= 1099511628211ULL;
        }
    };

    mix(static_cast<UInt64>(objects.GetSize()));

    for (SizeType i = 0; i < objects.GetSize(); ++i)
    {
        const FRenderObject& object = objects[i];

        // 索引与代都要混进来 —— 只看索引的话, 一个槽位被回收再分配给
        // 另一份几何体时签名不变, 而那正是"换场景"最常见的形态。
        mix(static_cast<UInt64>(object.VertexBuffer.GetIndex()));
        mix(static_cast<UInt64>(object.VertexBuffer.GetGeneration()));
        mix(static_cast<UInt64>(object.IndexBuffer.GetIndex()));
        mix(static_cast<UInt64>(object.IndexBuffer.GetGeneration()));
        mix(static_cast<UInt64>(object.VertexCount));
        mix(static_cast<UInt64>(object.VertexStride));
        mix(static_cast<UInt64>(object.IndexOffset));
        mix(static_cast<UInt64>(object.IndexCount));
        mix(static_cast<UInt64>(object.IndexType));
    }

    return hash;
}

ERHIResult FRayTracingScene::Update(const TArray<FRenderObject>& objects)
{
    if (m_Device == nullptr || !m_Tlas.IsValid())
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    const UInt64 signature = ComputeGeometrySignature(objects);

    if (signature != m_GeometrySignature || m_Blas.GetSize() == 0)
    {
        m_GeometrySignature = signature;
        m_NeedsBlasBuild    = true;

        return RebuildGeometry(objects);
    }

    // 几何体没变 —— 只刷新变换, BLAS 原样留着。
    return UpdateInstances(objects);
}

ERHIResult FRayTracingScene::RebuildGeometry(
    const TArray<FRenderObject>& objects)
{
    if (m_Device == nullptr || !m_Tlas.IsValid())
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    DestroyBlas();
    m_SkippedCount = 0;

    const SizeType objectCount = objects.GetSize();

    m_Blas.Reserve(objectCount);
    m_SourceIndices.Reserve(objectCount);

    // 整个重建期间把几何表映射着 —— 逐条 Map/Unmap 是几百次系统调用,
    // 而这块内存是持久映射的, Map 只是取一个已有的指针。
    FRayTracingGeometryEntry* tableEntries = nullptr;

    {
        void* mapped = nullptr;

        if (IsRHISuccess(m_Device->MapBuffer(m_GeometryTable, &mapped)) &&
            mapped != nullptr)
        {
            tableEntries = static_cast<FRayTracingGeometryEntry*>(mapped);

            // 每次重建都整块清零: 上一个场景的条目留在表里的话, 这一次
            // 被跳过的位置会读到**上一个场景的几何体**, 而那是"反射里
            // 出现了已经不存在的东西"。
            Memory::MemZero(tableEntries,
                            static_cast<SizeType>(GetGeometryTableBytes()));
        }
        else
        {
            LIMX_LOG(LogRenderer, Error, "[光追场景] 几何表映射失败");
        }
    }

    for (SizeType i = 0; i < objectCount; ++i)
    {
        const FRenderObject& object = objects[i];

        const UInt32 sourceIndex = static_cast<UInt32>(i);

        // 超出实例上限就停 —— 而且要说出来。
        //
        // 静默截断的后果是超出的那些物体在光栅化里在、在光追里不在:
        // 反射与光追阴影里凭空少一块, 而画面上没有任何错误提示。
        if (m_Blas.GetSize() >= kMaxInstances)
        {
            LIMX_LOG(LogRenderer, Error,
                     "[光追场景] 对象数 {} 超过实例上限 {} — "
                     "多出的部分不会出现在光追里",
                     objectCount, kMaxInstances);
            break;
        }

        // 几何体无效的对象跳过。跨度为 0 是"谁忘了填 VertexStride",
        // 与"这个对象本来就没有三角形"是两回事, 所以分开报。
        if (object.VertexStride == 0)
        {
            LIMX_LOG(LogRenderer, Error,
                     "[光追场景] 对象 {} ({}) 的顶点跨度为 0 — "
                     "构造它的地方漏填了 VertexStride",
                     i, object.DebugName);
            ++m_SkippedCount;
            continue;
        }

        if (object.VertexCount == 0 || object.IndexCount == 0 ||
            (object.IndexCount % 3) != 0)
        {
            ++m_SkippedCount;
            continue;
        }

        FRHIAccelStructGeometry geometry;
        geometry.VertexBuffer = object.VertexBuffer;
        geometry.VertexOffset = 0;
        geometry.VertexCount  = object.VertexCount;
        geometry.VertexStride = object.VertexStride;
        geometry.VertexFormat = EPixelFormat::RGB32_SFLOAT;
        geometry.IndexBuffer  = object.IndexBuffer;

        // IndexOffset 的单位是索引个数, 而这里要的是字节。
        //
        // 忘了乘位宽的后果是子网格全都从缓冲区开头附近取三角形 —— 每个
        // 子网格的加速结构都是"第一个子网格的一部分"。画面照旧, 只有光追
        // 里的形状不对。
        geometry.IndexOffset =
            static_cast<UInt64>(object.IndexOffset) *
            GetIndexTypeByteSize(object.IndexType);

        geometry.IndexCount = object.IndexCount;
        geometry.IndexType  = object.IndexType;

        // 蒙版材质在这里当作不透明。
        //
        // ray query 没有 any-hit, 评估不了 alpha 测试 —— 要正确处理必须
        // 把它们标成非不透明并在着色器里自己查纹理, 那是后面的事。现在
        // 的后果是蒙版几何体在光追里按完整三角形参与遮挡, 这一点必须
        // 写下来, 否则将来对不上时会去查加速结构而不是查这里。
        geometry.Opaque = true;

        FRHIBlasDesc blasDesc;
        blasDesc.Geometries    = &geometry;
        blasDesc.GeometryCount = 1;
        blasDesc.PreferFastTrace = true;
        blasDesc.DebugName     = object.DebugName;

        FRHIAccelStructHandle blas;

        if (!IsRHISuccess(m_Device->CreateBottomLevelAS(blasDesc, blas)))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[光追场景] 对象 {} ({}) 的 BLAS 创建失败",
                     i, object.DebugName);
            ++m_SkippedCount;
            continue;
        }

        m_Blas.Add(blas);
        m_SourceIndices.Add(static_cast<UInt32>(i));

        // 几何表的条目 —— 与 BLAS 的几何描述用**同一组数**。
        //
        // 分别算两遍的话, 两处只要有一处漂移, 光追命中的三角形与着色器
        // 取到的顶点就不是同一个 —— 那表现为"反射里的法线乱跳"。
        if (sourceIndex < kMaxInstances && tableEntries != nullptr)
        {
            FRayTracingGeometryEntry& entry = tableEntries[sourceIndex];

            entry.VertexAddress =
                m_Device->GetBufferDeviceAddress(object.VertexBuffer) +
                geometry.VertexOffset;

            entry.IndexAddress =
                m_Device->GetBufferDeviceAddress(object.IndexBuffer) +
                geometry.IndexOffset;

            entry.VertexStride = object.VertexStride;
            entry.IndexType =
                (object.IndexType == EIndexType::UInt16) ? 0u : 1u;
            entry.MaterialIndex = object.BindlessMaterialIndex;
        }
    }

    if (tableEntries != nullptr)
    {
        m_Device->UnmapBuffer(m_GeometryTable);
    }

    LIMX_LOG(LogRenderer, Display,
             "[光追场景] BLAS {} 个 (源对象 {} 个, 跳过 {} 个)",
             m_Blas.GetSize(), objectCount, m_SkippedCount);

    return UpdateInstances(objects);
}

// ============================================================================
// 实例
// ============================================================================

ERHIResult FRayTracingScene::UpdateInstances(
    const TArray<FRenderObject>& objects)
{
    if (m_Device == nullptr || !m_Tlas.IsValid())
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    const SizeType blasCount = m_Blas.GetSize();

    if (blasCount == 0)
    {
        m_InstanceCount = 0;
        return ERHIResult::Success;
    }

    TArray<FRHIAccelStructInstance> instances;
    instances.Reserve(blasCount);

    m_ClassCounts[0] = 0;
    m_ClassCounts[1] = 0;
    m_ClassCounts[2] = 0;

    for (SizeType i = 0; i < blasCount; ++i)
    {
        const UInt32 sourceIndex = m_SourceIndices[i];

        // 源列表变短了 (换了场景却没重建 BLAS) 时下标会越界。
        //
        // 这里判掉而不是相信调用方: 越界读到的是一块随机内存里的
        // FTransform, 结果是某个物体在光追里飞到天上去 —— 而那看起来
        // 像加速结构算错了。
        if (sourceIndex >= objects.GetSize())
        {
            LIMX_LOG(LogRenderer, Error,
                     "[光追场景] 实例 {} 的源下标 {} 越出对象列表长度 {} — "
                     "几何体变了却没重建 BLAS?",
                     i, sourceIndex, objects.GetSize());
            return ERHIResult::ErrorInvalidParameter;
        }

        const FRenderObject& object = objects[sourceIndex];

        const FMatrix model = object.Transform.ToMatrix();

        FRHIAccelStructInstance instance;

        // FMatrix 是行主序 M[行][列], 加速结构的实例变换也是行主序的
        // 3x4 —— 直接逐元素搬, 不转置。转置了的话物体会绕原点乱转,
        // 而单位变换下转置又是恒等的, 于是只有旋转过的物体才出错。
        for (UInt32 row = 0; row < 3; ++row)
        {
            for (UInt32 col = 0; col < 4; ++col)
            {
                instance.Transform[row * 4 + col] = model.M[row][col];
            }
        }

        // 自定义下标存**源对象下标**, 不是实例序号。
        //
        // 着色器据此去逐物体缓冲区里取材质 —— 而那个缓冲区是按源列表
        // 排的。存实例序号的话, 只要有一个对象被跳过, 后面所有物体的
        // 材质就整体错位一格。
        instance.CustomIndex = sourceIndex;
        instance.Blas        = m_Blas[i];

        // 掩码按混合模式分 —— 理由见 FRayTracingScene.h 顶部。
        //
        // 全部填 0xFF 的后果不是"多看见一些": 半透明几何体在光栅化里
        // 不写深度, 于是光追说"这里被玻璃挡住了", 深度缓冲区说"这里能看到
        // 玻璃后面的墙"。两边永远对不上, 而那看起来像加速结构算错了。
        switch (object.BlendMode)
        {
        case EMaterialBlendMode::Masked:
            instance.Mask = kRayMaskMasked;
            ++m_ClassCounts[1];
            break;

        case EMaterialBlendMode::Translucent:
            instance.Mask = kRayMaskTranslucent;
            ++m_ClassCounts[2];
            break;

        case EMaterialBlendMode::Opaque:
        default:
            instance.Mask = kRayMaskOpaque;
            ++m_ClassCounts[0];
            break;
        }

        instances.Add(instance);
    }

    m_InstanceCount = static_cast<UInt32>(instances.GetSize());

    return m_Device->UpdateTlasInstances(
        m_Tlas, instances.GetData(), m_InstanceCount);
}

// ============================================================================
// 构建
// ============================================================================

void FRayTracingScene::RecordBuild(IRHICommandBuffer* commandBuffer)
{
    if (commandBuffer == nullptr || !m_Tlas.IsValid() || m_InstanceCount == 0)
    {
        return;
    }

    if (m_NeedsBlasBuild)
    {
        m_NeedsBlasBuild = false;

        for (SizeType i = 0; i < m_Blas.GetSize(); ++i)
        {
            commandBuffer->BuildAccelStruct(m_Blas[i], 0);
        }

        // TLAS 的构建要读 BLAS 的内容 —— 少了这道屏障, 结果取决于驱动
        // 碰巧怎么调度。它多半是对的, 直到某一台机器上不对。
        commandBuffer->AccelStructBarrier();
    }

    commandBuffer->BuildAccelStruct(m_Tlas, m_InstanceCount);
    commandBuffer->AccelStructBarrier();
}

} // namespace Limx
