/*******************************************************************************
 * 文件: FByteBuffer.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   字节缓冲区 — 二进制数据的顺序读写
 *   支持写入/读取基础类型、字节数组，维护读写游标
 *   用于网络协议打包、资产序列化、内存流等场景
 *
 * 设计哲学:
 *   游标驱动 — 写游标追加数据，读游标顺序消费
 *   自动扩容 — 写入时缓冲区不足自动 2x 扩容
 *   小端序列化 — 多字节类型统一以小端写入 (可移植)
 *
 * 技术特性:
 *   - WriteUInt8/16/32/64, WriteInt32, WriteFloat32/64: 写入基础类型
 *   - WriteBytes: 写入原始字节块
 *   - ReadUInt8/16/32/64, ReadInt32, ReadFloat32/64: 读取基础类型
 *   - ReadBytes: 读取原始字节块
 *   - GetWritePos/GetReadPos: 游标位置
 *   - GetData/GetSize: 缓冲区访问
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/MemoryOps.h, Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 字节缓冲区 — 二进制序列化读写
class FByteBuffer
{
    static constexpr SizeType kDefaultCapacity = 256;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    FByteBuffer()
        : m_Data(nullptr)
        , m_Capacity(0)
        , m_WritePos(0)
        , m_ReadPos(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        Grow(kDefaultCapacity);
    }

    explicit FByteBuffer(SizeType initialCapacity)
        : m_Data(nullptr)
        , m_Capacity(0)
        , m_WritePos(0)
        , m_ReadPos(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        Grow(initialCapacity > 0 ? initialCapacity : kDefaultCapacity);
    }

    /// 从已有数据构造 (拷贝，仅用于读取)
    FByteBuffer(const UInt8* data, SizeType length)
        : m_Data(nullptr)
        , m_Capacity(0)
        , m_WritePos(0)
        , m_ReadPos(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        Grow(length);
        Memory::MemCopy(m_Data, data, length);
        m_WritePos = length;
    }

    ~FByteBuffer()
    {
        if (m_Data)
        {
            m_Allocator->Deallocate(m_Data);
        }
    }

    // 不可拷贝
    FByteBuffer(const FByteBuffer&) = delete;
    FByteBuffer& operator=(const FByteBuffer&) = delete;

    // 移动
    FByteBuffer(FByteBuffer&& other) noexcept
        : m_Data(other.m_Data)
        , m_Capacity(other.m_Capacity)
        , m_WritePos(other.m_WritePos)
        , m_ReadPos(other.m_ReadPos)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Data = nullptr;
        other.m_Capacity = 0;
        other.m_WritePos = 0;
        other.m_ReadPos = 0;
    }

    // ========================================================================
    // 写入 — 基础类型 (小端序列化)
    // ========================================================================

    void WriteUInt8(UInt8 value)
    {
        EnsureCapacity(1);
        m_Data[m_WritePos++] = value;
    }

    void WriteInt8(Int8 value)
    {
        WriteUInt8(static_cast<UInt8>(value));
    }

    void WriteBool(bool value)
    {
        WriteUInt8(value ? 1u : 0u);
    }

    void WriteUInt16(UInt16 value)
    {
        EnsureCapacity(2);
        m_Data[m_WritePos + 0] = static_cast<UInt8>(value & 0xFF);
        m_Data[m_WritePos + 1] = static_cast<UInt8>((value >> 8) & 0xFF);
        m_WritePos += 2;
    }

    void WriteInt16(Int16 value)
    {
        WriteUInt16(static_cast<UInt16>(value));
    }

    void WriteUInt32(UInt32 value)
    {
        EnsureCapacity(4);
        m_Data[m_WritePos + 0] = static_cast<UInt8>(value & 0xFF);
        m_Data[m_WritePos + 1] = static_cast<UInt8>((value >> 8) & 0xFF);
        m_Data[m_WritePos + 2] = static_cast<UInt8>((value >> 16) & 0xFF);
        m_Data[m_WritePos + 3] = static_cast<UInt8>((value >> 24) & 0xFF);
        m_WritePos += 4;
    }

    void WriteInt32(Int32 value)
    {
        WriteUInt32(static_cast<UInt32>(value));
    }

    void WriteUInt64(UInt64 value)
    {
        EnsureCapacity(8);
        for (SizeType byteIndex = 0; byteIndex < 8; ++byteIndex)
        {
            m_Data[m_WritePos + byteIndex] =
                static_cast<UInt8>((value >> (byteIndex * 8)) & 0xFF);
        }
        m_WritePos += 8;
    }

    void WriteInt64(Int64 value)
    {
        WriteUInt64(static_cast<UInt64>(value));
    }

    void WriteFloat32(Float32 value)
    {
        UInt32 bits;
        Memory::MemCopy(&bits, &value, sizeof(UInt32));
        WriteUInt32(bits);
    }

    void WriteFloat64(Float64 value)
    {
        UInt64 bits;
        Memory::MemCopy(&bits, &value, sizeof(UInt64));
        WriteUInt64(bits);
    }

    /// 写入原始字节块
    void WriteBytes(const void* data, SizeType length)
    {
        if (length == 0) return;
        EnsureCapacity(length);
        Memory::MemCopy(m_Data + m_WritePos, data, length);
        m_WritePos += length;
    }

    // ========================================================================
    // 读取 — 基础类型 (小端反序列化)
    // ========================================================================

    LIMX_NODISCARD UInt8 ReadUInt8()
    {
        LIMX_ASSERT(m_ReadPos + 1 <= m_WritePos);
        return m_Data[m_ReadPos++];
    }

    LIMX_NODISCARD Int8 ReadInt8()
    {
        return static_cast<Int8>(ReadUInt8());
    }

    LIMX_NODISCARD bool ReadBool()
    {
        return ReadUInt8() != 0;
    }

    LIMX_NODISCARD UInt16 ReadUInt16()
    {
        LIMX_ASSERT(m_ReadPos + 2 <= m_WritePos);
        UInt16 value =
            static_cast<UInt16>(m_Data[m_ReadPos + 0]) |
            (static_cast<UInt16>(m_Data[m_ReadPos + 1]) << 8);
        m_ReadPos += 2;
        return value;
    }

    LIMX_NODISCARD Int16 ReadInt16()
    {
        return static_cast<Int16>(ReadUInt16());
    }

    LIMX_NODISCARD UInt32 ReadUInt32()
    {
        LIMX_ASSERT(m_ReadPos + 4 <= m_WritePos);
        UInt32 value =
            static_cast<UInt32>(m_Data[m_ReadPos + 0]) |
            (static_cast<UInt32>(m_Data[m_ReadPos + 1]) << 8) |
            (static_cast<UInt32>(m_Data[m_ReadPos + 2]) << 16) |
            (static_cast<UInt32>(m_Data[m_ReadPos + 3]) << 24);
        m_ReadPos += 4;
        return value;
    }

    LIMX_NODISCARD Int32 ReadInt32()
    {
        return static_cast<Int32>(ReadUInt32());
    }

    LIMX_NODISCARD UInt64 ReadUInt64()
    {
        LIMX_ASSERT(m_ReadPos + 8 <= m_WritePos);
        UInt64 value = 0;
        for (SizeType byteIndex = 0; byteIndex < 8; ++byteIndex)
        {
            value |= static_cast<UInt64>(
                m_Data[m_ReadPos + byteIndex]) << (byteIndex * 8);
        }
        m_ReadPos += 8;
        return value;
    }

    LIMX_NODISCARD Int64 ReadInt64()
    {
        return static_cast<Int64>(ReadUInt64());
    }

    LIMX_NODISCARD Float32 ReadFloat32()
    {
        UInt32 bits = ReadUInt32();
        Float32 value;
        Memory::MemCopy(&value, &bits, sizeof(Float32));
        return value;
    }

    LIMX_NODISCARD Float64 ReadFloat64()
    {
        UInt64 bits = ReadUInt64();
        Float64 value;
        Memory::MemCopy(&value, &bits, sizeof(Float64));
        return value;
    }

    /// 读取原始字节块
    void ReadBytes(void* outData, SizeType length)
    {
        LIMX_ASSERT(m_ReadPos + length <= m_WritePos);
        Memory::MemCopy(outData, m_Data + m_ReadPos, length);
        m_ReadPos += length;
    }

    // ========================================================================
    // 游标与状态
    // ========================================================================

    /// 写游标位置 (= 已写入字节数)
    LIMX_NODISCARD SizeType GetWritePos() const { return m_WritePos; }

    /// 读游标位置
    LIMX_NODISCARD SizeType GetReadPos() const { return m_ReadPos; }

    /// 剩余可读字节数
    LIMX_NODISCARD SizeType GetReadableBytes() const
    {
        return m_WritePos - m_ReadPos;
    }

    /// 数据指针
    LIMX_NODISCARD const UInt8* GetData() const { return m_Data; }

    /// 已写入数据大小
    LIMX_NODISCARD SizeType GetSize() const { return m_WritePos; }

    /// 总容量
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }

    /// 是否读完
    LIMX_NODISCARD bool IsReadComplete() const
    {
        return m_ReadPos >= m_WritePos;
    }

    /// 重置读游标
    void ResetReadPos() { m_ReadPos = 0; }

    /// 重置读写游标 (清空数据)
    void Reset()
    {
        m_WritePos = 0;
        m_ReadPos = 0;
    }

    /// 设置读游标到指定位置
    void SeekRead(SizeType pos)
    {
        LIMX_ASSERT(pos <= m_WritePos);
        m_ReadPos = pos;
    }

private:
    /// 确保有足够的写入空间
    void EnsureCapacity(SizeType additionalBytes)
    {
        SizeType required = m_WritePos + additionalBytes;
        if (required > m_Capacity)
        {
            SizeType newCapacity = m_Capacity * 2;
            if (newCapacity < required)
            {
                newCapacity = required;
            }
            Grow(newCapacity);
        }
    }

    /// 扩容
    void Grow(SizeType newCapacity)
    {
        UInt8* newData = static_cast<UInt8*>(
            m_Allocator->Allocate(newCapacity, 16));

        if (m_Data && m_WritePos > 0)
        {
            Memory::MemCopy(newData, m_Data, m_WritePos);
        }

        if (m_Data)
        {
            m_Allocator->Deallocate(m_Data);
        }

        m_Data = newData;
        m_Capacity = newCapacity;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    UInt8*      m_Data;        ///< 字节缓冲区
    SizeType    m_Capacity;    ///< 总容量
    SizeType    m_WritePos;    ///< 写游标
    SizeType    m_ReadPos;     ///< 读游标
    IAllocator* m_Allocator;   ///< 内存分配器
};

} // namespace Limx
