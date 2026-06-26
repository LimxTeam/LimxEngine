// ============================================================
// 文件名称：graph_export.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：超越 UE5 — 提供模块依赖图的多格式高级可视化，
//           包含耦合度热力图、层级违规高亮、扇入/扇出分析、
//           交互式 HTML 报告，UE5 无此功能
// 功能描述：依赖图高级导出器 — DOT (Graphviz) 热力图模式、
//           Mermaid 增强模式、交互式 HTML (内嵌 SVG)、
//           耦合度矩阵、模块健康评分
// 技术特性：基于 DependencyGraph 数据结构，零外部依赖的
//           SVG 生成，自动颜色渐变映射，耦合度量化算法
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ GraphExporter              │ 图导出器主体                  │
// │ ExportOptions              │ 导出配置选项                  │
// │ ModuleMetrics              │ 模块耦合度/健康度量            │
// │ CouplingMatrix             │ 模块间耦合度矩阵              │
// │ GraphColorScheme           │ 颜色方案                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建导出器                    │
// │ export_dot_heatmap()       │ 导出带热力图的 DOT            │
// │ export_mermaid_enhanced()  │ 导出增强 Mermaid 图           │
// │ export_html_interactive()  │ 导出交互式 HTML 可视化         │
// │ compute_module_metrics()   │ 计算模块耦合度/健康指标        │
// │ compute_coupling_matrix()  │ 计算耦合度矩阵                │
// │ export_coupling_csv()      │ 导出耦合度 CSV                │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use std::collections::HashMap;
use std::path::Path;

use crate::core::dependency::DependencyGraph;

// =============================================================================
// 导出选项
// =============================================================================

/// 颜色方案
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GraphColorScheme {
    /// 按层级着色 (默认)
    ByLayer,
    /// 按耦合度热力图 (红=高耦合, 绿=低耦合)
    ByCoupling,
    /// 按依赖深度着色
    ByDepth,
    /// 按扇入数着色 (被依赖的次数)
    ByFanIn,
    /// 按扇出数着色 (依赖的数量)
    ByFanOut,
}

/// 导出选项
#[derive(Debug, Clone)]
pub struct ExportOptions {
    /// 颜色方案
    pub color_scheme: GraphColorScheme,
    /// 是否显示层级分组
    pub show_layer_groups: bool,
    /// 是否显示传递依赖 (虚线)
    pub show_transitive_edges: bool,
    /// 是否高亮层级违规边
    pub highlight_layer_violations: bool,
    /// 是否显示模块指标标签
    pub show_metrics_labels: bool,
    /// 图方向 (BT=底到顶, LR=左到右)
    pub direction: String,
    /// 最大节点标签宽度
    pub max_label_width: usize,
}

impl Default for ExportOptions {
    fn default() -> Self {
        Self {
            color_scheme: GraphColorScheme::ByCoupling,
            show_layer_groups: true,
            show_transitive_edges: false,
            highlight_layer_violations: true,
            show_metrics_labels: true,
            direction: "BT".to_string(),
            max_label_width: 30,
        }
    }
}

// =============================================================================
// 模块指标
// =============================================================================

/// 单个模块的耦合度与健康指标
#[derive(Debug, Clone)]
pub struct ModuleMetrics {
    /// 模块名
    pub module_name: String,
    /// 扇入 (被多少模块直接依赖)
    pub fan_in: usize,
    /// 扇出 (直接依赖多少模块)
    pub fan_out: usize,
    /// 传递扇入 (被多少模块间接依赖)
    pub transitive_fan_in: usize,
    /// 传递扇出 (间接依赖多少模块)
    pub transitive_fan_out: usize,
    /// 不稳定度 I = fan_out / (fan_in + fan_out), 0=稳定, 1=不稳定
    pub instability: f64,
    /// 层级
    pub layer: u32,
    /// 依赖深度
    pub depth: usize,
    /// 耦合度评分 (0~100, 越高越耦合)
    pub coupling_score: f64,
    /// 健康评分 (0~100, 越高越健康)
    pub health_score: f64,
}

/// 耦合度矩阵
#[derive(Debug, Clone)]
pub struct CouplingMatrix {
    /// 模块名列表 (行/列索引)
    pub module_names: Vec<String>,
    /// 矩阵数据: matrix[i][j] = 模块 i 依赖模块 j 的程度 (0=无, 1=直接, 0.5=传递)
    pub matrix: Vec<Vec<f64>>,
}

// =============================================================================
// 图导出器
// =============================================================================

/// 依赖图高级导出器
pub struct GraphExporter<'a> {
    /// 依赖图引用
    graph: &'a DependencyGraph,
    /// 导出选项
    options: ExportOptions,
    /// 缓存的模块指标
    metrics: Vec<ModuleMetrics>,
}

impl<'a> GraphExporter<'a> {
    /// 创建导出器
    pub fn new(graph: &'a DependencyGraph, options: ExportOptions) -> Self {
        let metrics = compute_module_metrics(graph);
        Self {
            graph,
            options,
            metrics,
        }
    }

    /// 使用默认选项创建
    pub fn with_defaults(graph: &'a DependencyGraph) -> Self {
        Self::new(graph, ExportOptions::default())
    }

    /// 获取模块指标
    pub fn metrics(&self) -> &[ModuleMetrics] {
        &self.metrics
    }

    // =========================================================================
    // DOT 热力图导出
    // =========================================================================

    /// 导出带热力图着色的 DOT 格式
    pub fn export_dot_heatmap(&self) -> String {
        let mut dot = String::with_capacity(4096);
        dot.push_str("digraph LimxDependencies {\n");
        dot.push_str(&format!("    rankdir={};\n", self.options.direction));
        dot.push_str(
            "    node [shape=box, style=\"filled,rounded\", fontname=\"Consolas\", fontsize=11];\n",
        );
        dot.push_str("    edge [color=\"#666666\", arrowsize=0.7];\n");
        dot.push_str(
            "    graph [fontname=\"Consolas\", bgcolor=\"#0d1117\", fontcolor=\"#c9d1d9\"];\n",
        );
        dot.push_str("    node [fontcolor=\"#c9d1d9\", color=\"#30363d\"];\n\n");

        // 按层级分组
        if self.options.show_layer_groups {
            for (layer, modules) in &self.graph.layers {
                dot.push_str(&format!("    subgraph cluster_layer{} {{\n", layer));
                dot.push_str(&format!("        label=\"Layer {}\";\n", layer));
                dot.push_str("        style=dashed;\n");
                dot.push_str("        color=\"#30363d\";\n");
                dot.push_str("        fontcolor=\"#8b949e\";\n\n");

                for module_name in modules {
                    let color = self.get_node_color(module_name);
                    let label = self.get_node_label(module_name);
                    dot.push_str(&format!(
                        "        \"{}\" [fillcolor=\"{}\", label=\"{}\"];\n",
                        module_name, color, label,
                    ));
                }
                dot.push_str("    }\n\n");
            }
        } else {
            for module_name in self.graph.modules.keys() {
                let color = self.get_node_color(module_name);
                let label = self.get_node_label(module_name);
                dot.push_str(&format!(
                    "    \"{}\" [fillcolor=\"{}\", label=\"{}\"];\n",
                    module_name, color, label,
                ));
            }
        }

        // 依赖边
        for (module_name, deps) in &self.graph.edges {
            for dep in deps {
                let edge_attrs = self.get_edge_attrs(module_name, dep);
                dot.push_str(&format!(
                    "    \"{}\" -> \"{}\" [{}];\n",
                    module_name, dep, edge_attrs,
                ));
            }
        }

        // 图例
        dot.push_str("\n    // 图例\n");
        dot.push_str("    subgraph cluster_legend {\n");
        dot.push_str("        label=\"图例\";\n");
        dot.push_str("        style=dashed;\n");
        dot.push_str("        color=\"#30363d\";\n");
        dot.push_str("        fontcolor=\"#8b949e\";\n");
        match self.options.color_scheme {
            GraphColorScheme::ByCoupling => {
                dot.push_str("        legend_low [label=\"低耦合\", fillcolor=\"#238636\", fontcolor=\"white\"];\n");
                dot.push_str("        legend_mid [label=\"中耦合\", fillcolor=\"#d29922\", fontcolor=\"white\"];\n");
                dot.push_str("        legend_high [label=\"高耦合\", fillcolor=\"#f85149\", fontcolor=\"white\"];\n");
            }
            GraphColorScheme::ByDepth => {
                dot.push_str("        legend_shallow [label=\"浅层\", fillcolor=\"#58a6ff\", fontcolor=\"white\"];\n");
                dot.push_str("        legend_deep [label=\"深层\", fillcolor=\"#bc8cff\", fontcolor=\"white\"];\n");
            }
            _ => {}
        }
        dot.push_str("    }\n");

        dot.push_str("}\n");
        dot
    }

    // =========================================================================
    // Mermaid 增强导出
    // =========================================================================

    /// 导出增强 Mermaid 图 (带样式和指标)
    pub fn export_mermaid_enhanced(&self) -> String {
        let mut out = String::with_capacity(4096);
        out.push_str(&format!("graph {}\n", self.options.direction));

        // 按层级分组
        if self.options.show_layer_groups {
            for (layer, modules) in &self.graph.layers {
                out.push_str(&format!(
                    "    subgraph Layer{} [\"Layer {}\"]\n",
                    layer, layer
                ));
                for module_name in modules {
                    let metrics = self.find_metrics(module_name);
                    let shape = if metrics.map_or(false, |m| m.fan_in > 5) {
                        // 高扇入模块用六边形
                        format!("{{{{{}}}}}", module_name)
                    } else {
                        format!("[{}]", module_name)
                    };
                    out.push_str(&format!("        {}{}\n", module_name, shape));
                }
                out.push_str("    end\n");
            }
        } else {
            for module_name in self.graph.modules.keys() {
                out.push_str(&format!("    {}[{}]\n", module_name, module_name));
            }
        }

        out.push('\n');

        // 依赖边
        for (module_name, deps) in &self.graph.edges {
            for dep in deps {
                let is_violation = self.is_layer_violation(module_name, dep);
                if is_violation && self.options.highlight_layer_violations {
                    // 层级违规用红色粗线
                    out.push_str(&format!("    {} -..->|违规| {}\n", module_name, dep));
                } else {
                    out.push_str(&format!("    {} --> {}\n", module_name, dep));
                }
            }
        }

        // 样式
        out.push('\n');
        for metrics in &self.metrics {
            let class = if metrics.coupling_score > 70.0 {
                "fill:#f85149,color:#fff"
            } else if metrics.coupling_score > 40.0 {
                "fill:#d29922,color:#fff"
            } else {
                "fill:#238636,color:#fff"
            };
            out.push_str(&format!("    style {} {}\n", metrics.module_name, class));
        }

        out
    }

    // =========================================================================
    // 交互式 HTML 导出
    // =========================================================================

    /// 导出交互式 HTML 可视化 (内嵌 SVG 力导向图)
    pub fn export_html_interactive(&self) -> String {
        let mut html = String::with_capacity(16384);

        // 准备 JSON 数据
        let nodes_json = self.build_nodes_json();
        let edges_json = self.build_edges_json();
        let metrics_json = self.build_metrics_json();

        html.push_str(&format!(r##"<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>Limx Engine 模块依赖图</title>
<style>
* {{ margin: 0; padding: 0; box-sizing: border-box; }}
body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
       background: #0d1117; color: #c9d1d9; overflow: hidden; }}
#canvas {{ width: 100vw; height: 100vh; display: block; }}
.tooltip {{
    position: absolute; display: none; background: #161b22; border: 1px solid #30363d;
    border-radius: 8px; padding: 12px; font-size: 13px; pointer-events: none;
    box-shadow: 0 8px 24px rgba(0,0,0,0.4); max-width: 320px; z-index: 100;
}}
.tooltip h3 {{ color: #58a6ff; margin-bottom: 6px; font-size: 15px; }}
.tooltip .metric {{ display: flex; justify-content: space-between; padding: 2px 0; }}
.tooltip .metric .label {{ color: #8b949e; }}
.tooltip .metric .value {{ font-weight: 600; }}
.toolbar {{
    position: fixed; top: 12px; left: 12px; display: flex; gap: 8px; z-index: 50;
}}
.toolbar button {{
    background: #21262d; border: 1px solid #30363d; color: #c9d1d9;
    padding: 6px 14px; border-radius: 6px; cursor: pointer; font-size: 13px;
}}
.toolbar button:hover {{ background: #30363d; }}
.toolbar button.active {{ background: #58a6ff; color: #fff; border-color: #58a6ff; }}
.legend {{
    position: fixed; bottom: 12px; right: 12px; background: #161b22;
    border: 1px solid #30363d; border-radius: 8px; padding: 12px; font-size: 12px;
}}
.legend-item {{ display: flex; align-items: center; gap: 6px; padding: 2px 0; }}
.legend-dot {{ width: 12px; height: 12px; border-radius: 3px; }}
.stats {{
    position: fixed; top: 12px; right: 12px; background: #161b22;
    border: 1px solid #30363d; border-radius: 8px; padding: 12px; font-size: 12px;
}}
.stats h4 {{ color: #58a6ff; margin-bottom: 4px; }}
</style>
</head>
<body>
<canvas id="canvas"></canvas>
<div class="tooltip" id="tooltip"></div>

<div class="toolbar">
    <button class="active" onclick="setColorMode('coupling')">耦合度</button>
    <button onclick="setColorMode('layer')">层级</button>
    <button onclick="setColorMode('depth')">深度</button>
    <button onclick="setColorMode('fanin')">扇入</button>
</div>

<div class="stats" id="stats">
    <h4>图统计</h4>
    <div>模块数: <strong>{module_count}</strong></div>
    <div>依赖边: <strong>{edge_count}</strong></div>
    <div>最大深度: <strong>{max_depth}</strong></div>
    <div>层级数: <strong>{layer_count}</strong></div>
</div>

<div class="legend" id="legend">
    <div class="legend-item"><div class="legend-dot" style="background:#238636"></div>低耦合 (0-30)</div>
    <div class="legend-item"><div class="legend-dot" style="background:#d29922"></div>中耦合 (30-70)</div>
    <div class="legend-item"><div class="legend-dot" style="background:#f85149"></div>高耦合 (70+)</div>
</div>

<script>
const NODES = {nodes_json};
const EDGES = {edges_json};
const METRICS = {metrics_json};

const canvas = document.getElementById('canvas');
const ctx = canvas.getContext('2d');
const tooltip = document.getElementById('tooltip');
let colorMode = 'coupling';
let dragging = null;
let hoveredNode = null;
let offsetX = 0, offsetY = 0;
let scale = 1;

// 初始化节点位置 (按层级排列)
NODES.forEach((node, i) => {{
    const layerNodes = NODES.filter(n => n.layer === node.layer);
    const layerIndex = layerNodes.indexOf(node);
    node.x = 200 + layerIndex * 180;
    node.y = canvas.height - 120 - node.layer * 160;
    node.vx = 0;
    node.vy = 0;
}});

function resize() {{
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
    draw();
}}
window.addEventListener('resize', resize);
resize();

function getNodeColor(node) {{
    const m = METRICS[node.name] || {{}};
    switch(colorMode) {{
        case 'coupling':
            return scoreToColor(m.coupling_score || 0);
        case 'layer':
            const layerColors = ['#58a6ff','#3fb950','#d29922','#f85149','#bc8cff','#f0883e'];
            return layerColors[node.layer % layerColors.length];
        case 'depth':
            return scoreToColor((m.depth || 0) / Math.max(1, {max_depth}) * 100);
        case 'fanin':
            return scoreToColor(Math.min(100, (m.fan_in || 0) * 15));
        default:
            return '#21262d';
    }}
}}

function scoreToColor(score) {{
    if (score < 30) return '#238636';
    if (score < 50) return '#56d364';
    if (score < 70) return '#d29922';
    if (score < 85) return '#f0883e';
    return '#f85149';
}}

function draw() {{
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.save();
    ctx.translate(offsetX, offsetY);
    ctx.scale(scale, scale);

    // 绘制边
    EDGES.forEach(edge => {{
        const from = NODES.find(n => n.name === edge.from);
        const to = NODES.find(n => n.name === edge.to);
        if (!from || !to) return;

        ctx.beginPath();
        ctx.moveTo(from.x, from.y);
        // 贝塞尔曲线
        const midY = (from.y + to.y) / 2;
        ctx.quadraticCurveTo(from.x, midY, to.x, to.y);

        if (edge.violation) {{
            ctx.strokeStyle = '#f8514980';
            ctx.lineWidth = 2.5;
            ctx.setLineDash([6, 4]);
        }} else if (hoveredNode && (edge.from === hoveredNode.name || edge.to === hoveredNode.name)) {{
            ctx.strokeStyle = '#58a6ff';
            ctx.lineWidth = 2;
            ctx.setLineDash([]);
        }} else {{
            ctx.strokeStyle = '#30363d';
            ctx.lineWidth = 1;
            ctx.setLineDash([]);
        }}
        ctx.stroke();
        ctx.setLineDash([]);

        // 箭头
        const angle = Math.atan2(to.y - midY, to.x - from.x);
        const arrowLen = 8;
        ctx.beginPath();
        ctx.moveTo(to.x, to.y);
        ctx.lineTo(to.x - arrowLen * Math.cos(angle - 0.3), to.y - arrowLen * Math.sin(angle - 0.3));
        ctx.lineTo(to.x - arrowLen * Math.cos(angle + 0.3), to.y - arrowLen * Math.sin(angle + 0.3));
        ctx.closePath();
        ctx.fillStyle = edge.violation ? '#f85149' : '#30363d';
        ctx.fill();
    }});

    // 绘制节点
    NODES.forEach(node => {{
        const w = Math.max(80, node.name.length * 8 + 20);
        const h = 36;
        const r = 6;
        const x = node.x - w/2, y = node.y - h/2;

        // 圆角矩形
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.lineTo(x + w - r, y);
        ctx.arcTo(x + w, y, x + w, y + r, r);
        ctx.lineTo(x + w, y + h - r);
        ctx.arcTo(x + w, y + h, x + w - r, y + h, r);
        ctx.lineTo(x + r, y + h);
        ctx.arcTo(x, y + h, x, y + h - r, r);
        ctx.lineTo(x, y + r);
        ctx.arcTo(x, y, x + r, y, r);
        ctx.closePath();

        ctx.fillStyle = getNodeColor(node);
        ctx.fill();
        ctx.strokeStyle = hoveredNode === node ? '#58a6ff' : '#30363d';
        ctx.lineWidth = hoveredNode === node ? 2 : 1;
        ctx.stroke();

        // 文字
        ctx.fillStyle = '#ffffff';
        ctx.font = '12px Consolas, monospace';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(node.name, node.x, node.y);
    }});

    ctx.restore();
}}

// 力导向布局模拟
function simulate() {{
    const repulsion = 5000;
    const attraction = 0.005;
    const damping = 0.9;

    NODES.forEach(a => {{
        a.vx = 0; a.vy = 0;
        NODES.forEach(b => {{
            if (a === b) return;
            const dx = a.x - b.x;
            const dy = a.y - b.y;
            const dist = Math.max(1, Math.sqrt(dx*dx + dy*dy));
            const force = repulsion / (dist * dist);
            a.vx += dx / dist * force;
            a.vy += dy / dist * force;
        }});
    }});

    EDGES.forEach(edge => {{
        const from = NODES.find(n => n.name === edge.from);
        const to = NODES.find(n => n.name === edge.to);
        if (!from || !to) return;
        const dx = to.x - from.x;
        const dy = to.y - from.y;
        const dist = Math.sqrt(dx*dx + dy*dy);
        const force = dist * attraction;
        from.vx += dx / dist * force;
        from.vy += dy / dist * force;
        to.vx -= dx / dist * force;
        to.vy -= dy / dist * force;
    }});

    // 层级 Y 约束
    NODES.forEach(node => {{
        const targetY = canvas.height - 120 - node.layer * 160;
        node.vy += (targetY - node.y) * 0.05;
    }});

    NODES.forEach(node => {{
        if (node === dragging) return;
        node.vx *= damping;
        node.vy *= damping;
        node.x += node.vx;
        node.y += node.vy;
    }});

    draw();
    requestAnimationFrame(simulate);
}}
simulate();

// 交互
canvas.addEventListener('mousemove', e => {{
    const mx = (e.clientX - offsetX) / scale;
    const my = (e.clientY - offsetY) / scale;
    hoveredNode = null;
    NODES.forEach(node => {{
        const w = Math.max(80, node.name.length * 8 + 20);
        if (Math.abs(mx - node.x) < w/2 && Math.abs(my - node.y) < 18) {{
            hoveredNode = node;
        }}
    }});

    if (hoveredNode) {{
        const m = METRICS[hoveredNode.name] || {{}};
        tooltip.style.display = 'block';
        tooltip.style.left = (e.clientX + 16) + 'px';
        tooltip.style.top = (e.clientY + 16) + 'px';
        tooltip.innerHTML = `<h3>${{hoveredNode.name}}</h3>
            <div class="metric"><span class="label">层级</span><span class="value">Layer ${{hoveredNode.layer}}</span></div>
            <div class="metric"><span class="label">扇入</span><span class="value">${{m.fan_in || 0}}</span></div>
            <div class="metric"><span class="label">扇出</span><span class="value">${{m.fan_out || 0}}</span></div>
            <div class="metric"><span class="label">不稳定度</span><span class="value">${{(m.instability || 0).toFixed(2)}}</span></div>
            <div class="metric"><span class="label">耦合度</span><span class="value">${{(m.coupling_score || 0).toFixed(0)}}%</span></div>
            <div class="metric"><span class="label">健康度</span><span class="value">${{(m.health_score || 0).toFixed(0)}}%</span></div>`;
    }} else {{
        tooltip.style.display = 'none';
    }}

    if (dragging) {{
        dragging.x = mx;
        dragging.y = my;
    }}
}});

canvas.addEventListener('mousedown', e => {{
    if (hoveredNode) {{ dragging = hoveredNode; }}
}});
canvas.addEventListener('mouseup', () => {{ dragging = null; }});
canvas.addEventListener('wheel', e => {{
    e.preventDefault();
    const factor = e.deltaY > 0 ? 0.9 : 1.1;
    scale *= factor;
    scale = Math.max(0.2, Math.min(3, scale));
}});

function setColorMode(mode) {{
    colorMode = mode;
    document.querySelectorAll('.toolbar button').forEach(b => b.classList.remove('active'));
    event.target.classList.add('active');
}}
</script>
</body>
</html>"##,
            module_count = self.graph.modules.len(),
            edge_count = self.graph.edges.values().map(|v| v.len()).sum::<usize>(),
            max_depth = self.graph.depths.values().max().copied().unwrap_or(0),
            layer_count = self.graph.layers.len(),
            nodes_json = nodes_json,
            edges_json = edges_json,
            metrics_json = metrics_json,
        ));

        html
    }

    // =========================================================================
    // 耦合度矩阵
    // =========================================================================

    /// 计算耦合度矩阵
    pub fn compute_coupling_matrix(&self) -> CouplingMatrix {
        let mut names: Vec<String> = self.graph.modules.keys().cloned().collect();
        names.sort();

        let size = names.len();
        let mut matrix = vec![vec![0.0f64; size]; size];

        let name_index: HashMap<&str, usize> = names
            .iter()
            .enumerate()
            .map(|(i, n)| (n.as_str(), i))
            .collect();

        for (module_name, deps) in &self.graph.edges {
            let Some(&from_idx) = name_index.get(module_name.as_str()) else {
                continue;
            };

            // 直接依赖 = 1.0
            for dep in deps {
                if let Some(&to_idx) = name_index.get(dep.as_str()) {
                    matrix[from_idx][to_idx] = 1.0;
                }
            }

            // 传递依赖 = 0.5
            let transitive = self.graph.get_transitive_dependencies(module_name);
            for trans_dep in &transitive {
                if let Some(&to_idx) = name_index.get(trans_dep.as_str()) {
                    if matrix[from_idx][to_idx] == 0.0 {
                        matrix[from_idx][to_idx] = 0.5;
                    }
                }
            }
        }

        CouplingMatrix {
            module_names: names,
            matrix,
        }
    }

    /// 导出耦合度矩阵为 CSV
    pub fn export_coupling_csv(&self, output_path: &Path) -> std::io::Result<()> {
        let coupling = self.compute_coupling_matrix();
        let mut csv = String::with_capacity(4096);

        // 表头
        csv.push(',');
        csv.push_str(&coupling.module_names.join(","));
        csv.push('\n');

        // 数据行
        for (i, row) in coupling.matrix.iter().enumerate() {
            csv.push_str(&coupling.module_names[i]);
            for val in row {
                csv.push_str(&format!(",{:.1}", val));
            }
            csv.push('\n');
        }

        std::fs::write(output_path, csv)
    }

    // =========================================================================
    // 内部辅助
    // =========================================================================

    /// 根据颜色方案获取节点颜色
    fn get_node_color(&self, module_name: &str) -> String {
        let metrics = self.find_metrics(module_name);

        match self.options.color_scheme {
            GraphColorScheme::ByLayer => {
                let layer = self
                    .graph
                    .modules
                    .get(module_name)
                    .map(|m| m.layer as u32)
                    .unwrap_or(0);
                match layer {
                    0 => "#58a6ff".to_string(),
                    1 => "#3fb950".to_string(),
                    2 => "#d29922".to_string(),
                    3 => "#f85149".to_string(),
                    4 => "#bc8cff".to_string(),
                    _ => "#f0883e".to_string(),
                }
            }
            GraphColorScheme::ByCoupling => {
                let score = metrics.map(|m| m.coupling_score).unwrap_or(0.0);
                score_to_hex_color(score)
            }
            GraphColorScheme::ByDepth => {
                let depth = self.graph.depths.get(module_name).copied().unwrap_or(0);
                let max_depth = self
                    .graph
                    .depths
                    .values()
                    .max()
                    .copied()
                    .unwrap_or(1)
                    .max(1);
                let ratio = depth as f64 / max_depth as f64 * 100.0;
                score_to_hex_color(ratio)
            }
            GraphColorScheme::ByFanIn => {
                let fan_in = metrics.map(|m| m.fan_in).unwrap_or(0);
                let score = (fan_in as f64 * 15.0).min(100.0);
                score_to_hex_color(score)
            }
            GraphColorScheme::ByFanOut => {
                let fan_out = metrics.map(|m| m.fan_out).unwrap_or(0);
                let score = (fan_out as f64 * 15.0).min(100.0);
                score_to_hex_color(score)
            }
        }
    }

    /// 获取节点标签
    fn get_node_label(&self, module_name: &str) -> String {
        if self.options.show_metrics_labels {
            if let Some(metrics) = self.find_metrics(module_name) {
                return format!(
                    "{}\\nI={:.2} C={:.0}",
                    module_name, metrics.instability, metrics.coupling_score,
                );
            }
        }
        module_name.to_string()
    }

    /// 获取边属性
    fn get_edge_attrs(&self, from: &str, to: &str) -> String {
        if self.options.highlight_layer_violations && self.is_layer_violation(from, to) {
            "color=\"#f85149\", penwidth=2.5, style=dashed, label=\"违规\"".to_string()
        } else {
            "color=\"#484f58\"".to_string()
        }
    }

    /// 检查是否层级违规
    fn is_layer_violation(&self, from: &str, to: &str) -> bool {
        let from_layer = self.graph.modules.get(from).map(|m| m.layer).unwrap_or(0);
        let to_layer = self.graph.modules.get(to).map(|m| m.layer).unwrap_or(0);
        to_layer > from_layer
    }

    /// 查找模块指标
    fn find_metrics(&self, module_name: &str) -> Option<&ModuleMetrics> {
        self.metrics.iter().find(|m| m.module_name == module_name)
    }

    /// 构建 JSON 节点数组
    fn build_nodes_json(&self) -> String {
        let mut parts = Vec::new();
        for (name, module) in &self.graph.modules {
            parts.push(format!(
                r#"{{"name":"{}","layer":{}}}"#,
                name, module.layer as u32,
            ));
        }
        format!("[{}]", parts.join(","))
    }

    /// 构建 JSON 边数组
    fn build_edges_json(&self) -> String {
        let mut parts = Vec::new();
        for (from, deps) in &self.graph.edges {
            for to in deps {
                let violation = self.is_layer_violation(from, to);
                parts.push(format!(
                    r#"{{"from":"{}","to":"{}","violation":{}}}"#,
                    from, to, violation,
                ));
            }
        }
        format!("[{}]", parts.join(","))
    }

    /// 构建 JSON 指标映射
    fn build_metrics_json(&self) -> String {
        let mut parts = Vec::new();
        for m in &self.metrics {
            parts.push(format!(
                r#""{}": {{"fan_in":{},"fan_out":{},"instability":{:.3},"coupling_score":{:.1},"health_score":{:.1},"depth":{}}}"#,
                m.module_name, m.fan_in, m.fan_out, m.instability,
                m.coupling_score, m.health_score, m.depth,
            ));
        }
        format!("{{{}}}", parts.join(","))
    }
}

// =============================================================================
// 模块指标计算
// =============================================================================

/// 计算所有模块的耦合度与健康指标
pub fn compute_module_metrics(graph: &DependencyGraph) -> Vec<ModuleMetrics> {
    graph
        .modules
        .keys()
        .map(|name| {
            let fan_in = graph.reverse_edges.get(name).map(|v| v.len()).unwrap_or(0);
            let fan_out = graph.edges.get(name).map(|v| v.len()).unwrap_or(0);
            let transitive_fan_in = graph.get_transitive_dependents(name).len();
            let transitive_fan_out = graph.get_transitive_dependencies(name).len();
            let depth = graph.depths.get(name).copied().unwrap_or(0);
            let layer = graph.modules.get(name).map(|m| m.layer as u32).unwrap_or(0);

            // 不稳定度: I = Ce / (Ca + Ce)
            // Ce = 传出耦合 (fan_out), Ca = 传入耦合 (fan_in)
            let instability = if fan_in + fan_out > 0 {
                fan_out as f64 / (fan_in + fan_out) as f64
            } else {
                0.0
            };

            // 耦合度评分: 综合考虑扇入、扇出、传递依赖
            let coupling_score =
                compute_coupling_score(fan_in, fan_out, transitive_fan_in, transitive_fan_out);

            // 健康度评分: 低耦合 + 合理层级 + 不稳定度符合预期
            let health_score = compute_health_score(coupling_score, instability, layer, depth);

            ModuleMetrics {
                module_name: name.clone(),
                fan_in,
                fan_out,
                transitive_fan_in,
                transitive_fan_out,
                instability,
                layer,
                depth,
                coupling_score,
                health_score,
            }
        })
        .collect()
}

/// 计算耦合度评分 (0~100)
fn compute_coupling_score(fan_in: usize, fan_out: usize, trans_in: usize, trans_out: usize) -> f64 {
    // 扇入权重 0.2, 扇出权重 0.3, 传递扇入 0.15, 传递扇出 0.35
    let score = (fan_in as f64 * 4.0).min(25.0)
        + (fan_out as f64 * 6.0).min(35.0)
        + (trans_in as f64 * 1.5).min(15.0)
        + (trans_out as f64 * 2.5).min(25.0);
    score.min(100.0)
}

/// 计算健康评分 (0~100)
fn compute_health_score(coupling_score: f64, instability: f64, layer: u32, depth: usize) -> f64 {
    let mut score = 100.0;

    // 高耦合扣分
    score -= coupling_score * 0.5;

    // 高层模块不稳定度应该高 (接近 1.0), 低层模块不稳定度应该低 (接近 0.0)
    // 稳定依赖原则 (SDP)
    let expected_instability = layer as f64 / 5.0; // 假设最多 5 层
    let instability_deviation = (instability - expected_instability).abs();
    score -= instability_deviation * 20.0;

    // 过深的依赖链扣分
    if depth > 5 {
        score -= (depth as f64 - 5.0) * 3.0;
    }

    score.clamp(0.0, 100.0)
}

/// 评分映射到十六进制颜色
fn score_to_hex_color(score: f64) -> String {
    if score < 30.0 {
        "#238636".to_string()
    } else if score < 50.0 {
        "#56d364".to_string()
    } else if score < 70.0 {
        "#d29922".to_string()
    } else if score < 85.0 {
        "#f0883e".to_string()
    } else {
        "#f85149".to_string()
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::config::{Dependencies, Module, ModuleConfig, ModuleInfo, ModuleType};
    use std::path::PathBuf;

    fn make_module(name: &str, layer: u8, public_deps: Vec<&str>) -> Module {
        Module {
            name: name.to_string(),
            module_type: ModuleType::Static,
            namespace: None,
            layer,
            path: PathBuf::from(format!("Source/{}", name)),
            config_path: PathBuf::from(format!("Source/{}/{}.limx.toml", name, name)),
            config: ModuleConfig {
                module: ModuleInfo {
                    name: name.to_string(),
                    r#type: ModuleType::Static,
                    namespace: None,
                    api_macro: None,
                    layer,
                    description: None,
                },
                dependencies: Dependencies {
                    public: public_deps.into_iter().map(String::from).collect(),
                    private: Vec::new(),
                },
                sources: Default::default(),
                compile: Default::default(),
                precompiled_header: Default::default(),
                reflection: Default::default(),
            },
        }
    }

    fn make_test_graph() -> DependencyGraph {
        let modules = vec![
            make_module("Platform", 0, vec![]),
            make_module("Core", 1, vec!["Platform"]),
            make_module("Renderer", 2, vec!["Core", "Platform"]),
            make_module("Physics", 2, vec!["Core"]),
            make_module("Editor", 3, vec!["Renderer", "Physics", "Core"]),
        ];
        crate::core::dependency::resolve_dependencies(&modules).unwrap()
    }

    #[test]
    fn test_module_metrics_computation() {
        let graph = make_test_graph();
        let metrics = compute_module_metrics(&graph);

        assert_eq!(metrics.len(), 5);

        // Platform 应该有最高扇入 (被最多模块依赖)
        let platform = metrics
            .iter()
            .find(|m| m.module_name == "Platform")
            .unwrap();
        assert!(platform.fan_in >= 2);
        assert_eq!(platform.fan_out, 0);
        assert!((platform.instability - 0.0).abs() < 0.001);

        // Editor 应该有最高扇出
        let editor = metrics.iter().find(|m| m.module_name == "Editor").unwrap();
        assert!(editor.fan_out >= 3);
    }

    #[test]
    fn test_coupling_matrix() {
        let graph = make_test_graph();
        let exporter = GraphExporter::with_defaults(&graph);
        let matrix = exporter.compute_coupling_matrix();

        assert_eq!(matrix.module_names.len(), 5);
        assert_eq!(matrix.matrix.len(), 5);

        // Core -> Platform 应该是直接依赖 (1.0)
        let core_idx = matrix
            .module_names
            .iter()
            .position(|n| n == "Core")
            .unwrap();
        let platform_idx = matrix
            .module_names
            .iter()
            .position(|n| n == "Platform")
            .unwrap();
        assert!((matrix.matrix[core_idx][platform_idx] - 1.0).abs() < 0.001);
    }

    #[test]
    fn test_dot_heatmap_output() {
        let graph = make_test_graph();
        let exporter = GraphExporter::with_defaults(&graph);
        let dot = exporter.export_dot_heatmap();

        assert!(dot.contains("digraph LimxDependencies"));
        assert!(dot.contains("Platform"));
        assert!(dot.contains("Core"));
        assert!(dot.contains("Renderer"));
        assert!(dot.contains("Editor"));
        assert!(dot.contains("Layer"));
    }

    #[test]
    fn test_mermaid_enhanced_output() {
        let graph = make_test_graph();
        let exporter = GraphExporter::with_defaults(&graph);
        let mermaid = exporter.export_mermaid_enhanced();

        assert!(mermaid.contains("graph BT"));
        assert!(mermaid.contains("Platform"));
        assert!(mermaid.contains("style"));
    }

    #[test]
    fn test_html_interactive_output() {
        let graph = make_test_graph();
        let exporter = GraphExporter::with_defaults(&graph);
        let html = exporter.export_html_interactive();

        assert!(html.contains("<!DOCTYPE html>"));
        assert!(html.contains("Limx Engine 模块依赖图"));
        assert!(html.contains("NODES"));
        assert!(html.contains("EDGES"));
        assert!(html.contains("METRICS"));
    }

    #[test]
    fn test_health_score_range() {
        let graph = make_test_graph();
        let metrics = compute_module_metrics(&graph);

        for m in &metrics {
            assert!(
                m.health_score >= 0.0 && m.health_score <= 100.0,
                "模块 {} 健康度 {} 超出范围",
                m.module_name,
                m.health_score
            );
            assert!(
                m.coupling_score >= 0.0 && m.coupling_score <= 100.0,
                "模块 {} 耦合度 {} 超出范围",
                m.module_name,
                m.coupling_score
            );
            assert!(
                m.instability >= 0.0 && m.instability <= 1.0,
                "模块 {} 不稳定度 {} 超出范围",
                m.module_name,
                m.instability
            );
        }
    }

    #[test]
    fn test_layer_violation_detection() {
        let graph = make_test_graph();
        let exporter = GraphExporter::with_defaults(&graph);

        // Renderer (L2) -> Core (L1): 不是违规 (依赖低层)
        assert!(!exporter.is_layer_violation("Renderer", "Core"));

        // 如果 Core (L1) -> Editor (L3): 这是违规 (依赖高层)
        // 但当前图中不存在这种边，仅测试逻辑
        assert!(exporter.is_layer_violation("Core", "Editor"));
    }

    #[test]
    fn test_color_schemes() {
        let graph = make_test_graph();

        for scheme in [
            GraphColorScheme::ByLayer,
            GraphColorScheme::ByCoupling,
            GraphColorScheme::ByDepth,
            GraphColorScheme::ByFanIn,
            GraphColorScheme::ByFanOut,
        ] {
            let options = ExportOptions {
                color_scheme: scheme,
                ..Default::default()
            };
            let exporter = GraphExporter::new(&graph, options);
            let dot = exporter.export_dot_heatmap();
            assert!(
                dot.contains("fillcolor="),
                "方案 {:?} 应包含填充颜色",
                scheme
            );
        }
    }
}
