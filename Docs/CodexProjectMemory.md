# Limx Engine Codex Project Memory

> This file is a handover memory created from a read-only project takeover pass.
> No build, generator, shader compilation, or executable run was intentionally
> performed for this survey after the user clarified the scope.

## Scope Of This Memory

- Workspace root: `C:\Development\LimxEngine`
- Project state inspected by reading Markdown, TOML, C++/GLSL/Rust source, and
  file layout.
- The root directory is not currently a Git repository.
- No build artifacts were present when inspected:
  `Binaries/`, `Intermediate/`, `Logs/`, `Programs/target/`, and
  `Binaries/Shaders/` were absent.
- Treat this as a source snapshot unless Git metadata is restored elsewhere.

## Non-Negotiable Project Rules

- C++ target is C++23, Windows x64, MSVC-oriented.
- Project rules require zero warnings and zero errors.
- Do not use STL in engine C++ source. Use Limx Core replacements.
- Do not use C standard integer/string/math headers in engine C++ source.
- Do not use raw `new/delete` in normal engine code. Use engine allocators and
  Core smart pointer helpers.
- Use project scalar types: `Int32`, `UInt32`, `Float32`, `SizeType`, etc.
- Preserve the existing large file-header style when adding C++ source files.
- Follow Allman braces and 4-space indentation.
- Before large or architecture-changing implementation, provide a concrete plan
  and get confirmation.

## Documentation Reality Check

Important first-party Markdown files:

- `README.md`: broad architecture and product vision. Much of it is aspirational
  and stale relative to current source layout.
- `Docs/LimxEngine产品白皮书.md`: product/investor whitepaper. Useful because it
  explicitly frames the project as early-stage, with M0/M1 milestones.
- `项目规则.md`: mandatory engineering rules.
- `.windsurf/rules/limx.md`: mandatory workspace rules and Core API quick
  reference. Some module names inside are stale.
- `CONTRIBUTING.md`: contribution/build expectations.
- `Programs/lbt/README.md`: Limx Build Tool overview.
- `Programs/lht/README.md`: Limx Header Tool/reflection overview.
- `Programs/lsc/README.md`: Limx Shader Compiler overview.

There are 53 Markdown files total. Most remaining Markdown files are vendored
third-party docs under `Source/ThirdParty/DLSS_Sample_App/donut/thirdparty`,
plus Intel OIDN and NVIDIA DLSS docs.

Main doc mismatch:

- README talks about modules such as `Platform`, `Luminance`, `Neural`,
  `Synergy`, `Studio`, USD, ReSTIR, VisBuffer, NRC, etc.
- Current source has `Core`, `ApplicationCore`, `RHI`, `RenderCore`, `Renderer`,
  `Object`, `Engine`, and `Editor/Launch`.
- Current runtime is a foundational Win32/Vulkan demo renderer and scene bridge,
  not a complete editor, asset pipeline, neural renderer, or USD pipeline.

## Module Graph

Runtime/editor modules from `.limx.toml`:

- `LimxCore` layer 1: base types, containers, memory, math, logging, HAL,
  threading, reflection macro markers.
- `LimxRHI` layer 2: RHI interfaces and Vulkan implementation. Depends on Core.
- `LimxApplicationCore` layer 2: Win32 window/input/clipboard/cursor. Depends on
  Core and RHI.
- `LimxObject` layer 3: `LObject`, `LType`, `LRegistry`, object macros.
  Depends on Core.
- `LimxRenderCore` layer 3: render context, shader manager, camera, materials,
  lights, geometry generator. Depends on Core, RHI, ApplicationCore.
- `LimxRenderer` layer 3: `FRenderer`, render pass system, depth prepass,
  forward pass. Depends on Core, RHI, ApplicationCore, RenderCore.
- `LimxEngine` layer 4: scene graph, traits, systems, render bridge. Depends on
  Core, Object, RHI, ApplicationCore, RenderCore, Renderer.
- `LimxLaunch` executable layer 4: Win32 startup and main loop. Depends on the
  above runtime modules.

External module declarations:

- `NvidiaDLSS`: external headers/libs only, `WITH_DLSS=1`.
- `IntelOIDN`: external headers/libs only, `WITH_INTELOIDN=1`.

Search found no first-party DLSS/OIDN integration calls outside third-party
packages and TOML declarations.

## Current Runtime Flow

Actual entry:

1. `Source/Editor/Launch/Private/main.cpp` uses `wWinMain`.
2. It sets current directory by stripping the executable path four levels.
3. It initializes file logging at `Logs/LimxEngine.log`.
4. It creates `FWindow` at 1280x720.
5. It creates `FRenderContext`, which creates RHI device, swapchain, frame sync.
6. It initializes `FRenderer`.
7. `FRenderer` creates three procedural render objects: cube, sphere, ground.
8. Launch creates `LScene` and wraps those existing GPU objects into
   `LNode + LMeshTrait` using `BuildDemoScene`.
9. Per frame:
   - `window.ProcessMessages()`
   - `scene->Tick(deltaTime)`
   - `FSceneManager::SyncScene(scene, deltaTime)`
   - `renderer.RenderFrame()`
10. Shutdown is scene, scene manager, renderer, render context, window, log sink.

This is a bridge-style demo data flow. The renderer still owns the real GPU
buffers; scene traits reference those handles.

## Core Runtime Areas

### ApplicationCore

- `FWindow`: thin Win32 wrapper, message pump, resize/minimize/close handling,
  raw mouse input registration, Win32 message routing to input manager.
- `FInputManager`: singleton polling input state, 256 key states, mouse position,
  raw mouse delta, mouse button state.
- `FCursorManager`: cursor visibility/lock/stack management.
- `FClipboard`: static text clipboard utility.

Risk:

- `EMouseButton` includes `Thumb1`, `Thumb2`, and `None`, but
  `m_MouseButtons` has only 3 entries. Querying or setting buttons outside
  Left/Right/Middle can index out of bounds.

### Core

Core is the largest current module. It provides the engine substitutes for STL
and CRT usage:

- Containers: `TArray`, `TMap`, `TSet`, `FString`, `FName`, dense/sparse/ring
  and fixed containers.
- Templates: `TUniquePtr`, `TSharedPtr`, `TWeakPtr`, `TOptional`, `TFunction`,
  `TResult`, `TVariant`, handles, observers, futures, state machines.
- Memory: default allocator, block/pool/stack/ring/range allocators, memory ops.
- Math: vector, matrix, quaternion, transform, color, geometry primitives,
  splines, AABB tree, spectral color helpers.
- HAL: platform types, platform file/time/memory.
- Logging, events, console variables, timers, subsystem helpers, threading.

Current rule check by search:

- No real STL include or `std::` engine usage was found in runtime/editor C++
  source outside explanatory comments.
- Runtime external includes found were Win32 and Vulkan headers.
- Several `#pragma warning(disable: 4273)` exist for CRT forward declarations.
  These conflict with the written "do not disable warnings" rule and need a
  deliberate policy decision.

Known Core rule tension:

- `FThread` uses raw `new ThreadContext()` and `delete context`; LBT currently
  exempts this file.
- `TRefCounted::Delete()` uses raw `delete`; LBT exempts this file.
- These are not normal business-code violations but should be revisited if the
  memory rule is intended to be absolute.

### Object

Object system:

- `LObject`: GUID, name, flags, virtual lifecycle hooks, runtime type API.
- `LType`: name, parent, size, factory, global type registry.
- `LRegistry`: singleton object registry and factory/destroy path.
- `LOBJECT_BODY`, `IMPLEMENT_LTYPE`, `IMPLEMENT_LTYPE_ABSTRACT`, `LCast`.

Critical risk:

- `LRegistry::Destroy(LObject* obj)` calls `obj->~LObject()` explicitly.
- Even though `LObject` has a virtual destructor, this explicit base destructor
  call does not safely dispatch the derived destructor chain.
- Consequence: derived destructors such as `LScene::~LScene`,
  `LNode::~LNode`, and `LSpatialTrait::~LSpatialTrait` may not run through this
  destroy path. Scene-owned systems/nodes/traits can leak or skip cleanup.
- This must be fixed or proven safe before expanding object lifecycle features.

### Engine

Engine layer:

- `LScene`: top-level runtime context, owns nodes and systems, supports delayed
  node removal via `m_PendingRemove`, drives `OnBegin/Tick/OnEnd`.
- `LNode`: trait container and transform delegation through root spatial trait.
- `LTrait`: attachable behavior base with enable state.
- `LSpatialTrait`: parent/child transform hierarchy.
- `LSystem`: abstract scene service.
- `LMeshTrait`: builds `FRenderObject` snapshots from GPU handles/material.
- `LLightTrait`: registers to `FLightManager`, syncs light transform/params.
- `LCameraTrait`: builds view/projection matrices and marks main camera.
- `FSceneManager`: singleton bridge from `LScene` to `FRenderer`.

Important current limitation:

- `FSceneManager` only updates renderer camera position from `LCameraTrait`;
  it computes view/projection matrices but discards them. Rotation/aspect/FOV are
  not fully transferred into `FRenderer::FCamera`.
- `LMeshTrait::BuildRenderObject` sets `DebugName` to `"LMeshTrait"` only when
  the trait name is empty, otherwise `nullptr`. That looks reversed.

### RHI And Vulkan

RHI:

- `IRHIDevice`: buffers, textures, views, samplers, shaders, render passes,
  framebuffers, descriptor layouts/sets, pipeline layouts, graphics/compute
  pipelines, sync primitives, command pools/buffers, swapchain, query pools.
- `IRHICommandBuffer`: command recording API.
- `RHIDefinitions`, `RHIResources`, and `RHIPipelineState` contain most typed
  enums and descriptors.
- `RHIFactory` currently creates Vulkan device/command buffer objects.

Vulkan implementation:

- `FVulkanDevice` creates instance, debug messenger, Win32 surface, physical
  device, logical device, descriptor pool.
- It manages resources via typed resource pools wrapping Vulkan handles.
- Buffers/textures allocate dedicated `VkDeviceMemory` per resource. There is no
  VMA-style allocator layer yet.
- Swapchain creation and per-image views are implemented.
- Graphics and compute pipeline creation are present.

Version mismatch:

- Project rules mention Vulkan SDK/API 1.4.321.1.
- Current Vulkan instance uses `VK_API_VERSION_1_3`.
- `Shaders/Shaders.limx.toml` also targets Vulkan 1.3.
- Do not assume Vulkan 1.4 features until the target is explicitly unified.

### RenderCore

RenderCore:

- `FRenderContext`: creates RHI device/swapchain/frame sync and handles
  `BeginFrame`, `EndFrame`, swapchain recreation, single-time commands.
- `FShaderManager`: loads already-compiled SPIR-V from `Binaries/Shaders`,
  caches it, validates magic/size/alignment, creates RHI shader modules.
- `FCamera`: renderer camera used for WASD/right-mouse movement.
- `FLightManager`: per-frame lighting UBO and descriptor set layout.
- `FMaterialManager`: material UBOs, default white texture/sampler, material
  descriptor set layout.
- `FGeometryGenerator`: procedural cube/sphere/plane mesh data.

Important runtime precondition:

- `FShaderManager` does not compile GLSL at runtime. It expects paths like
  `Binaries/Shaders/Builtin/pbr.vert.spv`.
- Those SPIR-V files are not present in the inspected workspace.
- `.gitignore` ignores `Binaries/` and `*.spv`.
- Running the current executable without compiling shaders first should fail
  during pass shader module creation.

### Renderer

Renderer:

- `FRenderer` initializes camera, procedural scene objects, UBOs, checkerboard
  texture, descriptor set 0, material manager set 1, light manager set 2,
  pipeline layout, and pass manager.
- `FPassManager` owns the shared D32 depth texture/view and orders passes.
- `FDepthPrePass`: depth-only pass, depth compare less/write true.
- `FForwardPass`: color + shared depth pass, depth compare equal/write false,
  uses `Builtin/pbr.vert` and `Builtin/pbr.frag`.

Current shading:

- `pbr.vert/pbr.frag` implement Cook-Torrance lighting against set 2 light UBO.
- Material set 1 exists at engine level, but `pbr.frag` still uses hardcoded
  metallic/roughness/ao and the checkerboard texture from set 0.
- Material texture/parameter integration is incomplete.

### Shaders

Source shaders:

- `Shaders/Builtin/depth_only.vert`
- `Shaders/Builtin/depth_only.frag`
- `Shaders/Builtin/pbr.vert`
- `Shaders/Builtin/pbr.frag`
- `Shaders/Builtin/triangle.vert`
- `Shaders/Builtin/triangle.frag`
- `Shaders/Builtin/ui.vert`
- `Shaders/Builtin/ui.frag`

`Shaders/Shaders.limx.toml` configures source root `Shaders`, output
`Binaries/Shaders`, Vulkan 1.3, and `.vert/.frag/.comp` groups.

### Launch

`LimxLaunch` is currently the only executable module. It is a runtime demo
launcher, not an editor/studio shell.

`LimxEngine.sln` appears stale/incomplete: it only references an
`Intermediate/ProjectFiles/Core/Core.vcxproj`, while the actual source has many
modules. Prefer LBT/CMake generation path once running tools is allowed.

## Rust Tooling

Rust workspace: `Programs/Cargo.toml`

- Members: `lbt`, `lht`, `lsc`.
- No `Programs/Cargo.lock` was present; `.gitignore` ignores `Cargo.lock`.

### LBT

Limx Build Tool:

- Discovers `*.limx.toml` with `WalkDir::max_depth(3)`.
- Parses module config.
- Builds dependency graph from public/private deps.
- Detects missing deps, cycles, layer violations.
- Generates solution/project/CMake/compile commands and build artifacts.
- Contains a checker for no-STL/no-CRT/no-raw-new/delete style rules.

Important:

- LBT has commands for build/generate/clean/check/etc. Do not run them when the
  user asks for read-only inspection.
- The dependency code reverses topological sort to build dependencies first.

### LHT

Limx Header Tool:

- Parses `LCLASS`, `LSTRUCT`, `LENUM`, `LPROPERTY`, `LFUNCTION`, `LDELEGATE`.
- Has reflection, serialization, RPC, editor metadata, script binding, migration,
  hot reload, and runtime header generators.
- Current runtime source mostly relies on `LOBJECT_BODY/IMPLEMENT_LTYPE`, not on
  generated LHT output as a hard runtime dependency.

### LSC

Limx Shader Compiler:

- Uses `shaderc`/glslang.
- Supports GLSL/HLSL stages, include resolution, SPIR-V validation, reflection,
  variants, manifests, PSO cache helpers, include graph and perf analysis.
- Writes outputs preserving source-relative folder structure, e.g.
  `Shaders/Builtin/pbr.vert` to `Binaries/Shaders/Builtin/pbr.vert.spv`.

## Third-Party Packages

Vendored packages include:

- NVIDIA DLSS SDK and DLSS Sample App/Donut.
- Intel Open Image Denoise 2.4.1.
- Donut third-party dependencies such as GLFW, imgui, jsoncpp, lz4, sqlite, stb.

Current integration status:

- DLSS/OIDN are declared as external modules but not connected to first-party
  runtime rendering code.
- Any future DLSS/OIDN work must first design ownership, lifetime, RHI resource
  interop, DLL deployment, and license/runtime distribution handling.

## High-Priority Risks To Address Before Feature Work

> **2026-08-29 状态复核**：下表为本清单在 Day 1–7 之后的实际状态。
> 标注为"未处理"的条目并非不重要，只是不在这七天的作业面上 —— 未经验证
> 就划掉比留着更危险。

| # | 条目 | 状态 |
|---|------|------|
| 1 | `LRegistry::Destroy` 派生析构分发 | **部分处理** — 分发本身经 `DestroySelf()` 正常工作；Day 6 修掉了它引出的悬垂指针问题（`~LTrait` 不摘宿主表、`~LSpatialTrait` 不清子级父指针） |
| 2 | Vulkan 目标版本（文档 1.4 / 代码 1.3） | **已解决**（Day 1）— 统一按本机 SDK 协商，实测 1.4.350 |
| 3 | 启动前可靠的着色器编译步骤 | **已解决**（Day 1）— `lbt build` 内部调用 `lsc`，`verify.ps1` 第 1 步独立校验 |
| 4 | 过时的 `LimxEngine.sln` | **已解决**（Day 7）— `lbt generate-solution` 重新生成，14 个模块齐全 |
| 5 | `#pragma warning(disable: 4273)` 政策 | **未处理** — 仍存在于 `FName.h` 与 `FString.h`，用于抑制 CRT 前向声明的 dll 链接不一致 |
| 6 | `FInputManager` 鼠标按键越界 / 枚举与存储不匹配 | **未处理** — 未核查 |
| 7 | `FThread` 与 `TRefCounted` 中的裸 `new`/`delete` | **未处理** — 仍存在（`FThread.h:166/186/282`、`TRefCounted.h:93`）。若"禁止裸 new/delete"是绝对规则，这两处需要显式豁免或改写 |
| 8 | `pbr.frag` 材质参数绑定不完整 | **已解决** — 材质 UBO 与 5 张贴图槽位全部接通，`FSceneLoader` 按用途分流 sRGB/线性 |
| 9 | 相机 Trait 同步只传位置，不传旋转/FOV/宽高比 | **已解决** — `FSceneManager::ResolveCamera` 传递 `SetAspectRatio` + `BuildViewMatrix` + `BuildProjectionMatrix` |
| 10 | 工具运行需先获得用户许可 | **已解决** — 构建、测试、基准均已获授权并实测通过 |


## Development Approach For Future Work

- Start from source facts, not README promises.
- For any renderer feature, trace all affected layers:
  shader source -> LSC output path -> FShaderManager load path -> RHI descriptors
  -> pass pipeline layout -> renderer/engine data source.
- For any engine object feature, first account for `LRegistry` lifecycle and
  trait ownership.
- For any platform/input feature, keep Win32 message semantics and array bounds
  explicit.
- For any tooling change, keep Rust tool behavior separate from C++ runtime.
- Avoid broad refactors until lifecycle, shader pipeline, and build generation
  are stable.
- When implementing, prefer existing Core types and helpers; do not introduce
  STL or standard library dependencies in engine C++.

## 2026-06-27 Toolchain Handoff Update

Release toolchain status:

- `cargo fetch`, `cargo check --workspace`, `cargo test --workspace`, and
  `cargo build --workspace --release` pass.
- Published binaries:
  - `Binaries/Tools/lbt.exe`
  - `Binaries/Tools/lht.exe`
  - `Binaries/Tools/lsc.exe`
- Distribution archive:
  - `Binaries/LimxToolchain-0.1.0-win64.zip`

Integrated/fixed in this pass:

- LBT `build` now invokes LSC automatically when a project `Shaders` directory
  exists, writing SPIR-V to `Binaries/Shaders`; use `--skip-shaders` to opt out.
- LBT tool lookup now checks sibling release/debug tools, `Programs/target`,
  `target`, and PATH fallback; `generate-reflection` no longer loses its
  fallback LHT path.
- LBT `integration` module LHT/LSC subprocess arguments were updated to the
  current CLI names.
- LSC `compile-all --recursive` now controls discovery depth.
- LSC `compile-all --jobs N` and manifest compiler `parallel_jobs` now use
  bounded rayon thread pools instead of treating every `N > 1` as unlimited.
- LSC `compile-manifest --tag` now filters manifest shader entries by tags.
- LSC `validate --vulkan-version` and `--strict` now affect source validation.
- LSC `watch --debounce`, `reflect --detailed`, and
  `inject-debug --source-map` now have observable behavior.
- LBT source lint discovery skips `ThirdParty`, `Intermediate`, and `Binaries`
  directories; `Binaries/Tools/lbt.exe check --source-dir Source` now checks
  first-party source only and passes on 285 files / 74763 lines.

Remaining tooling risks:

- LBT `check --analyze` still prints a TODO instead of driving MSVC `/analyze`.
- LSC manifest cache fields exist but are not yet wired to real cache reuse in
  manifest compilation.
- LSC disassembly is currently raw-ID only; `--raw-id` is now explicit but does
  not switch between two disassembly backends.

---

## 2026-08-29 Day 1 交接更新 — 地基验证与清债

本次接手按七天路线推进，Day 1 聚焦"先验证地基、再加功能"。以下为对上文
状态的**修订**，上文中被本节覆盖的部分以本节为准。

### 已修复的真实缺陷

按发现顺序列出。除第 1 项外，其余均由新建的单元测试直接捕获。

1. **LBT 编译缓存忽略头文件依赖（严重）**
   - `Programs/lbt/src/main.rs` 两处构造 `CacheKeyInput` 时把
     `dependencies_hash` 硬编码为字符串 `"pending"`，缓存键中的头依赖分量
     恒为常量。后果：修改任何头文件后 LBT 报告"0 个编译, N 个缓存跳过"，
     复用的 `.obj` 基于旧的类布局，与新编译的 TU 混合链接造成 ODR 违规。
     实测表现为运行期 `0xC0000374`（堆损坏），且崩溃点与改动处毫无关联。
   - 修复：在 `compiler/deps.rs` 新增 `IncludeResolver`，递归解析
     `#include "..."`（带跨模块复用的内容哈希缓存），产出真实的
     `dependencies_hash`；同时把 include 搜索路径提到源文件循环之外预计算，
     使缓存解析与实际编译看到同一份路径集；编译前记录的键在回填缓存时复用，
     避免两处重算导致键漂移。
   - 验证：触碰 `FVulkanDevice.h` → 重编译 6 个 TU；触碰 `Core/CoreTypes.h`
     → 重编译全部 48 个 TU；无改动 → 48 个全部命中缓存。
   - 旧缓存（554 MB）全部基于 `"pending"` 键，已清空。

2. **FPoolAllocator::Deallocate(void*) 导致堆损坏（严重）**
   - 无尺寸版本的释放直接把指针转交 `m_Fallback->Deallocate`，而该指针来自
     桶内 chunk，并非独立堆块。通过 `IAllocator` 接口多态使用该分配器的
     任何调用方（例如注入容器）走的正是这条路径，必然触发。
     原代码注释自承是"简化实现"。
   - 修复：为 `TFreeList` 增加 `Owns(ptr)`（chunk 区间 + 块边界双重校验），
     `Deallocate` 改为逐桶询问归属，全部未命中才判定为回退分配器的大块。

3. **三个分配器是抽象类，根本无法实例化**
   - `LinearAllocator`、`BlockAllocator`、`FPoolAllocator` 均继承
     `IAllocator` 却未实现纯虚的 `Reallocate` 与 `GetName`。这说明它们
     从未被实例化过 — 属于完全未验证的死代码。
   - 修复：三者补全接口。语义按各自模型定义并在注释中说明取舍：
     线性分配器仅支持扩展最近一次分配（其余返回 `nullptr`）；
     定长块分配器在新尺寸不超过块容量时原地返回；
     池分配器按归属桶获知旧容量后搬迁。

4. **LinearAllocator / FStackAllocator 违反对齐契约**
   - 两者按"缓冲区内偏移"而非绝对地址做对齐，而底层缓冲区仅按
     `alignof(void*)`（8 字节）申请。结果：请求 32/64/128/256 字节对齐时
     返回的指针实际未对齐，AVX / AVX-512 对齐加载会崩溃。
   - 修复：对齐运算改在绝对地址上进行。

5. **LinearAllocator 对预期失败路径断言**
   - 容量不足时 `LIMX_ASSERT(false)` 后再返回 `nullptr`。容量耗尽是
     `IAllocator` 契约中的正常失败，不是程序错误，Debug 构建下会误中断。
   - 修复：移除断言，保留 `nullptr` 返回。

6. **Vulkan 设备特性未经查询即启用**
   - `CreateLogicalDevice` 无条件启用 `bufferDeviceAddress`、
     `dynamicRendering` 等特性，在不支持的 GPU 上 `vkCreateDevice` 直接失败。
   - 修复：`PickPhysicalDevice` 用 `vkGetPhysicalDeviceFeatures2` 查询
     1.1/1.2/1.3/1.4 特性链，设备创建时只启用已确认支持的项；
     并对引擎强依赖的 `dynamicRendering`/`synchronization2` 做显式前置校验。

### Vulkan 版本统一（1.3 → 1.4）

本机 SDK 为 1.4.350，目标统一到 Vulkan 1.4：

- `VulkanCommon.h` 新增 `kLimxTargetApiVersion`(1.4) 与
  `kLimxMinimumApiVersion`(1.3)。
- `CreateInstance` 通过 `vkEnumerateInstanceVersion` 与设备 `apiVersion`
  三方取最小值协商，可在仅支持 1.3 的驱动上自动降级。
- `IRHIDevice.h` 新增 `ERHIResult::ErrorIncompatibleDriver`。
- LSC 新增 `TargetEnvironment::Vulkan1_4`（shaderc 0.8.3 的 `EnvVersion`
  未收录 1.4，按 Vulkan 版本编码规则直接构造）并显式固定 SPIR-V 1.6；
  CLI 默认值改为 1.4。`Shaders/Shaders.limx.toml` 与 `项目规则.md` 同步。
- 实测：RTX 3060 上协商为 API 1.4，设备 API 1.4.325，`maintenance5` 可用，
  8 个着色器全部产出 SPIR-V 1.6，引擎运行正常。

### 新增模块

- **`Source/Runtime/Testing`（LimxTesting，layer 2）** — 零 STL 测试框架。
  用例经侵入式静态链表在 `main` 之前自动注册（规避静态初始化顺序问题）；
  `FTrackingAllocator` 以内联记账头统计分配/释放并提供泄漏基线；
  运行器直写 stdout（Core 的 `ConsoleLogSink` 只走 `OutputDebugStringA`，
  对 CI 不可见），退出码即结论。
- **`Source/Tests/CoreTests`（LimxCoreTests，可执行）** — 250 个用例 /
  7065 项检查，覆盖 TArray、FString（含 SSO 边界）、TMap/TSet（含恒定哈希
  制造的最坏探测链）、智能指针、TOptional、四种分配器、向量/四元数/矩阵。
  数学用例以恒等式而非硬编码期望值断言。

### UI 模块处置

`Source/Runtime/UI{Core,Renderer,Style,Widgets}` 的源码已于 2026-04-08 移除，
目录予以保留。各目录下新增 `*.limx.toml.disabled`（LBT 按
`ends_with(".limx.toml")` 发现模块，故该后缀完全不参与构建），内含完整配置
与启用前的复核清单，供后续 Studio 编辑器阶段重建 UI 层时复用。
过期产物已清理：`LimxUI*.lib`、`Intermediate/Generated/UI*API.generated.h`、
`Intermediate/Development/Win64/LimxUI*`（约 7 MB）。

### 其他

- `LimxEngine.sln` 已用 `lbt generate-solution` 重新生成，现含全部 10 个
  模块项目（此前仅引用一个并不存在的 `Core.vcxproj`）。
- 新增 `.github/workflows/ci.yml`（三作业：工具链 / 引擎 / 着色器）。
  **注意：本仓库当前没有配置 git remote，该工作流未在真实 GitHub Actions
  环境验证过**，首次推送后需复核 Vulkan SDK 安装步骤与运行器镜像的兼容性。
- 新增 `Scripts/verify.ps1` — CI 的本地等价物，是目前唯一经过实测的验证
  入口，5 个步骤全部通过。

### 上文中已失效的表述

- "无构建产物""不是 Git 仓库" — 现已是 Git 仓库，且 `Binaries/` 下有
  完整的工具链、SPIR-V 与 `LimxLaunch.exe`。
- "Vulkan 版本不一致（文档 1.4 / 代码 1.3）" — 已统一到 1.4。
- "`LRegistry::Destroy` 显式基类析构""`FInputManager` 鼠标按钮越界"
  "`FSceneManager` 只传相机位置""`pbr.frag` 材质未接入" — 均已在本次接手
  之前被修复，复核确认。
- "`LimxEngine.sln` 过期" — 已重新生成。

---

## 2026-08-29 Day 2 交接更新 — Vulkan 显存分配器

### 问题

改造前每个缓冲区与纹理各自调用一次 `vkAllocateMemory`，并各自
`vkMapMemory`。这带来三个后果：

1. **撞 `maxMemoryAllocationCount`** — 该上限在 AMD/Intel/移动 GPU 上通常为
   4096。一个真实场景的网格与贴图轻易过万，加载会在中途直接失败。
   （本机 RTX 3060 的 NVIDIA 驱动上报 `UINT32_MAX`，不受此限，因此这台机器
   无法复现撞墙；但换到其他驱动上该限制是真实的。）
2. **驱动侧开销** — 每次 `vkAllocateMemory` 都是重量级调用。
3. **映射数量** — 同一 `VkDeviceMemory` 同时只允许一个活跃映射，
   逐资源映射在共享内存后必然违规。

### 新增: FSuballocationRegistry（`Source/Runtime/RHI/Public/RHI/Memory/`）

块内区间分配算法，**刻意不含任何 Vulkan 类型**，因此可在无 GPU 的环境下
完整单测。这是本次设计上最重要的决定：分配器的缺陷几乎全部集中在区间管理
（分裂、合并、对齐、粒度）而非 API 调用上。

- 分级自由列表 + 位图跳空桶，分配近似 O(1)
- 节点携带物理前驱/后继索引，释放时 O(1) 边界合并
- 支持 `bufferImageGranularity`：线性资源（缓冲区、LINEAR 图像）与非线性
  资源（OPTIMAL 图像）不共享同一粒度页 —— 违反此规则是未定义行为
- `Validate()` 校验六项内部不变式（物理链完整、总和守恒、无相邻空闲、
  自由链表与分桶一致、位图一致、用量吻合），供测试逐步断言
- 节点槽复用，避免长时间运行下节点数组无界增长

### 新增: FVulkanMemoryAllocator（`Private/Vulkan/`）

- 按内存类型分池，块尺寸取堆容量的 1/8 并收敛到 [32 MiB, 256 MiB]
- 主机可见的块在创建时**整块映射一次**并长期持有，子分配的地址由基址加
  偏移得出（规范禁止对同一 `VkDeviceMemory` 建立第二个活跃映射）
- 超过 16 MiB 的请求走专用 `vkAllocateMemory`，避免在共享块里留下
  难以复用的尾部空洞
- 块内最后一个子分配释放后立即销毁该块，显存及时归还驱动
- 失败路径逐级退让：现有块 → 新建标准块 → 按需求新建刚好够大的块 → 专用分配
- `Flush`/`Invalidate` 按 `nonCoherentAtomSize` 向外扩展范围并夹紧到块边界
- 逼近 `maxMemoryAllocationCount` 的 80% 时主动告警

### 接入改动

- `FVulkanBufferData` / `FVulkanTextureData` 的 `VkDeviceMemory Memory`
  改为 `FVulkanAllocation Allocation`
- `CreateBuffer` / `CreateTexture` 改走分配器，绑定时传入子分配偏移
- `CreateTexture` 按 `imageInfo.tiling` 判定线性/非线性，交给粒度约束
- `MapBuffer` 直接返回块映射基址加偏移；`UnmapBuffer` 只清除本资源的引用，
  **不做 `vkUnmapMemory`** —— 解除映射会让同块内其他缓冲区的指针一并失效
- 交换链图像的 `Allocation` 保持无效句柄，其内存归呈现引擎所有
- 分配器在逻辑设备创建后初始化，在 `vkDestroyDevice` **之前**关闭

### 顺带修复的回归

Day 1 的特性裁剪只复制了原代码明确列出的特性，漏掉了
`shaderDemoteToHelperInvocation`。面向 SPIR-V 1.6 编译时 glslang 会为
`discard` 生成 `DemoteToHelperInvocation` 能力，导致 `vkCreateShaderModule`
被验证层拒绝。现已补充启用该特性及 `shaderTerminateInvocation`、
`shaderZeroInitializeWorkgroupMemory`（均为条件启用）。

### 验证结果

- **RHITests 新模块**：27 个用例 / 115,861 项检查。含 7000 次确定性随机
  操作序列，每步校验不变式、区间不重叠、粒度页不共享。随机种子固定，
  任何失败可精确重放。
- **实测承载力**：256 MiB 单块容纳 **4755 个典型资源**（占用 254 MiB，
  利用率 99.2%）。旧路径下这需要 4755 次 `vkAllocateMemory`，已超出
  4096 的典型上限。
- **无退化**：12 轮加载/卸载循环后承载量不低于首轮的 90%（实测无下降），
  证明合并逻辑无残留碎片。
- **引擎实跑**：全部 GPU 资源仅用 **2 次 `vkAllocateMemory`**（改造前为
  每资源一次）；正常退出时块数、子分配数、专用分配数、设备分配数全部归零，
  **零显存泄漏**；Vulkan 验证层无错误。

### 已知限制

- 分配器**非线程安全**。当前渲染路径单线程提交，引入多线程命令录制时
  必须在此加锁。
- 未实现碎片整理（defragmentation）。长时间运行且资源尺寸分布剧烈变化的
  场景可能积累碎片，需要时再补。
- `FVulkanDevice` 析构时不清理资源池中残留的缓冲区/纹理句柄 —— 这是既有
  行为，本次未改动。分配器会在关闭时报告未归还的分配数，使泄漏可见。

---

## 2026-08-29 Day 3 交接更新 — 资产解析管线

### 新增: Core/Misc/FJson —— 零 STL JSON 解析器

glTF 是 JSON 格式，引擎此前没有 JSON 能力，故先补上。

- **节点池而非递归类型**：直觉写法是让 `FJsonValue` 内含
  `TArray<FJsonValue>`，但那要求容器支持不完整类型（标准未保证），
  且每层嵌套都产生独立堆分配。改用扁平节点数组 + `UInt32` 索引互指。
- **子索引池**：实现中发现一个真问题 —— 嵌套容器会把自身子节点追加到节点池
  尾部，父容器的子节点在池中**并不相邻**（`[1,[2],3]` 的三个元素分别是
  节点 1、2、4）。因此另设子索引池，容器解析期先把子索引压入共享 scratch 栈，
  结束时整段搬入。共享栈让任意嵌套深度都只用一块缓冲区。
- 字符串统一解码入连续池，`\uXXXX` 转义支持 UTF-16 代理对并输出 UTF-8。
- 数字按"尾数 + 十进制指数"解析并一次性缩放，避免逐位累积舍入误差；
  保留至多 19 位有效数字。非正确舍入，极端精度下可能有 1 ULP 级误差。
- 严格遵循 RFC 8259：拒绝注释、尾随逗号、前导零、字符串内裸控制字符、
  根值后的多余内容。错误携带行号与列号。
- 深度上限 256，拒绝恶意构造的深层嵌套。

### 新增模块: LimxAssetPipeline（layer 2，仅依赖 Core）

刻意**不依赖 RHI** —— 资产解析是纯 CPU 工作。这让它可以完整单元测试，
且 GPU 上传（Day 4-5）与文件格式彻底解耦。

**中性数据结构**（`FAssetTypes.h`）：所有解析器产出同一套
`FMeshData` / `FMaterialData` / `FSceneNode` / `FAssetScene`。渲染层只需
认识这一套，新增格式不必改渲染层。材质统一为金属粗糙度工作流。
场景层级用数组下标而非指针互指，整个场景是一块可整体拷贝的连续数据。

顶点为 72 字节交错胖顶点（位置/法线/切线/UV0/UV1/颜色），尺寸由
`static_assert` 钉住 —— 它直接决定顶点缓冲区步长，无意间加成员会让
GPU 侧属性偏移全部错位。

提供 `GenerateNormals`（面积加权，叉积模长天然正比于三角形面积）与
`GenerateTangents`（Lengyel 方法 + Gram-Schmidt 正交化 + 手性判定），
在解析结束时补齐缺失属性，使上传层拿到的永远是属性完整的顶点。

### 新增: FObjLoader —— Wavefront OBJ / MTL

- **顶点去重键是索引三元组**：OBJ 按 (位置/UV/法线) 三个独立索引描述顶点，
  同一位置在不同面上可配不同法线。用三个源索引组哈希键做折叠，
  既精确又不必比较浮点值。实测：立方体产出 24 个唯一顶点（8 位置 × 3 法线），
  只按位置去重会得到 8 个（法线错乱），不去重会得到 36 个。
- 支持 1 起始正索引与相对末尾的负索引；扇形三角化任意多边形。
- 按 `usemtl` 切分子网格；每个子网格的包围盒只覆盖自己引用的顶点。
- 默认翻转 UV 的 V 轴（OBJ 原点在左下，Vulkan 图像原点在左上）。
- **Phong → PBR 换算**：`roughness = sqrt(2/(Ns+2))`，这是 Blinn-Phong
  指数与 GGX 粗糙度的通行对应。**金属度默认取 0** —— OBJ 没有金属度概念，
  从 Ks 反推极不可靠，误判为金属会让材质整体发黑。MTL 若带 PBR 扩展
  （`Pr`/`Pm`）则一律优先采用，不做近似。
- 贴图行跳过 `-o`/`-s`/`-bm` 等选项参数；路径分隔符统一为正斜杠。
- 畸形行跳过而非中止，但每次跳过都记入 `Warnings` 使问题可见。

### 新增: FGltfLoader —— glTF 2.0 / GLB

- **访问器解引用集中一处**：glTF 每段数据都要穿过
  accessor → bufferView → buffer 三级间接，叠加两层 byteOffset、
  可选 byteStride、五种 componentType 与 normalized。全部收敛到
  `FAccessorReader`，各属性读取退化为"取第 i 个元素"。
- 支持紧密与交错（byteStride）两种布局；索引支持
  UNSIGNED_BYTE / UNSIGNED_SHORT / UNSIGNED_INT 三种宽度。
- **越界一律拒绝而非夹紧** —— 夹紧会让损坏的 glTF 静默产出扭曲几何，
  症状是模型局部变形，极难溯源。
- 缓冲区支持外部 `.bin`、`data:` URI（base64）与 GLB 二进制块三种来源。
- GLB 容器校验魔数、版本、块长度与 4 字节对齐；按**魔数而非扩展名**
  区分 `.gltf` 与 `.glb`（扩展名可能被改过，魔数不会）。
- 节点变换的 `matrix`（列主序）与 TRS 两种表达统一分解为 `FTransform`。
- 遇到 `KHR_draco_mesh_compression` / `EXT_meshopt_compression` 判定为
  **失败而非告警** —— 忽略压缩扩展只会产出空的或错误的几何。
- 非三角形图元跳过并告警。

### 顺带修复: CoreAPI.h 的宏优先级缺陷

`CoreAPI.h` 中 `LIMX_xxx_EXPORTS` 的判断排在 `LIMX_xxx_STATIC` 之前，
而 LBT 对静态库会**同时定义**这两个宏（`_STATIC` 由构建工具注入，
`_EXPORTS` 来自模块 toml）。结果：所有静态库都被按 `__declspec(dllexport)`
编译。后果是含模板成员的导出类触发 C4251，而 `/WX` 把它变成错误 ——
这解释了为什么此前 Core 里没有"带 `LIMX_CORE_API` 且含 `TArray` 成员"
的类：一旦有就编译不过。

各模块自己的 API 头（`ObjectAPI.h`、`TestingAPI.h`）顺序是正确的，
只有这个聚合文件反了。现已把 `_STATIC` 判断提前（Core / RHI / Renderer 三处）。

### 验证结果

- **CoreTests**：287 用例（JSON 新增 37 个，含交错嵌套、代理对、
  数字精度、错误定位、非法输入拒绝）
- **AssetTests 新模块**：68 用例（OBJ/MTL 39 个 + glTF/GLB 29 个）。
  全部夹具内嵌为字符串字面量或代码构造的 GLB 容器，不依赖外部资产文件。
- **RHITests**：27 用例
- `Scripts/verify.ps1` 全部 7 步通过。

### 已知限制

- glTF 不支持动画、蒙皮、变形目标 —— 遇到时忽略。
- glTF 不支持 Draco / meshopt 压缩 —— 明确判定为失败。
- 图像**只记录引用，不做解码** —— PNG/JPEG 解码是 Day 4 的内容。
  `FEmbeddedImage` 目前只保存原始压缩字节。
- OBJ 忽略平滑组（`s`），法线生成一律按整网格平滑。
- 仓库内的 Sponza 资产是 **Git LFS 指针**（`.obj` 仅 133 字节，
  实际 21.6 MB），需 `git lfs pull` 才能用真实资产做端到端验证。

---

## 2026-08-29 Day 4 交接更新 — 图像解码与资源注册表

按用户决定，图像解码全部自研，不引入 stb 等外部库。

### 新增: Core/Misc/FInflate —— DEFLATE 解压

PNG 的像素数据是 zlib 容器封装的 DEFLATE 流，而 Core 已有的 `FCompression`
是 LZ4 风格的自研格式，与 DEFLATE 毫无共同点，因此单独实现。

- 支持三种块类型：存储 / 固定 Huffman / 动态 Huffman
- Huffman 采用 zlib 参考实现 puff 的逐位规范解码 —— 比查表慢，
  但短小到可以逐行核对。解压错误产生的是花屏而非崩溃，极难靠现象定位，
  正确性因此优先于吞吐（若纹理加载成为瓶颈，可在此引入快速表）
- **反向引用必须逐字节复制**：DEFLATE 用"距离 1、长度 N"表达游程，
  源与目标重叠，`memcpy` 会产出错误结果
- zlib 容器校验 CMF/FLG 与 Adler-32，拒绝预置字典

### 顺带修复: TArray::Add 的自引用悬垂（严重）

实现 inflate 时长数据全部解压失败（长度正确但 Adler-32 不符），
根因不在 inflate：

`TArray::Add(const T&)` 的实现是「先 `EnsureCapacity` 再拷贝构造」。
若实参指向数组自身的元素（`values.Add(values[i])`，正是 LZ77 反向引用的
写法），扩容会释放旧缓冲区，**使传入的引用悬垂**，随后的拷贝读到的是
已释放内存。容量充足时完全不显现，只有恰好触发扩容的那一次才出错 ——
表现为偶发的数据损坏。

修复：需要扩容时先把值搬到临时对象再移入新位置（`Add(T&&)` 同样处理）。
`TArrayTests` 新增四个自引用回归用例钉住该行为。

### 顺带修复: StringFormat 缺十六进制支持

`StringFormat` 只识别 `{}`，不解析 `{:X}`。项目里有 4 处误用（其中
`FVulkanDeviceInit.cpp` 那处是既有代码），效果是日志里直接打印出
字面的 `0x{:X}`。图形编程中格式枚举、内存掩码、资源句柄几乎都以十六进制
阅读，因此新增 `FHex` 包装类型（支持前导补零），并修正全部误用。

### 新增: FPngDecoder

全格式覆盖而非只做常见格式 —— 真实资产库里带调色板的 UI 图标、
16 位高度图、隔行的网页遗留资源都存在，缺一种就有贴图加载不出来，
而缺贴图在渲染结果里表现为一片纯色，排查成本远高于一次做全。

- 五种颜色类型、1/2/4/8/16 全部位深、PLTE 调色板、tRNS 透明度
- 四种滤波器（None/Sub/Up/Average/Paeth）**按字节而非按像素**还原 ——
  滤波作用于"当前字节"与"左侧一个像素处的同位置字节"，以 bpp 为步长
  逐字节处理，五种滤波器共用一份实现
- Adam7 隔行：七遍各自独立反滤波后按采样格点回填。若当成一整幅图
  反滤波会得到纯噪声
- **分块 CRC 只告警不拒绝**：PNG 的分块结构使单块损坏未必影响可解码性，
  拒绝加载会让一处磁盘坏道毁掉整个场景；真正的像素损坏由 IDAT 的
  zlib 校验和拦下

### 新增: FJpegDecoder

只做基线（SOF0/SOF1），但把基线做全。渐进式与算术编码明确报错而非
产出花屏。

- 熵解码器内部消化 `0xFF00` 字节填充与 `RSTn` 重启标记，
  使上层 MCU 循环保持线性
- **DC 是差分而非绝对值**，重启标记处必须归零 —— 漏掉会让重启点之后的
  整幅图像出现亮度阶跃，且小图完全正常，只有够大的图才触发
- 支持任意整数采样因子组合（4:4:4 / 4:2:2 / 4:2:0 / 4:4:0）
- 色度上采样用**最近邻而非双线性**：双线性会让相邻块的色度互相渗透，
  在法线贴图这类非颜色数据上造成实际误差
- IDCT 为可分离的行列两趟浮点变换

### 新增: FImageDecoder（统一入口）

**按魔数而非扩展名分发** —— 资产库里被批量改名的 `.png` 其实是 JPEG
这类情况很常见，魔数来自文件内容本身，不会说谎。未知格式时报出前几个
字节，常能一眼认出是文本、LFS 指针还是别的容器。

### 新增: FAssetRegistry

Day 5 资源所有权重构的基础。

- **路径即身份**：同一张贴图常被十几个材质引用，以规范化路径（分隔符统一、
  转小写）为键去重，使解码次数等于文件数而非引用数
- **解码选项参与缓存键**：同一 PNG 以"保留 16 位"与"降为 8 位"加载会得到
  内容不同的两份数据，只以路径为键会让后一次拿到前一次的结果
- **代际句柄**：槽位卸载后复用，裸指针在那一刻变成悬垂。句柄带代际号后，
  误用得到的是干脆的 `nullptr` 而非错误的数据
- **失败也缓存**：损坏的贴图可能被上百个材质引用，不缓存会导致上百次
  无谓的磁盘访问
- **卸载是显式的**：引用归零不立即释放。材质切换与 LOD 过渡会让资源在
  一帧内被放下又拾起，立即释放会造成反复解码抖动

### 验证结果

| 测试集 | 用例 | 检查 |
|--------|------|------|
| CoreTests | 307 | 11,532 |
| RHITests | 27 | 115,861 |
| AssetTests | 127 | 1,120 |

Day 4 新增：Inflate 15 个、PNG 25 个、JPEG 15 个、AssetRegistry 19 个。
全部夹具内嵌 —— zlib 向量由标准库生成，PNG 与 JPEG 由脚本按规范手工构造
（量化表全 1 以消除量化误差，像素值可由坐标公式推导）。
`Scripts/verify.ps1` 全部 7 步通过。

### 已知限制

- JPEG 不支持渐进式（SOF2）、算术编码、无损、12 位精度 —— 明确报错
- JPEG 不解析 EXIF 方向标记，图像按存储顺序输出
- PNG 不支持 APNG 动画扩展，只解首帧
- **DDS / KTX2 未实现** —— 它们是 GPU 压缩容器，无需 CPU 解码，
  应走独立的直传路径而非 `FImageDecoder`。Day 5-6 若需要 BC 压缩纹理
  再补
- `FAssetRegistry` 非线程安全，不监听文件变更（热重载需外部触发）
- Inflate 的逐位解码未做查表优化

---

## 2026-08-29 Day 5 交接更新 — GPU 资源所有权归位

### 问题

所有权方向是反的：`FRenderer` 持有 GPU 缓冲区，场景节点只引用裸句柄。
渲染器是消费者而非所有者，这个方向让"加载任意场景"无从谈起 —— 每加一种
资产，渲染器就要为它的生命周期负一次责。

### 改动

| | 之前 | 之后 |
|---|---|---|
| GPU 缓冲区所有者 | `FRenderer` | `FRenderResourceManager`（挂在 `FRenderContext` 上） |
| `LMeshTrait` 持有 | 裸 `FRHIBufferHandle` | `FMeshResourceHandle` + 引用计数 |
| 渲染粒度 | 一个物体一次绘制 | 一个**子网格**一次绘制 |
| 顶点格式 | 44 字节 / 4 属性 | 72 字节 / 6 属性（含切线、次 UV） |

资源管理器挂在 `FRenderContext` 而非 `FRenderer` 上，因为资源的消费者不止
渲染器：场景、材质系统都要引用同一批资源。

`FRenderer` 删去 `CreateScene` / `DestroyScene` / `CreateRenderObjectBuffers`
共 164 行，现在只读本帧视图。

### 延迟销毁

引用归零的资源可能仍被尚未执行完的命令缓冲区引用。第一次跑
`CollectUnreferenced` 时校验层报了 6 条
`VUID-vkDestroyBuffer-buffer-00922`。

资源改为先"退役"进待销毁队列，记录退役时的单调帧序号，等
`MaxFramesInFlight` 帧之后（那一帧的栅栏已经通过）才真正销毁。槽位可以
立即复用，因为 GPU 对象已从槽位中移出。

`FRenderContext` 为此增加了单调帧计数器 —— `GetCurrentFrameIndex` 在
`[0, MaxFramesInFlight)` 内循环，无法用来判断"是否已过去足够多帧"。

### 三个吃掉整个画面的历史缺陷

重构完成后编译通过、日志干净、批次数正确、句柄全部有效，但屏幕上只有
清屏色。逐层探针切下去找到三个互相独立的原因，全部早于本次接手：

1. **着色器矩阵存储序**。`FMatrix` 是行主序（头文件注释写着"与着色器
   row_major 一致"），但四个着色器的 uniform 块都没写 `row_major`，
   GLSL 按默认列主序解读，等于整体转置。
2. **投影矩阵手性与 LookAt 不匹配**。`LookAt` 是右手系（视线 -Z），
   `Perspective`/`Ortho` 却按左手系（+Z）构造。两者单独看都对，组合起来
   让每个可见点的裁剪空间 w 为负，全部被裁掉。
3. **空间 Trait 不挂到节点根变换之下**。`LMeshTrait` 用自己的单位变换，
   节点摆到哪儿都没用。修完这个又暴露出 `~LSpatialTrait` 只清
   `m_Children` 不清子级的 `m_Parent`，关闭时访问违规（`0xC0000005`）。

共同点：三者都不产生任何报错，校验层也不会提示。**这类"静默正确性"
缺陷只能靠断言完整链路的测试拦下，而不是断言单个函数的返回值。**

### 防回归

`MathTests.cpp` 新增 9 个用例，断言的是 **view → proj 整条链路**：相机
前方的点 w 必须为正、近/远平面映射到 0/1、深度随距离单调、Vulkan Y 朝下、
单位立方体八个角全在裁剪体内。原有用例只测了 `LookAt`，因此手性不匹配
一直看不见。

`verify.ps1` 增加着色器 lint：含矩阵成员的 uniform/push_constant 块必须带
`row_major`。**第一版因 PowerShell 的 `ForEach-Object` 子作用域陷阱恒为
"通过"** —— 子作用域里的 `+=` 会先读外层变量再在本地新建同名变量。改用
`foreach` 语句，并构造违规验证它确实会红。

---

## 2026-08-29 Day 6 交接更新 — 剔除、合批与任意场景导入

### 视锥剔除与状态排序

剔除与排序放在 `FSceneManager`（生产端）而非渲染器（消费端）：渲染器拿到的
应当是一份可以照单全收的批次列表。把可见性判断留给渲染器，等于要求每个
Pass 各实现一遍，而 `FDepthPrePass` 与 `FForwardPass` 一旦剔除结果不一致，
`DepthCompareOp=Equal` 的 Early-Z 就会失效。

排序键以材质为主、网格为次 —— 材质描述符集的切换代价高于顶点缓冲区绑定。
两个 Pass 都跳过冗余绑定。

`FFrustum` 近平面此前用 OpenGL 的 `row3 + row2`；Vulkan 深度范围是 `[0,1]`，
近平面应为 `row2` 本身。旧公式把近平面推到设定距离的一半处，剔除率白丢。
**第一版测试通不过检验 —— 两种公式都过**，于是把断言卡到两者的分歧区间上，
再故意退回旧公式验证它确实会红。

### 基准数据

`Scripts/benchmark.ps1`，同一 exe、同一场景（60×60 = 3600 物体 / 48.5 万
三角形 / 8 材质），只切开关：

| 配置 | 可见批次 | 材质切换 | 平均帧耗时 | 加速比 |
|---|---:|---:|---:|---:|
| 基线（都关） | 3600 | 3600 | 32.46 ms | — |
| 仅剔除 | 1880 | 1876 | 17.39 ms | 1.87× |
| 仅排序 | 3600 | 8 | 13.15 ms | 2.47× |
| 剔除 + 排序 | 1880 | 8 | 7.10 ms | **4.57×** |

排序后材质切换 8 次 = 材质种类数，网格切换 24 次 = 8 材质 × 3 网格，
均为理论下界。

**排序的收益比剔除还大**，因为冗余描述符集绑定的成本高于被剔掉的三角形。

### Sponza

Khronos glTF-Sample-Assets 版本，19.2 万顶点 / 26.2 万三角形 / 103 批次 /
25 材质 / 69 张贴图（全部由自研 PNG + 基线 JPEG 解码器解出，缺失 0）。

| 配置 | 可见批次 | 三角形 | 平均帧耗时 |
|---|---:|---:|---:|
| 都关 | 103 | 262,267 | 0.852 ms |
| 仅排序 | 103 | 262,267 | 0.787 ms |
| 仅剔除 | 15 | 22,384 | 0.423 ms |
| 剔除 + 排序 | 15 | 22,384 | 0.430 ms |

剔除掉 85.4% 的批次。**排序在这里没有收益，甚至略微为负** —— 只剩 15 个
批次时，排序本身的开销已和它省下的绑定次数相当。合批的价值随批次数增长，
103 个批次还不到门槛。这个反差比单看一组数字有用得多。

### 场景导入

新增 `FSceneLoader`（Engine 层），打通 文件 → 解析 → 纹理解码去重 →
材质创建 → 网格上传 → 节点生成。

- 基色与自发光按 sRGB 上传，法线/金属粗糙度/遮蔽按线性 —— 依据是贴图在
  材质中的用途，而非文件声明
- 同路径贴图只上传一次（真实场景里几十个材质共享同一批贴图）
- 缺失贴图记数并回退到常量因子，不中断导入 —— 贴图缺失在真实资产里是常态，
  为此让整场导入失败，等于把显示问题升级成加载问题

顺带更正一处契约：`FTextureReference::Path` 的注释写作"相对路径"，但 OBJ
与 glTF 两个解析器写入的都是已拼好的完整路径。按解析器的实际行为更正说明，
并去掉导入器里多余的二次拼接（症状是 `dir/dir/tex.png`，贴图静默缺失）。

### 新增 EngineTests

场景图与 Trait 层级是纯 CPU 逻辑，却正是"物体渲染到错误位置"和"关闭时
崩溃"两类问题的源头。12 个用例覆盖节点变换传递、层级合成、旋转下的偏移、
销毁顺序安全性。**第 9 个用例当场崩出了 `~LTrait` 不从宿主节点 Trait 表中
摘掉自己的缺陷** —— `LRegistry::Destroy` 留下悬垂指针，场景销毁时二次释放。

---

## 2026-08-29 Day 7 交接更新 — 稳定性验证与文档对齐

### 稳定性

| 项目 | 方法 | 结果 |
|---|---|---|
| 交换链重建 | 8 次尺寸变更（200×150 ↔ 1920×1080）+ 最小化/还原 | 重建 10 次，退出码 0，零告警 |
| 长跑泄漏 | Sponza 场景运行 30 秒（约 7 万帧），逐 3 秒采样 | 工作集 345.3 → 347.7 MB 后**完全持平**，句柄数稳定 |
| 显存泄漏 | 每次退出检查分配器统计 | `块:0 子分配:0 专用:0 设备分配数 0` |
| 校验层 | 演示 / 压力 / OBJ / Sponza 四类场景 | **全部零错误零告警** |
| 错误路径 | 不存在的路径、非法 glTF、不支持的扩展名 | 记录错误 → 回退内置场景 → 退出码 0，无崩溃 |

### 仓库整理

远端旧仓库被删除后，`Source/ThirdParty/DLSS_Sample_App` 下的 67 个 Git LFS
指针成了**指向不存在对象的死指针**（本地对象缓存为空），LFS 预推送钩子
直接拒绝推送。

整个 `Source/ThirdParty` 从历史中剔除（`git filter-branch --index-filter`），
磁盘文件保留，改由 `.gitignore` 排除。跟踪文件 2227 → 501。

**已实测确认目录缺失不影响构建**：把 `Source/ThirdParty` 整个移开后，
`lbt` 发现 14 个模块（而非 16），构建成功、`verify.ps1` 九步全过、
Sponza 正常渲染。NvidiaDLSS 与 IntelOIDN 是 `External` 类型模块，
当前没有任何引擎模块依赖它们。（README 初稿曾把它们写成构建前置，
是未经验证的推断，已更正。）

**注意**：`filter-branch` 结束时会把工作区重置到新 HEAD，磁盘上的
`Source/ThirdParty/` 会被一并删除。已从 `refs/original` 恢复全部 1726 个
文件（用 `git archive` 时需以 `-c filter.lfs.process=` 绕过 LFS smudge
过滤器，否则它会去抓已不存在的对象而中断）。

**Sponza 的原始 LFS 资产已永久丢失** —— 对象只存在于被删除的远端，本地
缓存为空。现使用 Khronos glTF-Sample-Assets 版本，置于 `Content/Sponza/`
（不入库）。

### README 更正

README 的"快速开始"一节与仓库实际状态完全不符，在仓库公开后属于误导：

| 项 | README 原文 | 实际 |
|---|---|---|
| 克隆地址 | `aspect-ux/Limx.git` | `LimxTeam/LimxEngine.git` |
| 构建方式 | CMake | `lbt`（无 CMakeLists.txt） |
| 可执行文件 | `LimxDemo.exe` | `LimxLaunch.exe` |
| 目录结构 | `Source/{Platform,Luminance,Neural,Synergy,Studio}` | 均不存在 |

已改为与实际一致，并补上第三方 SDK 的前置说明与运行参数表。README 后续的
设计哲学与架构章节属于项目愿景，未作改动。

### 遗留事项

- **`FMaterial` 未参与纹理引用计数**。材质持有裸 `view`/`sampler` 句柄，
  与资源管理器无引用关系，因此导入的纹理运行时无法单独回收，只能随资源
  管理器整体销毁。Sponza 的 278 MB 纹理正是受此影响最大的场景。要支持
  关卡切换时回收显存，需让 `FMaterial` 也参与引用计数。
- **资产导入是同步的**。Sponza 导入耗时 6.6 秒，绝大部分花在 69 张 JPEG
  的解码上，未并行化。加载时间成为问题时，多线程解码是最直接的一刀。
- **`.github/workflows/ci.yml` 从未在真实环境跑通**。`Scripts/verify.ps1`
  是当前唯一经过实测的验证入口（9 步）。
- 本地 `.git` 仍保留 `refs/original` 与 `backup-before-filter`（约 209 MB），
  是回滚历史重写的唯一退路。确认无误后可 `git gc --prune=now` 清理。

### 测试总量

| 测试集 | 用例 | 检查 |
|--------|-----:|-----:|
| CoreTests | 325 | 15,335 |
| RHITests | 27 | 115,861 |
| AssetTests | 127 | 1,120 |
| EngineTests | 12 | 61 |
| **合计** | **491** | **132,377** |

---

# 第二周 (2026-08-29) — 视觉正确性补齐

主题不是"让画面更好看", 而是**让渲染器产出正确的图像**。第一周结束时
Sponza 能加载能渲染, 但四处空缺直接体现在画面上: 没有 mipmap (远处闪烁)、
没有混合状态 (半透明渲染成不透明)、没有阴影 (画面是平的)、色调映射写死在
PBR 着色器里 (无曝光、接不了后处理)。

## Day 1 — Mipmap 与采样质量

所有纹理都是单层 mip。后果不是"不够好看"而是画面错误: 缩小区域严重走样,
棋盘地面呈现整片摩尔纹干涉条纹。

同机位 A/B 度量相邻像素亮度差的平方均值 (走样的直接代理量):

| 区域 | 无 mip | 有 mip | 变化 |
|------|-------:|-------:|-----:|
| 地面远端 | 361.0 | 79.0 | −78% |
| 地面中段 | 781.5 | 104.9 | −87% |
| 地面近端 | 491.9 | 243.0 | −51% |

实现要点:
- 上传后用 `vkCmdBlitImage` 逐级降采样。**每级布局单独转换**而非整体转换,
  因为同一时刻不同 mip 处于不同布局; 第 i−1 级转 TransferSrc 的那次转换
  同时充当写后读屏障。
- mip 层数**由格式能力决定**: 逐级 blit 要求格式同时支持 BlitSrc、BlitDst
  与线性过滤, 这是按格式按设备变化的, 不是普遍保证。为此在 RHI 增加
  `GetFormatFeatures`, 不满足时退回单层并留日志。静默降级会让"某些机器上
  远处纹理闪烁"变成无从追查的问题。
- 纹理视图的 `MipLevelCount` 必须覆盖全部层级 —— 只暴露第 0 级等于生成了
  mip 链却永远采不到, 画面表现与完全没有 mip 一模一样。

顺带修掉两个采样器缺陷: `FSamplerKey::UseAnisotropy` 参与查重键却从未进入
描述符 (各向异性被无条件启用); `MaxAnisotropy` 写死 16, 在只支持 8 的设备上
会被拒绝。

**一次值得记下的验证失败**: 第一版 A/B 给出"两张图逐像素相同"。查了视图
mip 范围、采样器 MaxLod、blit 链都对 —— 真正的原因是**测试资产本身错了**:
棋盘 64px 只平铺 4 次, 每个纹素约占 4 个屏幕像素, 全程处于放大状态,
mip 永远不会被选中。把平铺改成 32 次后差异立刻显现。**"测不出差异"的
第一嫌疑人应该是测法, 不是实现。**

另外发现 `.gitignore` 的 `*.obj` 规则同时匹配了 Wavefront 模型 ——
`Content/TestScene/` 的贴图与 `.mtl` 都进了仓库, 唯独 `testscene.obj`
从未被提交。

## Day 2 — Alpha 测试、半透明与双面渲染

glTF 与 MTL 的 `alphaMode` / `doubleSided` 一直被解析出来却从未使用。
Sponza 的帷幔与植被全部渲染成不透明实心片, 且每片叶子只剩朝向相机的一半。

**三条彼此独立的正确性要求:**

1. **深度预 Pass 必须做同样的 alpha 测试。** 不做的话它会为完全透明的
   纹素写入深度, 把背后的东西挡掉; 而前向 Pass 又把这些纹素 discard,
   结果是叶片之间出现挖空的黑洞。两个 Pass 的裁剪结论必须逐纹素一致,
   否则 `DepthCompareOp=Equal` 的 Early-Z 在边缘处直接失配。
2. **半透明不能用 Equal 深度测试。** 它们不参与深度预 Pass (写了深度会
   挡住自己身后的同类), 深度缓冲区里没有它们的值, 用 Equal 会把它们
   **全部剔除**。改用 LessOrEqual, 仍禁止深度写入。
3. **双面材质的剔除模式两个 Pass 必须一致。** 深度预 Pass 剔掉的那一面在
   深度缓冲区里没值, 前向再画就整片失配。

管线排列: 前向 {不透明,半透明}×{单面,双面} 共 4 条, 深度预 {单面,双面}
共 2 条。半透明在**同一个 RenderPass 内**换管线继续绘制 —— 它要读已画好的
不透明像素做混合, 另起通道意味着颜色附件 Store 再 Load 一遍。

排序验证 (两块交叠玻璃, 反转排序做对照): 交叠区 **55% 的像素发生变化**,
最大通道差 17。正确顺序下近处的琥珀色在前 (170,167,145), 反转后变成蓝色
在前 (156,163,161), 与几何位置一致。

半透明排序**不受 `--no-sort` 影响** —— 它不是优化而是正确性要求。

## Day 3 — 方向光阴影贴图 (单张)

新增 `FShadowPass`: 从光源视角渲染 2048² D32 深度图, 前向 Pass 用比较
采样器做 3×3 PCF。

几个不显然的选择:
- **正交体积按包围球而非包围盒拟合。** 包围球半径与光源方向无关, 正交体积
  在光源转动时尺寸恒定; 用包围盒的话体积随角度变化, 转动太阳时阴影边缘
  会"呼吸"。
- **单面材质绘制背面 (`CullMode::Front`)。** 自遮挡的根源是深度图的量化
  误差; 只记录背面深度后, 正面着色点与记录值之间隔着整个物体的厚度。
  薄片几何没有厚度可用, 因此双面材质仍双面绘制并依赖 bias。
- **阴影贴图放在 set 2 而非 set 0。** set 0 的描述符集**阴影 Pass 自己也要
  绑** (它需要光源矩阵), 把正在写入的阴影贴图放进去会形成"同一帧内既作为
  附件写入又作为纹理读取"的冲突。
- **`sampler2DShadow` 而非手写比较。** 前者硬件在 2×2 邻域上先比较再平均;
  后者拿到的是四个纹素**深度值**的插值再比较 —— 那是在深度域里插值,
  边缘会出现明显阶梯。
- **采样器用 ClampToBorder + 白色边框。** 贴图之外深度视为 1.0, 任何着色点
  与它比较都通过, 即判为无遮挡。用 Clamp 会让边缘那一列纹素被无限拉伸,
  场景边界外出现一道贯穿画面的假阴影。

## Day 4 — 级联阴影 (3 级 CSM)

单张贴图覆盖 Sponza 30 单位宽的中庭时, 脚下与三十米外分到的精度完全一样,
而人眼对脚下敏感得多。同机位实测阴影边界的梯度幅值:

| 区域 | 单张 | 3 级级联 | 变化 |
|------|-----:|--------:|-----:|
| 拱券边界 (中景) | 25.7 | 62.8 | +144% |
| 地面斑块 (近景) | 44.3 | 75.7 | +71% |

- **每级拟合到视锥切片的包围球而非角点 AABB。** 球在相机旋转时半径不变,
  正交体积尺寸恒定; 用 AABB 的话体积随视角摆动, 阴影边缘随相机转动闪烁 ——
  这是级联阴影最经典的坑。
- **正交中心吸附到纹素网格。** 不做的话相机每移动一点点, 投影平移不到一个
  纹素的距离, 静止时看不出, 一走动就满屏爬行。
- **选级按到相机的径向距离而非视空间 Z**, 与包围球拟合口径一致。球是按
  径向距离定义的, 改用平面距离选级, 切片角落会选到未覆盖该处的级别。
- **切分用对数与均匀的加权混合 (λ=0.75)。** 纯对数数学最优但最近一级会薄到
  几十厘米, 相机稍一动就跨级, 边界突变反而更扎眼。

**一个改完把画面改掉了的修正**: Day 3 的阴影 Pass 直接用了 `FSceneManager`
推来的列表, 而那是**相机剔除后**的 —— Sponza 里只有 15/103 个批次进了阴影
贴图。相机背后的柱子照样会把影子投进画面。改为在剔除前另留一份投射体列表,
阴影 Pass 再按各级自己的光源视锥剔一次。

这个修正显著改变了画面: Sponza 内景从"地面有一块日照斑块"变成"整体处于
拱廊屋顶的阴影中"。**后者才是对的** —— 之前那块斑块正是屋顶未被画进阴影
贴图的产物。用两个对照排除了"改坏了"的可能: 测试场景完全没变, 外景视图里
朝光的外墙依然正常受光。

## Day 5 — 材质纹理引用计数与关卡切换

`FMaterial` 持有的是裸 view/sampler 句柄, 与资源管理器之间没有引用关系。
`FSceneLoader` 因此不敢释放纹理的创建引用 —— 一放, 引用计数即归零,
下一次 `CollectUnreferenced` 就会销毁仍被材质描述符集引用的视图。结果是
导入的纹理只能随资源管理器整体销毁, 关卡切换时显存只增不减。

新增 `FMaterial::BindTextureResource`: 绑定时加引用, 解绑/销毁时释放。
`FSceneLoadResult` 携带本次导入创建的材质列表, 配套 `UnloadMaterials()`。

卸载顺序是: 销毁场景 (Trait 析构释放网格引用) → 销毁材质 (释放纹理引用) →
收割 → 等 GPU 空闲后冲刷退役队列。顺序反了的话, 材质销毁时 Trait 还活着,
网格引用未放, 收割只能收回纹理。

新增 `--reload-test` 自检 (已并入 `verify.ps1` 第 10 步)。显存能否回落是
"引用计数是否真的接通"的唯一硬指标 —— 靠肉眼看画面完全看不出泄漏。

| 场景 | 加载后 | 卸载后 |
|------|-------:|-------:|
| 测试场景 | 83 KiB | 0 |
| Sponza | 387979 KiB | 0 |

### 顺带挖出一个 Core 级缺陷

`TArray::RemoveAt` 会销毁仍在数组中的元素。实现是"析构目标 → 移动构造整段
前移 → 析构源段", 而源段与目标段重叠。以 `[A,B,C,D]` 移除下标 0 为例:

```
MoveConstructItems(&d[0], &d[1], 3)  →  d = [B, C, D, null]
DestructItems(&d[1], 3)              →  销毁 C 和 D, 它们仍是活对象
```

潜伏至今是因为绝大多数 `TArray` 装的是句柄、浮点这类平凡析构类型,
`DestructItems` 对它们是空操作。直到 `TArray<TUniquePtr<FMaterial>>` 第一次
用上 `RemoveAt` —— 删一个材质连带释放掉它后面的若干个。

改为移动**赋值**前移 + 只析构末尾那一个。`RelocateItemsBackward` 的
`destination <= source` 分支有同样问题 (当前无调用方, 属潜伏陷阱), 一并修正。

新增 `ArrayRemovalTests.cpp` (8 个用例): 断言的是**析构次数**而非"元素还在
不在" —— 只有计数才能分辨"逻辑移除"与"对象销毁", 而缺陷正在那一层。

## Day 6 — HDR 离屏目标与 ACES 色调映射

色调映射此前写死在 `pbr.frag` 末尾 (Reinhard + `pow(1/2.2)`)。三个问题:
它按定义是对**最终图像**的操作; 曝光这类全局参数无处安放; Bloom、TAA 需要
的是线性 HDR 输入, 颜色若在进入它们之前已被压缩, 亮部信息已经丢失。

`FPassManager` 新增共享 HDR 目标 (RGBA16_SFLOAT)。前向 Pass 画进它,
新增的 `FPostProcessPass` (Order=900) 用全屏三角形采样它, 曝光 → ACES → sRGB
输出到交换链。

- **全屏三角形而非两个三角形拼的四边形。** 四边形的对角线把屏幕切成两半,
  边界处的 2×2 像素四方格被拆开, 导数与四方格利用率都受损。
- **ACES 而非 Reinhard。** Reinhard 把三个通道独立压缩, 强光下先后饱和,
  高光一律偏白; ACES 的矩阵变换让高光沿更接近胶片的路径滚降, 保留色相。
- **sRGB 用分段精确式而非 `pow(x, 1/2.2)`。** 后者在暗部与真实曲线偏差可达
  数个色阶, 渐变与阴影过渡处会显出色带。
- **颜色附件 `LoadOp=DontCare`。** 全屏三角形覆盖每一个像素, 清除是纯粹的
  带宽浪费。

前向 Pass 的 Framebuffer 从"每交换链图像一个"变为只有一个 —— 渲染目标是
那一张共享 HDR 纹理, 与交换链的多缓冲无关。

## Day 7 — 稳定化、基准回归与文档

### 补上级联切分的测试

Day 4 时记下的债: 级联切分与包围球拟合是纯数学、完全可测, 却没有测试。
把切分算法提为静态纯函数 `FShadowPass::ComputeCascadeSplits`, 新增
`CascadeSplitTests.cpp` (12 个用例)。

测的是不变式而非具体数值, 因为它的错误方式全都是**静默**的:
序列不严格递增 → 某一级退化为零厚度, 着色器仍会采样它;
最远一级与覆盖距离差几个 ulp → 最远处留下一圈没有阴影的环带;
近平面为零 → 对数项得到非有限值, 一路传进正交矩阵, 阴影整体消失。

### 阴影投射体排序 (性能回归修正)

第二周结束时跑基准, 60×60 压力场景从第一周的 7.10 ms 涨到 **28.8 ms**。
查下去发现: 阴影投射体列表是主列表**剔除前**的副本, 而排序发生在剔除之后 ——
这份列表从未被排序过。阴影 Pass 要把它走三遍 (每级一遍), 不排序意味着
每个物体都可能重绑一次管线与材质集, 代价是主 Pass 的三倍。

补上排序后: **28.8 → 15.58 ms** (1.85×), Sponza **1.373 → 0.966 ms**。

### 第二周结束时的基准

60×60 = 3600 物体 / 48.5 万三角形 / 8 材质, 含 3 级 CSM 与 HDR 后处理:

| 配置 | 可见批次 | 材质切换 | 平均耗时 | 加速比 |
|------|--------:|--------:|--------:|------:|
| 基线 (都关) | 3600 | 3600 | 62.26 ms | — |
| 仅剔除 | 1880 | 1876 | 43.40 ms | 1.43× |
| 仅排序 | 3600 | 8 | 21.19 ms | 2.94× |
| 剔除 + 排序 | 1880 | 8 | 15.69 ms | **3.97×** |

与第一周 (无阴影无后处理) 的 7.10 ms 相比, 3 级级联阴影 + HDR 后处理的
代价约为 2.2 倍。这是正确性的价格, 不是回归。

Sponza (103 批次 / 26.2 万三角形): **0.966 ms/帧**。

### 稳定性

| 项目 | 方法 | 结果 |
|------|------|------|
| 交换链重建 | 8 次尺寸变更 (320×240 ↔ 1920×1080) + 最小化/还原 | 退出码 0, 零告警 |
| 显存回收 | 两轮加载/卸载, 逐步核对 | 完全回落到基线 |
| 校验层 | 演示 / 压力 / OBJ / Sponza 四类场景 | 全部零错误零告警 |

### 测试总量

| 测试集 | 用例 | 检查 |
|--------|-----:|-----:|
| CoreTests | 333 | 15,472 |
| RHITests | 38 | 117,966 |
| AssetTests | 127 | 1,120 |
| EngineTests | 35 | 301 |
| **合计** | **533** | **134,859** |

`Scripts/verify.ps1` 现为 10 步 (新增显存回收自检)。

## 遗留事项

- **资产导入是同步的**。Sponza 导入约 4.7 秒, 绝大部分花在 69 张 JPEG 的
  解码上, 未并行化。
- **`.github/workflows/ci.yml` 从未在真实环境跑通**。显存回收自检需要真实
  GPU, CI 运行器通常只有软件光栅化器 —— 首次触发若失败, 应确认运行器是否
  具备 Vulkan 设备, 而非直接删掉该步。
- **阴影只有主方向光**。其余光源按无遮挡处理。
- **包围球拟合与纹素吸附没有单独的测试**, 只有切分算法有。它们同样是纯数学。
- 第一周遗留的 `pragma 4273` 与 `FThread`/`TRefCounted` 的裸 `new/delete`
  仍未处理。

---

# 第三周 — 基于图像的光照 (IBL)

主题是把环境光从一个常数变成真正的环境光照。做完之后, 金属会反射它周围的
世界, 朝天的面与朝地的面收到不同的光, 而这一切都由一张 HDR 环境图驱动。

副产品比主线更值得记: 这一周的三个测试手段 (独立参考实现、变异验证、白炉)
各自揪出了一批"不报错、不崩、看着还行"的缺陷, 其中有几个已经在代码里待了
很久。

## Day 1 — Radiance HDR 解码与天空盒

自研 `.hdr` (RGBE) 解码器, 输出 32 位浮点; 计算着色器把等距柱状图转成
立方体贴图; 新增天空盒 Pass。

**天空盒必须排在前向 Pass 之前。** 直觉上天空在最远处该最后画, 但前向 Pass
同时画不透明与半透明, 而半透明**不写深度** —— 天空若排在后面, 会依据半透明
像素下方仍为 1.0 的深度通过测试, 把悬空的半透明物体整片抹掉。这是一个只在
"半透明物体前方没有任何几何体"时才出现的错误, 室内场景里可能几周碰不到一次。
清屏职责因此从前向 Pass 移交给天空 Pass。

**2 的幂用查表而非 powf。** RGBE 转线性需要 2^(e-136)。这张表可以从 1.0 出发
靠反复乘 2 与乘 0.5 推出 —— 这两个乘法在 IEEE-754 下对 2 的幂是精确的,
一路到非规格化区间的最小值 2^-149 都不丢位。表是精确的, 不是近似的。

### 变异验证: 让测试证明自己有用

34 个解码器用例写完后, 逐个把实现改坏, 看用例是否失败:

| 变异 | 结果 |
|------|------|
| 指数偏置 136 → 135 | 15 个用例失败 ✓ |
| 通道平铺当成交错像素 | 3 个用例失败 ✓ |
| `-Y`/`+Y` 方向反向 | 4 个用例失败 ✓ |
| 零长度游程判定去掉 | **0 个失败** ✗ |

最后一个暴露了用例本身的问题: 夹具的零长度字节后面跟的是坏数据, 少了判定
也照样会失败。改成后面跟一段**完整合法**的编码才真正区分开 —— 少了判定时
那段数据会被正常解出来, 于是一份已经错位的文件"解码成功"。

顺带修正了实现里的注释: 原先写"不判会死循环", 而游标每轮至少推进一字节,
数据总会耗尽。真正的风险是静默跳过。

## Day 2 — 漫反射辐照度卷积

环境光从常数改为按法线查预卷积的辐照度贴图。

### 独立参考实现揪出五倍误差

第一版从 mip 0 采样。用 Python 写了一个**换一种求积方式**的参考实现 (遍历
源图像素按 sinθ dθ dφ 加权, 而非切空间均匀步进), 逐面比对后发现朝阳方向
差了近五倍。

查下去发现: 这张 HDRI 的太阳只占 **0.00056 立体角却携带 73% 的总能量**,
而 0.025 的步长每个样本只代表约 0.0004 立体角 —— 太阳只值一个多样本, 大多数
纹素的求积完全踩空。

改成从边长约 `(π/2)/sampleDelta` 的那一级 mip 采样后, 非太阳方向的四个面
与参考实现吻合到 **2.5% 以内**。

mip 选取取几何中点而非"第一个不超过目标的": mip 边长按 2 的幂跳变, 后者会
在目标略小于某一级时一路跳到它的一半, 平白多模糊一倍。

### 半精度钳位不再静默

剩下的太阳方向偏差全部来自 RGBA16F 的上限: 这张图有 **7 个像素**超过 65504
(峰值 183296)。仅这 7 个像素被钳, 朝阳方向的辐照度就少三成 —— 而画面上只
表现为"环境光有点暗"。现在上传时统计并告警。

## Day 3 — 镜面 IBL (split-sum)

预滤波 mip 链 (128², 六级粗糙度, 1024 个 GGX 重要性样本) + BRDF 查找表
(256² RG16F)。

**按 pdf 选源 mip。** 每个样本按自身概率密度算出它"代表"多大立体角, 再选一级
纹素立体角与之相当的 mip。这是 Day 2 太阳欠采样问题的精细版解法 —— 直接从
mip 0 采会在高粗糙度下爆出刺眼亮点。

**除以权重和而非样本数。** 用样本数会让结果随粗糙度递增地偏暗 (粗糙时更多
样本被 NdotL≤0 丢弃), 表现像是能量守恒出了问题。

**Smith 的 k 用 IBL 版 `roughness²/2`** 而非直接光的 `(roughness+1)²/8`。
混用会让掠射角能量差出十几个百分点, 看着像菲涅尔没调好。

预滤波与查找表共用 `ibl_common.h`: split-sum 拆开的前提就是两半用同一个法线
分布与同一套样本分布。两处各写一份, 只要有一处把 `a=roughness²` 写成
`a=roughness`, 两半就不再互补, 而表现只是"金属看着有点闷"。

### 参考实现也会欠采样

BRDF 表与独立的 Python 参考 (半球均匀网格直接积分, 不做重要性采样) 逐点
比对: 25 个采样点中 24 个偏差 ≤0.004。

`roughness→0` 那一行参考给出 0 —— 它的均匀网格分辨不出近似 delta 的 GGX 瓣,
是**参考侧**的欠采样, 与 Day 2 的太阳同一个失败模式换了个方向出现。改与
解析镜面极限 `A = 1-(1-n·v)⁵` 比对, 吻合到 0.002 (即 RG16F 的量化精度)。

## Day 4 — 白炉测试与能量守恒

白炉测试: 把物体放进各方向辐射度恒为 1 的合成环境, 反照率为 1 的表面必须
原样反射回 1, 也就是应当**完全消失在背景里**。这是能量守恒唯一的客观判据,
也是第一次有一个已知真值可以拿来核对渲染出的像素。

它一上来就抓出三个各自独立的 bug。

### 一、tonemap 双重 sRGB 编码

交换链拿到的是 `B8G8R8A8_SRGB`, 硬件在写入时已做 sRGB 编码, 而 `tonemap.frag`
又手工编了一遍。两遍 gamma 不产生任何瑕疵, 只是把整幅图往亮处推 —— 此前
所有截图那种发灰发白的观感就是它。

判据是硬的: 背景 (线性恰为 1) 应当是 **206**, 实测 **232** = 双重编码的预测值。
改为按 `IsSRGBFormat` 运行时决定后, 背景精确落在 206。

### 二、GenerateSphere 反向缠绕

所有球体三角形的缠绕都是朝内的, 于是背面剔除剔掉了朝向相机的那半球, 画面上
留下的是**远侧**半球 —— 法线整体背对相机。

诊断: 把 N 与 V 输出成颜色, 反解 ACES 后得 `N=(0.006,-0.017,-1.000)`、
`V=(0,0,+1.000)`, 残差为 0。N 与 V 恰好反向。再用 Python 复刻生成器算叉积:
48 个三角形全部朝内, 而立方体的 +Z 面朝外 —— 球体是唯一不合群的那个。

这个错误的轮廓与正确版本一模一样, 只有着色不同; "看到的是远侧表面"在有主光
的场景里只表现为"光好像从另一边来"。新增 6 个纯 CPU 用例钉住缠绕约定。

### 三、无光源时阴影贴图停在 UNDEFINED

关掉全部直接光后, 阴影 Pass 直接 return, 深度图再没被转换过布局; 而片段
着色器里那句 `sampler2DArrayShadow` 只要出现, Vulkan 就要求它在绘制时处于
`SHADER_READ_ONLY` —— 着色器有没有真去采样并不重要。

### 多次散射能量补偿

白炉实测单次散射 GGX 的损失: 粗糙度 0.5 丢 10%, 0.75 丢 39%, **1.0 丢 69%**。
接入 Fdez-Agüera (2019) 的近似 —— 不需要任何额外贴图, 所需的 A、B 就是已有的
查找表。

结果 (线性辐射度, 理想 1.000):

| | 补偿前 | 补偿后 |
|---|---|---|
| 金属度 0, 全部粗糙度 | 0.994 | 0.994 |
| 金属度 1, 粗糙度 0.05 | 0.994 | 0.994 |
| 金属度 1, 粗糙度 1.0 | **0.307** | **0.994** |

中间金属度落在 0.60~0.87, 与解析预测逐格吻合到 ±0.006。那不是 bug: F0 升到
0.52 时漫反射反照率只降到 0.5, 二者之和本来就小于 1 —— 而金属度 0.5 本就
不是物理上存在的材质, 它只为纹理混合而存在。

### 判读工具自身的陷阱

第一版判读脚本从"与背景不同的像素"的包围盒反推球心 —— 而白炉**通过**的球
恰恰与背景完全一致, 于是整行掉出包围盒, 把所有采样点都挪了位, 读出一张看似
合理其实全错的表。改为按场景与相机参数解析投影。

**判读工具不能依赖它要判读的那个性质。**

## Day 5 — 材质系统整合与 Sponza

在 Sponza 上验证材质系统与 IBL 的配合。资产侧核查未发现问题: 纹理色彩空间
按用途区分正确 (基色与自发光走 sRGB 格式, 法线/金属粗糙度/遮蔽走 UNORM),
glTF 因子与贴图的相乘关系与 spec 一致, 金属粗糙度贴图的通道映射正确。

三个真正的金属材质按其数据渲染。其中吊盆几乎全黑一度可疑, 实测区域均值
25.1/255 —— 与"F0=0.0165 的金属在单位环境下反射 1.5%"的预测精确吻合。

### 照着 bug 调出来的参数

Sponza 室内在没有 HDRI 时整片全黑。原因是常数环境光 0.03 当初是对着一个双重
sRGB 编码的输出用肉眼调出来的: 那时它渲染成 45/255, 编码修正之后同样的 0.03
只剩 6.8/255。改为 0.15, 恢复的正是当初调这个值时想要的观感。

这类"参数是照着 bug 调出来的"在 bug 修好那一刻会一起暴露 —— 不一并处理就会
留下一个看着没道理的常数, 以后谁也不敢动。

### 一次误判, 和白炉的用处

遮罩植被在光照下看着灰白无绿, 一度怀疑金属度串了通道。放进白炉一看, 叶片是
清楚的橄榄绿, 遮罩边缘干净 —— 之前看到的灰白是透过叶片缝隙的大理石柱, 加上
植被本就在拱廊阴影里。**白炉把"材质对不对"与"照明对不对"这两件事分开。**

### 关卡切换自检覆盖 IBL

IBL 的三张贴图挂在逐帧共享的光照描述符集上, 而描述符集的生存期跨越整个进程 ——
换关卡时若只销毁贴图而不改描述符, 集里留下的就是指向已释放图像的视图。这是
这条路径独有的失效方式, 原有的加载/卸载引用计数覆盖不到。

## Day 6 — 显存口径与基准回归

### "已占用"漏掉了专用分配

分配器的统计只累加块内子分配, 而大资源 (渲染目标、立方体贴图) 走的是专用
分配路径 —— 它们在"已占用"里一个字节都不体现。实测环境贴图那 17 MiB 完全
隐形: 加载 HDRI 前后报出来的数字只差 1 MiB。

### 块尺寸: 固定上限 → 逐块翻倍

原先每种内存类型的**第一次**分配就按上限 (堆的 1/8, 封顶 256 MiB) 开块。
一个空场景只用 59 MiB, 却向驱动申请了 560 MiB。改成从 32 MiB 起、每多一块
翻一倍:

| 场景 | 占用 | 申请 (改前 → 改后) | 分配数 |
|------|-----:|-----:|-----:|
| 空场景 | 59 MiB | 560 → **112** MiB | 3 → 3 |
| 空场景 + IBL | 76 MiB | 576 → **128** MiB | 4 → 4 |
| Sponza + IBL | 455 MiB | 832 → **576** MiB | 5 → 7 |

三个数字互相对得上: Sponza+IBL 455 = 资产 378 + 引擎常驻 76, 而引擎常驻里
IBL 恰好占 17 MiB (76 − 59), 与 `FEnvironmentMap` 自报的一致。

### 基准回归

60×60 压力场景四配置与第二周记录一致 —— 剔除+排序 **15.55 ms** (原 15.58 ms),
一周的 IBL 工作没有引入吞吐回退。

基准脚本新增第五项配置 (剔除+排序+IBL)。需要说明的是这个差值在压力场景里
量不出来 (16.46 vs 16.76 ms, 落在噪声内): 那个场景是绘制调用受限的, 1880 个
批次面前四次纹理采样无足轻重。换到片段受限的场景 (49 个球填满画面) 三轮实测
1.00 → 1.07 ms, 约 **+0.07 ms**。两个数字都留着 —— 前者是回归哨兵, 后者才是
IBL 真实的着色开销。

## Day 7 — 并发回归测试与收尾

`FJobSystem` 与 `FTaskGraph` 此前**零测试覆盖**。补测试的过程里发现:

### FJobSystem 根本没有执行器

`FJobSystem.h` 提供的只是作业的**描述** (计数器、作业、批次、并行 For 的切分),
不含任何线程池或调度器。调用方必须自己把批次喂给某个执行器。这个名字容易
让人以为它会自己跑起来。

**`FJobCounter::Decrement()` 少减一。** 底层 `_InterlockedDecrement` 本就返回
递减后的值, 而实现又减了一次 1。后果是把 N 个作业的完成回调提前到第 N-1 个
结束时触发, 最后一个还在跑。而 `IsComplete()` 直接读计数不受影响 —— 两个接口
对"完成"的判断彼此矛盾。

**`FParallelFor::Create` 按引用捕获迭代体。** 最常见的写法就是直接传一个临时
lambda, 它在整条语句结束时就析构, 而批次要到之后才被执行。

### FTaskGraph 的三个缺陷

**`WaitForAll` 有真实竞态。** 原实现是"先看队列空、再看执行中为零", 两个条件
之间存在空隙: 工作线程刚把任务取出队列、还没来得及给执行中计数加一。改为
单一的待办计数 (入队时加, 执行完后减)。

变异验证显示这不是理论问题 —— 退回旧实现后 **25 轮全部失败**。

**`NotifySuccessors` 取全局单例。** 任何非单例的 `FTaskGraph` 实例, 它的后继
任务都会被投递到全局队列去; 全局若未初始化, 这些任务永远不会执行, 而调用方
只看到 `WaitForAll` 一直等不到。改为逐任务记住所属调度器。

**`Shutdown` 不排空。** 文档写的是"等待所有任务完成", 实际只置停止标志然后
join —— 队列里没跑完的任务被整批丢弃, 它们持有的调度器引用也就永远不会释放。

顺带: `FThread::HardwareConcurrency()` **只有声明没有定义**。没有任何调用方,
因此从来没有链接失败过 —— 第一个用它的人拿到的不是编译期错误, 而是一条指不到
源头的链接错误。

新增 20 个并发用例。全部断言只依赖与调度顺序无关的不变量 (每个任务恰好执行
一次、有依赖的必定在前置之后完成、`WaitForAll` 返回时无遗留), 没有一处 sleep。
连续 40 轮零失败。

### 白炉自检进入 verify.ps1

`--furnace-check` 把白炉从"看一眼截图"变成一条可以跑在 CI 里的断言, 一次覆盖
整条 IBL 链, 以退出码报告。三条判据都是解析可知的, 不依赖任何具体 HDRI,
因此不需要外部资产:

```
[白炉自检] 辐照度 min=0.99707 max=0.99707 — 通过
[白炉自检] 预滤波 mip 0..5 min=0.9995 max=1 — 通过
[白炉自检] BRDF A+B 全表最大 1.000001 (须 ≤1) — 通过
```

变异验证: 把预滤波的归一化分母改成样本数, 退出码立刻变 5。

## 第三周结束时的状态

### 测试总量

| 测试集 | 用例 | 检查 |
|--------|-----:|-----:|
| CoreTests | 361 | 15,925 |
| RHITests | 38 | 117,966 |
| AssetTests | 161 | 1,574 |
| EngineTests | 41 | 463 |
| **合计** | **601** | **135,928** |

`Scripts/verify.ps1` 现为 15 步 (新增 IBL 白炉自检)。

### 这一周学到的

三个测试手段各自抓到了不同类型的缺陷, 而它们抓不到的东西也不一样:

- **独立参考实现**擅长抓数值上的系统性偏差 (辐照度差五倍)。前提是参考必须
  用**另一种**方法算 —— 抄同一套公式只会把同一个错误算两遍。而参考自己也会
  错 (BRDF 那一行的欠采样), 所以出现分歧时先问"哪一边更可信", 而不是默认
  实现有问题。
- **变异验证**擅长抓测试本身的空转。写完用例就把实现改坏一遍, 是唯一能证明
  用例有用的办法 —— 这一周有两次靠它发现用例其实什么都没测。
- **白炉测试**擅长抓"看着还行"的错误。它的力量来自有一个**已知真值**:
  一旦有了可以逐像素核对的东西, 双重 gamma、反向缠绕这类隐藏很久的问题
  会一起冒出来。

一个反复出现的模式: **参数是照着 bug 调出来的**。环境光 0.03、以及那些为了
"看起来对"而调过的值, 在 bug 修好的那一刻会同时暴露。修 bug 时要顺手把它们
一起重新标定, 否则留下的是一堆看着没道理、谁也不敢动的常数。

## 遗留事项

- **资产导入是同步的**。Sponza 导入约 9.4 秒 (含 69 张 JPEG 解码), 未并行化。
  `FJobSystem` 缺执行器正是拦在这件事前面的东西。
- **`FJobSystem` 没有执行器**。数据结构齐备但无人调度; 要么补一个执行器,
  要么让它直接建立在 `FTaskGraph` 之上。
- **`.github/workflows/ci.yml` 从未在真实环境跑通**。显存回收自检与白炉自检
  都需要真实 GPU。
- **阴影只有主方向光**。其余光源按无遮挡处理。
- **UV 球两极各有一圈退化三角形** (低细分下占六分之一)。已单列任务。
- **镜面遮蔽与漫反射共用同一张 AO 贴图**。偏差主要出现在缝隙深处。
- **预滤波假设 N=V=R**, 掠射角的高光偏圆 —— 这是 split-sum 的固有取舍。
- 第一周遗留的 `pragma 4273` 与 `FThread`/`TRefCounted` 的裸 `new/delete`
  仍未处理。

