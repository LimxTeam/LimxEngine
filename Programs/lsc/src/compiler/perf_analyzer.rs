// ============================================================
// 文件名称：perf_analyzer.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：着色器性能静态分析 — 在编译阶段即给出指令数、
//           寄存器压力、纹理采样负载、分支复杂度等性能指标，
//           UE5 没有此类内建工具，开发者只能依赖 GPU 厂商
//           的离线分析器 (RenderDoc/Nsight)，我们做到编译时
//           即时反馈
// 功能描述：着色器性能静态分析器 — GLSL/HLSL 源码级别的
//           指令估算、ALU/TEX/流控分类、寄存器压力预估、
//           纹理采样热点检测、分支发散风险评估、复杂度评分
// 技术特性：基于正则的 GLSL/HLSL 指令分类器、启发式寄存器
//           压力模型、加权复杂度评分算法
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ ShaderPerfAnalyzer         │ 着色器性能分析器主体           │
// │ PerfReport                 │ 性能分析报告                  │
// │ InstructionStats           │ 指令统计                     │
// │ RegisterPressure           │ 寄存器压力预估                │
// │ TextureSamplingInfo        │ 纹理采样信息                  │
// │ BranchComplexity           │ 分支复杂度信息                │
// │ PerfWarning                │ 性能警告                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建分析器                    │
// │ analyze_source()           │ 分析着色器源码                │
// │ estimate_instructions()    │ 估算指令数                    │
// │ estimate_register_pressure()│ 估算寄存器压力               │
// │ analyze_texture_sampling() │ 分析纹理采样                  │
// │ analyze_branches()         │ 分析分支复杂度                │
// │ compute_complexity_score() │ 计算复杂度评分                │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};

// =============================================================================
// 指令统计
// =============================================================================

/// 指令类别统计
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct InstructionStats {
    /// 算术/逻辑指令数 (加减乘除、比较、位运算)
    pub alu_count: usize,
    /// 纹理采样指令数
    pub texture_count: usize,
    /// 流控指令数 (分支、循环)
    pub flow_control_count: usize,
    /// 内存加载/存储指令数
    pub memory_count: usize,
    /// 特殊函数指令数 (sin, cos, exp, pow 等)
    pub special_func_count: usize,
    /// 矩阵运算指令数
    pub matrix_count: usize,
    /// 原子操作指令数
    pub atomic_count: usize,
    /// 屏障/同步指令数
    pub barrier_count: usize,
    /// 总估算指令数
    pub total_estimated: usize,
}

impl InstructionStats {
    /// 计算 ALU:TEX 比率
    pub fn alu_tex_ratio(&self) -> f64 {
        if self.texture_count == 0 {
            return f64::INFINITY;
        }
        self.alu_count as f64 / self.texture_count as f64
    }
}

// =============================================================================
// 寄存器压力
// =============================================================================

/// 寄存器压力预估
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct RegisterPressure {
    /// 估算的标量寄存器使用数
    pub scalar_registers: usize,
    /// 估算的向量寄存器使用数
    pub vector_registers: usize,
    /// 局部变量数
    pub local_variable_count: usize,
    /// uniform/constant 变量数
    pub uniform_count: usize,
    /// 输入 varying 数
    pub input_varying_count: usize,
    /// 输出 varying 数
    pub output_varying_count: usize,
    /// 临时变量估算数
    pub temp_register_estimate: usize,
    /// 压力等级 (0=低, 1=中, 2=高, 3=极高)
    pub pressure_level: u8,
    /// 是否可能导致寄存器溢出 (register spilling)
    pub likely_spilling: bool,
}

// =============================================================================
// 纹理采样信息
// =============================================================================

/// 纹理采样信息
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TextureSamplingInfo {
    /// 采样器数量
    pub sampler_count: usize,
    /// 纹理采样调用总数
    pub sample_call_count: usize,
    /// 带偏移的采样数 (textureOffset)
    pub offset_sample_count: usize,
    /// LOD 偏置采样数 (textureLod)
    pub lod_sample_count: usize,
    /// 梯度采样数 (textureGrad)
    pub grad_sample_count: usize,
    /// 投影采样数 (textureProj)
    pub proj_sample_count: usize,
    /// 在循环内的采样数
    pub loop_sample_count: usize,
    /// 依赖纹理读取数 (用一个采样结果的 UV 去做下一个采样)
    pub dependent_read_count: usize,
}

// =============================================================================
// 分支复杂度
// =============================================================================

/// 分支复杂度信息
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct BranchComplexity {
    /// if 语句数
    pub if_count: usize,
    /// if-else 语句数
    pub if_else_count: usize,
    /// for 循环数
    pub for_loop_count: usize,
    /// while 循环数
    pub while_loop_count: usize,
    /// 循环嵌套最大深度
    pub max_loop_nesting: usize,
    /// 分支嵌套最大深度
    pub max_branch_nesting: usize,
    /// 动态分支数 (基于非 uniform 条件的 if)
    pub dynamic_branch_count: usize,
    /// discard/clip 调用数
    pub discard_count: usize,
    /// 三元运算符数
    pub ternary_count: usize,
}

// =============================================================================
// 性能警告
// =============================================================================

/// 警告严重程度
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub enum WarningSeverity {
    /// 信息
    Info,
    /// 低
    Low,
    /// 中
    Medium,
    /// 高
    High,
    /// 严重
    Critical,
}

/// 性能警告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PerfWarning {
    /// 严重程度
    pub severity: WarningSeverity,
    /// 警告消息
    pub message: String,
    /// 优化建议
    pub suggestion: String,
    /// 相关行号 (若可确定)
    pub line_number: Option<usize>,
}

// =============================================================================
// 性能报告
// =============================================================================

/// 着色器性能分析报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PerfReport {
    /// 着色器名称
    pub shader_name: String,
    /// 源代码行数
    pub source_lines: usize,
    /// 指令统计
    pub instructions: InstructionStats,
    /// 寄存器压力
    pub register_pressure: RegisterPressure,
    /// 纹理采样信息
    pub texture_sampling: TextureSamplingInfo,
    /// 分支复杂度
    pub branch_complexity: BranchComplexity,
    /// 性能警告列表
    pub warnings: Vec<PerfWarning>,
    /// 复杂度评分 (0~100, 越低越好)
    pub complexity_score: f64,
    /// 预估 GPU 周期数 (非精确，仅供比较)
    pub estimated_gpu_cycles: usize,
    /// 性能等级 (A/B/C/D/F)
    pub performance_grade: char,
}

// =============================================================================
// 着色器性能分析器
// =============================================================================

/// 着色器性能静态分析器
pub struct ShaderPerfAnalyzer;

impl ShaderPerfAnalyzer {
    /// 创建分析器
    pub fn new() -> Self {
        Self
    }

    /// 分析着色器源码
    pub fn analyze_source(&self, shader_name: &str, source: &str) -> PerfReport {
        let source_lines = source.lines().count();
        let instructions = self.estimate_instructions(source);
        let register_pressure = self.estimate_register_pressure(source);
        let texture_sampling = self.analyze_texture_sampling(source);
        let branch_complexity = self.analyze_branches(source);
        let warnings = self.generate_warnings(
            &instructions,
            &register_pressure,
            &texture_sampling,
            &branch_complexity,
        );
        let complexity_score = self.compute_complexity_score(
            &instructions,
            &register_pressure,
            &texture_sampling,
            &branch_complexity,
        );
        let estimated_gpu_cycles = self.estimate_gpu_cycles(&instructions, &texture_sampling);
        let performance_grade = score_to_grade(complexity_score);

        PerfReport {
            shader_name: shader_name.to_string(),
            source_lines,
            instructions,
            register_pressure,
            texture_sampling,
            branch_complexity,
            warnings,
            complexity_score,
            estimated_gpu_cycles,
            performance_grade,
        }
    }

    /// 估算指令数
    fn estimate_instructions(&self, source: &str) -> InstructionStats {
        let mut stats = InstructionStats::default();

        for line in source.lines() {
            let trimmed = line.trim();

            // 跳过注释和空行
            if trimmed.is_empty()
                || trimmed.starts_with("//")
                || trimmed.starts_with("/*")
                || trimmed.starts_with('*')
            {
                continue;
            }

            // ALU 操作
            stats.alu_count += count_occurrences(
                trimmed,
                &["+", "-", "*", "/", "%", "&", "|", "^", "~", "<<", ">>"],
            );
            stats.alu_count += count_func_calls(
                trimmed,
                &[
                    "abs",
                    "sign",
                    "floor",
                    "ceil",
                    "fract",
                    "mod",
                    "min",
                    "max",
                    "clamp",
                    "mix",
                    "step",
                    "smoothstep",
                    "length",
                    "distance",
                    "dot",
                    "cross",
                    "normalize",
                    "reflect",
                    "refract",
                ],
            );

            // 特殊数学函数 (通常更昂贵)
            stats.special_func_count += count_func_calls(
                trimmed,
                &[
                    "sin",
                    "cos",
                    "tan",
                    "asin",
                    "acos",
                    "atan",
                    "exp",
                    "exp2",
                    "log",
                    "log2",
                    "pow",
                    "sqrt",
                    "inversesqrt",
                    "sinh",
                    "cosh",
                    "tanh",
                ],
            );

            // 纹理采样
            stats.texture_count += count_func_calls(
                trimmed,
                &[
                    "texture",
                    "textureLod",
                    "textureGrad",
                    "textureOffset",
                    "texelFetch",
                    "textureProjLod",
                    "textureProj",
                    "textureSample",
                    "SampleLevel",
                    "SampleGrad",
                    "Sample",
                    "imageLoad",
                    "imageStore",
                ],
            );

            // 矩阵运算
            stats.matrix_count += count_func_calls(
                trimmed,
                &[
                    "matrixCompMult",
                    "outerProduct",
                    "transpose",
                    "determinant",
                    "inverse",
                    "mul",
                ],
            );
            // mat * vec 也算矩阵运算
            if (trimmed.contains("mat")
                || trimmed.contains("float4x4")
                || trimmed.contains("float3x3"))
                && trimmed.contains('*')
            {
                stats.matrix_count += 1;
            }

            // 原子操作
            stats.atomic_count += count_func_calls(
                trimmed,
                &[
                    "atomicAdd",
                    "atomicMin",
                    "atomicMax",
                    "atomicAnd",
                    "atomicOr",
                    "atomicXor",
                    "atomicExchange",
                    "atomicCompSwap",
                    "InterlockedAdd",
                    "InterlockedMin",
                    "InterlockedMax",
                ],
            );

            // 屏障
            stats.barrier_count += count_func_calls(
                trimmed,
                &[
                    "barrier",
                    "memoryBarrier",
                    "groupMemoryBarrier",
                    "memoryBarrierShared",
                    "memoryBarrierBuffer",
                    "memoryBarrierImage",
                    "GroupMemoryBarrierWithGroupSync",
                    "DeviceMemoryBarrier",
                ],
            );

            // 内存操作
            stats.memory_count += count_func_calls(trimmed, &["imageLoad", "imageStore"]);
            if trimmed.contains("buffer") || trimmed.contains("ssbo") {
                stats.memory_count += 1;
            }

            // 流控
            if trimmed.starts_with("if")
                || trimmed.starts_with("} else")
                || trimmed.starts_with("else")
            {
                stats.flow_control_count += 1;
            }
            if trimmed.starts_with("for")
                || trimmed.starts_with("while")
                || trimmed.starts_with("do")
            {
                stats.flow_control_count += 1;
            }
        }

        // 总估算: ALU 1 周期, TEX 4 周期, 特殊函数 4 周期, 矩阵 4 周期
        stats.total_estimated = stats.alu_count
            + stats.texture_count * 4
            + stats.special_func_count * 4
            + stats.matrix_count * 4
            + stats.memory_count * 4
            + stats.atomic_count * 8
            + stats.barrier_count * 16
            + stats.flow_control_count;

        stats
    }

    /// 估算寄存器压力
    fn estimate_register_pressure(&self, source: &str) -> RegisterPressure {
        let mut pressure = RegisterPressure::default();

        for line in source.lines() {
            let trimmed = line.trim();

            // 局部变量声明
            if is_local_var_decl(trimmed) {
                pressure.local_variable_count += 1;

                // 向量类型使用更多寄存器
                if trimmed.contains("vec4")
                    || trimmed.contains("float4")
                    || trimmed.contains("mat4")
                    || trimmed.contains("float4x4")
                {
                    pressure.vector_registers += 4;
                } else if trimmed.contains("vec3")
                    || trimmed.contains("float3")
                    || trimmed.contains("mat3")
                    || trimmed.contains("float3x3")
                {
                    pressure.vector_registers += 3;
                } else if trimmed.contains("vec2") || trimmed.contains("float2") {
                    pressure.vector_registers += 2;
                } else {
                    pressure.scalar_registers += 1;
                }
            }

            // Uniform/Constant
            if trimmed.contains("uniform")
                || trimmed.contains("cbuffer")
                || trimmed.contains("ConstantBuffer")
            {
                pressure.uniform_count += 1;
            }

            // 输入 varying
            if trimmed.starts_with("in ")
                || trimmed.starts_with("layout") && trimmed.contains("in ")
            {
                pressure.input_varying_count += 1;
            }

            // 输出 varying
            if trimmed.starts_with("out ")
                || trimmed.starts_with("layout") && trimmed.contains("out ")
            {
                pressure.output_varying_count += 1;
            }
        }

        // 临时寄存器 ≈ 局部变量数 * 1.5 (编译器会引入临时变量)
        pressure.temp_register_estimate = (pressure.local_variable_count as f64 * 1.5) as usize;

        let total_registers =
            pressure.scalar_registers + pressure.vector_registers + pressure.temp_register_estimate;

        // 压力等级: < 32 低, < 64 中, < 128 高, >= 128 极高
        pressure.pressure_level = if total_registers < 32 {
            0
        } else if total_registers < 64 {
            1
        } else if total_registers < 128 {
            2
        } else {
            3
        };

        pressure.likely_spilling = total_registers > 96;

        pressure
    }

    /// 分析纹理采样
    fn analyze_texture_sampling(&self, source: &str) -> TextureSamplingInfo {
        let mut info = TextureSamplingInfo::default();
        let mut in_loop = false;
        let mut loop_depth = 0u32;

        for line in source.lines() {
            let trimmed = line.trim();

            // 追踪循环上下文
            if trimmed.starts_with("for") || trimmed.starts_with("while") {
                loop_depth += 1;
                in_loop = true;
            }
            if trimmed.contains('}') && loop_depth > 0 {
                loop_depth = loop_depth.saturating_sub(1);
                if loop_depth == 0 {
                    in_loop = false;
                }
            }

            // 采样器声明
            if trimmed.contains("sampler") || trimmed.contains("SamplerState") {
                info.sampler_count += 1;
            }

            // 采样调用分类
            let sample_calls = count_func_calls(trimmed, &["texture", "textureSample", "Sample"]);
            let lod_calls = count_func_calls(trimmed, &["textureLod", "SampleLevel"]);
            let grad_calls = count_func_calls(trimmed, &["textureGrad", "SampleGrad"]);
            let offset_calls = count_func_calls(trimmed, &["textureOffset"]);
            let proj_calls = count_func_calls(trimmed, &["textureProj", "textureProjLod"]);

            let total_this_line = sample_calls + lod_calls + grad_calls + offset_calls + proj_calls;
            info.sample_call_count += total_this_line;
            info.lod_sample_count += lod_calls;
            info.grad_sample_count += grad_calls;
            info.offset_sample_count += offset_calls;
            info.proj_sample_count += proj_calls;

            if in_loop && total_this_line > 0 {
                info.loop_sample_count += total_this_line;
            }
        }

        info
    }

    /// 分析分支复杂度
    fn analyze_branches(&self, source: &str) -> BranchComplexity {
        let mut complexity = BranchComplexity::default();
        let mut branch_depth = 0usize;
        let mut loop_depth = 0usize;
        let mut max_branch = 0usize;
        let mut max_loop = 0usize;

        for line in source.lines() {
            let trimmed = line.trim();

            // if
            if trimmed.starts_with("if") && trimmed.contains('(') {
                complexity.if_count += 1;
                branch_depth += 1;
                max_branch = max_branch.max(branch_depth);
            }

            // else
            if trimmed.starts_with("else") || trimmed.starts_with("} else") {
                complexity.if_else_count += 1;
            }

            // for
            if trimmed.starts_with("for") && trimmed.contains('(') {
                complexity.for_loop_count += 1;
                loop_depth += 1;
                max_loop = max_loop.max(loop_depth);
            }

            // while
            if trimmed.starts_with("while") && trimmed.contains('(') {
                complexity.while_loop_count += 1;
                loop_depth += 1;
                max_loop = max_loop.max(loop_depth);
            }

            // discard
            if trimmed.starts_with("discard")
                || trimmed.contains("discard;")
                || trimmed.starts_with("clip(")
            {
                complexity.discard_count += 1;
            }

            // 三元运算符
            complexity.ternary_count += trimmed.matches('?').count();

            // 关闭括号
            if trimmed.starts_with('}') || trimmed == "}" {
                if branch_depth > 0 {
                    branch_depth -= 1;
                }
                if loop_depth > 0 {
                    loop_depth -= 1;
                }
            }
        }

        complexity.max_branch_nesting = max_branch;
        complexity.max_loop_nesting = max_loop;

        complexity
    }

    /// 生成性能警告
    fn generate_warnings(
        &self,
        instructions: &InstructionStats,
        register_pressure: &RegisterPressure,
        texture_sampling: &TextureSamplingInfo,
        branch_complexity: &BranchComplexity,
    ) -> Vec<PerfWarning> {
        let mut warnings = Vec::new();

        // 高指令数
        if instructions.total_estimated > 500 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::High,
                message: format!(
                    "估算指令数过高 ({}), 可能导致 GPU 占用率降低",
                    instructions.total_estimated
                ),
                suggestion: "考虑简化计算、使用查找表 (LUT) 或分拆为多 Pass".to_string(),
                line_number: None,
            });
        } else if instructions.total_estimated > 200 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::Medium,
                message: format!("估算指令数较高 ({})", instructions.total_estimated),
                suggestion: "审查是否有可合并的计算或可移至顶点着色器的操作".to_string(),
                line_number: None,
            });
        }

        // 寄存器溢出风险
        if register_pressure.likely_spilling {
            warnings.push(PerfWarning {
                severity: WarningSeverity::Critical,
                message: "寄存器压力极高，可能导致寄存器溢出到显存 (register spilling)".to_string(),
                suggestion: "减少局部变量数量，缩小变量作用域，避免同时持有多个大向量".to_string(),
                line_number: None,
            });
        } else if register_pressure.pressure_level >= 2 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::High,
                message: "寄存器压力较高，可能影响 GPU 并发度 (occupancy)".to_string(),
                suggestion: "减少临时变量，复用寄存器，考虑分拆为更小的着色器".to_string(),
                line_number: None,
            });
        }

        // 循环内纹理采样
        if texture_sampling.loop_sample_count > 0 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::High,
                message: format!(
                    "检测到 {} 个循环内纹理采样",
                    texture_sampling.loop_sample_count
                ),
                suggestion: "循环内纹理采样代价极高，考虑预计算 UV 或展开循环".to_string(),
                line_number: None,
            });
        }

        // 过多纹理采样
        if texture_sampling.sample_call_count > 16 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::Medium,
                message: format!(
                    "纹理采样次数过多 ({}), 可能成为带宽瓶颈",
                    texture_sampling.sample_call_count
                ),
                suggestion: "考虑合并纹理通道、使用纹理数组或减少采样点数".to_string(),
                line_number: None,
            });
        }

        // 深层循环嵌套
        if branch_complexity.max_loop_nesting > 2 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::High,
                message: format!(
                    "循环嵌套深度 {} 层，GPU 效率急剧下降",
                    branch_complexity.max_loop_nesting
                ),
                suggestion: "扁平化循环嵌套，或将内层循环拆分为子函数".to_string(),
                line_number: None,
            });
        }

        // discard 使用
        if branch_complexity.discard_count > 0 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::Low,
                message: format!(
                    "使用了 {} 次 discard，可能阻止 Early-Z 优化",
                    branch_complexity.discard_count
                ),
                suggestion: "在 Depth Pre-Pass 中使用 discard，主 Pass 中避免使用".to_string(),
                line_number: None,
            });
        }

        // 过多原子操作
        if instructions.atomic_count > 4 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::High,
                message: format!(
                    "原子操作过多 ({})，可能导致严重的线程争用",
                    instructions.atomic_count
                ),
                suggestion: "减少原子操作，考虑使用局部归约或无锁算法".to_string(),
                line_number: None,
            });
        }

        // 特殊函数过多
        if instructions.special_func_count > 20 {
            warnings.push(PerfWarning {
                severity: WarningSeverity::Medium,
                message: format!(
                    "超越函数 (sin/cos/pow 等) 调用过多 ({})",
                    instructions.special_func_count
                ),
                suggestion: "考虑用多项式近似、查找表或半精度 (mediump) 替代".to_string(),
                line_number: None,
            });
        }

        warnings
    }

    /// 计算复杂度评分 (0~100, 越低越好)
    fn compute_complexity_score(
        &self,
        instructions: &InstructionStats,
        register_pressure: &RegisterPressure,
        texture_sampling: &TextureSamplingInfo,
        branch_complexity: &BranchComplexity,
    ) -> f64 {
        let mut score = 0.0;

        // 指令数贡献 (0~30 分)
        score += (instructions.total_estimated as f64 / 20.0).min(30.0);

        // 寄存器压力贡献 (0~20 分)
        score += match register_pressure.pressure_level {
            0 => 0.0,
            1 => 5.0,
            2 => 12.0,
            _ => 20.0,
        };

        // 纹理采样贡献 (0~20 分)
        score += (texture_sampling.sample_call_count as f64 * 1.0).min(10.0);
        score += (texture_sampling.loop_sample_count as f64 * 5.0).min(10.0);

        // 分支复杂度贡献 (0~20 分)
        score += (branch_complexity.max_loop_nesting as f64 * 5.0).min(10.0);
        score += (branch_complexity.max_branch_nesting as f64 * 3.0).min(6.0);
        score += (branch_complexity.discard_count as f64 * 2.0).min(4.0);

        // 原子/屏障贡献 (0~10 分)
        score += (instructions.atomic_count as f64 * 2.0).min(5.0);
        score += (instructions.barrier_count as f64 * 3.0).min(5.0);

        score.min(100.0)
    }

    /// 估算 GPU 周期数
    fn estimate_gpu_cycles(
        &self,
        instructions: &InstructionStats,
        texture_sampling: &TextureSamplingInfo,
    ) -> usize {
        // ALU: 1 周期/指令 (假设 SIMD 满载)
        // TEX: ~200 周期/采样 (含延迟隐藏)
        // 特殊函数: 4 周期
        // 原子: 8~64 周期
        let alu_cycles = instructions.alu_count;
        let tex_cycles = texture_sampling.sample_call_count * 4;
        let special_cycles = instructions.special_func_count * 4;
        let matrix_cycles = instructions.matrix_count * 4;
        let atomic_cycles = instructions.atomic_count * 32;

        alu_cycles + tex_cycles + special_cycles + matrix_cycles + atomic_cycles
    }
}

// =============================================================================
// 辅助函数
// =============================================================================

/// 统计字符串中操作符出现次数
fn count_occurrences(line: &str, operators: &[&str]) -> usize {
    let mut count = 0;
    for op in operators {
        // 排除在注释和字符串中的
        count += line.matches(op).count();
    }
    // 粗略去重 (>>= 等会被重复计数)
    count / 2 + count % 2
}

/// 统计函数调用次数
fn count_func_calls(line: &str, func_names: &[&str]) -> usize {
    let mut count = 0;
    for name in func_names {
        let pattern = format!("{}(", name);
        count += line.matches(&pattern).count();
    }
    count
}

/// 检查是否为局部变量声明
fn is_local_var_decl(line: &str) -> bool {
    let type_keywords = [
        "float", "int", "uint", "bool", "double", "vec2", "vec3", "vec4", "ivec2", "ivec3",
        "ivec4", "uvec2", "uvec3", "uvec4", "mat2", "mat3", "mat4", "float2", "float3", "float4",
        "int2", "int3", "int4", "uint2", "uint3", "uint4", "float2x2", "float3x3", "float4x4",
        "half", "half2", "half3", "half4",
    ];

    // 排除 uniform, in, out, layout 等声明
    if line.starts_with("uniform")
        || line.starts_with("in ")
        || line.starts_with("out ")
        || line.starts_with("layout")
        || line.starts_with("varying")
        || line.starts_with("attribute")
        || line.starts_with("#")
        || line.starts_with("//")
        || line.starts_with("struct")
    {
        return false;
    }

    for keyword in &type_keywords {
        if line.starts_with(keyword) && line.contains('=') {
            return true;
        }
        if line.starts_with(keyword) && line.contains(';') && !line.contains('(') {
            return true;
        }
    }

    false
}

/// 复杂度评分转性能等级
fn score_to_grade(score: f64) -> char {
    if score < 15.0 {
        'A'
    } else if score < 30.0 {
        'B'
    } else if score < 50.0 {
        'C'
    } else if score < 75.0 {
        'D'
    } else {
        'F'
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    const SIMPLE_FRAG_SHADER: &str = r#"
#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_normal;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_albedo;
layout(set = 0, binding = 1) uniform sampler2D u_normal;

void main() {
    vec4 albedo = texture(u_albedo, v_uv);
    vec3 normal = texture(u_normal, v_uv).xyz * 2.0 - 1.0;
    float diffuse = max(dot(normal, vec3(0.0, 1.0, 0.0)), 0.0);
    out_color = albedo * diffuse;
}
"#;

    const COMPLEX_COMPUTE_SHADER: &str = r#"
#version 450
layout(local_size_x = 256) in;

layout(set = 0, binding = 0) buffer DataBuffer {
    float data[];
};

layout(set = 0, binding = 1) uniform sampler2D u_tex;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    float sum = 0.0;

    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 64; j++) {
            vec2 uv = vec2(float(i) / 64.0, float(j) / 64.0);
            vec4 sample_val = texture(u_tex, uv);
            sum += sample_val.r * sin(float(i)) + cos(float(j));
            sum += pow(sample_val.g, 2.2);
            sum += exp(sample_val.b * 0.1);
        }
    }

    atomicAdd(data[0], sum);
    atomicAdd(data[1], sum * 0.5);
    barrier();
    memoryBarrier();
    data[idx] = sum;
}
"#;

    #[test]
    fn test_simple_shader_analysis() {
        let analyzer = ShaderPerfAnalyzer::new();
        let report = analyzer.analyze_source("simple_frag", SIMPLE_FRAG_SHADER);

        assert_eq!(report.shader_name, "simple_frag");
        assert!(report.source_lines > 0);
        assert!(
            report.instructions.texture_count >= 2,
            "应检测到 2 个纹理采样"
        );
        assert!(report.instructions.alu_count > 0, "应检测到 ALU 操作");
        assert!(report.complexity_score < 50.0, "简单着色器复杂度不应过高");
        assert!(
            report.performance_grade == 'A' || report.performance_grade == 'B',
            "简单着色器应得到 A 或 B 级"
        );
    }

    #[test]
    fn test_complex_shader_analysis() {
        let analyzer = ShaderPerfAnalyzer::new();
        let report = analyzer.analyze_source("complex_compute", COMPLEX_COMPUTE_SHADER);

        assert!(report.instructions.texture_count >= 1, "应检测到纹理采样");
        assert!(
            report.instructions.special_func_count >= 3,
            "应检测到 sin/cos/pow/exp"
        );
        assert!(report.instructions.atomic_count >= 2, "应检测到原子操作");
        assert!(report.instructions.barrier_count >= 1, "应检测到屏障");
        assert!(
            report.branch_complexity.for_loop_count >= 2,
            "应检测到 2 层循环"
        );
        assert!(
            report.branch_complexity.max_loop_nesting >= 2,
            "循环嵌套应 >= 2"
        );
        assert!(
            report.texture_sampling.loop_sample_count > 0,
            "应检测到循环内采样"
        );

        // 复杂着色器应有警告
        assert!(!report.warnings.is_empty(), "复杂着色器应有性能警告");
        assert!(
            report.complexity_score > 25.0,
            "复杂着色器评分应较高, 实际: {}",
            report.complexity_score
        );
    }

    #[test]
    fn test_register_pressure_basic() {
        let source = r#"
void main() {
    vec4 a = vec4(1.0);
    vec4 b = vec4(2.0);
    vec4 c = vec4(3.0);
    float d = 1.0;
    float e = 2.0;
}
"#;
        let analyzer = ShaderPerfAnalyzer::new();
        let pressure = analyzer.estimate_register_pressure(source);

        assert!(pressure.local_variable_count >= 5, "应检测到 5 个局部变量");
        assert!(
            pressure.vector_registers >= 12,
            "3 个 vec4 应使用 12 个向量寄存器"
        );
        assert!(
            pressure.scalar_registers >= 2,
            "2 个 float 应使用 2 个标量寄存器"
        );
    }

    #[test]
    fn test_texture_sampling_in_loop() {
        let source = r#"
void main() {
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 16; i++) {
        sum += texture(u_tex, vec2(float(i) / 16.0, 0.0));
    }
    out_color = sum;
}
"#;
        let analyzer = ShaderPerfAnalyzer::new();
        let sampling = analyzer.analyze_texture_sampling(source);

        assert!(sampling.sample_call_count >= 1);
        assert!(sampling.loop_sample_count >= 1, "应检测到循环内采样");
    }

    #[test]
    fn test_discard_warning() {
        let source = r#"
void main() {
    vec4 color = texture(u_tex, v_uv);
    if (color.a < 0.5) {
        discard;
    }
    out_color = color;
}
"#;
        let analyzer = ShaderPerfAnalyzer::new();
        let report = analyzer.analyze_source("alpha_test", source);

        assert!(report.branch_complexity.discard_count >= 1);
        assert!(
            report
                .warnings
                .iter()
                .any(|w| w.message.contains("discard")),
            "应有关于 discard 的警告"
        );
    }

    #[test]
    fn test_empty_shader() {
        let analyzer = ShaderPerfAnalyzer::new();
        let report = analyzer.analyze_source("empty", "");

        assert_eq!(report.instructions.total_estimated, 0);
        assert_eq!(report.complexity_score, 0.0);
        assert_eq!(report.performance_grade, 'A');
    }

    #[test]
    fn test_alu_tex_ratio() {
        let mut stats = InstructionStats::default();
        stats.alu_count = 100;
        stats.texture_count = 10;
        assert!((stats.alu_tex_ratio() - 10.0).abs() < 0.001);

        stats.texture_count = 0;
        assert!(stats.alu_tex_ratio().is_infinite());
    }

    #[test]
    fn test_score_to_grade() {
        assert_eq!(score_to_grade(0.0), 'A');
        assert_eq!(score_to_grade(14.9), 'A');
        assert_eq!(score_to_grade(15.0), 'B');
        assert_eq!(score_to_grade(30.0), 'C');
        assert_eq!(score_to_grade(50.0), 'D');
        assert_eq!(score_to_grade(75.0), 'F');
        assert_eq!(score_to_grade(100.0), 'F');
    }

    #[test]
    fn test_count_func_calls() {
        assert_eq!(count_func_calls("texture(u_tex, uv)", &["texture"]), 1);
        assert_eq!(count_func_calls("sin(x) + cos(y)", &["sin", "cos"]), 2);
        assert_eq!(count_func_calls("no calls here", &["sin", "cos"]), 0);
    }
}
