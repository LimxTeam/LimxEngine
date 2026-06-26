/*******************************************************************************
 * 文件: hotreload/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   热重载系统 - 支持运行时代码替换
 *   - 文件监控与变更检测
 *   - 增量编译触发
 *   - DLL 热替换
 *   - 状态保存与恢复
 *
 * 技术特性:
 *   - 基于 DLL 的热重载机制
 *   - 函数级别热替换
 *   - 对象状态序列化保存
 *   - 自动重新绑定
 *
 ******************************************************************************/

pub mod compiler;
pub mod loader;
pub mod state;
pub mod watcher;

pub use compiler::*;
pub use loader::*;
pub use state::*;
pub use watcher::*;

use anyhow::Result;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

/// 热重载配置
#[derive(Debug, Clone)]
pub struct HotReloadConfig {
    /// 源代码目录
    pub source_dirs: Vec<PathBuf>,
    /// 输出目录
    pub output_dir: PathBuf,
    /// 监控文件扩展名
    pub watch_extensions: Vec<String>,
    /// 编译命令
    pub compile_command: Option<String>,
    /// 是否启用状态保存
    pub preserve_state: bool,
    /// 重载延迟 (ms)
    pub debounce_ms: u64,
    /// 是否启用增量编译
    pub incremental: bool,
}

impl Default for HotReloadConfig {
    fn default() -> Self {
        Self {
            source_dirs: vec![],
            output_dir: PathBuf::from("Build/HotReload"),
            watch_extensions: vec![
                "h".to_string(),
                "hpp".to_string(),
                "cpp".to_string(),
                "c".to_string(),
            ],
            compile_command: None,
            preserve_state: true,
            debounce_ms: 500,
            incremental: true,
        }
    }
}

/// 热重载事件
#[derive(Debug, Clone)]
pub enum HotReloadEvent {
    /// 文件变更检测到
    FileChanged(PathBuf),
    /// 开始编译
    CompileStarted,
    /// 编译完成
    CompileFinished { success: bool, duration_ms: u64 },
    /// 开始重载
    ReloadStarted,
    /// 重载完成
    ReloadFinished { success: bool, modules: Vec<String> },
    /// 状态保存
    StateSaved { object_count: usize },
    /// 状态恢复
    StateRestored { object_count: usize },
    /// 错误
    Error(String),
}

/// 热重载回调
pub type HotReloadCallback = Box<dyn Fn(HotReloadEvent) + Send + Sync>;

/// 模块信息
#[derive(Debug, Clone)]
pub struct ModuleInfo {
    /// 模块名称
    pub name: String,
    /// DLL 路径
    pub dll_path: PathBuf,
    /// 版本号
    pub version: u32,
    /// 导出的类列表
    pub exported_classes: Vec<String>,
    /// 导出的函数列表
    pub exported_functions: Vec<String>,
    /// 加载时间
    pub load_time: u64,
}

/// 热重载管理器
pub struct HotReloadManager {
    /// 配置
    config: HotReloadConfig,
    /// 已加载模块
    modules: Arc<RwLock<HashMap<String, ModuleInfo>>>,
    /// 文件监控器
    watcher: Option<FileWatcher>,
    /// 状态管理器
    state_manager: StateManager,
    /// 事件回调
    callbacks: Vec<HotReloadCallback>,
    /// 是否运行中
    running: Arc<RwLock<bool>>,
    /// 备份 DLL (用于错误回滚)
    backup_dlls: HashMap<String, PathBuf>,
    /// 上次成功的状态快照
    last_good_snapshot: Option<Vec<u8>>,
}

impl HotReloadManager {
    /// 创建新的热重载管理器
    pub fn new(config: HotReloadConfig) -> Self {
        Self {
            config,
            modules: Arc::new(RwLock::new(HashMap::new())),
            watcher: None,
            state_manager: StateManager::new(),
            callbacks: Vec::new(),
            running: Arc::new(RwLock::new(false)),
            backup_dlls: HashMap::new(),
            last_good_snapshot: None,
        }
    }

    /// 注册事件回调
    pub fn on_event(&mut self, callback: HotReloadCallback) {
        self.callbacks.push(callback);
    }

    /// 发送事件
    fn emit_event(&self, event: HotReloadEvent) {
        for callback in &self.callbacks {
            callback(event.clone());
        }
    }

    /// 启动热重载
    pub fn start(&mut self) -> Result<()> {
        if let Ok(mut running) = self.running.write() {
            *running = true;
        }

        // 创建文件监控器
        let watcher = FileWatcher::new(
            self.config.source_dirs.clone(),
            self.config.watch_extensions.clone(),
            self.config.debounce_ms,
        )?;

        self.watcher = Some(watcher);

        Ok(())
    }

    /// 停止热重载
    pub fn stop(&mut self) {
        if let Ok(mut running) = self.running.write() {
            *running = false;
        }
        self.watcher = None;
    }

    /// 检查是否有变更
    pub fn poll(&mut self) -> Option<Vec<PathBuf>> {
        if let Some(ref mut watcher) = self.watcher {
            watcher.poll_changes()
        } else {
            None
        }
    }

    /// 执行热重载 (带错误回滚)
    pub fn reload(&mut self, changed_files: &[PathBuf]) -> Result<()> {
        self.emit_event(HotReloadEvent::ReloadStarted);

        // 1. 备份当前 DLL (用于回滚)
        self.backup_current_dlls()?;

        // 2. 保存状态
        if self.config.preserve_state {
            let count = self.state_manager.save_all()?;
            self.emit_event(HotReloadEvent::StateSaved {
                object_count: count,
            });
        }

        // 3. 编译变更的模块
        self.emit_event(HotReloadEvent::CompileStarted);
        let start = std::time::Instant::now();

        let compile_result = self.compile_modules(changed_files);
        let duration_ms = start.elapsed().as_millis() as u64;

        self.emit_event(HotReloadEvent::CompileFinished {
            success: compile_result.is_ok(),
            duration_ms,
        });

        if let Err(e) = compile_result {
            self.emit_event(HotReloadEvent::Error(format!("编译失败: {}", e)));
            // 编译失败不需要回滚，因为还没有加载新 DLL
            return Err(e);
        }

        // 4. 重新加载模块
        let reload_result = self.reload_modules(changed_files);

        match reload_result {
            Ok(reloaded_modules) => {
                // 5. 恢复状态
                if self.config.preserve_state {
                    let count = self.state_manager.restore_all()?;
                    self.emit_event(HotReloadEvent::StateRestored {
                        object_count: count,
                    });
                }

                // 清理备份
                self.cleanup_backups();

                self.emit_event(HotReloadEvent::ReloadFinished {
                    success: true,
                    modules: reloaded_modules,
                });

                Ok(())
            }
            Err(e) => {
                // 加载失败，执行回滚
                self.emit_event(HotReloadEvent::Error(format!("加载失败，正在回滚: {}", e)));
                self.rollback()?;
                Err(e)
            }
        }
    }

    /// 备份当前 DLL
    fn backup_current_dlls(&mut self) -> Result<()> {
        let backup_dir = self.config.output_dir.join("backup");
        std::fs::create_dir_all(&backup_dir)?;

        let Ok(modules) = self.modules.read() else {
            return Ok(());
        };
        for (name, info) in modules.iter() {
            if info.dll_path.exists() {
                let backup_path = backup_dir.join(format!("{}_backup.dll", name));
                std::fs::copy(&info.dll_path, &backup_path)?;
                self.backup_dlls.insert(name.clone(), backup_path);
            }
        }

        Ok(())
    }

    /// 回滚到备份
    fn rollback(&mut self) -> Result<()> {
        for (name, backup_path) in &self.backup_dlls {
            if backup_path.exists() {
                let original_path = self.config.output_dir.join(format!("{}.dll", name));
                std::fs::copy(backup_path, &original_path)?;
            }
        }
        self.cleanup_backups();
        Ok(())
    }

    /// 清理备份文件
    fn cleanup_backups(&mut self) {
        for (_, backup_path) in self.backup_dlls.drain() {
            let _ = std::fs::remove_file(backup_path);
        }
    }

    /// 编译模块
    fn compile_modules(&self, changed_files: &[PathBuf]) -> Result<()> {
        let compiler = IncrementalCompiler::new(&self.config);
        compiler.compile(changed_files)
    }

    /// 重新加载模块
    fn reload_modules(&mut self, changed_files: &[PathBuf]) -> Result<Vec<String>> {
        let mut reloaded = Vec::new();
        let mut loader = ModuleLoader::new(&self.config.output_dir);

        // 确定需要重载的模块
        let modules_to_reload = self.determine_affected_modules(changed_files);

        for module_name in modules_to_reload {
            // 卸载旧模块
            if let Ok(mut modules) = self.modules.write() {
                if let Some(old_module) = modules.remove(&module_name) {
                    loader.unload(&old_module)?;
                }
            }

            // 加载新模块
            let new_module = loader.load(&module_name)?;
            if let Ok(mut modules) = self.modules.write() {
                modules.insert(module_name.clone(), new_module);
            }
            reloaded.push(module_name);
        }

        Ok(reloaded)
    }

    /// 确定受影响的模块
    fn determine_affected_modules(&self, changed_files: &[PathBuf]) -> Vec<String> {
        let mut affected = Vec::new();

        for file in changed_files {
            if let Some(module_name) = self.file_to_module(file) {
                if !affected.contains(&module_name) {
                    affected.push(module_name);
                }
            }
        }

        affected
    }

    /// 文件路径转模块名
    fn file_to_module(&self, file: &Path) -> Option<String> {
        // 从路径中提取模块名
        for source_dir in &self.config.source_dirs {
            if file.starts_with(source_dir) {
                if let Ok(relative) = file.strip_prefix(source_dir) {
                    // 取第一级目录作为模块名
                    if let Some(first) = relative.components().next() {
                        return Some(first.as_os_str().to_string_lossy().to_string());
                    }
                }
            }
        }
        None
    }

    /// 获取已加载模块列表
    pub fn get_loaded_modules(&self) -> Vec<ModuleInfo> {
        self.modules
            .read()
            .ok()
            .map(|m| m.values().cloned().collect())
            .unwrap_or_default()
    }

    /// 检查模块是否已加载
    pub fn is_module_loaded(&self, name: &str) -> bool {
        self.modules
            .read()
            .ok()
            .map(|m| m.contains_key(name))
            .unwrap_or(false)
    }
}
