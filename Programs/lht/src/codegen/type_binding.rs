// ============================================================
// 文件名称：type_binding.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：编译期类型安全的属性绑定 — 生成 C++ 强类型属性
//           访问器和零开销反射适配器代码，将 UE5 的纯运行时
//           属性访问提升到编译期验证，消除类型转换错误
// 功能描述：类型安全属性绑定代码生成器 — 为每个 LCLASS/LSTRUCT
//           生成强类型 PropertyAccessor<T>，编译期验证属性名
//           和类型的正确性，生成 constexpr 属性元数据表
// 技术特性：C++ constexpr 元编程、CRTP 零开销适配器模式、
//           编译期属性名字符串哈希、类型萃取 (type traits)
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ TypeBindingGenerator       │ 类型绑定代码生成器            │
// │ BindingClass               │ 待生成绑定的类描述            │
// │ BindingProperty            │ 待生成绑定的属性描述          │
// │ BindingMethod              │ 待生成绑定的方法描述          │
// │ GeneratedBinding           │ 生成的绑定代码结果            │
// │ BindingOptions             │ 生成选项                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建生成器                    │
// │ generate_binding()         │ 为单个类生成绑定代码           │
// │ generate_all_bindings()    │ 为所有类生成绑定代码           │
// │ generate_accessor_code()   │ 生成属性访问器代码             │
// │ generate_meta_table()      │ 生成 constexpr 元数据表       │
// │ generate_adapter_code()    │ 生成 CRTP 适配器代码          │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};

// =============================================================================
// 绑定描述数据结构
// =============================================================================

/// 属性访问权限
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum PropertyAccess {
    /// 读写
    ReadWrite,
    /// 只读
    ReadOnly,
    /// 只写
    WriteOnly,
}

/// 属性复制模式
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ReplicationMode {
    /// 不复制
    None,
    /// 服务器到客户端
    ServerToClient,
    /// 双向
    Bidirectional,
}

/// 待生成绑定的属性描述
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BindingProperty {
    /// 属性名
    pub name: String,
    /// C++ 类型名
    pub cpp_type: String,
    /// 访问权限
    pub access: PropertyAccess,
    /// 是否可序列化
    pub serializable: bool,
    /// 是否可在编辑器中编辑
    pub editable: bool,
    /// 复制模式
    pub replication: ReplicationMode,
    /// 分类 (编辑器中的分组名)
    pub category: Option<String>,
    /// 显示名
    pub display_name: Option<String>,
    /// 提示文本
    pub tooltip: Option<String>,
    /// 值范围 (数值类型)
    pub range: Option<(f64, f64)>,
    /// 是否为 Transient (不持久化)
    pub transient: bool,
}

/// 待生成绑定的方法描述
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BindingMethod {
    /// 方法名
    pub name: String,
    /// 返回类型
    pub return_type: String,
    /// 参数列表 (名称, 类型)
    pub parameters: Vec<(String, String)>,
    /// 是否为 const 方法
    pub is_const: bool,
    /// 是否为虚方法
    pub is_virtual: bool,
    /// 是否为 RPC 方法
    pub is_rpc: bool,
}

/// 待生成绑定的类描述
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BindingClass {
    /// 类名
    pub class_name: String,
    /// 命名空间
    pub namespace: Option<String>,
    /// 父类名
    pub parent_class: Option<String>,
    /// 属性列表
    pub properties: Vec<BindingProperty>,
    /// 方法列表
    pub methods: Vec<BindingMethod>,
    /// 是否抽象类
    pub is_abstract: bool,
    /// 所属模块
    pub module_name: String,
}

impl BindingClass {
    /// 获取完全限定名
    pub fn qualified_name(&self) -> String {
        match &self.namespace {
            Some(ns) => format!("{}::{}", ns, self.class_name),
            None => self.class_name.clone(),
        }
    }
}

// =============================================================================
// 生成选项
// =============================================================================

/// 绑定代码生成选项
#[derive(Debug, Clone)]
pub struct BindingOptions {
    /// 是否生成 constexpr 元数据表
    pub generate_meta_table: bool,
    /// 是否生成 CRTP 适配器
    pub generate_crtp_adapter: bool,
    /// 是否生成属性访问器
    pub generate_accessors: bool,
    /// 是否生成类型特征 (type traits)
    pub generate_type_traits: bool,
    /// 是否生成编辑器元数据
    pub generate_editor_metadata: bool,
    /// 缩进字符串
    pub indent: String,
}

impl Default for BindingOptions {
    fn default() -> Self {
        Self {
            generate_meta_table: true,
            generate_crtp_adapter: true,
            generate_accessors: true,
            generate_type_traits: true,
            generate_editor_metadata: true,
            indent: "    ".to_string(),
        }
    }
}

// =============================================================================
// 生成结果
// =============================================================================

/// 单个类的生成绑定代码
#[derive(Debug, Clone)]
pub struct GeneratedBinding {
    /// 类名
    pub class_name: String,
    /// 头文件内容 (.binding.h)
    pub header_content: String,
    /// 实现文件内容 (.binding.cpp)
    pub impl_content: String,
    /// 生成的属性访问器数量
    pub accessor_count: usize,
    /// 生成的方法绑定数量
    pub method_binding_count: usize,
}

// =============================================================================
// 类型绑定生成器
// =============================================================================

/// 类型安全属性绑定代码生成器
pub struct TypeBindingGenerator {
    /// 生成选项
    options: BindingOptions,
}

impl TypeBindingGenerator {
    /// 创建生成器
    pub fn new(options: BindingOptions) -> Self {
        Self { options }
    }

    /// 使用默认选项创建
    pub fn with_defaults() -> Self {
        Self::new(BindingOptions::default())
    }

    /// 为单个类生成绑定代码
    pub fn generate_binding(&self, class: &BindingClass) -> GeneratedBinding {
        let mut header = String::with_capacity(8192);
        let mut impl_code = String::with_capacity(4096);

        let indent = &self.options.indent;
        let class_name = &class.class_name;

        // 头文件头部
        self.emit_header_preamble(&mut header, class);

        // constexpr 属性元数据表
        if self.options.generate_meta_table {
            self.emit_meta_table(&mut header, class, indent);
        }

        // 强类型属性访问器
        if self.options.generate_accessors {
            self.emit_property_accessors(&mut header, class, indent);
        }

        // CRTP 零开销反射适配器
        if self.options.generate_crtp_adapter {
            self.emit_crtp_adapter(&mut header, class, indent);
        }

        // 类型特征
        if self.options.generate_type_traits {
            self.emit_type_traits(&mut header, class);
        }

        // 头文件尾部
        header.push_str(&format!(
            "\n#endif // {}_BINDING_H\n",
            class_name.to_uppercase()
        ));

        // 实现文件
        self.emit_impl_file(&mut impl_code, class, indent);

        let accessor_count = class.properties.len();
        let method_binding_count = class.methods.len();

        GeneratedBinding {
            class_name: class_name.clone(),
            header_content: header,
            impl_content: impl_code,
            accessor_count,
            method_binding_count,
        }
    }

    /// 为多个类生成绑定代码
    pub fn generate_all_bindings(&self, classes: &[BindingClass]) -> Vec<GeneratedBinding> {
        classes.iter().map(|c| self.generate_binding(c)).collect()
    }

    // =========================================================================
    // 头文件前导
    // =========================================================================

    fn emit_header_preamble(&self, out: &mut String, class: &BindingClass) {
        let guard = format!("{}_BINDING_H", class.class_name.to_uppercase());
        out.push_str(&format!(
            "// 自动生成 — Limx Header Tool (LHT) 类型安全属性绑定\n\
             // 类: {}\n\
             // 模块: {}\n\
             // 警告: 请勿手动修改此文件\n\n\
             #ifndef {guard}\n\
             #define {guard}\n\n\
             #include \"Limx/Reflection/PropertyAccessor.h\"\n\
             #include \"Limx/Reflection/TypeTraits.h\"\n\n",
            class.qualified_name(),
            class.module_name,
        ));
    }

    // =========================================================================
    // constexpr 属性元数据表
    // =========================================================================

    fn emit_meta_table(&self, out: &mut String, class: &BindingClass, indent: &str) {
        let class_name = &class.class_name;

        out.push_str(&format!(
            "// ── constexpr 属性元数据表 ──────────────────────────\n\
             namespace Limx::Meta {{\n\n"
        ));

        // 属性名哈希常量
        out.push_str(&format!("{}// 编译期属性名哈希\n", indent));
        for prop in &class.properties {
            let hash = compile_time_hash(&prop.name);
            out.push_str(&format!(
                "{}constexpr uint64_t {}_{}_{} = 0x{:016X}ULL;\n",
                indent, class_name, prop.name, "HASH", hash,
            ));
        }
        out.push('\n');

        // 属性数量常量
        out.push_str(&format!(
            "{}constexpr size_t {}_PROPERTY_COUNT = {};\n\n",
            indent,
            class_name.to_uppercase(),
            class.properties.len(),
        ));

        // 属性元数据结构
        out.push_str(&format!(
            "{}struct {}PropertyMeta {{\n\
             {indent}{indent}const char* name;\n\
             {indent}{indent}const char* type_name;\n\
             {indent}{indent}uint64_t name_hash;\n\
             {indent}{indent}size_t offset;\n\
             {indent}{indent}bool is_readonly;\n\
             {indent}{indent}bool is_serializable;\n\
             {indent}{indent}bool is_editable;\n\
             {indent}{indent}bool is_replicated;\n\
             {indent}}};\n\n",
            indent, class_name,
        ));

        // constexpr 元数据数组
        out.push_str(&format!(
            "{}constexpr {}PropertyMeta {}_PROPERTIES[] = {{\n",
            indent,
            class_name,
            class_name.to_uppercase(),
        ));
        for prop in &class.properties {
            let hash = compile_time_hash(&prop.name);
            out.push_str(&format!(
                "{indent}{indent}{{ \"{name}\", \"{type}\", 0x{hash:016X}ULL, \
                 offsetof({cls}, {name}), {ro}, {ser}, {ed}, {rep} }},\n",
                name = prop.name,
                type = prop.cpp_type,
                hash = hash,
                cls = class_name,
                ro = if prop.access == PropertyAccess::ReadOnly { "true" } else { "false" },
                ser = if prop.serializable { "true" } else { "false" },
                ed = if prop.editable { "true" } else { "false" },
                rep = if prop.replication != ReplicationMode::None { "true" } else { "false" },
            ));
        }
        out.push_str(&format!("{}}};\n\n", indent));

        // 编译期属性查找
        out.push_str(&format!(
            "{}// 编译期属性查找 (O(N)，N 通常 < 50)\n\
             {}constexpr const {}PropertyMeta* {}_FindProperty(uint64_t hash) {{\n\
             {indent}{indent}for (size_t i = 0; i < {}_PROPERTY_COUNT; ++i) {{\n\
             {indent}{indent}{indent}if ({}_PROPERTIES[i].name_hash == hash) {{\n\
             {indent}{indent}{indent}{indent}return &{}_PROPERTIES[i];\n\
             {indent}{indent}{indent}}}\n\
             {indent}{indent}}}\n\
             {indent}{indent}return nullptr;\n\
             {indent}}}\n\n",
            indent,
            indent,
            class_name,
            class_name,
            class_name.to_uppercase(),
            class_name.to_uppercase(),
            class_name.to_uppercase(),
        ));

        out.push_str("} // namespace Limx::Meta\n\n");
    }

    // =========================================================================
    // 强类型属性访问器
    // =========================================================================

    fn emit_property_accessors(&self, out: &mut String, class: &BindingClass, indent: &str) {
        let class_name = &class.class_name;

        out.push_str(&format!(
            "// ── 强类型属性访问器 ──────────────────────────────\n\
             namespace Limx::Binding {{\n\n"
        ));

        // 为每个属性生成类型安全的 Accessor
        out.push_str(&format!(
            "{}template<typename OwnerType>\n\
             {}class {}Accessors {{\n\
             {}public:\n\
             {indent}{indent}explicit {}Accessors(OwnerType& owner) : owner_(owner) {{}}\n\n",
            indent, indent, class_name, indent, class_name,
        ));

        for prop in &class.properties {
            // Getter (始终生成)
            out.push_str(&format!(
                "{indent}{indent}// 属性: {} ({})\n",
                prop.name, prop.cpp_type,
            ));

            out.push_str(&format!(
                "{indent}{indent}const {type}& Get{pascal}() const {{ return owner_.{name}; }}\n",
                type = prop.cpp_type,
                pascal = to_pascal_case(&prop.name),
                name = prop.name,
            ));

            // Setter (仅非只读属性)
            if prop.access != PropertyAccess::ReadOnly {
                out.push_str(&format!(
                    "{indent}{indent}void Set{pascal}(const {type}& value) {{ owner_.{name} = value; }}\n",
                    type = prop.cpp_type,
                    pascal = to_pascal_case(&prop.name),
                    name = prop.name,
                ));
            }

            // 带验证的 Setter (有范围约束的)
            if let Some((min, max)) = &prop.range {
                out.push_str(&format!(
                    "{indent}{indent}bool SetClamped{pascal}(const {type}& value) {{\n\
                     {indent}{indent}{indent}if (value >= static_cast<{type}>({min}) && value <= static_cast<{type}>({max})) {{\n\
                     {indent}{indent}{indent}{indent}owner_.{name} = value;\n\
                     {indent}{indent}{indent}{indent}return true;\n\
                     {indent}{indent}{indent}}}\n\
                     {indent}{indent}{indent}return false;\n\
                     {indent}{indent}}}\n",
                    type = prop.cpp_type,
                    pascal = to_pascal_case(&prop.name),
                    name = prop.name,
                    min = min,
                    max = max,
                ));
            }

            out.push('\n');
        }

        out.push_str(&format!(
            "{indent}private:\n\
             {indent}{indent}OwnerType& owner_;\n\
             {indent}}};\n\n",
        ));

        out.push_str("} // namespace Limx::Binding\n\n");
    }

    // =========================================================================
    // CRTP 零开销反射适配器
    // =========================================================================

    fn emit_crtp_adapter(&self, out: &mut String, class: &BindingClass, indent: &str) {
        let class_name = &class.class_name;

        out.push_str(&format!(
            "// ── CRTP 零开销反射适配器 ─────────────────────────\n\
             namespace Limx::Reflection {{\n\n\
             {}template<typename Derived>\n\
             {}class {}ReflectionAdapter {{\n\
             {}public:\n",
            indent, indent, class_name, indent,
        ));

        // GetPropertyByName — 编译期哈希查找
        out.push_str(&format!(
            "{indent}{indent}// 通过属性名获取值 (运行时字符串匹配)\n\
             {indent}{indent}bool GetPropertyValue(const char* name, void* out_value, size_t buffer_size) const {{\n\
             {indent}{indent}{indent}const auto* self = static_cast<const Derived*>(this);\n",
        ));

        for (i, prop) in class.properties.iter().enumerate() {
            let prefix = if i == 0 { "if" } else { "else if" };
            out.push_str(&format!(
                "{indent}{indent}{indent}{prefix} (strcmp(name, \"{name}\") == 0) {{\n\
                 {indent}{indent}{indent}{indent}if (buffer_size >= sizeof({type})) {{\n\
                 {indent}{indent}{indent}{indent}{indent}memcpy(out_value, &self->{name}, sizeof({type}));\n\
                 {indent}{indent}{indent}{indent}{indent}return true;\n\
                 {indent}{indent}{indent}{indent}}}\n\
                 {indent}{indent}{indent}}}\n",
                prefix = prefix,
                name = prop.name,
                type = prop.cpp_type,
            ));
        }

        out.push_str(&format!(
            "{indent}{indent}{indent}return false;\n\
             {indent}{indent}}}\n\n",
        ));

        // SetPropertyByName
        out.push_str(&format!(
            "{indent}{indent}// 通过属性名设置值\n\
             {indent}{indent}bool SetPropertyValue(const char* name, const void* value, size_t value_size) {{\n\
             {indent}{indent}{indent}auto* self = static_cast<Derived*>(this);\n",
        ));

        for (i, prop) in class.properties.iter().enumerate() {
            if prop.access == PropertyAccess::ReadOnly {
                continue;
            }
            let prefix = if i == 0 { "if" } else { "else if" };
            out.push_str(&format!(
                "{indent}{indent}{indent}{prefix} (strcmp(name, \"{name}\") == 0) {{\n\
                 {indent}{indent}{indent}{indent}if (value_size >= sizeof({type})) {{\n\
                 {indent}{indent}{indent}{indent}{indent}memcpy(&self->{name}, value, sizeof({type}));\n\
                 {indent}{indent}{indent}{indent}{indent}return true;\n\
                 {indent}{indent}{indent}{indent}}}\n\
                 {indent}{indent}{indent}}}\n",
                prefix = prefix,
                name = prop.name,
                type = prop.cpp_type,
            ));
        }

        out.push_str(&format!(
            "{indent}{indent}{indent}return false;\n\
             {indent}{indent}}}\n\n",
        ));

        // GetPropertyCount
        out.push_str(&format!(
            "{indent}{indent}// 属性数量\n\
             {indent}{indent}static constexpr size_t GetPropertyCount() {{ return {}; }}\n\n",
            class.properties.len(),
        ));

        // ForEachProperty — 编译期属性遍历
        out.push_str(&format!(
            "{indent}{indent}// 遍历所有属性 (零开销，编译期展开)\n\
             {indent}{indent}template<typename Visitor>\n\
             {indent}{indent}void ForEachProperty(Visitor&& visitor) const {{\n\
             {indent}{indent}{indent}const auto* self = static_cast<const Derived*>(this);\n",
        ));

        for prop in &class.properties {
            out.push_str(&format!(
                "{indent}{indent}{indent}visitor(\"{name}\", \"{type}\", &self->{name});\n",
                name = prop.name,
                type = prop.cpp_type,
            ));
        }

        out.push_str(&format!("{indent}{indent}}}\n",));

        out.push_str(&format!(
            "{indent}}};\n\n\
             }} // namespace Limx::Reflection\n\n",
        ));
    }

    // =========================================================================
    // 类型特征
    // =========================================================================

    fn emit_type_traits(&self, out: &mut String, class: &BindingClass) {
        let class_name = &class.class_name;

        out.push_str(&format!(
            "// ── 类型特征 (Type Traits) ─────────────────────────\n\
             namespace Limx {{\n\n\
             template<> struct TypeInfo<{class_name}> {{\n\
             {indent}static constexpr const char* Name = \"{class_name}\";\n\
             {indent}static constexpr size_t PropertyCount = {count};\n\
             {indent}static constexpr bool IsAbstract = {is_abstract};\n\
             {indent}static constexpr bool HasParent = {has_parent};\n",
            indent = self.options.indent,
            count = class.properties.len(),
            is_abstract = if class.is_abstract { "true" } else { "false" },
            has_parent = if class.parent_class.is_some() {
                "true"
            } else {
                "false"
            },
        ));

        if let Some(parent) = &class.parent_class {
            out.push_str(&format!(
                "{}using ParentType = {};\n",
                self.options.indent, parent,
            ));
        }

        out.push_str("};\n\n} // namespace Limx\n\n");
    }

    // =========================================================================
    // 实现文件
    // =========================================================================

    fn emit_impl_file(&self, out: &mut String, class: &BindingClass, indent: &str) {
        let class_name = &class.class_name;

        out.push_str(&format!(
            "// 自动生成 — Limx Header Tool (LHT) 类型绑定实现\n\
             // 类: {}\n\n\
             #include \"{}.binding.h\"\n\n",
            class.qualified_name(),
            class_name,
        ));

        // 运行时注册函数
        out.push_str(&format!(
            "namespace Limx::Internal {{\n\n\
             {indent}void Register{class_name}Properties() {{\n\
             {indent}{indent}// 自动注册所有属性到运行时反射系统\n",
        ));

        for prop in &class.properties {
            let flags = build_property_flags(prop);
            out.push_str(&format!(
                "{indent}{indent}RegisterProperty<{cls}, {type}>(\n\
                 {indent}{indent}{indent}\"{name}\",\n\
                 {indent}{indent}{indent}offsetof({cls}, {name}),\n\
                 {indent}{indent}{indent}{flags}\n\
                 {indent}{indent});\n",
                cls = class_name,
                type = prop.cpp_type,
                name = prop.name,
                flags = flags,
            ));
        }

        out.push_str(&format!(
            "{indent}}}\n\n\
             }} // namespace Limx::Internal\n",
        ));
    }
}

// =============================================================================
// 辅助函数
// =============================================================================

/// 编译期字符串哈希 (FNV-1a 64位)
fn compile_time_hash(name: &str) -> u64 {
    let mut hash: u64 = 0xcbf29ce484222325;
    for byte in name.bytes() {
        hash ^= byte as u64;
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

/// 蛇形命名转大驼峰
fn to_pascal_case(snake: &str) -> String {
    snake
        .split('_')
        .map(|word| {
            let mut chars = word.chars();
            match chars.next() {
                Some(c) => c.to_uppercase().to_string() + chars.as_str(),
                None => String::new(),
            }
        })
        .collect()
}

/// 构建属性标志字符串
fn build_property_flags(prop: &BindingProperty) -> String {
    let mut flags = Vec::new();

    match prop.access {
        PropertyAccess::ReadWrite => flags.push("EPropertyFlags::ReadWrite"),
        PropertyAccess::ReadOnly => flags.push("EPropertyFlags::ReadOnly"),
        PropertyAccess::WriteOnly => flags.push("EPropertyFlags::WriteOnly"),
    }

    if prop.serializable {
        flags.push("EPropertyFlags::Serializable");
    }
    if prop.editable {
        flags.push("EPropertyFlags::Editable");
    }
    if prop.transient {
        flags.push("EPropertyFlags::Transient");
    }

    match prop.replication {
        ReplicationMode::None => {}
        ReplicationMode::ServerToClient => flags.push("EPropertyFlags::ReplicatedS2C"),
        ReplicationMode::Bidirectional => flags.push("EPropertyFlags::ReplicatedBidi"),
    }

    if flags.is_empty() {
        "EPropertyFlags::None".to_string()
    } else {
        flags.join(" | ")
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_test_class() -> BindingClass {
        BindingClass {
            class_name: "APlayerCharacter".to_string(),
            namespace: Some("Game".to_string()),
            parent_class: Some("ACharacter".to_string()),
            properties: vec![
                BindingProperty {
                    name: "health".to_string(),
                    cpp_type: "float".to_string(),
                    access: PropertyAccess::ReadWrite,
                    serializable: true,
                    editable: true,
                    replication: ReplicationMode::ServerToClient,
                    category: Some("Stats".to_string()),
                    display_name: Some("生命值".to_string()),
                    tooltip: Some("角色当前生命值".to_string()),
                    range: Some((0.0, 100.0)),
                    transient: false,
                },
                BindingProperty {
                    name: "player_name".to_string(),
                    cpp_type: "LString".to_string(),
                    access: PropertyAccess::ReadWrite,
                    serializable: true,
                    editable: true,
                    replication: ReplicationMode::None,
                    category: Some("Info".to_string()),
                    display_name: None,
                    tooltip: None,
                    range: None,
                    transient: false,
                },
                BindingProperty {
                    name: "internal_id".to_string(),
                    cpp_type: "uint64_t".to_string(),
                    access: PropertyAccess::ReadOnly,
                    serializable: false,
                    editable: false,
                    replication: ReplicationMode::None,
                    category: None,
                    display_name: None,
                    tooltip: None,
                    range: None,
                    transient: true,
                },
            ],
            methods: vec![BindingMethod {
                name: "TakeDamage".to_string(),
                return_type: "void".to_string(),
                parameters: vec![("amount".to_string(), "float".to_string())],
                is_const: false,
                is_virtual: true,
                is_rpc: false,
            }],
            is_abstract: false,
            module_name: "GameModule".to_string(),
        }
    }

    #[test]
    fn test_generate_binding_header() {
        let gen = TypeBindingGenerator::with_defaults();
        let binding = gen.generate_binding(&make_test_class());

        assert_eq!(binding.class_name, "APlayerCharacter");
        assert_eq!(binding.accessor_count, 3);
        assert_eq!(binding.method_binding_count, 1);

        // 头文件内容检查
        assert!(
            binding
                .header_content
                .contains("#ifndef APLAYERCHARACTER_BINDING_H"),
            "应有头文件保护宏"
        );
        assert!(
            binding
                .header_content
                .contains("#define APLAYERCHARACTER_BINDING_H"),
            "应有头文件定义宏"
        );
        assert!(
            binding.header_content.contains("constexpr"),
            "应有 constexpr 元数据"
        );
        assert!(
            binding.header_content.contains("GetHealth"),
            "应有 GetHealth 访问器"
        );
        assert!(
            binding.header_content.contains("SetHealth"),
            "应有 SetHealth 访问器"
        );
        assert!(
            binding.header_content.contains("GetPlayerName"),
            "应有 GetPlayerName 访问器"
        );
        assert!(
            binding.header_content.contains("GetInternalId"),
            "应有 GetInternalId 访问器"
        );
    }

    #[test]
    fn test_readonly_no_setter() {
        let gen = TypeBindingGenerator::with_defaults();
        let binding = gen.generate_binding(&make_test_class());

        // internal_id 是只读的，不应有 SetInternalId
        assert!(
            !binding.header_content.contains("SetInternalId"),
            "只读属性不应生成 Setter"
        );
    }

    #[test]
    fn test_range_clamped_setter() {
        let gen = TypeBindingGenerator::with_defaults();
        let binding = gen.generate_binding(&make_test_class());

        // health 有范围 [0, 100]，应生成 SetClampedHealth
        assert!(
            binding.header_content.contains("SetClampedHealth"),
            "有范围约束的属性应生成 Clamped Setter"
        );
    }

    #[test]
    fn test_meta_table_generation() {
        let gen = TypeBindingGenerator::with_defaults();
        let binding = gen.generate_binding(&make_test_class());

        assert!(
            binding
                .header_content
                .contains("APLAYERCHARACTER_PROPERTY_COUNT = 3"),
            "属性数量常量应正确"
        );
        assert!(
            binding
                .header_content
                .contains("APlayerCharacterPropertyMeta"),
            "应有属性元数据结构体"
        );
        assert!(
            binding
                .header_content
                .contains("APLAYERCHARACTER_PROPERTIES"),
            "应有属性元数据数组"
        );
    }

    #[test]
    fn test_crtp_adapter() {
        let gen = TypeBindingGenerator::with_defaults();
        let binding = gen.generate_binding(&make_test_class());

        assert!(
            binding
                .header_content
                .contains("APlayerCharacterReflectionAdapter"),
            "应有 CRTP 适配器类"
        );
        assert!(
            binding.header_content.contains("GetPropertyValue"),
            "应有 GetPropertyValue 方法"
        );
        assert!(
            binding.header_content.contains("SetPropertyValue"),
            "应有 SetPropertyValue 方法"
        );
        assert!(
            binding.header_content.contains("ForEachProperty"),
            "应有 ForEachProperty 方法"
        );
        assert!(
            binding.header_content.contains("GetPropertyCount"),
            "应有 GetPropertyCount 方法"
        );
    }

    #[test]
    fn test_type_traits() {
        let gen = TypeBindingGenerator::with_defaults();
        let binding = gen.generate_binding(&make_test_class());

        assert!(
            binding
                .header_content
                .contains("TypeInfo<APlayerCharacter>"),
            "应有 TypeInfo 特化"
        );
        assert!(
            binding.header_content.contains("PropertyCount = 3"),
            "应有正确的属性数量"
        );
        assert!(
            binding.header_content.contains("IsAbstract = false"),
            "应正确标记非抽象"
        );
        assert!(
            binding.header_content.contains("ParentType = ACharacter"),
            "应有父类类型"
        );
    }

    #[test]
    fn test_impl_file() {
        let gen = TypeBindingGenerator::with_defaults();
        let binding = gen.generate_binding(&make_test_class());

        assert!(
            binding
                .impl_content
                .contains("RegisterAPlayerCharacterProperties"),
            "应有注册函数"
        );
        assert!(
            binding.impl_content.contains("RegisterProperty"),
            "应有属性注册调用"
        );
        assert!(
            binding.impl_content.contains("EPropertyFlags"),
            "应有属性标志"
        );
    }

    #[test]
    fn test_compile_time_hash_deterministic() {
        assert_eq!(compile_time_hash("health"), compile_time_hash("health"));
        assert_ne!(compile_time_hash("health"), compile_time_hash("mana"));
    }

    #[test]
    fn test_to_pascal_case() {
        assert_eq!(to_pascal_case("health"), "Health");
        assert_eq!(to_pascal_case("player_name"), "PlayerName");
        assert_eq!(to_pascal_case("internal_id"), "InternalId");
        assert_eq!(to_pascal_case("a_b_c"), "ABC");
    }

    #[test]
    fn test_property_flags() {
        let prop = BindingProperty {
            name: "test".to_string(),
            cpp_type: "float".to_string(),
            access: PropertyAccess::ReadWrite,
            serializable: true,
            editable: true,
            replication: ReplicationMode::ServerToClient,
            category: None,
            display_name: None,
            tooltip: None,
            range: None,
            transient: false,
        };

        let flags = build_property_flags(&prop);
        assert!(flags.contains("ReadWrite"));
        assert!(flags.contains("Serializable"));
        assert!(flags.contains("Editable"));
        assert!(flags.contains("ReplicatedS2C"));
    }

    #[test]
    fn test_abstract_class_binding() {
        let class = BindingClass {
            class_name: "ABase".to_string(),
            namespace: None,
            parent_class: None,
            properties: vec![],
            methods: vec![],
            is_abstract: true,
            module_name: "Core".to_string(),
        };

        let gen = TypeBindingGenerator::with_defaults();
        let binding = gen.generate_binding(&class);

        assert!(binding.header_content.contains("IsAbstract = true"));
        assert!(binding.header_content.contains("HasParent = false"));
        assert_eq!(binding.accessor_count, 0);
    }

    #[test]
    fn test_multiple_bindings() {
        let classes = vec![
            BindingClass {
                class_name: "ClassA".to_string(),
                namespace: None,
                parent_class: None,
                properties: vec![BindingProperty {
                    name: "value".to_string(),
                    cpp_type: "int32_t".to_string(),
                    access: PropertyAccess::ReadWrite,
                    serializable: true,
                    editable: false,
                    replication: ReplicationMode::None,
                    category: None,
                    display_name: None,
                    tooltip: None,
                    range: None,
                    transient: false,
                }],
                methods: vec![],
                is_abstract: false,
                module_name: "Test".to_string(),
            },
            BindingClass {
                class_name: "ClassB".to_string(),
                namespace: None,
                parent_class: Some("ClassA".to_string()),
                properties: vec![],
                methods: vec![],
                is_abstract: false,
                module_name: "Test".to_string(),
            },
        ];

        let gen = TypeBindingGenerator::with_defaults();
        let bindings = gen.generate_all_bindings(&classes);

        assert_eq!(bindings.len(), 2);
        assert_eq!(bindings[0].class_name, "ClassA");
        assert_eq!(bindings[1].class_name, "ClassB");
    }

    #[test]
    fn test_qualified_name() {
        let with_ns = BindingClass {
            class_name: "MyClass".to_string(),
            namespace: Some("Game::Characters".to_string()),
            parent_class: None,
            properties: vec![],
            methods: vec![],
            is_abstract: false,
            module_name: "M".to_string(),
        };
        assert_eq!(with_ns.qualified_name(), "Game::Characters::MyClass");

        let without_ns = BindingClass {
            class_name: "MyClass".to_string(),
            namespace: None,
            parent_class: None,
            properties: vec![],
            methods: vec![],
            is_abstract: false,
            module_name: "M".to_string(),
        };
        assert_eq!(without_ns.qualified_name(), "MyClass");
    }
}
