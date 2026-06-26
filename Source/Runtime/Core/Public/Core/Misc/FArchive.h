/*******************************************************************************
 * 文件: FArchive.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   序列化归档 — 读写双向二进制流
 *   提供统一的序列化接口，通过 IsLoading/IsSaving 区分读写方向
 *   用于资产序列化、网络数据打包、存档保存/加载等场景
 *
 * 设计哲学:
 *   双向流 — 同一操作符 << 同时支持读取和写入
 *   版本化 — 内建版本号支持，便于数据迁移
 *   可扩展 — 基类定义接口，子类实现具体 I/O 后端
 *
 * 技术特性:
 *   - FArchive: 序列化归档基类
 *   - FMemoryArchiveWriter: 内存写归档
 *   - FMemoryArchiveReader: 内存读归档
 *   - operator<<: 基础类型序列化操作符
 *   - Serialize: 原始字节序列化
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/FByteBuffer.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

// FByteBuffer 前向声明 (避免循环依赖)
class FByteBuffer;

/// 序列化归档基类
class FArchive
{
public:
    virtual ~FArchive() = default;

    // ========================================================================
    // 方向查询
    // ========================================================================

    /// 是否为加载 (反序列化) 模式
    LIMX_NODISCARD bool IsLoading() const { return m_IsLoading; }

    /// 是否为保存 (序列化) 模式
    LIMX_NODISCARD bool IsSaving() const { return !m_IsLoading; }

    // ========================================================================
    // 版本
    // ========================================================================

    /// 获取归档版本号
    LIMX_NODISCARD UInt32 GetVersion() const { return m_Version; }

    /// 设置归档版本号
    void SetVersion(UInt32 version) { m_Version = version; }

    // ========================================================================
    // 原始序列化接口
    // ========================================================================

    /// 序列化原始字节
    /// 加载模式: 从归档读取 size 字节到 data
    /// 保存模式: 将 data 的 size 字节写入归档
    virtual void Serialize(void* data, SizeType size) = 0;

    // ========================================================================
    // 基础类型操作符
    // ========================================================================

    FArchive& operator<<(bool& value)
    {
        UInt8 byte = value ? 1u : 0u;
        Serialize(&byte, sizeof(UInt8));
        if (IsLoading()) value = (byte != 0);
        return *this;
    }

    FArchive& operator<<(Int8& value)
    {
        Serialize(&value, sizeof(Int8));
        return *this;
    }

    FArchive& operator<<(UInt8& value)
    {
        Serialize(&value, sizeof(UInt8));
        return *this;
    }

    FArchive& operator<<(Int16& value)
    {
        Serialize(&value, sizeof(Int16));
        return *this;
    }

    FArchive& operator<<(UInt16& value)
    {
        Serialize(&value, sizeof(UInt16));
        return *this;
    }

    FArchive& operator<<(Int32& value)
    {
        Serialize(&value, sizeof(Int32));
        return *this;
    }

    FArchive& operator<<(UInt32& value)
    {
        Serialize(&value, sizeof(UInt32));
        return *this;
    }

    FArchive& operator<<(Int64& value)
    {
        Serialize(&value, sizeof(Int64));
        return *this;
    }

    FArchive& operator<<(UInt64& value)
    {
        Serialize(&value, sizeof(UInt64));
        return *this;
    }

    FArchive& operator<<(Float32& value)
    {
        Serialize(&value, sizeof(Float32));
        return *this;
    }

    FArchive& operator<<(Float64& value)
    {
        Serialize(&value, sizeof(Float64));
        return *this;
    }

    // ========================================================================
    // 错误状态
    // ========================================================================

    /// 是否发生错误
    LIMX_NODISCARD bool HasError() const { return m_HasError; }

    /// 设置错误标记
    void SetError() { m_HasError = true; }

protected:
    explicit FArchive(bool isLoading)
        : m_IsLoading(isLoading)
        , m_Version(0)
        , m_HasError(false)
    {
    }

    bool   m_IsLoading;  ///< 加载模式标志
    UInt32 m_Version;     ///< 版本号
    bool   m_HasError;    ///< 错误标志
};

/// 内存写归档 — 序列化到内存缓冲区
class FMemoryArchiveWriter : public FArchive
{
public:
    FMemoryArchiveWriter()
        : FArchive(false)
        , m_Buffer(nullptr)
        , m_Capacity(0)
        , m_Position(0)
    {
        GrowBuffer(256);
    }

    ~FMemoryArchiveWriter() override
    {
        if (m_Buffer)
        {
            GetDefaultAllocator().Deallocate(m_Buffer);
        }
    }

    void Serialize(void* data, SizeType size) override
    {
        EnsureCapacity(size);
        Memory::MemCopy(m_Buffer + m_Position, data, size);
        m_Position += size;
    }

    /// 获取已写入数据
    LIMX_NODISCARD const UInt8* GetData() const { return m_Buffer; }

    /// 获取已写入大小
    LIMX_NODISCARD SizeType GetSize() const { return m_Position; }

    /// 重置
    void Reset() { m_Position = 0; }

private:
    void EnsureCapacity(SizeType additionalBytes)
    {
        SizeType required = m_Position + additionalBytes;
        if (required > m_Capacity)
        {
            SizeType newCapacity = m_Capacity * 2;
            if (newCapacity < required) newCapacity = required;
            GrowBuffer(newCapacity);
        }
    }

    void GrowBuffer(SizeType newCapacity)
    {
        UInt8* newBuffer = static_cast<UInt8*>(
            GetDefaultAllocator().Allocate(newCapacity, 16));
        if (m_Buffer && m_Position > 0)
        {
            Memory::MemCopy(newBuffer, m_Buffer, m_Position);
        }
        if (m_Buffer)
        {
            GetDefaultAllocator().Deallocate(m_Buffer);
        }
        m_Buffer = newBuffer;
        m_Capacity = newCapacity;
    }

    UInt8*   m_Buffer;
    SizeType m_Capacity;
    SizeType m_Position;
};

/// 内存读归档 — 从内存缓冲区反序列化
class FMemoryArchiveReader : public FArchive
{
public:
    FMemoryArchiveReader(const UInt8* data, SizeType size)
        : FArchive(true)
        , m_Data(data)
        , m_Size(size)
        , m_Position(0)
    {
    }

    void Serialize(void* data, SizeType size) override
    {
        if (m_Position + size > m_Size)
        {
            SetError();
            Memory::MemZero(data, size);
            return;
        }
        Memory::MemCopy(data, m_Data + m_Position, size);
        m_Position += size;
    }

    /// 获取当前读取位置
    LIMX_NODISCARD SizeType GetPosition() const { return m_Position; }

    /// 剩余可读字节数
    LIMX_NODISCARD SizeType GetRemaining() const
    {
        return m_Size - m_Position;
    }

    /// 是否读完
    LIMX_NODISCARD bool IsAtEnd() const
    {
        return m_Position >= m_Size;
    }

private:
    const UInt8* m_Data;
    SizeType     m_Size;
    SizeType     m_Position;
};

} // namespace Limx
