/*******************************************************************************
 * 文件: specifiers.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   属性说明符解析器
 *   支持 UE 风格的说明符语法:
 *   - 简单标记: Editable, Serializable
 *   - 键值对: Category="Transform", ClampMin=0.0
 *   - 元数据: meta=(DisplayName="位置", ToolTip="...")
 *
 ******************************************************************************/

use std::collections::HashMap;

/// 说明符值类型
#[derive(Debug, Clone, PartialEq)]
pub enum SpecifierValue {
    /// 布尔标记 (仅存在即为 true)
    Flag,
    /// 字符串值
    String(String),
    /// 数值
    Number(f64),
    /// 嵌套说明符 (用于 meta=(...))
    Nested(HashMap<String, SpecifierValue>),
}

/// 解析后的说明符集合
#[derive(Debug, Clone, Default)]
pub struct Specifiers {
    pub items: HashMap<String, SpecifierValue>,
}

impl Specifiers {
    pub fn new() -> Self {
        Self {
            items: HashMap::new(),
        }
    }

    /// 检查是否包含某个标记
    pub fn has_flag(&self, name: &str) -> bool {
        self.items.contains_key(name)
    }

    /// 获取字符串值
    pub fn get_string(&self, name: &str) -> Option<&str> {
        match self.items.get(name) {
            Some(SpecifierValue::String(s)) => Some(s),
            _ => None,
        }
    }

    /// 获取数值
    pub fn get_number(&self, name: &str) -> Option<f64> {
        match self.items.get(name) {
            Some(SpecifierValue::Number(n)) => Some(*n),
            _ => None,
        }
    }

    /// 获取嵌套说明符
    pub fn get_nested(&self, name: &str) -> Option<&HashMap<String, SpecifierValue>> {
        match self.items.get(name) {
            Some(SpecifierValue::Nested(map)) => Some(map),
            _ => None,
        }
    }

    /// 获取 meta 中的值
    pub fn get_meta(&self, key: &str) -> Option<&SpecifierValue> {
        self.get_nested("meta").and_then(|m| m.get(key))
    }

    /// 获取 meta 中的字符串值
    pub fn get_meta_string(&self, key: &str) -> Option<&str> {
        match self.get_meta(key) {
            Some(SpecifierValue::String(s)) => Some(s),
            _ => None,
        }
    }
}

/// 解析说明符字符串
/// 输入: "Editable, Serializable, Category=\"Transform\", ClampMin=0.0, meta=(DisplayName=\"位置\")"
pub fn parse_specifiers(input: &str) -> Specifiers {
    let mut specs = Specifiers::new();
    let input = input.trim();

    if input.is_empty() {
        return specs;
    }

    // 分割顶层说明符 (注意处理嵌套括号和引号)
    let tokens = tokenize_specifiers(input);

    for token in tokens {
        let token = token.trim();
        if token.is_empty() {
            continue;
        }

        // 检查是否是键值对
        if let Some(eq_pos) = find_equals(token) {
            let key = token[..eq_pos].trim().to_string();
            let value_str = token[eq_pos + 1..].trim();

            let value = parse_value(value_str);
            specs.items.insert(key, value);
        } else {
            // 简单标记
            specs.items.insert(token.to_string(), SpecifierValue::Flag);
        }
    }

    specs
}

/// 分割说明符 (处理嵌套括号)
fn tokenize_specifiers(input: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut paren_depth = 0;
    let mut in_quotes = false;
    let mut escape_next = false;

    for ch in input.chars() {
        if escape_next {
            current.push(ch);
            escape_next = false;
            continue;
        }

        match ch {
            '\\' => {
                escape_next = true;
                current.push(ch);
            }
            '"' => {
                in_quotes = !in_quotes;
                current.push(ch);
            }
            '(' if !in_quotes => {
                paren_depth += 1;
                current.push(ch);
            }
            ')' if !in_quotes => {
                paren_depth -= 1;
                current.push(ch);
            }
            ',' if !in_quotes && paren_depth == 0 => {
                if !current.trim().is_empty() {
                    tokens.push(current.trim().to_string());
                }
                current.clear();
            }
            _ => {
                current.push(ch);
            }
        }
    }

    if !current.trim().is_empty() {
        tokens.push(current.trim().to_string());
    }

    tokens
}

/// 查找等号位置 (忽略引号内的等号)
fn find_equals(input: &str) -> Option<usize> {
    let mut in_quotes = false;
    let mut paren_depth = 0;

    for (i, ch) in input.chars().enumerate() {
        match ch {
            '"' => in_quotes = !in_quotes,
            '(' if !in_quotes => paren_depth += 1,
            ')' if !in_quotes => paren_depth -= 1,
            '=' if !in_quotes && paren_depth == 0 => return Some(i),
            _ => {}
        }
    }

    None
}

/// 解析值
fn parse_value(value_str: &str) -> SpecifierValue {
    let value_str = value_str.trim();

    // 字符串值 (带引号)
    if value_str.starts_with('"') && value_str.ends_with('"') {
        let inner = &value_str[1..value_str.len() - 1];
        return SpecifierValue::String(inner.to_string());
    }

    // 嵌套说明符 (括号)
    if value_str.starts_with('(') && value_str.ends_with(')') {
        let inner = &value_str[1..value_str.len() - 1];
        let nested = parse_specifiers(inner);
        return SpecifierValue::Nested(nested.items);
    }

    // 尝试解析为数值
    if let Ok(num) = value_str.parse::<f64>() {
        return SpecifierValue::Number(num);
    }

    // 作为字符串处理
    SpecifierValue::String(value_str.to_string())
}

//=============================================================================
// 预定义说明符类型
//=============================================================================

/// 类说明符
#[derive(Debug, Clone, Default)]
pub struct ClassSpecifiers {
    // === 基础属性 ===
    /// 可序列化
    pub serializable: bool,
    /// 可反射
    pub reflectable: bool,
    /// 抽象类
    pub abstract_class: bool,
    /// 不可复制
    pub non_copyable: bool,
    /// 自定义构造函数
    pub custom_constructor: bool,

    // === 类型特性 ===
    /// 蓝图类型 (可用于蓝图)
    pub blueprint_type: bool,
    /// 可继承
    pub blueprintable: bool,
    /// 可实例化 (Actor/Object)
    pub placeable: bool,
    /// 不可放置
    pub not_placeable: bool,
    /// 组件类
    pub component_class: bool,

    // === 编辑器集成 ===
    /// 类组 (用于分类)
    pub class_group: Option<String>,
    /// 显示名称
    pub display_name: Option<String>,
    /// 工具提示
    pub tool_tip: Option<String>,
    /// 隐藏分类
    pub hide_categories: Vec<String>,
    /// 显示分类
    pub show_categories: Vec<String>,
    /// 自动展开分类
    pub auto_expand_categories: Vec<String>,

    // === 配置类 ===
    /// 配置文件名
    pub config_name: Option<String>,
    /// 默认配置
    pub default_config: Option<String>,
    /// 按实例配置
    pub per_object_config: bool,

    // === 内存管理 ===
    /// 内存池类型
    pub memory_pool: Option<String>,
    /// GC 标记
    pub gc_flags: Vec<String>,

    // === 开发特性 ===
    /// 开发中特性 (只在开发构建中启用)
    pub development_only: bool,
    /// 已废弃
    pub deprecated: bool,
    /// 废弃消息
    pub deprecation_message: Option<String>,

    // =========================================================================
    // Limx 独有特性 (UE 没有)
    // =========================================================================

    // === 线程安全 ===
    /// 线程安全类
    pub thread_safe: bool,
    /// 仅主线程
    pub main_thread_only: bool,
    /// 任意线程可访问
    pub any_thread: bool,
    /// 线程亲和性 (绑定到特定线程)
    pub thread_affinity: Option<String>,

    // === 性能优化 ===
    /// 对象池类型
    pub object_pool: bool,
    /// 池初始大小
    pub pool_size: Option<u32>,
    /// 池最大大小
    pub pool_max_size: Option<u32>,
    /// 缓存行对齐
    pub cache_line_aligned: bool,
    /// 内存对齐字节数
    pub alignment: Option<u32>,
    /// SIMD 优化
    pub simd_optimized: bool,

    // === 生命周期 ===
    /// 单例模式
    pub singleton: bool,
    /// 延迟初始化
    pub lazy_init: bool,
    /// 自动注册
    pub auto_register: bool,
    /// 子系统类型
    pub subsystem: bool,
    /// 生命周期优先级
    pub init_priority: Option<i32>,

    // === 序列化高级 ===
    /// 二进制序列化
    pub binary_serialize: bool,
    /// JSON 序列化
    pub json_serialize: bool,
    /// XML 序列化
    pub xml_serialize: bool,
    /// 自定义序列化器
    pub custom_serializer: Option<String>,
    /// 序列化版本
    pub serialize_version: Option<u32>,
    /// 向后兼容版本
    pub min_compatible_version: Option<u32>,

    // === 调试与分析 ===
    /// 启用内存跟踪
    pub memory_tracking: bool,
    /// 启用性能分析
    pub profiling: bool,
    /// 自定义分析组
    pub profiling_group: Option<String>,
    /// 日志类别
    pub log_category: Option<String>,

    // === 模块系统 ===
    /// 模块依赖
    pub module_dependency: Vec<String>,
    /// 导出 API
    pub api_export: bool,
    /// 插件类型
    pub plugin_type: Option<String>,

    // === 热重载 ===
    /// 支持热重载
    pub hot_reloadable: bool,
    /// 热重载回调
    pub hot_reload_callback: Option<String>,

    // === 资源管理 ===
    /// 资源类型
    pub asset_type: bool,
    /// 资源扩展名
    pub asset_extension: Option<String>,
    /// 资源工厂
    pub asset_factory: Option<String>,
    /// 异步加载
    pub async_loadable: bool,
    /// 流式加载
    pub streamable: bool,
}

impl ClassSpecifiers {
    pub fn from_specifiers(specs: &Specifiers) -> Self {
        Self {
            // 基础属性
            serializable: specs.has_flag("Serializable"),
            reflectable: specs.has_flag("Reflectable") || specs.has_flag("Serializable"),
            abstract_class: specs.has_flag("Abstract"),
            non_copyable: specs.has_flag("NonCopyable"),
            custom_constructor: specs.has_flag("CustomConstructor"),

            // 类型特性
            blueprint_type: specs.has_flag("BlueprintType"),
            blueprintable: specs.has_flag("Blueprintable"),
            placeable: specs.has_flag("Placeable"),
            not_placeable: specs.has_flag("NotPlaceable"),
            component_class: specs.has_flag("ComponentClass"),

            // 编辑器集成
            class_group: specs.get_string("ClassGroup").map(|s| s.to_string()),
            display_name: specs.get_meta_string("DisplayName").map(|s| s.to_string()),
            tool_tip: specs.get_meta_string("ToolTip").map(|s| s.to_string()),
            hide_categories: parse_string_list(specs.get_string("HideCategories")),
            show_categories: parse_string_list(specs.get_string("ShowCategories")),
            auto_expand_categories: parse_string_list(specs.get_string("AutoExpandCategories")),

            // 配置类
            config_name: specs.get_string("Config").map(|s| s.to_string()),
            default_config: specs.get_string("DefaultConfig").map(|s| s.to_string()),
            per_object_config: specs.has_flag("PerObjectConfig"),

            // 内存管理
            memory_pool: specs.get_string("MemoryPool").map(|s| s.to_string()),
            gc_flags: parse_string_list(specs.get_string("GCFlags")),

            // 开发特性
            development_only: specs.has_flag("DevelopmentOnly"),
            deprecated: specs.has_flag("Deprecated"),
            deprecation_message: specs
                .get_meta_string("DeprecationMessage")
                .map(|s| s.to_string()),

            // === Limx 独有特性 ===

            // 线程安全
            thread_safe: specs.has_flag("ThreadSafe"),
            main_thread_only: specs.has_flag("MainThreadOnly") || specs.has_flag("GameThreadOnly"),
            any_thread: specs.has_flag("AnyThread"),
            thread_affinity: specs.get_string("ThreadAffinity").map(|s| s.to_string()),

            // 性能优化
            object_pool: specs.has_flag("ObjectPool") || specs.has_flag("Pooled"),
            pool_size: specs.get_number("PoolSize").map(|n| n as u32),
            pool_max_size: specs.get_number("PoolMaxSize").map(|n| n as u32),
            cache_line_aligned: specs.has_flag("CacheLineAligned"),
            alignment: specs.get_number("Alignment").map(|n| n as u32),
            simd_optimized: specs.has_flag("SIMDOptimized") || specs.has_flag("Vectorized"),

            // 生命周期
            singleton: specs.has_flag("Singleton"),
            lazy_init: specs.has_flag("LazyInit") || specs.has_flag("DeferredInit"),
            auto_register: specs.has_flag("AutoRegister"),
            subsystem: specs.has_flag("Subsystem"),
            init_priority: specs.get_number("InitPriority").map(|n| n as i32),

            // 序列化高级
            binary_serialize: specs.has_flag("BinarySerialize"),
            json_serialize: specs.has_flag("JSONSerialize") || specs.has_flag("JsonSerialize"),
            xml_serialize: specs.has_flag("XMLSerialize") || specs.has_flag("XmlSerialize"),
            custom_serializer: specs.get_string("CustomSerializer").map(|s| s.to_string()),
            serialize_version: specs.get_number("SerializeVersion").map(|n| n as u32),
            min_compatible_version: specs.get_number("MinCompatibleVersion").map(|n| n as u32),

            // 调试与分析
            memory_tracking: specs.has_flag("MemoryTracking"),
            profiling: specs.has_flag("Profiling") || specs.has_flag("Profile"),
            profiling_group: specs.get_string("ProfilingGroup").map(|s| s.to_string()),
            log_category: specs.get_string("LogCategory").map(|s| s.to_string()),

            // 模块系统
            module_dependency: parse_string_list(specs.get_string("ModuleDependency")),
            api_export: specs.has_flag("APIExport") || specs.has_flag("DllExport"),
            plugin_type: specs.get_string("PluginType").map(|s| s.to_string()),

            // 热重载
            hot_reloadable: specs.has_flag("HotReloadable") || specs.has_flag("LiveReload"),
            hot_reload_callback: specs.get_string("HotReloadCallback").map(|s| s.to_string()),

            // 资源管理
            asset_type: specs.has_flag("AssetType") || specs.has_flag("Asset"),
            asset_extension: specs.get_string("AssetExtension").map(|s| s.to_string()),
            asset_factory: specs.get_string("AssetFactory").map(|s| s.to_string()),
            async_loadable: specs.has_flag("AsyncLoadable") || specs.has_flag("AsyncLoad"),
            streamable: specs.has_flag("Streamable"),
        }
    }
}

/// 解析字符串列表 ("A,B,C" -> vec!["A", "B", "C"])
fn parse_string_list(input: Option<&str>) -> Vec<String> {
    input
        .map(|s| s.split(',').map(|p| p.trim().to_string()).collect())
        .unwrap_or_default()
}

/// 属性说明符
#[derive(Debug, Clone, Default)]
pub struct PropertySpecifiers {
    // === 编辑器可见性 ===
    /// 可编辑 (编辑器)
    pub editable: bool,
    /// 仅可见 (编辑器只读)
    pub visible_only: bool,
    /// 仅在实例上编辑
    pub edit_instance_only: bool,
    /// 仅在默认值上编辑
    pub edit_defaults_only: bool,
    /// 高级显示 (需展开高级选项)
    pub advanced_display: bool,

    // === 序列化 ===
    /// 可序列化
    pub serializable: bool,
    /// 临时 (不序列化)
    pub transient: bool,
    /// 保存游戏
    pub save_game: bool,
    /// 复制时跳过
    pub skip_serialization: bool,

    // === 网络复制 ===
    /// 网络复制
    pub replicated: bool,
    /// 复制通知函数
    pub replicated_using: Option<String>,
    /// 复制条件
    pub replicate_condition: Option<String>,
    /// 仅初始复制
    pub initial_only: bool,
    /// 仅所有者复制
    pub owner_only: bool,

    // === 蓝图集成 ===
    /// 蓝图可读
    pub blueprint_read: bool,
    /// 蓝图可写
    pub blueprint_write: bool,
    /// 蓝图可赋值 (委托)
    pub blueprint_assignable: bool,
    /// 蓝图可调用 (委托)
    pub blueprint_callable: bool,

    // === 编辑器元数据 ===
    /// 分类
    pub category: Option<String>,
    /// 显示名称
    pub display_name: Option<String>,
    /// 工具提示
    pub tool_tip: Option<String>,
    /// 数值最小值约束
    pub clamp_min: Option<f64>,
    /// 数值最大值约束
    pub clamp_max: Option<f64>,
    /// UI 最小值
    pub ui_min: Option<f64>,
    /// UI 最大值
    pub ui_max: Option<f64>,
    /// 滑动条步长
    pub slider_exponent: Option<f64>,
    /// 增量值 (箭头调整)
    pub delta: Option<f64>,
    /// 单位
    pub units: Option<String>,
    /// 多行文本
    pub multiline: bool,
    /// 密码字段
    pub password_field: bool,

    // === 类型特定 ===
    /// 允许的类 (对象引用)
    pub allowed_classes: Vec<String>,
    /// 禁止的类
    pub disallowed_classes: Vec<String>,
    /// 元类 (子类选择器)
    pub meta_class: Option<String>,
    /// 数组最小长度
    pub array_size_min: Option<i32>,
    /// 数组最大长度
    pub array_size_max: Option<i32>,

    // === 条件显示 ===
    /// 编辑条件
    pub edit_condition: Option<String>,
    /// 编辑条件隐藏
    pub edit_condition_hides: bool,
    /// 内联编辑条件切换
    pub inline_edit_condition_toggle: bool,

    // === 开发特性 ===
    /// 已废弃
    pub deprecated: bool,
    /// 废弃消息
    pub deprecation_message: Option<String>,

    // =========================================================================
    // Limx 独有特性 (UE 没有)
    // =========================================================================

    // === 数据绑定 ===
    /// 双向绑定
    pub two_way_binding: bool,
    /// 绑定源属性
    pub bind_to: Option<String>,
    /// 绑定转换器
    pub binding_converter: Option<String>,
    /// 变更通知模式
    pub notify_mode: Option<String>,
    /// 观察者模式
    pub observable: bool,

    // === 验证系统 ===
    /// 验证函数
    pub validator: Option<String>,
    /// 正则验证
    pub regex_pattern: Option<String>,
    /// 非空验证
    pub not_null: bool,
    /// 非空字符串
    pub not_empty: bool,
    /// 自定义错误消息
    pub validation_message: Option<String>,
    /// 验证时机 (OnChange/OnSubmit/Realtime)
    pub validation_trigger: Option<String>,

    // === 计算属性 ===
    /// 计算属性 (只读，自动计算)
    pub computed: bool,
    /// 计算表达式
    pub compute_expression: Option<String>,
    /// 依赖属性列表
    pub depends_on: Vec<String>,
    /// 缓存计算结果
    pub cache_computed: bool,

    // === 历史与撤销 ===
    /// 支持撤销
    pub undoable: bool,
    /// 撤销分组
    pub undo_group: Option<String>,
    /// 合并连续修改
    pub merge_undo: bool,
    /// 历史记录
    pub track_history: bool,
    /// 最大历史数
    pub max_history: Option<u32>,

    // === 动画系统 ===
    /// 可动画属性
    pub animatable: bool,
    /// 动画曲线类型
    pub animation_curve: Option<String>,
    /// 插值函数
    pub interpolator: Option<String>,
    /// 动画速度
    pub animation_speed: Option<f64>,

    // === 本地化 ===
    /// 可本地化
    pub localizable: bool,
    /// 本地化键
    pub localization_key: Option<String>,
    /// 本地化表
    pub localization_table: Option<String>,

    // === 调试 ===
    /// 调试监视
    pub debug_watch: bool,
    /// 断点条件
    pub breakpoint_condition: Option<String>,
    /// 日志变更
    pub log_changes: bool,

    // === 性能 ===
    /// 脏标记优化
    pub dirty_flag: bool,
    /// 延迟更新
    pub deferred_update: bool,
    /// 批量更新
    pub batch_update: bool,
    /// 更新频率限制 (ms)
    pub update_throttle: Option<u32>,
}

impl PropertySpecifiers {
    pub fn from_specifiers(specs: &Specifiers) -> Self {
        Self {
            // 编辑器可见性
            editable: specs.has_flag("Editable") || specs.has_flag("EditAnywhere"),
            visible_only: specs.has_flag("VisibleOnly") || specs.has_flag("VisibleAnywhere"),
            edit_instance_only: specs.has_flag("EditInstanceOnly"),
            edit_defaults_only: specs.has_flag("EditDefaultsOnly"),
            advanced_display: specs.has_flag("AdvancedDisplay"),

            // 序列化
            serializable: specs.has_flag("Serializable") || specs.has_flag("SaveGame"),
            transient: specs.has_flag("Transient"),
            save_game: specs.has_flag("SaveGame"),
            skip_serialization: specs.has_flag("SkipSerialization"),

            // 网络复制
            replicated: specs.has_flag("Replicated"),
            replicated_using: specs.get_string("ReplicatedUsing").map(|s| s.to_string()),
            replicate_condition: specs
                .get_string("ReplicateCondition")
                .map(|s| s.to_string()),
            initial_only: specs.has_flag("InitialOnly"),
            owner_only: specs.has_flag("OwnerOnly"),

            // 蓝图集成
            blueprint_read: specs.has_flag("BlueprintReadOnly")
                || specs.has_flag("BlueprintReadWrite"),
            blueprint_write: specs.has_flag("BlueprintReadWrite"),
            blueprint_assignable: specs.has_flag("BlueprintAssignable"),
            blueprint_callable: specs.has_flag("BlueprintCallable"),

            // 编辑器元数据
            category: specs.get_string("Category").map(|s| s.to_string()),
            display_name: specs.get_meta_string("DisplayName").map(|s| s.to_string()),
            tool_tip: specs.get_meta_string("ToolTip").map(|s| s.to_string()),
            clamp_min: specs.get_number("ClampMin"),
            clamp_max: specs.get_number("ClampMax"),
            ui_min: specs.get_number("UIMin"),
            ui_max: specs.get_number("UIMax"),
            slider_exponent: specs.get_number("SliderExponent"),
            delta: specs.get_number("Delta"),
            units: specs.get_meta_string("Units").map(|s| s.to_string()),
            multiline: specs.has_flag("Multiline"),
            password_field: specs.has_flag("PasswordField"),

            // 类型特定
            allowed_classes: parse_string_list(specs.get_string("AllowedClasses")),
            disallowed_classes: parse_string_list(specs.get_string("DisallowedClasses")),
            meta_class: specs.get_string("MetaClass").map(|s| s.to_string()),
            array_size_min: specs.get_number("ArraySizeMin").map(|n| n as i32),
            array_size_max: specs.get_number("ArraySizeMax").map(|n| n as i32),

            // 条件显示
            edit_condition: specs.get_string("EditCondition").map(|s| s.to_string()),
            edit_condition_hides: specs.has_flag("EditConditionHides"),
            inline_edit_condition_toggle: specs.has_flag("InlineEditConditionToggle"),

            // 开发特性
            deprecated: specs.has_flag("Deprecated"),
            deprecation_message: specs
                .get_meta_string("DeprecationMessage")
                .map(|s| s.to_string()),

            // === Limx 独有特性 ===

            // 数据绑定
            two_way_binding: specs.has_flag("TwoWayBinding") || specs.has_flag("Bidirectional"),
            bind_to: specs.get_string("BindTo").map(|s| s.to_string()),
            binding_converter: specs.get_string("BindingConverter").map(|s| s.to_string()),
            notify_mode: specs.get_string("NotifyMode").map(|s| s.to_string()),
            observable: specs.has_flag("Observable"),

            // 验证系统
            validator: specs.get_string("Validator").map(|s| s.to_string()),
            regex_pattern: specs.get_string("RegexPattern").map(|s| s.to_string()),
            not_null: specs.has_flag("NotNull") || specs.has_flag("Required"),
            not_empty: specs.has_flag("NotEmpty"),
            validation_message: specs
                .get_meta_string("ValidationMessage")
                .map(|s| s.to_string()),
            validation_trigger: specs.get_string("ValidationTrigger").map(|s| s.to_string()),

            // 计算属性
            computed: specs.has_flag("Computed") || specs.has_flag("Calculated"),
            compute_expression: specs.get_string("ComputeExpression").map(|s| s.to_string()),
            depends_on: parse_string_list(specs.get_string("DependsOn")),
            cache_computed: specs.has_flag("CacheComputed"),

            // 历史与撤销
            undoable: specs.has_flag("Undoable") || specs.has_flag("Undo"),
            undo_group: specs.get_string("UndoGroup").map(|s| s.to_string()),
            merge_undo: specs.has_flag("MergeUndo"),
            track_history: specs.has_flag("TrackHistory"),
            max_history: specs.get_number("MaxHistory").map(|n| n as u32),

            // 动画系统
            animatable: specs.has_flag("Animatable") || specs.has_flag("Interp"),
            animation_curve: specs.get_string("AnimationCurve").map(|s| s.to_string()),
            interpolator: specs.get_string("Interpolator").map(|s| s.to_string()),
            animation_speed: specs.get_number("AnimationSpeed"),

            // 本地化
            localizable: specs.has_flag("Localizable") || specs.has_flag("Localized"),
            localization_key: specs.get_string("LocalizationKey").map(|s| s.to_string()),
            localization_table: specs.get_string("LocalizationTable").map(|s| s.to_string()),

            // 调试
            debug_watch: specs.has_flag("DebugWatch") || specs.has_flag("Watch"),
            breakpoint_condition: specs
                .get_string("BreakpointCondition")
                .map(|s| s.to_string()),
            log_changes: specs.has_flag("LogChanges"),

            // 性能
            dirty_flag: specs.has_flag("DirtyFlag"),
            deferred_update: specs.has_flag("DeferredUpdate") || specs.has_flag("Deferred"),
            batch_update: specs.has_flag("BatchUpdate"),
            update_throttle: specs.get_number("UpdateThrottle").map(|n| n as u32),
        }
    }
}

/// 函数说明符
#[derive(Debug, Clone, Default)]
pub struct FunctionSpecifiers {
    // === 基础属性 ===
    /// 可调用 (脚本)
    pub callable: bool,
    /// 纯函数 (无副作用)
    pub pure_func: bool,
    /// 常量函数
    pub const_func: bool,
    /// 静态函数
    pub static_func: bool,

    // === 蓝图集成 ===
    /// 蓝图可调用
    pub blueprint_callable: bool,
    /// 纯函数 (蓝图)
    pub blueprint_pure: bool,
    /// 蓝图可实现事件
    pub blueprint_implementable: bool,
    /// 蓝图原生事件
    pub blueprint_native_event: bool,
    /// 内部函数 (不暴露给蓝图)
    pub blueprint_internal: bool,
    /// 蓝图自动转换
    pub blueprint_auto_cast: bool,

    // === RPC 网络 ===
    /// 服务器 RPC
    pub server: bool,
    /// 客户端 RPC
    pub client: bool,
    /// 组播 RPC
    pub net_multicast: bool,
    /// 可靠 RPC
    pub reliable: bool,
    /// 不可靠 RPC
    pub unreliable: bool,
    /// 带验证
    pub with_validation: bool,
    /// 服务器广播
    pub service_request: bool,
    /// 服务器响应
    pub service_response: bool,

    // === 执行特性 ===
    /// 延迟执行
    pub latent: bool,
    /// 权限级别
    pub authority_only: bool,
    /// 在局域计算
    pub locally_controlled: bool,
    /// 编辑器专用
    pub editor_only: bool,
    /// 开发构建专用
    pub development_only: bool,

    // === 元数据 ===
    /// 分类
    pub category: Option<String>,
    /// 显示名称
    pub display_name: Option<String>,
    /// 工具提示
    pub tool_tip: Option<String>,
    /// 关键字 (搜索用)
    pub keywords: Vec<String>,
    /// 简化名称
    pub compact_node_title: Option<String>,
    /// 自定义图标
    pub custom_icon: Option<String>,

    // === 开发特性 ===
    /// 已废弃
    pub deprecated: bool,
    /// 废弃消息
    pub deprecation_message: Option<String>,
    /// 实验性功能
    pub experimental: bool,

    // =========================================================================
    // Limx 独有特性 (UE 没有)
    // =========================================================================

    // === 异步执行 ===
    /// 异步函数
    pub async_func: bool,
    /// 协程
    pub coroutine: bool,
    /// 任务优先级
    pub task_priority: Option<i32>,
    /// 执行线程
    pub execute_on: Option<String>,
    /// 超时时间 (ms)
    pub timeout: Option<u32>,
    /// 可取消
    pub cancellable: bool,

    // === 缓存与记忆化 ===
    /// 缓存结果
    pub cached: bool,
    /// 缓存过期时间 (s)
    pub cache_ttl: Option<u32>,
    /// 缓存键生成器
    pub cache_key: Option<String>,
    /// 记忆化 (相同参数返回缓存结果)
    pub memoized: bool,

    // === 重试与容错 ===
    /// 自动重试
    pub retry: bool,
    /// 最大重试次数
    pub max_retries: Option<u32>,
    /// 重试延迟 (ms)
    pub retry_delay: Option<u32>,
    /// 指数退避
    pub exponential_backoff: bool,
    /// 失败回调
    pub on_failure: Option<String>,

    // === 事务 ===
    /// 事务函数
    pub transactional: bool,
    /// 事务隔离级别
    pub isolation_level: Option<String>,
    /// 回滚条件
    pub rollback_on: Vec<String>,

    // === 性能监控 ===
    /// 性能分析
    pub profile: bool,
    /// 分析组
    pub profile_group: Option<String>,
    /// 执行时间警告阈值 (ms)
    pub warn_if_slow: Option<u32>,
    /// 调用计数
    pub track_calls: bool,

    // === 访问控制 ===
    /// 权限要求
    pub requires_permission: Vec<String>,
    /// 角色要求
    pub requires_role: Vec<String>,
    /// 速率限制 (次/秒)
    pub rate_limit: Option<u32>,
    /// IP 白名单
    pub ip_whitelist: Vec<String>,

    // === 日志与审计 ===
    /// 记录调用
    pub log_call: bool,
    /// 记录参数
    pub log_params: bool,
    /// 记录返回值
    pub log_result: bool,
    /// 审计追踪
    pub audit: bool,

    // === 测试 ===
    /// 测试函数
    pub test_func: bool,
    /// 性能测试
    pub benchmark: bool,
    /// 模拟返回值
    pub mock_return: Option<String>,
    /// 测试用例数据
    pub test_cases: Vec<String>,
}

impl FunctionSpecifiers {
    pub fn from_specifiers(specs: &Specifiers) -> Self {
        Self {
            // 基础属性
            callable: specs.has_flag("Callable"),
            pure_func: specs.has_flag("Pure"),
            const_func: specs.has_flag("Const"),
            static_func: specs.has_flag("Static"),

            // 蓝图集成
            blueprint_callable: specs.has_flag("BlueprintCallable"),
            blueprint_pure: specs.has_flag("BlueprintPure"),
            blueprint_implementable: specs.has_flag("BlueprintImplementableEvent"),
            blueprint_native_event: specs.has_flag("BlueprintNativeEvent"),
            blueprint_internal: specs.has_flag("BlueprintInternalUseOnly"),
            blueprint_auto_cast: specs.has_flag("BlueprintAutocast"),

            // RPC 网络
            server: specs.has_flag("Server"),
            client: specs.has_flag("Client"),
            net_multicast: specs.has_flag("NetMulticast"),
            reliable: specs.has_flag("Reliable"),
            unreliable: specs.has_flag("Unreliable"),
            with_validation: specs.has_flag("WithValidation"),
            service_request: specs.has_flag("ServiceRequest"),
            service_response: specs.has_flag("ServiceResponse"),

            // 执行特性
            latent: specs.has_flag("Latent"),
            authority_only: specs.has_flag("AuthorityOnly"),
            locally_controlled: specs.has_flag("LocallyControlled"),
            editor_only: specs.has_flag("EditorOnly") || specs.has_flag("CallInEditor"),
            development_only: specs.has_flag("DevelopmentOnly"),

            // 元数据
            category: specs.get_string("Category").map(|s| s.to_string()),
            display_name: specs.get_meta_string("DisplayName").map(|s| s.to_string()),
            tool_tip: specs.get_meta_string("ToolTip").map(|s| s.to_string()),
            keywords: parse_string_list(specs.get_meta_string("Keywords")),
            compact_node_title: specs
                .get_meta_string("CompactNodeTitle")
                .map(|s| s.to_string()),
            custom_icon: specs.get_meta_string("CustomIcon").map(|s| s.to_string()),

            // 开发特性
            deprecated: specs.has_flag("Deprecated"),
            deprecation_message: specs
                .get_meta_string("DeprecationMessage")
                .map(|s| s.to_string()),
            experimental: specs.has_flag("Experimental"),

            // === Limx 独有特性 ===

            // 异步执行
            async_func: specs.has_flag("Async") || specs.has_flag("AsyncFunction"),
            coroutine: specs.has_flag("Coroutine") || specs.has_flag("Coro"),
            task_priority: specs.get_number("TaskPriority").map(|n| n as i32),
            execute_on: specs.get_string("ExecuteOn").map(|s| s.to_string()),
            timeout: specs.get_number("Timeout").map(|n| n as u32),
            cancellable: specs.has_flag("Cancellable") || specs.has_flag("Cancelable"),

            // 缓存与记忆化
            cached: specs.has_flag("Cached") || specs.has_flag("Cache"),
            cache_ttl: specs.get_number("CacheTTL").map(|n| n as u32),
            cache_key: specs.get_string("CacheKey").map(|s| s.to_string()),
            memoized: specs.has_flag("Memoized") || specs.has_flag("Memoize"),

            // 重试与容错
            retry: specs.has_flag("Retry") || specs.has_flag("AutoRetry"),
            max_retries: specs.get_number("MaxRetries").map(|n| n as u32),
            retry_delay: specs.get_number("RetryDelay").map(|n| n as u32),
            exponential_backoff: specs.has_flag("ExponentialBackoff"),
            on_failure: specs.get_string("OnFailure").map(|s| s.to_string()),

            // 事务
            transactional: specs.has_flag("Transactional") || specs.has_flag("Transaction"),
            isolation_level: specs.get_string("IsolationLevel").map(|s| s.to_string()),
            rollback_on: parse_string_list(specs.get_string("RollbackOn")),

            // 性能监控
            profile: specs.has_flag("Profile") || specs.has_flag("Profiled"),
            profile_group: specs.get_string("ProfileGroup").map(|s| s.to_string()),
            warn_if_slow: specs.get_number("WarnIfSlow").map(|n| n as u32),
            track_calls: specs.has_flag("TrackCalls"),

            // 访问控制
            requires_permission: parse_string_list(specs.get_string("RequiresPermission")),
            requires_role: parse_string_list(specs.get_string("RequiresRole")),
            rate_limit: specs.get_number("RateLimit").map(|n| n as u32),
            ip_whitelist: parse_string_list(specs.get_string("IPWhitelist")),

            // 日志与审计
            log_call: specs.has_flag("LogCall"),
            log_params: specs.has_flag("LogParams"),
            log_result: specs.has_flag("LogResult"),
            audit: specs.has_flag("Audit") || specs.has_flag("Audited"),

            // 测试
            test_func: specs.has_flag("Test") || specs.has_flag("TestFunction"),
            benchmark: specs.has_flag("Benchmark") || specs.has_flag("Perf"),
            mock_return: specs.get_string("MockReturn").map(|s| s.to_string()),
            test_cases: parse_string_list(specs.get_string("TestCases")),
        }
    }

    /// 是否是 RPC 函数
    pub fn is_rpc(&self) -> bool {
        self.server || self.client || self.net_multicast
    }

    /// 是否是蓝图事件
    pub fn is_blueprint_event(&self) -> bool {
        self.blueprint_implementable || self.blueprint_native_event
    }

    /// 是否是异步函数
    pub fn is_async(&self) -> bool {
        self.async_func || self.coroutine
    }

    /// 是否需要权限验证
    pub fn requires_auth(&self) -> bool {
        !self.requires_permission.is_empty() || !self.requires_role.is_empty()
    }
}

/// 委托说明符
#[derive(Debug, Clone, Default)]
pub struct DelegateSpecifiers {
    /// 多播委托
    pub multicast: bool,
    /// 动态委托
    pub dynamic: bool,
    /// 稀疏委托
    pub sparse: bool,
    /// 蓝图可分配
    pub blueprint_assignable: bool,
}

impl DelegateSpecifiers {
    pub fn from_specifiers(specs: &Specifiers) -> Self {
        Self {
            multicast: specs.has_flag("Multicast"),
            dynamic: specs.has_flag("Dynamic"),
            sparse: specs.has_flag("Sparse"),
            blueprint_assignable: specs.has_flag("BlueprintAssignable"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_simple_flags() {
        let specs = parse_specifiers("Editable, Serializable");
        assert!(specs.has_flag("Editable"));
        assert!(specs.has_flag("Serializable"));
        assert!(!specs.has_flag("NotExist"));
    }

    #[test]
    fn test_parse_key_value() {
        let specs = parse_specifiers("Category=\"Transform\", ClampMin=0.0, ClampMax=100.0");
        assert_eq!(specs.get_string("Category"), Some("Transform"));
        assert_eq!(specs.get_number("ClampMin"), Some(0.0));
        assert_eq!(specs.get_number("ClampMax"), Some(100.0));
    }

    #[test]
    fn test_parse_meta() {
        let specs = parse_specifiers("Editable, meta=(DisplayName=\"位置\", ToolTip=\"对象位置\")");
        assert!(specs.has_flag("Editable"));
        assert_eq!(specs.get_meta_string("DisplayName"), Some("位置"));
        assert_eq!(specs.get_meta_string("ToolTip"), Some("对象位置"));
    }
}
