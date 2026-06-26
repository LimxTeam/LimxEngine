/*******************************************************************************
 * 文件: core/tests.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LBT 核心模块单元测试
 *
 ******************************************************************************/

#[cfg(test)]
mod tests {
    use super::super::*;
    use std::path::PathBuf;

    // =========================================================================
    // 模块配置解析测试
    // =========================================================================

    #[test]
    fn test_module_config_parse() {
        let toml_content = r#"
[module]
name = "TestModule"
type = "shared"
layer = 1

[dependencies]
public = ["LimxCore"]
private = []

[compile]
defines = ["TEST_DEFINE"]
"#;
        let config: config::ModuleConfig = toml::from_str(toml_content).unwrap();
        assert_eq!(config.module.name, "TestModule");
        assert_eq!(config.module.layer, 1);
        assert!(config.dependencies.public.contains(&"LimxCore".to_string()));
    }

    // =========================================================================
    // 增量编译缓存测试
    // =========================================================================

    #[test]
    fn test_cache_new() {
        let cache = cache::BuildCache::new();
        assert!(cache.modules.is_empty());
    }

    #[test]
    fn test_incremental_result_default() {
        let result = cache::IncrementalResult::default();
        assert!(result.dirty_modules.is_empty());
        assert!(result.clean_modules.is_empty());
        assert!(result.new_modules.is_empty());
        assert!(result.removed_modules.is_empty());
    }

    // =========================================================================
    // 依赖图测试
    // =========================================================================

    #[test]
    fn test_dependency_stats() {
        let stats = dependency::DependencyStats::default();
        assert_eq!(stats.total_modules, 0);
        assert_eq!(stats.total_edges, 0);
        assert_eq!(stats.max_depth, 0);
    }

    // =========================================================================
    // 模块类型测试
    // =========================================================================

    #[test]
    fn test_module_type_variants() {
        assert!(matches!(
            config::ModuleType::Static,
            config::ModuleType::Static
        ));
        assert!(matches!(
            config::ModuleType::Shared,
            config::ModuleType::Shared
        ));
        assert!(matches!(
            config::ModuleType::Executable,
            config::ModuleType::Executable
        ));
        assert!(matches!(
            config::ModuleType::HeaderOnly,
            config::ModuleType::HeaderOnly
        ));
    }

    #[test]
    fn test_module_config_defaults() {
        let toml_content = r#"
[module]
name = "MinimalModule"
type = "static"
layer = 0
"#;
        let config: config::ModuleConfig = toml::from_str(toml_content).unwrap();
        assert_eq!(config.module.name, "MinimalModule");
        assert!(config.dependencies.public.is_empty());
        assert!(config.dependencies.private.is_empty());
    }

    #[test]
    fn test_compile_options_parse() {
        let toml_content = r#"
[module]
name = "TestModule"
type = "shared"
layer = 1

[compile]
defines = ["DEBUG", "LIMX_EXPORT"]
warnings_as_errors = true
"#;
        let config: config::ModuleConfig = toml::from_str(toml_content).unwrap();
        assert!(config.compile.defines.contains(&"DEBUG".to_string()));
        assert!(config.compile.defines.contains(&"LIMX_EXPORT".to_string()));
    }

    // =========================================================================
    // 缓存测试扩展
    // =========================================================================

    #[test]
    fn test_cache_entry_creation() {
        let entry = cache::CacheEntry {
            path: std::path::PathBuf::from("test.cpp"),
            modified_time: 12345,
            file_size: 1024,
            content_hash: Some("abc123".to_string()),
        };
        assert_eq!(entry.file_size, 1024);
        assert!(entry.content_hash.is_some());
    }

    #[test]
    fn test_module_cache_default() {
        let mc = cache::ModuleCache::default();
        assert!(mc.name.is_empty());
        assert!(mc.source_entries.is_empty());
        assert!(mc.header_entries.is_empty());
    }

    #[test]
    fn test_cache_stats_new() {
        let stats = cache::CacheStats::new();
        assert_eq!(
            stats
                .files_checked
                .load(std::sync::atomic::Ordering::Relaxed),
            0
        );
        assert_eq!(
            stats.cache_hits.load(std::sync::atomic::Ordering::Relaxed),
            0
        );
    }

    // =========================================================================
    // 依赖图测试扩展
    // =========================================================================

    #[test]
    fn test_dependency_stats_values() {
        let stats = dependency::DependencyStats {
            total_modules: 5,
            total_edges: 8,
            max_depth: 3,
            avg_dependencies: 1.6,
            modules_by_layer: std::collections::BTreeMap::new(),
        };
        assert_eq!(stats.total_modules, 5);
        assert_eq!(stats.total_edges, 8);
        assert_eq!(stats.max_depth, 3);
    }

    // =========================================================================
    // 性能监控测试
    // =========================================================================

    #[test]
    fn test_performance_monitor_new() {
        let monitor = timing::PerformanceMonitor::new();
        assert!(monitor.total_duration() < std::time::Duration::from_secs(1));
    }

    #[test]
    fn test_build_phase_name() {
        assert_eq!(timing::BuildPhase::Discovery.name(), "模块发现");
        assert_eq!(timing::BuildPhase::DependencyResolution.name(), "依赖解析");
        assert_eq!(timing::BuildPhase::CmakeGeneration.name(), "CMake 生成");
    }

    // =========================================================================
    // 构建配置测试
    // =========================================================================

    #[test]
    fn test_build_config_default() {
        let config = build_config::BuildConfig::default();
        assert_eq!(config.project_name, "LimxEngine");
        assert!(config.compiler.parallel_compile);
    }

    #[test]
    fn test_optimization_level_cmake() {
        assert_eq!(
            build_config::OptimizationLevel::Debug.as_cmake_type(),
            "Debug"
        );
        assert_eq!(
            build_config::OptimizationLevel::Release.as_cmake_type(),
            "Release"
        );
    }

    #[test]
    fn test_compiler_config_cpp23() {
        let config = build_config::CompilerConfig::default();
        assert_eq!(config.cpp_standard, "c++23");
        assert_eq!(config.warning_level, 4);
    }

    // =========================================================================
    // 验证器测试
    // =========================================================================

    #[test]
    fn test_validator_new() {
        let _validator = validator::ModuleValidator::new();
        // ModuleValidator 使用内部状态，验证创建成功即可
    }

    #[test]
    fn test_validation_result_empty() {
        let result = validator::ValidationResult {
            errors: vec![],
            warnings: vec![],
        };
        assert!(result.is_valid());
    }

    #[test]
    fn test_validation_result_with_error() {
        let result = validator::ValidationResult {
            errors: vec![validator::ValidationError::DuplicateModule {
                name: "TestModule".to_string(),
            }],
            warnings: vec![],
        };
        assert!(!result.is_valid());
    }

    // =========================================================================
    // 错误类型测试
    // =========================================================================

    #[test]
    fn test_lbt_error_code() {
        let err = error::LbtError::ModuleNotFound {
            name: "Test".to_string(),
        };
        assert_eq!(err.error_code(), "E010");
    }

    #[test]
    fn test_lbt_error_is_config() {
        let err = error::LbtError::ConfigValidationError {
            message: "test".to_string(),
        };
        assert!(err.is_config_error());
    }
}
