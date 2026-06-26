/*******************************************************************************
 * 文件: dependency.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   依赖解析功能 (生产级增强版)
 *   - 构建模块依赖图 (DAG)
 *   - 检测循环依赖
 *   - 验证层级约束
 *   - 拓扑排序 (Kahn's 算法)
 *
 * 技术特性:
 *   - 反向依赖图 (快速查找依赖者)
 *   - 依赖深度分析
 *   - 并行层级验证
 *   - 依赖统计报告
 *   - 模块分组支持
 *
 ******************************************************************************/

use anyhow::Result;
use rayon::prelude::*;
use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};

use super::config::Module;
use super::error::LbtError;

/// 模块依赖图
#[derive(Debug, Clone)]
pub struct DependencyGraph {
    /// 模块映射 (名称 -> 模块)
    pub modules: HashMap<String, Module>,

    /// 依赖边 (模块名 -> 依赖的模块名列表)
    pub edges: HashMap<String, Vec<String>>,

    /// 反向依赖边 (模块名 -> 依赖此模块的模块列表)
    pub reverse_edges: HashMap<String, Vec<String>>,

    /// 拓扑排序后的模块顺序
    pub build_order: Vec<String>,

    /// 模块依赖深度
    pub depths: HashMap<String, usize>,

    /// 按层级分组的模块
    pub layers: BTreeMap<u32, Vec<String>>,
}

/// 依赖统计信息
#[derive(Debug, Default)]
pub struct DependencyStats {
    pub total_modules: usize,
    pub total_edges: usize,
    pub max_depth: usize,
    pub avg_dependencies: f64,
    pub modules_by_layer: BTreeMap<u32, usize>,
}

impl DependencyStats {
    pub fn print_summary(&self) {
        println!("\n依赖统计:");
        println!("  - 模块总数: {}", self.total_modules);
        println!("  - 依赖边总数: {}", self.total_edges);
        println!("  - 最大依赖深度: {}", self.max_depth);
        println!("  - 平均依赖数: {:.2}", self.avg_dependencies);
        println!("  - 层级分布:");
        for (layer, count) in &self.modules_by_layer {
            println!("    Layer {}: {} 模块", layer, count);
        }
    }
}

/// 解析模块依赖关系
pub fn resolve_dependencies(modules: &[Module]) -> Result<DependencyGraph> {
    let mut graph = DependencyGraph {
        modules: HashMap::with_capacity(modules.len()),
        edges: HashMap::with_capacity(modules.len()),
        reverse_edges: HashMap::with_capacity(modules.len()),
        build_order: Vec::with_capacity(modules.len()),
        depths: HashMap::with_capacity(modules.len()),
        layers: BTreeMap::new(),
    };

    // 构建模块映射
    for module in modules {
        graph.modules.insert(module.name.clone(), module.clone());
        graph.reverse_edges.insert(module.name.clone(), Vec::new());

        // 按层级分组
        graph
            .layers
            .entry(module.layer as u32)
            .or_insert_with(Vec::new)
            .push(module.name.clone());
    }

    // 构建依赖边和反向边
    for module in modules {
        let mut deps = Vec::new();

        // 合并公开和私有依赖
        for dep in &module.config.dependencies.public {
            if !graph.modules.contains_key(dep) {
                return Err(LbtError::MissingDependency {
                    module: module.name.clone(),
                    dependency: dep.clone(),
                }
                .into());
            }
            deps.push(dep.clone());

            // 添加反向边
            if let Some(reverse) = graph.reverse_edges.get_mut(dep) {
                reverse.push(module.name.clone());
            }
        }

        for dep in &module.config.dependencies.private {
            if !graph.modules.contains_key(dep) {
                return Err(LbtError::MissingDependency {
                    module: module.name.clone(),
                    dependency: dep.clone(),
                }
                .into());
            }
            if !deps.contains(dep) {
                deps.push(dep.clone());

                // 添加反向边
                if let Some(reverse) = graph.reverse_edges.get_mut(dep) {
                    reverse.push(module.name.clone());
                }
            }
        }

        graph.edges.insert(module.name.clone(), deps);
    }

    // 检测循环依赖
    detect_cycles(&graph)?;

    // 拓扑排序
    graph.build_order = topological_sort(&graph)?;

    // 计算依赖深度
    compute_depths(&mut graph);

    Ok(graph)
}

/// 计算每个模块的依赖深度
fn compute_depths(graph: &mut DependencyGraph) {
    for module_name in &graph.build_order {
        let deps = graph.edges.get(module_name).cloned().unwrap_or_default();
        let depth = if deps.is_empty() {
            0
        } else {
            deps.iter()
                .filter_map(|d| graph.depths.get(d))
                .max()
                .copied()
                .unwrap_or(0)
                + 1
        };
        graph.depths.insert(module_name.clone(), depth);
    }
}

/// 检测循环依赖 (DFS)
fn detect_cycles(graph: &DependencyGraph) -> Result<()> {
    let mut visited = HashSet::new();
    let mut rec_stack = HashSet::new();
    let mut path = Vec::new();

    for module_name in graph.modules.keys() {
        if !visited.contains(module_name) {
            if let Some(cycle) =
                dfs_detect_cycle(module_name, graph, &mut visited, &mut rec_stack, &mut path)
            {
                return Err(LbtError::CyclicDependency { cycle }.into());
            }
        }
    }

    Ok(())
}

fn dfs_detect_cycle(
    node: &str,
    graph: &DependencyGraph,
    visited: &mut HashSet<String>,
    rec_stack: &mut HashSet<String>,
    path: &mut Vec<String>,
) -> Option<String> {
    visited.insert(node.to_string());
    rec_stack.insert(node.to_string());
    path.push(node.to_string());

    if let Some(neighbors) = graph.edges.get(node) {
        for neighbor in neighbors {
            if !visited.contains(neighbor) {
                if let Some(cycle) = dfs_detect_cycle(neighbor, graph, visited, rec_stack, path) {
                    return Some(cycle);
                }
            } else if rec_stack.contains(neighbor) {
                // 找到循环
                let cycle_start = path.iter().position(|n| n == neighbor).unwrap_or(0);
                let cycle: Vec<_> = path[cycle_start..].to_vec();
                return Some(format!("{} -> {}", cycle.join(" -> "), neighbor));
            }
        }
    }

    path.pop();
    rec_stack.remove(node);
    None
}

/// 拓扑排序 (Kahn's algorithm)
fn topological_sort(graph: &DependencyGraph) -> Result<Vec<String>> {
    let mut in_degree: HashMap<String, usize> = HashMap::new();
    let mut result = Vec::new();
    let mut queue = VecDeque::new();

    // 初始化入度
    for module_name in graph.modules.keys() {
        in_degree.insert(module_name.clone(), 0);
    }

    // 计算入度
    for deps in graph.edges.values() {
        for dep in deps {
            if let Some(degree) = in_degree.get_mut(dep) {
                *degree += 1;
            }
        }
    }

    // 将入度为 0 的节点加入队列
    for (module_name, &degree) in &in_degree {
        if degree == 0 {
            queue.push_back(module_name.clone());
        }
    }

    // BFS
    while let Some(node) = queue.pop_front() {
        result.push(node.clone());

        if let Some(deps) = graph.edges.get(&node) {
            for dep in deps {
                if let Some(degree) = in_degree.get_mut(dep) {
                    *degree -= 1;
                    if *degree == 0 {
                        queue.push_back(dep.clone());
                    }
                }
            }
        }
    }

    // 反转得到正确的构建顺序 (先构建依赖)
    result.reverse();

    Ok(result)
}

/// 验证层级约束
pub fn validate_layer_constraints(graph: &DependencyGraph) -> Result<()> {
    for (module_name, deps) in &graph.edges {
        let Some(module) = graph.modules.get(module_name) else {
            continue;
        };

        for dep_name in deps {
            let Some(dep) = graph.modules.get(dep_name) else {
                continue;
            };

            // 层级约束：只能依赖同层或更低层的模块
            if dep.layer > module.layer {
                return Err(LbtError::LayerViolation {
                    from: module_name.clone(),
                    from_layer: module.layer,
                    to: dep_name.clone(),
                    to_layer: dep.layer,
                }
                .into());
            }
        }
    }

    Ok(())
}

impl DependencyGraph {
    /// 获取模块的所有传递依赖
    pub fn get_transitive_dependencies(&self, module_name: &str) -> Vec<String> {
        let mut result = Vec::new();
        let mut visited = HashSet::new();
        self.collect_deps(module_name, &mut result, &mut visited);
        result
    }

    fn collect_deps(
        &self,
        module_name: &str,
        result: &mut Vec<String>,
        visited: &mut HashSet<String>,
    ) {
        if visited.contains(module_name) {
            return;
        }
        visited.insert(module_name.to_string());

        if let Some(deps) = self.edges.get(module_name) {
            for dep in deps {
                self.collect_deps(dep, result, visited);
                if !result.contains(dep) {
                    result.push(dep.clone());
                }
            }
        }
    }

    /// 获取模块的公开传递依赖
    pub fn get_public_dependencies(&self, module_name: &str) -> Vec<String> {
        let mut result = Vec::new();
        let mut visited = HashSet::new();
        self.collect_public_deps(module_name, &mut result, &mut visited);
        result
    }

    fn collect_public_deps(
        &self,
        module_name: &str,
        result: &mut Vec<String>,
        visited: &mut HashSet<String>,
    ) {
        if visited.contains(module_name) {
            return;
        }
        visited.insert(module_name.to_string());

        if let Some(module) = self.modules.get(module_name) {
            for dep in &module.config.dependencies.public {
                self.collect_public_deps(dep, result, visited);
                if !result.contains(dep) {
                    result.push(dep.clone());
                }
            }
        }
    }

    /// 获取依赖此模块的所有模块 (反向依赖)
    pub fn get_dependents(&self, module_name: &str) -> Vec<String> {
        self.reverse_edges
            .get(module_name)
            .cloned()
            .unwrap_or_default()
    }

    /// 获取所有传递依赖此模块的模块
    pub fn get_transitive_dependents(&self, module_name: &str) -> Vec<String> {
        let mut result = Vec::new();
        let mut visited = HashSet::new();
        self.collect_reverse_deps(module_name, &mut result, &mut visited);
        result
    }

    fn collect_reverse_deps(
        &self,
        module_name: &str,
        result: &mut Vec<String>,
        visited: &mut HashSet<String>,
    ) {
        if visited.contains(module_name) {
            return;
        }
        visited.insert(module_name.to_string());

        if let Some(dependents) = self.reverse_edges.get(module_name) {
            for dep in dependents {
                if !result.contains(dep) {
                    result.push(dep.clone());
                }
                self.collect_reverse_deps(dep, result, visited);
            }
        }
    }

    /// 获取模块的依赖深度
    pub fn get_depth(&self, module_name: &str) -> usize {
        self.depths.get(module_name).copied().unwrap_or(0)
    }

    /// 获取指定层级的所有模块
    pub fn get_modules_by_layer(&self, layer: u32) -> Vec<String> {
        self.layers.get(&layer).cloned().unwrap_or_default()
    }

    /// 获取依赖统计信息
    pub fn get_stats(&self) -> DependencyStats {
        let total_edges: usize = self.edges.values().map(|v| v.len()).sum();
        let max_depth = self.depths.values().max().copied().unwrap_or(0);
        let avg_deps = if self.modules.is_empty() {
            0.0
        } else {
            total_edges as f64 / self.modules.len() as f64
        };

        let mut modules_by_layer = BTreeMap::new();
        for (layer, modules) in &self.layers {
            modules_by_layer.insert(*layer, modules.len());
        }

        DependencyStats {
            total_modules: self.modules.len(),
            total_edges,
            max_depth,
            avg_dependencies: avg_deps,
            modules_by_layer,
        }
    }

    /// 检查模块是否直接或间接依赖另一个模块
    pub fn depends_on(&self, module: &str, dependency: &str) -> bool {
        self.get_transitive_dependencies(module)
            .contains(&dependency.to_string())
    }

    /// 获取两个模块的共同依赖
    pub fn get_common_dependencies(&self, module_a: &str, module_b: &str) -> Vec<String> {
        let deps_a: HashSet<_> = self
            .get_transitive_dependencies(module_a)
            .into_iter()
            .collect();
        let deps_b: HashSet<_> = self
            .get_transitive_dependencies(module_b)
            .into_iter()
            .collect();
        deps_a.intersection(&deps_b).cloned().collect()
    }

    /// 获取可并行构建的模块组
    pub fn get_parallel_build_groups(&self) -> Vec<Vec<String>> {
        let mut groups = Vec::new();
        let mut remaining: HashSet<_> = self.modules.keys().cloned().collect();
        let mut built: HashSet<String> = HashSet::new();

        while !remaining.is_empty() {
            let mut group = Vec::new();

            for module_name in &remaining {
                let deps = self.edges.get(module_name).cloned().unwrap_or_default();
                if deps.iter().all(|d| built.contains(d)) {
                    group.push(module_name.clone());
                }
            }

            if group.is_empty() {
                break; // 不应该发生，除非有循环依赖
            }

            for name in &group {
                remaining.remove(name);
                built.insert(name.clone());
            }

            groups.push(group);
        }

        groups
    }

    /// 生成 DOT 格式的依赖图 (可用 Graphviz 可视化)
    pub fn to_dot(&self) -> String {
        let mut dot = String::new();
        dot.push_str("digraph Dependencies {\n");
        dot.push_str("    rankdir=BT;\n");
        dot.push_str("    node [shape=box, style=rounded];\n\n");

        // 按层级分组
        for (layer, modules) in &self.layers {
            dot.push_str(&format!("    subgraph cluster_layer{} {{\n", layer));
            dot.push_str(&format!("        label=\"Layer {}\";\n", layer));
            dot.push_str("        style=dashed;\n");
            for module in modules {
                let depth = self.depths.get(module).unwrap_or(&0);
                let color = match layer {
                    0 => "lightblue",
                    1 => "lightgreen",
                    2 => "lightyellow",
                    _ => "lightgray",
                };
                dot.push_str(&format!(
                    "        \"{}\" [fillcolor={}, style=\"filled,rounded\", tooltip=\"depth={}\"];\n",
                    module, color, depth
                ));
            }
            dot.push_str("    }\n\n");
        }

        // 添加边
        for (module, deps) in &self.edges {
            for dep in deps {
                dot.push_str(&format!("    \"{}\" -> \"{}\";\n", module, dep));
            }
        }

        dot.push_str("}\n");
        dot
    }

    /// 生成 Mermaid 格式的依赖图 (可在 Markdown 中显示)
    pub fn to_mermaid(&self) -> String {
        let mut mermaid = String::new();
        mermaid.push_str("```mermaid\ngraph BT\n");

        // 按层级分组
        for (layer, modules) in &self.layers {
            mermaid.push_str(&format!("    subgraph Layer{} [Layer {}]\n", layer, layer));
            for module in modules {
                mermaid.push_str(&format!("        {}\n", module));
            }
            mermaid.push_str("    end\n");
        }

        // 添加边
        for (module, deps) in &self.edges {
            for dep in deps {
                mermaid.push_str(&format!("    {} --> {}\n", module, dep));
            }
        }

        mermaid.push_str("```\n");
        mermaid
    }

    /// 打印依赖树 (ASCII 格式)
    pub fn print_tree(&self) {
        println!("\n依赖树:");
        for module_name in &self.build_order {
            let Some(module) = self.modules.get(module_name) else {
                continue;
            };
            let deps = self.edges.get(module_name).cloned().unwrap_or_default();
            let depth = self.depths.get(module_name).unwrap_or(&0);
            let indent = "  ".repeat(*depth);

            let dep_str = if deps.is_empty() {
                String::new()
            } else {
                format!(" -> [{}]", deps.join(", "))
            };

            println!("{}[L{}] {}{}", indent, module.layer, module_name, dep_str);
        }
    }
}
