/*******************************************************************************
 * 文件: variant/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LSC 着色器变体系统
 *   - 支持宏定义组合生成多个着色器变体
 *   - 支持条件编译
 *   - 支持变体索引和查找
 *   - 并行变体编译
 *
 * 设计哲学:
 *   1. 声明式变体定义
 *   2. 最小化变体数量 (避免组合爆炸)
 *   3. 高效的变体查找
 *
 * 技术特性:
 *   - 支持 #pragma variant 指令解析
 *   - 支持变体组 (互斥选项)
 *   - 支持变体依赖关系
 *   - 变体哈希缓存
 *
 ******************************************************************************/

use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;

//=============================================================================
// 变体定义
//=============================================================================

/// 着色器变体维度
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VariantDimension {
    /// 维度名称 (如 "QUALITY", "LIGHTING_MODEL")
    pub name: String,
    /// 可选值列表
    pub values: Vec<String>,
    /// 是否为布尔开关 (只有启用/禁用)
    pub is_boolean: bool,
    /// 默认值索引
    pub default_index: usize,
}

impl VariantDimension {
    /// 创建布尔变体维度
    pub fn boolean(name: &str) -> Self {
        Self {
            name: name.to_string(),
            values: vec!["0".to_string(), "1".to_string()],
            is_boolean: true,
            default_index: 0,
        }
    }

    /// 创建多值变体维度
    pub fn multi(name: &str, values: Vec<&str>) -> Self {
        Self {
            name: name.to_string(),
            values: values.into_iter().map(|s| s.to_string()).collect(),
            is_boolean: false,
            default_index: 0,
        }
    }

    /// 设置默认值
    pub fn with_default(mut self, index: usize) -> Self {
        self.default_index = index.min(self.values.len().saturating_sub(1));
        self
    }

    /// 获取值数量
    pub fn value_count(&self) -> usize {
        self.values.len()
    }
}

/// 变体组合
#[derive(Debug, Clone, Hash, PartialEq, Eq, Serialize, Deserialize)]
pub struct VariantKey {
    /// 各维度的值索引
    pub indices: Vec<usize>,
}

impl VariantKey {
    /// 创建新的变体键
    pub fn new(indices: Vec<usize>) -> Self {
        Self { indices }
    }

    /// 获取变体的唯一哈希ID
    pub fn hash_id(&self) -> u64 {
        use std::collections::hash_map::DefaultHasher;
        use std::hash::{Hash, Hasher};
        let mut hasher = DefaultHasher::new();
        self.hash(&mut hasher);
        hasher.finish()
    }
}

/// 变体配置
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VariantConfig {
    /// 变体维度列表
    pub dimensions: Vec<VariantDimension>,
    /// 排除的变体组合 (用于减少无效组合)
    pub exclusions: Vec<VariantExclusion>,
    /// 是否生成所有组合
    pub generate_all: bool,
    /// 最大变体数量限制
    pub max_variants: usize,
}

impl Default for VariantConfig {
    fn default() -> Self {
        Self {
            dimensions: Vec::new(),
            exclusions: Vec::new(),
            generate_all: true,
            max_variants: 1024,
        }
    }
}

/// 变体排除规则
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VariantExclusion {
    /// 条件 (维度名 -> 值)
    pub conditions: HashMap<String, String>,
}

impl VariantConfig {
    /// 添加布尔变体维度
    pub fn add_boolean(&mut self, name: &str) -> &mut Self {
        self.dimensions.push(VariantDimension::boolean(name));
        self
    }

    /// 添加多值变体维度
    pub fn add_dimension(&mut self, name: &str, values: Vec<&str>) -> &mut Self {
        self.dimensions.push(VariantDimension::multi(name, values));
        self
    }

    /// 添加排除规则
    pub fn exclude(&mut self, conditions: HashMap<String, String>) -> &mut Self {
        self.exclusions.push(VariantExclusion { conditions });
        self
    }

    /// 计算总变体数量 (不考虑排除)
    pub fn total_combinations(&self) -> usize {
        if self.dimensions.is_empty() {
            return 1;
        }
        self.dimensions.iter().map(|d| d.value_count()).product()
    }

    /// 生成所有有效变体键
    pub fn generate_variants(&self) -> Vec<VariantKey> {
        if self.dimensions.is_empty() {
            return vec![VariantKey::new(vec![])];
        }

        let mut variants = Vec::new();
        let mut indices = vec![0usize; self.dimensions.len()];

        loop {
            // 检查是否被排除
            if !self.is_excluded(&indices) {
                variants.push(VariantKey::new(indices.clone()));

                // 检查是否超过限制
                if variants.len() >= self.max_variants {
                    break;
                }
            }

            // 递增索引
            let mut carry = true;
            for (i, idx) in indices.iter_mut().enumerate() {
                if carry {
                    *idx += 1;
                    if *idx >= self.dimensions[i].value_count() {
                        *idx = 0;
                    } else {
                        carry = false;
                    }
                }
            }

            // 所有组合已遍历
            if carry {
                break;
            }
        }

        variants
    }

    /// 检查变体是否被排除
    fn is_excluded(&self, indices: &[usize]) -> bool {
        for exclusion in &self.exclusions {
            let mut all_match = true;
            for (dim_name, required_value) in &exclusion.conditions {
                if let Some(dim_idx) = self.dimensions.iter().position(|d| &d.name == dim_name) {
                    let current_value = &self.dimensions[dim_idx].values[indices[dim_idx]];
                    if current_value != required_value {
                        all_match = false;
                        break;
                    }
                }
            }
            if all_match {
                return true;
            }
        }
        false
    }

    /// 根据变体键生成宏定义
    pub fn generate_defines(&self, key: &VariantKey) -> Vec<(String, Option<String>)> {
        let mut defines = Vec::new();

        for (i, dim) in self.dimensions.iter().enumerate() {
            let value_idx = key.indices.get(i).copied().unwrap_or(dim.default_index);
            let value = &dim.values[value_idx];

            if dim.is_boolean {
                // 布尔变体: 只有值为 "1" 时定义
                if value == "1" {
                    defines.push((dim.name.clone(), None));
                }
            } else {
                // 多值变体: 定义为具体值
                defines.push((dim.name.clone(), Some(value.clone())));
            }
        }

        defines
    }
}

//=============================================================================
// 变体解析器
//=============================================================================

/// 从着色器源码解析变体配置
pub struct VariantParser;

impl VariantParser {
    /// 解析着色器文件中的变体指令
    pub fn parse(source: &str) -> VariantConfig {
        let mut config = VariantConfig::default();

        for line in source.lines() {
            let line = line.trim();

            // #pragma variant FEATURE_NAME
            if let Some(rest) = line.strip_prefix("#pragma variant ") {
                let name = rest.trim();
                if !name.is_empty() && !config.dimensions.iter().any(|d| d.name == name) {
                    config.add_boolean(name);
                }
            }

            // #pragma variant_multi QUALITY LOW MEDIUM HIGH
            if let Some(rest) = line.strip_prefix("#pragma variant_multi ") {
                let parts: Vec<&str> = rest.split_whitespace().collect();

                if parts.len() >= 2 {
                    let name = parts[0];
                    let values: Vec<&str> = parts[1..].to_vec();
                    if !config.dimensions.iter().any(|d| d.name == name) {
                        config.add_dimension(name, values);
                    }
                }
            }

            // #pragma variant_exclude FEATURE_A=1,FEATURE_B=0
            if let Some(rest) = line.strip_prefix("#pragma variant_exclude ") {
                let conditions_str = rest.trim();
                let mut conditions = HashMap::new();

                for part in conditions_str.split(',') {
                    let kv: Vec<&str> = part.split('=').collect();
                    if kv.len() == 2 {
                        conditions.insert(kv[0].trim().to_string(), kv[1].trim().to_string());
                    }
                }

                if !conditions.is_empty() {
                    config.exclude(conditions);
                }
            }
        }

        config
    }
}

//=============================================================================
// 变体编译结果
//=============================================================================

/// 编译后的变体
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CompiledVariant {
    /// 变体键
    pub key: VariantKey,
    /// SPIR-V 二进制
    pub spirv: Vec<u8>,
    /// 使用的宏定义
    pub defines: Vec<(String, Option<String>)>,
    /// 编译耗时 (毫秒)
    pub compile_time_ms: u64,
}

/// 变体包 (包含所有变体)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VariantBundle {
    /// 着色器名称
    pub shader_name: String,
    /// 变体配置
    pub config: VariantConfig,
    /// 所有编译的变体
    pub variants: Vec<CompiledVariant>,
    /// 变体索引 (哈希ID -> 索引)
    pub index: HashMap<u64, usize>,
}

impl VariantBundle {
    /// 创建新的变体包
    pub fn new(shader_name: &str, config: VariantConfig) -> Self {
        Self {
            shader_name: shader_name.to_string(),
            config,
            variants: Vec::new(),
            index: HashMap::new(),
        }
    }

    /// 添加编译好的变体
    pub fn add_variant(&mut self, variant: CompiledVariant) {
        let hash_id = variant.key.hash_id();
        let idx = self.variants.len();
        self.variants.push(variant);
        self.index.insert(hash_id, idx);
    }

    /// 根据变体键查找变体
    pub fn get_variant(&self, key: &VariantKey) -> Option<&CompiledVariant> {
        let hash_id = key.hash_id();
        self.index.get(&hash_id).map(|&idx| &self.variants[idx])
    }

    /// 获取默认变体
    pub fn default_variant(&self) -> Option<&CompiledVariant> {
        let default_indices: Vec<usize> = self
            .config
            .dimensions
            .iter()
            .map(|d| d.default_index)
            .collect();
        self.get_variant(&VariantKey::new(default_indices))
    }

    /// 获取变体数量
    pub fn variant_count(&self) -> usize {
        self.variants.len()
    }

    /// 获取总大小 (字节)
    pub fn total_size(&self) -> usize {
        self.variants.iter().map(|v| v.spirv.len()).sum()
    }

    /// 保存到文件
    pub fn save(&self, path: &PathBuf) -> Result<()> {
        let json = serde_json::to_string_pretty(self)?;
        std::fs::write(path, json)?;
        Ok(())
    }

    /// 从文件加载
    pub fn load(path: &PathBuf) -> Result<Self> {
        let json = std::fs::read_to_string(path)?;
        let bundle: Self = serde_json::from_str(&json)?;
        Ok(bundle)
    }
}

//=============================================================================
// 变体编译器
//=============================================================================

use crate::compiler::ShaderCompiler;
use crate::core::{CompileOptions, ShaderSource};
use rayon::prelude::*;
use std::time::Instant;

/// 变体编译器
pub struct VariantCompiler {
    compiler: ShaderCompiler,
}

impl VariantCompiler {
    /// 创建变体编译器
    pub fn new() -> Result<Self> {
        Ok(Self {
            compiler: ShaderCompiler::new()?,
        })
    }

    /// 编译所有变体
    pub fn compile_variants(
        &self,
        source: &ShaderSource,
        config: &VariantConfig,
        base_options: &CompileOptions,
    ) -> Result<VariantBundle> {
        let mut bundle = VariantBundle::new(&source.file_name(), config.clone());
        let variant_keys = config.generate_variants();

        for key in variant_keys {
            let variant = self.compile_single_variant(source, config, base_options, &key)?;
            bundle.add_variant(variant);
        }

        Ok(bundle)
    }

    /// 并行编译所有变体
    pub fn compile_variants_parallel(
        &self,
        source: &ShaderSource,
        config: &VariantConfig,
        base_options: &CompileOptions,
    ) -> Result<VariantBundle> {
        let variant_keys = config.generate_variants();
        let source_clone = source.clone();
        let config_clone = config.clone();
        let base_options_clone = base_options.clone();

        let results: Vec<Result<CompiledVariant>> = variant_keys
            .par_iter()
            .map(|key| {
                // 每个线程创建自己的编译器实例
                let compiler = ShaderCompiler::new()?;
                let variant_compiler = VariantCompiler { compiler };
                variant_compiler.compile_single_variant(
                    &source_clone,
                    &config_clone,
                    &base_options_clone,
                    key,
                )
            })
            .collect();

        let mut bundle = VariantBundle::new(&source.file_name(), config.clone());
        for result in results {
            bundle.add_variant(result?);
        }

        Ok(bundle)
    }

    /// 编译单个变体
    fn compile_single_variant(
        &self,
        source: &ShaderSource,
        config: &VariantConfig,
        base_options: &CompileOptions,
        key: &VariantKey,
    ) -> Result<CompiledVariant> {
        let start = Instant::now();

        // 生成变体特定的宏定义
        let variant_defines = config.generate_defines(key);

        // 合并基础选项和变体定义
        let mut options = base_options.clone();
        for (name, value) in &variant_defines {
            options.defines.push((name.clone(), value.clone()));
        }

        // 编译
        let result = self.compiler.compile(source, &options)?;

        Ok(CompiledVariant {
            key: key.clone(),
            spirv: result.spirv_binary,
            defines: variant_defines,
            compile_time_ms: start.elapsed().as_millis() as u64,
        })
    }
}

//=============================================================================
// 变体选择器
//=============================================================================

/// 运行时变体选择器
pub struct VariantSelector {
    /// 当前特性集
    features: HashSet<String>,
    /// 当前多值设置
    settings: HashMap<String, String>,
}

impl VariantSelector {
    pub fn new() -> Self {
        Self {
            features: HashSet::new(),
            settings: HashMap::new(),
        }
    }

    /// 启用特性
    pub fn enable(&mut self, feature: &str) -> &mut Self {
        self.features.insert(feature.to_string());
        self
    }

    /// 禁用特性
    pub fn disable(&mut self, feature: &str) -> &mut Self {
        self.features.remove(feature);
        self
    }

    /// 设置多值选项
    pub fn set(&mut self, name: &str, value: &str) -> &mut Self {
        self.settings.insert(name.to_string(), value.to_string());
        self
    }

    /// 根据当前设置生成变体键
    pub fn select(&self, config: &VariantConfig) -> VariantKey {
        let indices: Vec<usize> = config
            .dimensions
            .iter()
            .map(|dim| {
                if dim.is_boolean {
                    // 布尔变体
                    if self.features.contains(&dim.name) {
                        1
                    } else {
                        0
                    }
                } else {
                    // 多值变体
                    if let Some(value) = self.settings.get(&dim.name) {
                        dim.values
                            .iter()
                            .position(|v| v == value)
                            .unwrap_or(dim.default_index)
                    } else {
                        dim.default_index
                    }
                }
            })
            .collect();

        VariantKey::new(indices)
    }
}

impl Default for VariantSelector {
    fn default() -> Self {
        Self::new()
    }
}
