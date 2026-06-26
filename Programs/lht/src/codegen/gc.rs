/*******************************************************************************
 * 文件: codegen/gc.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   垃圾回收代码生成器 - 生成 GC 支持代码
 *   - 对象引用追踪
 *   - GC 标记函数
 *   - 弱引用支持
 *   - GC 根对象管理
 *
 * 设计哲学:
 *   1. 精确 GC - 不依赖保守扫描
 *   2. 增量 GC - 支持分代收集
 *   3. 低延迟 - 最小化暂停时间
 *
 * 技术特性:
 *   - 编译时生成引用遍历代码
 *   - 支持循环引用检测
 *   - 支持弱引用和软引用
 *   - 与序列化系统集成
 *
 ******************************************************************************/

use super::adapter::{ClassDeclExt, CppTypeExt, FieldDeclExt};
use crate::parser::ast::{ClassDecl, FieldDecl};
use std::collections::HashSet;

//=============================================================================
// GC 配置
//=============================================================================

/// GC 属性类型
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GcPropertyType {
    /// 强引用 - 阻止对象被回收
    Strong,
    /// 弱引用 - 不阻止对象被回收
    Weak,
    /// 软引用 - 内存压力时可被回收
    Soft,
    /// 非 GC 属性
    None,
}

/// GC 配置
#[derive(Debug, Clone)]
pub struct GcConfig {
    /// 是否启用分代 GC
    pub generational: bool,
    /// 是否启用增量 GC
    pub incremental: bool,
    /// 是否生成调试信息
    pub debug_info: bool,
    /// 是否支持弱引用回调
    pub weak_callbacks: bool,
}

impl Default for GcConfig {
    fn default() -> Self {
        Self {
            generational: true,
            incremental: true,
            debug_info: false,
            weak_callbacks: true,
        }
    }
}

//=============================================================================
// GC 代码生成器
//=============================================================================

/// GC 代码生成器
pub struct GcCodeGenerator {
    /// 配置
    config: GcConfig,
    /// 已知的 GC 托管类型
    gc_types: HashSet<String>,
}

impl GcCodeGenerator {
    pub fn new(config: GcConfig) -> Self {
        Self {
            config,
            gc_types: HashSet::new(),
        }
    }

    /// 注册 GC 托管类型
    pub fn register_gc_type(&mut self, type_name: &str) {
        self.gc_types.insert(type_name.to_string());
    }

    /// 生成类的 GC 支持代码
    pub fn generate_class_gc(&self, class: &ClassDecl) -> String {
        let mut code = String::with_capacity(4096);

        // GC 引用遍历函数
        code.push_str(&self.generate_add_referenced_objects(class));
        code.push('\n');

        // GC 标记函数
        code.push_str(&self.generate_gc_mark(class));
        code.push('\n');

        // 弱引用回调
        if self.config.weak_callbacks {
            code.push_str(&self.generate_weak_reference_callbacks(class));
            code.push('\n');
        }

        // GC 描述符
        code.push_str(&self.generate_gc_descriptor(class));
        code.push('\n');

        // 根对象管理
        code.push_str(&self.generate_root_management(class));

        code
    }

    /// 生成引用对象遍历函数
    fn generate_add_referenced_objects(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!(
            "void {}::AddReferencedObjects(FReferenceCollector& Collector)\n{{\n",
            class.name
        ));

        // 调用父类
        if let Some(base) = class.get_first_base() {
            code.push_str(&format!(
                "    {}::AddReferencedObjects(Collector);\n\n",
                base
            ));
        }

        // 遍历所有 GC 相关属性
        let gc_properties: Vec<_> = class
            .get_fields()
            .into_iter()
            .filter(|p| self.is_gc_property(p))
            .collect();

        if gc_properties.is_empty() {
            code.push_str("    // 无 GC 托管引用\n");
        } else {
            code.push_str("    // 添加 GC 托管引用\n");
            for prop in gc_properties {
                code.push_str(&self.generate_property_reference(prop));
            }
        }

        code.push_str("}\n");
        code
    }

    /// 生成属性引用代码
    fn generate_property_reference(&self, prop: &FieldDecl) -> String {
        let gc_type = self.get_gc_property_type(prop);

        match gc_type {
            GcPropertyType::Strong => {
                if self.is_container_type(&prop.field_type.to_string()) {
                    self.generate_container_reference(prop)
                } else if self.is_pointer_type(&prop.field_type.to_string()) {
                    format!("    Collector.AddReferencedObject({});\n", prop.name)
                } else {
                    format!("    {}.AddReferencedObjects(Collector);\n", prop.name)
                }
            }
            GcPropertyType::Weak => {
                format!("    // 弱引用: {} (不参与引用计数)\n", prop.name)
            }
            GcPropertyType::Soft => {
                format!("    Collector.AddSoftReference({});\n", prop.name)
            }
            GcPropertyType::None => String::new(),
        }
    }

    /// 生成容器引用代码
    fn generate_container_reference(&self, prop: &FieldDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!("    for (auto& Element : {})\n", prop.name));
        code.push_str("    {\n");

        if prop.field_type.to_string().contains("Map") {
            code.push_str("        Collector.AddReferencedObject(Element.Value);\n");
        } else {
            code.push_str("        Collector.AddReferencedObject(Element);\n");
        }

        code.push_str("    }\n");

        code
    }

    /// 生成 GC 标记函数
    fn generate_gc_mark(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!(
            "void {}::GcMark(UInt32 MarkGeneration)\n{{\n",
            class.name
        ));

        code.push_str("    // 检查是否已标记\n");
        code.push_str("    if (m_GcMark == MarkGeneration)\n");
        code.push_str("        return;\n\n");

        code.push_str("    // 设置标记\n");
        code.push_str("    m_GcMark = MarkGeneration;\n\n");

        // 标记所有引用对象
        let gc_properties: Vec<_> = class
            .get_fields()
            .into_iter()
            .filter(|p| {
                self.is_gc_property(p) && self.get_gc_property_type(p) == GcPropertyType::Strong
            })
            .collect();

        if !gc_properties.is_empty() {
            code.push_str("    // 递归标记引用对象\n");
            for prop in gc_properties {
                code.push_str(&self.generate_property_mark(prop));
            }
        }

        code.push_str("}\n");
        code
    }

    /// 生成属性标记代码
    fn generate_property_mark(&self, prop: &FieldDecl) -> String {
        let type_str = prop.field_type.to_string();
        if self.is_container_type(&type_str) {
            let mut code = String::new();
            code.push_str(&format!("    for (auto& Element : {})\n", prop.name));
            code.push_str("    {\n");
            code.push_str("        if (Element) Element->GcMark(MarkGeneration);\n");
            code.push_str("    }\n");
            code
        } else if self.is_pointer_type(&type_str) {
            format!(
                "    if ({}) {}->GcMark(MarkGeneration);\n",
                prop.name, prop.name
            )
        } else {
            format!("    {}.GcMark(MarkGeneration);\n", prop.name)
        }
    }

    /// 生成弱引用回调
    fn generate_weak_reference_callbacks(&self, class: &ClassDecl) -> String {
        let weak_props: Vec<_> = class
            .get_fields()
            .into_iter()
            .filter(|p| self.get_gc_property_type(p) == GcPropertyType::Weak)
            .collect();

        if weak_props.is_empty() {
            return String::new();
        }

        let mut code = String::new();

        code.push_str(&format!(
            "void {}::OnWeakReferenceCleared(LObject* Object)\n{{\n",
            class.name
        ));

        for prop in weak_props {
            code.push_str(&format!("    if ({}.Get() == Object)\n", prop.name));
            code.push_str("    {\n");
            code.push_str(&format!("        {}.Reset();\n", prop.name));

            // 检查是否有回调
            if let Some(callback) = prop.get_meta_value("OnCleared") {
                code.push_str(&format!("        {}();\n", callback));
            }

            code.push_str("    }\n");
        }

        code.push_str("}\n");
        code
    }

    /// 生成 GC 描述符
    fn generate_gc_descriptor(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!(
            "const GcTypeDescriptor* {}::GetGcDescriptor() const\n{{\n",
            class.name
        ));
        code.push_str(&format!("    static GcTypeDescriptor s_Descriptor = {{\n"));
        code.push_str(&format!("        .TypeName = \"{}\",\n", class.name));
        code.push_str(&format!("        .TypeSize = sizeof({}),\n", class.name));

        // 计算引用偏移量
        let gc_properties: Vec<_> = class
            .get_fields()
            .into_iter()
            .filter(|p| self.is_gc_property(p))
            .collect();

        code.push_str(&format!(
            "        .ReferenceCount = {},\n",
            gc_properties.len()
        ));

        // 标志
        let mut flags = Vec::new();
        if class.has_base() {
            flags.push("GcFlags::HasBaseClass");
        }
        if gc_properties
            .iter()
            .any(|p| self.get_gc_property_type(p) == GcPropertyType::Weak)
        {
            flags.push("GcFlags::HasWeakRefs");
        }
        if self.config.generational {
            flags.push("GcFlags::Generational");
        }

        let flags_str = if flags.is_empty() {
            "GcFlags::None".to_string()
        } else {
            flags.join(" | ")
        };
        code.push_str(&format!("        .Flags = {},\n", flags_str));

        code.push_str("    };\n");
        code.push_str("    return &s_Descriptor;\n");
        code.push_str("}\n");

        code
    }

    /// 生成根对象管理
    fn generate_root_management(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        // AddToRoot
        code.push_str(&format!("void {}::AddToRoot()\n{{\n", class.name));
        code.push_str("    if (!HasAnyFlags(ObjectFlags::RootSet))\n");
        code.push_str("    {\n");
        code.push_str("        SetFlags(ObjectFlags::RootSet);\n");
        code.push_str("        GGCRootSet.Add(this);\n");
        code.push_str("    }\n");
        code.push_str("}\n\n");

        // RemoveFromRoot
        code.push_str(&format!("void {}::RemoveFromRoot()\n{{\n", class.name));
        code.push_str("    if (HasAnyFlags(ObjectFlags::RootSet))\n");
        code.push_str("    {\n");
        code.push_str("        ClearFlags(ObjectFlags::RootSet);\n");
        code.push_str("        GGCRootSet.Remove(this);\n");
        code.push_str("    }\n");
        code.push_str("}\n\n");

        // IsRooted
        code.push_str(&format!("bool {}::IsRooted() const\n{{\n", class.name));
        code.push_str("    return HasAnyFlags(ObjectFlags::RootSet);\n");
        code.push_str("}\n");

        code
    }

    /// 检查属性是否参与 GC
    fn is_gc_property(&self, prop: &FieldDecl) -> bool {
        // 跳过 Transient 属性
        if prop.is_transient_field() {
            return false;
        }

        // 检查类型是否为 GC 托管
        self.is_gc_managed_type(&prop.field_type.to_string())
    }

    /// 检查类型是否为 GC 托管
    fn is_gc_managed_type(&self, type_name: &str) -> bool {
        // 指针类型
        if type_name.contains("TObjectPtr")
            || type_name.contains("TWeakObjectPtr")
            || type_name.contains("TSoftObjectPtr")
        {
            return true;
        }

        // 原始指针指向已知 GC 类型
        if type_name.contains('*') {
            let base_type = self.extract_base_type(type_name);
            return self.gc_types.contains(&base_type);
        }

        // 容器类型
        if type_name.contains("TArray") || type_name.contains("TMap") || type_name.contains("TSet")
        {
            // 检查元素类型
            if let Some(element_type) = self.extract_template_arg(type_name) {
                return self.is_gc_managed_type(&element_type);
            }
        }

        false
    }

    /// 获取 GC 属性类型
    fn get_gc_property_type(&self, prop: &FieldDecl) -> GcPropertyType {
        let type_str = prop.field_type.to_string();
        if type_str.contains("TWeakObjectPtr") {
            GcPropertyType::Weak
        } else if type_str.contains("TSoftObjectPtr") {
            GcPropertyType::Soft
        } else if self.is_gc_property(prop) {
            GcPropertyType::Strong
        } else {
            GcPropertyType::None
        }
    }

    /// 检查是否为容器类型
    fn is_container_type(&self, type_name: &str) -> bool {
        type_name.contains("TArray") || type_name.contains("TMap") || type_name.contains("TSet")
    }

    /// 检查是否为指针类型
    fn is_pointer_type(&self, type_name: &str) -> bool {
        type_name.contains('*')
            || type_name.contains("TObjectPtr")
            || type_name.contains("TWeakObjectPtr")
            || type_name.contains("TSoftObjectPtr")
    }

    /// 提取基类型
    fn extract_base_type(&self, type_name: &str) -> String {
        type_name.replace('*', "").trim().to_string()
    }

    /// 提取模板参数
    fn extract_template_arg(&self, type_name: &str) -> Option<String> {
        if let Some(start) = type_name.find('<') {
            if let Some(end) = type_name.rfind('>') {
                return Some(type_name[start + 1..end].trim().to_string());
            }
        }
        None
    }
}

impl Default for GcCodeGenerator {
    fn default() -> Self {
        Self::new(GcConfig::default())
    }
}

//=============================================================================
// GC 安全分析
//=============================================================================

/// GC 安全检查器
pub struct GcSafetyChecker {
    /// 不安全的操作警告
    warnings: Vec<GcSafetyWarning>,
}

/// GC 安全警告
#[derive(Debug, Clone)]
pub struct GcSafetyWarning {
    pub class_name: String,
    pub property_name: String,
    pub warning_type: GcWarningType,
    pub message: String,
}

/// GC 警告类型
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GcWarningType {
    /// 原始指针指向 GC 对象
    RawPointerToGcObject,
    /// 可能的悬空引用
    PotentialDanglingReference,
    /// 循环强引用
    CircularStrongReference,
    /// 未追踪的引用
    UntrackedReference,
}

impl GcSafetyChecker {
    pub fn new() -> Self {
        Self {
            warnings: Vec::new(),
        }
    }

    /// 检查类的 GC 安全性
    pub fn check_class(&mut self, class: &ClassDecl, gc_types: &HashSet<String>) {
        for prop in class.get_fields() {
            self.check_property(class, prop, gc_types);
        }
    }

    /// 检查属性的 GC 安全性
    fn check_property(&mut self, class: &ClassDecl, prop: &FieldDecl, gc_types: &HashSet<String>) {
        // 检查原始指针
        let type_str = prop.field_type.to_string();
        if type_str.contains('*') && !type_str.contains("TObjectPtr") {
            let base_type = type_str.replace('*', "").trim().to_string();
            if gc_types.contains(&base_type) {
                self.warnings.push(GcSafetyWarning {
                    class_name: class.name.clone(),
                    property_name: prop.name.clone(),
                    warning_type: GcWarningType::RawPointerToGcObject,
                    message: format!(
                        "属性 '{}' 使用原始指针指向 GC 对象 '{}',建议使用 TObjectPtr",
                        prop.name, base_type
                    ),
                });
            }
        }

        // 检查 Transient 指针
        if prop.is_transient_field() && (type_str.contains("TObjectPtr") || type_str.contains('*'))
        {
            self.warnings.push(GcSafetyWarning {
                class_name: class.name.clone(),
                property_name: prop.name.clone(),
                warning_type: GcWarningType::UntrackedReference,
                message: format!(
                    "Transient 属性 '{}' 包含对象引用，GC 期间可能导致悬空指针",
                    prop.name
                ),
            });
        }
    }

    /// 获取所有警告
    pub fn warnings(&self) -> &[GcSafetyWarning] {
        &self.warnings
    }

    /// 清除警告
    pub fn clear(&mut self) {
        self.warnings.clear();
    }

    /// 打印警告
    pub fn print_warnings(&self) {
        if self.warnings.is_empty() {
            return;
        }

        println!("\nGC 安全警告 ({} 个):", self.warnings.len());
        for warning in &self.warnings {
            println!(
                "  [{:?}] {}::{}: {}",
                warning.warning_type, warning.class_name, warning.property_name, warning.message
            );
        }
    }
}

impl Default for GcSafetyChecker {
    fn default() -> Self {
        Self::new()
    }
}
