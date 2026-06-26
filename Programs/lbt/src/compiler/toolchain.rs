/*******************************************************************************
 * 文件: compiler/toolchain.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   工具链管理 - 统一管理编译器、链接器、工具集
 *   - 工具链注册与查找
 *   - 版本管理
 *   - 工具链验证
 *
 ******************************************************************************/

use anyhow::{anyhow, Result};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

use super::platform::{PlatformInfo, VisualStudioInfo, WindowsSdkInfo};
use super::{Architecture, Platform};

//=============================================================================
// 工具链类型
//=============================================================================

/// 工具链类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ToolchainType {
    /// Microsoft Visual C++
    Msvc,
    /// LLVM/Clang
    Clang,
    /// GNU Compiler Collection
    Gcc,
    /// Intel C++ Compiler
    Intel,
    /// Apple Clang (Xcode)
    AppleClang,
}

impl ToolchainType {
    /// 获取工具链名称
    pub fn name(&self) -> &'static str {
        match self {
            Self::Msvc => "MSVC",
            Self::Clang => "Clang",
            Self::Gcc => "GCC",
            Self::Intel => "Intel",
            Self::AppleClang => "AppleClang",
        }
    }

    /// 获取默认工具链 (根据平台)
    pub fn default_for_platform(platform: Platform) -> Self {
        match platform {
            Platform::Windows => Self::Msvc,
            Platform::Linux => Self::Clang, // 优先 Clang
            Platform::MacOS => Self::AppleClang,
        }
    }

    /// 检查是否支持指定平台
    pub fn supports_platform(&self, platform: Platform) -> bool {
        match self {
            Self::Msvc => platform == Platform::Windows,
            Self::Clang => true,                        // 跨平台
            Self::Gcc => platform != Platform::Windows, // Windows 上支持有限
            Self::Intel => true,
            Self::AppleClang => platform == Platform::MacOS,
        }
    }
}

//=============================================================================
// 工具链信息
//=============================================================================

/// 工具链信息
#[derive(Debug, Clone)]
pub struct ToolchainInfo {
    /// 工具链类型
    pub toolchain_type: ToolchainType,
    /// 版本字符串
    pub version: String,
    /// 主版本号
    pub major_version: u32,
    /// 次版本号
    pub minor_version: u32,
    /// 修订版本号
    pub patch_version: u32,
    /// 安装路径
    pub install_path: PathBuf,
    /// 编译器可执行文件
    pub compiler_path: PathBuf,
    /// 链接器可执行文件
    pub linker_path: PathBuf,
    /// 归档工具 (静态库)
    pub archiver_path: PathBuf,
    /// 支持的平台
    pub supported_platforms: Vec<Platform>,
    /// 支持的架构
    pub supported_architectures: Vec<Architecture>,
    /// 预定义宏
    pub predefined_macros: HashMap<String, String>,
    /// 默认包含路径
    pub default_include_paths: Vec<PathBuf>,
    /// 默认库路径
    pub default_lib_paths: Vec<PathBuf>,
}

impl ToolchainInfo {
    /// 解析版本字符串
    pub fn parse_version(version_str: &str) -> (u32, u32, u32) {
        let parts: Vec<&str> = version_str.split('.').collect();
        let major = parts.get(0).and_then(|s| s.parse().ok()).unwrap_or(0);
        let minor = parts.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);
        let patch = parts.get(2).and_then(|s| s.parse().ok()).unwrap_or(0);
        (major, minor, patch)
    }

    /// 检查版本是否满足最低要求
    pub fn meets_minimum_version(&self, major: u32, minor: u32, patch: u32) -> bool {
        if self.major_version > major {
            return true;
        }
        if self.major_version < major {
            return false;
        }
        if self.minor_version > minor {
            return true;
        }
        if self.minor_version < minor {
            return false;
        }
        self.patch_version >= patch
    }

    /// 检查是否支持 C++23
    pub fn supports_cpp23(&self) -> bool {
        match self.toolchain_type {
            ToolchainType::Msvc => self.major_version >= 19 && self.minor_version >= 36,
            ToolchainType::Clang | ToolchainType::AppleClang => self.major_version >= 17,
            ToolchainType::Gcc => self.major_version >= 13,
            ToolchainType::Intel => self.major_version >= 2023,
        }
    }

    /// 检查是否支持模块 (C++20 modules)
    pub fn supports_modules(&self) -> bool {
        match self.toolchain_type {
            ToolchainType::Msvc => self.major_version >= 19 && self.minor_version >= 28,
            ToolchainType::Clang | ToolchainType::AppleClang => self.major_version >= 16,
            ToolchainType::Gcc => self.major_version >= 11,
            ToolchainType::Intel => self.major_version >= 2021,
        }
    }
}

//=============================================================================
// 工具链管理器
//=============================================================================

/// 工具链管理器
pub struct ToolchainManager {
    /// 已注册的工具链
    toolchains: RwLock<HashMap<String, Arc<ToolchainInfo>>>,
    /// 默认工具链 (按平台)
    defaults: RwLock<HashMap<Platform, String>>,
    /// 平台信息缓存
    platform_info: Option<PlatformInfo>,
}

impl ToolchainManager {
    /// 创建新的工具链管理器
    pub fn new() -> Self {
        Self {
            toolchains: RwLock::new(HashMap::new()),
            defaults: RwLock::new(HashMap::new()),
            platform_info: None,
        }
    }

    /// 初始化并检测所有可用工具链
    pub fn initialize(&mut self) -> Result<()> {
        // 检测平台信息
        self.platform_info = Some(PlatformInfo::detect()?);

        // 根据平台检测工具链
        match Platform::host() {
            Platform::Windows => {
                self.detect_msvc()?;
                self.detect_clang()?;
            }
            Platform::Linux => {
                self.detect_gcc()?;
                self.detect_clang()?;
            }
            Platform::MacOS => {
                self.detect_apple_clang()?;
            }
        }

        Ok(())
    }

    /// 检测 MSVC 工具链
    fn detect_msvc(&mut self) -> Result<()> {
        let vs = VisualStudioInfo::detect()?;
        let sdk = WindowsSdkInfo::detect()?;

        let (major, minor, patch) = ToolchainInfo::parse_version(&vs.toolchain_version);

        let mut predefined_macros = HashMap::new();
        predefined_macros.insert("_MSC_VER".to_string(), format!("{}{:02}", major, minor));
        predefined_macros.insert("_WIN32".to_string(), "1".to_string());
        predefined_macros.insert("_WIN64".to_string(), "1".to_string());

        let mut include_paths = vs.include_paths.clone();
        include_paths.extend(sdk.include_paths.clone());

        let mut lib_paths = vs.lib_paths.clone();
        lib_paths.extend(sdk.lib_paths.clone());

        let info = ToolchainInfo {
            toolchain_type: ToolchainType::Msvc,
            version: vs.toolchain_version.clone(),
            major_version: major,
            minor_version: minor,
            patch_version: patch,
            install_path: vs.install_path.clone(),
            compiler_path: vs.cl_path(),
            linker_path: vs.link_path(),
            archiver_path: vs.lib_path(),
            supported_platforms: vec![Platform::Windows],
            supported_architectures: vec![
                Architecture::X64,
                Architecture::X86,
                Architecture::ARM64,
            ],
            predefined_macros,
            default_include_paths: include_paths,
            default_lib_paths: lib_paths,
        };

        let id = format!("msvc-{}", vs.toolchain_version);
        self.register_toolchain(&id, info)?;
        self.set_default(Platform::Windows, &id)?;

        Ok(())
    }

    /// 检测 Clang 工具链
    fn detect_clang(&mut self) -> Result<()> {
        use std::process::Command;

        // 查找 clang
        let clang_paths = if cfg!(windows) {
            vec![
                PathBuf::from(r"C:\Program Files\LLVM\bin\clang.exe"),
                PathBuf::from(r"C:\Program Files (x86)\LLVM\bin\clang.exe"),
            ]
        } else {
            vec![
                PathBuf::from("/usr/bin/clang"),
                PathBuf::from("/usr/local/bin/clang"),
            ]
        };

        let clang_path = clang_paths.into_iter().find(|p| p.exists()).or_else(|| {
            // 尝试从 PATH 查找
            which::which("clang").ok()
        });

        let clang_path = match clang_path {
            Some(p) => p,
            None => return Ok(()), // Clang 未安装
        };

        // 获取版本
        let output = Command::new(&clang_path).arg("--version").output()?;

        if !output.status.success() {
            return Ok(());
        }

        let version_output = String::from_utf8_lossy(&output.stdout);
        let version = Self::parse_clang_version(&version_output);
        let (major, minor, patch) = ToolchainInfo::parse_version(&version);

        let install_dir = clang_path
            .parent()
            .and_then(|p| p.parent())
            .unwrap_or(Path::new("/usr"))
            .to_path_buf();

        let linker_name = if cfg!(windows) {
            "lld-link.exe"
        } else {
            "ld.lld"
        };
        let archiver_name = if cfg!(windows) {
            "llvm-ar.exe"
        } else {
            "llvm-ar"
        };

        let linker_path = install_dir.join("bin").join(linker_name);
        let archiver_path = install_dir.join("bin").join(archiver_name);

        let mut predefined_macros = HashMap::new();
        predefined_macros.insert("__clang__".to_string(), "1".to_string());
        predefined_macros.insert("__clang_major__".to_string(), major.to_string());
        predefined_macros.insert("__clang_minor__".to_string(), minor.to_string());

        let info = ToolchainInfo {
            toolchain_type: ToolchainType::Clang,
            version: version.clone(),
            major_version: major,
            minor_version: minor,
            patch_version: patch,
            install_path: install_dir.clone(),
            compiler_path: clang_path,
            linker_path: if linker_path.exists() {
                linker_path
            } else {
                install_dir.join("bin/ld")
            },
            archiver_path: if archiver_path.exists() {
                archiver_path
            } else {
                install_dir.join("bin/ar")
            },
            supported_platforms: vec![Platform::Windows, Platform::Linux, Platform::MacOS],
            supported_architectures: vec![
                Architecture::X64,
                Architecture::X86,
                Architecture::ARM64,
                Architecture::ARM32,
            ],
            predefined_macros,
            default_include_paths: vec![
                install_dir.join("include"),
                install_dir.join("lib/clang").join(&version).join("include"),
            ],
            default_lib_paths: vec![install_dir.join("lib")],
        };

        let id = format!("clang-{}", version);
        self.register_toolchain(&id, info)?;

        // 在 Linux 上设为默认
        if Platform::host() == Platform::Linux {
            self.set_default(Platform::Linux, &id)?;
        }

        Ok(())
    }

    /// 检测 GCC 工具链
    fn detect_gcc(&mut self) -> Result<()> {
        use std::process::Command;

        let gcc_path = which::which("g++").or_else(|_| which::which("gcc"))?;

        let output = Command::new(&gcc_path).arg("--version").output()?;

        if !output.status.success() {
            return Ok(());
        }

        let version_output = String::from_utf8_lossy(&output.stdout);
        let version = Self::parse_gcc_version(&version_output);
        let (major, minor, patch) = ToolchainInfo::parse_version(&version);

        let install_dir = gcc_path
            .parent()
            .and_then(|p| p.parent())
            .unwrap_or(Path::new("/usr"))
            .to_path_buf();

        let mut predefined_macros = HashMap::new();
        predefined_macros.insert("__GNUC__".to_string(), major.to_string());
        predefined_macros.insert("__GNUC_MINOR__".to_string(), minor.to_string());
        predefined_macros.insert("__GNUC_PATCHLEVEL__".to_string(), patch.to_string());

        let info = ToolchainInfo {
            toolchain_type: ToolchainType::Gcc,
            version: version.clone(),
            major_version: major,
            minor_version: minor,
            patch_version: patch,
            install_path: install_dir.clone(),
            compiler_path: gcc_path,
            linker_path: which::which("ld").unwrap_or_else(|_| PathBuf::from("/usr/bin/ld")),
            archiver_path: which::which("ar").unwrap_or_else(|_| PathBuf::from("/usr/bin/ar")),
            supported_platforms: vec![Platform::Linux, Platform::MacOS],
            supported_architectures: vec![
                Architecture::X64,
                Architecture::X86,
                Architecture::ARM64,
                Architecture::ARM32,
            ],
            predefined_macros,
            default_include_paths: vec![
                PathBuf::from("/usr/include"),
                PathBuf::from("/usr/local/include"),
                PathBuf::from(format!("/usr/include/c++/{}", major)),
            ],
            default_lib_paths: vec![PathBuf::from("/usr/lib"), PathBuf::from("/usr/local/lib")],
        };

        let id = format!("gcc-{}", version);
        self.register_toolchain(&id, info)?;

        Ok(())
    }

    /// 检测 Apple Clang 工具链
    fn detect_apple_clang(&mut self) -> Result<()> {
        use std::process::Command;

        let output = Command::new("xcrun").args(["--find", "clang"]).output()?;

        if !output.status.success() {
            return Err(anyhow!("无法找到 Apple Clang"));
        }

        let clang_path = PathBuf::from(String::from_utf8_lossy(&output.stdout).trim());

        let output = Command::new(&clang_path).arg("--version").output()?;

        let version_output = String::from_utf8_lossy(&output.stdout);
        let version = Self::parse_clang_version(&version_output);
        let (major, minor, patch) = ToolchainInfo::parse_version(&version);

        let mut predefined_macros = HashMap::new();
        predefined_macros.insert("__clang__".to_string(), "1".to_string());
        predefined_macros.insert("__APPLE__".to_string(), "1".to_string());
        predefined_macros.insert("__MACH__".to_string(), "1".to_string());

        let info = ToolchainInfo {
            toolchain_type: ToolchainType::AppleClang,
            version: version.clone(),
            major_version: major,
            minor_version: minor,
            patch_version: patch,
            install_path: PathBuf::from("/Applications/Xcode.app"),
            compiler_path: clang_path.clone(),
            linker_path: clang_path
                .parent()
                .unwrap_or(Path::new("/usr/bin"))
                .join("ld"),
            archiver_path: clang_path
                .parent()
                .unwrap_or(Path::new("/usr/bin"))
                .join("ar"),
            supported_platforms: vec![Platform::MacOS],
            supported_architectures: vec![Architecture::X64, Architecture::ARM64],
            predefined_macros,
            default_include_paths: vec![],
            default_lib_paths: vec![],
        };

        let id = format!("apple-clang-{}", version);
        self.register_toolchain(&id, info)?;
        self.set_default(Platform::MacOS, &id)?;

        Ok(())
    }

    /// 解析 Clang 版本
    fn parse_clang_version(output: &str) -> String {
        // "clang version 17.0.1" or "Apple clang version 15.0.0"
        for line in output.lines() {
            if let Some(pos) = line.find("version ") {
                let version_part = &line[pos + 8..];
                let version: String = version_part
                    .chars()
                    .take_while(|c| c.is_ascii_digit() || *c == '.')
                    .collect();
                if !version.is_empty() {
                    return version;
                }
            }
        }
        "0.0.0".to_string()
    }

    /// 解析 GCC 版本
    fn parse_gcc_version(output: &str) -> String {
        // "g++ (GCC) 13.2.0"
        for line in output.lines() {
            let parts: Vec<&str> = line.split_whitespace().collect();
            for (i, part) in parts.iter().enumerate() {
                if part
                    .chars()
                    .next()
                    .map(|c| c.is_ascii_digit())
                    .unwrap_or(false)
                {
                    let version: String = part
                        .chars()
                        .take_while(|c| c.is_ascii_digit() || *c == '.')
                        .collect();
                    if !version.is_empty() && version.contains('.') {
                        return version;
                    }
                }
            }
        }
        "0.0.0".to_string()
    }

    /// 注册工具链
    pub fn register_toolchain(&self, id: &str, info: ToolchainInfo) -> Result<()> {
        let mut toolchains = self
            .toolchains
            .write()
            .map_err(|_| anyhow!("无法获取工具链写锁"))?;
        toolchains.insert(id.to_string(), Arc::new(info));
        Ok(())
    }

    /// 设置默认工具链
    pub fn set_default(&self, platform: Platform, toolchain_id: &str) -> Result<()> {
        // 验证工具链存在
        let toolchains = self
            .toolchains
            .read()
            .map_err(|_| anyhow!("无法获取工具链读锁"))?;

        if !toolchains.contains_key(toolchain_id) {
            return Err(anyhow!("工具链不存在: {}", toolchain_id));
        }
        drop(toolchains);

        let mut defaults = self
            .defaults
            .write()
            .map_err(|_| anyhow!("无法获取默认工具链写锁"))?;
        defaults.insert(platform, toolchain_id.to_string());
        Ok(())
    }

    /// 获取默认工具链
    pub fn get_default(&self, platform: Platform) -> Option<Arc<ToolchainInfo>> {
        let defaults = self.defaults.read().ok()?;
        let id = defaults.get(&platform)?;

        let toolchains = self.toolchains.read().ok()?;
        toolchains.get(id).cloned()
    }

    /// 获取指定工具链
    pub fn get_toolchain(&self, id: &str) -> Option<Arc<ToolchainInfo>> {
        let toolchains = self.toolchains.read().ok()?;
        toolchains.get(id).cloned()
    }

    /// 获取所有已注册的工具链
    pub fn list_toolchains(&self) -> Vec<(String, Arc<ToolchainInfo>)> {
        let toolchains = match self.toolchains.read() {
            Ok(t) => t,
            Err(_) => return Vec::new(),
        };

        toolchains
            .iter()
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect()
    }

    /// 获取平台信息
    pub fn platform_info(&self) -> Option<&PlatformInfo> {
        self.platform_info.as_ref()
    }

    /// 选择最佳工具链
    pub fn select_best_toolchain(
        &self,
        platform: Platform,
        preferred: Option<ToolchainType>,
    ) -> Option<Arc<ToolchainInfo>> {
        let toolchains = match self.toolchains.read() {
            Ok(t) => t,
            Err(_) => return None,
        };

        // 如果有首选类型，优先查找
        if let Some(preferred_type) = preferred {
            for (_, info) in toolchains.iter() {
                if info.toolchain_type == preferred_type
                    && info.supported_platforms.contains(&platform)
                {
                    return Some(info.clone());
                }
            }
        }

        // 返回默认工具链
        drop(toolchains);
        self.get_default(platform)
    }
}

impl Default for ToolchainManager {
    fn default() -> Self {
        Self::new()
    }
}

//=============================================================================
// 全局工具链管理器
//=============================================================================

lazy_static::lazy_static! {
    /// 全局工具链管理器实例
    static ref TOOLCHAIN_MANAGER: RwLock<ToolchainManager> = RwLock::new(ToolchainManager::new());
}

/// 获取全局工具链管理器
pub fn toolchain_manager() -> &'static RwLock<ToolchainManager> {
    &TOOLCHAIN_MANAGER
}

/// 初始化全局工具链管理器
pub fn initialize_toolchains() -> Result<()> {
    let mut manager = TOOLCHAIN_MANAGER
        .write()
        .map_err(|_| anyhow!("无法获取工具链管理器写锁"))?;
    manager.initialize()
}
