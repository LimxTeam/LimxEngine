/*******************************************************************************
 * 文件: core/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LBT 核心模块
 *   - 模块配置解析
 *   - 模块发现
 *   - 依赖解析
 *   - 增量编译缓存
 *
 ******************************************************************************/

pub mod build_config;
pub mod build_profiler;
pub mod cache;
pub mod config;
pub mod dependency;
pub mod discovery;
pub mod error;
pub mod module_health;
pub mod plugin;
pub mod target;
pub mod timing;
pub mod validator;

#[cfg(test)]
mod tests;

pub use build_config::BuildConfig;
pub use build_profiler::{
    BuildProfiler, FileProfileEntry, LinkProfileEntry, LinkType, ProfileReport,
};
pub use cache::BuildCache;
pub use plugin::{
    discover_plugins, generate_example_plugin, load_plugin, PluginConfig, PluginRegistry,
    ResolvedPlugin,
};
pub use target::{
    discover_targets, generate_example_target, load_target, Target, TargetConfig, TargetRegistry,
    TargetType,
};
pub use timing::{BuildPhase, PerformanceMonitor};
pub use validator::ModuleValidator;
