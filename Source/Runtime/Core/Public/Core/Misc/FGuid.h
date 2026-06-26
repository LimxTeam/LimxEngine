/*******************************************************************************
 * 文件: FGuid.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   128 位全局唯一标识符 — 用于资产引用、对象标识、序列化索引
 *   提供生成、比较、哈希、字符串格式化等操作
 *   零 STL 依赖实现
 *
 * 设计哲学:
 *   UUID v4 兼容 — 128 位随机数 + 版本/变体位
 *   值语义 — 16 字节 POD 类型，可安全拷贝和序列化
 *   O(1) 比较 — 通过 64 位对比实现快速等价判断
 *
 * 技术特性:
 *   - 存储: 4 个 UInt32 (A, B, C, D) 共 16 字节
 *   - 生成: NewGuid() 使用平台 CSPRNG (Windows: BCryptGenRandom)
 *   - 格式化: ToString() → "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
 *   - 解析: FromString() 从 36 字符格式化字符串解析
 *   - 哈希: THash<FGuid> 特化
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"

// Windows CSPRNG 前向声明 (bcrypt.h 不属于 windows.h，始终需要前向声明)
#if LIMX_PLATFORM_WINDOWS
#if LIMX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable: 4273)
#endif
extern "C"
{
    // NTSTATUS BCryptGenRandom(void* hAlgorithm, unsigned char* pbBuffer,
    //                          unsigned long cbBuffer, unsigned long dwFlags);
    long __stdcall BCryptGenRandom(void*, unsigned char*, unsigned long,
                                    unsigned long);
}
#if LIMX_COMPILER_MSVC
#pragma warning(pop)
#endif
#endif

namespace Limx
{

/// 128 位全局唯一标识符
struct FGuid
{
    UInt32 A;  ///< 高 32 位
    UInt32 B;  ///< 中高 32 位
    UInt32 C;  ///< 中低 32 位
    UInt32 D;  ///< 低 32 位

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 全零 (无效 GUID)
    constexpr FGuid() : A(0), B(0), C(0), D(0) {}

    /// 分量构造
    constexpr FGuid(UInt32 inA, UInt32 inB, UInt32 inC, UInt32 inD)
        : A(inA), B(inB), C(inC), D(inD) {}

    // ========================================================================
    // 生成
    // ========================================================================

    /// 生成新的随机 GUID (UUID v4)
    LIMX_NODISCARD static FGuid NewGuid()
    {
        FGuid result;
        GenerateRandomBytes(
            reinterpret_cast<UInt8*>(&result), sizeof(FGuid));

        // UUID v4 版本位: B 的高 4 位 = 0100
        result.B = (result.B & 0xFFFF0FFFU) | 0x00004000U;
        // UUID 变体位: C 的高 2 位 = 10
        result.C = (result.C & 0x3FFFFFFFU) | 0x80000000U;

        return result;
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 是否有效 (非全零)
    LIMX_NODISCARD constexpr bool IsValid() const
    {
        return A != 0 || B != 0 || C != 0 || D != 0;
    }

    /// 使无效
    void Invalidate()
    {
        A = B = C = D = 0;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(const FGuid& other) const
    {
        return A == other.A && B == other.B &&
               C == other.C && D == other.D;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FGuid& other) const
    {
        return !(*this == other);
    }

    LIMX_NODISCARD constexpr bool operator<(const FGuid& other) const
    {
        if (A != other.A) return A < other.A;
        if (B != other.B) return B < other.B;
        if (C != other.C) return C < other.C;
        return D < other.D;
    }

    // ========================================================================
    // 格式化: "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" (36 字符)
    // ========================================================================

    /// 格式化为 36 字符标准格式 (需要至少 37 字节缓冲区)
    void ToString(AnsiChar* outBuffer, SizeType bufferSize) const
    {
        LIMX_ASSERT(bufferSize >= 37);
        // 手动格式化避免 CRT 依赖
        FormatHex32(outBuffer +  0, A);
        outBuffer[8] = '-';
        FormatHex16(outBuffer +  9, static_cast<UInt16>(B >> 16));
        outBuffer[13] = '-';
        FormatHex16(outBuffer + 14, static_cast<UInt16>(B & 0xFFFF));
        outBuffer[18] = '-';
        FormatHex16(outBuffer + 19, static_cast<UInt16>(C >> 16));
        outBuffer[23] = '-';
        FormatHex16(outBuffer + 24, static_cast<UInt16>(C & 0xFFFF));
        FormatHex32(outBuffer + 28, D);
        outBuffer[36] = '\0';
    }

    /// 从格式化字符串解析
    LIMX_NODISCARD static FGuid FromString(const AnsiChar* str)
    {
        if (!str)
        {
            return FGuid();
        }

        FGuid result;
        result.A = ParseHex32(str + 0);
        // str[8] = '-'
        UInt16 bHigh = ParseHex16(str + 9);
        // str[13] = '-'
        UInt16 bLow = ParseHex16(str + 14);
        result.B = (static_cast<UInt32>(bHigh) << 16) |
                   static_cast<UInt32>(bLow);
        // str[18] = '-'
        UInt16 cHigh = ParseHex16(str + 19);
        // str[23] = '-'
        UInt16 cLow = ParseHex16(str + 24);
        result.C = (static_cast<UInt32>(cHigh) << 16) |
                   static_cast<UInt32>(cLow);
        result.D = ParseHex32(str + 28);
        return result;
    }

private:
    // ========================================================================
    // 平台随机数生成
    // ========================================================================

    static void GenerateRandomBytes(UInt8* buffer, SizeType count)
    {
#if LIMX_PLATFORM_WINDOWS
        // BCRYPT_USE_SYSTEM_PREFERRED_RNG = 0x00000002
        long status = BCryptGenRandom(
            nullptr, buffer,
            static_cast<unsigned long>(count), 0x00000002);
        LIMX_ASSERT(status >= 0);  // NT_SUCCESS
        (void)status;
#else
        // 后备: 线性同余 (非密码安全, 仅用于非安全场景)
        static UInt64 seed = 0x5DEECE66DULL;
        for (SizeType index = 0; index < count; ++index)
        {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            buffer[index] = static_cast<UInt8>(seed >> 33);
        }
#endif
    }

    // ========================================================================
    // 十六进制格式化辅助
    // ========================================================================

    static constexpr AnsiChar kHexChars[] = "0123456789abcdef";

    static void FormatHex32(AnsiChar* out, UInt32 value)
    {
        for (Int32 index = 7; index >= 0; --index)
        {
            out[index] = kHexChars[value & 0xF];
            value >>= 4;
        }
    }

    static void FormatHex16(AnsiChar* out, UInt16 value)
    {
        for (Int32 index = 3; index >= 0; --index)
        {
            out[index] = kHexChars[value & 0xF];
            value >>= 4;
        }
    }

    static UInt8 HexCharToNibble(AnsiChar ch)
    {
        if (ch >= '0' && ch <= '9') return static_cast<UInt8>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<UInt8>(ch - 'a' + 10);
        if (ch >= 'A' && ch <= 'F') return static_cast<UInt8>(ch - 'A' + 10);
        return 0;
    }

    static UInt32 ParseHex32(const AnsiChar* str)
    {
        UInt32 result = 0;
        for (Int32 index = 0; index < 8; ++index)
        {
            result = (result << 4) | HexCharToNibble(str[index]);
        }
        return result;
    }

    static UInt16 ParseHex16(const AnsiChar* str)
    {
        UInt16 result = 0;
        for (Int32 index = 0; index < 4; ++index)
        {
            result = static_cast<UInt16>(
                (result << 4) | HexCharToNibble(str[index]));
        }
        return result;
    }
};

// THash 特化
template<>
struct THash<FGuid>
{
    LIMX_NODISCARD FORCEINLINE SizeType operator()(const FGuid& guid) const
    {
        // FNV-1a 混合 4 个分量
        SizeType hash = static_cast<SizeType>(guid.A);
        hash ^= static_cast<SizeType>(guid.B) * 2654435761U;
        hash ^= static_cast<SizeType>(guid.C) * 2246822519U;
        hash ^= static_cast<SizeType>(guid.D) * 3266489917U;
        return hash;
    }
};

} // namespace Limx
