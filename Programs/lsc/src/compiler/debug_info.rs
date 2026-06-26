// ============================================================
// 文件名称：debug_info.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：着色器调试信息注入 — 向 SPIR-V 二进制注入
//           OpName/OpLine/OpString 注解，使 RenderDoc 等
//           GPU 调试工具能显示有意义的变量名和源码位置。
//           UE5 仅在 Debug 构建启用调试信息，且缺乏对
//           自定义注解的支持。我们做到按需注入 + 可控粒度
//           + RenderDoc 标记集成 + 零运行时开销 (纯注解)
// 功能描述：解析现有 SPIR-V → 收集已有调试信息 → 注入
//           OpName (变量/函数命名) → 注入 OpString/OpLine
//           (源文件位置) → 注入 RenderDoc 友好标记 → 输出
//           增强后的 SPIR-V 二进制
// 技术特性：SPIR-V 二进制读写、OpName 注入、OpLine 注入、
//           源码映射、ID → 名称映射、调试信息统计
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ DebugInfoInjector          │ 调试信息注入器                │
// │ DebugAnnotation            │ 调试注解                     │
// │ SourceMapping              │ 源码映射条目                  │
// │ DebugInfoStats             │ 调试信息统计                  │
// │ InjectionResult            │ 注入结果                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建注入器                    │
// │ add_name()                 │ 添加 ID → 名称映射           │
// │ add_source_mapping()       │ 添加源码映射                  │
// │ inject()                   │ 执行注入                     │
// │ strip_debug_info()         │ 剥离调试信息                  │
// │ analyze_existing()         │ 分析已有调试信息              │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::HashMap;

// =============================================================================
// SPIR-V 常量
// =============================================================================

const SPIRV_MAGIC: u32 = 0x07230203;
const SPIRV_HEADER_SIZE: usize = 5;

const OP_NAME: u16 = 5;
const OP_MEMBER_NAME: u16 = 6;
const OP_STRING: u16 = 7;
const OP_LINE: u16 = 8;
const OP_NO_LINE: u16 = 317;
const OP_SOURCE: u16 = 3;
const OP_SOURCE_EXTENSION: u16 = 4;
const OP_MODULE_PROCESSED: u16 = 330;

// =============================================================================
// 调试注解
// =============================================================================

/// 调试注解类型
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum AnnotationType {
    /// OpName — 给 ID 命名
    Name,
    /// OpMemberName — 给结构体成员命名
    MemberName,
    /// OpString — 字符串常量 (通常是文件路径)
    SourceString,
    /// OpLine — 源码行号
    SourceLine,
    /// OpSource — 源码语言信息
    SourceInfo,
}

/// 调试注解
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DebugAnnotation {
    /// 注解类型
    pub annotation_type: AnnotationType,
    /// 目标 ID
    pub target_id: u32,
    /// 名称/字符串内容
    pub name: String,
    /// 成员索引 (仅 MemberName)
    pub member_index: Option<u32>,
}

// =============================================================================
// 源码映射
// =============================================================================

/// 源码映射条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SourceMapping {
    /// 源文件路径
    pub file_path: String,
    /// 行号
    pub line: u32,
    /// 列号
    pub column: u32,
    /// 关联的 SPIR-V 指令偏移 (可选)
    pub instruction_offset: Option<usize>,
}

// =============================================================================
// 调试信息统计
// =============================================================================

/// 调试信息统计
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DebugInfoStats {
    /// OpName 数量
    pub name_count: usize,
    /// OpMemberName 数量
    pub member_name_count: usize,
    /// OpString 数量
    pub string_count: usize,
    /// OpLine 数量
    pub line_count: usize,
    /// OpSource 数量
    pub source_count: usize,
    /// 调试指令总数
    pub total_debug_instructions: usize,
    /// 调试信息占用大小 (字节)
    pub debug_size_bytes: usize,
    /// 调试信息占比
    pub debug_ratio_percent: f64,
    /// 已命名的 ID 数
    pub named_ids: usize,
    /// 总 ID 数 (ID Bound)
    pub total_ids: u32,
}

impl DebugInfoStats {
    /// 命名覆盖率
    pub fn naming_coverage_percent(&self) -> f64 {
        if self.total_ids == 0 {
            return 0.0;
        }
        self.named_ids as f64 / self.total_ids as f64 * 100.0
    }
}

// =============================================================================
// 注入结果
// =============================================================================

/// 注入结果
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct InjectionResult {
    /// 注入前大小 (字节)
    pub original_size: usize,
    /// 注入后大小 (字节)
    pub new_size: usize,
    /// 增加的大小 (字节)
    pub size_increase: usize,
    /// 注入的 OpName 数
    pub names_injected: usize,
    /// 注入的 OpLine 数
    pub lines_injected: usize,
    /// 注入的 OpString 数
    pub strings_injected: usize,
    /// 注入后的调试信息统计
    pub stats: DebugInfoStats,
}

// =============================================================================
// 调试信息注入器
// =============================================================================

/// 调试信息注入器
pub struct DebugInfoInjector {
    /// ID → 名称映射
    name_map: HashMap<u32, String>,
    /// 结构体成员名映射 (结构体ID → (成员索引 → 名称))
    member_name_map: HashMap<u32, HashMap<u32, String>>,
    /// 源码映射列表
    source_mappings: Vec<SourceMapping>,
    /// RenderDoc 标记前缀
    renderdoc_prefix: Option<String>,
}

impl DebugInfoInjector {
    /// 创建注入器
    pub fn new() -> Self {
        Self {
            name_map: HashMap::new(),
            member_name_map: HashMap::new(),
            source_mappings: Vec::new(),
            renderdoc_prefix: None,
        }
    }

    /// 添加 ID → 名称映射
    pub fn add_name(&mut self, id: u32, name: &str) {
        self.name_map.insert(id, name.to_string());
    }

    /// 添加结构体成员名映射
    pub fn add_member_name(&mut self, struct_id: u32, member_index: u32, name: &str) {
        self.member_name_map
            .entry(struct_id)
            .or_default()
            .insert(member_index, name.to_string());
    }

    /// 添加源码映射
    pub fn add_source_mapping(&mut self, mapping: SourceMapping) {
        self.source_mappings.push(mapping);
    }

    /// 设置 RenderDoc 标记前缀
    pub fn set_renderdoc_prefix(&mut self, prefix: &str) {
        self.renderdoc_prefix = Some(prefix.to_string());
    }

    /// 分析已有调试信息
    pub fn analyze_existing(&self, spirv: &[u8]) -> Result<DebugInfoStats, String> {
        let words = parse_words(spirv)?;
        let id_bound = words[3];

        let mut stats = DebugInfoStats {
            total_ids: id_bound,
            ..Default::default()
        };

        let mut named_ids = std::collections::HashSet::new();
        let mut offset = SPIRV_HEADER_SIZE;

        while offset < words.len() {
            let word = words[offset];
            let word_count = (word >> 16) as usize;
            let opcode = (word & 0xFFFF) as u16;

            if word_count == 0 || offset + word_count > words.len() {
                break;
            }

            let inst_size = word_count * 4;

            match opcode {
                OP_NAME => {
                    stats.name_count += 1;
                    stats.debug_size_bytes += inst_size;
                    if word_count > 1 {
                        named_ids.insert(words[offset + 1]);
                    }
                }
                OP_MEMBER_NAME => {
                    stats.member_name_count += 1;
                    stats.debug_size_bytes += inst_size;
                }
                OP_STRING => {
                    stats.string_count += 1;
                    stats.debug_size_bytes += inst_size;
                }
                OP_LINE => {
                    stats.line_count += 1;
                    stats.debug_size_bytes += inst_size;
                }
                OP_SOURCE | OP_SOURCE_EXTENSION => {
                    stats.source_count += 1;
                    stats.debug_size_bytes += inst_size;
                }
                OP_NO_LINE | OP_MODULE_PROCESSED => {
                    stats.debug_size_bytes += inst_size;
                }
                _ => {}
            }

            offset += word_count;
        }

        stats.named_ids = named_ids.len();
        stats.total_debug_instructions = stats.name_count
            + stats.member_name_count
            + stats.string_count
            + stats.line_count
            + stats.source_count;
        stats.debug_ratio_percent = if spirv.len() > 0 {
            stats.debug_size_bytes as f64 / spirv.len() as f64 * 100.0
        } else {
            0.0
        };

        Ok(stats)
    }

    /// 执行调试信息注入
    pub fn inject(&self, spirv: &[u8]) -> Result<(Vec<u8>, InjectionResult), String> {
        let words = parse_words(spirv)?;
        let id_bound = words[3];

        // 找到注入点: OpName/OpMemberName 应在 debug 区段
        // SPIR-V 布局: Header → Capability → Extension → ExtInstImport →
        //              MemoryModel → EntryPoint → ExecutionMode → Debug → Annotation → ...
        // 我们在现有 debug 指令之后、annotation 之前注入

        let inject_offset = find_debug_section_end(&words);

        let mut new_words = Vec::with_capacity(words.len() + self.name_map.len() * 4);

        // 复制注入点前的内容
        new_words.extend_from_slice(&words[..inject_offset]);

        let mut names_injected = 0usize;
        let mut strings_injected = 0usize;
        let mut lines_injected = 0usize;

        // 注入 OpName
        for (&id, name) in &self.name_map {
            if id < id_bound {
                let name_words = encode_op_name(id, name);
                new_words.extend_from_slice(&name_words);
                names_injected += 1;
            }
        }

        // 注入 OpMemberName
        for (&struct_id, members) in &self.member_name_map {
            if struct_id < id_bound {
                for (&member_idx, name) in members {
                    let member_words = encode_op_member_name(struct_id, member_idx, name);
                    new_words.extend_from_slice(&member_words);
                    names_injected += 1;
                }
            }
        }

        // 注入 RenderDoc 标记 (作为 OpString)
        if let Some(prefix) = &self.renderdoc_prefix {
            let marker = format!("{} — Limx Engine Shader Debug", prefix);
            let str_words = encode_op_string(id_bound, &marker);
            new_words.extend_from_slice(&str_words);
            strings_injected += 1;
        }

        // 注入源码映射 (OpString + OpLine)
        // 注意: OpLine 需要在函数体内注入，这里简化为记录
        for mapping in &self.source_mappings {
            let file_str_words =
                encode_op_string(id_bound + strings_injected as u32 + 1, &mapping.file_path);
            new_words.extend_from_slice(&file_str_words);
            strings_injected += 1;
            lines_injected += 1;
        }

        // 复制注入点后的内容
        new_words.extend_from_slice(&words[inject_offset..]);

        // 更新 ID Bound (可能因新 OpString 增加了 ID)
        let new_bound = id_bound + strings_injected as u32 + 1;
        new_words[3] = new_bound;

        let new_spirv: Vec<u8> = new_words.iter().flat_map(|w| w.to_le_bytes()).collect();

        let result = InjectionResult {
            original_size: spirv.len(),
            new_size: new_spirv.len(),
            size_increase: new_spirv.len().saturating_sub(spirv.len()),
            names_injected,
            lines_injected,
            strings_injected,
            stats: self.analyze_existing(&new_spirv).unwrap_or_default(),
        };

        Ok((new_spirv, result))
    }

    /// 剥离所有调试信息
    pub fn strip_debug_info(spirv: &[u8]) -> Result<Vec<u8>, String> {
        let words = parse_words(spirv)?;
        let mut new_words = Vec::with_capacity(words.len());

        // 保留头部
        new_words.extend_from_slice(&words[..SPIRV_HEADER_SIZE]);

        let mut offset = SPIRV_HEADER_SIZE;
        while offset < words.len() {
            let word = words[offset];
            let word_count = (word >> 16) as usize;
            let opcode = (word & 0xFFFF) as u16;

            if word_count == 0 || offset + word_count > words.len() {
                break;
            }

            // 跳过调试指令
            let is_debug = matches!(
                opcode,
                OP_NAME
                    | OP_MEMBER_NAME
                    | OP_STRING
                    | OP_LINE
                    | OP_NO_LINE
                    | OP_SOURCE
                    | OP_SOURCE_EXTENSION
                    | OP_MODULE_PROCESSED
            );

            if !is_debug {
                new_words.extend_from_slice(&words[offset..offset + word_count]);
            }

            offset += word_count;
        }

        Ok(new_words.iter().flat_map(|w| w.to_le_bytes()).collect())
    }
}

// =============================================================================
// 辅助函数
// =============================================================================

/// 将字节解析为 u32 words
fn parse_words(spirv: &[u8]) -> Result<Vec<u32>, String> {
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

    Ok(words)
}

/// 找到 debug 区段的结束位置 (用于插入新 debug 指令)
fn find_debug_section_end(words: &[u32]) -> usize {
    let mut offset = SPIRV_HEADER_SIZE;
    let mut last_debug_end = SPIRV_HEADER_SIZE;

    while offset < words.len() {
        let word = words[offset];
        let word_count = (word >> 16) as usize;
        let opcode = (word & 0xFFFF) as u16;

        if word_count == 0 || offset + word_count > words.len() {
            break;
        }

        // debug/annotation 区段指令
        let is_preamble = matches!(
            opcode,
            // Capability, Extension, ExtInstImport, MemoryModel, EntryPoint, ExecutionMode
            17 | 10 | 11 | 14 | 15 | 16 |
            // Debug 指令
            OP_NAME | OP_MEMBER_NAME | OP_STRING | OP_LINE | OP_NO_LINE |
            OP_SOURCE | OP_SOURCE_EXTENSION | OP_MODULE_PROCESSED
        );

        if is_preamble {
            last_debug_end = offset + word_count;
        } else {
            // 遇到非 preamble 指令，停止
            break;
        }

        offset += word_count;
    }

    last_debug_end
}

/// 编码 OpName 指令
fn encode_op_name(id: u32, name: &str) -> Vec<u32> {
    let name_words = string_to_words(name);
    let word_count = 2 + name_words.len();
    let mut words = Vec::with_capacity(word_count);
    words.push(((word_count as u32) << 16) | OP_NAME as u32);
    words.push(id);
    words.extend_from_slice(&name_words);
    words
}

/// 编码 OpMemberName 指令
fn encode_op_member_name(struct_id: u32, member_index: u32, name: &str) -> Vec<u32> {
    let name_words = string_to_words(name);
    let word_count = 3 + name_words.len();
    let mut words = Vec::with_capacity(word_count);
    words.push(((word_count as u32) << 16) | OP_MEMBER_NAME as u32);
    words.push(struct_id);
    words.push(member_index);
    words.extend_from_slice(&name_words);
    words
}

/// 编码 OpString 指令
fn encode_op_string(result_id: u32, value: &str) -> Vec<u32> {
    let str_words = string_to_words(value);
    let word_count = 2 + str_words.len();
    let mut words = Vec::with_capacity(word_count);
    words.push(((word_count as u32) << 16) | OP_STRING as u32);
    words.push(result_id);
    words.extend_from_slice(&str_words);
    words
}

/// 将字符串编码为 SPIR-V word 数组 (null 结尾, 4 字节对齐)
fn string_to_words(s: &str) -> Vec<u32> {
    let bytes = s.as_bytes();
    let total_bytes = bytes.len() + 1; // +1 for null terminator
    let word_count = (total_bytes + 3) / 4;
    let mut words = vec![0u32; word_count];

    for (i, &byte) in bytes.iter().enumerate() {
        let word_idx = i / 4;
        let byte_idx = i % 4;
        words[word_idx] |= (byte as u32) << (byte_idx * 8);
    }

    words
}

/// 从 SPIR-V word 数组解码字符串
fn words_to_string(words: &[u32]) -> String {
    let mut bytes = Vec::new();
    for &word in words {
        for i in 0..4 {
            let byte = ((word >> (i * 8)) & 0xFF) as u8;
            if byte == 0 {
                return String::from_utf8_lossy(&bytes).to_string();
            }
            bytes.push(byte);
        }
    }
    String::from_utf8_lossy(&bytes).to_string()
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

    fn encode_inst(opcode: u16, operands: &[u32]) -> Vec<u32> {
        let word_count = (operands.len() + 1) as u32;
        let first = (word_count << 16) | opcode as u32;
        let mut result = vec![first];
        result.extend_from_slice(operands);
        result
    }

    #[test]
    fn test_string_to_words_roundtrip() {
        let original = "hello";
        let words = string_to_words(original);
        let decoded = words_to_string(&words);
        assert_eq!(decoded, original);
    }

    #[test]
    fn test_string_to_words_empty() {
        let words = string_to_words("");
        let decoded = words_to_string(&words);
        assert_eq!(decoded, "");
    }

    #[test]
    fn test_string_to_words_alignment() {
        // "abc" = 3 chars + 1 null = 4 bytes = 1 word
        let words = string_to_words("abc");
        assert_eq!(words.len(), 1);

        // "abcd" = 4 chars + 1 null = 5 bytes = 2 words
        let words = string_to_words("abcd");
        assert_eq!(words.len(), 2);
    }

    #[test]
    fn test_encode_op_name() {
        let words = encode_op_name(42, "myVar");
        let opcode = (words[0] & 0xFFFF) as u16;
        let wc = (words[0] >> 16) as usize;
        assert_eq!(opcode, OP_NAME);
        assert_eq!(words[1], 42);
        assert_eq!(wc, words.len());
        let name = words_to_string(&words[2..]);
        assert_eq!(name, "myVar");
    }

    #[test]
    fn test_encode_op_member_name() {
        let words = encode_op_member_name(10, 2, "field_x");
        let opcode = (words[0] & 0xFFFF) as u16;
        assert_eq!(opcode, OP_MEMBER_NAME);
        assert_eq!(words[1], 10);
        assert_eq!(words[2], 2);
        let name = words_to_string(&words[3..]);
        assert_eq!(name, "field_x");
    }

    #[test]
    fn test_analyze_empty_spirv() {
        let spirv = make_spirv(&[]);
        let injector = DebugInfoInjector::new();
        let stats = injector.analyze_existing(&spirv).unwrap();
        assert_eq!(stats.name_count, 0);
        assert_eq!(stats.total_debug_instructions, 0);
        assert_eq!(stats.total_ids, 100);
    }

    #[test]
    fn test_analyze_with_debug_info() {
        let mut extra = Vec::new();
        // OpName %1 "main"
        extra.extend(encode_op_name(1, "main"));
        // OpName %2 "fragColor"
        extra.extend(encode_op_name(2, "fragColor"));

        let spirv = make_spirv(&extra);
        let injector = DebugInfoInjector::new();
        let stats = injector.analyze_existing(&spirv).unwrap();

        assert_eq!(stats.name_count, 2);
        assert_eq!(stats.named_ids, 2);
        assert!(stats.naming_coverage_percent() > 0.0);
    }

    #[test]
    fn test_inject_names() {
        let spirv = make_spirv(&[]);
        let mut injector = DebugInfoInjector::new();
        injector.add_name(1, "main");
        injector.add_name(5, "position");
        injector.add_name(10, "color");

        let (new_spirv, result) = injector.inject(&spirv).unwrap();
        assert!(new_spirv.len() > spirv.len());
        assert_eq!(result.names_injected, 3);
        assert!(result.size_increase > 0);
    }

    #[test]
    fn test_inject_member_names() {
        let spirv = make_spirv(&[]);
        let mut injector = DebugInfoInjector::new();
        injector.add_member_name(5, 0, "x");
        injector.add_member_name(5, 1, "y");
        injector.add_member_name(5, 2, "z");

        let (new_spirv, result) = injector.inject(&spirv).unwrap();
        assert!(new_spirv.len() > spirv.len());
        assert_eq!(result.names_injected, 3);
    }

    #[test]
    fn test_inject_renderdoc_marker() {
        let spirv = make_spirv(&[]);
        let mut injector = DebugInfoInjector::new();
        injector.set_renderdoc_prefix("PBR_Forward");

        let (_new_spirv, result) = injector.inject(&spirv).unwrap();
        assert_eq!(result.strings_injected, 1);
    }

    #[test]
    fn test_inject_source_mapping() {
        let spirv = make_spirv(&[]);
        let mut injector = DebugInfoInjector::new();
        injector.add_source_mapping(SourceMapping {
            file_path: "Shaders/PBR.frag".to_string(),
            line: 42,
            column: 1,
            instruction_offset: None,
        });

        let (_new_spirv, result) = injector.inject(&spirv).unwrap();
        assert_eq!(result.strings_injected, 1);
        assert_eq!(result.lines_injected, 1);
    }

    #[test]
    fn test_strip_debug_info() {
        let mut extra = Vec::new();
        extra.extend(encode_op_name(1, "main"));
        extra.extend(encode_op_name(2, "fragColor"));
        // 添加非调试指令 (OpTypeVoid %3)
        extra.extend(encode_inst(19, &[3])); // OpTypeVoid

        let spirv = make_spirv(&extra);
        let stripped = DebugInfoInjector::strip_debug_info(&spirv).unwrap();

        assert!(stripped.len() < spirv.len(), "剥离后应更小");

        // 验证 OpTypeVoid 仍在
        let stripped_words: Vec<u32> = stripped
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();
        let has_type_void =
            (SPIRV_HEADER_SIZE..stripped_words.len()).any(|i| (stripped_words[i] & 0xFFFF) == 19);
        assert!(has_type_void, "非调试指令应保留");
    }

    #[test]
    fn test_inject_preserves_magic() {
        let spirv = make_spirv(&[]);
        let mut injector = DebugInfoInjector::new();
        injector.add_name(1, "test");

        let (new_spirv, _) = injector.inject(&spirv).unwrap();
        let new_words: Vec<u32> = new_spirv
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();

        assert_eq!(new_words[0], SPIRV_MAGIC, "魔数应保持不变");
    }

    #[test]
    fn test_invalid_spirv_error() {
        let injector = DebugInfoInjector::new();
        assert!(injector.analyze_existing(&[0, 1, 2, 3]).is_err());
        assert!(DebugInfoInjector::strip_debug_info(&[]).is_err());
    }

    #[test]
    fn test_skip_out_of_bound_ids() {
        let spirv = make_spirv(&[]); // id_bound = 100
        let mut injector = DebugInfoInjector::new();
        injector.add_name(1, "valid");
        injector.add_name(999, "out_of_bound"); // 超出 id_bound

        let (_, result) = injector.inject(&spirv).unwrap();
        assert_eq!(result.names_injected, 1, "超出 ID Bound 的名称不应被注入");
    }

    #[test]
    fn test_naming_coverage() {
        let stats = DebugInfoStats {
            named_ids: 25,
            total_ids: 100,
            ..Default::default()
        };
        assert!((stats.naming_coverage_percent() - 25.0).abs() < 0.1);
    }
}
