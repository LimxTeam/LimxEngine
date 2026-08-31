/*******************************************************************************
 * 文件: dds.rs
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   DDS (DirectDraw Surface) 容器的写出与解析, 固定使用 DX10 扩展头。
 *
 * 为什么是 DDS 而不是自定义格式:
 *   烘焙链路出错时最贵的一件事是"看不见"。DDS 能被 RenderDoc、
 *   Compressonator、texconv、Windows 照片查看器直接打开, 一张烘歪的
 *   法线图肉眼一眼就能看出来。自定义容器省下的那几十字节头, 换不来
 *   这个。
 *
 * 为什么必须是 DX10 头:
 *   老式的 FourCC 头 ('DXT1'/'DXT5'/'ATI1'/'ATI2') 没有地方表达
 *   sRGB。同一份 BC1 数据是 UNORM 还是 UNORM_SRGB, 在 FourCC 头里
 *   完全无法区分 —— 读取方只能靠猜, 而猜错的后果 (整体偏亮/偏暗)
 *   不会有任何报错。DX10 头里的 dxgiFormat 字段把这件事写死。
 *
 * 头里各字段约束了什么:
 *   dwSize / ddspf.dwSize     必须分别是 124 / 32。它们是版本探针,
 *                             读取方靠它确认自己没有读错结构体。
 *   dwFlags                   声明"哪些字段有效"。压缩纹理必须置
 *                             DDSD_LINEARSIZE 而不是 DDSD_PITCH ——
 *                             块压缩没有"每行字节数"这个概念。
 *   dwPitchOrLinearSize       在 LINEARSIZE 语义下 = 第 0 层的总字节数。
 *   dwMipMapCount             层数; 与 DDSD_MIPMAPCOUNT / DDSCAPS_MIPMAP
 *                             / DDSCAPS_COMPLEX 三个标志必须自洽,
 *                             否则一部分读取方只读第 0 层就返回。
 *   ddspf.dwFourCC            'DX10', 表示后面还有 20 字节扩展头。
 *   dxgiFormat                真正的格式 (含 sRGB 与否)。
 *   resourceDimension         3 = TEXTURE2D。
 *   arraySize                 1; 立方体贴图/数组这一轮不做, 解析时
 *                             遇到 >1 直接报错而不是静默只读第一片。
 *   miscFlags2                alpha 模式。BC3 写 STRAIGHT (未预乘) ——
 *                             我们没有做预乘, 声明成 PREMULTIPLIED 会
 *                             让正确的读取方把颜色再除一遍 alpha。
 *
 ******************************************************************************/

use anyhow::{bail, ensure, Context, Result};

use crate::format::{dxgi_block_bytes, dxgi_format_name, BcFormat, ColorSpace};
use crate::mip::{mip_level_count, next_mip_size};

pub const DDS_MAGIC: u32 = 0x2053_4444; // "DDS "
pub const FOURCC_DX10: u32 = 0x3031_5844; // "DX10"

pub const DDS_HEADER_SIZE: u32 = 124;
pub const DDS_PIXELFORMAT_SIZE: u32 = 32;
/// 4 (magic) + 124 (DDS_HEADER) + 20 (DDS_HEADER_DXT10)
pub const DDS_TOTAL_HEADER_SIZE: usize = 148;

// DDSD_* — dwFlags
const DDSD_CAPS: u32 = 0x1;
const DDSD_HEIGHT: u32 = 0x2;
const DDSD_WIDTH: u32 = 0x4;
const DDSD_PIXELFORMAT: u32 = 0x1000;
const DDSD_MIPMAPCOUNT: u32 = 0x2_0000;
const DDSD_LINEARSIZE: u32 = 0x8_0000;

// DDPF_* — ddspf.dwFlags
const DDPF_FOURCC: u32 = 0x4;

// DDSCAPS_* — dwCaps
const DDSCAPS_COMPLEX: u32 = 0x8;
const DDSCAPS_TEXTURE: u32 = 0x1000;
const DDSCAPS_MIPMAP: u32 = 0x40_0000;

// D3D10_RESOURCE_DIMENSION
const RESOURCE_DIMENSION_TEXTURE2D: u32 = 3;

// DDS_ALPHA_MODE
const DDS_ALPHA_MODE_UNKNOWN: u32 = 0;
const DDS_ALPHA_MODE_STRAIGHT: u32 = 1;

/// 从 DDS 文件里解析出来的、我们关心的全部字段。
/// 写出与读回都走这一个结构, 往返测试因此能逐字段核对。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DdsInfo {
    pub width: u32,
    pub height: u32,
    pub mip_count: u32,
    pub dxgi_format: u32,
    pub flags: u32,
    pub caps: u32,
    pub linear_size: u32,
    pub resource_dimension: u32,
    pub array_size: u32,
    pub misc_flags2: u32,
    /// 逐层字节数, 长度 == mip_count
    pub level_sizes: Vec<usize>,
    /// 逐层尺寸, 长度 == mip_count
    pub level_dims: Vec<(u32, u32)>,
    /// 载荷起始偏移 (恒为 DDS_TOTAL_HEADER_SIZE, 显式记下来便于 inspect 打印)
    pub payload_offset: usize,
}

/// 一张待写出的 BC 纹理: 头部参数 + 已压缩好的逐层数据。
pub struct DdsTexture {
    pub width: u32,
    pub height: u32,
    pub format: BcFormat,
    pub color_space: ColorSpace,
    /// 逐层压缩数据, `levels[0]` 是第 0 层
    pub levels: Vec<Vec<u8>>,
}

impl DdsTexture {
    /// 序列化成完整的 DDS 字节流。
    ///
    /// 返回 Result 而不是直接 Vec: 层数/层大小与声明的尺寸不符是
    /// 上游 bug, 必须在写盘之前拦住 —— 写出去之后再发现, 错误就变成
    /// 一堆肉眼看着"差不多"的文件了。
    pub fn encode(&self) -> Result<Vec<u8>> {
        ensure!(
            self.width > 0 && self.height > 0,
            "纹理尺寸不能为 0 (得到 {}×{})",
            self.width,
            self.height
        );
        ensure!(!self.levels.is_empty(), "mip 链为空, 至少要有第 0 层");

        let dxgi = self.format.dxgi_format(self.color_space).with_context(|| {
            format!(
                "{} 与色彩空间 {:?} 的组合没有对应的 DXGI 格式",
                self.format.name(),
                self.color_space
            )
        })?;

        // DDS 允许写出不完整的 mip 链 (dwMipMapCount 说了算), 所以这里
        // 只卡上限。"链是否应该是完整的" 是烘焙策略的问题, 由 bake() 负责,
        // 容器层不该越权 —— 但超过上限一定是块公式或降采样写错了。
        let expected_levels = mip_level_count(self.width, self.height) as usize;
        ensure!(
            self.levels.len() <= expected_levels,
            "mip 层数超过上限: {}×{} 最多 {} 层, 实际 {} 层",
            self.width,
            self.height,
            expected_levels,
            self.levels.len()
        );

        // 逐层核对字节数。这一步是最后一道能拦住"块公式算错"的关卡,
        // 而且它比读盘时再发现要早得多。
        let mut w = self.width;
        let mut h = self.height;
        for (i, level) in self.levels.iter().enumerate() {
            let expect = self.format.level_byte_size(w, h);
            ensure!(
                level.len() == expect,
                "第 {} 层 ({}×{}) 字节数不符: {} 格式应为 {} 字节, 实际 {} 字节",
                i,
                w,
                h,
                self.format.name(),
                expect,
                level.len()
            );
            w = next_mip_size(w);
            h = next_mip_size(h);
        }

        let mip_count = self.levels.len() as u32;
        let linear_size = self.levels[0].len() as u32;

        let mut flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
        let mut caps = DDSCAPS_TEXTURE;
        if mip_count > 1 {
            // 三个标志要一起置。只置 dwMipMapCount 而漏掉 DDSCAPS_MIPMAP,
            // 一部分读取方 (含 D3DX 的老实现) 会当成单层纹理。
            flags |= DDSD_MIPMAPCOUNT;
            caps |= DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
        }

        // 能携带 alpha 的格式都要声明 alpha 是直通 (未预乘) 的。
        // 我们没有做预乘, 声明成 PREMULTIPLIED 会让正确的读取方把颜色
        // 再除一遍 alpha。BC1/BC4/BC5 没有 alpha 通道, 写 UNKNOWN。
        let misc_flags2 = match self.format {
            BcFormat::Bc3 | BcFormat::Bc7 => DDS_ALPHA_MODE_STRAIGHT,
            _ => DDS_ALPHA_MODE_UNKNOWN,
        };

        let payload_bytes: usize = self.levels.iter().map(|l| l.len()).sum();
        let mut out = Vec::with_capacity(DDS_TOTAL_HEADER_SIZE + payload_bytes);

        let mut push = |v: u32| out.extend_from_slice(&v.to_le_bytes());

        push(DDS_MAGIC);
        // ── DDS_HEADER (124 字节 = 31 个 u32) ──
        push(DDS_HEADER_SIZE);
        push(flags);
        push(self.height); // 注意: height 在 width 之前
        push(self.width);
        push(linear_size);
        push(0); // dwDepth — 2D 纹理写 0
        push(mip_count);
        for _ in 0..11 {
            push(0); // dwReserved1[11]
        }
        // ── DDS_PIXELFORMAT (32 字节 = 8 个 u32) ──
        push(DDS_PIXELFORMAT_SIZE);
        push(DDPF_FOURCC);
        push(FOURCC_DX10);
        push(0); // dwRGBBitCount
        push(0); // dwRBitMask
        push(0); // dwGBitMask
        push(0); // dwBBitMask
        push(0); // dwABitMask
        // ── 回到 DDS_HEADER 尾部 ──
        push(caps);
        push(0); // dwCaps2
        push(0); // dwCaps3
        push(0); // dwCaps4
        push(0); // dwReserved2
        // ── DDS_HEADER_DXT10 (20 字节 = 5 个 u32) ──
        push(dxgi);
        push(RESOURCE_DIMENSION_TEXTURE2D);
        push(0); // miscFlag (立方体贴图标志, 这一轮不用)
        push(1); // arraySize
        push(misc_flags2);

        debug_assert_eq!(out.len(), DDS_TOTAL_HEADER_SIZE);

        for level in &self.levels {
            out.extend_from_slice(level);
        }

        Ok(out)
    }
}

#[inline]
fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

/// 解析 DDS 字节流。
///
/// 每一条失败都带上具体数值 —— "DDS 解析失败" 这种信息在批量烘焙的
/// 日志里毫无用处。
pub fn parse(bytes: &[u8]) -> Result<DdsInfo> {
    ensure!(
        bytes.len() >= DDS_TOTAL_HEADER_SIZE,
        "文件太小: {} 字节, DX10 DDS 的头部就要 {} 字节",
        bytes.len(),
        DDS_TOTAL_HEADER_SIZE
    );

    let magic = read_u32(bytes, 0);
    ensure!(
        magic == DDS_MAGIC,
        "魔数不是 'DDS ' (期望 0x{:08X}, 实际 0x{:08X}) —— 这不是一个 DDS 文件",
        DDS_MAGIC,
        magic
    );

    let header_size = read_u32(bytes, 4);
    ensure!(
        header_size == DDS_HEADER_SIZE,
        "DDS_HEADER.dwSize 应为 {}, 实际 {} —— 文件结构与规范不符",
        DDS_HEADER_SIZE,
        header_size
    );

    let flags = read_u32(bytes, 8);
    let height = read_u32(bytes, 12);
    let width = read_u32(bytes, 16);
    let linear_size = read_u32(bytes, 20);
    let mip_count_raw = read_u32(bytes, 28);

    let pf_size = read_u32(bytes, 76);
    ensure!(
        pf_size == DDS_PIXELFORMAT_SIZE,
        "DDS_PIXELFORMAT.dwSize 应为 {}, 实际 {}",
        DDS_PIXELFORMAT_SIZE,
        pf_size
    );

    let pf_flags = read_u32(bytes, 80);
    let fourcc = read_u32(bytes, 84);
    ensure!(
        pf_flags & DDPF_FOURCC != 0,
        "DDS_PIXELFORMAT.dwFlags 没有 DDPF_FOURCC(0x4), 这是一张未压缩纹理; lat 只处理块压缩纹理"
    );
    ensure!(
        fourcc == FOURCC_DX10,
        "FourCC 是 '{}' 而不是 'DX10' —— 老式 FourCC 头无法表达 sRGB, lat 不读它。\n  \
         用 texconv -f <格式> 转成 DX10 头后再试。",
        fourcc_to_string(fourcc)
    );

    let caps = read_u32(bytes, 108);

    let dxgi_format = read_u32(bytes, 128);
    let resource_dimension = read_u32(bytes, 132);
    let _misc_flag = read_u32(bytes, 136);
    let array_size = read_u32(bytes, 140);
    let misc_flags2 = read_u32(bytes, 144);

    ensure!(
        resource_dimension == RESOURCE_DIMENSION_TEXTURE2D,
        "resourceDimension = {} (期望 3 = TEXTURE2D); 1D/3D 纹理这一轮不支持",
        resource_dimension
    );
    ensure!(
        array_size == 1,
        "arraySize = {} (期望 1); 纹理数组与立方体贴图这一轮不支持 —— \
         静默只读第一片会让后续的显存统计全错",
        array_size
    );
    ensure!(width > 0 && height > 0, "尺寸非法: {}×{}", width, height);

    let block_bytes = dxgi_block_bytes(dxgi_format).with_context(|| {
        format!(
            "dxgiFormat = {} 不是 lat 认识的块压缩格式{}",
            dxgi_format,
            match dxgi_format_name(dxgi_format) {
                Some(n) => format!(" ({})", n),
                None => String::new(),
            }
        )
    })?;

    // mipMapCount 为 0 的 DDS 在野外确实存在, 语义是"只有第 0 层"。
    let mip_count = mip_count_raw.max(1);
    let max_levels = mip_level_count(width, height);
    ensure!(
        mip_count <= max_levels,
        "dwMipMapCount = {} 超过 {}×{} 的上限 {} 层",
        mip_count,
        width,
        height,
        max_levels
    );

    // 按块公式算出每层应有多少字节, 再和文件实际长度对账。
    // 这一步能同时抓住"文件被截断"和"头里的尺寸与载荷不匹配"。
    let mut level_sizes = Vec::with_capacity(mip_count as usize);
    let mut level_dims = Vec::with_capacity(mip_count as usize);
    let (mut w, mut h) = (width, height);
    for _ in 0..mip_count {
        let size = w.div_ceil(4) as usize * h.div_ceil(4) as usize * block_bytes;
        level_sizes.push(size);
        level_dims.push((w, h));
        w = next_mip_size(w);
        h = next_mip_size(h);
    }

    let expected_total: usize = DDS_TOTAL_HEADER_SIZE + level_sizes.iter().sum::<usize>();
    if bytes.len() < expected_total {
        bail!(
            "载荷被截断: 头部声明 {}×{} / {} 层 / {}, 需要 {} 字节, 文件只有 {} 字节 (缺 {})",
            width,
            height,
            mip_count,
            dxgi_format_name(dxgi_format).unwrap_or("未知格式"),
            expected_total,
            bytes.len(),
            expected_total - bytes.len()
        );
    }

    if bytes.len() > expected_total {
        // 多出来的字节不当作错误 (有些工具在尾部塞元数据), 但要能被看见。
        tracing::warn!(
            "文件尾部有 {} 字节多余数据 (期望 {} 字节, 实际 {} 字节)",
            bytes.len() - expected_total,
            expected_total,
            bytes.len()
        );
    }

    // dwPitchOrLinearSize 在 DDSD_LINEARSIZE 语义下必须等于第 0 层大小。
    // 不一致说明写出方的块公式和读取方的不一样 —— 正是我们要提前发现的。
    if flags & DDSD_LINEARSIZE != 0 && linear_size as usize != level_sizes[0] {
        bail!(
            "dwPitchOrLinearSize = {} 与按块公式算出的第 0 层大小 {} 不符",
            linear_size,
            level_sizes[0]
        );
    }

    Ok(DdsInfo {
        width,
        height,
        mip_count,
        dxgi_format,
        flags,
        caps,
        linear_size,
        resource_dimension,
        array_size,
        misc_flags2,
        level_sizes,
        level_dims,
        payload_offset: DDS_TOTAL_HEADER_SIZE,
    })
}

fn fourcc_to_string(v: u32) -> String {
    v.to_le_bytes()
        .iter()
        .map(|&b| {
            if (0x20..0x7f).contains(&b) {
                b as char
            } else {
                '?'
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::format::{
        DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_BC5_UNORM,
    };

    /// 造一张层数/层大小都正确的假纹理 (内容是 0, 我们这里只关心头)。
    fn fake_texture(width: u32, height: u32, format: BcFormat, cs: ColorSpace) -> DdsTexture {
        let mut levels = Vec::new();
        let (mut w, mut h) = (width, height);
        for _ in 0..mip_level_count(width, height) {
            levels.push(vec![0u8; format.level_byte_size(w, h)]);
            w = next_mip_size(w);
            h = next_mip_size(h);
        }
        DdsTexture {
            width,
            height,
            format,
            color_space: cs,
            levels,
        }
    }

    #[test]
    fn header_roundtrip_field_by_field() {
        let tex = fake_texture(1024, 1024, BcFormat::Bc1, ColorSpace::Srgb);
        let bytes = tex.encode().unwrap();
        assert_eq!(&bytes[..4], b"DDS ");
        assert_eq!(bytes.len(), DDS_TOTAL_HEADER_SIZE + 699_064);

        let info = parse(&bytes).unwrap();

        assert_eq!(info.width, 1024);
        assert_eq!(info.height, 1024);
        assert_eq!(info.mip_count, 11);
        assert_eq!(info.dxgi_format, DXGI_FORMAT_BC1_UNORM_SRGB);
        assert_eq!(info.resource_dimension, RESOURCE_DIMENSION_TEXTURE2D);
        assert_eq!(info.array_size, 1);
        assert_eq!(info.misc_flags2, DDS_ALPHA_MODE_UNKNOWN);
        assert_eq!(info.linear_size, 512 * 1024);
        assert_eq!(info.level_sizes[0], 512 * 1024);
        assert_eq!(info.payload_offset, DDS_TOTAL_HEADER_SIZE);

        // 标志位逐个核对: 少置任何一个都会让某些读取方行为不同。
        for (bit, name) in [
            (DDSD_CAPS, "DDSD_CAPS"),
            (DDSD_HEIGHT, "DDSD_HEIGHT"),
            (DDSD_WIDTH, "DDSD_WIDTH"),
            (DDSD_PIXELFORMAT, "DDSD_PIXELFORMAT"),
            (DDSD_LINEARSIZE, "DDSD_LINEARSIZE"),
            (DDSD_MIPMAPCOUNT, "DDSD_MIPMAPCOUNT"),
        ] {
            assert!(info.flags & bit != 0, "dwFlags 缺少 {}", name);
        }
        for (bit, name) in [
            (DDSCAPS_TEXTURE, "DDSCAPS_TEXTURE"),
            (DDSCAPS_MIPMAP, "DDSCAPS_MIPMAP"),
            (DDSCAPS_COMPLEX, "DDSCAPS_COMPLEX"),
        ] {
            assert!(info.caps & bit != 0, "dwCaps 缺少 {}", name);
        }

        // 逐层尺寸与字节数
        let expected_dims: Vec<(u32, u32)> = (0..11)
            .map(|i| {
                let s = (1024u32 >> i).max(1);
                (s, s)
            })
            .collect();
        assert_eq!(info.level_dims, expected_dims);
        for (i, ((w, h), size)) in info
            .level_dims
            .iter()
            .zip(info.level_sizes.iter())
            .enumerate()
        {
            assert_eq!(
                *size,
                BcFormat::Bc1.level_byte_size(*w, *h),
                "第 {} 层字节数不符",
                i
            );
        }
        assert_eq!(info.level_sizes.iter().sum::<usize>(), 699_064);
    }

    #[test]
    fn header_roundtrip_bc3_marks_straight_alpha() {
        let tex = fake_texture(64, 64, BcFormat::Bc3, ColorSpace::Srgb);
        let info = parse(&tex.encode().unwrap()).unwrap();
        assert_eq!(info.dxgi_format, DXGI_FORMAT_BC3_UNORM_SRGB);
        assert_eq!(
            info.misc_flags2, DDS_ALPHA_MODE_STRAIGHT,
            "BC3 必须声明直通 alpha, 否则读取方可能再除一次 alpha"
        );
    }

    #[test]
    fn header_roundtrip_non_power_of_two() {
        // 300×173 是本工具最容易算错的形状: 每一层的块数都要各自向上取整。
        let tex = fake_texture(300, 173, BcFormat::Bc5, ColorSpace::Linear);
        let bytes = tex.encode().unwrap();
        let info = parse(&bytes).unwrap();

        assert_eq!(info.dxgi_format, DXGI_FORMAT_BC5_UNORM);
        assert_eq!(info.mip_count, 9);
        assert_eq!(info.level_dims[0], (300, 173));
        assert_eq!(info.level_dims[8], (1, 1));
        assert_eq!(info.level_sizes[0], 75 * 44 * 16);
        assert_eq!(info.level_sizes[8], 16, "1×1 层仍占满一个 BC5 块");
        assert_eq!(
            bytes.len(),
            DDS_TOTAL_HEADER_SIZE + info.level_sizes.iter().sum::<usize>()
        );
    }

    #[test]
    fn single_level_texture_omits_mipmap_caps() {
        // 1×1 只有一层, 此时不该置 MIPMAP/COMPLEX —— 置了会让读取方
        // 去找并不存在的后续层。
        let tex = fake_texture(1, 1, BcFormat::Bc1, ColorSpace::Linear);
        let info = parse(&tex.encode().unwrap()).unwrap();
        assert_eq!(info.mip_count, 1);
        assert_eq!(info.caps & DDSCAPS_MIPMAP, 0);
        assert_eq!(info.caps & DDSCAPS_COMPLEX, 0);
        assert_eq!(info.flags & DDSD_MIPMAPCOUNT, 0);
        assert_eq!(info.level_sizes, vec![8]);
    }

    // ── 下面这一组证明解析器的检查真的能失败 ──────────────────────────
    // 每条都从一份 *合法* 的字节流出发, 只改一个地方。
    // 如果哪天有人把某条 ensure! 删掉, 对应的用例立刻变红。

    fn valid_bytes() -> Vec<u8> {
        fake_texture(256, 256, BcFormat::Bc1, ColorSpace::Srgb)
            .encode()
            .unwrap()
    }

    #[test]
    fn baseline_is_actually_valid() {
        // 对照组的对照组: 如果这条失败, 下面所有"必须失败"的用例都没有意义。
        assert!(parse(&valid_bytes()).is_ok());
    }

    #[test]
    fn rejects_bad_magic() {
        let mut b = valid_bytes();
        b[0] = b'X';
        let e = parse(&b).unwrap_err().to_string();
        assert!(e.contains("魔数"), "错误信息应指出魔数问题, 实际: {}", e);
    }

    #[test]
    fn rejects_truncated_payload() {
        let mut b = valid_bytes();
        let full = b.len();
        b.truncate(full - 1);
        let e = parse(&b).unwrap_err().to_string();
        assert!(e.contains("截断"), "错误信息应指出截断, 实际: {}", e);
        assert!(e.contains("缺 1"), "错误信息应给出缺多少字节, 实际: {}", e);
    }

    #[test]
    fn rejects_too_small_for_header() {
        let e = parse(&[0u8; 16]).unwrap_err().to_string();
        assert!(e.contains("太小"), "实际: {}", e);
    }

    #[test]
    fn rejects_legacy_fourcc() {
        let mut b = valid_bytes();
        b[84..88].copy_from_slice(b"DXT1");
        let e = parse(&b).unwrap_err().to_string();
        assert!(e.contains("DXT1") && e.contains("DX10"), "实际: {}", e);
    }

    #[test]
    fn rejects_wrong_header_size() {
        let mut b = valid_bytes();
        b[4..8].copy_from_slice(&125u32.to_le_bytes());
        assert!(parse(&b).is_err());
    }

    #[test]
    fn rejects_wrong_pixelformat_size() {
        let mut b = valid_bytes();
        b[76..80].copy_from_slice(&24u32.to_le_bytes());
        assert!(parse(&b).is_err());
    }

    #[test]
    fn rejects_texture_array() {
        let mut b = valid_bytes();
        b[140..144].copy_from_slice(&6u32.to_le_bytes());
        let e = parse(&b).unwrap_err().to_string();
        assert!(e.contains("arraySize"), "实际: {}", e);
    }

    #[test]
    fn rejects_unknown_dxgi_format() {
        let mut b = valid_bytes();
        b[128..132].copy_from_slice(&28u32.to_le_bytes()); // R8G8B8A8_UNORM
        let e = format!("{:#}", parse(&b).unwrap_err());
        assert!(e.contains("28"), "实际: {}", e);
    }

    #[test]
    fn rejects_linear_size_mismatch() {
        let mut b = valid_bytes();
        b[20..24].copy_from_slice(&12345u32.to_le_bytes());
        let e = parse(&b).unwrap_err().to_string();
        assert!(e.contains("dwPitchOrLinearSize"), "实际: {}", e);
    }

    #[test]
    fn rejects_mip_count_over_limit() {
        let mut b = valid_bytes();
        b[28..32].copy_from_slice(&12u32.to_le_bytes()); // 256×256 上限是 9
        let e = parse(&b).unwrap_err().to_string();
        assert!(e.contains("上限"), "实际: {}", e);
    }

    // ── 写出侧的自检也要能失败 ────────────────────────────────────────

    #[test]
    fn encode_rejects_too_many_levels() {
        // 256×256 上限 9 层。多写一层的 1×1 块大小正好也是 8 字节, 逐层
        // 尺寸检查抓不住它 —— 必须有独立的层数上限检查。
        let mut tex = fake_texture(256, 256, BcFormat::Bc1, ColorSpace::Srgb);
        assert_eq!(tex.levels.len(), 9);
        tex.levels.push(vec![0u8; 8]);
        let e = tex.encode().unwrap_err().to_string();
        assert!(e.contains("超过上限"), "实际: {}", e);
    }

    #[test]
    fn encode_allows_partial_chain() {
        // 只写第 0 层是合法的 DDS (--no-mipmaps 就走这条路)。
        let mut tex = fake_texture(256, 256, BcFormat::Bc1, ColorSpace::Srgb);
        tex.levels.truncate(1);
        let info = parse(&tex.encode().unwrap()).unwrap();
        assert_eq!(info.mip_count, 1);
    }

    #[test]
    fn encode_rejects_wrong_level_size() {
        let mut tex = fake_texture(256, 256, BcFormat::Bc1, ColorSpace::Srgb);
        tex.levels[3].push(0);
        let e = tex.encode().unwrap_err().to_string();
        assert!(e.contains("字节数不符"), "实际: {}", e);
    }

    #[test]
    fn encode_rejects_srgb_bc5() {
        let tex = fake_texture(64, 64, BcFormat::Bc5, ColorSpace::Srgb);
        assert!(
            tex.encode().is_err(),
            "BC5 + sRGB 没有对应 DXGI 格式, 必须在写盘前失败"
        );
    }
}
