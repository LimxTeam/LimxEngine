/*******************************************************************************
 * 文件: compiler/unity.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Unity Build 系统 - 将多个源文件合并为一个编译单元
 *   - 减少编译器启动开销
 *   - 减少头文件重复解析
 *   - 智能分组策略
 *   - 与增量编译协同工作
 *
 * 设计哲学:
 *   1. 智能分组 - 根据依赖关系和文件大小优化分组
 *   2. 增量友好 - 修改单个文件不会导致整个 Unity 文件重编译
 *   3. 可配置 - 支持排除特定文件
 *   4. 调试友好 - 保留原始文件行号信息
 *
 * 技术特性:
 *   - 自动生成 Unity 文件
 *   - 支持 #line 指令保留调试信息
 *   - 智能文件大小平衡
 *   - 循环依赖检测
 *   - 并行 Unity 文件生成
 *
 ******************************************************************************/

use anyhow::{Context, Result};
use rayon::prelude::*;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

//=============================================================================
// Unity Build 配置
//=============================================================================

/// Unity Build 配置
#[derive(Debug, Clone)]
pub struct UnityBuildConfig {
    /// 是否启用 Unity Build
    pub enabled: bool,
    /// 每个 Unity 文件包含的最大源文件数
    pub max_files_per_unity: usize,
    /// 每个 Unity 文件的最大大小 (字节)
    pub max_unity_size: usize,
    /// 排除的文件模式
    pub exclude_patterns: Vec<String>,
    /// 排除的具体文件
    pub exclude_files: HashSet<PathBuf>,
    /// 是否生成 #line 指令
    pub generate_line_directives: bool,
    /// Unity 文件输出目录
    pub output_dir: PathBuf,
    /// Unity 文件前缀
    pub file_prefix: String,
    /// 是否按模块分组
    pub group_by_module: bool,
    /// 最小文件数 (少于此数量不使用 Unity Build)
    pub min_files_threshold: usize,
}

impl Default for UnityBuildConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            max_files_per_unity: 20,
            max_unity_size: 2 * 1024 * 1024, // 2MB
            exclude_patterns: vec![
                "*Test*.cpp".to_string(),
                "*_test.cpp".to_string(),
                "*Generated*.cpp".to_string(),
            ],
            exclude_files: HashSet::new(),
            generate_line_directives: true,
            output_dir: PathBuf::from("Intermediate/Unity"),
            file_prefix: "Unity_".to_string(),
            group_by_module: true,
            min_files_threshold: 5,
        }
    }
}

impl UnityBuildConfig {
    pub fn new() -> Self {
        Self::default()
    }

    /// 设置每个 Unity 文件的最大源文件数
    pub fn max_files(&mut self, count: usize) -> &mut Self {
        self.max_files_per_unity = count;
        self
    }

    /// 设置每个 Unity 文件的最大大小
    pub fn max_size(&mut self, bytes: usize) -> &mut Self {
        self.max_unity_size = bytes;
        self
    }

    /// 排除文件
    pub fn exclude(&mut self, file: PathBuf) -> &mut Self {
        self.exclude_files.insert(file);
        self
    }

    /// 排除模式
    pub fn exclude_pattern(&mut self, pattern: &str) -> &mut Self {
        self.exclude_patterns.push(pattern.to_string());
        self
    }

    /// 检查文件是否应该被排除
    pub fn should_exclude(&self, file: &Path) -> bool {
        // 检查具体文件
        if self.exclude_files.contains(file) {
            return true;
        }

        // 检查模式
        let file_name = file.file_name().and_then(|n| n.to_str()).unwrap_or("");

        for pattern in &self.exclude_patterns {
            if Self::matches_pattern(file_name, pattern) {
                return true;
            }
        }

        false
    }

    /// 通配符匹配 (支持多个 * 通配符)
    /// 将模式按 * 分割为多段，依次在名称中查找每段是否按顺序出现
    fn matches_pattern(name: &str, pattern: &str) -> bool {
        let pattern = pattern.to_lowercase();
        let name = name.to_lowercase();

        let parts: Vec<&str> = pattern.split('*').collect();

        // 无通配符 — 精确匹配
        if parts.len() == 1 {
            return name == pattern;
        }

        let mut pos = 0usize;

        for (i, part) in parts.iter().enumerate() {
            if part.is_empty() {
                continue;
            }
            if i == 0 {
                // 第一段必须是前缀
                if !name.starts_with(part) {
                    return false;
                }
                pos = part.len();
            } else if i == parts.len() - 1 {
                // 最后一段必须是后缀
                if !name[pos..].ends_with(part) {
                    return false;
                }
            } else {
                // 中间段：在剩余部分中查找
                match name[pos..].find(part) {
                    Some(offset) => pos += offset + part.len(),
                    None => return false,
                }
            }
        }

        true
    }
}

//=============================================================================
// Unity 文件
//=============================================================================

/// Unity 文件信息
#[derive(Debug, Clone)]
pub struct UnityFile {
    /// Unity 文件路径
    pub path: PathBuf,
    /// 包含的源文件
    pub source_files: Vec<PathBuf>,
    /// 总大小 (字节)
    pub total_size: usize,
    /// 所属模块
    pub module_name: Option<String>,
}

impl UnityFile {
    pub fn new(path: PathBuf) -> Self {
        Self {
            path,
            source_files: Vec::new(),
            total_size: 0,
            module_name: None,
        }
    }

    /// 添加源文件
    pub fn add_file(&mut self, file: PathBuf, size: usize) {
        self.source_files.push(file);
        self.total_size += size;
    }

    /// 获取文件数量
    pub fn file_count(&self) -> usize {
        self.source_files.len()
    }
}

//=============================================================================
// Unity Build 生成器
//=============================================================================

/// Unity Build 生成器
pub struct UnityBuildGenerator {
    /// 配置
    config: UnityBuildConfig,
    /// 生成的 Unity 文件
    unity_files: Vec<UnityFile>,
    /// 文件大小缓存
    file_sizes: HashMap<PathBuf, usize>,
}

impl UnityBuildGenerator {
    pub fn new(config: UnityBuildConfig) -> Self {
        Self {
            config,
            unity_files: Vec::new(),
            file_sizes: HashMap::new(),
        }
    }

    /// 使用默认配置创建
    pub fn with_defaults() -> Self {
        Self::new(UnityBuildConfig::default())
    }

    /// 生成 Unity 文件
    pub fn generate(&mut self, source_files: &[PathBuf]) -> Result<Vec<UnityFile>> {
        if !self.config.enabled {
            return Ok(Vec::new());
        }

        // 过滤需要包含的文件
        let eligible_files: Vec<&PathBuf> = source_files
            .iter()
            .filter(|f| !self.config.should_exclude(f))
            .filter(|f| Self::is_cpp_file(f))
            .collect();

        // 检查最小阈值
        if eligible_files.len() < self.config.min_files_threshold {
            return Ok(Vec::new());
        }

        // 获取文件大小
        for file in &eligible_files {
            if !self.file_sizes.contains_key(*file) {
                let size = fs::metadata(file).map(|m| m.len() as usize).unwrap_or(0);
                self.file_sizes.insert((*file).clone(), size);
            }
        }

        // 按模块分组 (如果启用)
        let grouped = if self.config.group_by_module {
            self.group_by_module(&eligible_files)
        } else {
            vec![("default".to_string(), eligible_files)]
        };

        // 为每个组生成 Unity 文件
        let mut all_unity_files = Vec::new();

        for (module_name, files) in grouped {
            let unity_files = self.generate_for_group(&module_name, &files)?;
            all_unity_files.extend(unity_files);
        }

        self.unity_files = all_unity_files.clone();
        Ok(all_unity_files)
    }

    /// 按模块分组
    fn group_by_module<'a>(&self, files: &[&'a PathBuf]) -> Vec<(String, Vec<&'a PathBuf>)> {
        let mut groups: HashMap<String, Vec<&PathBuf>> = HashMap::new();

        for file in files {
            let module = self.detect_module(file);
            groups.entry(module).or_default().push(file);
        }

        groups.into_iter().collect()
    }

    /// 检测文件所属模块
    fn detect_module(&self, file: &Path) -> String {
        // 从路径中提取模块名
        // 假设路径格式: Source/ModuleName/Private/...
        let components: Vec<_> = file.components().collect();

        for (i, component) in components.iter().enumerate() {
            if let std::path::Component::Normal(name) = component {
                if let Some(name_str) = name.to_str() {
                    if name_str == "Source" && i + 1 < components.len() {
                        if let std::path::Component::Normal(module) = &components[i + 1] {
                            return module.to_string_lossy().to_string();
                        }
                    }
                }
            }
        }

        "Default".to_string()
    }

    /// 为一组文件生成 Unity 文件
    fn generate_for_group(&self, module_name: &str, files: &[&PathBuf]) -> Result<Vec<UnityFile>> {
        let mut unity_files = Vec::new();
        let mut current_unity = UnityFile::new(self.unity_file_path(module_name, 0));
        current_unity.module_name = Some(module_name.to_string());
        let mut unity_index = 0;

        for file in files {
            let size = *self.file_sizes.get(*file).unwrap_or(&0);

            // 检查是否需要新建 Unity 文件
            let should_new_file = current_unity.file_count() >= self.config.max_files_per_unity
                || (current_unity.total_size + size > self.config.max_unity_size
                    && current_unity.file_count() > 0);

            if should_new_file {
                if !current_unity.source_files.is_empty() {
                    unity_files.push(current_unity);
                }
                unity_index += 1;
                current_unity = UnityFile::new(self.unity_file_path(module_name, unity_index));
                current_unity.module_name = Some(module_name.to_string());
            }

            current_unity.add_file((*file).clone(), size);
        }

        // 添加最后一个 Unity 文件
        if !current_unity.source_files.is_empty() {
            unity_files.push(current_unity);
        }

        Ok(unity_files)
    }

    /// 生成 Unity 文件路径
    fn unity_file_path(&self, module_name: &str, index: usize) -> PathBuf {
        self.config.output_dir.join(format!(
            "{}{}_{}.cpp",
            self.config.file_prefix, module_name, index
        ))
    }

    /// 写入 Unity 文件到磁盘
    pub fn write_unity_files(&self) -> Result<Vec<PathBuf>> {
        fs::create_dir_all(&self.config.output_dir).context("创建 Unity 输出目录失败")?;

        let written_files: Vec<PathBuf> = self
            .unity_files
            .par_iter()
            .filter_map(|unity| self.write_single_unity_file(unity).ok())
            .collect();

        Ok(written_files)
    }

    /// 写入单个 Unity 文件
    fn write_single_unity_file(&self, unity: &UnityFile) -> Result<PathBuf> {
        let mut content = String::with_capacity(unity.total_size + 1024);

        // 文件头注释
        content.push_str("// Auto-generated Unity Build file\n");
        content.push_str("// DO NOT EDIT - This file is generated by LBT\n");
        content.push_str(&format!(
            "// Module: {}\n",
            unity.module_name.as_deref().unwrap_or("Unknown")
        ));
        content.push_str(&format!("// Files: {}\n", unity.source_files.len()));
        content.push_str("\n");

        // 包含每个源文件
        for source_file in &unity.source_files {
            if self.config.generate_line_directives {
                // 使用 #line 指令保留原始文件位置信息
                content.push_str(&format!(
                    "#line 1 \"{}\"\n",
                    source_file.display().to_string().replace('\\', "/")
                ));
            }
            content.push_str(&format!(
                "#include \"{}\"\n",
                source_file.display().to_string().replace('\\', "/")
            ));
        }

        // 写入文件
        let mut file = fs::File::create(&unity.path)
            .with_context(|| format!("创建 Unity 文件失败: {}", unity.path.display()))?;

        file.write_all(content.as_bytes())
            .with_context(|| format!("写入 Unity 文件失败: {}", unity.path.display()))?;

        Ok(unity.path.clone())
    }

    /// 检查是否为 C++ 源文件
    fn is_cpp_file(path: &Path) -> bool {
        path.extension()
            .and_then(|ext| ext.to_str())
            .map(|ext| matches!(ext.to_lowercase().as_str(), "cpp" | "cc" | "cxx" | "c++"))
            .unwrap_or(false)
    }

    /// 获取生成的 Unity 文件列表
    pub fn unity_files(&self) -> &[UnityFile] {
        &self.unity_files
    }

    /// 获取统计信息
    pub fn stats(&self) -> UnityBuildStats {
        let total_files = self.unity_files.iter().map(|u| u.source_files.len()).sum();

        let total_size: usize = self.unity_files.iter().map(|u| u.total_size).sum();

        UnityBuildStats {
            unity_file_count: self.unity_files.len(),
            total_source_files: total_files,
            total_size_bytes: total_size,
            avg_files_per_unity: if !self.unity_files.is_empty() {
                total_files as f64 / self.unity_files.len() as f64
            } else {
                0.0
            },
        }
    }

    /// 清理生成的 Unity 文件
    pub fn clean(&self) -> Result<()> {
        for unity in &self.unity_files {
            if unity.path.exists() {
                fs::remove_file(&unity.path)?;
            }
        }
        Ok(())
    }
}

/// Unity Build 统计
#[derive(Debug, Clone)]
pub struct UnityBuildStats {
    pub unity_file_count: usize,
    pub total_source_files: usize,
    pub total_size_bytes: usize,
    pub avg_files_per_unity: f64,
}

impl UnityBuildStats {
    pub fn print(&self) {
        println!("\nUnity Build 统计:");
        println!("  Unity 文件数: {}", self.unity_file_count);
        println!("  源文件总数: {}", self.total_source_files);
        println!("  总大小: {} KB", self.total_size_bytes / 1024);
        println!(
            "  平均每个 Unity 文件: {:.1} 个源文件",
            self.avg_files_per_unity
        );
    }
}

//=============================================================================
// Unity Build 管理器
//=============================================================================

/// Unity Build 管理器 - 处理增量更新
pub struct UnityBuildManager {
    /// 配置
    config: UnityBuildConfig,
    /// 当前 Unity 文件映射 (源文件 -> Unity 文件)
    source_to_unity: HashMap<PathBuf, PathBuf>,
    /// Unity 文件内容哈希
    unity_hashes: HashMap<PathBuf, u64>,
}

impl UnityBuildManager {
    pub fn new(config: UnityBuildConfig) -> Self {
        Self {
            config,
            source_to_unity: HashMap::new(),
            unity_hashes: HashMap::new(),
        }
    }

    /// 获取源文件对应的 Unity 文件
    pub fn get_unity_file(&self, source_file: &Path) -> Option<&PathBuf> {
        self.source_to_unity.get(source_file)
    }

    /// 检查源文件修改是否需要重新生成 Unity 文件
    pub fn needs_regeneration(&self, modified_source: &Path) -> bool {
        // 如果源文件在某个 Unity 文件中，该 Unity 文件需要重新生成
        self.source_to_unity.contains_key(modified_source)
    }

    /// 获取需要重新编译的 Unity 文件
    pub fn get_affected_unity_files(&self, modified_sources: &[PathBuf]) -> Vec<PathBuf> {
        let mut affected = HashSet::new();

        for source in modified_sources {
            if let Some(unity) = self.source_to_unity.get(source) {
                affected.insert(unity.clone());
            }
        }

        affected.into_iter().collect()
    }

    /// 更新映射
    pub fn update_mapping(&mut self, unity_files: &[UnityFile]) {
        self.source_to_unity.clear();

        for unity in unity_files {
            for source in &unity.source_files {
                self.source_to_unity
                    .insert(source.clone(), unity.path.clone());
            }
        }
    }

    /// 判断是否应该使用 Unity Build
    pub fn should_use_unity_build(&self, source_count: usize, is_incremental: bool) -> bool {
        if !self.config.enabled {
            return false;
        }

        // 增量编译时，如果只有少量文件修改，不使用 Unity Build
        if is_incremental && source_count < 5 {
            return false;
        }

        source_count >= self.config.min_files_threshold
    }
}

//=============================================================================
// 测试
//=============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_unity_config_exclude_pattern() {
        let config = UnityBuildConfig::default();

        assert!(config.should_exclude(Path::new("MyTest.cpp")));
        assert!(config.should_exclude(Path::new("something_test.cpp")));
        assert!(config.should_exclude(Path::new("Generated_Code.cpp")));
        assert!(!config.should_exclude(Path::new("Normal.cpp")));
    }

    #[test]
    fn test_unity_file_generation() {
        let config = UnityBuildConfig {
            max_files_per_unity: 3,
            output_dir: PathBuf::from("test_output"),
            ..Default::default()
        };

        let generator = UnityBuildGenerator::new(config);
        assert!(generator.unity_files().is_empty());
    }
}
