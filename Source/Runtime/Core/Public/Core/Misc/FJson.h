/*******************************************************************************
 * 文件: FJson.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   零 STL JSON 解析器 — 符合 RFC 8259 的只读文档模型
 *   解析结果为扁平节点池，FJsonValue 是指向节点的轻量视图
 *   面向 glTF 等结构化资产格式的随机访问需求设计
 *
 * 设计哲学:
 *   节点池而非递归类型 — 直觉写法是让 FJsonValue 内含 TArray<FJsonValue>，
 *   但那要求容器支持不完整类型（标准未保证），且每层嵌套都产生独立堆分配。
 *   改用扁平节点数组 + UInt32 索引互指：解析后全部节点连续存储，遍历缓存友好，
 *   且天然规避了不完整类型问题。
 *
 *   视图与所有权分离 — FJsonDocument 拥有节点池与字符串池，FJsonValue 只是
 *   (文档指针, 节点索引) 的二元组，按值传递零成本。这使得深层取值可以写成
 *   链式调用而不产生任何拷贝。
 *
 *   错误必须可定位 — 解析失败时记录行号、列号与具体原因。面对一个几百 KB 的
 *   glTF，"解析失败"这四个字毫无用处，必须精确到出错位置。
 *
 * 技术特性:
 *   - 单遍解析, 无回溯; 时间 O(n), 空间与节点数成正比
 *   - 字符串统一解码入连续池, 转义序列 (含 \uXXXX 与代理对) 转为 UTF-8
 *   - 数字按 尾数 + 十进制指数 解析, 保留至多 19 位有效数字
 *   - 深度上限防护, 拒绝恶意构造的深层嵌套导致的栈溢出
 *
 * 依赖关系:
 *   内部: Core/CoreMinimal.h, Core/Containers/TArray.h,
 *          Core/Containers/FString.h
 *
 * 注意事项:
 *   文档为只读模型 — 不支持修改或序列化输出
 *   FJsonValue 的生命周期不得超过其来源 FJsonDocument
 *   数字解析非正确舍入 — 极端精度场景 (超过 17 位有效数字) 可能有 1 ULP 级误差
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Containers/TArray.h"
#include "Core/Containers/FString.h"

namespace Limx
{

class FJsonDocument;

// ============================================================================
// EJsonType — JSON 值类型
// ============================================================================

/// JSON 值的六种类型
enum class EJsonType : UInt8
{
    /// 无效值 — 越界访问或查找失败时返回
    Invalid = 0,

    Null    = 1,
    Bool    = 2,
    Number  = 3,
    String  = 4,
    Array   = 5,
    Object  = 6,
};

// ============================================================================
// FJsonValue — 指向文档中一个节点的轻量视图
// ============================================================================

/// JSON 值视图
///
/// 按值传递零成本。所有访问器在类型不匹配时返回调用方给定的默认值而非断言，
/// 使得读取可选字段无需先做类型判断。
class LIMX_CORE_API FJsonValue
{
public:
    /// 无效节点索引
    static constexpr UInt32 kInvalidNode = 0xFFFFFFFFu;

    /// 构造无效值
    FJsonValue() = default;

    /// 由文档与节点索引构造 — 通常不直接调用
    FJsonValue(const FJsonDocument* document, UInt32 nodeIndex)
        : m_Document(document)
        , m_NodeIndex(nodeIndex)
    {
    }

    // ========================================================================
    // 类型
    // ========================================================================

    LIMX_NODISCARD EJsonType GetType() const;

    LIMX_NODISCARD bool IsValid() const  { return GetType() != EJsonType::Invalid; }
    LIMX_NODISCARD bool IsNull() const   { return GetType() == EJsonType::Null; }
    LIMX_NODISCARD bool IsBool() const   { return GetType() == EJsonType::Bool; }
    LIMX_NODISCARD bool IsNumber() const { return GetType() == EJsonType::Number; }
    LIMX_NODISCARD bool IsString() const { return GetType() == EJsonType::String; }
    LIMX_NODISCARD bool IsArray() const  { return GetType() == EJsonType::Array; }
    LIMX_NODISCARD bool IsObject() const { return GetType() == EJsonType::Object; }

    // ========================================================================
    // 标量取值 — 类型不符时返回默认值
    // ========================================================================

    LIMX_NODISCARD bool AsBool(bool defaultValue = false) const;

    LIMX_NODISCARD Float64 AsDouble(Float64 defaultValue = 0.0) const;

    LIMX_NODISCARD Float32 AsFloat(Float32 defaultValue = 0.0f) const
    {
        return static_cast<Float32>(AsDouble(static_cast<Float64>(defaultValue)));
    }

    LIMX_NODISCARD Int64 AsInt64(Int64 defaultValue = 0) const;

    LIMX_NODISCARD Int32 AsInt32(Int32 defaultValue = 0) const
    {
        return static_cast<Int32>(AsInt64(static_cast<Int64>(defaultValue)));
    }

    LIMX_NODISCARD UInt32 AsUInt32(UInt32 defaultValue = 0) const
    {
        const Int64 value = AsInt64(static_cast<Int64>(defaultValue));
        return (value < 0) ? defaultValue : static_cast<UInt32>(value);
    }

    /// 字符串内容 — 指向文档字符串池, 生命周期同文档
    LIMX_NODISCARD const AnsiChar* AsString(
        const AnsiChar* defaultValue = "") const;

    /// 字符串字节长度 — 非字符串返回 0
    LIMX_NODISCARD SizeType GetStringLength() const;

    // ========================================================================
    // 数组
    // ========================================================================

    /// 元素个数 — 非数组返回 0
    LIMX_NODISCARD SizeType GetArraySize() const;

    /// 按下标取元素 — 越界返回无效值
    LIMX_NODISCARD FJsonValue operator[](SizeType index) const;

    // ========================================================================
    // 对象
    // ========================================================================

    /// 成员个数 — 非对象返回 0
    LIMX_NODISCARD SizeType GetMemberCount() const;

    /// 按名查找成员 — 未找到返回无效值
    LIMX_NODISCARD FJsonValue operator[](const AnsiChar* name) const;

    /// 是否存在该成员
    LIMX_NODISCARD bool HasMember(const AnsiChar* name) const;

    /// 按下标取成员名 — 越界返回 nullptr
    LIMX_NODISCARD const AnsiChar* GetMemberName(SizeType index) const;

    /// 按下标取成员值 — 越界返回无效值
    LIMX_NODISCARD FJsonValue GetMemberValue(SizeType index) const;

    // ========================================================================
    // 便捷字段取值 — 等价于先查找成员再取标量
    // ========================================================================

    LIMX_NODISCARD bool GetBoolField(const AnsiChar* name,
                                     bool defaultValue = false) const
    {
        return (*this)[name].AsBool(defaultValue);
    }

    LIMX_NODISCARD Float64 GetDoubleField(const AnsiChar* name,
                                          Float64 defaultValue = 0.0) const
    {
        return (*this)[name].AsDouble(defaultValue);
    }

    LIMX_NODISCARD Float32 GetFloatField(const AnsiChar* name,
                                         Float32 defaultValue = 0.0f) const
    {
        return (*this)[name].AsFloat(defaultValue);
    }

    LIMX_NODISCARD Int32 GetInt32Field(const AnsiChar* name,
                                       Int32 defaultValue = 0) const
    {
        return (*this)[name].AsInt32(defaultValue);
    }

    LIMX_NODISCARD UInt32 GetUInt32Field(const AnsiChar* name,
                                         UInt32 defaultValue = 0) const
    {
        return (*this)[name].AsUInt32(defaultValue);
    }

    LIMX_NODISCARD const AnsiChar* GetStringField(
        const AnsiChar* name, const AnsiChar* defaultValue = "") const
    {
        return (*this)[name].AsString(defaultValue);
    }

private:
    /// 所属文档 — 不拥有
    const FJsonDocument* m_Document = nullptr;

    /// 节点索引
    UInt32 m_NodeIndex = kInvalidNode;
};

// ============================================================================
// FJsonDocument — 拥有节点池与字符串池的文档
// ============================================================================

/// JSON 文档
class LIMX_CORE_API FJsonDocument
{
public:
    /// 嵌套深度上限 — 防止恶意构造的深层嵌套耗尽解析栈
    static constexpr UInt32 kMaxDepth = 256;

    FJsonDocument() = default;
    ~FJsonDocument() = default;

    FJsonDocument(const FJsonDocument&)            = delete;
    FJsonDocument& operator=(const FJsonDocument&) = delete;

    // ========================================================================
    // 解析
    // ========================================================================

    /// 解析 JSON 文本
    /// @param text   UTF-8 文本, 无需以 '\0' 结尾
    /// @param length 字节长度
    /// @return 解析是否成功; 失败时用 GetError 系列获取诊断
    bool Parse(const AnsiChar* text, SizeType length);

    /// 解析以 '\0' 结尾的 JSON 文本
    bool Parse(const AnsiChar* text);

    /// 清空文档
    void Reset();

    // ========================================================================
    // 结果
    // ========================================================================

    /// 根值 — 解析失败时为无效值
    LIMX_NODISCARD FJsonValue GetRoot() const;

    /// 是否已成功解析出内容
    LIMX_NODISCARD bool IsValid() const { return m_Nodes.GetSize() > 0; }

    // ========================================================================
    // 诊断
    // ========================================================================

    /// 错误描述 — 成功时为空串
    LIMX_NODISCARD const FString& GetErrorMessage() const { return m_ErrorMessage; }

    /// 出错行号 (从 1 起) — 成功时为 0
    LIMX_NODISCARD UInt32 GetErrorLine() const { return m_ErrorLine; }

    /// 出错列号 (从 1 起) — 成功时为 0
    LIMX_NODISCARD UInt32 GetErrorColumn() const { return m_ErrorColumn; }

    /// 节点总数 — 用于诊断与容量评估
    LIMX_NODISCARD SizeType GetNodeCount() const { return m_Nodes.GetSize(); }

private:
    friend class FJsonValue;

    // ========================================================================
    // 节点
    // ========================================================================

    /// 扁平节点
    ///
    /// 子节点索引存放在独立的索引池中而非直接用节点池的连续区间 ——
    /// 嵌套容器会把自己的子节点追加到节点池尾部，导致父容器的子节点
    /// 在节点池里并不相邻（例如 [1,[2],3] 的三个元素分别是节点 1、2、4）。
    /// 索引池让父节点只需记录一段连续的索引区间即可。
    struct FNode
    {
        EJsonType Type = EJsonType::Invalid;

        /// 布尔值
        bool BoolValue = false;

        /// 数值
        Float64 NumberValue = 0.0;

        /// 字符串在池中的偏移与长度 (String 类型)
        UInt32 StringOffset = 0;
        UInt32 StringLength = 0;

        /// 成员名在池中的偏移与长度 (对象成员)
        UInt32 KeyOffset = 0;
        UInt32 KeyLength = 0;

        /// 子节点索引在索引池中的起始位置 (Array / Object)
        UInt32 ChildStart = 0;

        /// 子节点个数 (Array / Object)
        UInt32 ChildCount = 0;
    };

    /// 取节点 — 越界返回 nullptr
    LIMX_NODISCARD const FNode* GetNode(UInt32 index) const;

    /// 取字符串池中的内容
    LIMX_NODISCARD const AnsiChar* GetPooledString(UInt32 offset) const;

    // ========================================================================
    // 解析状态与实现
    // ========================================================================

    /// 解析器内部状态
    struct FParseContext
    {
        const AnsiChar* Text   = nullptr;
        SizeType        Length = 0;
        SizeType        Cursor = 0;
        UInt32          Line   = 1;
        UInt32          Column = 1;
        UInt32          Depth  = 0;
    };

    void SkipWhitespace(FParseContext& context);

    bool ParseValue(FParseContext& context, UInt32& outNodeIndex);
    bool ParseObject(FParseContext& context, UInt32 nodeIndex);
    bool ParseArray(FParseContext& context, UInt32 nodeIndex);
    bool ParseString(FParseContext& context, UInt32& outOffset,
                     UInt32& outLength);
    bool ParseNumber(FParseContext& context, Float64& outValue);
    bool ParseLiteral(FParseContext& context, const AnsiChar* literal);

    /// 解析 \uXXXX 转义并以 UTF-8 追加到字符串池
    bool ParseUnicodeEscape(FParseContext& context);

    /// 将码点以 UTF-8 编码追加到字符串池
    void AppendUtf8(UInt32 codePoint);

    /// 向字符串池追加一个字节
    void AppendPoolByte(AnsiChar byte);

    /// 记录错误
    bool SetError(const FParseContext& context, const AnsiChar* message);

    // ========================================================================
    // 成员数据
    // ========================================================================

    /// 节点池
    TArray<FNode> m_Nodes;

    /// 字符串池 — 连续存储, 各字符串以 '\0' 分隔
    /// 子节点索引池 — 每个容器节点占据其中一段连续区间
    TArray<UInt32> m_ChildIndices;

    /// 解析期的子节点收集栈
    ///
    /// 容器解析开始时记录当前栈顶，子节点索引依次压入；容器解析结束时把
    /// [栈顶, 末尾) 一段整体搬入 m_ChildIndices 并回退栈顶。这样嵌套解析
    /// 共用同一块缓冲区，不为每层容器单独分配。
    TArray<UInt32> m_ScratchChildren;

    TArray<AnsiChar> m_StringPool;

    FString m_ErrorMessage;
    UInt32  m_ErrorLine   = 0;
    UInt32  m_ErrorColumn = 0;
};

} // namespace Limx
