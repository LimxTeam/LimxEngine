// ============================================================
// 文件名称：FShaderManager.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：注册制着色器管理 — 单例管理器统一管控所有编译后的
//          SPIR-V 着色器。与 LSC 工具链配合，实现
//          "源码 (Shaders/) → LSC 编译 → .spv (Binaries/Shaders/)
//          → FShaderManager 加载+缓存+命名查找"完整流水线。
//          着色器按分组组织 (Builtin/PostProcess/UI/...)，
//          编译输出目录镜像源码目录结构。
//          架构参考 UE5 Engine/Shaders/ + FShaderMap 模式。
// 功能描述：着色器资源的运行时管理器 (单例)。
//          Initialize() 指定编译着色器根目录并准备就绪。
//          LoadSpirV() 按名称加载 SPIR-V 并缓存到内存。
//          CreateShaderModule() 一步完成加载+创建 VkShaderModule。
//          支持着色器分组目录 (Builtin/PostProcess/UI 等)。
//          缓存策略: 首次加载后驻留内存，ClearCache() 可强制清除。
// 技术特性：基于 FPlatformFile::ReadAllBytes 零 STL 二进制读取；
//          TMap<FString, TArray<UInt8>> 名称→SPIR-V 缓存；
//          SPIR-V 魔数 + 4 字节对齐 + 最小大小三重校验；
//          着色器名称使用正斜杠路径 (如 "Builtin/triangle.vert")，
//          自动追加 .spv 扩展名定位磁盘文件。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ Get()                    │ 获取单例引用                    │
// │ Initialize()             │ 初始化管理器 (设置根目录)         │
// │ Shutdown()               │ 关闭管理器 (清空缓存)            │
// │ IsInitialized()          │ 是否已初始化                    │
// │ LoadSpirV()              │ 按名称加载 SPIR-V (带缓存)       │
// │ CreateShaderModule()     │ 加载 SPIR-V + 创建着色器模块      │
// │ IsShaderCached()         │ 检查着色器是否已缓存              │
// │ ClearCache()             │ 清空 SPIR-V 缓存                │
// │ GetShaderBinaryRoot()    │ 获取编译着色器根目录              │
// │ ValidateSpirV()          │ SPIR-V 三重校验                 │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                    │ 类型                         │ 描述          │
// │──────────────────────────│────────────────────────────│──────────────│
// │ m_ShaderBinaryRoot       │ FString                    │ 编译着色器根目录│
// │ m_SpirVCache             │ TMap<FString, TArray<UInt8>>│ SPIR-V 缓存  │
// │ m_IsInitialized          │ bool                       │ 初始化标志     │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (FShaderLoader 重构)    │
// ============================================================

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

namespace Limx
{

// 着色器管理器日志类别
LIMX_DECLARE_LOG_CATEGORY(LogShaderManager)

// ============================================================================
// FShaderManager — 运行时着色器管理器 (单例)
// ============================================================================

class FShaderManager
{
public:
    LIMX_NON_COPYABLE(FShaderManager);

    /// SPIR-V 魔数: 0x07230203
    static constexpr UInt32 kSpirVMagic = 0x07230203;

    /// 默认编译着色器根目录 (相对于引擎根)
    static constexpr const AnsiChar* kDefaultShaderBinaryRoot =
        "Binaries/Shaders";

    // ========================================================================
    // 单例
    // ========================================================================

    /// 获取单例引用
    static FShaderManager& Get()
    {
        static FShaderManager instance;
        return instance;
    }

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 初始化管理器
    /// @param shaderBinaryRoot 编译着色器根目录 (默认 "Binaries/Shaders")
    void Initialize(
        const FString& shaderBinaryRoot = FString(kDefaultShaderBinaryRoot))
    {
        m_ShaderBinaryRoot = shaderBinaryRoot;
        m_IsInitialized = true;

        LIMX_LOG(LogShaderManager, Log,
                 "[ShaderManager] 初始化完成 — 着色器根目录: {}",
                 m_ShaderBinaryRoot);
    }

    /// 关闭管理器 — 清空 SPIR-V 缓存
    void Shutdown()
    {
        ClearCache();
        m_IsInitialized = false;

        LIMX_LOG(LogShaderManager, Log,
                 "[ShaderManager] 已关闭");
    }

    /// 是否已初始化
    LIMX_NODISCARD bool IsInitialized() const { return m_IsInitialized; }

    // ========================================================================
    // 着色器加载 (带缓存)
    // ========================================================================

    /// 按名称加载 SPIR-V 着色器 (首次从磁盘加载，后续从缓存返回)
    ///
    /// @param shaderName 着色器名称，使用正斜杠路径格式:
    ///                   "Builtin/triangle.vert"
    ///                   "PostProcess/bloom.frag"
    ///                   "UI/rect.vert"
    ///                   自动追加 .spv 扩展名定位磁盘文件。
    /// @return SPIR-V 字节数据指针 (缓存所有权)，失败返回 nullptr
    LIMX_NODISCARD const TArray<UInt8>* LoadSpirV(
        const FString& shaderName)
    {
        LIMX_CHECK(m_IsInitialized);

        // 检查缓存
        const TArray<UInt8>* cached = m_SpirVCache.Find(shaderName);
        if (cached != nullptr)
        {
            return cached;
        }

        // 构建磁盘路径: root/name.spv
        FString diskPath = FPath::Combine(
            m_ShaderBinaryRoot,
            shaderName + FString(".spv"));

        // 从磁盘加载
        TArray<UInt8> spirvData = LoadSpirVFromDisk(diskPath);
        if (spirvData.IsEmpty())
        {
            return nullptr;
        }

        // 存入缓存并返回指针
        m_SpirVCache.Add(shaderName, MoveTemp(spirvData));

        LIMX_LOG(LogShaderManager, Log,
                 "[ShaderManager] 着色器已缓存: {}",
                 shaderName);

        return m_SpirVCache.Find(shaderName);
    }

    /// 加载 SPIR-V 并创建着色器模块 — 一步完成
    ///
    /// @param device     RHI 设备
    /// @param shaderName 着色器名称 (如 "Builtin/triangle.vert")
    /// @param stage      着色器阶段
    /// @param outHandle  输出着色器句柄
    /// @return Success 或错误码
    ERHIResult CreateShaderModule(
        IRHIDevice* device,
        const FString& shaderName,
        EShaderStage stage,
        FRHIShaderHandle& outHandle)
    {
        const TArray<UInt8>* spirvData = LoadSpirV(shaderName);
        if (spirvData == nullptr)
        {
            LIMX_LOG(LogShaderManager, Error,
                     "[ShaderManager] 着色器加载失败, 无法创建模块: {}",
                     shaderName);
            return ERHIResult::ErrorShaderCompilation;
        }

        FRHIShaderDesc shaderDesc = {};
        shaderDesc.Stage        = stage;
        shaderDesc.ByteCode     = spirvData->GetData();
        shaderDesc.ByteCodeSize = static_cast<UInt64>(spirvData->GetSize());
        shaderDesc.EntryPoint   = "main";
        shaderDesc.DebugName    = shaderName.GetCStr();

        ERHIResult result = device->CreateShader(shaderDesc, outHandle);
        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogShaderManager, Error,
                     "[ShaderManager] 着色器模块创建失败: {}",
                     shaderName);
        }

        return result;
    }

    // ========================================================================
    // 缓存管理
    // ========================================================================

    /// 着色器是否已缓存
    LIMX_NODISCARD bool IsShaderCached(const FString& shaderName) const
    {
        return m_SpirVCache.Contains(shaderName);
    }

    /// 清空 SPIR-V 缓存
    void ClearCache()
    {
        SizeType cachedCount = m_SpirVCache.GetSize();
        m_SpirVCache.Clear();

        if (cachedCount > 0)
        {
            LIMX_LOG(LogShaderManager, Log,
                     "[ShaderManager] 缓存已清空 ({} 个着色器)",
                     cachedCount);
        }
    }

    /// 获取编译着色器根目录
    LIMX_NODISCARD const FString& GetShaderBinaryRoot() const
    {
        return m_ShaderBinaryRoot;
    }

    /// 获取缓存中的着色器数量
    LIMX_NODISCARD SizeType GetCachedShaderCount() const
    {
        return m_SpirVCache.GetSize();
    }

private:
    FShaderManager() = default;
    ~FShaderManager() = default;

    // ========================================================================
    // 内部方法
    // ========================================================================

    /// 从磁盘加载 SPIR-V 文件并执行三重校验
    LIMX_NODISCARD TArray<UInt8> LoadSpirVFromDisk(const FString& filePath)
    {
        // 检查文件是否存在
        if (!FPlatformFile::Exists(filePath))
        {
            LIMX_LOG(LogShaderManager, Error,
                     "[ShaderManager] 着色器文件不存在: {}",
                     filePath);
            return TArray<UInt8>();
        }

        // 读取完整文件内容
        TArray<UInt8> spirvData = FPlatformFile::ReadAllBytes(filePath);
        if (spirvData.IsEmpty())
        {
            LIMX_LOG(LogShaderManager, Error,
                     "[ShaderManager] 着色器文件读取失败或为空: {}",
                     filePath);
            return TArray<UInt8>();
        }

        // 三重校验: 最小大小 + 魔数 + 4 字节对齐
        if (!ValidateSpirV(spirvData, filePath))
        {
            return TArray<UInt8>();
        }

        LIMX_LOG(LogShaderManager, Log,
                 "[ShaderManager] 着色器加载: {} ({} bytes)",
                 filePath, spirvData.GetSize());

        return spirvData;
    }

    /// SPIR-V 三重校验: 最小大小 + 魔数 + 4 字节对齐
    LIMX_NODISCARD bool ValidateSpirV(
        const TArray<UInt8>& spirvData,
        const FString& filePath)
    {
        // 校验 1: SPIR-V 最小有效大小 (5 个 UInt32 头部 = 20 字节)
        if (spirvData.GetSize() < 20)
        {
            LIMX_LOG(LogShaderManager, Error,
                     "[ShaderManager] SPIR-V 过小 ({} bytes): {}",
                     spirvData.GetSize(), filePath);
            return false;
        }

        // 校验 2: 魔数 0x07230203
        UInt32 magic = static_cast<UInt32>(spirvData[0])
                     | (static_cast<UInt32>(spirvData[1]) << 8)
                     | (static_cast<UInt32>(spirvData[2]) << 16)
                     | (static_cast<UInt32>(spirvData[3]) << 24);

        if (magic != kSpirVMagic)
        {
            LIMX_LOG(LogShaderManager, Error,
                     "[ShaderManager] SPIR-V 魔数校验失败: {}",
                     filePath);
            return false;
        }

        // 校验 3: 4 字节对齐
        if (spirvData.GetSize() % 4 != 0)
        {
            LIMX_LOG(LogShaderManager, Error,
                     "[ShaderManager] SPIR-V 大小不是 4 的倍数 "
                     "({} bytes): {}",
                     spirvData.GetSize(), filePath);
            return false;
        }

        return true;
    }

    // ========================================================================
    // 成员
    // ========================================================================

    /// 编译着色器根目录 (如 "Binaries/Shaders")
    FString m_ShaderBinaryRoot;

    /// 名称 → SPIR-V 字节数据缓存
    /// Key: 着色器名称 (如 "Builtin/triangle.vert")
    /// Value: SPIR-V 二进制数据
    TMap<FString, TArray<UInt8>> m_SpirVCache;

    /// 初始化标志
    bool m_IsInitialized = false;
};

} // namespace Limx
