/*******************************************************************************
 * 文件: compiler/action.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   构建动作图系统 - 类似 UE 的 ActionGraph
 *   - 动作定义与管理
 *   - 动作依赖分析
 *   - 增量构建支持
 *   - 动作缓存与复用
 *
 * 设计哲学:
 *   1. 动作原子性 - 每个动作独立可执行
 *   2. 确定性 - 相同输入产生相同输出
 *   3. 可缓存 - 支持分布式缓存
 *   4. 可追溯 - 完整的依赖链追踪
 *
 ******************************************************************************/

use anyhow::{anyhow, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::hash::{Hash, Hasher};
use std::io::{BufReader, BufWriter};
use std::path::{Path, PathBuf};
use std::time::SystemTime;
use xxhash_rust::xxh3::xxh3_64;

use super::TargetType;

//=============================================================================
// 动作类型
//=============================================================================

/// 动作类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ActionType {
    /// 编译 C/C++ 源文件
    Compile,
    /// 创建预编译头
    PrecompileHeader,
    /// 链接可执行文件
    LinkExecutable,
    /// 链接动态库
    LinkDynamicLib,
    /// 创建静态库
    CreateStaticLib,
    /// 生成资源文件
    GenerateResource,
    /// 复制文件
    CopyFile,
    /// 自定义命令
    CustomCommand,
}

impl ActionType {
    pub fn name(&self) -> &'static str {
        match self {
            Self::Compile => "Compile",
            Self::PrecompileHeader => "PrecompileHeader",
            Self::LinkExecutable => "Link",
            Self::LinkDynamicLib => "LinkDLL",
            Self::CreateStaticLib => "Lib",
            Self::GenerateResource => "Resource",
            Self::CopyFile => "Copy",
            Self::CustomCommand => "Command",
        }
    }
}

//=============================================================================
// 构建动作
//=============================================================================

/// 构建动作 ID
pub type ActionId = u64;

/// 构建动作
#[derive(Debug, Clone)]
pub struct BuildAction {
    /// 动作 ID (基于输入哈希)
    pub id: ActionId,
    /// 动作类型
    pub action_type: ActionType,
    /// 所属模块
    pub module_name: String,
    /// 输入文件
    pub inputs: Vec<PathBuf>,
    /// 输出文件
    pub outputs: Vec<PathBuf>,
    /// 命令行
    pub command_line: String,
    /// 工作目录
    pub working_directory: PathBuf,
    /// 环境变量
    pub environment: HashMap<String, String>,
    /// 依赖的动作 ID
    pub dependencies: Vec<ActionId>,
    /// 是否可缓存
    pub cacheable: bool,
    /// 预计耗时 (毫秒)
    pub estimated_duration_ms: u64,
    /// 权重 (用于调度优先级)
    pub weight: i32,
    /// 响应文件路径 (如果命令行过长)
    pub response_file: Option<PathBuf>,
    /// 描述信息
    pub description: String,
}

impl BuildAction {
    /// 创建编译动作
    pub fn compile(
        module_name: &str,
        source: PathBuf,
        object: PathBuf,
        command_line: String,
    ) -> Self {
        let mut action = Self::new(ActionType::Compile, module_name);
        action.inputs.push(source.clone());
        action.outputs.push(object);
        action.command_line = command_line;
        action.description = format!(
            "编译 {}",
            source
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_default()
        );
        action.update_id();
        action
    }

    /// 创建链接动作
    pub fn link(
        module_name: &str,
        objects: Vec<PathBuf>,
        output: PathBuf,
        command_line: String,
        target_type: TargetType,
    ) -> Self {
        let action_type = match target_type {
            TargetType::Executable => ActionType::LinkExecutable,
            TargetType::DynamicLibrary => ActionType::LinkDynamicLib,
            TargetType::StaticLibrary => ActionType::CreateStaticLib,
            _ => ActionType::LinkExecutable,
        };

        let mut action = Self::new(action_type, module_name);
        action.inputs = objects;
        action.outputs.push(output.clone());
        action.command_line = command_line;
        action.description = format!(
            "链接 {}",
            output
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_default()
        );
        action.cacheable = false; // 链接通常不缓存
        action.update_id();
        action
    }

    /// 创建新动作
    fn new(action_type: ActionType, module_name: &str) -> Self {
        Self {
            id: 0,
            action_type,
            module_name: module_name.to_string(),
            inputs: Vec::new(),
            outputs: Vec::new(),
            command_line: String::new(),
            working_directory: std::env::current_dir().unwrap_or_default(),
            environment: HashMap::new(),
            dependencies: Vec::new(),
            cacheable: true,
            estimated_duration_ms: 1000,
            weight: 0,
            response_file: None,
            description: String::new(),
        }
    }

    /// 更新动作 ID (基于输入哈希)
    fn update_id(&mut self) {
        use std::collections::hash_map::DefaultHasher;
        let mut hasher = DefaultHasher::new();

        self.action_type.hash(&mut hasher);
        self.command_line.hash(&mut hasher);

        for input in &self.inputs {
            input.hash(&mut hasher);
        }
        for output in &self.outputs {
            output.hash(&mut hasher);
        }

        self.id = hasher.finish();
    }

    /// 添加依赖
    pub fn depends_on(&mut self, action_id: ActionId) {
        if !self.dependencies.contains(&action_id) {
            self.dependencies.push(action_id);
        }
    }

    /// 获取主输出文件
    pub fn primary_output(&self) -> Option<&Path> {
        self.outputs.first().map(|p| p.as_path())
    }

    /// 检查是否需要执行 (基于时间戳)
    pub fn needs_execution(&self) -> Result<bool> {
        // 如果输出不存在，需要执行
        for output in &self.outputs {
            if !output.exists() {
                return Ok(true);
            }
        }

        // 获取输出的最旧修改时间
        let oldest_output = self
            .outputs
            .iter()
            .filter_map(|p| fs::metadata(p).ok())
            .filter_map(|m| m.modified().ok())
            .min();

        let oldest_output = match oldest_output {
            Some(t) => t,
            None => return Ok(true),
        };

        // 检查是否有输入比输出新
        for input in &self.inputs {
            if let Ok(metadata) = fs::metadata(input) {
                if let Ok(modified) = metadata.modified() {
                    if modified > oldest_output {
                        return Ok(true);
                    }
                }
            }
        }

        Ok(false)
    }

    /// 计算内容哈希 (用于缓存)
    pub fn content_hash(&self) -> Result<u64> {
        use std::collections::hash_map::DefaultHasher;
        let mut hasher = DefaultHasher::new();

        // 哈希命令行
        self.command_line.hash(&mut hasher);

        // 哈希输入文件内容
        for input in &self.inputs {
            if input.exists() {
                if let Ok(content) = fs::read(input) {
                    content.hash(&mut hasher);
                }
            }
        }

        Ok(hasher.finish())
    }
}

//=============================================================================
// 动作图
//=============================================================================

/// 动作图
#[derive(Debug, Default)]
pub struct ActionGraph {
    /// 所有动作
    actions: HashMap<ActionId, BuildAction>,
    /// 按模块分组的动作
    by_module: HashMap<String, Vec<ActionId>>,
    /// 按输出文件索引
    by_output: HashMap<PathBuf, ActionId>,
    /// 根动作 (无依赖者)
    roots: Vec<ActionId>,
    /// 叶动作 (无依赖)
    leaves: Vec<ActionId>,
}

impl ActionGraph {
    pub fn new() -> Self {
        Self::default()
    }

    /// 添加动作
    pub fn add_action(&mut self, action: BuildAction) -> ActionId {
        let id = action.id;

        // 按模块索引
        self.by_module
            .entry(action.module_name.clone())
            .or_default()
            .push(id);

        // 按输出索引
        for output in &action.outputs {
            self.by_output.insert(output.clone(), id);
        }

        self.actions.insert(id, action);
        id
    }

    /// 获取动作
    pub fn get_action(&self, id: ActionId) -> Option<&BuildAction> {
        self.actions.get(&id)
    }

    /// 获取可变动作
    pub fn get_action_mut(&mut self, id: ActionId) -> Option<&mut BuildAction> {
        self.actions.get_mut(&id)
    }

    /// 根据输出文件获取动作
    pub fn get_action_by_output(&self, output: &Path) -> Option<&BuildAction> {
        self.by_output
            .get(output)
            .and_then(|id| self.actions.get(id))
    }

    /// 获取模块的所有动作
    pub fn get_module_actions(&self, module_name: &str) -> Vec<&BuildAction> {
        self.by_module
            .get(module_name)
            .map(|ids| ids.iter().filter_map(|id| self.actions.get(id)).collect())
            .unwrap_or_default()
    }

    /// 添加依赖关系
    pub fn add_dependency(&mut self, action_id: ActionId, depends_on: ActionId) -> Result<()> {
        if action_id == depends_on {
            return Err(anyhow!("动作不能依赖自身"));
        }

        if let Some(action) = self.actions.get_mut(&action_id) {
            action.depends_on(depends_on);
            Ok(())
        } else {
            Err(anyhow!("动作不存在: {}", action_id))
        }
    }

    /// 根据输出添加依赖
    pub fn add_dependency_on_output(&mut self, action_id: ActionId, output: &Path) -> Result<()> {
        if let Some(&dep_id) = self.by_output.get(output) {
            self.add_dependency(action_id, dep_id)
        } else {
            Ok(()) // 输出不在图中，忽略
        }
    }

    /// 完成图构建，计算根和叶节点
    pub fn finalize(&mut self) {
        // 收集所有被依赖的动作
        let mut has_dependents: HashSet<ActionId> = HashSet::new();
        for action in self.actions.values() {
            for &dep in &action.dependencies {
                has_dependents.insert(dep);
            }
        }

        // 根节点：没有被任何动作依赖
        self.roots = self
            .actions
            .keys()
            .filter(|id| !has_dependents.contains(id))
            .copied()
            .collect();

        // 叶节点：没有依赖任何动作
        self.leaves = self
            .actions
            .values()
            .filter(|a| a.dependencies.is_empty())
            .map(|a| a.id)
            .collect();
    }

    /// 获取根动作
    pub fn roots(&self) -> &[ActionId] {
        &self.roots
    }

    /// 获取叶动作
    pub fn leaves(&self) -> &[ActionId] {
        &self.leaves
    }

    /// 获取所有动作
    pub fn all_actions(&self) -> impl Iterator<Item = &BuildAction> {
        self.actions.values()
    }

    /// 获取动作数量
    pub fn action_count(&self) -> usize {
        self.actions.len()
    }

    /// 获取需要执行的动作
    pub fn get_outdated_actions(&self) -> Result<Vec<ActionId>> {
        let mut outdated = Vec::new();

        for (id, action) in &self.actions {
            if action.needs_execution()? {
                outdated.push(*id);
            }
        }

        Ok(outdated)
    }

    /// 拓扑排序
    pub fn topological_sort(&self) -> Result<Vec<ActionId>> {
        let mut result = Vec::with_capacity(self.actions.len());
        let mut visited = HashSet::new();
        let mut temp_visited = HashSet::new();

        for &id in self.actions.keys() {
            self.topological_visit(id, &mut visited, &mut temp_visited, &mut result)?;
        }

        result.reverse();
        Ok(result)
    }

    fn topological_visit(
        &self,
        id: ActionId,
        visited: &mut HashSet<ActionId>,
        temp_visited: &mut HashSet<ActionId>,
        result: &mut Vec<ActionId>,
    ) -> Result<()> {
        if visited.contains(&id) {
            return Ok(());
        }
        if temp_visited.contains(&id) {
            return Err(anyhow!("检测到循环依赖"));
        }

        temp_visited.insert(id);

        if let Some(action) = self.actions.get(&id) {
            for &dep in &action.dependencies {
                self.topological_visit(dep, visited, temp_visited, result)?;
            }
        }

        temp_visited.remove(&id);
        visited.insert(id);
        result.push(id);

        Ok(())
    }

    /// 计算关键路径
    pub fn critical_path(&self) -> Vec<ActionId> {
        let sorted = match self.topological_sort() {
            Ok(s) => s,
            Err(_) => return Vec::new(),
        };

        // 计算每个动作的最长路径时间
        let mut longest_path: HashMap<ActionId, u64> = HashMap::new();
        let mut predecessor: HashMap<ActionId, Option<ActionId>> = HashMap::new();

        for &id in &sorted {
            let action = match self.actions.get(&id) {
                Some(a) => a,
                None => continue,
            };

            let mut max_dep_time = 0u64;
            let mut max_pred = None;

            for &dep in &action.dependencies {
                if let Some(&time) = longest_path.get(&dep) {
                    if time > max_dep_time {
                        max_dep_time = time;
                        max_pred = Some(dep);
                    }
                }
            }

            longest_path.insert(id, max_dep_time + action.estimated_duration_ms);
            predecessor.insert(id, max_pred);
        }

        // 找到最长路径的终点
        let end = longest_path
            .iter()
            .max_by_key(|(_, &time)| time)
            .map(|(&id, _)| id);

        // 回溯关键路径
        let mut path = Vec::new();
        let mut current = end;
        while let Some(id) = current {
            path.push(id);
            current = predecessor.get(&id).copied().flatten();
        }

        path.reverse();
        path
    }

    /// 估计总构建时间 (串行)
    pub fn estimate_serial_time(&self) -> u64 {
        self.actions.values().map(|a| a.estimated_duration_ms).sum()
    }

    /// 估计总构建时间 (并行)
    pub fn estimate_parallel_time(&self, jobs: usize) -> u64 {
        // 简化估计：串行时间 / 并行度，但不低于关键路径时间
        let serial_time = self.estimate_serial_time();
        let critical_time: u64 = self
            .critical_path()
            .iter()
            .filter_map(|id| self.actions.get(id))
            .map(|a| a.estimated_duration_ms)
            .sum();

        (serial_time / jobs as u64).max(critical_time)
    }

    /// 打印图统计
    pub fn print_stats(&self) {
        println!("\n动作图统计:");
        println!("  总动作数: {}", self.actions.len());
        println!("  根动作数: {}", self.roots.len());
        println!("  叶动作数: {}", self.leaves.len());
        println!("  模块数: {}", self.by_module.len());

        // 按类型统计
        let mut by_type: HashMap<ActionType, usize> = HashMap::new();
        for action in self.actions.values() {
            *by_type.entry(action.action_type).or_default() += 1;
        }

        println!("  按类型:");
        for (action_type, count) in &by_type {
            println!("    {}: {}", action_type.name(), count);
        }

        // 估计时间
        let serial = self.estimate_serial_time();
        let parallel = self.estimate_parallel_time(num_cpus::get());
        println!("  估计串行时间: {:.1}s", serial as f64 / 1000.0);
        println!(
            "  估计并行时间 ({}核): {:.1}s",
            num_cpus::get(),
            parallel as f64 / 1000.0
        );
    }
}

//=============================================================================
// 动作缓存
//=============================================================================

/// 动作缓存条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ActionCacheEntry {
    /// 动作 ID
    pub action_id: ActionId,
    /// 内容哈希
    pub content_hash: u64,
    /// 输出文件哈希
    pub output_hashes: HashMap<PathBuf, u64>,
    /// 缓存时间
    pub cached_at: SystemTime,
    /// 构建耗时
    pub build_duration_ms: u64,
}

/// 动作缓存
#[derive(Debug, Default, Serialize, Deserialize)]
pub struct ActionCache {
    /// 缓存条目
    entries: HashMap<ActionId, ActionCacheEntry>,
    /// 缓存目录
    #[serde(skip)]
    cache_dir: PathBuf,
}

impl ActionCache {
    /// 创建缓存
    pub fn new(cache_dir: PathBuf) -> Self {
        Self {
            entries: HashMap::new(),
            cache_dir,
        }
    }

    /// 加载缓存
    pub fn load(cache_dir: &Path) -> Result<Self> {
        let cache_file = cache_dir.join("action_cache.json");

        if !cache_file.exists() {
            return Ok(Self::new(cache_dir.to_path_buf()));
        }

        let file = File::open(&cache_file).context("无法打开动作缓存文件")?;
        let reader = BufReader::new(file);
        let mut cache: ActionCache =
            serde_json::from_reader(reader).context("无法解析动作缓存 JSON")?;
        cache.cache_dir = cache_dir.to_path_buf();

        Ok(cache)
    }

    /// 保存缓存
    pub fn save(&self) -> Result<()> {
        fs::create_dir_all(&self.cache_dir)?;
        let cache_file = self.cache_dir.join("action_cache.json");

        let file = File::create(&cache_file).context("无法创建动作缓存文件")?;
        let writer = BufWriter::new(file);
        serde_json::to_writer_pretty(writer, self).context("无法序列化动作缓存")?;

        Ok(())
    }

    /// 检查缓存命中
    pub fn check_hit(&self, action: &BuildAction) -> Result<bool> {
        let entry = match self.entries.get(&action.id) {
            Some(e) => e,
            None => return Ok(false),
        };

        // 验证内容哈希
        let current_hash = action.content_hash()?;
        if current_hash != entry.content_hash {
            return Ok(false);
        }

        // 验证输出文件存在且哈希匹配
        for (output, &expected_hash) in &entry.output_hashes {
            if !output.exists() {
                return Ok(false);
            }
            // 验证输出文件哈希
            let current_hash = Self::compute_file_hash(output)?;
            if current_hash != expected_hash {
                return Ok(false);
            }
        }

        Ok(true)
    }

    /// 添加缓存条目
    pub fn add_entry(&mut self, action: &BuildAction, build_duration_ms: u64) -> Result<()> {
        let content_hash = action.content_hash()?;

        let mut output_hashes = HashMap::new();
        for output in &action.outputs {
            if output.exists() {
                let hash = Self::compute_file_hash(output)?;
                output_hashes.insert(output.clone(), hash);
            }
        }

        let entry = ActionCacheEntry {
            action_id: action.id,
            content_hash,
            output_hashes,
            cached_at: SystemTime::now(),
            build_duration_ms,
        };

        self.entries.insert(action.id, entry);
        Ok(())
    }

    /// 清除过期缓存
    pub fn clean_expired(&mut self, max_age: std::time::Duration) {
        let now = SystemTime::now();

        self.entries.retain(|_, entry| {
            entry
                .cached_at
                .elapsed()
                .map(|e| e < max_age)
                .unwrap_or(false)
        });
    }

    /// 获取缓存统计
    pub fn stats(&self) -> CacheStats {
        let total_size: u64 = self
            .entries
            .values()
            .flat_map(|e| e.output_hashes.keys())
            .filter_map(|p| fs::metadata(p).ok())
            .map(|m| m.len())
            .sum();

        CacheStats {
            total_entries: self.entries.len(),
            total_size_bytes: total_size,
        }
    }

    /// 计算文件哈希
    fn compute_file_hash(path: &Path) -> Result<u64> {
        let data = fs::read(path).with_context(|| format!("无法读取文件: {:?}", path))?;
        Ok(xxh3_64(&data))
    }
}

/// 缓存统计
#[derive(Debug, Clone)]
pub struct CacheStats {
    pub total_entries: usize,
    pub total_size_bytes: u64,
}
