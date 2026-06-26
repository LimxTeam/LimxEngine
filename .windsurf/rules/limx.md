---
trigger: always_on
---

# Limx Engine 工作空间规则

## 一、绝对禁止事项

1. **禁止使用 C++ STL** — 不允许 `#include <vector>`, `<string>`, `<map>`, `<memory>`, `<functional>`, `<optional>`, `<array>`, `<queue>`, `<stack>`, `<set>`, `<unordered_map>`, `<algorithm>`, `<cstdint>`, `<cstring>`, `<cmath>` 等任何 STL 头文件。所有功能已在 `Source/Core/` 中封装。
2. **禁止重复造轮子** — 实现任何功能前，必须先检查 Core 模块是否已有对应实现。如果有，直接使用，不得自行重写。
3. **禁止使用裸 `new`/`delete`** — 所有内存分配必须通过 Core 分配器体系。
4. **禁止使用 C 标准库类型** — 不用 `int`/`unsigned`/`size_t`/`uint32_t`，统一使用 `Int32`/`UInt32`/`SizeType` 等 Limx 类型。

## 二、Core 模块 API 速查表 — 必须优先使用

### 基础类型 (Core/HAL/PlatformTypes.h)
| 需要什么 | 用什么 | 禁止用什么 |
|---------|--------|-----------|
| 整数 | `Int8/16/32/64`, `UInt8/16/32/64` | `int`, `unsigned`, `short`, `long`, `int32_t` |
| 浮点 | `Float32`, `Float64` | `float`, `double`（代码中可出现但别名优先） |
| 大小/指针 | `SizeType`, `IntPtr`, `UIntPtr` | `size_t`, `ptrdiff_t`, `intptr_t` |
| 字符 | `AnsiChar`, `WideChar`, `TChar` | `char`, `wchar_t` |
| 布尔 | `bool` (允许原生) | — |

### 容器 (Core/Containers/)
| 需要什么 | 用什么 | 禁止用什么 |
|---------|--------|-----------|
| 动态数组 | `TArray<T>` | `std::vector` |
| 小数组 (栈优化) | `TSmallVector<T, N>` | — |
| 固定容量数组 | `TFixedVector<T, N>` / `TFixedArray<T, N>` | `std::array` |
| 静态数组 | `TStaticArray<T, N>` | C 风格数组 |
| 哈希表 | `TMap<K, V>` (Robin Hood) | `std::unordered_map` |
| 哈希集合 | `TSet<T>` (Robin Hood) | `std::unordered_set` |
| 有序扁平映射 | `TFlatMap<K, V>` | `std::map` |
| 有序扁平集合 | `TFlatSet<T>` | `std::set` |
| 稠密映射 | `TDenseMap<K, V>` | — |
| 多值映射 | `TMultiMap<K, V>` | `std::multimap` |
| 字符串映射 | `TStringMap<V>` | — |
| FIFO 队列 | `TQueue<T>` | `std::queue` |
| 优先队列 | `TPriorityQueue<T>` | `std::priority_queue` |
| 栈 | `TStack<T>` | `std::stack` |
| 双端队列 | `TDeque<T>` | `std::deque` |
| 环形缓冲区 | `TRingBuffer<T, N>` / `TCircularBuffer<T>` | — |
| 并发队列 | `TConcurrentQueue<T>` | — |
| 位数组 | `TBitArray` / `TBitSet<N>` | `std::bitset` |
| 稀疏数组 | `TSparseArray<T>` | — |
| 稳定地址数组 | `TStableVector<T>` / `TBucketArray<T, N>` | — |
| Slot Map | `TSlotMap<T>` | — |
| 侵入式链表 | `TIntrusiveList<T>` | — |
| 分块链表 | `TChunkedList<T, N>` | — |
| 图结构 | `TGraph<T>` | — |
| 区间树 | `TIntervalTree<K, V>` | — |
| LRU 缓存 | `TLruCache<K, V>` | — |
| 标签集合 | `TTagSet<T>` | — |

### 字符串 (Core/Containers/)
| 需要什么 | 用什么 | 禁止用什么 |
|---------|--------|-----------|
| 可变字符串 | `FString` (SSO 30字节) | `std::string` |
| 固定容量字符串 | `TFixedString<N>` | — |
| 字符串视图 | `FStringView` | `std::string_view` |
| 字符串构建 | `FStringBuilder` | — |
| 格式化 | `FStringFormat` (`{}` 占位符) | `printf`, `sprintf`, `std::format` |
| 不可变哈希字符串 | `FName` (FNV-1a) | — |
| 字符串池 | `FStringPool` | — |
| 字节缓冲区 | `FByteBuffer` | — |
| 排序 | `TSortAlgorithms` | `std::sort` |
| 搜索 | `TSearchAlgorithms` | `std::find`, `std::binary_search` |

### 智能指针与模板 (Core/Templates/)
| 需要什么 | 用什么 | 禁止用什么 |
|---------|--------|-----------|
| 独占指针 | `TUniquePtr<T>` + `MakeUnique` | `std::unique_ptr` |
| 共享指针 | `TSharedPtr<T>` + `MakeShared` | `std::shared_ptr` |
| 弱引用 | `TWeakPtr<T>` | `std::weak_ptr` |
| 作用域指针 | `TScopedPtr<T>` | — |
| 对象指针 | `TObjectPtr<T>` | 裸指针传递 |
| 引用计数基类 | `TRefCounted<T>` | — |
| 键值对 | `TPair<K, V>` + `MakePair` | `std::pair` |
| 可选值 | `TOptional<T>` | `std::optional` |
| 变体类型 | `TVariant<Ts...>` | `std::variant` |
| 结果类型 | `TResult<T, E>` | — |
| 视图切片 | `TSpan<T>` | `std::span` |
| 可调用对象 | `TFunction<Sig>` (SBO 56B) | `std::function` |
| 小函数 | `TInplaceFunction<Sig, N>` | — |
| 回调 | `TCallback<Sig>` | — |
| 委托 | `TDelegate` (单播/多播) | — |
| 信号/槽 | `TSignal<Sig>` | — |
| 观察者 | `TObserver<T>` | — |
| 句柄 (索引+代) | `THandle<Tag>` | 裸整数 ID |
| 类型化 ID | `TTypedId<Tag>` | — |
| 异步值 | `TFuture<T>` | `std::future` |
| 枚举位标志 | `TEnumFlags<E>` | 手写位运算 |
| 枚举数组 | `TEnumArray<E, V>` | — |
| 位域 | `TBitField<T>` / `TBitMask<N>` | — |
| 状态机 | `TStateMachine<S, E>` | — |
| 命令队列 | `TCommandQueue<Cmd>` | — |
| 对象工厂 | `TObjectFactory<Base>` | — |
| 属性访问器 | `TPropertyAccessor<T>` | — |
| 类型列表 | `TTypeList<Ts...>` | — |
| 类型映射 | `TTypeMap<Pairs...>` | — |
| 压缩对 | `TCompressedPair<A, B>` (EBO) | — |
| 对齐缓冲区 | `TAlignedBuffer<Size, Align>` | — |
| 双/三缓冲 | `TDoubleBuffer<T>` / `TTripleBuffer<T>` | — |
| 延迟初始化 | `TLazyInit<T>` | — |
| 版本化对象 | `TVersionedObject<T>` | — |
| 事件队列 | `TEventQueue<E>` | — |

### 数学 (Core/Math/)
| 需要什么 | 用什么 |
|---------|--------|
| 数学函数 | `FMath::Abs/Min/Max/Clamp/Lerp/Sin/Cos/Sqrt/...` |
| 向量 | `FVector2`, `FVector3`, `FVector4` |
| 矩阵 | `FMatrix` (4x4), `FMatrix3` (3x3) |
| 四元数 | `FQuat` |
| 变换 | `FTransform` (SRT), `FTransform2D` |
| 颜色 | `FColor` (RGBA8), `FLinearColor` (Float32), `FColor3`, `FColorHSL`, `FColorGradient` |
| 光谱颜色 | `FSpectralColor` |
| 角度 | `FRadians`, `FDegrees` (FAngle.h) |
| 包围盒 | `FBoundingBox` (3D AABB), `FAABBox2D` (2D) |
| 几何体 | `FPlane`, `FSphere`, `FRay`, `FLine`, `FTriangle`, `FCircle`, `FRect`, `FRect3D`, `FPolygon2D` |
| 视锥体 | `FFrustum` |
| 曲线 | `FBezier`, `FSpline`, `FInterpCurve` |
| 噪声 | `FPerlinNoise`, `FNoise` |
| 空间索引 | `FQuadTree`, `FAABBTree` |
| 网格拓扑 | `FMeshTopology` |
| 高斯分布 | `FGaussian` |
| 相机投影 | `FCameraProjection` |
| 网格 | `FGrid2D` |
| 缓动函数 | `FEasing` |

### 内存 (Core/Memory/)
| 需要什么 | 用什么 |
|---------|--------|
| 分配器接口 | `IAllocator` |
| 默认堆分配 | `FDefaultAllocator` |
| 块分配器 | `FBlockAllocator` |
| 线性分配器 | `FLinearAllocator` |
| 栈分配器 | `FStackAllocator` |
| 对象池 | `TObjectPool<T>` / `TPool<T>` |
| 类型化池分配 | `TPoolAllocatorTyped<T>` |
| 空闲链表 | `TFreeList<T>` |
| 内存竞技场 | `FMemoryArena` |
| 范围分配器 | `TRangeAllocator` |
| 环形分配器 | `TRingAllocator` |
| 索引池 | `TIndexPool<T>` |
| 页分配器 | `TPageAllocator<T>` |
| 内存操作 | `MemCopy/MemSet/MemZero/MemMove` (MemoryOps.h) |

### 杂项工具 (Core/Misc/)
| 需要什么 | 用什么 |
|---------|--------|
| GUID | `FGuid` (128 位 UUID v4) |
| 文件路径 | `FPath`, `FPathUtils` |
| 哈希 | `FHash`, `FMurmurHash`, `FStringHash`, `FCrc32` |
| Base64 | `FBase64` |
| 压缩 | `FCompression` |
| 序列化 | `FArchive` |
| 随机数 | `FRandom` |
| 噪声 | `FNoise` |
| 命令行 | `FCommandLine` |
| 配置文件 | `FConfigFile` |
| 控制台变量 | `FConsoleVariable` |
| 字节序 | `FEndian` |
| 位操作 | `FBitOps` |
| 统计 | `FStatistics` |
| 版本号 | `FVersion` |
| 计时器 | `FTimer` |
| 频率计数 | `TFrequencyCounter<T>` |
| 时间戳 | `TTimestamp<Clock>` |
| 字符串工具 | `FStringUtils`, `FStringConverter`, `FTokenizer` |
| 断言 | `FAssert`, `LIMX_ASSERT/CHECK/VERIFY/ENSURE` (CoreMacros.h) |
| 作用域守卫 | `FScope` |
| 自旋锁 | `FSpinLock` |
| 原子计数器 | `FAtomicCounter` |
| 对象 ID | `FObjectId` |
| 类型 ID | `FTypeId` |
| 子系统 | `FSubsystem` |
| 模块管理 | `FModuleManager` |

### 事件系统 (Core/Events/ + Core/Misc/)
| 需要什么 | 用什么 |
|---------|--------|
| 事件基类 | `FEventBase` (FEvent.h) |
| 局部分发器 | `FEventDispatcher` (优先级+RAII) |
| 全局事件总线 | `FEventBus` (优先级+延迟队列) |
| 聚合入口 | `FEventSystem.h` |

### 线程 (Core/Threading/)
| 需要什么 | 用什么 | 禁止用什么 |
|---------|--------|-----------|
| 原子操作 | `TAtomic<T>` | `std::atomic` |
| 互斥锁 | `FMutex` | `std::mutex` |
| 线程 | `FThread` | `std::thread` |
| 事件 | `FEvent` (Threading) | — |
| 信号量 | `FSemaphore` | — |
| 任务图 | `FTaskGraph` | — |
| 作业系统 | `FJobSystem` | — |
| 任务组 | `TTaskGroup` | — |
| 无锁队列 | `TLockFreeQueue<T>` | — |

### HAL (Core/HAL/)
| 需要什么 | 用什么 |
|---------|--------|
| 平台检测 | `LIMX_PLATFORM_WINDOWS`, `LIMX_COMPILER_MSVC` 等 (Platform.h) |
| 时间 | `FPlatformTime::Seconds()` |
| 时间跨度 | `FTimespan` |
| 日期时间 | `FDateTime` |
| 内存查询 | `FPlatformMemory` |
| 文件操作 | `FPlatformFile` |

### 日志 (Core/Logging/)
```cpp
LIMX_LOG(CategoryName, Verbosity, "格式 {} {}", arg1, arg2);
```

### 宏 (CoreMacros.h)
| 需要什么 | 用什么 |
|---------|--------|
| 强制内联 | `FORCEINLINE` |
| 禁止内联 | `NOINLINE` |
| 不可丢弃 | `LIMX_NODISCARD` |
| 不返回 | `LIMX_NORETURN` |
| 分支提示 | `LIMX_LIKELY` / `LIMX_UNLIKELY` |
| 断言 | `LIMX_ASSERT(expr)` (Debug), `LIMX_CHECK(expr)`, `LIMX_VERIFY(expr)`, `LIMX_ENSURE(expr)` |
| 对齐 | `LIMX_ALIGNOF(T)`, `LIMX_ALIGNAS(N)` |
| 偏移 | `LIMX_OFFSET_OF(Type, Member)` |
| 数组大小 | `LIMX_ARRAY_COUNT(arr)` |
| 位操作 | `LIMX_BIT(n)`, `LIMX_HAS_FLAG`, `LIMX_SET_FLAG`, `LIMX_CLEAR_FLAG` |
| 枚举位运算 | `LIMX_DEFINE_ENUM_BITWISE_OPS(EnumType)` |
| 禁止复制 | `LIMX_NON_COPYABLE(Class)` |
| 禁止移动 | `LIMX_NON_MOVABLE(Class)` |
| 缓存行对齐 | `LIMX_CACHE_LINE_ALIGNED` |
| 字符串化 | `LIMX_STRINGIFY(x)` |
| 拼接 | `LIMX_CONCAT(a, b)` |

### Platform 模块 (Source/Platform/Public/Platform/RHI/)
| 文件 | 提供什么 |
|------|---------|
| `RHIDefinitions.h` | 29 个 RHI 枚举 + 基础结构体 + 格式查询工具函数 |
| `RHIResources.h` | 18 种类型化 GPU 句柄 + 13 种资源创建描述符 |
| `RHIPipelineState.h` | 图形/计算管线完整创建描述符 |
| `IRHIDevice.h` | RHI 设备抽象接口 |
| `IRHICommandBuffer.h` | 命令缓冲区抽象接口 |

## 三、编码强制规则

1. **头文件包含** — 模块内源文件只需 `#include "Core/CoreMinimal.h"` 或 `#include "Platform/PlatformMinimal.h"`，不要手动逐个包含子头文件。
2. **命名空间** — 所有代码在 `namespace Limx` 内。
3. **文件头注释** — 每个文件必须有标准头部注释块（见用户全局规则）。
4. **构建验证** — 每次修改后用 `C:\Development\LimxEngine\Programs\target\release\lbt.exe build -s Source --rebuild` 验证。
5. **`static_assert`** — 新增类型后必须在对应模块的 `Module.cpp` 中添加编译时验证。
6. **枚举位掩码** — 需要位运算的 `enum class` 必须使用 `LIMX_DEFINE_ENUM_BITWISE_OPS`，不要手写运算符。
7. **类型特征** — 使用 `IsSameV<A,B>`, `IsBaseOfV<A,B>`, `RemoveCVT<T>` 等 TypeTraits.h 中的工具，不要用 STL type_traits。
8. **内存操作** — 使用 `MemCopy/MemSet/MemZero/MemMove`（MemoryOps.h），不要用 `memcpy/memset/memmove`。
9. **数学函数** — 使用 `FMath::` 命名空间下的函数，不要用 `<cmath>` 或裸 C 数学函数。

## 四、开发顺序与架构

### 模块依赖层级
```
Layer 1: Core (Source/Core/) — 零外部依赖
Layer 2: Platform (Source/Platform/) — 依赖 Core, 链接 Vulkan SDK
Layer 3: Luminance (Source/Luminance/) — 依赖 Core + Platform
```

### 当前状态
- **Core**: 完整，130+ 头文件，LBT 构建通过
- **Platform**: RHI 抽象层 + Vulkan 后端完整实现 (设备/资源/管线/同步/命令缓冲区/交换链)，RHIFactory 工厂接口
- **Luminance**: 初始渲染管线 — FWindow(Win32窗口) + FRenderContext(设备+交换链+帧同步) + FRenderer(清屏渲染)
- **构建工具**: LBT (Rust), 路径 `Programs/target/release/lbt.exe`
- **构建状态**: 3 模块全量编译通过，零错误零警告

### 技术约束
- C++23, MSVC 19.38+, `/W4 /WX` 零警告
- Vulkan 1.4.321.1 (SDK: `C:\MyProgram\VulkanSDK\1.4.321.1`)
- 仅 Windows x64
