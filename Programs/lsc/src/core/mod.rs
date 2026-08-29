/*******************************************************************************
 * 文件: core/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LSC 核心类型定义
 *
 ******************************************************************************/

pub mod error;

pub use error::*;

use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::str::FromStr;

//=============================================================================
// 着色器阶段
//=============================================================================

/// 着色器阶段
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default, Serialize, Deserialize)]
pub enum ShaderStage {
    #[default]
    Vertex,
    Fragment,
    Compute,
    Geometry,
    TessellationControl,
    TessellationEvaluation,
    RayGeneration,
    RayIntersection,
    RayAnyHit,
    RayClosestHit,
    RayMiss,
    Callable,
    Task,
    Mesh,
}

impl ShaderStage {
    /// 从文件扩展名推断阶段
    pub fn from_extension(ext: &str) -> Self {
        match ext.to_lowercase().as_str() {
            "vert" | "vs" | "vsh" => Self::Vertex,
            "frag" | "fs" | "fsh" | "ps" => Self::Fragment,
            "comp" | "cs" | "csh" => Self::Compute,
            "geom" | "gs" | "gsh" => Self::Geometry,
            "tesc" | "tcs" => Self::TessellationControl,
            "tese" | "tes" => Self::TessellationEvaluation,
            "rgen" => Self::RayGeneration,
            "rint" => Self::RayIntersection,
            "rahit" => Self::RayAnyHit,
            "rchit" => Self::RayClosestHit,
            "rmiss" => Self::RayMiss,
            "rcall" => Self::Callable,
            "task" => Self::Task,
            "mesh" => Self::Mesh,
            _ => Self::Vertex, // 默认
        }
    }

    /// 获取 shaderc 着色器类型
    pub fn to_shaderc_kind(&self) -> shaderc::ShaderKind {
        match self {
            Self::Vertex => shaderc::ShaderKind::Vertex,
            Self::Fragment => shaderc::ShaderKind::Fragment,
            Self::Compute => shaderc::ShaderKind::Compute,
            Self::Geometry => shaderc::ShaderKind::Geometry,
            Self::TessellationControl => shaderc::ShaderKind::TessControl,
            Self::TessellationEvaluation => shaderc::ShaderKind::TessEvaluation,
            Self::RayGeneration => shaderc::ShaderKind::RayGeneration,
            Self::RayIntersection => shaderc::ShaderKind::Intersection,
            Self::RayAnyHit => shaderc::ShaderKind::AnyHit,
            Self::RayClosestHit => shaderc::ShaderKind::ClosestHit,
            Self::RayMiss => shaderc::ShaderKind::Miss,
            Self::Callable => shaderc::ShaderKind::Callable,
            Self::Task => shaderc::ShaderKind::Task,
            Self::Mesh => shaderc::ShaderKind::Mesh,
        }
    }

    /// 获取阶段名称
    pub fn name(&self) -> &'static str {
        match self {
            Self::Vertex => "Vertex",
            Self::Fragment => "Fragment",
            Self::Compute => "Compute",
            Self::Geometry => "Geometry",
            Self::TessellationControl => "TessControl",
            Self::TessellationEvaluation => "TessEval",
            Self::RayGeneration => "RayGen",
            Self::RayIntersection => "RayIntersect",
            Self::RayAnyHit => "RayAnyHit",
            Self::RayClosestHit => "RayClosestHit",
            Self::RayMiss => "RayMiss",
            Self::Callable => "Callable",
            Self::Task => "Task",
            Self::Mesh => "Mesh",
        }
    }
}

impl FromStr for ShaderStage {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s.to_lowercase().as_str() {
            "vertex" | "vert" | "vs" => Ok(Self::Vertex),
            "fragment" | "frag" | "fs" | "pixel" | "ps" => Ok(Self::Fragment),
            "compute" | "comp" | "cs" => Ok(Self::Compute),
            "geometry" | "geom" | "gs" => Ok(Self::Geometry),
            "tesscontrol" | "tesc" | "tcs" => Ok(Self::TessellationControl),
            "tesseval" | "tese" | "tes" => Ok(Self::TessellationEvaluation),
            "raygen" | "rgen" => Ok(Self::RayGeneration),
            "rayintersect" | "rint" => Ok(Self::RayIntersection),
            "rayanyhit" | "rahit" => Ok(Self::RayAnyHit),
            "rayclosesthit" | "rchit" => Ok(Self::RayClosestHit),
            "raymiss" | "rmiss" => Ok(Self::RayMiss),
            "callable" | "rcall" => Ok(Self::Callable),
            "task" => Ok(Self::Task),
            "mesh" => Ok(Self::Mesh),
            "auto" => Ok(Self::Vertex), // 自动检测时默认
            _ => Err(format!("未知的着色器阶段: {}", s)),
        }
    }
}

//=============================================================================
// 着色器源码
//=============================================================================

/// 着色器源码
#[derive(Debug, Clone)]
pub struct ShaderSource {
    /// 源代码
    pub code: String,
    /// 文件路径 (可选)
    pub file_path: Option<PathBuf>,
    /// 着色器阶段
    pub stage: ShaderStage,
}

impl ShaderSource {
    /// 从文件加载
    pub fn from_file(path: &PathBuf) -> Result<Self, LscError> {
        let code = std::fs::read_to_string(path).map_err(|e| LscError::Io(e.to_string()))?;

        let stage =
            ShaderStage::from_extension(path.extension().and_then(|e| e.to_str()).unwrap_or(""));

        Ok(Self {
            code,
            file_path: Some(path.clone()),
            stage,
        })
    }

    /// 获取文件名
    pub fn file_name(&self) -> String {
        self.file_path
            .as_ref()
            .and_then(|p| p.file_name())
            .and_then(|n| n.to_str())
            .unwrap_or("unknown")
            .to_string()
    }
}

//=============================================================================
// 编译选项
//=============================================================================

/// 目标 Vulkan 环境
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum TargetEnvironment {
    Vulkan1_0,
    Vulkan1_1,
    Vulkan1_2,
    Vulkan1_3,
    #[default]
    Vulkan1_4,
}

impl TargetEnvironment {
    pub fn to_shaderc_env(&self) -> shaderc::TargetEnv {
        shaderc::TargetEnv::Vulkan
    }

    /// 目标环境版本号 — 使用 Vulkan 的 API 版本编码
    /// (major << 22) | (minor << 12) | patch，与 shaderc::EnvVersion 的
    /// 判别值一致。1.4 在 shaderc 0.8.3 的 EnvVersion 枚举中尚未收录，
    /// 因此按同一编码规则直接构造。
    pub fn to_shaderc_version(&self) -> u32 {
        match self {
            Self::Vulkan1_0 => shaderc::EnvVersion::Vulkan1_0 as u32,
            Self::Vulkan1_1 => shaderc::EnvVersion::Vulkan1_1 as u32,
            Self::Vulkan1_2 => shaderc::EnvVersion::Vulkan1_2 as u32,
            Self::Vulkan1_3 => shaderc::EnvVersion::Vulkan1_3 as u32,
            Self::Vulkan1_4 => (1u32 << 22) | (4u32 << 12),
        }
    }

    /// 该环境要求的 SPIR-V 版本
    ///
    /// Vulkan 1.3 与 1.4 同为 SPIR-V 1.6，故两者产出的字节码目标一致；
    /// 差异仅体现在 glslang 的验证规则集上。
    pub fn to_spirv_version(&self) -> shaderc::SpirvVersion {
        match self {
            Self::Vulkan1_0 => shaderc::SpirvVersion::V1_0,
            Self::Vulkan1_1 => shaderc::SpirvVersion::V1_3,
            Self::Vulkan1_2 => shaderc::SpirvVersion::V1_5,
            Self::Vulkan1_3 | Self::Vulkan1_4 => shaderc::SpirvVersion::V1_6,
        }
    }

    /// 人类可读的版本字符串
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Vulkan1_0 => "1.0",
            Self::Vulkan1_1 => "1.1",
            Self::Vulkan1_2 => "1.2",
            Self::Vulkan1_3 => "1.3",
            Self::Vulkan1_4 => "1.4",
        }
    }
}

impl FromStr for TargetEnvironment {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s.to_lowercase().as_str() {
            "vulkan1.0" | "vk1.0" | "1.0" => Ok(Self::Vulkan1_0),
            "vulkan1.1" | "vk1.1" | "1.1" => Ok(Self::Vulkan1_1),
            "vulkan1.2" | "vk1.2" | "1.2" => Ok(Self::Vulkan1_2),
            "vulkan1.3" | "vk1.3" | "1.3" => Ok(Self::Vulkan1_3),
            "vulkan1.4" | "vk1.4" | "1.4" => Ok(Self::Vulkan1_4),
            _ => Err(format!("未知的目标环境: {}", s)),
        }
    }
}

/// 编译选项
#[derive(Debug, Clone)]
pub struct CompileOptions {
    /// 入口点函数名
    pub entry_point: String,
    /// 预处理器宏定义 (名称, 值)
    pub defines: Vec<(String, Option<String>)>,
    /// 头文件包含目录
    pub include_dirs: Vec<PathBuf>,
    /// 优化级别
    pub optimization_level: crate::compiler::OptimizationLevel,
    /// 是否生成调试信息
    pub generate_debug_info: bool,
    /// 目标环境
    pub target_environment: TargetEnvironment,
    /// 是否启用 16 位类型
    pub enable_16bit_types: bool,
    /// HLSL 着色器模型
    pub hlsl_shader_model: Option<String>,
    /// 是否自动绑定 uniform
    pub auto_bind_uniforms: bool,
    /// 是否生成反射信息
    pub generate_reflection: bool,
}

impl Default for CompileOptions {
    fn default() -> Self {
        Self::new()
    }
}

impl CompileOptions {
    pub fn new() -> Self {
        Self {
            entry_point: "main".to_string(),
            defines: Vec::new(),
            include_dirs: Vec::new(),
            optimization_level: crate::compiler::OptimizationLevel::None,
            generate_debug_info: false,
            target_environment: TargetEnvironment::Vulkan1_3,
            enable_16bit_types: false,
            hlsl_shader_model: None,
            auto_bind_uniforms: false,
            generate_reflection: true,
        }
    }

    /// 添加宏定义
    pub fn define(&mut self, name: &str, value: Option<&str>) -> &mut Self {
        self.defines
            .push((name.to_string(), value.map(|s| s.to_string())));
        self
    }

    /// 添加包含目录
    pub fn include_dir(&mut self, dir: PathBuf) -> &mut Self {
        self.include_dirs.push(dir);
        self
    }
}

//=============================================================================
// 编译结果
//=============================================================================

/// 编译结果
#[derive(Debug, Clone)]
pub struct CompileResult {
    /// SPIR-V 二进制
    pub spirv_binary: Vec<u8>,
    /// 警告信息
    pub warnings: Vec<String>,
    /// 反射信息
    pub reflection: Option<crate::reflection::ShaderReflection>,
}

/// 批量编译结果
#[derive(Debug, Clone)]
pub struct BatchCompileResult {
    /// 源文件路径
    pub source_path: PathBuf,
    /// 输出文件路径
    pub output_path: PathBuf,
    /// 是否成功
    pub success: bool,
    /// 输出大小
    pub output_size: usize,
    /// 错误信息
    pub errors: Vec<String>,
    /// 警告信息
    pub warnings: Vec<String>,
    /// 编译耗时 (毫秒)
    pub duration_ms: u64,
}
