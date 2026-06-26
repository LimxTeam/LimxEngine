/*******************************************************************************
 * 文件: FConfigFile.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   INI 配置文件读写 — 支持 [Section] Key=Value 格式
 *   提供内存中的配置数据管理，支持从文件加载和保存
 *   用于引擎配置、项目设置、用户偏好等持久化场景
 *
 * 设计哲学:
 *   节-键-值三层结构 — 符合 INI 标准，简单直观
 *   内存优先 — 加载到内存后随机读写，按需持久化
 *   类型安全访问 — 提供 GetString/GetInt/GetFloat/GetBool 等类型化方法
 *
 * 技术特性:
 *   - Load(path): 从文件加载 INI 配置
 *   - Save(path): 保存 INI 配置到文件
 *   - GetString/SetString: 字符串键值
 *   - GetInt/SetInt: 整数键值
 *   - GetFloat/SetFloat: 浮点键值
 *   - GetBool/SetBool: 布尔键值
 *   - HasSection/HasKey: 存在性查询
 *   - 注释: 以 ';' 或 '#' 开头的行视为注释
 *
 * 依赖关系:
 *   内部: Core/Containers/FString.h, Core/Containers/TArray.h,
 *          Core/Templates/TPair.h, Core/Containers/TMap.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TPair.h"
#include "Core/Containers/TMap.h"

namespace Limx
{

/// INI 配置文件
class FConfigFile
{
    /// 配置节 — 包含键值对列表
    struct ConfigSection
    {
        FString                         Name;    ///< 节名
        TArray<TPair<FString, FString>> Entries; ///< 键值对

        /// 查找键的索引 (-1 表示未找到)
        Int32 FindKey(const FString& key) const
        {
            for (SizeType index = 0;
                 index < Entries.GetSize(); ++index)
            {
                if (Entries[index].First == key)
                {
                    return static_cast<Int32>(index);
                }
            }
            return -1;
        }
    };

public:
    // ========================================================================
    // 构造
    // ========================================================================

    FConfigFile() = default;
    ~FConfigFile() = default;

    // ========================================================================
    // 解析 — 从字符串内容加载
    // ========================================================================

    /// 从 INI 文本内容解析
    void ParseFromString(const FString& content)
    {
        m_Sections.Clear();

        FString currentSection;
        const AnsiChar* data = content.GetCStr();
        SizeType length = content.GetLength();
        SizeType lineStart = 0;

        while (lineStart < length)
        {
            // 查找行尾
            SizeType lineEnd = lineStart;
            while (lineEnd < length &&
                   data[lineEnd] != '\n' && data[lineEnd] != '\r')
            {
                ++lineEnd;
            }

            // 提取行内容
            SizeType lineLength = lineEnd - lineStart;
            if (lineLength > 0)
            {
                FString line(data + lineStart, lineLength);
                line = Trim(line);

                if (!line.IsEmpty())
                {
                    ParseLine(line, currentSection);
                }
            }

            // 跳过换行符
            lineStart = lineEnd;
            if (lineStart < length && data[lineStart] == '\r')
            {
                ++lineStart;
            }
            if (lineStart < length && data[lineStart] == '\n')
            {
                ++lineStart;
            }
        }
    }

    /// 序列化为 INI 文本
    LIMX_NODISCARD FString SerializeToString() const
    {
        FString result;
        for (SizeType sectionIndex = 0;
             sectionIndex < m_Sections.GetSize(); ++sectionIndex)
        {
            const ConfigSection& section = m_Sections[sectionIndex];

            // 节头
            if (!section.Name.IsEmpty())
            {
                result = result + FString("[") +
                         section.Name + FString("]\n");
            }

            // 键值对
            for (SizeType entryIndex = 0;
                 entryIndex < section.Entries.GetSize(); ++entryIndex)
            {
                const auto& pair = section.Entries[entryIndex];
                result = result + pair.First + FString("=") +
                         pair.Second + FString("\n");
            }

            // 节间空行
            if (sectionIndex + 1 < m_Sections.GetSize())
            {
                result = result + FString("\n");
            }
        }
        return result;
    }

    // ========================================================================
    // 字符串读写
    // ========================================================================

    /// 获取字符串值
    LIMX_NODISCARD FString GetString(const AnsiChar* section,
                                       const AnsiChar* key,
                                       const AnsiChar* defaultValue = "") const
    {
        const ConfigSection* sec = FindSection(FString(section));
        if (sec)
        {
            Int32 keyIndex = sec->FindKey(FString(key));
            if (keyIndex >= 0)
            {
                return sec->Entries[static_cast<SizeType>(keyIndex)].Second;
            }
        }
        return FString(defaultValue);
    }

    /// 设置字符串值
    void SetString(const AnsiChar* section,
                   const AnsiChar* key,
                   const AnsiChar* value)
    {
        ConfigSection& sec = FindOrCreateSection(FString(section));
        FString keyStr(key);
        Int32 keyIndex = sec.FindKey(keyStr);
        if (keyIndex >= 0)
        {
            sec.Entries[static_cast<SizeType>(keyIndex)].Second =
                FString(value);
        }
        else
        {
            sec.Entries.Add(MakePair(MoveTemp(keyStr), FString(value)));
        }
    }

    // ========================================================================
    // 类型化读写
    // ========================================================================

    /// 获取整数值
    LIMX_NODISCARD Int64 GetInt(const AnsiChar* section,
                                 const AnsiChar* key,
                                 Int64 defaultValue = 0) const
    {
        FString str = GetString(section, key);
        if (str.IsEmpty())
        {
            return defaultValue;
        }
        return ParseInt64(str);
    }

    /// 设置整数值
    void SetInt(const AnsiChar* section,
                const AnsiChar* key,
                Int64 value)
    {
        // 手动转换为字符串
        AnsiChar buffer[22];
        Int64ToStr(value, buffer);
        SetString(section, key, buffer);
    }

    /// 获取浮点值
    LIMX_NODISCARD Float64 GetFloat(const AnsiChar* section,
                                      const AnsiChar* key,
                                      Float64 defaultValue = 0.0) const
    {
        FString str = GetString(section, key);
        if (str.IsEmpty())
        {
            return defaultValue;
        }
        return ParseFloat64(str);
    }

    /// 获取布尔值
    LIMX_NODISCARD bool GetBool(const AnsiChar* section,
                                  const AnsiChar* key,
                                  bool defaultValue = false) const
    {
        FString str = GetString(section, key);
        if (str.IsEmpty())
        {
            return defaultValue;
        }
        // "true", "1", "yes", "on" → true
        if (str == FString("true") || str == FString("1") ||
            str == FString("yes") || str == FString("on") ||
            str == FString("True") || str == FString("Yes") ||
            str == FString("On") || str == FString("TRUE"))
        {
            return true;
        }
        return false;
    }

    /// 设置布尔值
    void SetBool(const AnsiChar* section,
                 const AnsiChar* key,
                 bool value)
    {
        SetString(section, key, value ? "true" : "false");
    }

    // ========================================================================
    // 存在性查询
    // ========================================================================

    /// 检查节是否存在
    LIMX_NODISCARD bool HasSection(const AnsiChar* section) const
    {
        return FindSection(FString(section)) != nullptr;
    }

    /// 检查键是否存在
    LIMX_NODISCARD bool HasKey(const AnsiChar* section,
                                 const AnsiChar* key) const
    {
        const ConfigSection* sec = FindSection(FString(section));
        if (sec)
        {
            return sec->FindKey(FString(key)) >= 0;
        }
        return false;
    }

    /// 清空所有配置
    void Clear()
    {
        m_Sections.Clear();
    }

private:
    // ========================================================================
    // 内部辅助
    // ========================================================================

    /// 解析单行
    void ParseLine(const FString& line, FString& currentSection)
    {
        const AnsiChar* str = line.GetCStr();
        SizeType length = line.GetLength();

        // 注释行
        if (str[0] == ';' || str[0] == '#')
        {
            return;
        }

        // 节头 [SectionName]
        if (str[0] == '[' && length > 2 && str[length - 1] == ']')
        {
            currentSection = FString(str + 1, length - 2);
            FindOrCreateSection(currentSection);
            return;
        }

        // 键值对 Key=Value
        SizeType equalPos = 0;
        bool hasEqual = false;
        for (SizeType index = 0; index < length; ++index)
        {
            if (str[index] == '=')
            {
                equalPos = index;
                hasEqual = true;
                break;
            }
        }

        if (hasEqual && equalPos > 0)
        {
            FString key = Trim(FString(str, equalPos));
            FString value = Trim(FString(str + equalPos + 1,
                                          length - equalPos - 1));
            ConfigSection& sec = FindOrCreateSection(currentSection);
            Int32 keyIndex = sec.FindKey(key);
            if (keyIndex >= 0)
            {
                sec.Entries[static_cast<SizeType>(keyIndex)].Second =
                    MoveTemp(value);
            }
            else
            {
                sec.Entries.Add(
                    MakePair(MoveTemp(key), MoveTemp(value)));
            }
        }
    }

    /// 查找节 (只读)
    const ConfigSection* FindSection(const FString& name) const
    {
        for (SizeType index = 0;
             index < m_Sections.GetSize(); ++index)
        {
            if (m_Sections[index].Name == name)
            {
                return &m_Sections[index];
            }
        }
        return nullptr;
    }

    /// 查找或创建节
    ConfigSection& FindOrCreateSection(const FString& name)
    {
        for (SizeType index = 0;
             index < m_Sections.GetSize(); ++index)
        {
            if (m_Sections[index].Name == name)
            {
                return m_Sections[index];
            }
        }
        ConfigSection newSection;
        newSection.Name = name;
        m_Sections.Add(MoveTemp(newSection));
        return m_Sections[m_Sections.GetSize() - 1];
    }

    /// 去除首尾空白
    static FString Trim(const FString& str)
    {
        const AnsiChar* data = str.GetCStr();
        SizeType length = str.GetLength();
        SizeType start = 0;
        SizeType end = length;

        while (start < end &&
               (data[start] == ' ' || data[start] == '\t'))
        {
            ++start;
        }
        while (end > start &&
               (data[end - 1] == ' ' || data[end - 1] == '\t'))
        {
            --end;
        }

        if (start == 0 && end == length)
        {
            return str;
        }
        return FString(data + start, end - start);
    }

    /// 解析 Int64
    static Int64 ParseInt64(const FString& str)
    {
        const AnsiChar* data = str.GetCStr();
        Int64 result = 0;
        bool isNegative = false;
        SizeType index = 0;

        if (data[0] == '-')
        {
            isNegative = true;
            index = 1;
        }
        else if (data[0] == '+')
        {
            index = 1;
        }

        while (data[index] >= '0' && data[index] <= '9')
        {
            result = result * 10 +
                     static_cast<Int64>(data[index] - '0');
            ++index;
        }

        return isNegative ? -result : result;
    }

    /// 解析 Float64 (简化实现)
    static Float64 ParseFloat64(const FString& str)
    {
        const AnsiChar* data = str.GetCStr();
        Float64 result = 0.0;
        Float64 sign = 1.0;
        SizeType index = 0;

        if (data[0] == '-')
        {
            sign = -1.0;
            index = 1;
        }
        else if (data[0] == '+')
        {
            index = 1;
        }

        // 整数部分
        while (data[index] >= '0' && data[index] <= '9')
        {
            result = result * 10.0 +
                     static_cast<Float64>(data[index] - '0');
            ++index;
        }

        // 小数部分
        if (data[index] == '.')
        {
            ++index;
            Float64 fraction = 0.1;
            while (data[index] >= '0' && data[index] <= '9')
            {
                result += static_cast<Float64>(data[index] - '0') *
                          fraction;
                fraction *= 0.1;
                ++index;
            }
        }

        return result * sign;
    }

    /// Int64 转字符串
    static void Int64ToStr(Int64 value, AnsiChar* buffer)
    {
        AnsiChar digits[22];
        Int32 digitCount = 0;
        bool isNegative = false;

        if (value < 0)
        {
            isNegative = true;
            UInt64 absValue = static_cast<UInt64>(-(value + 1)) + 1;
            do
            {
                digits[digitCount++] =
                    static_cast<AnsiChar>('0' + absValue % 10);
                absValue /= 10;
            } while (absValue > 0);
        }
        else
        {
            UInt64 uValue = static_cast<UInt64>(value);
            do
            {
                digits[digitCount++] =
                    static_cast<AnsiChar>('0' + uValue % 10);
                uValue /= 10;
            } while (uValue > 0);
        }

        Int32 offset = 0;
        if (isNegative)
        {
            buffer[offset++] = '-';
        }
        for (Int32 index = digitCount - 1; index >= 0; --index)
        {
            buffer[offset++] = digits[index];
        }
        buffer[offset] = '\0';
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    TArray<ConfigSection> m_Sections;  ///< 配置节列表
};

} // namespace Limx
