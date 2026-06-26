// ============================================================
// 文件名称：migration.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：属性迁移代码生成 — 当反射类型的属性发生变更
//           (重命名/类型变更/删除/添加) 时，自动生成 C++
//           数据迁移代码，确保旧版本序列化数据能正确加载到
//           新版本结构中。UE5 的 FPropertyTag 迁移机制需要
//           手动编写 CustomVersions + PostLoad，我们做到
//           全自动生成 + 类型安全 + 可验证的迁移链
// 功能描述：检测类型定义变更 → 生成 MigrateV{N}→V{N+1} 函数
//           → 生成迁移注册表 → 支持多版本链式迁移 → 生成
//           迁移测试骨架代码
// 技术特性：版本化类型签名、变更差分算法、C++ 迁移函数生成、
//           链式迁移组合、向后兼容保证
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ MigrationGenerator         │ 迁移代码生成器                │
// │ TypeVersion                │ 类型版本快照                  │
// │ PropertyDef                │ 属性定义                     │
// │ PropertyChange             │ 属性变更描述                  │
// │ MigrationStep              │ 单步迁移                     │
// │ MigrationChain             │ 迁移链                       │
// │ GeneratedMigration         │ 生成的迁移代码                │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建生成器                    │
// │ diff_versions()            │ 差分两个版本                  │
// │ generate_migration()       │ 生成迁移代码                  │
// │ generate_chain()           │ 生成链式迁移                  │
// │ generate_registry()        │ 生成迁移注册表                │
// │ generate_test_skeleton()   │ 生成测试骨架                  │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::HashMap;

// =============================================================================
// 属性定义
// =============================================================================

/// 属性定义
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PropertyDef {
    /// 属性名
    pub name: String,
    /// C++ 类型
    pub cpp_type: String,
    /// 默认值表达式 (可选)
    pub default_value: Option<String>,
    /// 是否可序列化
    pub is_serializable: bool,
}

// =============================================================================
// 类型版本快照
// =============================================================================

/// 类型版本快照
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TypeVersion {
    /// 类名
    pub class_name: String,
    /// 版本号
    pub version: u32,
    /// 属性列表
    pub properties: Vec<PropertyDef>,
}

impl TypeVersion {
    /// 按名称查找属性
    pub fn find_property(&self, name: &str) -> Option<&PropertyDef> {
        self.properties.iter().find(|p| p.name == name)
    }
}

// =============================================================================
// 属性变更
// =============================================================================

/// 变更类型
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum ChangeKind {
    /// 新增属性
    Added,
    /// 删除属性
    Removed,
    /// 重命名属性
    Renamed { old_name: String },
    /// 类型变更
    TypeChanged { old_type: String },
    /// 默认值变更
    DefaultChanged,
}

/// 属性变更描述
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PropertyChange {
    /// 变更类型
    pub kind: ChangeKind,
    /// 属性名 (新版本中的名字)
    pub property_name: String,
    /// 新版属性定义 (Added/TypeChanged/Renamed 时有值)
    pub new_def: Option<PropertyDef>,
    /// 旧版属性定义 (Removed/TypeChanged/Renamed 时有值)
    pub old_def: Option<PropertyDef>,
    /// 是否需要自定义转换逻辑
    pub needs_custom_converter: bool,
}

// =============================================================================
// 迁移步骤
// =============================================================================

/// 单步迁移 (从版本 N 到 N+1)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MigrationStep {
    /// 类名
    pub class_name: String,
    /// 源版本
    pub from_version: u32,
    /// 目标版本
    pub to_version: u32,
    /// 变更列表
    pub changes: Vec<PropertyChange>,
}

impl MigrationStep {
    /// 是否有破坏性变更
    pub fn has_breaking_changes(&self) -> bool {
        self.changes
            .iter()
            .any(|c| matches!(c.kind, ChangeKind::Removed | ChangeKind::TypeChanged { .. }))
    }

    /// 变更数量
    pub fn change_count(&self) -> usize {
        self.changes.len()
    }
}

/// 迁移链 (多版本)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MigrationChain {
    /// 类名
    pub class_name: String,
    /// 迁移步骤列表 (按版本递增)
    pub steps: Vec<MigrationStep>,
}

impl MigrationChain {
    /// 最低版本
    pub fn min_version(&self) -> u32 {
        self.steps.first().map(|s| s.from_version).unwrap_or(0)
    }

    /// 最高版本
    pub fn max_version(&self) -> u32 {
        self.steps.last().map(|s| s.to_version).unwrap_or(0)
    }

    /// 链长度
    pub fn length(&self) -> usize {
        self.steps.len()
    }
}

// =============================================================================
// 生成的迁移代码
// =============================================================================

/// 生成的迁移代码
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GeneratedMigration {
    /// 头文件内容
    pub header_content: String,
    /// 实现文件内容
    pub impl_content: String,
    /// 迁移注册表代码
    pub registry_content: String,
    /// 测试骨架代码
    pub test_content: String,
    /// 涉及的类
    pub class_name: String,
    /// 版本范围
    pub version_range: (u32, u32),
}

// =============================================================================
// 迁移代码生成器
// =============================================================================

/// 迁移代码生成器
pub struct MigrationGenerator {
    /// 已知的重命名映射 (手动指定)
    rename_hints: HashMap<String, HashMap<String, String>>,
    /// 类型转换器映射 (旧类型 → 新类型 → 转换表达式)
    type_converters: HashMap<(String, String), String>,
}

impl MigrationGenerator {
    /// 创建生成器
    pub fn new() -> Self {
        let mut type_converters = HashMap::new();

        // 内置类型转换器
        type_converters.insert(
            ("int32".to_string(), "int64".to_string()),
            "static_cast<int64>(${old})".to_string(),
        );
        type_converters.insert(
            ("float".to_string(), "double".to_string()),
            "static_cast<double>(${old})".to_string(),
        );
        type_converters.insert(
            ("int32".to_string(), "float".to_string()),
            "static_cast<float>(${old})".to_string(),
        );
        type_converters.insert(
            ("FString".to_string(), "FName".to_string()),
            "FName(${old})".to_string(),
        );
        type_converters.insert(
            ("bool".to_string(), "int32".to_string()),
            "${old} ? 1 : 0".to_string(),
        );

        Self {
            rename_hints: HashMap::new(),
            type_converters,
        }
    }

    /// 添加重命名提示 (类名 → 旧属性名 → 新属性名)
    pub fn add_rename_hint(&mut self, class: &str, old_name: &str, new_name: &str) {
        self.rename_hints
            .entry(class.to_string())
            .or_default()
            .insert(old_name.to_string(), new_name.to_string());
    }

    /// 添加自定义类型转换器
    pub fn add_type_converter(&mut self, old_type: &str, new_type: &str, expression: &str) {
        self.type_converters.insert(
            (old_type.to_string(), new_type.to_string()),
            expression.to_string(),
        );
    }

    /// 差分两个版本，产生变更列表
    pub fn diff_versions(&self, old: &TypeVersion, new: &TypeVersion) -> Vec<PropertyChange> {
        let mut changes = Vec::new();
        let class = &new.class_name;

        // 查找重命名提示
        let renames = self.rename_hints.get(class);

        // 检查旧属性
        for old_prop in &old.properties {
            // 检查是否被重命名
            let renamed_to = renames.and_then(|r| r.get(&old_prop.name));

            if let Some(new_name) = renamed_to {
                // 重命名
                if let Some(new_prop) = new.find_property(new_name) {
                    if old_prop.cpp_type != new_prop.cpp_type {
                        // 重命名 + 类型变更
                        changes.push(PropertyChange {
                            kind: ChangeKind::Renamed {
                                old_name: old_prop.name.clone(),
                            },
                            property_name: new_name.clone(),
                            new_def: Some(new_prop.clone()),
                            old_def: Some(old_prop.clone()),
                            needs_custom_converter: true,
                        });
                    } else {
                        changes.push(PropertyChange {
                            kind: ChangeKind::Renamed {
                                old_name: old_prop.name.clone(),
                            },
                            property_name: new_name.clone(),
                            new_def: Some(new_prop.clone()),
                            old_def: Some(old_prop.clone()),
                            needs_custom_converter: false,
                        });
                    }
                }
            } else if new.find_property(&old_prop.name).is_none() {
                // 删除
                changes.push(PropertyChange {
                    kind: ChangeKind::Removed,
                    property_name: old_prop.name.clone(),
                    new_def: None,
                    old_def: Some(old_prop.clone()),
                    needs_custom_converter: false,
                });
            } else {
                // 同名属性存在，检查类型变更
                let Some(new_prop) = new.find_property(&old_prop.name) else {
                    continue;
                };
                if old_prop.cpp_type != new_prop.cpp_type {
                    let has_converter = self
                        .type_converters
                        .contains_key(&(old_prop.cpp_type.clone(), new_prop.cpp_type.clone()));
                    changes.push(PropertyChange {
                        kind: ChangeKind::TypeChanged {
                            old_type: old_prop.cpp_type.clone(),
                        },
                        property_name: new_prop.name.clone(),
                        new_def: Some(new_prop.clone()),
                        old_def: Some(old_prop.clone()),
                        needs_custom_converter: !has_converter,
                    });
                } else if old_prop.default_value != new_prop.default_value {
                    changes.push(PropertyChange {
                        kind: ChangeKind::DefaultChanged,
                        property_name: new_prop.name.clone(),
                        new_def: Some(new_prop.clone()),
                        old_def: Some(old_prop.clone()),
                        needs_custom_converter: false,
                    });
                }
            }
        }

        // 检查新增属性
        let renamed_old_names: Vec<&String> =
            renames.map(|r| r.keys().collect()).unwrap_or_default();

        for new_prop in &new.properties {
            let is_renamed_target = renames
                .map(|r| r.values().any(|v| v == &new_prop.name))
                .unwrap_or(false);

            if !is_renamed_target && old.find_property(&new_prop.name).is_none() {
                // 检查是否有旧属性被重命名为此名字
                let from_rename = renamed_old_names
                    .iter()
                    .any(|old_name| renames.and_then(|r| r.get(*old_name)) == Some(&new_prop.name));

                if !from_rename {
                    changes.push(PropertyChange {
                        kind: ChangeKind::Added,
                        property_name: new_prop.name.clone(),
                        new_def: Some(new_prop.clone()),
                        old_def: None,
                        needs_custom_converter: false,
                    });
                }
            }
        }

        changes
    }

    /// 生成单步迁移代码
    pub fn generate_migration(&self, step: &MigrationStep) -> GeneratedMigration {
        let class = &step.class_name;
        let from = step.from_version;
        let to = step.to_version;

        // 头文件
        let header = self.generate_header(class, from, to);
        // 实现文件
        let impl_code = self.generate_impl(step);
        // 注册表
        let registry = self.generate_registry(class, from, to);
        // 测试骨架
        let test = self.generate_test_skeleton(step);

        GeneratedMigration {
            header_content: header,
            impl_content: impl_code,
            registry_content: registry,
            test_content: test,
            class_name: class.clone(),
            version_range: (from, to),
        }
    }

    /// 从版本快照列表生成迁移链
    pub fn generate_chain(&self, versions: &[TypeVersion]) -> Option<MigrationChain> {
        if versions.len() < 2 {
            return None;
        }

        let class_name = versions[0].class_name.clone();
        let mut steps = Vec::new();

        for window in versions.windows(2) {
            let old = &window[0];
            let new = &window[1];
            let changes = self.diff_versions(old, new);

            if !changes.is_empty() {
                steps.push(MigrationStep {
                    class_name: class_name.clone(),
                    from_version: old.version,
                    to_version: new.version,
                    changes,
                });
            }
        }

        if steps.is_empty() {
            return None;
        }

        Some(MigrationChain { class_name, steps })
    }

    // ─── 私有: 代码生成 ─────────────────────────────────────

    fn generate_header(&self, class: &str, from: u32, to: u32) -> String {
        let mut h = String::with_capacity(1024);
        h.push_str(&format!("// 自动生成 — {class} 迁移 V{from} → V{to}\n"));
        h.push_str("#pragma once\n\n");
        h.push_str("#include \"Reflection/MigrationBase.h\"\n\n");
        h.push_str(&format!("namespace Limx::Migration\n{{\n\n"));
        h.push_str(&format!(
            "class {class}_MigrateV{from}ToV{to} : public IMigrationStep\n{{\npublic:\n"
        ));
        h.push_str(&format!(
            "    static constexpr uint32 SourceVersion = {};\n",
            from
        ));
        h.push_str(&format!(
            "    static constexpr uint32 TargetVersion = {};\n\n",
            to
        ));
        h.push_str("    bool Execute(MigrationContext& Context) override;\n");
        h.push_str("    const char* GetDescription() const override;\n");
        h.push_str("};\n\n");
        h.push_str("} // namespace Limx::Migration\n");
        h
    }

    fn generate_impl(&self, step: &MigrationStep) -> String {
        let class = &step.class_name;
        let from = step.from_version;
        let to = step.to_version;

        let mut cpp = String::with_capacity(2048);
        cpp.push_str(&format!("// 自动生成 — {class} 迁移 V{from} → V{to}\n"));
        cpp.push_str(&format!("#include \"{class}_MigrateV{from}ToV{to}.h\"\n\n"));
        cpp.push_str(&format!("namespace Limx::Migration\n{{\n\n"));

        // Execute 函数
        cpp.push_str(&format!(
            "bool {class}_MigrateV{from}ToV{to}::Execute(MigrationContext& Context)\n{{\n"
        ));

        for change in &step.changes {
            match &change.kind {
                ChangeKind::Added => {
                    if let Some(def) = &change.new_def {
                        let default_val = def.default_value.as_deref().unwrap_or("{}");
                        cpp.push_str(&format!("    // 新增属性: {}\n", change.property_name));
                        cpp.push_str(&format!(
                            "    Context.SetProperty(\"{}\", {} {{{}}});\n\n",
                            change.property_name, def.cpp_type, default_val,
                        ));
                    }
                }
                ChangeKind::Removed => {
                    cpp.push_str(&format!(
                        "    // 删除属性: {} (旧数据将被忽略)\n",
                        change.property_name,
                    ));
                    cpp.push_str(&format!(
                        "    Context.RemoveProperty(\"{}\");\n\n",
                        change.property_name,
                    ));
                }
                ChangeKind::Renamed { old_name } => {
                    cpp.push_str(&format!(
                        "    // 重命名: {} → {}\n",
                        old_name, change.property_name,
                    ));
                    cpp.push_str(&format!(
                        "    Context.RenameProperty(\"{}\", \"{}\");\n\n",
                        old_name, change.property_name,
                    ));
                }
                ChangeKind::TypeChanged { old_type } => {
                    let new_type = change
                        .new_def
                        .as_ref()
                        .map(|d| d.cpp_type.as_str())
                        .unwrap_or("unknown");

                    cpp.push_str(&format!(
                        "    // 类型变更: {} ({} → {})\n",
                        change.property_name, old_type, new_type,
                    ));

                    let key = (old_type.clone(), new_type.to_string());
                    if let Some(converter) = self.type_converters.get(&key) {
                        let expr = converter.replace(
                            "${old}",
                            &format!(
                                "Context.GetProperty<{}>(\"{}\")",
                                old_type, change.property_name,
                            ),
                        );
                        cpp.push_str(&format!(
                            "    Context.ConvertProperty<{new_type}>(\"{}\", {expr});\n\n",
                            change.property_name,
                        ));
                    } else {
                        cpp.push_str(&format!(
                            "    // TODO: 需要自定义转换 {} → {}\n",
                            old_type, new_type,
                        ));
                        cpp.push_str(&format!(
                            "    // Context.ConvertProperty<{new_type}>(\"{}\", /* 转换表达式 */);\n\n",
                            change.property_name,
                        ));
                    }
                }
                ChangeKind::DefaultChanged => {
                    // 默认值变更通常不需要迁移现有数据
                    cpp.push_str(&format!(
                        "    // 默认值变更: {} (现有数据保持不变)\n\n",
                        change.property_name,
                    ));
                }
            }
        }

        cpp.push_str("    return true;\n}\n\n");

        // GetDescription
        cpp.push_str(&format!(
            "const char* {class}_MigrateV{from}ToV{to}::GetDescription() const\n{{\n"
        ));
        cpp.push_str(&format!(
            "    return \"{class}: 迁移 V{from} → V{to} ({} 项变更)\";\n",
            step.changes.len(),
        ));
        cpp.push_str("}\n\n");
        cpp.push_str("} // namespace Limx::Migration\n");
        cpp
    }

    fn generate_registry(&self, class: &str, from: u32, to: u32) -> String {
        let mut reg = String::with_capacity(512);
        reg.push_str(&format!("// 迁移注册 — {class}\n"));
        reg.push_str(&format!(
            "REGISTER_MIGRATION({class}, {from}, {to}, {class}_MigrateV{from}ToV{to});\n"
        ));
        reg
    }

    fn generate_test_skeleton(&self, step: &MigrationStep) -> String {
        let class = &step.class_name;
        let from = step.from_version;
        let to = step.to_version;

        let mut test = String::with_capacity(1024);
        test.push_str(&format!("// 迁移测试骨架 — {class} V{from} → V{to}\n"));
        test.push_str(&format!(
            "TEST_CASE(\"{class}_MigrateV{from}ToV{to}\")\n{{\n"
        ));
        test.push_str(&format!("    // 构造 V{from} 数据\n"));
        test.push_str("    MigrationContext ctx;\n");

        // 为每个变更生成测试步骤
        for change in &step.changes {
            match &change.kind {
                ChangeKind::Added => {
                    test.push_str(&format!(
                        "    // 验证新增属性 '{}' 有正确的默认值\n",
                        change.property_name,
                    ));
                }
                ChangeKind::Removed => {
                    if let Some(old_def) = &change.old_def {
                        test.push_str(&format!(
                            "    ctx.SetProperty(\"{}\", {} {{}});\n",
                            change.property_name, old_def.cpp_type,
                        ));
                    }
                }
                ChangeKind::Renamed { old_name } => {
                    if let Some(old_def) = &change.old_def {
                        test.push_str(&format!(
                            "    ctx.SetProperty(\"{}\", {} {{}});\n",
                            old_name, old_def.cpp_type,
                        ));
                    }
                }
                ChangeKind::TypeChanged { .. } => {
                    if let Some(old_def) = &change.old_def {
                        test.push_str(&format!(
                            "    ctx.SetProperty(\"{}\", {} {{}});\n",
                            change.property_name, old_def.cpp_type,
                        ));
                    }
                }
                ChangeKind::DefaultChanged => {}
            }
        }

        test.push_str(&format!(
            "\n    // 执行迁移\n    {class}_MigrateV{from}ToV{to} migration;\n"
        ));
        test.push_str("    REQUIRE(migration.Execute(ctx));\n\n");
        test.push_str("    // 验证结果\n");

        for change in &step.changes {
            match &change.kind {
                ChangeKind::Added => {
                    if let Some(new_def) = &change.new_def {
                        test.push_str(&format!(
                            "    CHECK(ctx.HasProperty(\"{}\"));\n",
                            change.property_name,
                        ));
                        if let Some(default_val) = &new_def.default_value {
                            test.push_str(&format!(
                                "    CHECK(ctx.GetProperty<{}>(\"{}\") == {});\n",
                                new_def.cpp_type, change.property_name, default_val,
                            ));
                        }
                    }
                }
                ChangeKind::Removed => {
                    test.push_str(&format!(
                        "    CHECK(!ctx.HasProperty(\"{}\"));\n",
                        change.property_name,
                    ));
                }
                ChangeKind::Renamed { old_name } => {
                    test.push_str(&format!("    CHECK(!ctx.HasProperty(\"{}\"));\n", old_name,));
                    test.push_str(&format!(
                        "    CHECK(ctx.HasProperty(\"{}\"));\n",
                        change.property_name,
                    ));
                }
                ChangeKind::TypeChanged { .. } => {
                    if let Some(new_def) = &change.new_def {
                        test.push_str(&format!(
                            "    CHECK(ctx.HasProperty<{}>(\"{}\"));\n",
                            new_def.cpp_type, change.property_name,
                        ));
                    }
                }
                ChangeKind::DefaultChanged => {}
            }
        }

        test.push_str("}\n");
        test
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_prop(name: &str, cpp_type: &str) -> PropertyDef {
        PropertyDef {
            name: name.to_string(),
            cpp_type: cpp_type.to_string(),
            default_value: None,
            is_serializable: true,
        }
    }

    fn make_prop_with_default(name: &str, cpp_type: &str, default: &str) -> PropertyDef {
        PropertyDef {
            name: name.to_string(),
            cpp_type: cpp_type.to_string(),
            default_value: Some(default.to_string()),
            is_serializable: true,
        }
    }

    #[test]
    fn test_detect_added_property() {
        let gen = MigrationGenerator::new();
        let v1 = TypeVersion {
            class_name: "APlayer".to_string(),
            version: 1,
            properties: vec![make_prop("Health", "float")],
        };
        let v2 = TypeVersion {
            class_name: "APlayer".to_string(),
            version: 2,
            properties: vec![make_prop("Health", "float"), make_prop("Stamina", "float")],
        };

        let changes = gen.diff_versions(&v1, &v2);
        assert_eq!(changes.len(), 1);
        assert_eq!(changes[0].kind, ChangeKind::Added);
        assert_eq!(changes[0].property_name, "Stamina");
    }

    #[test]
    fn test_detect_removed_property() {
        let gen = MigrationGenerator::new();
        let v1 = TypeVersion {
            class_name: "APlayer".to_string(),
            version: 1,
            properties: vec![make_prop("Health", "float"), make_prop("Mana", "float")],
        };
        let v2 = TypeVersion {
            class_name: "APlayer".to_string(),
            version: 2,
            properties: vec![make_prop("Health", "float")],
        };

        let changes = gen.diff_versions(&v1, &v2);
        assert_eq!(changes.len(), 1);
        assert_eq!(changes[0].kind, ChangeKind::Removed);
        assert_eq!(changes[0].property_name, "Mana");
    }

    #[test]
    fn test_detect_type_change() {
        let gen = MigrationGenerator::new();
        let v1 = TypeVersion {
            class_name: "APlayer".to_string(),
            version: 1,
            properties: vec![make_prop("Score", "int32")],
        };
        let v2 = TypeVersion {
            class_name: "APlayer".to_string(),
            version: 2,
            properties: vec![make_prop("Score", "int64")],
        };

        let changes = gen.diff_versions(&v1, &v2);
        assert_eq!(changes.len(), 1);
        assert!(matches!(changes[0].kind, ChangeKind::TypeChanged { .. }));
        assert!(
            !changes[0].needs_custom_converter,
            "int32→int64 有内置转换器"
        );
    }

    #[test]
    fn test_detect_rename() {
        let mut gen = MigrationGenerator::new();
        gen.add_rename_hint("APlayer", "HP", "Health");

        let v1 = TypeVersion {
            class_name: "APlayer".to_string(),
            version: 1,
            properties: vec![make_prop("HP", "float")],
        };
        let v2 = TypeVersion {
            class_name: "APlayer".to_string(),
            version: 2,
            properties: vec![make_prop("Health", "float")],
        };

        let changes = gen.diff_versions(&v1, &v2);
        assert_eq!(changes.len(), 1);
        assert!(matches!(changes[0].kind, ChangeKind::Renamed { .. }));
        assert_eq!(changes[0].property_name, "Health");
    }

    #[test]
    fn test_detect_default_change() {
        let gen = MigrationGenerator::new();
        let v1 = TypeVersion {
            class_name: "AWeapon".to_string(),
            version: 1,
            properties: vec![make_prop_with_default("Damage", "float", "10.0f")],
        };
        let v2 = TypeVersion {
            class_name: "AWeapon".to_string(),
            version: 2,
            properties: vec![make_prop_with_default("Damage", "float", "15.0f")],
        };

        let changes = gen.diff_versions(&v1, &v2);
        assert_eq!(changes.len(), 1);
        assert_eq!(changes[0].kind, ChangeKind::DefaultChanged);
    }

    #[test]
    fn test_no_changes() {
        let gen = MigrationGenerator::new();
        let v1 = TypeVersion {
            class_name: "AStatic".to_string(),
            version: 1,
            properties: vec![make_prop("Value", "int32")],
        };
        let v2 = TypeVersion {
            class_name: "AStatic".to_string(),
            version: 2,
            properties: vec![make_prop("Value", "int32")],
        };

        let changes = gen.diff_versions(&v1, &v2);
        assert!(changes.is_empty());
    }

    #[test]
    fn test_generate_migration_code() {
        let gen = MigrationGenerator::new();
        let step = MigrationStep {
            class_name: "APlayer".to_string(),
            from_version: 1,
            to_version: 2,
            changes: vec![
                PropertyChange {
                    kind: ChangeKind::Added,
                    property_name: "Stamina".to_string(),
                    new_def: Some(make_prop_with_default("Stamina", "float", "100.0f")),
                    old_def: None,
                    needs_custom_converter: false,
                },
                PropertyChange {
                    kind: ChangeKind::Removed,
                    property_name: "OldField".to_string(),
                    new_def: None,
                    old_def: Some(make_prop("OldField", "int32")),
                    needs_custom_converter: false,
                },
            ],
        };

        let result = gen.generate_migration(&step);

        // 头文件
        assert!(result.header_content.contains("APlayer_MigrateV1ToV2"));
        assert!(result.header_content.contains("IMigrationStep"));
        assert!(result.header_content.contains("SourceVersion = 1"));

        // 实现
        assert!(result.impl_content.contains("SetProperty(\"Stamina\""));
        assert!(result.impl_content.contains("RemoveProperty(\"OldField\""));
        assert!(result.impl_content.contains("return true;"));

        // 注册表
        assert!(result.registry_content.contains("REGISTER_MIGRATION"));

        // 测试
        assert!(result.test_content.contains("TEST_CASE"));
    }

    #[test]
    fn test_generate_type_change_with_converter() {
        let gen = MigrationGenerator::new();
        let step = MigrationStep {
            class_name: "APlayer".to_string(),
            from_version: 3,
            to_version: 4,
            changes: vec![PropertyChange {
                kind: ChangeKind::TypeChanged {
                    old_type: "int32".to_string(),
                },
                property_name: "Score".to_string(),
                new_def: Some(make_prop("Score", "int64")),
                old_def: Some(make_prop("Score", "int32")),
                needs_custom_converter: false,
            }],
        };

        let result = gen.generate_migration(&step);
        assert!(result.impl_content.contains("ConvertProperty<int64>"));
        assert!(result.impl_content.contains("static_cast<int64>"));
    }

    #[test]
    fn test_migration_chain() {
        let gen = MigrationGenerator::new();
        let versions = vec![
            TypeVersion {
                class_name: "AEnemy".to_string(),
                version: 1,
                properties: vec![make_prop("HP", "int32")],
            },
            TypeVersion {
                class_name: "AEnemy".to_string(),
                version: 2,
                properties: vec![make_prop("HP", "float"), make_prop("Armor", "float")],
            },
            TypeVersion {
                class_name: "AEnemy".to_string(),
                version: 3,
                properties: vec![
                    make_prop("HP", "float"),
                    make_prop("Armor", "float"),
                    make_prop("Shield", "float"),
                ],
            },
        ];

        let chain = gen.generate_chain(&versions).unwrap();
        assert_eq!(chain.class_name, "AEnemy");
        assert_eq!(chain.length(), 2);
        assert_eq!(chain.min_version(), 1);
        assert_eq!(chain.max_version(), 3);
    }

    #[test]
    fn test_has_breaking_changes() {
        let step = MigrationStep {
            class_name: "Test".to_string(),
            from_version: 1,
            to_version: 2,
            changes: vec![PropertyChange {
                kind: ChangeKind::Added,
                property_name: "New".to_string(),
                new_def: Some(make_prop("New", "int32")),
                old_def: None,
                needs_custom_converter: false,
            }],
        };
        assert!(!step.has_breaking_changes(), "仅新增不是破坏性变更");

        let step2 = MigrationStep {
            class_name: "Test".to_string(),
            from_version: 1,
            to_version: 2,
            changes: vec![PropertyChange {
                kind: ChangeKind::Removed,
                property_name: "Old".to_string(),
                new_def: None,
                old_def: Some(make_prop("Old", "int32")),
                needs_custom_converter: false,
            }],
        };
        assert!(step2.has_breaking_changes(), "删除是破坏性变更");
    }

    #[test]
    fn test_custom_type_converter() {
        let mut gen = MigrationGenerator::new();
        gen.add_type_converter(
            "FVector2D",
            "FVector3D",
            "FVector3D(${old}.X, ${old}.Y, 0.0f)",
        );

        let key = ("FVector2D".to_string(), "FVector3D".to_string());
        assert!(gen.type_converters.contains_key(&key));
    }

    #[test]
    fn test_complex_migration_multiple_changes() {
        let mut gen = MigrationGenerator::new();
        gen.add_rename_hint("ACharacter", "Vel", "Velocity");

        let v1 = TypeVersion {
            class_name: "ACharacter".to_string(),
            version: 1,
            properties: vec![
                make_prop("Health", "int32"),
                make_prop("Vel", "float"),
                make_prop("Legacy", "bool"),
            ],
        };
        let v2 = TypeVersion {
            class_name: "ACharacter".to_string(),
            version: 2,
            properties: vec![
                make_prop("Health", "float"),   // 类型变更
                make_prop("Velocity", "float"), // 重命名
                make_prop("Armor", "float"),    // 新增
                                                // Legacy 被删除
            ],
        };

        let changes = gen.diff_versions(&v1, &v2);

        // 应有4个变更: TypeChanged(Health), Renamed(Vel→Velocity), Removed(Legacy), Added(Armor)
        assert_eq!(
            changes.len(),
            4,
            "应检测到4个变更, 实际: {:?}",
            changes.iter().map(|c| &c.kind).collect::<Vec<_>>()
        );

        let type_changed = changes
            .iter()
            .find(|c| matches!(c.kind, ChangeKind::TypeChanged { .. }));
        assert!(type_changed.is_some(), "应检测到 Health 类型变更");

        let renamed = changes
            .iter()
            .find(|c| matches!(c.kind, ChangeKind::Renamed { .. }));
        assert!(renamed.is_some(), "应检测到 Vel → Velocity 重命名");

        let removed = changes.iter().find(|c| c.kind == ChangeKind::Removed);
        assert!(removed.is_some(), "应检测到 Legacy 被删除");

        let added = changes.iter().find(|c| c.kind == ChangeKind::Added);
        assert!(added.is_some(), "应检测到 Armor 新增");
    }
}
