/*******************************************************************************
 * 文件: scanner.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   头文件扫描功能
 *   - 遍历源目录查找 .h/.hpp 文件
 *   - 快速过滤包含反射宏的文件
 *
 ******************************************************************************/

use anyhow::Result;
use std::fs;
use std::path::{Path, PathBuf};
use tracing::debug;
use walkdir::WalkDir;

/// 反射宏关键字
const REFLECTION_MACROS: &[&str] = &["LCLASS", "LSTRUCT", "LENUM", "LPROPERTY", "LFUNCTION"];

/// 扫描目录中的头文件
pub fn scan_headers(source_dir: &Path, module_filter: Option<&str>) -> Result<Vec<PathBuf>> {
    let mut headers = Vec::new();

    if !source_dir.exists() {
        return Err(anyhow::anyhow!("源目录不存在: {}", source_dir.display()));
    }

    for entry in WalkDir::new(source_dir).into_iter().filter_map(|e| e.ok()) {
        let path = entry.path();

        // 只处理头文件
        if !path.is_file() {
            continue;
        }

        let extension = path.extension().and_then(|e| e.to_str());
        if !matches!(extension, Some("h") | Some("hpp")) {
            continue;
        }

        // 跳过已生成的文件
        if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
            if name.contains(".generated.") {
                continue;
            }
        }

        // 模块过滤
        if let Some(filter) = module_filter {
            let path_str = path.to_string_lossy();
            if !path_str.contains(filter) {
                continue;
            }
        }

        // 快速检查是否包含反射宏
        if contains_reflection_macro(path)? {
            debug!("发现反射头文件: {}", path.display());
            headers.push(path.to_path_buf());
        }
    }

    Ok(headers)
}

/// 快速检查文件是否包含反射宏
fn contains_reflection_macro(path: &Path) -> Result<bool> {
    let content = fs::read_to_string(path)?;

    for macro_name in REFLECTION_MACROS {
        if content.contains(macro_name) {
            return Ok(true);
        }
    }

    Ok(false)
}
