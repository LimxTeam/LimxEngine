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
    // ------------------------------------------------------------------
    // 数量一律不设上限 — 内联容量 + 分配器回退
    //
    // 附件/子通道/依赖的条数由调用方的通道结构决定, 不是本函数能约束的。
    // 静默截断的后果在这里尤其难查: 少一个附件描述, 引用它的子通道就指向
    // 了不存在的下标; 少一条子通道依赖, 通道之间的同步就没了。两者都不会
    // 让 vkCreateRenderPass 失败得有指向性。
    // ------------------------------------------------------------------

    // 转换附件描述
    constexpr SizeType kInlineAttachments = 16;
    TSmallVector<VkAttachmentDescription, kInlineAttachments> attachments;

    UInt32 attachmentCount = desc.AttachmentCount;
    if (attachmentCount > 0 && desc.Attachments == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateRenderPass: 附件数组为空但计数为 {}",
            attachmentCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    attachments.Reserve(static_cast<SizeType>(attachmentCount));

    for (UInt32 i = 0; i < attachmentCount; ++i)
    {
        const FRHIAttachmentDesc& src = desc.Attachments[i];

        VkAttachmentDescription dst;
        MemZero(&dst, sizeof(VkAttachmentDescription));

        dst.format         = ToVkFormat(src.Format);
        dst.samples        = ToVkSampleCountFlagBits(src.Samples);
        dst.loadOp         = ToVkAttachmentLoadOp(src.LoadOp);
        dst.storeOp        = ToVkAttachmentStoreOp(src.StoreOp);
        dst.stencilLoadOp  = ToVkAttachmentLoadOp(src.StencilLoadOp);
        dst.stencilStoreOp = ToVkAttachmentStoreOp(src.StencilStoreOp);
        dst.initialLayout  = ToVkImageLayout(src.InitialLayout);
        dst.finalLayout    = ToVkImageLayout(src.FinalLayout);

        attachments.Add(dst);
    }

    // ------------------------------------------------------------------
    // 转换子通道描述
    //
    // 附件引用数组要一次性预留够: VkSubpassDescription 里存的是**指针**,
    // 中途扩容会让先前几个子通道的 pColorAttachments 全部悬空。因此先扫
    // 一遍求总数, Reserve 之后再填 —— Reserve 保证后续 Add 不再重新分配。
    //
    // 原实现在这里还藏着一个比截断更糟的形态: 引用放不下时跳过填充, 但
    // colorAttachmentCount 已经按调用方给的数量写进去了, pColorAttachments
    // 却留在 nullptr。那是"计数非零 + 空指针"交给驱动, 后果是读野指针,
    // 而不是少几个附件。
    // ------------------------------------------------------------------

    constexpr SizeType kInlineSubpasses = 8;
    constexpr SizeType kInlineRefs      = 32;

    TSmallVector<VkSubpassDescription, kInlineSubpasses>  subpasses;
    TSmallVector<VkAttachmentReference, kInlineRefs>      colorRefs;
    TSmallVector<VkAttachmentReference, kInlineSubpasses> depthRefs;
    TSmallVector<VkAttachmentReference, kInlineRefs>      inputRefs;

    UInt32 subpassCount = desc.SubpassCount;
    if (subpassCount > 0 && desc.Subpasses == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateRenderPass: 子通道数组为空但计数为 {}",
            subpassCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    SizeType totalColorRefs = 0;
    SizeType totalInputRefs = 0;

    for (UInt32 s = 0; s < subpassCount; ++s)
    {
        totalColorRefs += desc.Subpasses[s].ColorAttachmentCount;
        totalInputRefs += desc.Subpasses[s].InputAttachmentCount;
    }

    subpasses.Reserve(static_cast<SizeType>(subpassCount));
    depthRefs.Reserve(static_cast<SizeType>(subpassCount));
    colorRefs.Reserve(totalColorRefs);
    inputRefs.Reserve(totalInputRefs);

    // 深度引用先全部占位 —— 与子通道一一对应, 下标即子通道号
    for (UInt32 s = 0; s < subpassCount; ++s)
    {
        depthRefs.Add(VkAttachmentReference{});
    }

    for (UInt32 s = 0; s < subpassCount; ++s)
    {
        const FRHISubpassDesc& src = desc.Subpasses[s];

        VkSubpassDescription dst;
        MemZero(&dst, sizeof(VkSubpassDescription));

        dst.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

        // 颜色附件引用
        dst.colorAttachmentCount = src.ColorAttachmentCount;
        if (src.ColorAttachmentCount > 0)
        {
            if (src.ColorAttachments == nullptr)
            {
                LIMX_LOG(LogRHI, Error,
                    "[Vulkan] CreateRenderPass: 子通道 {} 的颜色附件数组为空"
                    "但计数为 {}", s, src.ColorAttachmentCount);
                return ERHIResult::ErrorInvalidParameter;
            }

            dst.pColorAttachments = colorRefs.GetData() + colorRefs.GetSize();

            for (UInt32 c = 0; c < src.ColorAttachmentCount; ++c)
            {
                VkAttachmentReference ref;
                ref.attachment = src.ColorAttachments[c].AttachmentIndex;
                ref.layout =
                    ToVkImageLayout(src.ColorAttachments[c].Layout);
                colorRefs.Add(ref);
            }
        }

        // 输入附件引用
        dst.inputAttachmentCount = src.InputAttachmentCount;
        if (src.InputAttachmentCount > 0)
        {
            if (src.InputAttachments == nullptr)
            {
                LIMX_LOG(LogRHI, Error,
                    "[Vulkan] CreateRenderPass: 子通道 {} 的输入附件数组为空"
                    "但计数为 {}", s, src.InputAttachmentCount);
                return ERHIResult::ErrorInvalidParameter;
            }

            dst.pInputAttachments = inputRefs.GetData() + inputRefs.GetSize();

            for (UInt32 c = 0; c < src.InputAttachmentCount; ++c)
            {
                VkAttachmentReference ref;
                ref.attachment = src.InputAttachments[c].AttachmentIndex;
                ref.layout =
                    ToVkImageLayout(src.InputAttachments[c].Layout);
                inputRefs.Add(ref);
            }
        }

        // 深度模板附件引用
        if (src.DepthStencilAttachment != nullptr)
        {
            const UInt32 depthIndex =
                src.DepthStencilAttachment->AttachmentIndex;

            // 深度必须是最后一个附件。
            //
            // 这不是风格偏好, 是 BeginRenderPass 填清除值的方式决定的:
            // 它先按顺序填全部颜色清除值, 然后**无条件把深度清除值追加到
            // 末尾** (见 FVulkanCommandBuffer::BeginRenderPass)。附件顺序
            // 一旦不是 [颜色..., 深度], 清除值就整体错位。
            //
            // 而清除值的**数量**仍然是对的 —— 验证层不会报错。表现是深度
            // 被当颜色清、某张颜色图被当深度清, 画面几乎全黑, 而人会先去
            // 怀疑深度测试的比较函数。
            //
            // 在这里拒绝, 比在每个 Pass 里"记得放最后"可靠。
            if (depthIndex + 1 != attachmentCount)
            {
                LIMX_LOG(LogRHI, Error,
                    "[Vulkan] 渲染通道 '{}' 的深度附件在 {} 号, 但共有 {} 个"
                    "附件 —— 深度必须是最后一个, 否则清除值会整体错位",
                    (desc.DebugName != nullptr) ? desc.DebugName : "?",
                    depthIndex, attachmentCount);

                return ERHIResult::ErrorInvalidParameter;
            }

            depthRefs[s].attachment = depthIndex;
            depthRefs[s].layout =
                ToVkImageLayout(src.DepthStencilAttachment->Layout);
            dst.pDepthStencilAttachment = &depthRefs[s];
        }

        // 保留附件
        dst.preserveAttachmentCount = src.PreserveAttachmentCount;
        dst.pPreserveAttachments    = src.PreserveAttachments;

        subpasses.Add(dst);
    }

    // 转换子通道依赖 — 少一条依赖就是少一处子通道间同步, 不能截断
    constexpr SizeType kInlineDependencies = 16;
    TSmallVector<VkSubpassDependency, kInlineDependencies> dependencies;

    UInt32 depCount = desc.DependencyCount;
    if (depCount > 0 && desc.Dependencies == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateRenderPass: 依赖数组为空但计数为 {}", depCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    dependencies.Reserve(static_cast<SizeType>(depCount));

    for (UInt32 d = 0; d < depCount; ++d)
    {
        const FRHISubpassDependency& src = desc.Dependencies[d];

        VkSubpassDependency dst;
        dst.srcSubpass      = (src.SrcSubpass == 0xFFFFFFFF)
            ? VK_SUBPASS_EXTERNAL : src.SrcSubpass;
        dst.dstSubpass      = src.DstSubpass;
        dst.srcStageMask    = ToVkPipelineStageFlags(src.SrcStageMask);
        dst.dstStageMask    = ToVkPipelineStageFlags(src.DstStageMask);
        dst.srcAccessMask   = ToVkAccessFlags(src.SrcAccessMask);
        dst.dstAccessMask   = ToVkAccessFlags(src.DstAccessMask);
        dst.dependencyFlags = 0;

        dependencies.Add(dst);
    }

    VkRenderPassCreateInfo createInfo = {};
    createInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = attachmentCount;
    createInfo.pAttachments    = attachments.GetData();
    createInfo.subpassCount    = subpassCount;
    createInfo.pSubpasses      = subpasses.GetData();
    createInfo.dependencyCount = depCount;
    createInfo.pDependencies   = dependencies.GetData();

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

    // 转换附件视图句柄到 VkImageView — 数量必须与渲染通道一致, 不能截断
    constexpr SizeType kInlineAttachments = 16;
    TSmallVector<VkImageView, kInlineAttachments> imageViews;

    const UInt32 attachmentCount = desc.AttachmentCount;
    if (attachmentCount > 0 && desc.Attachments == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateFramebuffer: 附件数组为空但计数为 {}",
            attachmentCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    imageViews.Reserve(static_cast<SizeType>(attachmentCount));

    for (UInt32 i = 0; i < attachmentCount; ++i)
    {
        VkImageView view = GetVkImageView(desc.Attachments[i]);
        if (view == VK_NULL_HANDLE)
        {
            return ERHIResult::ErrorInvalidHandle;
        }
        imageViews.Add(view);
    }

    VkFramebufferCreateInfo createInfo = {};
    createInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    createInfo.renderPass      = rpData->RenderPass;
    createInfo.attachmentCount = attachmentCount;
    createInfo.pAttachments    = imageViews.GetData();
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
    // 绑定条数由着色器接口决定, 不能截断 —— 少一个绑定, 着色器里对应的
    // 资源就永远读到未定义内容, 而管线创建与描述符写入都不会报错。
    constexpr SizeType kInlineBindings = 32;
    TSmallVector<VkDescriptorSetLayoutBinding, kInlineBindings> bindings;
    TSmallVector<VkDescriptorBindingFlags, kInlineBindings>     bindingFlags;

    const UInt32 bindingCount = desc.BindingCount;
    if (bindingCount > 0 && desc.Bindings == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateDescSetLayout: 绑定数组为空但计数为 {}",
            bindingCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    bindings.Reserve(static_cast<SizeType>(bindingCount));
    bindingFlags.Reserve(static_cast<SizeType>(bindingCount));

    bool anyFlags = false;

    for (UInt32 i = 0; i < bindingCount; ++i)
    {
        const FRHIDescriptorBinding& src = desc.Bindings[i];

        VkDescriptorSetLayoutBinding dst;
        dst.binding            = src.Binding;
        dst.descriptorType     = ToVkDescriptorType(src.Type);
        dst.descriptorCount    = src.Count;
        dst.stageFlags         = ToVkShaderStageFlags(src.StageFlags);
        dst.pImmutableSamplers = nullptr;

        bindings.Add(dst);

        VkDescriptorBindingFlags flags = 0;

        if (src.PartiallyBound)
        {
            flags |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }

        if (src.UpdateAfterBind)
        {
            flags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        }

        bindingFlags.Add(flags);

        anyFlags = anyFlags || (flags != 0);
    }

    VkDescriptorSetLayoutCreateInfo createInfo = {};
    createInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.bindingCount = bindingCount;
    createInfo.pBindings    = bindings.GetData();

    // 只在真的用到标志时才挂扩展结构。
    //
    // 无条件挂也能工作 (全零标志等价于不挂), 但那会让每一个普通描述符集
    // 布局都走 descriptorIndexing 的路径, 在不支持该扩展的设备上直接失败。
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {};

    if (anyFlags)
    {
        flagsInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount  = bindingCount;
        flagsInfo.pBindingFlags = bindingFlags.GetData();

        createInfo.pNext = &flagsInfo;

        // UPDATE_AFTER_BIND 要求集本身也声明对应的创建标志
        for (UInt32 i = 0; i < bindingCount; ++i)
        {
            if ((bindingFlags[i]
                 & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0)
            {
                createInfo.flags |=
                    VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
                break;
            }
        }
    }

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
    // 集布局与推送常量范围都不能截断: 少一个集布局, 着色器里对应 set 的
    // 全部绑定就失效; 少一段推送常量, 对应阶段读到的就是垃圾。两者都不会
    // 让 vkCreatePipelineLayout 失败。
    constexpr SizeType kInlineSetLayouts      = 8;
    constexpr SizeType kInlinePushConstRanges = 4;

    TSmallVector<VkDescriptorSetLayout, kInlineSetLayouts> setLayouts;

    const UInt32 setLayoutCount = desc.SetLayoutCount;
    if (setLayoutCount > 0 && desc.SetLayouts == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreatePipelineLayout: 集布局数组为空但计数为 {}",
            setLayoutCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    setLayouts.Reserve(static_cast<SizeType>(setLayoutCount));

    for (UInt32 i = 0; i < setLayoutCount; ++i)
    {
        const FVulkanDescSetLayoutData* layoutData =
            m_DescSetLayouts.Get(desc.SetLayouts[i]);
        if (layoutData == nullptr)
        {
            return ERHIResult::ErrorInvalidHandle;
        }
        setLayouts.Add(layoutData->Layout);
    }

    TSmallVector<VkPushConstantRange, kInlinePushConstRanges> pushConstRanges;

    const UInt32 pushConstCount = desc.PushConstantRangeCount;
    if (pushConstCount > 0 && desc.PushConstantRanges == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreatePipelineLayout: 推送常量范围数组为空但计数为 {}",
            pushConstCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    pushConstRanges.Reserve(static_cast<SizeType>(pushConstCount));

    for (UInt32 i = 0; i < pushConstCount; ++i)
    {
        const FRHIPushConstantRange& src = desc.PushConstantRanges[i];

        VkPushConstantRange range;
        range.stageFlags = ToVkShaderStageFlags(src.StageFlags);
        range.offset     = src.Offset;
        range.size       = src.Size;

        pushConstRanges.Add(range);
    }

    VkPipelineLayoutCreateInfo createInfo = {};
    createInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.setLayoutCount         = setLayoutCount;
    createInfo.pSetLayouts            = setLayouts.GetData();
    createInfo.pushConstantRangeCount = pushConstCount;
    createInfo.pPushConstantRanges    = pushConstRanges.GetData();

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
    //
    // ShaderStages 是 FRHIGraphicsPipelineDesc 里的定长数组, 因此
    // ShaderStageCount 超过 kMaxShaderStages 时, 调用方自己就已经越界了。
    // 这里不能顺着写下去 —— 那是往栈上写越界。拒绝并说明原因。
    if (desc.ShaderStageCount > kMaxShaderStages)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateGraphicsPipeline: 着色器阶段数 {} 超过上限 {}",
            desc.ShaderStageCount, kMaxShaderStages);
        return ERHIResult::ErrorInvalidParameter;
    }

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
    //
    // 顶点属性由网格布局决定。少一个属性, 着色器里对应的 location 就读到
    // 未定义值 —— 表现是模型的法线/UV 之类整体错乱, 而管线创建成功。
    constexpr SizeType kInlineVertexBindings   = 16;
    constexpr SizeType kInlineVertexAttributes = 32;

    TSmallVector<VkVertexInputBindingDescription, kInlineVertexBindings>
        vertexBindings;

    const UInt32 vertBindCount = desc.VertexInput.BindingCount;
    if (vertBindCount > 0 && desc.VertexInput.Bindings == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateGraphicsPipeline: 顶点绑定数组为空但计数为 {}",
            vertBindCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    vertexBindings.Reserve(static_cast<SizeType>(vertBindCount));

    for (UInt32 i = 0; i < vertBindCount; ++i)
    {
        const FRHIVertexInputBinding& src = desc.VertexInput.Bindings[i];

        VkVertexInputBindingDescription dst;
        dst.binding   = src.Binding;
        dst.stride    = src.Stride;
        dst.inputRate = ToVkVertexInputRate(src.InputRate);

        vertexBindings.Add(dst);
    }

    TSmallVector<VkVertexInputAttributeDescription, kInlineVertexAttributes>
        vertexAttrs;

    const UInt32 vertAttrCount = desc.VertexInput.AttributeCount;
    if (vertAttrCount > 0 && desc.VertexInput.Attributes == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateGraphicsPipeline: 顶点属性数组为空但计数为 {}",
            vertAttrCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    vertexAttrs.Reserve(static_cast<SizeType>(vertAttrCount));

    for (UInt32 i = 0; i < vertAttrCount; ++i)
    {
        const FRHIVertexInputAttribute& src =
            desc.VertexInput.Attributes[i];

        VkVertexInputAttributeDescription dst;
        dst.location = src.Location;
        dst.binding  = src.Binding;
        dst.format   = ToVkFormat(src.Format);
        dst.offset   = src.Offset;

        vertexAttrs.Add(dst);
    }

    VkPipelineVertexInputStateCreateInfo vertexInputState = {};
    vertexInputState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount   = vertBindCount;
    vertexInputState.pVertexBindingDescriptions      = vertexBindings.GetData();
    vertexInputState.vertexAttributeDescriptionCount = vertAttrCount;
    vertexInputState.pVertexAttributeDescriptions    = vertexAttrs.GetData();

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
    //
    // 混合状态必须与渲染通道的颜色附件一一对应。少一个, Vulkan 就会因为
    // attachmentCount 与子通道的颜色附件数不匹配而拒绝 —— 但报出来的是
    // "数量不匹配", 而不是"你的第 9 个混合状态被丢了"。
    constexpr SizeType kInlineColorAttachments = 8;
    TSmallVector<VkPipelineColorBlendAttachmentState,
                 kInlineColorAttachments> blendAttachments;

    const UInt32 blendAttachCount = desc.ColorBlend.AttachmentCount;
    if (blendAttachCount > 0 && desc.ColorBlend.Attachments == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] CreateGraphicsPipeline: 混合附件数组为空但计数为 {}",
            blendAttachCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    blendAttachments.Reserve(static_cast<SizeType>(blendAttachCount));

    for (UInt32 i = 0; i < blendAttachCount; ++i)
    {
        const FRHIColorBlendAttachmentDesc& src =
            desc.ColorBlend.Attachments[i];

        VkPipelineColorBlendAttachmentState dst;
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

        blendAttachments.Add(dst);
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable   = desc.ColorBlend.IsLogicOpEnabled
        ? VK_TRUE : VK_FALSE;
    colorBlend.logicOp         = static_cast<VkLogicOp>(
        desc.ColorBlend.LogicOp);
    colorBlend.attachmentCount = blendAttachCount;
    colorBlend.pAttachments    = blendAttachments.GetData();
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

    // 写入条数不设上限 —— bindless 之下一次刷新成百上千个描述符是常态。
    // 截断的后果是被丢掉的那些绑定保持旧内容: 材质换了、贴图没换, 而
    // vkUpdateDescriptorSets 无返回值, 验证层也无从察觉。
    //
    // 三个数组必须一次性撑到位再填: vkWrites[i] 里存的是指向
    // bufferInfos[i] / imageInfos[i] 的**指针**, 中途扩容会让先前写好的
    // 指针全部悬空。
    constexpr SizeType kInlineWrites = 32;

    const UInt32 count = writeCount;

    TSmallVector<VkWriteDescriptorSet, kInlineWrites>   vkWrites;
    TSmallVector<VkDescriptorBufferInfo, kInlineWrites> bufferInfos;
    TSmallVector<VkDescriptorImageInfo, kInlineWrites>  imageInfos;

    ResizeZeroed(vkWrites, static_cast<SizeType>(count));
    ResizeZeroed(bufferInfos, static_cast<SizeType>(count));
    ResizeZeroed(imageInfos, static_cast<SizeType>(count));

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

    vkUpdateDescriptorSets(m_Device, count, vkWrites.GetData(), 0, nullptr);
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
