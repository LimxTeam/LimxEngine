/*******************************************************************************
 * 文件: ReflectionMacros.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎反射系统宏定义 — 供 LHT (Limx Header Tool) 解析
 *   定义 LCLASS, LSTRUCT, LENUM, LPROPERTY, LFUNCTION, LDELEGATE 等宏
 *   这些宏在编译时展开为空操作，仅作为 LHT 的标记被解析
 *   LHT 会根据这些宏生成 .generated.h/.generated.cpp 反射代码
 *
 * 设计哲学:
 *   零运行时开销 — 所有反射宏在编译时展开为空或最小代码
 *   LHT 驱动 — 宏本身不执行反射逻辑，仅为 LHT 提供解析锚点
 *   UE 风格 — 说明符语法兼容 UE 开发者的使用习惯
 *   Limx 扩展 — 在 UE 基础上增加线程安全、对象池、异步等说明符
 *
 * 技术特性:
 *   - LCLASS(说明符...) — 标记反射类
 *   - LSTRUCT(说明符...) — 标记反射结构体
 *   - LENUM(说明符...) — 标记反射枚举
 *   - LPROPERTY(说明符...) — 标记反射属性
 *   - LFUNCTION(说明符...) — 标记反射函数
 *   - LDELEGATE(说明符...) — 标记反射委托
 *   - LGENERATED_BODY() — 注入 LHT 生成的代码
 *   - LMETA(说明符...) — 附加元数据标记
 *
 * 依赖关系:
 *   内部: Core/HAL/Platform.h (编译器检测)
 *
 * 注意事项:
 *   每个使用 LCLASS/LSTRUCT 的类/结构体必须包含 LGENERATED_BODY()
 *   LGENERATED_BODY() 必须出现在类体的第一行（访问修饰符之前）
 *   包含反射宏的头文件必须有对应的 .generated.h 被 include
 *   LHT 会自动跳过 .generated.h/.generated.cpp 文件
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"

// ============================================================================
// 类型声明宏 — LHT 解析锚点
// ============================================================================

/// 标记反射类 — LHT 会提取说明符和类结构信息
/// 用法:
///   LCLASS(Serializable, BlueprintType)
///   class LIMX_CORE_API MyClass : public LObject
///   {
///       LGENERATED_BODY()
///       ...
///   };
///
/// 支持的说明符 (参见 LHT specifiers.rs):
///   基础: Serializable, Abstract, NonCopyable, CustomConstructor
///   蓝图: BlueprintType, Blueprintable, Placeable
///   编辑器: ClassGroup="xxx", HideCategories="xxx"
///   配置: Config="Game", PerObjectConfig
///   Limx 独有: ThreadSafe, Singleton, ObjectPool, HotReloadable
#define LCLASS(...)

/// 标记反射结构体 — 与 LCLASS 类似但用于 POD 或轻量数据类型
/// 用法:
///   LSTRUCT(Serializable)
///   struct LIMX_CORE_API FVector
///   {
///       LGENERATED_BODY()
///       ...
///   };
#define LSTRUCT(...)

/// 标记反射枚举 — LHT 会生成 EnumTraits 特化 (字符串化/反字符串化)
/// 用法:
///   LENUM(Flags)
///   enum class ETextureFormat : Limx::UInt8
///   {
///       RGBA8Unorm,
///       RGBA16Float = 0x10,
///   };
#define LENUM(...)

/// 标记反射委托 — LHT 会生成委托类型别名
/// 用法:
///   LDELEGATE(Multicast)
///   void OnHealthChanged(float newHealth, float oldHealth);
///
/// 支持的说明符: Multicast, Dynamic, Sparse, BlueprintAssignable
#define LDELEGATE(...)

// ============================================================================
// 成员声明宏 — LHT 解析锚点
// ============================================================================

/// 标记反射属性 — LHT 会提取类型、说明符、默认值等信息
/// 用法:
///   LPROPERTY(EditAnywhere, Serializable, Category="Transform",
///             meta=(DisplayName="位置", ToolTip="世界空间中的位置"))
///   FVector m_Position = FVector::Zero();
///
/// 支持的说明符:
///   编辑器: EditAnywhere, VisibleAnywhere, AdvancedDisplay
///   序列化: Serializable, Transient, SaveGame
///   网络: Replicated, ReplicatedUsing="OnRep_xxx"
///   蓝图: BlueprintReadOnly, BlueprintReadWrite
///   约束: ClampMin=0.0, ClampMax=100.0
///   Limx 独有: Observable, TwoWayBinding, Animatable, DirtyFlag
#define LPROPERTY(...)

/// 标记反射函数 — LHT 会提取签名、说明符、参数信息
/// 用法:
///   LFUNCTION(BlueprintCallable, Category="Combat")
///   float CalculateDamage(float baseDamage, float multiplier) const;
///
///   LFUNCTION(Server, Reliable, WithValidation)
///   void ServerFireWeapon(FVector origin, FVector direction);
///
/// 支持的说明符:
///   基础: Callable, Pure, Const, Static
///   蓝图: BlueprintCallable, BlueprintPure, BlueprintImplementableEvent
///   RPC: Server, Client, NetMulticast, Reliable, Unreliable
///   Limx 独有: Async, Coroutine, Cached, Transactional, Profile
#define LFUNCTION(...)

/// 附加元数据标记 — 为紧随其后的声明附加额外元数据
/// 用法:
///   LMETA(DisplayName="生命值", ToolTip="角色当前生命值")
///   此宏可与 LPROPERTY/LFUNCTION 配合使用，提供额外的 meta 信息
#define LMETA(...)

// ============================================================================
// 代码生成注入宏
// ============================================================================

/// 注入 LHT 生成的代码 — 必须出现在 LCLASS/LSTRUCT 类体的第一行
///
/// LHT 会将此宏替换为以下生成内容:
///   - 类型信息静态成员 (s_TypeInfo, s_Properties, s_Functions)
///   - 类型查询方法 (StaticTypeInfo(), GetTypeInfo())
///   - 构造/析构工厂 (StaticConstruct(), StaticDestruct())
///   - 序列化方法声明 (若标记 Serializable)
///   - RPC 代理方法声明 (若有 Server/Client 函数)
///   - 复制通知方法声明 (若有 Replicated 属性)
///   - Super/ThisClass 类型别名
///
/// 在 LHT 未运行时，此宏展开为最小骨架代码以允许编译通过
///
/// 用法:
///   LCLASS(...)
///   class MyClass : public LObject
///   {
///       LGENERATED_BODY()
///   public:
///       // ... 成员声明
///   };
///
/// 注意: LGENERATED_BODY() 会改变后续访问修饰符为 public
///       因此建议在其后显式声明 public:/protected:/private:

// 当 LHT 已生成代码时，.generated.h 中会 #define LGENERATED_BODY_ClassName()
// 并在此处通过 #include 展开
// 当 LHT 尚未运行时，提供默认空实现以允许编译
#if !defined(LGENERATED_BODY)
    #define LGENERATED_BODY()
#endif

// ============================================================================
// 反射辅助宏
// ============================================================================

/// 标记一个类型为反射系统的基类
/// 该类自身不需要 LCLASS 标记，但提供反射基础设施
#define LIMX_REFLECTION_BASE() \
public: \
    static constexpr bool HasReflection = false; \
    virtual const void* GetTypeInfo() const { return nullptr; }

/// 在 .cpp 文件中实现反射类的类型注册
/// 通常由 LHT 自动生成，手动使用场景极少
/// 用法: LIMX_IMPLEMENT_TYPE(MyClass)
#define LIMX_IMPLEMENT_TYPE(ClassName) \
    void ClassName::StaticRegisterType() {}

/// 声明前向引用的反射类型
/// 用法: LIMX_DECLARE_TYPE(MyClass)
#define LIMX_DECLARE_TYPE(ClassName) \
    class ClassName

/// 声明前向引用的反射结构体
/// 用法: LIMX_DECLARE_STRUCT(FMyStruct)
#define LIMX_DECLARE_STRUCT(StructName) \
    struct StructName

// ============================================================================
// 生成文件包含辅助
// ============================================================================

/// 包含当前文件对应的 .generated.h
/// 用法: 在头文件末尾（#pragma once 之后、namespace 之前）
///   #include LIMX_GENERATED_HEADER(MyClass)
/// 展开为: #include "MyClass.generated.h"
///
/// 注意: 此宏需要 LHT 已生成对应文件
///       在初次编译或 LHT 未运行时，可能需要先执行 lht generate
// #define LIMX_GENERATED_HEADER(FileName) LIMX_STRINGIFY(FileName.generated.h)
