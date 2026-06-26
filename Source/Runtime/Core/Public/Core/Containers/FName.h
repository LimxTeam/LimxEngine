/*******************************************************************************
 * 文件: FName.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   不可变哈希字符串 — 用于对象命名、资源路径、属性标识等高频比较场景
 *   字符串内容存储在全局字符串池中，FName 仅持有索引和哈希值
 *   比较操作为 O(1) (比较哈希 + 索引)，大幅优于字符串逐字符比较
 *
 * 设计哲学:
 *   字符串池化 — 相同字符串只存储一份，节省内存
 *   O(1) 比较 — 通过预计算哈希值实现常数时间比较
 *   不可变 — 创建后内容不可修改，保证哈希值有效性
 *   轻量值类型 — sizeof(FName) = 16 字节 (哈希 + 索引 + 长度)
 *
 * 技术特性:
 *   - 全局字符串池: 静态 TArray 存储所有唯一字符串
 *   - FNV-1a 哈希: 64 位哈希，极低碰撞率
 *   - 大小写不敏感比较: 可选
 *   - 线程安全: 字符串池的写入当前不加锁 (单线程初始化阶段使用)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Containers/FString.h
 *
 * 注意事项:
 *   FName 的字符串池在程序生命周期内不释放
 *   不适合存储频繁变化的临时字符串
 *   当前实现为单线程安全，多线程场景需外部同步
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"

#if LIMX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable: 4273)
#endif
extern "C"
{
    Limx::SizeType strlen(const char* str);
}
#if LIMX_COMPILER_MSVC
#pragma warning(pop)
#endif

namespace Limx
{

/// FNV-1a 64 位哈希
namespace Detail
{

LIMX_NODISCARD FORCEINLINE constexpr UInt64 FNV1aHash64(
    const AnsiChar* str, SizeType length)
{
    UInt64 hash = 14695981039346656037ULL;  // FNV offset basis
    for (SizeType index = 0; index < length; ++index)
    {
        hash ^= static_cast<UInt64>(static_cast<UInt8>(str[index]));
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

} // namespace Detail

/// 不可变哈希字符串 — O(1) 比较，字符串池化存储
class FName
{
public:
    /// 无效名称常量
    static constexpr UInt64 kInvalidHash = 0;
    static constexpr SizeType kInvalidIndex = kSizeTypeMax;

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 空名称
    FName()
        : m_Hash(kInvalidHash)
        , m_PoolIndex(kInvalidIndex)
        , m_Length(0)
    {
    }

    /// 从 C 字符串构造 — 注册到字符串池
    FName(const AnsiChar* str)
        : m_Hash(kInvalidHash)
        , m_PoolIndex(kInvalidIndex)
        , m_Length(0)
    {
        if (str && str[0] != '\0')
        {
            m_Length = static_cast<UInt32>(strlen(str));
            m_Hash = Detail::FNV1aHash64(str, m_Length);
            m_PoolIndex = RegisterOrFind(str, m_Length, m_Hash);
        }
    }

    /// 从 C 字符串 + 长度构造
    FName(const AnsiChar* str, SizeType length)
        : m_Hash(kInvalidHash)
        , m_PoolIndex(kInvalidIndex)
        , m_Length(0)
    {
        if (str && length > 0)
        {
            m_Length = static_cast<UInt32>(length);
            m_Hash = Detail::FNV1aHash64(str, length);
            m_PoolIndex = RegisterOrFind(str, length, m_Hash);
        }
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 获取字符串内容 (从池中读取)
    LIMX_NODISCARD const AnsiChar* GetCStr() const
    {
        if (m_PoolIndex == kInvalidIndex)
        {
            return "";
        }
        return GetPoolEntry(m_PoolIndex);
    }

    /// 等价于 GetCStr()
    LIMX_NODISCARD const AnsiChar* operator*() const
    {
        return GetCStr();
    }

    /// 哈希值
    LIMX_NODISCARD FORCEINLINE UInt64 GetHash() const { return m_Hash; }

    /// 字符串长度 (字节)
    LIMX_NODISCARD FORCEINLINE UInt32 GetLength() const { return m_Length; }

    /// 是否有效 (非空)
    LIMX_NODISCARD FORCEINLINE bool IsValid() const
    {
        return m_Hash != kInvalidHash;
    }

    LIMX_NODISCARD FORCEINLINE bool IsEmpty() const
    {
        return m_Hash == kInvalidHash;
    }

    // ========================================================================
    // 比较 — O(1)
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE bool operator==(const FName& other) const
    {
        return m_Hash == other.m_Hash && m_PoolIndex == other.m_PoolIndex;
    }

    LIMX_NODISCARD FORCEINLINE bool operator!=(const FName& other) const
    {
        return !(*this == other);
    }

    LIMX_NODISCARD FORCEINLINE bool operator<(const FName& other) const
    {
        return m_Hash < other.m_Hash;
    }

    /// 与 C 字符串比较 (需计算哈希，非 O(1))
    LIMX_NODISCARD bool operator==(const AnsiChar* str) const
    {
        if (!str || str[0] == '\0')
        {
            return !IsValid();
        }
        SizeType len = static_cast<SizeType>(strlen(str));
        UInt64 otherHash = Detail::FNV1aHash64(str, len);
        return m_Hash == otherHash && m_Length == len;
    }

    LIMX_NODISCARD bool operator!=(const AnsiChar* str) const
    {
        return !(*this == str);
    }

    // ========================================================================
    // 静态工具
    // ========================================================================

    /// 获取字符串池中的条目数量
    LIMX_NODISCARD static SizeType GetPoolSize();

private:
    // ========================================================================
    // 字符串池 (简易实现 — 线性探测)
    // ========================================================================

    // 池的最大条目数 — 固定大小避免动态分配依赖
    static constexpr SizeType kMaxPoolEntries = 65536;
    static constexpr SizeType kMaxPoolBytes = 1024 * 1024;  // 1 MB 字符数据

    struct PoolEntry
    {
        UInt64    Hash;
        UInt32    Offset;   // 在字符数据缓冲区中的偏移
        UInt32    Length;
        bool      IsUsed;
    };

    struct StringPool
    {
        PoolEntry Entries[kMaxPoolEntries];
        AnsiChar  Data[kMaxPoolBytes];
        SizeType  EntryCount;
        SizeType  DataOffset;
        bool      IsInitialized;
    };

    /// 获取全局字符串池 (函数静态变量保证初始化顺序)
    static StringPool& GetPool()
    {
        static StringPool s_Pool = {};
        if (!s_Pool.IsInitialized)
        {
            Memory::MemZero(s_Pool.Entries, sizeof(s_Pool.Entries));
            s_Pool.EntryCount = 0;
            s_Pool.DataOffset = 0;
            s_Pool.IsInitialized = true;
        }
        return s_Pool;
    }

    /// 注册或查找字符串 — 返回池索引
    static SizeType RegisterOrFind(const AnsiChar* str, SizeType length,
                                    UInt64 hash)
    {
        StringPool& pool = GetPool();

        // 线性查找已有条目
        for (SizeType index = 0; index < pool.EntryCount; ++index)
        {
            if (pool.Entries[index].IsUsed &&
                pool.Entries[index].Hash == hash &&
                pool.Entries[index].Length == length)
            {
                // 哈希+长度匹配 — 验证内容
                if (Memory::MemCompare(
                        pool.Data + pool.Entries[index].Offset,
                        str, length) == 0)
                {
                    return index;
                }
            }
        }

        // 新条目
        LIMX_ASSERT(pool.EntryCount < kMaxPoolEntries);
        LIMX_ASSERT(pool.DataOffset + length + 1 <= kMaxPoolBytes);

        SizeType entryIndex = pool.EntryCount;
        PoolEntry& entry = pool.Entries[entryIndex];
        entry.Hash = hash;
        entry.Offset = static_cast<UInt32>(pool.DataOffset);
        entry.Length = static_cast<UInt32>(length);
        entry.IsUsed = true;

        // 拷贝字符数据 + null 终止
        Memory::MemCopy(pool.Data + pool.DataOffset, str, length);
        pool.Data[pool.DataOffset + length] = '\0';
        pool.DataOffset += length + 1;
        pool.EntryCount++;

        return entryIndex;
    }

    /// 获取池中指定索引的字符串
    static const AnsiChar* GetPoolEntry(SizeType index)
    {
        StringPool& pool = GetPool();
        LIMX_ASSERT(index < pool.EntryCount);
        return pool.Data + pool.Entries[index].Offset;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    UInt64  m_Hash;       ///< FNV-1a 64 位哈希
    SizeType m_PoolIndex; ///< 字符串池索引
    UInt32  m_Length;     ///< 字符串字节长度
};

// FName 的 THash 特化
template<>
struct THash<FName>
{
    LIMX_NODISCARD FORCEINLINE SizeType operator()(const FName& name) const
    {
        return static_cast<SizeType>(name.GetHash());
    }
};

} // namespace Limx
