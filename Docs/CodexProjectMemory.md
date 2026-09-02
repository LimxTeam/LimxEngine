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

# 第四周 — 并行资产管线

主题是把资产导入从一条串行的路变成并行的。Sponza 的导入从 6.1 秒降到
0.82 秒 (7.4 倍), 而其中一笔关键收益并不来自并行。

这一周还顺手清掉了积攒的工程债, 并把验证拆成"无 GPU"与"需 GPU"两层。
清债的过程里揪出两个**从来不会失败的检查** —— 那比债本身要紧得多。

## Day 1 — 作业执行器

`FJobSystem` 的数据结构 (`FJobCounter`、`FJobBatch`、`FParallelFor`) 第一周
就写完了, 但没有任何东西调度它们 —— 它是一套没有执行器的作业系统。而
`FTaskGraph` 有工作线程和就绪队列, 只是不认识作业。

新增 `FJobExecutor` 把两者接起来: 作业投递进任务图, 完成时递减自己的计数器。
不另起一套线程, 因为线程池只该有一个。

修掉三个此前无人触发的缺陷:

- **`FJobCounter::Decrement()` 差一**。`_InterlockedDecrement` 返回的已经是
  减后的值, 代码又减了一次 1, 于是完成回调提前一个作业触发。没有执行器时
  没人调用它, 所以一直没暴露。
- **`FTaskGraph::WaitForAll` 有竞态**。原先是两阶段检查 (先看队列空, 再看
  活跃数为零), 两次检查之间任务可以从队列转到执行中。改成单一待处理计数器,
  在入队时加、执行后减。变异测试下原实现 **25 次运行全部失败**。
- **`FThread::HardwareConcurrency()` 声明了但没有定义**。第一个调用者直接
  撞上未解析外部符号。用现成的 `FPlatformMemory::GetProcessorCount()` 实现 ——
  第一次尝试是自己重新声明 `SYSTEM_INFO`, 与工程手工声明 Win32 API 的做法
  冲突。

`FParallelFor::Create` 原先按引用捕获函数体。批次是异步的, 调用方的栈帧
早就没了 —— 改为按值捕获。

## Day 2 — 并行纹理解码

Sponza 的 69 张贴图解码占导入总时间的 87%。解码是纯 CPU、解码器无可变
静态状态, 可以随便扇出; 上传必须留在调用线程 —— `BeginSingleTimeCommands`
用的是同一个命令池, 而 Vulkan 要求命令池由调用方外部同步。

导入 6.1 秒 → 1.0 秒。

**两次从背靠背运行里得出错误结论。** 先是"解码耗时在 580–2217 ms 之间剧烈
波动", 后是"上传从 125 ms 涨到 644 ms"。两次都是上一个进程的退出 (释放几百
MiB 显存、销毁设备) 与下一个进程的启动重叠。运行之间留 5 秒间隔之后,
解码 602–643 ms、上传 72–101 ms, 波动全部消失。

这条已经写进 `benchmark.ps1`: 导入基准的每两次运行之间强制 `Start-Sleep 5`。

## Day 3 — 流水线分波

一次解完 69 张图, 结果会同时驻留 272 MiB, 而这个数字随贴图总数线性增长 ——
五百张图的场景就是 2 GiB。分波把峰值钉在"一波的大小"上, 与场景规模无关。

但逐波阻塞很贵: 每一波末尾都要等最慢的那张图, 那段时间其余线程全闲着。
实测波长 16 时这项损耗高达三分之一 (816 ms vs 一次解完的 610 ms)。

改成流水线: 先把下一波排上队, 再去等本波。两个计数器轮流用, 同一时刻最多
两波在飞 (一波在解、一波在传), 峰值内存仍然封顶, 而线程不再有空窗。

## Day 4 — 图元装配

解码并行之后, 解析成了新的大头 (305 ms), 其中 103 个图元的装配占 283 ms。
它们原本追加进同一份 `FMeshData`, 索引偏移逐个依赖前一个的顶点数 —— 要并行
就得各建各的再合并。

**归因(实测, 不是估计):**

| | 装配 | 合并 |
|---|---:|---:|
| 原始 (串行 + 逐个 Reserve) | 283 ms (两者混在一起) | |
| 串行装配 + 预留总量 | 20 ms | 6.3 ms |
| 并行装配 + 预留总量 | 6.3 ms | 6.1 ms |

**大头不是并行, 是 `Reserve`。** 原先每个图元合并前都 `Reserve(当前 + 新增)`,
103 次调用把一个最终 11.5 MB 的数组反复搬了 103 遍。改成合并前一次算出总量、
预留一次, 单这一项就值 263 ms。并行只值 14 ms。

先量再改这条纪律这次省了大力气: 如果按直觉直接上并行, 会得到"并行化让导入
快了 8 倍"的结论, 而真实原因跟线程毫无关系。

并行仍然保留, 但加了阈值: 图元总数不足 32 就走串行, 不建任务图。建一张图
要创建十几个线程, 摊到两三个图元上完全是净亏。

合并保持串行且严格按原顺序 —— 渲染时按子网格顺序绑材质, 顺序一乱就是满场
材质错位, 而顶点数、三角形数这些统计量全都对得上, 看不出任何异常。为此补了
4 个多图元用例, 同时压住阈值两侧的两条分支 (线上唯一的 glTF 只走得到并行
那条), 其中一个重复解析 20 次 —— 顺序错乱是竞态, 单次运行大概率不发作。

`benchmark.ps1` 新增第二段: 资产导入基准, 取中位数, 超预算即以 1 退出。
导入是一次性成本, 不出现在任何逐帧数字里, 逐帧基准再绿也盖不住它退化。

## Day 5 — 工程债与 CI 分层

### 裸 new/delete

全工程只剩四处, 集中在两个文件。`FThread` 的线程上下文是跨线程转移所有权,
改用 `TUniquePtr` + `Release()` 表达。`TRefCounted::Delete()` 用裸 delete ——
而工程禁止裸 new, 也就是说这个设施当时**根本没有合规的创建方式**, 零使用者、
零测试。改为走默认分配器并补上配对的 `MakeRefCounted` 工厂, 加了 6 个用例。

豁免清单里八个文件只有这两个真含裸 new/delete, 其余六个早就改用分配器了,
豁免只是没人撤。两处修好后整份清单一起清空。

### pragma 4273

六处 pragma, 实测只有一处真会触发。UCRT 把 `strlen`/`strcmp`/`strstr`/`strchr`
声明为编译器内建 (不带 dllimport), 只有 `strncmp` 带 `_ACRTIMP`。于是不再压
警告, 改为对上链接属性: 新增 `LIMX_CRT_IMPORT`, 条件照抄 `_ACRTIMP` 的
`!defined _CORECRT_BUILD && defined _DLL`, 只加在 `strncmp` 上。

这个修法编译器自己会守: 给 `strcmp` 多加会报 C4273, 把 `strncmp` 的去掉也
报 C4273。压制做不到这一点, 而且会把同一段区域里真正的链接不一致一起吞掉。

### UV 球极点

两极处整行顶点坍缩到同一点, 而索引生成对每个格子一律发两个三角形, 于是每极
多出 `slices` 个零面积三角形 (slices=16 时占总数 12.5%)。改为两极那两圈各只
发一个。

顺序是先让测试红: 第三周的 `CountOutwardTriangles` 其实一直在统计退化三角形,
只是从没断言过 —— 等于默认接受。

### 两个从来不会失败的检查

清债过程里揪出的, 比债本身要紧:

**一、`lbt check` 的退出码。** 它一直是打印完错误就返回 `Ok`, 无论发现多少
Error 都退出 0。也就是说 `verify.ps1` 的"源码规则"那一步从未拦住过任何东西,
整套规则在 CI 里只是装饰。修好之后它立刻暴露了一个真 bug: checker 的块注释
追踪对首行做 `starts_with("/*")`, 而 `str::trim` 按 Unicode White_Space 判定,
U+FEFF 不在其中 —— 带 BOM 的文件首行失配, 整段文件头注释被当成代码扫。

**二、GUI 子系统程序不会被等待。** `LimxLaunch.exe` 是 GUI 程序
(PE Subsystem = 2)。PowerShell 的调用运算符 `&` 对 GUI 程序**不等待、也不
回填 `$LASTEXITCODE`**。`Invoke-Step` 把"未设置"当成 0, 于是显存回收自检与
白炉自检**从来没有真正验证过任何东西** —— 哪怕进程立刻崩溃, 那两步照样是
绿的。而那恰好是唯二两个跑真实 GPU 的步骤。

改为 `Start-Process -Wait -PassThru` 并显式取 `ExitCode`。验证方式是把
`$EngineExe` 换成 `cmd.exe /c exit 5`: 修之前报"通过", 修之后如实报失败。

这也解释了一个症状离奇的问题: 白炉自检的进程没被等待就继续跑下一步, 而它
持有日志文件 (`FILE_SHARE_READ`), 紧接着的基准脚本删不掉日志, 于是读到上一步
留下的陈旧内容, 报"日志中没有基准结果" —— 表面看像日志格式变了。

### 两层

`verify.ps1` 的每个步骤现在带层归属 (`Invoke-Step -RequiresGpu`):

- **无 GPU 层** (9 步): 工具链、着色器、源码规则、C++ 构建、单元测试
- **需 GPU 层** (3 步): 显存回收自检、IBL 白炉自检、资产导入回归哨兵

新增 `-SkipGpu` / `-OnlyGpu`。跳过的步骤会逐条列出, 汇总措辞是"无 GPU 层
9 个步骤全部通过"而不是"全部通过"。`ci.yml` 相应拆成四个作业, GPU 那个用
self-hosted + gpu 标签, 没有这类运行器时不会开始, 也就不会用假的绿色掩盖
"没验过"。

层的归属写在步骤本身而不是脚本外面, 是为了 `verify.ps1` 与 `ci.yml` 不会
各记一份而悄悄漂移。

## Day 6 — 基准与显存复核

逐帧基准 (60×60 网格, 300 帧):

| 配置 | 平均 ms | 帧率 | 相对基线 |
|------|-------:|-----:|--------:|
| 基线 (剔除关 排序关) | 63.42 | 15.8 | — |
| 仅剔除 | 44.42 | 22.5 | 1.43x |
| 仅排序 | 22.61 | 44.2 | 2.80x |
| 剔除 + 排序 | 16.00 | 62.5 | 3.96x |
| 剔除 + 排序 + IBL | 16.08 | 62.2 | 3.94x |

IBL 的逐帧成本 0.08 ms —— 三次立方体贴图采样加一次查找表采样, 与预期一致。

Sponza 导入 (5 次中位数): 总 819 ms | 解析 34 | 解码 644 | 上传 98。

显存两轮加载卸载完全回落, 设备关闭前块/子分配/专用全为 0。

## 第四周结束时的状态

### 导入耗时的变化

| 阶段 | 第三周末 | 第四周末 | 倍数 |
|------|--------:|--------:|-----:|
| 纹理解码 | 5530 ms | 644 ms | 8.6x |
| 解析 | 316 ms | 34 ms | 9.3x |
| 纹理上传 | 125 ms | 98 ms | 1.3x |
| 网格上传 | 3 ms | 2 ms | — |
| **总计** | **6085 ms** | **819 ms** | **7.4x** |

(均为缓存预热后的数字。冷启动第一次约 9.4 秒, 差额是文件系统缓存。)

四天的收益依次是: 并行解码 6085 → 1040 ms, 流水线分波把峰值内存从 269 MiB
压到 125 MiB 而耗时不升, 图元装配 305 → 37 ms。

### 测试总量

| 测试集 | 用例 | 检查 |
|--------|-----:|-----:|
| CoreTests | 382 | 19,192 |
| RHITests | 38 | 117,966 |
| AssetTests | 165 | 11,377 |
| EngineTests | 42 | 564 |
| **合计** | **627** | **149,099** |

`Scripts/verify.ps1` 现为 16 步 (新增资产导入回归哨兵), 分两层。

### 这一周学到的

**先量再改。** Day 4 的收益 95% 来自一个 `Reserve` 的误用, 5% 来自并行化。
按直觉直接上并行会得到一个完全错误的因果结论 —— 而那个结论会被"确实快了
8 倍"的事实所支持。

**当心失败模式指向"通过"的工具。** 这一周有三个:

- `lbt check` 打印错误但退出 0
- PowerShell 不等待 GUI 程序, `$LASTEXITCODE` 未设置被当成 0
- 我自己的变异脚本按中文解析测试输出, 解析失败时 `failed` 保持初值 0,
  于是把四个**确实被抓住**的变异全报成"漏过"

三者形状相同: 判定依赖一个在失败路径上取不到的值, 而取不到时默认落在"通过"
那一侧。这类工具比没有工具更糟 —— 它让人放弃正确的改动, 或者以为验证过了。
判定应当走退出码; 必须解析文本时, 解析失败要显式报错而不是 fallback。

**测量环境本身会说谎。** 背靠背运行同一个 GPU 程序, 上一个进程的退出会与
下一个的启动重叠, 把上传耗时推高十倍。这一周因此两次得出"某项优化引起退化"
的错误结论。


## 遗留事项

- **`.github/workflows/ci.yml` 从未在真实环境跑通**。第四周已拆成两层:
  无 GPU 层用托管运行器, 需 GPU 层用 self-hosted + gpu 标签。两层都还没在
  真实 GitHub Actions 上触发过。
- **纹理解码仍占导入的 79%** (644 ms / 819 ms)。已经并行到硬件线程数, 再往下
  要么换更快的 JPEG 解码路径, 要么改用预烘焙的 GPU 压缩格式 (BC7/ASTC) ——
  后者顺带能把 371 MiB 的纹理显存降下来。
- **纹理上传是串行的**。`BeginSingleTimeCommands` 共用一个命令池, 而 Vulkan
  要求命令池外部同步。要并行上传得先给资源路径引入每线程命令池。
- **阴影只有主方向光**。其余光源按无遮挡处理。
- **镜面遮蔽与漫反射共用同一张 AO 贴图**。偏差主要出现在缝隙深处。
- **预滤波假设 N=V=R**, 掠射角的高光偏圆 —— 这是 split-sum 的固有取舍。
- **场景导入整体仍是阻塞的**。并行的是导入内部, 但主线程仍要等它结束 ——
  还做不到边加载边出画面。


# 十五天计划 — 现代渲染管线

第五个周期从"七天"改成"十五天", 主线是现代渲染管线, 四条副线并行:
资产烘焙管线、CI 真正跑通、RHI 补齐与可移植性、文档与示例场景。

## Day 1 — GPU 逐 Pass 计时

在此之前所有性能判断都基于 CPU 侧的墙钟时间。加了时间戳查询之后第一件
事就是推翻了一个长期假设: **整条渲染是 CPU 受限的**, GPU 整帧只有
0.16 ms 而 CPU 帧时 14.78 ms。此前所有"优化 GPU"的方向都是错的。

- `IRHIDevice` 新增 `GetQueryResults` / `GetTimestampPeriod` /
  `GetTimestampValidBits`, 以及 `ComputeTimestampDelta` / `MakeTimestampMask`
  两个自由函数
- `FGpuProfiler` (RenderCore/Profiling): 32 个作用域 × 4 个帧槽位, 每槽
  `2 + 2*32` 个时间戳; 整帧占 0/1, 各作用域占 2+2i / 3+2i

两个坑值得记:

**`vkGetQueryPoolResults` 的 `VK_NOT_READY` 是非负值**, 所以判定必须写
`!= ERHIResult::Success` 而不是 `IsRHISuccess(...)` —— 后者会把"还没好"
当成成功, 于是读到上一帧甚至未初始化的时间戳。

**时间戳有效位可能少于 64。** `MakeTimestampMask(64)` 写成 `1ull << 64`
是 UB, MSVC 把它常量折叠成 0, 而 `0 - 1` 回绕成全 1 —— 结果**恰好正确**。
用例因此通不出问题。改用 `volatile UInt32` 挡住常量折叠之后, 那条变异
才如实变红。

## Day 2 — 次级命令缓冲区与并行录制

`FParallelRecorder` (Renderer/Recording): 把一个 Pass 的绘制切成若干段,
每段一个次级命令缓冲区, 并行录制后一次性 Execute 进主缓冲区。

三件事按顺序发生, 每一件都推翻了上一件的前提:

1. **先并行化了错的那个 Pass。** 直觉选了前向 Pass, 实测它只占 2.48 ms,
   而阴影 Pass 占 8.17 ms。加了逐 Pass 的 CPU 计时才看出来。
2. **验证层抓出了每线程命令池的设计错误。** `VkCommandPool 同时被线程
   A 与线程 B 使用` —— 静态绑定的是"段 → 池"而不是"段 → 线程", 两者在
   任务被窃取时不等价。改成一段一池。
3. **默认线程数是最差的配置。** 16 线程 19.15 ms, 而内联只要 14.66 ms。
   根因是任务图的就绪队列在单个互斥量后面, 线程越多争用越重。上限压到 4。

整帧 14.7 → 10.4 ms。

## Day 3 — 管线缓存与 bindless

**管线缓存持久化** (`Intermediate/PipelineCache.bin`): 管线创建
7.85 → 1.33 ms。

**bindless 材质**: `FBindlessTable` (RenderCore/Material), 1024 个纹理
槽 + 4096 项材质表, 逐 draw 的描述符集绑定 **9 → 0** 次。

这里有个 Vulkan 的坑值得单独记: **`descriptorIndexing` 是一个汇总特性,
打开它并不会打开 `descriptorBindingPartiallyBound`、
`descriptorBindingSampledImageUpdateAfterBind`、
`shaderSampledImageArrayNonUniformIndexing`、`runtimeDescriptorArray`**。
每一个都要显式请求。漏掉的表现不是创建失败, 而是运行时的未定义行为。
现在启动日志会逐项打印这几个子特性的状态。

## Day 4 — 薄 G-Buffer 与速度矢量

### OnResize: 一条从未被执行过的路径

把 5 个 Pass 的 `OnResize` 从 8 个位置参数重构成 `FPassResizeDesc`。
重构本身很平淡, 有意思的是**它从来没被验证过**: `OnResize` 只在交换链
重建时才走, 而自动化里没有任何东西会改窗口尺寸。"构建通过 + 画面没变"
看着像验过了 —— 而画面没变恰恰是因为那段代码根本没执行。

新增 `--resize-test N` 强制每 N 帧重建一次交换链。加上之后做变异 (把
深度视图换成颜色视图) 立刻产出 86 条验证层错误。

### 薄 G-Buffer

深度预通道从"只写深度"扩成三个附件: `[0]` 八面体编码的世界法线、
`[1]` 屏幕空间速度 (都是 RG16_SFLOAT)、`[2]` 深度。

**深度必须排在最后。** `BeginRenderPass` 填清除值时先按顺序填全部颜色
附件再无条件把深度追加到末尾; 顺序不对则清除值整体错位, 而数量仍然对
得上, 验证层不报错。`FVulkanDevice::CreateRenderPass` 里加了一条硬检查
专门拒绝错误的顺序。

八面体编码有两份实现 (`Shaders/Builtin/gbuffer_common.h` 与
`RenderCore/Profiling/FOctahedral.h`), 必须逐行对应。CPU 那份存在的唯一
理由是让编解码的正确性可以脱离 GPU 测 —— 而风险恰恰在边界方向上:
**GLSL 的 `sign(0)` 返回 0, 会让整条向量塌成零向量**, 而分量恰好为 0
的方向 (任何轴对齐的面) 在真实场景里到处都是。

法线附件的清除值取 **(2,2)** 这个哨兵。八面体编码的值域恰好填满
`[-1,1]^2`, 所以任何分量绝对值大于 1 的取值都不可能来自真实几何 ——
消费方据此区分"这里没有几何"与"这里有一个朝向恰好如此的面"。清成
(0,0) 也是合法单位向量, 但它 **是** 一个合法编码 (对应 +Z), 于是两者
永久无法区分。

### 相机矩阵 UBO 收成一份声明

此前五个顶点着色器各自声明一遍 `ViewProjUBO`。C++ 侧加字段要同步改五
处, 而**漏一处的表现是那条通道矩阵整体错位、画面全黑且无任何报错** ——
std140 不会因为着色器少声明一个字段就报错, 它只按自己声明的偏移去读。

现在统一 `#include "view_common.h"`。结构上不可能再漂移。

UBO 里多了一个预乘的 `viewProj`, 理由不是省一次乘法:

- **前向 Pass 的深度测试是 `Equal`**, 要求 `pbr.vert` 与 `gbuffer.vert`
  算出的 `gl_Position.z` **逐位相同**, 差一个 ulp 就整片剔除、物体消失。
  两处各写一遍 `proj * view * world` 时, 编译器对两处的重结合不保证一致。
- 速度要拿本帧与上一帧的裁剪坐标相减。上一帧那侧只能是单个矩阵; 本帧
  那侧若走两步路径, 相机静止时也会差一个 ulp, 速度成为 1e-7 的噪声。

### TAA 亚像素抖动

Halton(2,3), 周期 16 帧, 默认**关闭** (TAA 本身还没落地, 单开抖动只是
画质倒退)。

抖动只作用在写进 UBO 的那份投影矩阵拷贝上, 不回写相机 —— 剔除视锥由
`FSceneManager` 从相机矩阵导出, 抖动若进了那里, 每帧的可见集合会在压着
视锥边界的物体上反复跳变。

**速度矢量不含抖动。** 于是有了一条很强的不变量: 相机与物体都静止时,
速度缓冲**精确为零, 无论抖动是否开启**。

矩阵那一步单独抽成 `ApplyJitterToProjection`, 因为它有一个在画面上完全
看不出来的性质: 偏移必须与深度无关。写成 `M[0][3] += jx` 时画面同样
"在抖", 只是近处抖得多、远处几乎不抖 —— 表现为 TAA 对远景没效果, 而那
会被归因到 TAA 的历史权重上。GPU 侧的自检也抓不到 (两种写法都会让覆盖
掩码变化), 只有把变换结果逐点算出来才分得清。

### --gbuffer-check

三阶段 GPU 自检, 以退出码报告。**顺序不能换**:

- A. 相机静止渲一帧 —— 建立"上一帧矩阵"
- B. 只转动偏航角 (不平移) —— 速度必须**非零且逐像素等于 CPU 预测值**
- C. 相机保持不动 —— 速度必须**处处恰好为零**

只测 C 的话, 一个"速度恒为零"的实现会完美通过。B 证明这条通路真的能产
出非零值, C 才证明它在该为零时确实归零。

B 只转不平移是有讲究的: 纯旋转下同一条视线上的所有点投影到同一个像素,
速度因而与深度无关 —— 可以只凭像素的 NDC 坐标算出精确预测值。而深度在
场景渲染结束后是 `DontCare` 的 (前向 Pass 如此声明), 本来也读不回来。

实测: 509684 个覆盖像素逐像素吻合, 最大偏差 6.6e-5 (约 1 个半精度 ulp)。
抖动开启时两帧覆盖差 683 个像素 —— 这是"抖动真的作用到了投影矩阵上"的
直接证据。

### lat: BC 纹理离线烘焙

新工具 `Programs/lat` (Rust, ~3265 行): PNG/JPG → 带完整 mip 链的
BC1/BC3/BC4/BC5/BC7 DDS (DX10 容器)。纹理显存 **362.67 → 47.34 MiB**
(7.66 倍)。

### lsc 反射一直是坏的

`lsc compile-all` 产出的 `.json` 反射从来没对过, 而且坏得没有任何报错。
根因是 **`OpMemberDecorate` (opcode 72) 根本没有解析分支** —— 没有
`Offset` 装饰就无从算布局, 于是 `size` 是写死的 0。同族的还有:
`entry_point` 越界读进了 interface 变量 id 列表; 顶点输入的 `vec_size`
恒为 4; `bindlessTextures[1024]` 因为采样器匹配用数组类型去比而**整个
绑定从 JSON 里消失**; 变量从 `HashMap` 迭代导致同一个 `.spv` 两次产出
的 JSON 字节不同。

还有一处吞错误: `reflect_spirv(...).ok()` 把失败静默丢掉。

### 跨语言的布局一致性

反射可信之后, `Scripts/verify-shader-layout.ps1` 才有意义: 从 C++ 的
`static_assert` 正则读出期望值, 与 SPIR-V 反射逐个比对。verify.ps1 与
CI 调同一份脚本。要检查哪些着色器由"谁 include 了 view_common.h"决定。

这补上的是**跨语言那一跳**: 两侧各自都编得过、各自都有保障, 只有画面
会错。

## 这几天反复出现的同一件事

**失败模式落在"通过"上**的检查, 这个周期又抓到七个:

- 时间戳的 `VK_NOT_READY` 被 `IsRHISuccess` 当成成功
- `1ull << 64` 被折叠成 0, 而 `0 - 1` 回绕成全 1, 结果恰好正确
- `lsc validate` 不接受目录 (`read_to_string` 对目录报 os error 5),
  CI 的着色器作业一直是红的却没人看
- `LimxEngineTests.exe` 静态导入 `vulkan-1.dll`, 在 CI 运行器上根本
  起不来 —— 靠一个 `--list` 启动探针步骤才发现
- 我自己写的"法线解码后是单位向量"是**同义反复** (`Decode` 末尾就归一
  化了, 恒真), 而且没有覆盖率下限, 整张图全是哨兵时照样报"全部通过"
- `lsc` 反射的 `size` 写死为 0, 下游分不出"没有 UBO"和"读错了"
- 我的变异脚本把步骤体摊在 PowerShell 顶层, 体内的 `return` 直接结束
  脚本, 后面的 `exit $LASTEXITCODE` 走不到, 退出码变成 0

Day 9 又添两个, 都是同一形状:

- 三个套件 (ClusterGrid、Octahedral、CompressedTextureFormat) 注册了整整
  一个周期却**一次都没执行过** —— `verify.ps1` 与 `ci.yml` 都是逐个套件名
  手写的, 加了套件没人补那一行。"没执行"与"全过"在退出码上完全一样, 而
  `--list` 那一步只列举不执行。
- 图集 Pass 的裁剪矩形: 我自己写下的自检只用两块, 都落在交换链矩形之内,
  把 `SetScissor` 注释掉照样通过。**"检查通过"不等于"这一行可以删"。**

形状完全相同: 判定依赖一个在失败路径上取不到的值, 而取不到时默认落在
"通过"那一侧。**唯一可靠的对策是变异验证**: 每写一条检查, 先把被检查的
东西改错, 确认它真的会红。这个周期的每一条检查都做了, 判定一律走退出码。

Day 9 之后又多一条推论: **变异验证本身也有覆盖边界。** 裁剪矩形那一条在两块
的规模下逃掉了, 换到 64 块才被抓住 —— 同一个变异, 同一条判据, 只是场景的规模
不同。所以"某条变异被抓住"要连着**在什么规模下**一起记, 否则下一个人会以为
这条路径已经保住了。

Day 10 与 Day 11 把这条边界补全成三类:

1. **判据不够** —— 裁剪矩形那条在两块时逃掉, 64 块才现形。
2. **场景不够** —— 压力场景全是单面材质, 于是"分组漏看单双面"无从判定; 没有
   地面, 于是整条级联阴影路径的三条变异一条都抓不住。判据完全正确, 只是场景
   里没有能让它区分的东西。
3. **后果不在画面上** —— "拿未初始化的显存当间接命令"把两道防线同时拆掉,
   两条判据仍然全绿。风险在 indexCount 可能是几十亿, 而那要么什么都不发生,
   要么设备复位, 从不落在某个像素上。

第三类没有解法, 只能写下来。前两类有: 判据里加一条"这个场景够不够判"的元判据
—— --gpu-driven-check 现在会报源列表里双面物体的数量, 单双面没混合就直接判
失败。**通过与"没东西可判"必须分得开。**


## Day 5 — 分簇光照 (Clustered Forward+)

光源上限从 16 提到 1024, 前向 Pass 的时间与光源数解耦。

### 数字 (RTX 3060, 1280×720)

前向 Pass 的 GPU 时间:

| 光源数 | 暴力 | 分簇 | |
|---|---|---|---|
| 2 | 0.066 ms | 0.073 ms | 0.90x |
| 146 | 2.005 | 0.073 | 27.6x |
| 402 | 5.430 | 0.067 | 81.6x |
| 786 | 11.082 | 0.068 | 164x |

算上剔除通道本身的整帧数字:

| 光源数 | 暴力整帧 | 分簇整帧 | 其中剔除 | 加速 |
|---|---|---|---|---|
| 146 | 2.276 ms | 0.231 ms | 0.051 ms | 9.87x |
| 402 | 6.330 | 0.285 | 0.111 | 22.2x |
| 786 | 11.967 | 0.376 | 0.203 | 31.8x |

### 结构

- **光源数据搬进 storage buffer**。UBO 装不下 (保证上限 65536 字节, 而
  1024 × 80 = 81920), 而且分簇的计算着色器要读同一份数据。
- **网格固定 32×18×32**, 不随分辨率变化。这是刻意的取舍: 随分辨率变化的
  网格意味着簇缓冲区要在交换链重建时重新分配, 而 Pass 的 `OnResize` 拿不到
  描述符句柄 —— "描述符仍指向已销毁的缓冲区"是个只在 resize 时触发、平时
  完全看不见的坑。固定网格把这一整类问题从结构上消掉。代价是高分辨率下
  剔除粒度变粗。
- **方向光排在光源缓冲区最前面**。它们不参与分簇 (照亮整个场景, 分给每个簇
  等于没剔除), 所以分簇模式下片段着色器要单独遍历它们。散落在缓冲区各处的
  话, 那一遍就得扫过全部光源并逐个判类型 —— 每像素 O(N) 的分支, 而分簇的
  全部意义就是消掉那个 O(N)。
- **索引表用全局原子计数器碰撞式分配**, 每簇拿到一段连续区间。先数一遍再
  分配, 而不是边测边 atomicAdd —— 后者会让同一簇的索引在表里不连续, 而片段
  着色器要的是连续区间。
- **容量超限显式报告**。静默截断只在光源密集时触发, 而那正是分簇本该发挥
  作用的场景; 表现是"某些区域少几盏光", 看起来像衰减参数的问题。

### 两条判据, 覆盖完全不同的失效方式

- `--cluster-check` 回读簇表, 与 `FClusterGrid.h` 的 CPU 参照**逐簇逐项**
  比对。实测 18432 个簇全部吻合 (包围盒最大相对偏差 1e-6)。
- `--light-cull-check` 同一帧、同一个着色器、只翻转一个布尔值, 两次画面
  必须逐像素一致。实测 2764800 个通道**最大差异 0/255**。

簇表全对而片段着色器查错簇时, 前一条全绿。反过来也一样。

用运行时开关而非着色器变体, 就是为了让后一条能在**同一份代码**上比对 ——
两个变体的话, 比出来的差异里就混进了"编译器对两份代码的不同优化"。

### 这一天抓到的

**`--cluster-check` 第一次跑就抓到了真错误**: 两个计算着色器的 push
constant 块漏了 `row_major`。GLSL 按列主序解读 C++ 的行主序矩阵 = 整体转置,
簇边界在 x/y 上差一个数量级而 z 恰好不变 (z 是显式写的 `-depth`)。画面上
"有光", 只是每个簇拿到的光源集合都不对。`verify.ps1` 的"矩阵存储序约定"那
一步也会拦住它 —— 两张独立的网抓到同一个错。

**`barrier()` 与提前 return。** 共享内存优化引入的坑: `barrier()` 要求
工作组里全部线程都到达, 在非一致的控制流里调用是未定义行为。当前
`kClusterCount = 18432` 恰好是 64 的整数倍所以没有越界线程 —— 但那是巧合式
的前提。改用标志位之后这个前提不必成立。

**一条用例的前提本身写错了。** 断言相邻簇的包围盒"首尾相接"。用轴对齐盒去
包视锥切片时相邻盒**必然重叠** —— cx 的右边界来自它远端那一面, cx+1 的左
边界来自近端那一面。重叠是良性的 (剔除偏保守), 有缝才是错的。判据改成
"不能有缝"。

**变异验证的对照组也会设错。** "只用演示场景的两盏光"本想作为空转对照, 但
单盏 range 8 的点光就覆盖 67% 的簇 —— 那是真实分配。真正的空转对照是"计算
通道整个不执行"。

### 附带修掉的

- `verify-shader-layout.ps1` 只查 set 0/binding 0, 而 `pbr.frag` 里
  `LightData lights[16]` 是个**字面量**。C++ 侧改 `kMaxLightCount` 时
  `static_assert` 会编译失败逼你改数字, 然后你改了数字、着色器依然是 16 ——
  之后 UBO 里所有字段整体错位, 零报错。检查改成一张结构体表。
- 新增**图像回归** (`Scripts/image-signature.ps1`): 32×18 平均池化的签名,
  1728 字节可提交。此前所有 GPU 自检验的都是数值性质, 没有一条能回答"画面
  看起来还对不对"。变异验证 3/3 (改环境光 / 少一盏光 / 关阴影)。
- RHI 的三处静默上限 (屏障、清除值、无效次级命令缓冲区)。观测手段是图像
  布局状态机与同步验证 syncval —— 光跑一遍看结果证明不了任何事, 少一个屏障
  在 RTX 3060 上照样跑出正确结果。


## Day 6 — 时域抗锯齿

Day 4 就位的抖动基础设施一直空转, 这一天接上解析。

### 结构

- **MRT 同时写解析目标与历史。** 解析目标是固定的一张纹理, 后处理通道的
  描述符一次写定; 历史是乒乓的两张之一。若让后处理直接采样乒乓的历史, 它的
  描述符就要逐帧改 —— 而改一个正在被上一帧使用的描述符集是验证层错误。用
  一次全屏拷贝也能解决, 但 MRT 的第二次写入几乎免费。
- **抖动与解析同开同关。** 分成两个开关的话, 三种非法组合里有两种在画面上
  看起来"只是有点糊": 抖动开而解析关 = 纯粹多一层每帧变化的亚像素噪声;
  解析开而抖动关 = 每帧采样位置相同, 累积不出任何新信息。
- **方差裁剪**而非 3x3 min/max 包围盒。后者会被邻域里单个亮点撑得很大, 于是
  失效的历史也落在范围内被接受, 表现为高光周围的拖影。

### 验收: 证明它真的在做抗锯齿

TAA 最危险的失效方式是**看起来正常但什么都没做**。裁剪范围取小了历史每帧
都被拉回当前值, 画面完全正常, 只是锯齿还在 —— 而那要对着屏幕看边缘才发现。

`--taa-check` 的判据是数值的, 三条:

1. **有效性**: 抖动 32 帧 (周期 16 的整数倍, 于是每个抖动位置权重相同) 的
   均匀平均就是超采样真值。TAA 输出必须比其中任何单帧都显著更接近它。
2. **收敛性**: 静止场景下连续两帧的输出必须几乎相同。
3. **对照**: 单帧与均值的距离必须足够大 —— 否则第一条退化成"0 < 0"。

实测: TAA 与 32 帧平均的 RMS **0.478**, 单帧 **1.705** —— 近 **3.57 倍**;
连续帧 RMS 0.190。变异验证 7/7。

### 踩到的

**第一帧历史纹理停在 Undefined 布局。** 着色器虽然跳过对它的采样, 但 Vulkan
要求描述符指向的图像在绘制时处于描述符写入时声明的布局 —— 验证层无法证明
那个动态分支不会走到。

**自检里的回调忘了摘。** 阶段 2 的 32 次 RenderFrame 还挂着阶段 1 最后一个
栈上 FScreenshotCapture 的引用, 而它已经出作用域。表现是
"dstBuffer is VK_NULL_HANDLE" 后接访问违例, **崩溃点离真正的原因隔着几十帧**。

GPU 开销 0.058 ms (1280×720)。


## Day 7 — GTAO 环境光遮蔽

Day 4 那张 G-Buffer 法线的正式用途。

### 位置不是随便挑的

排在深度预通道 (100) 与前向通道 (200) 之间: 前向通道对深度的 StoreOp 是
DontCare, 之后深度内容就是未定义的, 所以任何要读深度的东西都必须在它之前;
而 AO 的结果又要被前向通道消费。区间只剩 (100, 200)。

法线取自 G-Buffer 而不是从深度差分重建 —— 差分在深度不连续处会给出完全错误
的方向, 而那正是 AO 最需要准确的地方 (物体边缘)。

关闭时输出恒为 1 而不是让前向通道分支: 有分支的话"AO 通道没跑"与"AO 恰好
全是 1"在画面上无法区分, 而前者是缺陷。

### 验收: 墙角的解析值

90 度凹角处余弦加权的可见度解析值是 0.5 (半个半球被挡住)。但那是"搜索半径
→ ∞"的极限 —— 有限半径下只看得到墙的一段, 遮挡必然偏小。实测:

    半径 0.8 → 0.679    半径 2 → 0.586    半径 8 → 0.557

所以判据不是"等于 0.5", 而是**随半径增大朝 0.5 单调收敛**。那是这个算法的
物理签名, 而写错的实现不会有: 它们要么恒为 1, 要么与半径无关, 要么往反方向走。
自检跑两个半径并断言这个趋势。

用专门的 `--corner-scene` (两个 20x20 平面成直角)。多一个物体解析值就不再
成立, 而"解析值不成立"与"实现算错了"在结果上无法区分。

变异验证 8/8。

### 这一天最实在的发现

**半径衰减最初写的是线性** (1 - len/radius), 结果凹角处收敛到 **0.735** 而非
0.5 —— 差了近 50%。原因是遮挡分布在各个距离上, 而线性衰减把每一处都按距离
打了折, 系统性地压低地平线角。

改成二值 (半径内全额计入, 半径外不计) 之后才朝 0.5 收敛。二值把半径还原成
它本来的含义: 一个**搜索范围**, 而不是一个权重。

**这个错误在画面上完全看不出来** —— 两种写法都给出一张"边角发暗"的图, 而
那正是人眼期待看到的。没有解析判据的话它会一直留在那里。

### 一次失败的优化, 以及它教的东西

试着把视空间重建从逆投影矩阵换成"视线方向 x 线性深度": 每样本从 16 次乘加
加一次除法降到 3 次乘法, 而每像素有 64 次采样。

改完逐像素结果完全一致 (0.585693 vs 0.585694), 而 GPU 耗时 **0.856 ms 对
0.854 ms** —— 各测三次取中位数, 差异落在噪声里。这个着色器**不是 ALU 受限
的**: 64 次深度采样的取数延迟完全盖过了算术。回退。

过程中有一次单点测量给出 1.32 ms, 比基线高 42%, 差点让我得出"优化反而变慢"
的结论。三次取中位之后两边都在 0.85。**单点测量在 GPU 上的波动足以掩盖或
伪造 40% 的差异。**

真正的杠杆是减少采样次数 (半分辨率 + 双边上采样)。留作后续。

GPU 开销 0.85 ms (1280×720, 4 方向 x 8 步进)。默认关闭。


## Day 8 — 泛光

13 抽头降采样 (Jimenez, SIGGRAPH 2014) + 3x3 帐篷升采样, 6 级链, 半分辨率。

### 为什么不是 2x2 box

box 降采样每一级都让高频混叠, 而链要降 6 级 —— 混叠逐级累积。表现是画面上有
细小高光时泛光会**闪烁**: 高光移动不到一个像素, 采样点却跳到了另一个纹素上。
那种闪烁静止时看不出来, 一动起来就明显, 而那时人会去怀疑 TAA。

13 抽头分两组: 4 个在半纹素对角 (合计权重 0.5), 9 个在整纹素的 3x3 (合计
0.5)。两组都归一化, 整个核的权重和恰好 1 —— 能量守恒, 自检直接断言这一点。

阈值提取融进第 0 级降采样。分两个通道也行, 但那要多一张全分辨率的中间纹理和
一次全屏读写, 而阈值本身只是一个 max。用**软膝盖**而非硬截断: 硬截断会让亮度
刚好跨过阈值的像素在泛光里突然出现, 表现为物体边缘的泛光边界随相机移动而抖动。

### 验收: 点扩散函数

一个孤立亮点经过整条链之后应当得到**径向对称、单调衰减**的光晕 —— 那就是这条
链的 PSF。

判据这样选是因为降采样/升采样链最典型的缺陷是**半纹素偏移**: 核的采样坐标算错
半个纹素, 每一级都把图像往同一个方向挪一点, 六级累积下来光晕整体偏离光源几个
像素。而画面上那仍然是"一团发光的东西", 没人看得出来它偏了 —— 除非拿对称性去量。

### 这一天的两次判据返工

**第一次: 对称性判据量错了对象。** 最初比"距中心等距的两点亮度是否相等", 结果
在一个正确的实现上报出 22.7% 的不对称。量到的其实是光源自己的硬边缘, 加上我
自己把中心取整到像素 (真实中心在 x.5)。换成"平台质心与光晕质心的差"之后才是
在量 PSF 本身。

**第二次: 光源太大, 判据整个失效。** 第一版方块 0.4 单位 (半分辨率下约 60
像素)。一个大光源的"泛光"主要由它自己的形状决定, 而不是由核决定 —— 实测把
降采样退化成单点采样 (完全不模糊)、或者整条升采样链跳过, 光晕仍然有 30 像素
宽, **7 条变异里逃掉 4 条**。缩到 0.03 单位 (约 4 像素) 之后核的贡献才占主导,
7/7 全部被抓住。

这两次是同一件事的两面: **判据必须量到被测的那个量, 而不是量到一个恰好也会
变化的量。**

### 已知未覆盖

升采样的偏移用**目标 mip** 的纹素尺寸而不是源 mip 的。写反了核的半径会差一倍,
但 --bloom-check 抓不到 —— 实测两种写法的核外能量占比是 0.391 对 0.3435, 差异
在判据容差之内。要抓它需要一条"逐级 PSF 半径必须成比例"的判据, 而那要回读六
张中间纹理。记在这里, 是因为"检查通过"不等于"这一行可以随便改"。

GPU 开销 0.095 ms (1280×720, 6 级半分辨率链)。默认关闭。


## Day 9 — 聚光灯阴影图集

4096² 的深度图集切成 8x8 共 64 块, 每盏投影聚光灯占一块。单次渲染通道, 逐块
换视口。

### 为什么先写一个纯 CPU 的头文件

`FShadowAtlas.h` 里只有分块与矩阵, 没有任何 GPU 调用。这两件事放在一起是因为
它们的错误方式相同: **不崩、不报错、只是阴影落在错误的位置**。而阴影落错位置
在画面上极难与"阴影偏移参数没调好"区分开 —— 后者是人人都会先怀疑的方向, 于是
真正的原因可以藏很久。

抽到纯 CPU 之后它们可以被数值断言钉死。10 条用例, 10 条变异全部被抓住。其中
两条值得单说:

**分块顺序必须逐值钉死。** "块下标到矩形是个双射"这一条, 行优先与列优先都满足
—— 而计算侧写块与着色侧读块用的必须是同一个约定。不一致的表现是"这盏灯的阴影
形状完全不对", 而不是"没有阴影"。

**竖直朝下的聚光灯。** 用 (0,1,0) 作上向量时叉积为零, 视图矩阵整个塌掉。而
"灯朝正下方"恰恰是最常见的摆法 (路灯、射灯), 不是边角情形。

### 三个不显然的实现决定

**UV 变换随矩阵一起上传, 而不是让着色器按块下标现算。** 现算意味着着色器要
复制一份图集布局的知识, 而那份复制品无法被单元测试覆盖。上传之后布局只有一处
定义, 而那一处已经被用例钉死。代价是每块多 16 字节, 64 块共 1 KiB。

**阴影矩阵预乘进 push constant 的 model。** `depth_only.vert` 算的是
`proj*view*model*pos`, set 0 里放单位阵、model 传 (阴影矩阵 x 物体变换), 结果
完全相同。否则每块每帧一份 UBO, 3 帧 x 64 块 = 192 个描述符集, 全为了搬同一
个矩阵。

**块的分配在 FLightManager 打包光源时做, 不在图集 Pass 里做。** 块下标要同时
写进光源数据 (给片段着色器) 与阴影数据 (给图集 Pass), 两处各分一次必然有漂移
的可能。

### 深度偏移: 一个不能照抄的量

级联那套的偏移量加在 NDC 深度上。透视投影下不能照抄:

    d(ndc)/dd = near*far / ((far-near) * d^2)

近远平面 0.05/30 时, 在 d=6 处只有 0.0014 每单位。**照搬级联的 0.0015 等于把
采样点沿光线推开一整个世界单位**, 阴影整片脱离物体 —— 而画面上那看起来像"这
盏灯根本没有阴影"。

改成世界单位、沿光照方向挪之后, 这个数与近远平面、与光源 range 都无关。

沿光线挪还有一个不显然的好处: **它完全不会移动影子的边界。** 点光源的阴影测试
是"从灯出发经过该点的射线撞不撞得到遮挡物", 沿那条射线挪, 挪完还在同一条射线
上。代入相似三角形算一遍, 偏移量在分子分母里正好约掉。实测把偏移放大十倍, 量
到的边界一动不动 (误差仍是 0.013)。

反过来说, 边界判据**抓不到**"深度偏移调错"这一类。抓得到的是法线偏移过大 (那
是沿法线挪, 会真的移动边界) 和偏移大到把采样点推过遮挡物 (那时影子整片消失)。

### 验收: 相似三角形

灯在 (0,0,6), 薄板在 z=3, 墙在 z=0。影子边界由相似三角形唯一确定为
`板半宽 * 6/(6-3)`。实测 [-1.804, 1.805] 对解析 ±1.8182, 差 0.8% —— 而那 0.8%
有确切来源: 法线偏移把接收点推离墙面 0.027 单位。

**容差是量出来的, 不是猜的。** 下界 0.014 (实测), 上界 0.098 (法线偏移大十倍
时的位移, 那是必须报错的 peter-panning), 取 4% = 0.073 卡在中间。

两个场景设计上的约束:

**灯必须在相机轴上。** 遮挡物在画面上会挡住一部分它自己的影子。灯离板子比相机
近, 放大率更大 —— 只要两者同轴, 影子就一定落在遮挡物的像之外。灯偏离相机轴的
话这个保证就没了。

**板子偏离灯轴放。** 影子的上下两条边到灯轴的距离因此不等。对称的话, 阴影贴图
上下翻转这种缺陷完全看不出来。

**两盏灯而非一盏。** 只有一块的话, "块偏移算错"会退化成"偏到了图集的空白区",
而空白区深度是 1.0 恰好判为无遮挡 —— 与"这盏灯没有影子"在画面上一模一样。

变异验证 13/13。

### 一个自己写下、当天补上的盲点

图集 Pass 的 `SetScissor` 那一行: 最初的注释说"视口不裁, 要靠裁剪矩形", 那是
错的 —— 裁剪体早把 NDC 之外的三角形裁掉了。实测把那行注释掉, 自检照样通过。

真正的理由是动态状态: 声明了 `EDynamicState::Scissor` 却不设, 沿用的是上一个
Pass 留下的矩形 (通常是交换链大小)。图集是 4096², 交换链常见 1280x720, 于是
横坐标超过 1280 的块会被整块裁掉。

补法是 `--shadow-lights N`: 用填充灯把被测的两盏顶到第 62/63 块 (纹素 x 是
3072 与 3584)。同一个变异在那个规模下立刻被抓住 ——

    不设裁剪矩形 → 2 盏灯退出码 0 / 64 盏灯退出码 14

两个规模都进了 verify.ps1, 因为它们抓的不是同一类东西。

### 代价

GPU 图集 Pass (同一堵墙 + 一块薄板):

    2 块 0.027 ms | 16 块 0.068 | 32 块 0.115 | 64 块 0.211

CPU 录制:

    2 块 0.044 ms | 16 块 0.122 | 32 块 0.214 | 64 块 0.397

近似线性, 每块约 3 微秒 GPU / 6 微秒 CPU。**CPU 是大头** —— 逐块重扫一遍投射体
列表, 而每块只有视锥剔除的结果不同。下一步该走 GPU 驱动的间接绘制。

### 这一天翻出来的两个旧问题

**FLight 的移动构造是手写逐字段搬的。** 新加的 `m_CastsShadow` 漏了一处, 表现
是 `AddLight` (按值移入数组) 之后所有光源都不投影, 而调用方明明设过。不报错,
查了一圈阴影图集才回到这里。已在声明处写明这个坑。

**三个套件注册了整整一周期, 一次都没跑过。** ClusterGrid、Octahedral、
CompressedTextureFormat 在 `LimxEngineTests` 里注册着, 但 `verify.ps1` 与
`ci.yml` 都是**逐个套件名手写的** —— 加了套件没人补那一行。而"没执行"与"全过"
在退出码上完全一样; `--list` 那一步只列举不执行, 所以它也发现不了。

补法是给测试运行器加 `--min-cases N`: 执行的用例少于 N 就返回 1。判据放在
**二进制里**而不是脚本里 —— 脚本要判断就得解析输出文本, 而解析失败会退化成
"没发现问题", 又一个落在"通过"上的失效。

同时 `verify-shader-layout` 扩到 storage buffer, 比的是**元素步长**而非块大小
(运行期定长数组的块大小是 0)。新覆盖三处, 其中 `light_cull.comp` 此前完全在
检查范围之外 —— 脚本只 glob `*.vert` 与 `*.frag`。

verify.ps1 从 33 步涨到 39 步。


## Day 10 — GPU 驱动的剔除与间接绘制

Day 1 那次逐 Pass 计时给出的结论一直没变: **整条渲染是 CPU 受限的**。这一天
把相机通道的逐物体录制搬到了显卡上。

计算着色器做视锥剔除并写出间接命令, 图形通道按"同一对顶点/索引缓冲区"分组,
每组一次 DrawIndexedIndirect。压力场景 576 个物体分成 24 组。

### 实测 (压力场景 24×24)

    帧耗时      2.683 → 2.172 ms   (373 → 460 fps)
    深度预通道  0.342 → 0.124 ms   录制, 2.75 倍
    前向通道    0.376 → 0.111 ms   录制, 3.39 倍
    剔除通道    0.016 → 0.060 ms   (逐物体数据上传)
    三者合计    0.733 → 0.295 ms
    GPU 整帧    0.313 → 0.319 ms   (+0.006, 剔除的分派)

阴影通道**没动**, 仍是 1.38 ms —— 现在它占剩余录制开销的 77%。它需要逐视图
(三级级联 + 每块图集) 各一套间接命令, 是另一件事。

### 四个不显然的决定

**逐物体数据从 push constant 搬进 storage buffer, 而且两条路径都用它。**
顶点着色器只有一条代码路径; 逐物体绘制那条只是把物体下标经 firstInstance
传进去 (gl_InstanceIndex 就是 firstInstance 加实例号)。这样逐像素比对才有
意义 —— 比出来的差异只可能来自剔除与命令下发, 不会混进"两份着色器本来就
不一样"。分簇光照那天是同一个理由。

搬家本身还有个硬性理由: **间接绘制根本没有逐 draw 推送 push constant 这回
事** —— 一次 DrawIndexedIndirect 覆盖几百个物体, 而 push constant 在整次
调用里是常量。

**命令不压实。** 计算着色器为每个物体都写一条命令, 不可见的 instanceCount
写 0。压实 (只写可见的, 用原子分配位置) 会把不同组的命令混在一起, 而分组
要知道自己那段从第几条开始。零实例命令在命令处理器里就被跳过, 实测量不出
开销。

**包围球外接而非内切。** 外接球包住整个 AABB, 于是"球在视锥外"蕴含"盒在
视锥外"—— GPU 保留的一定是 CPU 保留的超集 (实测 576 个里 GPU 判可见 376,
CPU 366)。多画的那些在视锥外, 会被裁掉, 一个像素都不变。反过来 (内切) 就是
GPU 剔掉了 CPU 保留的物体, 画面上少东西 —— 而那看着像资源没加载。

**drawIndirectFirstInstance 必须查询, 不支持要明确报错。** 不支持时
firstInstance 恒为 0, 整个场景挤在 0 号物体的变换上 —— 不崩, 只有开着验证层
才报。悄悄退回逐物体绘制的表现是"开了开关却没变快", 而那会导出"GPU 驱动
没用"这个完全错误的结论。

### 验收

`--gpu-driven-check`: 两条路径逐像素比对 (2764800 个通道, 最大差异 0/255),
外加四条防"判据失效"的判据 ——

  1. 可见数必须真的**少于**总数。一个什么都不做的剔除实现会给出完全正确的
     画面, 那时这条判据无从判定。
  2. 可见数必须**不为零**。第一版就栽在这里: 计数器的回读隔着并行帧数, 只
     渲一帧读的是另一个帧下标上一轮写的值 —— 读出 0 而画面完全正确。若判据
     当初只写成"不能等于总数", 这个错误会一直留着。
  3. 分组必须**连续覆盖**整个列表, 且组内每个物体的绑定状态与组一致。
  4. 组数必须真的**少于**物体数。分组没起作用时下发次数与逐物体一样多,
     而画面依然完全正确。

变异验证 10/10。

### 两条"变异根本不是缺陷"

过程里有两条变异改完画面完全正常, 而那本身是发现:

  只去掉分组条件里的**顶点缓冲区**一项 —— 每个网格自带一对顶点/索引缓冲区,
  索引那一条照样把组切开。
  只去掉**单双面**一项 —— FBatchStateLess 的首要排序键就是单双面, 分组最多
  跨过那一个边界, 而边界两侧的网格恰好不同。

要构造出真缺陷得把整个分组条件拿掉 (那时判据立刻报错)。

四项一个都没删。它们的冗余是**排序器当前键顺序的副产品**, 而分组不该依赖
那个顺序 —— 排序换个写法, 冗余立刻消失。这一条写进了代码注释, 因为下一个人
看到"这三项好像都没用"时需要知道为什么它们还在。

### 又一个子串匹配的坑

`verify-shader-layout` 判断"这个着色器在用 FModelPushConstant"用的是源码里
有没有 `materialIndex`。pbr.vert 与 gbuffer.vert 删掉 push constant 之后新增
了一个叫 `fragMaterialIndex` 的 flat varying —— **子串照样命中**, 于是脚本去
比一个根本不存在的 push constant, 报"是 (空) 字节"。

这与那个文件顶上自己写的那条注释是同一个教训: 标记是文本搜索, 而变量名也是
文本。改成匹配声明本身, 且必须含 materialIndex 成员 —— 两个条件缺一不可,
只匹配块名的话遗留的 triangle.vert 会被算进来 (它有个 64 字节的同名块)。

### 场景也可能让判据失效

压力场景原本全是单面材质, 于是"分组漏看单双面"这条变异**无从判定**。改成一半
双面之后, 自检会报源列表里双面物体的数量, 单双面没混合就直接判失败 ——
通过与"没东西可判"必须分得开。

这与 Day 9 那条推论是同一件事: 变异验证的覆盖边界不只在判据上, 也在**场景**
上。


## Day 11 — 多视图 GPU 驱动 (级联阴影)

Day 10 只覆盖了相机通道, 阴影通道 1.38 ms 占剩余录制的 77%。这一天把剔除扩成
多视图: 相机是视图 0, 三级级联是视图 1~3, 各占间接命令缓冲区里**固定长度**的
一段。

### 实测 (压力场景 24×24)

    帧耗时      2.285 → 1.030 ms   (438 → 971 fps)
    阴影通道    0.994 → 0.303 ms   录制, 3.28 倍
    深度预通道  0.329 → 0.111 ms   录制, 2.97 倍
    前向通道    0.373 → 0.114 ms   录制, 3.28 倍
    剔除通道    0.044 → 0.084 ms
    录制合计    1.879 → 0.720 ms   2.61 倍
    GPU 整帧    0.506 → 0.495 ms

从 Day 10 开始算 (那时 3.481 ms), 整条链快了 **3.38 倍**。

### push constant 换了含义

原来装的是逐物体的 model 矩阵 (mat4 + 材质下标)。model 逐物体, 搬进了 set 3
的 storage buffer; 而剩下的逐**通道**量正是视图矩阵 —— 阴影的每一级级联、图集
的每一块各一个。

push constant 恰好是这个粒度: 一次 DrawIndexedIndirect 覆盖几百个物体, 而它在
整次调用里是常量。

换完消掉了两样东西: 阴影通道每帧每级一份 UBO 加一个描述符集 (3 帧 × 3 级 =
9 套) 整个删掉; 图集通道把块矩阵**预乘进 model** 的技巧不再需要 —— 那本是逐
物体数据还在 push constant 里时的权宜之计。

### 三件踩出来的事

**逐物体缓冲区必须分段。** 相机通道的逐物体路径读 RenderObjects (已剔除),
阴影通道读 ShadowCasterObjects (未剔除) —— 两份列表的下标毫无对应关系, 而且
它们各自排序, 排序不稳定, 比较相等的物体先后可以不同。

先前只上传一份, 表现是**某一盏聚光灯的阴影整个消失**, 另一盏却完全正确 (那个
场景里两份列表恰好只在那一处不同)。不崩、不报错 —— 又一次落在"阴影落错位置"
这个高度趋同的表现上。

现在分三段: 相机列表 / 投射体列表 / 半透明, 各段的起点由剔除通道给出。

**剔除通道的 Order 从 90 改到 10。** 90 是"深度预通道之前", 在只有相机通道用
间接绘制时看着够用。级联阴影接上之后就错了: 阴影通道在 50, 它录下的
DrawIndexedIndirect 在 GPU 上**先于**剔除的计算着色器执行, 读到的是上一次写
进那一帧下标的命令。

静止场景里那些值恰好相同, 所以画面看不出问题 —— 直到 --gpu-driven-check 的
第一帧: 之前所有帧都关着 GPU 驱动, 那一段命令从来没被写过。判据立刻报出
81675 个通道不一致。

教训: **生产者要排在每一个消费者之前, 而不是当时想得起来的那几个之前。**

**压力场景没有地面。** 级联阴影落在虚空里 —— 阴影贴图照画, 画面一个像素都不
受影响, 于是逐像素判据对整条阴影路径完全无从判定。

加地面之前, "阴影通道用相机的视图下标""三级级联全用第 0 级的视锥""逐物体路径
不加段起点"这三条变异**一条都抓不住**。这与 Day 10 那条 (压力场景全是单面
材质) 是同一件事: 判据的覆盖边界不只在判据上, 也在场景上。

### 一条量不出来的

剔除通道每帧写满全部 8 个视图段 —— 本帧用不到的那些用一个"全拒"视锥写成
instanceCount = 0。防的是"拿未初始化的显存当间接命令"。

实测把这道防线与阴影通道那道"本级有没有被剔除过"的判断**同时拆掉**, 两条判据
仍然全绿。因为那个场景构造不出来: 没有有效方向光时 ShadowEnabled 是 0, 片段
着色器压根不采样级联贴图, 往里画什么都看不见。

但风险是实在的, 只是不在画面上 —— 未初始化的 indexCount 可能是几十亿, 那是读
到索引缓冲区之外。**删掉它不会有任何检查变红**, 所以这段话写进了代码。

这是变异验证的第三类边界: 前两类是判据不够 (Day 9 的规模) 和场景不够 (Day 10
的单双面、Day 11 的地面), 这一类是**后果根本不在画面上**。

### 验收

变异验证 8/8 —— --gpu-driven-check 与 --shadow-check 一起跑, 不同缺陷落在不同
的判据上。逐视图可见数也报出来并断言非零: 只报相机那一个的话, 级联视图整段没
写过这种情况看不出来。

    上传 577 个物体, 相机视图判可见 377, 视图 1/2/3 分别 99 / 441 / 577

三级级联的可见数递增, 与"每级覆盖更大的体积"一致。


## Day 12 — 点光源立方体阴影

方向光有级联、聚光灯有图集, 点光源是最后一个没有阴影的光源类型。这一天补上,
用的是同一张 4096² 图集: 每盏投影点光源占**连续的六块**。

连续是着色器那边的前提 —— 它拿到第一面的块下标, 再加上由片段方向算出的面
下标。FLightData 里只有一个 float 的位置放下标, 塞不下六个。

聚光灯与点光源共用同一个 `ComputeSpotShadow`: 它只要一个"世界 → 该视图裁剪
空间"的矩阵与一块 UV, 而那对透视阴影是通用的。两者的差别只在"取哪一块"。

### 正好 90 度是不够的

六个 90 度的面在数学上恰好拼满球面。但阴影贴图不是数学:

面与面的交界处, 遮挡物恰好落在两个视锥的**边界上** —— 那条射线在两边的深度
图里都只被画到一半。再叠上 3x3 的 PCF (采样点越过块边界会被钳回块内), 交界
处就出现一条窄缝。

实测: 影子在 x=2 (正是面边界) 处断了约 0.036 世界单位, 两三个像素。而判据
报出来的是"影子外缘只到 1.965, 解析值是 4.21" —— 因为找暗区的函数遇到那条
缝就停了, 后面那一大段暗区它根本没看。

**这个数字差了一倍多, 而真正的缺陷只有两三个像素宽。** 排查时先怀疑的是
矩阵, 而矩阵是对的。分别强制用 0 号面和 5 号面各跑一次才定位到: 两个面
各自都正确, 只是接不上。

每面多留 2 度余量 (相邻两面重叠约 7%) 之后, 跨面区间 266/266 个采样点全暗。
有效分辨率降 7%, 换掉一条断缝。

### 场景必须让判据有得判

灯刻意放得离墙很近 (2 单位)。于是墙上 |x| 超过 2 的地方, "从灯指向片段"的
主轴就从 -Z 翻到 ±X —— 影子**必然**跨过面边界。

灯放远一点 (比如和聚光灯那组一样 z=6) 的话, 可见范围内根本不会跨面, 整条
选面逻辑就无从判定, 而判据照样全绿。这是 Day 10、Day 11 那条教训的第三次
应用: 判据正确不等于场景能让它区分。

### 阈值也要跟着物理走

灯离墙只有 2 单位而测量区横跨 5 单位, 于是 N·L 从 1 掉到 0.37 —— 亮处最暗
的地方比"亮暗中点"还暗, 会被判成影子。那时"两端都必须是亮的"这一条直接不
成立, 而判据报的却是"没找到完整的暗区"。

阈值改成"离环境光多远"(0.2 的比例) 就没这个问题, 而且有物理含义: 影子区
**只有环境光**, 亮区哪怕衰减到三分之一也仍然带着直接光。阈值卡在这两者
之间, 分的正是"有没有直接光"这件事, 与 N·L 掉多少无关。

### 用例

立方体面的数学 7 条用例, 变异验证 9/9。其中两条专门钉编号约定:

  面的编号整体循环移位 —— **往返测试单独是抓不住的** (循环移位后往返仍然
    成立), 要靠"六个方向两两成对相反"那一条;
  改成 +X,+Y,+Z,-X,-Y,-Z —— 同上。

C++ 的 `CubeFaceFromDirection` 与 GLSL 里那一份分处不同语言, 没有任何编译期
保障。两者的 `>=` 链要逐字对应 —— 平局 (棱上) 时选谁都对, 但两处必须选同
一个, 否则棱附近会出现一条一像素宽的错误阴影。

`--point-shadow --shadow-check` 的变异验证 9/9。

verify.ps1 从 40 步涨到 41 步。


## Day 13 — 半分辨率 GTAO + 双边上采样

Day 7 结尾记下的那句"真正的杠杆是减少采样次数 (半分辨率 + 双边上采样)"。

GPU 驱动把 CPU 从 3.48 ms 压到 1.03 ms 之后, 天平第一次倒向 GPU —— 而 GPU
那边 GTAO 一个人占 64.6% (1.128 ms / 1.745 ms)。

    GTAO       0.698 → 0.288 ms   (2.42 倍, 已含上采样那一趟)
    GPU 整帧   1.165 → 0.755 ms   (35%)

CPU 帧时没变 (1.14 ms) —— 天平又倒回 CPU 那边了。

### 质量

墙角的解析收敛判据在半分辨率下照样过, 而且逐档跟得很紧:

    z 0~0.25    R=2: 0.585694 → 0.580737   R=8: 0.557389 → 0.554984
    z 2.5~2.75  R=2: 0.978206 → 0.975964   R=8: 0.846468 → 0.845793

逐像素比对 (墙角场景): 平均差 0.014394, 最大 0.280761。

### 判据里最要紧的那一条: 差异不能为零

`--ao-half-check` 有三条判据, 而最要紧的是**平均差不能为零**。

一个把 `--gtao-half` 忽略掉的实现会得到两张**完全相同**的图, 而"差异要小"
那一条对它满分通过 —— 于是"半分辨率没生效"与"半分辨率完美无损"在判据上无法
区分。这与之前几天反复出现的形状是同一个: 失败模式落在"通过"上。

变异验证 4/4。

### 一条量出来的负面结果

想给**双边加权**单独建一条判据, 结论是建不起来。逐条量过:

  墙角场景两块平面填满视野, 深度不连续的像素**一个都没有**;
  阴影场景有 1096 个 (占万分之十二), 而把双边退化成双线性之后, 那些像素上
    的平均差从 0.005806 变成 0.006379 —— 整幅平均则是 0.037312 对 0.037313,
    小数点后五位才分得开。

另外三条变异 (上采样的近远平面传反 / 求解时传全分辨率尺寸 / 上采样传全分辨率
尺寸) 指向同一件事:

  第一条只影响 depthWeight;
  第二条是因为 gtao.frag 只拿 ScreenW/H 做两件事 —— 噪声图案的输入, 与"半径
    小于一个像素就直接返回"的守卫。地平线搜索本身在 UV 空间里做, 与分辨率
    无关;
  第三条里 halfSize 在 `fragUV*halfSize-0.5` 与 `coord/halfSize` 中出现两次
    且互相抵消, 退化成"在 fragUV 处做硬件双线性采样"。

深度加权仍然留着 —— 它便宜, 防的是物体边缘一圈光晕, 而那种光晕看起来像
"AO 半径调大了", 调半径根本治不了。但判据**只报数不判定**: 阈值宽到能放过
双线性的话, 那条判据就是摆设。

这是变异验证第三类边界 (后果不在画面上) 的一个变体: 后果**在**画面上, 但只
在万分之十二的像素上, 而且幅度只有 0.0006 —— 任何整幅统计量都盖不住它。

### 容差与场景绑定

平均差的容差 0.025 是从**墙角场景**量出来的 (正确实现 0.014394)。同一个正确
实现在阴影场景上是 0.0373 —— 那里 AO 的梯度陡得多。

所以这条判据只在墙角场景上跑, verify.ps1 里也是这么调的, 而代码注释里写明了
这个数与场景绑定。不写明的话, 下一个人在别的场景上跑它会得到一个莫名其妙的
失败, 然后把阈值调宽 —— 那时判据就废了。

### 顺带

共享深度纹理加了 `TransferSrc`, 好让自检能按深度不连续挑像素。不放在调试开关
后面 —— **"判据只在调试构建里有效"等于没有判据**。一个用途位不占显存, 也不改
布局转换的语义。

verify.ps1 从 41 步涨到 43 步。


## Day 14 — 综合示例场景

到 Day 13 为止, 每条判据各用一个**最小场景**: 墙角只有两块平面, 泛光只有一个
方块, 阴影只有一堵墙加一块板。那是刻意的 —— 多一样东西, 解析判据就不再成立。

但那也留下一个空白: **没有任何一个场景同时跑全部子系统**。而子系统之间会互相
影响 —— 分簇决定哪些光参与着色, 阴影图集的块下标存在光源数据里, GPU 驱动的
逐物体缓冲区被四个通道共用, TAA 的历史依赖速度矢量而速度矢量来自深度预通道。
任何一处对不上, 单独的最小场景都发现不了。

`--showcase`: 35 个节点, 三种光源类型各一盏 (都投影), 不透明/蒙版/半透明/自
发光四类材质, 四根柱子**故意**放在相机背后。

### 判据换了一种问法

前面所有的判据问的都是"这个数对不对"。这一条问的是"**这个子系统到底跑没跑**"。

    分簇       索引表 30814 条
    GPU 驱动   33 个物体, 相机视图可见 29, 3 组, 4 个视图
    阴影图集   7 块 (聚光灯 1 + 点光源 6)
    级联       三个视图的可见数都非零
    GTAO       350893 / 921600 个像素有明显遮蔽
    半透明     2 个批次
    光源       三种类型都在, 且都投影

每一条都带"够不够判"的元判据: 场景里没有对应的东西时**直接判失败**, 而不是
悄悄通过。相机背后那四根柱子就是为此而设 —— 全部物体都可见的话, 一个什么都
不做的剔除实现也会给出完全正确的画面。它们同时还验了"阴影用未经相机剔除的
列表"那条路径。

变异验证 8/8。

### 变异抓出的一个真缺陷

`GetRenderedTileCount()` 报的是**分配**的块数, 不是实际画进去的:

```
m_RenderedTileCount = static_cast<UInt32>(casters.GetSize());   // 应该画几块
```

把绘制循环改成只画第一块, 计数照样报 7, 自检满分通过。改成在循环里逐块累加
之后立刻被抓住。

**报意图而不是报事实的计数器, 与没有计数器等价。** 而 Day 9 的阴影自检也一直
在用这个数 —— 它那条"图集应当绘制 2 块"的判据同样是空的。

### 场景本身也会骗人

两次:

**顶点色。** 程序化图元自带调试用的顶点色 (立方体六面各一色, 球体按经纬映射
色相), 而 pbr.frag 算的是 `albedo = fragColor * baseColor` —— 材质的基色被
整个盖掉。第一版截图里地面是绿的、柱子红蓝相间, 我以为是材质下标串了位, 查了
一圈才发现是顶点色。综合场景把它刷白: 那些颜色对"看清一个图元的朝向"很有用,
但与这个场景的目的冲突。

**相机朝向。** 第一版 yaw 写成了 π —— 那是压力场景的写法, 它的相机在 -Z 一侧。
于是相机背对整个场景: 29 个物体只有 1 个可见, GTAO 一个像素都没遮蔽。

第二次是判据自己抓出来的。这说明"每个子系统都要留下痕迹"这条判据抓的不只是
子系统, 也包括**场景本身摆得对不对** —— 而那是写场景时最容易出错、又最不容易
自查的一件事。

### 两份图像回归基线

`demo-scene.sig` (演示场景, 一盏方向光) 与 `showcase-scene.sig` (综合场景)。

两份都要, 因为覆盖面不同: 演示场景轻, 抓基础着色的回归; 综合场景把三条阴影
路径、分簇、GPU 驱动、TAA、GTAO、泛光同时跑起来, 子系统之间的互相影响只有它
抓得住。

README 里写明了重新生成的命令**必须与 verify.ps1 里那一步逐字一致** —— 少一个
`--gtao-half` 画面就不同, 而那时人会以为是渲染回归。

verify.ps1 从 43 步涨到 45 步。


## Day 15 — 收尾与总测量

### 周期末的数字 (NVIDIA RTX 3060 / 1280x720 / Vulkan 1.4)

**综合场景** (35 个物体, 三种光源类型都投影, 全套后处理):

    帧时间     1.19 ms  (839 fps)
    CPU 合计   1.18 ms  (命令录制 0.60)
    GPU 整帧   1.07 ms  (GTAO 0.37 / 前向 0.22 / 级联阴影 0.17)
    显存       180 MiB

**压力场景** (576 个物体, 全套后处理):

    逐物体绘制   2.50 ms (400 fps)   CPU 2.50   GPU 0.92
    GPU 驱动     1.19 ms (839 fps)   CPU 1.19   GPU 0.91

### 本周期的性能演进 (压力场景, 576 物体)

| 阶段 | 帧时间 | 做了什么 |
|------|--------|----------|
| Day 9 结束 | 3.48 ms | 逐物体绘制, 逐 Pass 推送 push constant |
| Day 10 | 2.17 ms | 相机通道走间接绘制 |
| Day 11 | 1.03 ms | 三级级联阴影也走间接绘制 |
| Day 13 | — | GTAO 0.70 → 0.29 ms, GPU 整帧降 35% |

整条链 **3.4 倍**, 而画面逐像素一致 (2,764,800 个通道最大差异 0/255)。

### 这七天新增的判据

verify.ps1 从 32 步涨到 45 步:

    聚光灯阴影 (相似三角形的解析边界)      变异 13/13
    聚光灯阴影 (用满 64 块图集)            补上"忘了设裁剪矩形"的盲点
    点光源立方体阴影 (跨面不断 + 解析边界)  变异 9/9
    GPU 驱动绘制 (与逐物体绘制逐像素比对)   变异 10/10 + 多视图 8/8
    GTAO 半分辨率的解析收敛                同一条判据换分辨率再跑
    GTAO 半分辨率与全分辨率逐像素比对       变异 4/4
    综合场景 (每个子系统都留下痕迹)         变异 8/8
    图像回归 (综合场景)                    第二份基线
    引擎层测试 · ClusterGrid / Octahedral /
      CompressedTextureFormat / ShadowAtlas 注册了一个周期, 此前从未执行
    引擎层测试 · 用例数下限                 防"套件悄悄消失"

单元测试 78 → 85 条 (立方体面的数学 7 条, 变异 9/9); 分块与聚光灯矩阵 10 条,
变异 10/10。

### 这七天抓到的、失败模式落在"通过"上的缺陷

1. **三个套件注册了整整一个周期却一次都没跑过** —— verify.ps1 与 ci.yml 都是
   逐个套件名手写的, 加了套件没人补那一行。而"没执行"与"全过"在退出码上完全
   一样; `--list` 那一步只列举不执行。
2. **`GetRenderedTileCount()` 报的是分配的块数**, 不是实际画进去的。"只画第一
   块"这条变异满分通过。
3. **`FLight` 的移动构造漏搬新成员** —— 手写逐字段搬, 加 `m_CastsShadow` 时
   漏了一处。表现是按值移入数组之后所有光源都不投影, 而调用方明明设过。
4. **剔除通道的 Order 排在消费者之后** —— 静止场景里读到的旧命令恰好相同,
   画面完全正确, 直到自检的第一帧读到从未写过的显存。
5. **逐物体缓冲区只上传一份列表** —— 阴影通道按投射体列表的下标去索引, 而
   缓冲区里装的是相机列表。某一盏聚光灯的阴影整个消失, 另一盏完全正确。
6. **verify-shader-layout 的标记又栽在子串匹配上** —— `fragMaterialIndex`
   命中了 `materialIndex`, 于是脚本去比一个根本不存在的 push constant。
7. **压力场景全是单面材质 / 没有地面 / 综合场景相机朝反了** —— 三次都是场景
   让判据无从判定, 而判据本身完全正确。

### 变异验证的三类覆盖边界 (本周期补全)

| 类别 | 例子 | 有没有解法 |
|------|------|------------|
| **判据不够** | 裁剪矩形那条在两块时逃掉, 64 块才现形 | 有 —— 换规模再跑一遍 |
| **场景不够** | 单双面没混合 / 没有地面 / 没有深度不连续 | 有 —— 判据里加一条"这个场景够不够判"的元判据 |
| **后果不在画面上** | 拿未初始化的显存当间接命令 | 没有 —— 只能写下来 |

第二类的解法是这七天最实用的一条: **通过与"没东西可判"必须分得开。**
`--gpu-driven-check` 会报源列表里双面物体的数量, 单双面没混合就直接判失败;
`--point-shadow` 的判据会检查影子有没有真的跨过面边界, 没跨过就判失败。

### 留给下一周期的

**主线**:
- 点光源的立方体阴影没有接进 GPU 驱动的多视图剔除 (六个面需要六个视图,
  而 `kMaxCullViews` 现在是 8, 一盏点光就占满)。图集通道现在仍走逐物体绘制,
  实测 0.13 ms —— 不是瓶颈, 但那条路径与其余三条不一致。
- 两阶段遮挡剔除 (Hi-Z)。现在只有视锥剔除, 被挡住的物体照画。
- 阴影图集的动态分块 (按屏幕占比分配分辨率)。现在是固定 512。

**已知盲点** (代码里都写了):
- 泛光升采样的偏移用错 mip 这一条抓不住 (核外能量占比 0.391 对 0.3435, 在
  容差之内)。要抓它需要"逐级 PSF 半径成比例"的判据, 那要回读六张中间纹理。
- 双边上采样的深度加权在手上的场景里量不出来 (万分之十二的像素, 幅度 0.0006)。
- "拿未初始化的显存当间接命令"没有任何画面判据抓得住。

**副线**:
- BC7 的 mode 0/2/4/5 编码器未实现 (解码器八种全支持)。
- CI 只有 CPU 侧, GPU 自检全部靠本地。

---

## 第五个周期结束时的状态

**渲染管线**: GPU 驱动的多视图剔除与间接绘制 / 分簇前向光照 / 三种光源类型的
阴影 (级联、图集、立方体) / TAA / 半分辨率 GTAO / 泛光 / IBL / 薄 G-Buffer /
bindless 材质 / 并行命令录制。

**验证**: 45 步全绿, 749 个单元测试, 两份图像回归基线。**每一条检查都做过变异
验证** —— 先把被检查的东西改错, 确认它真的会红。

**性能**: 综合场景 1.19 ms (839 fps), 显存 180 MiB。

**这个周期真正学到的一件事**: 判据的价值不在它有多严, 而在**它失败时能不能
被看见**。这七天写的每一条判据都先被故意弄红过一次; 而抓到的十来个缺陷里,
没有一个是靠"看画面"发现的 —— 它们全都不崩、不报错, 只是悄悄地不对。


# 第六周期 — 硬件光线追踪与虚拟几何

## Day 1 — 光追 RHI 基础

开工时 RHI 里关于光追的只有五个枚举占位 (`EDescriptorType::AccelerationStructure`
一类), **加速结构 API 一行都没有**, 设备也只启用了 swapchain 一个扩展。

### 装起来的东西

    设备扩展   acceleration_structure + ray_query + deferred_host_operations
    特性链     AccelerationStructureFeaturesKHR / RayQueryFeaturesKHR
    显存       所有分配带上 DEVICE_ADDRESS 标志
    句柄       FRHIAccelStructHandle + TVulkanResourcePool
    RHI 接口   CreateBottomLevelAS / CreateTopLevelAS / UpdateTlasInstances /
               DestroyAccelStruct / GetAccelStructDeviceAddress /
               GetBufferDeviceAddress
    命令接口   BuildAccelStruct / AccelStructBarrier
    描述符     UpdateDescriptorSets 支持加速结构 (走 pNext)
    着色器     ray_query_test.comp (GL_EXT_ray_query)

实测: `扩展齐备:true 加速结构特性:true rayQuery 特性:true 设备地址:true → 启用`,
暂存对齐 128, 最大实例 16777215 (RTX 3060 / Vulkan 1.4.325)。

### 三处"失败会落在通过上"的地方, 开工就先堵掉

**1. `IsRayTracingSupported()` 只查扩展不查特性。**

原来的实现每次调用都重新枚举一遍设备扩展, 而且只看扩展在不在。驱动完全可以
报告扩展却把特性位关掉 (虚拟化与软件实现里很常见) —— 那时这个函数报"支持",
而任何一次加速结构调用都会失败, 报的却是别的错。现在它读的是初始化时定下的
那个值, 与"设备创建时到底启用了没有"是同一个事实。

**2. 扩展名做前缀匹配。**

顺手写的字符串比较只判"目标名走完了", 于是 `VK_KHR_ray_query` 会命中一个叫
`VK_KHR_ray_query_something` 的扩展。这个项目在着色器布局检查上已经栽过一次
同样的跟头 (`fragMaterialIndex` 命中 `materialIndex`), 所以两个终止条件都写上。

**3. `UpdateDescriptorSets` 的 `default: break;`。**

加一种描述符类型却忘了在 switch 里加分支时, 这一条写入会带着空载荷交给驱动:
描述符保持旧内容, 而着色器照读不误。改成记 Error。

顺带修掉一个同类的: 描述符池的 `poolSizeCount` 是写死的 11, 与旁边那个 11 项的
数组各改各的。

### 判据

`--rt-check`: 建一个 BLAS (一个方片) + 一个 TLAS (五个实例), 发 496 条射线,
把 GPU 遍历出的**命中与否 / 命中距离 / 实例自定义下标 / 图元下标**逐条与 CPU
的 Moller-Trumbore 解析解比对。

两套实现连算法都不是一回事 —— 一边是驱动的 BVH, 一边是引擎自己的逐三角形
求交 —— 所以不存在"照着对方调到一致"这条路。

    射线 496 条 — 命中 130 落空 366
    实例命中 42/34/22/0/32
    遮挡区 42 条  旋转实例 32 条
    最大距离误差 0.000002 (容差 0.0001)
    不一致 0 条

最大误差 2e-6, t 最大约 13, 即相对误差 1.5e-7 —— 单精度的极限。

### 场景是照着"能不能分辨对错"设计的

五个实例, 每一个都有存在的理由:

| 实例 | 摆位 | 为什么要有它 |
|------|------|--------------|
| 0 | z=2 | 基本命中 |
| 1 | x=3, z=5 | 不同射线打到不同实例 |
| 2 | x=-3, z=9 | 自定义下标不能靠"顺序恰好一致"蒙对 |
| 3 | z=6, 与 0 同轴 | **遮挡** —— 它一次都不该被命中 |
| 4 | x=6, 绕 Y 转 45 度 | 非单位变换 |

射线也分两组: 沿 +Z 的平行射线 (轴向是 BVH 遍历的退化情形, 方向分量上的符号
错误在这里可能刚好抵消) 与从一点发散的扇形 (方向每个分量都非零)。

元判据: 命中数为 0 / 落空数为 0 / 被命中的实例不是 4 个 / 实例 3 被命中过 /
没有射线经过遮挡区 / 旋转实例一次没被命中 —— 任何一条成立就直接判失败。
**通过与"没东西可判"必须分得开。**

设备不支持光追时判**失败**而不是跳过。判通过的话它在任何不支持的机器上都是
空的, 而换一台机器正是"不支持"最常见的来源。

### 变异验证 13/16, 三条逃逸全部查清成因

先跑 14 条, 抓住 11 条。三条逃逸里有一条是**判据不够**, 补上之后被抓住:

**"BLAS 图元数漏除以 3"逃掉了**, 因为索引缓冲区之后是零 —— 多读出来的索引
全是 0, 构成退化三角形, 一条射线都碰不到。而真实引擎里一个网格的索引后面紧跟
的是**另一个网格的索引**, 越界读到的是能被命中的几何体。

所以把场景改成真实的样子: 索引表放 18 个索引而只声明用前 6 个, 后 12 个指向
一组摆在**更近 0.5** 处的哨兵顶点。正确实现只建 2 个三角形, 看不到它们
(加了哨兵之后基线的 130 命中与 2e-6 误差一个数都没变); 读越界的实现会把它们
也建进去, 命中距离整体前移 0.5 —— 容差的五千倍。

补上之后 16 条抓住 13 条。剩下两条**推到极端之后确认是驱动上的恒等变换**:

| 逃逸 | 怎么查的 | 结论 |
|------|----------|------|
| `maxVertex` 报错 | 推到 0 和推到 1000000 | 两次结果与基线**逐字节相同** (130 命中 / 同样的实例分布 / 同样的 2e-6)。NVIDIA 在设备侧构建时根本不读这个字段。规范仍要求填对 (别的厂商可能用它做边界), 但外部判据看不见 |
| 取值时传 `false` 而非 `true` | 把**类型判定**也改成取候选 | 类型判定那条**被抓住了** (366 条不一致)。遍历结束后已无候选记录, 驱动对取值函数返回的仍是已提交的那条 —— 逃逸只限于一个冗余参数, 逻辑本身验到了 |

被抓住的 13 条覆盖: 不建 BLAS / 不建 TLAS / 图元数算错 / 实例变换转置 /
自定义下标丢失 / 可见性掩码为 0 / 顶点跨度错 / 暂存地址不对齐 / 少一道屏障 /
描述符不写 pNext / 着色器不遍历就取结果 / 取错查询函数 / 射线方向取反。

"暂存地址不做对齐取整"这一条被抓住值得单说 —— 规范要求它是
`minAccelerationStructureScratchOffsetAlignment` (这台机器上 128) 的整数倍,
不对齐时行为未定义。这类"未定义"常常表现为偶尔不对, 而这里它稳定地红了。

verify.ps1 从 45 步涨到 46 步。


## Day 2 — 加速结构接上真实场景, 以及一条藏了一个周期的缺陷

### 装起来的东西

`FRayTracingScene`: 逐渲染对象一棵 BLAS, 全场景一个 TLAS, 每帧刷新实例变换。
"要不要重建 BLAS"由它自己按几何签名判 (FNV-1a 混入缓冲区句柄的索引与代、
索引区间、顶点数与跨度; **不含变换** —— 物体移动只需重建 TLAS)。

交给调用方判的话, 它必须记住"换了网格要重建、只是移动了不用"。判错的那一半
的表现是: 光追里的形状还是上一个场景的, 位置却是新场景的 —— 画面照常, 只有
反射与光追阴影不对。

TLAS 实例按混合模式分掩码 (不透明 0x01 / 蒙版 0x02 / 半透明 0x04)。分类依据是
**这个物体在光栅化里写不写深度**: 半透明不写, 把它按不透明算进遮挡的话, 光追
说"被玻璃挡住", 深度缓冲区说"看得到玻璃后面", 两边永远对不上。

### 判据: 光追深度 vs 光栅化深度

上一条 (`--rt-check`) 验的是"加速结构这套机制对不对", 用的是手搭的方片。
这一条验的是**加速结构里装的是不是屏幕上那个场景**。

    综合场景  33 物体 + 天空 + 蒙版   919234 像素, 不符 19 个 (0.0021%)
    OBJ 场景  6 个子网格共用缓冲区    198364 像素, 不符 18 个 (0.0091%)
    墙角/阴影 几何体填满整屏          921600 像素, 不符 0 个

两个场景缺一不可: 只有 OBJ 场景能验到"子网格的索引偏移" (别的场景
IndexOffset 全是 0, 乘不乘位宽都一样); 只有综合场景有蒙版材质。

**容差按深度缓冲区的可表示精度算, 不用固定的相对数。** 透视深度在远处压缩得
极厉害: 同一个 float32 最低位, 在 near=0.1 的场景代表 5.5e-5 世界单位, 在
near=0.01 的场景代表 5.5e-4 —— 差十倍。固定相对容差等于对不同场景用了完全
不同的严格程度。改成"不超过 N 个最低位"之后判据与近远平面无关, 而无轮廓的
场景实测只差 1.4 与 1.9 个最低位 —— 判据的分辨率就是深度缓冲区本身的分辨率。

变异验证 12/13。唯一逃逸是半透明的掩码分支今天不可达 (加速结构从阴影投射体
列表建, 那份列表按定义不含半透明)。

### 抓到的三条缺陷

**1. 前向通道的深度 StoreOp 是 DontCare。**

DontCare 的语义不是"不需要写回", 而是"通道结束后这个附件的内容**未定义**"。
而未定义在实际硬件上不是"保持原样": 深度附件是压缩存储的, 只被清除过、从未
被绘制覆盖过的区域并不真的在显存里, 它只是一份"这整块都等于清除值"的元数据。
DontCare 让驱动可以把它丢掉, 于是那些区域回读出来是 0 而不是 1.0。

症状极隐蔽: 被几何体覆盖过的像素完全正确, 只有背景是错的; 画面本身一点问题
都没有 (帧内没有任何通道在此之后读深度); 几何体填满整屏的场景一切正常。而
此前所有用到深度回读的判据**恰好都跑在那类场景上**。

排查是逐条排除法: 缓冲区通路 (计算着色器写已知斜坡, 921600 格全对) → 采样
通路 (同一条路读 HDR 颜色是正常图像) → 读取方式 (图像拷贝与 texelFetch 读出
同一份) → 布局解压 (General 与 ShaderReadOnly 逐字节相同) → 显存重叠 (逐资源
打印块与偏移) → 并行录制 → 帧数 (1/8/30 帧完全一致)。全部排除之后才落到
StoreOp 上。

修好之后综合场景从 91.7% 不符降到 0.0021%。

**2. `gtao_upsample.frag` 的 LinearizeDepth 分母符号写反。**

它把 0.1..100 的距离映到 -0.1..-0.05 —— 负数, 而且随距离减小。于是
`max(max(a,b), 1e-4)` 永远取到 1e-4, 相对差被放大四个量级,
`exp(-relative/0.05)` 直接下溢成零。**双边上采样一直在做最近邻。**

上个周期"去掉双边加权"那条变异之所以逃逸, 就是因为当时根本没有双边加权。

**3. `FMatrix::Inverse()` 一个单元测试都没有** —— 而它是屏幕空间反投影的
地基。补了 5 条, 其中"世界点投到 NDC 再反投影回来必须回到原处"是必要的:
只验 `M * M^-1 ≈ I` 放得过转置了的逆。Core 406 -> 411 条。

### 一条判据被撤下来又放回去

`--ao-edge-check` 是 Day 2 早些时候加的, 当天就被我自己撤了 —— 它挑"深度不
连续像素"用的正是那份坏掉的深度回读, 挑出来的 157095 个大多是未初始化显存与
有效数据的边界。它确实能稳定分开纯双线性与双边, 但**不是因为它名字说的那个
理由**, 那就不算判据。

StoreOp 修好之后真实数字是 7236 个, 在这一组上重测:

    双边 (正确)          渗色  919   不连续处平均差 0.0947
    退化成最近邻          渗色  918   平均差 0.0947   判不出来
    去掉双线性因子        渗色  905   平均差 0.0950   判不出来
    纯双线性             渗色 1361   平均差 0.1201   抓得住
    加权反向             渗色 1801   平均差 0.1675   抓得住

阈值按真实数据重设为 1100, 判据放回流水线。已知盲点 (分得开"太糊", 分不开
"太锐") 与全部实测数据都写在代码里。

### 这一天真正的教训

三条缺陷是同一件事的三个面: **判据只在能看见后果的场景上跑过。**

- 深度回读的判据只跑在几何体填满整屏的场景上 —— 背景永远看不见。
- 双边加权的判据只跑在没有深度不连续的墙角场景上 —— 加权永远不起作用。
- 反投影没有任何判据 —— 逆矩阵错了也没人知道。

而它们能被抓到, 靠的是**一条把整幅画面逐像素与另一条独立路径比对的判据**。
它没有"挑一小块看"的余地, 于是那些藏在画面某个角落、某类场景、某个从未被
读过的字段里的东西, 一次全暴露了。

verify.ps1 45 -> 49 步。


## Day 3 — 光追阴影

`FRayTracedShadowPass` (order 120, 深度预通道之后、天空与前向之前): 逐像素从
深度缓冲区还原世界坐标, 向指定光源发一条阴影射线, 输出一张全分辨率的 R8
可见度掩码。射线用 `TerminateOnFirstHit` —— 阴影只关心"有没有挡住", 不关心
"最近的是哪个"。

掩码接到 set 2 binding 11, 着色阶段按 UBO 里的"哪一盏灯走光追"决定用它还是
用阴影贴图。一次只有一盏 (掩码单通道), 而这一点由 CPU 侧写进 UBO, 不让着色器
去猜 —— 猜错的表现是某盏灯用了另一盏灯的可见度, 那是"有影子、位置完全不对"。

### 这个通道刻意没有效果参数

阴影贴图那一套里的深度偏置、法线偏置、PCF 半径、级联过渡宽度, 每一个都是在
补离散化的窟窿。光追这里唯一的容差是盖住深度缓冲区自身的量化误差, 而那个量
算得出来: 世界坐标是从深度反投影来的, 实测 near=0.1 的场景上 10 米处约
5.5e-5 世界单位, 默认取 1e-3 是近二十倍余量。

留下可调旋钮的代价不是多一行代码, 是从此没人知道"对"是什么。

### 数字

判据与 `--shadow-check` 用同一条扫描线、同一组解析常量, 差别只在输入 ——
于是量出来的差就只能是阴影本身的差, 不是测量方法的差。

    一个像素 = 0.00487 世界单位

    阴影贴图  边界 [-1.803991, 1.805238]  误差 0.01419  / 0.01294
    光追掩码  边界 [-1.819707, 1.816273]  误差 0.00153  / 0.00191
    光追着色  边界 [-1.817366, 1.818614]  误差 0.000814 / 0.000432
    解析值         [-1.818181, 1.818181]

**着色后的画面上, 光追准十七到三十倍** —— 六分之一个像素, 而阴影贴图是三个。

开销 (RTX 3060, 1280x720, 综合场景, 单光源全分辨率无降噪):

    整帧      1.175 -> 1.422 ms   (+21%)
    该通道    0.661 ms GPU        (整帧 GPU 的 36.7%)

### 判据的四条

前三条验掩码本身: 边界落在解析位置上 / 掩码必须是二值的 / 影子宽度对得上。
第四条抓一帧**着色后的画面**, 用同一条扫描线量同一个边界。

第四条是必需的: 掩码算得再对, 只要它没被着色阶段读到, 画面上的影子仍然是
阴影贴图那一版 —— 而前三条会全部满分通过。加上它之后, "着色阶段不读掩码"
与"UBO 里的光源下标恒为 -1"两条变异都被抓住。

### 变异验证 10/12, 两条逃逸都查清了

**tMin 从 1e-3 放大到 0.1 —— 逃逸, 而且这是理论上就该逃的。**

上个周期从相似三角形推过: 沿光线方向的偏置不会移动影子边界, 起点沿射线推进
多少完全抵消。这次由判据独立验到了。

对照组: **法线偏移**从 1e-3 放大到 0.1 **被抓住**。

两条放在一起, 判据恰好分开了"影响边界的偏置"与"不影响的偏置" —— 而那正是
阴影偏置这件事的核心。tMin 过大真正的危害是接触阴影漏光 (遮挡物贴着接收面
时), 要验它需要一个遮挡物离墙不到 0.1 的场景。

**tMax 用光源衰减距离而不是到光源的距离 —— 逃逸。** 危险在于"光源背后的
几何体也来投影", 而阴影场景里灯背后什么都没有。要验它需要一个灯背后有东西
的场景。

### 留下的

- 一次只处理一盏灯。多光源要么多通道打包 (R8 -> RGBA8, 四盏) 要么多张图。
- 蒙版几何体在光追里是实心的 —— ray query 没有 any-hit, 评估不了 alpha。
- 阴影贴图那几个通道仍然在跑: 光追只替掉一盏灯, 其余的还走图集。全部转光追
  之后那些通道可以整个跳过。
- 硬阴影。软阴影要对面光源多采样, 那需要降噪, 是另一件事。

verify.ps1 50 步。


## Day 4 — 光追环境光遮蔽

`FRayTracedAoPass` (order 130): 逐像素在法线半球内按余弦加权投射若干条射线,
输出全分辨率 R8 遮蔽图。

余弦加权采样让估计量**就是命中比例本身** —— 不需要再乘 cos 再除以概率密度。
少一处能写错的地方, 而那种错误的表现是"AO 看起来对但数值系统性偏高"。

### 这一天真正的收获: 这个量有闭式解

地面 y=0, 墙 z=0, 地面上一点离墙 d, 搜索半径 R, 令 c = d/R:

    遮蔽率 = (1-c²)/2 - (2/π) ∫_c^1 s·arcsin(c/s) ds
    AO     = 1 - 遮蔽率

c=0 时正好 0.5 (半个半球被挡住), c≥1 时正好 1 (墙在半径之外)。

推导: 方向 ω 命中墙当且仅当 ω_z < -c。余弦加权的立体角积分对 φ 先积,
{φ : sinφ < -c/s} 的测度是 (π - 2·arcsin(c/s)); 对 s = sinθ 再积即得。

**这个式子在 Python 里用四十万次余弦加权采样独立验过**, 七个 c 值上最大差
7.6e-4 —— 正是那个采样数的噪声量级。参考值本身也要验, 否则整条判据建在
一个没人查过的公式上。

### 数字

    半径 0.8   有遮蔽区 39236 像素   实测均值 0.801607  解析 0.801690
               有符号误差 -0.000083  绝对误差 0.003465
    半径 2.0   有遮蔽区 116036 像素  实测均值 0.810914  解析 0.811038
               有符号误差 -0.000124  绝对误差 0.003379

**绝对误差 0.0034 与 R8 的量化步长 1/255 = 0.0039 同量级。** 也就是说逐像素
的误差已经压到输出纹理的精度极限 —— 再准也表示不出来了。

对照: GTAO 那条判据只能验"随半径增大朝 0.5 单调收敛" (0.679 → 0.586 →
0.557), 因为屏幕空间的近似没有解析解可对。

### 统计范围: 只取有遮蔽的那一段

第一版拿全体地面像素平均, 得到绝对误差 0.0003 —— **比量化步长还小**, 一看
就不对。原因是 c≥1 的地方闭式解恒为 1, 而地面在视野里从 0.02 一直铺到 4.7,
半径 0.8 时八成以上的像素落在那一段。它们的误差恒为零, 把信号稀释了五倍。

一个"AO 恒为 1"的错误实现在全体平均上只差百分之几, 而在 d<R 那一段上差
二十个百分点。所以判据只统计 d<R, 并且加一条元判据: 那一段的**解析均值**
必须明显小于 1, 否则说明这一段本身就没有遮蔽, 判据是空的。

### 变异验证 7/10 —— 三条逃逸都是这个场景里的真等价

**法线偏移放大一百倍 (1e-3 → 0.1)** —— 数字一位没变。墙角场景的地面法线是
+Y, 而墙是 z=0 平面 —— **法线平行于墙**。沿它推 0.1 只是贴着墙滑一段, 到墙
的距离 d 一点没变。

这与 Day 3 那边"沿光线方向的偏置不移动阴影边界"是同一类事: **偏置移动的方向
与被遮挡的方向正交时, 它不产生后果。** 两天里这个规律各自独立地出现了一次。

**实例变换转置** —— 转置把平移丢了 (第四列读到的是底行 (0,0,0,1)), 于是地面
从 z∈[0,20] 挪到 z∈[-10,10]、墙从 y∈[0,20] 挪到 y∈[-10,10]。而测量区在
z∈[0,6]、y≥0 —— 两块板挪完之后仍然盖住测量区。这一条在 `--rt-depth-check`
上是被抓住的, 所以覆盖没有缺口, 只是不在这条判据里。

**逐像素旋转去掉** —— 0.0032 对 0.0034, 差别落在量化步长之下。256 个
Hammersley 点本身已经足够均匀。它在低采样数下才现形, 而那时噪声 0.03 已经
超过判据的阈值 0.008。

### 阈值按实测收紧

有符号误差 0.003 (实测的二十四倍), 绝对误差 0.008 (2.3 倍)。第一版取的是
0.01 与 0.06 —— 那时采样数从 256 掉到 16 (噪声 0.03) 都能通过, 判据验的就
不是实现了。

### 开销

    整帧 0.94 -> 4.01 ms
    该通道 3.26 ms GPU (整帧的 81%)

每像素十六条射线, 一千四百七十万条。真实用法要靠时域累积与降噪把它压到
一到两条 —— 那是另一件事, 现在不假装做了。

### 顺带修的

通道禁用时纹理停在 Undefined 布局, 而它被绑进了光照描述符集 —— Vulkan 在
提交时检查**描述符声明的布局**与图像实际布局是否一致, 与着色器读不读无关。
现在无论启不启用都先做一次布局初始化。

(附带发现: 验证层一直是开着的, 之前没输出只是因为没问题。)

verify.ps1 51 步。


## Day 5 — 光追反射

屏幕空间反射的缺口是**看不出来的**。相机背后的、被前景挡住的、视野之外的
东西反射不出来, 而画面上只是"那里没有反射" —— 与"那里本来就不该有反射"
长得一模一样。这类缺陷没有症状, 只有对比着看才知道少了什么。

光追没有这个缺口, 代价是命中之后要**自己**把顶点、法线、材质取回来:
光栅化那条路上由固定功能硬件做的属性插值, 这里得手写。

### 几何表

`FRayTracingGeometryEntry`, 32 字节/实例: 顶点缓冲区与索引缓冲区的**设备
地址**、顶点跨度、索引位宽、材质下标。

用地址而不是描述符数组, 是因为一个场景几百个网格, 每个占一个描述符槽的话
bindless 表会被几何体挤满; 而地址只是两个 32 位数, 塞进一个 SSBO 就完了。

表按**源对象下标**索引, 与 `VkAccelerationStructureInstanceKHR` 的自定义
下标一致。按实例序号索引的话, 只要有一个对象被跳过 (不可见、无索引缓冲区),
后面所有物体的材质就整体错位一格 —— 而错位一格在画面上是"某个球的反射里
颜色不对", 极难归因。

着色器用 `GL_EXT_buffer_reference` 取回三个顶点, 按重心坐标插值法线, 用
`rayQueryGetIntersectionObjectToWorldEXT` 变到世界空间, 查材质基色, 算一次
朗伯着色并**再发一条阴影射线** —— 不发的话反射里的一切都是全亮的, 那正是
SSR 最容易被一眼认出来的破绽之一。

### 判据: 四个能逐像素对上的原始量

判据不看颜色。颜色是一串运算的末端, 对不上时不知道是哪一步错了; 对上了也
可能是两处错误相消。看四个原始量:

    命中距离  与 -P.z/R.z 比          225280 像素全对, 最大误差 0.000154
    材质下标  与墙的 bindless 下标比    一个都不错 (地面 0 / 墙 1)
    命中法线  与墙的法线 (0,0,1) 比     最大误差**恰好 0**
    位置自洽  由 t 算的点 == 由顶点插的点, 最大残差 0.000003

### 第四个量是这一天真正的收获

前三个量都依赖**场景里的量有变化**, 而墙角场景太均匀了: 两块平面上九个
顶点的法线完全相同。于是"取错顶点""取错索引""重心权重算错"三条变异
**全部逃逸** —— 第一轮只有 5/10。

位置不一样。三个顶点的位置**必然**不同 (否则三角形退化), 取错任何一处,
由 t 算出的点与由顶点插出的点就分开了。这一条与场景无关:

    positionResidual = length(origin + t * direction - interpolate(v0,v1,v2,bary))

加上它之后 8/10。这是本周期第二次遇到"与场景无关的自洽量比与场景有关的
解析值更能分辨对错" —— 解析值验的是"算得对不对", 自洽量验的是"有没有在
算同一件事", 而后一类错误更常见。

### 第二个场景补上最后一维

OBJ 测试场景的六个子网格**共用一对缓冲区**, 索引字节偏移分别是
0/768/840/912/984/1080 —— 这是唯一能验到"索引地址漏加偏移"的场景。它没有
解析值, 只跑位置自洽 (43931 个命中像素, 最大残差 0.000001)。

两个场景合起来 **9/10**。

唯一逃逸: "几何表按实例序号写而不是源下标"。两个场景里都没有对象被跳过,
于是两者恒等。要验它需要一个**有对象被跳过**的场景 —— 这一条写在代码里,
不是"以后再说", 是"知道缺什么"。

### 顺带

墙角场景的墙给了独立材质 (地面 0 / 墙 1)。共用一个的话"命中处的材质下标"
那条判据就是空的: 无论几何表怎么错位, 查到的都是同一个数。判据里加了一条
**元判据**把这件事钉住 —— 两个材质下标必须不同, 否则这条判据自己判失败。

### 开销

    GPU 整帧 0.710 -> 1.671 ms, 该通道 0.897 ms (53.7%)

一次弹射 + 每个命中一条阴影射线。

### --hidden

自检与变异验证一轮要跑几十次, 每次弹一个窗口。加了 `--hidden`: 只是不
`ShowWindow`, 判据量到的数值与可见时**逐位相同** (实测光追 AO 的 39236 个
像素、均值 0.801607, 两种模式完全一致)。

**但性能数字不可比。** 窗口不可见时合成器不再取用交换链图像, 实测 GPU 整帧
0.70 (隐藏) 对 1.02 (可见)。跑基准必须用可见窗口 —— 这一点写在标志的注释里,
不写的话下一次拿隐藏模式的数字去比会得出错误的结论。

verify.ps1 51 -> 53 步。


## Day 6 — 接进画面, 以及一个只在画面上现形的缺陷

前五天做的东西都在**旁路**上: 通道跑了, 图产出了, 判据对上了, 但画面没变。
这一天把光追 AO 与光追反射接进着色, 并把 AO 的开销压下来。

### 接法

光照描述符集加两个绑定 (12 = 光追 AO, 13 = 光追反射), 生效与否由光照 UBO
里的**位域**决定。

无条件绑定、让着色器按图的内容去猜的话, "一张全 1 的 AO"与"AO 通道根本
没跑"分不开 —— 又是一条失败会落在通过上的路。

半透明片元一律不读这两张图。这一条在阴影上刚栽过一次 (见下)。

反射的接法: 够光滑的表面上替掉预滤波环境贴图, 按粗糙度在 0.25..0.45 之间
过渡。一条射线只能采样反射波瓣里的**一个方向**: 镜面时那个波瓣就是一条线,
一条射线正好够; 粗糙表面的波瓣是一大片, 一条射线给出的是噪声, 而预滤波
贴图给出的正是那一片的积分。0.25 对应的波瓣张角约 15 度 —— 一条射线与整片
积分的差在那以内还看不出来, 再粗就看得出了。

第一版写在 IBL 分支里, 而综合场景没绑环境贴图 —— 于是 `--rt-reflection`
一个像素都不改, 看起来像"反射没接上"。现在没有 IBL 时它是唯一的镜面环境项。

### 半分辨率 AO: 一个性质就是一条判据

半分辨率不是"把深度降采样再解", 而是**每隔一个像素解一次**。

差别要紧: 前者要先对深度做一次重采样, 而深度在不连续处**不能插值** ——
前景与背景平均出来是一个不存在的表面, 于是那个像素的 AO 是围绕一个幽灵
算出来的。后者取到的每个深度都是真实像素上的原值。

于是半分辨率的结果是全分辨率结果的**严格子集**。而那正是一条判据:

    偶数像素 230400 个, 与全分辨率不同的 **0 个** (最大差 0)
    上采样后 有遮蔽区绝对误差 0.002718 (全分辨率 0.003465)

第一条不留容差 —— 不是"接近", 是**逐位相同**。同一条射线、同一个样本图案、
同一个深度, 差一位都说明半分辨率那条路上多做了一次重采样。

上采样之后误差反而**更小**: 双边平滑把蒙特卡洛噪声抹掉了一部分。这不是
运气, 是十六个样本的方差本来就比双边核引入的偏差大。

    该通道 GPU  2.547 -> 0.687 ms   (3.7 倍)
    整帧        3.302 -> 1.363 ms

这条判据当场抓到一个缺陷: `push.Width/Height/PixelStep` 是在 `PushConstants`
**之后**赋值的, 于是半分辨率跑的是全分辨率的参数、只是格点少了四分之三 ——
78621 个偶数像素与全分辨率不同, 偏差 +0.198。写完判据到抓到它之间隔了一次
运行。

### 双边上采样抽成共享头

`bilateral_common.h`: 一份 `LinearizeViewDepth`, 一份 `BilateralDepthWeight`,
GTAO 与光追 AO 共用。

这两个函数曾经有过第二份, 而那一份的**分母符号写错了整整一个周期**。多一份
实现就多一次写反的机会, 而这一类错误的特征恰恰是**没有症状** —— 权重全都
接近零时双边退化成"取中心点", 画面只是略糊。抽完之后 GTAO 的渗色像素一个
不差 (919)。

### 开销表

RTX 3060, 1280x720, 综合场景, **可见窗口**, GPU 整帧:

    基线                              0.713 ms
    + 光追阴影                        0.966
    + 光追 AO (半分辨率)              1.363
    + 光追 AO (全分辨率)              3.302
    + 光追反射                        1.152
    阴影 + AO半分 + 反射              2.272

三个通道全开 2.272 ms, 而单独开销之和是 0.966+1.363+1.152-2*0.713 = 2.055 ——
超出的部分是三张图各自的带宽与布局转换。

### 变异验证的第四类覆盖边界: 后果只在画面上

前三类 (判据不够 / 场景不够 / 后果不在画面上) 是上一个周期总结的。这一天
补上第四类, 而它是前一类的镜像。

**其一: 半透明片元读了不属于它的阴影掩码。** 光追阴影掩码是**按屏幕像素**
索引的, 而深度缓冲区里存的是最靠前的不透明表面。玻璃自己站在阳光下, 却拿到
了玻璃背后地面的阴影值。所有逐像素的数值判据都抓不到 —— 它们比的是掩码图
本身, 而掩码图是**对的**。是看截图看出来的。修完之后差异像素 25516 -> 15175,
均值 0.5146 -> 0.2165。

**其二: 背景墙上的横条纹。** 射线起点的偏移是固定的 1e-3, 而世界坐标是从
深度反投影来的, 那个误差**随距离急剧增大** (透视深度在远处压缩得极厉害)。
深度量化把连续表面切成一条条等深度的带, 带内所有像素的反投影误差相同,
于是整行一起自遮挡。改成按该像素的深度量子推 (沿视线 16 倍 + 沿法线 4 倍):

    背景墙行均值标准差   2.5824 -> 0.4729
    最大行间跳变         6.7600 -> 0.0857   (79 倍)

### 但第二条没有自动判据, 而我停下来承认了这件事

试了三种引擎内统计量:

  - **半径设成 0.02 要求 AO 处处为 1。** 不成立 —— 球压在地面上, 接触圈处
    两个表面确实相距不到 0.02。有 bug 时 1346 个像素被遮挡, 修好之后 1302
    个, 差别全在噪声里。
  - **远处 (>20 单位) 的 AO 缺口。** 远处也有柱子, 真实遮挡混在里面。
  - **深度连续处的相邻行差。** 两版 0.003917 对 0.003867, 分不开。

三种都判不出来, 所以它们**一条都不进流水线**。一条不会红的判据比没有判据
更糟: 没有判据时你知道自己不知道; 有一条永远绿的判据时你以为自己知道。

代码留着 (`RunRayTracedAoSelfCheck`), 头上写明"试过什么、为什么不行", 好让
下一个人不必从头试一遍。唯一能分辨它的是**截图统计**, 而那正是这个缺陷被
发现的方式。

verify.ps1 53 步全绿。


## Day 7 — 判据固化

这一天不加功能。六天下来攒了三条**记录在案的逃逸**, 这一天把它们逐条堵掉,
然后对整段硬件光追做一轮统一的变异验证。

三条逃逸的成因各不相同, 而它们合起来正好覆盖了变异验证的三类覆盖边界。

### 一、判据不够: 旁路判据证明不了画面

前六天的每一条光追判据都是同一个形状 —— 把通道产出的那张图读回来, 与解析
值逐像素比。那证明了"这张图算得对"; 它**没有**证明"画面用了这张图"。

中间隔着的东西不少: 描述符绑定的槽位、光照 UBO 里的位域、着色器里那个 if、
半透明的护栏。任何一处断了, 旁路判据照样满分通过 —— 因为那张图确实还是
对的, 只是没人读它。

`--rt-hybrid-check` 的三条子判据都不看颜色 (颜色对不对是别的判据的事),
只看**因果**:

    AO 只能变暗                    实测变亮 0 个像素
    AO 暗的地方画面得跟着变           实测 82.9%   (阈值 20%)
    反射改到的像素比例落在区间内       实测 3.7%    (区间 0.5%..25%)

第一条是从物理来的: 环境光遮蔽是从环境项里减光, 它没有任何途径让一个像素
变亮。有像素变亮就说明它被乘到了错的项上, 或者符号反了。

第三条**两头都卡**。只卡下限的话, "把反射无条件加到每个像素上"照样通过 ——
而粗糙度过渡窗口存在的全部理由就是不能那么做。

五条变异全红: AO 分支断开 / AO 反着乘 / rtFlags 位序对调 / 反射不看粗糙度
/ 反射分支断开。

### 二、场景不够: 场景里没有那件事

Day 5 记下过一条逃逸。几何表写成 `table[实例序号]` 还是 `table[源对象下标]`,
只在**有对象被跳过**时才分得开 —— 而三个测试场景一个都不跳过 (墙角 2/2、
综合 33/33、OBJ 3/3)。判据没有覆盖到它, 不是因为判据不够严。

`--rt-geometry-table-check` 自己造那件事: 取真实场景的对象列表, 在中间插一个
没有三角形的对象 (`IndexCount = 0` —— 点精灵、纯变换节点都是这样), 单独建
一份加速结构, 把几何表读回来逐条比对。

两条元判据缺一不可:

  - 那个对象**真的被跳过了**。没跳过就没有错位, 判据是空的。
  - 跳过点之后**至少有一对相邻对象是可分的**。全场景共用一对缓冲区、同一个
    材质的话, 两种写法写出来的表一模一样。

第二条是 Day 5 那个教训的直接产物 —— 那一天四个量里有三个正是栽在"场景
太均匀"上。

变异实测退出码 26, 不符 5 条表项。顺带把 `FRayTracingScene.h` 头上那段说
"第 i 个实例对应第 i 个对象"的设计哲学改对: Day 5 起就不成立了, 而一段
说错了的设计哲学比没有更坏 —— 下一个人会照着它写。

### 三、判据跑错了场景

把双边权重整个去掉, 墙角场景上**所有**判据全绿。

不是判据不够严, 是那个场景几乎没有深度不连续, 而双边加权的**全部作用**都在
不连续处。平坦区域上双边、双线性、最近邻三者给出的结果完全一样:

    墙角场景, 半分辨率绝对误差
    双边 0.002621 | 双线性 0.002621 | 最近邻 0.002612

前两个连小数点后六位都相同。

`--rt-ao-upsample-check` 跑综合场景 (球与柱子的轮廓、物体与远景之间到处是
深度不连续), 而且**限定在跨越深度不连续的像素上**再取均值:

    双边 (正确)     0.006633
    退化成双线性     0.019440   -> 红
    退化成最近邻     0.006240   -> 见下

阈值 0.012。整幅的最大差不行 (噪声主导, 三个变体都接近 1), 整幅的均值也不行
(不连续像素只占百分之几, 摊平就没了)。

### 一条**撤掉**的元判据

第一版这条判据里还有一句: "不连续处的平均差必须大于连续处的"。想法是防住
"深度门限没选出任何东西"。

它在**正确的实现上就是红的**: 0.006633 对 0.006831, 不连续处反而更小。

而那恰恰是双边加权在做的事 —— 它把不连续处变成不难的地方。要求"不连续处
更难"等于要求这个滤波器失效。**一条判据要求被测对象出错, 那不是判据严,
是判据写反了。**

撤掉了, 并把这段话留在代码里: 撤掉之后代码里什么也看不见, 而下一个人多半会
想到同一条"元判据"并再写一遍。

### 深度线性化四份并成一份

上面那个"退化成最近邻"是深度线性化的分母符号写反的后果 —— 就是上一个周期
那条藏了整整一个周期的历史缺陷。而它在上面那条统计量上比正确实现还**小**。

不是巧合: 偶数像素上半分辨率与全分辨率逐位相同, 最近邻总是取其中一个,
于是它比任何混合都更贴近全分辨率。**它错在画面上 (边缘台阶), 不错在这个
数上。**

Day 6 以为共享头解决了这件事。其实只并了两份, 还剩两份自己写的
(`rt_depth.comp` 与 `rt_ao.comp`)。现在四份并成一份 —— 而 `rt_depth.comp`
拥有全引擎最严的一条深度判据 (与光追命中距离逐像素比到 64 ULP)。并进来
之后符号一反, 距离变成负数, 那条判据立刻红 (实测退出码 20)。

**判据不必条条万能, 但每条缺陷都得有一条判据能红。**

头文件同时改名 `bilateral_common.h` -> `depth_common.h`。那个名字自己在
制造重复: 光追深度比对与光追 AO 都要线性化深度, 而它们都不做双边滤波 ——
于是各自又写了一份。名字要说的是"这里放什么", 不是"第一个用它的人在做
什么"。

### 变异验证 10/10

| 变异 | 落在哪条判据上 | 退出码 |
|---|---|---|
| AO 分支断开 | rt-hybrid | 27 |
| AO 反着乘 | rt-hybrid | 27 |
| rtFlags 位序对调 | rt-hybrid | 27 |
| 反射不看粗糙度 | rt-hybrid | 27 |
| 反射分支断开 | rt-hybrid | 27 |
| 半分辨率忽略步长 | rt-ao | 22 |
| 上采样退化成双线性 | rt-ao-upsample | 28 |
| 上采样错开一个像素 | rt-ao | 22 |
| 深度线性化分母符号 | rt-depth | 20 |
| 几何表按实例序号索引 | rt-geometry-table | 26 |

零逃逸。这是本周期第一轮零逃逸的扫描, 而它花了整整一天 —— 前六天每天都
留了一两条"知道缺什么"的记录, 这一天把它们兑现。

verify.ps1 53 -> 56 步, 全绿。


## Day 8 — Meshlet 构建器

虚拟几何的第一块: 把三角形网格切成 meshlet (64 顶点 / 124 三角形上限),
每个带一组 3 字节局部索引、一个包围球、一个法线锥。

### 判据先于优化

这个数据结构的全部价值建立在一件事上: **meshlet 是原网格的一个划分**。
下游 (剔除、可见性缓冲、材质解析) 全都这么假定。少一个三角形是"模型上有
个洞", 多一个是 Z 冲突 —— 而两者都可能只在某个视角下才看得见, 于是"画面
对不对"那条路上的判据抓不住它们。

所以 `--meshlet-check` 不看画面、不依赖场景、不需要 GPU, 只问一件事:
**展开全部 meshlet 得到的三角形集合, 与原始索引数组是不是同一个多重集。**

"同一个"含绕序: (a,b,c) 与 (a,c,b) 是两个不同的三角形 (法线相反);
只允许旋转 —— (a,b,c)、(b,c,a)、(c,a,b) 是同一个。

十条判据, 后两条是**质量**判据 —— 一个三角形一个 meshlet 满足前八条的
每一条, 而它把顶点数据放大三倍、把剔除粒度缩到没有意义。

### 判据当场抓到两个质量缺陷

第一版贪心只从"与当前 meshlet 共享顶点的三角形"里挑, 邻接一用尽就收尾:

    立方体 (六个面互不共享顶点)   12 个三角形切成 **6** 个 meshlet
    球体 (连通)                  平均只填到 62.7 / 124 个三角形
    OBJ 测试网格                 182 个三角形切成 **30** 个

加了一条退路: 邻接用尽时改挑"窗口内最近且装得下的"。距离要有上限 ——
不设上限的话场景另一头的三角形也会被拉进来, 包围球撑满整个网格, 剔除彻底
失效, 而正确性判据对此一个字都不会说。

系数是量出来的 (球体 64x48, 6016 个三角形):

    系数   meshlet 数   平均三角形   平均包围球半径
    关     96           62.67        0.2405
    1      94           64.00        0.2449  (+1.8%)
    2      90           66.84        0.2597  (+8.0%)
    4      81           74.27        0.2942  (+22.3%)
    8      73           82.41        0.3370  (+40.1%)

取 1。这条退路是**为病态情形准备的, 不是用来榨填充率的**: 系数 1 时立方体
已经并回 1 个 meshlet、OBJ 从 30 并回 7 —— 该修的全修好了。再往上加, 买到
的填充率全部由剔除精度付账 (剔除效率随投影面积走, 半径涨 40% 是面积涨 96%)。

### 两条逃逸, 两条新判据

**其一: 把包围球心从"包围盒中心"改成"局部顶点表的第一个点"。** 包含性
判据一个字都不说 —— 半径是取到最远顶点的, 球心取哪里都包得住。而平均半径
涨了 66% (球体 0.2449 → 0.4059, 立方体正好两倍)。

补的判据**不需要调参**: 半径不得超过自身包围盒的半对角线。那是"盒内任一
点到盒心的距离不超过半对角线"这个几何事实, 不是经验值。球心一偏离盒心就
可能越过它, 取到角上时正好是两倍。

**其二: 法线锥张角超过半球时不标记无效。** 包含性判据仍然满足 (最小投影
本来就是那个余弦), 而这个锥对背面剔除毫无意义。补的判据是哨兵值的形态:
余弦要么恰好是无效标记 (-2), 要么严格为正; 落在 (-1, 0] 里就是"张过了半球
却没标记"。

这一条**当天还没有消费者** —— 剔除是 Day 9 的事, 所以它的后果当时不在
画面上。但契约是当天定的, 判据也该当天写。(Day 9 果然用到了它: 那个哨兵
约定正是"相机在球内"那条 early-out 是死代码的证明前提。)

哨兵取 -2 而不是 -1 是同一个道理: -1 是"张角恰好 180 度"这个合法值, 拿它
当哨兵的话, 一个真的张满半球的 meshlet 会被误判成无效 —— 而那**恰好也是
安全的**, 于是这个混淆不会有症状, 会一直留着。

### 变异 10/10

丢掉每个 meshlet 的最后一个三角形 / 绕序翻转 / 局部索引写成全局 / 顶点
上限放宽到 255 / 三角形偏移按字节算 / 包围球半径缩水 1% / 球心取第一个
顶点 / 法线锥余弦取平均 / 张角超半球不标记 / 不看邻接按索引顺序切。

verify.ps1 56 → 57 步。


## Day 9 — 两级剔除

第一级按实例 (视锥), 第二级按 meshlet (视锥 + 法线锥背面剔除)。第一级把
活下来的实例压实成一张紧凑表, 第二级按"一个工作组一个可见实例"间接分派。

两级不是"做两遍同样的事": 第一级的输入是实例数 (几十到几千), 第二级是
meshlet 数 (几万到几百万)。第一级每剔掉一个实例, 第二级就少几百次测试 ——
省下的不是一次测试, 是一整棵子树。实测综合场景 33 个实例剔到 21 个,
185 个 meshlet 里 81 个被视锥剔、18 个被背面剔。

meshlet 按**批次**切而不是按网格切: 材质解析要能从"命中了哪个 meshlet"
直接得到"用哪个材质", 跨材质的 meshlet 没有唯一答案。代价是批次边界上的
meshlet 填不满 —— 那是必须付的, 省下来的话材质就得逐三角形存。

### 判据: 与 CPU 参考实现逐条相同

参考实现**逐字照抄** `meshlet_common.h` 里那三个函数。各写各的话, 比的是
"两个实现一不一样", 而两个实现可以一起错; 照抄的话, 比的是"GPU 上跑的与
CPU 上跑的是不是同一段逻辑" —— 而这条判据真正要拦的正是让两者分道扬镳的
东西: 描述符绑错、push constant 布局不对、屏障漏了、原子累加溢出。

meshlet 头也是从 GPU **读回来**的, 不是 CPU 内存里那份 —— 那样上传与汇总
拷贝这两段路径才进了覆盖。

### 判据抓到一个我自己写进去的缺陷

第一级用实例包围球, 第二级用逐 meshlet 包围球。我原以为"实例包围球外接自
包围盒, 所以它包住每个 meshlet 包围球"。

**不成立。** meshlet 的球会从包围盒的角上鼓出去。实测综合场景里有 2 个
meshlet 就这样被第一级误剔 —— 而那在画面上是整块物体消失。

而集合比对**抓不到它**: 第一级剔掉的实例根本不进第二级, GPU 那边压根没有
它的痕迹。抓到它的是一条专门写的判据: "第一级剔掉的实例, 逐 meshlet 判据
也必须剔掉"。修法是给实例包围球加上该批次最大的 meshlet 半径 —— 那时包含
是可证明的。

教训: **参考实现要照着真实的算法写, 不是照着一个"应该等价"的简化版写。**
等价性本身是要证明的东西, 而这里它是假的。

### 分支覆盖计数当判据

第一轮扫描 12 条变异逃了 4 条, 成因全是同一个: **那个分支在这个视角下根本
没被走到**, 而一致性判据对没走到的分支毫无约束。

所以每个分支一个计数器, 每个计数器一条"必须大于零"的元判据:

    六个视锥平面各自**独自**剔掉过 meshlet     67/82/6/22/20/15
    法线锥无效的 early-out 被走到              51 次
    非均匀缩放的 early-out 被走到              38 次

"独自"要紧: 只统计"被 p 剔掉"的话, 一个 meshlet 同时在三个平面外面就给
三个平面各记一笔, 而其中两个可能从来没单独起过作用。

判据自己造条件 —— 六组相机配置 (窄视场 / 只视锥 / 宽视场 / 远平面拉到 8 /
近平面推到 12 / 相机放进某个包围球)。综合场景为此加了两个物体: 一个均匀
缩放的立方体 (法线锥无效) 与一个椭球 (非均匀缩放但锥有效)。

加它们是因为原场景里**每一个立方体都是非均匀缩放的**, 于是"缩放不一致就
不剔"那条 early-out 先返回, 把"法线锥无效就不剔"那条完全遮住 ——
**两条 early-out 互相遮掩**这件事, 只有分支覆盖计数看得出来。

### 删掉一条可证明的死代码

背面判据原本还有一条"相机在包围球内就不剔"。它改不了任何结果:

    dot(d, axis) <= |d| = distance                        (柯西-施瓦茨)
    相机在球内时 distance <= radius
    有效锥的 cutoff 恒为正 (Day 8 的哨兵判据保证)
    => cutoff * distance + radius >= radius >= distance >= dot(d, axis)

那个 >= 永远不成立。是变异验证逼出来的: 把它删掉判据一动不动地绿。
**一条不会红的分支就是没有判据的分支**, 留着只会让下一个人以为它在起作用。

这与 Day 6 那条判不出来的 AO 自检是同一条原则的两面: 那次是判据不会红,
这次是代码不会错 —— 处理都是删掉, 并把理由写下来。

### 导入基准的哨兵逮到 74 ms 退化

meshlet 构建加进 `CreateMesh` 之后, Sponza 导入从 2,520 ms 的预算涨到
2,594 ms。热点是局部顶点查找的线性扫描: 它在最内层, 而 `CanFit` 与
`NewVertexCount` 还各算了一遍 —— 每个候选三角形六次最长 64 步的扫描。

换成 128 槽开放寻址表并把两次合成一次:

    Sponza 导入 2,594 → 1,217 ms

切分结果**逐位不变** (球体仍是 94 个 meshlet / 平均 64 三角形 / 复用 4.166)。

这条退化不出现在任何逐帧数字里 —— 那正是那条哨兵存在的理由。

### 变异 12/12

verify.ps1 57 → 58 步, 全绿 (综合场景的图像基线随两个新物体重新生成)。

**欠账写在代码里**: 本通道当天没有图形消费者, 所以它的判据全是数值判据,
而数值判据证明不了画面 (Day 7 的教训)。端到端判据是 Day 10 的义务。


## Day 10 — 网格着色器路径与回退

Day 9 的判据全是数值判据 (GPU 剔出来的集合与 CPU 参考实现相同), 而数值判据
**证明不了画面** —— 那是 Day 7 的教训, 也是 Day 9 明写在代码里的欠账。
这一天 meshlet 真的被光栅化了, 于是可以问画面。

### 两条路径, 而两条路径本身就是判据

网格着色器 (一个工作组一个可见 meshlet) 与计算展开回退 (把可见 meshlet
展开成一条顶点流 + 经典顶点着色器)。

回退不是"以后再说": 网格着色器在相当一部分在役硬件上没有, 而一条只在新卡上
跑的渲染路径等于没有路径。

更要紧的是, **两条路径给出同一张图这件事本身就是最强的判据** —— 它把
"簇化数据结构解得对不对"与"某一条光栅化路径写得对不对"分开了。

顶点数学只有一份实现 (`meshlet_raster_common.h`), 连运算顺序都钉死成
`(viewProj * model) * p`, 与 `depth_only.vert` 逐字相同。写成
`viewProj * (model * p)` 数学上等价而浮点上不等价, 于是深度差一两个最低位:
不崩、不报错, 只让逐位比对永远差那么一点, 而没人说得清是哪里的问题。

### 判据抓到两个我自己写进去的缺陷

**其一: 工作组数拿错了。** 光栅化按"场景 meshlet 总数"分派。那是错的 ——
可见表里是 (实例, meshlet) 对, 同一个 meshlet 会因为多个实例出现多次, 表长
远大于场景 meshlet 数。实测综合场景 14 个 meshlet 对应九十多条可见记录,
于是两条路径各画了任意的 14 条 (原子追加的顺序每帧都不同), 判据报出 28334
个像素不同。

改成从 GPU 取: 剔除通道把可见数拷进一份间接参数, 两条路径都间接分派。
拷贝只拷第一个 uint —— 整个拷过去的话 y/z 会被诊断计数覆盖, 间接分派的
Y 维变成几十, 同一批 meshlet 被画几十遍。

**其二: 漏了逐实例基址。** meshlet 头里的偏移量是**逐网格**的, 而汇总缓冲区
是**场景级**的。第一个网格 (基址为 0) 一切正常, 后面每个网格都去读第一个
网格的数据。两条路径都错, 但错的方式不同 (一条按可见表顺序读, 另一条按原子
追加顺序), 于是"逐位相同"报出 19026 个像素。

修完之后:

    网格着色器 vs 计算展开回退   不同的像素 **0** 个 (最大差 0)
    墙角场景 vs 经典深度         921600 / 921600 **处处逐位相同**
    综合场景 vs 经典深度         比经典更近的像素 **0** 个, 相等 97.4%

### 阈值按场景分两档, 而分档的依据是可验的

综合场景那 2.6% 的差额来自蒙版材质: 经典路径在那里做 alpha 测试挖了洞、
露出后面的不透明面, 而 meshlet 路径 (只画不透明批次) 直接画那个不透明面。
前后关系没变 (判据二仍然成立), 只是深度值不同。

判据据此分两档: 场景里没有蒙版批次时要求**处处相等** (实测 1.000000),
有则要求 0.95。

分两档的价值在于**强的那一档真的会跑**。只留一个 0.95 下限的话, "顶点变换
写成了等价但不同的表达式"这类差一两个最低位的缺陷会安然通过 —— 而它正是
这条判据最该拦的东西。

### 一条不是缺陷的变异

把网格着色器的 `max_primitives` 从 124 调到 100, 判据全绿。

查下去发现那不是逃逸: 真正卡住构建器的是**顶点**上限。球体 (64x48) 上单个
meshlet 最多 98 个三角形而顶点恰好 64 个 —— 124 个三角形需要复用率 6
(规则三角网格的理论上限) 而实际在 4 附近。所以 100 一个图元都不会丢, 它是
一个更紧但同样正确的配置。

变异改成 124 → 32 (真的丢图元) 之后红。

同时补了一条判据把这个**跨语言的耦合**变成可验的: 网格着色器声明的上限
必须容得下构建器实际产出的最大 meshlet。GLSL 里那两个常量 C++ 读不到,
但"构建器能产出多大"是可以量的 —— 把 `kMaxMeshletVertices` 从 64 改成 128
之后那条判据红。

顺带给构建器的统计加了"单个 meshlet 最多几个三角形/顶点"。没有它就答不上
"两个上限里哪一个才是真正卡住的那个", 而上面那整段推理都建立在那个答案上。

### RHI

`VK_EXT_mesh_shader` 的扩展与特性 (meshShader + taskShader 两个位都要),
函数入口按光追同一个模式载入 —— 任何一个取不到就整个关掉, 因为"直接分派
能跑而间接分派是空指针"是最糟的状态: `IsMeshShaderSupported()` 返回真, 而
实际调用时崩在一个与网格着色器毫无关系的地方。

`EShaderStage` 的位布局与 Vulkan 的 `VkShaderStageFlagBits` 恰好一致
(Task = 0x40, Mesh = 0x80), 所以那个直接 cast 的转换函数不用改。lsc 的批量
编译扩展名加上 task/mesh。

### 变异 12/12

verify.ps1 58 → 60 步 (墙角与综合各一条), 全绿。


## Day 11 — 可见性缓冲

光栅化的同一次执行多写一张 R32_UINT: 每个像素记 (可见记录槽位, 三角形序号)
+ 1, 0 表示这里没有几何体。可见记录唯一确定实例与 meshlet, 所以不必再存
它们 —— 存了反而多一层"这几个数一致吗"的问题。

### +1 偏移不是小聪明

清除颜色附件走的是浮点通道 (`FRHIClearColorValue` 只有四个 Float32), 而
R32_UINT 上只有 `0.0f` 的位模式恰好是整数 0 —— 别的值都要靠位重解释, 那要
动 RHI。

而 0 本身是**合法**编号 (第 0 条记录的第 0 个三角形), 直接拿它当空值的话
那一个三角形与"没画"永远分不开。加一之后 0 空出来了。

这与法线哨兵 (2,2)、法线锥哨兵 (-2) 是同一件事的第三次出现:
**空值必须取一个被测量本身产不出的值。**

### 两个片段着色器, 而这是没办法的事

逐**图元**的输出是网格着色器独有的 (`perprimitiveEXT`); 经典管线里想做到
逐三角形只能靠"给每个三角形三份独立顶点" —— 而回退路径的顶点流本来就是那样
展开的, 所以它用普通的 `flat` 逐顶点输出。

两个片段着色器的接口声明不同, Vulkan 要求前后阶段匹配, 合不成一个。两者的
函数体是同一行, 而"两条路径的可见性逐位相同"那条判据正是对着这一对写的。

### 判据比的是编号**解出来**的三元组, 不是编号本身

第一版直接比编号, 综合场景报出 **817036** 个像素不同 —— 而它们画的是同一批
三角形。

原因是编号里的槽位来自剔除通道的原子追加, 而原子追加的顺序**每帧都不同**。

能比的是槽位解出来的 (实例下标, meshlet 全局下标, 三角形序号): 那三个数是
几何本身的属性, 与压实顺序无关。

为此可见性缓冲区与同一帧的可见记录表必须在**同一个帧内回调**里一起拷出来
—— 分两次读的话中间隔着帧的推进, 表里已经是另一次压实的结果, 解出来的
三元组会张冠李戴。

    墙角场景   两条路径解出的三元组不同的像素 **0** 个
    综合场景   **0** 个 (可见记录 174 条)

这一条是本周期第二次遇到"直接比中间表示是错的, 要比它的语义"。第一次是
Day 9 的可见集合 (比集合而不是比数量)。

### 覆盖一致性

编号为空的像素上深度必须是 1.0, 反过来也一样。两者是同一次光栅化的两个
附件, 覆盖范围只能相同 —— 不同就说明清除值、混合状态或者写掩码出了问题,
而那种错要到材质解析那一天才现形 (一个"深度上有东西但编号是空"的像素会去
解一个不存在的三角形)。实测两个场景都是 0。

以及解码越界: 解出的实例下标 < 实例数、meshlet 下标 < 场景 meshlet 数、
三角形序号 < 124。实测 0。

### 变异 8/8

编号漏 +1 偏移 / 三角形位宽写成 6 位 / 网格路径的三角形序号错 / 网格路径的
槽位错 / 回退路径的三角形序号错 / 回退路径查错实例 / 片段着色器写常量 /
清除值不是 0。

verify.ps1 60 步全绿。

**一次没能复现的失败**: 资产导入哨兵在一次运行里报失败, 复跑两次通过。
未埋点比例实测 1.9~2.9% (上限 5%)、导入中位数 1190 ms (预算 2520), 两项
都有两倍以上余量, 没能复现成因。记在这里而不是当没发生过。


## Day 12 — 材质解析

可见性缓冲区上一个像素只有一个数。解析把它展开回"这个像素上是什么":

    槽位 -> 可见记录 -> (实例, meshlet 全局下标)
    实例 -> 变换矩阵、缓冲区基址、材质下标
    三角形序号 + meshlet -> 三个顶点
    三个顶点 + 像素中心 -> 重心坐标 -> 插值出来的属性

这条链上任何一环错了, 画面上都是"某处的着色不对" —— 而那种错在光栅化阶段
一点痕迹都没有 (深度是对的, 编号也是对的)。

### 一条判据钉住整条链

重算的深度必须与光栅器写的逐像素吻合。那不是"差不多", 而是同一个量的两种
算法: 光栅器做顶点变换再在屏幕空间线性插值 z/w, 解析做同样的顶点变换、在
像素中心解重心坐标、做同样的插值。两者只在浮点舍入上有差别。

    覆盖 919234 个像素, 超过 64 ULP 的 **0** 个 (最大 15.6 ULP)
    覆盖不一致 **0** 个

任何一处解错了, 深度就落在别的三角形上 —— 相邻三角形在屏幕上差一个像素,
深度差是 1e-4 量级, 而一个 ULP 在深度 0.99 处是 6e-8, 差着四个量级。

这是本周期第三次用同一个招式 (Day 5 的位置自洽残差、Day 10 的两条路径逐位
相同): **与场景无关的自洽量比与场景有关的解析值更能分辨对错。**

### 法线只能在同一个表面上比

法线与经典 G-Buffer 吻合 —— 平均夹角 **0.0026 度**, 最大 0.39 度。

但只在两条路径画了**同一个表面**的像素上比: 蒙版材质那些地方经典路径挖了
洞露出后面的面, 而 meshlet 路径直接画那个面, 两者的法线本来就该不同。
不排除的话 2.6% 的像素超差 —— 而那 2.6% 恰好是 Day 10 量到的蒙版板子的
屏幕面积。

判据要问的是"同一个三角形上两条路径插出来的法线一样吗", 拿不同的三角形去
比是在问另一个问题。

### 第四条判据是被逃逸逼出来的

第一轮扫描里"属性不做透视校正"这条变异逃逸了。查下去发现是场景的问题:
综合场景里法线变化大的三角形 (球) 都很小, 而跨深度大的三角形 (地面) 三个
顶点的法线相同 —— 于是任何权重插出来都一样。

补的判据是**插值出来的世界坐标投回屏幕必须落在这个像素的中心上**。它与
场景无关, 而且只有透视校正的权重满足它: 屏幕空间权重插出来的点也在三角形
平面上, 但不是这个像素看到的那个点。

    最大偏离 **0.0005 个像素** (不做透视校正时是几十个像素)

差着五个量级 —— 阈值取在哪都行。

### 一个布局的坑

第一次跑判据时 919234 个像素里有 909499 个深度超差, 最大 **1687 万**个
ULP。那个数量级本身就说明不是精度问题。

std430 下 vec2 要 8 字节对齐, 于是 `{float depth; vec2 normal; uint
material;}` 是 0/8/16 共 24 字节, 而 C++ 那边是紧凑的 16 字节 —— 读回来的
深度是别人的法线。改成 scalar 布局。

不崩、不报错、每个数都不对 —— 而判据一眼就把它与"精度问题"分开了。这正是
"容差不能随手加"的反面例证: 如果这条判据当初留了一个宽容差, 这个错会被
读成精度问题而一直留着。

### 变异 11/11

像素中心不加 0.5 / 屏幕映射漏 0.5 偏移 / y 翻转 / 重心权重不归一 /
深度用透视校正权重 / 属性不做透视校正 / 法线矩阵用 mat3(model) /
材质按 meshlet 下标查 / 材质按槽位查 / 三角形序号不参与取顶点 /
结果缓冲区用 std430。

verify.ps1 60 → 61 步, 全绿。


## Day 13 — Hi-Z 两阶段遮挡剔除

一帧里的顺序:

    第一阶段: 拿**上一帧**的金字塔剔, 剔掉的进待定表
    第一次绘制: 画剩下的
    重建金字塔: 从这一帧刚画出来的深度
    第二阶段: 待定表再测一遍, 补回来的追加进可见表
    第二次绘制: LoadOp::Load, 把补回来的画上去

第一阶段用上一帧的金字塔是必然的 —— 这一帧的深度还没画出来。代价是视角一动
"上一帧挡住而这一帧露出来"的东西会被判成挡住, 而第二阶段就是为了在**同一帧
之内**把它们捞回来。没有第二阶段的话, 那些东西要闪一帧才出现。

### 自纠错的架构让画面判据几乎瞎了

判据的主干很直接: 遮挡剔除是纯粹的优化, 开关它画面必须一个像素都不变。这条
判据自己造条件 —— 相机摆到量出来最有遮挡的位置 (一半以上的 meshlet 被剔),
制造一次视角跳变让第一阶段拿到错的金字塔, 再摆一次到柱子内部让包围球穿近
平面。

它对"没有第二阶段"这条变异是灵的。但只有它的话, **十一条变异只红了三条**。

成因不是判据不严, 是架构本身: 两阶段是自纠错的。第一阶段剔得太狠, 第二阶段
拿重建后的金字塔一测就补回来了 —— 而重建用的深度里恰好没有那些被错剔的东西,
所以它们必然通过。于是遮挡测试本身怎么错, 画面都是对的。

那八条变异损害的是**效率**: 第二阶段要重画一大堆。而效率上的损害不该靠画面
去抓 —— 画面判据在这里不是"不够严", 是**问错了问题**。

### 探针: 越过画面, 直接盯住那两个函数

hiz_probe.comp 不参与渲染, 只把 MeshletProjectSphere 与 MeshletHizMaxDepth
的中间量原样吐出来。310 个包围球, 逐个验六条:

    投出来的屏幕矩形    包得住球的真实投影, 而且不许松
    最近深度            不大于球面上的真实最小深度
    金字塔查到的最大值  不小于第 0 级在同一矩形上的真实最大值
    遮挡结论            等于 (最近深度 > 那个最大值)
    相等的边界          判成"没挡住"
    退化的球            必须报投影失败

前四条都是**单向**的不等式: 保守的方向留着余量, 激进的方向一步都不让。错剔
只可能来自激进的那一侧。

参考值是在球面上撒点 (96×49 的经纬网格) 撒出来的 —— 撒出来的包围盒比真实的
略小、最小深度比真实的略大, 两个偏差都落在让判据**更宽松**的一侧。于是采样
密度不足只会漏报, 不会假报警。

### 探针当场量出两个真缺陷

两个都在原实现里有注释信誓旦旦地保证过。

**四角近似包不住透视投影的球。** 原来取球心沿相机右/上各推一个半径的四个点,
投影之后取包围盒, 注释写着"它一定包得住"。它包不住: 距离 d 半径 r 的球张开
的角是 asin(r/d), 而那四个点只张开 atan(r/d) —— 球越近越大, 差得越多。

    304 个探针里 **203 个**的矩形包不住真实投影, 最多缺 **977 个像素**

改成逐轴解切线。在"这个轴 + 相机前向"张成的平面里, 球退化成一个半径不变的圆
(可达的 (u·a, u·f) 恰好是单位圆盘), 于是 viewX/viewZ 在球上的极值**就是**从
原点向这个圆作的两条切线的斜率 —— 精确解, 不是近似。

斜率转成 NDC 时不直接乘 P00/P11: 它们的**符号**从矩阵里取不出来 (取模长会把
负号吃掉, 而 Vulkan 的 Y 常常是翻的)。造一个"视空间横向比值正好等于这个斜率"
的世界点再投影, 符号就由矩阵自己带出来了。

**"球面上离相机最近的那一点"不是深度最小的点。** 它的视深度是
centerDepth − r·cos(夹角), 只有球正对着相机时才等于 centerDepth − r; 偏到
一侧就偏大, 而偏大就是把没挡住的判成挡住了。

    255 个投影成功的探针里 **156 个**中招, 最多偏 **0.0076**

标准透视矩阵的 z/w 只依赖视深度而且随它单调, 所以深度最小的点就是视深度最小
的点 —— 视深度 = 球心视深度 − 半径, 一句话的事。

两个缺陷都只让第一阶段多剔, 都被第二阶段补了回去, 画面上从来没露过头。修完
之后:

    第一阶段剔 102 -> **94** 个, 第二阶段补 46 -> **25** 个

补画的工作量少了 46%。判据找出来的不是崩溃, 是**白干的活**。

### 三个"判据自己是绿的"

**取样取晚了。** 回读函数自己会渲一帧, 而遮挡判据要看的正是视角跳变的那一帧。
分三次读深度、可见性、可见表的话, 第一次看到的是跳变帧, 第二次已经是金字塔
重建之后的稳定帧了 —— 两者不是同一次光栅化, 而判据比的正是它们。表现是判据
全绿而"第二阶段补回来几个"恒为 0。绿的原因不是实现对, 是取样取晚了。改成一帧
里同时取三样。

**诊断量恒为零。** "第二阶段补回来几个"原来拿剔除通道的统计算, 而那份统计是
剔除通道在**自己的** Execute 末尾拷的 —— 那时第二阶段还没跑, 于是它永远等于
第一阶段的数, 两者相减恒为零。

一个恒为零的诊断量比没有诊断量更糟: 它掩盖了另一个缺陷 (第二阶段的分派参数
y/z 没置 1, 一个工作组都没起), 而那个缺陷让整个第二阶段是死代码。

**跨帧比对。** 新加的"金字塔第 0 级必须就是深度缓冲区"第一版拿跳变帧的深度去
比逐级读回来的金字塔 —— 而逐级读要一级渲一帧, 十一级就是十一帧, 那期间相机
早停下来了。报出 66774 个纹素不符, 而那个数恰好等于跳变帧与稳定帧之间的差,
一眼看去像是"拷贝那一步没生效"。

这一条本身是必要的: 归约判据只验"每一级是上一级的最大值", 它对内容一无所知
—— 整张金字塔全是 0 也满分通过, 而全是 0 意味着"最近", 于是一切都被判成挡住了。

### 补画那一步整个是空的

最难找的一个。症状: 第二阶段的统计一路正常 (剔 102 补 46), 而开关遮挡剔除的
画面差**分毫不差**地停在 66774 个像素。

把第二阶段改成无条件补回全部 113 个 —— 画面差还是 66774。三次差值完全相同,
这个"完全没变"才是线索: 补回来的 meshlet 一个都没画上去。

原因是补画的间接参数 y/z 是 0。与分派参数一模一样的坑, 但藏得更深: "第二阶段
补回来 N 个"读的是**计算着色器**写的计数器, 它一路正常, 而
vkCmdDrawMeshTasksIndirectEXT 的 groupCountY = 0 让第二次绘制一个像素都画不
出来。

同一段代码还有两个: 第二阶段写进可见表的那一段没有屏障 (补画的网格着色器读不
到), 越界判断拿 push constant 里的上限而不是真实待定数 (越界线程会把待定表里
上一帧的记录当成"这一帧新露出来的"补画出来)。

### 三条不会红的分支

变异验证顺手挖出三条构造上永远不会红的判断。

    相机在球内            被近平面判断完全盖住 —— 相机在球内则
                          球心视深度 <= |delta| < r, 必然先返回
    近平面判断写了两遍    centerClip.w 恒等于 centerDepth, 最近点的
                          裁剪 w 恒等于 centerDepth - r, 两句同一个条件
    第二阶段的投影判断    待定表里的记录都是第一阶段投影成功才追加的,
                          同一帧同一视图重算结果必然一样

前两条删掉了 —— 留着只会让人以为那种情形被专门处理过。第三条留着 (它把"存疑
不剔"的意图写在明面上, 而且退一万步, 投影失败时 rect 全零, 退化矩形本身就让
遮挡测试返回"没挡住"), 但在注释里说清楚它不可达, 变异验证里单独列出、不计入
分母。

**一条不会红的分支就是没有判据的分支** —— 这是本周期第三次撞上它 (Day 6 的
相机在球内早退、Day 9 的元判据写反)。

### 相等的边界要造出来才验得到

"遮挡判定用 > 还是 >=" 这条变异一度红过一次, 换个相机姿态又绿了。成因: 两者
只在**恰好相等**时不同, 而真实场景里包围球的最近点严格在几何体前面, 这个条件
的测度是零。它红那一次是偶然。

造出来: 把同一个矩形查出来的最大值再喂回同一个函数, 两边逐位相同。正确的实现
必然返回"没挡住"。这条判据是确定性的, 不靠场景撞运气。

### 探针也得摆对姿态

"相机右方向取列而不是取行"这条变异在探针下也逃了一次。成因: 探针原来跟着画面
判据在 yaw = pitch = 0 的姿态上跑, 而那时视图矩阵的旋转部分是单位阵 ——
视图投影矩阵的第 0 行与第 0 列**恰好相等**。两个取法拿到同一个向量, 探针的每
一个数都分毫不差。

这是**判据的场景不够**, 不是判据不严。探针改成自己摆一个 yaw 与 pitch 都不为
零的姿态 (那时旋转矩阵没有一个零元素), 立刻红。

四个覆盖边界 (判据不够 / 场景不够 / 后果不在画面上 / 后果只在画面上) 这一天
撞上了三个: 画面判据看不见效率损害是第三个, 探针姿态与柱内摆位是第二个, 归约
判据对内容一无所知是第一个。

### 变异 11/11

    金字塔取最小而不是最大              红
    金字塔漏收奇数尺寸的最后一行/列       红
    遮挡判据用 >= 而不是 >              红
    挑级数用 floor 而不是 ceil          红
    只采一个纹素而不是 2x2              红
    最近深度用球心而不是球面最近处        红
    切线不算半径 (退回球心方向)          红
    两个轴都用横向分量                  红
    球与近平面相交时照剔不误             红
    相机右/上方向取列而不是行            红
    没有第二阶段 (单阶段)               红

    第二阶段在投影失败时剔掉             构造上不可达, 不计入分母

其中八条是探针加上去之后才红的 —— 画面判据对它们全绿。

## 附: BC7 编码器

`lat` 补上 BC7 (1812 行, 零第三方依赖 —— 打开 block_compression 的 bc7
feature 会把 wgpu 拉进来, 而这个工具的价值就在于离线无 GPU)。

编码器产出 4 种 mode (6/1/3/7), **解码器 8 种全支持**。解码器完整实现是
刻意的: 它是拿来验编码器的 oracle, 只实现用到的那几种会让"编码器写错、
解码器跟着错"互相掩盖。

真实纹理上比 BC1 高 **7~11 dB**; checker/stripes 这类纯色块图**无损**
(PSNR = inf) —— 那是 mode 6 的 7+1 位端点精度的直接推论, 同时也是选 mode
正确性的证据。

端到端: TestScene 烘成 BC7 后加载, 纹理显存 69 → 17 KiB, 与未压缩 PNG 版
逐像素比对**最大差 2/255、均值 0.004** (BC1/BC3/BC5 那一批是均值 0.294)。
