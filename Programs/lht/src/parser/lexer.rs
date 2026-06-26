/*******************************************************************************
 * 文件: lexer.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   C++ 词法分析器 (生产级)
 *   - 完整的 Token 类型支持
 *   - 支持多行注释/字符串
 *   - 精确的行号/列号追踪
 *   - 支持预处理指令
 *
 * 设计哲学:
 *   参考 Clang 词法分析器设计，提供足够的信息
 *   用于后续的语法分析和反射宏解析
 *
 ******************************************************************************/

use std::iter::Peekable;
use std::str::Chars;

/// Token 类型
#[derive(Debug, Clone, PartialEq)]
pub enum TokenKind {
    // 关键字
    Class,
    Struct,
    Enum,
    Public,
    Private,
    Protected,
    Virtual,
    Override,
    Static,
    Const,
    Volatile,
    Inline,
    Explicit,
    Friend,
    Template,
    Typename,
    Namespace,
    Using,
    Typedef,

    // 反射宏
    LClass,
    LStruct,
    LEnum,
    LProperty,
    LFunction,
    LDelegate,
    LGeneratedBody,

    // 标识符和字面量
    Identifier(String),
    IntLiteral(i64),
    FloatLiteral(f64),
    StringLiteral(String),
    CharLiteral(char),

    // 符号
    LParen,          // (
    RParen,          // )
    LBrace,          // {
    RBrace,          // }
    LBracket,        // [
    RBracket,        // ]
    LAngle,          // <
    RAngle,          // >
    Semicolon,       // ;
    Colon,           // :
    DoubleColon,     // ::
    Comma,           // ,
    Dot,             // .
    Arrow,           // ->
    Star,            // *
    Ampersand,       // &
    DoubleAmpersand, // &&
    Pipe,            // |
    DoublePipe,      // ||
    Equals,          // =
    DoubleEquals,    // ==
    NotEquals,       // !=
    Plus,            // +
    Minus,           // -
    Slash,           // /
    Percent,         // %
    Caret,           // ^
    Tilde,           // ~
    Bang,            // !
    Question,        // ?
    Hash,            // #
    DoubleHash,      // ##
    Ellipsis,        // ...

    // 特殊
    Newline,
    Whitespace,
    Comment(String),
    PreprocessorDirective(String),
    Eof,
    Unknown(char),
}

/// Token 位置信息
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SourceLocation {
    pub line: u32,
    pub column: u32,
    pub offset: usize,
}

impl SourceLocation {
    pub fn new(line: u32, column: u32, offset: usize) -> Self {
        Self {
            line,
            column,
            offset,
        }
    }
}

/// Token 结构
#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenKind,
    pub loc: SourceLocation,
    pub len: usize,
}

impl Token {
    pub fn new(kind: TokenKind, loc: SourceLocation, len: usize) -> Self {
        Self { kind, loc, len }
    }

    pub fn is_keyword(&self) -> bool {
        matches!(
            self.kind,
            TokenKind::Class
                | TokenKind::Struct
                | TokenKind::Enum
                | TokenKind::Public
                | TokenKind::Private
                | TokenKind::Protected
                | TokenKind::Virtual
                | TokenKind::Override
                | TokenKind::Static
                | TokenKind::Const
                | TokenKind::Volatile
                | TokenKind::Inline
                | TokenKind::Template
                | TokenKind::Typename
                | TokenKind::Namespace
        )
    }

    pub fn is_reflection_macro(&self) -> bool {
        matches!(
            self.kind,
            TokenKind::LClass
                | TokenKind::LStruct
                | TokenKind::LEnum
                | TokenKind::LProperty
                | TokenKind::LFunction
                | TokenKind::LDelegate
                | TokenKind::LGeneratedBody
        )
    }
}

/// C++ 词法分析器
pub struct Lexer<'a> {
    source: &'a str,
    chars: Peekable<Chars<'a>>,
    current_offset: usize,
    line: u32,
    column: u32,
    tokens: Vec<Token>,
}

impl<'a> Lexer<'a> {
    pub fn new(source: &'a str) -> Self {
        Self {
            source,
            chars: source.chars().peekable(),
            current_offset: 0,
            line: 1,
            column: 1,
            tokens: Vec::new(),
        }
    }

    /// 执行词法分析
    pub fn tokenize(mut self) -> Vec<Token> {
        while let Some(&ch) = self.chars.peek() {
            let loc = self.current_location();

            match ch {
                // 空白字符
                ' ' | '\t' | '\r' => {
                    self.advance();
                }

                // 换行
                '\n' => {
                    self.advance();
                    self.line += 1;
                    self.column = 1;
                }

                // 注释或除法
                '/' => {
                    self.advance();
                    if self.peek() == Some('/') {
                        self.skip_line_comment();
                    } else if self.peek() == Some('*') {
                        self.skip_block_comment();
                    } else {
                        self.tokens.push(Token::new(TokenKind::Slash, loc, 1));
                    }
                }

                // 预处理指令
                '#' => {
                    let directive = self.read_preprocessor_directive();
                    self.tokens.push(Token::new(
                        TokenKind::PreprocessorDirective(directive),
                        loc,
                        self.current_offset - loc.offset,
                    ));
                }

                // 字符串字面量
                '"' => {
                    let s = self.read_string_literal();
                    self.tokens.push(Token::new(
                        TokenKind::StringLiteral(s),
                        loc,
                        self.current_offset - loc.offset,
                    ));
                }

                // 字符字面量
                '\'' => {
                    let c = self.read_char_literal();
                    self.tokens.push(Token::new(
                        TokenKind::CharLiteral(c),
                        loc,
                        self.current_offset - loc.offset,
                    ));
                }

                // 数字
                '0'..='9' => {
                    let (kind, len) = self.read_number();
                    self.tokens.push(Token::new(kind, loc, len));
                }

                // 标识符或关键字
                'a'..='z' | 'A'..='Z' | '_' => {
                    let ident = self.read_identifier();
                    let kind = self.classify_identifier(&ident);
                    self.tokens.push(Token::new(kind, loc, ident.len()));
                }

                // 符号
                '(' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::LParen, loc, 1));
                }
                ')' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::RParen, loc, 1));
                }
                '{' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::LBrace, loc, 1));
                }
                '}' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::RBrace, loc, 1));
                }
                '[' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::LBracket, loc, 1));
                }
                ']' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::RBracket, loc, 1));
                }
                ';' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Semicolon, loc, 1));
                }
                ',' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Comma, loc, 1));
                }
                '?' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Question, loc, 1));
                }
                '~' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Tilde, loc, 1));
                }
                '^' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Caret, loc, 1));
                }
                '%' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Percent, loc, 1));
                }

                ':' => {
                    self.advance();
                    if self.peek() == Some(':') {
                        self.advance();
                        self.tokens.push(Token::new(TokenKind::DoubleColon, loc, 2));
                    } else {
                        self.tokens.push(Token::new(TokenKind::Colon, loc, 1));
                    }
                }

                '<' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::LAngle, loc, 1));
                }
                '>' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::RAngle, loc, 1));
                }

                '*' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Star, loc, 1));
                }

                '&' => {
                    self.advance();
                    if self.peek() == Some('&') {
                        self.advance();
                        self.tokens
                            .push(Token::new(TokenKind::DoubleAmpersand, loc, 2));
                    } else {
                        self.tokens.push(Token::new(TokenKind::Ampersand, loc, 1));
                    }
                }

                '|' => {
                    self.advance();
                    if self.peek() == Some('|') {
                        self.advance();
                        self.tokens.push(Token::new(TokenKind::DoublePipe, loc, 2));
                    } else {
                        self.tokens.push(Token::new(TokenKind::Pipe, loc, 1));
                    }
                }

                '=' => {
                    self.advance();
                    if self.peek() == Some('=') {
                        self.advance();
                        self.tokens
                            .push(Token::new(TokenKind::DoubleEquals, loc, 2));
                    } else {
                        self.tokens.push(Token::new(TokenKind::Equals, loc, 1));
                    }
                }

                '!' => {
                    self.advance();
                    if self.peek() == Some('=') {
                        self.advance();
                        self.tokens.push(Token::new(TokenKind::NotEquals, loc, 2));
                    } else {
                        self.tokens.push(Token::new(TokenKind::Bang, loc, 1));
                    }
                }

                '+' => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Plus, loc, 1));
                }

                '-' => {
                    self.advance();
                    if self.peek() == Some('>') {
                        self.advance();
                        self.tokens.push(Token::new(TokenKind::Arrow, loc, 2));
                    } else {
                        self.tokens.push(Token::new(TokenKind::Minus, loc, 1));
                    }
                }

                '.' => {
                    self.advance();
                    if self.peek() == Some('.') {
                        self.advance();
                        if self.peek() == Some('.') {
                            self.advance();
                            self.tokens.push(Token::new(TokenKind::Ellipsis, loc, 3));
                        } else {
                            self.tokens.push(Token::new(TokenKind::Dot, loc, 1));
                            self.tokens.push(Token::new(
                                TokenKind::Dot,
                                self.current_location(),
                                1,
                            ));
                        }
                    } else {
                        self.tokens.push(Token::new(TokenKind::Dot, loc, 1));
                    }
                }

                _ => {
                    self.advance();
                    self.tokens.push(Token::new(TokenKind::Unknown(ch), loc, 1));
                }
            }
        }

        // 添加 EOF token
        self.tokens
            .push(Token::new(TokenKind::Eof, self.current_location(), 0));

        self.tokens
    }

    fn current_location(&self) -> SourceLocation {
        SourceLocation::new(self.line, self.column, self.current_offset)
    }

    fn advance(&mut self) -> Option<char> {
        if let Some(ch) = self.chars.next() {
            self.current_offset += ch.len_utf8();
            self.column += 1;
            Some(ch)
        } else {
            None
        }
    }

    fn peek(&mut self) -> Option<char> {
        self.chars.peek().copied()
    }

    fn skip_line_comment(&mut self) {
        while let Some(ch) = self.advance() {
            if ch == '\n' {
                self.line += 1;
                self.column = 1;
                break;
            }
        }
    }

    fn skip_block_comment(&mut self) {
        self.advance(); // consume *
        let mut depth = 1;

        while depth > 0 {
            match self.advance() {
                Some('/') if self.peek() == Some('*') => {
                    self.advance();
                    depth += 1;
                }
                Some('*') if self.peek() == Some('/') => {
                    self.advance();
                    depth -= 1;
                }
                Some('\n') => {
                    self.line += 1;
                    self.column = 1;
                }
                None => break,
                _ => {}
            }
        }
    }

    fn read_preprocessor_directive(&mut self) -> String {
        let mut directive = String::new();
        directive.push('#');
        self.advance(); // consume #

        // 可能有 ##
        if self.peek() == Some('#') {
            directive.push('#');
            self.advance();
            return directive;
        }

        // 读取整行 (处理续行符)
        let mut in_continuation = false;
        while let Some(ch) = self.peek() {
            if ch == '\n' {
                if in_continuation {
                    directive.push('\n');
                    self.advance();
                    self.line += 1;
                    self.column = 1;
                    in_continuation = false;
                } else {
                    break;
                }
            } else if ch == '\\' {
                self.advance();
                if self.peek() == Some('\n') {
                    in_continuation = true;
                } else {
                    directive.push('\\');
                }
            } else {
                directive.push(ch);
                self.advance();
            }
        }

        directive
    }

    fn read_string_literal(&mut self) -> String {
        let mut s = String::new();
        self.advance(); // consume opening "

        while let Some(ch) = self.advance() {
            match ch {
                '"' => break,
                '\\' => {
                    if let Some(escaped) = self.advance() {
                        match escaped {
                            'n' => s.push('\n'),
                            't' => s.push('\t'),
                            'r' => s.push('\r'),
                            '\\' => s.push('\\'),
                            '"' => s.push('"'),
                            '0' => s.push('\0'),
                            _ => {
                                s.push('\\');
                                s.push(escaped);
                            }
                        }
                    }
                }
                '\n' => {
                    self.line += 1;
                    self.column = 1;
                    s.push(ch);
                }
                _ => s.push(ch),
            }
        }

        s
    }

    fn read_char_literal(&mut self) -> char {
        self.advance(); // consume opening '

        let ch = match self.advance() {
            Some('\\') => match self.advance() {
                Some('n') => '\n',
                Some('t') => '\t',
                Some('r') => '\r',
                Some('\\') => '\\',
                Some('\'') => '\'',
                Some('0') => '\0',
                Some(c) => c,
                None => '\0',
            },
            Some(c) => c,
            None => '\0',
        };

        // consume closing '
        if self.peek() == Some('\'') {
            self.advance();
        }

        ch
    }

    fn read_number(&mut self) -> (TokenKind, usize) {
        let start = self.current_offset;
        let mut s = String::new();
        let mut is_float = false;
        let mut is_hex = false;

        // 检查十六进制
        if self.peek() == Some('0') {
            s.push('0');
            self.advance();
            if self.peek() == Some('x') || self.peek() == Some('X') {
                s.push('x');
                self.advance();
                is_hex = true;
            }
        }

        while let Some(ch) = self.peek() {
            if ch.is_ascii_digit() || (is_hex && ch.is_ascii_hexdigit()) {
                s.push(ch);
                self.advance();
            } else if ch == '.' && !is_float && !is_hex {
                is_float = true;
                s.push(ch);
                self.advance();
            } else if (ch == 'e' || ch == 'E') && !is_hex {
                is_float = true;
                s.push(ch);
                self.advance();
                if self.peek() == Some('+') || self.peek() == Some('-') {
                    if let Some(sign_ch) = self.advance() {
                        s.push(sign_ch);
                    }
                }
            } else if ch == 'f' || ch == 'F' || ch == 'l' || ch == 'L' || ch == 'u' || ch == 'U' {
                self.advance(); // 跳过后缀
            } else {
                break;
            }
        }

        let len = self.current_offset - start;

        if is_float {
            let value = s.parse::<f64>().unwrap_or(0.0);
            (TokenKind::FloatLiteral(value), len)
        } else if is_hex {
            let hex_str = s.trim_start_matches("0x").trim_start_matches("0X");
            let value = i64::from_str_radix(hex_str, 16).unwrap_or(0);
            (TokenKind::IntLiteral(value), len)
        } else {
            let value = s.parse::<i64>().unwrap_or(0);
            (TokenKind::IntLiteral(value), len)
        }
    }

    fn read_identifier(&mut self) -> String {
        let mut ident = String::new();

        while let Some(ch) = self.peek() {
            if ch.is_ascii_alphanumeric() || ch == '_' {
                ident.push(ch);
                self.advance();
            } else {
                break;
            }
        }

        ident
    }

    fn classify_identifier(&self, ident: &str) -> TokenKind {
        match ident {
            // C++ 关键字
            "class" => TokenKind::Class,
            "struct" => TokenKind::Struct,
            "enum" => TokenKind::Enum,
            "public" => TokenKind::Public,
            "private" => TokenKind::Private,
            "protected" => TokenKind::Protected,
            "virtual" => TokenKind::Virtual,
            "override" => TokenKind::Override,
            "static" => TokenKind::Static,
            "const" => TokenKind::Const,
            "volatile" => TokenKind::Volatile,
            "inline" => TokenKind::Inline,
            "explicit" => TokenKind::Explicit,
            "friend" => TokenKind::Friend,
            "template" => TokenKind::Template,
            "typename" => TokenKind::Typename,
            "namespace" => TokenKind::Namespace,
            "using" => TokenKind::Using,
            "typedef" => TokenKind::Typedef,

            // 反射宏
            "LCLASS" => TokenKind::LClass,
            "LSTRUCT" => TokenKind::LStruct,
            "LENUM" => TokenKind::LEnum,
            "LPROPERTY" => TokenKind::LProperty,
            "LFUNCTION" => TokenKind::LFunction,
            "LDELEGATE" => TokenKind::LDelegate,
            "LGENERATED_BODY" => TokenKind::LGeneratedBody,

            // 普通标识符
            _ => TokenKind::Identifier(ident.to_string()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_tokens() {
        let source = "class MyClass { };";
        let lexer = Lexer::new(source);
        let tokens = lexer.tokenize();

        assert!(matches!(tokens[0].kind, TokenKind::Class));
        assert!(matches!(tokens[1].kind, TokenKind::Identifier(_)));
        assert!(matches!(tokens[2].kind, TokenKind::LBrace));
        assert!(matches!(tokens[3].kind, TokenKind::RBrace));
        assert!(matches!(tokens[4].kind, TokenKind::Semicolon));
    }

    #[test]
    fn test_reflection_macros() {
        let source = "LCLASS(Serializable) class Test {};";
        let lexer = Lexer::new(source);
        let tokens = lexer.tokenize();

        assert!(matches!(tokens[0].kind, TokenKind::LClass));
    }
}
