// ============================================================
// 文件名称：generators/compile_commands.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：标准 JSON 格式输出，零外部工具依赖，IDE 无缝集成
// 功能描述：生成 compile_commands.json — 提供给 clangd/clang-tidy/
//           VS Code/Rider/CLion 等工具精确的编译命令信息
//           支持完整的包含路径、宏定义、编译标志，超越 CMake 生成
// 技术特性：并行文件扫描，增量更新，多平台路径归一化，
//           编译器感知标志转换 (MSVC → Clang 语法)
//
// ── 函数表 ──────────────────────────────────────────────────
// │ generate_compile_commands()     │ 生成完整 compile_commands.json │
// │ generate_entry()                │ 为单个源文件生成条目            │
// │ normalize_flags_for_clang()     │ MSVC 标志转换为 clang 兼容格式  │
// │ CompileCommandsGenerator        │ 主生成器结构体                  │
// │ CompileCommandEntry             │ 单条目结构体                    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                               │
// │ 2026-04-06   │ LimxTeam  │ 初始创建                            │
// ============================================================

use anyhow::Result;
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

use crate::core::config::Module;
use crate::core::dependency::DependencyGraph;

// ──────────────────────────────────────────────────────────────
// compile_commands.json 条目结构
// ──────────────────────────────────────────────────────────────

/// compile_commands.json 单个条目 (遵循 LLVM JSON Compilation Database 规范)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CompileCommandEntry {
    /// 编译工作目录 (绝对路径)
    pub directory: String,

    /// 源文件路径 (绝对路径)
    pub file: String,

    /// 完整编译命令字符串
    pub command: String,

    /// 编译命令参数数组 (与 command 二选一，优先使用 arguments)
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub arguments: Vec<String>,

    /// 输出文件路径
    #[serde(skip_serializing_if = "Option::is_none")]
    pub output: Option<String>,
}

// ──────────────────────────────────────────────────────────────
// 生成器配置
// ──────────────────────────────────────────────────────────────

/// compile_commands.json 生成配置
#[derive(Debug, Clone)]
pub struct CompileCommandsConfig {
    /// 工程根目录
    pub project_root: PathBuf,

    /// 编译器路径 (用于 clangd 识别)
    pub compiler_path: String,

    /// C++ 标准
    pub cpp_standard: String,

    /// 全局宏定义
    pub global_defines: Vec<String>,

    /// 全局包含路径
    pub global_include_paths: Vec<PathBuf>,

    /// 全局编译标志 (将被转换为 clang 格式)
    pub global_flags: Vec<String>,

    /// 是否使用 arguments 数组格式 (vs command 字符串格式)
    pub use_arguments_format: bool,

    /// 是否包含系统头文件路径
    pub include_system_includes: bool,
}

impl Default for CompileCommandsConfig {
    fn default() -> Self {
        Self {
            project_root: PathBuf::from("."),
            compiler_path: "clang++".to_string(),
            cpp_standard: "c++23".to_string(),
            global_defines: vec![
                "LIMX_ENGINE=1".to_string(),
                "UNICODE=1".to_string(),
                "_UNICODE=1".to_string(),
            ],
            global_include_paths: Vec::new(),
            global_flags: Vec::new(),
            use_arguments_format: true,
            include_system_includes: false,
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 主生成器
// ──────────────────────────────────────────────────────────────

/// compile_commands.json 生成器
pub struct CompileCommandsGenerator {
    config: CompileCommandsConfig,
}

impl CompileCommandsGenerator {
    pub fn new(config: CompileCommandsConfig) -> Self {
        Self { config }
    }

    /// 为所有模块生成 compile_commands.json
    pub fn generate(
        &self,
        graph: &DependencyGraph,
        modules: &[Module],
        output_path: &Path,
    ) -> Result<usize> {
        let entries = self.collect_all_entries(graph, modules)?;
        let entry_count = entries.len();

        let json = serde_json::to_string_pretty(&entries)?;
        std::fs::write(output_path, json)?;

        Ok(entry_count)
    }

    /// 收集所有模块的编译命令条目
    fn collect_all_entries(
        &self,
        graph: &DependencyGraph,
        modules: &[Module],
    ) -> Result<Vec<CompileCommandEntry>> {
        // 构建全局包含路径 (所有模块 Public 目录)
        let all_public_dirs: Vec<PathBuf> = modules
            .iter()
            .map(|m| m.path.join("Public"))
            .filter(|p| p.exists())
            .collect();

        // 并行处理每个模块
        let results: Vec<Vec<CompileCommandEntry>> = modules
            .par_iter()
            .map(|module| {
                self.generate_module_entries(module, modules, graph, &all_public_dirs)
                    .unwrap_or_default()
            })
            .collect();

        Ok(results.into_iter().flatten().collect())
    }

    /// 为单个模块生成所有源文件的编译命令条目
    fn generate_module_entries(
        &self,
        module: &Module,
        all_modules: &[Module],
        graph: &DependencyGraph,
        all_public_dirs: &[PathBuf],
    ) -> Result<Vec<CompileCommandEntry>> {
        let mut entries = Vec::new();

        // 收集该模块的源文件
        let source_files = collect_cpp_files(&module.path);
        if source_files.is_empty() {
            return Ok(entries);
        }

        // 构建此模块的包含路径
        let mut include_paths: Vec<PathBuf> =
            vec![module.path.join("Public"), module.path.join("Private")];

        // 添加所有模块的 Public 目录
        include_paths.extend(all_public_dirs.iter().cloned());

        // 添加依赖模块的 Public 目录 (精确)
        if let Some(deps) = graph.edges.get(&module.name) {
            for dep_name in deps {
                if let Some(dep_module) = all_modules.iter().find(|m| &m.name == dep_name) {
                    let dep_public = dep_module.path.join("Public");
                    if dep_public.exists() && !include_paths.contains(&dep_public) {
                        include_paths.push(dep_public);
                    }
                }
            }
        }

        // 添加全局包含路径
        include_paths.extend(self.config.global_include_paths.iter().cloned());

        // 去重
        include_paths.dedup();

        // 构建宏定义
        let mut defines: Vec<String> = self.config.global_defines.clone();
        defines.extend(module.config.compile.defines.iter().cloned());

        // 添加 API 导出宏
        if let Some(ref api_macro) = module.config.module.api_macro {
            let export_macro = format!("{}_EXPORTS=1", api_macro.replace("_API", ""));
            defines.push(export_macro);
        }

        // 为每个源文件生成条目
        for source_file in &source_files {
            let entry = self.build_entry(source_file, &include_paths, &defines);
            entries.push(entry);
        }

        Ok(entries)
    }

    /// 构建单个源文件的编译命令条目
    fn build_entry(
        &self,
        source_file: &Path,
        include_paths: &[PathBuf],
        defines: &[String],
    ) -> CompileCommandEntry {
        let work_dir = self.config.project_root.to_str().unwrap_or(".").to_string();

        let file_path = source_file.to_str().unwrap_or("").replace('\\', "/");

        // 构建参数列表
        let mut args: Vec<String> = vec![self.config.compiler_path.clone()];

        // C++ 标准
        args.push(format!("-std={}", self.config.cpp_standard));

        // 包含路径
        for include in include_paths {
            if let Some(path_str) = include.to_str() {
                args.push(format!("-I{}", path_str.replace('\\', "/")));
            }
        }

        // 宏定义
        for define in defines {
            args.push(format!("-D{}", define));
        }

        // 全局标志 (转换为 clang 格式)
        for flag in &self.config.global_flags {
            args.extend(normalize_flags_for_clang(flag));
        }

        // 源文件
        args.push("-c".to_string());
        args.push(file_path.clone());

        // 输出文件
        let output_file = source_file
            .with_extension("o")
            .to_str()
            .unwrap_or("")
            .replace('\\', "/");
        args.push("-o".to_string());
        args.push(output_file.clone());

        if self.config.use_arguments_format {
            CompileCommandEntry {
                directory: work_dir,
                file: file_path,
                command: String::new(),
                arguments: args,
                output: Some(output_file),
            }
        } else {
            let command = args.join(" ");
            CompileCommandEntry {
                directory: work_dir,
                file: file_path,
                command,
                arguments: Vec::new(),
                output: Some(output_file),
            }
        }
    }

    /// 增量更新 compile_commands.json
    /// 只更新发生变化的模块的条目
    pub fn update_incremental(
        &self,
        existing_path: &Path,
        graph: &DependencyGraph,
        changed_modules: &[&Module],
        all_modules: &[Module],
    ) -> Result<usize> {
        // 读取现有的条目
        let mut all_entries: HashMap<String, CompileCommandEntry> = if existing_path.exists() {
            let json = std::fs::read_to_string(existing_path)?;
            let existing: Vec<CompileCommandEntry> =
                serde_json::from_str(&json).unwrap_or_default();
            existing.into_iter().map(|e| (e.file.clone(), e)).collect()
        } else {
            HashMap::new()
        };

        let all_public_dirs: Vec<PathBuf> = all_modules
            .iter()
            .map(|m| m.path.join("Public"))
            .filter(|p| p.exists())
            .collect();

        // 更新变化模块的条目
        let mut update_count = 0;
        for module in changed_modules {
            let new_entries =
                self.generate_module_entries(module, all_modules, graph, &all_public_dirs)?;

            // 先删除此模块的旧条目 (通过检查文件是否属于该模块目录)
            let module_path_prefix = module.path.to_str().unwrap_or("").replace('\\', "/");
            all_entries.retain(|file, _| !file.starts_with(&module_path_prefix));

            update_count += new_entries.len();
            for entry in new_entries {
                all_entries.insert(entry.file.clone(), entry);
            }
        }

        // 写回
        let final_entries: Vec<&CompileCommandEntry> = {
            let mut v: Vec<&CompileCommandEntry> = all_entries.values().collect();
            v.sort_by(|a, b| a.file.cmp(&b.file));
            v
        };

        let json = serde_json::to_string_pretty(&final_entries)?;
        std::fs::write(existing_path, json)?;

        Ok(update_count)
    }
}

// ──────────────────────────────────────────────────────────────
// 辅助函数
// ──────────────────────────────────────────────────────────────

/// 收集目录下所有 C++ 源文件 (.cpp/.cc/.cxx)
fn collect_cpp_files(dir: &Path) -> Vec<PathBuf> {
    let mut files = Vec::new();
    let private_dir = dir.join("Private");
    let src_dir = dir.join("Src");

    for search_dir in &[&private_dir, &src_dir, dir] {
        if !search_dir.exists() {
            continue;
        }
        if let Ok(iter) = walkdir::WalkDir::new(search_dir)
            .max_depth(8)
            .into_iter()
            .filter_map(|e| e.ok())
            .map(|e| e.path().to_path_buf())
            .filter(|p| {
                if let Some(ext) = p.extension().and_then(|e| e.to_str()) {
                    matches!(ext.to_lowercase().as_str(), "cpp" | "cc" | "cxx" | "c")
                } else {
                    false
                }
            })
            .collect::<Vec<_>>()
            .into_iter()
            .map(Ok::<PathBuf, ()>)
            .collect::<Result<Vec<_>, _>>()
        {
            // iter is Vec<PathBuf>
            files.extend(iter);
        }
    }
    files.dedup();
    files
}

/// 将 MSVC 编译标志归一化为 Clang 兼容格式
/// 供 clangd 等工具识别使用
fn normalize_flags_for_clang(flag: &str) -> Vec<String> {
    match flag {
        // MSVC 警告级别
        "/W4" => vec!["-Wall".to_string(), "-Wextra".to_string()],
        "/WX" => vec!["-Werror".to_string()],
        "/W3" => vec!["-Wall".to_string()],
        "/W2" => vec!["-Wextra".to_string()],
        "/W1" => vec!["-Wmost".to_string()],
        "/W0" | "/w" => vec!["-w".to_string()],
        // MSVC 优化
        "/Od" => vec!["-O0".to_string()],
        "/O1" => vec!["-O1".to_string()],
        "/O2" => vec!["-O2".to_string()],
        "/Ox" => vec!["-O3".to_string()],
        // MSVC 调试信息
        "/Zi" | "/ZI" => vec!["-g".to_string()],
        // MSVC RTTI
        "/GR" => vec!["-frtti".to_string()],
        "/GR-" => vec!["-fno-rtti".to_string()],
        // MSVC 异常
        "/EHsc" => vec!["-fexceptions".to_string()],
        "/EHs-c-" => vec!["-fno-exceptions".to_string()],
        // MSVC 字符集
        "/utf-8" => vec!["-finput-charset=UTF-8".to_string()],
        // 其他 MSVC 特定标志 — 忽略
        f if f.starts_with("/FS") => vec![],
        f if f.starts_with("/Fp") => vec![],
        f if f.starts_with("/Yu") => vec![],
        f if f.starts_with("/Yc") => vec![],
        f if f.starts_with("/MP") => vec![],
        f if f.starts_with("/GL") => vec!["-flto".to_string()],
        // 直接透传 Clang 格式标志
        f if f.starts_with('-') => vec![f.to_string()],
        // 未知 MSVC 标志 — 忽略，避免 clangd 出错
        _ => vec![],
    }
}

/// 生成带有 Target 信息的完整 compile_commands.json
pub fn generate_compile_commands(
    graph: &DependencyGraph,
    modules: &[Module],
    project_root: &Path,
    output_path: &Path,
    compiler_path: Option<&str>,
) -> Result<usize> {
    let config = CompileCommandsConfig {
        project_root: project_root.to_path_buf(),
        compiler_path: compiler_path.unwrap_or("clang++").to_string(),
        ..Default::default()
    };

    let generator = CompileCommandsGenerator::new(config);
    generator.generate(graph, modules, output_path)
}

// ──────────────────────────────────────────────────────────────
// 单元测试
// ──────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_normalize_msvc_flags() {
        assert_eq!(normalize_flags_for_clang("/W4"), vec!["-Wall", "-Wextra"]);
        assert_eq!(normalize_flags_for_clang("/WX"), vec!["-Werror"]);
        assert_eq!(normalize_flags_for_clang("/Od"), vec!["-O0"]);
        assert_eq!(normalize_flags_for_clang("/O2"), vec!["-O2"]);
        assert_eq!(normalize_flags_for_clang("/Zi"), vec!["-g"]);
        assert_eq!(normalize_flags_for_clang("/GR-"), vec!["-fno-rtti"]);
        assert_eq!(normalize_flags_for_clang("/EHsc"), vec!["-fexceptions"]);
        assert!(normalize_flags_for_clang("/FS").is_empty());
    }

    #[test]
    fn test_compile_command_entry_serialization() {
        let entry = CompileCommandEntry {
            directory: "/project".to_string(),
            file: "/project/Source/Core/Private/MyClass.cpp".to_string(),
            command: String::new(),
            arguments: vec![
                "clang++".to_string(),
                "-std=c++23".to_string(),
                "-DLIMX_ENGINE=1".to_string(),
            ],
            output: Some("/project/Intermediate/Core/MyClass.o".to_string()),
        };

        let json = serde_json::to_string(&entry).unwrap();
        assert!(json.contains("clang++"));
        assert!(json.contains("-std=c++23"));
        assert!(json.contains("LIMX_ENGINE=1"));
    }
}
