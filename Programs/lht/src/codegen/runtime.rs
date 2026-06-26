// ============================================================
// 文件名称：codegen/runtime.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：零外部依赖，纯生成，编译期类型安全，运行时零开销
// 功能描述：C++ 反射运行时头文件生成器
//           生成 TypeInfo.h / PropertyInfo.h / FunctionInfo.h /
//           TypeRegistry.h / ObjectBase.h — 反射代码依赖的完整
//           C++ 运行时基础设施，超越 UE 的 UObject/UStruct 体系
// 技术特性：编译期哈希 (FNV-1a)，链式注册宏，零虚函数开销，
//           属性反射 API，类型 ID 系统，工厂函数注册
//
// ── 函数表 ──────────────────────────────────────────────────
// │ RuntimeHeaderGenerator        │ 主生成器结构体                  │
// │ generate_type_info_header()   │ 生成 TypeInfo.h                 │
// │ generate_property_info_header()│ 生成 PropertyInfo.h            │
// │ generate_function_info_header()│ 生成 FunctionInfo.h            │
// │ generate_type_registry_header()│ 生成 TypeRegistry.h            │
// │ generate_object_base_header() │ 生成 ObjectBase.h               │
// │ generate_all_runtime_headers()│ 生成完整运行时头文件集            │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                               │
// │ 2026-04-06   │ LimxTeam  │ 初始创建 — 完整 C++ 运行时基础设施   │
// ============================================================

use anyhow::Result;
use std::path::{Path, PathBuf};

// ──────────────────────────────────────────────────────────────
// 运行时头文件生成器
// ──────────────────────────────────────────────────────────────

/// C++ 反射运行时头文件生成器
pub struct RuntimeHeaderGenerator {
    /// 命名空间 (如 "Limx")
    pub namespace: String,
    /// 是否生成 GC 支持代码
    pub enable_gc: bool,
    /// 是否生成网络复制支持
    pub enable_replication: bool,
    /// 是否生成编辑器绑定支持
    pub enable_editor_bindings: bool,
}

impl Default for RuntimeHeaderGenerator {
    fn default() -> Self {
        Self {
            namespace: "Limx".to_string(),
            enable_gc: true,
            enable_replication: true,
            enable_editor_bindings: true,
        }
    }
}

impl RuntimeHeaderGenerator {
    pub fn new() -> Self {
        Self::default()
    }

    /// 生成所有运行时头文件到指定目录
    pub fn generate_all(&self, output_dir: &Path) -> Result<Vec<PathBuf>> {
        std::fs::create_dir_all(output_dir)?;

        let files = vec![
            ("TypeInfo.h", self.generate_type_info_header()),
            ("PropertyInfo.h", self.generate_property_info_header()),
            ("FunctionInfo.h", self.generate_function_info_header()),
            ("TypeRegistry.h", self.generate_type_registry_header()),
            ("ObjectBase.h", self.generate_object_base_header()),
            (
                "ReflectionMacros.h",
                self.generate_reflection_macros_header(),
            ),
        ];

        let mut written_paths = Vec::new();
        for (file_name, content) in files {
            let file_path = output_dir.join(file_name);
            std::fs::write(&file_path, content)?;
            written_paths.push(file_path);
        }

        Ok(written_paths)
    }

    // ──────────────────────────────────────────────────────────
    // TypeInfo.h — 核心类型信息结构
    // ──────────────────────────────────────────────────────────

    pub fn generate_type_info_header(&self) -> String {
        let ns = &self.namespace;
        format!(
            r#"// ============================================================
// 自动生成文件: TypeInfo.h
// 生成时间: 由 LHT (Limx Header Tool) 生成
// 警告: 请勿手动修改此文件
// ============================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace {ns} {{
namespace Reflection {{

// ──────────────────────────────────────────────────────────────
// 编译期字符串哈希 (FNV-1a 32bit)
// ──────────────────────────────────────────────────────────────

namespace Detail {{

constexpr uint32_t FNV_PRIME = 16777619u;
constexpr uint32_t FNV_OFFSET = 2166136261u;

constexpr uint32_t fnv1a_hash(const char* str, uint32_t hash = FNV_OFFSET) noexcept {{
    return (*str == '\0')
        ? hash
        : fnv1a_hash(str + 1, (hash ^ static_cast<uint32_t>(*str)) * FNV_PRIME);
}}

}} // namespace Detail

/// 编译期类型 ID (基于类型名称哈希)
using TypeId = uint32_t;

constexpr TypeId INVALID_TYPE_ID = 0u;

/// 编译期计算类型 ID
#define LIMX_TYPE_ID(TypeName) \
    ({ns}::Reflection::Detail::fnv1a_hash(#TypeName))

// ──────────────────────────────────────────────────────────────
// 类型类别
// ──────────────────────────────────────────────────────────────

enum class TypeKind : uint8_t {{
    Unknown     = 0,
    Primitive   = 1,   ///< bool/int/float/double 等基础类型
    Enum        = 2,   ///< 枚举 (含 enum class)
    Struct      = 3,   ///< LSTRUCT 标注的结构体
    Class       = 4,   ///< LCLASS 标注的类
    Interface   = 5,   ///< 接口类
    Delegate    = 6,   ///< LDELEGATE 委托
    Array       = 7,   ///< 数组容器
    Map         = 8,   ///< Map 容器
    Set         = 9,   ///< Set 容器
    Pointer     = 10,  ///< 原始指针
    WeakPtr     = 11,  ///< 弱引用指针
}};

// ──────────────────────────────────────────────────────────────
// 类型标志
// ──────────────────────────────────────────────────────────────

enum class TypeFlags : uint32_t {{
    None        = 0,
    Abstract    = 1u << 0,   ///< 抽象类
    Final       = 1u << 1,   ///< Final 类
    Serializable= 1u << 2,   ///< 可序列化
    BlueprintType=1u << 3,   ///< 可在蓝图中使用
    Config      = 1u << 4,   ///< 可从配置文件读取
    Transient   = 1u << 5,   ///< 不持久化
    Deprecated  = 1u << 6,   ///< 已废弃
    EngineType  = 1u << 7,   ///< 引擎内置类型
    NativeType  = 1u << 8,   ///< 原生 C++ 类型
}};

inline constexpr TypeFlags operator|(TypeFlags a, TypeFlags b) noexcept {{
    return static_cast<TypeFlags>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b)
    );
}}

inline constexpr bool has_flag(TypeFlags flags, TypeFlags flag) noexcept {{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}}

// ──────────────────────────────────────────────────────────────
// TypeInfo — 核心类型描述符
// ──────────────────────────────────────────────────────────────

class PropertyInfo;
class FunctionInfo;

/// 类型描述符 — 描述一个 C++ 类型的完整反射信息
struct TypeInfo {{
    // ── 基础信息 ──────────────────────────────────────
    TypeId          id;             ///< 类型唯一 ID (编译期哈希)
    const char*     name;           ///< 类型名称 (如 "LimxActor")
    const char*     full_name;      ///< 全限定名 (如 "Limx::Core::LimxActor")
    const char*     display_name;   ///< 显示名称 (来自 meta)
    TypeKind        kind;           ///< 类型类别
    TypeFlags       flags;          ///< 类型标志
    size_t          size;           ///< sizeof(T)
    size_t          alignment;      ///< alignof(T)

    // ── 继承关系 ──────────────────────────────────────
    const TypeInfo* super_type;     ///< 父类型 (nullptr = 无父类)
    const TypeInfo* const* interfaces; ///< 实现的接口列表
    uint16_t        interface_count;   ///< 接口数量

    // ── 属性与函数 ───────────────────────────────────
    const PropertyInfo* properties;  ///< 属性数组首地址
    uint16_t            property_count;
    const FunctionInfo* functions;   ///< 函数数组首地址
    uint16_t            function_count;

    // ── 工厂函数 ──────────────────────────────────────
    void* (*create_instance)();      ///< 默认构造工厂 (nullptr = 不可实例化)
    void  (*destroy_instance)(void*);///< 析构函数

    // ──────────────────────────────────────────────────
    // 查询 API
    // ──────────────────────────────────────────────────

    /// 检查此类型是否为 other 的子类型 (含自身)
    bool is_a(const TypeInfo* other) const noexcept {{
        const TypeInfo* t = this;
        while (t != nullptr) {{
            if (t == other) return true;
            t = t->super_type;
        }}
        return false;
    }}

    /// 通过名称查找属性
    const PropertyInfo* find_property(const char* prop_name) const noexcept;

    /// 通过名称查找函数
    const FunctionInfo* find_function(const char* func_name) const noexcept;

    /// 检查是否有指定标志
    bool has_flag(TypeFlags flag) const noexcept {{
        return {ns}::Reflection::has_flag(flags, flag);
    }}

    /// 检查是否可序列化
    bool is_serializable() const noexcept {{
        return has_flag(TypeFlags::Serializable);
    }}

    /// 是否为抽象类
    bool is_abstract() const noexcept {{
        return has_flag(TypeFlags::Abstract);
    }}
}};

}} // namespace Reflection
}} // namespace {ns}
"#,
            ns = ns
        )
    }

    // ──────────────────────────────────────────────────────────
    // PropertyInfo.h — 属性描述符
    // ──────────────────────────────────────────────────────────

    pub fn generate_property_info_header(&self) -> String {
        let ns = &self.namespace;
        format!(
            r#"// ============================================================
// 自动生成文件: PropertyInfo.h
// 生成时间: 由 LHT (Limx Header Tool) 生成
// 警告: 请勿手动修改此文件
// ============================================================
#pragma once

#include "TypeInfo.h"
#include <cstdint>

namespace {ns} {{
namespace Reflection {{

// ──────────────────────────────────────────────────────────────
// 属性标志
// ──────────────────────────────────────────────────────────────

enum class PropertyFlags : uint32_t {{
    None            = 0,
    EditAnywhere    = 1u << 0,   ///< 编辑器中可编辑
    VisibleAnywhere = 1u << 1,   ///< 编辑器中可见但只读
    BlueprintReadWrite = 1u << 2,
    BlueprintReadOnly  = 1u << 3,
    Transient       = 1u << 4,   ///< 不序列化
    Serializable    = 1u << 5,   ///< 可序列化
    Replicated      = 1u << 6,   ///< 网络复制
    SaveGame        = 1u << 7,   ///< 存档时保存
    Config          = 1u << 8,   ///< 从配置文件读取
    NoClear         = 1u << 9,   ///< 不允许在编辑器中清空
    Deprecated      = 1u << 10,  ///< 已废弃
    IsPtr           = 1u << 11,  ///< 是指针类型
    IsArray         = 1u << 12,  ///< 是数组类型
    IsOptional      = 1u << 13,  ///< 是 Optional 类型
    GCTracked       = 1u << 14,  ///< GC 追踪此属性的对象引用
}};

inline constexpr PropertyFlags operator|(PropertyFlags a, PropertyFlags b) noexcept {{
    return static_cast<PropertyFlags>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b)
    );
}}

inline constexpr bool has_flag(PropertyFlags flags, PropertyFlags flag) noexcept {{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}}

// ──────────────────────────────────────────────────────────────
// PropertyInfo — 属性描述符
// ──────────────────────────────────────────────────────────────

/// 属性描述符 — 描述一个 C++ 成员变量的完整反射信息
struct PropertyInfo {{
    // ── 基础信息 ──────────────────────────────────────
    const char*     name;           ///< 属性名称
    const char*     display_name;   ///< 显示名称
    const char*     tooltip;        ///< 鼠标提示
    const char*     category;       ///< 分类 (编辑器分组)
    PropertyFlags   flags;          ///< 属性标志
    size_t          offset;         ///< 在对象中的字节偏移量
    size_t          size;           ///< 属性大小
    const TypeInfo* property_type;  ///< 属性的类型信息

    // ── 范围约束 ──────────────────────────────────────
    double          clamp_min;      ///< 最小值 (数值类型)
    double          clamp_max;      ///< 最大值 (数值类型)

    // ── 访问器 ────────────────────────────────────────
    void* (*get_ptr)(void* object);  ///< 获取属性地址
    void  (*copy_value)(void* dest, const void* src); ///< 复制属性值

    // ──────────────────────────────────────────────────
    // 通过反射读写属性值
    // ──────────────────────────────────────────────────

    /// 获取属性的原始指针 (通过偏移量)
    void* get_raw_ptr(void* object) const noexcept {{
        return static_cast<uint8_t*>(object) + offset;
    }}

    const void* get_raw_ptr(const void* object) const noexcept {{
        return static_cast<const uint8_t*>(object) + offset;
    }}

    /// 类型安全的属性读取
    template<typename T>
    const T& get_value(const void* object) const noexcept {{
        return *static_cast<const T*>(get_raw_ptr(object));
    }}

    /// 类型安全的属性写入
    template<typename T>
    void set_value(void* object, const T& value) const noexcept {{
        *static_cast<T*>(get_raw_ptr(object)) = value;
    }}

    /// 检查属性标志
    bool has_flag(PropertyFlags flag) const noexcept {{
        return {ns}::Reflection::has_flag(flags, flag);
    }}

    bool is_editable() const noexcept {{
        return has_flag(PropertyFlags::EditAnywhere);
    }}

    bool is_serializable() const noexcept {{
        return has_flag(PropertyFlags::Serializable);
    }}

    bool is_replicated() const noexcept {{
        return has_flag(PropertyFlags::Replicated);
    }}

    bool is_gc_tracked() const noexcept {{
        return has_flag(PropertyFlags::GCTracked);
    }}
}};

}} // namespace Reflection
}} // namespace {ns}
"#,
            ns = ns
        )
    }

    // ──────────────────────────────────────────────────────────
    // FunctionInfo.h — 函数描述符
    // ──────────────────────────────────────────────────────────

    pub fn generate_function_info_header(&self) -> String {
        let ns = &self.namespace;
        format!(
            r#"// ============================================================
// 自动生成文件: FunctionInfo.h
// 生成时间: 由 LHT (Limx Header Tool) 生成
// 警告: 请勿手动修改此文件
// ============================================================
#pragma once

#include "TypeInfo.h"
#include <cstdint>

namespace {ns} {{
namespace Reflection {{

// ──────────────────────────────────────────────────────────────
// 函数标志
// ──────────────────────────────────────────────────────────────

enum class FunctionFlags : uint32_t {{
    None            = 0,
    BlueprintCallable = 1u << 0,  ///< 可在蓝图中调用
    BlueprintPure   = 1u << 1,    ///< 纯函数 (无副作用)
    CallInEditor    = 1u << 2,    ///< 可在编辑器中调用
    Server          = 1u << 3,    ///< 服务器 RPC
    Client          = 1u << 4,    ///< 客户端 RPC
    NetMulticast    = 1u << 5,    ///< 多播 RPC
    Reliable        = 1u << 6,    ///< 可靠 RPC
    Unreliable      = 1u << 7,    ///< 不可靠 RPC
    WithValidation  = 1u << 8,    ///< 带验证函数
    Static          = 1u << 9,    ///< 静态函数
    Const           = 1u << 10,   ///< const 成员函数
    Virtual         = 1u << 11,   ///< 虚函数
    Override        = 1u << 12,   ///< override 函数
    Deprecated      = 1u << 13,   ///< 已废弃
}};

inline constexpr FunctionFlags operator|(FunctionFlags a, FunctionFlags b) noexcept {{
    return static_cast<FunctionFlags>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b)
    );
}}

// ──────────────────────────────────────────────────────────────
// 参数信息
// ──────────────────────────────────────────────────────────────

/// 函数参数信息
struct ParameterInfo {{
    const char*     name;       ///< 参数名
    const TypeInfo* type;       ///< 参数类型
    bool            is_const;   ///< 是否 const
    bool            is_ref;     ///< 是否引用
    bool            is_ptr;     ///< 是否指针
    bool            has_default;///< 是否有默认值
}};

// ──────────────────────────────────────────────────────────────
// FunctionInfo — 函数描述符
// ──────────────────────────────────────────────────────────────

/// 函数描述符 — 描述一个反射函数的完整元信息
struct FunctionInfo {{
    // ── 基础信息 ──────────────────────────────────────
    const char*     name;           ///< 函数名称
    const char*     display_name;   ///< 显示名称
    const char*     tooltip;        ///< 描述
    const char*     category;       ///< 分类
    FunctionFlags   flags;          ///< 函数标志

    // ── 签名信息 ──────────────────────────────────────
    const TypeInfo*     return_type;    ///< 返回类型 (nullptr = void)
    const ParameterInfo* parameters;   ///< 参数信息数组
    uint8_t             param_count;   ///< 参数数量

    // ── 调用接口 ──────────────────────────────────────
    /// 通用调用接口 (参数以 void* 数组传递)
    void (*invoke)(void* object, void** args, void* return_value);

    // ──────────────────────────────────────────────────
    // 查询 API
    // ──────────────────────────────────────────────────

    bool has_flag(FunctionFlags flag) const noexcept {{
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
    }}

    bool is_server_rpc() const noexcept {{
        return has_flag(FunctionFlags::Server);
    }}

    bool is_client_rpc() const noexcept {{
        return has_flag(FunctionFlags::Client);
    }}

    bool is_multicast_rpc() const noexcept {{
        return has_flag(FunctionFlags::NetMulticast);
    }}

    bool is_reliable() const noexcept {{
        return has_flag(FunctionFlags::Reliable);
    }}

    bool is_static() const noexcept {{
        return has_flag(FunctionFlags::Static);
    }}

    bool is_blueprint_callable() const noexcept {{
        return has_flag(FunctionFlags::BlueprintCallable);
    }}
}};

}} // namespace Reflection
}} // namespace {ns}
"#,
            ns = ns
        )
    }

    // ──────────────────────────────────────────────────────────
    // TypeRegistry.h — 全局类型注册表
    // ──────────────────────────────────────────────────────────

    pub fn generate_type_registry_header(&self) -> String {
        let ns = &self.namespace;
        format!(
            r#"// ============================================================
// 自动生成文件: TypeRegistry.h
// 生成时间: 由 LHT (Limx Header Tool) 生成
// 警告: 请勿手动修改此文件
// ============================================================
#pragma once

#include "TypeInfo.h"
#include <cstdint>
#include <cstring>

namespace {ns} {{
namespace Reflection {{

// ──────────────────────────────────────────────────────────────
// 注册节点 (链表节点，用于静态链式注册)
// ──────────────────────────────────────────────────────────────

/// 类型注册链表节点 — 避免使用 STL 动态容器
struct TypeRegistryNode {{
    const TypeInfo*     type_info;
    TypeRegistryNode*   next;

    TypeRegistryNode(const TypeInfo* info, TypeRegistryNode*& head) noexcept
        : type_info(info), next(head)
    {{
        head = this;
    }}
}};

// ──────────────────────────────────────────────────────────────
// TypeRegistry — 全局类型注册表
// ──────────────────────────────────────────────────────────────

/// 全局类型注册表 — 线程安全，零堆分配，静态初始化
class TypeRegistry {{
public:
    /// 获取单例实例
    static TypeRegistry& get() noexcept;

    /// 注册类型 (由生成的代码调用)
    void register_type(const TypeInfo* type_info) noexcept;

    /// 通过名称查找类型
    const TypeInfo* find_by_name(const char* name) const noexcept;

    /// 通过 TypeId 查找类型
    const TypeInfo* find_by_id(TypeId id) const noexcept;

    /// 遍历所有已注册类型
    template<typename Fn>
    void for_each(Fn&& fn) const noexcept {{
        TypeRegistryNode* node = head_;
        while (node != nullptr) {{
            fn(node->type_info);
            node = node->next;
        }}
    }}

    /// 已注册类型数量
    uint32_t type_count() const noexcept {{ return count_; }}

    /// 检查类型是否已注册
    bool is_registered(TypeId id) const noexcept {{
        return find_by_id(id) != nullptr;
    }}

    /// 根据类型信息创建实例
    void* create_instance(TypeId id) const;

    /// 销毁实例
    void destroy_instance(TypeId id, void* instance) const;

    /// 获取所有实现了指定接口的类型
    /// (调用者负责管理结果缓冲区)
    uint32_t find_implementors(
        const TypeInfo* interface_type,
        const TypeInfo** out_buffer,
        uint32_t        buffer_capacity
    ) const noexcept;

private:
    TypeRegistry() = default;
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    TypeRegistryNode*   head_   = nullptr;
    uint32_t            count_  = 0;
}};

// ──────────────────────────────────────────────────────────────
// 自动注册助手宏 (由 LHT 生成的代码调用)
// ──────────────────────────────────────────────────────────────

/// 类型自动注册器 — 利用静态初始化顺序
struct AutoTypeRegistrar {{
    AutoTypeRegistrar(const TypeInfo* type_info) noexcept {{
        TypeRegistry::get().register_type(type_info);
    }}
}};

/// 在匿名命名空间中声明自动注册对象
#define LIMX_IMPL_REGISTER_TYPE(TypeClass)                          \
    namespace {{                                                     \
        static {ns}::Reflection::AutoTypeRegistrar                  \
            LIMX_CONCAT(s_auto_register_, TypeClass)(               \
                TypeClass::StaticTypeInfo()                         \
            );                                                       \
    }}

#define LIMX_CONCAT_IMPL(a, b) a##b
#define LIMX_CONCAT(a, b) LIMX_CONCAT_IMPL(a, b)

}} // namespace Reflection
}} // namespace {ns}
"#,
            ns = ns
        )
    }

    // ──────────────────────────────────────────────────────────
    // ObjectBase.h — 所有反射对象的基类
    // ──────────────────────────────────────────────────────────

    pub fn generate_object_base_header(&self) -> String {
        let ns = &self.namespace;
        let gc_code = if self.enable_gc {
            format!(
                r#"
    // ── GC 接口 (由 LHT 生成的子类重写) ─────────────────
    /// 遍历此对象持有的所有 GC 追踪引用
    virtual void gc_trace_references(void* gc_visitor) noexcept {{ (void)gc_visitor; }}

    /// 对象是否存活 (GC 标记位)
    bool gc_is_marked() const noexcept {{ return (flags_ & kGcMarked) != 0; }}

    /// 设置 GC 标记
    void gc_mark() noexcept {{ flags_ |= kGcMarked; }}

    /// 清除 GC 标记
    void gc_unmark() noexcept {{ flags_ &= ~kGcMarked; }}
"#
            )
        } else {
            String::new()
        };

        let replication_code = if self.enable_replication {
            r#"
    // ── 网络复制接口 (由 LHT 生成的子类重写) ──────────────
    /// 序列化需要复制的属性
    virtual void replicate_properties(void* archive) noexcept { (void)archive; }

    /// 接收到复制数据时调用
    virtual void on_replicated() noexcept {}
"#
            .to_string()
        } else {
            String::new()
        };

        format!(
            r#"// ============================================================
// 自动生成文件: ObjectBase.h
// 生成时间: 由 LHT (Limx Header Tool) 生成
// 警告: 请勿手动修改此文件
// ============================================================
#pragma once

#include "TypeRegistry.h"
#include "PropertyInfo.h"
#include "FunctionInfo.h"
#include <cstdint>

namespace {ns} {{

// ──────────────────────────────────────────────────────────────
// ObjectBase — 所有 LCLASS 标注类的基类
// ──────────────────────────────────────────────────────────────

/// 所有反射对象的基类 — 轻量级，无虚表开销的核心功能
/// 使用 CRTP 模式由生成代码扩展
class ObjectBase {{
public:
    virtual ~ObjectBase() = default;

    // ── 类型查询 API ──────────────────────────────────
    /// 获取运行时类型信息
    virtual const Reflection::TypeInfo* get_type_info() const noexcept = 0;

    /// 获取类型名称
    const char* get_type_name() const noexcept {{
        return get_type_info()->name;
    }}

    /// 检查是否为指定类型或其子类
    bool is_a(const Reflection::TypeInfo* type) const noexcept {{
        return get_type_info()->is_a(type);
    }}

    template<typename T>
    bool is_a() const noexcept {{
        return is_a(T::StaticTypeInfo());
    }}

    /// 安全类型转换 (失败返回 nullptr)
    template<typename T>
    T* cast_to() noexcept {{
        return is_a<T>() ? static_cast<T*>(this) : nullptr;
    }}

    template<typename T>
    const T* cast_to() const noexcept {{
        return is_a<T>() ? static_cast<const T*>(this) : nullptr;
    }}

    // ── 对象 ID ──────────────────────────────────────
    /// 获取对象唯一 ID
    uint64_t get_object_id() const noexcept {{ return object_id_; }}
{gc_code}{replication_code}
protected:
    /// 分配唯一对象 ID (子类构造函数中调用)
    void init_object_id() noexcept;

private:
    static constexpr uint32_t kGcMarked = 1u << 0;

    uint64_t object_id_ = 0;
    uint32_t flags_     = 0;
}};

// ──────────────────────────────────────────────────────────────
// LGENERATED_BODY 宏展开模板
// ──────────────────────────────────────────────────────────────

/// 由 LGENERATED_BODY 宏插入到每个 LCLASS 类体中
/// TypeClass: 当前类名, ParentClass: 父类名
#define LIMX_GENERATED_BODY_IMPL(TypeClass, ParentClass)            \
public:                                                              \
    using ThisClass   = TypeClass;                                   \
    using Super       = ParentClass;                                 \
    static const {ns}::Reflection::TypeInfo* StaticTypeInfo()        \
        noexcept;                                                    \
    const {ns}::Reflection::TypeInfo* get_type_info()                \
        const noexcept override                                      \
    {{                                                                \
        return StaticTypeInfo();                                     \
    }}                                                               \
    static TypeClass* StaticCreate() noexcept;                      \
private:

}} // namespace {ns}
"#,
            ns = ns,
            gc_code = gc_code,
            replication_code = replication_code,
        )
    }

    // ──────────────────────────────────────────────────────────
    // ReflectionMacros.h — 全套反射宏定义
    // ──────────────────────────────────────────────────────────

    pub fn generate_reflection_macros_header(&self) -> String {
        let ns = &self.namespace;
        format!(
            r#"// ============================================================
// 自动生成文件: ReflectionMacros.h
// 生成时间: 由 LHT (Limx Header Tool) 生成
// 警告: 请勿手动修改此文件
// ============================================================
#pragma once

// ──────────────────────────────────────────────────────────────
// 反射宏 — 在头文件中使用
// ──────────────────────────────────────────────────────────────

/// 标注类为反射类
/// 用法: LCLASS(Specifiers...) class MyClass : public ObjectBase {{ ... }};
#define LCLASS(...)

/// 标注结构体为反射结构体
#define LSTRUCT(...)

/// 标注枚举为反射枚举
#define LENUM(...)

/// 标注属性为反射属性
#define LPROPERTY(...)

/// 标注函数为反射函数
#define LFUNCTION(...)

/// 标注委托
#define LDELEGATE(...)

/// 必须放在 LCLASS/LSTRUCT 类体第一行
/// 由 LHT 生成的 .generated.h 中展开为实际代码
#define LGENERATED_BODY() LIMX_GENERATED_BODY_PLACEHOLDER

/// 元数据标注
#define LMETA(...)

// ──────────────────────────────────────────────────────────────
// 辅助宏
// ──────────────────────────────────────────────────────────────

/// 获取类型的静态 TypeInfo
#define LIMX_TYPEOF(TypeClass) \
    (TypeClass::StaticTypeInfo())

/// 安全类型转换
#define LIMX_CAST(TypeClass, Ptr) \
    ((Ptr) ? (Ptr)->cast_to<TypeClass>() : nullptr)

/// 断言类型转换 (调试模式下崩溃)
#define LIMX_CHECKED_CAST(TypeClass, Ptr) \
    ([&]() -> TypeClass* {{                \
        auto* result = (Ptr)->cast_to<TypeClass>(); \
        LIMX_ASSERT(result != nullptr, "类型转换失败"); \
        return result;                    \
    }}())

/// 类型 ID 比较
#define LIMX_IS_A(Ptr, TypeClass) \
    ((Ptr) && (Ptr)->is_a<TypeClass>())
"#
        )
    }
}

// ──────────────────────────────────────────────────────────────
// 便捷生成函数
// ──────────────────────────────────────────────────────────────

/// 生成完整的 C++ 反射运行时头文件集
/// 输出到 output_dir/Runtime/ 子目录
pub fn generate_all_runtime_headers(output_dir: &Path) -> Result<Vec<PathBuf>> {
    let runtime_dir = output_dir.join("Runtime");
    let generator = RuntimeHeaderGenerator::default();
    generator.generate_all(&runtime_dir)
}

// ──────────────────────────────────────────────────────────────
// 单元测试
// ──────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_type_info_header_contains_key_elements() {
        let gen = RuntimeHeaderGenerator::default();
        let header = gen.generate_type_info_header();
        assert!(header.contains("TypeId"));
        assert!(header.contains("TypeInfo"));
        assert!(header.contains("TypeKind"));
        assert!(header.contains("TypeFlags"));
        assert!(header.contains("fnv1a_hash"));
        assert!(header.contains("is_a("));
        assert!(header.contains("find_property("));
    }

    #[test]
    fn test_property_info_header_contains_key_elements() {
        let gen = RuntimeHeaderGenerator::default();
        let header = gen.generate_property_info_header();
        assert!(header.contains("PropertyInfo"));
        assert!(header.contains("PropertyFlags"));
        assert!(header.contains("offset"));
        assert!(header.contains("get_raw_ptr"));
        assert!(header.contains("set_value"));
        assert!(header.contains("Replicated"));
    }

    #[test]
    fn test_function_info_header_contains_rpc_flags() {
        let gen = RuntimeHeaderGenerator::default();
        let header = gen.generate_function_info_header();
        assert!(header.contains("Server"));
        assert!(header.contains("Client"));
        assert!(header.contains("NetMulticast"));
        assert!(header.contains("Reliable"));
        assert!(header.contains("is_server_rpc"));
    }

    #[test]
    fn test_object_base_with_gc() {
        let gen = RuntimeHeaderGenerator {
            enable_gc: true,
            ..Default::default()
        };
        let header = gen.generate_object_base_header();
        assert!(header.contains("gc_trace_references"));
        assert!(header.contains("gc_is_marked"));
        assert!(header.contains("gc_mark"));
    }

    #[test]
    fn test_object_base_with_replication() {
        let gen = RuntimeHeaderGenerator {
            enable_replication: true,
            ..Default::default()
        };
        let header = gen.generate_object_base_header();
        assert!(header.contains("replicate_properties"));
        assert!(header.contains("on_replicated"));
    }

    #[test]
    fn test_type_registry_header_contains_singleton() {
        let gen = RuntimeHeaderGenerator::default();
        let header = gen.generate_type_registry_header();
        assert!(header.contains("TypeRegistry"));
        assert!(header.contains("find_by_name"));
        assert!(header.contains("find_by_id"));
        assert!(header.contains("AutoTypeRegistrar"));
        assert!(header.contains("LIMX_IMPL_REGISTER_TYPE"));
    }

    #[test]
    fn test_reflection_macros_header_contains_all_macros() {
        let gen = RuntimeHeaderGenerator::default();
        let header = gen.generate_reflection_macros_header();
        assert!(header.contains("#define LCLASS"));
        assert!(header.contains("#define LSTRUCT"));
        assert!(header.contains("#define LENUM"));
        assert!(header.contains("#define LPROPERTY"));
        assert!(header.contains("#define LFUNCTION"));
        assert!(header.contains("#define LDELEGATE"));
        assert!(header.contains("#define LGENERATED_BODY"));
    }
}
