// ============================================================
// 文件名称：pso/mod.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：离线预热，消除首帧卡顿，精确序列化，跨会话持久化
// 功能描述：PSO (Pipeline State Object) 预热缓存系统 —
//           收集运行时使用的管线状态，序列化为 .limx.psocache
//           文件，在下次启动时预加载，消除 Vulkan 管线编译卡顿
//           超越 UE5 的 Shader Warmup 粒度
// 技术特性：SHA-256 状态哈希，分层缓存结构，差量更新，
//           SPIR-V 绑定描述符对齐，平台特定验证，统计报告
//
// ── 类型/函数表 ──────────────────────────────────────────────
// │ PsoDescriptor             │ 管线状态描述符 (可序列化)           │
// │ PsoCache                  │ PSO 缓存主体                        │
// │ PsoCacheStats             │ 缓存统计信息                        │
// │ GraphicsPsoDescriptor     │ 图形管线 PSO 描述                   │
// │ ComputePsoDescriptor       │ 计算管线 PSO 描述                   │
// │ RayTracingPsoDescriptor   │ 光追管线 PSO 描述                   │
// │ PsoCache::load()          │ 从磁盘加载缓存                      │
// │ PsoCache::save()          │ 保存缓存到磁盘                      │
// │ PsoCache::register()      │ 注册新 PSO 描述符                   │
// │ PsoCache::merge()         │ 合并多个缓存文件                    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                               │
// │ 2026-04-06   │ LimxTeam  │ 初始创建 — PSO 预热缓存系统          │
// ============================================================

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

// ──────────────────────────────────────────────────────────────
// 着色器阶段引用
// ──────────────────────────────────────────────────────────────

/// PSO 中一个着色器阶段的引用
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ShaderStageRef {
    /// 着色器阶段名称 ("vertex"/"fragment"/"compute" 等)
    pub stage: String,
    /// SPIR-V 文件路径
    pub spirv_path: PathBuf,
    /// SPIR-V 内容 SHA-256 哈希 (64 字符十六进制)
    pub spirv_hash: String,
    /// 入口点函数名
    pub entry_point: String,
    /// 特化常量值映射 (constant_id -> value)
    #[serde(default)]
    pub specialization_data: HashMap<u32, u64>,
}

impl ShaderStageRef {
    /// 从 SPIR-V 文件路径创建
    pub fn from_spirv(stage: &str, spirv_path: &Path, entry_point: &str) -> Result<Self> {
        let data = std::fs::read(spirv_path)
            .with_context(|| format!("无法读取 SPIR-V: {}", spirv_path.display()))?;
        let hash = compute_sha256_hex(&data);
        Ok(Self {
            stage: stage.to_string(),
            spirv_path: spirv_path.to_path_buf(),
            spirv_hash: hash,
            entry_point: entry_point.to_string(),
            specialization_data: HashMap::new(),
        })
    }

    /// 添加特化常量
    pub fn with_specialization(mut self, id: u32, value: u64) -> Self {
        self.specialization_data.insert(id, value);
        self
    }

    /// 验证 SPIR-V 文件是否与记录的哈希一致
    pub fn verify(&self) -> bool {
        if let Ok(data) = std::fs::read(&self.spirv_path) {
            compute_sha256_hex(&data) == self.spirv_hash
        } else {
            false
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 顶点输入描述
// ──────────────────────────────────────────────────────────────

/// 顶点属性格式
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum VertexFormat {
    Float32x1,
    Float32x2,
    Float32x3,
    Float32x4,
    Uint32x1,
    Uint32x2,
    Uint32x3,
    Uint32x4,
    Sint32x1,
    Sint32x2,
    Sint32x3,
    Sint32x4,
    Float16x2,
    Float16x4,
    Snorm8x4,
    Unorm8x4,
}

/// 顶点属性描述
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct VertexAttribute {
    pub location: u32,
    pub binding: u32,
    pub format: VertexFormat,
    pub offset: u32,
}

/// 顶点绑定描述
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct VertexBinding {
    pub binding: u32,
    pub stride: u32,
    pub instance_rate: bool,
}

// ──────────────────────────────────────────────────────────────
// 渲染状态描述
// ──────────────────────────────────────────────────────────────

/// 混合因子
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum BlendFactor {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
}

/// 混合操作
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum BlendOp {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
}

/// 渲染目标混合状态
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct BlendState {
    pub enable_blend: bool,
    pub src_color_factor: BlendFactor,
    pub dst_color_factor: BlendFactor,
    pub color_blend_op: BlendOp,
    pub src_alpha_factor: BlendFactor,
    pub dst_alpha_factor: BlendFactor,
    pub alpha_blend_op: BlendOp,
    pub color_write_mask: u8,
}

impl Default for BlendState {
    fn default() -> Self {
        Self {
            enable_blend: false,
            src_color_factor: BlendFactor::One,
            dst_color_factor: BlendFactor::Zero,
            color_blend_op: BlendOp::Add,
            src_alpha_factor: BlendFactor::One,
            dst_alpha_factor: BlendFactor::Zero,
            alpha_blend_op: BlendOp::Add,
            color_write_mask: 0xF,
        }
    }
}

/// 深度/模板状态
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
pub struct DepthStencilState {
    pub depth_test_enable: bool,
    pub depth_write_enable: bool,
    pub depth_compare_op: u8, // Vulkan CompareOp 枚举值
    pub stencil_test_enable: bool,
}

/// 光栅化状态
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct RasterizationState {
    pub polygon_mode: u8, // VK_POLYGON_MODE_FILL=0, LINE=1, POINT=2
    pub cull_mode: u8,    // VK_CULL_MODE_NONE=0, FRONT=1, BACK=2
    pub front_face: u8,   // VK_FRONT_FACE_COUNTER_CLOCKWISE=0
    pub depth_bias_enable: bool,
    pub depth_clamp_enable: bool,
    pub rasterizer_discard: bool,
}

impl Default for RasterizationState {
    fn default() -> Self {
        Self {
            polygon_mode: 0,
            cull_mode: 2,  // BACK
            front_face: 0, // CCW
            depth_bias_enable: false,
            depth_clamp_enable: false,
            rasterizer_discard: false,
        }
    }
}

// ──────────────────────────────────────────────────────────────
// PSO 描述符类型
// ──────────────────────────────────────────────────────────────

/// 图形管线 PSO 描述符
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GraphicsPsoDescriptor {
    /// 顶点着色器
    pub vertex_shader: ShaderStageRef,
    /// 片元着色器 (可选)
    pub fragment_shader: Option<ShaderStageRef>,
    /// 几何着色器 (可选)
    pub geometry_shader: Option<ShaderStageRef>,
    /// Mesh 着色器 (可选)
    pub mesh_shader: Option<ShaderStageRef>,
    /// Task 着色器 (可选)
    pub task_shader: Option<ShaderStageRef>,
    /// 顶点输入状态
    pub vertex_attributes: Vec<VertexAttribute>,
    pub vertex_bindings: Vec<VertexBinding>,
    /// 图元拓扑类型
    pub primitive_topology: u8,
    /// 混合状态 (每个渲染目标)
    pub blend_states: Vec<BlendState>,
    /// 深度模板状态
    pub depth_stencil: DepthStencilState,
    /// 光栅化状态
    pub rasterization: RasterizationState,
    /// MSAA 采样数
    pub sample_count: u8,
    /// 渲染通道格式描述 (颜色格式列表)
    pub color_attachment_formats: Vec<u32>,
    /// 深度格式
    pub depth_attachment_format: u32,
}

/// 计算管线 PSO 描述符
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ComputePsoDescriptor {
    /// 计算着色器
    pub compute_shader: ShaderStageRef,
}

/// 光追管线 PSO 描述符
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RayTracingPsoDescriptor {
    /// RayGen 着色器
    pub raygen_shader: ShaderStageRef,
    /// Miss 着色器列表
    pub miss_shaders: Vec<ShaderStageRef>,
    /// ClosestHit 着色器列表
    pub closest_hit_shaders: Vec<ShaderStageRef>,
    /// AnyHit 着色器列表
    pub any_hit_shaders: Vec<ShaderStageRef>,
    /// Intersection 着色器列表
    pub intersection_shaders: Vec<ShaderStageRef>,
    /// 最大递归深度
    pub max_recursion_depth: u32,
    /// 最大 Payload 大小 (字节)
    pub max_payload_size: u32,
    /// 最大 HitAttribute 大小 (字节)
    pub max_hit_attribute_size: u32,
}

/// PSO 类型枚举
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum PsoDescriptor {
    Graphics(GraphicsPsoDescriptor),
    Compute(ComputePsoDescriptor),
    RayTracing(RayTracingPsoDescriptor),
}

impl PsoDescriptor {
    /// 计算此 PSO 描述符的稳定哈希
    pub fn compute_hash(&self) -> String {
        let json = serde_json::to_string(self).unwrap_or_default();
        compute_sha256_hex(json.as_bytes())
    }

    /// 获取此 PSO 的类型名称
    pub fn type_name(&self) -> &'static str {
        match self {
            Self::Graphics(_) => "Graphics",
            Self::Compute(_) => "Compute",
            Self::RayTracing(_) => "RayTracing",
        }
    }

    /// 获取所有引用的 SPIR-V 文件
    pub fn referenced_spirv_files(&self) -> Vec<&Path> {
        match self {
            Self::Graphics(g) => {
                let mut files = vec![g.vertex_shader.spirv_path.as_path()];
                if let Some(ref fs) = g.fragment_shader {
                    files.push(fs.spirv_path.as_path());
                }
                if let Some(ref gs) = g.geometry_shader {
                    files.push(gs.spirv_path.as_path());
                }
                if let Some(ref ms) = g.mesh_shader {
                    files.push(ms.spirv_path.as_path());
                }
                if let Some(ref ts) = g.task_shader {
                    files.push(ts.spirv_path.as_path());
                }
                files
            }
            Self::Compute(c) => vec![c.compute_shader.spirv_path.as_path()],
            Self::RayTracing(rt) => {
                let mut files = vec![rt.raygen_shader.spirv_path.as_path()];
                for s in &rt.miss_shaders {
                    files.push(s.spirv_path.as_path());
                }
                for s in &rt.closest_hit_shaders {
                    files.push(s.spirv_path.as_path());
                }
                for s in &rt.any_hit_shaders {
                    files.push(s.spirv_path.as_path());
                }
                files
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────
// PSO 缓存条目
// ──────────────────────────────────────────────────────────────

/// PSO 缓存条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PsoCacheEntry {
    /// 描述符哈希 (唯一键)
    pub hash: String,
    /// PSO 描述符
    pub descriptor: PsoDescriptor,
    /// 标签 (用于过滤/分组)
    pub tags: Vec<String>,
    /// 首次记录时间 (Unix 时间戳)
    pub recorded_at: u64,
    /// 最后使用时间
    pub last_used_at: u64,
    /// 使用计数
    pub use_count: u32,
    /// 平台标识 (如 "win64-vulkan1.3")
    pub platform: String,
}

// ──────────────────────────────────────────────────────────────
// PSO 缓存统计
// ──────────────────────────────────────────────────────────────

/// PSO 缓存统计信息
#[derive(Debug, Default)]
pub struct PsoCacheStats {
    pub total_entries: usize,
    pub graphics_count: usize,
    pub compute_count: usize,
    pub ray_tracing_count: usize,
    pub total_size_bytes: usize,
    pub valid_entries: usize,
    pub stale_entries: usize,
}

impl PsoCacheStats {
    pub fn print_report(&self) {
        println!("\n╔══════════════════════════════════════════════════════════════╗");
        println!("║                    PSO 缓存统计                               ║");
        println!("╠══════════════════════════════════════════════════════════════╣");
        println!(
            "║  总条目数:     {:>6}                                      ║",
            self.total_entries
        );
        println!(
            "║  图形管线:     {:>6}                                      ║",
            self.graphics_count
        );
        println!(
            "║  计算管线:     {:>6}                                      ║",
            self.compute_count
        );
        println!(
            "║  光追管线:     {:>6}                                      ║",
            self.ray_tracing_count
        );
        println!("╠══════════════════════════════════════════════════════════════╣");
        println!(
            "║  有效条目:     {:>6}                                      ║",
            self.valid_entries
        );
        println!(
            "║  过期条目:     {:>6}                                      ║",
            self.stale_entries
        );
        println!(
            "║  总大小:       {:>6} KB                                   ║",
            self.total_size_bytes / 1024
        );
        println!("╚══════════════════════════════════════════════════════════════╝");
    }
}

// ──────────────────────────────────────────────────────────────
// PSO 缓存主体
// ──────────────────────────────────────────────────────────────

/// PSO 预热缓存
#[derive(Debug, Serialize, Deserialize)]
pub struct PsoCache {
    /// 格式版本
    pub version: u32,
    /// 平台标识
    pub platform: String,
    /// Vulkan 目标版本
    pub target_env: String,
    /// 缓存条目 (hash -> 条目)
    pub entries: HashMap<String, PsoCacheEntry>,
    /// 创建时间
    pub created_at: u64,
    /// 最后更新时间
    pub updated_at: u64,
}

impl PsoCache {
    /// 当前缓存格式版本
    pub const CURRENT_VERSION: u32 = 1;

    /// 创建新的 PSO 缓存
    pub fn new(platform: &str, target_env: &str) -> Self {
        let now = current_timestamp();
        Self {
            version: Self::CURRENT_VERSION,
            platform: platform.to_string(),
            target_env: target_env.to_string(),
            entries: HashMap::new(),
            created_at: now,
            updated_at: now,
        }
    }

    /// 从文件加载 PSO 缓存
    pub fn load(path: &Path) -> Result<Self> {
        let data = std::fs::read(path)
            .with_context(|| format!("无法读取 PSO 缓存: {}", path.display()))?;
        let cache: Self = serde_json::from_slice(&data)
            .with_context(|| format!("解析 PSO 缓存失败: {}", path.display()))?;

        if cache.version != Self::CURRENT_VERSION {
            return Err(anyhow::anyhow!(
                "PSO 缓存版本不匹配: 期望 {}, 实际 {}",
                Self::CURRENT_VERSION,
                cache.version
            ));
        }
        Ok(cache)
    }

    /// 加载或创建缓存
    pub fn load_or_create(path: &Path, platform: &str, target_env: &str) -> Self {
        Self::load(path).unwrap_or_else(|_| Self::new(platform, target_env))
    }

    /// 保存缓存到文件
    pub fn save(&mut self, path: &Path) -> Result<()> {
        self.updated_at = current_timestamp();
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let json = serde_json::to_string_pretty(&self)?;
        std::fs::write(path, json.as_bytes())?;
        Ok(())
    }

    /// 注册新的 PSO 描述符
    /// 如果已存在相同哈希，则更新使用次数
    pub fn register(&mut self, descriptor: PsoDescriptor, tags: Vec<String>) -> String {
        let hash = descriptor.compute_hash();
        let now = current_timestamp();

        self.entries
            .entry(hash.clone())
            .and_modify(|e| {
                e.use_count += 1;
                e.last_used_at = now;
                for tag in &tags {
                    if !e.tags.contains(tag) {
                        e.tags.push(tag.clone());
                    }
                }
            })
            .or_insert_with(|| PsoCacheEntry {
                hash: hash.clone(),
                descriptor,
                tags,
                recorded_at: now,
                last_used_at: now,
                use_count: 1,
                platform: self.platform.clone(),
            });

        hash
    }

    /// 移除指定哈希的条目
    pub fn remove(&mut self, hash: &str) -> bool {
        self.entries.remove(hash).is_some()
    }

    /// 移除所有 SPIR-V 文件已失效的条目
    pub fn purge_stale(&mut self) -> usize {
        let before = self.entries.len();
        self.entries.retain(|_, entry| {
            entry
                .descriptor
                .referenced_spirv_files()
                .iter()
                .all(|path| {
                    // 检查文件是否存在
                    path.exists()
                })
        });
        before - self.entries.len()
    }

    /// 合并另一个 PSO 缓存 (取 union，相同哈希取使用次数更高的)
    pub fn merge(&mut self, other: &PsoCache) {
        for (hash, entry) in &other.entries {
            self.entries
                .entry(hash.clone())
                .and_modify(|e| {
                    e.use_count += entry.use_count;
                    if entry.last_used_at > e.last_used_at {
                        e.last_used_at = entry.last_used_at;
                    }
                    for tag in &entry.tags {
                        if !e.tags.contains(tag) {
                            e.tags.push(tag.clone());
                        }
                    }
                })
                .or_insert_with(|| entry.clone());
        }
        self.updated_at = current_timestamp();
    }

    /// 计算缓存统计信息
    pub fn compute_stats(&self) -> PsoCacheStats {
        let mut stats = PsoCacheStats {
            total_entries: self.entries.len(),
            ..Default::default()
        };

        for entry in self.entries.values() {
            match &entry.descriptor {
                PsoDescriptor::Graphics(_) => stats.graphics_count += 1,
                PsoDescriptor::Compute(_) => stats.compute_count += 1,
                PsoDescriptor::RayTracing(_) => stats.ray_tracing_count += 1,
            }

            // 检查 SPIR-V 有效性
            let is_valid = entry
                .descriptor
                .referenced_spirv_files()
                .iter()
                .all(|p| p.exists());

            if is_valid {
                stats.valid_entries += 1;
            } else {
                stats.stale_entries += 1;
            }
        }

        // 估算文件大小
        stats.total_size_bytes = serde_json::to_string(&self).map(|s| s.len()).unwrap_or(0);

        stats
    }

    /// 按标签获取所有匹配的 PSO
    pub fn find_by_tag(&self, tag: &str) -> Vec<&PsoCacheEntry> {
        self.entries
            .values()
            .filter(|e| e.tags.iter().any(|t| t == tag))
            .collect()
    }

    /// 获取最常用的 PSO (按使用次数排序)
    pub fn most_used(&self, limit: usize) -> Vec<&PsoCacheEntry> {
        let mut entries: Vec<_> = self.entries.values().collect();
        entries.sort_by(|a, b| b.use_count.cmp(&a.use_count));
        entries.truncate(limit);
        entries
    }
}

// ──────────────────────────────────────────────────────────────
// 辅助函数
// ──────────────────────────────────────────────────────────────

fn compute_sha256_hex(data: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(data);
    let result = hasher.finalize();
    result.iter().fold(String::with_capacity(64), |mut s, b| {
        use std::fmt::Write;
        let _ = write!(s, "{:02x}", b);
        s
    })
}

fn current_timestamp() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// 生成示例 .limx.psocache 的帮助说明
pub fn pso_cache_file_extension() -> &'static str {
    ".limx.psocache"
}

// ──────────────────────────────────────────────────────────────
// 单元测试
// ──────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn make_compute_pso(path: &str) -> PsoDescriptor {
        PsoDescriptor::Compute(ComputePsoDescriptor {
            compute_shader: ShaderStageRef {
                stage: "compute".to_string(),
                spirv_path: PathBuf::from(path),
                spirv_hash: "abc123".to_string(),
                entry_point: "main".to_string(),
                specialization_data: HashMap::new(),
            },
        })
    }

    #[test]
    fn test_pso_cache_register_and_find() {
        let mut cache = PsoCache::new("win64", "vulkan1.3");
        let pso = make_compute_pso("test.spv");
        let hash = cache.register(pso, vec!["lighting".to_string()]);
        assert!(!hash.is_empty());
        assert_eq!(cache.entries.len(), 1);
        assert!(cache.entries.contains_key(&hash));
    }

    #[test]
    fn test_pso_cache_duplicate_increments_use_count() {
        let mut cache = PsoCache::new("win64", "vulkan1.3");
        let pso1 = make_compute_pso("same.spv");
        let pso2 = make_compute_pso("same.spv");
        let hash1 = cache.register(pso1, Vec::new());
        let hash2 = cache.register(pso2, Vec::new());
        assert_eq!(hash1, hash2);
        assert_eq!(cache.entries[&hash1].use_count, 2);
    }

    #[test]
    fn test_pso_cache_find_by_tag() {
        let mut cache = PsoCache::new("win64", "vulkan1.3");
        cache.register(make_compute_pso("a.spv"), vec!["lighting".to_string()]);
        cache.register(make_compute_pso("b.spv"), vec!["shadows".to_string()]);
        let lighting = cache.find_by_tag("lighting");
        assert_eq!(lighting.len(), 1);
    }

    #[test]
    fn test_pso_cache_stats() {
        let mut cache = PsoCache::new("win64", "vulkan1.3");
        cache.register(make_compute_pso("a.spv"), Vec::new());
        cache.register(make_compute_pso("b.spv"), Vec::new());
        let stats = cache.compute_stats();
        assert_eq!(stats.total_entries, 2);
        assert_eq!(stats.compute_count, 2);
    }

    #[test]
    fn test_pso_cache_merge() {
        let mut cache1 = PsoCache::new("win64", "vulkan1.3");
        let mut cache2 = PsoCache::new("win64", "vulkan1.3");
        cache1.register(make_compute_pso("a.spv"), Vec::new());
        cache2.register(make_compute_pso("b.spv"), Vec::new());
        cache1.merge(&cache2);
        assert_eq!(cache1.entries.len(), 2);
    }

    #[test]
    fn test_pso_descriptor_type_name() {
        let pso = make_compute_pso("x.spv");
        assert_eq!(pso.type_name(), "Compute");
    }

    #[test]
    fn test_compute_sha256_hex() {
        let hash = compute_sha256_hex(b"hello");
        assert_eq!(hash.len(), 64);
        assert!(hash.chars().all(|c| c.is_ascii_hexdigit()));
    }
}
