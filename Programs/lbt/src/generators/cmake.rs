/*******************************************************************************
 * 文件: generator.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   CMake 生成功能
 *   - 为每个模块生成 CMakeLists.txt
 *   - 生成主项目配置
 *   - 处理依赖传递
 *
 ******************************************************************************/

use anyhow::Result;
use std::fs;
use std::path::Path;

use crate::core::build_config::BuildConfig;
use crate::core::config::ModuleType;
use crate::core::dependency::DependencyGraph;

/// 生成 CMake 项目配置
pub fn generate_cmake(
    graph: &DependencyGraph,
    output_dir: &Path,
    source_dir: &Path,
    platform: &str,
    config: &str,
) -> Result<()> {
    // 加载构建配置
    let build_config = BuildConfig::load_from_dir(source_dir).unwrap_or_default();

    // 确保输出目录存在
    fs::create_dir_all(output_dir)?;

    // 生成主 CMakeLists.txt
    generate_main_cmake_with_config(graph, output_dir, platform, config, &build_config)?;

    // 为每个模块生成配置
    for module_name in &graph.build_order {
        if graph.modules.contains_key(module_name) {
            generate_module_cmake_with_config(
                module_name,
                graph,
                output_dir,
                source_dir,
                &build_config,
            )?;
        }
    }

    Ok(())
}

/// 生成主 CMakeLists.txt (带配置)
fn generate_main_cmake_with_config(
    graph: &DependencyGraph,
    output_dir: &Path,
    _platform: &str,
    _config: &str,
    build_config: &BuildConfig,
) -> Result<()> {
    let mut content = String::new();

    let project_name = &build_config.project_name;
    let cpp_std = &build_config.compiler.cpp_standard;
    let warning_level = build_config.compiler.warning_level;
    let warnings_as_errors = build_config.compiler.warnings_as_errors;

    content.push_str(&format!(
        r#"#===============================================================================
# {project_name} - 自动生成的 CMake 配置
# 由 LBT (Limx Build Tool) 生成
# 警告: 请勿手动修改此文件，它会被自动覆盖
#
# 构建配置:
#   - C++ 标准: {cpp_std}
#   - 警告级别: {warning_level}
#   - 警告视为错误: {warnings_as_errors}
#===============================================================================

cmake_minimum_required(VERSION 3.28)
project({project_name} LANGUAGES CXX)

# C++ 标准
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
"#
    ));

    // 编译器缓存
    content.push_str(
        r#"
#===============================================================================
# 编译器缓存 (sccache/ccache)
#===============================================================================
find_program(SCCACHE_PROGRAM sccache)
find_program(CCACHE_PROGRAM ccache)

if(SCCACHE_PROGRAM)
    message(STATUS "Using sccache: ${SCCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
elseif(CCACHE_PROGRAM)
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

#===============================================================================
# 编译选项
#===============================================================================
if(MSVC)
    add_compile_options(/W4 /utf-8 /MP)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(/Od /Zi)
    else()
        add_compile_options(/O2)
    endif()
endif()

#===============================================================================
# 输出目录结构
#===============================================================================
if(WIN32)
    set(LIMX_PLATFORM "Win64")
elseif(APPLE)
    set(LIMX_PLATFORM "Mac")
else()
    set(LIMX_PLATFORM "Linux")
endif()

set(LIMX_BUILD_CONFIG "$<IF:$<CONFIG:Debug>,Debug,$<IF:$<CONFIG:Release>,Release,Development>>")
set(LIMX_BINARIES_DIR "${CMAKE_SOURCE_DIR}/Binaries/${LIMX_PLATFORM}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${LIMX_BINARIES_DIR}/${LIMX_BUILD_CONFIG}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${LIMX_BINARIES_DIR}/${LIMX_BUILD_CONFIG}")
set(LIMX_LIBRARIES_DIR "${CMAKE_SOURCE_DIR}/Binaries/${LIMX_PLATFORM}/Libraries")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${LIMX_LIBRARIES_DIR}/${LIMX_BUILD_CONFIG}")
set(LIMX_INTERMEDIATE_DIR "${CMAKE_SOURCE_DIR}/Intermediate/${LIMX_PLATFORM}")

foreach(CONFIG_TYPE Debug Release Development)
    string(TOUPPER ${CONFIG_TYPE} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${LIMX_BINARIES_DIR}/${CONFIG_TYPE}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${LIMX_BINARIES_DIR}/${CONFIG_TYPE}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${LIMX_LIBRARIES_DIR}/${CONFIG_TYPE}")
endforeach()

set(CMAKE_PDB_OUTPUT_DIRECTORY "${LIMX_BINARIES_DIR}/${LIMX_BUILD_CONFIG}")
set(LIMX_GENERATED_DIR "${CMAKE_SOURCE_DIR}/Intermediate/Generated/${LIMX_PLATFORM}")

"#,
    );

    // 添加模块目录
    content.push_str("# 模块列表 (按依赖顺序)\n");
    for module_name in &graph.build_order {
        content.push_str(&format!(
            "include(${{CMAKE_CURRENT_LIST_DIR}}/{}.cmake)\n",
            module_name
        ));
    }

    content.push_str("\n# 模块依赖关系图\n");
    content.push_str("# ");
    for module_name in &graph.build_order {
        if let Some(module) = graph.modules.get(module_name) {
            content.push_str(&format!("Layer {}: {} | ", module.layer, module_name));
        }
    }
    content.push_str("\n");

    fs::write(output_dir.join("LimxModules.cmake"), content)?;

    Ok(())
}

/// 为单个模块生成 CMake 配置 (带配置)
fn generate_module_cmake_with_config(
    module_name: &str,
    graph: &DependencyGraph,
    output_dir: &Path,
    source_dir: &Path,
    _build_config: &BuildConfig,
) -> Result<()> {
    let module = match graph.modules.get(module_name) {
        Some(m) => m,
        None => return Err(anyhow::anyhow!("模块 '{}' 在依赖图中不存在", module_name)),
    };
    let mut content = String::new();

    // 头部注释
    content.push_str(&format!(
        r#"#===============================================================================
# 模块: {name}
# 层级: Layer {layer}
# 类型: {module_type:?}
# 由 LBT 自动生成
#===============================================================================

"#,
        name = module_name,
        layer = module.layer,
        module_type = module.module_type
    ));

    // 源文件收集 (计算相对于 source_dir 的路径)
    // 规范化路径并计算相对路径
    let module_path = module
        .path
        .canonicalize()
        .unwrap_or_else(|_| module.path.clone());
    let source_path = source_dir
        .canonicalize()
        .unwrap_or_else(|_| source_dir.to_path_buf());

    let relative_path = if let Ok(rel) = module_path.strip_prefix(&source_path) {
        // 成功获取相对路径
        let source_dir_name = source_dir
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("Source");
        format!(
            "{}/{}",
            source_dir_name,
            rel.display().to_string().replace('\\', "/")
        )
    } else {
        // 无法获取相对路径，使用绝对路径
        module.path.display().to_string().replace('\\', "/")
    };
    content.push_str(&format!(
        r#"# 源文件
set({name}_SOURCE_DIR "${{CMAKE_SOURCE_DIR}}/{path}")

file(GLOB_RECURSE {name}_SOURCES
    "${{{name}_SOURCE_DIR}}/Private/*.cpp"
    "${{{name}_SOURCE_DIR}}/Private/*.c"
)

file(GLOB_RECURSE {name}_HEADERS
    "${{{name}_SOURCE_DIR}}/Public/*.h"
    "${{{name}_SOURCE_DIR}}/Public/*.hpp"
)

"#,
        name = module_name,
        path = relative_path
    ));

    // 目标定义
    match module.module_type {
        ModuleType::Static => {
            content.push_str(&format!(
                "add_library({} STATIC ${{{}_SOURCES}} ${{{}_HEADERS}})\n\n",
                module_name, module_name, module_name
            ));
        }
        ModuleType::Shared => {
            content.push_str(&format!(
                "add_library({} SHARED ${{{}_SOURCES}} ${{{}_HEADERS}})\n\n",
                module_name, module_name, module_name
            ));
        }
        ModuleType::Executable => {
            content.push_str(&format!(
                "add_executable({} ${{{}_SOURCES}} ${{{}_HEADERS}})\n\n",
                module_name, module_name, module_name
            ));
        }
        ModuleType::HeaderOnly => {
            content.push_str(&format!("add_library({} INTERFACE)\n\n", module_name));
        }
        ModuleType::External => {
            content.push_str(&format!(
                "add_library({} INTERFACE IMPORTED)\n\n",
                module_name
            ));
        }
    }

    // 模块中间文件目录
    if module.module_type != ModuleType::HeaderOnly {
        content.push_str(&format!(
            r#"# 中间文件目录
set_target_properties({name} PROPERTIES
    # 对象文件输出目录
    OBJECT_OUTPUT_DIRECTORY "${{LIMX_INTERMEDIATE_DIR}}/{name}"
    # 预编译头目录
    VS_GLOBAL_PrecompiledHeaderOutputDirectory "${{LIMX_INTERMEDIATE_DIR}}/{name}/PCH"
)

"#,
            name = module_name
        ));
    }

    // 包含目录
    let include_keyword = if module.module_type == ModuleType::HeaderOnly {
        "INTERFACE"
    } else {
        "PUBLIC"
    };

    content.push_str(&format!(
        r#"# 包含目录
target_include_directories({name} {keyword}
    ${{{name}_SOURCE_DIR}}/Public
)

"#,
        name = module_name,
        keyword = include_keyword
    ));

    // 编译定义
    if !module.config.compile.defines.is_empty() {
        content.push_str(&format!(
            "# 编译定义\ntarget_compile_definitions({} PRIVATE\n",
            module_name
        ));
        for define in &module.config.compile.defines {
            content.push_str(&format!("    {}\n", define));
        }
        content.push_str(")\n\n");
    }

    // 公开依赖
    if !module.config.dependencies.public.is_empty() {
        content.push_str(&format!(
            "# 公开依赖\ntarget_link_libraries({} PUBLIC\n",
            module_name
        ));
        for dep in &module.config.dependencies.public {
            content.push_str(&format!("    {}\n", dep));
        }
        content.push_str(")\n\n");
    }

    // 私有依赖
    if !module.config.dependencies.private.is_empty() {
        content.push_str(&format!(
            "# 私有依赖\ntarget_link_libraries({} PRIVATE\n",
            module_name
        ));
        for dep in &module.config.dependencies.private {
            content.push_str(&format!("    {}\n", dep));
        }
        content.push_str(")\n\n");
    }

    // PCH 预编译头支持
    content.push_str(&format!(
        r#"# 预编译头 (PCH) 支持
if(EXISTS "${{{name}_SOURCE_DIR}}/Private/{name}PCH.h")
    target_precompile_headers({name} PRIVATE
        "${{{name}_SOURCE_DIR}}/Private/{name}PCH.h"
    )
    message(STATUS "[{name}] PCH enabled: {name}PCH.h")
endif()

"#,
        name = module_name
    ));

    // Unity Build 支持
    content.push_str(&format!(
        r#"# Unity Build 支持 (可选)
option({name}_UNITY_BUILD "Enable Unity Build for {name}" OFF)
if({name}_UNITY_BUILD)
    set_target_properties({name} PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 16
    )
    message(STATUS "[{name}] Unity Build enabled")
endif()

"#,
        name = module_name
    ));

    // 并行编译
    if module.module_type != ModuleType::HeaderOnly {
        content.push_str(&format!(
            r#"# 并行编译优化
if(MSVC)
    target_compile_options({name} PRIVATE /MP)  # 多处理器编译
endif()

"#,
            name = module_name
        ));
    }

    // 源文件分组 (IDE)
    content.push_str(&format!(
        r#"# IDE 源文件分组
source_group(TREE "${{{name}_SOURCE_DIR}}" FILES ${{{name}_SOURCES}} ${{{name}_HEADERS}})
"#,
        name = module_name
    ));

    fs::write(output_dir.join(format!("{}.cmake", module_name)), content)?;

    Ok(())
}
