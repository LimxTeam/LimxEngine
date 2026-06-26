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

        // 解析 SPIR-V
        let words: Vec<u32> = spirv
            .chunks_exact(4)
            .map(|chunk| u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
            .collect();

        let mut reflection = ShaderReflection::default();

        // 解析指令
        let mut i = 5; // 跳过头部
        let mut names: HashMap<u32, String> = HashMap::new();
        let mut decorations: HashMap<u32, Vec<(u32, Vec<u32>)>> = HashMap::new();
        let mut types: HashMap<u32, SpirvType> = HashMap::new();
        let mut variables: HashMap<u32, SpirvVariable> = HashMap::new();
        let mut constants: HashMap<u32, u32> = HashMap::new();

        while i < words.len() {
            let instruction = words[i];
            let opcode = instruction & 0xFFFF;
            let word_count = (instruction >> 16) as usize;

            if word_count == 0 || i + word_count > words.len() {
                break;
            }

            match opcode {
                // OpName
                5 => {
                    if word_count > 2 {
                        let id = words[i + 1];
                        let name = parse_spirv_string(&words[i + 2..i + word_count]);
                        names.insert(id, name);
                    }
                }
                // OpMemberName
                6 => {
                    // 成员名称，暂时跳过
                }
                // OpDecorate
                71 => {
                    if word_count >= 3 {
                        let id = words[i + 1];
                        let decoration = words[i + 2];
                        let operands: Vec<u32> = words[i + 3..i + word_count].to_vec();
                        decorations
                            .entry(id)
                            .or_default()
                            .push((decoration, operands));
                    }
                }
                // OpTypeVoid
                19 => {
                    let id = words[i + 1];
                    types.insert(id, SpirvType::Void);
                }
                // OpTypeBool
                20 => {
                    let id = words[i + 1];
                    types.insert(id, SpirvType::Bool);
                }
                // OpTypeInt
                21 => {
                    let id = words[i + 1];
                    let width = words[i + 2];
                    let signed = words[i + 3] != 0;
                    types.insert(id, SpirvType::Int { width, signed });
                }
                // OpTypeFloat
                22 => {
                    let id = words[i + 1];
                    let width = words[i + 2];
                    types.insert(id, SpirvType::Float { width });
                }
                // OpTypeVector
                23 => {
                    let id = words[i + 1];
                    let component_type = words[i + 2];
                    let count = words[i + 3];
                    types.insert(
                        id,
                        SpirvType::Vector {
                            component_type,
                            count,
                        },
                    );
                }
                // OpTypeMatrix
                24 => {
                    let id = words[i + 1];
                    let column_type = words[i + 2];
                    let columns = words[i + 3];
                    types.insert(
                        id,
                        SpirvType::Matrix {
                            column_type,
                            columns,
                        },
                    );
                }
                // OpTypeImage
                25 => {
                    let id = words[i + 1];
                    types.insert(id, SpirvType::Image);
                }
                // OpTypeSampler
                26 => {
                    let id = words[i + 1];
                    types.insert(id, SpirvType::Sampler);
                }
                // OpTypeSampledImage
                27 => {
                    let id = words[i + 1];
                    types.insert(id, SpirvType::SampledImage);
                }
                // OpTypeArray
                28 => {
                    let id = words[i + 1];
                    let element_type = words[i + 2];
                    let length_id = words[i + 3];
                    let length = constants.get(&length_id).copied().unwrap_or(0);
                    types.insert(
                        id,
                        SpirvType::Array {
                            element_type,
                            length,
                        },
                    );
                }
                // OpTypeStruct
                30 => {
                    let id = words[i + 1];
                    let members: Vec<u32> = words[i + 2..i + word_count].to_vec();
                    types.insert(id, SpirvType::Struct { members });
                }
                // OpTypePointer
                32 => {
                    let id = words[i + 1];
                    let storage_class = words[i + 2];
                    let pointee = words[i + 3];
                    types.insert(
                        id,
                        SpirvType::Pointer {
                            storage_class,
                            pointee,
                        },
                    );
                }
                // OpConstant
                43 => {
                    if word_count >= 4 {
                        let id = words[i + 2];
                        let value = words[i + 3];
                        constants.insert(id, value);
                    }
                }
                // OpVariable
                59 => {
                    let type_id = words[i + 1];
                    let id = words[i + 2];
                    let storage_class = words[i + 3];
                    variables.insert(
                        id,
                        SpirvVariable {
                            type_id,
                            storage_class,
                        },
                    );
                }
                // OpEntryPoint
                15 => {
                    if word_count >= 4 {
                        let execution_model = words[i + 1];
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
                _ => {}
            }

            i += word_count;
        }

        // 处理变量
        for (id, var) in &variables {
            let name = names.get(id).cloned().unwrap_or_default();
            let decors = decorations.get(id).cloned().unwrap_or_default();

            let mut location = None;
            let mut binding = None;
            let mut set = None;
            let mut input_attachment_index = None;

            for (decor, operands) in &decors {
                match *decor {
                    30 => location = operands.first().copied(), // Location
                    33 => binding = operands.first().copied(),  // Binding
                    34 => set = operands.first().copied(),      // DescriptorSet
                    43 => input_attachment_index = operands.first().copied(), // InputAttachmentIndex
                    _ => {}
                }
            }

            match var.storage_class {
                // Input
                1 => {
                    if let Some(loc) = location {
                        reflection.inputs.push(ShaderVariable {
                            name,
                            location: loc,
                            data_type: DataType {
                                base_type: BaseType::Float,
                                vec_size: 4,
                                columns: 1,
                            },
                            array_size: 0,
                        });
                    }
                }
                // Output
                3 => {
                    if let Some(loc) = location {
                        reflection.outputs.push(ShaderVariable {
                            name,
                            location: loc,
                            data_type: DataType {
                                base_type: BaseType::Float,
                                vec_size: 4,
                                columns: 1,
                            },
                            array_size: 0,
                        });
                    }
                }
                // Uniform
                2 => {
                    if let (Some(s), Some(b)) = (set, binding) {
                        reflection.uniform_buffers.push(UniformBuffer {
                            name,
                            set: s,
                            binding: b,
                            size: 0,
                            members: Vec::new(),
                        });
                    }
                }
                // StorageBuffer
                12 => {
                    if let (Some(s), Some(b)) = (set, binding) {
                        reflection.storage_buffers.push(StorageBuffer {
                            name,
                            set: s,
                            binding: b,
                            readonly: false,
                            members: Vec::new(),
                        });
                    }
                }
                // PushConstant
                9 => {
                    reflection.push_constants = Some(PushConstantRange {
                        offset: 0,
                        size: 0,
                        members: Vec::new(),
                    });
                }
                // UniformConstant (samplers, textures)
                0 => {
                    if let (Some(s), Some(b)) = (set, binding) {
                        // 判断是采样器还是纹理
                        if let Some(SpirvType::Pointer { pointee, .. }) = types.get(&var.type_id) {
                            match types.get(pointee) {
                                Some(SpirvType::Sampler) => {
                                    reflection.samplers.push(SamplerBinding {
                                        name,
                                        set: s,
                                        binding: b,
                                    });
                                }
                                Some(SpirvType::SampledImage) | Some(SpirvType::Image) => {
                                    reflection.textures.push(TextureBinding {
                                        name,
                                        set: s,
                                        binding: b,
                                        dimension: TextureDimension::Tex2D,
                                        multisampled: false,
                                        arrayed: false,
                                    });
                                }
                                _ => {}
                            }
                        }
                    }
                }
                _ => {}
            }
        }

        Ok(reflection)
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
    Array { element_type: u32, length: u32 },
    Struct { members: Vec<u32> },
    Pointer { storage_class: u32, pointee: u32 },
}

/// SPIR-V 变量
#[derive(Debug, Clone)]
struct SpirvVariable {
    type_id: u32,
    storage_class: u32,
}

/// 解析 SPIR-V 字符串
fn parse_spirv_string(words: &[u32]) -> String {
    let mut bytes = Vec::new();
    for word in words {
        let word_bytes = word.to_le_bytes();
        for &b in &word_bytes {
            if b == 0 {
                break;
            }
            bytes.push(b);
        }
    }
    String::from_utf8_lossy(&bytes).to_string()
}
