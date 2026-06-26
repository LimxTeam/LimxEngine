/*******************************************************************************
 * 文件: compiler/platform.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   平台抽象层 - 统一不同操作系统的构建差异
 *   - 平台检测与识别
 *   - SDK 路径查找
 *   - 平台特定配置
 *   - 环境变量管理
 *
 * 技术特性:
 *   - 自动 SDK 版本检测
 *   - Windows SDK / UCRT 支持
 *   - macOS SDK / Xcode 支持
 *   - Linux sysroot 支持
 *
 ******************************************************************************/

use anyhow::{anyhow, Context, Result};
use std::collections::HashMap;
use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

//=============================================================================
// 平台枚举
//=============================================================================

/// 目标平台
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Platform {
    Windows,
    Linux,
    MacOS,
}

impl Platform {
    /// 获取平台名称
    pub fn name(&self) -> &'static str {
        match self {
            Self::Windows => "Win64",
            Self::Linux => "Linux",
            Self::MacOS => "Mac",
        }
    }

    /// 获取平台短名称
    pub fn short_name(&self) -> &'static str {
        match self {
            Self::Windows => "win",
            Self::Linux => "linux",
            Self::MacOS => "mac",
        }
    }

    /// 检测当前主机平台
    pub fn host() -> Self {
        #[cfg(target_os = "windows")]
        {
            Self::Windows
        }
        #[cfg(target_os = "linux")]
        {
            Self::Linux
        }
        #[cfg(target_os = "macos")]
        {
            Self::MacOS
        }
    }

    /// 是否为类 Unix 系统
    pub fn is_unix(&self) -> bool {
        matches!(self, Self::Linux | Self::MacOS)
    }

    /// 获取可执行文件扩展名
    pub fn exe_extension(&self) -> &'static str {
        match self {
            Self::Windows => ".exe",
            _ => "",
        }
    }

    /// 获取动态库扩展名
    pub fn dll_extension(&self) -> &'static str {
        match self {
            Self::Windows => ".dll",
            Self::Linux => ".so",
            Self::MacOS => ".dylib",
        }
    }

    /// 获取静态库扩展名
    pub fn lib_extension(&self) -> &'static str {
        match self {
            Self::Windows => ".lib",
            _ => ".a",
        }
    }

    /// 获取对象文件扩展名
    pub fn obj_extension(&self) -> &'static str {
        match self {
            Self::Windows => ".obj",
            _ => ".o",
        }
    }

    /// 获取动态库前缀
    pub fn dll_prefix(&self) -> &'static str {
        match self {
            Self::Windows => "",
            _ => "lib",
        }
    }

    /// 获取静态库前缀
    pub fn lib_prefix(&self) -> &'static str {
        match self {
            Self::Windows => "",
            _ => "lib",
        }
    }

    /// 获取路径分隔符
    pub fn path_separator(&self) -> char {
        match self {
            Self::Windows => ';',
            _ => ':',
        }
    }
}

impl Default for Platform {
    fn default() -> Self {
        Self::host()
    }
}

//=============================================================================
// 平台 SDK 信息
//=============================================================================

/// Windows SDK 信息
#[derive(Debug, Clone)]
pub struct WindowsSdkInfo {
    /// SDK 版本 (如 "10.0.22621.0")
    pub version: String,
    /// SDK 根目录
    pub root_path: PathBuf,
    /// Include 目录
    pub include_paths: Vec<PathBuf>,
    /// Lib 目录
    pub lib_paths: Vec<PathBuf>,
    /// Bin 目录
    pub bin_path: PathBuf,
    /// UCRT 版本
    pub ucrt_version: Option<String>,
}

impl WindowsSdkInfo {
    /// 检测 Windows SDK
    pub fn detect() -> Result<Self> {
        // 尝试从注册表读取
        let sdk_root = Self::find_sdk_root()?;
        let version = Self::find_latest_version(&sdk_root)?;

        let include_base = sdk_root.join("Include").join(&version);
        let lib_base = sdk_root.join("Lib").join(&version);

        let include_paths = vec![
            include_base.join("ucrt"),
            include_base.join("um"),
            include_base.join("shared"),
            include_base.join("winrt"),
            include_base.join("cppwinrt"),
        ];

        let lib_paths = vec![
            lib_base.join("ucrt").join("x64"),
            lib_base.join("um").join("x64"),
        ];

        let bin_path = sdk_root.join("bin").join(&version).join("x64");

        Ok(Self {
            version: version.clone(),
            root_path: sdk_root,
            include_paths,
            lib_paths,
            bin_path,
            ucrt_version: Some(version),
        })
    }

    fn find_sdk_root() -> Result<PathBuf> {
        // 尝试环境变量
        if let Ok(path) = env::var("WindowsSdkDir") {
            let path = PathBuf::from(path);
            if path.exists() {
                return Ok(path);
            }
        }

        // 默认路径
        let default_paths = [
            r"C:\Program Files (x86)\Windows Kits\10",
            r"C:\Program Files\Windows Kits\10",
        ];

        for path in &default_paths {
            let path = PathBuf::from(path);
            if path.exists() {
                return Ok(path);
            }
        }

        Err(anyhow!("无法找到 Windows SDK"))
    }

    fn find_latest_version(sdk_root: &Path) -> Result<String> {
        // 从环境变量获取
        if let Ok(version) = env::var("WindowsSDKVersion") {
            let version = version.trim_end_matches('\\').to_string();
            return Ok(version);
        }

        // 扫描 Include 目录找最新版本
        let include_dir = sdk_root.join("Include");
        if !include_dir.exists() {
            return Err(anyhow!("Windows SDK Include 目录不存在"));
        }

        let mut versions: Vec<String> = std::fs::read_dir(&include_dir)?
            .filter_map(|e| e.ok())
            .filter(|e| e.path().is_dir())
            .filter_map(|e| e.file_name().into_string().ok())
            .filter(|name| name.starts_with("10."))
            .collect();

        versions.sort_by(|a, b| {
            let parse_version =
                |s: &str| -> Vec<u32> { s.split('.').filter_map(|p| p.parse().ok()).collect() };
            let va = parse_version(a);
            let vb = parse_version(b);
            vb.cmp(&va)
        });

        versions
            .into_iter()
            .next()
            .ok_or_else(|| anyhow!("无法找到 Windows SDK 版本"))
    }
}

/// Visual Studio 信息
#[derive(Debug, Clone)]
pub struct VisualStudioInfo {
    /// VS 版本 (如 "17.8.3")
    pub version: String,
    /// VS 安装路径
    pub install_path: PathBuf,
    /// MSVC 工具链版本
    pub toolchain_version: String,
    /// VC 工具目录
    pub vc_tools_path: PathBuf,
    /// Include 目录
    pub include_paths: Vec<PathBuf>,
    /// Lib 目录
    pub lib_paths: Vec<PathBuf>,
    /// Bin 目录
    pub bin_path: PathBuf,
}

impl VisualStudioInfo {
    /// 检测 Visual Studio
    pub fn detect() -> Result<Self> {
        let install_path = Self::find_vs_install_path()?;
        let toolchain_version = Self::find_toolchain_version(&install_path)?;

        let vc_tools_path = install_path
            .join("VC")
            .join("Tools")
            .join("MSVC")
            .join(&toolchain_version);

        let include_paths = vec![
            vc_tools_path.join("include"),
            vc_tools_path.join("atlmfc").join("include"),
        ];

        let lib_paths = vec![
            vc_tools_path.join("lib").join("x64"),
            vc_tools_path.join("atlmfc").join("lib").join("x64"),
        ];

        let bin_path = vc_tools_path.join("bin").join("Hostx64").join("x64");

        // 获取 VS 版本
        let version = Self::get_vs_version(&install_path)?;

        Ok(Self {
            version,
            install_path,
            toolchain_version,
            vc_tools_path,
            include_paths,
            lib_paths,
            bin_path,
        })
    }

    fn find_vs_install_path() -> Result<PathBuf> {
        // 使用 vswhere 查找
        let vswhere_paths = [
            r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe",
            r"C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe",
        ];

        for vswhere in &vswhere_paths {
            if Path::new(vswhere).exists() {
                let output = Command::new(vswhere)
                    .args([
                        "-latest",
                        "-requires",
                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-property",
                        "installationPath",
                    ])
                    .output()
                    .context("执行 vswhere 失败")?;

                if output.status.success() {
                    let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
                    if !path.is_empty() {
                        return Ok(PathBuf::from(path));
                    }
                }
            }
        }

        // 尝试环境变量 (从新到旧)
        for env_key in ["VS2026INSTALLDIR", "VS2022INSTALLDIR"] {
            if let Ok(path) = env::var(env_key) {
                let path = PathBuf::from(path);
                if path.exists() {
                    return Ok(path);
                }
            }
        }

        // 默认路径 (从新版本到旧版本)
        // VS2026 使用内部版本号 "18"，预览版使用 "Insiders"/"Preview" 子目录
        let default_paths = [
            r"C:\Program Files\Microsoft Visual Studio\18\Insiders",
            r"C:\Program Files\Microsoft Visual Studio\18\Preview",
            r"C:\Program Files\Microsoft Visual Studio\18\Enterprise",
            r"C:\Program Files\Microsoft Visual Studio\18\Professional",
            r"C:\Program Files\Microsoft Visual Studio\18\Community",
            r"C:\Program Files\Microsoft Visual Studio\2026\Enterprise",
            r"C:\Program Files\Microsoft Visual Studio\2026\Professional",
            r"C:\Program Files\Microsoft Visual Studio\2026\Community",
            r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
            r"C:\Program Files\Microsoft Visual Studio\2022\Professional",
            r"C:\Program Files\Microsoft Visual Studio\2022\Community",
            r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise",
            r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional",
            r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community",
        ];

        for path in &default_paths {
            let path = PathBuf::from(path);
            if path.exists() {
                return Ok(path);
            }
        }

        Err(anyhow!("无法找到 Visual Studio 安装"))
    }

    fn find_toolchain_version(install_path: &Path) -> Result<String> {
        let vc_tools_dir = install_path.join("VC").join("Tools").join("MSVC");

        if !vc_tools_dir.exists() {
            return Err(anyhow!("VC Tools 目录不存在: {:?}", vc_tools_dir));
        }

        let mut versions: Vec<String> = std::fs::read_dir(&vc_tools_dir)?
            .filter_map(|e| e.ok())
            .filter(|e| e.path().is_dir())
            .filter_map(|e| e.file_name().into_string().ok())
            .filter(|name| {
                name.chars()
                    .next()
                    .map(|c| c.is_ascii_digit())
                    .unwrap_or(false)
            })
            .collect();

        versions.sort_by(|a, b| b.cmp(a));

        versions
            .into_iter()
            .next()
            .ok_or_else(|| anyhow!("无法找到 MSVC 工具链版本"))
    }

    fn get_vs_version(install_path: &Path) -> Result<String> {
        // 尝试从 catalog.json 读取版本
        let catalog_path = install_path
            .join("Common7")
            .join("IDE")
            .join(".catalog.json");

        if catalog_path.exists() {
            if let Ok(content) = std::fs::read_to_string(&catalog_path) {
                // 简单解析 JSON
                if let Some(start) = content.find("\"productDisplayVersion\"") {
                    if let Some(colon) = content[start..].find(':') {
                        let rest = &content[start + colon + 1..];
                        if let Some(quote1) = rest.find('"') {
                            if let Some(quote2) = rest[quote1 + 1..].find('"') {
                                return Ok(rest[quote1 + 1..quote1 + 1 + quote2].to_string());
                            }
                        }
                    }
                }
            }
        }

        // 从路径推断
        let path_str = install_path.to_string_lossy();
        if path_str.contains("2026") || path_str.contains(r"\18\") {
            Ok("18.0".to_string())
        } else if path_str.contains("2022") {
            Ok("17.0".to_string())
        } else if path_str.contains("2019") {
            Ok("16.0".to_string())
        } else {
            Ok("Unknown".to_string())
        }
    }

    /// 获取编译器路径
    pub fn cl_path(&self) -> PathBuf {
        self.bin_path.join("cl.exe")
    }

    /// 获取链接器路径
    pub fn link_path(&self) -> PathBuf {
        self.bin_path.join("link.exe")
    }

    /// 获取库管理器路径
    pub fn lib_path(&self) -> PathBuf {
        self.bin_path.join("lib.exe")
    }
}

/// macOS SDK 信息
#[derive(Debug, Clone)]
pub struct MacOsSdkInfo {
    /// SDK 版本
    pub version: String,
    /// SDK 路径
    pub sdk_path: PathBuf,
    /// 最低部署目标
    pub deployment_target: String,
}

impl MacOsSdkInfo {
    /// 检测 macOS SDK
    pub fn detect() -> Result<Self> {
        // 使用 xcrun 查找 SDK
        let output = Command::new("xcrun")
            .args(["--show-sdk-path"])
            .output()
            .context("执行 xcrun 失败")?;

        if !output.status.success() {
            return Err(anyhow!("xcrun 执行失败"));
        }

        let sdk_path = PathBuf::from(String::from_utf8_lossy(&output.stdout).trim());

        // 获取版本
        let output = Command::new("xcrun")
            .args(["--show-sdk-version"])
            .output()
            .context("获取 SDK 版本失败")?;

        let version = String::from_utf8_lossy(&output.stdout).trim().to_string();

        // 默认部署目标
        let deployment_target =
            env::var("MACOSX_DEPLOYMENT_TARGET").unwrap_or_else(|_| "10.15".to_string());

        Ok(Self {
            version,
            sdk_path,
            deployment_target,
        })
    }
}

/// Linux 系统信息
#[derive(Debug, Clone)]
pub struct LinuxSystemInfo {
    /// 发行版名称
    pub distro: String,
    /// 系统根目录
    pub sysroot: PathBuf,
    /// GCC 版本
    pub gcc_version: Option<String>,
    /// Clang 版本
    pub clang_version: Option<String>,
}

impl LinuxSystemInfo {
    /// 检测 Linux 系统信息
    pub fn detect() -> Result<Self> {
        let distro = Self::detect_distro()?;
        let gcc_version = Self::detect_gcc_version();
        let clang_version = Self::detect_clang_version();

        Ok(Self {
            distro,
            sysroot: PathBuf::from("/"),
            gcc_version,
            clang_version,
        })
    }

    fn detect_distro() -> Result<String> {
        // 读取 /etc/os-release
        if let Ok(content) = std::fs::read_to_string("/etc/os-release") {
            for line in content.lines() {
                if line.starts_with("PRETTY_NAME=") {
                    let name = line.trim_start_matches("PRETTY_NAME=").trim_matches('"');
                    return Ok(name.to_string());
                }
            }
        }

        Ok("Unknown Linux".to_string())
    }

    fn detect_gcc_version() -> Option<String> {
        Command::new("gcc")
            .arg("--version")
            .output()
            .ok()
            .and_then(|o| {
                if o.status.success() {
                    let output = String::from_utf8_lossy(&o.stdout);
                    output.lines().next().map(|s| s.to_string())
                } else {
                    None
                }
            })
    }

    fn detect_clang_version() -> Option<String> {
        Command::new("clang")
            .arg("--version")
            .output()
            .ok()
            .and_then(|o| {
                if o.status.success() {
                    let output = String::from_utf8_lossy(&o.stdout);
                    output.lines().next().map(|s| s.to_string())
                } else {
                    None
                }
            })
    }
}

//=============================================================================
// 平台信息聚合
//=============================================================================

/// 平台信息
#[derive(Debug, Clone)]
pub enum PlatformInfo {
    Windows {
        vs: VisualStudioInfo,
        sdk: WindowsSdkInfo,
    },
    Linux(LinuxSystemInfo),
    MacOS(MacOsSdkInfo),
}

impl PlatformInfo {
    /// 检测当前平台信息
    pub fn detect() -> Result<Self> {
        match Platform::host() {
            Platform::Windows => {
                let vs = VisualStudioInfo::detect()?;
                let sdk = WindowsSdkInfo::detect()?;
                Ok(Self::Windows { vs, sdk })
            }
            Platform::Linux => {
                let info = LinuxSystemInfo::detect()?;
                Ok(Self::Linux(info))
            }
            Platform::MacOS => {
                let info = MacOsSdkInfo::detect()?;
                Ok(Self::MacOS(info))
            }
        }
    }

    /// 获取平台类型
    pub fn platform(&self) -> Platform {
        match self {
            Self::Windows { .. } => Platform::Windows,
            Self::Linux(_) => Platform::Linux,
            Self::MacOS(_) => Platform::MacOS,
        }
    }

    /// 获取所有包含路径
    pub fn include_paths(&self) -> Vec<PathBuf> {
        match self {
            Self::Windows { vs, sdk } => {
                let mut paths = vs.include_paths.clone();
                paths.extend(sdk.include_paths.clone());
                paths
            }
            Self::Linux(_) => {
                vec![
                    PathBuf::from("/usr/include"),
                    PathBuf::from("/usr/local/include"),
                ]
            }
            Self::MacOS(info) => {
                vec![info.sdk_path.join("usr/include")]
            }
        }
    }

    /// 获取所有库路径
    pub fn lib_paths(&self) -> Vec<PathBuf> {
        match self {
            Self::Windows { vs, sdk } => {
                let mut paths = vs.lib_paths.clone();
                paths.extend(sdk.lib_paths.clone());
                paths
            }
            Self::Linux(_) => {
                vec![
                    PathBuf::from("/usr/lib"),
                    PathBuf::from("/usr/lib/x86_64-linux-gnu"),
                    PathBuf::from("/usr/local/lib"),
                ]
            }
            Self::MacOS(info) => {
                vec![info.sdk_path.join("usr/lib")]
            }
        }
    }

    /// 构建编译器环境变量
    pub fn build_environment(&self) -> HashMap<String, String> {
        let mut env = HashMap::new();

        match self {
            Self::Windows { vs, sdk } => {
                // PATH
                let path_dirs = vec![
                    vs.bin_path.to_string_lossy().to_string(),
                    sdk.bin_path.to_string_lossy().to_string(),
                ];

                if let Ok(existing_path) = std::env::var("PATH") {
                    env.insert(
                        "PATH".to_string(),
                        format!("{};{}", path_dirs.join(";"), existing_path),
                    );
                } else {
                    env.insert("PATH".to_string(), path_dirs.join(";"));
                }

                // INCLUDE
                let includes: Vec<String> = vs
                    .include_paths
                    .iter()
                    .chain(sdk.include_paths.iter())
                    .map(|p| p.to_string_lossy().to_string())
                    .collect();
                env.insert("INCLUDE".to_string(), includes.join(";"));

                // LIB
                let libs: Vec<String> = vs
                    .lib_paths
                    .iter()
                    .chain(sdk.lib_paths.iter())
                    .map(|p| p.to_string_lossy().to_string())
                    .collect();
                env.insert("LIB".to_string(), libs.join(";"));

                // LIBPATH
                env.insert("LIBPATH".to_string(), libs.join(";"));
            }
            Self::Linux(_) => {
                // Linux 通常不需要特殊环境变量
            }
            Self::MacOS(info) => {
                env.insert(
                    "SDKROOT".to_string(),
                    info.sdk_path.to_string_lossy().to_string(),
                );
                env.insert(
                    "MACOSX_DEPLOYMENT_TARGET".to_string(),
                    info.deployment_target.clone(),
                );
            }
        }

        env
    }
}

//=============================================================================
// 单元测试
//=============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_platform_host() {
        let platform = Platform::host();
        #[cfg(target_os = "windows")]
        assert_eq!(platform, Platform::Windows);
        #[cfg(target_os = "linux")]
        assert_eq!(platform, Platform::Linux);
        #[cfg(target_os = "macos")]
        assert_eq!(platform, Platform::MacOS);
    }

    #[test]
    fn test_platform_extensions() {
        let win = Platform::Windows;
        assert_eq!(win.exe_extension(), ".exe");
        assert_eq!(win.dll_extension(), ".dll");
        assert_eq!(win.lib_extension(), ".lib");
        assert_eq!(win.obj_extension(), ".obj");

        let linux = Platform::Linux;
        assert_eq!(linux.exe_extension(), "");
        assert_eq!(linux.dll_extension(), ".so");
        assert_eq!(linux.lib_extension(), ".a");
        assert_eq!(linux.obj_extension(), ".o");
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn test_windows_sdk_detection() {
        // 仅在 Windows 上运行
        let result = WindowsSdkInfo::detect();
        assert!(result.is_ok(), "Windows SDK 检测失败: {:?}", result.err());

        let sdk = result.unwrap();
        assert!(!sdk.version.is_empty());
        assert!(sdk.root_path.exists());
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn test_visual_studio_detection() {
        let result = VisualStudioInfo::detect();
        // 若当前环境未安装 VS 则跳过，不应裸 panic
        let vs = match result {
            Ok(vs) => vs,
            Err(e) => {
                eprintln!("跳过测试: 未检测到 Visual Studio — {}", e);
                return;
            }
        };
        assert!(!vs.version.is_empty());
        assert!(vs.install_path.exists());
        assert!(vs.cl_path().exists());
    }
}
