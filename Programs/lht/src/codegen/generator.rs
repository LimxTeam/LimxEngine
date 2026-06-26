/*******************************************************************************
 * 文件: generator.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   反射代码生成器 (生产级)
 *   - 生成 .generated.h 文件 (LGENERATED_BODY 宏展开)
 *   - 生成 .generated.cpp 文件 (类型注册、序列化)
 *   - 支持完整的 UE 风格反射
 *
 * 设计哲学:
 *   生成的代码应该:
 *   1. 无需手动修改即可编译
 *   2. 支持所有反射功能
 *   3. 高效的运行时性能
 *
 ******************************************************************************/

use anyhow::Result;
use std::fs;
use std::path::Path;

use crate::parser::ast::*;
use crate::parser::specifiers::Specifiers;

/// 代码生成器
pub struct CodeGenerator {
    /// 模块名
    module_name: String,
    /// API 宏
    api_macro: String,
}

impl CodeGenerator {
    pub fn new(module_name: &str, api_macro: &str) -> Self {
        Self {
            module_name: module_name.to_string(),
            api_macro: api_macro.to_string(),
        }
    }

    /// 为翻译单元生成代码
    pub fn generate(&self, unit: &TranslationUnit, output_dir: &Path) -> Result<GeneratedFiles> {
        let file_stem = Path::new(&unit.file_path)
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("Unknown");

        let classes = unit.get_reflected_classes();
        let enums = unit.get_reflected_enums();
        let delegates = unit.get_delegates();

        if classes.is_empty() && enums.is_empty() && delegates.is_empty() {
            return Ok(GeneratedFiles::empty());
        }

        fs::create_dir_all(output_dir)?;

        // 生成 .generated.h
        let header_content = self.generate_header(file_stem, &classes, &enums, &delegates);
        let header_path = output_dir.join(format!("{}.generated.h", file_stem));
        fs::write(&header_path, &header_content)?;

        // 生成 .generated.cpp
        let cpp_content = self.generate_cpp(file_stem, &unit.file_path, &classes, &enums);
        let cpp_path = output_dir.join(format!("{}.generated.cpp", file_stem));
        fs::write(&cpp_path, &cpp_content)?;

        Ok(GeneratedFiles {
            header: header_path.to_string_lossy().to_string(),
            source: cpp_path.to_string_lossy().to_string(),
            classes: classes.iter().map(|c| c.name.clone()).collect(),
            enums: enums.iter().map(|e| e.name.clone()).collect(),
        })
    }

    /// 生成头文件
    fn generate_header(
        &self,
        file_stem: &str,
        classes: &[&ClassDecl],
        enums: &[&EnumDecl],
        delegates: &[&DelegateDecl],
    ) -> String {
        let mut out = String::new();

        // 文件头
        out.push_str(&format!(
            r#"/*******************************************************************************
 * 文件: {file_stem}.generated.h
 * 
 * 警告: 此文件由 LHT (Limx Header Tool) 自动生成
 *       请勿手动修改，任何修改都会被覆盖
 *
 * 模块: {module}
 *
 ******************************************************************************/

#pragma once

#include "Limx/Core/Reflection.h"

"#,
            module = self.module_name
        ));

        // 为每个类生成 LGENERATED_BODY 宏
        for class in classes {
            out.push_str(&self.generate_class_macros(class));
        }

        // 枚举特化
        for enum_decl in enums {
            out.push_str(&self.generate_enum_traits(enum_decl));
        }

        // 委托类型定义
        for delegate in delegates {
            out.push_str(&self.generate_delegate_typedef(delegate));
        }

        out
    }

    /// 生成类的宏
    fn generate_class_macros(&self, class: &ClassDecl) -> String {
        let name = &class.name;
        let _api = &self.api_macro;

        // 收集反射属性
        let properties: Vec<&FieldDecl> = class
            .members
            .iter()
            .filter_map(|m| match m {
                ClassMember::Field(f) if f.is_reflected() => Some(f),
                _ => None,
            })
            .collect();

        // 收集反射函数
        let methods: Vec<&MethodDecl> = class
            .members
            .iter()
            .filter_map(|m| match m {
                ClassMember::Method(m) if m.specifiers.is_some() => Some(m),
                _ => None,
            })
            .collect();

        let prop_count = properties.len();
        let _func_count = methods.len();

        let _has_base = class.base_classes.first().is_some();
        let base_type_info = if let Some(base) = class.base_classes.first() {
            format!("{}::StaticTypeInfo()", base.name)
        } else {
            "nullptr".to_string()
        };

        // 检查是否可序列化
        let is_serializable = class.specifiers.has_flag("Serializable");

        let mut out = format!(
            r#"
// =============================================================================
// {name} 反射代码
// =============================================================================

#undef LGENERATED_BODY
#define LGENERATED_BODY() LGENERATED_BODY_{name}()

#define LGENERATED_BODY_{name}() \
private: \
    static FTypeInfo s_TypeInfo; \
    static FPropertyInfo s_Properties[{prop_count_plus}]; \
public: \
    static const FTypeInfo* StaticTypeInfo() {{ return &s_TypeInfo; }} \
    virtual const FTypeInfo* GetTypeInfo() const override {{ return &s_TypeInfo; }} \
    static {name}* StaticConstruct() {{ return new {name}(); }} \
"#,
            prop_count_plus = if prop_count > 0 { prop_count } else { 1 }
        );

        // 序列化声明
        if is_serializable {
            out.push_str(
                r#"    virtual void Serialize(FArchive& Ar); \
    virtual void Deserialize(FArchive& Ar); \
"#,
            );
        }

        out.push_str("\n\n");

        // 实现宏
        out.push_str(&format!(
            r#"#define LIMX_IMPLEMENT_{name_upper}() \
    FPropertyInfo {name}::s_Properties[] = {{ \
"#,
            name_upper = name.to_uppercase()
        ));

        if properties.is_empty() {
            out.push_str(&format!(
                r#"        {{ nullptr, nullptr, EPropertyType::Unknown, 0, 0, EPropertyFlags::None }} \
"#));
        } else {
            for prop in &properties {
                let prop_type = self.cpp_type_to_property_type(&prop.field_type);
                let flags = self.specifiers_to_property_flags(prop.specifiers.as_ref());
                out.push_str(&format!(
                    r#"        {{ "{prop_name}", "{type_name}", {prop_type}, offsetof({name}, {prop_name}), sizeof({type_str}), {flags} }}, \
"#,
                    prop_name = prop.name,
                    type_name = prop.field_type.to_string(),
                    type_str = prop.field_type.to_string(),
                ));
            }
        }

        out.push_str(&format!(
            r#"    }}; \
    FTypeInfo {name}::s_TypeInfo = {{ \
        "{name}", \
        sizeof({name}), \
        alignof({name}), \
        ETypeFlags::Reflectable{serializable_flag}, \
        {base_type_info}, \
        {name}::s_Properties, \
        {prop_count} \
    }};

"#,
            serializable_flag = if is_serializable {
                " | ETypeFlags::Serializable"
            } else {
                ""
            },
        ));

        out
    }

    /// 生成枚举特化
    fn generate_enum_traits(&self, enum_decl: &EnumDecl) -> String {
        let name = &enum_decl.name;
        let is_flags = enum_decl
            .specifiers
            .as_ref()
            .map(|s| s.has_flag("Flags"))
            .unwrap_or(false);

        let mut out = format!(
            r#"
// =============================================================================
// {name} 枚举反射
// =============================================================================

template<>
struct TEnumTraits<{name}>
{{
    static constexpr const char* Name = "{name}";
    static constexpr bool IsFlags = {is_flags};
    
    struct Entry
    {{
        {name} Value;
        const char* Name;
    }};
    
    static constexpr Entry Entries[] = {{
"#,
            is_flags = if is_flags { "true" } else { "false" }
        );

        for value in &enum_decl.values {
            out.push_str(&format!(
                r#"        {{ {name}::{val}, "{val}" }},
"#,
                val = value.name
            ));
        }

        out.push_str(&format!(
            r#"    }};
    
    static constexpr uint32 Count = sizeof(Entries) / sizeof(Entry);
    
    static const char* ToString({name} value)
    {{
        for (const auto& e : Entries)
            if (e.Value == value) return e.Name;
        return "Unknown";
    }}
}};

"#
        ));

        out
    }

    /// 生成委托类型定义
    fn generate_delegate_typedef(&self, delegate: &DelegateDecl) -> String {
        let name = &delegate.name;
        let ret = delegate.return_type.to_string();

        let params: Vec<String> = delegate
            .parameters
            .iter()
            .map(|p| p.param_type.to_string())
            .collect();
        let params_str = params.join(", ");

        let is_multicast = delegate.specifiers.has_flag("Multicast");

        if is_multicast {
            format!(
                r#"
// {name} 多播委托
using {name} = TMulticastDelegate<void({params_str})>;

"#
            )
        } else {
            format!(
                r#"
// {name} 委托
using {name} = TDelegate<{ret}({params_str})>;

"#
            )
        }
    }

    /// 生成 CPP 文件
    fn generate_cpp(
        &self,
        file_stem: &str,
        original_header: &str,
        classes: &[&ClassDecl],
        enums: &[&EnumDecl],
    ) -> String {
        let header_name = Path::new(original_header)
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("Unknown.h");

        let mut out = format!(
            r#"/*******************************************************************************
 * 文件: {file_stem}.generated.cpp
 * 
 * 警告: 此文件由 LHT (Limx Header Tool) 自动生成
 *       请勿手动修改，任何修改都会被覆盖
 *
 ******************************************************************************/

#include "{header_name}"
#include "{file_stem}.generated.h"

"#
        );

        // 实现宏调用
        for class in classes {
            out.push_str(&format!(
                "LIMX_IMPLEMENT_{}()\n\n",
                class.name.to_uppercase()
            ));

            // 序列化实现
            if class.specifiers.has_flag("Serializable") {
                out.push_str(&self.generate_serialization_impl(class));
            }
        }

        // 自动注册
        out.push_str(&self.generate_auto_registration(classes, enums));

        out
    }

    /// 生成序列化实现
    fn generate_serialization_impl(&self, class: &ClassDecl) -> String {
        let name = &class.name;

        let properties: Vec<&FieldDecl> = class
            .members
            .iter()
            .filter_map(|m| match m {
                ClassMember::Field(f) if f.is_reflected() => Some(f),
                _ => None,
            })
            .collect();

        let mut serialize_body = String::new();
        let mut deserialize_body = String::new();

        // 基类序列化
        if let Some(base) = class.base_classes.first() {
            serialize_body.push_str(&format!("    {}::Serialize(Ar);\n", base.name));
            deserialize_body.push_str(&format!("    {}::Deserialize(Ar);\n", base.name));
        }

        // 属性序列化
        for prop in &properties {
            let is_serializable = prop
                .specifiers
                .as_ref()
                .map(|s| s.has_flag("Serializable"))
                .unwrap_or(false);
            let is_transient = prop
                .specifiers
                .as_ref()
                .map(|s| s.has_flag("Transient"))
                .unwrap_or(false);

            if is_serializable && !is_transient {
                serialize_body.push_str(&format!("    Ar << {};\n", prop.name));
                deserialize_body.push_str(&format!("    Ar << {};\n", prop.name));
            }
        }

        format!(
            r#"void {name}::Serialize(FArchive& Ar)
{{
{serialize_body}}}

void {name}::Deserialize(FArchive& Ar)
{{
{deserialize_body}}}

"#
        )
    }

    /// 生成自动注册代码
    fn generate_auto_registration(&self, classes: &[&ClassDecl], _enums: &[&EnumDecl]) -> String {
        if classes.is_empty() {
            return String::new();
        }

        let mut out = String::from(
            r#"// =============================================================================
// 自动类型注册
// =============================================================================

namespace
{
    struct FAutoRegister
    {
        FAutoRegister()
        {
"#,
        );

        for class in classes {
            out.push_str(&format!(
                "            FTypeRegistry::Get().Register({}::StaticTypeInfo());\n",
                class.name
            ));
        }

        out.push_str(
            r#"        }
    };
    
    static FAutoRegister s_AutoRegister;
}
"#,
        );

        out
    }

    /// C++ 类型到属性类型
    fn cpp_type_to_property_type(&self, cpp_type: &CppType) -> &'static str {
        let base = cpp_type.base_type.as_str();

        match base {
            "bool" => "EPropertyType::Bool",
            "int8" | "int8_t" | "char" => "EPropertyType::Int8",
            "int16" | "int16_t" | "short" => "EPropertyType::Int16",
            "int32" | "int32_t" | "int" => "EPropertyType::Int32",
            "int64" | "int64_t" | "long long" => "EPropertyType::Int64",
            "uint8" | "uint8_t" | "unsigned char" => "EPropertyType::UInt8",
            "uint16" | "uint16_t" | "unsigned short" => "EPropertyType::UInt16",
            "uint32" | "uint32_t" | "unsigned int" => "EPropertyType::UInt32",
            "uint64" | "uint64_t" | "unsigned long long" => "EPropertyType::UInt64",
            "float" => "EPropertyType::Float",
            "double" => "EPropertyType::Double",
            _ if cpp_type.modifiers.is_pointer => "EPropertyType::Object",
            _ => "EPropertyType::Struct",
        }
    }

    /// 说明符到属性标志
    fn specifiers_to_property_flags(&self, specs: Option<&Specifiers>) -> String {
        let Some(specs) = specs else {
            return "EPropertyFlags::None".to_string();
        };

        let mut flags = Vec::new();

        if specs.has_flag("Editable") || specs.has_flag("EditAnywhere") {
            flags.push("EPropertyFlags::Editable");
        }
        if specs.has_flag("Serializable") {
            flags.push("EPropertyFlags::Serializable");
        }
        if specs.has_flag("Replicated") {
            flags.push("EPropertyFlags::Replicated");
        }
        if specs.has_flag("Transient") {
            flags.push("EPropertyFlags::Transient");
        }

        if flags.is_empty() {
            "EPropertyFlags::None".to_string()
        } else {
            flags.join(" | ")
        }
    }
}

/// 生成的文件信息
#[derive(Debug, Default)]
pub struct GeneratedFiles {
    pub header: String,
    pub source: String,
    pub classes: Vec<String>,
    pub enums: Vec<String>,
}

impl GeneratedFiles {
    pub fn empty() -> Self {
        Self::default()
    }

    pub fn is_empty(&self) -> bool {
        self.classes.is_empty() && self.enums.is_empty()
    }
}
