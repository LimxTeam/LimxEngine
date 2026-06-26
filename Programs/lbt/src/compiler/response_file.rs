/*******************************************************************************
 * 文件: compiler/response_file.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   响应文件管理系统 - 处理超长命令行
 *   - 自动检测命令行长度
 *   - 生成响应文件
 *   - 支持不同编译器格式
 *   - 临时文件管理
 *
 * 设计哲学:
 *   1. 透明性 - 自动处理，无需用户干预
 *   2. 兼容性 - 支持所有主流编译器
 *   3. 清洁性 - 自动清理临时文件
 *
 * 技术特性:
 *   - MSVC @file 格式
 *   - GCC/Clang @file 格式
 *   - 自动路径转义
 *   - Unicode 支持
 *
 ******************************************************************************/

use anyhow::{Context, Result};
use std::collections::HashMap;
use std::fs;
use std::io::Write;
use std::path::PathBuf;

//=============================================================================
// 响应文件格式
//=============================================================================

/// 响应文件格式
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResponseFileFormat {
    /// MSVC 格式 (使用双引号和反斜杠转义)
    Msvc,
    /// GCC/Clang 格式 (使用单引号或双引号)
    Gcc,
    /// Ninja 格式
    Ninja,
}

impl ResponseFileFormat {
    /// 获取推荐的文件扩展名
    pub fn extension(&self) -> &'static str {
        match self {
            Self::Msvc => "rsp",
            Self::Gcc => "rsp",
            Self::Ninja => "rsp",
        }
    }

    /// 获取命令行前缀
    pub fn command_prefix(&self) -> &'static str {
        "@"
    }

    /// 转义参数
    pub fn escape_arg(&self, arg: &str) -> String {
        match self {
            Self::Msvc => Self::escape_msvc(arg),
            Self::Gcc => Self::escape_gcc(arg),
            Self::Ninja => Self::escape_ninja(arg),
        }
    }

    /// MSVC 转义规则
    fn escape_msvc(arg: &str) -> String {
        if arg.is_empty() {
            return "\"\"".to_string();
        }

        // 检查是否需要引号
        let needs_quotes = arg.contains(' ') || arg.contains('\t') || arg.contains('"');

        if !needs_quotes {
            return arg.to_string();
        }

        // MSVC 使用双引号包围，内部双引号用反斜杠转义
        let mut result = String::with_capacity(arg.len() + 4);
        result.push('"');

        let mut backslash_count = 0;
        for c in arg.chars() {
            if c == '\\' {
                backslash_count += 1;
            } else if c == '"' {
                // 在引号前，每个反斜杠需要加倍
                for _ in 0..backslash_count {
                    result.push('\\');
                }
                result.push('\\');
                result.push('"');
                backslash_count = 0;
            } else {
                for _ in 0..backslash_count {
                    result.push('\\');
                }
                result.push(c);
                backslash_count = 0;
            }
        }

        // 末尾的反斜杠需要加倍
        for _ in 0..backslash_count {
            result.push('\\');
        }

        result.push('"');
        result
    }

    /// GCC/Clang 转义规则
    fn escape_gcc(arg: &str) -> String {
        if arg.is_empty() {
            return "''".to_string();
        }

        // 检查是否需要引号
        let needs_quotes = arg.chars().any(|c| {
            c.is_whitespace()
                || matches!(
                    c,
                    '"' | '\''
                        | '\\'
                        | '$'
                        | '`'
                        | '!'
                        | '*'
                        | '?'
                        | '['
                        | ']'
                        | '#'
                        | '~'
                        | '&'
                        | '|'
                        | ';'
                        | '<'
                        | '>'
                        | '('
                        | ')'
                        | '{'
                        | '}'
                )
        });

        if !needs_quotes {
            return arg.to_string();
        }

        // 使用单引号包围，内部单引号用 '\'' 转义
        let mut result = String::with_capacity(arg.len() + 4);
        result.push('\'');

        for c in arg.chars() {
            if c == '\'' {
                result.push_str("'\\''");
            } else {
                result.push(c);
            }
        }

        result.push('\'');
        result
    }

    /// Ninja 转义规则
    fn escape_ninja(arg: &str) -> String {
        // Ninja 使用 $ 作为转义字符
        let mut result = String::with_capacity(arg.len());

        for c in arg.chars() {
            match c {
                '$' => result.push_str("$$"),
                ' ' => result.push_str("$ "),
                ':' => result.push_str("$:"),
                '\n' => result.push_str("$\n"),
                _ => result.push(c),
            }
        }

        result
    }
}

//=============================================================================
// 响应文件
//=============================================================================

/// 响应文件
#[derive(Debug, Clone)]
pub struct ResponseFile {
    /// 文件路径
    pub path: PathBuf,
    /// 格式
    pub format: ResponseFileFormat,
    /// 参数列表
    pub arguments: Vec<String>,
    /// 是否为临时文件
    pub is_temporary: bool,
}

impl ResponseFile {
    /// 创建新的响应文件
    pub fn new(path: PathBuf, format: ResponseFileFormat) -> Self {
        Self {
            path,
            format,
            arguments: Vec::new(),
            is_temporary: true,
        }
    }

    /// 创建临时响应文件
    pub fn temporary(format: ResponseFileFormat) -> Result<Self> {
        let temp_dir = std::env::temp_dir();
        let file_name = format!("lbt_rsp_{}.{}", uuid::Uuid::new_v4(), format.extension());
        let path = temp_dir.join(file_name);

        Ok(Self {
            path,
            format,
            arguments: Vec::new(),
            is_temporary: true,
        })
    }

    /// 添加参数
    pub fn add_arg(&mut self, arg: impl Into<String>) -> &mut Self {
        self.arguments.push(arg.into());
        self
    }

    /// 批量添加参数
    pub fn add_args(&mut self, args: impl IntoIterator<Item = impl Into<String>>) -> &mut Self {
        for arg in args {
            self.arguments.push(arg.into());
        }
        self
    }

    /// 生成文件内容
    pub fn content(&self) -> String {
        self.arguments
            .iter()
            .map(|arg| self.format.escape_arg(arg))
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// 写入文件
    pub fn write(&self) -> Result<()> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent)?;
        }

        let content = self.content();

        // 写入文件 (MSVC 需要 UTF-8 BOM)
        let mut file = fs::File::create(&self.path)
            .with_context(|| format!("创建响应文件失败: {}", self.path.display()))?;

        if self.format == ResponseFileFormat::Msvc {
            // 写入 UTF-8 BOM
            file.write_all(&[0xEF, 0xBB, 0xBF])?;
        }

        file.write_all(content.as_bytes())
            .with_context(|| format!("写入响应文件失败: {}", self.path.display()))?;

        Ok(())
    }

    /// 获取命令行参数 (@file)
    pub fn command_arg(&self) -> String {
        format!("{}{}", self.format.command_prefix(), self.path.display())
    }

    /// 删除文件
    pub fn delete(&self) -> Result<()> {
        if self.path.exists() {
            fs::remove_file(&self.path)?;
        }
        Ok(())
    }

    /// 计算原始命令行长度
    pub fn original_length(&self) -> usize {
        self.arguments.iter().map(|a| a.len() + 1).sum()
    }
}

impl Drop for ResponseFile {
    fn drop(&mut self) {
        if self.is_temporary && self.path.exists() {
            let _ = fs::remove_file(&self.path);
        }
    }
}

//=============================================================================
// 响应文件管理器
//=============================================================================

/// 命令行长度限制
#[derive(Debug, Clone, Copy)]
pub struct CommandLineLimits {
    /// Windows 命令行限制 (约 32KB)
    pub windows: usize,
    /// Linux 命令行限制 (约 2MB，但实际受 ARG_MAX 限制)
    pub linux: usize,
    /// macOS 命令行限制
    pub macos: usize,
}

impl Default for CommandLineLimits {
    fn default() -> Self {
        Self {
            windows: 30000, // 留一些余量
            linux: 128000,  // 通常 ARG_MAX 为 2MB，但单个参数限制较小
            macos: 256000,
        }
    }
}

/// 响应文件管理器
pub struct ResponseFileManager {
    /// 命令行长度限制
    limits: CommandLineLimits,
    /// 当前平台
    platform: Platform,
    /// 响应文件输出目录
    output_dir: PathBuf,
    /// 已创建的响应文件
    created_files: Vec<PathBuf>,
    /// 是否总是使用响应文件
    always_use_response_file: bool,
}

/// 平台类型 (简化版，避免循环依赖)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Platform {
    Windows,
    Linux,
    MacOS,
}

impl Platform {
    /// 检测当前平台
    pub fn current() -> Self {
        #[cfg(windows)]
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
        #[cfg(not(any(windows, target_os = "linux", target_os = "macos")))]
        {
            Self::Linux
        }
    }
}

impl ResponseFileManager {
    pub fn new() -> Self {
        Self {
            limits: CommandLineLimits::default(),
            platform: Platform::current(),
            output_dir: PathBuf::from("Intermediate/ResponseFiles"),
            created_files: Vec::new(),
            always_use_response_file: false,
        }
    }

    /// 设置输出目录
    pub fn output_dir(&mut self, dir: PathBuf) -> &mut Self {
        self.output_dir = dir;
        self
    }

    /// 设置总是使用响应文件
    pub fn always_use(&mut self, always: bool) -> &mut Self {
        self.always_use_response_file = always;
        self
    }

    /// 获取当前平台的命令行限制
    pub fn command_line_limit(&self) -> usize {
        match self.platform {
            Platform::Windows => self.limits.windows,
            Platform::Linux => self.limits.linux,
            Platform::MacOS => self.limits.macos,
        }
    }

    /// 检查是否需要响应文件
    pub fn needs_response_file(&self, args: &[String]) -> bool {
        if self.always_use_response_file {
            return true;
        }

        let total_length: usize = args.iter().map(|a| a.len() + 1).sum();
        total_length > self.command_line_limit()
    }

    /// 获取推荐的响应文件格式
    pub fn recommended_format(&self) -> ResponseFileFormat {
        match self.platform {
            Platform::Windows => ResponseFileFormat::Msvc,
            _ => ResponseFileFormat::Gcc,
        }
    }

    /// 创建响应文件
    pub fn create(&mut self, name: &str, args: &[String]) -> Result<ResponseFile> {
        let format = self.recommended_format();
        let path = self
            .output_dir
            .join(format!("{}.{}", name, format.extension()));

        let mut rsp = ResponseFile::new(path.clone(), format);
        rsp.add_args(args.iter().cloned());
        rsp.is_temporary = false;
        rsp.write()?;

        self.created_files.push(path);
        Ok(rsp)
    }

    /// 创建临时响应文件
    pub fn create_temporary(&mut self, args: &[String]) -> Result<ResponseFile> {
        let format = self.recommended_format();
        let mut rsp = ResponseFile::temporary(format)?;
        rsp.add_args(args.iter().cloned());
        rsp.write()?;

        self.created_files.push(rsp.path.clone());
        Ok(rsp)
    }

    /// 根据需要创建响应文件或返回原始参数
    pub fn maybe_create_response_file(
        &mut self,
        name: &str,
        args: Vec<String>,
    ) -> Result<Vec<String>> {
        if self.needs_response_file(&args) {
            let rsp = self.create(name, &args)?;
            Ok(vec![rsp.command_arg()])
        } else {
            Ok(args)
        }
    }

    /// 清理所有创建的响应文件
    pub fn cleanup(&mut self) -> Result<usize> {
        let mut count = 0;
        for path in self.created_files.drain(..) {
            if path.exists() {
                fs::remove_file(&path)?;
                count += 1;
            }
        }
        Ok(count)
    }

    /// 清理输出目录中的所有响应文件
    pub fn cleanup_all(&self) -> Result<usize> {
        if !self.output_dir.exists() {
            return Ok(0);
        }

        let mut count = 0;
        for entry in fs::read_dir(&self.output_dir)? {
            let entry = entry?;
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) == Some("rsp") {
                fs::remove_file(&path)?;
                count += 1;
            }
        }
        Ok(count)
    }
}

impl Default for ResponseFileManager {
    fn default() -> Self {
        Self::new()
    }
}

//=============================================================================
// 命令行构建器
//=============================================================================

/// 命令行构建器 - 自动处理响应文件
pub struct CommandLineBuilder {
    /// 可执行文件
    executable: PathBuf,
    /// 参数列表
    arguments: Vec<String>,
    /// 响应文件管理器
    rsp_manager: ResponseFileManager,
    /// 环境变量
    environment: HashMap<String, String>,
    /// 工作目录
    working_dir: Option<PathBuf>,
}

impl CommandLineBuilder {
    pub fn new(executable: impl Into<PathBuf>) -> Self {
        Self {
            executable: executable.into(),
            arguments: Vec::new(),
            rsp_manager: ResponseFileManager::new(),
            environment: HashMap::new(),
            working_dir: None,
        }
    }

    /// 添加参数
    pub fn arg(&mut self, arg: impl Into<String>) -> &mut Self {
        self.arguments.push(arg.into());
        self
    }

    /// 批量添加参数
    pub fn args(&mut self, args: impl IntoIterator<Item = impl Into<String>>) -> &mut Self {
        for arg in args {
            self.arguments.push(arg.into());
        }
        self
    }

    /// 添加环境变量
    pub fn env(&mut self, key: impl Into<String>, value: impl Into<String>) -> &mut Self {
        self.environment.insert(key.into(), value.into());
        self
    }

    /// 设置工作目录
    pub fn working_dir(&mut self, dir: impl Into<PathBuf>) -> &mut Self {
        self.working_dir = Some(dir.into());
        self
    }

    /// 设置响应文件输出目录
    pub fn response_file_dir(&mut self, dir: impl Into<PathBuf>) -> &mut Self {
        self.rsp_manager.output_dir(dir.into());
        self
    }

    /// 构建命令
    pub fn build(&mut self, name: &str) -> Result<Command> {
        let final_args = self
            .rsp_manager
            .maybe_create_response_file(name, self.arguments.clone())?;

        Ok(Command {
            executable: self.executable.clone(),
            arguments: final_args,
            environment: self.environment.clone(),
            working_dir: self.working_dir.clone(),
        })
    }

    /// 获取完整命令行字符串 (用于显示)
    pub fn command_line_string(&self) -> String {
        let mut parts = vec![self.executable.display().to_string()];
        parts.extend(self.arguments.iter().cloned());
        parts.join(" ")
    }

    /// 获取命令行长度
    pub fn command_line_length(&self) -> usize {
        self.executable.display().to_string().len()
            + 1
            + self.arguments.iter().map(|a| a.len() + 1).sum::<usize>()
    }
}

/// 构建的命令
#[derive(Debug, Clone)]
pub struct Command {
    pub executable: PathBuf,
    pub arguments: Vec<String>,
    pub environment: HashMap<String, String>,
    pub working_dir: Option<PathBuf>,
}

impl Command {
    /// 转换为 std::process::Command
    pub fn to_std_command(&self) -> std::process::Command {
        let mut cmd = std::process::Command::new(&self.executable);
        cmd.args(&self.arguments);

        for (key, value) in &self.environment {
            cmd.env(key, value);
        }

        if let Some(ref dir) = self.working_dir {
            cmd.current_dir(dir);
        }

        cmd
    }

    /// 执行命令
    pub fn execute(&self) -> Result<std::process::Output> {
        self.to_std_command()
            .output()
            .with_context(|| format!("执行命令失败: {}", self.executable.display()))
    }

    /// 获取命令行字符串
    pub fn to_string(&self) -> String {
        let mut parts = vec![self.executable.display().to_string()];
        parts.extend(self.arguments.iter().cloned());
        parts.join(" ")
    }
}

//=============================================================================
// 测试
//=============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_msvc_escape() {
        assert_eq!(ResponseFileFormat::escape_msvc("simple"), "simple");
        assert_eq!(
            ResponseFileFormat::escape_msvc("with space"),
            "\"with space\""
        );
        assert_eq!(
            ResponseFileFormat::escape_msvc("with\"quote"),
            "\"with\\\"quote\""
        );
        assert_eq!(
            ResponseFileFormat::escape_msvc("path\\to\\file"),
            "path\\to\\file"
        );
        assert_eq!(
            ResponseFileFormat::escape_msvc("path with\\space"),
            "\"path with\\space\""
        );
    }

    #[test]
    fn test_gcc_escape() {
        assert_eq!(ResponseFileFormat::escape_gcc("simple"), "simple");
        assert_eq!(ResponseFileFormat::escape_gcc("with space"), "'with space'");
        assert_eq!(
            ResponseFileFormat::escape_gcc("with'quote"),
            "'with'\\''quote'"
        );
    }

    #[test]
    fn test_response_file_content() {
        let mut rsp = ResponseFile::new(PathBuf::from("test.rsp"), ResponseFileFormat::Gcc);
        rsp.add_arg("-c");
        rsp.add_arg("file with space.cpp");
        rsp.add_arg("-o");
        rsp.add_arg("output.o");

        let content = rsp.content();
        assert!(content.contains("-c"));
        assert!(content.contains("'file with space.cpp'"));
    }

    #[test]
    fn test_needs_response_file() {
        let manager = ResponseFileManager::new();

        // 短参数列表
        let short_args: Vec<String> = vec!["-c".to_string(), "file.cpp".to_string()];
        assert!(!manager.needs_response_file(&short_args));

        // 长参数列表
        let long_args: Vec<String> = (0..5000)
            .map(|i| format!("-I/very/long/path/number/{}", i))
            .collect();
        assert!(manager.needs_response_file(&long_args));
    }
}
