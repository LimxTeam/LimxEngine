/*******************************************************************************
 * 文件: TVersionedObject.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   版本化对象 — 脏标记 + 版本号的变更追踪包装器
 *   每次修改时自动递增版本号并设置脏标记
 *   用于缓存失效、增量序列化、编辑器撤销/重做等场景
 *
 * 设计哲学:
 *   透明包装 — 通过操作符重载透明地访问内部值
 *   自动追踪 — 任何写操作自动递增版本号
 *   轻量 — 仅额外存储一个 UInt32 版本号和 bool 脏标记
 *
 * 技术特性:
 *   - TVersionedObject<T>: 版本化值包装
 *   - Get/Set: 读写访问
 *   - GetVersion: 获取当前版本号
 *   - IsDirty/ClearDirty: 脏标记管理
 *   - GetMutable: 获取可写引用 (自动标脏)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 版本化对象包装器
/// @tparam T 被包装的值类型
template<typename T>
class TVersionedObject
{
public:
    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造
    TVersionedObject()
        : m_Value()
        , m_Version(0)
        , m_IsDirty(false)
    {
    }

    /// 从值构造
    explicit TVersionedObject(const T& value)
        : m_Value(value)
        , m_Version(0)
        , m_IsDirty(false)
    {
    }

    /// 从值构造 (移动)
    explicit TVersionedObject(T&& value)
        : m_Value(MoveTemp(value))
        , m_Version(0)
        , m_IsDirty(false)
    {
    }

    // ========================================================================
    // 读访问 (不改变版本)
    // ========================================================================

    /// 获取只读引用
    LIMX_NODISCARD const T& Get() const { return m_Value; }

    /// 隐式转换为只读引用
    LIMX_NODISCARD operator const T&() const { return m_Value; }

    /// 箭头操作符 (只读)
    LIMX_NODISCARD const T* operator->() const { return &m_Value; }

    // ========================================================================
    // 写访问 (自动递增版本)
    // ========================================================================

    /// 设置新值
    void Set(const T& newValue)
    {
        m_Value = newValue;
        MarkDirty();
    }

    /// 设置新值 (移动)
    void Set(T&& newValue)
    {
        m_Value = MoveTemp(newValue);
        MarkDirty();
    }

    /// 获取可写引用 — 调用者负责修改后的语义
    /// 自动标记为脏
    LIMX_NODISCARD T& GetMutable()
    {
        MarkDirty();
        return m_Value;
    }

    // ========================================================================
    // 版本与脏标记
    // ========================================================================

    /// 获取当前版本号
    LIMX_NODISCARD UInt32 GetVersion() const { return m_Version; }

    /// 是否为脏 (自上次 ClearDirty 后有修改)
    LIMX_NODISCARD bool IsDirty() const { return m_IsDirty; }

    /// 清除脏标记
    void ClearDirty() { m_IsDirty = false; }

    /// 手动标记为脏 (递增版本号)
    void MarkDirty()
    {
        ++m_Version;
        m_IsDirty = true;
    }

    /// 重置版本号和脏标记
    void ResetVersion()
    {
        m_Version = 0;
        m_IsDirty = false;
    }

private:
    T      m_Value;    ///< 被包装的值
    UInt32 m_Version;  ///< 版本号 (每次修改递增)
    bool   m_IsDirty;  ///< 脏标记
};

} // namespace Limx
