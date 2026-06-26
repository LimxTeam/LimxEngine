/*******************************************************************************
 * 文件: FString.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎字符串类型 — 替代 std::string 的零 STL 依赖实现
 *   内部使用 AnsiChar (char) 存储 UTF-8 编码文本
 *   支持动态增长、拼接、查找、子串、格式化等常用操作
 *   通过分配器接口管理内存，默认使用 DefaultAllocator
 *
 * 设计哲学:
 *   UTF-8 优先 — 内部存储始终是 UTF-8 编码的 null 终止字符串
 *   隐式 null 终止 — 始终保证尾部有 '\0'，可直接传给 C API
 *   值语义 — 支持拷贝/移动/比较，行为类似基本类型
 *   小字符串优化 (SSO) — 短字符串 (≤30 字节) 不触发堆分配
 *
 * 技术特性:
 *   - SSO: 30 字节内联缓冲区 (sizeof(FString) = 32 on x64)
 *   - 动态扩容: 2 倍增长策略
 *   - 支持: Append, Prepend, Find, Substring, Replace, Split
 *   - C 兼容: GetCStr() 返回 const char*
 *   - 比较: operator==, operator!=, operator<
 *   - 拼接: operator+, operator+=
 *
 * 依赖关系:
 *   内部: Core/CoreTypes.h (完整类型系统)
 *
 * 注意事项:
 *   FString 不是线程安全的 — 并发读写需外部同步
 *   所有长度/索引以字节计（非 Unicode 码点数）
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

// ============================================================================
// CRT 字符串函数前向声明 — 无需 #include <cstring>
// 注意: 当第三方库 (如 Vulkan SDK) 间接引入 CRT 头文件时，这些声明可能
//       因签名差异而触发 C4273/C2556，此处安全地抑制相关警告。
// ============================================================================

#if LIMX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable: 4273) // dll 链接不一致 — CRT 头文件使用 dllimport
#endif

extern "C"
{
    Limx::SizeType strlen(const char* str);
    int strcmp(const char* str1, const char* str2);
    int strncmp(const char* str1, const char* str2, Limx::SizeType count);
    const char* strstr(const char* haystack, const char* needle);
    const char* strchr(const char* str, int character);
}

#if LIMX_COMPILER_MSVC
#pragma warning(pop)
#endif

namespace Limx
{

/// 引擎字符串类型 — UTF-8 编码，null 终止，SSO 优化
class FString
{
public:
    // SSO 内联缓冲区大小 — 30 字节内容 + 1 字节 null + 1 字节长度/标志
    // 总 sizeof(FString) = 32 (x64 对齐)
    static constexpr SizeType kSSOCapacity = 30;

    // 无效索引
    static constexpr SizeType kNPos = kSizeTypeMax;

    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 空字符串
    FString()
        : m_Allocator(&GetDefaultAllocator())
    {
        SetSSO();
        m_SSO.Data[0] = '\0';
        SetSSOLength(0);
    }

    /// 指定分配器构造
    explicit FString(IAllocator& allocator)
        : m_Allocator(&allocator)
    {
        SetSSO();
        m_SSO.Data[0] = '\0';
        SetSSOLength(0);
    }

    /// 从 C 字符串构造
    FString(const AnsiChar* str)
        : m_Allocator(&GetDefaultAllocator())
    {
        InitFromCStr(str);
    }

    /// 从 C 字符串 + 指定分配器构造
    FString(const AnsiChar* str, IAllocator& allocator)
        : m_Allocator(&allocator)
    {
        InitFromCStr(str);
    }

    /// 从 C 字符串 + 长度构造
    FString(const AnsiChar* str, SizeType length)
        : m_Allocator(&GetDefaultAllocator())
    {
        InitFromBuffer(str, length);
    }

    /// 拷贝构造
    FString(const FString& other)
        : m_Allocator(other.m_Allocator)
    {
        InitFromBuffer(other.GetCStr(), other.GetLength());
    }

    /// 移动构造
    FString(FString&& other) noexcept
        : m_Allocator(other.m_Allocator)
    {
        if (other.IsUsingSSO())
        {
            // SSO 数据直接拷贝
            Memory::MemCopy(&m_SSO, &other.m_SSO, sizeof(SSOBuffer));
        }
        else
        {
            // 堆数据转移所有权
            m_Heap = other.m_Heap;
            other.SetSSO();
            other.m_SSO.Data[0] = '\0';
            other.SetSSOLength(0);
        }
    }

    /// 析构
    ~FString()
    {
        if (!IsUsingSSO() && m_Heap.Data)
        {
            m_Allocator->Deallocate(m_Heap.Data);
        }
    }

    // ========================================================================
    // 赋值运算符
    // ========================================================================

    FString& operator=(const FString& other)
    {
        if (this != &other)
        {
            FreeHeap();
            InitFromBuffer(other.GetCStr(), other.GetLength());
        }
        return *this;
    }

    FString& operator=(FString&& other) noexcept
    {
        if (this != &other)
        {
            FreeHeap();
            m_Allocator = other.m_Allocator;

            if (other.IsUsingSSO())
            {
                Memory::MemCopy(&m_SSO, &other.m_SSO, sizeof(SSOBuffer));
            }
            else
            {
                m_Heap = other.m_Heap;
                other.SetSSO();
                other.m_SSO.Data[0] = '\0';
                other.SetSSOLength(0);
            }
        }
        return *this;
    }

    FString& operator=(const AnsiChar* str)
    {
        FreeHeap();
        InitFromCStr(str);
        return *this;
    }

    // ========================================================================
    // 元素访问
    // ========================================================================

    /// 返回 null 终止的 C 字符串
    LIMX_NODISCARD FORCEINLINE const AnsiChar* GetCStr() const
    {
        return IsUsingSSO() ? m_SSO.Data : m_Heap.Data;
    }

    /// 等价于 GetCStr()，方便 C API 调用
    LIMX_NODISCARD FORCEINLINE const AnsiChar* operator*() const
    {
        return GetCStr();
    }

    /// 下标访问
    LIMX_NODISCARD FORCEINLINE AnsiChar& operator[](SizeType index)
    {
        LIMX_ASSERT(index < GetLength());
        return GetMutableData()[index];
    }

    LIMX_NODISCARD FORCEINLINE AnsiChar operator[](SizeType index) const
    {
        LIMX_ASSERT(index < GetLength());
        return GetCStr()[index];
    }

    // ========================================================================
    // 长度与容量
    // ========================================================================

    /// 字符串字节长度（不含 null 终止符）
    LIMX_NODISCARD FORCEINLINE SizeType GetLength() const
    {
        return IsUsingSSO() ? GetSSOLength() : m_Heap.Length;
    }

    /// 是否为空字符串
    LIMX_NODISCARD FORCEINLINE bool IsEmpty() const
    {
        return GetLength() == 0;
    }

    /// 当前缓冲区容量（不含 null）
    LIMX_NODISCARD FORCEINLINE SizeType GetCapacity() const
    {
        return IsUsingSSO() ? kSSOCapacity : m_Heap.Capacity;
    }

    // ========================================================================
    // 修改操作
    // ========================================================================

    /// 追加字符串
    FString& Append(const AnsiChar* str, SizeType length)
    {
        if (length == 0)
        {
            return *this;
        }

        SizeType currentLength = GetLength();
        SizeType newLength = currentLength + length;

        EnsureCapacity(newLength);

        AnsiChar* data = GetMutableData();
        Memory::MemCopy(data + currentLength, str, length);
        data[newLength] = '\0';
        SetLength(newLength);

        return *this;
    }

    FString& Append(const AnsiChar* str)
    {
        if (str)
        {
            return Append(str, static_cast<SizeType>(strlen(str)));
        }
        return *this;
    }

    FString& Append(const FString& other)
    {
        return Append(other.GetCStr(), other.GetLength());
    }

    /// 追加单个字符
    FString& AppendChar(AnsiChar character)
    {
        return Append(&character, 1);
    }

    /// 清空内容但保留内存
    void Clear()
    {
        if (IsUsingSSO())
        {
            m_SSO.Data[0] = '\0';
            SetSSOLength(0);
        }
        else
        {
            m_Heap.Data[0] = '\0';
            m_Heap.Length = 0;
        }
    }

    /// 重置 — 清空并释放堆内存
    void Reset()
    {
        FreeHeap();
        SetSSO();
        m_SSO.Data[0] = '\0';
        SetSSOLength(0);
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 查找子字符串首次出现位置
    /// @return 找到返回字节偏移，未找到返回 kNPos
    LIMX_NODISCARD SizeType Find(const AnsiChar* needle) const
    {
        if (!needle || needle[0] == '\0')
        {
            return kNPos;
        }

        const AnsiChar* haystack = GetCStr();
        const AnsiChar* found = strstr(haystack, needle);
        if (found)
        {
            return static_cast<SizeType>(found - haystack);
        }
        return kNPos;
    }

    /// 查找字符首次出现位置
    LIMX_NODISCARD SizeType FindChar(AnsiChar character) const
    {
        const AnsiChar* data = GetCStr();
        const AnsiChar* found = strchr(data, character);
        if (found)
        {
            return static_cast<SizeType>(found - data);
        }
        return kNPos;
    }

    /// 是否包含子字符串
    LIMX_NODISCARD bool Contains(const AnsiChar* needle) const
    {
        return Find(needle) != kNPos;
    }

    /// 是否以指定前缀开头
    LIMX_NODISCARD bool StartsWith(const AnsiChar* prefix) const
    {
        if (!prefix)
        {
            return false;
        }
        SizeType prefixLen = static_cast<SizeType>(strlen(prefix));
        if (prefixLen > GetLength())
        {
            return false;
        }
        return strncmp(GetCStr(), prefix, prefixLen) == 0;
    }

    /// 是否以指定后缀结尾
    LIMX_NODISCARD bool EndsWith(const AnsiChar* suffix) const
    {
        if (!suffix)
        {
            return false;
        }
        SizeType suffixLen = static_cast<SizeType>(strlen(suffix));
        SizeType len = GetLength();
        if (suffixLen > len)
        {
            return false;
        }
        return strncmp(GetCStr() + len - suffixLen, suffix, suffixLen) == 0;
    }

    // ========================================================================
    // 子串
    // ========================================================================

    /// 提取子字符串
    /// @param startIndex 起始字节偏移
    /// @param count      字节数 (kNPos 表示到末尾)
    LIMX_NODISCARD FString Substring(SizeType startIndex,
                                      SizeType count = kNPos) const
    {
        SizeType len = GetLength();
        if (startIndex >= len)
        {
            return FString(*m_Allocator);
        }

        SizeType actualCount = count;
        if (actualCount == kNPos || startIndex + actualCount > len)
        {
            actualCount = len - startIndex;
        }

        return FString(GetCStr() + startIndex, actualCount);
    }

    /// 提取左侧 count 个字节
    LIMX_NODISCARD FString Left(SizeType count) const
    {
        return Substring(0, count);
    }

    /// 提取右侧 count 个字节
    LIMX_NODISCARD FString Right(SizeType count) const
    {
        SizeType len = GetLength();
        if (count >= len)
        {
            return FString(*this);
        }
        return Substring(len - count, count);
    }

    // ========================================================================
    // 运算符重载
    // ========================================================================

    /// 拼接运算符
    FString& operator+=(const FString& other)
    {
        return Append(other);
    }

    FString& operator+=(const AnsiChar* str)
    {
        return Append(str);
    }

    FString& operator+=(AnsiChar character)
    {
        return AppendChar(character);
    }

    LIMX_NODISCARD friend FString operator+(const FString& lhs, const FString& rhs)
    {
        FString result(lhs);
        result.Append(rhs);
        return result;
    }

    LIMX_NODISCARD friend FString operator+(const FString& lhs, const AnsiChar* rhs)
    {
        FString result(lhs);
        result.Append(rhs);
        return result;
    }

    LIMX_NODISCARD friend FString operator+(const AnsiChar* lhs, const FString& rhs)
    {
        FString result(lhs);
        result.Append(rhs);
        return result;
    }

    // ========================================================================
    // 比较运算符
    // ========================================================================

    LIMX_NODISCARD friend bool operator==(const FString& lhs, const FString& rhs)
    {
        if (lhs.GetLength() != rhs.GetLength())
        {
            return false;
        }
        return Memory::MemCompare(lhs.GetCStr(), rhs.GetCStr(), lhs.GetLength()) == 0;
    }

    LIMX_NODISCARD friend bool operator!=(const FString& lhs, const FString& rhs)
    {
        return !(lhs == rhs);
    }

    LIMX_NODISCARD friend bool operator==(const FString& lhs, const AnsiChar* rhs)
    {
        return strcmp(lhs.GetCStr(), rhs ? rhs : "") == 0;
    }

    LIMX_NODISCARD friend bool operator!=(const FString& lhs, const AnsiChar* rhs)
    {
        return !(lhs == rhs);
    }

    LIMX_NODISCARD friend bool operator<(const FString& lhs, const FString& rhs)
    {
        return strcmp(lhs.GetCStr(), rhs.GetCStr()) < 0;
    }

    // ========================================================================
    // 迭代器 — 支持范围 for
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE const AnsiChar* begin() const { return GetCStr(); }
    LIMX_NODISCARD FORCEINLINE const AnsiChar* end() const
    {
        return GetCStr() + GetLength();
    }
    LIMX_NODISCARD FORCEINLINE AnsiChar* begin() { return GetMutableData(); }
    LIMX_NODISCARD FORCEINLINE AnsiChar* end()
    {
        return GetMutableData() + GetLength();
    }

private:
    // ========================================================================
    // SSO 内部布局
    // ========================================================================

    // SSO 标志位存储在最后一个字节的最高位
    // 当该位为 0 时使用 SSO，为 1 时使用堆分配
    // SSO 长度存储在最后一个字节的低 7 位 (最大 30 < 127)

    struct SSOBuffer
    {
        AnsiChar Data[kSSOCapacity + 1];  // 30 字节内容 + 1 字节 null
        UInt8 LengthAndFlag;               // 高 1 位: 堆标志, 低 7 位: 长度
    };

    struct HeapBuffer
    {
        AnsiChar* Data;      // 堆分配的缓冲区
        SizeType  Length;    // 当前字节长度 (不含 null)
        SizeType  Capacity;  // 缓冲区容量 (不含 null)
        UInt8     Padding[sizeof(SSOBuffer) - sizeof(AnsiChar*) - sizeof(SizeType) * 2 - 1];
        UInt8     Flag;      // 高 1 位必须为 1 表示堆模式
    };

    static_assert(sizeof(SSOBuffer) == sizeof(HeapBuffer),
        "SSO 和 Heap 布局大小必须一致");

    // ========================================================================
    // SSO 辅助
    // ========================================================================

    FORCEINLINE bool IsUsingSSO() const
    {
        // 检查最后一个字节的最高位
        return (m_SSO.LengthAndFlag & 0x80) == 0;
    }

    FORCEINLINE void SetSSO()
    {
        m_SSO.LengthAndFlag = 0;  // 清除堆标志
    }

    FORCEINLINE void SetHeapFlag()
    {
        m_Heap.Flag = 0x80;
    }

    FORCEINLINE SizeType GetSSOLength() const
    {
        return static_cast<SizeType>(m_SSO.LengthAndFlag & 0x7F);
    }

    FORCEINLINE void SetSSOLength(SizeType length)
    {
        LIMX_ASSERT(length <= kSSOCapacity);
        m_SSO.LengthAndFlag = static_cast<UInt8>(length & 0x7F);
    }

    FORCEINLINE void SetLength(SizeType length)
    {
        if (IsUsingSSO())
        {
            SetSSOLength(length);
        }
        else
        {
            m_Heap.Length = length;
        }
    }

    FORCEINLINE AnsiChar* GetMutableData()
    {
        return IsUsingSSO() ? m_SSO.Data : m_Heap.Data;
    }

    // ========================================================================
    // 初始化辅助
    // ========================================================================

    void InitFromCStr(const AnsiChar* str)
    {
        if (!str || str[0] == '\0')
        {
            SetSSO();
            m_SSO.Data[0] = '\0';
            SetSSOLength(0);
            return;
        }

        SizeType length = static_cast<SizeType>(strlen(str));
        InitFromBuffer(str, length);
    }

    void InitFromBuffer(const AnsiChar* str, SizeType length)
    {
        if (length <= kSSOCapacity)
        {
            // SSO 路径
            SetSSO();
            if (str && length > 0)
            {
                Memory::MemCopy(m_SSO.Data, str, length);
            }
            m_SSO.Data[length] = '\0';
            SetSSOLength(length);
        }
        else
        {
            // 堆分配路径
            SizeType capacity = CalculateGrowth(length);
            m_Heap.Data = static_cast<AnsiChar*>(
                m_Allocator->Allocate(capacity + 1, kDefaultAlignment));
            if (str && length > 0)
            {
                Memory::MemCopy(m_Heap.Data, str, length);
            }
            m_Heap.Data[length] = '\0';
            m_Heap.Length = length;
            m_Heap.Capacity = capacity;
            SetHeapFlag();
        }
    }

    // ========================================================================
    // 容量管理
    // ========================================================================

    void EnsureCapacity(SizeType requiredLength)
    {
        SizeType currentCapacity = GetCapacity();
        if (requiredLength <= currentCapacity)
        {
            return;
        }

        SizeType newCapacity = CalculateGrowth(requiredLength);

        if (IsUsingSSO())
        {
            // SSO → 堆: 分配新缓冲区，复制 SSO 数据
            SizeType currentLength = GetSSOLength();
            AnsiChar* newData = static_cast<AnsiChar*>(
                m_Allocator->Allocate(newCapacity + 1, kDefaultAlignment));
            if (currentLength > 0)
            {
                Memory::MemCopy(newData, m_SSO.Data, currentLength);
            }
            newData[currentLength] = '\0';

            m_Heap.Data = newData;
            m_Heap.Length = currentLength;
            m_Heap.Capacity = newCapacity;
            SetHeapFlag();
        }
        else
        {
            // 堆 → 更大的堆
            AnsiChar* newData = static_cast<AnsiChar*>(
                m_Allocator->Allocate(newCapacity + 1, kDefaultAlignment));
            if (m_Heap.Length > 0)
            {
                Memory::MemCopy(newData, m_Heap.Data, m_Heap.Length);
            }
            newData[m_Heap.Length] = '\0';

            m_Allocator->Deallocate(m_Heap.Data);
            m_Heap.Data = newData;
            m_Heap.Capacity = newCapacity;
            SetHeapFlag();
        }
    }

    void FreeHeap()
    {
        if (!IsUsingSSO() && m_Heap.Data)
        {
            m_Allocator->Deallocate(m_Heap.Data);
        }
    }

    static SizeType CalculateGrowth(SizeType requiredLength)
    {
        // 至少 64 字节，然后 2 倍增长
        SizeType capacity = 64;
        while (capacity < requiredLength)
        {
            capacity *= 2;
        }
        return capacity;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    union
    {
        SSOBuffer  m_SSO;
        HeapBuffer m_Heap;
    };
    IAllocator* m_Allocator;
};

} // namespace Limx
