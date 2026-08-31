/*******************************************************************************
 * 文件: FRenderResourceManager.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 资源管理器 — 拥有全部网格与纹理资源，负责上传、引用计数与回收
 *   资产管线产出的中性数据在此变为可直接绘制的 GPU 对象
 *
 * 设计哲学:
 *   所有权集中于此 — 此前 GPU 缓冲区散落在 FRenderer 内部，场景节点持有它们的
 *   裸句柄。这让渲染器被迫为每种资产的生命周期负责，也让"加载任意场景"无从谈起。
 *   资源集中到管理器后：管理器拥有，场景引用，渲染器只读，三方职责互不重叠。
 *
 *   上传是同步的 — 当前用一次性命令缓冲区提交并等待完成。异步上传需要独立的
 *   传输队列、信号量与"资源就绪"状态机，在还没有真实加载压力时引入这套机制
 *   只会增加出错面。等加载耗时成为可测量的问题时再改，接口已为此留好位置。
 *
 *   纹理的色彩空间由调用方决定 — 基色贴图按 sRGB 采样，法线与粗糙度贴图必须
 *   按线性采样。文件里的声明只是提示，真正的依据是这张图在材质中的用途，
 *   因此格式选择作为参数由上传方传入，而不是从图像自身推断。
 *
 *   卸载是延迟的 — 引用归零的资源可能仍被尚未执行完的命令缓冲区引用。
 *   立即调用 vkDestroyBuffer 会撞上 VUID-vkDestroyBuffer-buffer-00922。
 *   资源改为先"退役"进待销毁队列, 等到 MaxFramesInFlight 帧之后
 *   (即那一帧的栅栏已经通过) 才真正销毁。槽位可以立即复用, 因为 GPU
 *   对象已从槽位中移出。
 *
 * 技术特性:
 *   - 索引宽度按顶点数自适应, 不超过 65535 时用 16 位
 *   - 采样器按配置去重共享, 而非每张纹理创建一个
 *   - 槽位复用配合代际号, 卸载后旧句柄立即失效
 *   - 引用归零不立即释放, 由 CollectUnreferenced 统一回收
 *   - 退役资源按帧序号延迟销毁, 避免销毁 GPU 仍在使用的对象
 *
 * 依赖关系:
 *   内部: RenderCore/Resources/FRenderResources.h, RHI/RHI/IRHIDevice.h,
 *          RenderCore/Renderer/FRenderContext.h,
 *          AssetPipeline/FAssetTypes.h, AssetPipeline/FImageTypes.h
 *
 * 注意事项:
 *   非线程安全 — 上传走一次性命令缓冲区并等待, 并发调用会互相干扰
 *   Shutdown 前必须完成 vkDeviceWaitIdle — 它会无条件冲刷待销毁队列
 *
 ******************************************************************************/

#pragma once

// RHI 头先行 —— 它引入 windows.h。Core 的部分头在 windows.h 缺席时会
// 前向声明 Win32 API, 顺序颠倒会与真正的声明冲突。
#include "RHI/RHIMinimal.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"

#include "RenderCore/Resources/FRenderResources.h"
#include "AssetPipeline/FAssetTypes.h"
#include "AssetPipeline/FImageTypes.h"
#include "AssetPipeline/FDdsDecoder.h"
#include "Core/Containers/TMap.h"

namespace Limx
{

// FRenderContext 只在实现中使用, 此处前向声明即可。
// 直接包含它的头会形成循环: RenderCoreMinimal 聚合本头, 而 FRenderContext.h
// 又包含 RenderCoreMinimal。
class FRenderContext;

// ============================================================================
// FTextureUploadOptions — 纹理上传选项
// ============================================================================

/// 纹理上传选项
struct FTextureUploadOptions
{
    /// 是否按 sRGB 格式创建
    ///
    /// 必须由调用方按用途决定: 基色与自发光贴图是 sRGB，法线、粗糙度、
    /// 金属度、遮蔽贴图是线性数据。若按线性采样 sRGB 贴图，颜色会整体发暗；
    /// 若按 sRGB 采样法线贴图，光照方向会系统性偏移。
    bool IsSrgb = false;

    /// 采样时的寻址模式
    ESamplerAddressMode AddressMode = ESamplerAddressMode::Repeat;

    /// 是否启用各向异性过滤
    bool UseAnisotropy = true;
};

// ============================================================================
// FRenderResourceManager — GPU 资源管理器
// ============================================================================

/// GPU 资源管理器
class LIMX_RENDERCORE_API FRenderResourceManager
{
public:
    FRenderResourceManager() = default;
    ~FRenderResourceManager();

    FRenderResourceManager(const FRenderResourceManager&)            = delete;
    FRenderResourceManager& operator=(const FRenderResourceManager&) = delete;

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 初始化
    /// @param device  RHI 设备
    /// @param context 渲染上下文 (提供一次性命令缓冲区)
    ERHIResult Initialize(IRHIDevice* device, FRenderContext* context);

    /// 销毁全部资源
    ///
    /// 调用前必须确保 GPU 已空闲, 否则可能销毁仍在使用的资源。
    void Shutdown();

    LIMX_NODISCARD bool IsInitialized() const { return m_Device != nullptr; }

    // ========================================================================
    // 网格
    // ========================================================================

    /// 由 CPU 网格数据创建 GPU 网格
    ///
    /// 返回时引用计数为 1 —— 调用方持有一份所有权引用。把句柄交给
    /// 其他持有者 (如 LMeshTrait) 后, 调用方必须释放自己这一份,
    /// 否则资源永远不会变成可回收状态。
    /// @param meshData 资产管线产出的网格
    /// @param name     调试名称; 为空时取 meshData 的名称
    /// @return 句柄; 失败时返回无效句柄
    FMeshResourceHandle CreateMesh(const FMeshData& meshData,
                                   const FName& name = FName());

    /// 取网格资源 — 句柄失效返回 nullptr
    LIMX_NODISCARD const FMeshResource* GetMesh(FMeshResourceHandle handle) const;

    // ========================================================================
    // 纹理
    // ========================================================================

    /// 由 CPU 图像数据创建 GPU 纹理
    ///
    /// 与 CreateMesh 相同, 返回时引用计数为 1, 调用方持有一份所有权引用。
    /// @param image   解码后的图像
    /// @param options 上传选项 (色彩空间由调用方按用途决定)
    /// @param name    调试名称
    FTextureResourceHandle CreateTexture(
        const FImageData& image,
        const FTextureUploadOptions& options = FTextureUploadOptions(),
        const FName& name = FName());

    /// 由块压缩纹理 (DDS) 创建 GPU 纹理
    ///
    /// 与未压缩路径的关键差别: mip 链**不生成**, 而是逐层从文件里搬上去。
    /// 块压缩格式没有 BLIT_SRC/BLIT_DST 能力, vkCmdBlitImage 对它们是非法
    /// 调用 —— 验证层会直接报错。离线烘好的 mip 链质量也更高: 运行时逐级
    /// blit 是"解压 → 放缩 → 重压", 量化误差会沿链累积。
    ///
    /// 色彩空间取自文件而非 options.IsSrgb —— DDS 的 dxgiFormat 已经把
    /// sRGB 与否写死了, 那才是事实。两者不一致时以文件为准并记一条告警。
    /// @param image   FDdsDecoder 解析出来的压缩纹理
    /// @param options 上传选项 (寻址模式与各向异性; 色彩空间见上)
    /// @param name    调试名称
    FTextureResourceHandle CreateTexture(
        const FCompressedImageData& image,
        const FTextureUploadOptions& options = FTextureUploadOptions(),
        const FName& name = FName());

    /// 块压缩格式 → RHI 像素格式
    ///
    /// 一一对应且无默认分支 —— 认不出来返回 Unknown 由调用方报错。
    /// 公开出来是为了让上层 (材质、场景导入) 能在上传之前就知道
    /// 目标格式, 而不必先建一张纹理再去问它。
    LIMX_NODISCARD static EPixelFormat MapCompressedPixelFormat(
        EBlockCompressionFormat format);

    /// 创建一张纯色纹理 — 用于缺失贴图的兜底
    FTextureResourceHandle CreateSolidColorTexture(
        UInt8 red, UInt8 green, UInt8 blue, UInt8 alpha,
        bool isSrgb, const FName& name);

    /// 取纹理资源 — 句柄失效返回 nullptr
    LIMX_NODISCARD const FTextureResource* GetTexture(
        FTextureResourceHandle handle) const;

    // ========================================================================
    // 引用计数
    // ========================================================================

    void AddMeshReference(FMeshResourceHandle handle);
    void ReleaseMeshReference(FMeshResourceHandle handle);

    void AddTextureReference(FTextureResourceHandle handle);
    void ReleaseTextureReference(FTextureResourceHandle handle);

    LIMX_NODISCARD UInt32 GetMeshReferenceCount(FMeshResourceHandle handle) const;
    LIMX_NODISCARD UInt32 GetTextureReferenceCount(
        FTextureResourceHandle handle) const;

    // ========================================================================
    // 回收
    // ========================================================================

    /// 退役全部引用计数为零的资源
    ///
    /// 引用归零不立即释放: 材质切换与 LOD 过渡会让资源在一帧内被放下又拾起，
    /// 立即释放会造成反复的上传抖动。由调用方在帧末或关卡切换时统一收割。
    ///
    /// 退役后槽位立即可复用, 句柄立即失效, 但 GPU 对象要再过
    /// MaxFramesInFlight 帧才真正销毁。
    /// @return 被退役的资源数
    UInt32 CollectUnreferenced();

    /// 强制退役指定网格 — 退役后该句柄立即失效
    void UnloadMesh(FMeshResourceHandle handle);

    /// 强制退役指定纹理
    void UnloadTexture(FTextureResourceHandle handle);

    /// 销毁已确定脱离 GPU 使用的退役资源
    ///
    /// 由 FRenderContext::BeginFrame 在栅栏等待之后调用 —— 那一刻
    /// MaxFramesInFlight 帧之前的提交必然已执行完毕。
    /// @return 本次销毁的资源数
    UInt32 ProcessPendingReleases();

    /// 无条件销毁全部退役资源 — 仅在 GPU 已空闲时调用
    /// @return 本次销毁的资源数
    UInt32 FlushPendingReleases();

    /// 待销毁队列中的资源数
    LIMX_NODISCARD UInt32 GetPendingReleaseCount() const
    {
        return static_cast<UInt32>(m_PendingReleases.GetSize());
    }

    // ========================================================================
    // 统计
    // ========================================================================

    LIMX_NODISCARD FRenderResourceStats GetStats() const;

    /// 向日志输出一份用量报告
    void LogStats(const AnsiChar* context) const;

private:
    /// 网格槽位
    struct FMeshSlot
    {
        FMeshResource Resource;
        UInt32        Generation     = 1;
        UInt32        ReferenceCount = 0;
        bool          IsActive       = false;
    };

    /// 纹理槽位
    struct FTextureSlot
    {
        FTextureResource Resource;
        UInt32           Generation     = 1;
        UInt32           ReferenceCount = 0;
        bool             IsActive       = false;
    };

    /// 待销毁条目 — 已从槽位中移出、等待 GPU 用完的 GPU 对象
    ///
    /// 网格与纹理共用一个队列: 两者的字段互不重叠, 无效句柄会被跳过,
    /// 而两条独立队列意味着两份相同的帧序号判断逻辑。
    struct FPendingRelease
    {
        FRHIBufferHandle      VertexBuffer;
        FRHIBufferHandle      IndexBuffer;
        FRHITextureHandle     Texture;
        FRHITextureViewHandle View;

        /// 退役时的帧序号 —— 该帧的提交是最后一批可能引用这些对象的命令
        UInt64 RetireFrame = 0;
    };

    /// 采样器缓存键 — 相同配置的采样器共享同一个对象
    struct FSamplerKey
    {
        ESamplerAddressMode AddressMode = ESamplerAddressMode::Repeat;
        bool                UseAnisotropy = true;
        UInt32              MipLevels     = 1;

        LIMX_NODISCARD bool operator==(const FSamplerKey& other) const
        {
            return AddressMode == other.AddressMode &&
                   UseAnisotropy == other.UseAnisotropy &&
                   MipLevels == other.MipLevels;
        }
    };

    // ========================================================================
    // 内部实现
    // ========================================================================

    /// 创建设备本地缓冲区并经暂存缓冲区上传数据
    ERHIResult UploadBuffer(const void* data, UInt64 byteCount,
                            EBufferUsage usage,
                            FRHIBufferHandle& outBuffer);

    /// 创建纹理、上传像素并逐级生成 mip 链
    ///
    /// mip 层数由格式能力决定而非调用方指定 —— 逐级 blit 要求格式支持
    /// BlitSrc/BlitDst 与线性过滤，不满足时退回单层。实际层数经
    /// outMipLevels 返回，供视图与采样器使用。
    ERHIResult UploadTexture2D(const void* pixels, UInt64 byteCount,
                               UInt32 width, UInt32 height,
                               EPixelFormat format,
                               UInt32& outMipLevels,
                               FRHITextureHandle& outTexture);

    /// 创建纹理并逐层上传已经烘好的 mip 链
    ///
    /// 不做任何 mip 生成 —— 块压缩格式不能 blit, 而 DDS 本来就带全了。
    /// 每一层发一条 CopyBufferToTexture, 缓冲区偏移取该层在载荷中的偏移;
    /// BufferRowLength/BufferImageHeight 填 0 让驱动按紧凑排列算 ——
    /// 这两个字段对压缩格式的语义是**像素**数而非块数, 手工填极易填成块数。
    ERHIResult UploadCompressedTexture2D(const FCompressedImageData& image,
                                         EPixelFormat format,
                                         FRHITextureHandle& outTexture);

    /// 取或创建匹配配置的采样器
    FRHISamplerHandle AcquireSampler(const FSamplerKey& key);

    /// 把图像格式与色彩空间映射为 RHI 像素格式
    LIMX_NODISCARD static EPixelFormat MapPixelFormat(EImageFormat format,
                                                      bool isSrgb);

    /// 校验句柄并返回槽位
    LIMX_NODISCARD const FMeshSlot* ResolveMesh(FMeshResourceHandle handle) const;
    LIMX_NODISCARD FMeshSlot* ResolveMesh(FMeshResourceHandle handle);

    LIMX_NODISCARD const FTextureSlot* ResolveTexture(
        FTextureResourceHandle handle) const;
    LIMX_NODISCARD FTextureSlot* ResolveTexture(FTextureResourceHandle handle);

    /// 把槽位持有的 GPU 对象移入待销毁队列并重置槽位
    void RetireMeshSlot(FMeshSlot& slot);
    void RetireTextureSlot(FTextureSlot& slot);

    /// 立即销毁一条待销毁条目持有的 GPU 对象
    void DestroyPendingRelease(FPendingRelease& entry);

    /// 取当前帧序号 — 上下文缺失时返回 0
    LIMX_NODISCARD UInt64 GetCurrentFrame() const;

    // ========================================================================
    // 成员数据
    // ========================================================================

    IRHIDevice*     m_Device  = nullptr;
    FRenderContext* m_Context = nullptr;

    TArray<FMeshSlot>    m_Meshes;
    TArray<UInt32>       m_FreeMeshSlots;

    TArray<FTextureSlot> m_Textures;
    TArray<UInt32>       m_FreeTextureSlots;

    /// 待销毁队列 — 按退役帧序号延迟销毁
    TArray<FPendingRelease>   m_PendingReleases;

    /// 共享采样器 — 相同配置只创建一次
    TArray<FSamplerKey>       m_SamplerKeys;
    TArray<FRHISamplerHandle> m_Samplers;
};

} // namespace Limx
