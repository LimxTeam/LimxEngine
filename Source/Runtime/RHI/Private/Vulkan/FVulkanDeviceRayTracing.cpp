// ============================================================
// 文件名称：FVulkanDeviceRayTracing.cpp
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：加速结构的"创建"与"构建"彻底分开 — 创建只分配存储并拿到句柄，
//          构建必须排进命令流在 GPU 上执行。分开的理由是构建需要队列而
//          创建不需要；混在一起会逼着资源创建路径去持有命令缓冲区。
//          每个加速结构自带存储、暂存、实例三块缓冲区，跟着它一起活到
//          销毁 —— 因为重建 (每帧更新 TLAS) 是常态而不是例外。
// 功能描述：FVulkanDevice 的光线追踪加速结构实现 — 扩展函数载入、BLAS 与
//          TLAS 的创建/销毁、TLAS 实例数组上传、加速结构与缓冲区的设备
//          地址查询。
// 技术特性：暂存缓冲区按 minAccelerationStructureScratchOffsetAlignment
//          对齐 (超额分配后把地址向上取整)；扩展函数逐个
//          vkGetDeviceProcAddr，任一缺失即把整个光追能力判为不可用；
//          实例的 CustomIndex 超过 24 位时报错而不是静默截断。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ LoadRayTracingFunctions()      │ 载入五个扩展函数入口       │
// │ CreateAccelStructCommon()      │ 创建的公共部分            │
// │ CreateBottomLevelAS()          │ 创建 BLAS (三角形)        │
// │ CreateTopLevelAS()             │ 创建 TLAS (实例)          │
// │ DestroyAccelStruct()           │ 销毁加速结构与三块缓冲区   │
// │ UpdateTlasInstances()          │ 上传 TLAS 实例数组        │
// │ GetAccelStructDeviceAddress()  │ 取加速结构设备地址        │
// │ GetBufferDeviceAddress()       │ 取缓冲区设备地址          │
// ============================================================

#include "Vulkan/FVulkanDevice.h"

namespace Limx
{

// ============================================================================
// 扩展函数载入
// ============================================================================

void FVulkanDevice::LoadRayTracingFunctions()
{
    if (!m_RayTracingAvailable)
    {
        return;
    }

    m_RayTracingFunctions.Create =
        reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            vkGetDeviceProcAddr(m_Device, "vkCreateAccelerationStructureKHR"));
    m_RayTracingFunctions.Destroy =
        reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            vkGetDeviceProcAddr(m_Device, "vkDestroyAccelerationStructureKHR"));
    m_RayTracingFunctions.GetBuildSizes =
        reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            vkGetDeviceProcAddr(m_Device,
                "vkGetAccelerationStructureBuildSizesKHR"));
    m_RayTracingFunctions.CmdBuild =
        reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(m_Device,
                "vkCmdBuildAccelerationStructuresKHR"));
    m_RayTracingFunctions.GetDeviceAddress =
        reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            vkGetDeviceProcAddr(m_Device,
                "vkGetAccelerationStructureDeviceAddressKHR"));

    if (!m_RayTracingFunctions.IsComplete())
    {
        // 扩展报告可用却拿不到函数入口 —— 这是驱动出了问题, 而不是"这台机器
        // 没有光追"。把能力整个判假, 否则后面每一次调用都是空指针解引用。
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 光追扩展已启用但函数入口缺失 — "
            "Create:{} Destroy:{} GetBuildSizes:{} CmdBuild:{} GetAddr:{}",
            m_RayTracingFunctions.Create != nullptr,
            m_RayTracingFunctions.Destroy != nullptr,
            m_RayTracingFunctions.GetBuildSizes != nullptr,
            m_RayTracingFunctions.CmdBuild != nullptr,
            m_RayTracingFunctions.GetDeviceAddress != nullptr);

        m_RayTracingAvailable = false;
        return;
    }

    // 加速结构的设备限制。这个结构必须通过 GetProperties2 查, 而不是
    // GetProperties —— 后者只给核心限制。
    m_AccelStructProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

    VkPhysicalDeviceProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &m_AccelStructProperties;

    vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &props2);

    LIMX_LOG(LogRHI, Display,
        "[Vulkan] 光追就绪 — 最大几何体:{} 最大实例:{} 最大图元:{} "
        "暂存对齐:{}",
        m_AccelStructProperties.maxGeometryCount,
        m_AccelStructProperties.maxInstanceCount,
        m_AccelStructProperties.maxPrimitiveCount,
        m_AccelStructProperties.minAccelerationStructureScratchOffsetAlignment);
}

// ============================================================================
// 设备地址
// ============================================================================

UInt64 FVulkanDevice::GetBufferDeviceAddress(FRHIBufferHandle handle) const
{
    const FVulkanBufferData* data = m_Buffers.Get(handle);

    if (data == nullptr || data->Buffer == VK_NULL_HANDLE)
    {
        return 0;
    }

    VkBufferDeviceAddressInfo info = {};
    info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = data->Buffer;

    return static_cast<UInt64>(vkGetBufferDeviceAddress(m_Device, &info));
}

UInt64 FVulkanDevice::GetAccelStructDeviceAddress(
    FRHIAccelStructHandle handle) const
{
    const FVulkanAccelStructData* data = m_AccelStructs.Get(handle);

    if (data == nullptr || data->AccelStruct == VK_NULL_HANDLE ||
        m_RayTracingFunctions.GetDeviceAddress == nullptr)
    {
        return 0;
    }

    VkAccelerationStructureDeviceAddressInfoKHR info = {};
    info.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    info.accelerationStructure = data->AccelStruct;

    return static_cast<UInt64>(
        m_RayTracingFunctions.GetDeviceAddress(m_Device, &info));
}

// ============================================================================
// 创建的公共部分
// ============================================================================

ERHIResult FVulkanDevice::CreateAccelStructCommon(
    FVulkanAccelStructData& data,
    const VkAccelerationStructureBuildGeometryInfoKHR& buildInfo,
    UInt32 primitiveCount,
    const char* debugName,
    FRHIAccelStructHandle& outHandle)
{
    // ------------------------------------------------------------------
    // 问驱动要多大
    //
    // 尺寸取决于图元数与构建标志, 不能自己算 —— 不同驱动的加速结构内部
    // 表示完全不同。
    // ------------------------------------------------------------------
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
    sizeInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

    m_RayTracingFunctions.GetBuildSizes(
        m_Device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo,
        &primitiveCount,
        &sizeInfo);

    if (sizeInfo.accelerationStructureSize == 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 加速结构尺寸查询返回 0 — 图元数:{}", primitiveCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    // ------------------------------------------------------------------
    // 存储缓冲区
    // ------------------------------------------------------------------
    FRHIBufferDesc storageDesc;
    storageDesc.Size = sizeInfo.accelerationStructureSize;
    storageDesc.Usage = EBufferUsage::AccelStructStorage |
                        EBufferUsage::ShaderDeviceAddress;
    storageDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    storageDesc.DebugName = debugName;

    ERHIResult result = CreateBuffer(storageDesc, data.StorageBuffer);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ------------------------------------------------------------------
    // 暂存缓冲区 —— 地址必须对齐
    //
    // 规范要求 scratchData.deviceAddress 是
    // minAccelerationStructureScratchOffsetAlignment 的整数倍。缓冲区自己的
    // 设备地址由驱动决定, 不保证满足这个对齐, 所以多要一段, 构建时把地址
    // 向上取整。多要的量正好是 (对齐 - 1), 够任何情况下的取整。
    //
    // 不对齐的后果是构建行为未定义 —— 在验证层之外表现为"结果偶尔不对",
    // 而那种偶发的错误几乎没法定位。
    // ------------------------------------------------------------------
    const UInt64 reportedAlign = static_cast<UInt64>(
        m_AccelStructProperties.minAccelerationStructureScratchOffsetAlignment);
    const UInt64 scratchAlign = (reportedAlign > 0) ? reportedAlign : 1;

    FRHIBufferDesc scratchDesc;
    scratchDesc.Size = sizeInfo.buildScratchSize + scratchAlign - 1;
    scratchDesc.Usage = EBufferUsage::StorageBuffer |
                        EBufferUsage::ShaderDeviceAddress;
    scratchDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    scratchDesc.DebugName = "AccelStructScratch";

    result = CreateBuffer(scratchDesc, data.ScratchBuffer);

    if (!IsRHISuccess(result))
    {
        DestroyBuffer(data.StorageBuffer);
        return result;
    }

    // ------------------------------------------------------------------
    // 加速结构本身 —— 只是存储缓冲区上的一个"视图"
    // ------------------------------------------------------------------
    VkAccelerationStructureCreateInfoKHR createInfo = {};
    createInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = GetVkBuffer(data.StorageBuffer);
    createInfo.offset = 0;
    createInfo.size   = sizeInfo.accelerationStructureSize;
    createInfo.type   = buildInfo.type;

    const VkResult vkResult = m_RayTracingFunctions.Create(
        m_Device, &createInfo, nullptr, &data.AccelStruct);

    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateAccelerationStructureKHR 失败 — 结果:{}",
            static_cast<Int32>(vkResult));

        DestroyBuffer(data.ScratchBuffer);
        DestroyBuffer(data.StorageBuffer);
        return ERHIResult::ErrorUnknown;
    }

    data.PrimitiveCount = primitiveCount;
    data.BuildFlags     = buildInfo.flags;

    outHandle = m_AccelStructs.Allocate(data);

    return ERHIResult::Success;
}

// ============================================================================
// BLAS
// ============================================================================

ERHIResult FVulkanDevice::CreateBottomLevelAS(
    const FRHIBlasDesc& desc, FRHIAccelStructHandle& outHandle)
{
    if (!m_RayTracingAvailable)
    {
        LIMX_LOG(LogRHI, Error, "[Vulkan] 设备不支持光追 — BLAS 创建失败");
        return ERHIResult::ErrorIncompatibleDriver;
    }

    if (desc.Geometries == nullptr || desc.GeometryCount == 0)
    {
        LIMX_LOG(LogRHI, Error, "[Vulkan] BLAS 需要至少一份几何体");
        return ERHIResult::ErrorInvalidParameter;
    }

    // 目前只支持单几何体。多几何体要求把 primitiveCount 做成数组、
    // 把 VertexAddress 等字段做成每几何体一份 —— 那是实打实的一段工作,
    // 而不是"顺手加个循环"。在支持之前明确报错, 不做只处理第一份的假支持。
    if (desc.GeometryCount != 1)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] BLAS 目前只支持单几何体 — 收到 {} 份",
            desc.GeometryCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    const FRHIAccelStructGeometry& geom = desc.Geometries[0];

    if (geom.IndexCount == 0 || (geom.IndexCount % 3) != 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] BLAS 的索引数必须是 3 的正整数倍 — 收到 {}",
            geom.IndexCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    if (geom.VertexCount == 0 || geom.VertexStride == 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] BLAS 的顶点数与跨度都必须非零 — 顶点:{} 跨度:{}",
            geom.VertexCount, geom.VertexStride);
        return ERHIResult::ErrorInvalidParameter;
    }

    const UInt64 vertexAddress = GetBufferDeviceAddress(geom.VertexBuffer);
    const UInt64 indexAddress  = GetBufferDeviceAddress(geom.IndexBuffer);

    // 地址为 0 的原因通常是缓冲区创建时漏了 ShaderDeviceAddress 用途 ——
    // 而那时 vkGetBufferDeviceAddress 本身不报错, 只是返回 0。构建会在这个
    // 空地址上读几何体, 结果是一棵空树: 所有射线都不命中, 画面全亮或全暗,
    // 没有任何一层会说"你的顶点地址是 0"。
    if (vertexAddress == 0 || indexAddress == 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] BLAS 的顶点/索引缓冲区取不到设备地址 (顶点:{} 索引:{})"
            " — 缓冲区创建时是否带了 ShaderDeviceAddress 用途?",
            vertexAddress, indexAddress);
        return ERHIResult::ErrorInvalidParameter;
    }

    FVulkanAccelStructData data;
    data.IsTopLevel      = false;
    data.VertexAddress   = static_cast<VkDeviceAddress>(vertexAddress) +
                           geom.VertexOffset;
    data.IndexAddress    = static_cast<VkDeviceAddress>(indexAddress) +
                           geom.IndexOffset;
    data.MaxVertex       = geom.VertexCount - 1;
    data.VertexStride    = geom.VertexStride;
    data.VertexFormat    = ToVkFormat(geom.VertexFormat);
    data.IndexType       = (geom.IndexType == EIndexType::UInt16)
                           ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    data.GeometryFlags   = geom.Opaque
                           ? VK_GEOMETRY_OPAQUE_BIT_KHR
                           : static_cast<VkGeometryFlagsKHR>(0);

    VkAccelerationStructureGeometryKHR vkGeom = {};
    vkGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    vkGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    vkGeom.flags        = data.GeometryFlags;

    VkAccelerationStructureGeometryTrianglesDataKHR& tri =
        vkGeom.geometry.triangles;
    tri.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = data.VertexFormat;
    tri.vertexData.deviceAddress = data.VertexAddress;
    tri.vertexStride = data.VertexStride;
    tri.maxVertex    = data.MaxVertex;
    tri.indexType    = data.IndexType;
    tri.indexData.deviceAddress = data.IndexAddress;
    tri.transformData.deviceAddress = 0;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type =
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = desc.PreferFastTrace
        ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
        : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &vkGeom;

    return CreateAccelStructCommon(
        data, buildInfo, geom.IndexCount / 3,
        (desc.DebugName != nullptr) ? desc.DebugName : "Blas",
        outHandle);
}

// ============================================================================
// TLAS
// ============================================================================

ERHIResult FVulkanDevice::CreateTopLevelAS(
    const FRHITlasDesc& desc, FRHIAccelStructHandle& outHandle)
{
    if (!m_RayTracingAvailable)
    {
        LIMX_LOG(LogRHI, Error, "[Vulkan] 设备不支持光追 — TLAS 创建失败");
        return ERHIResult::ErrorIncompatibleDriver;
    }

    if (desc.MaxInstanceCount == 0)
    {
        LIMX_LOG(LogRHI, Error, "[Vulkan] TLAS 的实例数上限必须非零");
        return ERHIResult::ErrorInvalidParameter;
    }

    FVulkanAccelStructData data;
    data.IsTopLevel       = true;
    data.MaxInstanceCount = desc.MaxInstanceCount;

    // ------------------------------------------------------------------
    // 实例缓冲区 —— 主机可写, 因为实例变换每帧都可能变
    // ------------------------------------------------------------------
    FRHIBufferDesc instanceDesc;
    instanceDesc.Size =
        static_cast<UInt64>(desc.MaxInstanceCount) *
        sizeof(VkAccelerationStructureInstanceKHR);
    instanceDesc.Usage = EBufferUsage::AccelStructBuild |
                         EBufferUsage::ShaderDeviceAddress |
                         EBufferUsage::TransferDst;
    instanceDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
    instanceDesc.DebugName = "TlasInstances";

    ERHIResult result = CreateBuffer(instanceDesc, data.InstanceBuffer);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    data.InstanceAddress = static_cast<VkDeviceAddress>(
        GetBufferDeviceAddress(data.InstanceBuffer));

    if (data.InstanceAddress == 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] TLAS 实例缓冲区取不到设备地址");
        DestroyBuffer(data.InstanceBuffer);
        return ERHIResult::ErrorUnknown;
    }

    VkAccelerationStructureGeometryKHR vkGeom = {};
    vkGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    vkGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    vkGeom.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;

    VkAccelerationStructureGeometryInstancesDataKHR& inst =
        vkGeom.geometry.instances;
    inst.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    inst.arrayOfPointers = VK_FALSE;
    inst.data.deviceAddress = data.InstanceAddress;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type  = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = desc.PreferFastTrace
        ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
        : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &vkGeom;

    // 按上限问尺寸 —— 之后每次构建的实际实例数只能少不能多。
    result = CreateAccelStructCommon(
        data, buildInfo, desc.MaxInstanceCount,
        (desc.DebugName != nullptr) ? desc.DebugName : "Tlas",
        outHandle);

    if (!IsRHISuccess(result))
    {
        DestroyBuffer(data.InstanceBuffer);
    }

    return result;
}

// ============================================================================
// 实例上传
// ============================================================================

ERHIResult FVulkanDevice::UpdateTlasInstances(
    FRHIAccelStructHandle handle,
    const FRHIAccelStructInstance* instances,
    UInt32 count)
{
    FVulkanAccelStructData* data = m_AccelStructs.Get(handle);

    if (data == nullptr || !data->IsTopLevel)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] UpdateTlasInstances 的句柄不是有效的 TLAS");
        return ERHIResult::ErrorInvalidParameter;
    }

    if (instances == nullptr && count > 0)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    // 超上限就失败 —— 不截断。截断的后果是多出来的物体在光追里凭空消失,
    // 而画面上不会有任何错误提示: 反射里少一个物体和"那里本来就没东西"
    // 长得一模一样。
    if (count > data->MaxInstanceCount)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] TLAS 实例数 {} 超过创建时的上限 {}",
            count, data->MaxInstanceCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    void* mapped = nullptr;
    const ERHIResult mapResult = MapBuffer(data->InstanceBuffer, &mapped);

    if (!IsRHISuccess(mapResult) || mapped == nullptr)
    {
        return ERHIResult::ErrorUnknown;
    }

    auto* dst = static_cast<VkAccelerationStructureInstanceKHR*>(mapped);

    for (UInt32 i = 0; i < count; ++i)
    {
        const FRHIAccelStructInstance& src = instances[i];

        // CustomIndex 只有 24 位。超出时 Vulkan 的位域会静默地把高位丢掉,
        // 于是着色器读到一个别的物体下标 —— 材质串位, 而且随物体数量增长
        // 才出现。这里直接报错。
        if (src.CustomIndex > kAccelStructMaxCustomIndex)
        {
            UnmapBuffer(data->InstanceBuffer);
            LIMX_LOG(LogRHI, Error,
                "[Vulkan] TLAS 实例 {} 的 CustomIndex {} 超过 24 位上限 {}",
                i, src.CustomIndex, kAccelStructMaxCustomIndex);
            return ERHIResult::ErrorInvalidParameter;
        }

        if (src.Mask > 0xFFu)
        {
            UnmapBuffer(data->InstanceBuffer);
            LIMX_LOG(LogRHI, Error,
                "[Vulkan] TLAS 实例 {} 的 Mask {} 超过 8 位上限",
                i, src.Mask);
            return ERHIResult::ErrorInvalidParameter;
        }

        const UInt64 blasAddress = GetAccelStructDeviceAddress(src.Blas);

        if (blasAddress == 0)
        {
            UnmapBuffer(data->InstanceBuffer);
            LIMX_LOG(LogRHI, Error,
                "[Vulkan] TLAS 实例 {} 引用的 BLAS 取不到设备地址", i);
            return ERHIResult::ErrorInvalidParameter;
        }

        VkAccelerationStructureInstanceKHR& out = dst[i];

        for (UInt32 row = 0; row < 3; ++row)
        {
            for (UInt32 col = 0; col < 4; ++col)
            {
                out.transform.matrix[row][col] = src.Transform[row * 4 + col];
            }
        }

        out.instanceCustomIndex = src.CustomIndex;
        out.mask                = src.Mask;
        out.instanceShaderBindingTableRecordOffset = 0;
        out.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        out.accelerationStructureReference = blasAddress;
    }

    UnmapBuffer(data->InstanceBuffer);

    return ERHIResult::Success;
}

// ============================================================================
// 销毁
// ============================================================================

void FVulkanDevice::DestroyAccelStruct(FRHIAccelStructHandle& handle)
{
    FVulkanAccelStructData* data = m_AccelStructs.Get(handle);

    if (data == nullptr)
    {
        handle = FRHIAccelStructHandle();
        return;
    }

    if (data->AccelStruct != VK_NULL_HANDLE &&
        m_RayTracingFunctions.Destroy != nullptr)
    {
        m_RayTracingFunctions.Destroy(m_Device, data->AccelStruct, nullptr);
        data->AccelStruct = VK_NULL_HANDLE;
    }

    // 三块缓冲区都是本加速结构私有的, 一起放掉。
    DestroyBuffer(data->ScratchBuffer);
    DestroyBuffer(data->StorageBuffer);

    if (data->IsTopLevel)
    {
        DestroyBuffer(data->InstanceBuffer);
    }

    m_AccelStructs.Free(handle);
}

} // namespace Limx
