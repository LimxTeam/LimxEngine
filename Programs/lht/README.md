# LHT - Limx Header Tool

> C++ 反射代码生成器和热重载系统

## 功能特性

### 反射系统
- **宏解析** - LCLASS, LSTRUCT, LENUM, LPROPERTY, LFUNCTION, LDELEGATE
- **代码生成** - `.generated.h/cpp` 自动生成
- **类型注册** - 运行时类型信息 (RTTI)
- **序列化代码** - 二进制/JSON/网络序列化生成
- **RPC 代码** - 客户端/服务器/多播 RPC 自动生成
- **GC 支持** - 垃圾回收引用遍历代码生成

### 热重载
- **文件监控** - 实时检测源码变更
- **增量编译** - 仅编译修改的文件
- **DLL 热交换** - 运行时替换模块
- **状态保持** - 跨重载保持对象状态
- **错误回滚** - 编译失败自动回滚

### 开发工具
- **语法检查** - 反射宏语法验证
- **API 文档** - Markdown 格式自动生成
- **反射统计** - 类型/属性/函数统计分析

## 安装

```bash
cd Programs
cargo build --release
```

## 命令列表

| 命令 | 描述 |
|------|------|
| `generate` | 生成反射代码 |
| `check` | 检查反射宏语法 |
| `watch` | 热重载监控模式 |
| `docs` | 生成 API 文档 |
| `stats` | 显示反射统计 |

## 使用

### 生成反射代码

```bash
# 基础生成
lht generate -s Source -o Intermediate/Generated

# 指定模块
lht generate -s Source -o Intermediate/Generated -m LimxCore

# 生成序列化代码
lht generate -s Source -o Generated --serialization

# 生成 RPC 代码
lht generate -s Source -o Generated --rpc

# 生成 GC 支持代码
lht generate -s Source -o Generated --gc

# 生成所有高级功能
lht generate -s Source -o Generated --all
```

### 检查与统计

```bash
# 检查反射宏语法
lht check -s Source

# 显示反射统计
lht stats -s Source
```

### 热重载模式

```bash
# 启动热重载监控
lht watch -s Source -o Intermediate/Generated
```

### API 文档

```bash
# 生成 API 文档
lht docs -s Source -o Docs/API
```

## 反射宏

### 类

```cpp
LCLASS(Serializable, BlueprintType)
class LIMX_API AMyActor : public AActor
{
    LGENERATED_BODY()
    
    LPROPERTY(Replicated, EditAnywhere)
    float Health;
    
    LFUNCTION(Server, Reliable)
    void TakeDamage(float Amount);
};
```

### 结构体

```cpp
LSTRUCT(BlueprintType)
struct FMyStruct
{
    LGENERATED_BODY()
    
    LPROPERTY(EditAnywhere)
    int32 Value;
};
```

### 枚举

```cpp
LENUM(BlueprintType)
enum class EMyEnum : uint8
{
    None    LMETA(DisplayName="无"),
    First   LMETA(DisplayName="第一"),
    Second  LMETA(DisplayName="第二"),
};
```

### 委托

```cpp
LDELEGATE(Multicast)
DECLARE_DELEGATE_OneParam(FOnHealthChanged, float);
```

## 说明符

### 类说明符
| 说明符 | 描述 |
|--------|------|
| `Serializable` | 启用序列化 |
| `BlueprintType` | 蓝图可用 |
| `Abstract` | 抽象类 |
| `Config=Game` | 配置文件绑定 |

### 属性说明符
| 说明符 | 描述 |
|--------|------|
| `EditAnywhere` | 编辑器可编辑 |
| `VisibleAnywhere` | 编辑器只读 |
| `Replicated` | 网络复制 |
| `Transient` | 不序列化 |
| `BlueprintReadWrite` | 蓝图读写 |

### 函数说明符
| 说明符 | 描述 |
|--------|------|
| `BlueprintCallable` | 蓝图可调用 |
| `Server` | 服务器 RPC |
| `Client` | 客户端 RPC |
| `Reliable` | 可靠传输 |
| `NetMulticast` | 组播 |

## 生成文件

```
Intermediate/Generated/
├── ModuleName/
│   ├── ClassName.generated.h
│   └── ClassName.generated.cpp
```

### 生成的代码示例

```cpp
// MyClass.generated.h
#define LGENERATED_BODY_MyClass() \
    static Limx::Core::TypeInfo s_TypeInfo; \
    static const Limx::Core::TypeInfo* StaticTypeInfo(); \
    virtual const Limx::Core::TypeInfo* GetTypeInfo() const;
```

## 热重载架构

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Watcher   │────▶│  Compiler   │────▶│   Loader    │
│  (文件监控)  │     │  (增量编译)  │     │  (DLL加载)  │
└─────────────┘     └─────────────┘     └─────────────┘
                                              │
                                              ▼
                                        ┌─────────────┐
                                        │    State    │
                                        │  (状态管理)  │
                                        └─────────────┘
```

## 测试

```bash
cargo test -p lht
```

## 许可证

MIT License - LimxTeam
