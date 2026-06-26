/*******************************************************************************
 * 文件: ast.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   抽象语法树 (AST) 定义 (生产级)
 *   - 完整的 C++ 类型系统表示
 *   - 支持模板、嵌套类型、引用/指针
 *   - 精确的源码位置追踪
 *
 ******************************************************************************/

use super::lexer::SourceLocation;
use super::specifiers::Specifiers;

//=============================================================================
// 类型表示
//=============================================================================

/// C++ 类型
#[derive(Debug, Clone)]
pub struct CppType {
    /// 基础类型名 (如 "int", "std::string", "MyClass")
    pub base_type: String,
    /// 模板参数 (如 vector<int> 中的 int)
    pub template_args: Vec<CppType>,
    /// 修饰符
    pub modifiers: TypeModifiers,
    /// 数组维度 (None = 非数组, Some(0) = 动态数组, Some(n) = 固定大小)
    pub array_size: Option<usize>,
}

impl CppType {
    pub fn simple(name: &str) -> Self {
        Self {
            base_type: name.to_string(),
            template_args: Vec::new(),
            modifiers: TypeModifiers::default(),
            array_size: None,
        }
    }

    pub fn with_pointer(mut self) -> Self {
        self.modifiers.is_pointer = true;
        self
    }

    pub fn with_reference(mut self) -> Self {
        self.modifiers.is_reference = true;
        self
    }

    pub fn with_const(mut self) -> Self {
        self.modifiers.is_const = true;
        self
    }

    /// 转换为字符串表示
    pub fn to_string(&self) -> String {
        let mut s = String::new();

        if self.modifiers.is_const {
            s.push_str("const ");
        }

        s.push_str(&self.base_type);

        if !self.template_args.is_empty() {
            s.push('<');
            for (i, arg) in self.template_args.iter().enumerate() {
                if i > 0 {
                    s.push_str(", ");
                }
                s.push_str(&arg.to_string());
            }
            s.push('>');
        }

        if self.modifiers.is_pointer {
            s.push('*');
        }
        if self.modifiers.is_reference {
            s.push('&');
        }
        if self.modifiers.is_rvalue_reference {
            s.push_str("&&");
        }

        if let Some(size) = self.array_size {
            if size > 0 {
                s.push_str(&format!("[{}]", size));
            } else {
                s.push_str("[]");
            }
        }

        s
    }
}

/// 类型修饰符
#[derive(Debug, Clone, Default)]
pub struct TypeModifiers {
    pub is_const: bool,
    pub is_volatile: bool,
    pub is_pointer: bool,
    pub is_reference: bool,
    pub is_rvalue_reference: bool,
    pub is_mutable: bool,
}

//=============================================================================
// 类/结构体声明
//=============================================================================

/// 访问修饰符
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AccessSpecifier {
    #[default]
    Private,
    Protected,
    Public,
}

/// 类/结构体声明
#[derive(Debug, Clone)]
pub struct ClassDecl {
    /// 类型 (class 或 struct)
    pub kind: ClassKind,
    /// 类名
    pub name: String,
    /// 完整限定名 (包括命名空间)
    pub qualified_name: String,
    /// 基类列表
    pub base_classes: Vec<BaseClass>,
    /// API 导出宏 (如 LIMX_CORE_API)
    pub api_macro: Option<String>,
    /// 反射说明符
    pub specifiers: Specifiers,
    /// 成员列表
    pub members: Vec<ClassMember>,
    /// 源码位置
    pub location: SourceLocation,
    /// 是否有 LGENERATED_BODY
    pub has_generated_body: bool,
}

/// 类类型
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClassKind {
    Class,
    Struct,
}

/// 基类信息
#[derive(Debug, Clone)]
pub struct BaseClass {
    pub name: String,
    pub access: AccessSpecifier,
    pub is_virtual: bool,
}

/// 类成员
#[derive(Debug, Clone)]
pub enum ClassMember {
    Field(FieldDecl),
    Method(MethodDecl),
    Constructor(ConstructorDecl),
    Destructor(DestructorDecl),
    NestedType(Box<ClassDecl>),
    EnumDecl(EnumDecl),
    TypeAlias(TypeAliasDecl),
    AccessSpecifier(AccessSpecifier),
}

//=============================================================================
// 字段声明
//=============================================================================

/// 字段/属性声明
#[derive(Debug, Clone)]
pub struct FieldDecl {
    /// 字段名
    pub name: String,
    /// 类型
    pub field_type: CppType,
    /// 反射说明符 (来自 LPROPERTY)
    pub specifiers: Option<Specifiers>,
    /// 默认值表达式
    pub default_value: Option<String>,
    /// 访问修饰符
    pub access: AccessSpecifier,
    /// 是否是静态成员
    pub is_static: bool,
    /// 是否是 mutable
    pub is_mutable: bool,
    /// 位域大小 (如果有)
    pub bit_field_size: Option<u32>,
    /// 源码位置
    pub location: SourceLocation,
}

impl FieldDecl {
    /// 是否是反射属性
    pub fn is_reflected(&self) -> bool {
        self.specifiers.is_some()
    }
}

//=============================================================================
// 方法声明
//=============================================================================

/// 方法声明
#[derive(Debug, Clone)]
pub struct MethodDecl {
    /// 方法名
    pub name: String,
    /// 返回类型
    pub return_type: CppType,
    /// 参数列表
    pub parameters: Vec<ParameterDecl>,
    /// 反射说明符 (来自 LFUNCTION)
    pub specifiers: Option<Specifiers>,
    /// 访问修饰符
    pub access: AccessSpecifier,
    /// 方法修饰符
    pub modifiers: MethodModifiers,
    /// 源码位置
    pub location: SourceLocation,
}

/// 方法修饰符
#[derive(Debug, Clone, Default)]
pub struct MethodModifiers {
    pub is_virtual: bool,
    pub is_override: bool,
    pub is_final: bool,
    pub is_static: bool,
    pub is_const: bool,
    pub is_noexcept: bool,
    pub is_inline: bool,
    pub is_explicit: bool,
    pub is_constexpr: bool,
    pub is_pure_virtual: bool,
}

/// 参数声明
#[derive(Debug, Clone)]
pub struct ParameterDecl {
    /// 参数名 (可能为空)
    pub name: String,
    /// 参数类型
    pub param_type: CppType,
    /// 默认值
    pub default_value: Option<String>,
}

/// 构造函数声明
#[derive(Debug, Clone)]
pub struct ConstructorDecl {
    /// 参数列表
    pub parameters: Vec<ParameterDecl>,
    /// 访问修饰符
    pub access: AccessSpecifier,
    /// 是否是 explicit
    pub is_explicit: bool,
    /// 是否是 default
    pub is_default: bool,
    /// 是否是 delete
    pub is_deleted: bool,
    /// 初始化列表
    pub initializer_list: Vec<MemberInitializer>,
    /// 源码位置
    pub location: SourceLocation,
}

/// 析构函数声明
#[derive(Debug, Clone)]
pub struct DestructorDecl {
    /// 访问修饰符
    pub access: AccessSpecifier,
    /// 是否是虚函数
    pub is_virtual: bool,
    /// 是否是 default
    pub is_default: bool,
    /// 源码位置
    pub location: SourceLocation,
}

/// 成员初始化
#[derive(Debug, Clone)]
pub struct MemberInitializer {
    pub member_name: String,
    pub value: String,
}

//=============================================================================
// 枚举声明
//=============================================================================

/// 枚举声明
#[derive(Debug, Clone)]
pub struct EnumDecl {
    /// 枚举名
    pub name: String,
    /// 是否是 enum class
    pub is_scoped: bool,
    /// 底层类型
    pub underlying_type: Option<CppType>,
    /// 反射说明符
    pub specifiers: Option<Specifiers>,
    /// 枚举值列表
    pub values: Vec<EnumValueDecl>,
    /// 源码位置
    pub location: SourceLocation,
}

/// 枚举值声明
#[derive(Debug, Clone)]
pub struct EnumValueDecl {
    /// 值名
    pub name: String,
    /// 显式值 (如果有)
    pub value: Option<i64>,
    /// 源码位置
    pub location: SourceLocation,
}

//=============================================================================
// 其他声明
//=============================================================================

/// 类型别名声明
#[derive(Debug, Clone)]
pub struct TypeAliasDecl {
    /// 别名
    pub name: String,
    /// 原类型
    pub aliased_type: CppType,
    /// 源码位置
    pub location: SourceLocation,
}

/// 委托声明
#[derive(Debug, Clone)]
pub struct DelegateDecl {
    /// 委托名
    pub name: String,
    /// 返回类型
    pub return_type: CppType,
    /// 参数列表
    pub parameters: Vec<ParameterDecl>,
    /// 反射说明符
    pub specifiers: Specifiers,
    /// 源码位置
    pub location: SourceLocation,
}

/// 命名空间声明
#[derive(Debug, Clone)]
pub struct NamespaceDecl {
    /// 命名空间名
    pub name: String,
    /// 是否是内联命名空间
    pub is_inline: bool,
    /// 子声明
    pub declarations: Vec<Declaration>,
    /// 源码位置
    pub location: SourceLocation,
}

//=============================================================================
// 顶级声明
//=============================================================================

/// 顶级声明
#[derive(Debug, Clone)]
pub enum Declaration {
    Class(ClassDecl),
    Enum(EnumDecl),
    Delegate(DelegateDecl),
    Namespace(NamespaceDecl),
    TypeAlias(TypeAliasDecl),
}

/// 翻译单元 (一个头文件的 AST)
#[derive(Debug, Clone)]
pub struct TranslationUnit {
    /// 文件路径
    pub file_path: String,
    /// 声明列表
    pub declarations: Vec<Declaration>,
    /// 包含的头文件
    pub includes: Vec<String>,
}

impl TranslationUnit {
    pub fn new(file_path: String) -> Self {
        Self {
            file_path,
            declarations: Vec::new(),
            includes: Vec::new(),
        }
    }

    /// 获取所有反射类
    pub fn get_reflected_classes(&self) -> Vec<&ClassDecl> {
        let mut result = Vec::new();
        self.collect_classes(&self.declarations, &mut result);
        result
    }

    fn collect_classes<'a>(&'a self, decls: &'a [Declaration], result: &mut Vec<&'a ClassDecl>) {
        for decl in decls {
            match decl {
                Declaration::Class(c) => {
                    if !c.specifiers.items.is_empty() {
                        result.push(c);
                    }
                }
                Declaration::Namespace(ns) => {
                    self.collect_classes(&ns.declarations, result);
                }
                _ => {}
            }
        }
    }

    /// 获取所有反射枚举
    pub fn get_reflected_enums(&self) -> Vec<&EnumDecl> {
        let mut result = Vec::new();
        self.collect_enums(&self.declarations, &mut result);
        result
    }

    fn collect_enums<'a>(&'a self, decls: &'a [Declaration], result: &mut Vec<&'a EnumDecl>) {
        for decl in decls {
            match decl {
                Declaration::Enum(e) => {
                    if e.specifiers.is_some() {
                        result.push(e);
                    }
                }
                Declaration::Namespace(ns) => {
                    self.collect_enums(&ns.declarations, result);
                }
                _ => {}
            }
        }
    }

    /// 获取所有委托
    pub fn get_delegates(&self) -> Vec<&DelegateDecl> {
        let mut result = Vec::new();
        self.collect_delegates(&self.declarations, &mut result);
        result
    }

    fn collect_delegates<'a>(
        &'a self,
        decls: &'a [Declaration],
        result: &mut Vec<&'a DelegateDecl>,
    ) {
        for decl in decls {
            match decl {
                Declaration::Delegate(d) => {
                    result.push(d);
                }
                Declaration::Namespace(ns) => {
                    self.collect_delegates(&ns.declarations, result);
                }
                _ => {}
            }
        }
    }
}
