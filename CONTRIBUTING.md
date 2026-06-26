<div align="center">

<table><tr><td valign="middle">
<img src="Resources/icon.png" alt="Limx" width="36"/>
</td><td valign="middle">
<img src="https://readme-typing-svg.demolab.com?font=Orbitron&weight=700&size=45&duration=3000&pause=1000&color=A855F7&vCenter=true&width=280&height=52&lines=贡献指南" alt="贡献指南" />
</td></tr></table>

<img src="https://readme-typing-svg.demolab.com?font=Fira+Code&weight=400&size=13&duration=2500&pause=1500&color=E879F9&center=true&vCenter=true&width=320&height=30&lines=%E4%B8%BA+Limx+Engine+%E8%B4%A1%E7%8C%AE%E4%BB%A3%E7%A0%81;Contributing+to+Limx+Engine" alt="subtitle" />

</div>

感谢您对 **Limx Engine** 的关注！我们欢迎所有形式的贡献，无论是代码、文档、Bug 报告还是功能建议。

---

## 目录

- [行为准则](#行为准则)
- [如何贡献](#如何贡献)
  - [报告 Bug](#报告-bug)
  - [提出功能建议](#提出功能建议)
  - [提交代码](#提交代码)
- [开发环境设置](#开发环境设置)
- [代码规范](#代码规范)
- [提交规范](#提交规范)
- [Pull Request 流程](#pull-request-流程)
- [代码审查](#代码审查)
- [许可证](#许可证)

---

## 行为准则

参与本项目即表示您同意遵守我们的行为准则：

- **尊重他人** — 保持友善和专业的交流态度
- **包容多元** — 欢迎来自不同背景的贡献者
- **建设性反馈** — 提供有帮助的、具体的反馈意见
- **专注技术** — 讨论应围绕技术问题展开

---

## 如何贡献

### 报告 Bug

如果您发现了 Bug，请通过 [GitHub Issues](https://github.com/aspect-ux/Limx/issues) 提交报告。

**Bug 报告应包含：**

```markdown
### Bug 描述
[清晰简洁地描述问题]

### 复现步骤
1. 执行 '...'
2. 点击 '...'
3. 观察到 '...'

### 预期行为
[描述您期望发生的情况]

### 实际行为
[描述实际发生的情况]

### 环境信息
- 操作系统: [例如 Windows 11]
- GPU: [例如 RTX 4080]
- 驱动版本: [例如 551.23]
- Vulkan SDK 版本: [例如 1.4.321.1]
- 编译器: [例如 MSVC 19.38]

### 截图/日志
[如有相关截图或日志，请附上]

### 附加信息
[任何其他可能有帮助的信息]
```

### 提出功能建议

我们欢迎新功能的建议！请通过 [GitHub Issues](https://github.com/aspect-ux/Limx/issues) 提交，并使用 `enhancement` 标签。

**功能建议应包含：**

```markdown
### 功能描述
[清晰描述您希望添加的功能]

### 动机
[解释为什么需要这个功能，它解决什么问题]

### 建议的实现方案
[如果有想法，描述可能的实现方式]

### 替代方案
[您考虑过的其他解决方案]

### 附加信息
[任何其他相关信息、参考资料或示例]
```

### 提交代码

1. **Fork** 本仓库
2. **Clone** 您的 Fork
3. 创建**特性分支**
4. 进行修改并**测试**
5. 提交 **Pull Request**

---

## 开发环境设置

### 系统要求

| 组件 | 要求 |
|------|------|
| **操作系统** | Windows 10/11 64-bit |
| **编译器** | MSVC 19.38+ (Visual Studio 2022 17.8+) |
| **C++ 标准** | C++23 |
| **Vulkan SDK** | 1.4.321.1+ |
| **CMake** | 3.28+ |
| **Git** | 2.40+ |

### 构建步骤

```bash
# 1. Fork 并克隆仓库
git clone https://github.com/YOUR_USERNAME/Limx.git
cd Limx

# 2. 添加上游仓库
git remote add upstream https://github.com/aspect-ux/Limx.git

# 3. 创建特性分支
git checkout -b feature/your-feature-name

# 4. 配置项目
cmake -B build -G "Visual Studio 17 2022" -A x64

# 5. 编译 (Debug 模式用于开发)
cmake --build build --config Debug

# 6. 运行测试
ctest --test-dir build -C Debug
```

### 推荐的 IDE 设置

**Visual Studio 2022:**
- 安装 "C++ CMake tools for Windows" 工作负载
- 启用 `/W4 /WX` 警告级别
- 启用 C++23 标准 (`/std:c++latest`)

**VS Code:**
- 安装 C/C++ 扩展
- 安装 CMake Tools 扩展
- 配置 `c_cpp_properties.json` 使用 C++23

---

## 代码规范

Limx Engine 有严格的代码规范，请务必遵守。完整规范请参阅 [项目规则.md](项目规则.md)。

### 核心约束

| 约束项 | 要求 |
|--------|------|
| **C++ 标准** | C++23 |
| **标准库** | **禁止使用** STL |
| **编译警告** | 零警告 (`/W4 /WX`) |

### 命名规范

| 类型 | 规范 | 示例 |
|------|------|------|
| 类/结构体 | PascalCase | `BlockAllocator`, `RenderGraph` |
| 接口 | I + PascalCase | `IAllocator`, `IPlugin` |
| 函数/方法 | PascalCase | `Allocate()`, `CreateBuffer()` |
| 常量 | k + PascalCase | `kMaxSize`, `kDefaultAlignment` |
| 私有成员 | m_ + PascalCase | `m_Capacity`, `m_Data` |
| 静态成员 | s_ + PascalCase | `s_Instance`, `s_Counter` |
| 全局变量 | g_ + PascalCase | `g_Allocator`, `g_Device` |
| 局部变量 | camelCase | `bufferSize`, `elementCount` |
| 命名空间 | PascalCase | `Limx::Memory`, `Limx::RHI` |

### 代码风格

```cpp
// ✓ 正确示例
namespace Limx
{
    class BlockAllocator : public IAllocator
    {
    public:
        void* Allocate(SizeType size, SizeType alignment) override;
        void Deallocate(void* ptr) override;

    private:
        void* m_Memory;
        SizeType m_Capacity;
    };
}

// ✗ 错误示例
namespace Limx {
    class block_allocator : public IAllocator {  // 命名错误，大括号位置错误
    public:
        void* allocate(size_t size);  // 使用了 STL 类型，命名错误
    private:
        void* memory;  // 缺少 m_ 前缀
    };
}
```

### 文件头注释

每个源文件必须包含标准文件头：

```cpp
/*******************************************************************************
 * 文件: [文件名]
 * 创建时间: 2025-12-DD
 * 创建者: [您的名字/GitHub用户名]
 *
 * 功能描述:
 *   [详细描述文件的核心功能]
 *
 * 设计哲学:
 *   [阐述设计决策和架构考虑]
 *
 * 依赖关系:
 *   [内部依赖]
 *   [外部依赖]
 *
 ******************************************************************************/
```

---

## 提交规范

### Commit Message 格式

我们使用 [Conventional Commits](https://www.conventionalcommits.org/) 规范：

```
<type>(<scope>): <subject>

<body>

<footer>
```

### Type 类型

| Type | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 文档更新 |
| `style` | 代码格式（不影响功能） |
| `refactor` | 重构（不是新功能也不是修复） |
| `perf` | 性能优化 |
| `test` | 测试相关 |
| `chore` | 构建/工具相关 |

### Scope 范围

| Scope | 说明 |
|-------|------|
| `platform` | Limx.Platform 模块 |
| `core` | Limx.Core 模块 |
| `luminance` | Limx.Luminance 模块 |
| `neural` | Limx.Neural 模块 |
| `synergy` | Limx.Synergy 模块 |
| `studio` | Limx.Studio 模块 |
| `build` | 构建系统 |
| `ci` | CI/CD |

### 示例

```bash
# 新功能
feat(luminance): add ReSTIR GI implementation

# Bug 修复
fix(core): fix memory leak in BlockAllocator

# 文档
docs(readme): update build instructions

# 性能优化
perf(luminance): optimize VisBuffer culling by 30%

# 重构
refactor(platform): simplify Vulkan device selection logic
```

---

## Pull Request 流程

### 1. 准备工作

```bash
# 确保您的分支是最新的
git fetch upstream
git rebase upstream/main

# 确保代码编译通过且无警告
cmake --build build --config Release

# 运行测试
ctest --test-dir build -C Release
```

### 2. 创建 Pull Request

在 GitHub 上创建 PR 时，请填写以下模板：

```markdown
## 描述
[清晰描述此 PR 的目的和更改内容]

## 更改类型
- [ ] Bug 修复
- [ ] 新功能
- [ ] 重构
- [ ] 文档更新
- [ ] 性能优化
- [ ] 其他

## 相关 Issue
Closes #[issue number]

## 更改内容
- [更改 1]
- [更改 2]
- [更改 3]

## 测试
- [ ] 已在本地编译通过
- [ ] 已运行相关测试
- [ ] 已添加新测试（如适用）

## 检查清单
- [ ] 代码符合项目规范
- [ ] 无编译警告
- [ ] 已更新相关文档（如适用）
- [ ] Commit message 符合规范

## 截图（如适用）
[添加截图以帮助解释您的更改]
```

### 3. PR 要求

- **单一职责** — 每个 PR 只解决一个问题或添加一个功能
- **小而精** — 尽量保持 PR 规模适中，便于审查
- **完整测试** — 确保所有测试通过
- **零警告** — 代码必须无编译警告
- **文档同步** — 如有 API 变更，同步更新文档

---

## 代码审查

### 审查标准

所有 PR 都需要至少一位维护者的审查批准。审查将关注：

| 方面 | 检查内容 |
|------|----------|
| **正确性** | 代码逻辑是否正确，是否解决了问题 |
| **规范性** | 是否符合代码规范和命名约定 |
| **性能** | 是否有性能问题或可优化空间 |
| **安全性** | 是否有内存泄漏、越界等问题 |
| **可维护性** | 代码是否清晰易懂，是否有适当注释 |
| **测试** | 是否有足够的测试覆盖 |

### 审查反馈

- 请认真对待审查意见
- 如有疑问，可以在 PR 中讨论
- 修改后请重新请求审查

---

## 许可证

通过向 Limx Engine 贡献代码，您同意您的贡献将按照 [Apache License 2.0](LICENSE) 进行许可。

---

## 联系我们

- **GitHub Issues** — Bug 报告和功能建议
- **GitHub Discussions** — 一般性讨论
- **Discord** — 实时交流 *(即将开放)*
- **Email** — contact@yunsio.com

---

再次感谢您的贡献！🎉

*Limx Engine — 重塑数字艺术的边界*
