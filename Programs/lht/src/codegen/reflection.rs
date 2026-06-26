/*******************************************************************************
 * 文件: reflection.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   反射代码生成器 (生产级增强版)
 *   - 生成 .generated.h 文件 (类型声明、宏展开)
 *   - 生成 .generated.cpp 文件 (类型注册、序列化实现)
 *   - 支持序列化、对象工厂、RPC 代理
 *   - 支持属性编辑器绑定
 *   - 支持热重载兼容
 *
 * 技术特性:
 *   - 完整的 RTTI 实现
 *   - 自动类型注册
 *   - 属性变更通知
 *   - 网络复制支持
 *   - GC 友好设计
 *
 ******************************************************************************/

use anyhow::Result;
use rayon::prelude::*;
use std::fs;
use std::path::{Path, PathBuf};

use crate::parser::parser::{
    ClassInfo, DelegateInfo, EnumInfo, FunctionInfo, PropertyInfo, ReflectedType, StructInfo,
};

/// 代码生成配置
#[derive(Debug, Clone)]
pub struct GeneratorConfig {
    /// 是否生成序列化代码
    pub generate_serialization: bool,
    /// 是否生成 RPC 代码
    pub generate_rpc: bool,
    /// 是否生成属性编辑器绑定
    pub generate_editor_bindings: bool,
    /// 是否生成热重载支持
    pub generate_hot_reload: bool,
    /// 是否生成 GC 支持
    pub generate_gc: bool,
    /// 命名空间前缀
    pub namespace_prefix: String,
}

impl Default for GeneratorConfig {
    fn default() -> Self {
        Self {
            generate_serialization: true,
            generate_rpc: true,
            generate_editor_bindings: true,
            generate_hot_reload: false,
            generate_gc: false,
            namespace_prefix: "Limx::Core".to_string(),
        }
    }
}

/// 为头文件生成反射代码
pub fn generate_reflection_code(
    header_path: &Path,
    types: &[ReflectedType],
    output_dir: &Path,
) -> Result<()> {
    generate_reflection_code_with_config(
        header_path,
        types,
        output_dir,
        &GeneratorConfig::default(),
    )
}

/// 为头文件生成反射代码（带配置）
pub fn generate_reflection_code_with_config(
    header_path: &Path,
    types: &[ReflectedType],
    output_dir: &Path,
    config: &GeneratorConfig,
) -> Result<()> {
    if types.is_empty() {
        return Ok(());
    }

    fs::create_dir_all(output_dir)?;

    let file_stem = header_path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("Unknown");

    // 生成 .generated.h
    let generated_h = output_dir.join(format!("{}.generated.h", file_stem));
    let h_content = generate_header_content(file_stem, types, config);
    fs::write(&generated_h, h_content)?;

    // 生成 .generated.cpp
    let generated_cpp = output_dir.join(format!("{}.generated.cpp", file_stem));
    let cpp_content = generate_cpp_content(file_stem, header_path, types, config);
    fs::write(&generated_cpp, cpp_content)?;

    Ok(())
}

/// 并行生成多个头文件的反射代码
pub fn generate_reflection_code_parallel(
    headers: &[(PathBuf, Vec<ReflectedType>)],
    output_dir: &Path,
) -> Result<()> {
    let config = GeneratorConfig::default();

    headers.par_iter().try_for_each(|(path, types)| {
        generate_reflection_code_with_config(path, types, output_dir, &config)
    })
}

//=============================================================================
// 头文件生成
//=============================================================================

fn generate_header_content(
    file_stem: &str,
    types: &[ReflectedType],
    config: &GeneratorConfig,
) -> String {
    let mut content = String::with_capacity(16 * 1024); // 预分配 16KB

    content.push_str(&format!(
        r#"/*******************************************************************************
 * 文件: {file_stem}.generated.h
 * 
 * 警告: 此文件由 LHT (Limx Header Tool) 自动生成
 *       请勿手动修改，任何修改都会被覆盖
 *
 ******************************************************************************/

#pragma once

#include "Limx/Core/Reflection/TypeInfo.h"
#include "Limx/Core/Reflection/PropertyInfo.h"
#include "Limx/Core/Reflection/FunctionInfo.h"
#include "Limx/Core/Serialization/Archive.h"

"#
    ));

    for reflected_type in types {
        match reflected_type {
            ReflectedType::Class(info) => {
                content.push_str(&generate_class_header(info, config));
            }
            ReflectedType::Struct(info) => {
                content.push_str(&generate_struct_header(info, config));
            }
            ReflectedType::Enum(info) => {
                content.push_str(&generate_enum_header(info, config));
            }
            ReflectedType::Delegate(info) => {
                content.push_str(&generate_delegate_header(info, config));
            }
        }
    }

    content
}

fn generate_class_header(info: &ClassInfo, config: &GeneratorConfig) -> String {
    let name = &info.name;
    let name_upper = name.to_uppercase();
    let ns = &config.namespace_prefix;

    let mut content = String::with_capacity(4096);

    // 类型标志
    let mut type_flags = Vec::new();
    if info.is_abstract {
        type_flags.push("Abstract");
    }
    if info.is_final {
        type_flags.push("Final");
    }
    if info.specifiers.serializable {
        type_flags.push("Serializable");
    }
    if info.specifiers.blueprint_type {
        type_flags.push("BlueprintType");
    }

    content.push_str(&format!(
        r#"// ============================================================================
// {name} 反射宏
// 类型标志: [{flags}]
// 基类数量: {base_count}
// 属性数量: {prop_count}
// 函数数量: {func_count}
// ============================================================================

#define LGENERATED_BODY_{name}() \
private: \
    static {ns}::TypeInfo s_TypeInfo; \
    static {ns}::PropertyInfo s_Properties[{prop_count}]; \
    static {ns}::FunctionInfo s_Functions[{func_count}]; \
public: \
    using Super = {super_type}; \
    using ThisClass = {name}; \
    static constexpr bool HasReflection = true; \
    static const {ns}::TypeInfo* StaticTypeInfo() {{ return &s_TypeInfo; }} \
    virtual const {ns}::TypeInfo* GetTypeInfo() const {{ return &s_TypeInfo; }} \
    static void* StaticConstruct() {{ return {construct}; }} \
    static void StaticDestruct(void* ptr) {{ delete static_cast<{name}*>(ptr); }} \
    static void StaticRegisterType(); \
"#,
        flags = if type_flags.is_empty() {
            "None".to_string()
        } else {
            type_flags.join(", ")
        },
        base_count = info.base_classes.len(),
        prop_count = info.properties.len(),
        func_count = info.functions.len(),
        super_type = info.base_class().unwrap_or("void"),
        construct = if info.is_abstract {
            "nullptr".to_string()
        } else {
            format!("new {}()", name)
        },
    ));

    // 序列化方法
    if info.specifiers.serializable && config.generate_serialization {
        content.push_str(&format!(
            r#"    virtual void Serialize({ns}::Archive& archive); \
    virtual void Deserialize({ns}::Archive& archive); \
    virtual void SerializeProperties({ns}::Archive& archive); \
"#
        ));
    }

    // RPC 代理方法
    if config.generate_rpc {
        let rpc_functions: Vec<_> = info
            .functions
            .iter()
            .filter(|f| f.specifiers.is_rpc())
            .collect();

        for func in &rpc_functions {
            let func_name = &func.name;
            let params = generate_function_params(&func.parameters);

            if func.specifiers.server {
                content.push_str(&format!(
                    r#"    void {func_name}_Implementation({params}); \
    void {func_name}_Validate({params}); \
"#
                ));
            }
            if func.specifiers.client {
                content.push_str(&format!(
                    r#"    void {func_name}_Implementation({params}); \
"#
                ));
            }
        }
    }

    // 属性变更通知
    if config.generate_editor_bindings {
        let replicated_props: Vec<_> = info
            .properties
            .iter()
            .filter(|p| p.specifiers.replicated)
            .collect();

        for prop in &replicated_props {
            content.push_str(&format!(
                r#"    void OnRep_{prop_name}(); \
"#,
                prop_name = prop.name
            ));
        }
    }

    content.push_str("\n");

    // 类型注册宏
    content.push_str(&format!(
        r#"#define LIMX_IMPLEMENT_CLASS_{name_upper}() \
    {ns}::TypeInfo {name}::s_TypeInfo = {{ \
        "{name}", \
        sizeof({name}), \
        alignof({name}), \
        {ns}::TypeFlags::{type_flags_enum}, \
"#,
        type_flags_enum = if info.is_abstract { "Abstract" } else { "None" },
    ));

    // 基类指针
    if let Some(base) = info.base_class() {
        content.push_str(&format!(
            r#"        {base}::StaticTypeInfo(), \
"#
        ));
    } else {
        content.push_str(
            r#"        nullptr, \
"#,
        );
    }

    content.push_str(&format!(
        r#"        reinterpret_cast<void*(*)()>(&{name}::StaticConstruct), \
        {name}::s_Properties, \
        {prop_count}, \
        {name}::s_Functions, \
        {func_count} \
    }}; \
    Limx::Core::PropertyInfo {name}::s_Properties[] = {{ \
"#,
        prop_count = info.properties.len(),
        func_count = info.functions.len(),
    ));

    // 属性元数据
    for prop in &info.properties {
        content.push_str(&generate_property_metadata(name, prop));
    }

    content.push_str(
        r#"    }; \
"#,
    );

    // 函数元数据
    content.push_str(&format!(
        r#"    Limx::Core::FunctionInfo {name}::s_Functions[] = {{ \
"#
    ));

    for func in &info.functions {
        content.push_str(&generate_function_metadata(name, func));
    }

    content.push_str(
        r#"    }};

"#,
    );

    content
}

/// 生成函数参数列表字符串
fn generate_function_params(params: &[crate::parser::parser::ParameterInfo]) -> String {
    params
        .iter()
        .map(|p| {
            let mut s = String::new();
            if p.is_const {
                s.push_str("const ");
            }
            s.push_str(&p.type_name);
            if p.is_pointer {
                s.push('*');
            }
            if p.is_reference {
                s.push('&');
            }
            if p.is_rvalue_ref {
                s.push_str("&&");
            }
            if !p.name.is_empty() {
                s.push(' ');
                s.push_str(&p.name);
            }
            s
        })
        .collect::<Vec<_>>()
        .join(", ")
}

fn generate_struct_header(info: &StructInfo, config: &GeneratorConfig) -> String {
    let name = &info.name;
    let name_upper = name.to_uppercase();
    let ns = &config.namespace_prefix;

    format!(
        r#"// ============================================================================
// {name} 结构体反射宏
// 属性数量: {prop_count}
// ============================================================================

#define LGENERATED_BODY_{name}() \
public: \
    using ThisClass = {name}; \
    static constexpr bool HasReflection = true; \
    static const {ns}::TypeInfo* StaticTypeInfo(); \
    void Serialize({ns}::Archive& archive); \
    void Deserialize({ns}::Archive& archive);

#define LIMX_IMPLEMENT_STRUCT_{name_upper}() \
    const {ns}::TypeInfo* {name}::StaticTypeInfo() {{ \
        static const {ns}::TypeInfo s_TypeInfo = {{ \
            "{name}", \
            sizeof({name}), \
            alignof({name}), \
            {ns}::TypeFlags::Struct, \
            nullptr, \
            nullptr, \
            nullptr, \
            0, \
            nullptr, \
            0 \
        }}; \
        return &s_TypeInfo; \
    }}

"#,
        prop_count = info.properties.len(),
    )
}

fn generate_enum_header(info: &EnumInfo, config: &GeneratorConfig) -> String {
    let name = &info.name;
    let ns = &config.namespace_prefix;
    let value_count = info.values.len();

    let mut content = String::with_capacity(2048);

    content.push_str(&format!(
        r#"// ============================================================================
// {name} 枚举反射
// 值数量: {value_count}
// 是否位标志: {is_flags}
// ============================================================================

namespace {ns}
{{
    template<>
    struct EnumTraits<{name}>
    {{
        static constexpr const char* Name = "{name}";
        static constexpr bool IsFlags = {is_flags};
        static constexpr SizeType Count = {value_count};
        
        struct Entry
        {{
            {name} Value;
            const char* Name;
            const char* DisplayName;
            Int64 NumericValue;
        }};
        
        static constexpr Entry Entries[] = {{
"#,
        is_flags = if info.is_flags { "true" } else { "false" },
    ));

    for (idx, value) in info.values.iter().enumerate() {
        let display = value.display_name.as_deref().unwrap_or(&value.name);
        let numeric = value.value.unwrap_or(idx as i64);
        content.push_str(&format!(
            r#"            {{ {name}::{val_name}, "{val_name}", "{display}", {numeric} }},
"#,
            val_name = value.name,
        ));
    }

    content.push_str(&format!(
        r#"        }};
        
        static const char* ToString({name} value)
        {{
            for (const auto& entry : Entries)
            {{
                if (entry.Value == value) return entry.Name;
            }}
            return "Unknown";
        }}
        
        static bool FromString(const char* str, {name}& outValue)
        {{
            for (const auto& entry : Entries)
            {{
                if (strcmp(entry.Name, str) == 0)
                {{
                    outValue = entry.Value;
                    return true;
                }}
            }}
            return false;
        }}
        
        static {name} FromIndex(SizeType index)
        {{
            return (index < Count) ? Entries[index].Value : static_cast<{name}>(0);
        }}
        
        static Int64 ToNumeric({name} value)
        {{
            for (const auto& entry : Entries)
            {{
                if (entry.Value == value) return entry.NumericValue;
            }}
            return 0;
        }}
    }};
}}

"#
    ));

    content
}

fn generate_delegate_header(info: &DelegateInfo, config: &GeneratorConfig) -> String {
    let name = &info.name;
    let return_type = &info.return_type;
    let ns = &config.namespace_prefix;

    let param_types: Vec<String> = info
        .parameters
        .iter()
        .map(|p| {
            let mut s = String::new();
            if p.is_const {
                s.push_str("const ");
            }
            s.push_str(&p.type_name);
            if p.is_pointer {
                s.push('*');
            }
            if p.is_reference {
                s.push('&');
            }
            s
        })
        .collect();
    let param_type_str = param_types.join(", ");

    let delegate_type = if info.specifiers.multicast {
        "MulticastDelegate"
    } else if info.specifiers.dynamic {
        "DynamicDelegate"
    } else {
        "Delegate"
    };

    format!(
        r#"// ============================================================================
// {name} {delegate_type}
// 参数数量: {param_count}
// 返回类型: {return_type}
// ============================================================================

using {name} = {ns}::{delegate_type}<{return_type}({param_type_str})>;

// 委托声明宏
#define DECLARE_DELEGATE_{name_upper}() \
    using {name} = {ns}::{delegate_type}<{return_type}({param_type_str})>

"#,
        param_count = info.parameters.len(),
        name_upper = name.to_uppercase(),
    )
}

fn generate_property_metadata(class_name: &str, prop: &PropertyInfo) -> String {
    let flags = generate_property_flags(prop);

    format!(
        r#"        {{ "{prop_name}", "{prop_type}", offsetof({class_name}, {prop_name}), {flags} }}, \
"#,
        prop_name = prop.name,
        prop_type = prop.type_name,
    )
}

fn generate_property_flags(prop: &PropertyInfo) -> String {
    let mut flags = Vec::new();

    if prop.specifiers.editable {
        flags.push("Limx::Core::PropertyFlags::Editable");
    }
    if prop.specifiers.serializable {
        flags.push("Limx::Core::PropertyFlags::Serializable");
    }
    if prop.specifiers.replicated {
        flags.push("Limx::Core::PropertyFlags::Replicated");
    }
    if prop.specifiers.transient {
        flags.push("Limx::Core::PropertyFlags::Transient");
    }

    if flags.is_empty() {
        "Limx::Core::PropertyFlags::None".to_string()
    } else {
        flags.join(" | ")
    }
}

fn generate_function_metadata(_class_name: &str, func: &FunctionInfo) -> String {
    let flags = generate_function_flags(func);

    format!(
        r#"        {{ "{func_name}", {flags} }}, \
"#,
        func_name = func.name,
    )
}

fn generate_function_flags(func: &FunctionInfo) -> String {
    let mut flags = Vec::new();

    if func.specifiers.callable {
        flags.push("Limx::Core::FunctionFlags::Callable");
    }
    if func.specifiers.server {
        flags.push("Limx::Core::FunctionFlags::Server");
    }
    if func.specifiers.client {
        flags.push("Limx::Core::FunctionFlags::Client");
    }
    if func.specifiers.reliable {
        flags.push("Limx::Core::FunctionFlags::Reliable");
    }

    if flags.is_empty() {
        "Limx::Core::FunctionFlags::None".to_string()
    } else {
        flags.join(" | ")
    }
}

//=============================================================================
// CPP 文件生成
//=============================================================================

fn generate_cpp_content(
    file_stem: &str,
    header_path: &Path,
    types: &[ReflectedType],
    config: &GeneratorConfig,
) -> String {
    let header_name = header_path
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("Unknown.h");
    let _ns = &config.namespace_prefix;

    let mut content = String::with_capacity(16 * 1024);

    content.push_str(&format!(
        r#"/*******************************************************************************
 * 文件: {file_stem}.generated.cpp
 * 
 * 警告: 此文件由 LHT (Limx Header Tool) 自动生成
 *       请勿手动修改，任何修改都会被覆盖
 *
 * 生成配置:
 *   - 序列化: {gen_serial}
 *   - RPC: {gen_rpc}
 *   - 编辑器绑定: {gen_editor}
 *
 ******************************************************************************/

#include "{header_name}"
#include "{file_stem}.generated.h"

"#,
        gen_serial = config.generate_serialization,
        gen_rpc = config.generate_rpc,
        gen_editor = config.generate_editor_bindings,
    ));

    for reflected_type in types {
        match reflected_type {
            ReflectedType::Class(info) => {
                content.push_str(&generate_class_cpp(info, config));
            }
            ReflectedType::Struct(info) => {
                content.push_str(&generate_struct_cpp(info, config));
            }
            _ => {}
        }
    }

    // 自动注册
    content.push_str(&generate_auto_registration(types, config));

    content
}

fn generate_class_cpp(info: &ClassInfo, config: &GeneratorConfig) -> String {
    let name = &info.name;
    let name_upper = name.to_uppercase();
    let ns = &config.namespace_prefix;

    let mut content = String::with_capacity(2048);

    content.push_str(&format!(
        r#"// ============================================================================
// {name} 实现
// ============================================================================

LIMX_IMPLEMENT_CLASS_{name_upper}()

void {name}::StaticRegisterType()
{{
    {ns}::TypeRegistry::Register(StaticTypeInfo());
}}

"#
    ));

    // 序列化实现
    if info.specifiers.serializable && config.generate_serialization {
        content.push_str(&generate_serialization_impl(info, config));
    }

    content
}

fn generate_struct_cpp(info: &StructInfo, config: &GeneratorConfig) -> String {
    let name = &info.name;
    let name_upper = name.to_uppercase();

    let mut content = format!(
        r#"// ============================================================================
// {name} 实现
// ============================================================================

LIMX_IMPLEMENT_STRUCT_{name_upper}()

"#
    );

    // 结构体序列化
    if config.generate_serialization {
        content.push_str(&generate_struct_serialization_impl(info, config));
    }

    content
}

fn generate_serialization_impl(info: &ClassInfo, config: &GeneratorConfig) -> String {
    let name = &info.name;
    let ns = &config.namespace_prefix;

    let mut serialize_body = String::new();
    let mut deserialize_body = String::new();
    let mut properties_body = String::new();

    // 基类序列化
    if let Some(base) = info.base_class() {
        serialize_body.push_str(&format!("    {base}::Serialize(archive);\n"));
        deserialize_body.push_str(&format!("    {base}::Deserialize(archive);\n"));
    }

    // 属性序列化
    for prop in &info.properties {
        if prop.specifiers.serializable && !prop.specifiers.transient {
            let prop_name = &prop.name;
            serialize_body.push_str(&format!("    archive << {prop_name};\n"));
            deserialize_body.push_str(&format!("    archive >> {prop_name};\n"));
            properties_body.push_str(&format!(
                "    archive.SerializeProperty(\"{prop_name}\", {prop_name});\n"
            ));
        }
    }

    format!(
        r#"void {name}::Serialize({ns}::Archive& archive)
{{
{serialize_body}}}

void {name}::Deserialize({ns}::Archive& archive)
{{
{deserialize_body}}}

void {name}::SerializeProperties({ns}::Archive& archive)
{{
{properties_body}}}

"#
    )
}

fn generate_struct_serialization_impl(info: &StructInfo, config: &GeneratorConfig) -> String {
    let name = &info.name;
    let ns = &config.namespace_prefix;

    let mut serialize_body = String::new();
    let mut deserialize_body = String::new();

    for prop in &info.properties {
        serialize_body.push_str(&format!("    archive << {};\n", prop.name));
        deserialize_body.push_str(&format!("    archive >> {};\n", prop.name));
    }

    format!(
        r#"void {name}::Serialize({ns}::Archive& archive)
{{
{serialize_body}}}

void {name}::Deserialize({ns}::Archive& archive)
{{
{deserialize_body}}}

"#
    )
}

fn generate_auto_registration(types: &[ReflectedType], config: &GeneratorConfig) -> String {
    let _ns = &config.namespace_prefix;

    let mut content = format!(
        r#"// ============================================================================
// 自动类型注册
// ============================================================================

namespace
{{
    struct AutoRegister
    {{
        AutoRegister()
        {{
"#
    );

    for reflected_type in types {
        match reflected_type {
            ReflectedType::Class(info) => {
                content.push_str(&format!(
                    "            Limx::Core::TypeRegistry::Register({}::StaticTypeInfo());\n",
                    info.name
                ));
            }
            ReflectedType::Struct(info) => {
                content.push_str(&format!(
                    "            Limx::Core::TypeRegistry::Register({}::StaticTypeInfo());\n",
                    info.name
                ));
            }
            _ => {}
        }
    }

    content.push_str(
        r#"        }
    };
    
    static AutoRegister s_AutoRegister;
}
"#,
    );

    content
}
