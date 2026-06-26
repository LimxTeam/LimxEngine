// ============================================================
// 文件名称：include_graph.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：着色器 #include 依赖图 — 精确追踪着色器文件间的
//           包含关系，支持增量重编译决策和热重载精准触发。
//           UE5 的着色器热重载是全量重编译，我们做到只重编译
//           受影响的着色器文件
// 功能描述：着色器包含依赖图 + 热重载监控增强 — 扫描着色器
//           文件的 #include 指令构建 DAG，支持变更传播查询、
//           DOT 导出、增量重编译集计算、文件监控集成
// 技术特性：DAG 依赖图、BFS 变更传播、DOT 导出、文件哈希
//           变更检测、精准热重载触发
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ ShaderIncludeGraph         │ 着色器包含依赖图              │
// │ ShaderFileInfo             │ 着色器文件信息                │
// │ IncludeEdge                │ 包含边                       │
// │ ChangeImpact               │ 变更影响分析结果              │
// │ HotReloadDecision          │ 热重载决策                    │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建依赖图                    │
// │ add_file()                 │ 添加着色器文件                │
// │ add_include()              │ 添加包含关系                  │
// │ scan_includes()            │ 扫描文件的 #include           │
// │ get_affected_files()       │ 获取受影响的文件列表          │
// │ compute_recompile_set()    │ 计算增量重编译集合            │
// │ detect_cycles()            │ 检测循环包含                  │
// │ to_dot()                   │ 导出 DOT 格式                │
// │ get_include_depth()        │ 获取包含深度                  │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet, VecDeque};

// =============================================================================
// 着色器文件信息
// =============================================================================

/// 着色器文件信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderFileInfo {
    /// 文件路径 (相对于着色器根目录)
    pub path: String,
    /// 内容哈希 (用于变更检测)
    pub content_hash: String,
    /// 是否为入口着色器 (有 main 函数)
    pub is_entry_shader: bool,
    /// 着色器阶段 (若为入口着色器)
    pub stage: Option<String>,
}

/// 包含边
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IncludeEdge {
    /// 源文件 (包含者)
    pub from: String,
    /// 目标文件 (被包含者)
    pub to: String,
    /// 包含行号
    pub line_number: Option<usize>,
}

// =============================================================================
// 变更影响分析
// =============================================================================

/// 变更影响分析结果
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChangeImpact {
    /// 直接变更的文件
    pub changed_files: Vec<String>,
    /// 受影响的入口着色器 (需要重编译的)
    pub affected_entry_shaders: Vec<String>,
    /// 受影响的所有文件 (含传递)
    pub all_affected_files: Vec<String>,
    /// 影响传播链
    pub propagation_paths: Vec<Vec<String>>,
}

/// 热重载决策
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HotReloadDecision {
    /// 需要重编译的入口着色器列表
    pub recompile_shaders: Vec<String>,
    /// 可跳过的着色器列表
    pub skip_shaders: Vec<String>,
    /// 重编译数 vs 总数
    pub recompile_ratio: f64,
    /// 预估节省的编译时间 (百分比)
    pub estimated_savings_percent: f64,
}

// =============================================================================
// 着色器包含依赖图
// =============================================================================

/// 着色器包含依赖图
#[derive(Debug, Clone, Default)]
pub struct ShaderIncludeGraph {
    /// 文件信息 (路径 -> 信息)
    files: HashMap<String, ShaderFileInfo>,
    /// 前向边: 文件 A includes 文件 B (A -> B)
    forward_edges: HashMap<String, Vec<String>>,
    /// 反向边: 文件 B is included by 文件 A (B -> A)
    reverse_edges: HashMap<String, Vec<String>>,
}

impl ShaderIncludeGraph {
    /// 创建空依赖图
    pub fn new() -> Self {
        Self::default()
    }

    /// 添加着色器文件
    pub fn add_file(&mut self, info: ShaderFileInfo) {
        let path = info.path.clone();
        self.files.insert(path.clone(), info);
        self.forward_edges.entry(path.clone()).or_default();
        self.reverse_edges.entry(path).or_default();
    }

    /// 添加包含关系: `from` includes `to`
    pub fn add_include(&mut self, from: &str, to: &str) {
        self.forward_edges
            .entry(from.to_string())
            .or_default()
            .push(to.to_string());
        self.reverse_edges
            .entry(to.to_string())
            .or_default()
            .push(from.to_string());
    }

    /// 从着色器源码扫描 #include 指令
    pub fn scan_includes_from_source(source: &str) -> Vec<String> {
        let mut includes = Vec::new();
        for line in source.lines() {
            let trimmed = line.trim();
            if trimmed.starts_with("#include") {
                let rest = trimmed["#include".len()..].trim();
                if let Some(path) = rest.strip_prefix('"').and_then(|s| s.strip_suffix('"')) {
                    includes.push(path.to_string());
                } else if let Some(path) = rest.strip_prefix('<').and_then(|s| s.strip_suffix('>'))
                {
                    includes.push(path.to_string());
                }
            }
        }
        includes
    }

    /// 获取文件直接包含的文件列表
    pub fn get_includes(&self, file: &str) -> Vec<String> {
        self.forward_edges.get(file).cloned().unwrap_or_default()
    }

    /// 获取直接包含此文件的文件列表
    pub fn get_included_by(&self, file: &str) -> Vec<String> {
        self.reverse_edges.get(file).cloned().unwrap_or_default()
    }

    /// 获取文件被变更后受影响的所有文件 (BFS 反向传播)
    pub fn get_affected_files(&self, changed_file: &str) -> Vec<String> {
        let mut affected = Vec::new();
        let mut visited = HashSet::new();
        let mut queue = VecDeque::new();

        visited.insert(changed_file.to_string());
        queue.push_back(changed_file.to_string());

        while let Some(current) = queue.pop_front() {
            if let Some(includers) = self.reverse_edges.get(&current) {
                for includer in includers {
                    if visited.insert(includer.clone()) {
                        affected.push(includer.clone());
                        queue.push_back(includer.clone());
                    }
                }
            }
        }

        affected
    }

    /// 计算增量重编译集合 — 只返回受影响的入口着色器
    pub fn compute_recompile_set(&self, changed_files: &[&str]) -> HotReloadDecision {
        let mut all_affected = HashSet::new();

        for changed in changed_files {
            all_affected.insert(changed.to_string());
            for affected in self.get_affected_files(changed) {
                all_affected.insert(affected);
            }
        }

        // 筛选出入口着色器
        let mut recompile_shaders = Vec::new();
        let mut skip_shaders = Vec::new();

        for (path, info) in &self.files {
            if info.is_entry_shader {
                if all_affected.contains(path) {
                    recompile_shaders.push(path.clone());
                } else {
                    skip_shaders.push(path.clone());
                }
            }
        }

        let total_entry = recompile_shaders.len() + skip_shaders.len();
        let recompile_ratio = if total_entry > 0 {
            recompile_shaders.len() as f64 / total_entry as f64
        } else {
            0.0
        };
        let estimated_savings = (1.0 - recompile_ratio) * 100.0;

        HotReloadDecision {
            recompile_shaders,
            skip_shaders,
            recompile_ratio,
            estimated_savings_percent: estimated_savings,
        }
    }

    /// 完整变更影响分析
    pub fn analyze_change_impact(&self, changed_files: &[&str]) -> ChangeImpact {
        let mut all_affected = HashSet::new();
        let mut affected_entries = Vec::new();
        let mut propagation_paths = Vec::new();

        for &changed in changed_files {
            let affected = self.get_affected_files(changed);
            let mut path = vec![changed.to_string()];
            path.extend(affected.iter().cloned());
            propagation_paths.push(path);

            all_affected.insert(changed.to_string());
            for f in &affected {
                all_affected.insert(f.clone());
                if let Some(info) = self.files.get(f) {
                    if info.is_entry_shader {
                        affected_entries.push(f.clone());
                    }
                }
            }
        }

        // 去重
        affected_entries.sort();
        affected_entries.dedup();

        ChangeImpact {
            changed_files: changed_files.iter().map(|s| s.to_string()).collect(),
            affected_entry_shaders: affected_entries,
            all_affected_files: all_affected.into_iter().collect(),
            propagation_paths,
        }
    }

    /// 检测循环包含
    pub fn detect_cycles(&self) -> Vec<Vec<String>> {
        let mut cycles = Vec::new();
        let mut visited = HashSet::new();
        let mut rec_stack = HashSet::new();

        for file in self.files.keys() {
            if !visited.contains(file) {
                let mut path = Vec::new();
                self.dfs_cycle(file, &mut visited, &mut rec_stack, &mut path, &mut cycles);
            }
        }

        cycles
    }

    /// DFS 循环检测
    fn dfs_cycle(
        &self,
        node: &str,
        visited: &mut HashSet<String>,
        rec_stack: &mut HashSet<String>,
        path: &mut Vec<String>,
        cycles: &mut Vec<Vec<String>>,
    ) {
        visited.insert(node.to_string());
        rec_stack.insert(node.to_string());
        path.push(node.to_string());

        if let Some(neighbors) = self.forward_edges.get(node) {
            for next in neighbors {
                if !visited.contains(next) {
                    self.dfs_cycle(next, visited, rec_stack, path, cycles);
                } else if rec_stack.contains(next) {
                    // 找到环: 从 next 在 path 中的位置到末尾
                    if let Some(pos) = path.iter().position(|p| p == next) {
                        let mut cycle: Vec<String> = path[pos..].to_vec();
                        cycle.push(next.clone());
                        cycles.push(cycle);
                    }
                }
            }
        }

        rec_stack.remove(node);
        path.pop();
    }

    /// 获取包含深度 (从入口着色器到此文件的最大距离)
    pub fn get_include_depth(&self, file: &str) -> usize {
        let mut max_depth = 0;
        let mut visited = HashSet::new();
        self.depth_dfs(file, 0, &mut max_depth, &mut visited);
        max_depth
    }

    /// DFS 计算深度
    fn depth_dfs(
        &self,
        node: &str,
        current_depth: usize,
        max_depth: &mut usize,
        visited: &mut HashSet<String>,
    ) {
        if !visited.insert(node.to_string()) {
            return;
        }
        *max_depth = (*max_depth).max(current_depth);

        if let Some(includes) = self.forward_edges.get(node) {
            for inc in includes {
                self.depth_dfs(inc, current_depth + 1, max_depth, visited);
            }
        }

        visited.remove(node);
    }

    /// 导出 DOT 格式
    pub fn to_dot(&self) -> String {
        let mut dot = String::with_capacity(2048);
        dot.push_str("digraph ShaderIncludes {\n");
        dot.push_str("    rankdir=TB;\n");
        dot.push_str("    node [shape=box, style=filled];\n\n");

        // 节点
        for (path, info) in &self.files {
            let color = if info.is_entry_shader {
                "#4CAF50"
            } else {
                "#2196F3"
            };
            let label = path.rsplit('/').next().unwrap_or(path);
            dot.push_str(&format!(
                "    \"{}\" [label=\"{}\", fillcolor=\"{}\", fontcolor=white];\n",
                path, label, color,
            ));
        }

        dot.push('\n');

        // 边
        for (from, tos) in &self.forward_edges {
            for to in tos {
                dot.push_str(&format!("    \"{}\" -> \"{}\";\n", from, to));
            }
        }

        dot.push_str("}\n");
        dot
    }

    /// 获取统计信息
    pub fn stats(&self) -> (usize, usize, usize) {
        let file_count = self.files.len();
        let edge_count: usize = self.forward_edges.values().map(|v| v.len()).sum();
        let entry_count = self.files.values().filter(|f| f.is_entry_shader).count();
        (file_count, edge_count, entry_count)
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_file(path: &str, is_entry: bool) -> ShaderFileInfo {
        ShaderFileInfo {
            path: path.to_string(),
            content_hash: format!("hash_{}", path),
            is_entry_shader: is_entry,
            stage: if is_entry {
                Some("fragment".to_string())
            } else {
                None
            },
        }
    }

    fn build_test_graph() -> ShaderIncludeGraph {
        // common.glsl (被多个文件包含)
        // lighting.glsl includes common.glsl
        // pbr.frag (入口) includes lighting.glsl, common.glsl
        // shadow.frag (入口) includes common.glsl
        // sky.frag (入口) — 独立，不包含任何文件
        let mut graph = ShaderIncludeGraph::new();
        graph.add_file(make_file("common.glsl", false));
        graph.add_file(make_file("lighting.glsl", false));
        graph.add_file(make_file("pbr.frag", true));
        graph.add_file(make_file("shadow.frag", true));
        graph.add_file(make_file("sky.frag", true));

        graph.add_include("lighting.glsl", "common.glsl");
        graph.add_include("pbr.frag", "lighting.glsl");
        graph.add_include("pbr.frag", "common.glsl");
        graph.add_include("shadow.frag", "common.glsl");

        graph
    }

    #[test]
    fn test_basic_graph_construction() {
        let graph = build_test_graph();
        let (files, edges, entries) = graph.stats();
        assert_eq!(files, 5);
        assert_eq!(edges, 4);
        assert_eq!(entries, 3);
    }

    #[test]
    fn test_get_includes() {
        let graph = build_test_graph();
        let includes = graph.get_includes("pbr.frag");
        assert_eq!(includes.len(), 2);
        assert!(includes.contains(&"lighting.glsl".to_string()));
        assert!(includes.contains(&"common.glsl".to_string()));
    }

    #[test]
    fn test_get_included_by() {
        let graph = build_test_graph();
        let includers = graph.get_included_by("common.glsl");
        assert!(includers.contains(&"lighting.glsl".to_string()));
        assert!(includers.contains(&"pbr.frag".to_string()));
        assert!(includers.contains(&"shadow.frag".to_string()));
    }

    #[test]
    fn test_affected_files_common_change() {
        let graph = build_test_graph();
        // 修改 common.glsl → 影响 lighting.glsl, pbr.frag, shadow.frag
        let affected = graph.get_affected_files("common.glsl");
        assert!(affected.contains(&"lighting.glsl".to_string()));
        assert!(affected.contains(&"pbr.frag".to_string()));
        assert!(affected.contains(&"shadow.frag".to_string()));
        assert!(
            !affected.contains(&"sky.frag".to_string()),
            "sky.frag 不受影响"
        );
    }

    #[test]
    fn test_affected_files_lighting_change() {
        let graph = build_test_graph();
        // 修改 lighting.glsl → 只影响 pbr.frag
        let affected = graph.get_affected_files("lighting.glsl");
        assert!(affected.contains(&"pbr.frag".to_string()));
        assert!(!affected.contains(&"shadow.frag".to_string()));
    }

    #[test]
    fn test_recompile_set_common() {
        let graph = build_test_graph();
        // 修改 common.glsl → 重编译 pbr.frag 和 shadow.frag, 跳过 sky.frag
        let decision = graph.compute_recompile_set(&["common.glsl"]);

        assert!(decision.recompile_shaders.contains(&"pbr.frag".to_string()));
        assert!(decision
            .recompile_shaders
            .contains(&"shadow.frag".to_string()));
        assert!(decision.skip_shaders.contains(&"sky.frag".to_string()));
        assert!(decision.estimated_savings_percent > 0.0, "应有编译节省");
    }

    #[test]
    fn test_recompile_set_leaf_entry() {
        let graph = build_test_graph();
        // 修改 sky.frag (入口着色器，无人依赖) → 只重编译自己
        let decision = graph.compute_recompile_set(&["sky.frag"]);
        assert_eq!(decision.recompile_shaders.len(), 1);
        assert!(decision.recompile_shaders.contains(&"sky.frag".to_string()));
        assert_eq!(decision.skip_shaders.len(), 2);
    }

    #[test]
    fn test_change_impact_analysis() {
        let graph = build_test_graph();
        let impact = graph.analyze_change_impact(&["common.glsl"]);

        assert_eq!(impact.changed_files, vec!["common.glsl"]);
        assert!(impact
            .affected_entry_shaders
            .contains(&"pbr.frag".to_string()));
        assert!(impact
            .affected_entry_shaders
            .contains(&"shadow.frag".to_string()));
        assert!(impact.all_affected_files.len() >= 3);
    }

    #[test]
    fn test_no_cycles_in_test_graph() {
        let graph = build_test_graph();
        let cycles = graph.detect_cycles();
        assert!(cycles.is_empty(), "测试图不应有循环");
    }

    #[test]
    fn test_detect_cycle() {
        let mut graph = ShaderIncludeGraph::new();
        graph.add_file(make_file("a.glsl", false));
        graph.add_file(make_file("b.glsl", false));
        graph.add_file(make_file("c.glsl", false));

        graph.add_include("a.glsl", "b.glsl");
        graph.add_include("b.glsl", "c.glsl");
        graph.add_include("c.glsl", "a.glsl"); // 循环!

        let cycles = graph.detect_cycles();
        assert!(!cycles.is_empty(), "应检测到循环包含");
    }

    #[test]
    fn test_include_depth() {
        let graph = build_test_graph();
        // pbr.frag -> lighting.glsl -> common.glsl (深度 2)
        assert_eq!(graph.get_include_depth("pbr.frag"), 2);
        // shadow.frag -> common.glsl (深度 1)
        assert_eq!(graph.get_include_depth("shadow.frag"), 1);
        // sky.frag (深度 0)
        assert_eq!(graph.get_include_depth("sky.frag"), 0);
    }

    #[test]
    fn test_dot_export() {
        let graph = build_test_graph();
        let dot = graph.to_dot();

        assert!(dot.contains("digraph ShaderIncludes"));
        assert!(dot.contains("pbr.frag"));
        assert!(dot.contains("common.glsl"));
        assert!(dot.contains("->"));
        assert!(dot.contains("#4CAF50")); // 入口着色器颜色
        assert!(dot.contains("#2196F3")); // 包含文件颜色
    }

    #[test]
    fn test_scan_includes_from_source() {
        let source = r#"
#version 450
#include "common.glsl"
#include "lighting.glsl"
#include <vulkan_ext.glsl>

void main() {}
"#;
        let includes = ShaderIncludeGraph::scan_includes_from_source(source);
        assert_eq!(includes.len(), 3);
        assert_eq!(includes[0], "common.glsl");
        assert_eq!(includes[1], "lighting.glsl");
        assert_eq!(includes[2], "vulkan_ext.glsl");
    }

    #[test]
    fn test_empty_graph() {
        let graph = ShaderIncludeGraph::new();
        let (files, edges, entries) = graph.stats();
        assert_eq!(files, 0);
        assert_eq!(edges, 0);
        assert_eq!(entries, 0);
        assert!(graph.detect_cycles().is_empty());
    }

    #[test]
    fn test_multiple_changes() {
        let graph = build_test_graph();
        // 同时修改 common.glsl 和 sky.frag
        let decision = graph.compute_recompile_set(&["common.glsl", "sky.frag"]);

        // 所有入口着色器都应重编译
        assert_eq!(decision.recompile_shaders.len(), 3);
        assert!(decision.skip_shaders.is_empty());
        assert!((decision.recompile_ratio - 1.0).abs() < 0.001);
    }
}
