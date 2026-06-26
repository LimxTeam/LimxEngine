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

1. Fix or validate `LRegistry::Destroy` derived destructor dispatch.
2. Decide Vulkan target version: docs say 1.4, code/shaders target 1.3.
3. Establish a reliable shader compile step before launch.
4. Regenerate or replace stale `LimxEngine.sln`.
5. Decide policy for `#pragma warning(disable: 4273)` in Core CRT forward
   declaration files.
6. Fix `FInputManager` mouse button bounds or enum/storage mismatch.
7. Replace or formally encapsulate raw `new/delete` in `FThread` and
   `TRefCounted` if the allocator rule is absolute.
8. Complete material parameter binding in `pbr.frag` or document current shader
   material limitations.
9. Make camera trait sync actually transfer rotation/FOV/aspect, not only
   position.
10. Confirm build/tool behavior only after the user allows running tools.

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
