/*******************************************************************************
 * 文件: TPropertyAccessor.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   属性访问器 — 类型安全的成员变量偏移量访问
 *   通过编译时偏移量实现对象属性的读写
 *   用于反射系统属性注册、序列化字段映射、编辑器属性绑定等场景
 *
 * 设计哲学:
 *   偏移量驱动 — 存储成员在对象内的字节偏移
 *   类型擦除 — 运行时通过 void* + 偏移量访问任意类型属性
 *   类型安全 — 模板接口保证编译时类型检查
 *
 * 技术特性:
 *   - TPropertyAccessor<Class, PropType>: 类型化属性访问器
 *   - FPropertyOffset: 类型擦除的属性偏移
 *   - LIMX_PROPERTY_OFFSET: 计算成员偏移量宏
 *   - Get/Set: 类型安全读写
 *   - GetRaw/SetRaw: 原始指针读写
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

// ============================================================================
// FPropertyOffset — 类型擦除的属性偏移
// ============================================================================

/// 类型擦除的属性偏移描述
struct FPropertyOffset
{
    SizeType Offset;    ///< 成员在对象内的字节偏移
    SizeType Size;      ///< 属性大小 (字节)
    SizeType Alignment; ///< 属性对齐要求

    constexpr FPropertyOffset()
        : Offset(0), Size(0), Alignment(0) {}

    constexpr FPropertyOffset(SizeType offset, SizeType size,
                               SizeType alignment)
        : Offset(offset), Size(size), Alignment(alignment) {}

    /// 从对象基址获取属性原始指针
    LIMX_NODISCARD void* GetPtr(void* objectPtr) const
    {
        return static_cast<UInt8*>(objectPtr) + Offset;
    }

    /// 从对象基址获取属性只读指针
    LIMX_NODISCARD const void* GetPtr(const void* objectPtr) const
    {
        return static_cast<const UInt8*>(objectPtr) + Offset;
    }

    /// 原始拷贝 — 从属性拷贝到目标
    void CopyTo(const void* objectPtr, void* outValue) const
    {
        Memory::MemCopy(outValue, GetPtr(objectPtr), Size);
    }

    /// 原始拷贝 — 从源拷贝到属性
    void CopyFrom(void* objectPtr, const void* value) const
    {
        Memory::MemCopy(GetPtr(objectPtr), value, Size);
    }

    /// 是否有效
    LIMX_NODISCARD constexpr bool IsValid() const
    {
        return Size > 0;
    }
};

// ============================================================================
// TPropertyAccessor — 类型化属性访问器
// ============================================================================

/// 类型化属性访问器
/// @tparam ClassType 拥有该属性的类
/// @tparam PropType  属性类型
template<typename ClassType, typename PropType>
class TPropertyAccessor
{
public:
    /// 从成员指针构造
    /// 使用 offset-of 技巧获取偏移量
    constexpr TPropertyAccessor()
        : m_Offset(0)
    {
    }

    constexpr explicit TPropertyAccessor(SizeType offset)
        : m_Offset(offset)
    {
    }

    /// 获取属性引用
    LIMX_NODISCARD PropType& Get(ClassType& object) const
    {
        UInt8* basePtr = reinterpret_cast<UInt8*>(&object);
        return *reinterpret_cast<PropType*>(
            basePtr + m_Offset);
    }

    /// 获取属性只读引用
    LIMX_NODISCARD const PropType& Get(
        const ClassType& object) const
    {
        const UInt8* basePtr =
            reinterpret_cast<const UInt8*>(&object);
        return *reinterpret_cast<const PropType*>(
            basePtr + m_Offset);
    }

    /// 设置属性值
    void Set(ClassType& object, const PropType& value) const
    {
        Get(object) = value;
    }

    /// 设置属性值 (移动)
    void Set(ClassType& object, PropType&& value) const
    {
        Get(object) = MoveTemp(value);
    }

    /// 获取属性指针
    LIMX_NODISCARD PropType* GetPtr(ClassType& object) const
    {
        return &Get(object);
    }

    LIMX_NODISCARD const PropType* GetPtr(
        const ClassType& object) const
    {
        return &Get(object);
    }

    /// 获取偏移量
    LIMX_NODISCARD constexpr SizeType GetOffset() const
    {
        return m_Offset;
    }

    /// 获取属性大小
    LIMX_NODISCARD static constexpr SizeType GetPropertySize()
    {
        return sizeof(PropType);
    }

    /// 获取属性对齐
    LIMX_NODISCARD static constexpr SizeType GetPropertyAlignment()
    {
        return alignof(PropType);
    }

    /// 转换为类型擦除的偏移描述
    LIMX_NODISCARD constexpr FPropertyOffset ToPropertyOffset() const
    {
        return FPropertyOffset(
            m_Offset, sizeof(PropType), alignof(PropType));
    }

private:
    SizeType m_Offset;  ///< 成员偏移量
};

/// 创建属性访问器的辅助宏
/// 用法: auto accessor = LIMX_PROPERTY_ACCESSOR(MyClass, MyMember);
#define LIMX_PROPERTY_ACCESSOR(ClassType, MemberName) \
    ::Limx::TPropertyAccessor<ClassType, \
        decltype(ClassType::MemberName)>( \
            offsetof(ClassType, MemberName))

/// 获取成员偏移量宏
#define LIMX_PROPERTY_OFFSET(ClassType, MemberName) \
    ::Limx::FPropertyOffset( \
        offsetof(ClassType, MemberName), \
        sizeof(decltype(ClassType::MemberName)), \
        alignof(decltype(ClassType::MemberName)))

} // namespace Limx
