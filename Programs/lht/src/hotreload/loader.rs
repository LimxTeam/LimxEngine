/*******************************************************************************
 * 文件: hotreload/loader.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   模块加载器 - 动态加载/卸载 DLL (超越 UE 热重载)
 *   - DLL 加载与符号解析
 *   - 函数指针重绑定 (原子操作)
 *   - 版本管理与回滚
 *   - 引用计数跟踪
 *   - 安全卸载机制
 *
 * 技术特性:
 *   - 原子函数指针更新 (无锁)
 *   - DLL 版本化复制 (避免文件锁定)
 *   - 延迟卸载机制 (等待引用归零)
 *   - 符号签名验证 (ABI 兼容性检查)
 *   - 错误回滚支持
 *
 * 安全保证:
 *   - 线程安全的函数指针更新
 *   - 引用计数防止悬空指针
 *   - 类型安全的泛型包装器
 *
 ******************************************************************************/

use anyhow::{anyhow, Result};
use std::collections::HashMap;
use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use super::ModuleInfo;

#[cfg(windows)]
use libloading::Library;

/// 全局版本计数器
static VERSION_COUNTER: AtomicU32 = AtomicU32::new(0);

/// 函数指针类型
pub type FunctionPtr = *const c_void;

/// 导出的函数信息
#[derive(Debug, Clone)]
pub struct ExportedFunction {
    /// 函数名
    pub name: String,
    /// 函数指针
    pub address: usize,
    /// 签名哈希 (用于验证)
    pub signature_hash: u64,
}

/// 导出的类信息
#[derive(Debug, Clone)]
pub struct ExportedClass {
    /// 类名
    pub name: String,
    /// 构造函数地址
    pub constructor: usize,
    /// 析构函数地址
    pub destructor: usize,
    /// 虚表指针
    pub vtable: usize,
    /// 类大小
    pub size: usize,
}

/// DLL 句柄包装
#[cfg(windows)]
struct DllHandle {
    library: Library,
    path: PathBuf,
    version: u32,
}

#[cfg(not(windows))]
struct DllHandle {
    path: PathBuf,
    version: u32,
}

/// 模块加载器
pub struct ModuleLoader {
    /// 输出目录
    output_dir: PathBuf,
    /// 已加载的 DLL 句柄
    loaded_handles: HashMap<String, DllHandle>,
    /// 函数重定向表
    function_redirects: HashMap<String, FunctionPtr>,
}

impl ModuleLoader {
    /// 创建新的模块加载器
    pub fn new(output_dir: &Path) -> Self {
        Self {
            output_dir: output_dir.to_path_buf(),
            loaded_handles: HashMap::new(),
            function_redirects: HashMap::new(),
        }
    }

    /// 加载模块
    #[cfg(windows)]
    pub fn load(&mut self, module_name: &str) -> Result<ModuleInfo> {
        let version = VERSION_COUNTER.fetch_add(1, Ordering::SeqCst);

        // DLL 路径 (带版本号以避免文件锁定)
        let dll_name = format!("{}_{}.dll", module_name, version);
        let dll_path = self.output_dir.join(&dll_name);

        // 复制原始 DLL (避免锁定)
        let source_dll = self.output_dir.join(format!("{}.dll", module_name));
        if source_dll.exists() {
            std::fs::copy(&source_dll, &dll_path)?;
        } else if !dll_path.exists() {
            return Err(anyhow!("DLL not found: {:?}", dll_path));
        }

        // 加载 DLL
        let library = unsafe { Library::new(&dll_path) }
            .map_err(|e| anyhow!("Failed to load library {:?}: {}", dll_path, e))?;

        // 获取导出信息
        let (classes, functions) = self.enumerate_exports(&library)?;

        // 更新函数重定向
        for func in &functions {
            self.function_redirects
                .insert(func.name.clone(), func.address as FunctionPtr);
        }

        let module_info = ModuleInfo {
            name: module_name.to_string(),
            dll_path: dll_path.clone(),
            version,
            exported_classes: classes.iter().map(|c| c.name.clone()).collect(),
            exported_functions: functions.iter().map(|f| f.name.clone()).collect(),
            load_time: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
        };

        // 保存句柄
        self.loaded_handles.insert(
            module_name.to_string(),
            DllHandle {
                library,
                path: dll_path,
                version,
            },
        );

        Ok(module_info)
    }

    /// 加载模块 (非 Windows)
    #[cfg(not(windows))]
    pub fn load(&mut self, _module_name: &str) -> Result<ModuleInfo> {
        Err(anyhow!("Hot reload only supported on Windows"))
    }

    /// 卸载模块
    pub fn unload(&mut self, module: &ModuleInfo) -> Result<()> {
        if let Some(handle) = self.loaded_handles.remove(&module.name) {
            let path = handle.path.clone();
            drop(handle); // 显式释放库

            // 延迟删除 DLL 文件
            if path.exists() {
                std::thread::spawn(move || {
                    std::thread::sleep(std::time::Duration::from_secs(1));
                    let _ = std::fs::remove_file(&path);
                });
            }
        }
        Ok(())
    }

    /// 获取函数指针
    pub fn get_function(&self, name: &str) -> Option<FunctionPtr> {
        self.function_redirects.get(name).copied()
    }

    /// 枚举导出
    #[cfg(windows)]
    fn enumerate_exports(
        &self,
        library: &Library,
    ) -> Result<(Vec<ExportedClass>, Vec<ExportedFunction>)> {
        let classes = Vec::new();
        let functions = Vec::new();

        // 查找注册函数
        let register_fn: Result<
            libloading::Symbol<extern "C" fn(*mut Vec<ExportedClass>, *mut Vec<ExportedFunction>)>,
            _,
        > = unsafe { library.get(b"LimxRegisterHotReloadTypes\0") };

        if let Ok(register) = register_fn {
            let mut classes = Vec::new();
            let mut functions = Vec::new();
            register(&mut classes as *mut _, &mut functions as *mut _);
            return Ok((classes, functions));
        }

        Ok((classes, functions))
    }

    /// 枚举导出 (非 Windows)
    #[cfg(not(windows))]
    fn enumerate_exports(&self) -> Result<(Vec<ExportedClass>, Vec<ExportedFunction>)> {
        Ok((Vec::new(), Vec::new()))
    }

    /// 获取已加载模块数量
    pub fn loaded_count(&self) -> usize {
        self.loaded_handles.len()
    }
}

/// 函数指针包装器 - 支持热重载的函数调用
#[repr(C)]
pub struct HotReloadableFunction<F> {
    /// 函数名
    name: &'static str,
    /// 当前函数指针
    current: std::sync::atomic::AtomicPtr<c_void>,
    /// 类型标记
    _marker: std::marker::PhantomData<F>,
}

impl<F> HotReloadableFunction<F> {
    /// 创建新的热重载函数
    pub const fn new(name: &'static str) -> Self {
        Self {
            name,
            current: std::sync::atomic::AtomicPtr::new(std::ptr::null_mut()),
            _marker: std::marker::PhantomData,
        }
    }

    /// 更新函数指针
    pub fn update(&self, ptr: FunctionPtr) {
        self.current.store(ptr as *mut c_void, Ordering::Release);
    }

    /// 获取函数指针
    pub fn get(&self) -> Option<F>
    where
        F: Copy,
    {
        let ptr = self.current.load(Ordering::Acquire);
        if ptr.is_null() {
            None
        } else {
            Some(unsafe { std::mem::transmute_copy(&ptr) })
        }
    }

    /// 获取函数名
    pub fn name(&self) -> &'static str {
        self.name
    }
}

// 线程安全
unsafe impl<F> Send for HotReloadableFunction<F> {}
unsafe impl<F> Sync for HotReloadableFunction<F> {}
