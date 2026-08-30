/*******************************************************************************
 * 文件: CoreTypes.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   核心类型聚合头文件 — 按依赖顺序包含所有基础类型定义
 *   包含平台检测、基础类型、API 导出宏、核心宏、类型特征
 *   不包含反射宏（反射宏需要用户按需显式包含）
 *
 * 设计哲学:
 *   单一包含点 — 业务代码只需 #include "Core/CoreTypes.h" 即可获得
 *   完整的类型系统基础设施
 *   严格的包含顺序 — 确保依赖链的正确性
 *
 * 包含顺序:
 *   1. Platform.h        — 编译器/平台/架构检测 (最底层，零依赖)
 *   2. PlatformTypes.h   — 固定宽度类型、常量 (依赖 Platform.h)
 *   3. CoreAPI.h         — DLL 导出宏 (依赖 Platform.h)
 *   4. CoreMacros.h      — 工具宏 (依赖 Platform.h, PlatformTypes.h)
 *   5. TypeTraits.h      — 类型特征 (依赖 Platform.h)
 *   6. MemoryOps.h       — 内存操作原语 (依赖 1-5)
 *   7. IAllocator.h      — 分配器接口 (依赖 1-4)
 *   8. DefaultAllocator.h— 默认堆分配器 (依赖 6-7)
 *   9. TArray.h          — 动态数组容器 (依赖 6-8)
 *  10. FString.h         — 字符串类型 (依赖 6-8)
 *  11. TUniquePtr.h      — 独占智能指针 (依赖 6-8)
 *  12. TSharedPtr.h      — 共享智能指针 (依赖 6-8)
 *  13. TMap.h            — 哈希表容器 (依赖 6-9)
 *  14. TFunction.h       — 类型擦除可调用对象 (依赖 6-8)
 *
 * 依赖关系:
 *   本文件是上述 14 个头文件的聚合入口
 *
 ******************************************************************************/

#pragma once

// --- 第 1 层: 平台检测 (零依赖) ---
#include "Core/HAL/Platform.h"

// --- 第 2 层: 基础类型定义 ---
#include "Core/HAL/PlatformTypes.h"

// --- 第 3 层: API 导出宏 ---
#include "Core/CoreAPI.h"

// --- 第 4 层: 核心工具宏 ---
#include "Core/CoreMacros.h"

// --- 第 5 层: 类型特征 ---
#include "Core/TypeTraits/TypeTraits.h"

// --- 第 6 层: 内存操作原语 ---
#include "Core/Memory/MemoryOps.h"

// --- 第 7 层: 分配器接口 ---
#include "Core/Memory/IAllocator.h"

// --- 第 8 层: 分配器 ---
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Memory/BlockAllocator.h"
#include "Core/Memory/LinearAllocator.h"
#include "Core/Memory/FStackAllocator.h"
#include "Core/Memory/TObjectPool.h"
#include "Core/Memory/TFreeList.h"
#include "Core/Memory/FMemoryArena.h"
#include "Core/Memory/TPoolAllocatorTyped.h"
#include "Core/Memory/TRangeAllocator.h"
#include "Core/Memory/TPool.h"
#include "Core/Memory/TRingAllocator.h"
#include "Core/Memory/TIndexPool.h"

// --- 第 9 层: 基础容器 ---
#include "Core/Containers/TArray.h"
#include "Core/Containers/FString.h"

// --- 第 10 层: 智能指针 ---
#include "Core/Templates/TUniquePtr.h"
#include "Core/Templates/TSharedPtr.h"

// --- 第 11 层: 高级容器与模板 ---
#include "Core/Templates/TPair.h"
#include "Core/Templates/TSpan.h"
#include "Core/Templates/TStaticArray.h"
#include "Core/Templates/TVariant.h"
#include "Core/Templates/TResult.h"
#include "Core/Containers/TMap.h"
#include "Core/Containers/TSet.h"
#include "Core/Containers/TQueue.h"
#include "Core/Containers/TRingBuffer.h"
#include "Core/Containers/TBitArray.h"
#include "Core/Containers/TSparseArray.h"
#include "Core/Containers/TMultiMap.h"
#include "Core/Containers/TFixedArray.h"
#include "Core/Containers/FStringView.h"
#include "Core/Containers/TIntrusiveList.h"
#include "Core/Containers/TSlotMap.h"
#include "Core/Containers/FByteBuffer.h"
#include "Core/Containers/TBitSet.h"
#include "Core/Containers/TLruCache.h"
#include "Core/Containers/TSmallVector.h"
#include "Core/Containers/TPriorityQueue.h"
#include "Core/Containers/TGraph.h"
#include "Core/Containers/FStringPool.h"
#include "Core/Containers/TTagSet.h"
#include "Core/Containers/TConcurrentQueue.h"
#include "Core/Containers/TCircularBuffer.h"
#include "Core/Containers/TIntervalTree.h"
#include "Core/Containers/TDenseMap.h"
#include "Core/Containers/TFlatSet.h"
#include "Core/Containers/TFlatMap.h"
#include "Core/Containers/TStringMap.h"
#include "Core/Containers/TStableVector.h"
#include "Core/Containers/TDeque.h"
#include "Core/Containers/TStack.h"
#include "Core/Containers/TFixedString.h"
#include "Core/Containers/TFixedVector.h"
#include "Core/Containers/TBucketArray.h"
#include "Core/Containers/TChunkedList.h"
#include "Core/Containers/TSortAlgorithms.h"
#include "Core/Containers/TSearchAlgorithms.h"
#include "Core/Containers/FStringFormat.h"
#include "Core/Containers/FStringBuilder.h"
#include "Core/Templates/TFunction.h"
#include "Core/Templates/TOptional.h"
#include "Core/Templates/TDelegate.h"
#include "Core/Templates/THandle.h"
#include "Core/Templates/TEnumFlags.h"
#include "Core/Templates/TTypeList.h"
#include "Core/Templates/TCallback.h"
#include "Core/Templates/TPropertyAccessor.h"
#include "Core/Templates/TSignal.h"
#include "Core/Templates/TStateMachine.h"
#include "Core/Templates/TVersionedObject.h"
#include "Core/Templates/TObjectFactory.h"
#include "Core/Templates/TCommandQueue.h"
#include "Core/Templates/TTypeMap.h"
#include "Core/Templates/TInplaceFunction.h"
#include "Core/Templates/TBitField.h"
#include "Core/Templates/TObjectPtr.h"
#include "Core/Templates/TScopedPtr.h"
#include "Core/Templates/TDoubleBuffer.h"
#include "Core/Templates/TTripleBuffer.h"
#include "Core/Templates/TCompressedPair.h"
#include "Core/Templates/TAlignedBuffer.h"
#include "Core/Templates/TTypedId.h"
#include "Core/Templates/TLazyInit.h"
#include "Core/Templates/TEventQueue.h"
#include "Core/Templates/TObserver.h"
#include "Core/Templates/TBitMask.h"
#include "Core/Templates/TRefCounted.h"
#include "Core/Templates/TEnumArray.h"
#include "Core/Templates/TFuture.h"

// --- 第 12 层: 数学库 ---
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMatrix.h"
#include "Core/Math/FQuat.h"
#include "Core/Math/FTransform.h"
#include "Core/Math/FColor.h"
#include "Core/Math/FBoundingBox.h"
#include "Core/Math/FPlane.h"
#include "Core/Math/FSphere.h"
#include "Core/Math/FRay.h"
#include "Core/Math/FFrustum.h"
#include "Core/Math/FColorGradient.h"
#include "Core/Math/FBezier.h"
#include "Core/Math/FSpline.h"
#include "Core/Math/FInterpCurve.h"
#include "Core/Math/FPerlinNoise.h"
#include "Core/Math/FRect.h"
#include "Core/Math/FLine.h"
#include "Core/Math/FGrid2D.h"
#include "Core/Math/FPolygon2D.h"
#include "Core/Math/FTriangle.h"
#include "Core/Math/FMatrix3.h"
#include "Core/Math/FTransform2D.h"
#include "Core/Math/FColor3.h"
#include "Core/Math/FCircle.h"
#include "Core/Math/FQuadTree.h"
#include "Core/Math/FAABBTree.h"
#include "Core/Math/FAABBox2D.h"
#include "Core/Math/FMeshTopology.h"
#include "Core/Math/FColorHSL.h"
#include "Core/Math/FAngle.h"
#include "Core/Math/FSpectralColor.h"
#include "Core/Math/FRect3D.h"
#include "Core/Math/FGaussian.h"
#include "Core/Math/FCameraProjection.h"

// --- 第 13 层: 标识、路径、哈希与工具 ---
#include "Core/Containers/FName.h"
#include "Core/Misc/FGuid.h"
#include "Core/Misc/FPath.h"
#include "Core/Misc/FHash.h"
#include "Core/Misc/FAssert.h"
#include "Core/Misc/FTypeId.h"
#include "Core/Misc/FRandom.h"
#include "Core/Misc/FNoise.h"
#include "Core/Misc/FCommandLine.h"
#include "Core/Misc/FConfigFile.h"
#include "Core/Misc/FEndian.h"
#include "Core/Misc/FCrc32.h"
#include "Core/Misc/FBase64.h"
#include "Core/Misc/FStringHash.h"
#include "Core/Misc/FScope.h"
#include "Core/Misc/FCompression.h"
#include "Core/Misc/FArchive.h"
#include "Core/Misc/FObjectId.h"
#include "Core/Misc/FConsoleVariable.h"
#include "Core/Misc/FPathUtils.h"
#include "Core/Misc/FMurmurHash.h"
#include "Core/Misc/FAtomicCounter.h"
#include "Core/Misc/FSpinLock.h"
#include "Core/Misc/FStringConverter.h"
#include "Core/Misc/FEasing.h"
#include "Core/Misc/FTokenizer.h"
#include "Core/Misc/FStringUtils.h"
#include "Core/Misc/TFrequencyCounter.h"
#include "Core/Misc/TTimestamp.h"
#include "Core/Misc/FVersion.h"
#include "Core/Misc/FBitOps.h"
#include "Core/Misc/FStatistics.h"
#include "Core/Events/FEvent.h"
#include "Core/Events/FEventDispatcher.h"
#include "Core/Events/FEventSystem.h"
#include "Core/Misc/FEventBus.h"
#include "Core/Misc/FTimer.h"
#include "Core/Misc/FSubsystem.h"
#include "Core/Misc/FModuleManager.h"
#include "Core/HAL/FPlatformFile.h"

// --- 第 14 层: 平台时间与内存查询 ---
#include "Core/HAL/FPlatformTime.h"
#include "Core/HAL/FTimespan.h"
#include "Core/HAL/FDateTime.h"
#include "Core/HAL/FPlatformMemory.h"

// --- 第 15 层: 线程基础设施 ---
#include "Core/Threading/FAtomic.h"
#include "Core/Threading/FMutex.h"
#include "Core/Threading/FThread.h"
#include "Core/Threading/FEvent.h"
#include "Core/Threading/FSemaphore.h"
#include "Core/Threading/FTaskGraph.h"
#include "Core/Threading/TLockFreeQueue.h"
#include "Core/Threading/FJobSystem.h"
#include "Core/Threading/FJobExecutor.h"
#include "Core/Threading/TTaskGroup.h"

// --- 第 16 层: 日志系统 ---
#include "Core/Logging/FLog.h"
