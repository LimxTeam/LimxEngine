/*******************************************************************************
 * 文件: compiler/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LSC 着色器编译器核心实现
 *   - 基于 shaderc (glslang) 的 SPIR-V 编译
 *   - 支持 GLSL 和 HLSL
 *   - 自定义 #include 处理
 *   - 并行批量编译
 *
 ******************************************************************************/

pub mod debug_info;
pub mod include_graph;
pub mod perf_analyzer;
pub mod spirv_optimizer;
pub mod stage_interface;
pub mod variant_pruner;

use anyhow::Result;
use rayon::prelude::*;
use shaderc::{CompileOptions as ShadercOptions, Compiler, IncludeType, ResolvedInclude};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use crate::core::{BatchCompileResult, CompileOptions, CompileResult, ShaderSource};
use crate::reflection::ShaderReflection;

//=============================================================================
// SPIR-V 验证结果
//=============================================================================

/// SPIR-V 验证结果
#[derive(Debug, Clone)]
pub struct SpirvValidationResult {
    /// 是否有效
    pub valid: bool,
    /// 错误列表
    pub errors: Vec<String>,
    /// 警告列表
    pub warnings: Vec<String>,
    /// SPIR-V 版本 (主版本, 次版本)
    pub version: (u8, u8),
    /// 生成器 ID
    pub generator: u32,
    /// ID Bound
    pub bound: u32,
    /// 指令数量
    pub instruction_count: usize,
}

impl SpirvValidationResult {
    /// 检查是否有错误
    pub fn has_errors(&self) -> bool {
        !self.errors.is_empty()
    }

    /// 检查是否有警告
    pub fn has_warnings(&self) -> bool {
        !self.warnings.is_empty()
    }

    /// 打印摘要
    pub fn print_summary(&self) {
        if self.valid {
            println!("✓ SPIR-V 验证通过");
        } else {
            println!("✗ SPIR-V 验证失败");
        }
        println!("  版本: {}.{}", self.version.0, self.version.1);
        println!("  ID Bound: {}", self.bound);
        println!("  指令数: {}", self.instruction_count);

        if !self.errors.is_empty() {
            println!("  错误 ({}):", self.errors.len());
            for err in &self.errors {
                println!("    - {}", err);
            }
        }
        if !self.warnings.is_empty() {
            println!("  警告 ({}):", self.warnings.len());
            for warn in &self.warnings {
                println!("    - {}", warn);
            }
        }
    }
}

//=============================================================================
// 优化级别
//=============================================================================

/// 优化级别
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum OptimizationLevel {
    /// 无优化
    #[default]
    None,
    /// 大小优化
    Size,
    /// 性能优化
    Performance,
}

impl OptimizationLevel {
    pub fn to_shaderc(&self) -> shaderc::OptimizationLevel {
        match self {
            Self::None => shaderc::OptimizationLevel::Zero,
            Self::Size => shaderc::OptimizationLevel::Size,
            Self::Performance => shaderc::OptimizationLevel::Performance,
        }
    }
}

//=============================================================================
// Include 回调
//=============================================================================

/// Include 解析器
struct IncludeResolver {
    include_dirs: Vec<PathBuf>,
    source_dir: Option<PathBuf>,
}

impl IncludeResolver {
    fn new(include_dirs: Vec<PathBuf>, source_dir: Option<PathBuf>) -> Self {
        Self {
            include_dirs,
            source_dir,
        }
    }

    fn resolve(
        &self,
        name: &str,
        include_type: IncludeType,
        source_file: &str,
    ) -> Result<ResolvedInclude, String> {
        let search_paths = match include_type {
            IncludeType::Relative => {
                // 相对包含：先从源文件目录搜索
                let mut paths = Vec::new();

                // 源文件所在目录
                let source_path = Path::new(source_file);
                if let Some(parent) = source_path.parent() {
                    paths.push(parent.to_path_buf());
                }

                // 然后是 include 目录
                paths.extend(self.include_dirs.clone());

                // 最后是原始源目录
                if let Some(ref dir) = self.source_dir {
                    paths.push(dir.clone());
                }

                paths
            }
            IncludeType::Standard => {
                // 标准包含：只搜索 include 目录
                self.include_dirs.clone()
            }
        };

        for dir in &search_paths {
            let full_path = dir.join(name);
            if full_path.exists() {
                match std::fs::read_to_string(&full_path) {
                    Ok(content) => {
                        return Ok(ResolvedInclude {
                            resolved_name: full_path.to_string_lossy().to_string(),
                            content,
                        });
                    }
                    Err(e) => {
                        return Err(format!("无法读取包含文件 '{}': {}", full_path.display(), e));
                    }
                }
            }
        }

        Err(format!("找不到包含文件: '{}' (从 '{}')", name, source_file))
    }
}

//=============================================================================
// 着色器编译器
//=============================================================================

/// 着色器编译器
pub struct ShaderCompiler {
    compiler: Compiler,
}

impl ShaderCompiler {
    /// 创建新的编译器实例
    pub fn new() -> Result<Self> {
        let compiler =
            Compiler::new().ok_or_else(|| anyhow::anyhow!("无法初始化 shaderc 编译器"))?;

        Ok(Self { compiler })
    }

    /// 编译着色器
    pub fn compile(
        &self,
        source: &ShaderSource,
        options: &CompileOptions,
    ) -> Result<CompileResult> {
        let mut shaderc_options =
            ShadercOptions::new().ok_or_else(|| anyhow::anyhow!("无法创建编译选项"))?;

        // 设置目标环境
        shaderc_options.set_target_env(
            options.target_environment.to_shaderc_env(),
            options.target_environment.to_shaderc_version(),
        );

        // 设置优化级别
        shaderc_options.set_optimization_level(options.optimization_level.to_shaderc());

        // 设置调试信息
        if options.generate_debug_info {
            shaderc_options.set_generate_debug_info();
        }

        // 添加宏定义
        for (name, value) in &options.defines {
            match value {
                Some(v) => shaderc_options.add_macro_definition(name, Some(v.as_str())),
                None => shaderc_options.add_macro_definition(name, None),
            }
        }

        // 设置 include 回调
        let source_dir = source
            .file_path
            .as_ref()
            .and_then(|p| p.parent().map(|d| d.to_path_buf()));
        let resolver = IncludeResolver::new(options.include_dirs.clone(), source_dir);

        shaderc_options.set_include_callback(move |name, include_type, source_file, _depth| {
            resolver.resolve(name, include_type, source_file)
        });

        // 编译
        let file_name = source.file_name();
        let shader_kind = source.stage.to_shaderc_kind();

        let result = self
            .compiler
            .compile_into_spirv(
                &source.code,
                shader_kind,
                &file_name,
                &options.entry_point,
                Some(&shaderc_options),
            )
            .map_err(|e| anyhow::anyhow!("编译失败: {}", e))?;

        // 收集警告
        let warnings: Vec<String> = if result.get_num_warnings() > 0 {
            result
                .get_warning_messages()
                .lines()
                .map(|s| s.to_string())
                .collect()
        } else {
            Vec::new()
        };

        let spirv_binary = result.as_binary_u8().to_vec();

        // 生成反射信息
        let reflection = if options.generate_reflection {
            self.reflect_spirv(&spirv_binary).ok()
        } else {
            None
        };

        Ok(CompileResult {
            spirv_binary,
            warnings,
            reflection,
        })
    }

    /// 批量编译着色器
    pub fn compile_batch(
        &self,
        sources: &[PathBuf],
        options: &CompileOptions,
        output_dir: &Path,
        source_root: &Path,
    ) -> Result<Vec<BatchCompileResult>> {
        let mut results = Vec::with_capacity(sources.len());

        for source_path in sources {
            let result = self.compile_single(source_path, options, output_dir, source_root);
            results.push(result);
        }

        Ok(results)
    }

    /// 批量编译着色器（带进度回调）
    pub fn compile_batch_with_progress<F>(
        &self,
        sources: &[PathBuf],
        options: &CompileOptions,
        output_dir: &Path,
        source_root: &Path,
        on_complete: F,
    ) -> Result<Vec<BatchCompileResult>>
    where
        F: Fn(&BatchCompileResult),
    {
        let mut results = Vec::with_capacity(sources.len());

        for source_path in sources {
            let result = self.compile_single(source_path, options, output_dir, source_root);
            on_complete(&result);
            results.push(result);
        }

        Ok(results)
    }

    /// 并行批量编译着色器
    pub fn compile_batch_parallel(
        &self,
        sources: &[PathBuf],
        options: &CompileOptions,
        output_dir: &Path,
        source_root: &Path,
    ) -> Result<Vec<BatchCompileResult>> {
        // shaderc::Compiler 不是线程安全的，需要为每个线程创建新实例
        let options = Arc::new(options.clone());
        let output_dir = output_dir.to_path_buf();
        let source_root = source_root.to_path_buf();

        let results: Vec<BatchCompileResult> = sources
            .par_iter()
            .map(|source_path| {
                // 每个线程创建自己的编译器
                let compiler = match ShaderCompiler::new() {
                    Ok(c) => c,
                    Err(e) => {
                        return BatchCompileResult {
                            source_path: source_path.clone(),
                            output_path: PathBuf::new(),
                            success: false,
                            output_size: 0,
                            errors: vec![e.to_string()],
                            warnings: Vec::new(),
                            duration_ms: 0,
                        };
                    }
                };

                compiler.compile_single(source_path, &options, &output_dir, &source_root)
            })
            .collect();

        Ok(results)
    }

    /// 并行批量编译着色器（带进度回调）
    pub fn compile_batch_parallel_with_progress<F>(
        &self,
        sources: &[PathBuf],
        options: &CompileOptions,
        output_dir: &Path,
        source_root: &Path,
        on_complete: F,
    ) -> Result<Vec<BatchCompileResult>>
    where
        F: Fn(&BatchCompileResult) + Send + Sync,
    {
        let options = Arc::new(options.clone());
        let output_dir = output_dir.to_path_buf();
        let source_root = source_root.to_path_buf();
        let callback = &on_complete;

        let results: Vec<BatchCompileResult> = sources
            .par_iter()
            .map(|source_path| {
                let compiler = match ShaderCompiler::new() {
                    Ok(c) => c,
                    Err(e) => {
                        let result = BatchCompileResult {
                            source_path: source_path.clone(),
                            output_path: PathBuf::new(),
                            success: false,
                            output_size: 0,
                            errors: vec![e.to_string()],
                            warnings: Vec::new(),
                            duration_ms: 0,
                        };
                        callback(&result);
                        return result;
                    }
                };

                let result =
                    compiler.compile_single(source_path, &options, &output_dir, &source_root);
                callback(&result);
                result
            })
            .collect();

        Ok(results)
    }

    /// 编译单个着色器文件
    ///
    /// `source_root` 用于计算相对路径，保留源码目录结构。
    /// 例如：source_root="Shaders/", source_path="Shaders/Builtin/pbr.vert"
    /// → 相对路径 "Builtin/pbr.vert" → 输出 "output_dir/Builtin/pbr.vert.spv"
    fn compile_single(
        &self,
        source_path: &Path,
        options: &CompileOptions,
        output_dir: &Path,
        source_root: &Path,
    ) -> BatchCompileResult {
        let start = Instant::now();

        // 读取源文件
        let source = match ShaderSource::from_file(&source_path.to_path_buf()) {
            Ok(s) => s,
            Err(e) => {
                return BatchCompileResult {
                    source_path: source_path.to_path_buf(),
                    output_path: PathBuf::new(),
                    success: false,
                    output_size: 0,
                    errors: vec![e.to_string()],
                    warnings: Vec::new(),
                    duration_ms: start.elapsed().as_millis() as u64,
                };
            }
        };

        // 生成输出路径 — 保留源码目录结构，文件名追加 .spv
        // Shaders/Builtin/pbr.vert → Binaries/Shaders/Builtin/pbr.vert.spv
        let relative_path = source_path.strip_prefix(source_root).unwrap_or(source_path);
        let output_name = format!(
            "{}.spv",
            relative_path
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_else(|| "unknown.spv".to_string())
        );
        let output_path = output_dir
            .join(relative_path.parent().unwrap_or(Path::new("")))
            .join(&output_name);

        // 编译
        match self.compile(&source, options) {
            Ok(result) => {
                // 确保输出子目录存在（保留目录结构）
                let actual_output_dir = output_path.parent().unwrap_or(output_dir);
                if let Err(e) = std::fs::create_dir_all(actual_output_dir) {
                    return BatchCompileResult {
                        source_path: source_path.to_path_buf(),
                        output_path,
                        success: false,
                        output_size: 0,
                        errors: vec![format!("无法创建输出目录: {}", e)],
                        warnings: result.warnings,
                        duration_ms: start.elapsed().as_millis() as u64,
                    };
                }

                // 写入输出
                if let Err(e) = std::fs::write(&output_path, &result.spirv_binary) {
                    return BatchCompileResult {
                        source_path: source_path.to_path_buf(),
                        output_path,
                        success: false,
                        output_size: 0,
                        errors: vec![format!("无法写入输出文件: {}", e)],
                        warnings: result.warnings,
                        duration_ms: start.elapsed().as_millis() as u64,
                    };
                }

                // 写入反射信息
                if let Some(ref reflection) = result.reflection {
                    let reflection_path = output_path.with_extension("json");
                    if let Ok(json) = serde_json::to_string_pretty(reflection) {
                        let _ = std::fs::write(&reflection_path, json);
                    }
                }

                BatchCompileResult {
                    source_path: source_path.to_path_buf(),
                    output_path,
                    success: true,
                    output_size: result.spirv_binary.len(),
                    errors: Vec::new(),
                    warnings: result.warnings,
                    duration_ms: start.elapsed().as_millis() as u64,
                }
            }
            Err(e) => BatchCompileResult {
                source_path: source_path.to_path_buf(),
                output_path,
                success: false,
                output_size: 0,
                errors: vec![e.to_string()],
                warnings: Vec::new(),
                duration_ms: start.elapsed().as_millis() as u64,
            },
        }
    }

    /// 从 SPIR-V 提取反射信息
    pub fn reflect_spirv(&self, spirv: &[u8]) -> Result<ShaderReflection> {
        ShaderReflection::from_spirv(spirv)
    }

    /// 验证 SPIR-V - 完整的结构验证
    ///
    /// 执行以下验证:
    /// 1. 魔数验证 (0x07230203)
    /// 2. 版本验证 (1.0 - 1.6)
    /// 3. 头部长度验证
    /// 4. 指令边界验证
    /// 5. OpCode 有效性验证
    pub fn validate_spirv(&self, spirv: &[u8]) -> Result<SpirvValidationResult> {
        let mut result = SpirvValidationResult {
            valid: true,
            errors: Vec::new(),
            warnings: Vec::new(),
            version: (0, 0),
            generator: 0,
            bound: 0,
            instruction_count: 0,
        };

        // 最小长度检查 (头部 5 个 u32)
        if spirv.len() < 20 {
            result.valid = false;
            result
                .errors
                .push("SPIR-V 文件太小，最小需要 20 字节".to_string());
            return Ok(result);
        }

        // 确保长度是 4 的倍数
        if spirv.len() % 4 != 0 {
            result.valid = false;
            result
                .errors
                .push(format!("SPIR-V 长度 {} 不是 4 的倍数", spirv.len()));
            return Ok(result);
        }

        // 解析为 u32 数组
        let words: Vec<u32> = spirv
            .chunks_exact(4)
            .map(|chunk| u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
            .collect();

        // 1. 魔数验证
        let magic = words[0];
        if magic != 0x07230203 {
            result.valid = false;
            result.errors.push(format!(
                "无效的 SPIR-V 魔数: 0x{:08X}，期望 0x07230203",
                magic
            ));
            return Ok(result);
        }

        // 2. 版本验证
        let version = words[1];
        let major = (version >> 16) & 0xFF;
        let minor = (version >> 8) & 0xFF;
        result.version = (major as u8, minor as u8);

        if major < 1 || major > 1 || minor > 6 {
            result.warnings.push(format!(
                "SPIR-V 版本 {}.{} 可能不受支持 (推荐 1.0-1.6)",
                major, minor
            ));
        }

        // 3. 生成器 ID
        result.generator = words[2];

        // 4. Bound (最大 ID + 1)
        result.bound = words[3];
        if result.bound == 0 {
            result.valid = false;
            result.errors.push("无效的 ID Bound: 0".to_string());
            return Ok(result);
        }
        if result.bound > 4_194_304 {
            result.warnings.push(format!(
                "ID Bound {} 非常大，可能导致性能问题",
                result.bound
            ));
        }

        // 5. 保留字段
        if words[4] != 0 {
            result
                .warnings
                .push(format!("保留字段应为 0，实际值: {}", words[4]));
        }

        // 6. 指令验证
        let mut offset = 5; // 跳过头部
        while offset < words.len() {
            let word = words[offset];
            let word_count = (word >> 16) as usize;
            let opcode = word & 0xFFFF;

            if word_count == 0 {
                result.valid = false;
                result
                    .errors
                    .push(format!("偏移 {} 处的指令字数为 0", offset));
                break;
            }

            if offset + word_count > words.len() {
                result.valid = false;
                result.errors.push(format!(
                    "偏移 {} 处的指令超出边界: 需要 {} 字，剩余 {} 字",
                    offset,
                    word_count,
                    words.len() - offset
                ));
                break;
            }

            // 验证常见 OpCode
            if !Self::is_valid_opcode(opcode as u16) {
                result.warnings.push(format!(
                    "偏移 {} 处的 OpCode {} 可能无效或未知",
                    offset, opcode
                ));
            }

            result.instruction_count += 1;
            offset += word_count;
        }

        Ok(result)
    }

    /// 检查 OpCode 是否有效
    fn is_valid_opcode(opcode: u16) -> bool {
        // SPIR-V 1.6 常用 OpCode 范围
        // 完整验证需要 spirv-headers 库
        matches!(opcode,
            0..=87 |           // 核心指令
            109..=127 |        // 类型声明
            128..=255 |        // 常量和变量
            256..=320 |        // 函数和控制流
            321..=400 |        // 其他核心指令
            4416..=4500 |      // GLSL 扩展
            5000..=5100        // Vulkan 扩展
        )
    }

    /// 反汇编 SPIR-V (简化版)
    pub fn disassemble_spirv(&self, spirv: &[u8]) -> Result<String> {
        let validation = self.validate_spirv(spirv)?;
        if !validation.valid {
            return Err(anyhow::anyhow!(
                "无法反汇编无效的 SPIR-V: {:?}",
                validation.errors
            ));
        }

        let mut output = String::with_capacity(spirv.len() * 4);

        output.push_str(&format!(
            "; SPIR-V 版本 {}.{}\n",
            validation.version.0, validation.version.1
        ));
        output.push_str(&format!("; 生成器 ID: 0x{:08X}\n", validation.generator));
        output.push_str(&format!("; ID Bound: {}\n", validation.bound));
        output.push_str(&format!("; 指令数: {}\n\n", validation.instruction_count));

        // 简化的反汇编 - 只显示 OpCode 和操作数
        let words: Vec<u32> = spirv
            .chunks_exact(4)
            .map(|chunk| u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
            .collect();

        let mut offset = 5;
        while offset < words.len() {
            let word = words[offset];
            let word_count = (word >> 16) as usize;
            let opcode = word & 0xFFFF;

            output.push_str(&format!("  Op{} ", opcode));
            for i in 1..word_count {
                if offset + i < words.len() {
                    output.push_str(&format!("%{} ", words[offset + i]));
                }
            }
            output.push('\n');

            offset += word_count;
        }

        Ok(output)
    }

    /// 获取编译器版本
    pub fn version(&self) -> String {
        "shaderc (glslang)".to_string()
    }
}

//=============================================================================
// 着色器预处理
//=============================================================================

/// 预处理着色器源码
pub fn preprocess_shader(source: &str, defines: &[(String, Option<String>)]) -> String {
    let mut result = String::with_capacity(source.len() + 256);

    // 添加版本声明 (如果没有)
    if !source.contains("#version") {
        result.push_str("#version 460 core\n");
    }

    // 添加扩展
    result.push_str("#extension GL_GOOGLE_include_directive : require\n");
    result.push_str("#extension GL_EXT_scalar_block_layout : enable\n");

    // 添加宏定义
    for (name, value) in defines {
        match value {
            Some(v) => result.push_str(&format!("#define {} {}\n", name, v)),
            None => result.push_str(&format!("#define {}\n", name)),
        }
    }

    result.push_str("\n");
    result.push_str(source);

    result
}

/// 检测着色器源语言
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SourceLanguage {
    Glsl,
    Hlsl,
}

impl SourceLanguage {
    pub fn detect(source: &str, file_path: Option<&Path>) -> Self {
        // 从文件扩展名检测
        if let Some(path) = file_path {
            if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                match ext.to_lowercase().as_str() {
                    "hlsl" | "fx" => return Self::Hlsl,
                    "glsl" | "vert" | "frag" | "comp" | "geom" | "tesc" | "tese" => {
                        return Self::Glsl
                    }
                    _ => {}
                }
            }
        }

        // 从内容检测
        if source.contains("cbuffer") || source.contains("SV_") || source.contains("register(") {
            Self::Hlsl
        } else {
            Self::Glsl
        }
    }
}
