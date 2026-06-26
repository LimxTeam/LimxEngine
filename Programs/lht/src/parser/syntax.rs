/*******************************************************************************
 * 文件: syntax.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   C++ 语法分析器 (生产级)
 *   - 解析反射宏标记的类/结构体/枚举
 *   - 提取属性、方法、委托信息
 *   - 支持模板、嵌套类型、命名空间
 *
 * 设计哲学:
 *   不尝试解析完整的 C++，只解析反射所需的部分
 *   遇到不理解的语法时优雅跳过
 *
 ******************************************************************************/

use super::ast::*;
use super::lexer::{Lexer, Token, TokenKind};
use super::specifiers::{parse_specifiers, Specifiers};
use anyhow::{bail, Result};

/// 语法分析器
pub struct Parser {
    tokens: Vec<Token>,
    pos: usize,
    current_namespace: Vec<String>,
    current_access: AccessSpecifier,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self {
        Self {
            tokens,
            pos: 0,
            current_namespace: Vec::new(),
            current_access: AccessSpecifier::Private,
        }
    }

    /// 从源代码解析
    pub fn parse_source(source: &str) -> Result<TranslationUnit> {
        let lexer = Lexer::new(source);
        let tokens = lexer.tokenize();
        let mut parser = Parser::new(tokens);
        parser.parse_translation_unit("")
    }

    /// 解析翻译单元
    pub fn parse_translation_unit(&mut self, file_path: &str) -> Result<TranslationUnit> {
        let mut unit = TranslationUnit::new(file_path.to_string());

        while !self.is_at_end() {
            // 跳过预处理指令
            if let TokenKind::PreprocessorDirective(ref directive) = self.current().kind {
                if directive.starts_with("#include") {
                    let include = self.extract_include(directive);
                    if let Some(inc) = include {
                        unit.includes.push(inc);
                    }
                }
                self.advance();
                continue;
            }

            // 尝试解析声明
            if let Some(decl) = self.try_parse_declaration()? {
                unit.declarations.push(decl);
            } else {
                // 跳过无法识别的 token
                self.advance();
            }
        }

        Ok(unit)
    }

    /// 尝试解析声明
    fn try_parse_declaration(&mut self) -> Result<Option<Declaration>> {
        match &self.current().kind {
            TokenKind::Namespace => {
                let ns = self.parse_namespace()?;
                Ok(Some(Declaration::Namespace(ns)))
            }
            TokenKind::LClass => {
                let class = self.parse_reflected_class(ClassKind::Class)?;
                Ok(Some(Declaration::Class(class)))
            }
            TokenKind::LStruct => {
                let class = self.parse_reflected_class(ClassKind::Struct)?;
                Ok(Some(Declaration::Class(class)))
            }
            TokenKind::LEnum => {
                let enum_decl = self.parse_reflected_enum()?;
                Ok(Some(Declaration::Enum(enum_decl)))
            }
            TokenKind::LDelegate => {
                let delegate = self.parse_delegate()?;
                Ok(Some(Declaration::Delegate(delegate)))
            }
            TokenKind::Class | TokenKind::Struct => {
                // 非反射的类/结构体，跳过
                self.skip_class_declaration();
                Ok(None)
            }
            TokenKind::Enum => {
                // 非反射的枚举，跳过
                self.skip_enum_declaration();
                Ok(None)
            }
            _ => Ok(None),
        }
    }

    /// 解析命名空间
    fn parse_namespace(&mut self) -> Result<NamespaceDecl> {
        let loc = self.current().loc;
        self.expect(TokenKind::Namespace)?;

        // 可能是内联命名空间
        let is_inline = false;

        // 获取命名空间名
        let name = self.expect_identifier()?;
        self.current_namespace.push(name.clone());

        self.expect(TokenKind::LBrace)?;

        let mut declarations = Vec::new();

        while !self.check(TokenKind::RBrace) && !self.is_at_end() {
            if let Some(decl) = self.try_parse_declaration()? {
                declarations.push(decl);
            } else {
                self.advance();
            }
        }

        self.expect(TokenKind::RBrace)?;
        self.current_namespace.pop();

        Ok(NamespaceDecl {
            name,
            is_inline,
            declarations,
            location: loc,
        })
    }

    /// 解析反射类/结构体
    fn parse_reflected_class(&mut self, kind: ClassKind) -> Result<ClassDecl> {
        let loc = self.current().loc;

        // 解析 LCLASS/LSTRUCT(specifiers)
        let specifiers = self.parse_macro_specifiers()?;

        // 跳过 class/struct 关键字
        if self.check(TokenKind::Class) || self.check(TokenKind::Struct) {
            self.advance();
        }

        // 解析 API 宏 (如 LIMX_CORE_API)
        let api_macro = self.try_parse_api_macro();

        // 解析类名
        let name = self.expect_identifier()?;
        let qualified_name = self.make_qualified_name(&name);

        // 解析基类
        let base_classes = if self.check(TokenKind::Colon) {
            self.parse_base_classes()?
        } else {
            Vec::new()
        };

        self.expect(TokenKind::LBrace)?;

        // 解析成员
        let default_access = match kind {
            ClassKind::Class => AccessSpecifier::Private,
            ClassKind::Struct => AccessSpecifier::Public,
        };
        self.current_access = default_access;

        let (members, has_generated_body) = self.parse_class_members()?;

        self.expect(TokenKind::RBrace)?;

        // 可选的分号
        if self.check(TokenKind::Semicolon) {
            self.advance();
        }

        Ok(ClassDecl {
            kind,
            name,
            qualified_name,
            base_classes,
            api_macro,
            specifiers,
            members,
            location: loc,
            has_generated_body,
        })
    }

    /// 解析反射枚举
    fn parse_reflected_enum(&mut self) -> Result<EnumDecl> {
        let loc = self.current().loc;

        // 解析 LENUM(specifiers)
        let specifiers = self.parse_macro_specifiers()?;

        // enum 关键字
        self.expect(TokenKind::Enum)?;

        // 可能是 enum class
        let is_scoped = if self.check(TokenKind::Class) || self.check(TokenKind::Struct) {
            self.advance();
            true
        } else {
            false
        };

        // 枚举名
        let name = self.expect_identifier()?;

        // 底层类型
        let underlying_type = if self.check(TokenKind::Colon) {
            self.advance();
            Some(self.parse_type()?)
        } else {
            None
        };

        self.expect(TokenKind::LBrace)?;

        // 解析枚举值
        let values = self.parse_enum_values()?;

        self.expect(TokenKind::RBrace)?;

        // 分号
        if self.check(TokenKind::Semicolon) {
            self.advance();
        }

        Ok(EnumDecl {
            name,
            is_scoped,
            underlying_type,
            specifiers: Some(specifiers),
            values,
            location: loc,
        })
    }

    /// 解析委托
    fn parse_delegate(&mut self) -> Result<DelegateDecl> {
        let loc = self.current().loc;

        // 解析 LDELEGATE(specifiers)
        let specifiers = self.parse_macro_specifiers()?;

        // 返回类型
        let return_type = self.parse_type()?;

        // 委托名
        let name = self.expect_identifier()?;

        // 参数列表
        self.expect(TokenKind::LParen)?;
        let parameters = self.parse_parameter_list()?;
        self.expect(TokenKind::RParen)?;

        // 分号
        if self.check(TokenKind::Semicolon) {
            self.advance();
        }

        Ok(DelegateDecl {
            name,
            return_type,
            parameters,
            specifiers,
            location: loc,
        })
    }

    /// 解析宏说明符
    fn parse_macro_specifiers(&mut self) -> Result<Specifiers> {
        // 跳过宏名 (LCLASS/LSTRUCT/LENUM/etc)
        self.advance();

        self.expect(TokenKind::LParen)?;

        // 收集括号内的内容
        let mut spec_str = String::new();
        let mut depth = 1;

        while depth > 0 && !self.is_at_end() {
            match &self.current().kind {
                TokenKind::LParen => {
                    spec_str.push('(');
                    depth += 1;
                }
                TokenKind::RParen => {
                    depth -= 1;
                    if depth > 0 {
                        spec_str.push(')');
                    }
                }
                TokenKind::Comma => spec_str.push(','),
                TokenKind::Equals => spec_str.push('='),
                TokenKind::StringLiteral(s) => {
                    spec_str.push('"');
                    spec_str.push_str(s);
                    spec_str.push('"');
                }
                TokenKind::Identifier(s) => spec_str.push_str(s),
                TokenKind::IntLiteral(n) => spec_str.push_str(&n.to_string()),
                TokenKind::FloatLiteral(n) => spec_str.push_str(&n.to_string()),
                _ => {}
            }
            self.advance();
        }

        Ok(parse_specifiers(&spec_str))
    }

    /// 尝试解析 API 宏
    fn try_parse_api_macro(&mut self) -> Option<String> {
        if let TokenKind::Identifier(ref s) = self.current().kind {
            if s.ends_with("_API") {
                let macro_name = s.clone();
                self.advance();
                return Some(macro_name);
            }
        }
        None
    }

    /// 解析基类列表
    fn parse_base_classes(&mut self) -> Result<Vec<BaseClass>> {
        self.expect(TokenKind::Colon)?;

        let mut bases = Vec::new();

        loop {
            let mut access = AccessSpecifier::Private;
            let mut is_virtual = false;

            // virtual
            if self.check(TokenKind::Virtual) {
                is_virtual = true;
                self.advance();
            }

            // 访问修饰符
            if self.check(TokenKind::Public) {
                access = AccessSpecifier::Public;
                self.advance();
            } else if self.check(TokenKind::Protected) {
                access = AccessSpecifier::Protected;
                self.advance();
            } else if self.check(TokenKind::Private) {
                access = AccessSpecifier::Private;
                self.advance();
            }

            // virtual 也可以在 access 后面
            if self.check(TokenKind::Virtual) {
                is_virtual = true;
                self.advance();
            }

            // 基类名 (可能是限定名)
            let name = self.parse_qualified_identifier()?;

            bases.push(BaseClass {
                name,
                access,
                is_virtual,
            });

            if !self.check(TokenKind::Comma) {
                break;
            }
            self.advance();
        }

        Ok(bases)
    }

    /// 解析类成员
    fn parse_class_members(&mut self) -> Result<(Vec<ClassMember>, bool)> {
        let mut members = Vec::new();
        let mut has_generated_body = false;

        while !self.check(TokenKind::RBrace) && !self.is_at_end() {
            // 访问修饰符
            if self.check(TokenKind::Public) {
                self.current_access = AccessSpecifier::Public;
                self.advance();
                self.expect(TokenKind::Colon)?;
                members.push(ClassMember::AccessSpecifier(AccessSpecifier::Public));
                continue;
            }
            if self.check(TokenKind::Protected) {
                self.current_access = AccessSpecifier::Protected;
                self.advance();
                self.expect(TokenKind::Colon)?;
                members.push(ClassMember::AccessSpecifier(AccessSpecifier::Protected));
                continue;
            }
            if self.check(TokenKind::Private) {
                self.current_access = AccessSpecifier::Private;
                self.advance();
                self.expect(TokenKind::Colon)?;
                members.push(ClassMember::AccessSpecifier(AccessSpecifier::Private));
                continue;
            }

            // LGENERATED_BODY()
            if self.check(TokenKind::LGeneratedBody) {
                has_generated_body = true;
                self.advance();
                self.expect(TokenKind::LParen)?;
                self.expect(TokenKind::RParen)?;
                continue;
            }

            // LPROPERTY
            if self.check(TokenKind::LProperty) {
                let field = self.parse_reflected_field()?;
                members.push(ClassMember::Field(field));
                continue;
            }

            // LFUNCTION
            if self.check(TokenKind::LFunction) {
                let method = self.parse_reflected_method()?;
                members.push(ClassMember::Method(method));
                continue;
            }

            // 跳过其他成员
            self.skip_until_semicolon_or_brace();
        }

        Ok((members, has_generated_body))
    }

    /// 解析反射字段
    fn parse_reflected_field(&mut self) -> Result<FieldDecl> {
        let loc = self.current().loc;
        let specifiers = self.parse_macro_specifiers()?;

        // 静态
        let is_static = if self.check(TokenKind::Static) {
            self.advance();
            true
        } else {
            false
        };

        // mutable
        let is_mutable = if let TokenKind::Identifier(ref s) = self.current().kind {
            if s == "mutable" {
                self.advance();
                true
            } else {
                false
            }
        } else {
            false
        };

        // 类型
        let field_type = self.parse_type()?;

        // 字段名
        let name = self.expect_identifier()?;

        // 数组
        let field_type = if self.check(TokenKind::LBracket) {
            self.parse_array_suffix(field_type)?
        } else {
            field_type
        };

        // 位域
        let bit_field_size = if self.check(TokenKind::Colon) {
            self.advance();
            if let TokenKind::IntLiteral(n) = self.current().kind {
                self.advance();
                Some(n as u32)
            } else {
                None
            }
        } else {
            None
        };

        // 默认值
        let default_value = if self.check(TokenKind::Equals) {
            self.advance();
            Some(self.parse_initializer()?)
        } else {
            None
        };

        self.expect(TokenKind::Semicolon)?;

        Ok(FieldDecl {
            name,
            field_type,
            specifiers: Some(specifiers),
            default_value,
            access: self.current_access,
            is_static,
            is_mutable,
            bit_field_size,
            location: loc,
        })
    }

    /// 解析反射方法
    fn parse_reflected_method(&mut self) -> Result<MethodDecl> {
        let loc = self.current().loc;
        let specifiers = self.parse_macro_specifiers()?;

        let mut modifiers = MethodModifiers::default();

        // 修饰符
        loop {
            match &self.current().kind {
                TokenKind::Virtual => {
                    modifiers.is_virtual = true;
                    self.advance();
                }
                TokenKind::Static => {
                    modifiers.is_static = true;
                    self.advance();
                }
                TokenKind::Inline => {
                    modifiers.is_inline = true;
                    self.advance();
                }
                TokenKind::Identifier(s) if s == "constexpr" => {
                    modifiers.is_constexpr = true;
                    self.advance();
                }
                _ => break,
            }
        }

        // 返回类型
        let return_type = self.parse_type()?;

        // 方法名
        let name = self.expect_identifier()?;

        // 参数列表
        self.expect(TokenKind::LParen)?;
        let parameters = self.parse_parameter_list()?;
        self.expect(TokenKind::RParen)?;

        // 后置修饰符
        loop {
            match &self.current().kind {
                TokenKind::Const => {
                    modifiers.is_const = true;
                    self.advance();
                }
                TokenKind::Override => {
                    modifiers.is_override = true;
                    self.advance();
                }
                TokenKind::Identifier(s) if s == "noexcept" => {
                    modifiers.is_noexcept = true;
                    self.advance();
                }
                TokenKind::Identifier(s) if s == "final" => {
                    modifiers.is_final = true;
                    self.advance();
                }
                _ => break,
            }
        }

        // 纯虚函数
        if self.check(TokenKind::Equals) {
            self.advance();
            if let TokenKind::IntLiteral(0) = self.current().kind {
                modifiers.is_pure_virtual = true;
                self.advance();
            }
        }

        // 跳过函数体或分号
        if self.check(TokenKind::LBrace) {
            self.skip_brace_block();
        } else {
            self.expect(TokenKind::Semicolon)?;
        }

        Ok(MethodDecl {
            name,
            return_type,
            parameters,
            specifiers: Some(specifiers),
            access: self.current_access,
            modifiers,
            location: loc,
        })
    }

    /// 解析参数列表
    fn parse_parameter_list(&mut self) -> Result<Vec<ParameterDecl>> {
        let mut params = Vec::new();

        if self.check(TokenKind::RParen) {
            return Ok(params);
        }

        loop {
            // 类型
            let param_type = self.parse_type()?;

            // 参数名 (可选)
            let name = if let TokenKind::Identifier(_) = self.current().kind {
                self.expect_identifier()?
            } else {
                String::new()
            };

            // 数组后缀
            let param_type = if self.check(TokenKind::LBracket) {
                self.parse_array_suffix(param_type)?
            } else {
                param_type
            };

            // 默认值
            let default_value = if self.check(TokenKind::Equals) {
                self.advance();
                Some(self.parse_initializer()?)
            } else {
                None
            };

            params.push(ParameterDecl {
                name,
                param_type,
                default_value,
            });

            if !self.check(TokenKind::Comma) {
                break;
            }
            self.advance();
        }

        Ok(params)
    }

    /// 解析枚举值
    fn parse_enum_values(&mut self) -> Result<Vec<EnumValueDecl>> {
        let mut values = Vec::new();

        while !self.check(TokenKind::RBrace) && !self.is_at_end() {
            let loc = self.current().loc;
            let name = self.expect_identifier()?;

            let value = if self.check(TokenKind::Equals) {
                self.advance();
                if let TokenKind::IntLiteral(n) = self.current().kind {
                    self.advance();
                    Some(n)
                } else {
                    // 跳过复杂表达式
                    self.skip_until_comma_or_brace();
                    None
                }
            } else {
                None
            };

            values.push(EnumValueDecl {
                name,
                value,
                location: loc,
            });

            if self.check(TokenKind::Comma) {
                self.advance();
            }
        }

        Ok(values)
    }

    /// 解析类型
    fn parse_type(&mut self) -> Result<CppType> {
        let mut modifiers = TypeModifiers::default();

        // const
        if self.check(TokenKind::Const) {
            modifiers.is_const = true;
            self.advance();
        }

        // volatile
        if self.check(TokenKind::Volatile) {
            modifiers.is_volatile = true;
            self.advance();
        }

        // 基础类型
        let base_type = self.parse_qualified_identifier()?;

        // 模板参数
        let template_args = if self.check(TokenKind::LAngle) {
            self.parse_template_args()?
        } else {
            Vec::new()
        };

        // 后置 const
        if self.check(TokenKind::Const) {
            modifiers.is_const = true;
            self.advance();
        }

        // 指针/引用
        loop {
            if self.check(TokenKind::Star) {
                modifiers.is_pointer = true;
                self.advance();
                // const 指针
                if self.check(TokenKind::Const) {
                    self.advance();
                }
            } else if self.check(TokenKind::Ampersand) {
                if self
                    .peek_next()
                    .map(|t| matches!(t.kind, TokenKind::Ampersand))
                    .unwrap_or(false)
                {
                    modifiers.is_rvalue_reference = true;
                    self.advance();
                    self.advance();
                } else {
                    modifiers.is_reference = true;
                    self.advance();
                }
            } else {
                break;
            }
        }

        Ok(CppType {
            base_type,
            template_args,
            modifiers,
            array_size: None,
        })
    }

    /// 解析模板参数
    fn parse_template_args(&mut self) -> Result<Vec<CppType>> {
        self.expect(TokenKind::LAngle)?;

        let mut args = Vec::new();
        let mut depth = 1;

        while depth > 0 && !self.is_at_end() {
            if self.check(TokenKind::LAngle) {
                depth += 1;
            } else if self.check(TokenKind::RAngle) {
                depth -= 1;
                if depth == 0 {
                    break;
                }
            }

            if depth == 1 && !self.check(TokenKind::Comma) {
                args.push(self.parse_type()?);
            }

            if self.check(TokenKind::Comma) && depth == 1 {
                self.advance();
            } else if !self.check(TokenKind::RAngle) {
                self.advance();
            }
        }

        self.expect(TokenKind::RAngle)?;

        Ok(args)
    }

    /// 解析数组后缀
    fn parse_array_suffix(&mut self, mut base_type: CppType) -> Result<CppType> {
        self.expect(TokenKind::LBracket)?;

        if let TokenKind::IntLiteral(n) = self.current().kind {
            base_type.array_size = Some(n as usize);
            self.advance();
        } else {
            base_type.array_size = Some(0); // 动态数组
        }

        self.expect(TokenKind::RBracket)?;

        Ok(base_type)
    }

    /// 解析限定标识符 (如 std::vector)
    fn parse_qualified_identifier(&mut self) -> Result<String> {
        let mut name = String::new();

        // 可能以 :: 开头 (全局作用域)
        if self.check(TokenKind::DoubleColon) {
            name.push_str("::");
            self.advance();
        }

        name.push_str(&self.expect_identifier()?);

        while self.check(TokenKind::DoubleColon) {
            name.push_str("::");
            self.advance();
            name.push_str(&self.expect_identifier()?);
        }

        Ok(name)
    }

    /// 解析初始化表达式
    fn parse_initializer(&mut self) -> Result<String> {
        let mut expr = String::new();
        let mut depth = 0;

        while !self.is_at_end() {
            match &self.current().kind {
                TokenKind::LParen | TokenKind::LBrace | TokenKind::LBracket => depth += 1,
                TokenKind::RParen | TokenKind::RBrace | TokenKind::RBracket => {
                    if depth == 0 {
                        break;
                    }
                    depth -= 1;
                }
                TokenKind::Semicolon | TokenKind::Comma if depth == 0 => break,
                _ => {}
            }

            expr.push_str(&self.token_to_string(&self.current().kind));
            expr.push(' ');
            self.advance();
        }

        Ok(expr.trim().to_string())
    }

    //=========================================================================
    // 辅助方法
    //=========================================================================

    fn current(&self) -> &Token {
        self.tokens
            .get(self.pos)
            .unwrap_or(&self.tokens[self.tokens.len() - 1])
    }

    fn peek_next(&self) -> Option<&Token> {
        self.tokens.get(self.pos + 1)
    }

    fn advance(&mut self) {
        if !self.is_at_end() {
            self.pos += 1;
        }
    }

    fn is_at_end(&self) -> bool {
        matches!(self.current().kind, TokenKind::Eof)
    }

    fn check(&self, kind: TokenKind) -> bool {
        std::mem::discriminant(&self.current().kind) == std::mem::discriminant(&kind)
    }

    fn expect(&mut self, kind: TokenKind) -> Result<()> {
        if self.check(kind.clone()) {
            self.advance();
            Ok(())
        } else {
            bail!(
                "Expected {:?}, got {:?} at line {}",
                kind,
                self.current().kind,
                self.current().loc.line
            )
        }
    }

    fn expect_identifier(&mut self) -> Result<String> {
        if let TokenKind::Identifier(s) = &self.current().kind {
            let name = s.clone();
            self.advance();
            Ok(name)
        } else {
            bail!(
                "Expected identifier, got {:?} at line {}",
                self.current().kind,
                self.current().loc.line
            )
        }
    }

    fn make_qualified_name(&self, name: &str) -> String {
        if self.current_namespace.is_empty() {
            name.to_string()
        } else {
            format!("{}::{}", self.current_namespace.join("::"), name)
        }
    }

    fn extract_include(&self, directive: &str) -> Option<String> {
        if let Some(start) = directive.find('"') {
            if let Some(end) = directive[start + 1..].find('"') {
                return Some(directive[start + 1..start + 1 + end].to_string());
            }
        }
        if let Some(start) = directive.find('<') {
            if let Some(end) = directive[start + 1..].find('>') {
                return Some(directive[start + 1..start + 1 + end].to_string());
            }
        }
        None
    }

    fn skip_class_declaration(&mut self) {
        // 跳过到 { 或 ;
        while !self.is_at_end() {
            if self.check(TokenKind::LBrace) {
                self.skip_brace_block();
                if self.check(TokenKind::Semicolon) {
                    self.advance();
                }
                return;
            }
            if self.check(TokenKind::Semicolon) {
                self.advance();
                return;
            }
            self.advance();
        }
    }

    fn skip_enum_declaration(&mut self) {
        self.skip_class_declaration();
    }

    fn skip_brace_block(&mut self) {
        if !self.check(TokenKind::LBrace) {
            return;
        }

        self.advance();
        let mut depth = 1;

        while depth > 0 && !self.is_at_end() {
            if self.check(TokenKind::LBrace) {
                depth += 1;
            } else if self.check(TokenKind::RBrace) {
                depth -= 1;
            }
            self.advance();
        }
    }

    fn skip_until_semicolon_or_brace(&mut self) {
        let mut depth = 0;
        while !self.is_at_end() {
            match &self.current().kind {
                TokenKind::LBrace | TokenKind::LParen | TokenKind::LBracket => depth += 1,
                TokenKind::RBrace => {
                    if depth == 0 {
                        return;
                    }
                    depth -= 1;
                }
                TokenKind::RParen | TokenKind::RBracket => {
                    if depth > 0 {
                        depth -= 1;
                    }
                }
                TokenKind::Semicolon if depth == 0 => {
                    self.advance();
                    return;
                }
                _ => {}
            }
            self.advance();
        }
    }

    fn skip_until_comma_or_brace(&mut self) {
        let mut depth = 0;
        while !self.is_at_end() {
            match &self.current().kind {
                TokenKind::LParen | TokenKind::LBrace | TokenKind::LBracket => depth += 1,
                TokenKind::RParen | TokenKind::RBracket => {
                    if depth > 0 {
                        depth -= 1;
                    }
                }
                TokenKind::RBrace => {
                    if depth == 0 {
                        return;
                    }
                    depth -= 1;
                }
                TokenKind::Comma if depth == 0 => return,
                _ => {}
            }
            self.advance();
        }
    }

    fn token_to_string(&self, kind: &TokenKind) -> String {
        match kind {
            TokenKind::Identifier(s) => s.clone(),
            TokenKind::IntLiteral(n) => n.to_string(),
            TokenKind::FloatLiteral(n) => n.to_string(),
            TokenKind::StringLiteral(s) => format!("\"{}\"", s),
            TokenKind::CharLiteral(c) => format!("'{}'", c),
            TokenKind::LParen => "(".to_string(),
            TokenKind::RParen => ")".to_string(),
            TokenKind::LBrace => "{".to_string(),
            TokenKind::RBrace => "}".to_string(),
            TokenKind::LBracket => "[".to_string(),
            TokenKind::RBracket => "]".to_string(),
            TokenKind::LAngle => "<".to_string(),
            TokenKind::RAngle => ">".to_string(),
            TokenKind::Semicolon => ";".to_string(),
            TokenKind::Colon => ":".to_string(),
            TokenKind::DoubleColon => "::".to_string(),
            TokenKind::Comma => ",".to_string(),
            TokenKind::Dot => ".".to_string(),
            TokenKind::Arrow => "->".to_string(),
            TokenKind::Star => "*".to_string(),
            TokenKind::Ampersand => "&".to_string(),
            TokenKind::Equals => "=".to_string(),
            TokenKind::Plus => "+".to_string(),
            TokenKind::Minus => "-".to_string(),
            TokenKind::Slash => "/".to_string(),
            _ => String::new(),
        }
    }
}
