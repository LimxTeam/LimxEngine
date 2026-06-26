/*******************************************************************************
 * 文件: codegen/rpc.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   RPC 代码生成器 - 生成远程过程调用支持代码
 *   - 客户端 RPC (Server -> Client)
 *   - 服务器 RPC (Client -> Server)
 *   - 多播 RPC (Server -> All Clients)
 *   - 可靠/不可靠 RPC
 *
 * 设计哲学:
 *   1. 类型安全 - 编译时参数检查
 *   2. 高性能 - 最小化序列化开销
 *   3. 透明性 - 调用语法与本地方法一致
 *
 * 技术特性:
 *   - 自动参数序列化
 *   - 自动函数 ID 分配
 *   - 支持异步返回值
 *   - 带宽优化
 *   - 验证和安全检查
 *
 ******************************************************************************/

use super::adapter::{ClassDeclExt, MethodDeclExt, ParameterDeclExt};
use crate::parser::ast::{ClassDecl, MethodDecl, ParameterDecl};
use std::collections::HashMap;

//=============================================================================
// RPC 类型
//=============================================================================

/// RPC 类型
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RpcType {
    /// 客户端 RPC - 服务器调用，客户端执行
    Client,
    /// 服务器 RPC - 客户端调用，服务器执行
    Server,
    /// 多播 RPC - 服务器调用，所有客户端执行
    NetMulticast,
    /// 不是 RPC
    None,
}

impl RpcType {
    /// 从方法声明解析 RPC 类型
    pub fn from_method(method: &MethodDecl) -> Self {
        if method.is_client_rpc() {
            Self::Client
        } else if method.is_server_rpc() {
            Self::Server
        } else if method.is_multicast_rpc() {
            Self::NetMulticast
        } else {
            Self::None
        }
    }

    /// 获取类型名称
    pub fn name(&self) -> &'static str {
        match self {
            Self::Client => "Client",
            Self::Server => "Server",
            Self::NetMulticast => "NetMulticast",
            Self::None => "None",
        }
    }
}

/// RPC 可靠性
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RpcReliability {
    /// 可靠 - 保证送达
    Reliable,
    /// 不可靠 - 可能丢失
    Unreliable,
    /// 不可靠但有序
    UnreliableOrdered,
}

//=============================================================================
// RPC 配置
//=============================================================================

/// RPC 配置
#[derive(Debug, Clone)]
pub struct RpcConfig {
    /// 是否生成验证代码
    pub generate_validation: bool,
    /// 是否生成带宽统计
    pub bandwidth_stats: bool,
    /// 是否生成调试日志
    pub debug_logging: bool,
    /// 最大参数大小 (字节)
    pub max_params_size: usize,
    /// 是否启用压缩
    pub enable_compression: bool,
}

impl Default for RpcConfig {
    fn default() -> Self {
        Self {
            generate_validation: true,
            bandwidth_stats: false,
            debug_logging: false,
            max_params_size: 65536,
            enable_compression: false,
        }
    }
}

//=============================================================================
// RPC 代码生成器
//=============================================================================

/// RPC 代码生成器
pub struct RpcCodeGenerator {
    /// 配置
    config: RpcConfig,
    /// 函数 ID 映射
    function_ids: HashMap<String, u32>,
    /// 下一个函数 ID
    next_function_id: u32,
}

impl RpcCodeGenerator {
    pub fn new(config: RpcConfig) -> Self {
        Self {
            config,
            function_ids: HashMap::new(),
            next_function_id: 1,
        }
    }

    /// 生成类的所有 RPC 代码
    pub fn generate_class_rpc(&mut self, class: &ClassDecl) -> String {
        let mut code = String::with_capacity(16384);

        // 收集所有 RPC 函数
        let rpc_functions: Vec<_> = class
            .get_methods()
            .into_iter()
            .filter(|f| f.is_any_rpc())
            .collect();

        if rpc_functions.is_empty() {
            return String::new();
        }

        // 生成 RPC 注册表
        code.push_str(&self.generate_rpc_registry(class, &rpc_functions));
        code.push('\n');

        // 为每个 RPC 函数生成代码
        for func in &rpc_functions {
            code.push_str(&self.generate_rpc_function(class, func));
            code.push('\n');
        }

        // 生成 RPC 调度器
        code.push_str(&self.generate_rpc_dispatcher(class, &rpc_functions));
        code.push('\n');

        // 生成验证函数
        if self.config.generate_validation {
            code.push_str(&self.generate_rpc_validation(class, &rpc_functions));
        }

        code
    }

    /// 生成 RPC 注册表
    fn generate_rpc_registry(&mut self, class: &ClassDecl, functions: &[&MethodDecl]) -> String {
        let mut code = String::new();

        code.push_str(
            "//=============================================================================\n",
        );
        code.push_str(&format!("// {} RPC 注册表\n", class.name));
        code.push_str(
            "//=============================================================================\n\n",
        );

        // 枚举所有 RPC 函数 ID
        code.push_str(&format!("namespace {}RpcIds\n{{\n", class.name));

        for func in functions {
            let func_key = format!("{}::{}", class.name, func.name);
            let id = self.allocate_function_id(&func_key);
            code.push_str(&format!(
                "    static constexpr UInt32 {} = {};\n",
                func.name, id
            ));
        }

        code.push_str("}\n\n");

        // RPC 信息结构
        code.push_str(&format!(
            "static const RpcFunctionInfo {}RpcInfos[] = {{\n",
            class.name
        ));

        for func in functions {
            let rpc_type = RpcType::from_method(func);
            let reliable = if func.is_reliable_rpc() {
                "true"
            } else {
                "false"
            };

            code.push_str(&format!(
                "    {{ {}RpcIds::{}, \"{}\", RpcType::{}, {} }},\n",
                class.name,
                func.name,
                func.name,
                rpc_type.name(),
                reliable
            ));
        }

        code.push_str("    { 0, nullptr, RpcType::None, false } // 终止符\n");
        code.push_str("};\n");

        code
    }

    /// 生成单个 RPC 函数
    fn generate_rpc_function(&self, class: &ClassDecl, func: &MethodDecl) -> String {
        let mut code = String::new();
        let rpc_type = RpcType::from_method(func);

        code.push_str(&format!(
            "//-----------------------------------------------------------------------------\n"
        ));
        code.push_str(&format!("// RPC: {} ({})\n", func.name, rpc_type.name()));
        code.push_str(&format!(
            "//-----------------------------------------------------------------------------\n\n"
        ));

        // 生成实现函数声明
        code.push_str(&self.generate_implementation_function(class, func));
        code.push('\n');

        // 生成包装函数 (序列化参数并发送)
        code.push_str(&self.generate_wrapper_function(class, func, rpc_type));
        code.push('\n');

        // 生成接收处理函数
        code.push_str(&self.generate_receive_function(class, func, rpc_type));

        code
    }

    /// 生成实现函数 (带 _Implementation 后缀)
    fn generate_implementation_function(&self, class: &ClassDecl, func: &MethodDecl) -> String {
        let mut code = String::new();

        // 返回类型
        let return_type = func.return_type_str();
        let return_type = if return_type.is_empty() {
            "void"
        } else {
            &return_type
        };

        // 参数列表
        let params = self.format_parameters(&func.parameters);

        code.push_str(&format!(
            "{} {}::{}_Implementation({})\n",
            return_type, class.name, func.name, params
        ));
        code.push_str("{\n");
        code.push_str("    // 由用户实现\n");
        code.push_str("}\n");

        code
    }

    /// 生成包装函数 (调用端)
    fn generate_wrapper_function(
        &self,
        class: &ClassDecl,
        func: &MethodDecl,
        rpc_type: RpcType,
    ) -> String {
        let mut code = String::new();

        let return_type = func.return_type_str();
        let return_type = if return_type.is_empty() || return_type == "void" {
            "void"
        } else {
            &return_type
        };
        let params = self.format_parameters(&func.parameters);

        code.push_str(&format!(
            "{} {}::{}({})\n",
            return_type, class.name, func.name, params
        ));
        code.push_str("{\n");

        // 调试日志
        if self.config.debug_logging {
            code.push_str(&format!(
                "    LX_LOG(LogRpc, Verbose, \"Calling RPC: {}\");\n",
                func.name
            ));
        }

        // 验证
        if self.config.generate_validation {
            code.push_str(&format!("    if (!Validate_{}())\n", func.name));
            code.push_str("    {\n");
            code.push_str(&format!(
                "        LX_LOG(LogRpc, Warning, \"RPC validation failed: {}\");\n",
                func.name
            ));
            if return_type != "void" {
                code.push_str(&format!("        return {}{{}};\n", return_type));
            } else {
                code.push_str("        return;\n");
            }
            code.push_str("    }\n\n");
        }

        // 根据 RPC 类型处理
        match rpc_type {
            RpcType::Client => {
                code.push_str("    // 客户端 RPC: 仅在服务器上发送\n");
                code.push_str("    if (!HasAuthority())\n");
                code.push_str("    {\n");
                code.push_str(&format!(
                    "        {}_Implementation({});\n",
                    func.name,
                    self.format_parameter_names(&func.parameters)
                ));
                if return_type != "void" {
                    code.push_str(&format!("        return {}{{}};\n", return_type));
                } else {
                    code.push_str("        return;\n");
                }
                code.push_str("    }\n\n");
            }
            RpcType::Server => {
                code.push_str("    // 服务器 RPC: 仅在客户端发送\n");
                code.push_str("    if (HasAuthority())\n");
                code.push_str("    {\n");
                code.push_str(&format!(
                    "        {}_Implementation({});\n",
                    func.name,
                    self.format_parameter_names(&func.parameters)
                ));
                if return_type != "void" {
                    code.push_str(&format!("        return {}{{}};\n", return_type));
                } else {
                    code.push_str("        return;\n");
                }
                code.push_str("    }\n\n");
            }
            RpcType::NetMulticast => {
                code.push_str("    // 多播 RPC: 服务器广播给所有客户端\n");
                code.push_str("    if (HasAuthority())\n");
                code.push_str("    {\n");
                code.push_str("        // 本地也执行\n");
                code.push_str(&format!(
                    "        {}_Implementation({});\n",
                    func.name,
                    self.format_parameter_names(&func.parameters)
                ));
                code.push_str("    }\n\n");
            }
            RpcType::None => {}
        }

        // 序列化参数
        code.push_str("    // 序列化参数\n");
        code.push_str("    FMemoryWriter Writer;\n");
        code.push_str(&format!(
            "    Writer << {}RpcIds::{};\n",
            class.name, func.name
        ));

        for param in &func.parameters {
            code.push_str(&format!("    Writer << {};\n", param.name));
        }

        // 发送
        match rpc_type {
            RpcType::Client => {
                code.push_str("\n    // 发送给拥有的客户端\n");
                code.push_str(
                    "    GetOwningConnection()->SendRpc(Writer.GetData(), Writer.GetSize());\n",
                );
            }
            RpcType::Server => {
                code.push_str("\n    // 发送给服务器\n");
                code.push_str(
                    "    GetNetConnection()->SendRpc(Writer.GetData(), Writer.GetSize());\n",
                );
            }
            RpcType::NetMulticast => {
                code.push_str("\n    // 广播给所有连接的客户端\n");
                code.push_str("    for (auto* Connection : GetNetDriver()->GetConnections())\n");
                code.push_str("    {\n");
                code.push_str("        Connection->SendRpc(Writer.GetData(), Writer.GetSize());\n");
                code.push_str("    }\n");
            }
            RpcType::None => {}
        }

        // 带宽统计
        if self.config.bandwidth_stats {
            code.push_str(&format!(
                "\n    GRpcStats.RecordSent(\"{}\", Writer.GetSize());\n",
                func.name
            ));
        }

        if return_type != "void" {
            code.push_str(&format!("\n    return {}{{}};\n", return_type));
        }

        code.push_str("}\n");
        code
    }

    /// 生成接收处理函数
    fn generate_receive_function(
        &self,
        class: &ClassDecl,
        func: &MethodDecl,
        _rpc_type: RpcType,
    ) -> String {
        let mut code = String::new();

        code.push_str(&format!(
            "void {}::Receive_{}(FMemoryReader& Reader)\n",
            class.name, func.name
        ));
        code.push_str("{\n");

        // 调试日志
        if self.config.debug_logging {
            code.push_str(&format!(
                "    LX_LOG(LogRpc, Verbose, \"Received RPC: {}\");\n",
                func.name
            ));
        }

        // 反序列化参数
        code.push_str("    // 反序列化参数\n");
        for param in &func.parameters {
            code.push_str(&format!(
                "    {} {};\n",
                param.param_type.to_string(),
                param.name
            ));
            code.push_str(&format!("    Reader >> {};\n", param.name));
        }

        // 调用实现
        code.push_str(&format!(
            "\n    {}_Implementation({});\n",
            func.name,
            self.format_parameter_names(&func.parameters)
        ));

        // 带宽统计
        if self.config.bandwidth_stats {
            code.push_str(&format!(
                "\n    GRpcStats.RecordReceived(\"{}\", Reader.GetSize());\n",
                func.name
            ));
        }

        code.push_str("}\n");
        code
    }

    /// 生成 RPC 调度器
    fn generate_rpc_dispatcher(&self, class: &ClassDecl, functions: &[&MethodDecl]) -> String {
        let mut code = String::new();

        code.push_str(&format!(
            "void {}::DispatchRpc(UInt32 FunctionId, FMemoryReader& Reader)\n",
            class.name
        ));
        code.push_str("{\n");
        code.push_str("    switch (FunctionId)\n");
        code.push_str("    {\n");

        for func in functions {
            code.push_str(&format!("    case {}RpcIds::{}:\n", class.name, func.name));
            code.push_str(&format!("        Receive_{}(Reader);\n", func.name));
            code.push_str("        break;\n");
        }

        code.push_str("    default:\n");

        // 调用父类
        if let Some(base) = class.get_first_base() {
            code.push_str(&format!(
                "        {}::DispatchRpc(FunctionId, Reader);\n",
                base
            ));
        } else {
            code.push_str(
                "        LX_LOG(LogRpc, Warning, \"Unknown RPC function ID: %u\", FunctionId);\n",
            );
        }

        code.push_str("        break;\n");
        code.push_str("    }\n");
        code.push_str("}\n");

        code
    }

    /// 生成 RPC 验证函数
    fn generate_rpc_validation(&self, class: &ClassDecl, functions: &[&MethodDecl]) -> String {
        let mut code = String::new();

        code.push_str("\n// RPC 验证函数\n");

        for func in functions {
            let rpc_type = RpcType::from_method(func);

            code.push_str(&format!(
                "bool {}::Validate_{}() const\n",
                class.name, func.name
            ));
            code.push_str("{\n");

            match rpc_type {
                RpcType::Client => {
                    code.push_str("    // 客户端 RPC: 必须在服务器上调用\n");
                    code.push_str("    if (!HasAuthority())\n");
                    code.push_str("    {\n");
                    code.push_str("        return false;\n");
                    code.push_str("    }\n");
                }
                RpcType::Server => {
                    code.push_str("    // 服务器 RPC: 必须在客户端调用\n");
                    code.push_str("    if (HasAuthority())\n");
                    code.push_str("    {\n");
                    code.push_str("        return false;\n");
                    code.push_str("    }\n");
                }
                RpcType::NetMulticast => {
                    code.push_str("    // 多播 RPC: 必须在服务器上调用\n");
                    code.push_str("    if (!HasAuthority())\n");
                    code.push_str("    {\n");
                    code.push_str("        return false;\n");
                    code.push_str("    }\n");
                }
                RpcType::None => {}
            }

            // 检查是否有自定义验证
            let validate_func = format!("{}_Validate", func.name);
            if class.get_methods().iter().any(|f| f.name == validate_func) {
                code.push_str(&format!("\n    if (!{}())\n", validate_func));
                code.push_str("    {\n");
                code.push_str("        return false;\n");
                code.push_str("    }\n");
            }

            code.push_str("\n    return true;\n");
            code.push_str("}\n\n");
        }

        code
    }

    /// 分配函数 ID
    fn allocate_function_id(&mut self, func_key: &str) -> u32 {
        if let Some(&id) = self.function_ids.get(func_key) {
            return id;
        }

        let id = self.next_function_id;
        self.next_function_id += 1;
        self.function_ids.insert(func_key.to_string(), id);
        id
    }

    /// 格式化参数列表
    fn format_parameters(&self, params: &[ParameterDecl]) -> String {
        params
            .iter()
            .map(|p| format!("{} {}", p.param_type.to_string(), p.name))
            .collect::<Vec<_>>()
            .join(", ")
    }

    /// 格式化参数名称列表
    fn format_parameter_names(&self, params: &[ParameterDecl]) -> String {
        params
            .iter()
            .map(|p| p.name.as_str())
            .collect::<Vec<_>>()
            .join(", ")
    }
}

impl Default for RpcCodeGenerator {
    fn default() -> Self {
        Self::new(RpcConfig::default())
    }
}

//=============================================================================
// RPC 带宽分析
//=============================================================================

/// RPC 参数大小估算器
pub struct RpcSizeEstimator;

impl RpcSizeEstimator {
    /// 估算 RPC 调用的大小
    pub fn estimate_call_size(func: &MethodDecl) -> usize {
        let mut size = 4; // 函数 ID

        for param in &func.parameters {
            size += Self::estimate_type_size(&param.param_type.to_string());
        }

        size
    }

    /// 估算类型大小
    fn estimate_type_size(type_name: &str) -> usize {
        match type_name {
            "bool" | "Int8" | "UInt8" => 1,
            "Int16" | "UInt16" => 2,
            "Int32" | "UInt32" | "Float32" | "float" | "int" => 4,
            "Int64" | "UInt64" | "Float64" | "double" => 8,
            "FVector" | "FVector3" => 12,
            "FVector4" | "FQuat" => 16,
            "FRotator" => 12,
            "FTransform" => 40,
            "FString" | "String" => 64, // 估算平均值
            _ => 32,                    // 默认估算
        }
    }

    /// 检查 RPC 是否过大
    pub fn is_oversized(func: &MethodDecl, max_size: usize) -> bool {
        Self::estimate_call_size(func) > max_size
    }

    /// 生成大小警告
    pub fn generate_size_warnings(class: &ClassDecl, max_size: usize) -> Vec<String> {
        let mut warnings = Vec::new();

        for func in class.get_methods() {
            if RpcType::from_method(func) != RpcType::None {
                let size = Self::estimate_call_size(func);
                if size > max_size {
                    warnings.push(format!(
                        "RPC {}::{} 估算大小 {} 字节超过限制 {} 字节",
                        class.name, func.name, size, max_size
                    ));
                }
            }
        }

        warnings
    }
}
