// ============================================================
// 文件名称：editor_meta.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：编辑器元数据增强生成 — 从 LPROPERTY 说明符自动
//           生成编辑器 UI 所需的元数据结构体和绑定代码。UE5
//           的属性面板需要大量手动 CustomizeDetail + Meta 标签，
//           我们做到 声明即 UI — 属性说明符直接驱动编辑器面板
//           布局、控件类型、范围约束、条件可见性和工具提示
// 功能描述：解析属性说明符中的编辑器相关元数据 (Category,
//           DisplayName, Tooltip, ClampMin/Max, EditCondition,
//           MakeStructureDefaultValue, ArraySizeEnum) → 生成
//           C++ EditorPropertyMeta 结构体 → 生成属性面板注册代码
// 技术特性：说明符驱动、constexpr 元数据表、条件可见性表达式
//           编译、Slider/Dropdown/Color/FilePath 控件映射
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ EditorMetaGenerator        │ 编辑器元数据生成器             │
// │ EditorPropertyMeta         │ 属性编辑器元数据               │
// │ EditorClassMeta            │ 类编辑器元数据                 │
// │ WidgetType                 │ 控件类型枚举                   │
// │ EditCondition              │ 条件可见性表达式               │
// │ GeneratedEditorMeta        │ 生成的代码                    │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建生成器                    │
// │ generate_class_meta()      │ 生成类的编辑器元数据           │
// │ infer_widget_type()        │ 推断属性控件类型               │
// │ generate_header()          │ 生成头文件                    │
// │ generate_registration()    │ 生成注册代码                  │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};

// =============================================================================
// 控件类型
// =============================================================================

/// 编辑器控件类型
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum WidgetType {
    /// 默认文本输入框
    Default,
    /// 滑块 (数值)
    Slider,
    /// 旋转框 (数值微调)
    SpinBox,
    /// 复选框 (布尔)
    Checkbox,
    /// 下拉菜单 (枚举)
    Dropdown,
    /// 颜色选择器
    ColorPicker,
    /// 向量编辑器 (2D/3D/4D)
    VectorEditor,
    /// 旋转编辑器 (Euler/Quat)
    RotatorEditor,
    /// 文件路径选择器
    FilePath,
    /// 资产引用选择器
    AssetPicker,
    /// 多行文本编辑器
    TextArea,
    /// 曲线编辑器
    CurveEditor,
    /// 对象引用选择器
    ObjectPicker,
}

impl WidgetType {
    /// C++ 枚举值名称
    pub fn cpp_enum_name(&self) -> &'static str {
        match self {
            Self::Default => "EWidgetType::Default",
            Self::Slider => "EWidgetType::Slider",
            Self::SpinBox => "EWidgetType::SpinBox",
            Self::Checkbox => "EWidgetType::Checkbox",
            Self::Dropdown => "EWidgetType::Dropdown",
            Self::ColorPicker => "EWidgetType::ColorPicker",
            Self::VectorEditor => "EWidgetType::VectorEditor",
            Self::RotatorEditor => "EWidgetType::RotatorEditor",
            Self::FilePath => "EWidgetType::FilePath",
            Self::AssetPicker => "EWidgetType::AssetPicker",
            Self::TextArea => "EWidgetType::TextArea",
            Self::CurveEditor => "EWidgetType::CurveEditor",
            Self::ObjectPicker => "EWidgetType::ObjectPicker",
        }
    }
}

// =============================================================================
// 编辑条件
// =============================================================================

/// 条件可见性表达式
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EditCondition {
    /// 条件属性名
    pub property_name: String,
    /// 是否取反
    pub is_negated: bool,
    /// 比较操作符 (可选, 如 "==", "!=", ">")
    pub compare_op: Option<String>,
    /// 比较值 (可选)
    pub compare_value: Option<String>,
}

impl EditCondition {
    /// 简单布尔条件
    pub fn boolean(property_name: &str, negated: bool) -> Self {
        Self {
            property_name: property_name.to_string(),
            is_negated: negated,
            compare_op: None,
            compare_value: None,
        }
    }

    /// 比较条件
    pub fn comparison(property_name: &str, op: &str, value: &str) -> Self {
        Self {
            property_name: property_name.to_string(),
            is_negated: false,
            compare_op: Some(op.to_string()),
            compare_value: Some(value.to_string()),
        }
    }

    /// 生成 C++ 条件表达式
    pub fn to_cpp_expression(&self) -> String {
        let prefix = if self.is_negated { "!" } else { "" };
        if let (Some(op), Some(val)) = (&self.compare_op, &self.compare_value) {
            format!("{}{} {} {}", prefix, self.property_name, op, val)
        } else {
            format!("{}{}", prefix, self.property_name)
        }
    }
}

// =============================================================================
// 属性编辑器元数据
// =============================================================================

/// 属性编辑器元数据
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EditorPropertyMeta {
    /// 属性名
    pub property_name: String,
    /// 显示名称 (本地化 key 或直接字符串)
    pub display_name: Option<String>,
    /// 分类
    pub category: String,
    /// 工具提示
    pub tooltip: Option<String>,
    /// 控件类型
    pub widget_type: WidgetType,
    /// 最小值约束
    pub clamp_min: Option<f64>,
    /// 最大值约束
    pub clamp_max: Option<f64>,
    /// UI 最小值 (滑块范围)
    pub ui_min: Option<f64>,
    /// UI 最大值 (滑块范围)
    pub ui_max: Option<f64>,
    /// 步进值
    pub delta: Option<f64>,
    /// 小数位数
    pub decimal_places: Option<u32>,
    /// 单位后缀 (如 "cm", "deg", "%")
    pub units: Option<String>,
    /// 条件可见性
    pub edit_condition: Option<EditCondition>,
    /// 条件编辑 (条件满足时才可编辑)
    pub edit_condition_hides: bool,
    /// 是否只读
    pub is_read_only: bool,
    /// 是否高级属性 (默认折叠)
    pub is_advanced: bool,
    /// 内联编辑子属性
    pub inline_edit: bool,
    /// 数组大小枚举限制
    pub array_size_enum: Option<String>,
    /// 文件过滤器 (FilePath 控件)
    pub file_filter: Option<String>,
    /// 允许的类 (ObjectPicker 控件)
    pub allowed_classes: Vec<String>,
    /// 排序优先级 (越小越靠前)
    pub sort_order: i32,
}

impl Default for EditorPropertyMeta {
    fn default() -> Self {
        Self {
            property_name: String::new(),
            display_name: None,
            category: "Default".to_string(),
            tooltip: None,
            widget_type: WidgetType::Default,
            clamp_min: None,
            clamp_max: None,
            ui_min: None,
            ui_max: None,
            delta: None,
            decimal_places: None,
            units: None,
            edit_condition: None,
            edit_condition_hides: false,
            is_read_only: false,
            is_advanced: false,
            inline_edit: false,
            array_size_enum: None,
            file_filter: None,
            allowed_classes: Vec::new(),
            sort_order: 0,
        }
    }
}

// =============================================================================
// 类编辑器元数据
// =============================================================================

/// 类编辑器元数据
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EditorClassMeta {
    /// 类名
    pub class_name: String,
    /// 类显示名称
    pub display_name: Option<String>,
    /// 属性元数据列表
    pub properties: Vec<EditorPropertyMeta>,
    /// 分类排序
    pub category_order: Vec<String>,
}

impl EditorClassMeta {
    /// 按分类分组属性
    pub fn properties_by_category(&self) -> Vec<(String, Vec<&EditorPropertyMeta>)> {
        let mut categories: Vec<String> = Vec::new();
        for prop in &self.properties {
            if !categories.contains(&prop.category) {
                categories.push(prop.category.clone());
            }
        }
        // 按 category_order 排序
        categories.sort_by(|a, b| {
            let pos_a = self
                .category_order
                .iter()
                .position(|c| c == a)
                .unwrap_or(usize::MAX);
            let pos_b = self
                .category_order
                .iter()
                .position(|c| c == b)
                .unwrap_or(usize::MAX);
            pos_a.cmp(&pos_b)
        });

        categories
            .into_iter()
            .map(|cat| {
                let props: Vec<&EditorPropertyMeta> = self
                    .properties
                    .iter()
                    .filter(|p| p.category == cat)
                    .collect();
                (cat, props)
            })
            .collect()
    }

    /// 属性总数
    pub fn property_count(&self) -> usize {
        self.properties.len()
    }

    /// 有条件可见性的属性数
    pub fn conditional_property_count(&self) -> usize {
        self.properties
            .iter()
            .filter(|p| p.edit_condition.is_some())
            .count()
    }
}

// =============================================================================
// 生成的编辑器元数据代码
// =============================================================================

/// 生成的编辑器元数据代码
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GeneratedEditorMeta {
    /// 头文件内容
    pub header_content: String,
    /// 注册代码
    pub registration_content: String,
    /// 类名
    pub class_name: String,
}

// =============================================================================
// 编辑器元数据生成器
// =============================================================================

/// 属性输入信息 (从 LPROPERTY 解析而来)
#[derive(Debug, Clone)]
pub struct PropertyInput {
    /// 属性名
    pub name: String,
    /// C++ 类型
    pub cpp_type: String,
    /// 说明符键值对
    pub specifiers: Vec<(String, Option<String>)>,
}

/// 编辑器元数据生成器
pub struct EditorMetaGenerator;

impl EditorMetaGenerator {
    /// 创建生成器
    pub fn new() -> Self {
        Self
    }

    /// 从属性输入生成类编辑器元数据
    pub fn generate_class_meta(
        &self,
        class_name: &str,
        properties: &[PropertyInput],
    ) -> EditorClassMeta {
        let mut metas = Vec::new();
        let mut categories = Vec::new();

        for prop in properties {
            let meta = self.build_property_meta(prop);
            if !categories.contains(&meta.category) {
                categories.push(meta.category.clone());
            }
            metas.push(meta);
        }

        // 按 sort_order 排序
        metas.sort_by_key(|m| m.sort_order);

        EditorClassMeta {
            class_name: class_name.to_string(),
            display_name: None,
            properties: metas,
            category_order: categories,
        }
    }

    /// 构建单个属性的编辑器元数据
    fn build_property_meta(&self, prop: &PropertyInput) -> EditorPropertyMeta {
        let mut meta = EditorPropertyMeta {
            property_name: prop.name.clone(),
            ..Default::default()
        };

        // 推断默认控件类型
        meta.widget_type = infer_widget_type(&prop.cpp_type);

        // 解析说明符
        for (key, value) in &prop.specifiers {
            match key.as_str() {
                "Category" => {
                    if let Some(v) = value {
                        meta.category = v.clone();
                    }
                }
                "DisplayName" => {
                    meta.display_name = value.clone();
                }
                "Tooltip" | "ToolTip" => {
                    meta.tooltip = value.clone();
                }
                "ClampMin" => {
                    if let Some(v) = value {
                        meta.clamp_min = v.parse().ok();
                    }
                }
                "ClampMax" => {
                    if let Some(v) = value {
                        meta.clamp_max = v.parse().ok();
                    }
                }
                "UIMin" => {
                    if let Some(v) = value {
                        meta.ui_min = v.parse().ok();
                    }
                }
                "UIMax" => {
                    if let Some(v) = value {
                        meta.ui_max = v.parse().ok();
                    }
                }
                "Delta" => {
                    if let Some(v) = value {
                        meta.delta = v.parse().ok();
                    }
                }
                "Units" => {
                    meta.units = value.clone();
                }
                "EditCondition" => {
                    if let Some(v) = value {
                        meta.edit_condition = Some(parse_edit_condition(v));
                    }
                }
                "EditConditionHides" => {
                    meta.edit_condition_hides = true;
                }
                "VisibleAnywhere" | "VisibleDefaultsOnly" => {
                    meta.is_read_only = true;
                }
                "AdvancedDisplay" => {
                    meta.is_advanced = true;
                }
                "InlineEditConditionToggle" | "EditInline" => {
                    meta.inline_edit = true;
                }
                "ArraySizeEnum" => {
                    meta.array_size_enum = value.clone();
                }
                "FilePathFilter" => {
                    meta.file_filter = value.clone();
                    meta.widget_type = WidgetType::FilePath;
                }
                "AllowedClasses" => {
                    if let Some(v) = value {
                        meta.allowed_classes = v.split(',').map(|s| s.trim().to_string()).collect();
                    }
                    meta.widget_type = WidgetType::ObjectPicker;
                }
                _ => {}
            }
        }

        // 如果有 ClampMin/ClampMax 且是数值类型，升级为 Slider
        if (meta.clamp_min.is_some() || meta.clamp_max.is_some())
            && matches!(meta.widget_type, WidgetType::Default | WidgetType::SpinBox)
        {
            meta.widget_type = WidgetType::Slider;
        }

        meta
    }

    /// 生成 C++ 头文件
    pub fn generate_header(&self, class_meta: &EditorClassMeta) -> String {
        let class = &class_meta.class_name;
        let mut h = String::with_capacity(2048);

        h.push_str(&format!("// 自动生成 — {} 编辑器元数据\n", class));
        h.push_str("#pragma once\n\n");
        h.push_str("#include \"Editor/EditorPropertyMeta.h\"\n\n");
        h.push_str(&format!("namespace Limx::Editor\n{{\n\n"));

        // constexpr 属性元数据数组
        h.push_str(&format!(
            "inline constexpr EditorPropertyMetaEntry {}_EditorMeta[] =\n{{\n",
            class,
        ));

        for prop in &class_meta.properties {
            h.push_str(&format!("    {{\n"));
            h.push_str(&format!(
                "        .PropertyName = \"{}\",\n",
                prop.property_name
            ));
            if let Some(display) = &prop.display_name {
                h.push_str(&format!("        .DisplayName = \"{}\",\n", display));
            }
            h.push_str(&format!("        .Category = \"{}\",\n", prop.category));
            if let Some(tooltip) = &prop.tooltip {
                h.push_str(&format!("        .Tooltip = \"{}\",\n", tooltip));
            }
            h.push_str(&format!(
                "        .Widget = {},\n",
                prop.widget_type.cpp_enum_name()
            ));
            if let Some(min) = prop.clamp_min {
                h.push_str(&format!("        .ClampMin = {:.1}f,\n", min));
            }
            if let Some(max) = prop.clamp_max {
                h.push_str(&format!("        .ClampMax = {:.1}f,\n", max));
            }
            if prop.is_read_only {
                h.push_str("        .bReadOnly = true,\n");
            }
            if prop.is_advanced {
                h.push_str("        .bAdvanced = true,\n");
            }
            if let Some(cond) = &prop.edit_condition {
                h.push_str(&format!(
                    "        .EditCondition = \"{}\",\n",
                    cond.to_cpp_expression(),
                ));
                if prop.edit_condition_hides {
                    h.push_str("        .bEditConditionHides = true,\n");
                }
            }
            if let Some(units) = &prop.units {
                h.push_str(&format!("        .Units = \"{}\",\n", units));
            }
            h.push_str("    },\n");
        }

        h.push_str("};\n\n");

        // 属性数量常量
        h.push_str(&format!(
            "inline constexpr size_t {}_EditorMetaCount = {};\n\n",
            class,
            class_meta.properties.len(),
        ));

        h.push_str("} // namespace Limx::Editor\n");
        h
    }

    /// 生成注册代码
    pub fn generate_registration(&self, class_meta: &EditorClassMeta) -> String {
        let class = &class_meta.class_name;
        let mut cpp = String::with_capacity(512);

        cpp.push_str(&format!("// 自动生成 — {} 编辑器元数据注册\n", class));
        cpp.push_str(&format!(
            "REGISTER_EDITOR_META({class}, {class}_EditorMeta, {class}_EditorMetaCount);\n"
        ));

        cpp
    }

    /// 生成完整的元数据代码
    pub fn generate(&self, class_name: &str, properties: &[PropertyInput]) -> GeneratedEditorMeta {
        let class_meta = self.generate_class_meta(class_name, properties);
        let header = self.generate_header(&class_meta);
        let registration = self.generate_registration(&class_meta);

        GeneratedEditorMeta {
            header_content: header,
            registration_content: registration,
            class_name: class_name.to_string(),
        }
    }
}

// =============================================================================
// 辅助函数
// =============================================================================

/// 从 C++ 类型推断控件类型
pub fn infer_widget_type(cpp_type: &str) -> WidgetType {
    let t = cpp_type.trim();
    match t {
        "bool" | "uint8" if t == "bool" => WidgetType::Checkbox,
        "float" | "double" | "int32" | "int64" | "uint32" | "uint64" | "int16" | "uint16"
        | "int8" => WidgetType::SpinBox,
        "FString" | "FName" | "FText" => WidgetType::Default,
        "FVector" | "FVector2D" | "FVector4" | "FIntPoint" | "FIntVector" => {
            WidgetType::VectorEditor
        }
        "FRotator" | "FQuat" => WidgetType::RotatorEditor,
        "FLinearColor" | "FColor" => WidgetType::ColorPicker,
        "FFilePath" => WidgetType::FilePath,
        "FCurveFloat" | "FRuntimeFloatCurve" => WidgetType::CurveEditor,
        _ => {
            // 指针或软引用 → ObjectPicker
            if t.ends_with('*') || t.starts_with("TSoftObjectPtr") || t.starts_with("TObjectPtr") {
                WidgetType::ObjectPicker
            } else {
                WidgetType::Default
            }
        }
    }
}

/// 解析 EditCondition 表达式
pub fn parse_edit_condition(expr: &str) -> EditCondition {
    let trimmed = expr.trim();

    // 取反
    if let Some(inner) = trimmed.strip_prefix('!') {
        return EditCondition::boolean(inner.trim(), true);
    }

    // 比较运算符
    for op in &["==", "!=", ">=", "<=", ">", "<"] {
        if let Some(pos) = trimmed.find(op) {
            let prop = trimmed[..pos].trim();
            let val = trimmed[pos + op.len()..].trim();
            return EditCondition::comparison(prop, op, val);
        }
    }

    // 简单布尔
    EditCondition::boolean(trimmed, false)
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_input(name: &str, cpp_type: &str, specs: Vec<(&str, Option<&str>)>) -> PropertyInput {
        PropertyInput {
            name: name.to_string(),
            cpp_type: cpp_type.to_string(),
            specifiers: specs
                .into_iter()
                .map(|(k, v)| (k.to_string(), v.map(|s| s.to_string())))
                .collect(),
        }
    }

    #[test]
    fn test_infer_widget_bool() {
        assert_eq!(infer_widget_type("bool"), WidgetType::Checkbox);
    }

    #[test]
    fn test_infer_widget_numeric() {
        assert_eq!(infer_widget_type("float"), WidgetType::SpinBox);
        assert_eq!(infer_widget_type("int32"), WidgetType::SpinBox);
    }

    #[test]
    fn test_infer_widget_vector() {
        assert_eq!(infer_widget_type("FVector"), WidgetType::VectorEditor);
        assert_eq!(infer_widget_type("FVector2D"), WidgetType::VectorEditor);
    }

    #[test]
    fn test_infer_widget_color() {
        assert_eq!(infer_widget_type("FLinearColor"), WidgetType::ColorPicker);
    }

    #[test]
    fn test_infer_widget_pointer() {
        assert_eq!(infer_widget_type("AActor*"), WidgetType::ObjectPicker);
        assert_eq!(
            infer_widget_type("TSoftObjectPtr<UTexture>"),
            WidgetType::ObjectPicker
        );
    }

    #[test]
    fn test_parse_edit_condition_boolean() {
        let cond = parse_edit_condition("bIsEnabled");
        assert_eq!(cond.property_name, "bIsEnabled");
        assert!(!cond.is_negated);
        assert_eq!(cond.to_cpp_expression(), "bIsEnabled");
    }

    #[test]
    fn test_parse_edit_condition_negated() {
        let cond = parse_edit_condition("!bIsHidden");
        assert_eq!(cond.property_name, "bIsHidden");
        assert!(cond.is_negated);
        assert_eq!(cond.to_cpp_expression(), "!bIsHidden");
    }

    #[test]
    fn test_parse_edit_condition_comparison() {
        let cond = parse_edit_condition("RenderMode == 1");
        assert_eq!(cond.property_name, "RenderMode");
        assert_eq!(cond.compare_op.as_deref(), Some("=="));
        assert_eq!(cond.compare_value.as_deref(), Some("1"));
        assert_eq!(cond.to_cpp_expression(), "RenderMode == 1");
    }

    #[test]
    fn test_generate_class_meta() {
        let gen = EditorMetaGenerator::new();
        let props = vec![
            make_input(
                "Health",
                "float",
                vec![
                    ("Category", Some("Combat")),
                    ("ClampMin", Some("0")),
                    ("ClampMax", Some("100")),
                    ("Tooltip", Some("角色生命值")),
                ],
            ),
            make_input(
                "bIsAlive",
                "bool",
                vec![("Category", Some("Combat")), ("VisibleAnywhere", None)],
            ),
            make_input(
                "PlayerName",
                "FString",
                vec![
                    ("Category", Some("Info")),
                    ("DisplayName", Some("玩家名称")),
                ],
            ),
        ];

        let meta = gen.generate_class_meta("APlayer", &props);
        assert_eq!(meta.class_name, "APlayer");
        assert_eq!(meta.property_count(), 3);

        // Health 应推断为 Slider (因为有 ClampMin/ClampMax)
        let health = &meta.properties[0];
        assert_eq!(health.widget_type, WidgetType::Slider);
        assert_eq!(health.clamp_min, Some(0.0));
        assert_eq!(health.clamp_max, Some(100.0));
        assert_eq!(health.tooltip.as_deref(), Some("角色生命值"));

        // bIsAlive 应为 Checkbox + ReadOnly
        let alive = &meta.properties[1];
        assert_eq!(alive.widget_type, WidgetType::Checkbox);
        assert!(alive.is_read_only);

        // PlayerName 应为 Default
        let name = &meta.properties[2];
        assert_eq!(name.widget_type, WidgetType::Default);
        assert_eq!(name.display_name.as_deref(), Some("玩家名称"));
    }

    #[test]
    fn test_properties_by_category() {
        let gen = EditorMetaGenerator::new();
        let props = vec![
            make_input("A", "float", vec![("Category", Some("Physics"))]),
            make_input("B", "float", vec![("Category", Some("Rendering"))]),
            make_input("C", "float", vec![("Category", Some("Physics"))]),
        ];

        let meta = gen.generate_class_meta("Test", &props);
        let by_cat = meta.properties_by_category();
        assert_eq!(by_cat.len(), 2);
        assert_eq!(by_cat[0].0, "Physics");
        assert_eq!(by_cat[0].1.len(), 2);
        assert_eq!(by_cat[1].0, "Rendering");
    }

    #[test]
    fn test_edit_condition_hides() {
        let gen = EditorMetaGenerator::new();
        let props = vec![
            make_input("bUseFog", "bool", vec![("Category", Some("Rendering"))]),
            make_input(
                "FogDensity",
                "float",
                vec![
                    ("Category", Some("Rendering")),
                    ("EditCondition", Some("bUseFog")),
                    ("EditConditionHides", None),
                ],
            ),
        ];

        let meta = gen.generate_class_meta("AVolume", &props);
        let fog = &meta.properties[1];
        assert!(fog.edit_condition.is_some());
        assert!(fog.edit_condition_hides);
        assert_eq!(
            fog.edit_condition.as_ref().unwrap().to_cpp_expression(),
            "bUseFog"
        );
    }

    #[test]
    fn test_generate_header_output() {
        let gen = EditorMetaGenerator::new();
        let props = vec![make_input(
            "Speed",
            "float",
            vec![
                ("Category", Some("Movement")),
                ("ClampMin", Some("0")),
                ("Units", Some("cm/s")),
            ],
        )];

        let result = gen.generate("ACharacter", &props);

        assert!(result.header_content.contains("ACharacter_EditorMeta"));
        assert!(result.header_content.contains("\"Speed\""));
        assert!(result.header_content.contains("EWidgetType::Slider"));
        assert!(result.header_content.contains("\"cm/s\""));
        assert!(result
            .header_content
            .contains("constexpr size_t ACharacter_EditorMetaCount"));
        assert!(result.registration_content.contains("REGISTER_EDITOR_META"));
    }

    #[test]
    fn test_file_path_widget() {
        let gen = EditorMetaGenerator::new();
        let props = vec![make_input(
            "ConfigPath",
            "FString",
            vec![
                ("Category", Some("Settings")),
                ("FilePathFilter", Some("cfg")),
            ],
        )];

        let meta = gen.generate_class_meta("USettings", &props);
        assert_eq!(meta.properties[0].widget_type, WidgetType::FilePath);
        assert_eq!(meta.properties[0].file_filter.as_deref(), Some("cfg"));
    }

    #[test]
    fn test_object_picker_widget() {
        let gen = EditorMetaGenerator::new();
        let props = vec![make_input(
            "TargetMesh",
            "UStaticMesh*",
            vec![
                ("Category", Some("Visual")),
                ("AllowedClasses", Some("UStaticMesh, USkeletalMesh")),
            ],
        )];

        let meta = gen.generate_class_meta("AMeshActor", &props);
        assert_eq!(meta.properties[0].widget_type, WidgetType::ObjectPicker);
        assert_eq!(meta.properties[0].allowed_classes.len(), 2);
    }

    #[test]
    fn test_advanced_display() {
        let gen = EditorMetaGenerator::new();
        let props = vec![make_input(
            "DebugDraw",
            "bool",
            vec![("Category", Some("Debug")), ("AdvancedDisplay", None)],
        )];

        let meta = gen.generate_class_meta("ADebug", &props);
        assert!(meta.properties[0].is_advanced);
    }

    #[test]
    fn test_conditional_property_count() {
        let gen = EditorMetaGenerator::new();
        let props = vec![
            make_input("A", "bool", vec![("Category", Some("X"))]),
            make_input(
                "B",
                "float",
                vec![("Category", Some("X")), ("EditCondition", Some("A"))],
            ),
            make_input("C", "float", vec![("Category", Some("X"))]),
        ];

        let meta = gen.generate_class_meta("T", &props);
        assert_eq!(meta.conditional_property_count(), 1);
    }
}
