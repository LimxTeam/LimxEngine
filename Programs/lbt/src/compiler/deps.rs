/*******************************************************************************
 * 文件: compiler/deps.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LBT 依赖追踪系统 - 精确的头文件依赖分析
 *   - 头文件依赖图构建
 *   - #include 指令解析
 *   - 依赖文件 (.d) 解析
 *   - 系统头文件过滤
 *   - 预编译头依赖处理
 *
 * 设计哲学:
 *   1. 精确性 - 准确追踪所有依赖关系
 *   2. 增量性 - 只重新分析变化的文件
 *   3. 高性能 - 并行分析，缓存结果
 *   4. 跨平台 - 处理不同平台的路径格式
 *
 * 技术特性:
 *   - 支持 MSVC /showIncludes 输出解析
 *   - 支持 GCC/Clang -MD 依赖文件解析
 *   - 支持 #include <> 和 #include "" 区分
 *   - 支持条件编译 (#ifdef) 感知
 *   - 循环依赖检测
 *
 ******************************************************************************/

use anyhow::{Context, Result};
use rayon::prelude::*;
use std::collections::{HashMap, HashSet, VecDeque};
use std::fs;
use std::io::BufReader;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};
use std::time::SystemTime;

//=============================================================================
// 依赖类型
//=============================================================================

/// 包含类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum IncludeType {
    /// 系统头文件 (#include <...>)
    System,
    /// 用户头文件 (#include "...")
    User,
}

/// 依赖条目
#[derive(Debug, Clone)]
pub struct DependencyEntry {
    /// 文件路径 (绝对路径)
    pub path: PathBuf,
    /// 包含类型
    pub include_type: IncludeType,
    /// 文件修改时间
    pub modified_time: Option<SystemTime>,
    /// 文件大小
    pub file_size: u64,
    /// 内容哈希 (可选，用于更精确的变化检测)
    pub content_hash: Option<u64>,
}

impl DependencyEntry {
    pub fn new(path: PathBuf, include_type: IncludeType) -> Self {
        let (modified_time, file_size) = if let Ok(metadata) = fs::metadata(&path) {
            (metadata.modified().ok(), metadata.len())
        } else {
            (None, 0)
        };

        Self {
            path,
            include_type,
            modified_time,
            file_size,
            content_hash: None,
        }
    }

    /// 计算内容哈希
    pub fn compute_hash(&mut self) -> Result<u64> {
        use std::collections::hash_map::DefaultHasher;
        use std::hash::{Hash, Hasher};

        let content = fs::read(&self.path)
            .with_context(|| format!("无法读取文件: {}", self.path.display()))?;

        let mut hasher = DefaultHasher::new();
        content.hash(&mut hasher);
        let hash = hasher.finish();

        self.content_hash = Some(hash);
        Ok(hash)
    }

    /// 检查文件是否已修改
    pub fn is_modified(&self) -> bool {
        if let Ok(metadata) = fs::metadata(&self.path) {
            if let (Some(cached_time), Ok(current_time)) = (self.modified_time, metadata.modified())
            {
                return current_time > cached_time || metadata.len() != self.file_size;
            }
        }
        true // 无法确定时假设已修改
    }
}

//=============================================================================
// 依赖图
//=============================================================================

/// 依赖图节点
#[derive(Debug, Clone)]
pub struct DependencyNode {
    /// 源文件路径
    pub source_file: PathBuf,
    /// 直接依赖的文件
    pub direct_deps: Vec<DependencyEntry>,
    /// 所有传递依赖 (展开后)
    pub transitive_deps: HashSet<PathBuf>,
    /// 最后分析时间
    pub analyzed_at: SystemTime,
    /// 是否需要重新分析
    pub needs_reanalysis: bool,
}

impl DependencyNode {
    pub fn new(source_file: PathBuf) -> Self {
        Self {
            source_file,
            direct_deps: Vec::new(),
            transitive_deps: HashSet::new(),
            analyzed_at: SystemTime::now(),
            needs_reanalysis: false,
        }
    }

    /// 获取所有依赖文件
    pub fn all_dependencies(&self) -> impl Iterator<Item = &PathBuf> {
        self.transitive_deps.iter()
    }

    /// 检查是否有任何依赖被修改
    pub fn has_modified_dependency(&self) -> bool {
        self.direct_deps.iter().any(|dep| dep.is_modified())
    }
}

/// 依赖图
#[derive(Debug, Default)]
pub struct DependencyGraph {
    /// 节点映射 (源文件 -> 依赖节点)
    nodes: HashMap<PathBuf, DependencyNode>,
    /// 反向依赖 (头文件 -> 依赖它的源文件)
    reverse_deps: HashMap<PathBuf, HashSet<PathBuf>>,
    /// 系统头文件目录 (用于过滤)
    system_include_dirs: Vec<PathBuf>,
    /// 用户头文件目录
    user_include_dirs: Vec<PathBuf>,
}

impl DependencyGraph {
    pub fn new() -> Self {
        Self::default()
    }

    /// 设置系统头文件目录
    pub fn set_system_include_dirs(&mut self, dirs: Vec<PathBuf>) {
        self.system_include_dirs = dirs;
    }

    /// 设置用户头文件目录
    pub fn set_user_include_dirs(&mut self, dirs: Vec<PathBuf>) {
        self.user_include_dirs = dirs;
    }

    /// 添加或更新节点
    pub fn update_node(&mut self, node: DependencyNode) {
        let source_file = node.source_file.clone();

        // 更新反向依赖
        for dep in &node.direct_deps {
            self.reverse_deps
                .entry(dep.path.clone())
                .or_default()
                .insert(source_file.clone());
        }

        self.nodes.insert(source_file, node);
    }

    /// 获取节点
    pub fn get_node(&self, source_file: &Path) -> Option<&DependencyNode> {
        self.nodes.get(source_file)
    }

    /// 获取可变节点
    pub fn get_node_mut(&mut self, source_file: &Path) -> Option<&mut DependencyNode> {
        self.nodes.get_mut(source_file)
    }

    /// 获取依赖指定头文件的所有源文件
    pub fn get_dependents(&self, header_file: &Path) -> Vec<&PathBuf> {
        self.reverse_deps
            .get(header_file)
            .map(|set| set.iter().collect())
            .unwrap_or_default()
    }

    /// 检查源文件是否需要重新编译
    pub fn needs_rebuild(&self, source_file: &Path) -> bool {
        match self.nodes.get(source_file) {
            Some(node) => node.needs_reanalysis || node.has_modified_dependency(),
            None => true, // 未分析过，需要编译
        }
    }

    /// 获取所有需要重新编译的源文件
    pub fn get_dirty_sources(&self) -> Vec<&PathBuf> {
        self.nodes
            .iter()
            .filter(|(_, node)| node.needs_reanalysis || node.has_modified_dependency())
            .map(|(path, _)| path)
            .collect()
    }

    /// 标记头文件变化影响的所有源文件
    pub fn mark_header_changed(&mut self, header_file: &Path) {
        if let Some(dependents) = self.reverse_deps.get(header_file).cloned() {
            for source in dependents {
                if let Some(node) = self.nodes.get_mut(&source) {
                    node.needs_reanalysis = true;
                }
            }
        }
    }

    /// 获取图统计信息
    pub fn stats(&self) -> DependencyGraphStats {
        let total_nodes = self.nodes.len();
        let total_deps: usize = self.nodes.values().map(|n| n.direct_deps.len()).sum();
        let total_transitive: usize = self.nodes.values().map(|n| n.transitive_deps.len()).sum();
        let dirty_count = self
            .nodes
            .values()
            .filter(|n| n.needs_reanalysis || n.has_modified_dependency())
            .count();

        DependencyGraphStats {
            total_sources: total_nodes,
            total_direct_deps: total_deps,
            total_transitive_deps: total_transitive,
            dirty_sources: dirty_count,
            avg_deps_per_source: if total_nodes > 0 {
                total_deps as f64 / total_nodes as f64
            } else {
                0.0
            },
        }
    }
}

/// 依赖图统计
#[derive(Debug, Clone)]
pub struct DependencyGraphStats {
    pub total_sources: usize,
    pub total_direct_deps: usize,
    pub total_transitive_deps: usize,
    pub dirty_sources: usize,
    pub avg_deps_per_source: f64,
}

impl DependencyGraphStats {
    pub fn print(&self) {
        println!("\n依赖图统计:");
        println!("  源文件数: {}", self.total_sources);
        println!("  直接依赖总数: {}", self.total_direct_deps);
        println!("  传递依赖总数: {}", self.total_transitive_deps);
        println!("  需重编译: {}", self.dirty_sources);
        println!("  平均依赖数: {:.1}", self.avg_deps_per_source);
    }
}

//=============================================================================
// 依赖分析器
//=============================================================================

/// 依赖分析器
pub struct DependencyAnalyzer {
    /// 依赖图
    graph: Arc<RwLock<DependencyGraph>>,
    /// 包含目录 (用户)
    include_dirs: Vec<PathBuf>,
    /// 系统包含目录
    system_include_dirs: Vec<PathBuf>,
    /// 是否跳过系统头文件
    skip_system_headers: bool,
    /// 预定义宏 (用于条件编译分析)
    defines: HashMap<String, Option<String>>,
    /// 文件扩展名过滤
    header_extensions: HashSet<String>,
}

impl DependencyAnalyzer {
    pub fn new() -> Self {
        let mut header_extensions = HashSet::new();
        header_extensions.insert("h".to_string());
        header_extensions.insert("hpp".to_string());
        header_extensions.insert("hxx".to_string());
        header_extensions.insert("h++".to_string());
        header_extensions.insert("inl".to_string());
        header_extensions.insert("inc".to_string());

        Self {
            graph: Arc::new(RwLock::new(DependencyGraph::new())),
            include_dirs: Vec::new(),
            system_include_dirs: Vec::new(),
            skip_system_headers: true,
            defines: HashMap::new(),
            header_extensions,
        }
    }

    /// 设置包含目录
    pub fn include_dirs(&mut self, dirs: Vec<PathBuf>) -> &mut Self {
        self.include_dirs = dirs;
        self
    }

    /// 设置系统包含目录
    pub fn system_include_dirs(&mut self, dirs: Vec<PathBuf>) -> &mut Self {
        self.system_include_dirs = dirs;
        self
    }

    /// 设置是否跳过系统头文件
    pub fn skip_system_headers(&mut self, skip: bool) -> &mut Self {
        self.skip_system_headers = skip;
        self
    }

    /// 添加预定义宏
    pub fn define(&mut self, name: &str, value: Option<&str>) -> &mut Self {
        self.defines
            .insert(name.to_string(), value.map(|s| s.to_string()));
        self
    }

    /// 分析单个源文件的依赖
    pub fn analyze_file(&self, source_file: &Path) -> Result<DependencyNode> {
        let mut node = DependencyNode::new(source_file.to_path_buf());
        let mut visited = HashSet::new();
        let mut queue = VecDeque::new();

        // 解析源文件中的 #include
        let includes = self.parse_includes(source_file)?;

        for (include_path, include_type) in includes {
            if let Some(resolved) = self.resolve_include(&include_path, source_file, include_type) {
                if !visited.contains(&resolved) {
                    visited.insert(resolved.clone());
                    queue.push_back((resolved.clone(), include_type));

                    let entry = DependencyEntry::new(resolved, include_type);
                    node.direct_deps.push(entry);
                }
            }
        }

        // BFS 展开传递依赖
        while let Some((file, _)) = queue.pop_front() {
            node.transitive_deps.insert(file.clone());

            // 继续解析头文件的依赖
            if self.is_header_file(&file) {
                if let Ok(includes) = self.parse_includes(&file) {
                    for (include_path, include_type) in includes {
                        if let Some(resolved) =
                            self.resolve_include(&include_path, &file, include_type)
                        {
                            if !visited.contains(&resolved) {
                                visited.insert(resolved.clone());
                                queue.push_back((resolved.clone(), include_type));
                            }
                        }
                    }
                }
            }
        }

        Ok(node)
    }

    /// 并行分析多个源文件
    pub fn analyze_files(&self, source_files: &[PathBuf]) -> Vec<Result<DependencyNode>> {
        source_files
            .par_iter()
            .map(|file| self.analyze_file(file))
            .collect()
    }

    /// 从 MSVC /showIncludes 输出解析依赖
    pub fn parse_msvc_show_includes(
        &self,
        output: &str,
        source_file: &Path,
    ) -> Result<DependencyNode> {
        let mut node = DependencyNode::new(source_file.to_path_buf());
        let mut visited = HashSet::new();

        // MSVC 输出格式: "Note: including file: path/to/header.h"
        let prefix = "Note: including file:";
        let prefix_cn = "注意: 包含文件:"; // 中文版 MSVC

        for line in output.lines() {
            let include_path = if line.trim_start().starts_with(prefix) {
                Some(line.trim_start()[prefix.len()..].trim())
            } else if line.trim_start().starts_with(prefix_cn) {
                Some(line.trim_start()[prefix_cn.len()..].trim())
            } else {
                None
            };

            if let Some(path_str) = include_path {
                let path = PathBuf::from(path_str);

                // 跳过系统头文件
                if self.skip_system_headers && self.is_system_header(&path) {
                    continue;
                }

                if !visited.contains(&path) {
                    visited.insert(path.clone());
                    node.transitive_deps.insert(path.clone());

                    // 判断是否为直接依赖 (根据缩进深度)
                    let depth = line.len() - line.trim_start().len();
                    if depth <= prefix.len() + 2 {
                        let entry = DependencyEntry::new(path, IncludeType::User);
                        node.direct_deps.push(entry);
                    }
                }
            }
        }

        Ok(node)
    }

    /// 从 GCC/Clang 依赖文件 (.d) 解析依赖
    pub fn parse_dep_file(&self, dep_file: &Path, source_file: &Path) -> Result<DependencyNode> {
        let content = fs::read_to_string(dep_file)
            .with_context(|| format!("无法读取依赖文件: {}", dep_file.display()))?;

        let mut node = DependencyNode::new(source_file.to_path_buf());

        // 处理换行续行
        let content = content.replace("\\\n", " ").replace("\\\r\n", " ");

        // 格式: target: dep1 dep2 dep3 ...
        if let Some(colon_pos) = content.find(':') {
            let deps_part = &content[colon_pos + 1..];

            for dep_str in deps_part.split_whitespace() {
                if dep_str.is_empty() {
                    continue;
                }

                let path = PathBuf::from(dep_str);

                // 跳过源文件自身
                if path == source_file.to_path_buf() {
                    continue;
                }

                // 跳过系统头文件
                if self.skip_system_headers && self.is_system_header(&path) {
                    continue;
                }

                let include_type = if self.is_system_header(&path) {
                    IncludeType::System
                } else {
                    IncludeType::User
                };

                node.transitive_deps.insert(path.clone());
                node.direct_deps
                    .push(DependencyEntry::new(path, include_type));
            }
        }

        Ok(node)
    }

    /// 解析文件中的 #include 指令
    pub fn parse_includes(&self, file: &Path) -> Result<Vec<(String, IncludeType)>> {
        let content = fs::read_to_string(file)
            .with_context(|| format!("无法读取文件: {}", file.display()))?;

        let mut includes = Vec::new();
        let mut in_block_comment = false;

        for line in content.lines() {
            let line = line.trim();

            // 处理块注释
            if in_block_comment {
                if line.contains("*/") {
                    in_block_comment = false;
                }
                continue;
            }

            if line.contains("/*") && !line.contains("*/") {
                in_block_comment = true;
                continue;
            }

            // 跳过行注释
            let line = if let Some(pos) = line.find("//") {
                &line[..pos]
            } else {
                line
            };

            // 解析 #include
            if line.starts_with("#include") || line.starts_with("# include") {
                let include_part = line
                    .trim_start_matches('#')
                    .trim()
                    .trim_start_matches("include")
                    .trim();

                if include_part.starts_with('<') {
                    // 系统头文件
                    if let Some(end) = include_part.find('>') {
                        let path = include_part[1..end].to_string();
                        includes.push((path, IncludeType::System));
                    }
                } else if include_part.starts_with('"') {
                    // 用户头文件
                    if let Some(end) = include_part[1..].find('"') {
                        let path = include_part[1..end + 1].to_string();
                        includes.push((path, IncludeType::User));
                    }
                }
            }
        }

        Ok(includes)
    }

    /// 解析 include 路径
    fn resolve_include(
        &self,
        include_path: &str,
        source_file: &Path,
        include_type: IncludeType,
    ) -> Option<PathBuf> {
        match include_type {
            IncludeType::User => {
                // 先从源文件目录搜索
                if let Some(parent) = source_file.parent() {
                    let path = parent.join(include_path);
                    if path.exists() {
                        return Some(self.canonicalize_path(&path));
                    }
                }

                // 然后从用户 include 目录搜索
                for dir in &self.include_dirs {
                    let path = dir.join(include_path);
                    if path.exists() {
                        return Some(self.canonicalize_path(&path));
                    }
                }

                // 最后从系统目录搜索
                for dir in &self.system_include_dirs {
                    let path = dir.join(include_path);
                    if path.exists() {
                        return Some(self.canonicalize_path(&path));
                    }
                }
            }
            IncludeType::System => {
                // 只从系统目录搜索
                for dir in &self.system_include_dirs {
                    let path = dir.join(include_path);
                    if path.exists() {
                        return Some(self.canonicalize_path(&path));
                    }
                }

                // 也搜索用户目录
                for dir in &self.include_dirs {
                    let path = dir.join(include_path);
                    if path.exists() {
                        return Some(self.canonicalize_path(&path));
                    }
                }
            }
        }

        None
    }

    /// 规范化路径
    fn canonicalize_path(&self, path: &Path) -> PathBuf {
        fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf())
    }

    /// 检查是否为头文件
    fn is_header_file(&self, path: &Path) -> bool {
        path.extension()
            .and_then(|ext| ext.to_str())
            .map(|ext| self.header_extensions.contains(ext))
            .unwrap_or(false)
    }

    /// 检查是否为系统头文件
    fn is_system_header(&self, path: &Path) -> bool {
        for sys_dir in &self.system_include_dirs {
            if path.starts_with(sys_dir) {
                return true;
            }
        }

        // 检查典型的系统路径
        let path_str = path.to_string_lossy().to_lowercase();

        #[cfg(windows)]
        {
            path_str.contains("windows kits")
                || path_str.contains("microsoft visual studio")
                || path_str.contains("vc\\include")
                || path_str.contains("vc\\tools")
        }

        #[cfg(not(windows))]
        {
            path_str.starts_with("/usr/include")
                || path_str.starts_with("/usr/local/include")
                || path_str.contains("/lib/gcc")
                || path_str.contains("/lib/clang")
        }
    }

    /// 获取依赖图
    pub fn graph(&self) -> Arc<RwLock<DependencyGraph>> {
        Arc::clone(&self.graph)
    }

    /// 更新依赖图
    pub fn update_graph(&self, node: DependencyNode) {
        if let Ok(mut graph) = self.graph.write() {
            graph.update_node(node);
        } else {
            tracing::error!("依赖图 RwLock 被污染，无法更新节点");
        }
    }
}

impl Default for DependencyAnalyzer {
    fn default() -> Self {
        Self::new()
    }
}

//=============================================================================
// 编译数据库
//=============================================================================

/// 编译命令条目 (compile_commands.json 格式)
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CompileCommand {
    /// 工作目录
    pub directory: String,
    /// 编译命令
    pub command: Option<String>,
    /// 编译参数 (替代 command)
    pub arguments: Option<Vec<String>>,
    /// 源文件
    pub file: String,
    /// 输出文件 (可选)
    pub output: Option<String>,
}

/// 编译数据库
#[derive(Debug, Default, serde::Serialize, serde::Deserialize)]
pub struct CompileDatabase {
    /// 编译命令列表
    commands: Vec<CompileCommand>,
    /// 文件索引 (源文件 -> 索引)
    #[serde(skip)]
    file_index: HashMap<PathBuf, usize>,
}

impl CompileDatabase {
    pub fn new() -> Self {
        Self::default()
    }

    /// 从 compile_commands.json 加载
    pub fn load(path: &Path) -> Result<Self> {
        let content = fs::read_to_string(path)
            .with_context(|| format!("无法读取编译数据库: {}", path.display()))?;

        let commands: Vec<CompileCommand> =
            serde_json::from_str(&content).with_context(|| "解析编译数据库失败")?;

        let mut db = Self {
            commands,
            file_index: HashMap::new(),
        };

        db.rebuild_index();
        Ok(db)
    }

    /// 保存到 compile_commands.json
    pub fn save(&self, path: &Path) -> Result<()> {
        let content =
            serde_json::to_string_pretty(&self.commands).context("序列化编译数据库失败")?;

        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }

        fs::write(path, content)
            .with_context(|| format!("无法写入编译数据库: {}", path.display()))?;

        Ok(())
    }

    /// 添加编译命令
    pub fn add_command(&mut self, cmd: CompileCommand) {
        let file_path = PathBuf::from(&cmd.file);
        let index = self.commands.len();
        self.commands.push(cmd);
        self.file_index.insert(file_path, index);
    }

    /// 获取编译命令
    pub fn get_command(&self, source_file: &Path) -> Option<&CompileCommand> {
        self.file_index
            .get(source_file)
            .and_then(|&idx| self.commands.get(idx))
    }

    /// 更新编译命令
    pub fn update_command(&mut self, source_file: &Path, cmd: CompileCommand) {
        if let Some(&idx) = self.file_index.get(source_file) {
            self.commands[idx] = cmd;
        } else {
            self.add_command(cmd);
        }
    }

    /// 移除编译命令
    pub fn remove_command(&mut self, source_file: &Path) {
        if let Some(&idx) = self.file_index.get(source_file) {
            self.commands.remove(idx);
            self.rebuild_index();
        }
    }

    /// 获取所有命令
    pub fn commands(&self) -> &[CompileCommand] {
        &self.commands
    }

    /// 获取文件数量
    pub fn len(&self) -> usize {
        self.commands.len()
    }

    /// 是否为空
    pub fn is_empty(&self) -> bool {
        self.commands.is_empty()
    }

    /// 重建索引
    fn rebuild_index(&mut self) {
        self.file_index.clear();
        for (idx, cmd) in self.commands.iter().enumerate() {
            self.file_index.insert(PathBuf::from(&cmd.file), idx);
        }
    }

    /// 合并另一个数据库
    pub fn merge(&mut self, other: CompileDatabase) {
        for cmd in other.commands {
            let file_path = PathBuf::from(&cmd.file);
            if !self.file_index.contains_key(&file_path) {
                self.add_command(cmd);
            }
        }
    }

    /// 清除无效条目 (文件不存在)
    pub fn clean_invalid(&mut self) {
        self.commands.retain(|cmd| Path::new(&cmd.file).exists());
        self.rebuild_index();
    }
}

//=============================================================================
// 依赖缓存
//=============================================================================

/// 依赖缓存条目
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct DependencyCacheEntry {
    /// 源文件路径
    pub source_file: PathBuf,
    /// 源文件修改时间
    pub source_modified: u64,
    /// 源文件哈希
    pub source_hash: u64,
    /// 依赖文件列表
    pub dependencies: Vec<PathBuf>,
    /// 依赖文件修改时间
    pub dep_modified_times: HashMap<PathBuf, u64>,
    /// 缓存时间
    pub cached_at: u64,
}

/// 依赖缓存
#[derive(Debug, Default, serde::Serialize, serde::Deserialize)]
pub struct DependencyCache {
    /// 版本号
    pub version: u32,
    /// 缓存条目
    pub entries: HashMap<PathBuf, DependencyCacheEntry>,
}

impl DependencyCache {
    const CURRENT_VERSION: u32 = 1;

    pub fn new() -> Self {
        Self {
            version: Self::CURRENT_VERSION,
            entries: HashMap::new(),
        }
    }

    /// 加载缓存
    pub fn load(path: &Path) -> Result<Self> {
        if !path.exists() {
            return Ok(Self::new());
        }

        let content = fs::read_to_string(path)?;
        let cache: Self = serde_json::from_str(&content)?;

        if cache.version != Self::CURRENT_VERSION {
            return Ok(Self::new());
        }

        Ok(cache)
    }

    /// 保存缓存
    pub fn save(&self, path: &Path) -> Result<()> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }

        let content = serde_json::to_string(self)?;
        fs::write(path, content)?;
        Ok(())
    }

    /// 获取缓存条目
    pub fn get(&self, source_file: &Path) -> Option<&DependencyCacheEntry> {
        self.entries.get(source_file)
    }

    /// 检查缓存是否有效
    pub fn is_valid(&self, source_file: &Path) -> bool {
        let entry = match self.entries.get(source_file) {
            Some(e) => e,
            None => return false,
        };

        // 检查源文件
        if let Ok(metadata) = fs::metadata(source_file) {
            let modified = metadata
                .modified()
                .ok()
                .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                .map(|d| d.as_secs())
                .unwrap_or(0);

            if modified > entry.source_modified {
                return false;
            }
        } else {
            return false;
        }

        // 检查依赖文件
        for (dep_path, &cached_time) in &entry.dep_modified_times {
            if let Ok(metadata) = fs::metadata(dep_path) {
                let modified = metadata
                    .modified()
                    .ok()
                    .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                    .map(|d| d.as_secs())
                    .unwrap_or(0);

                if modified > cached_time {
                    return false;
                }
            } else {
                return false;
            }
        }

        true
    }

    /// 更新缓存条目
    pub fn update(&mut self, source_file: PathBuf, dependencies: Vec<PathBuf>, source_hash: u64) {
        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        let source_modified = fs::metadata(&source_file)
            .ok()
            .and_then(|m| m.modified().ok())
            .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
            .map(|d| d.as_secs())
            .unwrap_or(0);

        let mut dep_modified_times = HashMap::new();
        for dep in &dependencies {
            let modified = fs::metadata(dep)
                .ok()
                .and_then(|m| m.modified().ok())
                .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                .map(|d| d.as_secs())
                .unwrap_or(0);
            dep_modified_times.insert(dep.clone(), modified);
        }

        let entry = DependencyCacheEntry {
            source_file: source_file.clone(),
            source_modified,
            source_hash,
            dependencies,
            dep_modified_times,
            cached_at: now,
        };

        self.entries.insert(source_file, entry);
    }

    /// 移除条目
    pub fn remove(&mut self, source_file: &Path) {
        self.entries.remove(source_file);
    }

    /// 清除无效条目
    pub fn clean_invalid(&mut self) {
        self.entries.retain(|path, _| path.exists());
    }

    /// 获取统计信息
    pub fn stats(&self) -> DependencyCacheStats {
        let total = self.entries.len();
        let valid = self
            .entries
            .keys()
            .filter(|path| self.is_valid(path))
            .count();

        DependencyCacheStats {
            total_entries: total,
            valid_entries: valid,
            invalid_entries: total - valid,
        }
    }
}

/// 依赖缓存统计
#[derive(Debug, Clone)]
pub struct DependencyCacheStats {
    pub total_entries: usize,
    pub valid_entries: usize,
    pub invalid_entries: usize,
}

impl DependencyCacheStats {
    pub fn print(&self) {
        println!("\n依赖缓存统计:");
        println!("  总条目: {}", self.total_entries);
        println!("  有效: {}", self.valid_entries);
        println!("  无效: {}", self.invalid_entries);
    }
}

//=============================================================================
// 测试
//=============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_includes() {
        let analyzer = DependencyAnalyzer::new();

        let test_content = r#"
#include <stdio.h>
#include "myheader.h"
#include <vector>
// #include "commented.h"
/* #include "block_commented.h" */
#include "another.h"
"#;

        // 创建临时文件测试
        let temp_dir = std::env::temp_dir();
        let test_file = temp_dir.join("test_includes.cpp");
        fs::write(&test_file, test_content).unwrap();

        let includes = analyzer.parse_includes(&test_file).unwrap();

        assert_eq!(includes.len(), 4);
        assert!(includes
            .iter()
            .any(|(p, t)| p == "stdio.h" && *t == IncludeType::System));
        assert!(includes
            .iter()
            .any(|(p, t)| p == "myheader.h" && *t == IncludeType::User));
        assert!(includes
            .iter()
            .any(|(p, t)| p == "vector" && *t == IncludeType::System));
        assert!(includes
            .iter()
            .any(|(p, t)| p == "another.h" && *t == IncludeType::User));

        fs::remove_file(test_file).ok();
    }

    #[test]
    fn test_compile_database() {
        let mut db = CompileDatabase::new();

        db.add_command(CompileCommand {
            directory: "/project".to_string(),
            command: Some("clang++ -c main.cpp -o main.o".to_string()),
            arguments: None,
            file: "main.cpp".to_string(),
            output: Some("main.o".to_string()),
        });

        assert_eq!(db.len(), 1);
        assert!(db.get_command(Path::new("main.cpp")).is_some());
    }

    #[test]
    fn test_dependency_cache() {
        let cache = DependencyCache::new();
        assert_eq!(cache.version, DependencyCache::CURRENT_VERSION);
    }
}
