/*******************************************************************************
 * 文件: FJson.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   JSON 解析器实现 — 递归下降解析、字符串转义解码、数字解析、错误定位
 *
 * 设计哲学:
 *   容器子节点两段式收集 — 嵌套容器会把自身子节点追加到节点池尾部，父容器的
 *   子节点因此在节点池中并不相邻。解析容器时先把子节点索引压入共享 scratch 栈，
 *   容器结束时整段搬入索引池。共享栈让任意嵌套深度都只用一块缓冲区。
 *
 *   数字按尾数加指数解析 — 逐位做 result = result * 10 + digit 再除以 10^k
 *   会累积可观误差。改为累积整数尾数并单独记录十进制指数，最后一次性缩放，
 *   在 19 位有效数字内保持接近最优的精度。
 *
 * 技术特性:
 *   - 错误携带行列号, 定位到具体字符
 *   - \uXXXX 转义支持 UTF-16 代理对, 输出 UTF-8
 *   - 深度计数拒绝超过 kMaxDepth 的嵌套, 防止解析栈溢出
 *   - 拒绝尾随内容: 根值之后除空白外的任何字符都视为错误
 *
 * 依赖关系:
 *   内部: Core/Misc/FJson.h, Core/Math/FMath.h
 *
 * 注意事项:
 *   本实现拒绝 JSON5 扩展 (注释、尾逗号、单引号、裸键)，严格遵循 RFC 8259
 *
 ******************************************************************************/

#include "Core/Misc/FJson.h"
#include "Core/Math/FMath.h"

namespace Limx
{

namespace
{

/// 十进制指数的合理范围 — 超出即视为上溢/下溢
constexpr Int32 kMaxDecimalExponent = 308;
constexpr Int32 kMinDecimalExponent = -324;

/// UInt64 能无损容纳的十进制位数
constexpr Int32 kMaxMantissaDigits = 19;

FORCEINLINE bool IsJsonWhitespace(AnsiChar c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

FORCEINLINE bool IsDigit(AnsiChar c)
{
    return c >= '0' && c <= '9';
}

/// 十六进制字符转数值 — 非法字符返回 -1
FORCEINLINE Int32 HexDigitValue(AnsiChar c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/// 把数组截断回指定长度
///
/// TArray 未提供直接设置长度的接口，而移除末尾元素无需搬移后续数据，
/// 因此逐个弹出与直接改写长度的代价相同。
void TruncateTo(TArray<UInt32>& array, SizeType mark)
{
    while (array.GetSize() > mark)
    {
        array.RemoveAt(array.GetSize() - 1);
    }
}

/// 逐字节比较两个 C 字符串
bool AreCStringsEqual(const AnsiChar* left, const AnsiChar* right)
{
    if (left == nullptr || right == nullptr)
    {
        return left == right;
    }

    SizeType index = 0;
    while (left[index] != '\0' && right[index] != '\0')
    {
        if (left[index] != right[index])
        {
            return false;
        }

        ++index;
    }

    return left[index] == right[index];
}

} // namespace

// ============================================================================
// FJsonValue
// ============================================================================

EJsonType FJsonValue::GetType() const
{
    if (m_Document == nullptr)
    {
        return EJsonType::Invalid;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    return (node != nullptr) ? node->Type : EJsonType::Invalid;
}

bool FJsonValue::AsBool(bool defaultValue) const
{
    if (m_Document == nullptr)
    {
        return defaultValue;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Bool)
    {
        return defaultValue;
    }

    return node->BoolValue;
}

Float64 FJsonValue::AsDouble(Float64 defaultValue) const
{
    if (m_Document == nullptr)
    {
        return defaultValue;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Number)
    {
        return defaultValue;
    }

    return node->NumberValue;
}

Int64 FJsonValue::AsInt64(Int64 defaultValue) const
{
    if (m_Document == nullptr)
    {
        return defaultValue;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Number)
    {
        return defaultValue;
    }

    return static_cast<Int64>(node->NumberValue);
}

const AnsiChar* FJsonValue::AsString(const AnsiChar* defaultValue) const
{
    if (m_Document == nullptr)
    {
        return defaultValue;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::String)
    {
        return defaultValue;
    }

    return m_Document->GetPooledString(node->StringOffset);
}

SizeType FJsonValue::GetStringLength() const
{
    if (m_Document == nullptr)
    {
        return 0;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::String)
    {
        return 0;
    }

    return static_cast<SizeType>(node->StringLength);
}

SizeType FJsonValue::GetArraySize() const
{
    if (m_Document == nullptr)
    {
        return 0;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Array)
    {
        return 0;
    }

    return static_cast<SizeType>(node->ChildCount);
}

FJsonValue FJsonValue::operator[](SizeType index) const
{
    if (m_Document == nullptr)
    {
        return FJsonValue();
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Array)
    {
        return FJsonValue();
    }

    if (index >= static_cast<SizeType>(node->ChildCount))
    {
        return FJsonValue();
    }

    const UInt32 childSlot = node->ChildStart + static_cast<UInt32>(index);
    if (childSlot >= m_Document->m_ChildIndices.GetSize())
    {
        return FJsonValue();
    }

    return FJsonValue(m_Document, m_Document->m_ChildIndices[childSlot]);
}

SizeType FJsonValue::GetMemberCount() const
{
    if (m_Document == nullptr)
    {
        return 0;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Object)
    {
        return 0;
    }

    return static_cast<SizeType>(node->ChildCount);
}

FJsonValue FJsonValue::operator[](const AnsiChar* name) const
{
    if (m_Document == nullptr || name == nullptr)
    {
        return FJsonValue();
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Object)
    {
        return FJsonValue();
    }

    // 线性查找 — glTF 的对象成员通常不超过十余个, 建索引的收益
    // 抵不过为每个对象额外分配哈希表的代价
    for (UInt32 i = 0; i < node->ChildCount; ++i)
    {
        const UInt32 childSlot = node->ChildStart + i;
        if (childSlot >= m_Document->m_ChildIndices.GetSize())
        {
            break;
        }

        const UInt32 childIndex = m_Document->m_ChildIndices[childSlot];
        const FJsonDocument::FNode* child = m_Document->GetNode(childIndex);

        if (child == nullptr)
        {
            continue;
        }

        const AnsiChar* key = m_Document->GetPooledString(child->KeyOffset);
        if (AreCStringsEqual(key, name))
        {
            return FJsonValue(m_Document, childIndex);
        }
    }

    return FJsonValue();
}

bool FJsonValue::HasMember(const AnsiChar* name) const
{
    return (*this)[name].IsValid();
}

const AnsiChar* FJsonValue::GetMemberName(SizeType index) const
{
    if (m_Document == nullptr)
    {
        return nullptr;
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Object)
    {
        return nullptr;
    }

    if (index >= static_cast<SizeType>(node->ChildCount))
    {
        return nullptr;
    }

    const UInt32 childSlot = node->ChildStart + static_cast<UInt32>(index);
    if (childSlot >= m_Document->m_ChildIndices.GetSize())
    {
        return nullptr;
    }

    const FJsonDocument::FNode* child =
        m_Document->GetNode(m_Document->m_ChildIndices[childSlot]);

    return (child != nullptr)
               ? m_Document->GetPooledString(child->KeyOffset)
               : nullptr;
}

FJsonValue FJsonValue::GetMemberValue(SizeType index) const
{
    if (m_Document == nullptr)
    {
        return FJsonValue();
    }

    const FJsonDocument::FNode* node = m_Document->GetNode(m_NodeIndex);
    if (node == nullptr || node->Type != EJsonType::Object)
    {
        return FJsonValue();
    }

    if (index >= static_cast<SizeType>(node->ChildCount))
    {
        return FJsonValue();
    }

    const UInt32 childSlot = node->ChildStart + static_cast<UInt32>(index);
    if (childSlot >= m_Document->m_ChildIndices.GetSize())
    {
        return FJsonValue();
    }

    return FJsonValue(m_Document, m_Document->m_ChildIndices[childSlot]);
}

// ============================================================================
// FJsonDocument — 访问
// ============================================================================

const FJsonDocument::FNode* FJsonDocument::GetNode(UInt32 index) const
{
    if (index >= m_Nodes.GetSize())
    {
        return nullptr;
    }

    return &m_Nodes[index];
}

const AnsiChar* FJsonDocument::GetPooledString(UInt32 offset) const
{
    if (offset >= m_StringPool.GetSize())
    {
        return "";
    }

    return &m_StringPool[offset];
}

FJsonValue FJsonDocument::GetRoot() const
{
    if (m_Nodes.GetSize() == 0)
    {
        return FJsonValue();
    }

    return FJsonValue(this, 0);
}

void FJsonDocument::Reset()
{
    m_Nodes.Clear();
    m_ChildIndices.Clear();
    m_ScratchChildren.Clear();
    m_StringPool.Clear();

    m_ErrorMessage.Clear();
    m_ErrorLine   = 0;
    m_ErrorColumn = 0;
}

// ============================================================================
// FJsonDocument — 字符串池
// ============================================================================

void FJsonDocument::AppendPoolByte(AnsiChar byte)
{
    m_StringPool.Add(byte);
}

void FJsonDocument::AppendUtf8(UInt32 codePoint)
{
    if (codePoint < 0x80u)
    {
        AppendPoolByte(static_cast<AnsiChar>(codePoint));
    }
    else if (codePoint < 0x800u)
    {
        AppendPoolByte(static_cast<AnsiChar>(0xC0u | (codePoint >> 6)));
        AppendPoolByte(static_cast<AnsiChar>(0x80u | (codePoint & 0x3Fu)));
    }
    else if (codePoint < 0x10000u)
    {
        AppendPoolByte(static_cast<AnsiChar>(0xE0u | (codePoint >> 12)));
        AppendPoolByte(static_cast<AnsiChar>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        AppendPoolByte(static_cast<AnsiChar>(0x80u | (codePoint & 0x3Fu)));
    }
    else
    {
        AppendPoolByte(static_cast<AnsiChar>(0xF0u | (codePoint >> 18)));
        AppendPoolByte(static_cast<AnsiChar>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        AppendPoolByte(static_cast<AnsiChar>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        AppendPoolByte(static_cast<AnsiChar>(0x80u | (codePoint & 0x3Fu)));
    }
}

// ============================================================================
// FJsonDocument — 错误
// ============================================================================

bool FJsonDocument::SetError(const FParseContext& context,
                             const AnsiChar* message)
{
    m_ErrorMessage = FString(message);
    m_ErrorLine    = context.Line;
    m_ErrorColumn  = context.Column;

    return false;
}

// ============================================================================
// FJsonDocument — 词法
// ============================================================================

void FJsonDocument::SkipWhitespace(FParseContext& context)
{
    while (context.Cursor < context.Length)
    {
        const AnsiChar c = context.Text[context.Cursor];

        if (!IsJsonWhitespace(c))
        {
            break;
        }

        if (c == '\n')
        {
            ++context.Line;
            context.Column = 1;
        }
        else
        {
            ++context.Column;
        }

        ++context.Cursor;
    }
}

bool FJsonDocument::ParseLiteral(FParseContext& context,
                                 const AnsiChar* literal)
{
    SizeType index = 0;

    while (literal[index] != '\0')
    {
        if (context.Cursor + index >= context.Length ||
            context.Text[context.Cursor + index] != literal[index])
        {
            return false;
        }

        ++index;
    }

    context.Cursor += index;
    context.Column += static_cast<UInt32>(index);

    return true;
}

// ============================================================================
// FJsonDocument — 字符串
// ============================================================================

bool FJsonDocument::ParseUnicodeEscape(FParseContext& context)
{
    // 调用时 Cursor 指向 'u' 之后的第一个十六进制位
    if (context.Cursor + 4 > context.Length)
    {
        return SetError(context, "\\u 转义缺少四位十六进制数");
    }

    UInt32 codeUnit = 0;
    for (Int32 i = 0; i < 4; ++i)
    {
        const Int32 digit = HexDigitValue(context.Text[context.Cursor + i]);
        if (digit < 0)
        {
            return SetError(context, "\\u 转义包含非十六进制字符");
        }

        codeUnit = (codeUnit << 4) | static_cast<UInt32>(digit);
    }

    context.Cursor += 4;
    context.Column += 4;

    // ------------------------------------------------------------------
    // UTF-16 代理对: 高位代理必须紧跟一个低位代理, 二者合成一个补充平面码点
    // ------------------------------------------------------------------

    if (codeUnit >= 0xD800u && codeUnit <= 0xDBFFu)
    {
        if (context.Cursor + 6 > context.Length ||
            context.Text[context.Cursor] != '\\' ||
            context.Text[context.Cursor + 1] != 'u')
        {
            return SetError(context, "高位代理后缺少低位代理");
        }

        UInt32 lowUnit = 0;
        for (Int32 i = 0; i < 4; ++i)
        {
            const Int32 digit = HexDigitValue(context.Text[context.Cursor + 2 + i]);
            if (digit < 0)
            {
                return SetError(context, "低位代理包含非十六进制字符");
            }

            lowUnit = (lowUnit << 4) | static_cast<UInt32>(digit);
        }

        if (lowUnit < 0xDC00u || lowUnit > 0xDFFFu)
        {
            return SetError(context, "代理对的低位不在合法范围");
        }

        context.Cursor += 6;
        context.Column += 6;

        const UInt32 codePoint = 0x10000u +
                                 ((codeUnit - 0xD800u) << 10) +
                                 (lowUnit - 0xDC00u);

        AppendUtf8(codePoint);
        return true;
    }

    if (codeUnit >= 0xDC00u && codeUnit <= 0xDFFFu)
    {
        return SetError(context, "出现无配对的低位代理");
    }

    AppendUtf8(codeUnit);
    return true;
}

bool FJsonDocument::ParseString(FParseContext& context, UInt32& outOffset,
                                UInt32& outLength)
{
    if (context.Cursor >= context.Length ||
        context.Text[context.Cursor] != '"')
    {
        return SetError(context, "期望字符串起始的双引号");
    }

    ++context.Cursor;
    ++context.Column;

    outOffset = static_cast<UInt32>(m_StringPool.GetSize());

    while (true)
    {
        if (context.Cursor >= context.Length)
        {
            return SetError(context, "字符串在结束引号前意外终止");
        }

        const AnsiChar c = context.Text[context.Cursor];

        if (c == '"')
        {
            ++context.Cursor;
            ++context.Column;
            break;
        }

        if (c == '\\')
        {
            ++context.Cursor;
            ++context.Column;

            if (context.Cursor >= context.Length)
            {
                return SetError(context, "转义序列在字符串末尾被截断");
            }

            const AnsiChar escaped = context.Text[context.Cursor];
            ++context.Cursor;
            ++context.Column;

            switch (escaped)
            {
                case '"':  AppendPoolByte('"');  break;
                case '\\': AppendPoolByte('\\'); break;
                case '/':  AppendPoolByte('/');  break;
                case 'b':  AppendPoolByte('\b'); break;
                case 'f':  AppendPoolByte('\f'); break;
                case 'n':  AppendPoolByte('\n'); break;
                case 'r':  AppendPoolByte('\r'); break;
                case 't':  AppendPoolByte('\t'); break;

                case 'u':
                    if (!ParseUnicodeEscape(context))
                    {
                        return false;
                    }
                    break;

                default:
                    return SetError(context, "无法识别的转义序列");
            }

            continue;
        }

        // RFC 8259: 字符串内不得出现未转义的控制字符
        if (static_cast<UInt8>(c) < 0x20u)
        {
            return SetError(context, "字符串内出现未转义的控制字符");
        }

        AppendPoolByte(c);
        ++context.Cursor;
        ++context.Column;
    }

    outLength = static_cast<UInt32>(m_StringPool.GetSize()) - outOffset;

    // 补上终止符, 使池中的内容可直接当作 C 字符串使用
    AppendPoolByte('\0');

    return true;
}

// ============================================================================
// FJsonDocument — 数字
// ============================================================================

bool FJsonDocument::ParseNumber(FParseContext& context, Float64& outValue)
{
    const SizeType start = context.Cursor;

    bool isNegative = false;

    if (context.Cursor < context.Length &&
        context.Text[context.Cursor] == '-')
    {
        isNegative = true;
        ++context.Cursor;
        ++context.Column;
    }

    // ------------------------------------------------------------------
    // 整数部分 — RFC 8259 禁止前导零 (0 本身除外)
    // ------------------------------------------------------------------

    if (context.Cursor >= context.Length ||
        !IsDigit(context.Text[context.Cursor]))
    {
        return SetError(context, "数字缺少整数部分");
    }

    UInt64 mantissa      = 0;
    Int32  decimalExponent = 0;
    Int32  digitCount    = 0;

    if (context.Text[context.Cursor] == '0')
    {
        ++context.Cursor;
        ++context.Column;

        if (context.Cursor < context.Length &&
            IsDigit(context.Text[context.Cursor]))
        {
            return SetError(context, "数字含有前导零");
        }
    }
    else
    {
        while (context.Cursor < context.Length &&
               IsDigit(context.Text[context.Cursor]))
        {
            const Int32 digit = context.Text[context.Cursor] - '0';

            if (digitCount < kMaxMantissaDigits)
            {
                mantissa = mantissa * 10u + static_cast<UInt64>(digit);
                ++digitCount;
            }
            else
            {
                // 超出尾数容量的高位数字改为抬高指数, 保留数量级
                ++decimalExponent;
            }

            ++context.Cursor;
            ++context.Column;
        }
    }

    // ------------------------------------------------------------------
    // 小数部分
    // ------------------------------------------------------------------

    if (context.Cursor < context.Length &&
        context.Text[context.Cursor] == '.')
    {
        ++context.Cursor;
        ++context.Column;

        if (context.Cursor >= context.Length ||
            !IsDigit(context.Text[context.Cursor]))
        {
            return SetError(context, "小数点后缺少数字");
        }

        while (context.Cursor < context.Length &&
               IsDigit(context.Text[context.Cursor]))
        {
            const Int32 digit = context.Text[context.Cursor] - '0';

            if (digitCount < kMaxMantissaDigits)
            {
                mantissa = mantissa * 10u + static_cast<UInt64>(digit);
                ++digitCount;
                --decimalExponent;
            }
            // 超出精度的低位小数直接丢弃 — 它们对结果无影响

            ++context.Cursor;
            ++context.Column;
        }
    }

    // ------------------------------------------------------------------
    // 指数部分
    // ------------------------------------------------------------------

    if (context.Cursor < context.Length &&
        (context.Text[context.Cursor] == 'e' ||
         context.Text[context.Cursor] == 'E'))
    {
        ++context.Cursor;
        ++context.Column;

        bool exponentNegative = false;

        if (context.Cursor < context.Length &&
            (context.Text[context.Cursor] == '+' ||
             context.Text[context.Cursor] == '-'))
        {
            exponentNegative = (context.Text[context.Cursor] == '-');
            ++context.Cursor;
            ++context.Column;
        }

        if (context.Cursor >= context.Length ||
            !IsDigit(context.Text[context.Cursor]))
        {
            return SetError(context, "指数部分缺少数字");
        }

        Int32 explicitExponent = 0;

        while (context.Cursor < context.Length &&
               IsDigit(context.Text[context.Cursor]))
        {
            // 夹紧以避免整数溢出 — 超过该量级后结果必然是 0 或无穷
            if (explicitExponent < 100000)
            {
                explicitExponent =
                    explicitExponent * 10 + (context.Text[context.Cursor] - '0');
            }

            ++context.Cursor;
            ++context.Column;
        }

        decimalExponent += exponentNegative ? -explicitExponent
                                            : explicitExponent;
    }

    LIMX_UNUSED(start);

    // ------------------------------------------------------------------
    // 组装 — 尾数一次性缩放, 避免逐位累积舍入误差
    // ------------------------------------------------------------------

    Float64 value = static_cast<Float64>(mantissa);

    if (decimalExponent > kMaxDecimalExponent)
    {
        // 上溢 — 以尾数符号返回一个极大值而非 inf, 便于下游继续计算
        value = (mantissa == 0) ? 0.0 : 1.0e308;
    }
    else if (decimalExponent < kMinDecimalExponent)
    {
        value = 0.0;
    }
    else if (decimalExponent != 0)
    {
        value *= FMath::Pow(10.0, static_cast<Float64>(decimalExponent));
    }

    outValue = isNegative ? -value : value;

    return true;
}

// ============================================================================
// FJsonDocument — 结构
// ============================================================================

bool FJsonDocument::ParseArray(FParseContext& context, UInt32 nodeIndex)
{
    // 调用时 Cursor 已越过 '['
    const SizeType scratchMark = m_ScratchChildren.GetSize();

    SkipWhitespace(context);

    if (context.Cursor < context.Length && context.Text[context.Cursor] == ']')
    {
        ++context.Cursor;
        ++context.Column;

        m_Nodes[nodeIndex].ChildStart = static_cast<UInt32>(m_ChildIndices.GetSize());
        m_Nodes[nodeIndex].ChildCount = 0;
        return true;
    }

    while (true)
    {
        UInt32 elementIndex = 0;
        if (!ParseValue(context, elementIndex))
        {
            TruncateTo(m_ScratchChildren, scratchMark);
            return false;
        }

        m_ScratchChildren.Add(elementIndex);

        SkipWhitespace(context);

        if (context.Cursor >= context.Length)
        {
            TruncateTo(m_ScratchChildren, scratchMark);
            return SetError(context, "数组在结束方括号前意外终止");
        }

        const AnsiChar c = context.Text[context.Cursor];

        if (c == ',')
        {
            ++context.Cursor;
            ++context.Column;
            SkipWhitespace(context);

            // RFC 8259 不允许尾随逗号
            if (context.Cursor < context.Length &&
                context.Text[context.Cursor] == ']')
            {
                TruncateTo(m_ScratchChildren, scratchMark);
                return SetError(context, "数组中出现尾随逗号");
            }

            continue;
        }

        if (c == ']')
        {
            ++context.Cursor;
            ++context.Column;
            break;
        }

        TruncateTo(m_ScratchChildren, scratchMark);
        return SetError(context, "数组元素之间期望逗号或结束方括号");
    }

    // 把本层收集到的子索引整段搬入索引池
    const UInt32 childStart = static_cast<UInt32>(m_ChildIndices.GetSize());

    for (SizeType i = scratchMark; i < m_ScratchChildren.GetSize(); ++i)
    {
        m_ChildIndices.Add(m_ScratchChildren[i]);
    }

    m_Nodes[nodeIndex].ChildStart = childStart;
    m_Nodes[nodeIndex].ChildCount =
        static_cast<UInt32>(m_ScratchChildren.GetSize() - scratchMark);

    TruncateTo(m_ScratchChildren, scratchMark);

    return true;
}

bool FJsonDocument::ParseObject(FParseContext& context, UInt32 nodeIndex)
{
    // 调用时 Cursor 已越过 '{'
    const SizeType scratchMark = m_ScratchChildren.GetSize();

    SkipWhitespace(context);

    if (context.Cursor < context.Length && context.Text[context.Cursor] == '}')
    {
        ++context.Cursor;
        ++context.Column;

        m_Nodes[nodeIndex].ChildStart = static_cast<UInt32>(m_ChildIndices.GetSize());
        m_Nodes[nodeIndex].ChildCount = 0;
        return true;
    }

    while (true)
    {
        SkipWhitespace(context);

        // ---- 成员名 ----
        UInt32 keyOffset = 0;
        UInt32 keyLength = 0;

        if (!ParseString(context, keyOffset, keyLength))
        {
            TruncateTo(m_ScratchChildren, scratchMark);
            return false;
        }

        SkipWhitespace(context);

        if (context.Cursor >= context.Length ||
            context.Text[context.Cursor] != ':')
        {
            TruncateTo(m_ScratchChildren, scratchMark);
            return SetError(context, "对象成员名之后期望冒号");
        }

        ++context.Cursor;
        ++context.Column;

        // ---- 成员值 ----
        UInt32 valueIndex = 0;
        if (!ParseValue(context, valueIndex))
        {
            TruncateTo(m_ScratchChildren, scratchMark);
            return false;
        }

        // 键信息记录在值节点上 — 对象成员没有独立的节点
        m_Nodes[valueIndex].KeyOffset = keyOffset;
        m_Nodes[valueIndex].KeyLength = keyLength;

        m_ScratchChildren.Add(valueIndex);

        SkipWhitespace(context);

        if (context.Cursor >= context.Length)
        {
            TruncateTo(m_ScratchChildren, scratchMark);
            return SetError(context, "对象在结束花括号前意外终止");
        }

        const AnsiChar c = context.Text[context.Cursor];

        if (c == ',')
        {
            ++context.Cursor;
            ++context.Column;
            SkipWhitespace(context);

            if (context.Cursor < context.Length &&
                context.Text[context.Cursor] == '}')
            {
                TruncateTo(m_ScratchChildren, scratchMark);
                return SetError(context, "对象中出现尾随逗号");
            }

            continue;
        }

        if (c == '}')
        {
            ++context.Cursor;
            ++context.Column;
            break;
        }

        TruncateTo(m_ScratchChildren, scratchMark);
        return SetError(context, "对象成员之间期望逗号或结束花括号");
    }

    const UInt32 childStart = static_cast<UInt32>(m_ChildIndices.GetSize());

    for (SizeType i = scratchMark; i < m_ScratchChildren.GetSize(); ++i)
    {
        m_ChildIndices.Add(m_ScratchChildren[i]);
    }

    m_Nodes[nodeIndex].ChildStart = childStart;
    m_Nodes[nodeIndex].ChildCount =
        static_cast<UInt32>(m_ScratchChildren.GetSize() - scratchMark);

    TruncateTo(m_ScratchChildren, scratchMark);

    return true;
}

bool FJsonDocument::ParseValue(FParseContext& context, UInt32& outNodeIndex)
{
    if (context.Depth >= kMaxDepth)
    {
        return SetError(context, "嵌套层级超过上限");
    }

    SkipWhitespace(context);

    if (context.Cursor >= context.Length)
    {
        return SetError(context, "期望一个值, 却已到达输入末尾");
    }

    const AnsiChar c = context.Text[context.Cursor];

    // 先占位分配节点 —— 容器解析过程中会追加更多节点, 必须先固定自身索引
    outNodeIndex = static_cast<UInt32>(m_Nodes.Add(FNode()));

    switch (c)
    {
        case '{':
        {
            ++context.Cursor;
            ++context.Column;

            m_Nodes[outNodeIndex].Type = EJsonType::Object;

            ++context.Depth;
            const bool ok = ParseObject(context, outNodeIndex);
            --context.Depth;

            return ok;
        }

        case '[':
        {
            ++context.Cursor;
            ++context.Column;

            m_Nodes[outNodeIndex].Type = EJsonType::Array;

            ++context.Depth;
            const bool ok = ParseArray(context, outNodeIndex);
            --context.Depth;

            return ok;
        }

        case '"':
        {
            UInt32 offset = 0;
            UInt32 length = 0;

            if (!ParseString(context, offset, length))
            {
                return false;
            }

            m_Nodes[outNodeIndex].Type         = EJsonType::String;
            m_Nodes[outNodeIndex].StringOffset = offset;
            m_Nodes[outNodeIndex].StringLength = length;

            return true;
        }

        case 't':
        {
            if (!ParseLiteral(context, "true"))
            {
                return SetError(context, "无法识别的字面量, 期望 true");
            }

            m_Nodes[outNodeIndex].Type      = EJsonType::Bool;
            m_Nodes[outNodeIndex].BoolValue = true;

            return true;
        }

        case 'f':
        {
            if (!ParseLiteral(context, "false"))
            {
                return SetError(context, "无法识别的字面量, 期望 false");
            }

            m_Nodes[outNodeIndex].Type      = EJsonType::Bool;
            m_Nodes[outNodeIndex].BoolValue = false;

            return true;
        }

        case 'n':
        {
            if (!ParseLiteral(context, "null"))
            {
                return SetError(context, "无法识别的字面量, 期望 null");
            }

            m_Nodes[outNodeIndex].Type = EJsonType::Null;

            return true;
        }

        default:
        {
            if (c == '-' || IsDigit(c))
            {
                Float64 number = 0.0;
                if (!ParseNumber(context, number))
                {
                    return false;
                }

                m_Nodes[outNodeIndex].Type        = EJsonType::Number;
                m_Nodes[outNodeIndex].NumberValue = number;

                return true;
            }

            return SetError(context, "无法识别的值起始字符");
        }
    }
}

// ============================================================================
// FJsonDocument — 入口
// ============================================================================

bool FJsonDocument::Parse(const AnsiChar* text, SizeType length)
{
    Reset();

    if (text == nullptr || length == 0)
    {
        FParseContext empty;
        return SetError(empty, "输入为空");
    }

    FParseContext context;
    context.Text   = text;
    context.Length = length;

    // 跳过 UTF-8 BOM — 不少工具会在 JSON 文件开头写入
    if (length >= 3 &&
        static_cast<UInt8>(text[0]) == 0xEFu &&
        static_cast<UInt8>(text[1]) == 0xBBu &&
        static_cast<UInt8>(text[2]) == 0xBFu)
    {
        context.Cursor = 3;
    }

    UInt32 rootIndex = 0;
    if (!ParseValue(context, rootIndex))
    {
        m_Nodes.Clear();
        return false;
    }

    // 根节点必须是索引 0 — 视图 GetRoot 依赖这一点
    LIMX_ASSERT(rootIndex == 0);

    // 根值之后只允许空白
    SkipWhitespace(context);

    if (context.Cursor < context.Length)
    {
        m_Nodes.Clear();
        return SetError(context, "根值之后存在多余内容");
    }

    return true;
}

bool FJsonDocument::Parse(const AnsiChar* text)
{
    if (text == nullptr)
    {
        FParseContext empty;
        return SetError(empty, "输入为空");
    }

    SizeType length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }

    return Parse(text, length);
}

} // namespace Limx
