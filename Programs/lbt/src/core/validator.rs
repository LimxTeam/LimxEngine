/*******************************************************************************
 * 文件: core/validator.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   模块配置验证器
 *   - 配置文件格式验证
 *   - 依赖有效性检查
 *   - 层级约束验证
 *   - 路径存在性检查
 *
 ******************************************************************************/

use std::collections::HashSet;

use super::config::{Module, ModuleType};

/// 验证结果
#[derive(Debug, Default)]
pub struct ValidationResult {
    pub errors: Vec<ValidationError>,
    pub warnings: Vec<ValidationWarning>,
}

impl ValidationResult {
    pub fn is_valid(&self) -> bool {
        self.errors.is_empty()
    }

    pub fn add_error(&mut self, error: ValidationError) {
        self.errors.push(error);
    }

    pub fn add_warning(&mut self, warning: ValidationWarning) {
        self.warnings.push(warning);
    }

    pub fn print_report(&self) {
        if !self.errors.is_empty() {
            println!("\n❌ 验证错误 ({}):", self.errors.len());
            for error in &self.errors {
                println!("  - {}", error);
            }
        }

        if !self.warnings.is_empty() {
            println!("\n⚠️  验证警告 ({}):", self.warnings.len());
            for warning in &self.warnings {
                println!("  - {}", warning);
            }
        }

        if self.is_valid() && self.warnings.is_empty() {
            println!("\n✅ 配置验证通过");
        }
    }
}

/// 验证错误
#[derive(Debug)]
pub enum ValidationError {
    /// 模块名无效
    InvalidModuleName { name: String, reason: String },
    /// 层级无效
    InvalidLayer { module: String, layer: u8 },
    /// 依赖不存在
    MissingDependency { module: String, dependency: String },
    /// 循环依赖
    CircularDependency { cycle: String },
    /// 层级约束违反
    LayerViolation {
        from: String,
        to: String,
        from_layer: u8,
        to_layer: u8,
    },
    /// 目录不存在
    DirectoryNotFound { module: String, path: String },
    /// 配置文件语法错误
    ConfigSyntaxError { module: String, error: String },
    /// 重复模块名
    DuplicateModule { name: String },
}

impl std::fmt::Display for ValidationError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::InvalidModuleName { name, reason } => {
                write!(f, "模块名 '{}' 无效: {}", name, reason)
            }
            Self::InvalidLayer { module, layer } => {
                write!(f, "模块 '{}' 层级 {} 无效 (有效范围: 0-5)", module, layer)
            }
            Self::MissingDependency { module, dependency } => {
                write!(f, "模块 '{}' 依赖的 '{}' 不存在", module, dependency)
            }
            Self::CircularDependency { cycle } => write!(f, "循环依赖: {}", cycle),
            Self::LayerViolation {
                from,
                to,
                from_layer,
                to_layer,
            } => write!(
                f,
                "层级违反: '{}' (L{}) 不能依赖 '{}' (L{})",
                from, from_layer, to, to_layer
            ),
            Self::DirectoryNotFound { module, path } => {
                write!(f, "模块 '{}' 目录不存在: {}", module, path)
            }
            Self::ConfigSyntaxError { module, error } => {
                write!(f, "模块 '{}' 配置语法错误: {}", module, error)
            }
            Self::DuplicateModule { name } => write!(f, "重复的模块名: '{}'", name),
        }
    }
}

/// 验证警告
///
/// 分两级, 由 is_advisory() 区分:
///
///   警告   —— 大概率是配置错误 (依赖为空、没有源文件、模块无人依赖)
///   建议   —— 可选的改进 (加个 PCH)
///
/// --strict 只对前者失败。不分级的话, 每一条"建议加个 PCH"都会变成硬性
/// 要求, 于是要么给每个模块都加上 PCH, 要么整个 --strict 不能用 —— 而
/// 后者正是 CI 与本地验证脚本此前的分歧所在。
#[derive(Debug)]
pub enum ValidationWarning {
    /// 空依赖列表
    EmptyDependencies { module: String },
    /// 未使用的模块
    UnusedModule { module: String },
    /// 缺少 PCH 文件
    MissingPch { module: String },
    /// 源文件为空
    NoSourceFiles { module: String },
    /// 建议优化
    Suggestion { module: String, message: String },
}

impl ValidationWarning {
    /// 是否只是建议 (--strict 不因它失败)
    pub fn is_advisory(&self) -> bool {
        matches!(self, Self::MissingPch { .. } | Self::Suggestion { .. })
    }
}

impl std::fmt::Display for ValidationWarning {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::EmptyDependencies { module } => {
                write!(f, "模块 '{}' 没有依赖，可能是独立模块", module)
            }
            Self::UnusedModule { module } => write!(f, "模块 '{}' 没有被其他模块依赖", module),
            Self::MissingPch { module } => write!(
                f,
                "模块 '{}' 没有预编译头文件 (建议添加 {}PCH.h)",
                module, module
            ),
            Self::NoSourceFiles { module } => write!(f, "模块 '{}' 没有源文件", module),
            Self::Suggestion { module, message } => write!(f, "[{}] {}", module, message),
        }
    }
}

/// 模块验证器
pub struct ModuleValidator {
    known_modules: HashSet<String>,
}

impl ModuleValidator {
    pub fn new() -> Self {
        Self {
            known_modules: HashSet::new(),
        }
    }

    /// 验证所有模块
    pub fn validate_all(&mut self, modules: &[Module]) -> ValidationResult {
        let mut result = ValidationResult::default();

        // 收集所有模块名
        for module in modules {
            if !self.known_modules.insert(module.name.clone()) {
                result.add_error(ValidationError::DuplicateModule {
                    name: module.name.clone(),
                });
            }
        }

        // 验证每个模块
        for module in modules {
            self.validate_module(module, &mut result);
        }

        // 检查未使用的模块
        self.check_unused_modules(modules, &mut result);

        result
    }

    /// 验证单个模块
    fn validate_module(&self, module: &Module, result: &mut ValidationResult) {
        // 验证模块名
        self.validate_module_name(&module.name, result);

        // 验证层级
        if module.layer > 5 {
            result.add_error(ValidationError::InvalidLayer {
                module: module.name.clone(),
                layer: module.layer,
            });
        }

        // 验证目录存在
        if !module.path.exists() {
            result.add_error(ValidationError::DirectoryNotFound {
                module: module.name.clone(),
                path: module.path.display().to_string(),
            });
        }

        // 验证依赖存在
        for dep in &module.config.dependencies.public {
            if !self.known_modules.contains(dep) {
                result.add_error(ValidationError::MissingDependency {
                    module: module.name.clone(),
                    dependency: dep.clone(),
                });
            }
        }

        for dep in &module.config.dependencies.private {
            if !self.known_modules.contains(dep) {
                result.add_error(ValidationError::MissingDependency {
                    module: module.name.clone(),
                    dependency: dep.clone(),
                });
            }
        }

        // 检查 PCH 文件
        let pch_path = module
            .path
            .join("Private")
            .join(format!("{}PCH.h", module.name));
        if !pch_path.exists() && module.module_type != ModuleType::HeaderOnly {
            result.add_warning(ValidationWarning::MissingPch {
                module: module.name.clone(),
            });
        }

        // 检查源文件
        let private_dir = module.path.join("Private");
        if private_dir.exists() {
            let has_sources = std::fs::read_dir(&private_dir)
                .map(|entries| {
                    entries.filter_map(|e| e.ok()).any(|e| {
                        e.path()
                            .extension()
                            .map(|ext| ext == "cpp" || ext == "c")
                            .unwrap_or(false)
                    })
                })
                .unwrap_or(false);

            if !has_sources && module.module_type != ModuleType::HeaderOnly {
                result.add_warning(ValidationWarning::NoSourceFiles {
                    module: module.name.clone(),
                });
            }
        }
    }

    /// 验证模块名
    fn validate_module_name(&self, name: &str, result: &mut ValidationResult) {
        if name.is_empty() {
            result.add_error(ValidationError::InvalidModuleName {
                name: name.to_string(),
                reason: "名称不能为空".to_string(),
            });
            return;
        }

        if !name.starts_with(|c: char| c.is_alphabetic()) {
            result.add_error(ValidationError::InvalidModuleName {
                name: name.to_string(),
                reason: "必须以字母开头".to_string(),
            });
        }

        if name.contains(' ') {
            result.add_error(ValidationError::InvalidModuleName {
                name: name.to_string(),
                reason: "不能包含空格".to_string(),
            });
        }
    }

    /// 检查未使用的模块
    fn check_unused_modules(&self, modules: &[Module], result: &mut ValidationResult) {
        let mut used_modules: HashSet<String> = HashSet::new();

        for module in modules {
            for dep in &module.config.dependencies.public {
                used_modules.insert(dep.clone());
            }
            for dep in &module.config.dependencies.private {
                used_modules.insert(dep.clone());
            }
        }

        for module in modules {
            // 可执行文件按定义就是叶子 —— 没有任何东西会依赖一个 exe。
            //
            // 这里原先不分模块类型, 于是每个测试程序和 Launch 都被报成
            // "未使用"。在 --strict 下这直接让 CI 的模块校验步骤失败, 而
            // 那五条警告一条都不是真问题。
            if matches!(module.config.module.r#type, ModuleType::Executable) {
                continue;
            }

            // 层级 0 的模块作为基础模块，不警告
            if module.layer > 0 && !used_modules.contains(&module.name) {
                result.add_warning(ValidationWarning::UnusedModule {
                    module: module.name.clone(),
                });
            }
        }
    }
}

impl Default for ModuleValidator {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_validation_result_is_valid() {
        let result = ValidationResult::default();
        assert!(result.is_valid());
    }

    #[test]
    fn test_validation_error_display() {
        let error = ValidationError::MissingDependency {
            module: "A".to_string(),
            dependency: "B".to_string(),
        };
        assert!(error.to_string().contains("A"));
        assert!(error.to_string().contains("B"));
    }
}
