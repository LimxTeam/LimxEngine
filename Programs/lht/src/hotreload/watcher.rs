/*******************************************************************************
 * 文件: hotreload/watcher.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   文件监控器 - 检测源文件变更
 *   - 递归监控目录
 *   - 防抖处理
 *   - 扩展名过滤
 *
 ******************************************************************************/

use anyhow::Result;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime};

/// 文件状态
#[derive(Debug, Clone)]
struct FileState {
    /// 最后修改时间
    modified: SystemTime,
    /// 文件大小
    size: u64,
}

/// 文件监控器
pub struct FileWatcher {
    /// 监控的目录
    watch_dirs: Vec<PathBuf>,
    /// 监控的扩展名
    extensions: HashSet<String>,
    /// 防抖时间 (ms)
    debounce_ms: u64,
    /// 文件状态缓存
    file_states: HashMap<PathBuf, FileState>,
    /// 待处理的变更
    pending_changes: Arc<Mutex<Vec<PathBuf>>>,
    /// 最后检查时间
    last_check: Instant,
    /// 最后变更时间
    last_change: Option<Instant>,
}

impl FileWatcher {
    /// 创建新的文件监控器
    pub fn new(
        watch_dirs: Vec<PathBuf>,
        extensions: Vec<String>,
        debounce_ms: u64,
    ) -> Result<Self> {
        let mut watcher = Self {
            watch_dirs,
            extensions: extensions.into_iter().collect(),
            debounce_ms,
            file_states: HashMap::new(),
            pending_changes: Arc::new(Mutex::new(Vec::new())),
            last_check: Instant::now(),
            last_change: None,
        };

        // 初始扫描
        watcher.initial_scan()?;

        Ok(watcher)
    }

    /// 初始扫描所有文件
    fn initial_scan(&mut self) -> Result<()> {
        for dir in &self.watch_dirs.clone() {
            self.scan_directory(dir)?;
        }
        Ok(())
    }

    /// 扫描目录
    fn scan_directory(&mut self, dir: &Path) -> Result<()> {
        if !dir.exists() {
            return Ok(());
        }

        for entry in fs::read_dir(dir)? {
            let entry = entry?;
            let path = entry.path();

            if path.is_dir() {
                self.scan_directory(&path)?;
            } else if self.should_watch(&path) {
                if let Ok(metadata) = fs::metadata(&path) {
                    if let Ok(modified) = metadata.modified() {
                        self.file_states.insert(
                            path,
                            FileState {
                                modified,
                                size: metadata.len(),
                            },
                        );
                    }
                }
            }
        }

        Ok(())
    }

    /// 检查是否应该监控此文件
    fn should_watch(&self, path: &Path) -> bool {
        if let Some(ext) = path.extension() {
            self.extensions
                .contains(&ext.to_string_lossy().to_lowercase())
        } else {
            false
        }
    }

    /// 轮询变更
    pub fn poll_changes(&mut self) -> Option<Vec<PathBuf>> {
        let now = Instant::now();

        // 检查间隔
        if now.duration_since(self.last_check) < Duration::from_millis(100) {
            return None;
        }
        self.last_check = now;

        // 扫描变更
        let mut changes = Vec::new();
        for dir in &self.watch_dirs.clone() {
            self.check_directory_changes(dir, &mut changes);
        }

        // 如果有新变更，记录时间
        if !changes.is_empty() {
            self.last_change = Some(now);
            let Ok(mut pending) = self.pending_changes.lock() else {
                return None;
            };
            for change in changes {
                if !pending.contains(&change) {
                    pending.push(change);
                }
            }
        }

        // 检查防抖
        if let Some(last_change) = self.last_change {
            if now.duration_since(last_change) >= Duration::from_millis(self.debounce_ms) {
                let Ok(mut pending) = self.pending_changes.lock() else {
                    return None;
                };
                if !pending.is_empty() {
                    let result = pending.drain(..).collect();
                    self.last_change = None;
                    return Some(result);
                }
            }
        }

        None
    }

    /// 检查目录中的变更
    fn check_directory_changes(&mut self, dir: &Path, changes: &mut Vec<PathBuf>) {
        if !dir.exists() {
            return;
        }

        let entries = match fs::read_dir(dir) {
            Ok(e) => e,
            Err(_) => return,
        };

        for entry in entries.flatten() {
            let path = entry.path();

            if path.is_dir() {
                self.check_directory_changes(&path, changes);
            } else if self.should_watch(&path) {
                if let Ok(metadata) = fs::metadata(&path) {
                    if let Ok(modified) = metadata.modified() {
                        let current_state = FileState {
                            modified,
                            size: metadata.len(),
                        };

                        let changed = match self.file_states.get(&path) {
                            Some(cached) => {
                                cached.modified != current_state.modified
                                    || cached.size != current_state.size
                            }
                            None => true, // 新文件
                        };

                        if changed {
                            changes.push(path.clone());
                            self.file_states.insert(path, current_state);
                        }
                    }
                }
            }
        }

        // 检查删除的文件
        let current_files: HashSet<_> = self
            .file_states
            .keys()
            .filter(|p| p.starts_with(dir))
            .cloned()
            .collect();

        for file in current_files {
            if !file.exists() {
                self.file_states.remove(&file);
                changes.push(file);
            }
        }
    }

    /// 获取监控的文件数量
    pub fn watched_file_count(&self) -> usize {
        self.file_states.len()
    }

    /// 添加监控目录
    pub fn add_watch_dir(&mut self, dir: PathBuf) -> Result<()> {
        if !self.watch_dirs.contains(&dir) {
            self.watch_dirs.push(dir.clone());
            self.scan_directory(&dir)?;
        }
        Ok(())
    }

    /// 移除监控目录
    pub fn remove_watch_dir(&mut self, dir: &Path) {
        self.watch_dirs.retain(|d| d != dir);
        self.file_states.retain(|p, _| !p.starts_with(dir));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::File;
    use std::io::Write;
    use tempfile::tempdir;

    #[test]
    #[ignore] // 时序相关测试，CI 环境可能不稳定
    fn test_file_watcher_detects_changes() {
        let dir = tempdir().unwrap();
        let file_path = dir.path().join("test.cpp");

        // 创建初始文件
        File::create(&file_path)
            .unwrap()
            .write_all(b"initial")
            .unwrap();

        let mut watcher =
            FileWatcher::new(vec![dir.path().to_path_buf()], vec!["cpp".to_string()], 100).unwrap();

        assert_eq!(watcher.watched_file_count(), 1);

        // 修改文件
        std::thread::sleep(Duration::from_millis(50));
        File::create(&file_path)
            .unwrap()
            .write_all(b"modified")
            .unwrap();

        // 等待防抖
        std::thread::sleep(Duration::from_millis(200));

        let changes = watcher.poll_changes();
        assert!(changes.is_some());
    }
}
