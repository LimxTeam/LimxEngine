/*******************************************************************************
 * 文件: FPlatformFile.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   平台文件 I/O 抽象 — 零 STL 依赖的文件系统操作
 *   封装 Windows CreateFile/ReadFile/WriteFile 等 API
 *   提供文件读写、存在性检查、目录创建等基础操作
 *
 * 设计哲学:
 *   静态工具类 — 无状态的文件操作集合
 *   RAII 文件句柄 — FFileHandle 构造时打开，析构时关闭
 *   二进制优先 — 默认二进制模式，文本模式通过参数指定
 *   缓冲区读写 — 直接操作 UInt8* 缓冲区，不引入流抽象
 *
 * 技术特性:
 *   - FFileHandle: RAII 文件句柄 (读/写/追加模式)
 *   - FPlatformFile::Exists: 文件/目录存在性检查
 *   - FPlatformFile::CreateDirectory: 创建目录 (递归)
 *   - FPlatformFile::ReadAllBytes: 一次性读取整个文件到 TArray<UInt8>
 *   - FPlatformFile::WriteAllBytes: 一次性写入整个文件
 *   - FPlatformFile::GetFileSize: 获取文件大小
 *   - FPlatformFile::Delete: 删除文件
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h,
 *          Core/Containers/FString.h
 *   外部: Windows API (CreateFileA, ReadFile, WriteFile, etc.)
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Containers/FString.h"

// Windows 文件 API 前向声明
#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    // CreateFileA
    void* __stdcall CreateFileA(
        const char* lpFileName,
        unsigned long dwDesiredAccess,
        unsigned long dwShareMode,
        void* lpSecurityAttributes,
        unsigned long dwCreationDisposition,
        unsigned long dwFlagsAndAttributes,
        void* hTemplateFile);

    // ReadFile
    int __stdcall ReadFile(
        void* hFile,
        void* lpBuffer,
        unsigned long nNumberOfBytesToRead,
        unsigned long* lpNumberOfBytesRead,
        void* lpOverlapped);

    // WriteFile
    int __stdcall WriteFile(
        void* hFile,
        const void* lpBuffer,
        unsigned long nNumberOfBytesToWrite,
        unsigned long* lpNumberOfBytesWritten,
        void* lpOverlapped);

    // CloseHandle
    int __stdcall CloseHandle(void* hObject);

    // GetFileSize
    unsigned long __stdcall GetFileSize(void* hFile,
                                         unsigned long* lpFileSizeHigh);

    // SetFilePointer
    unsigned long __stdcall SetFilePointer(void* hFile,
                                            long lDistanceToMove,
                                            long* lpDistanceToMoveHigh,
                                            unsigned long dwMoveMethod);

    // GetFileAttributesA
    unsigned long __stdcall GetFileAttributesA(const char* lpFileName);

    // CreateDirectoryA
    int __stdcall CreateDirectoryA(const char* lpPathName,
                                    void* lpSecurityAttributes);

    // DeleteFileA
    int __stdcall DeleteFileA(const char* lpFileName);

    // RemoveDirectoryA
    int __stdcall RemoveDirectoryA(const char* lpPathName);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

// Windows 常量 — 避免包含 Windows.h
namespace PlatformFileConstants
{
    // 访问权限
    static constexpr unsigned long kGenericRead    = 0x80000000UL;
    static constexpr unsigned long kGenericWrite   = 0x40000000UL;

    // 共享模式
    static constexpr unsigned long kFileShareRead  = 0x00000001UL;

    // 创建方式
    static constexpr unsigned long kCreateAlways   = 2;
    static constexpr unsigned long kOpenExisting   = 3;
    static constexpr unsigned long kOpenAlways     = 4;

    // 属性
    static constexpr unsigned long kFileAttributeNormal  = 0x00000080UL;
    static constexpr unsigned long kInvalidFileAttributes = 0xFFFFFFFFUL;
    static constexpr unsigned long kFileAttributeDirectory = 0x00000010UL;

    // 无效句柄 — INVALID_HANDLE_VALUE = (void*)(intptr_t)-1
    // reinterpret_cast 不允许在 constexpr 中使用
    static inline void* const kInvalidHandle =
        reinterpret_cast<void*>(static_cast<long long>(-1));

    // 文件指针移动方式
    static constexpr unsigned long kFileBegin   = 0;
    static constexpr unsigned long kFileCurrent = 1;
    static constexpr unsigned long kFileEnd     = 2;
}

/// 文件打开模式
enum class FileOpenMode : UInt8
{
    Read   = 0,  ///< 只读
    Write  = 1,  ///< 写入 (覆盖)
    Append = 2   ///< 追加
};

/// RAII 文件句柄
class FFileHandle
{
public:
    FFileHandle()
        : m_Handle(PlatformFileConstants::kInvalidHandle)
    {
    }

    ~FFileHandle()
    {
        Close();
    }

    // 不可拷贝
    FFileHandle(const FFileHandle&) = delete;
    FFileHandle& operator=(const FFileHandle&) = delete;

    // 可移动
    FFileHandle(FFileHandle&& other) noexcept
        : m_Handle(other.m_Handle)
    {
        other.m_Handle = PlatformFileConstants::kInvalidHandle;
    }

    FFileHandle& operator=(FFileHandle&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            m_Handle = other.m_Handle;
            other.m_Handle = PlatformFileConstants::kInvalidHandle;
        }
        return *this;
    }

    /// 是否有效
    LIMX_NODISCARD bool IsValid() const
    {
        return m_Handle != PlatformFileConstants::kInvalidHandle &&
               m_Handle != nullptr;
    }

    /// 读取数据到缓冲区 — 返回实际读取的字节数
    SizeType Read(UInt8* buffer, SizeType bytesToRead)
    {
        if (!IsValid())
        {
            return 0;
        }
#if LIMX_PLATFORM_WINDOWS
        unsigned long bytesRead = 0;
        ReadFile(m_Handle, buffer,
                 static_cast<unsigned long>(bytesToRead),
                 &bytesRead, nullptr);
        return static_cast<SizeType>(bytesRead);
#else
        return 0;
#endif
    }

    /// 写入数据 — 返回实际写入的字节数
    SizeType Write(const UInt8* buffer, SizeType bytesToWrite)
    {
        if (!IsValid())
        {
            return 0;
        }
#if LIMX_PLATFORM_WINDOWS
        unsigned long bytesWritten = 0;
        WriteFile(m_Handle, buffer,
                  static_cast<unsigned long>(bytesToWrite),
                  &bytesWritten, nullptr);
        return static_cast<SizeType>(bytesWritten);
#else
        return 0;
#endif
    }

    /// 获取文件大小
    LIMX_NODISCARD Int64 GetSize() const
    {
        if (!IsValid())
        {
            return -1;
        }
#if LIMX_PLATFORM_WINDOWS
        unsigned long high = 0;
        unsigned long low = GetFileSize(m_Handle, &high);
        return static_cast<Int64>(low) |
               (static_cast<Int64>(high) << 32);
#else
        return -1;
#endif
    }

    /// 关闭文件
    void Close()
    {
        if (IsValid())
        {
#if LIMX_PLATFORM_WINDOWS
            CloseHandle(m_Handle);
#endif
            m_Handle = PlatformFileConstants::kInvalidHandle;
        }
    }

    /// 获取原生句柄
    LIMX_NODISCARD void* GetNativeHandle() const { return m_Handle; }

private:
    friend struct FPlatformFile;

    explicit FFileHandle(void* handle) : m_Handle(handle) {}

    void* m_Handle;
};

/// 平台文件操作 — 静态工具类
struct FPlatformFile
{
    // ========================================================================
    // 文件打开
    // ========================================================================

    /// 打开文件
    LIMX_NODISCARD static FFileHandle Open(const FString& path,
                                            FileOpenMode mode)
    {
#if LIMX_PLATFORM_WINDOWS
        unsigned long access = 0;
        unsigned long creation = 0;

        switch (mode)
        {
        case FileOpenMode::Read:
            access = PlatformFileConstants::kGenericRead;
            creation = PlatformFileConstants::kOpenExisting;
            break;
        case FileOpenMode::Write:
            access = PlatformFileConstants::kGenericWrite;
            creation = PlatformFileConstants::kCreateAlways;
            break;
        case FileOpenMode::Append:
            access = PlatformFileConstants::kGenericWrite;
            creation = PlatformFileConstants::kOpenAlways;
            break;
        }

        void* handle = CreateFileA(
            path.GetCStr(),
            access,
            PlatformFileConstants::kFileShareRead,
            nullptr,
            creation,
            PlatformFileConstants::kFileAttributeNormal,
            nullptr);

        if (mode == FileOpenMode::Append && handle !=
            PlatformFileConstants::kInvalidHandle)
        {
            SetFilePointer(handle, 0, nullptr,
                           PlatformFileConstants::kFileEnd);
        }

        return FFileHandle(handle);
#else
        return FFileHandle();
#endif
    }

    // ========================================================================
    // 便捷读写
    // ========================================================================

    /// 一次性读取整个文件到字节数组
    LIMX_NODISCARD static TArray<UInt8> ReadAllBytes(const FString& path)
    {
        TArray<UInt8> result;
        FFileHandle file = Open(path, FileOpenMode::Read);
        if (!file.IsValid())
        {
            return result;
        }

        Int64 fileSize = file.GetSize();
        if (fileSize <= 0)
        {
            return result;
        }

        result.Reserve(static_cast<SizeType>(fileSize));
        // 直接调整大小并读入
        for (Int64 remaining = fileSize; remaining > 0; )
        {
            UInt8 buffer[4096];
            SizeType toRead = remaining > 4096
                ? 4096
                : static_cast<SizeType>(remaining);
            SizeType bytesRead = file.Read(buffer, toRead);
            if (bytesRead == 0)
            {
                break;
            }
            for (SizeType index = 0; index < bytesRead; ++index)
            {
                result.Add(buffer[index]);
            }
            remaining -= static_cast<Int64>(bytesRead);
        }

        return result;
    }

    /// 一次性读取整个文件为字符串
    LIMX_NODISCARD static FString ReadAllText(const FString& path)
    {
        TArray<UInt8> bytes = ReadAllBytes(path);
        if (bytes.IsEmpty())
        {
            return FString();
        }
        return FString(reinterpret_cast<const AnsiChar*>(bytes.GetData()),
                       bytes.GetSize());
    }

    /// 一次性写入字节数组到文件
    static bool WriteAllBytes(const FString& path,
                               const UInt8* data,
                               SizeType size)
    {
        FFileHandle file = Open(path, FileOpenMode::Write);
        if (!file.IsValid())
        {
            return false;
        }
        SizeType written = file.Write(data, size);
        return written == size;
    }

    /// 一次性写入字符串到文件
    static bool WriteAllText(const FString& path, const FString& text)
    {
        return WriteAllBytes(path,
                             reinterpret_cast<const UInt8*>(text.GetCStr()),
                             text.GetLength());
    }

    // ========================================================================
    // 文件系统查询
    // ========================================================================

    /// 文件或目录是否存在
    LIMX_NODISCARD static bool Exists(const FString& path)
    {
#if LIMX_PLATFORM_WINDOWS
        unsigned long attrs = GetFileAttributesA(path.GetCStr());
        return attrs != PlatformFileConstants::kInvalidFileAttributes;
#else
        return false;
#endif
    }

    /// 是否为目录
    LIMX_NODISCARD static bool IsDirectory(const FString& path)
    {
#if LIMX_PLATFORM_WINDOWS
        unsigned long attrs = GetFileAttributesA(path.GetCStr());
        if (attrs == PlatformFileConstants::kInvalidFileAttributes)
        {
            return false;
        }
        return (attrs & PlatformFileConstants::kFileAttributeDirectory) != 0;
#else
        return false;
#endif
    }

    /// 获取文件大小 (不打开文件)
    LIMX_NODISCARD static Int64 GetFileSize(const FString& path)
    {
        FFileHandle file = Open(path, FileOpenMode::Read);
        if (!file.IsValid())
        {
            return -1;
        }
        return file.GetSize();
    }

    // ========================================================================
    // 文件系统修改
    // ========================================================================

    /// 创建目录 (单层)
    static bool CreateDirectory(const FString& path)
    {
#if LIMX_PLATFORM_WINDOWS
        return CreateDirectoryA(path.GetCStr(), nullptr) != 0;
#else
        return false;
#endif
    }

    /// 递归创建目录
    static bool CreateDirectoryTree(const FString& path)
    {
        if (path.IsEmpty())
        {
            return false;
        }
        if (Exists(path))
        {
            return true;
        }

        // 查找父目录并递归
        SizeType lastSep = FindLastSeparator(path);
        if (lastSep != FString::kNPos && lastSep > 0)
        {
            FString parent = path.Substring(0, lastSep);
            if (!parent.IsEmpty() && !Exists(parent))
            {
                if (!CreateDirectoryTree(parent))
                {
                    return false;
                }
            }
        }

        return CreateDirectory(path);
    }

    /// 删除文件
    static bool DeleteFile(const FString& path)
    {
#if LIMX_PLATFORM_WINDOWS
        return DeleteFileA(path.GetCStr()) != 0;
#else
        return false;
#endif
    }

    /// 删除空目录
    static bool RemoveDirectory(const FString& path)
    {
#if LIMX_PLATFORM_WINDOWS
        return RemoveDirectoryA(path.GetCStr()) != 0;
#else
        return false;
#endif
    }

private:
    static SizeType FindLastSeparator(const FString& path)
    {
        SizeType result = FString::kNPos;
        for (SizeType index = 0; index < path.GetLength(); ++index)
        {
            AnsiChar ch = path[index];
            if (ch == '/' || ch == '\\')
            {
                result = index;
            }
        }
        return result;
    }
};

} // namespace Limx
