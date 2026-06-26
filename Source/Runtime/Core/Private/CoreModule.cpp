/*******************************************************************************
 * 文件: CoreModule.cpp
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LimxCore 模块入口 — 模块初始化与关闭
 *   负责核心子系统的启动顺序编排和优雅关闭
 *   作为引擎最底层模块，在所有其他模块之前初始化
 *
 * 设计哲学:
 *   显式初始化 — 不依赖全局构造函数的执行顺序
 *   可审计 — 所有初始化步骤都有明确的日志记录
 *   可回退 — 初始化失败时能安全回退已完成的步骤
 *
 * 技术特性:
 *   - 模块生命周期管理 (Startup/Shutdown)
 *   - 编译时类型系统验证
 *   - 预留反射类型注册入口
 *
 * 依赖关系:
 *   内部: Core/CoreMinimal.h
 *
 ******************************************************************************/

#include "Core/CoreMinimal.h"

namespace Limx::Core
{

// ============================================================================
// 编译时验证 — 确保类型系统在当前平台上正确
// ============================================================================

namespace StaticValidation
{
    // 验证基础类型宽度
    static_assert(sizeof(Int8)   == 1, "Int8 宽度验证失败");
    static_assert(sizeof(Int16)  == 2, "Int16 宽度验证失败");
    static_assert(sizeof(Int32)  == 4, "Int32 宽度验证失败");
    static_assert(sizeof(Int64)  == 8, "Int64 宽度验证失败");
    static_assert(sizeof(UInt8)  == 1, "UInt8 宽度验证失败");
    static_assert(sizeof(UInt16) == 2, "UInt16 宽度验证失败");
    static_assert(sizeof(UInt32) == 4, "UInt32 宽度验证失败");
    static_assert(sizeof(UInt64) == 8, "UInt64 宽度验证失败");
    static_assert(sizeof(Float32) == 4, "Float32 宽度验证失败");
    static_assert(sizeof(Float64) == 8, "Float64 宽度验证失败");

    // 验证指针相关类型
    static_assert(sizeof(void*) == LIMX_POINTER_SIZE,
        "指针大小与 LIMX_POINTER_SIZE 不匹配");
    static_assert(sizeof(SizeType) == sizeof(void*),
        "SizeType 宽度与指针宽度不匹配");
    static_assert(sizeof(IntPtr) == sizeof(void*),
        "IntPtr 宽度与指针宽度不匹配");

    // 验证类型特征正确性
    static_assert(IsIntegralV<Int32>, "IsIntegral<Int32> 验证失败");
    static_assert(IsIntegralV<UInt64>, "IsIntegral<UInt64> 验证失败");
    static_assert(!IsIntegralV<Float32>, "Float32 不应被识别为整数类型");
    static_assert(IsFloatingPointV<Float32>, "IsFloatingPoint<Float32> 验证失败");
    static_assert(IsFloatingPointV<Float64>, "IsFloatingPoint<Float64> 验证失败");
    static_assert(!IsFloatingPointV<Int32>, "Int32 不应被识别为浮点类型");
    static_assert(IsArithmeticV<Float32>, "IsArithmetic<Float32> 验证失败");
    static_assert(IsArithmeticV<Int32>, "IsArithmetic<Int32> 验证失败");

    // 验证类型变换
    static_assert(IsSameV<RemoveConstT<const Int32>, Int32>,
        "RemoveConst<const Int32> 应为 Int32");
    static_assert(IsSameV<RemoveReferenceT<Int32&>, Int32>,
        "RemoveReference<Int32&> 应为 Int32");
    static_assert(IsSameV<RemoveReferenceT<Int32&&>, Int32>,
        "RemoveReference<Int32&&> 应为 Int32");
    static_assert(IsSameV<RemovePointerT<Int32*>, Int32>,
        "RemovePointer<Int32*> 应为 Int32");

    // 验证条件选择
    static_assert(IsSameV<ConditionalT<true, Int32, Int64>, Int32>,
        "Conditional<true, Int32, Int64> 应为 Int32");
    static_assert(IsSameV<ConditionalT<false, Int32, Int64>, Int64>,
        "Conditional<false, Int32, Int64> 应为 Int64");

    // 验证小端序 (x64/ARM64 均为小端)
    static_assert(LIMX_LITTLE_ENDIAN == 1,
        "当前平台应为小端序");

    // 验证 C++ 标准版本 (MSVC /std:c++latest 报告 202004)
    static_assert(LIMX_CPP_VERSION >= 202004L,
        "需要 C++20 或更高标准");

    // ========================================================================
    // 内存系统验证
    // ========================================================================

    // 验证 IAllocator 接口可实例化 (通过 DefaultAllocator)
    static_assert(sizeof(DefaultAllocator) > 0,
        "DefaultAllocator 大小不可为零");
    static_assert(kDefaultAlignment == 16,
        "默认对齐应为 16 字节 (SSE 要求)");

    // ========================================================================
    // 容器验证
    // ========================================================================

    // TArray 基本布局
    static_assert(sizeof(TArray<Int32>) > 0,
        "TArray<Int32> 必须可实例化");
    static_assert(sizeof(TArray<Float64>) > 0,
        "TArray<Float64> 必须可实例化");

    // FString SSO 容量
    static_assert(FString::kSSOCapacity == 30,
        "FString SSO 容量应为 30 字节");

    // TMap 基本布局
    static_assert(sizeof(TMap<Int32, Int32>) > 0,
        "TMap<Int32, Int32> 必须可实例化");
    // TKeyValuePair 布局: 编译器可能在 Int32 和 Float64 之间插入 4 字节填充以满足对齐
    static_assert(sizeof(TKeyValuePair<Int32, Float64>) >= sizeof(Int32) + sizeof(Float64),
        "TKeyValuePair<Int32, Float64> 大小不应小于各字段之和");

    // ========================================================================
    // 智能指针验证
    // ========================================================================

    // TUniquePtr 默认删除器下应与裸指针大小接近
    static_assert(sizeof(TUniquePtr<Int32>) <= sizeof(void*) * 2,
        "TUniquePtr<Int32> 不应超过双指针大小");

    // TSharedPtr 应为双指针大小 (对象指针 + 控制块指针)
    static_assert(sizeof(TSharedPtr<Int32>) == sizeof(void*) * 2,
        "TSharedPtr<Int32> 应为双指针大小");

    // TWeakPtr 应与 TSharedPtr 大小一致
    static_assert(sizeof(TWeakPtr<Int32>) == sizeof(TSharedPtr<Int32>),
        "TWeakPtr 应与 TSharedPtr 大小一致");

    // ========================================================================
    // 模板工具验证
    // ========================================================================

    // TOptional 内联存储
    static_assert(sizeof(TOptional<Int32>) >= sizeof(Int32) + 1,
        "TOptional<Int32> 至少需要 sizeof(Int32) + 1 字节标志");

    // TFunction SBO
    static_assert(sizeof(TFunction<void()>) > 0,
        "TFunction<void()> 必须可实例化");

    // TDelegate
    static_assert(sizeof(TDelegate<void()>) > 0,
        "TDelegate<void()> 必须可实例化");
    static_assert(sizeof(TMulticastDelegate<void()>) > 0,
        "TMulticastDelegate<void()> 必须可实例化");

    // DelegateHandle
    static_assert(sizeof(DelegateHandle) == sizeof(UInt64),
        "DelegateHandle 应为 8 字节");

    // NullOpt 标记
    static_assert(sizeof(NullOptT) > 0, "NullOptT 必须可实例化");

    // ========================================================================
    // 数学库验证
    // ========================================================================

    static_assert(sizeof(FVector2) == sizeof(Float32) * 2,
        "FVector2 应为 8 字节");
    static_assert(sizeof(FVector3) == sizeof(Float32) * 3,
        "FVector3 应为 12 字节");
    static_assert(sizeof(FVector4) == sizeof(Float32) * 4,
        "FVector4 应为 16 字节");
    static_assert(sizeof(FMatrix) == sizeof(Float32) * 16,
        "FMatrix 应为 64 字节 (4x4 Float32)");
    static_assert(sizeof(FQuat) == sizeof(Float32) * 4,
        "FQuat 应为 16 字节");

    // ========================================================================
    // 事件系统验证
    // ========================================================================

    // 事件基类
    static_assert(sizeof(FEventBase) > 0,
        "FEventBase 必须可实例化");
    static_assert(sizeof(FEventBase::IsConsumed) == 1,
        "FEventBase::IsConsumed 应为 1 字节 bool");

    // 事件订阅句柄
    static_assert(sizeof(FEventHandle) == sizeof(UInt64),
        "FEventHandle 应为 8 字节");
    static_assert(sizeof(FEventListenerHandle) == sizeof(UInt64),
        "FEventListenerHandle 应为 8 字节");

    // 优先级枚举大小
    static_assert(sizeof(EEventPriority) == sizeof(UInt8),
        "EEventPriority 应为 1 字节");

    // 全局总线可实例化
    static_assert(sizeof(FEventBus) > 0,
        "FEventBus 必须可实例化");

    // 局部分发器可实例化
    static_assert(sizeof(FEventDispatcher) > 0,
        "FEventDispatcher 必须可实例化");

    // RAII 自动注销句柄
    static_assert(sizeof(FAutoEventListener) > 0,
        "FAutoEventListener 必须可实例化");

    // 延迟队列
    static_assert(sizeof(TEventQueue<FEventBase>) > 0,
        "TEventQueue<FEventBase> 必须可实例化");

} // namespace StaticValidation

// ============================================================================
// 模块生命周期
// ============================================================================

/// 模块启动 — 在引擎启动时由模块加载器调用
/// 负责初始化核心子系统
LIMX_CORE_API void ModuleStartup()
{
    // 当前阶段: 仅类型定义，无需运行时初始化
    // 后续将在此处初始化:
    //   - 内存分配器
    //   - 日志系统
    //   - 反射类型注册表
    //   - 性能计数器
}

/// 模块关闭 — 在引擎关闭时由模块加载器调用
/// 负责优雅释放核心子系统资源
LIMX_CORE_API void ModuleShutdown()
{
    // 当前阶段: 无需清理
    // 后续将在此处关闭:
    //   - 刷新日志
    //   - 释放全局内存池
    //   - 注销反射类型
}

} // namespace Limx::Core
