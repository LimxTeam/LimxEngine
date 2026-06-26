# LBT - Limx Build Tool

> 模块化 C++ 构建系统，专为 Limx Engine 设计

## 功能特性

### 核心功能
- **模块发现** - 自动扫描 `*.limx.toml` 配置文件
- **依赖解析** - 拓扑排序、循环检测、层级验证
- **CMake 生成** - UE 风格目录结构、PCH、Unity Build
- **VS 解决方案** - 直接生成 `.sln` 和 `.vcxproj`
- **IDE 项目** - VS Code、Rider、CLion 支持

### 编译系统
- **多编译器** - MSVC、Clang、GCC 自动检测
- **并行调度** - DAG 依赖图任务调度
- **增量编译** - ActionCache 智能缓存
- **预编译头 (PCH)** - 自动检测和配置
- **Unity Build** - 合并编译单元加速构建

### 分布式编译
- **协调器模式** - 管理多个工作节点
- **工作节点** - 接收并执行编译任务
- **任务分发** - 自动负载均衡

### 开发工具
- **依赖图可视化** - DOT、Mermaid、ASCII Tree
- **头文件分析** - 依赖追踪和可视化
- **配置验证** - 模块名、层级、依赖检查
- **彩色输出** - 进度条、状态高亮

## 安装

```bash
cd Programs
cargo build --release
```

## 命令列表

| 命令 | 描述 |
|------|------|
| `generate` | 生成 CMake 配置 |
| `build` | 构建项目 (完整流程) |
| `generate-solution` | 生成 VS 解决方案 |
| `generate-project` | 生成 IDE 项目文件 |
| `generate-reflection` | 生成反射代码 (调用 LHT) |
| `new-module` | 创建新模块 |
| `list` | 列出所有模块 |
| `check` | 检查依赖配置 |
| `graph` | 显示依赖图 |
| `stats` | 显示构建统计 |
| `validate` | 验证模块配置 |
| `clean` | 清理构建缓存 |
| `dist-coordinator` | 启动分布式编译协调器 |
| `dist-worker` | 启动分布式编译工作节点 |
| `analyze-deps` | 分析头文件依赖 |

## 使用

### 基本命令

```bash
# 生成 CMake 配置
lbt generate -s Source -o Intermediate/Build

# 生成 VS 解决方案
lbt generate-solution -s Source -n LimxEngine

# 构建项目
lbt build -s Source -c development

# 使用 Clang 构建
lbt build -s Source --compiler clang -j 8

# 启用 PCH 和 Unity Build
lbt build -s Source --pch --unity

# 强制重新编译
lbt build -s Source --rebuild

# 列出模块
lbt list -s Source

# 检查依赖
lbt check -s Source
```

### 分布式编译

```bash
# 启动协调器 (主机)
lbt dist-coordinator -p 19283 --max-workers 100

# 启动工作节点 (从机)
lbt dist-worker -c 192.168.1.100:19283 -j 8
```

### 依赖分析

```bash
# ASCII 树形显示
lbt graph -s Source

# DOT 格式 (Graphviz)
lbt graph -s Source -f dot -o deps.dot

# Mermaid 格式 (Markdown)
lbt graph -s Source -f mermaid

# 头文件依赖分析
lbt analyze-deps -s Source -f json -o deps.json
```

### 构建统计

```bash
# 显示统计信息
lbt stats -s Source

# 验证配置
lbt validate -s Source

# 严格模式 (警告视为错误)
lbt validate -s Source --strict
```

### 清理

```bash
# 清理构建缓存
lbt clean

# 清理所有中间文件
lbt clean --all
```

## 模块配置

模块通过 `ModuleName.limx.toml` 文件配置：

```toml
[module]
name = "LimxCore"
type = "shared"      # static, shared, executable, header-only
layer = 1            # 0-5, 低层不能依赖高层

[dependencies]
public = ["LimxPlatform"]
private = []

[compile]
defines = ["LIMX_CORE_API=__declspec(dllexport)"]
include_paths = []
warnings_as_errors = true

[pch]
enabled = true
header = "LimxCorePCH.h"
```

## 层级架构

```
Layer 0: Platform (平台抽象)
Layer 1: Core (核心系统)
Layer 2: Engine (引擎功能)
Layer 3: Editor (编辑器)
Layer 4: Game (游戏逻辑)
Layer 5: Plugin (插件)
```

## 输出目录结构

```
Binaries/
├── Win64/
│   ├── Debug/
│   ├── Development/
│   └── Release/
Intermediate/
├── Build/          # CMake 文件
├── Generated/      # 反射代码
└── ProjectFiles/   # VS 项目
```

## 测试

```bash
cargo test -p lbt
```

## 许可证

MIT License - LimxTeam
