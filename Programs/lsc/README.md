# LSC - Limx Shader Compiler

Vulkan SPIR-V 着色器编译工具

## 功能特性

- **仅 Vulkan** - 专注单一图形 API，不支持其他后端
- **GLSL/HLSL** - 支持两种着色器语言
- **SPIR-V 输出** - 直接编译为 Vulkan 可用的 SPIR-V
- **并行编译** - 基于 Rayon 的多核并行编译
- **增量编译** - 基于内容哈希的智能缓存
- **反射提取** - 自动提取绑定、Push Constants 等信息
- **文件监控** - 实时监控变化并自动重编译

## 支持的着色器阶段

| 阶段 | 扩展名 |
|------|--------|
| Vertex | `.vert`, `.vs` |
| Fragment | `.frag`, `.fs`, `.ps` |
| Compute | `.comp`, `.cs` |
| Geometry | `.geom`, `.gs` |
| Tessellation Control | `.tesc`, `.tcs` |
| Tessellation Evaluation | `.tese`, `.tes` |
| Ray Generation | `.rgen` |
| Ray Intersection | `.rint` |
| Ray Any Hit | `.rahit` |
| Ray Closest Hit | `.rchit` |
| Ray Miss | `.rmiss` |
| Callable | `.rcall` |
| Task | `.task` |
| Mesh | `.mesh` |

## 使用方法

### 编译单个着色器

```bash
lsc compile -s shader.vert -o shader.spv
lsc compile -s shader.frag -o shader.spv -O  # 启用优化
lsc compile -s shader.comp -o shader.spv -D DEBUG=1 -I ./includes
```

### 批量编译

```bash
lsc compile-all -s Shaders/ -o Intermediate/Shaders/ -O -j
```

### 监视模式

```bash
lsc watch -s Shaders/ -o Intermediate/Shaders/
```

### 提取反射信息

```bash
lsc reflect -s shader.spv -o shader.json
```

### 验证着色器

```bash
lsc validate -s shader.spv
```

## 编译选项

| 选项 | 说明 |
|------|------|
| `-s, --source` | 源文件路径 |
| `-o, --output` | 输出文件路径 |
| `-D, --define` | 预处理器宏定义 |
| `-I, --include` | 头文件包含目录 |
| `-O, --optimize` | 启用优化 |
| `-g, --debug-info` | 生成调试信息 |
| `--target-env` | 目标 Vulkan 版本 (1.0/1.1/1.2/1.3) |
| `--entry-point` | 入口点函数名 (默认: main) |

## 反射信息

LSC 自动提取以下反射信息并输出为 JSON：

- 入口点和着色器阶段
- 输入/输出变量 (Location)
- Uniform Buffers (Set, Binding, 成员布局)
- Storage Buffers
- Push Constants
- 采样器和纹理绑定
- 存储图像
- 特化常量
- 工作组大小 (计算着色器)

## 与 LBT 集成

LSC 设计为与 LBT 构建系统集成：

```bash
# LBT 自动调用 LSC 编译着色器
lbt build --source-dir Source --config Development
```

## 依赖

- shaderc (glslang) - SPIR-V 编译
- notify - 文件监控
- rayon - 并行处理
