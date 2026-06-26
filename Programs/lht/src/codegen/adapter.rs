/*******************************************************************************
 * 文件: codegen/adapter.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   AST 适配器 - 提供代码生成所需的辅助方法
 *   使用扩展 trait 模式避免与已有方法冲突
 *
 ******************************************************************************/

use crate::parser::ast::*;
use crate::parser::lexer::SourceLocation;
use crate::parser::parser::{ClassInfo, FunctionInfo, PropertyInfo};

//=============================================================================
// ClassInfo -> ClassDecl 转换
//=============================================================================

/// 将 parser::ClassInfo 转换为 ast::ClassDecl
pub fn class_info_to_decl(info: &ClassInfo) -> ClassDecl {
    let mut members = Vec::new();

    // 转换属性
    for prop in &info.properties {
        members.push(ClassMember::Field(property_info_to_field(prop)));
    }

    // 转换函数
    for func in &info.functions {
        members.push(ClassMember::Method(function_info_to_method(func)));
    }

    // 转换基类
    let base_classes: Vec<BaseClass> = info
        .base_classes
        .iter()
        .map(|b| BaseClass {
            name: b.name.clone(),
            access: match b.access {
                crate::parser::parser::AccessModifier::Public => AccessSpecifier::Public,
                crate::parser::parser::AccessModifier::Protected => AccessSpecifier::Protected,
                crate::parser::parser::AccessModifier::Private => AccessSpecifier::Private,
            },
            is_virtual: b.is_virtual,
        })
        .collect();

    ClassDecl {
        kind: ClassKind::Class,
        name: info.name.clone(),
        qualified_name: info.name.clone(),
        base_classes,
        api_macro: info.api_macro.clone(),
        specifiers: info.raw_specifiers.clone(),
        members,
        location: SourceLocation {
            line: 0,
            column: 0,
            offset: 0,
        },
        has_generated_body: true,
    }
}

/// 将 PropertyInfo 转换为 FieldDecl
fn property_info_to_field(prop: &PropertyInfo) -> FieldDecl {
    FieldDecl {
        name: prop.name.clone(),
        field_type: CppType::simple(&prop.type_name),
        specifiers: Some(prop.raw_specifiers.clone()),
        default_value: prop.default_value.clone(),
        access: AccessSpecifier::Public,
        is_static: prop.is_static,
        is_mutable: false,
        bit_field_size: None,
        location: SourceLocation {
            line: 0,
            column: 0,
            offset: 0,
        },
    }
}

/// 将 FunctionInfo 转换为 MethodDecl
fn function_info_to_method(func: &FunctionInfo) -> MethodDecl {
    let parameters: Vec<ParameterDecl> = func
        .parameters
        .iter()
        .map(|p| ParameterDecl {
            name: p.name.clone(),
            param_type: CppType::simple(&p.type_name),
            default_value: p.default_value.clone(),
        })
        .collect();

    MethodDecl {
        name: func.name.clone(),
        return_type: CppType::simple(&func.return_type),
        parameters,
        specifiers: Some(func.raw_specifiers.clone()),
        access: AccessSpecifier::Public,
        modifiers: MethodModifiers {
            is_virtual: func.is_virtual,
            is_const: func.is_const,
            is_static: func.is_static,
            ..Default::default()
        },
        location: SourceLocation {
            line: 0,
            column: 0,
            offset: 0,
        },
    }
}

//=============================================================================
// 代码生成辅助 Trait
//=============================================================================

/// 类声明代码生成扩展
pub trait ClassDeclExt {
    fn get_fields(&self) -> Vec<&FieldDecl>;
    fn get_methods(&self) -> Vec<&MethodDecl>;
    fn get_first_base(&self) -> Option<&str>;
    fn has_base(&self) -> bool;
}

impl ClassDeclExt for ClassDecl {
    fn get_fields(&self) -> Vec<&FieldDecl> {
        self.members
            .iter()
            .filter_map(|m| {
                if let ClassMember::Field(f) = m {
                    Some(f)
                } else {
                    None
                }
            })
            .collect()
    }

    fn get_methods(&self) -> Vec<&MethodDecl> {
        self.members
            .iter()
            .filter_map(|m| {
                if let ClassMember::Method(m) = m {
                    Some(m)
                } else {
                    None
                }
            })
            .collect()
    }

    fn get_first_base(&self) -> Option<&str> {
        self.base_classes.first().map(|b| b.name.as_str())
    }

    fn has_base(&self) -> bool {
        !self.base_classes.is_empty()
    }
}

//=============================================================================
// FieldDecl 扩展
//=============================================================================

/// 字段声明代码生成扩展
pub trait FieldDeclExt {
    fn type_str(&self) -> String;
    fn has_spec(&self, name: &str) -> bool;
    fn get_meta_value(&self, key: &str) -> Option<&str>;
    fn has_meta_key(&self, key: &str) -> bool;
    fn is_transient_field(&self) -> bool;
    fn is_serializable_field(&self) -> bool;
    fn is_replicated_field(&self) -> bool;
}

impl FieldDeclExt for FieldDecl {
    fn type_str(&self) -> String {
        self.field_type.to_string()
    }

    fn has_spec(&self, name: &str) -> bool {
        self.specifiers
            .as_ref()
            .map(|s| s.has_flag(name))
            .unwrap_or(false)
    }

    fn get_meta_value(&self, key: &str) -> Option<&str> {
        self.specifiers
            .as_ref()
            .and_then(|s| s.get_meta_string(key))
    }

    fn has_meta_key(&self, key: &str) -> bool {
        self.specifiers
            .as_ref()
            .map(|s| s.get_meta(key).is_some())
            .unwrap_or(false)
    }

    fn is_transient_field(&self) -> bool {
        self.has_spec("Transient")
    }

    fn is_serializable_field(&self) -> bool {
        !self.is_transient_field() && !self.has_spec("SkipSerialization")
    }

    fn is_replicated_field(&self) -> bool {
        self.has_spec("Replicated")
    }
}

//=============================================================================
// MethodDecl 扩展
//=============================================================================

/// 方法声明代码生成扩展
pub trait MethodDeclExt {
    fn return_type_str(&self) -> String;
    fn has_spec(&self, name: &str) -> bool;
    fn is_client_rpc(&self) -> bool;
    fn is_server_rpc(&self) -> bool;
    fn is_multicast_rpc(&self) -> bool;
    fn is_any_rpc(&self) -> bool;
    fn is_reliable_rpc(&self) -> bool;
    fn params_string(&self) -> String;
    fn param_names_string(&self) -> String;
}

impl MethodDeclExt for MethodDecl {
    fn return_type_str(&self) -> String {
        self.return_type.to_string()
    }

    fn has_spec(&self, name: &str) -> bool {
        self.specifiers
            .as_ref()
            .map(|s| s.has_flag(name))
            .unwrap_or(false)
    }

    fn is_client_rpc(&self) -> bool {
        self.has_spec("Client")
    }

    fn is_server_rpc(&self) -> bool {
        self.has_spec("Server")
    }

    fn is_multicast_rpc(&self) -> bool {
        self.has_spec("NetMulticast")
    }

    fn is_any_rpc(&self) -> bool {
        self.is_client_rpc() || self.is_server_rpc() || self.is_multicast_rpc()
    }

    fn is_reliable_rpc(&self) -> bool {
        self.has_spec("Reliable")
    }

    fn params_string(&self) -> String {
        self.parameters
            .iter()
            .map(|p| format!("{} {}", p.param_type.to_string(), p.name))
            .collect::<Vec<_>>()
            .join(", ")
    }

    fn param_names_string(&self) -> String {
        self.parameters
            .iter()
            .map(|p| p.name.as_str())
            .collect::<Vec<_>>()
            .join(", ")
    }
}

//=============================================================================
// ParameterDecl 扩展
//=============================================================================

/// 参数声明代码生成扩展
pub trait ParameterDeclExt {
    fn type_str(&self) -> String;
}

impl ParameterDeclExt for ParameterDecl {
    fn type_str(&self) -> String {
        self.param_type.to_string()
    }
}

//=============================================================================
// CppType 扩展
//=============================================================================

/// C++ 类型代码生成扩展
pub trait CppTypeExt {
    fn is_ptr_type(&self) -> bool;
    fn is_container_type(&self) -> bool;
    fn is_weak_ptr_type(&self) -> bool;
    fn is_soft_ptr_type(&self) -> bool;
    fn is_pod_type(&self) -> bool;
}

impl CppTypeExt for CppType {
    fn is_ptr_type(&self) -> bool {
        self.modifiers.is_pointer
            || self.base_type.contains("TObjectPtr")
            || self.base_type.contains("TWeakObjectPtr")
            || self.base_type.contains("TSoftObjectPtr")
            || self.base_type.contains("TSharedPtr")
            || self.base_type.contains("TUniquePtr")
    }

    fn is_container_type(&self) -> bool {
        self.base_type.starts_with("TArray")
            || self.base_type.starts_with("TMap")
            || self.base_type.starts_with("TSet")
            || self.base_type.contains("Array")
            || self.base_type.contains("Map")
            || self.base_type.contains("Set")
    }

    fn is_weak_ptr_type(&self) -> bool {
        self.base_type.contains("TWeakObjectPtr") || self.base_type.contains("TWeakPtr")
    }

    fn is_soft_ptr_type(&self) -> bool {
        self.base_type.contains("TSoftObjectPtr") || self.base_type.contains("TSoftClassPtr")
    }

    fn is_pod_type(&self) -> bool {
        matches!(
            self.base_type.as_str(),
            "bool"
                | "char"
                | "int"
                | "float"
                | "double"
                | "Int8"
                | "Int16"
                | "Int32"
                | "Int64"
                | "UInt8"
                | "UInt16"
                | "UInt32"
                | "UInt64"
                | "Float32"
                | "Float64"
                | "int8_t"
                | "int16_t"
                | "int32_t"
                | "int64_t"
                | "uint8_t"
                | "uint16_t"
                | "uint32_t"
                | "uint64_t"
                | "size_t"
                | "ptrdiff_t"
        )
    }
}

//=============================================================================
// EnumDecl 扩展
//=============================================================================

/// 枚举声明代码生成扩展
pub trait EnumDeclExt {
    fn underlying_type_str(&self) -> String;
}

impl EnumDeclExt for EnumDecl {
    fn underlying_type_str(&self) -> String {
        self.underlying_type
            .as_ref()
            .map(|t| t.to_string())
            .unwrap_or_else(|| "int".to_string())
    }
}

//=============================================================================
// ClassInfo → EditorMeta PropertyInput 转换
//=============================================================================

use crate::codegen::editor_meta::PropertyInput;
use crate::codegen::script_binding::{
    map_to_script_type, ParamDirection, ScriptClassBinding, ScriptFunction, ScriptParam,
    ScriptProperty,
};

/// 将 ClassInfo 的属性列表转换为 EditorMetaGenerator 所需的 PropertyInput
pub fn class_info_to_editor_inputs(info: &ClassInfo) -> Vec<PropertyInput> {
    info.properties
        .iter()
        .map(|prop| {
            // 从 raw_specifiers.items 收集键值对
            let mut specifiers = Vec::new();
            for (key, val) in &prop.raw_specifiers.items {
                let opt_val = match val {
                    crate::parser::specifiers::SpecifierValue::Flag => None,
                    crate::parser::specifiers::SpecifierValue::String(s) => Some(s.clone()),
                    crate::parser::specifiers::SpecifierValue::Number(n) => Some(n.to_string()),
                    crate::parser::specifiers::SpecifierValue::Nested(_) => None,
                };
                specifiers.push((key.clone(), opt_val));
            }
            // 也收集 meta 子级
            if let Some(meta_map) = prop.raw_specifiers.get_nested("meta") {
                for (mk, mv) in meta_map {
                    let opt_val = match mv {
                        crate::parser::specifiers::SpecifierValue::String(s) => Some(s.clone()),
                        crate::parser::specifiers::SpecifierValue::Number(n) => Some(n.to_string()),
                        _ => None,
                    };
                    specifiers.push((mk.clone(), opt_val));
                }
            }

            PropertyInput {
                name: prop.name.clone(),
                cpp_type: prop.type_name.clone(),
                specifiers,
            }
        })
        .collect()
}

/// 将 ClassInfo 转换为 ScriptClassBinding (仅包含 BlueprintCallable/BlueprintReadWrite 成员)
pub fn class_info_to_script_binding(info: &ClassInfo) -> ScriptClassBinding {
    let functions: Vec<ScriptFunction> = info
        .functions
        .iter()
        .filter(|f| {
            f.raw_specifiers.has_flag("BlueprintCallable")
                || f.raw_specifiers.has_flag("BlueprintImplementableEvent")
                || f.raw_specifiers.has_flag("BlueprintNativeEvent")
        })
        .map(|f| {
            let params: Vec<ScriptParam> = f
                .parameters
                .iter()
                .map(|p| {
                    let direction = if p.is_out {
                        ParamDirection::Out
                    } else if p.is_reference && !p.is_const {
                        ParamDirection::InOut
                    } else {
                        ParamDirection::In
                    };
                    ScriptParam {
                        name: p.name.clone(),
                        cpp_type: p.type_name.clone(),
                        script_type: map_to_script_type(&p.type_name),
                        direction,
                        is_const: p.is_const,
                        is_reference: p.is_reference,
                        default_value: p.default_value.clone(),
                    }
                })
                .collect();

            let return_type = if f.return_type == "void" {
                None
            } else {
                Some(ScriptParam {
                    name: "ReturnValue".to_string(),
                    cpp_type: f.return_type.clone(),
                    script_type: map_to_script_type(&f.return_type),
                    direction: ParamDirection::Return,
                    is_const: false,
                    is_reference: false,
                    default_value: None,
                })
            };

            let category = f
                .raw_specifiers
                .get_string("Category")
                .unwrap_or("Default")
                .to_string();

            ScriptFunction {
                name: f.name.clone(),
                display_name: f
                    .raw_specifiers
                    .get_meta_string("DisplayName")
                    .map(|s| s.to_string()),
                params,
                return_type,
                is_const: f.is_const,
                is_static: f.is_static,
                is_event: f.raw_specifiers.has_flag("BlueprintImplementableEvent"),
                tooltip: f
                    .raw_specifiers
                    .get_meta_string("ToolTip")
                    .map(|s| s.to_string()),
                category,
            }
        })
        .collect();

    let properties: Vec<ScriptProperty> = info
        .properties
        .iter()
        .filter(|p| {
            p.raw_specifiers.has_flag("BlueprintReadWrite")
                || p.raw_specifiers.has_flag("BlueprintReadOnly")
        })
        .map(|p| {
            let writable = p.raw_specifiers.has_flag("BlueprintReadWrite");
            let category = p
                .raw_specifiers
                .get_string("Category")
                .unwrap_or("Default")
                .to_string();
            ScriptProperty {
                name: p.name.clone(),
                cpp_type: p.type_name.clone(),
                script_type: map_to_script_type(&p.type_name),
                readable: true,
                writable,
                category,
            }
        })
        .collect();

    ScriptClassBinding {
        class_name: info.name.clone(),
        parent_class: info.base_class().map(|s| s.to_string()),
        functions,
        properties,
    }
}

/// 将 ClassInfo 转换为 TypeBindingGenerator 的 BindingClass 输入
pub fn class_info_to_binding_class(info: &ClassInfo) -> crate::codegen::type_binding::BindingClass {
    use crate::codegen::type_binding::*;

    let properties = info
        .properties
        .iter()
        .map(|p| {
            let is_read_only = p.raw_specifiers.has_flag("VisibleAnywhere")
                || p.raw_specifiers.has_flag("BlueprintReadOnly")
                || p.is_const;
            let is_write_only = p.raw_specifiers.has_flag("WriteOnly");

            let access = if is_write_only {
                PropertyAccess::WriteOnly
            } else if is_read_only {
                PropertyAccess::ReadOnly
            } else {
                PropertyAccess::ReadWrite
            };

            let replication = if p.raw_specifiers.has_flag("Replicated") {
                ReplicationMode::ServerToClient
            } else if p.raw_specifiers.has_flag("ReplicatedBidirectional") {
                ReplicationMode::Bidirectional
            } else {
                ReplicationMode::None
            };

            let range = p
                .raw_specifiers
                .get_meta_string("ClampMin")
                .and_then(|min_s| min_s.parse::<f64>().ok())
                .and_then(|min_v| {
                    p.raw_specifiers
                        .get_meta_string("ClampMax")
                        .and_then(|max_s| max_s.parse::<f64>().ok())
                        .map(|max_v| (min_v, max_v))
                });

            BindingProperty {
                name: p.name.clone(),
                cpp_type: p.type_name.clone(),
                access,
                serializable: !p.raw_specifiers.has_flag("Transient")
                    && !p.raw_specifiers.has_flag("SkipSerialization"),
                editable: p.raw_specifiers.has_flag("EditAnywhere")
                    || p.raw_specifiers.has_flag("EditDefaultsOnly")
                    || p.raw_specifiers.has_flag("EditInstanceOnly"),
                replication,
                category: p
                    .raw_specifiers
                    .get_string("Category")
                    .map(|s| s.to_string()),
                display_name: p
                    .raw_specifiers
                    .get_meta_string("DisplayName")
                    .map(|s| s.to_string()),
                tooltip: p
                    .raw_specifiers
                    .get_meta_string("ToolTip")
                    .map(|s| s.to_string()),
                range,
                transient: p.raw_specifiers.has_flag("Transient"),
            }
        })
        .collect();

    let methods = info
        .functions
        .iter()
        .map(|f| {
            let parameters = f
                .parameters
                .iter()
                .map(|param| (param.name.clone(), param.type_name.clone()))
                .collect();

            BindingMethod {
                name: f.name.clone(),
                return_type: f.return_type.clone(),
                parameters,
                is_const: f.is_const,
                is_virtual: f.is_virtual,
                is_rpc: f.raw_specifiers.has_flag("Server")
                    || f.raw_specifiers.has_flag("Client")
                    || f.raw_specifiers.has_flag("NetMulticast"),
            }
        })
        .collect();

    let parent_class = info.base_classes.first().map(|b| b.name.clone());

    BindingClass {
        class_name: info.name.clone(),
        namespace: None,
        parent_class,
        properties,
        methods,
        is_abstract: info.is_abstract,
        module_name: String::new(),
    }
}

/// 将 ClassInfo 转换为 MigrationGenerator 的 TypeVersion 快照
pub fn class_info_to_type_version(
    info: &ClassInfo,
    version: u32,
) -> crate::codegen::migration::TypeVersion {
    let properties = info
        .properties
        .iter()
        .map(|p| {
            let is_serializable = !p.raw_specifiers.has_flag("Transient")
                && !p.raw_specifiers.has_flag("SkipSerialization");
            crate::codegen::migration::PropertyDef {
                name: p.name.clone(),
                cpp_type: p.type_name.clone(),
                default_value: p.default_value.clone(),
                is_serializable,
            }
        })
        .collect();

    crate::codegen::migration::TypeVersion {
        class_name: info.name.clone(),
        version,
        properties,
    }
}
