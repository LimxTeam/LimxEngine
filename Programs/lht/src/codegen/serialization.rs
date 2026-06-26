/*******************************************************************************
 * 文件: codegen/serialization.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   序列化代码生成器 - 生成对象序列化/反序列化代码
 *   - 二进制序列化
 *   - JSON 序列化
 *   - 版本兼容性
 *   - 差异序列化
 *   - 网络序列化
 *
 * 设计哲学:
 *   1. 高性能 - 最小化运行时开销
 *   2. 版本化 - 支持数据格式演进
 *   3. 可扩展 - 自定义序列化器
 *
 * 技术特性:
 *   - 编译时生成序列化代码
 *   - 支持循环引用
 *   - 支持多态对象
 *   - 属性级别控制
 *
 ******************************************************************************/

use super::adapter::{ClassDeclExt, EnumDeclExt, FieldDeclExt};
use crate::parser::ast::{ClassDecl, EnumDecl, FieldDecl};
use std::collections::HashMap;

//=============================================================================
// 序列化配置
//=============================================================================

/// 序列化格式
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SerializationFormat {
    /// 二进制格式 (高性能)
    Binary,
    /// JSON 格式 (可读)
    Json,
    /// 网络格式 (紧凑)
    Network,
    /// 存档格式 (版本化)
    Archive,
}

/// 序列化选项
#[derive(Debug, Clone)]
pub struct SerializationOptions {
    /// 是否生成版本信息
    pub versioned: bool,
    /// 是否支持差异序列化
    pub delta_serialization: bool,
    /// 是否生成校验和
    pub generate_checksum: bool,
    /// 是否压缩数据
    pub compress: bool,
    /// 字节序
    pub big_endian: bool,
    /// 对齐要求
    pub alignment: usize,
}

impl Default for SerializationOptions {
    fn default() -> Self {
        Self {
            versioned: true,
            delta_serialization: false,
            generate_checksum: false,
            compress: false,
            big_endian: false,
            alignment: 4,
        }
    }
}

//=============================================================================
// 序列化代码生成器
//=============================================================================

/// 序列化代码生成器
pub struct SerializationGenerator {
    /// 选项
    options: SerializationOptions,
    /// 类型版本映射
    type_versions: HashMap<String, u32>,
}

impl SerializationGenerator {
    pub fn new(options: SerializationOptions) -> Self {
        Self {
            options,
            type_versions: HashMap::new(),
        }
    }

    /// 设置类型版本
    pub fn set_type_version(&mut self, type_name: &str, version: u32) {
        self.type_versions.insert(type_name.to_string(), version);
    }

    /// 生成类的序列化代码
    pub fn generate_class_serialization(&self, class: &ClassDecl) -> String {
        let mut code = String::with_capacity(8192);

        // 生成 Serialize 方法
        code.push_str(&self.generate_serialize_method(class));
        code.push('\n');

        // 生成 Deserialize 方法
        code.push_str(&self.generate_deserialize_method(class));
        code.push('\n');

        // 生成版本化序列化
        if self.options.versioned {
            code.push_str(&self.generate_versioned_serialization(class));
            code.push('\n');
        }

        // 生成差异序列化
        if self.options.delta_serialization {
            code.push_str(&self.generate_delta_serialization(class));
            code.push('\n');
        }

        // 生成网络序列化
        code.push_str(&self.generate_network_serialization(class));

        code
    }

    /// 生成 Serialize 方法
    fn generate_serialize_method(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!(
            "void {}::Serialize(Archive& Ar)\n{{\n",
            class.name
        ));

        // 调用父类序列化
        if let Some(base) = class.get_first_base() {
            code.push_str(&format!("    {}::Serialize(Ar);\n\n", base));
        }

        // 版本号
        if self.options.versioned {
            let version = self.type_versions.get(&class.name).unwrap_or(&1);
            code.push_str(&format!("    UInt32 Version = {};\n", version));
            code.push_str("    Ar << Version;\n\n");
        }

        // 序列化每个属性
        for prop in class.get_fields() {
            if self.should_serialize_property(prop) {
                code.push_str(&self.generate_property_serialization(prop));
            }
        }

        code.push_str("}\n");
        code
    }

    /// 生成 Deserialize 方法
    fn generate_deserialize_method(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!(
            "void {}::Deserialize(Archive& Ar)\n{{\n",
            class.name
        ));

        // 调用父类反序列化
        if let Some(base) = class.get_first_base() {
            code.push_str(&format!("    {}::Deserialize(Ar);\n\n", base));
        }

        // 版本号
        if self.options.versioned {
            code.push_str("    UInt32 Version;\n");
            code.push_str("    Ar >> Version;\n\n");
        }

        // 反序列化每个属性
        for prop in class.get_fields() {
            if self.should_serialize_property(prop) {
                code.push_str(&self.generate_property_deserialization(prop));
            }
        }

        code.push_str("}\n");
        code
    }

    /// 生成版本化序列化支持
    fn generate_versioned_serialization(&self, class: &ClassDecl) -> String {
        let version = self.type_versions.get(&class.name).unwrap_or(&1);

        format!(
            r#"// 版本信息
static constexpr UInt32 {}_SerializationVersion = {};

UInt32 {}::GetSerializationVersion() const
{{
    return {}_SerializationVersion;
}}

bool {}::IsSerializationVersionCompatible(UInt32 FileVersion) const
{{
    return FileVersion <= {}_SerializationVersion;
}}
"#,
            class.name, version, class.name, class.name, class.name, class.name
        )
    }

    /// 生成差异序列化
    fn generate_delta_serialization(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!(
            "void {}::SerializeDelta(Archive& Ar, const {}* Default)\n{{\n",
            class.name, class.name
        ));

        code.push_str("    if (!Default)\n");
        code.push_str("    {\n");
        code.push_str("        Serialize(Ar);\n");
        code.push_str("        return;\n");
        code.push_str("    }\n\n");

        // 使用位掩码标记哪些属性被修改
        let fields = class.get_fields();
        let prop_count = fields.len();
        let mask_size = (prop_count + 7) / 8;

        code.push_str(&format!(
            "    UInt8 ChangeMask[{}] = {{0}};\n\n",
            mask_size.max(1)
        ));

        // 检测变化并设置掩码
        code.push_str("    // 检测属性变化\n");
        for (i, prop) in fields.iter().enumerate() {
            if self.should_serialize_property(prop) {
                let byte_idx = i / 8;
                let bit_idx = i % 8;
                code.push_str(&format!(
                    "    if ({} != Default->{})\n",
                    prop.name, prop.name
                ));
                code.push_str(&format!(
                    "        ChangeMask[{}] |= (1 << {});\n",
                    byte_idx, bit_idx
                ));
            }
        }

        code.push_str("\n    // 写入变化掩码\n");
        code.push_str(&format!(
            "    Ar.SerializeRaw(ChangeMask, {});\n\n",
            mask_size.max(1)
        ));

        // 只序列化变化的属性
        code.push_str("    // 序列化变化的属性\n");
        for (i, prop) in fields.iter().enumerate() {
            if self.should_serialize_property(prop) {
                let byte_idx = i / 8;
                let bit_idx = i % 8;
                code.push_str(&format!(
                    "    if (ChangeMask[{}] & (1 << {}))\n",
                    byte_idx, bit_idx
                ));
                code.push_str("    {\n");
                code.push_str(&format!("        Ar << {};\n", prop.name));
                code.push_str("    }\n");
            }
        }

        code.push_str("}\n");
        code
    }

    /// 生成网络序列化
    fn generate_network_serialization(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        // NetSerialize - 紧凑的网络序列化
        code.push_str(&format!(
            "bool {}::NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)\n{{\n",
            class.name
        ));

        code.push_str("    bOutSuccess = true;\n\n");

        // 只序列化标记为 Replicated 的属性
        let replicated_props: Vec<_> = class
            .get_fields()
            .into_iter()
            .filter(|p| p.is_replicated_field())
            .collect();

        if replicated_props.is_empty() {
            code.push_str("    // 无复制属性\n");
        } else {
            for prop in &replicated_props {
                code.push_str(&self.generate_net_property_serialization(prop));
            }
        }

        code.push_str("    return true;\n");
        code.push_str("}\n\n");

        // GetLifetimeReplicatedProps - 属性复制设置
        code.push_str(&format!(
            "void {}::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const\n{{\n",
            class.name
        ));

        if let Some(base) = class.get_first_base() {
            code.push_str(&format!(
                "    {}::GetLifetimeReplicatedProps(OutLifetimeProps);\n\n",
                base
            ));
        }

        for prop in &replicated_props {
            let condition = self.get_replication_condition(prop);
            code.push_str(&format!(
                "    DOREPLIFETIME_CONDITION(this, {}, {});\n",
                prop.name, condition
            ));
        }

        code.push_str("}\n");

        code
    }

    /// 生成属性序列化代码
    fn generate_property_serialization(&self, prop: &FieldDecl) -> String {
        let prop_type = prop.type_str();

        // 检查是否有自定义序列化
        if prop.has_spec("CustomSerialize") {
            return format!("    Serialize_{}(Ar);\n", prop.name);
        }

        // 根据类型生成序列化代码
        if self.is_pod_type(&prop_type) {
            format!("    Ar << {};\n", prop.name)
        } else if self.is_container_type(&prop_type) {
            self.generate_container_serialization(prop)
        } else if self.is_pointer_type(&prop_type) {
            self.generate_pointer_serialization(prop)
        } else if self.is_object_type(&prop_type) {
            format!("    {}.Serialize(Ar);\n", prop.name)
        } else {
            format!("    Ar << {};\n", prop.name)
        }
    }

    /// 生成属性反序列化代码
    fn generate_property_deserialization(&self, prop: &FieldDecl) -> String {
        let prop_type = prop.type_str();

        if prop.has_spec("CustomSerialize") {
            return format!("    Deserialize_{}(Ar);\n", prop.name);
        }

        if self.is_pod_type(&prop_type) {
            format!("    Ar >> {};\n", prop.name)
        } else if self.is_container_type(&prop_type) {
            self.generate_container_deserialization(prop)
        } else if self.is_pointer_type(&prop_type) {
            self.generate_pointer_deserialization(prop)
        } else if self.is_object_type(&prop_type) {
            format!("    {}.Deserialize(Ar);\n", prop.name)
        } else {
            format!("    Ar >> {};\n", prop.name)
        }
    }

    /// 生成容器序列化
    fn generate_container_serialization(&self, prop: &FieldDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!("    // 序列化容器: {}\n", prop.name));
        code.push_str("    {\n");
        code.push_str(&format!("        SizeType Count = {}.Size();\n", prop.name));
        code.push_str("        Ar << Count;\n");
        code.push_str(&format!("        for (auto& Element : {})\n", prop.name));
        code.push_str("        {\n");
        code.push_str("            Ar << Element;\n");
        code.push_str("        }\n");
        code.push_str("    }\n");

        code
    }

    /// 生成容器反序列化
    fn generate_container_deserialization(&self, prop: &FieldDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!("    // 反序列化容器: {}\n", prop.name));
        code.push_str("    {\n");
        code.push_str("        SizeType Count;\n");
        code.push_str("        Ar >> Count;\n");
        code.push_str(&format!("        {}.Clear();\n", prop.name));
        code.push_str(&format!("        {}.Reserve(Count);\n", prop.name));
        code.push_str("        for (SizeType i = 0; i < Count; ++i)\n");
        code.push_str("        {\n");

        // 根据容器元素类型生成代码
        if prop.type_str().contains("Map") {
            code.push_str("            auto Key = decltype({}.Keys()[0]){};\n");
            code.push_str("            auto Value = decltype({}.Values()[0]){};\n");
            code.push_str("            Ar >> Key >> Value;\n");
            code.push_str(&format!("            {}.Insert(Key, Value);\n", prop.name));
        } else {
            code.push_str(&format!(
                "            auto Element = decltype({}[0]){{}};\n",
                prop.name
            ));
            code.push_str("            Ar >> Element;\n");
            code.push_str(&format!("            {}.Add(Element);\n", prop.name));
        }

        code.push_str("        }\n");
        code.push_str("    }\n");

        code
    }

    /// 生成指针序列化
    fn generate_pointer_serialization(&self, prop: &FieldDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!("    // 序列化指针: {}\n", prop.name));
        code.push_str("    {\n");
        code.push_str(&format!(
            "        bool bIsValid = ({} != nullptr);\n",
            prop.name
        ));
        code.push_str("        Ar << bIsValid;\n");
        code.push_str("        if (bIsValid)\n");
        code.push_str("        {\n");

        let prop_type = prop.type_str();
        if prop_type.contains("TObjectPtr") || prop_type.contains("*") {
            code.push_str(&format!("            Ar.SerializeObject({});\n", prop.name));
        } else {
            code.push_str(&format!("            {}->Serialize(Ar);\n", prop.name));
        }

        code.push_str("        }\n");
        code.push_str("    }\n");

        code
    }

    /// 生成指针反序列化
    fn generate_pointer_deserialization(&self, prop: &FieldDecl) -> String {
        let mut code = String::new();

        code.push_str(&format!("    // 反序列化指针: {}\n", prop.name));
        code.push_str("    {\n");
        code.push_str("        bool bIsValid;\n");
        code.push_str("        Ar >> bIsValid;\n");
        code.push_str("        if (bIsValid)\n");
        code.push_str("        {\n");

        let prop_type = prop.type_str();
        if prop_type.contains("TObjectPtr") || prop_type.contains("*") {
            code.push_str(&format!(
                "            {} = Ar.DeserializeObject<{}>({});\n",
                prop.name,
                self.extract_base_type(&prop_type),
                prop.name
            ));
        } else {
            code.push_str(&format!(
                "            if (!{}) {{ {} = new {}(); }}\n",
                prop.name,
                prop.name,
                self.extract_base_type(&prop_type)
            ));
            code.push_str(&format!("            {}->Deserialize(Ar);\n", prop.name));
        }

        code.push_str("        }\n");
        code.push_str("        else\n");
        code.push_str("        {\n");
        code.push_str(&format!("            {} = nullptr;\n", prop.name));
        code.push_str("        }\n");
        code.push_str("    }\n");

        code
    }

    /// 生成网络属性序列化
    fn generate_net_property_serialization(&self, prop: &FieldDecl) -> String {
        // 使用紧凑的网络序列化格式
        format!(
            "    Ar.SerializeBits(&{}, sizeof({}) * 8);\n",
            prop.name, prop.name
        )
    }

    /// 获取复制条件
    fn get_replication_condition(&self, prop: &FieldDecl) -> &'static str {
        // 从属性元数据中获取复制条件
        if prop.has_meta_key("ReplicatedUsing") {
            "COND_Custom"
        } else if prop.has_spec("OwnerOnly") {
            "COND_OwnerOnly"
        } else if prop.has_spec("SkipOwner") {
            "COND_SkipOwner"
        } else if prop.has_spec("SimulatedOnly") {
            "COND_SimulatedOnly"
        } else if prop.has_spec("InitialOnly") {
            "COND_InitialOnly"
        } else {
            "COND_None"
        }
    }

    /// 检查属性是否应该序列化
    fn should_serialize_property(&self, prop: &FieldDecl) -> bool {
        prop.is_serializable_field()
    }

    /// 检查是否为 POD 类型
    fn is_pod_type(&self, type_name: &str) -> bool {
        matches!(
            type_name,
            "Int8"
                | "Int16"
                | "Int32"
                | "Int64"
                | "UInt8"
                | "UInt16"
                | "UInt32"
                | "UInt64"
                | "Float32"
                | "Float64"
                | "bool"
                | "int"
                | "float"
                | "double"
                | "char"
                | "int8_t"
                | "int16_t"
                | "int32_t"
                | "int64_t"
                | "uint8_t"
                | "uint16_t"
                | "uint32_t"
                | "uint64_t"
        )
    }

    /// 检查是否为容器类型
    fn is_container_type(&self, type_name: &str) -> bool {
        type_name.contains("TArray")
            || type_name.contains("TMap")
            || type_name.contains("TSet")
            || type_name.contains("Array")
            || type_name.contains("Map")
            || type_name.contains("Set")
    }

    /// 检查是否为指针类型
    fn is_pointer_type(&self, type_name: &str) -> bool {
        type_name.contains('*')
            || type_name.contains("TObjectPtr")
            || type_name.contains("TWeakPtr")
            || type_name.contains("TSharedPtr")
    }

    /// 检查是否为对象类型
    fn is_object_type(&self, type_name: &str) -> bool {
        // 假设以大写字母开头且不是基本类型的都是对象类型
        type_name
            .chars()
            .next()
            .map(|c| c.is_uppercase())
            .unwrap_or(false)
            && !self.is_pod_type(type_name)
            && !self.is_container_type(type_name)
            && !self.is_pointer_type(type_name)
    }

    /// 提取基类型 (去除指针、引用等)
    fn extract_base_type(&self, type_name: &str) -> String {
        let mut result = type_name.to_string();

        // 移除指针
        result = result.replace('*', "").trim().to_string();

        // 移除模板包装
        if let Some(start) = result.find('<') {
            if let Some(end) = result.rfind('>') {
                result = result[start + 1..end].trim().to_string();
            }
        }

        result
    }
}

//=============================================================================
// 枚举序列化
//=============================================================================

impl SerializationGenerator {
    /// 生成枚举序列化代码
    pub fn generate_enum_serialization(&self, enum_def: &EnumDecl) -> String {
        let mut code = String::new();

        // 枚举值到字符串
        code.push_str(&format!(
            "const char* {}ToString({} Value)\n{{\n",
            enum_def.name, enum_def.name
        ));
        code.push_str("    switch (Value)\n");
        code.push_str("    {\n");

        for value in &enum_def.values {
            code.push_str(&format!(
                "        case {}::{}: return \"{}\";\n",
                enum_def.name, value.name, value.name
            ));
        }

        code.push_str("        default: return \"Unknown\";\n");
        code.push_str("    }\n");
        code.push_str("}\n\n");

        // 字符串到枚举值
        code.push_str(&format!(
            "bool {}FromString(const char* String, {}& OutValue)\n{{\n",
            enum_def.name, enum_def.name
        ));

        for value in &enum_def.values {
            code.push_str(&format!(
                "    if (strcmp(String, \"{}\") == 0) {{ OutValue = {}::{}; return true; }}\n",
                value.name, enum_def.name, value.name
            ));
        }

        code.push_str("    return false;\n");
        code.push_str("}\n\n");

        // Archive 操作符
        code.push_str(&format!(
            "Archive& operator<<(Archive& Ar, {}& Value)\n{{\n",
            enum_def.name
        ));
        let underlying = enum_def.underlying_type_str();
        code.push_str("    if (Ar.IsSaving())\n");
        code.push_str("    {\n");
        code.push_str(&format!(
            "        {} UnderlyingValue = static_cast<{}>(Value);\n",
            underlying, underlying
        ));
        code.push_str("        Ar << UnderlyingValue;\n");
        code.push_str("    }\n");
        code.push_str("    else\n");
        code.push_str("    {\n");
        code.push_str(&format!("        {} UnderlyingValue;\n", underlying));
        code.push_str("        Ar >> UnderlyingValue;\n");
        code.push_str(&format!(
            "        Value = static_cast<{}>(UnderlyingValue);\n",
            enum_def.name
        ));
        code.push_str("    }\n");
        code.push_str("    return Ar;\n");
        code.push_str("}\n");

        code
    }
}

//=============================================================================
// JSON 序列化
//=============================================================================

/// JSON 序列化生成器
pub struct JsonSerializationGenerator;

impl JsonSerializationGenerator {
    pub fn new() -> Self {
        Self
    }

    /// 生成 JSON 序列化代码
    pub fn generate_class_json(&self, class: &ClassDecl) -> String {
        let mut code = String::new();

        // ToJson
        code.push_str(&format!("JsonValue {}::ToJson() const\n{{\n", class.name));
        code.push_str("    JsonObject Obj;\n\n");

        // 添加类型标识
        code.push_str(&format!("    Obj[\"_type\"] = \"{}\";\n", class.name));

        // 序列化属性
        for prop in class.get_fields() {
            if !prop.is_transient_field() {
                code.push_str(&self.generate_property_to_json(prop));
            }
        }

        code.push_str("\n    return JsonValue(Obj);\n");
        code.push_str("}\n\n");

        // FromJson
        code.push_str(&format!(
            "bool {}::FromJson(const JsonValue& Json)\n{{\n",
            class.name
        ));
        code.push_str("    if (!Json.IsObject()) return false;\n");
        code.push_str("    const auto& Obj = Json.AsObject();\n\n");

        for prop in class.get_fields() {
            if !prop.is_transient_field() {
                code.push_str(&self.generate_property_from_json(prop));
            }
        }

        code.push_str("\n    return true;\n");
        code.push_str("}\n");

        code
    }

    fn generate_property_to_json(&self, prop: &FieldDecl) -> String {
        format!("    Obj[\"{}\"] = {};\n", prop.name, prop.name)
    }

    fn generate_property_from_json(&self, prop: &FieldDecl) -> String {
        format!(
            "    if (Obj.Contains(\"{}\")) {{ {} = Obj[\"{}\"].As<{}>(); }}\n",
            prop.name,
            prop.name,
            prop.name,
            prop.type_str()
        )
    }
}

impl Default for JsonSerializationGenerator {
    fn default() -> Self {
        Self::new()
    }
}
