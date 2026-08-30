// ============================================================
// 文件名称：FVulkanDevicePipeline.cpp
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：管线创建严格按 Vulkan 规范构建所有中间状态结构体，
//          确保每个固定功能阶段都有完整且正确的配置。
// 功能描述：FVulkanDevice 管线相关资源实现 — 渲染通道、帧缓冲、
//          描述符集布局、管线布局、图形管线、计算管线、
//          描述符集的创建与销毁。
// 技术特性：图形管线创建构建完整的 11 阶段 VkGraphicsPipelineCreateInfo；
//          动态状态通过 CollectVkDynamicStates 从位掩码展开；
//          渲染通道支持多子通道和子通道依赖。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ CreateRenderPass()             │ 创建 VkRenderPass         │
// │ DestroyRenderPass()            │ 销毁渲染通道              │
// │ CreateFramebuffer()            │ 创建 VkFramebuffer        │
// │ DestroyFramebuffer()           │ 销毁帧缓冲               │
// │ CreateDescSetLayout()          │ 创建描述符集布局           │
// │ DestroyDescSetLayout()         │ 销毁描述符集布局           │
// │ CreatePipelineLayout()         │ 创建管线布局              │
// │ DestroyPipelineLayout()        │ 销毁管线布局              │
// │ CreateGraphicsPipeline()       │ 创建图形管线              │
// │ DestroyGraphicsPipeline()      │ 销毁图形管线              │
// │ CreateComputePipeline()        │ 创建计算管线              │
// │ DestroyComputePipeline()       │ 销毁计算管线              │
// │ AllocateDescriptorSet()        │ 分配描述符集              │
// │ FreeDescriptorSet()            │ 释放描述符集              │
// │ GetVkRenderPass()              │ 句柄→VkRenderPass        │
// │ GetVkFramebuffer()             │ 句柄→VkFramebuffer       │
// │ GetVkGraphicsPipeline()        │ 句柄→VkPipeline          │
// │ GetVkComputePipeline()         │ 句柄→VkPipeline          │
// │ GetVkPipelineLayout()          │ 句柄→VkPipelineLayout    │
// │ GetVkDescriptorSet()           │ 句柄→VkDescriptorSet     │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "Vulkan/FVulkanDevice.h"
#include "Core/HAL/FPlatformTime.h"

namespace Limx
{

// ============================================================================
// 渲染通道
// ============================================================================

ERHIResult FVulkanDevice::CreateRenderPass(
    const FRHIRenderPassDesc& desc,
    FRHIRenderPassHandle& outHandle)
{
    // 转换附件描述
    constexpr UInt32 kMaxAttachments = 16;
    VkAttachmentDescription attachments[kMaxAttachments];
    UInt32 attachmentCount = desc.AttachmentCount;
    if (attachmentCount > kMaxAttachments)
    {
        attachmentCount = kMaxAttachments;
    }

    for (UInt32 i = 0; i < attachmentCount; ++i)
    {
        const FRHIAttachmentDesc& src = desc.Attachments[i];
        VkAttachmentDescription& dst  = attachments[i];
        MemZero(&dst, sizeof(VkAttachmentDescription));

        dst.format         = ToVkFormat(src.Format);
        dst.samples        = ToVkSampleCountFlagBits(src.Samples);
        dst.loadOp         = ToVkAttachmentLoadOp(src.LoadOp);
        dst.storeOp        = ToVkAttachmentStoreOp(src.StoreOp);
        dst.stencilLoadOp  = ToVkAttachmentLoadOp(src.StencilLoadOp);
        dst.stencilStoreOp = ToVkAttachmentStoreOp(src.StencilStoreOp);
        dst.initialLayout  = ToVkImageLayout(src.InitialLayout);
        dst.finalLayout    = ToVkImageLayout(src.FinalLayout);
    }

    // 转换子通道描述
    constexpr UInt32 kMaxSubpasses = 8;
    constexpr UInt32 kMaxRefs      = 32;
    VkSubpassDescription subpasses[kMaxSubpasses];
    VkAttachmentReference colorRefs[kMaxRefs];
    VkAttachmentReference depthRefs[kMaxSubpasses];
    VkAttachmentReference inputRefs[kMaxRefs];

    UInt32 subpassCount = desc.SubpassCount;
    if (subpassCount > kMaxSubpasses)
    {
        subpassCount = kMaxSubpasses;
    }

    UInt32 colorRefOffset = 0;
    UInt32 inputRefOffset = 0;

    for (UInt32 s = 0; s < subpassCount; ++s)
    {
        const FRHISubpassDesc& src = desc.Subpasses[s];
        VkSubpassDescription& dst  = subpasses[s];
        MemZero(&dst, sizeof(VkSubpassDescription));

        dst.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

        // 颜色附件引用
        dst.colorAttachmentCount = src.ColorAttachmentCount;
        if (src.ColorAttachmentCount > 0 &&
            colorRefOffset + src.ColorAttachmentCount <= kMaxRefs)
        {
            dst.pColorAttachments = &colorRefs[colorRefOffset];
            for (UInt32 c = 0; c < src.ColorAttachmentCount; ++c)
            {
                colorRefs[colorRefOffset + c].attachment =
                    src.ColorAttachments[c].AttachmentIndex;
                colorRefs[colorRefOffset + c].layout =
                    ToVkImageLayout(src.ColorAttachments[c].Layout);
            }
            colorRefOffset += src.ColorAttachmentCount;
        }

        // 输入附件引用
        dst.inputAttachmentCount = src.InputAttachmentCount;
        if (src.InputAttachmentCount > 0 &&
            inputRefOffset + src.InputAttachmentCount <= kMaxRefs)
        {
            dst.pInputAttachments = &inputRefs[inputRefOffset];
            for (UInt32 c = 0; c < src.InputAttachmentCount; ++c)
            {
                inputRefs[inputRefOffset + c].attachment =
                    src.InputAttachments[c].AttachmentIndex;
                inputRefs[inputRefOffset + c].layout =
                    ToVkImageLayout(src.InputAttachments[c].Layout);
            }
            inputRefOffset += src.InputAttachmentCount;
        }

        // 深度模板附件引用
        if (src.DepthStencilAttachment != nullptr)
        {
            depthRefs[s].attachment =
                src.DepthStencilAttachment->AttachmentIndex;
            depthRefs[s].layout =
                ToVkImageLayout(src.DepthStencilAttachment->Layout);
            dst.pDepthStencilAttachment = &depthRefs[s];
        }

        // 保留附件
        dst.preserveAttachmentCount = src.PreserveAttachmentCount;
        dst.pPreserveAttachments    = src.PreserveAttachments;
    }

    // 转换子通道依赖
    constexpr UInt32 kMaxDependencies = 16;
    VkSubpassDependency dependencies[kMaxDependencies];
    UInt32 depCount = desc.DependencyCount;
    if (depCount > kMaxDependencies)
    {
        depCount = kMaxDependencies;
    }

    for (UInt32 d = 0; d < depCount; ++d)
    {
        const FRHISubpassDependency& src = desc.Dependencies[d];
        VkSubpassDependency& dst         = dependencies[d];

        dst.srcSubpass      = (src.SrcSubpass == 0xFFFFFFFF)
            ? VK_SUBPASS_EXTERNAL : src.SrcSubpass;
        dst.dstSubpass      = src.DstSubpass;
        dst.srcStageMask    = ToVkPipelineStageFlags(src.SrcStageMask);
        dst.dstStageMask    = ToVkPipelineStageFlags(src.DstStageMask);
        dst.srcAccessMask   = ToVkAccessFlags(src.SrcAccessMask);
        dst.dstAccessMask   = ToVkAccessFlags(src.DstAccessMask);
        dst.dependencyFlags = 0;
    }

    VkRenderPassCreateInfo createInfo = {};
    createInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = attachmentCount;
    createInfo.pAttachments    = attachments;
    createInfo.subpassCount    = subpassCount;
    createInfo.pSubpasses      = subpasses;
    createInfo.dependencyCount = depCount;
    createInfo.pDependencies   = dependencies;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateRenderPass(m_Device, &createInfo,
                                             nullptr, &renderPass);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateRenderPass 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanRenderPassData data;
    data.RenderPass = renderPass;
    outHandle = m_RenderPasses.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyRenderPass(FRHIRenderPassHandle& handle)
{
    FVulkanRenderPassData* data = m_RenderPasses.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyRenderPass(m_Device, data->RenderPass, nullptr);
    m_RenderPasses.Free(handle);
}

// ============================================================================
// 帧缓冲
// ============================================================================

ERHIResult FVulkanDevice::CreateFramebuffer(
    const FRHIFramebufferDesc& desc,
    FRHIFramebufferHandle& outHandle)
{
    const FVulkanRenderPassData* rpData =
        m_RenderPasses.Get(desc.RenderPass);
    if (rpData == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    // 转换附件视图句柄到 VkImageView
    constexpr UInt32 kMaxAttachments = 16;
    VkImageView imageViews[kMaxAttachments];
    UInt32 attachmentCount = desc.AttachmentCount;
    if (attachmentCount > kMaxAttachments)
    {
        attachmentCount = kMaxAttachments;
    }

    for (UInt32 i = 0; i < attachmentCount; ++i)
    {
        VkImageView view = GetVkImageView(desc.Attachments[i]);
        if (view == VK_NULL_HANDLE)
        {
            return ERHIResult::ErrorInvalidHandle;
        }
        imageViews[i] = view;
    }

    VkFramebufferCreateInfo createInfo = {};
    createInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    createInfo.renderPass      = rpData->RenderPass;
    createInfo.attachmentCount = attachmentCount;
    createInfo.pAttachments    = imageViews;
    createInfo.width           = desc.Width;
    createInfo.height          = desc.Height;
    createInfo.layers          = desc.Layers;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateFramebuffer(m_Device, &createInfo,
                                              nullptr, &framebuffer);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateFramebuffer 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanFramebufferData data;
    data.Framebuffer = framebuffer;
    outHandle = m_Framebuffers.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyFramebuffer(FRHIFramebufferHandle& handle)
{
    FVulkanFramebufferData* data = m_Framebuffers.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyFramebuffer(m_Device, data->Framebuffer, nullptr);
    m_Framebuffers.Free(handle);
}

// ============================================================================
// 描述符集布局
// ============================================================================

ERHIResult FVulkanDevice::CreateDescSetLayout(
    const FRHIDescSetLayoutDesc& desc,
    FRHIDescSetLayoutHandle& outHandle)
{
    constexpr UInt32 kMaxBindings = 32;
    VkDescriptorSetLayoutBinding bindings[kMaxBindings];
    UInt32 bindingCount = desc.BindingCount;
    if (bindingCount > kMaxBindings)
    {
        bindingCount = kMaxBindings;
    }

    for (UInt32 i = 0; i < bindingCount; ++i)
    {
        const FRHIDescriptorBinding& src = desc.Bindings[i];
        VkDescriptorSetLayoutBinding& dst = bindings[i];

        dst.binding            = src.Binding;
        dst.descriptorType     = ToVkDescriptorType(src.Type);
        dst.descriptorCount    = src.Count;
        dst.stageFlags         = ToVkShaderStageFlags(src.StageFlags);
        dst.pImmutableSamplers = nullptr;
    }

    VkDescriptorSetLayoutCreateInfo createInfo = {};
    createInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.bindingCount = bindingCount;
    createInfo.pBindings    = bindings;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateDescriptorSetLayout(m_Device, &createInfo,
                                                      nullptr, &layout);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateDescriptorSetLayout 失败: {}",
            (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanDescSetLayoutData data;
    data.Layout = layout;
    outHandle = m_DescSetLayouts.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyDescSetLayout(
    FRHIDescSetLayoutHandle& handle)
{
    FVulkanDescSetLayoutData* data = m_DescSetLayouts.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyDescriptorSetLayout(m_Device, data->Layout, nullptr);
    m_DescSetLayouts.Free(handle);
}

// ============================================================================
// 管线布局
// ============================================================================

ERHIResult FVulkanDevice::CreatePipelineLayout(
    const FRHIPipelineLayoutDesc& desc,
    FRHIPipelineLayoutHandle& outHandle)
{
    constexpr UInt32 kMaxSetLayouts       = 8;
    constexpr UInt32 kMaxPushConstRanges  = 4;

    VkDescriptorSetLayout setLayouts[kMaxSetLayouts];
    UInt32 setLayoutCount = desc.SetLayoutCount;
    if (setLayoutCount > kMaxSetLayouts)
    {
        setLayoutCount = kMaxSetLayouts;
    }

    for (UInt32 i = 0; i < setLayoutCount; ++i)
    {
        const FVulkanDescSetLayoutData* layoutData =
            m_DescSetLayouts.Get(desc.SetLayouts[i]);
        if (layoutData == nullptr)
        {
            return ERHIResult::ErrorInvalidHandle;
        }
        setLayouts[i] = layoutData->Layout;
    }

    VkPushConstantRange pushConstRanges[kMaxPushConstRanges];
    UInt32 pushConstCount = desc.PushConstantRangeCount;
    if (pushConstCount > kMaxPushConstRanges)
    {
        pushConstCount = kMaxPushConstRanges;
    }

    for (UInt32 i = 0; i < pushConstCount; ++i)
    {
        const FRHIPushConstantRange& src = desc.PushConstantRanges[i];
        pushConstRanges[i].stageFlags = ToVkShaderStageFlags(
            src.StageFlags);
        pushConstRanges[i].offset     = src.Offset;
        pushConstRanges[i].size       = src.Size;
    }

    VkPipelineLayoutCreateInfo createInfo = {};
    createInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.setLayoutCount         = setLayoutCount;
    createInfo.pSetLayouts            = setLayouts;
    createInfo.pushConstantRangeCount = pushConstCount;
    createInfo.pPushConstantRanges    = pushConstRanges;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult vkResult = vkCreatePipelineLayout(m_Device, &createInfo,
                                                 nullptr, &layout);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreatePipelineLayout 失败: {}",
            (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanPipelineLayoutData data;
    data.Layout = layout;
    outHandle = m_PipelineLayouts.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyPipelineLayout(
    FRHIPipelineLayoutHandle& handle)
{
    FVulkanPipelineLayoutData* data = m_PipelineLayouts.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyPipelineLayout(m_Device, data->Layout, nullptr);
    m_PipelineLayouts.Free(handle);
}

// ============================================================================
// 图形管线
// ============================================================================

ERHIResult FVulkanDevice::CreateGraphicsPipeline(
    const FRHIGraphicsPipelineDesc& desc,
    FRHIGraphicsPipelineHandle& outHandle)
{
    // ---- 着色器阶段 ----
    VkPipelineShaderStageCreateInfo shaderStages[kMaxShaderStages];
    for (UInt32 i = 0; i < desc.ShaderStageCount; ++i)
    {
        const FRHIShaderStageDesc& src = desc.ShaderStages[i];
        VkPipelineShaderStageCreateInfo& dst = shaderStages[i];
        MemZero(&dst, sizeof(VkPipelineShaderStageCreateInfo));

        dst.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        dst.stage  = static_cast<VkShaderStageFlagBits>(
            ToVkShaderStageFlags(src.Stage));
        dst.pName  = src.EntryPoint;

        const FVulkanShaderData* shaderData = m_Shaders.Get(src.Shader);
        if (shaderData == nullptr)
        {
            return ERHIResult::ErrorInvalidHandle;
        }
        dst.module = shaderData->Module;
    }

    // ---- 顶点输入 ----
    constexpr UInt32 kMaxVertexBindings   = 16;
    constexpr UInt32 kMaxVertexAttributes = 32;

    VkVertexInputBindingDescription vertexBindings[kMaxVertexBindings];
    UInt32 vertBindCount = desc.VertexInput.BindingCount;
    if (vertBindCount > kMaxVertexBindings)
    {
        vertBindCount = kMaxVertexBindings;
    }

    for (UInt32 i = 0; i < vertBindCount; ++i)
    {
        const FRHIVertexInputBinding& src = desc.VertexInput.Bindings[i];
        vertexBindings[i].binding   = src.Binding;
        vertexBindings[i].stride    = src.Stride;
        vertexBindings[i].inputRate = ToVkVertexInputRate(src.InputRate);
    }

    VkVertexInputAttributeDescription vertexAttrs[kMaxVertexAttributes];
    UInt32 vertAttrCount = desc.VertexInput.AttributeCount;
    if (vertAttrCount > kMaxVertexAttributes)
    {
        vertAttrCount = kMaxVertexAttributes;
    }

    for (UInt32 i = 0; i < vertAttrCount; ++i)
    {
        const FRHIVertexInputAttribute& src =
            desc.VertexInput.Attributes[i];
        vertexAttrs[i].location = src.Location;
        vertexAttrs[i].binding  = src.Binding;
        vertexAttrs[i].format   = ToVkFormat(src.Format);
        vertexAttrs[i].offset   = src.Offset;
    }

    VkPipelineVertexInputStateCreateInfo vertexInputState = {};
    vertexInputState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount   = vertBindCount;
    vertexInputState.pVertexBindingDescriptions      = vertexBindings;
    vertexInputState.vertexAttributeDescriptionCount = vertAttrCount;
    vertexInputState.pVertexAttributeDescriptions    = vertexAttrs;

    // ---- 输入装配 ----
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = ToVkPrimitiveTopology(
        desc.InputAssembly.Topology);
    inputAssembly.primitiveRestartEnable =
        desc.InputAssembly.IsPrimitiveRestartEnabled ? VK_TRUE : VK_FALSE;

    // ---- 视口/裁剪 (动态状态，仅指定数量) ----
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    // ---- 光栅化 ----
    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable =
        desc.Rasterization.IsDepthClampEnabled ? VK_TRUE : VK_FALSE;
    rasterization.rasterizerDiscardEnable =
        desc.Rasterization.IsRasterizerDiscardEnabled
            ? VK_TRUE : VK_FALSE;
    rasterization.polygonMode = ToVkPolygonMode(
        desc.Rasterization.PolygonMode);
    rasterization.cullMode    = ToVkCullModeFlags(
        desc.Rasterization.CullMode);
    rasterization.frontFace   = ToVkFrontFace(
        desc.Rasterization.FrontFace);
    rasterization.depthBiasEnable =
        desc.Rasterization.IsDepthBiasEnabled ? VK_TRUE : VK_FALSE;
    rasterization.depthBiasConstantFactor =
        desc.Rasterization.DepthBiasConstantFactor;
    rasterization.depthBiasClamp          =
        desc.Rasterization.DepthBiasClamp;
    rasterization.depthBiasSlopeFactor    =
        desc.Rasterization.DepthBiasSlopeFactor;
    rasterization.lineWidth = desc.Rasterization.LineWidth;

    // ---- 多重采样 ----
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = ToVkSampleCountFlagBits(
        desc.Multisample.RasterizationSamples);
    multisample.sampleShadingEnable =
        desc.Multisample.IsSampleShadingEnabled ? VK_TRUE : VK_FALSE;
    multisample.minSampleShading =
        desc.Multisample.MinSampleShading;
    multisample.pSampleMask = desc.Multisample.SampleMask;
    multisample.alphaToCoverageEnable =
        desc.Multisample.IsAlphaToCoverageEnabled ? VK_TRUE : VK_FALSE;
    multisample.alphaToOneEnable =
        desc.Multisample.IsAlphaToOneEnabled ? VK_TRUE : VK_FALSE;

    // ---- 深度模板 ----
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable =
        desc.DepthStencil.IsDepthTestEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable =
        desc.DepthStencil.IsDepthWriteEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = ToVkCompareOp(
        desc.DepthStencil.DepthCompareOp);
    depthStencil.depthBoundsTestEnable =
        desc.DepthStencil.IsDepthBoundsTestEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.minDepthBounds = desc.DepthStencil.MinDepthBounds;
    depthStencil.maxDepthBounds = desc.DepthStencil.MaxDepthBounds;
    depthStencil.stencilTestEnable =
        desc.DepthStencil.IsStencilTestEnabled ? VK_TRUE : VK_FALSE;

    // 正面模板
    depthStencil.front.failOp      = ToVkStencilOp(
        desc.DepthStencil.Front.FailOp);
    depthStencil.front.passOp      = ToVkStencilOp(
        desc.DepthStencil.Front.PassOp);
    depthStencil.front.depthFailOp = ToVkStencilOp(
        desc.DepthStencil.Front.DepthFailOp);
    depthStencil.front.compareOp   = ToVkCompareOp(
        desc.DepthStencil.Front.CompareOp);
    depthStencil.front.compareMask = desc.DepthStencil.Front.CompareMask;
    depthStencil.front.writeMask   = desc.DepthStencil.Front.WriteMask;
    depthStencil.front.reference   = desc.DepthStencil.Front.Reference;

    // 背面模板
    depthStencil.back.failOp      = ToVkStencilOp(
        desc.DepthStencil.Back.FailOp);
    depthStencil.back.passOp      = ToVkStencilOp(
        desc.DepthStencil.Back.PassOp);
    depthStencil.back.depthFailOp = ToVkStencilOp(
        desc.DepthStencil.Back.DepthFailOp);
    depthStencil.back.compareOp   = ToVkCompareOp(
        desc.DepthStencil.Back.CompareOp);
    depthStencil.back.compareMask = desc.DepthStencil.Back.CompareMask;
    depthStencil.back.writeMask   = desc.DepthStencil.Back.WriteMask;
    depthStencil.back.reference   = desc.DepthStencil.Back.Reference;

    // ---- 颜色混合 ----
    constexpr UInt32 kMaxColorAttachments = 8;
    VkPipelineColorBlendAttachmentState blendAttachments[
        kMaxColorAttachments];
    UInt32 blendAttachCount = desc.ColorBlend.AttachmentCount;
    if (blendAttachCount > kMaxColorAttachments)
    {
        blendAttachCount = kMaxColorAttachments;
    }

    for (UInt32 i = 0; i < blendAttachCount; ++i)
    {
        const FRHIColorBlendAttachmentDesc& src =
            desc.ColorBlend.Attachments[i];
        VkPipelineColorBlendAttachmentState& dst = blendAttachments[i];

        dst.blendEnable         = src.IsBlendEnabled ? VK_TRUE : VK_FALSE;
        dst.srcColorBlendFactor = ToVkBlendFactor(
            src.SrcColorBlendFactor);
        dst.dstColorBlendFactor = ToVkBlendFactor(
            src.DstColorBlendFactor);
        dst.colorBlendOp        = ToVkBlendOp(src.ColorBlendOp);
        dst.srcAlphaBlendFactor = ToVkBlendFactor(
            src.SrcAlphaBlendFactor);
        dst.dstAlphaBlendFactor = ToVkBlendFactor(
            src.DstAlphaBlendFactor);
        dst.alphaBlendOp        = ToVkBlendOp(src.AlphaBlendOp);
        dst.colorWriteMask      = ToVkColorComponentFlags(
            src.ColorWriteMask);
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable   = desc.ColorBlend.IsLogicOpEnabled
        ? VK_TRUE : VK_FALSE;
    colorBlend.logicOp         = static_cast<VkLogicOp>(
        desc.ColorBlend.LogicOp);
    colorBlend.attachmentCount = blendAttachCount;
    colorBlend.pAttachments    = blendAttachments;
    MemCopy(colorBlend.blendConstants,
            desc.ColorBlend.BlendConstants, sizeof(Float32) * 4);

    // ---- 动态状态 ----
    constexpr UInt32 kMaxDynStates = 32;
    VkDynamicState dynamicStates[kMaxDynStates];
    UInt32 dynStateCount = CollectVkDynamicStates(
        desc.DynamicState.EnabledStates, dynamicStates, kMaxDynStates);

    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = dynStateCount;
    dynamicState.pDynamicStates    = dynamicStates;

    // ---- 管线布局 ----
    VkPipelineLayout pipelineLayout = GetVkPipelineLayout(
        desc.PipelineLayout);
    if (pipelineLayout == VK_NULL_HANDLE)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    // ---- 渲染通道 ----
    VkRenderPass renderPass = GetVkRenderPass(desc.RenderPass);
    if (renderPass == VK_NULL_HANDLE)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    // ---- 创建图形管线 ----
    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = desc.ShaderStageCount;
    pipelineInfo.pStages             = shaderStages;
    pipelineInfo.pVertexInputState   = &vertexInputState;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pTessellationState  = nullptr;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState   = &multisample;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = pipelineLayout;
    pipelineInfo.renderPass          = renderPass;
    pipelineInfo.subpass             = desc.SubpassIndex;

    // 累计管线创建耗时 —— 用来量管线缓存到底值多少。
    //
    // 只能累计而不能取单次: 单条管线的创建在有无缓存两种情况下都只有
    // 几毫秒, 而启动时要建十几条, 差别在总量上才看得出来。
    const Float64 createBegin = FPlatformTime::Seconds();

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateGraphicsPipelines(
        m_Device, m_PipelineCache, 1, &pipelineInfo, nullptr, &pipeline);

    m_PipelineCreateMs += (FPlatformTime::Seconds() - createBegin) * 1000.0;
    ++m_PipelineCreateCount;
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateGraphicsPipelines 失败: {}",
            (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    FVulkanGraphicsPipelineData data;
    data.Pipeline = pipeline;
    outHandle = m_GraphicsPipelines.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyGraphicsPipeline(
    FRHIGraphicsPipelineHandle& handle)
{
    FVulkanGraphicsPipelineData* data = m_GraphicsPipelines.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyPipeline(m_Device, data->Pipeline, nullptr);
    m_GraphicsPipelines.Free(handle);
}

// ============================================================================
// 计算管线
// ============================================================================

ERHIResult FVulkanDevice::CreateComputePipeline(
    const FRHIComputePipelineDesc& desc,
    FRHIComputePipelineHandle& outHandle)
{
    const FVulkanShaderData* shaderData =
        m_Shaders.Get(desc.ComputeShader.Shader);
    if (shaderData == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkPipelineLayout pipelineLayout = GetVkPipelineLayout(
        desc.PipelineLayout);
    if (pipelineLayout == VK_NULL_HANDLE)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkPipelineShaderStageCreateInfo shaderStage = {};
    shaderStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = shaderData->Module;
    shaderStage.pName  = desc.ComputeShader.EntryPoint;

    VkComputePipelineCreateInfo createInfo = {};
    createInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    createInfo.stage  = shaderStage;
    createInfo.layout = pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateComputePipelines(
        m_Device, m_PipelineCache, 1, &createInfo, nullptr, &pipeline);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateComputePipelines 失败: {}",
            (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    FVulkanComputePipelineData data;
    data.Pipeline = pipeline;
    outHandle = m_ComputePipelines.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyComputePipeline(
    FRHIComputePipelineHandle& handle)
{
    FVulkanComputePipelineData* data = m_ComputePipelines.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyPipeline(m_Device, data->Pipeline, nullptr);
    m_ComputePipelines.Free(handle);
}

// ============================================================================
// 描述符集
// ============================================================================

ERHIResult FVulkanDevice::AllocateDescriptorSet(
    FRHIDescSetLayoutHandle layout,
    FRHIDescriptorSetHandle& outHandle)
{
    const FVulkanDescSetLayoutData* layoutData =
        m_DescSetLayouts.Get(layout);
    if (layoutData == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layoutData->Layout;

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkResult vkResult = vkAllocateDescriptorSets(m_Device, &allocInfo,
                                                   &descriptorSet);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkAllocateDescriptorSets 失败: {}",
            (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanDescriptorSetData data;
    data.Set = descriptorSet;
    outHandle = m_DescriptorSets.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::FreeDescriptorSet(
    FRHIDescriptorSetHandle& handle)
{
    FVulkanDescriptorSetData* data = m_DescriptorSets.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkFreeDescriptorSets(m_Device, m_DescriptorPool, 1, &data->Set);
    m_DescriptorSets.Free(handle);
}

// ============================================================================
// 描述符集写入
// ============================================================================

void FVulkanDevice::UpdateDescriptorSets(const FRHIDescriptorWrite* writes,
                                          UInt32 writeCount)
{
    if (writes == nullptr || writeCount == 0)
    {
        return;
    }

    constexpr UInt32 kMaxWrites = 32;
    UInt32 count = writeCount;
    if (count > kMaxWrites)
    {
        count = kMaxWrites;
    }

    VkWriteDescriptorSet   vkWrites[kMaxWrites]     = {};
    VkDescriptorBufferInfo bufferInfos[kMaxWrites]   = {};
    VkDescriptorImageInfo  imageInfos[kMaxWrites]    = {};

    for (UInt32 i = 0; i < count; ++i)
    {
        const FRHIDescriptorWrite& src = writes[i];
        VkWriteDescriptorSet& dst = vkWrites[i];

        dst.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        dst.dstSet          = GetVkDescriptorSet(src.DescriptorSet);
        dst.dstBinding      = src.Binding;
        dst.dstArrayElement = src.ArrayElement;
        dst.descriptorCount = 1;
        dst.descriptorType  = ToVkDescriptorType(src.Type);

        switch (src.Type)
        {
        case EDescriptorType::UniformBuffer:
        case EDescriptorType::StorageBuffer:
        case EDescriptorType::UniformBufferDynamic:
        case EDescriptorType::StorageBufferDynamic:
        {
            VkDescriptorBufferInfo& bufInfo = bufferInfos[i];
            bufInfo.buffer = GetVkBuffer(src.Buffer);
            bufInfo.offset = src.BufferOffset;
            bufInfo.range  = (src.BufferRange == 0)
                ? VK_WHOLE_SIZE : src.BufferRange;
            dst.pBufferInfo = &bufferInfos[i];
            break;
        }

        case EDescriptorType::CombinedImageSampler:
        case EDescriptorType::SampledImage:
        case EDescriptorType::StorageImage:
        case EDescriptorType::InputAttachment:
        {
            VkDescriptorImageInfo& imgInfo = imageInfos[i];
            imgInfo.imageView   = GetVkImageView(src.ImageView);
            imgInfo.sampler     = GetVkSampler(src.Sampler);
            imgInfo.imageLayout = ToVkImageLayout(src.ImageLayout);
            dst.pImageInfo = &imageInfos[i];
            break;
        }

        case EDescriptorType::Sampler:
        {
            VkDescriptorImageInfo& imgInfo = imageInfos[i];
            imgInfo.sampler     = GetVkSampler(src.Sampler);
            imgInfo.imageView   = VK_NULL_HANDLE;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            dst.pImageInfo = &imageInfos[i];
            break;
        }

        default:
            break;
        }
    }

    vkUpdateDescriptorSets(m_Device, count, vkWrites, 0, nullptr);
}

// ============================================================================
// 句柄访问辅助
// ============================================================================

VkRenderPass FVulkanDevice::GetVkRenderPass(
    FRHIRenderPassHandle handle) const
{
    const FVulkanRenderPassData* data = m_RenderPasses.Get(handle);
    return (data != nullptr) ? data->RenderPass : VK_NULL_HANDLE;
}

VkFramebuffer FVulkanDevice::GetVkFramebuffer(
    FRHIFramebufferHandle handle) const
{
    const FVulkanFramebufferData* data = m_Framebuffers.Get(handle);
    return (data != nullptr) ? data->Framebuffer : VK_NULL_HANDLE;
}

VkPipeline FVulkanDevice::GetVkGraphicsPipeline(
    FRHIGraphicsPipelineHandle handle) const
{
    const FVulkanGraphicsPipelineData* data =
        m_GraphicsPipelines.Get(handle);
    return (data != nullptr) ? data->Pipeline : VK_NULL_HANDLE;
}

VkPipeline FVulkanDevice::GetVkComputePipeline(
    FRHIComputePipelineHandle handle) const
{
    const FVulkanComputePipelineData* data =
        m_ComputePipelines.Get(handle);
    return (data != nullptr) ? data->Pipeline : VK_NULL_HANDLE;
}

VkPipelineLayout FVulkanDevice::GetVkPipelineLayout(
    FRHIPipelineLayoutHandle handle) const
{
    const FVulkanPipelineLayoutData* data =
        m_PipelineLayouts.Get(handle);
    return (data != nullptr) ? data->Layout : VK_NULL_HANDLE;
}

VkDescriptorSet FVulkanDevice::GetVkDescriptorSet(
    FRHIDescriptorSetHandle handle) const
{
    const FVulkanDescriptorSetData* data = m_DescriptorSets.Get(handle);
    return (data != nullptr) ? data->Set : VK_NULL_HANDLE;
}

} // namespace Limx
