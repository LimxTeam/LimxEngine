/*******************************************************************************
 * 文件: codegen/docs.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   API 文档生成器
 *   - 生成 Markdown 格式文档
 *   - 类型层次结构
 *   - 属性/函数列表
 *
 ******************************************************************************/

use anyhow::Result;
use std::fs;
use std::path::Path;

use crate::parser::parser::{
    AccessModifier, ClassInfo, EnumInfo, FunctionInfo, ReflectedType, StructInfo,
};

/// 文档生成配置
#[derive(Debug, Clone)]
pub struct DocsConfig {
    /// 输出格式
    pub format: DocsFormat,
    /// 是否包含私有成员
    pub include_private: bool,
    /// 是否生成索引
    pub generate_index: bool,
}

/// 文档格式
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum DocsFormat {
    Markdown,
    Html,
}

impl Default for DocsConfig {
    fn default() -> Self {
        Self {
            format: DocsFormat::Markdown,
            include_private: false,
            generate_index: true,
        }
    }
}

/// 生成 API 文档
pub fn generate_api_docs(
    types: &[ReflectedType],
    output_dir: &Path,
    config: &DocsConfig,
) -> Result<()> {
    fs::create_dir_all(output_dir)?;

    let mut index = String::new();
    index.push_str("# Limx Engine API 文档\n\n");
    index.push_str("> 由 LHT (Limx Header Tool) 自动生成\n\n");

    // 统计信息
    let class_count = types
        .iter()
        .filter(|t| matches!(t, ReflectedType::Class(_)))
        .count();
    let struct_count = types
        .iter()
        .filter(|t| matches!(t, ReflectedType::Struct(_)))
        .count();
    let enum_count = types
        .iter()
        .filter(|t| matches!(t, ReflectedType::Enum(_)))
        .count();

    index.push_str("## 概览\n\n");
    index.push_str(&format!("| 类型 | 数量 |\n"));
    index.push_str("|------|------|\n");
    index.push_str(&format!("| 类 | {} |\n", class_count));
    index.push_str(&format!("| 结构体 | {} |\n", struct_count));
    index.push_str(&format!("| 枚举 | {} |\n", enum_count));
    index.push_str("\n");

    // 类文档
    if class_count > 0 {
        index.push_str("## 类\n\n");
        for t in types {
            if let ReflectedType::Class(info) = t {
                let doc = generate_class_doc(info, config);
                let filename = format!("{}.md", info.name);
                fs::write(output_dir.join(&filename), &doc)?;
                index.push_str(&format!("- [{}]({})\n", info.name, filename));
            }
        }
        index.push_str("\n");
    }

    // 结构体文档
    if struct_count > 0 {
        index.push_str("## 结构体\n\n");
        for t in types {
            if let ReflectedType::Struct(info) = t {
                let doc = generate_struct_doc(info, config);
                let filename = format!("{}.md", info.name);
                fs::write(output_dir.join(&filename), &doc)?;
                index.push_str(&format!("- [{}]({})\n", info.name, filename));
            }
        }
        index.push_str("\n");
    }

    // 枚举文档
    if enum_count > 0 {
        index.push_str("## 枚举\n\n");
        for t in types {
            if let ReflectedType::Enum(info) = t {
                let doc = generate_enum_doc(info, config);
                let filename = format!("{}.md", info.name);
                fs::write(output_dir.join(&filename), &doc)?;
                index.push_str(&format!("- [{}]({})\n", info.name, filename));
            }
        }
        index.push_str("\n");
    }

    // 写入索引
    if config.generate_index {
        fs::write(output_dir.join("README.md"), index)?;
    }

    Ok(())
}

/// 生成类文档
fn generate_class_doc(info: &ClassInfo, config: &DocsConfig) -> String {
    let mut doc = String::new();

    // 标题
    doc.push_str(&format!("# {}\n\n", info.name));

    // 类型标签
    let mut tags = Vec::new();
    if info.is_abstract {
        tags.push("`abstract`");
    }
    if info.is_final {
        tags.push("`final`");
    }
    if info.specifiers.serializable {
        tags.push("`serializable`");
    }
    if info.specifiers.blueprint_type {
        tags.push("`blueprint`");
    }

    if !tags.is_empty() {
        doc.push_str(&format!("{}\n\n", tags.join(" ")));
    }

    // 继承关系
    if !info.base_classes.is_empty() {
        doc.push_str("## 继承\n\n");
        let base_names: Vec<_> = info.base_classes.iter().map(|b| b.name.as_str()).collect();
        doc.push_str(&format!("**基类**: {}\n\n", base_names.join(", ")));
    }

    // 属性
    let props: Vec<_> = info
        .properties
        .iter()
        .filter(|p| config.include_private || p.access == AccessModifier::Public)
        .collect();

    if !props.is_empty() {
        doc.push_str("## 属性\n\n");
        doc.push_str("| 名称 | 类型 | 描述 |\n");
        doc.push_str("|------|------|------|\n");

        for prop in props {
            doc.push_str(&format!("| `{}` | `{}` | - |\n", prop.name, prop.type_name));
        }
        doc.push_str("\n");
    }

    // 函数
    let funcs: Vec<_> = info
        .functions
        .iter()
        .filter(|f| config.include_private || f.access == AccessModifier::Public)
        .collect();

    if !funcs.is_empty() {
        doc.push_str("## 函数\n\n");

        for func in funcs {
            doc.push_str(&format!("### {}\n\n", func.name));
            doc.push_str(&format!(
                "```cpp\n{}\n```\n\n",
                format_function_signature(func)
            ));

            if !func.parameters.is_empty() {
                doc.push_str("**参数**:\n");
                for param in &func.parameters {
                    doc.push_str(&format!("- `{}`: `{}`\n", param.name, param.type_name));
                }
                doc.push_str("\n");
            }

            doc.push_str(&format!("**返回**: `{}`\n\n", func.return_type));
        }
    }

    doc
}

/// 生成结构体文档
fn generate_struct_doc(info: &StructInfo, _config: &DocsConfig) -> String {
    let mut doc = String::new();

    doc.push_str(&format!("# {}\n\n", info.name));
    doc.push_str("`struct`\n\n");

    // 属性
    if !info.properties.is_empty() {
        doc.push_str("## 属性\n\n");
        doc.push_str("| 名称 | 类型 | 描述 |\n");
        doc.push_str("|------|------|------|\n");

        for prop in &info.properties {
            doc.push_str(&format!("| `{}` | `{}` | - |\n", prop.name, prop.type_name));
        }
        doc.push_str("\n");
    }

    doc
}

/// 生成枚举文档
fn generate_enum_doc(info: &EnumInfo, _config: &DocsConfig) -> String {
    let mut doc = String::new();

    doc.push_str(&format!("# {}\n\n", info.name));

    if info.is_flags {
        doc.push_str("`enum flags`\n\n");
    } else {
        doc.push_str("`enum`\n\n");
    }

    // 值
    if !info.values.is_empty() {
        doc.push_str("## 值\n\n");
        doc.push_str("| 名称 | 值 | 描述 |\n");
        doc.push_str("|------|-----|------|\n");

        for (i, value) in info.values.iter().enumerate() {
            let display = value.display_name.as_deref().unwrap_or("-");
            let val_str = value
                .value
                .map(|v| v.to_string())
                .unwrap_or_else(|| i.to_string());
            doc.push_str(&format!(
                "| `{}` | {} | {} |\n",
                value.name, val_str, display
            ));
        }
        doc.push_str("\n");
    }

    doc
}

/// 格式化函数签名
fn format_function_signature(func: &FunctionInfo) -> String {
    let params: Vec<String> = func
        .parameters
        .iter()
        .map(|p| format!("{} {}", p.type_name, p.name))
        .collect();

    format!("{} {}({})", func.return_type, func.name, params.join(", "))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_docs_config_default() {
        let config = DocsConfig::default();
        assert_eq!(config.format, DocsFormat::Markdown);
        assert!(!config.include_private);
        assert!(config.generate_index);
    }
}
