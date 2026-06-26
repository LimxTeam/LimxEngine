/*******************************************************************************
 * 文件: ide_generator.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   IDE 项目文件生成器
 *   - CMakePresets.json (VS/VSCode/Rider/CLion)
 *   - .vscode 配置 (VSCode)
 *   - 构建脚本 (批处理/PowerShell/Shell)
 *
 ******************************************************************************/

use anyhow::Result;
use std::fs;
use std::path::Path;

use crate::core::config::Module;
use crate::core::dependency::DependencyGraph;

/// IDE 类型
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IdeType {
    VisualStudio,
    VSCode,
    Rider,
    CLion,
    All,
}

/// 生成所有 IDE 项目文件
pub fn generate_project_files(
    graph: &DependencyGraph,
    modules: &[Module],
    project_dir: &Path,
    ide: IdeType,
) -> Result<()> {
    // 生成 CMakePresets.json (所有 IDE 都需要)
    generate_cmake_presets(project_dir)?;

    // 生成 CMakeLists.txt 主文件 (如果不存在则生成模板)
    generate_root_cmake(graph, project_dir)?;

    match ide {
        IdeType::VSCode | IdeType::All => {
            generate_vscode_config(modules, project_dir)?;
        }
        _ => {}
    }

    // 生成构建脚本
    generate_build_scripts(project_dir)?;

    Ok(())
}

/// 生成 CMakePresets.json
fn generate_cmake_presets(project_dir: &Path) -> Result<()> {
    let content = r#"{
    "version": 6,
    "cmakeMinimumRequired": {
        "major": 3,
        "minor": 28,
        "patch": 0
    },
    "configurePresets": [
        {
            "name": "base",
            "hidden": true,
            "generator": "Visual Studio 17 2022",
            "binaryDir": "${sourceDir}/Intermediate/Build/${presetName}",
            "cacheVariables": {
                "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
            }
        },
        {
            "name": "windows-base",
            "hidden": true,
            "inherits": "base",
            "condition": {
                "type": "equals",
                "lhs": "${hostSystemName}",
                "rhs": "Windows"
            },
            "architecture": {
                "value": "x64",
                "strategy": "external"
            },
            "toolset": {
                "value": "host=x64",
                "strategy": "external"
            },
            "vendor": {
                "microsoft.com/VisualStudioSettings/CMake/1.0": {
                    "hostOS": ["Windows"],
                    "intelliSenseMode": "windows-msvc-x64"
                }
            }
        },
        {
            "name": "debug",
            "displayName": "Debug",
            "description": "调试构建 - 完整调试信息，无优化",
            "inherits": "windows-base",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_FLAGS": "/DLIMX_DEBUG=1 /DLIMX_BUILD_DEBUG=1"
            }
        },
        {
            "name": "development",
            "displayName": "Development", 
            "description": "开发构建 - 调试信息 + 优化",
            "inherits": "windows-base",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "RelWithDebInfo",
                "CMAKE_CXX_FLAGS": "/DLIMX_DEVELOPMENT=1 /DLIMX_BUILD_DEVELOPMENT=1"
            }
        },
        {
            "name": "release",
            "displayName": "Release",
            "description": "发布构建 - 最大优化，无调试信息",
            "inherits": "windows-base",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Release",
                "CMAKE_CXX_FLAGS": "/DLIMX_RELEASE=1 /DLIMX_BUILD_SHIPPING=1"
            }
        }
    ],
    "buildPresets": [
        {
            "name": "debug",
            "displayName": "Build Debug",
            "configurePreset": "debug"
        },
        {
            "name": "development",
            "displayName": "Build Development",
            "configurePreset": "development"
        },
        {
            "name": "release",
            "displayName": "Build Release",
            "configurePreset": "release"
        }
    ],
    "testPresets": [
        {
            "name": "debug",
            "displayName": "Test Debug",
            "configurePreset": "debug"
        },
        {
            "name": "development", 
            "displayName": "Test Development",
            "configurePreset": "development"
        }
    ]
}
"#;

    fs::write(project_dir.join("CMakePresets.json"), content)?;
    Ok(())
}

/// 生成根 CMakeLists.txt
fn generate_root_cmake(graph: &DependencyGraph, project_dir: &Path) -> Result<()> {
    let cmake_path = project_dir.join("CMakeLists.txt");

    // 如果已存在，检查是否包含我们的 include
    if cmake_path.exists() {
        let content = fs::read_to_string(&cmake_path)?;
        if !content.contains("LimxModules.cmake") {
            // 追加 include
            let mut new_content = content;
            new_content
                .push_str("\n# Limx 模块 (由 LBT 生成)\ninclude(Generated/LimxModules.cmake)\n");
            fs::write(&cmake_path, new_content)?;
        }
        return Ok(());
    }

    // 生成新的 CMakeLists.txt
    let mut content = String::from(
        r#"#===============================================================================
# Limx Engine - 主 CMakeLists.txt
# 由 LBT (Limx Build Tool) 生成
#===============================================================================

cmake_minimum_required(VERSION 3.28)

project(LimxEngine
    VERSION 0.1.0
    LANGUAGES CXX C
)

#===============================================================================
# C++ 标准设置
#===============================================================================

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#===============================================================================
# 编译器设置
#===============================================================================

if(MSVC)
    # 禁止使用标准库
    add_compile_definitions(_HAS_EXCEPTIONS=0)
    
    # 警告级别
    add_compile_options(/W4 /WX)
    
    # 多处理器编译
    add_compile_options(/MP)
    
    # UTF-8 源文件
    add_compile_options(/utf-8)
    
    # 快速浮点
    add_compile_options(/fp:fast)
endif()

#===============================================================================
# Vulkan SDK
#===============================================================================

find_package(Vulkan 1.4 REQUIRED)

if(Vulkan_FOUND)
    message(STATUS "Vulkan SDK: ${Vulkan_VERSION}")
    message(STATUS "Vulkan Include: ${Vulkan_INCLUDE_DIRS}")
endif()

#===============================================================================
# Limx 模块 (由 LBT 生成)
#===============================================================================

include(Generated/LimxModules.cmake)

#===============================================================================
# 打印配置摘要
#===============================================================================

message(STATUS "")
message(STATUS "========================================")
message(STATUS "  Limx Engine 配置摘要")
message(STATUS "========================================")
message(STATUS "  版本:       ${PROJECT_VERSION}")
message(STATUS "  C++ 标准:   C++${CMAKE_CXX_STANDARD}")
message(STATUS "  编译器:     ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  构建类型:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Vulkan:     ${Vulkan_VERSION}")
message(STATUS "========================================")
"#,
    );

    // 添加模块信息
    content.push_str("\nmessage(STATUS \"  模块:\")\n");
    for module_name in &graph.build_order {
        content.push_str(&format!("message(STATUS \"    - {}\")\n", module_name));
    }
    content.push_str("message(STATUS \"========================================\")\n");

    fs::write(&cmake_path, content)?;
    Ok(())
}

/// 生成 VSCode 配置
fn generate_vscode_config(modules: &[Module], project_dir: &Path) -> Result<()> {
    let vscode_dir = project_dir.join(".vscode");
    fs::create_dir_all(&vscode_dir)?;

    // settings.json
    let settings = r#"{
    "cmake.configureOnOpen": true,
    "cmake.buildDirectory": "${workspaceFolder}/Intermediate/Build/${buildType}",
    "cmake.generator": "Ninja",
    "cmake.configureSettings": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
    },
    
    "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools",
    "C_Cpp.default.cppStandard": "c++23",
    "C_Cpp.default.intelliSenseMode": "windows-msvc-x64",
    
    "files.associations": {
        "*.limx.toml": "toml",
        "*.generated.h": "cpp",
        "*.generated.cpp": "cpp"
    },
    
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "ms-vscode.cpptools",
    
    "[cpp]": {
        "editor.defaultFormatter": "ms-vscode.cpptools"
    },
    
    "files.exclude": {
        "Binaries": true,
        "Intermediate": true,
        "**/target": true
    },
    
    "search.exclude": {
        "Binaries": true,
        "Intermediate": true,
        "**/target": true
    }
}
"#;
    fs::write(vscode_dir.join("settings.json"), settings)?;

    // tasks.json
    let tasks = r#"{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "LBT: Generate Project",
            "type": "shell",
            "command": "Programs/target/release/lbt.exe",
            "args": ["generate", "--source-dir", "Source", "--output-dir", "Intermediate/Build"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "LHT: Generate Reflection",
            "type": "shell", 
            "command": "Programs/target/release/lht.exe",
            "args": ["generate", "--source-dir", "Source", "--output-dir", "Intermediate/Generated"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "CMake: Configure Debug",
            "type": "shell",
            "command": "cmake",
            "args": ["--preset", "debug"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "CMake: Configure Development",
            "type": "shell",
            "command": "cmake",
            "args": ["--preset", "development"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "CMake: Configure Release",
            "type": "shell",
            "command": "cmake",
            "args": ["--preset", "release"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "Build: Debug",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "--preset", "debug"],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "problemMatcher": ["$msCompile"]
        },
        {
            "label": "Build: Development",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "--preset", "development"],
            "group": "build",
            "problemMatcher": ["$msCompile"]
        },
        {
            "label": "Build: Release",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "--preset", "release"],
            "group": "build",
            "problemMatcher": ["$msCompile"]
        },
        {
            "label": "Full Build: Debug",
            "dependsOrder": "sequence",
            "dependsOn": ["LBT: Generate Project", "LHT: Generate Reflection", "CMake: Configure Debug", "Build: Debug"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "Clean",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "--preset", "debug", "--target", "clean"],
            "group": "build",
            "problemMatcher": []
        }
    ]
}
"#;
    fs::write(vscode_dir.join("tasks.json"), tasks)?;

    // launch.json
    let launch = r#"{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Debug: Limx Editor",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "${workspaceFolder}/Binaries/Win64/Debug/LimxEditor.exe",
            "args": [],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "environment": [],
            "console": "integratedTerminal",
            "preLaunchTask": "Build: Debug"
        },
        {
            "name": "Development: Limx Editor",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "${workspaceFolder}/Binaries/Win64/Development/LimxEditor.exe",
            "args": [],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "environment": [],
            "console": "integratedTerminal",
            "preLaunchTask": "Build: Development"
        }
    ]
}
"#;
    fs::write(vscode_dir.join("launch.json"), launch)?;

    // extensions.json
    let extensions = r#"{
    "recommendations": [
        "ms-vscode.cpptools",
        "ms-vscode.cmake-tools",
        "twxs.cmake",
        "rust-lang.rust-analyzer",
        "tamasfe.even-better-toml"
    ]
}
"#;
    fs::write(vscode_dir.join("extensions.json"), extensions)?;

    // c_cpp_properties.json
    let mut include_paths = vec![
        "${workspaceFolder}/Source/**/Public".to_string(),
        "${workspaceFolder}/Intermediate/Generated".to_string(),
        "${env:VULKAN_SDK}/Include".to_string(),
    ];

    for module in modules {
        include_paths.push(format!(
            "${{workspaceFolder}}/{}/Public",
            module.path.display()
        ));
    }

    let cpp_properties = format!(
        r#"{{
    "configurations": [
        {{
            "name": "Win64",
            "includePath": [
                {}
            ],
            "defines": [
                "_DEBUG",
                "LIMX_DEBUG=1",
                "WIN32",
                "_WINDOWS"
            ],
            "windowsSdkVersion": "10.0.22621.0",
            "compilerPath": "cl.exe",
            "cStandard": "c17",
            "cppStandard": "c++23",
            "intelliSenseMode": "windows-msvc-x64",
            "configurationProvider": "ms-vscode.cmake-tools"
        }}
    ],
    "version": 4
}}
"#,
        include_paths
            .iter()
            .map(|p| format!("\"{}\"", p))
            .collect::<Vec<_>>()
            .join(",\n                ")
    );
    fs::write(vscode_dir.join("c_cpp_properties.json"), cpp_properties)?;

    Ok(())
}

/// 生成构建脚本
fn generate_build_scripts(project_dir: &Path) -> Result<()> {
    // Windows 批处理脚本
    let build_bat = r#"@echo off
setlocal

REM ============================================================================
REM Limx Engine 构建脚本
REM ============================================================================

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=development

echo.
echo ========================================
echo   Limx Engine Build Script
echo   Configuration: %CONFIG%
echo ========================================
echo.

REM 检查 LBT
if not exist "Programs\target\release\lbt.exe" (
    echo [ERROR] LBT not found. Building tools first...
    cd Programs
    cargo build --release
    cd ..
)

REM 生成项目
echo [1/4] Generating project files...
Programs\target\release\lbt.exe generate --source-dir Source --output-dir Generated
if errorlevel 1 goto :error

REM 生成反射代码
echo [2/4] Generating reflection code...
Programs\target\release\lht.exe generate --source-dir Source --output-dir Intermediate\Generated
if errorlevel 1 goto :error

REM 配置 CMake
echo [3/4] Configuring CMake...
cmake --preset %CONFIG%
if errorlevel 1 goto :error

REM 构建
echo [4/4] Building...
cmake --build --preset %CONFIG%
if errorlevel 1 goto :error

echo.
echo ========================================
echo   Build Successful!
echo   Output: Binaries\Win64\%CONFIG%
echo ========================================
goto :eof

:error
echo.
echo [ERROR] Build failed!
exit /b 1
"#;
    fs::write(project_dir.join("Build.bat"), build_bat)?;

    // PowerShell 脚本
    let build_ps1 = r#"#===============================================================================
# Limx Engine 构建脚本 (PowerShell)
#===============================================================================

param(
    [ValidateSet("debug", "development", "release")]
    [string]$Config = "development",
    
    [switch]$Clean,
    [switch]$GenerateOnly,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Limx Engine Build Script" -ForegroundColor Cyan
Write-Host "  Configuration: $Config" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# 清理
if ($Clean) {
    Write-Host "[Clean] Removing build artifacts..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "Intermediate"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "Binaries"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "Generated"
}

# 检查 LBT
$lbtPath = "Programs\target\release\lbt.exe"
if (-not (Test-Path $lbtPath)) {
    Write-Host "[Tools] Building LBT/LHT..." -ForegroundColor Yellow
    Push-Location Programs
    cargo build --release
    Pop-Location
}

# 生成项目文件
Write-Host "[1/4] Generating project files..." -ForegroundColor Green
& $lbtPath generate --source-dir Source --output-dir Generated
if ($LASTEXITCODE -ne 0) { throw "LBT failed" }

# 生成反射代码
Write-Host "[2/4] Generating reflection code..." -ForegroundColor Green
& "Programs\target\release\lht.exe" generate --source-dir Source --output-dir "Intermediate\Generated"
if ($LASTEXITCODE -ne 0) { throw "LHT failed" }

if ($GenerateOnly) {
    Write-Host "Generate only mode - stopping here." -ForegroundColor Yellow
    exit 0
}

# 配置 CMake
Write-Host "[3/4] Configuring CMake ($Config)..." -ForegroundColor Green
cmake --preset $Config
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

if ($NoBuild) {
    Write-Host "No build mode - stopping here." -ForegroundColor Yellow
    exit 0
}

# 构建
Write-Host "[4/4] Building..." -ForegroundColor Green
cmake --build --preset $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Build Successful!" -ForegroundColor Green
Write-Host "  Output: Binaries\Win64\$Config" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
"#;
    fs::write(project_dir.join("Build.ps1"), build_ps1)?;

    Ok(())
}
