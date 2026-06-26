/*******************************************************************************
 * 文件: codegen/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LHT 代码生成模块 (生产级)
 *   - 反射代码生成器
 *   - 类型注册代码
 *   - 序列化代码
 *   - GC 支持代码
 *   - RPC 网络代码
 *
 ******************************************************************************/

pub mod adapter;
pub mod docs;
pub mod generator;
pub mod reflection;

// 高级代码生成模块
pub mod gc;
pub mod rpc;
pub mod runtime;
pub mod serialization;

pub mod editor_meta;
pub mod incremental;
pub mod migration;
pub mod script_binding;
pub mod type_binding;

pub use incremental::{
    ChangeDetectionResult, ImpactAnalysis, IncrementalCodegen, SignatureCache, TypeKind,
    TypeSignature,
};
pub use runtime::{generate_all_runtime_headers, RuntimeHeaderGenerator};
pub use type_binding::{
    BindingClass, BindingMethod, BindingProperty, GeneratedBinding, TypeBindingGenerator,
};
