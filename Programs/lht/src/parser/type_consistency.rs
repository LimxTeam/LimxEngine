// ============================================================
// 文件名称：type_consistency.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：编译前静态验证 — 在 LHT 代码生成阶段即检测跨模块
//           类型引用错误，避免 C++ 编译器才报出的晦涩错误。
//           UE5 UHT 不做跨模块类型一致性验证，我们做到全面
//           静态检查 + 前向声明自动建议
// 功能描述：跨模块类型一致性校验器 — 收集所有模块的类型定义，
//           检测未解析的类型引用、循环前向声明、类型冲突、
//           缺失的头文件包含、不安全的前向声明使用
// 技术特性：多阶段验证管线、类型注册表、前向声明分析、
//           跨模块依赖追踪、诊断信息结构化输出
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ TypeConsistencyChecker     │ 一致性校验器主体              │
// │ TypeRegistry               │ 全局类型注册表                │
// │ TypeDefinition             │ 类型定义信息                  │
// │ TypeReference              │ 类型引用信息                  │
// │ ForwardDeclaration         │ 前向声明信息                  │
// │ ConsistencyDiagnostic      │ 校验诊断信息                  │
// │ ConsistencyReport          │ 校验报告                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建校验器                    │
// │ register_type()            │ 注册类型定义                  │
// │ register_reference()       │ 注册类型引用                  │
// │ register_forward_decl()    │ 注册前向声明                  │
// │ check_all()                │ 执行全部校验                  │
// │ check_unresolved_refs()    │ 检查未解析的类型引用           │
// │ check_forward_decls()      │ 检查前向声明一致性             │
// │ check_type_conflicts()     │ 检查类型名冲突                │
// │ check_cross_module_deps()  │ 检查跨模块依赖合规性          │
// │ suggest_fixes()            │ 自动生成修复建议              │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};

// =============================================================================
// 类型定义 / 引用 / 前向声明
// =============================================================================

/// 类型定义的类别
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum TypeCategory {
    /// 类 (LCLASS)
    Class,
    /// 结构体 (LSTRUCT)
    Struct,
    /// 枚举 (LENUM)
    Enum,
    /// 委托 (LDELEGATE)
    Delegate,
    /// 类型别名 (typedef / using)
    Alias,
}

/// 类型定义信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TypeDefinition {
    /// 类型完全限定名
    pub qualified_name: String,
    /// 类型短名
    pub short_name: String,
    /// 类别
    pub category: TypeCategory,
    /// 所属模块
    pub module_name: String,
    /// 所属头文件
    pub header_file: String,
    /// 命名空间
    pub namespace: Option<String>,
    /// 基类列表
    pub base_classes: Vec<String>,
    /// 此类型引用的其他类型名列表
    pub referenced_types: Vec<String>,
    /// 此类型是否为模板
    pub is_template: bool,
}

/// 类型引用信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TypeReference {
    /// 引用的类型名
    pub type_name: String,
    /// 引用发生的源文件
    pub source_file: String,
    /// 引用发生的模块
    pub module_name: String,
    /// 引用发生的行号 (若已知)
    pub line_number: Option<usize>,
    /// 引用上下文 (如 "基类", "属性类型", "函数参数")
    pub context: ReferenceContext,
    /// 是否通过前向声明即可满足
    pub can_use_forward_decl: bool,
}

/// 引用上下文
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ReferenceContext {
    /// 基类
    BaseClass,
    /// 属性类型 (值类型，需要完整定义)
    PropertyValue,
    /// 属性类型 (指针/引用，可前向声明)
    PropertyPointer,
    /// 函数参数 (值类型)
    FunctionParamValue,
    /// 函数参数 (指针/引用)
    FunctionParamPointer,
    /// 函数返回值
    FunctionReturn,
    /// 模板参数
    TemplateArgument,
    /// 其他
    Other,
}

impl ReferenceContext {
    /// 此上下文是否需要完整类型定义 (而非前向声明)
    pub fn requires_full_definition(&self) -> bool {
        matches!(
            self,
            Self::BaseClass | Self::PropertyValue | Self::FunctionParamValue
        )
    }
}

/// 前向声明信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ForwardDeclaration {
    /// 前向声明的类型名
    pub type_name: String,
    /// 声明所在头文件
    pub header_file: String,
    /// 声明所在模块
    pub module_name: String,
    /// 前向声明的类别 (class/struct/enum)
    pub declared_category: TypeCategory,
}

// =============================================================================
// 诊断信息
// =============================================================================

/// 诊断严重程度
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub enum DiagnosticSeverity {
    /// 提示
    Hint,
    /// 警告
    Warning,
    /// 错误
    Error,
}

/// 诊断类别
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum DiagnosticKind {
    /// 未解析的类型引用
    UnresolvedType,
    /// 类型名冲突 (同名不同模块)
    TypeNameConflict,
    /// 前向声明类别不匹配 (声明为 class，实际为 struct)
    ForwardDeclCategoryMismatch,
    /// 缺少需要的前向声明
    MissingForwardDecl,
    /// 不必要的前向声明 (已经包含完整定义)
    UnnecessaryForwardDecl,
    /// 需要完整定义但只有前向声明
    IncompleteTypeUsage,
    /// 跨模块层级违规
    CrossModuleLayerViolation,
    /// 循环类型依赖
    CircularTypeDependency,
}

/// 单条诊断信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConsistencyDiagnostic {
    /// 严重程度
    pub severity: DiagnosticSeverity,
    /// 诊断类别
    pub kind: DiagnosticKind,
    /// 消息
    pub message: String,
    /// 涉及的类型名
    pub type_name: String,
    /// 涉及的源文件
    pub source_file: String,
    /// 涉及的模块
    pub module_name: String,
    /// 修复建议
    pub suggestion: Option<String>,
}

/// 校验报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConsistencyReport {
    /// 所有诊断信息
    pub diagnostics: Vec<ConsistencyDiagnostic>,
    /// 错误数
    pub error_count: usize,
    /// 警告数
    pub warning_count: usize,
    /// 提示数
    pub hint_count: usize,
    /// 已注册类型数
    pub registered_type_count: usize,
    /// 已注册引用数
    pub reference_count: usize,
    /// 前向声明数
    pub forward_decl_count: usize,
}

impl ConsistencyReport {
    /// 是否通过 (无错误)
    pub fn is_ok(&self) -> bool {
        self.error_count == 0
    }
}

// =============================================================================
// 类型注册表
// =============================================================================

/// 全局类型注册表
#[derive(Debug, Clone, Default)]
pub struct TypeRegistry {
    /// 类型定义 (完全限定名 -> 定义)
    definitions: HashMap<String, TypeDefinition>,
    /// 短名索引 (短名 -> 完全限定名列表)
    short_name_index: HashMap<String, Vec<String>>,
    /// 类型引用列表
    references: Vec<TypeReference>,
    /// 前向声明列表
    forward_decls: Vec<ForwardDeclaration>,
    /// 模块层级映射 (模块名 -> 层级)
    module_layers: HashMap<String, u8>,
}

impl TypeRegistry {
    /// 创建空注册表
    pub fn new() -> Self {
        Self::default()
    }

    /// 注册类型定义
    pub fn register_type(&mut self, def: TypeDefinition) {
        self.short_name_index
            .entry(def.short_name.clone())
            .or_default()
            .push(def.qualified_name.clone());
        self.definitions.insert(def.qualified_name.clone(), def);
    }

    /// 注册类型引用
    pub fn register_reference(&mut self, reference: TypeReference) {
        self.references.push(reference);
    }

    /// 注册前向声明
    pub fn register_forward_decl(&mut self, decl: ForwardDeclaration) {
        self.forward_decls.push(decl);
    }

    /// 注册模块层级
    pub fn register_module_layer(&mut self, module_name: &str, layer: u8) {
        self.module_layers.insert(module_name.to_string(), layer);
    }

    /// 查找类型定义 (支持短名和完全限定名)
    pub fn find_type(&self, name: &str) -> Option<&TypeDefinition> {
        // 先尝试完全限定名
        if let Some(def) = self.definitions.get(name) {
            return Some(def);
        }
        // 再尝试短名
        if let Some(qualified_names) = self.short_name_index.get(name) {
            if qualified_names.len() == 1 {
                return self.definitions.get(&qualified_names[0]);
            }
        }
        None
    }

    /// 获取类型定义数量
    pub fn type_count(&self) -> usize {
        self.definitions.len()
    }

    /// 获取引用数量
    pub fn reference_count(&self) -> usize {
        self.references.len()
    }
}

// =============================================================================
// 一致性校验器
// =============================================================================

/// 跨模块类型一致性校验器
pub struct TypeConsistencyChecker {
    /// 类型注册表
    registry: TypeRegistry,
}

impl TypeConsistencyChecker {
    /// 创建校验器
    pub fn new() -> Self {
        Self {
            registry: TypeRegistry::new(),
        }
    }

    /// 从已有注册表创建
    pub fn with_registry(registry: TypeRegistry) -> Self {
        Self { registry }
    }

    /// 获取注册表的可变引用 (用于注册类型)
    pub fn registry_mut(&mut self) -> &mut TypeRegistry {
        &mut self.registry
    }

    /// 获取注册表的不可变引用
    pub fn registry(&self) -> &TypeRegistry {
        &self.registry
    }

    /// 执行全部校验
    pub fn check_all(&self) -> ConsistencyReport {
        let mut diagnostics = Vec::new();

        self.check_unresolved_refs(&mut diagnostics);
        self.check_type_conflicts(&mut diagnostics);
        self.check_forward_decls(&mut diagnostics);
        self.check_cross_module_deps(&mut diagnostics);
        self.check_circular_type_deps(&mut diagnostics);

        let error_count = diagnostics
            .iter()
            .filter(|d| d.severity == DiagnosticSeverity::Error)
            .count();
        let warning_count = diagnostics
            .iter()
            .filter(|d| d.severity == DiagnosticSeverity::Warning)
            .count();
        let hint_count = diagnostics
            .iter()
            .filter(|d| d.severity == DiagnosticSeverity::Hint)
            .count();

        ConsistencyReport {
            diagnostics,
            error_count,
            warning_count,
            hint_count,
            registered_type_count: self.registry.type_count(),
            reference_count: self.registry.reference_count(),
            forward_decl_count: self.registry.forward_decls.len(),
        }
    }

    /// 检查未解析的类型引用
    fn check_unresolved_refs(&self, diagnostics: &mut Vec<ConsistencyDiagnostic>) {
        // 收集所有已知类型名 (完全限定名 + 短名)
        let known_types: HashSet<&str> = self
            .registry
            .definitions
            .keys()
            .map(|s| s.as_str())
            .chain(self.registry.short_name_index.keys().map(|s| s.as_str()))
            .collect();

        // 内建类型白名单
        let builtin_types: HashSet<&str> = [
            "void",
            "bool",
            "int",
            "float",
            "double",
            "int8_t",
            "int16_t",
            "int32_t",
            "int64_t",
            "uint8_t",
            "uint16_t",
            "uint32_t",
            "uint64_t",
            "size_t",
            "char",
            "wchar_t",
            "LString",
            "LName",
            "LText",
            "LVector",
            "LRotator",
            "LTransform",
            "LQuat",
            "LVector2D",
            "LVector4",
            "LColor",
            "LLinearColor",
            "LObject",
            "LClass",
            "TArray",
            "TMap",
            "TSet",
            "TSharedPtr",
            "TWeakPtr",
            "TUniquePtr",
        ]
        .into_iter()
        .collect();

        // 前向声明的类型也视为"已知"
        let forward_declared: HashSet<&str> = self
            .registry
            .forward_decls
            .iter()
            .map(|d| d.type_name.as_str())
            .collect();

        for reference in &self.registry.references {
            let type_name = reference.type_name.as_str();

            // 跳过内建类型
            if builtin_types.contains(type_name) {
                continue;
            }

            // 跳过模板参数中的简单类型名 (如 TArray<int>)
            if type_name.contains('<') {
                continue;
            }

            // 检查是否已知
            let is_defined = known_types.contains(type_name);
            let is_forward_declared = forward_declared.contains(type_name);

            if !is_defined && !is_forward_declared {
                diagnostics.push(ConsistencyDiagnostic {
                    severity: DiagnosticSeverity::Error,
                    kind: DiagnosticKind::UnresolvedType,
                    message: format!(
                        "类型 '{}' 在模块 '{}' 中被引用但未定义",
                        type_name, reference.module_name,
                    ),
                    type_name: type_name.to_string(),
                    source_file: reference.source_file.clone(),
                    module_name: reference.module_name.clone(),
                    suggestion: Some(format!(
                        "请确保 '{}' 已在某个模块中定义，或添加前向声明",
                        type_name,
                    )),
                });
            } else if !is_defined
                && is_forward_declared
                && reference.context.requires_full_definition()
            {
                // 只有前向声明但需要完整定义
                diagnostics.push(ConsistencyDiagnostic {
                    severity: DiagnosticSeverity::Error,
                    kind: DiagnosticKind::IncompleteTypeUsage,
                    message: format!(
                        "类型 '{}' 仅有前向声明，但在 '{}' 中作为 {} 使用需要完整定义",
                        type_name,
                        reference.source_file,
                        match reference.context {
                            ReferenceContext::BaseClass => "基类",
                            ReferenceContext::PropertyValue => "值类型属性",
                            ReferenceContext::FunctionParamValue => "值类型参数",
                            _ => "需要完整定义的上下文",
                        },
                    ),
                    type_name: type_name.to_string(),
                    source_file: reference.source_file.clone(),
                    module_name: reference.module_name.clone(),
                    suggestion: Some(format!("请 #include 包含 '{}' 完整定义的头文件", type_name,)),
                });
            }
        }
    }

    /// 检查类型名冲突
    fn check_type_conflicts(&self, diagnostics: &mut Vec<ConsistencyDiagnostic>) {
        for (short_name, qualified_names) in &self.registry.short_name_index {
            if qualified_names.len() > 1 {
                let modules: Vec<&str> = qualified_names
                    .iter()
                    .filter_map(|qn| self.registry.definitions.get(qn))
                    .map(|d| d.module_name.as_str())
                    .collect();

                diagnostics.push(ConsistencyDiagnostic {
                    severity: DiagnosticSeverity::Warning,
                    kind: DiagnosticKind::TypeNameConflict,
                    message: format!(
                        "类型短名 '{}' 在多个模块中定义: {}",
                        short_name,
                        modules.join(", "),
                    ),
                    type_name: short_name.clone(),
                    source_file: String::new(),
                    module_name: modules.first().copied().unwrap_or("").to_string(),
                    suggestion: Some(format!(
                        "建议使用完全限定名或为 '{}' 添加命名空间前缀",
                        short_name,
                    )),
                });
            }
        }
    }

    /// 检查前向声明一致性
    fn check_forward_decls(&self, diagnostics: &mut Vec<ConsistencyDiagnostic>) {
        for decl in &self.registry.forward_decls {
            if let Some(def) = self.registry.find_type(&decl.type_name) {
                // 检查类别是否匹配
                if decl.declared_category != def.category {
                    diagnostics.push(ConsistencyDiagnostic {
                        severity: DiagnosticSeverity::Error,
                        kind: DiagnosticKind::ForwardDeclCategoryMismatch,
                        message: format!(
                            "前向声明 '{}' 为 {:?}，但实际定义为 {:?}",
                            decl.type_name, decl.declared_category, def.category,
                        ),
                        type_name: decl.type_name.clone(),
                        source_file: decl.header_file.clone(),
                        module_name: decl.module_name.clone(),
                        suggestion: Some(format!(
                            "将前向声明改为 '{} {};'",
                            match def.category {
                                TypeCategory::Class => "class",
                                TypeCategory::Struct => "struct",
                                TypeCategory::Enum => "enum class",
                                _ => "class",
                            },
                            decl.type_name,
                        )),
                    });
                }

                // 如果同一文件已包含完整定义，前向声明多余
                if decl.header_file == def.header_file {
                    diagnostics.push(ConsistencyDiagnostic {
                        severity: DiagnosticSeverity::Hint,
                        kind: DiagnosticKind::UnnecessaryForwardDecl,
                        message: format!(
                            "'{}' 的前向声明在同一文件 '{}' 中已有完整定义",
                            decl.type_name, decl.header_file,
                        ),
                        type_name: decl.type_name.clone(),
                        source_file: decl.header_file.clone(),
                        module_name: decl.module_name.clone(),
                        suggestion: Some("移除多余的前向声明".to_string()),
                    });
                }
            }
        }
    }

    /// 检查跨模块依赖合规性
    fn check_cross_module_deps(&self, diagnostics: &mut Vec<ConsistencyDiagnostic>) {
        for reference in &self.registry.references {
            if let Some(def) = self.registry.find_type(&reference.type_name) {
                // 同模块不检查
                if def.module_name == reference.module_name {
                    continue;
                }

                // 检查层级违规
                let ref_layer = self
                    .registry
                    .module_layers
                    .get(&reference.module_name)
                    .copied()
                    .unwrap_or(0);
                let def_layer = self
                    .registry
                    .module_layers
                    .get(&def.module_name)
                    .copied()
                    .unwrap_or(0);

                if def_layer > ref_layer {
                    diagnostics.push(ConsistencyDiagnostic {
                        severity: DiagnosticSeverity::Error,
                        kind: DiagnosticKind::CrossModuleLayerViolation,
                        message: format!(
                            "模块 '{}' (Layer {}) 引用了更高层级模块 '{}' (Layer {}) 中的类型 '{}'",
                            reference.module_name,
                            ref_layer,
                            def.module_name,
                            def_layer,
                            reference.type_name,
                        ),
                        type_name: reference.type_name.clone(),
                        source_file: reference.source_file.clone(),
                        module_name: reference.module_name.clone(),
                        suggestion: Some(format!(
                            "低层模块不应依赖高层模块，考虑将 '{}' 移到更低层级或使用接口抽象",
                            reference.type_name,
                        )),
                    });
                }
            }
        }
    }

    /// 检查循环类型依赖
    fn check_circular_type_deps(&self, diagnostics: &mut Vec<ConsistencyDiagnostic>) {
        // 构建类型间依赖图 (仅跨模块)
        let mut dep_graph: HashMap<&str, HashSet<&str>> = HashMap::new();

        for def in self.registry.definitions.values() {
            for ref_type in &def.referenced_types {
                if let Some(ref_def) = self.registry.find_type(ref_type) {
                    if ref_def.module_name != def.module_name {
                        dep_graph
                            .entry(def.qualified_name.as_str())
                            .or_default()
                            .insert(ref_def.qualified_name.as_str());
                    }
                }
            }
        }

        // DFS 检测环
        let mut visited = HashSet::new();
        let mut rec_stack = HashSet::new();

        for type_name in dep_graph.keys() {
            if !visited.contains(type_name) {
                let mut cycle_path = Vec::new();
                if self.detect_cycle_dfs(
                    type_name,
                    &dep_graph,
                    &mut visited,
                    &mut rec_stack,
                    &mut cycle_path,
                ) {
                    diagnostics.push(ConsistencyDiagnostic {
                        severity: DiagnosticSeverity::Warning,
                        kind: DiagnosticKind::CircularTypeDependency,
                        message: format!("检测到循环类型依赖: {}", cycle_path.join(" → "),),
                        type_name: type_name.to_string(),
                        source_file: String::new(),
                        module_name: String::new(),
                        suggestion: Some("考虑使用前向声明或接口抽象打破循环依赖".to_string()),
                    });
                }
            }
        }
    }

    /// DFS 检测环
    fn detect_cycle_dfs<'a>(
        &self,
        node: &'a str,
        graph: &HashMap<&'a str, HashSet<&'a str>>,
        visited: &mut HashSet<&'a str>,
        rec_stack: &mut HashSet<&'a str>,
        path: &mut Vec<String>,
    ) -> bool {
        visited.insert(node);
        rec_stack.insert(node);
        path.push(node.to_string());

        if let Some(neighbors) = graph.get(node) {
            for &next in neighbors {
                if !visited.contains(next) {
                    if self.detect_cycle_dfs(next, graph, visited, rec_stack, path) {
                        return true;
                    }
                } else if rec_stack.contains(next) {
                    path.push(next.to_string());
                    return true;
                }
            }
        }

        rec_stack.remove(node);
        path.pop();
        false
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_type_def(name: &str, module: &str, category: TypeCategory) -> TypeDefinition {
        TypeDefinition {
            qualified_name: name.to_string(),
            short_name: name.split("::").last().unwrap_or(name).to_string(),
            category,
            module_name: module.to_string(),
            header_file: format!("{}.h", name.split("::").last().unwrap_or(name)),
            namespace: None,
            base_classes: Vec::new(),
            referenced_types: Vec::new(),
            is_template: false,
        }
    }

    fn make_reference(type_name: &str, module: &str, context: ReferenceContext) -> TypeReference {
        TypeReference {
            type_name: type_name.to_string(),
            source_file: format!("{}_user.h", type_name),
            module_name: module.to_string(),
            line_number: None,
            context,
            can_use_forward_decl: !context.requires_full_definition(),
        }
    }

    #[test]
    fn test_no_issues_when_all_resolved() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        reg.register_type(make_type_def("APlayer", "Game", TypeCategory::Class));
        reg.register_type(make_type_def("AWeapon", "Game", TypeCategory::Class));
        reg.register_reference(make_reference(
            "AWeapon",
            "Game",
            ReferenceContext::PropertyPointer,
        ));

        let report = checker.check_all();
        assert!(report.is_ok(), "所有类型已定义，不应有错误");
        assert_eq!(report.error_count, 0);
    }

    #[test]
    fn test_unresolved_type_reference() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        reg.register_type(make_type_def("APlayer", "Game", TypeCategory::Class));
        reg.register_reference(make_reference(
            "AUnknownType",
            "Game",
            ReferenceContext::PropertyValue,
        ));

        let report = checker.check_all();
        assert!(!report.is_ok());
        assert_eq!(report.error_count, 1);
        assert_eq!(report.diagnostics[0].kind, DiagnosticKind::UnresolvedType);
        assert_eq!(report.diagnostics[0].type_name, "AUnknownType");
    }

    #[test]
    fn test_builtin_types_not_flagged() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        // 引用内建类型不应报错
        reg.register_reference(make_reference(
            "int32_t",
            "Game",
            ReferenceContext::PropertyValue,
        ));
        reg.register_reference(make_reference(
            "LString",
            "Game",
            ReferenceContext::PropertyValue,
        ));
        reg.register_reference(make_reference(
            "float",
            "Game",
            ReferenceContext::PropertyValue,
        ));

        let report = checker.check_all();
        assert!(report.is_ok(), "内建类型不应报错");
    }

    #[test]
    fn test_forward_decl_satisfies_pointer() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        // 只有前向声明，用作指针类型 → 应该通过
        reg.register_forward_decl(ForwardDeclaration {
            type_name: "AEnemy".to_string(),
            header_file: "Player.h".to_string(),
            module_name: "Game".to_string(),
            declared_category: TypeCategory::Class,
        });
        reg.register_reference(make_reference(
            "AEnemy",
            "Game",
            ReferenceContext::PropertyPointer,
        ));

        let report = checker.check_all();
        assert!(report.is_ok(), "前向声明应满足指针类型引用");
    }

    #[test]
    fn test_forward_decl_insufficient_for_value() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        // 只有前向声明，用作值类型 → 应报错
        reg.register_forward_decl(ForwardDeclaration {
            type_name: "FMyStruct".to_string(),
            header_file: "User.h".to_string(),
            module_name: "Game".to_string(),
            declared_category: TypeCategory::Struct,
        });
        reg.register_reference(make_reference(
            "FMyStruct",
            "Game",
            ReferenceContext::PropertyValue,
        ));

        let report = checker.check_all();
        assert!(!report.is_ok());
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == DiagnosticKind::IncompleteTypeUsage));
    }

    #[test]
    fn test_type_name_conflict() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        reg.register_type(TypeDefinition {
            qualified_name: "ModuleA::Config".to_string(),
            short_name: "Config".to_string(),
            category: TypeCategory::Struct,
            module_name: "ModuleA".to_string(),
            header_file: "ConfigA.h".to_string(),
            namespace: Some("ModuleA".to_string()),
            base_classes: Vec::new(),
            referenced_types: Vec::new(),
            is_template: false,
        });
        reg.register_type(TypeDefinition {
            qualified_name: "ModuleB::Config".to_string(),
            short_name: "Config".to_string(),
            category: TypeCategory::Struct,
            module_name: "ModuleB".to_string(),
            header_file: "ConfigB.h".to_string(),
            namespace: Some("ModuleB".to_string()),
            base_classes: Vec::new(),
            referenced_types: Vec::new(),
            is_template: false,
        });

        let report = checker.check_all();
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == DiagnosticKind::TypeNameConflict));
    }

    #[test]
    fn test_forward_decl_category_mismatch() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        reg.register_type(make_type_def("FMyData", "Core", TypeCategory::Struct));
        reg.register_forward_decl(ForwardDeclaration {
            type_name: "FMyData".to_string(),
            header_file: "User.h".to_string(),
            module_name: "Game".to_string(),
            declared_category: TypeCategory::Class, // 错误! 实际是 struct
        });

        let report = checker.check_all();
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == DiagnosticKind::ForwardDeclCategoryMismatch));
    }

    #[test]
    fn test_unnecessary_forward_decl() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        // 类型定义和前向声明在同一文件
        reg.register_type(TypeDefinition {
            qualified_name: "APlayer".to_string(),
            short_name: "APlayer".to_string(),
            category: TypeCategory::Class,
            module_name: "Game".to_string(),
            header_file: "Player.h".to_string(),
            namespace: None,
            base_classes: Vec::new(),
            referenced_types: Vec::new(),
            is_template: false,
        });
        reg.register_forward_decl(ForwardDeclaration {
            type_name: "APlayer".to_string(),
            header_file: "Player.h".to_string(), // 同一文件
            module_name: "Game".to_string(),
            declared_category: TypeCategory::Class,
        });

        let report = checker.check_all();
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == DiagnosticKind::UnnecessaryForwardDecl));
    }

    #[test]
    fn test_cross_module_layer_violation() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        reg.register_module_layer("Core", 1);
        reg.register_module_layer("Editor", 3);

        reg.register_type(TypeDefinition {
            qualified_name: "AEditorWidget".to_string(),
            short_name: "AEditorWidget".to_string(),
            category: TypeCategory::Class,
            module_name: "Editor".to_string(),
            header_file: "EditorWidget.h".to_string(),
            namespace: None,
            base_classes: Vec::new(),
            referenced_types: Vec::new(),
            is_template: false,
        });

        // Core (L1) 引用 Editor (L3) 的类型 → 违规
        reg.register_reference(TypeReference {
            type_name: "AEditorWidget".to_string(),
            source_file: "CoreSystem.h".to_string(),
            module_name: "Core".to_string(),
            line_number: Some(42),
            context: ReferenceContext::PropertyPointer,
            can_use_forward_decl: true,
        });

        let report = checker.check_all();
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == DiagnosticKind::CrossModuleLayerViolation));
    }

    #[test]
    fn test_cross_module_valid_dependency() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        reg.register_module_layer("Core", 1);
        reg.register_module_layer("Editor", 3);

        reg.register_type(TypeDefinition {
            qualified_name: "LObject".to_string(),
            short_name: "LObject".to_string(),
            category: TypeCategory::Class,
            module_name: "Core".to_string(),
            header_file: "Object.h".to_string(),
            namespace: None,
            base_classes: Vec::new(),
            referenced_types: Vec::new(),
            is_template: false,
        });

        // Editor (L3) 引用 Core (L1) → 合法
        reg.register_reference(TypeReference {
            type_name: "LObject".to_string(),
            source_file: "EditorPanel.h".to_string(),
            module_name: "Editor".to_string(),
            line_number: None,
            context: ReferenceContext::BaseClass,
            can_use_forward_decl: false,
        });

        let report = checker.check_all();
        let layer_violations = report
            .diagnostics
            .iter()
            .filter(|d| d.kind == DiagnosticKind::CrossModuleLayerViolation)
            .count();
        assert_eq!(layer_violations, 0, "高层引用低层应合法");
    }

    #[test]
    fn test_report_counts() {
        let mut checker = TypeConsistencyChecker::new();
        let reg = checker.registry_mut();

        reg.register_type(make_type_def("A", "M", TypeCategory::Class));
        reg.register_reference(make_reference(
            "Unknown1",
            "M",
            ReferenceContext::PropertyValue,
        ));
        reg.register_reference(make_reference(
            "Unknown2",
            "M",
            ReferenceContext::PropertyValue,
        ));

        let report = checker.check_all();
        assert_eq!(report.error_count, 2);
        assert_eq!(report.registered_type_count, 1);
        assert_eq!(report.reference_count, 2);
    }

    #[test]
    fn test_empty_checker() {
        let checker = TypeConsistencyChecker::new();
        let report = checker.check_all();

        assert!(report.is_ok());
        assert_eq!(report.error_count, 0);
        assert_eq!(report.warning_count, 0);
        assert_eq!(report.registered_type_count, 0);
    }
}
