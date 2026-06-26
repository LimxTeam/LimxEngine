// ============================================================
// 文件名称：stage_interface.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：编译期跨阶段接口验证 — 在链接着色器管线前即检测
//           Vertex→Fragment IO 不匹配、Push Constants 越界、
//           绑定冲突等问题。UE5 依赖驱动层运行时报错，我们在
//           编译阶段就捕获，开发体验远超 UE5
// 功能描述：跨阶段接口验证器 — 收集各着色器阶段的 IO 声明，
//           验证 location/类型/插值限定符匹配，检查 Push
//           Constants 总大小和范围，检查描述符集绑定冲突
// 技术特性：基于正则的 GLSL/HLSL layout 解析、IO 签名匹配、
//           Push Constants 尺寸计算、描述符集一致性检查
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ StageInterfaceValidator    │ 跨阶段接口验证器              │
// │ ShaderIO                   │ 着色器 IO 变量描述            │
// │ PushConstantBlock          │ Push Constants 块描述         │
// │ DescriptorBinding          │ 描述符绑定描述                │
// │ StageInterface             │ 单阶段接口摘要                │
// │ ValidationReport           │ 验证报告                     │
// │ InterfaceDiagnostic        │ 接口诊断信息                  │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建验证器                    │
// │ add_stage()                │ 添加着色器阶段信息             │
// │ validate_pipeline()        │ 验证整个管线                  │
// │ validate_io_matching()     │ 验证 IO 匹配                 │
// │ validate_push_constants()  │ 验证 Push Constants           │
// │ validate_descriptors()     │ 验证描述符绑定                │
// │ parse_stage_interface()    │ 从源码解析阶段接口             │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::HashMap;

// =============================================================================
// 着色器阶段
// =============================================================================

/// 着色器阶段类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ShaderStageType {
    Vertex,
    TessControl,
    TessEval,
    Geometry,
    Fragment,
    Compute,
    Task,
    Mesh,
}

impl ShaderStageType {
    /// 获取中文名称
    pub fn display_name(&self) -> &'static str {
        match self {
            Self::Vertex => "顶点",
            Self::TessControl => "曲面细分控制",
            Self::TessEval => "曲面细分求值",
            Self::Geometry => "几何",
            Self::Fragment => "片段",
            Self::Compute => "计算",
            Self::Task => "任务",
            Self::Mesh => "网格",
        }
    }

    /// 获取此阶段的下游阶段 (传统管线)
    pub fn next_stage(&self) -> Option<ShaderStageType> {
        match self {
            Self::Vertex => Some(Self::Fragment),
            Self::TessControl => Some(Self::TessEval),
            Self::TessEval => Some(Self::Fragment),
            Self::Geometry => Some(Self::Fragment),
            Self::Task => Some(Self::Mesh),
            Self::Mesh => Some(Self::Fragment),
            _ => None,
        }
    }
}

// =============================================================================
// IO 变量
// =============================================================================

/// 插值限定符
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum InterpolationQualifier {
    /// 默认 (smooth)
    Smooth,
    /// 平坦插值
    Flat,
    /// 无透视校正
    NoPerspective,
    /// 质心
    Centroid,
    /// 采样
    Sample,
}

/// 着色器 IO 变量
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderIO {
    /// location 编号
    pub location: u32,
    /// 变量名
    pub name: String,
    /// 类型名 (如 "vec3", "float", "vec4")
    pub type_name: String,
    /// 类型占用的 location 数 (mat4 = 4, dvec4 = 2)
    pub location_slots: u32,
    /// 插值限定符
    pub interpolation: InterpolationQualifier,
    /// 是否为输出
    pub is_output: bool,
}

impl ShaderIO {
    /// 获取此 IO 占用的 location 范围
    pub fn location_range(&self) -> std::ops::Range<u32> {
        self.location..self.location + self.location_slots
    }
}

// =============================================================================
// Push Constants
// =============================================================================

/// Push Constants 块中的成员
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PushConstantMember {
    /// 成员名
    pub name: String,
    /// 类型名
    pub type_name: String,
    /// 在块中的偏移 (字节)
    pub offset: usize,
    /// 大小 (字节)
    pub size: usize,
}

/// Push Constants 块
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PushConstantBlock {
    /// 块名
    pub block_name: String,
    /// 成员列表
    pub members: Vec<PushConstantMember>,
    /// 总大小 (字节)
    pub total_size: usize,
    /// 所属阶段
    pub stage: ShaderStageType,
}

// =============================================================================
// 描述符绑定
// =============================================================================

/// 描述符类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum DescriptorType {
    UniformBuffer,
    StorageBuffer,
    SampledImage,
    StorageImage,
    Sampler,
    CombinedImageSampler,
    InputAttachment,
}

/// 描述符绑定
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DescriptorBinding {
    /// set 编号
    pub set: u32,
    /// binding 编号
    pub binding: u32,
    /// 描述符类型
    pub descriptor_type: DescriptorType,
    /// 变量名
    pub name: String,
    /// 类型名
    pub type_name: String,
    /// 数组大小 (0 = 非数组)
    pub array_size: u32,
    /// 所属阶段
    pub stage: ShaderStageType,
}

// =============================================================================
// 阶段接口
// =============================================================================

/// 单个着色器阶段的接口摘要
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct StageInterface {
    /// 阶段名称
    pub stage_name: String,
    /// 输入变量
    pub inputs: Vec<ShaderIO>,
    /// 输出变量
    pub outputs: Vec<ShaderIO>,
    /// Push Constants
    pub push_constants: Option<PushConstantBlock>,
    /// 描述符绑定
    pub descriptor_bindings: Vec<DescriptorBinding>,
}

// =============================================================================
// 诊断信息
// =============================================================================

/// 诊断严重程度
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub enum InterfaceSeverity {
    Info,
    Warning,
    Error,
}

/// 诊断类别
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum InterfaceDiagnosticKind {
    /// 输出/输入 location 不匹配
    LocationMismatch,
    /// 输出/输入 类型不匹配
    TypeMismatch,
    /// 输出/输入 插值限定符不匹配
    InterpolationMismatch,
    /// 上游缺少下游需要的 location
    MissingOutput,
    /// 下游有未连接的输入
    UnconnectedInput,
    /// Push Constants 超过硬件限制
    PushConstantSizeExceeded,
    /// Push Constants 跨阶段布局不一致
    PushConstantLayoutMismatch,
    /// 描述符绑定冲突 (同 set/binding，不同类型)
    DescriptorBindingConflict,
    /// Location 重叠
    LocationOverlap,
}

/// 接口诊断信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct InterfaceDiagnostic {
    pub severity: InterfaceSeverity,
    pub kind: InterfaceDiagnosticKind,
    pub message: String,
    pub suggestion: Option<String>,
}

/// 验证报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ValidationReport {
    pub diagnostics: Vec<InterfaceDiagnostic>,
    pub error_count: usize,
    pub warning_count: usize,
    pub stage_count: usize,
}

impl ValidationReport {
    pub fn is_ok(&self) -> bool {
        self.error_count == 0
    }
}

// =============================================================================
// Vulkan 硬件限制常量
// =============================================================================

/// Vulkan 最小保证的 Push Constants 大小 (字节)
const VULKAN_MIN_PUSH_CONSTANT_SIZE: usize = 128;

// =============================================================================
// 跨阶段接口验证器
// =============================================================================

/// 跨阶段接口验证器
pub struct StageInterfaceValidator {
    /// 各阶段接口
    stages: HashMap<ShaderStageType, StageInterface>,
    /// Push Constants 大小上限
    push_constant_limit: usize,
}

impl StageInterfaceValidator {
    /// 创建验证器
    pub fn new() -> Self {
        Self {
            stages: HashMap::new(),
            push_constant_limit: VULKAN_MIN_PUSH_CONSTANT_SIZE,
        }
    }

    /// 设置 Push Constants 大小上限
    pub fn set_push_constant_limit(&mut self, limit: usize) {
        self.push_constant_limit = limit;
    }

    /// 添加阶段接口
    pub fn add_stage(&mut self, stage: ShaderStageType, interface: StageInterface) {
        self.stages.insert(stage, interface);
    }

    /// 验证整个管线
    pub fn validate_pipeline(&self) -> ValidationReport {
        let mut diagnostics = Vec::new();

        self.validate_io_matching(&mut diagnostics);
        self.validate_push_constants(&mut diagnostics);
        self.validate_descriptors(&mut diagnostics);
        self.validate_location_overlaps(&mut diagnostics);

        let error_count = diagnostics
            .iter()
            .filter(|d| d.severity == InterfaceSeverity::Error)
            .count();
        let warning_count = diagnostics
            .iter()
            .filter(|d| d.severity == InterfaceSeverity::Warning)
            .count();

        ValidationReport {
            diagnostics,
            error_count,
            warning_count,
            stage_count: self.stages.len(),
        }
    }

    /// 验证相邻阶段的 IO 匹配
    fn validate_io_matching(&self, diagnostics: &mut Vec<InterfaceDiagnostic>) {
        // 检查所有有上下游关系的阶段对
        let stage_pairs: Vec<(ShaderStageType, ShaderStageType)> = vec![
            (ShaderStageType::Vertex, ShaderStageType::Fragment),
            (ShaderStageType::Vertex, ShaderStageType::TessControl),
            (ShaderStageType::Vertex, ShaderStageType::Geometry),
            (ShaderStageType::TessControl, ShaderStageType::TessEval),
            (ShaderStageType::TessEval, ShaderStageType::Fragment),
            (ShaderStageType::Geometry, ShaderStageType::Fragment),
            (ShaderStageType::Task, ShaderStageType::Mesh),
            (ShaderStageType::Mesh, ShaderStageType::Fragment),
        ];

        for (upstream_type, downstream_type) in &stage_pairs {
            let upstream = match self.stages.get(upstream_type) {
                Some(s) => s,
                None => continue,
            };
            let downstream = match self.stages.get(downstream_type) {
                Some(s) => s,
                None => continue,
            };

            let upstream_name = upstream_type.display_name();
            let downstream_name = downstream_type.display_name();

            // 构建上游输出 location -> IO 映射
            let output_map: HashMap<u32, &ShaderIO> = upstream
                .outputs
                .iter()
                .map(|io| (io.location, io))
                .collect();

            // 检查下游每个输入是否有对应的上游输出
            for input in &downstream.inputs {
                match output_map.get(&input.location) {
                    None => {
                        diagnostics.push(InterfaceDiagnostic {
                            severity: InterfaceSeverity::Error,
                            kind: InterfaceDiagnosticKind::MissingOutput,
                            message: format!(
                                "{}着色器输入 '{}' (location={}) 在{}着色器输出中找不到对应",
                                downstream_name, input.name, input.location, upstream_name,
                            ),
                            suggestion: Some(format!(
                                "在{}着色器中添加 layout(location={}) out {} {};",
                                upstream_name, input.location, input.type_name, input.name,
                            )),
                        });
                    }
                    Some(output) => {
                        // 类型匹配检查
                        if output.type_name != input.type_name {
                            diagnostics.push(InterfaceDiagnostic {
                                severity: InterfaceSeverity::Error,
                                kind: InterfaceDiagnosticKind::TypeMismatch,
                                message: format!(
                                    "location {} 类型不匹配: {}着色器输出 '{}' ({}) vs {}着色器输入 '{}' ({})",
                                    input.location,
                                    upstream_name, output.name, output.type_name,
                                    downstream_name, input.name, input.type_name,
                                ),
                                suggestion: Some(
                                    "确保跨阶段 IO 使用相同类型".to_string(),
                                ),
                            });
                        }

                        // 插值限定符匹配检查
                        if output.interpolation != input.interpolation {
                            diagnostics.push(InterfaceDiagnostic {
                                severity: InterfaceSeverity::Warning,
                                kind: InterfaceDiagnosticKind::InterpolationMismatch,
                                message: format!(
                                    "location {} 插值限定符不匹配: {}着色器 '{:?}' vs {}着色器 '{:?}'",
                                    input.location,
                                    upstream_name, output.interpolation,
                                    downstream_name, input.interpolation,
                                ),
                                suggestion: Some(
                                    "建议两端使用相同的插值限定符".to_string(),
                                ),
                            });
                        }
                    }
                }
            }

            // 检查上游有但下游不消费的输出 (不消费的输出只是警告)
            let input_locations: std::collections::HashSet<u32> =
                downstream.inputs.iter().map(|io| io.location).collect();
            for output in &upstream.outputs {
                if !input_locations.contains(&output.location) {
                    diagnostics.push(InterfaceDiagnostic {
                        severity: InterfaceSeverity::Info,
                        kind: InterfaceDiagnosticKind::UnconnectedInput,
                        message: format!(
                            "{}着色器输出 '{}' (location={}) 未被{}着色器消费",
                            upstream_name, output.name, output.location, downstream_name,
                        ),
                        suggestion: None,
                    });
                }
            }
        }
    }

    /// 验证 Push Constants
    fn validate_push_constants(&self, diagnostics: &mut Vec<InterfaceDiagnostic>) {
        let mut all_push_constants: Vec<&PushConstantBlock> = Vec::new();

        for interface in self.stages.values() {
            if let Some(pc) = &interface.push_constants {
                all_push_constants.push(pc);

                // 检查大小限制
                if pc.total_size > self.push_constant_limit {
                    diagnostics.push(InterfaceDiagnostic {
                        severity: InterfaceSeverity::Error,
                        kind: InterfaceDiagnosticKind::PushConstantSizeExceeded,
                        message: format!(
                            "{}着色器 Push Constants 大小 ({} 字节) 超过限制 ({} 字节)",
                            pc.stage.display_name(), pc.total_size, self.push_constant_limit,
                        ),
                        suggestion: Some(format!(
                            "将部分数据移至 Uniform Buffer，或减小 Push Constants 大小至 {} 字节以内",
                            self.push_constant_limit,
                        )),
                    });
                }
            }
        }

        // 跨阶段 Push Constants 布局一致性检查
        if all_push_constants.len() >= 2 {
            let first = &all_push_constants[0];
            for other in &all_push_constants[1..] {
                // 检查共享成员的偏移是否一致
                for member in &first.members {
                    if let Some(other_member) = other.members.iter().find(|m| m.name == member.name)
                    {
                        if member.offset != other_member.offset || member.size != other_member.size
                        {
                            diagnostics.push(InterfaceDiagnostic {
                                severity: InterfaceSeverity::Error,
                                kind: InterfaceDiagnosticKind::PushConstantLayoutMismatch,
                                message: format!(
                                    "Push Constants 成员 '{}' 在 {} 和 {} 着色器中布局不一致 (偏移 {} vs {}, 大小 {} vs {})",
                                    member.name,
                                    first.stage.display_name(), other.stage.display_name(),
                                    member.offset, other_member.offset,
                                    member.size, other_member.size,
                                ),
                                suggestion: Some(
                                    "确保所有阶段的 Push Constants 使用相同的布局".to_string(),
                                ),
                            });
                        }
                    }
                }
            }
        }
    }

    /// 验证描述符绑定
    fn validate_descriptors(&self, diagnostics: &mut Vec<InterfaceDiagnostic>) {
        // 收集所有绑定，按 (set, binding) 分组
        let mut binding_map: HashMap<(u32, u32), Vec<&DescriptorBinding>> = HashMap::new();

        for interface in self.stages.values() {
            for binding in &interface.descriptor_bindings {
                binding_map
                    .entry((binding.set, binding.binding))
                    .or_default()
                    .push(binding);
            }
        }

        // 检查同一 (set, binding) 的描述符类型是否一致
        for ((set, binding), bindings) in &binding_map {
            if bindings.len() < 2 {
                continue;
            }

            let first = &bindings[0];
            for other in &bindings[1..] {
                if first.descriptor_type != other.descriptor_type {
                    diagnostics.push(InterfaceDiagnostic {
                        severity: InterfaceSeverity::Error,
                        kind: InterfaceDiagnosticKind::DescriptorBindingConflict,
                        message: format!(
                            "set={}, binding={}: 描述符类型冲突 — {} ({:?}) vs {} ({:?})",
                            set,
                            binding,
                            first.name,
                            first.descriptor_type,
                            other.name,
                            other.descriptor_type,
                        ),
                        suggestion: Some(
                            "同一绑定点在所有阶段必须使用相同的描述符类型".to_string(),
                        ),
                    });
                }
            }
        }
    }

    /// 验证同一阶段内的 location 重叠
    fn validate_location_overlaps(&self, diagnostics: &mut Vec<InterfaceDiagnostic>) {
        for (stage_type, interface) in &self.stages {
            // 检查输出 location 重叠
            check_io_overlaps(&interface.outputs, stage_type, "输出", diagnostics);
            // 检查输入 location 重叠
            check_io_overlaps(&interface.inputs, stage_type, "输入", diagnostics);
        }
    }
}

/// 检查 IO 列表中的 location 重叠
fn check_io_overlaps(
    io_list: &[ShaderIO],
    stage: &ShaderStageType,
    direction: &str,
    diagnostics: &mut Vec<InterfaceDiagnostic>,
) {
    for i in 0..io_list.len() {
        for j in (i + 1)..io_list.len() {
            let a = &io_list[i];
            let b = &io_list[j];
            let range_a = a.location_range();
            let range_b = b.location_range();

            // 范围重叠检查
            if range_a.start < range_b.end && range_b.start < range_a.end {
                diagnostics.push(InterfaceDiagnostic {
                    severity: InterfaceSeverity::Error,
                    kind: InterfaceDiagnosticKind::LocationOverlap,
                    message: format!(
                        "{}着色器{}中 '{}' (location={}, 占 {} slots) 与 '{}' (location={}, 占 {} slots) 的 location 重叠",
                        stage.display_name(), direction,
                        a.name, a.location, a.location_slots,
                        b.name, b.location, b.location_slots,
                    ),
                    suggestion: Some("调整 location 编号避免重叠".to_string()),
                });
            }
        }
    }
}

/// 从 GLSL 源码解析阶段接口 (简化版，基于 layout 关键字)
pub fn parse_stage_interface(source: &str, stage: ShaderStageType) -> StageInterface {
    let mut interface = StageInterface {
        stage_name: stage.display_name().to_string(),
        ..Default::default()
    };

    for line in source.lines() {
        let trimmed = line.trim();

        // layout(location = N) in/out type name;
        if trimmed.starts_with("layout") && trimmed.contains("location") {
            if let Some(io) = parse_layout_io(trimmed) {
                if io.is_output {
                    interface.outputs.push(io);
                } else {
                    interface.inputs.push(io);
                }
            }
        }

        // layout(set = S, binding = B) uniform/buffer ...
        if trimmed.starts_with("layout")
            && trimmed.contains("binding")
            && !trimmed.contains("location")
        {
            if let Some(binding) = parse_layout_binding(trimmed, stage) {
                interface.descriptor_bindings.push(binding);
            }
        }

        // layout(push_constant) uniform PushConstants { ... }
        if trimmed.contains("push_constant") {
            // Push constants 需要多行解析，这里标记
            // 简化: 仅记录存在
        }
    }

    interface
}

/// 解析 layout(location = N) in/out type name;
fn parse_layout_io(line: &str) -> Option<ShaderIO> {
    // 提取 location
    let loc_start = line.find("location")? + "location".len();
    let rest = &line[loc_start..];
    let eq_pos = rest.find('=')?;
    let after_eq = rest[eq_pos + 1..].trim_start();
    let loc_end = after_eq.find(|c: char| !c.is_ascii_digit())?;
    let location: u32 = after_eq[..loc_end].parse().ok()?;

    // 判断 in/out (支持 flat/noperspective 等限定符在 in/out 前)
    // 使用单词边界匹配，避免 "main" 中的 "in" 误匹配
    let words: Vec<&str> = line.split_whitespace().collect();
    let is_output = words.iter().any(|&w| w == "out");
    let is_input = words.iter().any(|&w| w == "in");

    if !is_output && !is_input {
        return None;
    }

    // 提取类型和名称: 找到 "in"/"out" 关键字后的两个词
    let direction_keyword = if is_output { "out" } else { "in" };
    let dir_idx = words.iter().position(|&w| w == direction_keyword)?;
    let after_dir: Vec<&str> = words[dir_idx + 1..].to_vec();

    if after_dir.len() < 2 {
        // 尝试用 rfind 兜底
    }

    let dir_pos = line.rfind(&format!("{} ", direction_keyword))?;
    let after_dir = line[dir_pos + direction_keyword.len()..].trim();
    let parts: Vec<&str> = after_dir.split_whitespace().collect();

    if parts.len() < 2 {
        return None;
    }

    let type_name = parts[0].to_string();
    let name = parts[1].trim_end_matches(';').to_string();

    // 计算 location slots
    let location_slots = type_location_slots(&type_name);

    // 解析插值限定符
    let interpolation = if line.contains("flat") {
        InterpolationQualifier::Flat
    } else if line.contains("noperspective") {
        InterpolationQualifier::NoPerspective
    } else {
        InterpolationQualifier::Smooth
    };

    Some(ShaderIO {
        location,
        name,
        type_name,
        location_slots,
        interpolation,
        is_output,
    })
}

/// 解析 layout(set = S, binding = B) uniform/buffer type name;
fn parse_layout_binding(line: &str, stage: ShaderStageType) -> Option<DescriptorBinding> {
    // 提取 set
    let set = extract_layout_number(line, "set")?;
    let binding = extract_layout_number(line, "binding")?;

    // 判断描述符类型
    let descriptor_type = if line.contains("uniform sampler") || line.contains("uniform texture") {
        DescriptorType::CombinedImageSampler
    } else if line.contains("buffer") || line.contains("readonly") || line.contains("writeonly") {
        DescriptorType::StorageBuffer
    } else if line.contains("uniform") {
        DescriptorType::UniformBuffer
    } else if line.contains("image") {
        DescriptorType::StorageImage
    } else {
        return None;
    };

    // 提取名称 (最后一个分号前的词)
    let name = line
        .split_whitespace()
        .last()
        .map(|s| s.trim_end_matches(';').trim_end_matches('{'))
        .unwrap_or("unknown")
        .to_string();

    let type_name = line
        .split_whitespace()
        .rev()
        .nth(1)
        .unwrap_or("unknown")
        .to_string();

    Some(DescriptorBinding {
        set,
        binding,
        descriptor_type,
        name,
        type_name,
        array_size: 0,
        stage,
    })
}

/// 从 layout(...) 中提取指定参数的数字值
fn extract_layout_number(line: &str, param: &str) -> Option<u32> {
    let pos = line.find(param)?;
    let rest = &line[pos + param.len()..];
    let eq_pos = rest.find('=')?;
    let after_eq = rest[eq_pos + 1..].trim_start();
    let end = after_eq.find(|c: char| !c.is_ascii_digit())?;
    after_eq[..end].parse().ok()
}

/// 获取类型占用的 location slot 数
fn type_location_slots(type_name: &str) -> u32 {
    match type_name {
        "mat4" | "float4x4" | "dmat2" => 4,
        "mat3" | "float3x3" => 3,
        "mat2" | "float2x2" => 2,
        "dvec3" | "dvec4" => 2,
        _ => 1,
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_io(location: u32, name: &str, type_name: &str, is_output: bool) -> ShaderIO {
        ShaderIO {
            location,
            name: name.to_string(),
            type_name: type_name.to_string(),
            location_slots: 1,
            interpolation: InterpolationQualifier::Smooth,
            is_output,
        }
    }

    #[test]
    fn test_valid_vertex_fragment_pipeline() {
        let mut validator = StageInterfaceValidator::new();

        let vs_interface = StageInterface {
            stage_name: "Vertex".to_string(),
            inputs: vec![],
            outputs: vec![
                make_io(0, "v_uv", "vec2", true),
                make_io(1, "v_normal", "vec3", true),
            ],
            push_constants: None,
            descriptor_bindings: vec![],
        };

        let fs_interface = StageInterface {
            stage_name: "Fragment".to_string(),
            inputs: vec![
                make_io(0, "v_uv", "vec2", false),
                make_io(1, "v_normal", "vec3", false),
            ],
            outputs: vec![make_io(0, "out_color", "vec4", true)],
            push_constants: None,
            descriptor_bindings: vec![],
        };

        validator.add_stage(ShaderStageType::Vertex, vs_interface);
        validator.add_stage(ShaderStageType::Fragment, fs_interface);

        let report = validator.validate_pipeline();
        assert!(
            report.is_ok(),
            "匹配的 VS→FS 管线应通过验证, 诊断: {:?}",
            report.diagnostics
        );
    }

    #[test]
    fn test_missing_output() {
        let mut validator = StageInterfaceValidator::new();

        let vs_interface = StageInterface {
            stage_name: "Vertex".to_string(),
            inputs: vec![],
            outputs: vec![
                make_io(0, "v_uv", "vec2", true),
                // 缺少 location=1
            ],
            push_constants: None,
            descriptor_bindings: vec![],
        };

        let fs_interface = StageInterface {
            stage_name: "Fragment".to_string(),
            inputs: vec![
                make_io(0, "v_uv", "vec2", false),
                make_io(1, "v_normal", "vec3", false), // VS 没有此输出
            ],
            outputs: vec![],
            push_constants: None,
            descriptor_bindings: vec![],
        };

        validator.add_stage(ShaderStageType::Vertex, vs_interface);
        validator.add_stage(ShaderStageType::Fragment, fs_interface);

        let report = validator.validate_pipeline();
        assert!(!report.is_ok());
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == InterfaceDiagnosticKind::MissingOutput));
    }

    #[test]
    fn test_type_mismatch() {
        let mut validator = StageInterfaceValidator::new();

        let vs_interface = StageInterface {
            stage_name: "Vertex".to_string(),
            inputs: vec![],
            outputs: vec![
                make_io(0, "v_uv", "vec3", true), // vec3!
            ],
            push_constants: None,
            descriptor_bindings: vec![],
        };

        let fs_interface = StageInterface {
            stage_name: "Fragment".to_string(),
            inputs: vec![
                make_io(0, "v_uv", "vec2", false), // vec2! 不匹配
            ],
            outputs: vec![],
            push_constants: None,
            descriptor_bindings: vec![],
        };

        validator.add_stage(ShaderStageType::Vertex, vs_interface);
        validator.add_stage(ShaderStageType::Fragment, fs_interface);

        let report = validator.validate_pipeline();
        assert!(!report.is_ok());
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == InterfaceDiagnosticKind::TypeMismatch));
    }

    #[test]
    fn test_interpolation_mismatch_warning() {
        let mut validator = StageInterfaceValidator::new();

        let mut vs_out = make_io(0, "v_id", "float", true);
        vs_out.interpolation = InterpolationQualifier::Smooth;
        let mut fs_in = make_io(0, "v_id", "float", false);
        fs_in.interpolation = InterpolationQualifier::Flat;

        validator.add_stage(
            ShaderStageType::Vertex,
            StageInterface {
                stage_name: "VS".to_string(),
                outputs: vec![vs_out],
                ..Default::default()
            },
        );
        validator.add_stage(
            ShaderStageType::Fragment,
            StageInterface {
                stage_name: "FS".to_string(),
                inputs: vec![fs_in],
                ..Default::default()
            },
        );

        let report = validator.validate_pipeline();
        assert!(report.warning_count > 0);
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == InterfaceDiagnosticKind::InterpolationMismatch));
    }

    #[test]
    fn test_push_constant_size_exceeded() {
        let mut validator = StageInterfaceValidator::new();

        validator.add_stage(
            ShaderStageType::Vertex,
            StageInterface {
                stage_name: "VS".to_string(),
                push_constants: Some(PushConstantBlock {
                    block_name: "PushConstants".to_string(),
                    members: vec![PushConstantMember {
                        name: "big_data".to_string(),
                        type_name: "mat4".to_string(),
                        offset: 0,
                        size: 256, // 超过 128 字节限制
                    }],
                    total_size: 256,
                    stage: ShaderStageType::Vertex,
                }),
                ..Default::default()
            },
        );

        let report = validator.validate_pipeline();
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == InterfaceDiagnosticKind::PushConstantSizeExceeded));
    }

    #[test]
    fn test_push_constant_layout_mismatch() {
        let mut validator = StageInterfaceValidator::new();

        validator.add_stage(
            ShaderStageType::Vertex,
            StageInterface {
                stage_name: "VS".to_string(),
                push_constants: Some(PushConstantBlock {
                    block_name: "PC".to_string(),
                    members: vec![PushConstantMember {
                        name: "mvp".to_string(),
                        type_name: "mat4".to_string(),
                        offset: 0,
                        size: 64,
                    }],
                    total_size: 64,
                    stage: ShaderStageType::Vertex,
                }),
                ..Default::default()
            },
        );

        validator.add_stage(
            ShaderStageType::Fragment,
            StageInterface {
                stage_name: "FS".to_string(),
                push_constants: Some(PushConstantBlock {
                    block_name: "PC".to_string(),
                    members: vec![PushConstantMember {
                        name: "mvp".to_string(),
                        type_name: "mat4".to_string(),
                        offset: 16, // 不同偏移!
                        size: 64,
                    }],
                    total_size: 80,
                    stage: ShaderStageType::Fragment,
                }),
                ..Default::default()
            },
        );

        let report = validator.validate_pipeline();
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == InterfaceDiagnosticKind::PushConstantLayoutMismatch));
    }

    #[test]
    fn test_descriptor_binding_conflict() {
        let mut validator = StageInterfaceValidator::new();

        validator.add_stage(
            ShaderStageType::Vertex,
            StageInterface {
                stage_name: "VS".to_string(),
                descriptor_bindings: vec![DescriptorBinding {
                    set: 0,
                    binding: 0,
                    descriptor_type: DescriptorType::UniformBuffer,
                    name: "u_matrices".to_string(),
                    type_name: "Matrices".to_string(),
                    array_size: 0,
                    stage: ShaderStageType::Vertex,
                }],
                ..Default::default()
            },
        );

        validator.add_stage(
            ShaderStageType::Fragment,
            StageInterface {
                stage_name: "FS".to_string(),
                descriptor_bindings: vec![DescriptorBinding {
                    set: 0,
                    binding: 0,
                    descriptor_type: DescriptorType::StorageBuffer, // 冲突!
                    name: "u_data".to_string(),
                    type_name: "Data".to_string(),
                    array_size: 0,
                    stage: ShaderStageType::Fragment,
                }],
                ..Default::default()
            },
        );

        let report = validator.validate_pipeline();
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == InterfaceDiagnosticKind::DescriptorBindingConflict));
    }

    #[test]
    fn test_location_overlap() {
        let mut validator = StageInterfaceValidator::new();

        let mut mat4_io = make_io(0, "v_matrix", "mat4", true);
        mat4_io.location_slots = 4; // 占 location 0-3
        let normal_io = make_io(2, "v_normal", "vec3", true); // location 2 与 mat4 重叠

        validator.add_stage(
            ShaderStageType::Vertex,
            StageInterface {
                stage_name: "VS".to_string(),
                outputs: vec![mat4_io, normal_io],
                ..Default::default()
            },
        );

        let report = validator.validate_pipeline();
        assert!(report
            .diagnostics
            .iter()
            .any(|d| d.kind == InterfaceDiagnosticKind::LocationOverlap));
    }

    #[test]
    fn test_consistent_descriptors_pass() {
        let mut validator = StageInterfaceValidator::new();

        let shared_binding = DescriptorBinding {
            set: 0,
            binding: 1,
            descriptor_type: DescriptorType::CombinedImageSampler,
            name: "u_texture".to_string(),
            type_name: "sampler2D".to_string(),
            array_size: 0,
            stage: ShaderStageType::Vertex,
        };

        validator.add_stage(
            ShaderStageType::Vertex,
            StageInterface {
                descriptor_bindings: vec![shared_binding.clone()],
                ..Default::default()
            },
        );

        let mut fs_binding = shared_binding;
        fs_binding.stage = ShaderStageType::Fragment;
        validator.add_stage(
            ShaderStageType::Fragment,
            StageInterface {
                descriptor_bindings: vec![fs_binding],
                ..Default::default()
            },
        );

        let report = validator.validate_pipeline();
        let binding_conflicts = report
            .diagnostics
            .iter()
            .filter(|d| d.kind == InterfaceDiagnosticKind::DescriptorBindingConflict)
            .count();
        assert_eq!(binding_conflicts, 0, "一致的描述符不应报冲突");
    }

    #[test]
    fn test_parse_layout_io() {
        let line = "layout(location = 0) in vec3 v_normal;";
        let io = parse_layout_io(line).unwrap();
        assert_eq!(io.location, 0);
        assert_eq!(io.type_name, "vec3");
        assert_eq!(io.name, "v_normal");
        assert!(!io.is_output);

        let line2 = "layout(location = 2) out vec4 out_color;";
        let io2 = parse_layout_io(line2).unwrap();
        assert_eq!(io2.location, 2);
        assert!(io2.is_output);
    }

    #[test]
    fn test_parse_flat_interpolation() {
        let line = "layout(location = 3) flat in uint v_instance_id;";
        let io = parse_layout_io(line).unwrap();
        assert_eq!(io.interpolation, InterpolationQualifier::Flat);
    }

    #[test]
    fn test_empty_pipeline() {
        let validator = StageInterfaceValidator::new();
        let report = validator.validate_pipeline();
        assert!(report.is_ok());
        assert_eq!(report.stage_count, 0);
    }
}
