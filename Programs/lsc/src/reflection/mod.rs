/*******************************************************************************
 * 文件: reflection/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   SPIR-V 反射信息提取
 *   - 提取 Uniform Buffer 布局
 *   - 提取 Push Constants
 *   - 提取输入/输出变量
 *   - 提取采样器和纹理绑定
 *
 ******************************************************************************/

use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;

//=============================================================================
// 反射数据结构
//=============================================================================

/// 着色器反射信息
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ShaderReflection {
    /// 入口点名称
    pub entry_point: String,
    /// 着色器阶段
    pub stage: String,
    /// 输入变量
    pub inputs: Vec<ShaderVariable>,
    /// 输出变量
    pub outputs: Vec<ShaderVariable>,
    /// Uniform Buffers
    pub uniform_buffers: Vec<UniformBuffer>,
    /// Storage Buffers
    pub storage_buffers: Vec<StorageBuffer>,
    /// Push Constants
    pub push_constants: Option<PushConstantRange>,
    /// 采样器
    pub samplers: Vec<SamplerBinding>,
    /// 纹理
    pub textures: Vec<TextureBinding>,
    /// 存储图像
    pub storage_images: Vec<StorageImageBinding>,
    /// 子通道输入
    pub subpass_inputs: Vec<SubpassInput>,
    /// 特化常量
    pub specialization_constants: Vec<SpecializationConstant>,
    /// 工作组大小 (计算着色器)
    pub workgroup_size: Option<[u32; 3]>,
}

/// 着色器变量
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderVariable {
    /// 变量名称
    pub name: String,
    /// Location
    pub location: u32,
    /// 类型
    pub data_type: DataType,
    /// 数组大小 (0 = 非数组)
    pub array_size: u32,
}

/// 数据类型
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataType {
    /// 基本类型
    pub base_type: BaseType,
    /// 向量维度 (1-4)
    pub vec_size: u32,
    /// 矩阵列数 (1 = 非矩阵)
    pub columns: u32,
}

/// 基本类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum BaseType {
    Float,
    Double,
    Int,
    UInt,
    Bool,
    Struct,
    Image,
    Sampler,
    SampledImage,
}

impl Default for BaseType {
    fn default() -> Self {
        Self::Float
    }
}

/// Uniform Buffer
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UniformBuffer {
    /// 名称
    pub name: String,
    /// Set
    pub set: u32,
    /// Binding
    pub binding: u32,
    /// 大小 (字节)
    pub size: u32,
    /// 成员
    pub members: Vec<BufferMember>,
}

/// Storage Buffer
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StorageBuffer {
    /// 名称
    pub name: String,
    /// Set
    pub set: u32,
    /// Binding
    pub binding: u32,
    /// 是否只读
    pub readonly: bool,
    /// 成员
    pub members: Vec<BufferMember>,
}

/// Buffer 成员
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BufferMember {
    /// 名称
    pub name: String,
    /// 偏移量
    pub offset: u32,
    /// 大小
    pub size: u32,
    /// 类型
    pub data_type: DataType,
    /// 数组大小
    pub array_size: u32,
    /// 数组步长
    pub array_stride: u32,
    /// 矩阵步长
    pub matrix_stride: u32,
}

/// Push Constant 范围
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PushConstantRange {
    /// 偏移量
    pub offset: u32,
    /// 大小
    pub size: u32,
    /// 成员
    pub members: Vec<BufferMember>,
}

/// 采样器绑定
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SamplerBinding {
    /// 名称
    pub name: String,
    /// Set
    pub set: u32,
    /// Binding
    pub binding: u32,
}

/// 纹理绑定
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TextureBinding {
    /// 名称
    pub name: String,
    /// Set
    pub set: u32,
    /// Binding
    pub binding: u32,
    /// 纹理维度
    pub dimension: TextureDimension,
    /// 是否多采样
    pub multisampled: bool,
    /// 是否数组
    pub arrayed: bool,
}

/// 纹理维度
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum TextureDimension {
    Tex1D,
    Tex2D,
    Tex3D,
    TexCube,
    Buffer,
}

impl Default for TextureDimension {
    fn default() -> Self {
        Self::Tex2D
    }
}

/// 存储图像绑定
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StorageImageBinding {
    /// 名称
    pub name: String,
    /// Set
    pub set: u32,
    /// Binding
    pub binding: u32,
    /// 格式
    pub format: String,
    /// 维度
    pub dimension: TextureDimension,
    /// 是否只读
    pub readonly: bool,
    /// 是否只写
    pub writeonly: bool,
}

/// 子通道输入
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SubpassInput {
    /// 名称
    pub name: String,
    /// Set
    pub set: u32,
    /// Binding
    pub binding: u32,
    /// 输入附件索引
    pub input_attachment_index: u32,
}

/// 特化常量
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SpecializationConstant {
    /// 名称
    pub name: String,
    /// Constant ID
    pub constant_id: u32,
    /// 类型
    pub data_type: DataType,
    /// 默认值
    pub default_value: String,
}

//=============================================================================
// SPIR-V 常量
//=============================================================================

// 存储类
const SC_UNIFORM_CONSTANT: u32 = 0;
const SC_INPUT: u32 = 1;
const SC_UNIFORM: u32 = 2;
const SC_OUTPUT: u32 = 3;
const SC_PUSH_CONSTANT: u32 = 9;
const SC_STORAGE_BUFFER: u32 = 12;

// 装饰
//
// 用具名常量而不是字面量: Location(30) / Binding(33) / DescriptorSet(34) /
// Offset(35) 四个编号紧挨着且含义毫不相干, 串一位不会报错, 只会让下游拿到
// 一份编号自洽但全错的绑定表。
const DEC_BUFFER_BLOCK: u32 = 3;
const DEC_ROW_MAJOR: u32 = 4;
const DEC_ARRAY_STRIDE: u32 = 6;
const DEC_MATRIX_STRIDE: u32 = 7;
const DEC_NON_WRITABLE: u32 = 24;
const DEC_LOCATION: u32 = 30;
const DEC_BINDING: u32 = 33;
const DEC_DESCRIPTOR_SET: u32 = 34;
const DEC_OFFSET: u32 = 35;
const DEC_INPUT_ATTACHMENT_INDEX: u32 = 43;

//=============================================================================
// SPIR-V 解析
//=============================================================================

impl ShaderReflection {
    /// 从 SPIR-V 二进制提取反射信息
    pub fn from_spirv(spirv: &[u8]) -> Result<Self> {
        // 验证 SPIR-V 魔数
        if spirv.len() < 20 {
            return Err(anyhow!("SPIR-V 数据太短"));
        }

        let magic = u32::from_le_bytes([spirv[0], spirv[1], spirv[2], spirv[3]]);
        if magic != 0x07230203 {
            return Err(anyhow!("无效的 SPIR-V 魔数"));
        }

        // SPIR-V 以 32 位字为单位, 长度不是 4 的倍数说明文件被截断了。下面的
        // chunks_exact 会把末尾不足一字的字节直接扔掉, 不查就等于把一份残缺的
        // 字节码当完整的反射出去。
        if !spirv.len().is_multiple_of(4) {
            return Err(anyhow!("SPIR-V 长度 {} 不是 4 的倍数", spirv.len()));
        }

        // 解析 SPIR-V
        let words: Vec<u32> = spirv
            .chunks_exact(4)
            .map(|chunk| u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
            .collect();

        let mut reflection = ShaderReflection::default();
        let mut module = SpirvModule::default();

        // 变量按模块内出现顺序收集。用 HashMap 存会让同一份 SPIR-V 每次跑出
        // 不同顺序的 JSON —— 构建产物逐字节变动, 缓存与 diff 都失去意义。
        let mut variables: Vec<(u32, SpirvVariable)> = Vec::new();

        // 解析指令
        let mut i = 5; // 跳过头部
        while i < words.len() {
            let instruction = words[i];
            let opcode = instruction & 0xFFFF;
            let word_count = (instruction >> 16) as usize;

            // 越界或零长指令说明字节码本身坏了。此处原先是 break, 于是残缺的
            // 模块会安静地反射出半份结果 —— 那正是"失败落在通过上"。
            if word_count == 0 || i + word_count > words.len() {
                return Err(anyhow!("SPIR-V 指令流在第 {} 个字处损坏", i));
            }

            match opcode {
                // OpName
                5 => {
                    if word_count > 2 {
                        let id = words[i + 1];
                        let name = parse_spirv_string(&words[i + 2..i + word_count]);
                        module.names.insert(id, name);
                    }
                }
                // OpMemberName
                6 => {
                    if word_count > 3 {
                        let struct_id = words[i + 1];
                        let member = words[i + 2];
                        let name = parse_spirv_string(&words[i + 3..i + word_count]);
                        module.member_names.insert((struct_id, member), name);
                    }
                }
                // OpEntryPoint
                15 => {
                    if word_count >= 4 {
                        let execution_model = words[i + 1];
                        // 名字之后紧跟 interface 变量的 id 列表, 截断由
                        // parse_spirv_string 在第一个 NUL 处负责
                        reflection.entry_point = parse_spirv_string(&words[i + 3..i + word_count]);
                        reflection.stage = match execution_model {
                            0 => "Vertex",
                            1 => "TessellationControl",
                            2 => "TessellationEvaluation",
                            3 => "Geometry",
                            4 => "Fragment",
                            5 => "GLCompute",
                            _ => "Unknown",
                        }
                        .to_string();
                    }
                }
                // OpExecutionMode
                16 => {
                    if word_count >= 3 {
                        let mode = words[i + 2];
                        // LocalSize (计算着色器工作组大小)
                        if mode == 17 && word_count >= 6 {
                            reflection.workgroup_size =
                                Some([words[i + 3], words[i + 4], words[i + 5]]);
                        }
                    }
                }
                // OpTypeVoid
                19 => {
                    if word_count >= 2 {
                        module.types.insert(words[i + 1], SpirvType::Void);
                    }
                }
                // OpTypeBool
                20 => {
                    if word_count >= 2 {
                        module.types.insert(words[i + 1], SpirvType::Bool);
                    }
                }
                // OpTypeInt
                21 => {
                    if word_count >= 4 {
                        let id = words[i + 1];
                        let width = words[i + 2];
                        let signed = words[i + 3] != 0;
                        module.types.insert(id, SpirvType::Int { width, signed });
                    }
                }
                // OpTypeFloat
                22 => {
                    if word_count >= 3 {
                        let id = words[i + 1];
                        let width = words[i + 2];
                        module.types.insert(id, SpirvType::Float { width });
                    }
                }
                // OpTypeVector
                23 => {
                    if word_count >= 4 {
                        let id = words[i + 1];
                        let component_type = words[i + 2];
                        let count = words[i + 3];
                        module.types.insert(
                            id,
                            SpirvType::Vector {
                                component_type,
                                count,
                            },
                        );
                    }
                }
                // OpTypeMatrix
                24 => {
                    if word_count >= 4 {
                        let id = words[i + 1];
                        let column_type = words[i + 2];
                        let columns = words[i + 3];
                        module.types.insert(
                            id,
                            SpirvType::Matrix {
                                column_type,
                                columns,
                            },
                        );
                    }
                }
                // OpTypeImage
                25 => {
                    if word_count >= 2 {
                        module.types.insert(words[i + 1], SpirvType::Image);
                    }
                }
                // OpTypeSampler
                26 => {
                    if word_count >= 2 {
                        module.types.insert(words[i + 1], SpirvType::Sampler);
                    }
                }
                // OpTypeSampledImage
                27 => {
                    if word_count >= 2 {
                        module.types.insert(words[i + 1], SpirvType::SampledImage);
                    }
                }
                // OpTypeArray
                28 => {
                    if word_count >= 4 {
                        let id = words[i + 1];
                        let element_type = words[i + 2];
                        // 长度常量留到用的时候再查: 数组类型与它的长度常量在
                        // 模块里的先后顺序由生成器决定, 解析期直接查会漏
                        let length_id = words[i + 3];
                        module.types.insert(
                            id,
                            SpirvType::Array {
                                element_type,
                                length_id,
                            },
                        );
                    }
                }
                // OpTypeRuntimeArray
                29 => {
                    if word_count >= 3 {
                        let id = words[i + 1];
                        let element_type = words[i + 2];
                        module
                            .types
                            .insert(id, SpirvType::RuntimeArray { element_type });
                    }
                }
                // OpTypeStruct
                30 => {
                    if word_count >= 2 {
                        let id = words[i + 1];
                        let members: Vec<u32> = words[i + 2..i + word_count].to_vec();
                        module.types.insert(id, SpirvType::Struct { members });
                    }
                }
                // OpTypePointer
                32 => {
                    if word_count >= 4 {
                        let id = words[i + 1];
                        let storage_class = words[i + 2];
                        let pointee = words[i + 3];
                        module.types.insert(
                            id,
                            SpirvType::Pointer {
                                storage_class,
                                pointee,
                            },
                        );
                    }
                }
                // OpConstant / OpSpecConstant
                //
                // 两者都可能当数组长度用; 特化常量取的是默认值, 那也正是不做
                // 特化时驱动看到的长度
                43 | 50 => {
                    if word_count >= 4 {
                        let id = words[i + 2];
                        let value = words[i + 3];
                        module.constants.insert(id, value);
                    }
                }
                // OpVariable
                59 => {
                    if word_count >= 4 {
                        let type_id = words[i + 1];
                        let id = words[i + 2];
                        let storage_class = words[i + 3];
                        variables.push((
                            id,
                            SpirvVariable {
                                type_id,
                                storage_class,
                            },
                        ));
                    }
                }
                // OpDecorate
                71 => {
                    if word_count >= 3 {
                        let id = words[i + 1];
                        let decoration = words[i + 2];
                        let operands: Vec<u32> = words[i + 3..i + word_count].to_vec();
                        module
                            .decorations
                            .entry(id)
                            .or_default()
                            .push((decoration, operands));
                    }
                }
                // OpMemberDecorate
                //
                // 成员的 Offset / MatrixStride / RowMajor 全在这里。此前完全没
                // 解析这条指令, 于是块布局无从算起, size 只能恒为 0。
                72 => {
                    if word_count >= 4 {
                        let struct_id = words[i + 1];
                        let member = words[i + 2];
                        let decoration = words[i + 3];
                        let operands: Vec<u32> = words[i + 4..i + word_count].to_vec();
                        module
                            .member_decorations
                            .entry((struct_id, member))
                            .or_default()
                            .push((decoration, operands));
                    }
                }
                _ => {}
            }

            i += word_count;
        }

        // 处理变量
        for (id, var) in &variables {
            let name = module.names.get(id).cloned().unwrap_or_default();

            let location = module.decoration(*id, DEC_LOCATION);
            let binding = module.decoration(*id, DEC_BINDING);
            let set = module.decoration(*id, DEC_DESCRIPTOR_SET);
            let input_attachment_index = module.decoration(*id, DEC_INPUT_ATTACHMENT_INDEX);

            match var.storage_class {
                SC_INPUT | SC_OUTPUT => {
                    // 没有 Location 的是内建变量 (gl_Position / gl_VertexIndex 等),
                    // 它们不占顶点属性槽位
                    let Some(location) = location else { continue };

                    let pointee = module.pointee(var.type_id)?;
                    // 曲面细分/几何着色器的接口变量整体是数组, 元素类型才是
                    // location 上真正的那个类型
                    let (element_type, array_size) = module.strip_arrays(pointee)?;

                    let variable = ShaderVariable {
                        name,
                        location,
                        data_type: module.data_type(element_type)?,
                        array_size,
                    };
                    if var.storage_class == SC_INPUT {
                        reflection.inputs.push(variable);
                    } else {
                        reflection.outputs.push(variable);
                    }
                }
                SC_UNIFORM => {
                    let Some((set, binding)) = set.zip(binding) else { continue };
                    let block = module.block_struct(var.type_id)?;

                    // Uniform 存储类里带 BufferBlock 装饰的是 SSBO 的旧写法
                    // (SPIR-V 1.3 之前没有 StorageBuffer 存储类)。按 UBO 归类会
                    // 让它的无界数组算出 size 0, 看着像个空 UBO。
                    if module.has_decoration(block, DEC_BUFFER_BLOCK) {
                        reflection.storage_buffers.push(StorageBuffer {
                            name,
                            set,
                            binding,
                            readonly: module.has_decoration(*id, DEC_NON_WRITABLE),
                            members: module.struct_members(block)?,
                        });
                    } else {
                        reflection.uniform_buffers.push(UniformBuffer {
                            name,
                            set,
                            binding,
                            size: module.struct_extent(block)?,
                            members: module.struct_members(block)?,
                        });
                    }
                }
                SC_STORAGE_BUFFER => {
                    let Some((set, binding)) = set.zip(binding) else { continue };
                    let block = module.block_struct(var.type_id)?;

                    reflection.storage_buffers.push(StorageBuffer {
                        name,
                        set,
                        binding,
                        readonly: module.has_decoration(*id, DEC_NON_WRITABLE),
                        members: module.struct_members(block)?,
                    });
                }
                SC_PUSH_CONSTANT => {
                    let block = module.block_struct(var.type_id)?;
                    let members = module.struct_members(block)?;

                    // VkPushConstantRange 描述的是块在 128 字节窗口里占的那一段,
                    // 起点未必是 0 —— 取成员偏移的下界与上界之差
                    let offset = members.iter().map(|m| m.offset).min().unwrap_or(0);
                    let end = members.iter().map(|m| m.offset + m.size).max().unwrap_or(0);

                    reflection.push_constants = Some(PushConstantRange {
                        offset,
                        size: end.saturating_sub(offset),
                        members,
                    });
                }
                // UniformConstant (samplers, textures)
                SC_UNIFORM_CONSTANT => {
                    let Some((set, binding)) = set.zip(binding) else { continue };

                    let pointee = module.pointee(var.type_id)?;
                    // bindless 写法 (uniform sampler2D tex[1024]) 在指针与图像之间
                    // 夹着一层数组。此前没剥这层, match 落到 _ 分支, 整条绑定被
                    // 无声丢掉 —— 下游照反射建描述符布局会少一个 binding。
                    let (element_type, _) = module.strip_arrays(pointee)?;

                    match module.types.get(&element_type) {
                        Some(SpirvType::Sampler) => {
                            reflection.samplers.push(SamplerBinding { name, set, binding });
                        }
                        Some(SpirvType::SampledImage) | Some(SpirvType::Image) => {
                            reflection.textures.push(TextureBinding {
                                name,
                                set,
                                binding,
                                dimension: TextureDimension::Tex2D,
                                multisampled: false,
                                arrayed: false,
                            });
                        }
                        _ => {}
                    }
                }
                _ => {}
            }
        }

        // 排序让反射 JSON 只取决于 SPIR-V 内容本身
        reflection.inputs.sort_by_key(|v| v.location);
        reflection.outputs.sort_by_key(|v| v.location);
        reflection.uniform_buffers.sort_by_key(|b| (b.set, b.binding));
        reflection.storage_buffers.sort_by_key(|b| (b.set, b.binding));
        reflection.samplers.sort_by_key(|b| (b.set, b.binding));
        reflection.textures.sort_by_key(|b| (b.set, b.binding));

        Ok(reflection)
    }
}

//=============================================================================
// 类型与布局
//=============================================================================

/// SPIR-V 模块里与反射有关的几张表
///
/// 块的大小与偏移一律从 `OpMemberDecorate Offset` / `OpTypeStruct` /
/// `OpTypeMatrix` / `OpTypeVector` / `OpTypeFloat` 现算, 走的是驱动实际用的
/// 那条路。这些装饰是 Vulkan 规范要求 Block 必须带的, 和 `OpName` 那类调试
/// 信息无关 —— 所以 `-O` 把名字剥光之后, 布局信息依然完整。
#[derive(Debug, Default)]
struct SpirvModule {
    names: HashMap<u32, String>,
    member_names: HashMap<(u32, u32), String>,
    decorations: HashMap<u32, DecorationList>,
    member_decorations: HashMap<(u32, u32), DecorationList>,
    types: HashMap<u32, SpirvType>,
    constants: HashMap<u32, u32>,
}

/// 一个目标 (变量/类型, 或结构体的某个成员) 身上的装饰, 每项是 (装饰, 操作数)
type DecorationList = Vec<(u32, Vec<u32>)>;

impl SpirvModule {
    /// 取装饰的第一个操作数
    fn decoration(&self, id: u32, decoration: u32) -> Option<u32> {
        self.decorations
            .get(&id)?
            .iter()
            .find(|(d, _)| *d == decoration)
            .and_then(|(_, operands)| operands.first().copied())
    }

    /// 判断是否带某个无操作数的装饰
    fn has_decoration(&self, id: u32, decoration: u32) -> bool {
        self.decorations
            .get(&id)
            .is_some_and(|list| list.iter().any(|(d, _)| *d == decoration))
    }

    /// 取成员装饰的第一个操作数
    fn member_decoration(&self, struct_id: u32, member: u32, decoration: u32) -> Option<u32> {
        self.member_decorations
            .get(&(struct_id, member))?
            .iter()
            .find(|(d, _)| *d == decoration)
            .and_then(|(_, operands)| operands.first().copied())
    }

    /// 判断成员是否带某个无操作数的装饰
    fn has_member_decoration(&self, struct_id: u32, member: u32, decoration: u32) -> bool {
        self.member_decorations
            .get(&(struct_id, member))
            .is_some_and(|list| list.iter().any(|(d, _)| *d == decoration))
    }

    /// 指针类型 → 它指向的类型
    fn pointee(&self, pointer_type: u32) -> Result<u32> {
        match self.types.get(&pointer_type) {
            Some(SpirvType::Pointer { pointee, .. }) => Ok(*pointee),
            _ => Err(anyhow!("类型 %{} 不是指针", pointer_type)),
        }
    }

    /// 剥掉所有数组层, 返回 (元素类型, 元素总数)
    ///
    /// 元素总数取各维之积。运行时数组 (SSBO 尾部的无界数组) 的长度由绑定时
    /// 的缓冲区大小决定, 编译期算不出, 记 0 —— 它的 array_stride 仍有值,
    /// 下游据此能把"无界"和"非数组"分开。
    fn strip_arrays(&self, type_id: u32) -> Result<(u32, u32)> {
        let mut current = type_id;
        let mut count = 1u32;
        let mut stripped = false;

        loop {
            match self.types.get(&current) {
                Some(SpirvType::Array {
                    element_type,
                    length_id,
                }) => {
                    // 长度查不到就报错而不是记 0: 记 0 会让一个 1024 项的
                    // bindless 数组显示成空数组, 而且没有任何提示
                    let length = self.constants.get(length_id).copied().ok_or_else(|| {
                        anyhow!("数组 %{} 的长度常量 %{} 未定义", current, length_id)
                    })?;
                    count = count.saturating_mul(length);
                    current = *element_type;
                    stripped = true;
                }
                Some(SpirvType::RuntimeArray { element_type }) => {
                    count = 0;
                    current = *element_type;
                    stripped = true;
                }
                _ => break,
            }
        }

        // array_size 的约定是 0 表示非数组
        Ok((current, if stripped { count } else { 0 }))
    }

    /// 变量的指针类型 → 它描述的那个 Block 结构体
    fn block_struct(&self, pointer_type: u32) -> Result<u32> {
        let pointee = self.pointee(pointer_type)?;
        // uniform Block { ... } blocks[4] 这种写法在指针与结构体之间还夹着数组
        let (struct_id, _) = self.strip_arrays(pointee)?;

        match self.types.get(&struct_id) {
            Some(SpirvType::Struct { .. }) => Ok(struct_id),
            _ => Err(anyhow!("指针 %{} 指向的不是结构体", pointer_type)),
        }
    }

    /// 向量的分量数
    fn component_count(&self, type_id: u32) -> Result<u32> {
        match self.types.get(&type_id) {
            Some(SpirvType::Vector { count, .. }) => Ok(*count),
            _ => Err(anyhow!("类型 %{} 不是向量", type_id)),
        }
    }

    /// 计算类型占用的字节数
    ///
    /// `matrix_stride` / `row_major` 来自**外层结构体成员**的装饰而不是矩阵
    /// 类型本身 —— 同一个 mat4 类型在两个块里可以有不同的行列序和跨度。
    fn type_size(&self, type_id: u32, matrix_stride: Option<u32>, row_major: bool) -> Result<u32> {
        match self.types.get(&type_id) {
            // 块内的 bool 按 32 位存, 不是宿主语言的 sizeof(bool)
            Some(SpirvType::Bool) => Ok(4),
            Some(SpirvType::Int { width, .. }) | Some(SpirvType::Float { width }) => Ok(width / 8),
            Some(SpirvType::Vector {
                component_type,
                count,
            }) => Ok(count * self.type_size(*component_type, None, false)?),
            Some(SpirvType::Matrix {
                column_type,
                columns,
            }) => {
                // 必须用 MatrixStride 而不是"列数 × 列大小": std140 下 mat3 的
                // 每列按 vec4 对齐, 按紧密排列算每列会短 4 字节
                let stride = match matrix_stride {
                    Some(stride) => stride,
                    None => self.type_size(*column_type, None, false)?,
                };
                // 行主序时跨度走的是行, 行数等于列向量的分量数
                let lines = if row_major {
                    self.component_count(*column_type)?
                } else {
                    *columns
                };
                Ok(stride * lines)
            }
            Some(SpirvType::Array {
                element_type,
                length_id,
            }) => {
                let length = self
                    .constants
                    .get(length_id)
                    .copied()
                    .ok_or_else(|| anyhow!("数组 %{} 的长度常量 %{} 未定义", type_id, length_id))?;
                // ArrayStride 含元素之间的对齐填充, 与元素自身大小不是一回事
                let stride = match self.decoration(type_id, DEC_ARRAY_STRIDE) {
                    Some(stride) => stride,
                    None => self.type_size(*element_type, matrix_stride, row_major)?,
                };
                Ok(stride * length)
            }
            // 长度绑定时才确定, 编译期没有确定大小
            Some(SpirvType::RuntimeArray { .. }) => Ok(0),
            Some(SpirvType::Struct { .. }) => self.struct_extent(type_id),
            Some(other) => Err(anyhow!("类型 %{} ({:?}) 没有确定的字节大小", type_id, other)),
            None => Err(anyhow!("类型 %{} 未定义", type_id)),
        }
    }

    /// 结构体总占用 = 各成员 offset + size 的上界
    fn struct_extent(&self, struct_id: u32) -> Result<u32> {
        let members = self.struct_members(struct_id)?;
        Ok(members
            .iter()
            .map(|m| m.offset + m.size)
            .max()
            .unwrap_or(0))
    }

    /// 展开结构体的成员布局
    fn struct_members(&self, struct_id: u32) -> Result<Vec<BufferMember>> {
        let member_types = match self.types.get(&struct_id) {
            Some(SpirvType::Struct { members }) => members.clone(),
            _ => return Err(anyhow!("类型 %{} 不是结构体", struct_id)),
        };

        let mut members = Vec::with_capacity(member_types.len());
        for (index, member_type) in member_types.iter().enumerate() {
            let index = index as u32;

            // Offset 缺失时不能退回 0: 那会把所有成员摞在块首, 下游拿到的是
            // 一份看着完整、实际全错的布局, 而且没有任何报错
            let offset = self
                .member_decoration(struct_id, index, DEC_OFFSET)
                .ok_or_else(|| {
                    anyhow!("结构体 %{} 的成员 {} 缺少 Offset 装饰", struct_id, index)
                })?;

            let matrix_stride = self.member_decoration(struct_id, index, DEC_MATRIX_STRIDE);
            let row_major = self.has_member_decoration(struct_id, index, DEC_ROW_MAJOR);
            let (element_type, array_size) = self.strip_arrays(*member_type)?;

            members.push(BufferMember {
                // -O 会剥掉 OpMemberName, 名字为空是预期的; 大小与偏移不受影响
                name: self
                    .member_names
                    .get(&(struct_id, index))
                    .cloned()
                    .unwrap_or_default(),
                offset,
                size: self.type_size(*member_type, matrix_stride, row_major)?,
                data_type: self.data_type(element_type)?,
                array_size,
                array_stride: self.decoration(*member_type, DEC_ARRAY_STRIDE).unwrap_or(0),
                matrix_stride: matrix_stride.unwrap_or(0),
            });
        }

        Ok(members)
    }

    /// SPIR-V 类型 → 反射用的 DataType
    fn data_type(&self, type_id: u32) -> Result<DataType> {
        let data_type = match self.types.get(&type_id) {
            Some(SpirvType::Bool) => DataType {
                base_type: BaseType::Bool,
                vec_size: 1,
                columns: 1,
            },
            Some(SpirvType::Int { signed, .. }) => DataType {
                base_type: if *signed {
                    BaseType::Int
                } else {
                    BaseType::UInt
                },
                vec_size: 1,
                columns: 1,
            },
            Some(SpirvType::Float { width }) => DataType {
                base_type: if *width == 64 {
                    BaseType::Double
                } else {
                    BaseType::Float
                },
                vec_size: 1,
                columns: 1,
            },
            Some(SpirvType::Vector {
                component_type,
                count,
            }) => DataType {
                base_type: self.data_type(*component_type)?.base_type,
                vec_size: *count,
                columns: 1,
            },
            Some(SpirvType::Matrix {
                column_type,
                columns,
            }) => {
                let column = self.data_type(*column_type)?;
                DataType {
                    // vec_size 是行数 (列向量的分量数), columns 是列数
                    base_type: column.base_type,
                    vec_size: column.vec_size,
                    columns: *columns,
                }
            }
            Some(SpirvType::Struct { .. }) => DataType {
                base_type: BaseType::Struct,
                vec_size: 1,
                columns: 1,
            },
            Some(SpirvType::Image) => DataType {
                base_type: BaseType::Image,
                vec_size: 1,
                columns: 1,
            },
            Some(SpirvType::Sampler) => DataType {
                base_type: BaseType::Sampler,
                vec_size: 1,
                columns: 1,
            },
            Some(SpirvType::SampledImage) => DataType {
                base_type: BaseType::SampledImage,
                vec_size: 1,
                columns: 1,
            },
            Some(SpirvType::Array { element_type, .. })
            | Some(SpirvType::RuntimeArray { element_type }) => self.data_type(*element_type)?,
            Some(SpirvType::Void) | Some(SpirvType::Pointer { .. }) | None => {
                return Err(anyhow!("类型 %{} 无法映射为反射类型", type_id))
            }
        };

        Ok(data_type)
    }
}

/// SPIR-V 类型
#[derive(Debug, Clone)]
enum SpirvType {
    Void,
    Bool,
    Int { width: u32, signed: bool },
    Float { width: u32 },
    Vector { component_type: u32, count: u32 },
    Matrix { column_type: u32, columns: u32 },
    Image,
    Sampler,
    SampledImage,
    Array { element_type: u32, length_id: u32 },
    RuntimeArray { element_type: u32 },
    Struct { members: Vec<u32> },
    Pointer { storage_class: u32, pointee: u32 },
}

/// SPIR-V 变量
#[derive(Debug, Clone)]
struct SpirvVariable {
    type_id: u32,
    storage_class: u32,
}

/// 解析 SPIR-V 字面串
///
/// 字面串以 NUL 结尾, 按 4 字节字对齐补零, 而**它后面还可以跟别的操作数**
/// —— OpEntryPoint 的 interface 变量 id 列表就紧接在名字之后。所以必须在第
/// 一个 NUL 处整体停下: 只跳过当前字的剩余字节是不够的, 那样会把 id 列表当
/// 成字符续读进来 (id 14/22/34… 变成 "main\u{e}\u{16}\u{22}…")。
fn parse_spirv_string(words: &[u32]) -> String {
    let mut bytes = Vec::new();
    'outer: for word in words {
        for &b in &word.to_le_bytes() {
            if b == 0 {
                break 'outer;
            }
            bytes.push(b);
        }
    }
    String::from_utf8_lossy(&bytes).to_string()
}

//=============================================================================
// 测试
//=============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::compiler::{OptimizationLevel, ShaderCompiler};
    use crate::core::{CompileOptions, ShaderSource, ShaderStage};

    /// gbuffer.vert 的骨架 —— 顶点输入、UBO、push constant 三样都在
    ///
    /// 每个输入和每个块成员都被真正用到: 用不到的变量会被优化器整个删掉,
    /// 那样测的就不是反射而是优化器的删除策略了。
    const GBUFFER_VERT: &str = r#"
#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord0;

layout(row_major, set = 0, binding = 0) uniform ViewProjUBO {
    mat4 view;
    mat4 proj;
    mat4 prevViewProj;
} ubo;

layout(row_major, push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) out vec4 fragPrevClip;

void main()
{
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);

    gl_Position     = ubo.proj * ubo.view * worldPos;
    fragPrevClip    = ubo.prevViewProj * worldPos * float(pc.materialIndex);
    fragTexCoord    = inTexCoord0;
    fragWorldNormal = inNormal;
}
"#;

    /// 材质 SSBO + bindless 采样器数组 —— 无界数组与数组绑定两条路径
    const BINDLESS_FRAG: &str = r#"
#version 450

struct Material {
    vec4  baseColor;
    float metallic;
    float roughness;
    uint  albedoIndex;
    uint  flags;
};

layout(std430, set = 1, binding = 0) readonly buffer MaterialBuffer {
    Material materials[];
} materialBuffer;

layout(set = 1, binding = 1) uniform sampler2D bindlessTextures[4];

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main()
{
    Material m = materialBuffer.materials[0];
    outColor = m.baseColor
             * texture(bindlessTextures[m.albedoIndex], inTexCoord)
             + vec4(m.metallic, m.roughness, float(m.flags), 1.0);
}
"#;

    /// 用 lsc 自己的编译流程把 GLSL 编成真实 SPIR-V
    ///
    /// 固定开 Performance 优化 —— 线上就是带 `-O` 跑的, 而 `-O` 会剥掉
    /// OpName/OpMemberName。用未优化的字节码测等于绕开了真正要防的回归:
    /// 布局必须从 Offset/ArrayStride/MatrixStride 装饰算出来, 而不是从调试
    /// 信息里读出来。
    fn compile(stage: ShaderStage, code: &str) -> Vec<u8> {
        let source = ShaderSource {
            code: code.to_string(),
            file_path: None,
            stage,
        };

        let mut options = CompileOptions::new();
        options.optimization_level = OptimizationLevel::Performance;

        let compiler = ShaderCompiler::new().expect("shaderc 初始化失败");
        compiler
            .compile(&source, &options)
            .expect("测试着色器编译失败")
            .spirv_binary
    }

    fn reflect(stage: ShaderStage, code: &str) -> ShaderReflection {
        ShaderReflection::from_spirv(&compile(stage, code)).expect("反射失败")
    }

    /// OpEntryPoint 的名字后面紧跟 interface id 列表, 必须在 NUL 处收住
    #[test]
    fn test_entry_point_stops_at_first_nul() {
        let reflection = reflect(ShaderStage::Vertex, GBUFFER_VERT);

        assert_eq!(reflection.entry_point, "main");
        assert_eq!(reflection.stage, "Vertex");
    }

    /// -O 剥掉名字之后, UBO 的大小与成员偏移依然要算得出来
    #[test]
    fn test_uniform_buffer_layout() {
        let reflection = reflect(ShaderStage::Vertex, GBUFFER_VERT);

        assert_eq!(reflection.uniform_buffers.len(), 1);
        let ubo = &reflection.uniform_buffers[0];

        assert_eq!(ubo.set, 0);
        assert_eq!(ubo.binding, 0);
        assert_eq!(ubo.size, 192);
        assert_eq!(ubo.members.len(), 3);

        for (index, member) in ubo.members.iter().enumerate() {
            assert_eq!(member.offset, 64 * index as u32);
            assert_eq!(member.size, 64);
            assert_eq!(member.matrix_stride, 16);
            assert_eq!(member.array_size, 0);
            assert_eq!(member.data_type.base_type, BaseType::Float);
            assert_eq!(member.data_type.vec_size, 4);
            assert_eq!(member.data_type.columns, 4);
        }
    }

    /// push constant 块: mat4 + uint = 68 字节, 不是 0
    #[test]
    fn test_push_constant_layout() {
        let reflection = reflect(ShaderStage::Vertex, GBUFFER_VERT);

        let push = reflection
            .push_constants
            .as_ref()
            .expect("push constant 块丢失");

        assert_eq!(push.offset, 0);
        assert_eq!(push.size, 68);
        assert_eq!(push.members.len(), 2);

        assert_eq!(push.members[0].offset, 0);
        assert_eq!(push.members[0].size, 64);
        assert_eq!(push.members[0].matrix_stride, 16);
        assert_eq!(push.members[0].data_type.base_type, BaseType::Float);
        assert_eq!(push.members[0].data_type.columns, 4);

        assert_eq!(push.members[1].offset, 64);
        assert_eq!(push.members[1].size, 4);
        assert_eq!(push.members[1].data_type.base_type, BaseType::UInt);
        assert_eq!(push.members[1].data_type.vec_size, 1);
        assert_eq!(push.members[1].data_type.columns, 1);
    }

    /// 顶点输入的分量数来自 OpTypeVector, 不是一律 vec4
    #[test]
    fn test_vertex_input_types() {
        let reflection = reflect(ShaderStage::Vertex, GBUFFER_VERT);

        // 顺序只取决于 location: 变量在 SPIR-V 里的出现顺序是 0/3/1
        let locations: Vec<u32> = reflection.inputs.iter().map(|v| v.location).collect();
        assert_eq!(locations, vec![0, 1, 3]);

        let expected = [(0u32, 3u32), (1, 3), (3, 2)];
        for (location, vec_size) in expected {
            let input = reflection
                .inputs
                .iter()
                .find(|v| v.location == location)
                .unwrap_or_else(|| panic!("location {} 的顶点输入丢失", location));

            assert_eq!(input.data_type.vec_size, vec_size);
            assert_eq!(input.data_type.base_type, BaseType::Float);
            assert_eq!(input.data_type.columns, 1);
            assert_eq!(input.array_size, 0);
        }
    }

    /// 输出走的是同一条类型解析路径, 同样不能一律报 vec4
    #[test]
    fn test_vertex_output_types() {
        let reflection = reflect(ShaderStage::Vertex, GBUFFER_VERT);

        let expected = [(0u32, 2u32), (1, 3), (2, 4)];
        for (location, vec_size) in expected {
            let output = reflection
                .outputs
                .iter()
                .find(|v| v.location == location)
                .unwrap_or_else(|| panic!("location {} 的输出丢失", location));

            assert_eq!(output.data_type.vec_size, vec_size);
            assert_eq!(output.data_type.base_type, BaseType::Float);
        }

        // gl_Position 没有 Location 装饰, 不该混进 location 列表
        assert_eq!(reflection.outputs.len(), 3);
    }

    /// SSBO 的成员布局与只读标记
    #[test]
    fn test_storage_buffer_layout() {
        let reflection = reflect(ShaderStage::Fragment, BINDLESS_FRAG);

        assert_eq!(reflection.storage_buffers.len(), 1);
        let ssbo = &reflection.storage_buffers[0];

        assert_eq!(ssbo.set, 1);
        assert_eq!(ssbo.binding, 0);
        assert!(ssbo.readonly);
        assert_eq!(ssbo.members.len(), 1);

        let materials = &ssbo.members[0];
        assert_eq!(materials.offset, 0);
        assert_eq!(materials.data_type.base_type, BaseType::Struct);
        // std430 下 Material = vec4 + 4×4 字节 = 32
        assert_eq!(materials.array_stride, 32);
        // 无界数组: 长度与总大小要到绑定时才确定
        assert_eq!(materials.array_size, 0);
        assert_eq!(materials.size, 0);
    }

    /// bindless 数组绑定不能被整条丢掉
    #[test]
    fn test_bindless_texture_array_binding() {
        let reflection = reflect(ShaderStage::Fragment, BINDLESS_FRAG);

        let texture = reflection
            .textures
            .iter()
            .find(|t| t.set == 1 && t.binding == 1)
            .expect("set=1 binding=1 的采样器数组丢失");

        assert_eq!(texture.set, 1);
        assert_eq!(texture.binding, 1);
    }

    /// 从真实字节码里摘掉第一条 OpMemberDecorate Offset
    ///
    /// 改一处真编译出来的模块, 而不是手搓一份假 SPIR-V: 假的既验证不了真实
    /// 布局, 也复现不了"装饰缺一条"这种真实的损坏形态。
    fn strip_first_member_offset(spirv: &[u8]) -> Vec<u8> {
        let mut words: Vec<u32> = spirv
            .chunks_exact(4)
            .map(|chunk| u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
            .collect();

        let mut i = 5;
        while i < words.len() {
            let opcode = words[i] & 0xFFFF;
            let word_count = (words[i] >> 16) as usize;
            assert!(word_count > 0, "字节码损坏");

            // OpMemberDecorate <struct> <member> Offset <value>
            if opcode == 72 && word_count >= 5 && words[i + 3] == DEC_OFFSET {
                words.drain(i..i + word_count);
                return words.iter().flat_map(|w| w.to_le_bytes()).collect();
            }
            i += word_count;
        }

        panic!("字节码里没有 OpMemberDecorate Offset, 测试前提不成立");
    }

    /// 少一条 Offset 装饰时必须报错, 不能把成员默认摆到偏移 0
    #[test]
    fn test_missing_member_offset_is_rejected() {
        let spirv = compile(ShaderStage::Vertex, GBUFFER_VERT);

        // 前提: 完整的字节码本来是能反射的, 差别只在少了那一条装饰
        assert!(ShaderReflection::from_spirv(&spirv).is_ok());

        let stripped = strip_first_member_offset(&spirv);
        assert!(ShaderReflection::from_spirv(&stripped).is_err());
    }

    /// 截断的字节码要报错, 不能安静地反射出半份结果
    #[test]
    fn test_truncated_spirv_is_rejected() {
        let spirv = compile(ShaderStage::Vertex, GBUFFER_VERT);

        // 末尾缺半个字 —— chunks_exact 会不声不响地把它扔掉
        let mut partial_word = spirv.clone();
        partial_word.truncate(spirv.len() - 2);
        assert!(ShaderReflection::from_spirv(&partial_word).is_err());

        // 只剩文件头加一个字 —— 第一条指令 (OpCapability, 两个字) 越过了结尾
        let mut cut_instruction = spirv.clone();
        cut_instruction.truncate(24);
        assert!(ShaderReflection::from_spirv(&cut_instruction).is_err());
    }
}
