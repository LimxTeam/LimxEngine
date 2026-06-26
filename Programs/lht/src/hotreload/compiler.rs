/*******************************************************************************
 * 文件: hotreload/compiler.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   增量编译器 - 编译变更的代码为可热重载的 DLL
 *   - 增量编译
 *   - 并行编译
 *   - 编译缓存
 *
 ******************************************************************************/

use anyhow::{anyhow, Result};
use rayon::prelude::*;
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};

use super::HotReloadConfig;

/// 编译结果
#[derive(Debug)]
pub struct CompileResult {
    /// 是否成功
    pub success: bool,
    /// 输出 DLL 路径
    pub output_dll: Option<PathBuf>,
    /// 编译耗时 (ms)
    pub duration_ms: u64,
    /// 错误信息
    pub errors: Vec<String>,
    /// 警告信息
    pub warnings: Vec<String>,
}

/// 增量编译器
pub struct IncrementalCompiler {
    /// 配置
    config: HotReloadConfig,
    /// 编译缓存 (文件 -> 对象文件)
    object_cache: HashMap<PathBuf, PathBuf>,
    /// 编译器路径
    compiler_path: PathBuf,
    /// 链接器路径
    linker_path: PathBuf,
}

impl IncrementalCompiler {
    /// 创建新的增量编译器
    pub fn new(config: &HotReloadConfig) -> Self {
        Self {
            config: config.clone(),
            object_cache: HashMap::new(),
            compiler_path: Self::find_compiler(),
            linker_path: Self::find_linker(),
        }
    }

    /// 查找编译器
    fn find_compiler() -> PathBuf {
        // 尝试查找 MSVC cl.exe
        if let Ok(vs_path) = std::env::var("VS2022INSTALLDIR") {
            let cl_path = PathBuf::from(vs_path)
                .join("VC/Tools/MSVC")
                .join("14.38.33130") // 可能需要动态检测版本
                .join("bin/Hostx64/x64/cl.exe");
            if cl_path.exists() {
                return cl_path;
            }
        }

        // 使用 vswhere 查找
        if let Ok(output) = Command::new("vswhere")
            .args(["-latest", "-property", "installationPath"])
            .output()
        {
            if output.status.success() {
                let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
                // 查找最新的 MSVC 版本
                let vc_tools = PathBuf::from(&path).join("VC/Tools/MSVC");
                if let Ok(entries) = fs::read_dir(&vc_tools) {
                    let mut versions: Vec<_> =
                        entries.filter_map(|e| e.ok()).map(|e| e.path()).collect();
                    versions.sort();
                    if let Some(latest) = versions.last() {
                        let cl_path = latest.join("bin/Hostx64/x64/cl.exe");
                        if cl_path.exists() {
                            return cl_path;
                        }
                    }
                }
            }
        }

        // 默认假设在 PATH 中
        PathBuf::from("cl.exe")
    }

    /// 查找链接器
    fn find_linker() -> PathBuf {
        // 链接器通常与编译器在同一目录
        let compiler = Self::find_compiler();
        compiler
            .parent()
            .map(|p| p.join("link.exe"))
            .unwrap_or_else(|| PathBuf::from("link.exe"))
    }

    /// 编译变更的文件
    pub fn compile(&self, changed_files: &[PathBuf]) -> Result<()> {
        if changed_files.is_empty() {
            return Ok(());
        }

        // 确保输出目录存在
        fs::create_dir_all(&self.config.output_dir)?;

        // 分离头文件和源文件
        let (headers, sources): (Vec<_>, Vec<_>) = changed_files.iter().partition(|f| {
            f.extension()
                .map(|e| e == "h" || e == "hpp")
                .unwrap_or(false)
        });

        // 如果只有头文件变更，需要找到包含它们的源文件
        let files_to_compile = if sources.is_empty() && !headers.is_empty() {
            self.find_dependent_sources(&headers)?
        } else {
            sources.into_iter().cloned().collect()
        };

        if files_to_compile.is_empty() {
            return Ok(());
        }

        // 编译每个源文件 (并行)
        let obj_dir = self.config.output_dir.join("obj");
        fs::create_dir_all(&obj_dir)?;

        // 使用 rayon 并行编译
        let errors: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let object_files: Vec<PathBuf> = files_to_compile
            .par_iter()
            .filter_map(|source| match self.compile_source(source, &obj_dir) {
                Ok(obj) => Some(obj),
                Err(e) => {
                    if let Ok(mut errs) = errors.lock() {
                        errs.push(format!("{}: {}", source.display(), e));
                    }
                    None
                }
            })
            .collect();

        // 检查编译错误
        let Ok(compile_errors) = errors.lock() else {
            return Err(anyhow!("Failed to access error list"));
        };
        if !compile_errors.is_empty() {
            return Err(anyhow!(
                "Compilation failed:\n{}",
                compile_errors.join("\n")
            ));
        }

        if object_files.is_empty() {
            return Ok(());
        }

        // 链接成 DLL
        self.link_dll(&object_files)?;

        Ok(())
    }

    /// 编译单个源文件
    fn compile_source(&self, source: &Path, obj_dir: &Path) -> Result<PathBuf> {
        let file_stem = source
            .file_stem()
            .ok_or_else(|| anyhow!("Invalid source file: {:?}", source))?;
        let obj_file = obj_dir.join(format!("{}.obj", file_stem.to_string_lossy()));

        // 构建编译命令
        let mut cmd = Command::new(&self.compiler_path);
        cmd.args([
            "/c",                  // 仅编译
            "/EHsc",               // 异常处理
            "/std:c++23",          // C++23 标准
            "/utf-8",              // UTF-8 编码
            "/O2",                 // 优化
            "/MD",                 // 多线程 DLL 运行时
            "/DLIMX_HOT_RELOAD=1", // 热重载宏
            "/DLIMX_DLL_EXPORT=1", // DLL 导出
        ]);

        // 添加包含目录
        for dir in &self.config.source_dirs {
            cmd.arg(format!("/I{}", dir.display()));
        }

        // 输入和输出
        cmd.arg(format!("/Fo{}", obj_file.display()));
        cmd.arg(source);

        // 执行编译
        let output = cmd.output()?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            return Err(anyhow!("Compilation failed: {}", stderr));
        }

        Ok(obj_file)
    }

    /// 链接成 DLL
    fn link_dll(&self, object_files: &[PathBuf]) -> Result<PathBuf> {
        let dll_path = self.config.output_dir.join("HotReload.dll");

        let mut cmd = Command::new(&self.linker_path);
        cmd.args([
            "/DLL",
            "/INCREMENTAL",
            &format!("/OUT:{}", dll_path.display()),
        ]);

        for obj in object_files {
            cmd.arg(obj);
        }

        let output = cmd.output()?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            return Err(anyhow!("Linking failed: {}", stderr));
        }

        Ok(dll_path)
    }

    /// 查找依赖指定头文件的源文件
    fn find_dependent_sources(&self, headers: &[&PathBuf]) -> Result<Vec<PathBuf>> {
        let mut dependents = Vec::new();

        for dir in &self.config.source_dirs {
            self.search_dependents(dir, headers, &mut dependents)?;
        }

        Ok(dependents)
    }

    /// 递归搜索依赖源文件
    fn search_dependents(
        &self,
        dir: &Path,
        headers: &[&PathBuf],
        dependents: &mut Vec<PathBuf>,
    ) -> Result<()> {
        if !dir.exists() {
            return Ok(());
        }

        for entry in fs::read_dir(dir)? {
            let entry = entry?;
            let path = entry.path();

            if path.is_dir() {
                self.search_dependents(&path, headers, dependents)?;
            } else if let Some(ext) = path.extension() {
                if ext == "cpp" || ext == "c" {
                    // 检查是否包含变更的头文件
                    if self.source_includes_headers(&path, headers)? {
                        if !dependents.contains(&path) {
                            dependents.push(path);
                        }
                    }
                }
            }
        }

        Ok(())
    }

    /// 检查源文件是否包含指定的头文件
    fn source_includes_headers(&self, source: &Path, headers: &[&PathBuf]) -> Result<bool> {
        let content = fs::read_to_string(source)?;

        for header in headers {
            if let Some(header_name) = header.file_name() {
                let header_str = header_name.to_string_lossy();
                // 简单的 #include 检查
                if content.contains(&format!("#include \"{}", header_str))
                    || content.contains(&format!("#include <{}", header_str))
                {
                    return Ok(true);
                }
            }
        }

        Ok(false)
    }

    /// 清理编译缓存
    pub fn clean(&mut self) -> Result<()> {
        self.object_cache.clear();
        if self.config.output_dir.exists() {
            fs::remove_dir_all(&self.config.output_dir)?;
        }
        Ok(())
    }
}

/// 编译诊断信息
#[derive(Debug, Clone)]
pub struct CompileDiagnostic {
    /// 文件路径
    pub file: PathBuf,
    /// 行号
    pub line: u32,
    /// 列号
    pub column: u32,
    /// 消息
    pub message: String,
    /// 严重程度
    pub severity: DiagnosticSeverity,
}

/// 诊断严重程度
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum DiagnosticSeverity {
    Error,
    Warning,
    Note,
}

impl CompileDiagnostic {
    /// 解析 MSVC 编译器输出
    pub fn parse_msvc_output(output: &str) -> Vec<Self> {
        let mut diagnostics = Vec::new();

        for line in output.lines() {
            // MSVC 格式: file(line,col): error/warning CODE: message
            if let Some(diag) = Self::parse_msvc_line(line) {
                diagnostics.push(diag);
            }
        }

        diagnostics
    }

    fn parse_msvc_line(line: &str) -> Option<Self> {
        // 简单解析，实际实现需要更复杂的正则
        let error_idx = line.find(": error ");
        let warning_idx = line.find(": warning ");

        let (severity, msg_start) = if let Some(idx) = error_idx {
            (DiagnosticSeverity::Error, idx)
        } else if let Some(idx) = warning_idx {
            (DiagnosticSeverity::Warning, idx)
        } else {
            return None;
        };

        let file_part = &line[..msg_start];
        let message = line[msg_start + 2..].to_string();

        // 解析文件和位置
        if let Some(paren_start) = file_part.rfind('(') {
            let file = PathBuf::from(&file_part[..paren_start]);
            // 简化：不解析行列号
            return Some(Self {
                file,
                line: 0,
                column: 0,
                message,
                severity,
            });
        }

        None
    }
}
