// ============================================================
// 文件名称：spirv_optimizer.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：SPIR-V 指令级优化建议 — 对编译后的 SPIR-V 二进制
//           进行静态分析，识别冗余指令、向量化机会、常量折叠
//           遗漏、死代码等，生成可操作的优化建议。UE5 仅依赖
//           spirv-opt 做黑盒优化，缺乏可视化的优化诊断。
//           我们做到指令级透明分析 + 量化建议 + 优先级排序
// 功能描述：解析 SPIR-V 二进制 → 统计各类指令分布 → 检测
//           冗余模式 (连续标量运算可向量化、重复加载、未使用
//           的 ID、可折叠的常量表达式) → 生成优化建议列表
// 技术特性：SPIR-V 二进制解析、OpCode 模式匹配、指令分布
//           统计、向量化机会检测、常量折叠检测、死 ID 检测
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ SpirvOptimizer             │ SPIR-V 优化分析器             │
// │ SpirvInstruction           │ 解析后的 SPIR-V 指令          │
// │ InstructionStats           │ 指令分布统计                  │
// │ OptimizationHint           │ 优化建议                     │
// │ SpirvAnalysisReport        │ 分析报告                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建分析器                    │
// │ analyze()                  │ 分析 SPIR-V 二进制            │
// │ parse_instructions()       │ 解析指令流                    │
// │ detect_redundancies()      │ 检测冗余指令                  │
// │ detect_vectorization()     │ 检测向量化机会                │
// │ detect_constant_folding()  │ 检测常量折叠机会              │
// │ detect_dead_ids()          │ 检测死 ID                    │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};

// =============================================================================
// SPIR-V 常量
// =============================================================================

/// SPIR-V 魔数
const SPIRV_MAGIC: u32 = 0x07230203;
/// SPIR-V 头部大小 (word 数)
const SPIRV_HEADER_SIZE: usize = 5;

// 常用 OpCode
const OP_NOP: u16 = 0;
const OP_NAME: u16 = 5;
const OP_STRING: u16 = 7;
const OP_LINE: u16 = 8;
const OP_NO_LINE: u16 = 317;
const OP_TYPE_VOID: u16 = 19;
const OP_TYPE_BOOL: u16 = 20;
const OP_TYPE_INT: u16 = 21;
const OP_TYPE_FLOAT: u16 = 22;
const OP_TYPE_VECTOR: u16 = 23;
const OP_TYPE_MATRIX: u16 = 24;
const OP_TYPE_POINTER: u16 = 32;
const OP_TYPE_FUNCTION: u16 = 33;
const OP_CONSTANT: u16 = 43;
const OP_CONSTANT_COMPOSITE: u16 = 44;
const OP_VARIABLE: u16 = 59;
const OP_LOAD: u16 = 61;
const OP_STORE: u16 = 62;
const OP_ACCESS_CHAIN: u16 = 65;
const OP_FADD: u16 = 129;
const OP_FSUB: u16 = 131;
const OP_FMUL: u16 = 133;
const OP_FDIV: u16 = 136;
const OP_IADD: u16 = 128;
const OP_ISUB: u16 = 130;
const OP_IMUL: u16 = 132;
const OP_COMPOSITE_CONSTRUCT: u16 = 80;
const OP_COMPOSITE_EXTRACT: u16 = 81;
const OP_VECTOR_SHUFFLE: u16 = 79;
const OP_DOT: u16 = 148;
const OP_BRANCH: u16 = 249;
const OP_BRANCH_CONDITIONAL: u16 = 250;
const OP_RETURN: u16 = 253;
const OP_RETURN_VALUE: u16 = 254;

// =============================================================================
// SPIR-V 指令
// =============================================================================

/// 解析后的 SPIR-V 指令
#[derive(Debug, Clone)]
pub struct SpirvInstruction {
    /// OpCode
    pub opcode: u16,
    /// 字数 (含 opcode 字)
    pub word_count: u16,
    /// 结果 ID (如果有)
    pub result_id: Option<u32>,
    /// 结果类型 ID (如果有)
    pub result_type_id: Option<u32>,
    /// 操作数 (原始 u32)
    pub operands: Vec<u32>,
    /// 在指令流中的偏移 (word 索引)
    pub offset: usize,
}

impl SpirvInstruction {
    /// 获取 OpCode 的可读名称
    pub fn opcode_name(&self) -> &'static str {
        opcode_to_name(self.opcode)
    }

    /// 是否是算术指令
    pub fn is_arithmetic(&self) -> bool {
        matches!(
            self.opcode,
            OP_FADD | OP_FSUB | OP_FMUL | OP_FDIV | OP_IADD | OP_ISUB | OP_IMUL
        )
    }

    /// 是否是浮点算术
    pub fn is_float_arithmetic(&self) -> bool {
        matches!(self.opcode, OP_FADD | OP_FSUB | OP_FMUL | OP_FDIV)
    }

    /// 是否是类型定义
    pub fn is_type_definition(&self) -> bool {
        matches!(
            self.opcode,
            OP_TYPE_VOID
                | OP_TYPE_BOOL
                | OP_TYPE_INT
                | OP_TYPE_FLOAT
                | OP_TYPE_VECTOR
                | OP_TYPE_MATRIX
                | OP_TYPE_POINTER
                | OP_TYPE_FUNCTION
                | 19..=39
        )
    }

    /// 是否是调试指令
    pub fn is_debug(&self) -> bool {
        matches!(self.opcode, OP_NAME | OP_STRING | OP_LINE | OP_NO_LINE)
    }
}

// =============================================================================
// 指令统计
// =============================================================================

/// 指令分布统计
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct InstructionStats {
    /// 总指令数
    pub total_instructions: usize,
    /// 算术指令数
    pub arithmetic_count: usize,
    /// 内存访问指令数 (Load/Store)
    pub memory_count: usize,
    /// 控制流指令数 (Branch/Return)
    pub control_flow_count: usize,
    /// 类型定义指令数
    pub type_definition_count: usize,
    /// 常量定义指令数
    pub constant_count: usize,
    /// 调试/注解指令数
    pub debug_count: usize,
    /// Composite 操作数
    pub composite_count: usize,
    /// NOP 指令数
    pub nop_count: usize,
    /// OpCode 频率分布
    pub opcode_frequency: HashMap<u16, usize>,
    /// 唯一 ID 数
    pub unique_ids: usize,
    /// ID Bound
    pub id_bound: u32,
}

impl InstructionStats {
    /// 算术指令占比
    pub fn arithmetic_ratio(&self) -> f64 {
        if self.total_instructions == 0 {
            return 0.0;
        }
        self.arithmetic_count as f64 / self.total_instructions as f64
    }

    /// ID 利用率 (唯一 ID / Bound)
    pub fn id_utilization(&self) -> f64 {
        if self.id_bound == 0 {
            return 0.0;
        }
        self.unique_ids as f64 / self.id_bound as f64
    }
}

// =============================================================================
// 优化建议
// =============================================================================

/// 优化建议优先级
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub enum HintPriority {
    /// 低优先级 (微小收益)
    Low = 0,
    /// 中优先级 (明显收益)
    Medium = 1,
    /// 高优先级 (显著收益)
    High = 2,
    /// 关键 (严重性能问题)
    Critical = 3,
}

/// 优化建议类别
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum HintCategory {
    /// 冗余指令
    Redundancy,
    /// 向量化机会
    Vectorization,
    /// 常量折叠
    ConstantFolding,
    /// 死代码
    DeadCode,
    /// 内存访问
    MemoryAccess,
    /// 指令调度
    InstructionScheduling,
}

/// 优化建议
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OptimizationHint {
    /// 类别
    pub category: HintCategory,
    /// 优先级
    pub priority: HintPriority,
    /// 描述
    pub description: String,
    /// 位置 (指令偏移)
    pub location: Option<usize>,
    /// 涉及的指令数
    pub affected_instructions: usize,
    /// 预估节省的指令数
    pub estimated_savings: usize,
}

// =============================================================================
// 分析报告
// =============================================================================

/// SPIR-V 分析报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SpirvAnalysisReport {
    /// 着色器名称
    pub shader_name: String,
    /// SPIR-V 大小 (字节)
    pub spirv_size_bytes: usize,
    /// 指令统计
    pub stats: InstructionStats,
    /// 优化建议列表
    pub hints: Vec<OptimizationHint>,
    /// 优化潜力评分 (0-100, 越高越有优化空间)
    pub optimization_potential: f64,
    /// 预估优化后大小减少百分比
    pub estimated_size_reduction_percent: f64,
}

impl SpirvAnalysisReport {
    /// 按优先级排序的建议
    pub fn hints_by_priority(&self) -> Vec<&OptimizationHint> {
        let mut sorted: Vec<&OptimizationHint> = self.hints.iter().collect();
        sorted.sort_by(|a, b| b.priority.cmp(&a.priority));
        sorted
    }

    /// 导出 Markdown 报告
    pub fn to_markdown(&self) -> String {
        let mut md = String::with_capacity(2048);
        md.push_str(&format!("# SPIR-V 优化分析: {}\n\n", self.shader_name));

        md.push_str("## 概要\n\n");
        md.push_str("| 指标 | 值 |\n|------|----|\n");
        md.push_str(&format!("| 大小 | {} 字节 |\n", self.spirv_size_bytes));
        md.push_str(&format!(
            "| 总指令数 | {} |\n",
            self.stats.total_instructions
        ));
        md.push_str(&format!(
            "| 算术指令 | {} ({:.1}%) |\n",
            self.stats.arithmetic_count,
            self.stats.arithmetic_ratio() * 100.0,
        ));
        md.push_str(&format!("| 内存访问 | {} |\n", self.stats.memory_count));
        md.push_str(&format!(
            "| ID 利用率 | {:.1}% |\n",
            self.stats.id_utilization() * 100.0,
        ));
        md.push_str(&format!(
            "| 优化潜力 | **{:.0}/100** |\n",
            self.optimization_potential,
        ));
        md.push_str(&format!(
            "| 预估大小减少 | {:.1}% |\n\n",
            self.estimated_size_reduction_percent,
        ));

        if !self.hints.is_empty() {
            md.push_str("## 优化建议\n\n");
            md.push_str("| 优先级 | 类别 | 描述 | 影响指令数 | 预估节省 |\n");
            md.push_str("|--------|------|------|------------|----------|\n");
            for hint in self.hints_by_priority() {
                let prio = match hint.priority {
                    HintPriority::Critical => "🔴 关键",
                    HintPriority::High => "🟠 高",
                    HintPriority::Medium => "🟡 中",
                    HintPriority::Low => "🟢 低",
                };
                let cat = match hint.category {
                    HintCategory::Redundancy => "冗余",
                    HintCategory::Vectorization => "向量化",
                    HintCategory::ConstantFolding => "常量折叠",
                    HintCategory::DeadCode => "死代码",
                    HintCategory::MemoryAccess => "内存",
                    HintCategory::InstructionScheduling => "调度",
                };
                md.push_str(&format!(
                    "| {} | {} | {} | {} | {} |\n",
                    prio, cat, hint.description, hint.affected_instructions, hint.estimated_savings,
                ));
            }
        }

        md
    }
}

// =============================================================================
// SPIR-V 优化分析器
// =============================================================================

/// SPIR-V 优化分析器
pub struct SpirvOptimizer;

impl SpirvOptimizer {
    /// 创建分析器
    pub fn new() -> Self {
        Self
    }

    /// 分析 SPIR-V 二进制
    pub fn analyze(&self, shader_name: &str, spirv: &[u8]) -> Result<SpirvAnalysisReport, String> {
        if spirv.len() < SPIRV_HEADER_SIZE * 4 {
            return Err("SPIR-V 数据太小".to_string());
        }
        if spirv.len() % 4 != 0 {
            return Err("SPIR-V 长度非 4 的倍数".to_string());
        }

        let words: Vec<u32> = spirv
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();

        if words[0] != SPIRV_MAGIC {
            return Err(format!("无效 SPIR-V 魔数: 0x{:08X}", words[0]));
        }

        let id_bound = words[3];
        let instructions = parse_instructions(&words);
        let stats = compute_stats(&instructions, id_bound);

        let mut hints = Vec::new();
        detect_nop_redundancy(&instructions, &mut hints);
        detect_debug_overhead(&stats, &mut hints);
        detect_consecutive_scalar_ops(&instructions, &mut hints);
        detect_repeated_loads(&instructions, &mut hints);
        detect_dead_ids(&instructions, &mut hints);
        detect_id_bound_waste(&stats, &mut hints);

        // 按优先级排序
        hints.sort_by(|a, b| b.priority.cmp(&a.priority));

        let total_savings: usize = hints.iter().map(|h| h.estimated_savings).sum();
        let potential = if stats.total_instructions > 0 {
            (total_savings as f64 / stats.total_instructions as f64 * 100.0).min(100.0)
        } else {
            0.0
        };
        let size_reduction = if stats.total_instructions > 0 {
            total_savings as f64 / stats.total_instructions as f64 * 100.0
        } else {
            0.0
        };

        Ok(SpirvAnalysisReport {
            shader_name: shader_name.to_string(),
            spirv_size_bytes: spirv.len(),
            stats,
            hints,
            optimization_potential: potential,
            estimated_size_reduction_percent: size_reduction,
        })
    }
}

// =============================================================================
// 解析和检测函数
// =============================================================================

/// 解析 SPIR-V 指令流
pub fn parse_instructions(words: &[u32]) -> Vec<SpirvInstruction> {
    let mut instructions = Vec::new();
    let mut offset = SPIRV_HEADER_SIZE;

    while offset < words.len() {
        let word = words[offset];
        let word_count = (word >> 16) as u16;
        let opcode = (word & 0xFFFF) as u16;

        if word_count == 0 || offset + word_count as usize > words.len() {
            break;
        }

        let operands: Vec<u32> = words[offset + 1..offset + word_count as usize].to_vec();

        // 检测 result_type_id 和 result_id
        let (result_type_id, result_id) = extract_result_ids(opcode, &operands);

        instructions.push(SpirvInstruction {
            opcode,
            word_count,
            result_id,
            result_type_id,
            operands,
            offset,
        });

        offset += word_count as usize;
    }

    instructions
}

/// 提取结果类型 ID 和结果 ID
fn extract_result_ids(opcode: u16, operands: &[u32]) -> (Option<u32>, Option<u32>) {
    // 大多数产生结果的指令格式: OpCode ResultTypeID ResultID ...
    match opcode {
        // 无结果的指令
        OP_NOP
        | OP_NAME
        | OP_STRING
        | OP_LINE
        | OP_NO_LINE
        | OP_STORE
        | OP_BRANCH
        | OP_BRANCH_CONDITIONAL
        | OP_RETURN => (None, None),
        // 仅有结果 ID (类型定义指令)
        OP_TYPE_VOID | OP_TYPE_BOOL | OP_TYPE_INT | OP_TYPE_FLOAT | OP_TYPE_VECTOR
        | OP_TYPE_MATRIX | OP_TYPE_POINTER | OP_TYPE_FUNCTION => {
            if !operands.is_empty() {
                (None, Some(operands[0]))
            } else {
                (None, None)
            }
        }
        // 有结果类型 + 结果 ID
        _ => {
            if operands.len() >= 2 {
                (Some(operands[0]), Some(operands[1]))
            } else if operands.len() == 1 {
                (None, Some(operands[0]))
            } else {
                (None, None)
            }
        }
    }
}

/// 计算指令统计
fn compute_stats(instructions: &[SpirvInstruction], id_bound: u32) -> InstructionStats {
    let mut stats = InstructionStats {
        id_bound,
        ..Default::default()
    };

    let mut defined_ids = HashSet::new();

    for inst in instructions {
        stats.total_instructions += 1;
        *stats.opcode_frequency.entry(inst.opcode).or_insert(0) += 1;

        if let Some(id) = inst.result_id {
            defined_ids.insert(id);
        }

        if inst.is_arithmetic() {
            stats.arithmetic_count += 1;
        }
        if matches!(inst.opcode, OP_LOAD | OP_STORE | OP_ACCESS_CHAIN) {
            stats.memory_count += 1;
        }
        if matches!(
            inst.opcode,
            OP_BRANCH | OP_BRANCH_CONDITIONAL | OP_RETURN | OP_RETURN_VALUE
        ) {
            stats.control_flow_count += 1;
        }
        if inst.is_type_definition() {
            stats.type_definition_count += 1;
        }
        if matches!(inst.opcode, OP_CONSTANT | OP_CONSTANT_COMPOSITE) {
            stats.constant_count += 1;
        }
        if inst.is_debug() {
            stats.debug_count += 1;
        }
        if matches!(
            inst.opcode,
            OP_COMPOSITE_CONSTRUCT | OP_COMPOSITE_EXTRACT | OP_VECTOR_SHUFFLE
        ) {
            stats.composite_count += 1;
        }
        if inst.opcode == OP_NOP {
            stats.nop_count += 1;
        }
    }

    stats.unique_ids = defined_ids.len();
    stats
}

/// 检测 NOP 冗余
fn detect_nop_redundancy(instructions: &[SpirvInstruction], hints: &mut Vec<OptimizationHint>) {
    let nop_count = instructions.iter().filter(|i| i.opcode == OP_NOP).count();
    if nop_count > 0 {
        hints.push(OptimizationHint {
            category: HintCategory::Redundancy,
            priority: HintPriority::Low,
            description: format!("发现 {} 条 OpNop 指令, 可安全移除", nop_count),
            location: None,
            affected_instructions: nop_count,
            estimated_savings: nop_count,
        });
    }
}

/// 检测调试信息开销
fn detect_debug_overhead(stats: &InstructionStats, hints: &mut Vec<OptimizationHint>) {
    if stats.debug_count > 0 && stats.total_instructions > 0 {
        let ratio = stats.debug_count as f64 / stats.total_instructions as f64;
        if ratio > 0.1 {
            hints.push(OptimizationHint {
                category: HintCategory::Redundancy,
                priority: HintPriority::Medium,
                description: format!(
                    "调试/注解指令占 {:.1}% ({} 条), 发布构建可移除",
                    ratio * 100.0,
                    stats.debug_count,
                ),
                location: None,
                affected_instructions: stats.debug_count,
                estimated_savings: stats.debug_count,
            });
        }
    }
}

/// 检测连续标量运算 (向量化机会)
fn detect_consecutive_scalar_ops(
    instructions: &[SpirvInstruction],
    hints: &mut Vec<OptimizationHint>,
) {
    let mut consecutive_count = 0usize;
    let mut last_opcode: Option<u16> = None;
    let mut opportunities = 0usize;
    let mut total_affected = 0usize;

    for inst in instructions {
        if inst.is_float_arithmetic() {
            if last_opcode == Some(inst.opcode) {
                consecutive_count += 1;
            } else {
                if consecutive_count >= 3 {
                    opportunities += 1;
                    total_affected += consecutive_count + 1;
                }
                consecutive_count = 0;
            }
            last_opcode = Some(inst.opcode);
        } else {
            if consecutive_count >= 3 {
                opportunities += 1;
                total_affected += consecutive_count + 1;
            }
            consecutive_count = 0;
            last_opcode = None;
        }
    }

    // 处理尾部
    if consecutive_count >= 3 {
        opportunities += 1;
        total_affected += consecutive_count + 1;
    }

    if opportunities > 0 {
        hints.push(OptimizationHint {
            category: HintCategory::Vectorization,
            priority: HintPriority::High,
            description: format!(
                "发现 {} 处连续标量浮点运算 (共 {} 条), 可合并为向量运算",
                opportunities, total_affected,
            ),
            location: None,
            affected_instructions: total_affected,
            estimated_savings: total_affected * 3 / 4, // 向量化约省 75%
        });
    }
}

/// 检测重复加载
fn detect_repeated_loads(instructions: &[SpirvInstruction], hints: &mut Vec<OptimizationHint>) {
    // 统计相同指针的 Load 次数
    let mut load_sources: HashMap<u32, usize> = HashMap::new();

    for inst in instructions {
        if inst.opcode == OP_LOAD && inst.operands.len() >= 3 {
            // operands[2] 是指针操作数
            *load_sources.entry(inst.operands[2]).or_insert(0) += 1;
        }
    }

    let repeated: usize = load_sources
        .values()
        .filter(|&&c| c > 1)
        .map(|c| c - 1)
        .sum();
    if repeated > 0 {
        let sources_count = load_sources.values().filter(|&&c| c > 1).count();
        hints.push(OptimizationHint {
            category: HintCategory::MemoryAccess,
            priority: HintPriority::Medium,
            description: format!(
                "发现 {} 个指针的重复 Load (共 {} 次冗余), 可缓存到局部变量",
                sources_count, repeated,
            ),
            location: None,
            affected_instructions: repeated,
            estimated_savings: repeated,
        });
    }
}

/// 检测死 ID (定义了但从未被引用)
fn detect_dead_ids(instructions: &[SpirvInstruction], hints: &mut Vec<OptimizationHint>) {
    let mut defined_ids: HashSet<u32> = HashSet::new();
    let mut referenced_ids: HashSet<u32> = HashSet::new();

    for inst in instructions {
        if let Some(id) = inst.result_id {
            defined_ids.insert(id);
        }
        // 所有操作数中引用的 ID (排除自身结果 ID 和结果类型 ID)
        let skip = match (inst.result_type_id, inst.result_id) {
            (Some(_), Some(_)) => 2,
            (Some(_), None) | (None, Some(_)) => 1,
            (None, None) => 0,
        };
        for &op in inst.operands.iter().skip(skip) {
            if op > 0 && op < u32::MAX / 2 {
                referenced_ids.insert(op);
            }
        }
    }

    // 类型定义和常量通常不直接引用自身，但被操作数引用
    let dead: Vec<u32> = defined_ids
        .iter()
        .filter(|id| !referenced_ids.contains(id))
        .copied()
        .collect();

    // 排除入口点和类型定义 (它们可能仅被隐式引用)
    let dead_count = dead.len();
    if dead_count > 2 {
        hints.push(OptimizationHint {
            category: HintCategory::DeadCode,
            priority: if dead_count > 10 {
                HintPriority::High
            } else {
                HintPriority::Medium
            },
            description: format!("发现约 {} 个定义但未被引用的 ID, 可能是死代码", dead_count,),
            location: None,
            affected_instructions: dead_count,
            estimated_savings: dead_count,
        });
    }
}

/// 检测 ID Bound 浪费
fn detect_id_bound_waste(stats: &InstructionStats, hints: &mut Vec<OptimizationHint>) {
    let utilization = stats.id_utilization();
    if utilization > 0.0 && utilization < 0.5 && stats.id_bound > 100 {
        let wasted = stats.id_bound as usize - stats.unique_ids;
        hints.push(OptimizationHint {
            category: HintCategory::Redundancy,
            priority: HintPriority::Low,
            description: format!(
                "ID Bound {} 但仅使用 {} 个 ({:.1}% 利用率), 重新编号可减小文件",
                stats.id_bound,
                stats.unique_ids,
                utilization * 100.0,
            ),
            location: None,
            affected_instructions: 0,
            estimated_savings: wasted / 10, // 间接节省
        });
    }
}

/// OpCode 转名称
fn opcode_to_name(opcode: u16) -> &'static str {
    match opcode {
        0 => "OpNop",
        5 => "OpName",
        7 => "OpString",
        8 => "OpLine",
        11 => "OpExtInstImport",
        12 => "OpExtInst",
        14 => "OpMemoryModel",
        15 => "OpEntryPoint",
        16 => "OpExecutionMode",
        17 => "OpCapability",
        19 => "OpTypeVoid",
        20 => "OpTypeBool",
        21 => "OpTypeInt",
        22 => "OpTypeFloat",
        23 => "OpTypeVector",
        24 => "OpTypeMatrix",
        32 => "OpTypePointer",
        33 => "OpTypeFunction",
        43 => "OpConstant",
        44 => "OpConstantComposite",
        54 => "OpFunction",
        56 => "OpFunctionEnd",
        59 => "OpVariable",
        61 => "OpLoad",
        62 => "OpStore",
        65 => "OpAccessChain",
        71 => "OpDecorate",
        72 => "OpMemberDecorate",
        79 => "OpVectorShuffle",
        80 => "OpCompositeConstruct",
        81 => "OpCompositeExtract",
        128 => "OpIAdd",
        129 => "OpFAdd",
        130 => "OpISub",
        131 => "OpFSub",
        132 => "OpIMul",
        133 => "OpFMul",
        136 => "OpFDiv",
        148 => "OpDot",
        249 => "OpBranch",
        250 => "OpBranchConditional",
        253 => "OpReturn",
        254 => "OpReturnValue",
        317 => "OpNoLine",
        _ => "OpUnknown",
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    /// 构造最小 SPIR-V 二进制
    fn make_spirv(extra_words: &[u32]) -> Vec<u8> {
        let id_bound = 100u32;
        let mut words = vec![
            SPIRV_MAGIC,
            0x00010300, // 版本 1.3
            0,          // 生成器
            id_bound,   // ID bound
            0,          // 保留
        ];
        words.extend_from_slice(extra_words);
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    /// 编码指令 (opcode, operands)
    fn encode_inst(opcode: u16, operands: &[u32]) -> Vec<u32> {
        let word_count = (operands.len() + 1) as u32;
        let first = (word_count << 16) | opcode as u32;
        let mut result = vec![first];
        result.extend_from_slice(operands);
        result
    }

    #[test]
    fn test_parse_minimal_spirv() {
        let spirv = make_spirv(&[]);
        let optimizer = SpirvOptimizer::new();
        let report = optimizer.analyze("test.spv", &spirv).unwrap();
        assert_eq!(report.stats.total_instructions, 0);
    }

    #[test]
    fn test_parse_instructions() {
        let mut extra = Vec::new();
        extra.extend(encode_inst(OP_TYPE_VOID, &[1])); // OpTypeVoid %1
        extra.extend(encode_inst(OP_TYPE_FLOAT, &[2, 32])); // OpTypeFloat %2 32
        extra.extend(encode_inst(OP_CONSTANT, &[2, 3, 0x3F800000])); // OpConstant %2 %3 1.0f

        let spirv = make_spirv(&extra);
        let words: Vec<u32> = spirv
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();

        let instructions = parse_instructions(&words);
        assert_eq!(instructions.len(), 3);
        assert_eq!(instructions[0].opcode, OP_TYPE_VOID);
        assert_eq!(instructions[1].opcode, OP_TYPE_FLOAT);
        assert_eq!(instructions[2].opcode, OP_CONSTANT);
    }

    #[test]
    fn test_detect_nop() {
        let mut extra = Vec::new();
        extra.extend(encode_inst(OP_NOP, &[]));
        extra.extend(encode_inst(OP_NOP, &[]));
        extra.extend(encode_inst(OP_NOP, &[]));

        let spirv = make_spirv(&extra);
        let optimizer = SpirvOptimizer::new();
        let report = optimizer.analyze("nop.spv", &spirv).unwrap();

        assert_eq!(report.stats.nop_count, 3);
        assert!(report
            .hints
            .iter()
            .any(|h| h.category == HintCategory::Redundancy && h.description.contains("OpNop")));
    }

    #[test]
    fn test_detect_debug_overhead() {
        let mut extra = Vec::new();
        // 添加 12 条调试指令和 8 条其他指令 (60% 调试)
        for _ in 0..12 {
            extra.extend(encode_inst(OP_NAME, &[1, 0x74736574])); // OpName %1 "test"
        }
        for _ in 0..8 {
            extra.extend(encode_inst(OP_NOP, &[]));
        }

        let spirv = make_spirv(&extra);
        let optimizer = SpirvOptimizer::new();
        let report = optimizer.analyze("debug.spv", &spirv).unwrap();

        assert!(report.stats.debug_count >= 12);
        assert!(report
            .hints
            .iter()
            .any(|h| h.category == HintCategory::Redundancy && h.description.contains("调试")));
    }

    #[test]
    fn test_detect_consecutive_scalar() {
        let mut extra = Vec::new();
        // 类型定义
        extra.extend(encode_inst(OP_TYPE_FLOAT, &[1, 32]));
        // 4 个连续 FAdd (向量化机会)
        for i in 0..4u32 {
            extra.extend(encode_inst(OP_FADD, &[1, 10 + i, 20 + i, 30 + i]));
        }

        let spirv = make_spirv(&extra);
        let optimizer = SpirvOptimizer::new();
        let report = optimizer.analyze("scalar.spv", &spirv).unwrap();

        assert!(
            report
                .hints
                .iter()
                .any(|h| h.category == HintCategory::Vectorization),
            "应检测到向量化机会"
        );
    }

    #[test]
    fn test_detect_repeated_loads() {
        let mut extra = Vec::new();
        extra.extend(encode_inst(OP_TYPE_FLOAT, &[1, 32]));
        // 从同一指针多次 Load
        extra.extend(encode_inst(OP_LOAD, &[1, 10, 50]));
        extra.extend(encode_inst(OP_LOAD, &[1, 11, 50]));
        extra.extend(encode_inst(OP_LOAD, &[1, 12, 50]));

        let spirv = make_spirv(&extra);
        let optimizer = SpirvOptimizer::new();
        let report = optimizer.analyze("loads.spv", &spirv).unwrap();

        assert!(report.hints.iter().any(
            |h| h.category == HintCategory::MemoryAccess && h.description.contains("重复 Load")
        ));
    }

    #[test]
    fn test_instruction_stats() {
        let mut extra = Vec::new();
        extra.extend(encode_inst(OP_TYPE_FLOAT, &[1, 32]));
        extra.extend(encode_inst(OP_CONSTANT, &[1, 2, 0]));
        extra.extend(encode_inst(OP_FADD, &[1, 3, 2, 2]));
        extra.extend(encode_inst(OP_LOAD, &[1, 4, 5]));
        extra.extend(encode_inst(OP_STORE, &[5, 3]));
        extra.extend(encode_inst(OP_BRANCH, &[10]));

        let spirv = make_spirv(&extra);
        let optimizer = SpirvOptimizer::new();
        let report = optimizer.analyze("stats.spv", &spirv).unwrap();

        assert_eq!(report.stats.total_instructions, 6);
        assert_eq!(report.stats.arithmetic_count, 1);
        assert!(report.stats.memory_count >= 2);
        assert!(report.stats.control_flow_count >= 1);
    }

    #[test]
    fn test_opcode_name() {
        assert_eq!(opcode_to_name(OP_NOP), "OpNop");
        assert_eq!(opcode_to_name(OP_FADD), "OpFAdd");
        assert_eq!(opcode_to_name(OP_LOAD), "OpLoad");
        assert_eq!(opcode_to_name(9999), "OpUnknown");
    }

    #[test]
    fn test_report_markdown() {
        let mut extra = Vec::new();
        extra.extend(encode_inst(OP_NOP, &[]));
        extra.extend(encode_inst(OP_TYPE_FLOAT, &[1, 32]));

        let spirv = make_spirv(&extra);
        let optimizer = SpirvOptimizer::new();
        let report = optimizer.analyze("test.spv", &spirv).unwrap();
        let md = report.to_markdown();

        assert!(md.contains("# SPIR-V 优化分析: test.spv"));
        assert!(md.contains("总指令数"));
    }

    #[test]
    fn test_invalid_spirv() {
        let optimizer = SpirvOptimizer::new();
        assert!(optimizer.analyze("bad", &[0, 1, 2, 3]).is_err());
        assert!(optimizer.analyze("small", &[1, 2, 3]).is_err());
    }

    #[test]
    fn test_hints_by_priority() {
        let report = SpirvAnalysisReport {
            shader_name: "test".to_string(),
            spirv_size_bytes: 100,
            stats: InstructionStats::default(),
            hints: vec![
                OptimizationHint {
                    category: HintCategory::Redundancy,
                    priority: HintPriority::Low,
                    description: "低".to_string(),
                    location: None,
                    affected_instructions: 1,
                    estimated_savings: 1,
                },
                OptimizationHint {
                    category: HintCategory::Vectorization,
                    priority: HintPriority::High,
                    description: "高".to_string(),
                    location: None,
                    affected_instructions: 5,
                    estimated_savings: 3,
                },
            ],
            optimization_potential: 10.0,
            estimated_size_reduction_percent: 5.0,
        };

        let sorted = report.hints_by_priority();
        assert_eq!(sorted[0].priority, HintPriority::High);
        assert_eq!(sorted[1].priority, HintPriority::Low);
    }

    #[test]
    fn test_instruction_methods() {
        let inst = SpirvInstruction {
            opcode: OP_FADD,
            word_count: 5,
            result_id: Some(3),
            result_type_id: Some(1),
            operands: vec![1, 3, 4, 5],
            offset: 10,
        };

        assert!(inst.is_arithmetic());
        assert!(inst.is_float_arithmetic());
        assert!(!inst.is_type_definition());
        assert!(!inst.is_debug());
        assert_eq!(inst.opcode_name(), "OpFAdd");
    }
}
