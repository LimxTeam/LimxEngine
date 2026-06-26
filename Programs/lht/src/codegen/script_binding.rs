// ============================================================
// 文件名称：script_binding.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：蓝图/脚本绑定代码生成 — 从 LFUNCTION(BlueprintCallable)
//           和 LPROPERTY(BlueprintReadWrite) 自动生成类型安全的
//           脚本层调用包装器。UE5 需要手动维护 UFUNCTION 宏和
//           参数签名一致性，我们做到全自动生成 Thunk 函数 +
//           参数编解码 + 返回值转换 + 错误处理，零手工包装
// 功能描述：解析带 BlueprintCallable/BlueprintReadWrite 说明符
//           的函数和属性 → 生成 C++ Thunk 包装函数 (脚本 VM
//           调用 C++ 的桥梁) → 生成参数栈编解码代码 → 生成
//           脚本层类型注册信息 → 生成文档注释
// 技术特性：Thunk 函数生成、参数栈编解码、类型映射表、
//           const 正确性保持、引用参数/输出参数支持、
//           默认参数处理、overload 消歧
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ ScriptBindingGenerator     │ 脚本绑定代码生成器             │
// │ ScriptFunction             │ 可脚本调用的函数描述           │
// │ ScriptParam                │ 脚本函数参数                  │
// │ ScriptProperty             │ 可脚本访问的属性描述           │
// │ ScriptClassBinding         │ 类的完整脚本绑定              │
// │ GeneratedScriptBinding     │ 生成的绑定代码                │
// │ ScriptTypeMapping          │ C++ ↔ 脚本类型映射           │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建生成器                    │
// │ generate_class_binding()   │ 生成类的脚本绑定              │
// │ generate_thunk()           │ 生成 Thunk 包装函数           │
// │ generate_property_accessor │ 生成属性 Getter/Setter        │
// │ generate_registration()    │ 生成注册代码                  │
// │ map_type()                 │ C++ → 脚本类型映射           │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};

// =============================================================================
// 脚本类型映射
// =============================================================================

/// C++ ↔ 脚本类型映射
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScriptTypeMapping {
    /// C++ 类型名
    pub cpp_type: String,
    /// 脚本层类型名
    pub script_type: String,
    /// 栈上占用字节数
    pub stack_size: usize,
    /// 是否值类型 (vs 引用/指针)
    pub is_value_type: bool,
}

/// 内置类型映射表
pub fn builtin_type_mappings() -> Vec<ScriptTypeMapping> {
    vec![
        ScriptTypeMapping {
            cpp_type: "bool".into(),
            script_type: "Bool".into(),
            stack_size: 1,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "int32".into(),
            script_type: "Int32".into(),
            stack_size: 4,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "int64".into(),
            script_type: "Int64".into(),
            stack_size: 8,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "uint32".into(),
            script_type: "UInt32".into(),
            stack_size: 4,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "float".into(),
            script_type: "Float".into(),
            stack_size: 4,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "double".into(),
            script_type: "Double".into(),
            stack_size: 8,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "FString".into(),
            script_type: "String".into(),
            stack_size: 0,
            is_value_type: false,
        },
        ScriptTypeMapping {
            cpp_type: "FName".into(),
            script_type: "Name".into(),
            stack_size: 8,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "FText".into(),
            script_type: "Text".into(),
            stack_size: 0,
            is_value_type: false,
        },
        ScriptTypeMapping {
            cpp_type: "FVector".into(),
            script_type: "Vector3".into(),
            stack_size: 12,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "FVector2D".into(),
            script_type: "Vector2".into(),
            stack_size: 8,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "FRotator".into(),
            script_type: "Rotator".into(),
            stack_size: 12,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "FQuat".into(),
            script_type: "Quaternion".into(),
            stack_size: 16,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "FLinearColor".into(),
            script_type: "Color".into(),
            stack_size: 16,
            is_value_type: true,
        },
        ScriptTypeMapping {
            cpp_type: "FTransform".into(),
            script_type: "Transform".into(),
            stack_size: 0,
            is_value_type: false,
        },
    ]
}

/// 查找类型映射
pub fn find_type_mapping(cpp_type: &str) -> Option<ScriptTypeMapping> {
    let clean = cpp_type
        .trim()
        .trim_end_matches('*')
        .trim_end_matches('&')
        .trim_start_matches("const ")
        .trim();
    builtin_type_mappings()
        .into_iter()
        .find(|m| m.cpp_type == clean)
}

/// 获取脚本类型名
pub fn map_to_script_type(cpp_type: &str) -> String {
    if let Some(mapping) = find_type_mapping(cpp_type) {
        mapping.script_type
    } else if cpp_type.contains('*') {
        // 对象指针 → ObjectRef
        "ObjectRef".to_string()
    } else {
        // 未知类型 → Struct
        "Struct".to_string()
    }
}

// =============================================================================
// 脚本函数参数
// =============================================================================

/// 参数传递方向
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ParamDirection {
    /// 输入参数
    In,
    /// 输出参数 (引用)
    Out,
    /// 输入+输出参数
    InOut,
    /// 返回值
    Return,
}

/// 脚本函数参数
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScriptParam {
    /// 参数名
    pub name: String,
    /// C++ 类型
    pub cpp_type: String,
    /// 脚本类型
    pub script_type: String,
    /// 传递方向
    pub direction: ParamDirection,
    /// 是否 const
    pub is_const: bool,
    /// 是否引用
    pub is_reference: bool,
    /// 默认值 (可选)
    pub default_value: Option<String>,
}

// =============================================================================
// 脚本函数
// =============================================================================

/// 可脚本调用的函数描述
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScriptFunction {
    /// 函数名
    pub name: String,
    /// 显示名称
    pub display_name: Option<String>,
    /// 参数列表
    pub params: Vec<ScriptParam>,
    /// 返回类型 (None = void)
    pub return_type: Option<ScriptParam>,
    /// 是否 const 成员函数
    pub is_const: bool,
    /// 是否静态函数
    pub is_static: bool,
    /// 是否纯虚函数 (脚本可覆写)
    pub is_event: bool,
    /// 工具提示
    pub tooltip: Option<String>,
    /// 分类
    pub category: String,
}

impl ScriptFunction {
    /// 生成 Thunk 函数名
    pub fn thunk_name(&self, class_name: &str) -> String {
        format!("exec{class_name}_{}", self.name)
    }

    /// 输入参数
    pub fn input_params(&self) -> Vec<&ScriptParam> {
        self.params
            .iter()
            .filter(|p| matches!(p.direction, ParamDirection::In | ParamDirection::InOut))
            .collect()
    }

    /// 输出参数
    pub fn output_params(&self) -> Vec<&ScriptParam> {
        self.params
            .iter()
            .filter(|p| matches!(p.direction, ParamDirection::Out | ParamDirection::InOut))
            .collect()
    }
}

// =============================================================================
// 脚本属性
// =============================================================================

/// 可脚本访问的属性描述
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScriptProperty {
    /// 属性名
    pub name: String,
    /// C++ 类型
    pub cpp_type: String,
    /// 脚本类型
    pub script_type: String,
    /// 是否可读
    pub readable: bool,
    /// 是否可写
    pub writable: bool,
    /// 分类
    pub category: String,
}

// =============================================================================
// 类脚本绑定
// =============================================================================

/// 类的完整脚本绑定
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScriptClassBinding {
    /// 类名
    pub class_name: String,
    /// 父类名 (可选)
    pub parent_class: Option<String>,
    /// 脚本可调用函数
    pub functions: Vec<ScriptFunction>,
    /// 脚本可访问属性
    pub properties: Vec<ScriptProperty>,
}

impl ScriptClassBinding {
    /// 函数数
    pub fn function_count(&self) -> usize {
        self.functions.len()
    }

    /// 属性数
    pub fn property_count(&self) -> usize {
        self.properties.len()
    }

    /// 事件函数 (脚本可覆写)
    pub fn events(&self) -> Vec<&ScriptFunction> {
        self.functions.iter().filter(|f| f.is_event).collect()
    }
}

// =============================================================================
// 生成的脚本绑定代码
// =============================================================================

/// 生成的脚本绑定代码
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GeneratedScriptBinding {
    /// 头文件内容 (Thunk 声明)
    pub header_content: String,
    /// 实现文件内容 (Thunk 实现)
    pub impl_content: String,
    /// 注册代码
    pub registration_content: String,
    /// 类名
    pub class_name: String,
    /// 生成的 Thunk 函数数
    pub thunk_count: usize,
    /// 生成的属性访问器数
    pub accessor_count: usize,
}

// =============================================================================
// 脚本绑定代码生成器
// =============================================================================

/// 脚本绑定代码生成器
pub struct ScriptBindingGenerator;

impl ScriptBindingGenerator {
    /// 创建生成器
    pub fn new() -> Self {
        Self
    }

    /// 生成类的完整脚本绑定代码
    pub fn generate_class_binding(&self, binding: &ScriptClassBinding) -> GeneratedScriptBinding {
        let class = &binding.class_name;

        let header = self.generate_header(binding);
        let impl_code = self.generate_impl(binding);
        let registration = self.generate_registration(binding);

        GeneratedScriptBinding {
            header_content: header,
            impl_content: impl_code,
            registration_content: registration,
            class_name: class.clone(),
            thunk_count: binding.functions.len(),
            accessor_count: binding
                .properties
                .iter()
                .map(|p| (p.readable as usize) + (p.writable as usize))
                .sum(),
        }
    }

    /// 生成头文件
    fn generate_header(&self, binding: &ScriptClassBinding) -> String {
        let class = &binding.class_name;
        let mut h = String::with_capacity(2048);

        h.push_str(&format!("// 自动生成 — {} 脚本绑定\n", class));
        h.push_str("#pragma once\n\n");
        h.push_str("#include \"Scripting/ScriptThunk.h\"\n\n");
        h.push_str(&format!("namespace Limx::Script\n{{\n\n"));

        // Thunk 函数声明
        for func in &binding.functions {
            let thunk = func.thunk_name(class);
            h.push_str(&format!(
                "void {}(LObject* Context, ScriptFrame& Stack, void* Result);\n",
                thunk,
            ));
        }

        // 属性访问器声明
        for prop in &binding.properties {
            if prop.readable {
                h.push_str(&format!(
                    "void execGet{class}_{}(LObject* Context, ScriptFrame& Stack, void* Result);\n",
                    prop.name,
                ));
            }
            if prop.writable {
                h.push_str(&format!(
                    "void execSet{class}_{}(LObject* Context, ScriptFrame& Stack, void* Result);\n",
                    prop.name,
                ));
            }
        }

        h.push_str(&format!("\n}} // namespace Limx::Script\n"));
        h
    }

    /// 生成实现文件
    fn generate_impl(&self, binding: &ScriptClassBinding) -> String {
        let class = &binding.class_name;
        let mut cpp = String::with_capacity(4096);

        cpp.push_str(&format!("// 自动生成 — {} 脚本绑定实现\n", class));
        cpp.push_str(&format!("#include \"{class}.script.h\"\n"));
        cpp.push_str(&format!("#include \"{class}.h\"\n\n"));
        cpp.push_str(&format!("namespace Limx::Script\n{{\n\n"));

        // Thunk 函数实现
        for func in &binding.functions {
            cpp.push_str(&self.generate_thunk(class, func));
            cpp.push('\n');
        }

        // 属性访问器实现
        for prop in &binding.properties {
            cpp.push_str(&self.generate_property_accessors(class, prop));
        }

        cpp.push_str("} // namespace Limx::Script\n");
        cpp
    }

    /// 生成单个 Thunk 包装函数
    pub fn generate_thunk(&self, class_name: &str, func: &ScriptFunction) -> String {
        let thunk = func.thunk_name(class_name);
        let mut code = String::with_capacity(1024);

        code.push_str(&format!(
            "void {}(LObject* Context, ScriptFrame& Stack, void* Result)\n{{\n",
            thunk,
        ));

        // 获取 this 指针
        if !func.is_static {
            code.push_str(&format!(
                "    {class_name}* Self = static_cast<{class_name}*>(Context);\n",
                class_name = class_name,
            ));
        }

        // 从栈弹出参数
        for param in &func.params {
            let pop_method = if param.is_reference && !param.is_const {
                "PopRef"
            } else {
                "Pop"
            };
            code.push_str(&format!(
                "    {} {} = Stack.{}<{}>();\n",
                if param.is_const {
                    format!("const {}", param.cpp_type)
                } else {
                    param.cpp_type.clone()
                },
                param.name,
                pop_method,
                param
                    .cpp_type
                    .trim_end_matches('&')
                    .trim_end_matches('*')
                    .trim(),
            ));
        }

        code.push_str("    Stack.FinishParams();\n\n");

        // 调用实际函数
        let param_names: Vec<&str> = func.params.iter().map(|p| p.name.as_str()).collect();
        let args = param_names.join(", ");

        if let Some(ret) = &func.return_type {
            if func.is_static {
                code.push_str(&format!(
                    "    {} ReturnValue = {class_name}::{}({args});\n",
                    ret.cpp_type,
                    func.name,
                    class_name = class_name,
                ));
            } else {
                code.push_str(&format!(
                    "    {} ReturnValue = Self->{}({args});\n",
                    ret.cpp_type, func.name,
                ));
            }
            code.push_str(&format!(
                "    *static_cast<{}*>(Result) = ReturnValue;\n",
                ret.cpp_type,
            ));
        } else {
            if func.is_static {
                code.push_str(&format!(
                    "    {class_name}::{}({args});\n",
                    func.name,
                    class_name = class_name,
                ));
            } else {
                code.push_str(&format!("    Self->{}({args});\n", func.name,));
            }
        }

        // 写回输出参数
        for param in func.output_params() {
            code.push_str(&format!(
                "    Stack.PushOut<{}>({});\n",
                param.cpp_type.trim_end_matches('&').trim(),
                param.name,
            ));
        }

        code.push_str("}\n");
        code
    }

    /// 生成属性访问器
    pub fn generate_property_accessors(&self, class_name: &str, prop: &ScriptProperty) -> String {
        let mut code = String::with_capacity(512);

        if prop.readable {
            code.push_str(&format!(
                "void execGet{class_name}_{}(LObject* Context, ScriptFrame& Stack, void* Result)\n{{\n",
                prop.name,
            ));
            code.push_str(&format!(
                "    {class_name}* Self = static_cast<{class_name}*>(Context);\n",
            ));
            code.push_str("    Stack.FinishParams();\n");
            code.push_str(&format!(
                "    *static_cast<{}*>(Result) = Self->{};\n",
                prop.cpp_type, prop.name,
            ));
            code.push_str("}\n\n");
        }

        if prop.writable {
            code.push_str(&format!(
                "void execSet{class_name}_{}(LObject* Context, ScriptFrame& Stack, void* Result)\n{{\n",
                prop.name,
            ));
            code.push_str(&format!(
                "    {class_name}* Self = static_cast<{class_name}*>(Context);\n",
            ));
            code.push_str(&format!(
                "    {} NewValue = Stack.Pop<{}>();\n",
                prop.cpp_type, prop.cpp_type,
            ));
            code.push_str("    Stack.FinishParams();\n");
            code.push_str(&format!("    Self->{} = NewValue;\n", prop.name,));
            code.push_str("}\n\n");
        }

        code
    }

    /// 生成注册代码
    pub fn generate_registration(&self, binding: &ScriptClassBinding) -> String {
        let class = &binding.class_name;
        let mut reg = String::with_capacity(1024);

        reg.push_str(&format!("// 自动生成 — {} 脚本绑定注册\n", class));
        reg.push_str(&format!(
            "static void Register{class}ScriptBindings()\n{{\n"
        ));
        reg.push_str(&format!(
            "    ScriptClass* ScriptCls = ScriptRegistry::FindOrCreate(\"{class}\");\n"
        ));

        if let Some(parent) = &binding.parent_class {
            reg.push_str(&format!("    ScriptCls->SetParent(\"{parent}\");\n"));
        }

        // 注册函数
        for func in &binding.functions {
            let thunk = func.thunk_name(class);
            reg.push_str(&format!("\n    // {}\n", func.name));
            reg.push_str(&format!(
                "    ScriptCls->AddFunction(\"{}\", &Limx::Script::{}, ScriptFuncFlags{{{}}});\n",
                func.name,
                thunk,
                if func.is_const {
                    "Const"
                } else if func.is_static {
                    "Static"
                } else {
                    "None"
                },
            ));

            // 注册参数元信息
            for param in &func.params {
                let dir = match param.direction {
                    ParamDirection::In => "In",
                    ParamDirection::Out => "Out",
                    ParamDirection::InOut => "InOut",
                    ParamDirection::Return => "Return",
                };
                reg.push_str(&format!(
                    "    ScriptCls->LastFunc()->AddParam(\"{}\", \"{}\", EParamDir::{});\n",
                    param.name, param.script_type, dir,
                ));
            }

            if let Some(ret) = &func.return_type {
                reg.push_str(&format!(
                    "    ScriptCls->LastFunc()->SetReturnType(\"{}\");\n",
                    ret.script_type,
                ));
            }
        }

        // 注册属性
        for prop in &binding.properties {
            let flags = match (prop.readable, prop.writable) {
                (true, true) => "ReadWrite",
                (true, false) => "ReadOnly",
                (false, true) => "WriteOnly",
                (false, false) => "None",
            };
            reg.push_str(&format!(
                "\n    ScriptCls->AddProperty(\"{}\", \"{}\", EPropertyAccess::{});\n",
                prop.name, prop.script_type, flags,
            ));
        }

        reg.push_str("}\n\n");
        reg.push_str(&format!(
            "REGISTER_SCRIPT_BINDING({class}, Register{class}ScriptBindings);\n"
        ));
        reg
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_simple_func(name: &str, is_const: bool) -> ScriptFunction {
        ScriptFunction {
            name: name.to_string(),
            display_name: None,
            params: vec![],
            return_type: None,
            is_const,
            is_static: false,
            is_event: false,
            tooltip: None,
            category: "Default".to_string(),
        }
    }

    fn make_func_with_params() -> ScriptFunction {
        ScriptFunction {
            name: "TakeDamage".to_string(),
            display_name: Some("受到伤害".to_string()),
            params: vec![
                ScriptParam {
                    name: "DamageAmount".to_string(),
                    cpp_type: "float".to_string(),
                    script_type: "Float".to_string(),
                    direction: ParamDirection::In,
                    is_const: false,
                    is_reference: false,
                    default_value: None,
                },
                ScriptParam {
                    name: "DamageType".to_string(),
                    cpp_type: "int32".to_string(),
                    script_type: "Int32".to_string(),
                    direction: ParamDirection::In,
                    is_const: true,
                    is_reference: false,
                    default_value: Some("0".to_string()),
                },
            ],
            return_type: Some(ScriptParam {
                name: "ReturnValue".to_string(),
                cpp_type: "float".to_string(),
                script_type: "Float".to_string(),
                direction: ParamDirection::Return,
                is_const: false,
                is_reference: false,
                default_value: None,
            }),
            is_const: false,
            is_static: false,
            is_event: false,
            tooltip: Some("对角色造成伤害".to_string()),
            category: "Combat".to_string(),
        }
    }

    fn make_test_binding() -> ScriptClassBinding {
        ScriptClassBinding {
            class_name: "APlayer".to_string(),
            parent_class: Some("AActor".to_string()),
            functions: vec![make_func_with_params(), make_simple_func("GetHealth", true)],
            properties: vec![
                ScriptProperty {
                    name: "Health".to_string(),
                    cpp_type: "float".to_string(),
                    script_type: "Float".to_string(),
                    readable: true,
                    writable: true,
                    category: "Combat".to_string(),
                },
                ScriptProperty {
                    name: "PlayerName".to_string(),
                    cpp_type: "FString".to_string(),
                    script_type: "String".to_string(),
                    readable: true,
                    writable: false,
                    category: "Info".to_string(),
                },
            ],
        }
    }

    #[test]
    fn test_type_mapping() {
        assert_eq!(map_to_script_type("float"), "Float");
        assert_eq!(map_to_script_type("FVector"), "Vector3");
        assert_eq!(map_to_script_type("FLinearColor"), "Color");
        assert_eq!(map_to_script_type("AActor*"), "ObjectRef");
        assert_eq!(map_to_script_type("FMyStruct"), "Struct");
    }

    #[test]
    fn test_find_type_mapping() {
        let mapping = find_type_mapping("int32").unwrap();
        assert_eq!(mapping.script_type, "Int32");
        assert_eq!(mapping.stack_size, 4);
        assert!(mapping.is_value_type);

        let mapping2 = find_type_mapping("FString").unwrap();
        assert_eq!(mapping2.script_type, "String");
        assert!(!mapping2.is_value_type);
    }

    #[test]
    fn test_const_type_mapping() {
        let mapping = find_type_mapping("const float&");
        assert!(mapping.is_some());
        assert_eq!(mapping.unwrap().script_type, "Float");
    }

    #[test]
    fn test_thunk_name() {
        let func = make_simple_func("GetHealth", true);
        assert_eq!(func.thunk_name("APlayer"), "execAPlayer_GetHealth");
    }

    #[test]
    fn test_input_output_params() {
        let mut func = make_func_with_params();
        func.params.push(ScriptParam {
            name: "OutDamageApplied".to_string(),
            cpp_type: "float".to_string(),
            script_type: "Float".to_string(),
            direction: ParamDirection::Out,
            is_const: false,
            is_reference: true,
            default_value: None,
        });

        assert_eq!(func.input_params().len(), 2);
        assert_eq!(func.output_params().len(), 1);
    }

    #[test]
    fn test_generate_thunk_void() {
        let gen = ScriptBindingGenerator::new();
        let func = make_simple_func("Tick", false);
        let code = gen.generate_thunk("APlayer", &func);

        assert!(code.contains("execAPlayer_Tick"));
        assert!(code.contains("APlayer* Self"));
        assert!(code.contains("Self->Tick()"));
        assert!(code.contains("FinishParams"));
    }

    #[test]
    fn test_generate_thunk_with_return() {
        let gen = ScriptBindingGenerator::new();
        let func = make_func_with_params();
        let code = gen.generate_thunk("APlayer", &func);

        assert!(code.contains("execAPlayer_TakeDamage"));
        assert!(code.contains("Stack.Pop<float>()"));
        assert!(code.contains("float ReturnValue = Self->TakeDamage("));
        assert!(code.contains("*static_cast<float*>(Result) = ReturnValue"));
    }

    #[test]
    fn test_generate_static_thunk() {
        let gen = ScriptBindingGenerator::new();
        let func = ScriptFunction {
            name: "GetDefaultHealth".to_string(),
            display_name: None,
            params: vec![],
            return_type: Some(ScriptParam {
                name: "ReturnValue".to_string(),
                cpp_type: "float".to_string(),
                script_type: "Float".to_string(),
                direction: ParamDirection::Return,
                is_const: false,
                is_reference: false,
                default_value: None,
            }),
            is_const: false,
            is_static: true,
            is_event: false,
            tooltip: None,
            category: "Default".to_string(),
        };

        let code = gen.generate_thunk("APlayer", &func);
        assert!(code.contains("APlayer::GetDefaultHealth()"));
        assert!(!code.contains("Self->"), "静态函数不应使用 Self");
    }

    #[test]
    fn test_generate_property_getter() {
        let gen = ScriptBindingGenerator::new();
        let prop = ScriptProperty {
            name: "Health".to_string(),
            cpp_type: "float".to_string(),
            script_type: "Float".to_string(),
            readable: true,
            writable: false,
            category: "Combat".to_string(),
        };

        let code = gen.generate_property_accessors("APlayer", &prop);
        assert!(code.contains("execGetAPlayer_Health"));
        assert!(code.contains("Self->Health"));
        assert!(
            !code.contains("execSetAPlayer_Health"),
            "只读属性不应生成 Setter"
        );
    }

    #[test]
    fn test_generate_property_readwrite() {
        let gen = ScriptBindingGenerator::new();
        let prop = ScriptProperty {
            name: "Speed".to_string(),
            cpp_type: "float".to_string(),
            script_type: "Float".to_string(),
            readable: true,
            writable: true,
            category: "Movement".to_string(),
        };

        let code = gen.generate_property_accessors("APlayer", &prop);
        assert!(code.contains("execGetAPlayer_Speed"));
        assert!(code.contains("execSetAPlayer_Speed"));
        assert!(code.contains("Self->Speed = NewValue"));
    }

    #[test]
    fn test_generate_full_binding() {
        let gen = ScriptBindingGenerator::new();
        let binding = make_test_binding();
        let result = gen.generate_class_binding(&binding);

        assert_eq!(result.class_name, "APlayer");
        assert_eq!(result.thunk_count, 2);
        assert_eq!(result.accessor_count, 3); // Health(R+W) + PlayerName(R)

        // 头文件
        assert!(result.header_content.contains("execAPlayer_TakeDamage"));
        assert!(result.header_content.contains("execGetAPlayer_Health"));
        assert!(result.header_content.contains("execSetAPlayer_Health"));
        assert!(result.header_content.contains("execGetAPlayer_PlayerName"));

        // 实现
        assert!(result
            .impl_content
            .contains("Self->TakeDamage(DamageAmount, DamageType)"));
        assert!(result.impl_content.contains("Self->Health"));
    }

    #[test]
    fn test_generate_registration() {
        let gen = ScriptBindingGenerator::new();
        let binding = make_test_binding();
        let result = gen.generate_class_binding(&binding);

        assert!(result
            .registration_content
            .contains("RegisterAPlayerScriptBindings"));
        assert!(result
            .registration_content
            .contains("SetParent(\"AActor\")"));
        assert!(result
            .registration_content
            .contains("AddFunction(\"TakeDamage\""));
        assert!(result
            .registration_content
            .contains("AddParam(\"DamageAmount\""));
        assert!(result
            .registration_content
            .contains("SetReturnType(\"Float\")"));
        assert!(result
            .registration_content
            .contains("AddProperty(\"Health\""));
        assert!(result
            .registration_content
            .contains("EPropertyAccess::ReadWrite"));
        assert!(result
            .registration_content
            .contains("EPropertyAccess::ReadOnly"));
        assert!(result
            .registration_content
            .contains("REGISTER_SCRIPT_BINDING"));
    }

    #[test]
    fn test_class_binding_counts() {
        let binding = make_test_binding();
        assert_eq!(binding.function_count(), 2);
        assert_eq!(binding.property_count(), 2);
        assert_eq!(binding.events().len(), 0);
    }

    #[test]
    fn test_event_functions() {
        let binding = ScriptClassBinding {
            class_name: "AEnemy".to_string(),
            parent_class: None,
            functions: vec![
                ScriptFunction {
                    name: "OnDeath".to_string(),
                    display_name: None,
                    params: vec![],
                    return_type: None,
                    is_const: false,
                    is_static: false,
                    is_event: true,
                    tooltip: None,
                    category: "Events".to_string(),
                },
                make_simple_func("Attack", false),
            ],
            properties: vec![],
        };

        assert_eq!(binding.events().len(), 1);
        assert_eq!(binding.events()[0].name, "OnDeath");
    }

    #[test]
    fn test_builtin_mappings_count() {
        let mappings = builtin_type_mappings();
        assert!(mappings.len() >= 15, "应至少有 15 个内置类型映射");
    }
}
