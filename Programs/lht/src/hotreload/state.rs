/*******************************************************************************
 * 文件: hotreload/state.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   状态管理器 - 热重载时保存/恢复对象状态
 *   - 对象序列化
 *   - 状态快照
 *   - 引用修复
 *
 ******************************************************************************/

use anyhow::Result;
use std::collections::HashMap;
use std::sync::{Arc, RwLock};

/// 对象 ID
pub type ObjectId = u64;

/// 序列化的属性值
#[derive(Debug, Clone)]
pub enum SerializedValue {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(String),
    Bytes(Vec<u8>),
    Array(Vec<SerializedValue>),
    Object(HashMap<String, SerializedValue>),
    Reference(ObjectId),
}

/// 对象状态快照
#[derive(Debug, Clone)]
pub struct ObjectSnapshot {
    /// 对象 ID
    pub id: ObjectId,
    /// 类名
    pub class_name: String,
    /// 属性值
    pub properties: HashMap<String, SerializedValue>,
    /// 外部引用
    pub references: Vec<ObjectId>,
}

/// 状态快照
#[derive(Debug, Default)]
pub struct StateSnapshot {
    /// 对象快照
    pub objects: HashMap<ObjectId, ObjectSnapshot>,
    /// 根对象
    pub roots: Vec<ObjectId>,
    /// 快照时间
    pub timestamp: u64,
    /// 版本
    pub version: u32,
}

/// 对象注册表 - 跟踪所有热重载对象
pub struct ObjectRegistry {
    /// 对象 -> 元数据
    objects: HashMap<ObjectId, ObjectMetadata>,
    /// 下一个 ID
    next_id: ObjectId,
}

/// 对象元数据
struct ObjectMetadata {
    /// 类名
    class_name: String,
    /// 原始指针
    ptr: usize,
    /// 是否是根对象
    is_root: bool,
}

impl ObjectRegistry {
    pub fn new() -> Self {
        Self {
            objects: HashMap::new(),
            next_id: 1,
        }
    }

    /// 注册对象
    pub fn register(&mut self, class_name: &str, ptr: usize, is_root: bool) -> ObjectId {
        let id = self.next_id;
        self.next_id += 1;

        self.objects.insert(
            id,
            ObjectMetadata {
                class_name: class_name.to_string(),
                ptr,
                is_root,
            },
        );

        id
    }

    /// 注销对象
    pub fn unregister(&mut self, id: ObjectId) {
        self.objects.remove(&id);
    }

    /// 获取对象指针
    pub fn get_ptr(&self, id: ObjectId) -> Option<usize> {
        self.objects.get(&id).map(|m| m.ptr)
    }

    /// 更新对象指针
    pub fn update_ptr(&mut self, id: ObjectId, new_ptr: usize) {
        if let Some(meta) = self.objects.get_mut(&id) {
            meta.ptr = new_ptr;
        }
    }

    /// 获取所有根对象
    pub fn get_roots(&self) -> Vec<ObjectId> {
        self.objects
            .iter()
            .filter(|(_, m)| m.is_root)
            .map(|(id, _)| *id)
            .collect()
    }

    /// 获取类名
    pub fn get_class_name(&self, id: ObjectId) -> Option<&str> {
        self.objects.get(&id).map(|m| m.class_name.as_str())
    }
}

impl Default for ObjectRegistry {
    fn default() -> Self {
        Self::new()
    }
}

/// 序列化接口
pub trait HotReloadSerializable {
    /// 序列化对象状态
    fn serialize_state(&self) -> HashMap<String, SerializedValue>;

    /// 反序列化对象状态
    fn deserialize_state(&mut self, state: &HashMap<String, SerializedValue>);

    /// 获取类名
    fn class_name(&self) -> &'static str;
}

/// 状态管理器
pub struct StateManager {
    /// 对象注册表
    registry: Arc<RwLock<ObjectRegistry>>,
    /// 当前快照
    current_snapshot: Option<StateSnapshot>,
    /// 序列化器
    serializers: HashMap<String, Box<dyn ObjectSerializer>>,
}

/// 对象序列化器 trait
pub trait ObjectSerializer: Send + Sync {
    /// 序列化
    fn serialize(&self, ptr: usize) -> Result<HashMap<String, SerializedValue>>;
    /// 反序列化
    fn deserialize(&self, ptr: usize, state: &HashMap<String, SerializedValue>) -> Result<()>;
    /// 创建新实例
    fn create_instance(&self) -> Result<usize>;
    /// 销毁实例
    fn destroy_instance(&self, ptr: usize) -> Result<()>;
}

impl StateManager {
    /// 创建新的状态管理器
    pub fn new() -> Self {
        Self {
            registry: Arc::new(RwLock::new(ObjectRegistry::new())),
            current_snapshot: None,
            serializers: HashMap::new(),
        }
    }

    /// 注册序列化器
    pub fn register_serializer(&mut self, class_name: &str, serializer: Box<dyn ObjectSerializer>) {
        self.serializers.insert(class_name.to_string(), serializer);
    }

    /// 获取注册表
    pub fn registry(&self) -> Arc<RwLock<ObjectRegistry>> {
        self.registry.clone()
    }

    /// 保存所有对象状态
    pub fn save_all(&mut self) -> Result<usize> {
        let Ok(registry) = self.registry.read() else {
            return Ok(0);
        };
        let mut snapshot = StateSnapshot {
            objects: HashMap::new(),
            roots: registry.get_roots(),
            timestamp: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
            version: 1,
        };

        let mut count = 0;

        for (id, _) in registry.objects.iter() {
            if let Some(class_name) = registry.get_class_name(*id) {
                if let Some(serializer) = self.serializers.get(class_name) {
                    if let Some(ptr) = registry.get_ptr(*id) {
                        if let Ok(properties) = serializer.serialize(ptr) {
                            // 收集引用 - 扫描属性中的对象引用
                            let references = Self::collect_references(&properties);
                            snapshot.objects.insert(
                                *id,
                                ObjectSnapshot {
                                    id: *id,
                                    class_name: class_name.to_string(),
                                    properties,
                                    references,
                                },
                            );
                            count += 1;
                        }
                    }
                }
            }
        }

        self.current_snapshot = Some(snapshot);
        Ok(count)
    }

    /// 恢复所有对象状态
    pub fn restore_all(&mut self) -> Result<usize> {
        let snapshot = match &self.current_snapshot {
            Some(s) => s,
            None => return Ok(0),
        };

        let Ok(mut registry) = self.registry.write() else {
            return Ok(0);
        };
        let mut count = 0;

        for (id, obj_snapshot) in &snapshot.objects {
            if let Some(serializer) = self.serializers.get(&obj_snapshot.class_name) {
                // 检查对象是否仍存在
                if let Some(ptr) = registry.get_ptr(*id) {
                    // 恢复状态
                    if serializer
                        .deserialize(ptr, &obj_snapshot.properties)
                        .is_ok()
                    {
                        count += 1;
                    }
                } else {
                    // 对象不存在，需要重新创建
                    if let Ok(new_ptr) = serializer.create_instance() {
                        registry.update_ptr(*id, new_ptr);
                        if serializer
                            .deserialize(new_ptr, &obj_snapshot.properties)
                            .is_ok()
                        {
                            count += 1;
                        }
                    }
                }
            }
        }

        Ok(count)
    }

    /// 清除快照
    pub fn clear_snapshot(&mut self) {
        self.current_snapshot = None;
    }

    /// 获取快照大小
    pub fn snapshot_size(&self) -> usize {
        self.current_snapshot
            .as_ref()
            .map(|s| s.objects.len())
            .unwrap_or(0)
    }

    /// 从属性中收集对象引用
    fn collect_references(properties: &HashMap<String, SerializedValue>) -> Vec<ObjectId> {
        let mut refs = Vec::new();
        Self::collect_references_recursive(properties, &mut refs);
        refs
    }

    /// 递归收集引用
    fn collect_references_recursive(
        properties: &HashMap<String, SerializedValue>,
        refs: &mut Vec<ObjectId>,
    ) {
        for value in properties.values() {
            Self::collect_value_references(value, refs);
        }
    }

    /// 从单个值收集引用
    fn collect_value_references(value: &SerializedValue, refs: &mut Vec<ObjectId>) {
        match value {
            SerializedValue::Reference(id) => {
                if !refs.contains(id) {
                    refs.push(*id);
                }
            }
            SerializedValue::Array(arr) => {
                for item in arr {
                    Self::collect_value_references(item, refs);
                }
            }
            SerializedValue::Object(obj) => {
                Self::collect_references_recursive(obj, refs);
            }
            _ => {}
        }
    }
}

impl Default for StateManager {
    fn default() -> Self {
        Self::new()
    }
}

/// 生成的序列化代码会实现这个宏
#[macro_export]
macro_rules! impl_hot_reload_serializable {
    ($type:ty, $class_name:expr, { $($field:ident : $field_type:ty),* $(,)? }) => {
        impl $crate::hotreload::HotReloadSerializable for $type {
            fn serialize_state(&self) -> std::collections::HashMap<String, $crate::hotreload::SerializedValue> {
                let mut map = std::collections::HashMap::new();
                $(
                    map.insert(
                        stringify!($field).to_string(),
                        $crate::hotreload::serialize_value(&self.$field),
                    );
                )*
                map
            }

            fn deserialize_state(&mut self, state: &std::collections::HashMap<String, $crate::hotreload::SerializedValue>) {
                $(
                    if let Some(value) = state.get(stringify!($field)) {
                        if let Some(v) = $crate::hotreload::deserialize_value::<$field_type>(value) {
                            self.$field = v;
                        }
                    }
                )*
            }

            fn class_name(&self) -> &'static str {
                $class_name
            }
        }
    };
}

/// 序列化值辅助函数
pub fn serialize_value<T: Serialize>(value: &T) -> SerializedValue {
    value.to_serialized()
}

/// 反序列化值辅助函数
pub fn deserialize_value<T: Deserialize>(value: &SerializedValue) -> Option<T> {
    T::from_serialized(value)
}

/// 序列化 trait
pub trait Serialize {
    fn to_serialized(&self) -> SerializedValue;
}

/// 反序列化 trait
pub trait Deserialize: Sized {
    fn from_serialized(value: &SerializedValue) -> Option<Self>;
}

// 基础类型实现
impl Serialize for bool {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Bool(*self)
    }
}

impl Deserialize for bool {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Bool(b) => Some(*b),
            _ => None,
        }
    }
}

impl Serialize for i32 {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Int(*self as i64)
    }
}

impl Deserialize for i32 {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Int(i) => Some(*i as i32),
            _ => None,
        }
    }
}

impl Serialize for i64 {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Int(*self)
    }
}

impl Deserialize for i64 {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Int(i) => Some(*i),
            _ => None,
        }
    }
}

impl Serialize for f32 {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Float(*self as f64)
    }
}

impl Deserialize for f32 {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Float(f) => Some(*f as f32),
            _ => None,
        }
    }
}

impl Serialize for f64 {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Float(*self)
    }
}

impl Deserialize for f64 {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Float(f) => Some(*f),
            _ => None,
        }
    }
}

impl Serialize for String {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::String(self.clone())
    }
}

impl Deserialize for String {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::String(s) => Some(s.clone()),
            _ => None,
        }
    }
}

impl<T: Serialize> Serialize for Vec<T> {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Array(self.iter().map(|v| v.to_serialized()).collect())
    }
}

impl<T: Deserialize> Deserialize for Vec<T> {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Array(arr) => arr.iter().map(|v| T::from_serialized(v)).collect(),
            _ => None,
        }
    }
}

// =========================================================================
// 复杂类型序列化 (HashMap, HashSet, Option)
// =========================================================================

impl<K: Serialize + ToString, V: Serialize> Serialize for HashMap<K, V> {
    fn to_serialized(&self) -> SerializedValue {
        let map: HashMap<String, SerializedValue> = self
            .iter()
            .map(|(k, v)| (k.to_string(), v.to_serialized()))
            .collect();
        SerializedValue::Object(map)
    }
}

impl<K: Deserialize + std::str::FromStr + Eq + std::hash::Hash, V: Deserialize> Deserialize
    for HashMap<K, V>
{
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Object(map) => {
                let mut result = HashMap::new();
                for (k, v) in map {
                    if let (Ok(key), Some(val)) = (k.parse(), V::from_serialized(v)) {
                        result.insert(key, val);
                    }
                }
                Some(result)
            }
            _ => None,
        }
    }
}

impl<T: Serialize> Serialize for Option<T> {
    fn to_serialized(&self) -> SerializedValue {
        match self {
            Some(v) => v.to_serialized(),
            None => SerializedValue::Null,
        }
    }
}

impl<T: Deserialize> Deserialize for Option<T> {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Null => Some(None),
            other => T::from_serialized(other).map(Some),
        }
    }
}

impl Serialize for u32 {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Int(*self as i64)
    }
}

impl Deserialize for u32 {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Int(i) => Some(*i as u32),
            _ => None,
        }
    }
}

impl Serialize for u64 {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Int(*self as i64)
    }
}

impl Deserialize for u64 {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Int(i) => Some(*i as u64),
            _ => None,
        }
    }
}

impl Serialize for usize {
    fn to_serialized(&self) -> SerializedValue {
        SerializedValue::Int(*self as i64)
    }
}

impl Deserialize for usize {
    fn from_serialized(value: &SerializedValue) -> Option<Self> {
        match value {
            SerializedValue::Int(i) => Some(*i as usize),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_serialize_primitives() {
        assert!(matches!(42i32.to_serialized(), SerializedValue::Int(42)));
        assert!(matches!(3.14f64.to_serialized(), SerializedValue::Float(_)));
        assert!(matches!(true.to_serialized(), SerializedValue::Bool(true)));

        let s = "hello".to_string();
        assert!(matches!(s.to_serialized(), SerializedValue::String(_)));
    }

    #[test]
    fn test_deserialize_primitives() {
        let int_val = SerializedValue::Int(100);
        assert_eq!(i32::from_serialized(&int_val), Some(100));

        let float_val = SerializedValue::Float(2.5);
        assert_eq!(f64::from_serialized(&float_val), Some(2.5));

        let bool_val = SerializedValue::Bool(true);
        assert_eq!(bool::from_serialized(&bool_val), Some(true));
    }

    #[test]
    fn test_serialize_vec() {
        let vec = vec![1i32, 2, 3];
        let serialized = vec.to_serialized();
        assert!(matches!(serialized, SerializedValue::Array(_)));
    }

    #[test]
    fn test_serialize_option() {
        let some_val: Option<i32> = Some(42);
        let none_val: Option<i32> = None;

        assert!(matches!(some_val.to_serialized(), SerializedValue::Int(42)));
        assert!(matches!(none_val.to_serialized(), SerializedValue::Null));
    }

    #[test]
    fn test_state_manager_new() {
        let mut manager = StateManager::new();
        assert_eq!(manager.save_all().unwrap(), 0);
    }
}
