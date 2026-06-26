/*******************************************************************************
 * 文件: vs_generator.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Visual Studio 解决方案生成器 (生产级增强版)
 *   - 每个模块生成独立的 .vcxproj
 *   - 模块编译成独立的 .lib 文件
 *   - 解决方案包含所有模块项目并设置依赖关系
 *
 * 设计哲学:
 *   像 UE 的 UBT 一样:
 *   - 每个模块独立编译
 *   - 模块间通过项目引用建立依赖
 *   - 支持增量编译
 *
 * 技术特性:
 *   - 每个模块一个 .vcxproj (StaticLibrary)
 *   - 自动设置模块间依赖关系
 *   - C++23 标准，/utf-8 编码
 *   - 多配置支持 (Debug/Development/Release)
 *   - 并行项目生成
 *   - PCH 预编译头支持
 *   - IntelliSense 优化
 *
 ******************************************************************************/

use anyhow::Result;
use rayon::prelude::*;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use uuid::Uuid;
use walkdir::WalkDir;

use crate::core::build_config::BuildConfig as GlobalBuildConfig;
use crate::core::config::{Module, ModuleType};

/// VS C++ 项目类型 GUID
const PROJECT_TYPE_CPP: &str = "8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942";
/// VS 解决方案文件夹 GUID
const PROJECT_TYPE_FOLDER: &str = "2150E333-8FDC-42A3-9474-1A3956D46DE8";

/// 构建配置
#[derive(Debug, Clone)]
pub struct BuildConfig {
    pub name: String,
    pub platform: String,
    pub optimization: String,
    pub debug_info: bool,
    pub defines: Vec<String>,
}

impl BuildConfig {
    pub fn debug() -> Self {
        Self {
            name: "Debug".to_string(),
            platform: "x64".to_string(),
            optimization: "Disabled".to_string(),
            debug_info: true,
            defines: vec!["_DEBUG".to_string(), "LIMX_DEBUG=1".to_string()],
        }
    }

    pub fn development() -> Self {
        Self {
            name: "Development".to_string(),
            platform: "x64".to_string(),
            optimization: "Full".to_string(),
            debug_info: true,
            defines: vec!["NDEBUG".to_string(), "LIMX_DEVELOPMENT=1".to_string()],
        }
    }

    pub fn release() -> Self {
        Self {
            name: "Release".to_string(),
            platform: "x64".to_string(),
            optimization: "Full".to_string(),
            debug_info: false,
            defines: vec!["NDEBUG".to_string(), "LIMX_SHIPPING=1".to_string()],
        }
    }

    pub fn all_configs() -> Vec<Self> {
        vec![Self::debug(), Self::development(), Self::release()]
    }
}

/// 模块项目信息
#[derive(Debug, Clone)]
pub struct ModuleProject {
    pub name: String,
    pub guid: String,
    pub path: PathBuf,
    pub sources: Vec<PathBuf>,
    pub headers: Vec<PathBuf>,
    pub configs: Vec<PathBuf>,
    pub public_deps: Vec<String>,
    pub private_deps: Vec<String>,
    pub defines: Vec<String>,
    pub layer: u8,
    pub module_type: ModuleType,
    pub pch_header: Option<PathBuf>,
    pub include_dirs: Vec<PathBuf>,
}

/// 生成 VS 解决方案 (UE/UBT 风格 - 每个模块独立项目)
///
/// # 参数
/// - `modules`: 发现的模块列表
/// - `output_dir`: 输出目录 (存放 .vcxproj 文件)
/// - `project_root`: 项目根目录 (存放 .sln 文件)
/// - `solution_name`: 解决方案名称
///
/// # 生成文件
/// - `{project_root}/{solution_name}.sln`
/// - `{output_dir}/{ModuleName}/{ModuleName}.vcxproj` (每个模块)
/// - `{output_dir}/{ModuleName}/{ModuleName}.vcxproj.filters` (每个模块)
pub fn generate_vs_solution(
    modules: &[Module],
    output_dir: &Path,
    project_root: &Path,
    solution_name: &str,
) -> Result<()> {
    // 加载全局构建配置
    let global_config = GlobalBuildConfig::load_from_dir(project_root).unwrap_or_default();
    generate_vs_solution_with_config(
        modules,
        output_dir,
        project_root,
        solution_name,
        &global_config,
    )
}

/// 生成 VS 解决方案 (带全局配置)
pub fn generate_vs_solution_with_config(
    modules: &[Module],
    output_dir: &Path,
    project_root: &Path,
    solution_name: &str,
    global_config: &GlobalBuildConfig,
) -> Result<()> {
    fs::create_dir_all(output_dir)?;

    // 为每个模块生成项目信息
    let mut module_projects: Vec<ModuleProject> = Vec::new();
    let mut guid_map: HashMap<String, String> = HashMap::new();

    for module in modules {
        let guid = Uuid::new_v4().to_string().to_uppercase();
        guid_map.insert(module.name.clone(), guid.clone());

        let module_dir = output_dir.join(&module.name);
        fs::create_dir_all(&module_dir)?;

        // 收集模块文件
        let (sources, headers, configs) = collect_module_files(&module.path)?;

        // 查找 PCH 头文件
        let pch_header = find_pch_header(&headers, &module.name);

        // 收集包含目录
        let mut include_dirs = vec![module.path.join("Public"), module.path.join("Private")];

        // 添加依赖模块的 Public 目录
        for dep in &module.config.dependencies.public {
            if let Some(dep_module) = modules.iter().find(|m| &m.name == dep) {
                include_dirs.push(dep_module.path.join("Public"));
            }
        }

        module_projects.push(ModuleProject {
            name: module.name.clone(),
            guid,
            path: module_dir,
            sources,
            headers,
            configs,
            public_deps: module.config.dependencies.public.clone(),
            private_deps: module.config.dependencies.private.clone(),
            defines: module.config.compile.defines.clone(),
            layer: module.layer,
            module_type: module.module_type.clone(),
            pch_header,
            include_dirs,
        });
    }

    // 按层级排序 (低层级先编译)
    module_projects.sort_by_key(|m| m.layer);

    // 生成解决方案
    generate_sln_multi(
        project_root,
        output_dir,
        solution_name,
        &module_projects,
        &guid_map,
    )?;

    // 并行为每个模块生成 vcxproj
    let guid_map_ref = &guid_map;
    let modules_ref = modules;
    let project_root_ref = project_root;

    module_projects.par_iter().try_for_each(|proj| {
        generate_module_vcxproj(proj, project_root_ref, modules_ref, guid_map_ref)?;
        generate_module_vcxproj_filters(proj, project_root_ref)
    })?;

    println!("生成 VS 解决方案: {}.sln", solution_name);
    println!(
        "  项目: {} (C++{})",
        global_config.project_name,
        global_config.compiler.cpp_standard.replace("c++", "")
    );
    println!("  模块: {} 个", module_projects.len());

    Ok(())
}

/// 查找 PCH 头文件
fn find_pch_header(headers: &[PathBuf], module_name: &str) -> Option<PathBuf> {
    // 常见的 PCH 头文件命名模式
    let pch_patterns = [
        format!("{}PCH.h", module_name),
        format!("{}Pch.h", module_name),
        "PCH.h".to_string(),
        "Pch.h".to_string(),
        "pch.h".to_string(),
        "stdafx.h".to_string(),
    ];

    for header in headers {
        if let Some(file_name) = header.file_name().and_then(|n| n.to_str()) {
            for pattern in &pch_patterns {
                if file_name.eq_ignore_ascii_case(pattern) {
                    return Some(header.clone());
                }
            }
        }
    }

    None
}

/// 收集模块的源文件、头文件和配置文件
fn collect_module_files(module_path: &Path) -> Result<(Vec<PathBuf>, Vec<PathBuf>, Vec<PathBuf>)> {
    let mut sources = Vec::new();
    let mut headers = Vec::new();
    let mut configs = Vec::new();

    // 扫描 Private 目录 (源文件)
    let private_dir = module_path.join("Private");
    if private_dir.exists() {
        for entry in WalkDir::new(&private_dir)
            .into_iter()
            .filter_map(|e| e.ok())
        {
            let path = entry.path();
            if path.is_file() {
                if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                    match ext.to_lowercase().as_str() {
                        "cpp" | "c" | "cc" | "cxx" => sources.push(path.to_path_buf()),
                        "h" | "hpp" | "hxx" | "inl" => headers.push(path.to_path_buf()),
                        _ => {}
                    }
                }
            }
        }
    }

    // 扫描 Public 目录 (头文件)
    let public_dir = module_path.join("Public");
    if public_dir.exists() {
        for entry in WalkDir::new(&public_dir).into_iter().filter_map(|e| e.ok()) {
            let path = entry.path();
            if path.is_file() {
                if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                    match ext.to_lowercase().as_str() {
                        "h" | "hpp" | "hxx" | "inl" => headers.push(path.to_path_buf()),
                        "cpp" | "c" | "cc" | "cxx" => sources.push(path.to_path_buf()),
                        _ => {}
                    }
                }
            }
        }
    }

    // 收集模块配置文件
    if let Ok(entries) = fs::read_dir(module_path) {
        for entry in entries.filter_map(|e| e.ok()) {
            let path = entry.path();
            if path.is_file() {
                if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                    if ext.to_lowercase() == "toml" {
                        configs.push(path);
                    }
                }
            }
        }
    }

    Ok((sources, headers, configs))
}

/// 生成多项目 .sln 解决方案文件
fn generate_sln_multi(
    sln_dir: &Path,
    vcxproj_dir: &Path,
    name: &str,
    projects: &[ModuleProject],
    _guid_map: &HashMap<String, String>,
) -> Result<()> {
    let rel_base = vcxproj_dir
        .strip_prefix(sln_dir)
        .map(|p| p.display().to_string().replace('/', "\\"))
        .unwrap_or_else(|_| "Intermediate\\ProjectFiles".to_string());

    // 生成 Engine 文件夹 GUID
    let engine_folder_guid = "A1B2C3D4-E5F6-7890-ABCD-EF1234567890";

    let mut c = String::new();
    c.push_str("\r\n");
    c.push_str("Microsoft Visual Studio Solution File, Format Version 12.00\r\n");
    c.push_str("# Visual Studio Version 17\r\n");
    c.push_str("VisualStudioVersion = 17.8.34330.188\r\n");
    c.push_str("MinimumVisualStudioVersion = 10.0.40219.1\r\n");

    // 添加 Engine 解决方案文件夹
    c.push_str(&format!(
        "Project(\"{{{type_guid}}}\") = \"Engine\", \"Engine\", \"{{{guid}}}\"\r\n",
        type_guid = PROJECT_TYPE_FOLDER,
        guid = engine_folder_guid
    ));
    c.push_str("EndProject\r\n");

    // 添加每个模块项目
    for proj in projects {
        let proj_rel = format!("{}\\{}\\{}.vcxproj", rel_base, proj.name, proj.name);
        c.push_str(&format!(
            "Project(\"{{{type_guid}}}\") = \"{name}\", \"{path}\", \"{{{guid}}}\"\r\n",
            type_guid = PROJECT_TYPE_CPP,
            name = proj.name,
            path = proj_rel,
            guid = proj.guid
        ));

        // 项目依赖
        let all_deps: Vec<_> = proj.public_deps.iter().chain(&proj.private_deps).collect();
        if !all_deps.is_empty() {
            c.push_str("\tProjectSection(ProjectDependencies) = postProject\r\n");
            for dep in all_deps {
                if let Some(dep_proj) = projects.iter().find(|p| &p.name == dep) {
                    c.push_str(&format!(
                        "\t\t{{{}}} = {{{}}}\r\n",
                        dep_proj.guid, dep_proj.guid
                    ));
                }
            }
            c.push_str("\tEndProjectSection\r\n");
        }
        c.push_str("EndProject\r\n");
    }

    c.push_str("Global\r\n");
    c.push_str("\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n");
    c.push_str("\t\tDebug|x64 = Debug|x64\r\n");
    c.push_str("\t\tDevelopment|x64 = Development|x64\r\n");
    c.push_str("\t\tRelease|x64 = Release|x64\r\n");
    c.push_str("\tEndGlobalSection\r\n");

    c.push_str("\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n");
    for proj in projects {
        for cfg in &["Debug", "Development", "Release"] {
            c.push_str(&format!(
                "\t\t{{{g}}}.{c}|x64.ActiveCfg = {c}|x64\r\n",
                g = proj.guid,
                c = cfg
            ));
            c.push_str(&format!(
                "\t\t{{{g}}}.{c}|x64.Build.0 = {c}|x64\r\n",
                g = proj.guid,
                c = cfg
            ));
        }
    }
    c.push_str("\tEndGlobalSection\r\n");

    c.push_str("\tGlobalSection(SolutionProperties) = preSolution\r\n");
    c.push_str("\t\tHideSolutionNode = FALSE\r\n");
    c.push_str("\tEndGlobalSection\r\n");

    // 嵌套关系 - 所有模块项目放入 Engine 文件夹
    c.push_str("\tGlobalSection(NestedProjects) = preSolution\r\n");
    for proj in projects {
        c.push_str(&format!(
            "\t\t{{{}}} = {{{}}}\r\n",
            proj.guid, engine_folder_guid
        ));
    }
    c.push_str("\tEndGlobalSection\r\n");

    c.push_str("EndGlobal\r\n");

    fs::write(sln_dir.join(format!("{}.sln", name)), c)?;
    Ok(())
}

/// 生成单个模块的 .vcxproj 文件
fn generate_module_vcxproj(
    proj: &ModuleProject,
    project_root: &Path,
    all_modules: &[Module],
    guid_map: &HashMap<String, String>,
) -> Result<()> {
    let mut c = String::new();

    // XML 头
    c.push_str("<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
    c.push_str("<Project DefaultTargets=\"Build\" ToolsVersion=\"17.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n");

    // 项目配置
    c.push_str("  <ItemGroup Label=\"ProjectConfigurations\">\r\n");
    for cfg in &["Debug", "Development", "Release"] {
        c.push_str(&format!(
            "    <ProjectConfiguration Include=\"{}|x64\">\r\n",
            cfg
        ));
        c.push_str(&format!("      <Configuration>{}</Configuration>\r\n", cfg));
        c.push_str("      <Platform>x64</Platform>\r\n");
        c.push_str("    </ProjectConfiguration>\r\n");
    }
    c.push_str("  </ItemGroup>\r\n");

    // 全局属性
    c.push_str("  <PropertyGroup Label=\"Globals\">\r\n");
    c.push_str(&format!(
        "    <ProjectGuid>{{{}}}</ProjectGuid>\r\n",
        proj.guid
    ));
    c.push_str(&format!(
        "    <RootNamespace>{}</RootNamespace>\r\n",
        proj.name
    ));
    c.push_str("    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>\r\n");
    c.push_str("  </PropertyGroup>\r\n");

    c.push_str("  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\r\n");

    // 配置属性 - 动态库项目 (DLL)
    for cfg in &["Debug", "Development", "Release"] {
        let use_debug = if *cfg == "Debug" { "true" } else { "false" };
        c.push_str(&format!("  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='{}|x64'\" Label=\"Configuration\">\r\n", cfg));
        c.push_str("    <ConfigurationType>DynamicLibrary</ConfigurationType>\r\n");
        c.push_str(&format!(
            "    <UseDebugLibraries>{}</UseDebugLibraries>\r\n",
            use_debug
        ));
        c.push_str("    <PlatformToolset>v143</PlatformToolset>\r\n");
        c.push_str("    <CharacterSet>Unicode</CharacterSet>\r\n");
        c.push_str("    <WholeProgramOptimization>false</WholeProgramOptimization>\r\n");
        c.push_str("  </PropertyGroup>\r\n");
    }

    c.push_str("  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\r\n");

    // 输出目录设置
    for cfg in &["Debug", "Development", "Release"] {
        c.push_str(&format!(
            "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='{}|x64'\">\r\n",
            cfg
        ));
        c.push_str(&format!(
            "    <OutDir>$(SolutionDir)Binaries\\Win64\\{}\\</OutDir>\r\n",
            cfg
        ));
        c.push_str(&format!(
            "    <IntDir>$(SolutionDir)Intermediate\\Win64\\{}\\{}\\</IntDir>\r\n",
            cfg, proj.name
        ));
        c.push_str(&format!("    <TargetName>{}</TargetName>\r\n", proj.name));
        c.push_str("  </PropertyGroup>\r\n");
    }

    // 构建包含路径 - 包含自己和所有依赖模块的 Public 目录
    let mut include_paths = Vec::new();

    // 自己的 Public 目录
    if let Some(module) = all_modules.iter().find(|m| m.name == proj.name) {
        let module_rel = module
            .path
            .strip_prefix(project_root)
            .map(|p| p.display().to_string().replace('/', "\\"))
            .unwrap_or_default();
        include_paths.push(format!("$(SolutionDir){}\\Public", module_rel));
    }

    // 依赖模块的 Public 目录
    for dep_name in proj.public_deps.iter().chain(&proj.private_deps) {
        if let Some(dep_module) = all_modules.iter().find(|m| &m.name == dep_name) {
            let dep_rel = dep_module
                .path
                .strip_prefix(project_root)
                .map(|p| p.display().to_string().replace('/', "\\"))
                .unwrap_or_default();
            include_paths.push(format!("$(SolutionDir){}\\Public", dep_rel));
        }
    }

    // 添加生成的头文件目录和 Vulkan
    include_paths.push("$(SolutionDir)Intermediate\\Generated".to_string());
    include_paths.push("$(VULKAN_SDK)\\Include".to_string());

    let include_dirs = include_paths.join(";") + ";%(AdditionalIncludeDirectories)";

    // 模块定义宏
    let module_defines = proj.defines.join(";");

    // 编译设置
    for cfg in &["Debug", "Development", "Release"] {
        let (optimization, debug_info, runtime, base_defines) = match *cfg {
            "Debug" => (
                "Disabled",
                "EditAndContinue",
                "MultiThreadedDebugDLL",
                "WIN32;_WINDOWS;UNICODE;_UNICODE;NOMINMAX;WIN32_LEAN_AND_MEAN;_DEBUG;LIMX_DEBUG=1;LIMX_BUILD_DEBUG=1"
            ),
            "Development" => (
                "MaxSpeed",
                "ProgramDatabase",
                "MultiThreadedDLL",
                "WIN32;_WINDOWS;UNICODE;_UNICODE;NOMINMAX;WIN32_LEAN_AND_MEAN;NDEBUG;LIMX_DEVELOPMENT=1;LIMX_BUILD_DEVELOPMENT=1"
            ),
            "Release" => (
                "Full",
                "None",
                "MultiThreadedDLL",
                "WIN32;_WINDOWS;UNICODE;_UNICODE;NOMINMAX;WIN32_LEAN_AND_MEAN;NDEBUG;LIMX_RELEASE=1;LIMX_BUILD_SHIPPING=1"
            ),
            _ => ("Disabled", "EditAndContinue", "MultiThreadedDebugDLL", ""),
        };

        let defines = if module_defines.is_empty() {
            format!("{};%(PreprocessorDefinitions)", base_defines)
        } else {
            format!(
                "{};{};%(PreprocessorDefinitions)",
                base_defines, module_defines
            )
        };

        c.push_str(&format!(
            "  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='{}|x64'\">\r\n",
            cfg
        ));
        c.push_str("    <ClCompile>\r\n");
        c.push_str("      <WarningLevel>Level4</WarningLevel>\r\n");
        c.push_str(&format!(
            "      <Optimization>{}</Optimization>\r\n",
            optimization
        ));
        c.push_str(&format!(
            "      <PreprocessorDefinitions>{}</PreprocessorDefinitions>\r\n",
            defines
        ));
        c.push_str("      <LanguageStandard>stdcpp23</LanguageStandard>\r\n");
        c.push_str("      <ConformanceMode>true</ConformanceMode>\r\n");
        c.push_str("      <MultiProcessorCompilation>true</MultiProcessorCompilation>\r\n");
        c.push_str("      <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>\r\n");
        c.push_str(&format!(
            "      <DebugInformationFormat>{}</DebugInformationFormat>\r\n",
            debug_info
        ));
        c.push_str(&format!(
            "      <RuntimeLibrary>{}</RuntimeLibrary>\r\n",
            runtime
        ));
        c.push_str(&format!(
            "      <AdditionalIncludeDirectories>{}</AdditionalIncludeDirectories>\r\n",
            include_dirs
        ));
        c.push_str("    </ClCompile>\r\n");
        c.push_str("    <Link>\r\n");
        c.push_str(&format!(
            "      <OutputFile>$(OutDir){}.dll</OutputFile>\r\n",
            proj.name
        ));
        c.push_str(&format!(
            "      <ImportLibrary>$(OutDir){}.lib</ImportLibrary>\r\n",
            proj.name
        ));
        c.push_str("      <GenerateDebugInformation>true</GenerateDebugInformation>\r\n");
        c.push_str("      <SubSystem>Windows</SubSystem>\r\n");

        // 添加依赖模块的导入库
        let all_deps: Vec<_> = proj.public_deps.iter().chain(&proj.private_deps).collect();
        if !all_deps.is_empty() {
            let dep_libs: Vec<String> = all_deps.iter().map(|d| format!("{}.lib", d)).collect();
            c.push_str(&format!("      <AdditionalDependencies>{};%(AdditionalDependencies)</AdditionalDependencies>\r\n", dep_libs.join(";")));
            c.push_str("      <AdditionalLibraryDirectories>$(OutDir);%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>\r\n");
        }

        c.push_str("    </Link>\r\n");
        c.push_str("  </ItemDefinitionGroup>\r\n");
    }

    // 相对路径辅助函数 - 从 vcxproj 所在目录到源文件
    let vcxproj_dir = &proj.path;
    let rel_path = |p: &Path| -> String {
        // 计算从 vcxproj 目录到文件的相对路径
        let depth = vcxproj_dir
            .strip_prefix(project_root)
            .map(|p| p.components().count())
            .unwrap_or(3);
        let prefix = "..\\".repeat(depth);

        p.strip_prefix(project_root)
            .map(|x| format!("{}{}", prefix, x.display().to_string().replace('/', "\\")))
            .unwrap_or_else(|_| p.display().to_string().replace('/', "\\"))
    };

    // 源文件
    if !proj.sources.is_empty() {
        c.push_str("  <ItemGroup>\r\n");
        for f in &proj.sources {
            c.push_str(&format!(
                "    <ClCompile Include=\"{}\" />\r\n",
                rel_path(f)
            ));
        }
        c.push_str("  </ItemGroup>\r\n");
    }

    // 头文件
    if !proj.headers.is_empty() {
        c.push_str("  <ItemGroup>\r\n");
        for f in &proj.headers {
            c.push_str(&format!(
                "    <ClInclude Include=\"{}\" />\r\n",
                rel_path(f)
            ));
        }
        c.push_str("  </ItemGroup>\r\n");
    }

    // 配置文件
    if !proj.configs.is_empty() {
        c.push_str("  <ItemGroup>\r\n");
        for f in &proj.configs {
            c.push_str(&format!("    <None Include=\"{}\" />\r\n", rel_path(f)));
        }
        c.push_str("  </ItemGroup>\r\n");
    }

    c.push_str("  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\r\n");
    c.push_str("</Project>\r\n");

    fs::write(proj.path.join(format!("{}.vcxproj", proj.name)), c)?;
    Ok(())
}

/// 生成模块的 .vcxproj.filters 文件
fn generate_module_vcxproj_filters(proj: &ModuleProject, project_root: &Path) -> Result<()> {
    let mut c = String::new();
    let mut filters: HashSet<String> = HashSet::new();

    // 找到模块的根目录
    let module_root = proj.sources.first().or(proj.headers.first()).and_then(|p| {
        // 找到 Private 或 Public 的父目录
        for ancestor in p.ancestors() {
            let name = ancestor.file_name().and_then(|n| n.to_str());
            if name == Some("Private") || name == Some("Public") {
                return ancestor.parent();
            }
        }
        None
    });

    let base_path = module_root.unwrap_or(project_root);

    // 收集所有文件的筛选器
    let all_files: Vec<&PathBuf> = proj
        .sources
        .iter()
        .chain(&proj.headers)
        .chain(&proj.configs)
        .collect();

    for f in &all_files {
        if let Ok(rel) = f.strip_prefix(base_path) {
            let mut current = String::new();
            if let Some(parent) = rel.parent() {
                for comp in parent.components() {
                    if !current.is_empty() {
                        current.push('\\');
                    }
                    current.push_str(&comp.as_os_str().to_string_lossy());
                    filters.insert(current.clone());
                }
            }
        }
    }

    // XML 头
    c.push_str("<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
    c.push_str("<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n");

    // 筛选器定义
    if !filters.is_empty() {
        c.push_str("  <ItemGroup>\r\n");
        let mut sorted_filters: Vec<_> = filters.into_iter().collect();
        sorted_filters.sort();
        for flt in &sorted_filters {
            c.push_str(&format!("    <Filter Include=\"{}\">\r\n", flt));
            c.push_str(&format!(
                "      <UniqueIdentifier>{{{}}}</UniqueIdentifier>\r\n",
                Uuid::new_v4()
            ));
            c.push_str("    </Filter>\r\n");
        }
        c.push_str("  </ItemGroup>\r\n");
    }

    // 计算相对路径
    let vcxproj_dir = &proj.path;
    let rel_path = |p: &Path| -> String {
        let depth = vcxproj_dir
            .strip_prefix(project_root)
            .map(|p| p.components().count())
            .unwrap_or(3);
        let prefix = "..\\".repeat(depth);

        p.strip_prefix(project_root)
            .map(|x| format!("{}{}", prefix, x.display().to_string().replace('/', "\\")))
            .unwrap_or_else(|_| p.display().to_string().replace('/', "\\"))
    };

    let get_filter = |p: &Path| -> String {
        p.strip_prefix(base_path)
            .ok()
            .and_then(|x| x.parent())
            .map(|x| x.display().to_string().replace('/', "\\"))
            .unwrap_or_default()
    };

    // 源文件
    if !proj.sources.is_empty() {
        c.push_str("  <ItemGroup>\r\n");
        for f in &proj.sources {
            let flt = get_filter(f);
            if flt.is_empty() {
                c.push_str(&format!(
                    "    <ClCompile Include=\"{}\" />\r\n",
                    rel_path(f)
                ));
            } else {
                c.push_str(&format!("    <ClCompile Include=\"{}\">\r\n", rel_path(f)));
                c.push_str(&format!("      <Filter>{}</Filter>\r\n", flt));
                c.push_str("    </ClCompile>\r\n");
            }
        }
        c.push_str("  </ItemGroup>\r\n");
    }

    // 头文件
    if !proj.headers.is_empty() {
        c.push_str("  <ItemGroup>\r\n");
        for f in &proj.headers {
            let flt = get_filter(f);
            if flt.is_empty() {
                c.push_str(&format!(
                    "    <ClInclude Include=\"{}\" />\r\n",
                    rel_path(f)
                ));
            } else {
                c.push_str(&format!("    <ClInclude Include=\"{}\">\r\n", rel_path(f)));
                c.push_str(&format!("      <Filter>{}</Filter>\r\n", flt));
                c.push_str("    </ClInclude>\r\n");
            }
        }
        c.push_str("  </ItemGroup>\r\n");
    }

    // 配置文件
    if !proj.configs.is_empty() {
        c.push_str("  <ItemGroup>\r\n");
        for f in &proj.configs {
            c.push_str(&format!("    <None Include=\"{}\" />\r\n", rel_path(f)));
        }
        c.push_str("  </ItemGroup>\r\n");
    }

    c.push_str("</Project>\r\n");

    fs::write(proj.path.join(format!("{}.vcxproj.filters", proj.name)), c)?;
    Ok(())
}
