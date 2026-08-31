/*******************************************************************************
 * 文件: format.rs
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   BC 格式的选择策略、块尺寸算术、以及到 DXGI 格式号的映射。
 *
 * 设计要点:
 *   这一层刻意不认识"文件名"。格式与色彩空间由调用方通过命令行显式给出,
 *   工具只根据 *像素数据本身* 的通道数与 alpha 是否有效来做剩下的决定。
 *   —— 从 "xxx_normal.png" 猜语义在资产管线里是经典的踩坑点: 一旦有人
 *   改名或换导出器, 法线图会被静默当成 albedo 烘成 sRGB BC1, 而这个错误
 *   不会崩、不会报警, 只会让光照微妙地不对。
 *
 ******************************************************************************/

use anyhow::{bail, Result};
use block_compression::CompressionVariant;

/// 输出的 BC 格式。
///
/// 这一轮只做 BC1/BC3/BC4/BC5 —— 它们共用同一套 RGB565 + 索引 / 单通道插值
/// 的结构, 编码器是纯算术的, 跑通全链路后拿到的数字才可信。BC7 的搜索空间
/// (8 种 mode × partition) 大一个量级, 单独作为第二步。
#[derive(Copy, Clone, Debug, PartialEq, Eq, clap::ValueEnum)]
pub enum BcFormat {
    /// RGB, 每 4×4 块 8 字节。无 alpha (1-bit 抠图 alpha 不使用)
    Bc1,
    /// RGBA, 每 4×4 块 16 字节。8 字节 alpha 块 + 8 字节 BC1 颜色块
    Bc3,
    /// 单通道 (R), 每 4×4 块 8 字节
    Bc4,
    /// 双通道 (RG), 每 4×4 块 16 字节 = 两个 BC4 块
    Bc5,
}

/// 命令行上的格式选项: `auto` 走策略, 其余是显式指定。
#[derive(Copy, Clone, Debug, PartialEq, Eq, clap::ValueEnum)]
pub enum FormatOption {
    Auto,
    Bc1,
    Bc3,
    Bc4,
    Bc5,
}

/// 源图的色彩空间。必须由调用方显式指定, 没有默认值。
///
/// 它同时决定两件事, 两件都不可省:
///   1. DDS 头里写 `BC1_UNORM` 还是 `BC1_UNORM_SRGB` —— 采样时硬件是否做
///      电光转换, 直接影响最终亮度;
///   2. mip 链降采样时是否先转到线性光再平均 —— 对 sRGB 编码值直接取平均
///      会把结果推向偏亮 (因为 sRGB 曲线是凹的), 远处的 mip 会整体发灰。
#[derive(Copy, Clone, Debug, PartialEq, Eq, clap::ValueEnum)]
pub enum ColorSpace {
    /// albedo / emissive / baseColor —— 存的是给人眼看的颜色
    Srgb,
    /// normal / metallicRoughness / occlusion / height —— 存的是数值
    Linear,
}

impl BcFormat {
    /// 每个 4×4 块占多少字节。
    ///
    /// BC1/BC4 = 8 字节: 两个端点 + 每像素 2 bit 索引。
    /// BC3 = BC4 的 alpha 块(8) + BC1 的颜色块(8)。
    /// BC5 = 两个 BC4 块 (R 一个, G 一个)。
    pub const fn block_bytes(self) -> usize {
        match self {
            BcFormat::Bc1 | BcFormat::Bc4 => 8,
            BcFormat::Bc3 | BcFormat::Bc5 => 16,
        }
    }

    /// 一个 mip 层压缩后的字节数。
    ///
    /// 宽高各自向上取整到 4 的倍数再除以 4 —— BC 的最小单位是块, 不是像素。
    /// 一张 1×1 的 mip 层仍然要占满一个完整的 4×4 块 (BC1 是 8 字节),
    /// 这就是为什么 BC 纹理的 mip 链末端不会像未压缩纹理那样收敛到 4 字节。
    pub const fn level_byte_size(self, width: u32, height: u32) -> usize {
        let blocks_x = width.div_ceil(4) as usize;
        let blocks_y = height.div_ceil(4) as usize;
        blocks_x * blocks_y * self.block_bytes()
    }

    /// 映射到 block_compression 的编码器变体。
    pub const fn variant(self) -> CompressionVariant {
        match self {
            BcFormat::Bc1 => CompressionVariant::BC1,
            BcFormat::Bc3 => CompressionVariant::BC3,
            BcFormat::Bc4 => CompressionVariant::BC4,
            BcFormat::Bc5 => CompressionVariant::BC5,
        }
    }

    /// DDS DX10 头里的 `dxgiFormat` 字段值。
    ///
    /// BC4/BC5 没有 `_SRGB` 变体 —— 硬件层面就不存在。这不是本工具的限制:
    /// 单/双通道 BC 存的是标量数据 (粗糙度、法线 XY), sRGB 曲线对它们没有
    /// 任何意义。调用方如果同时要了 BC4/BC5 和 --color-space srgb, 说明它
    /// 对这张图的语义判断是矛盾的, 这里直接失败而不是悄悄丢掉 sRGB 标记 ——
    /// 丢掉的话引擎会按线性采样, 结果偏暗, 而且没有任何地方会报错。
    pub const fn dxgi_format(self, color_space: ColorSpace) -> Option<u32> {
        match (self, color_space) {
            (BcFormat::Bc1, ColorSpace::Linear) => Some(DXGI_FORMAT_BC1_UNORM),
            (BcFormat::Bc1, ColorSpace::Srgb) => Some(DXGI_FORMAT_BC1_UNORM_SRGB),
            (BcFormat::Bc3, ColorSpace::Linear) => Some(DXGI_FORMAT_BC3_UNORM),
            (BcFormat::Bc3, ColorSpace::Srgb) => Some(DXGI_FORMAT_BC3_UNORM_SRGB),
            (BcFormat::Bc4, ColorSpace::Linear) => Some(DXGI_FORMAT_BC4_UNORM),
            (BcFormat::Bc5, ColorSpace::Linear) => Some(DXGI_FORMAT_BC5_UNORM),
            (BcFormat::Bc4 | BcFormat::Bc5, ColorSpace::Srgb) => None,
        }
    }

    /// 该格式实际会读取源 RGBA8 的哪几个通道 —— 往返测试比对时只能比这几个,
    /// 因为解码器对未使用的通道会填 0 而不是原值。
    pub const fn meaningful_channels(self) -> &'static [usize] {
        match self {
            BcFormat::Bc1 => &[0, 1, 2],
            BcFormat::Bc3 => &[0, 1, 2, 3],
            BcFormat::Bc4 => &[0],
            BcFormat::Bc5 => &[0, 1],
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            BcFormat::Bc1 => "BC1",
            BcFormat::Bc3 => "BC3",
            BcFormat::Bc4 => "BC4",
            BcFormat::Bc5 => "BC5",
        }
    }
}

// ── DXGI_FORMAT 常量 (来自 dxgiformat.h) ────────────────────────────────
// 只列出本工具会写出的, 加上 inspect 需要认得的邻居值。
pub const DXGI_FORMAT_BC1_UNORM: u32 = 71;
pub const DXGI_FORMAT_BC1_UNORM_SRGB: u32 = 72;
pub const DXGI_FORMAT_BC2_UNORM: u32 = 74;
pub const DXGI_FORMAT_BC2_UNORM_SRGB: u32 = 75;
pub const DXGI_FORMAT_BC3_UNORM: u32 = 77;
pub const DXGI_FORMAT_BC3_UNORM_SRGB: u32 = 78;
pub const DXGI_FORMAT_BC4_UNORM: u32 = 80;
pub const DXGI_FORMAT_BC4_SNORM: u32 = 81;
pub const DXGI_FORMAT_BC5_UNORM: u32 = 83;
pub const DXGI_FORMAT_BC5_SNORM: u32 = 84;
pub const DXGI_FORMAT_BC6H_UF16: u32 = 95;
pub const DXGI_FORMAT_BC6H_SF16: u32 = 96;
pub const DXGI_FORMAT_BC7_UNORM: u32 = 98;
pub const DXGI_FORMAT_BC7_UNORM_SRGB: u32 = 99;

/// 把 DXGI 格式号翻回可读名字, 供 `lat inspect` 使用。
/// 认不出来的返回 None —— inspect 要能诚实地说"这个格式我不认识"。
pub fn dxgi_format_name(value: u32) -> Option<&'static str> {
    Some(match value {
        DXGI_FORMAT_BC1_UNORM => "BC1_UNORM",
        DXGI_FORMAT_BC1_UNORM_SRGB => "BC1_UNORM_SRGB",
        DXGI_FORMAT_BC2_UNORM => "BC2_UNORM",
        DXGI_FORMAT_BC2_UNORM_SRGB => "BC2_UNORM_SRGB",
        DXGI_FORMAT_BC3_UNORM => "BC3_UNORM",
        DXGI_FORMAT_BC3_UNORM_SRGB => "BC3_UNORM_SRGB",
        DXGI_FORMAT_BC4_UNORM => "BC4_UNORM",
        DXGI_FORMAT_BC4_SNORM => "BC4_SNORM",
        DXGI_FORMAT_BC5_UNORM => "BC5_UNORM",
        DXGI_FORMAT_BC5_SNORM => "BC5_SNORM",
        DXGI_FORMAT_BC6H_UF16 => "BC6H_UF16",
        DXGI_FORMAT_BC6H_SF16 => "BC6H_SF16",
        DXGI_FORMAT_BC7_UNORM => "BC7_UNORM",
        DXGI_FORMAT_BC7_UNORM_SRGB => "BC7_UNORM_SRGB",
        _ => return None,
    })
}

/// 已知 BC 格式的每块字节数 —— inspect 用它核对声明的层大小是否自洽。
pub fn dxgi_block_bytes(value: u32) -> Option<usize> {
    Some(match value {
        DXGI_FORMAT_BC1_UNORM | DXGI_FORMAT_BC1_UNORM_SRGB => 8,
        DXGI_FORMAT_BC4_UNORM | DXGI_FORMAT_BC4_SNORM => 8,
        DXGI_FORMAT_BC2_UNORM
        | DXGI_FORMAT_BC2_UNORM_SRGB
        | DXGI_FORMAT_BC3_UNORM
        | DXGI_FORMAT_BC3_UNORM_SRGB
        | DXGI_FORMAT_BC5_UNORM
        | DXGI_FORMAT_BC5_SNORM
        | DXGI_FORMAT_BC6H_UF16
        | DXGI_FORMAT_BC6H_SF16
        | DXGI_FORMAT_BC7_UNORM
        | DXGI_FORMAT_BC7_UNORM_SRGB => 16,
        _ => return None,
    })
}

/// 源图的通道特征 —— 由 `image_io` 从解码结果里量出来, 不看文件名。
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct SourceTraits {
    /// 源图声明的通道数 (1=L, 2=LA, 3=RGB, 4=RGBA)
    pub channels: u8,
    /// alpha 是否真的携带信息。全 255 的 alpha 通道等于没有 alpha,
    /// 按 BC3 存会白白多花一倍显存。
    pub alpha_significant: bool,
}

/// 解析最终格式。
///
/// 策略分两支, 分界线就是 `--color-space`:
///
/// * **srgb** —— 调用方声明"这是给人眼看的颜色"。颜色数据只能落到有 sRGB
///   变体的格式上, 所以通道数在这里 *只* 用来决定要不要 alpha:
///   有效 alpha → BC3, 否则 → BC1。一张灰度的 albedo 仍然是 sRGB 颜色,
///   走 BC1 (灰度复制到 RGB) 比走 BC4 正确 —— BC4 会把 sRGB 标记丢掉。
///
/// * **linear** —— 调用方声明"这是数值"。此时通道数就是语义:
///   1 → BC4 (occlusion / roughness 单通道)
///   2 → BC5 (切线空间法线的 XY)
///   3/4 → BC1/BC3 (线性的彩色数据, 例如已经打包好的 MR 贴图)
///
/// 注意 3 通道的切线空间法线图 (glTF 的常见形态) 会落到 BC1 —— 这是错的
/// 选择但也是唯一诚实的默认: 从像素上分不出"RGB 法线"和"RGB 颜色"。
/// 调用方必须显式 `--format bc5`, 那时我们只取 R/G 两通道, Z 由着色器
/// 用 sqrt(1-x²-y²) 重建。这个决定必须留给调用方, 不能猜。
pub fn resolve_format(
    option: FormatOption,
    traits: SourceTraits,
    color_space: ColorSpace,
) -> Result<BcFormat> {
    let format = match option {
        FormatOption::Bc1 => BcFormat::Bc1,
        FormatOption::Bc3 => BcFormat::Bc3,
        FormatOption::Bc4 => BcFormat::Bc4,
        FormatOption::Bc5 => BcFormat::Bc5,
        FormatOption::Auto => match color_space {
            ColorSpace::Srgb => {
                if traits.alpha_significant {
                    BcFormat::Bc3
                } else {
                    BcFormat::Bc1
                }
            }
            ColorSpace::Linear => match traits.channels {
                1 => BcFormat::Bc4,
                2 => BcFormat::Bc5,
                _ => {
                    if traits.alpha_significant {
                        BcFormat::Bc3
                    } else {
                        BcFormat::Bc1
                    }
                }
            },
        },
    };

    // 这里必须失败而不是降级。见 dxgi_format 的注释。
    if format.dxgi_format(color_space).is_none() {
        bail!(
            "{} 没有 sRGB 变体, 但 --color-space 指定了 srgb。\n  \
             {} 存的是标量数据 (法线 XY / 粗糙度 / 遮蔽), sRGB 传输曲线对它没有意义。\n  \
             修法二选一: 这张图确实是数据 → 改用 --color-space linear;\n  \
             或者它其实是颜色 → 改用 --format bc1 / bc3。",
            format.name(),
            format.name()
        );
    }

    Ok(format)
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── 块尺寸算术 ────────────────────────────────────────────────────
    // 这一组用例的重点是"向上取整到 4 的倍数"这一步真的在起作用。
    // 如果实现写成 (w/4)*(h/4), 下面每一个非 4 倍数的用例都会失败。

    #[test]
    fn block_size_power_of_two() {
        assert_eq!(BcFormat::Bc1.level_byte_size(1024, 1024), 256 * 256 * 8);
        assert_eq!(BcFormat::Bc3.level_byte_size(1024, 1024), 256 * 256 * 16);
        assert_eq!(BcFormat::Bc4.level_byte_size(1024, 1024), 256 * 256 * 8);
        assert_eq!(BcFormat::Bc5.level_byte_size(1024, 1024), 256 * 256 * 16);
    }

    #[test]
    fn block_size_sub_block_levels() {
        // 4×4 以下的 mip 层仍然占满一个块。这是 BC 纹理最容易算错的地方:
        // 一条 1024 的 mip 链末端 4 层 (4,2,1 以及 8) 全都是单块。
        for (w, h) in [(4, 4), (3, 3), (2, 2), (1, 1), (1, 4), (4, 1)] {
            assert_eq!(
                BcFormat::Bc1.level_byte_size(w, h),
                8,
                "BC1 {}×{} 应当正好是一个块",
                w,
                h
            );
            assert_eq!(
                BcFormat::Bc5.level_byte_size(w, h),
                16,
                "BC5 {}×{} 应当正好是一个块",
                w,
                h
            );
        }
    }

    #[test]
    fn block_size_non_power_of_two() {
        // 300 → 75 块整除; 173 → 43.25 → 44 块 (向上取整生效)
        assert_eq!(BcFormat::Bc1.level_byte_size(300, 173), 75 * 44 * 8);
        // 若忘记向上取整会得到 75*43*8, 两者必须不同 —— 否则上面那条断言
        // 根本没有区分能力。
        assert_ne!(75 * 44 * 8, 75 * 43 * 8);

        // 1 像素宽也要占一整块的宽度。写成 "块宽 × 块高 × 每块字节"
        // 才能一眼看出 1 像素那一维被抬成了 1 个块而不是 0 个。
        #[allow(clippy::identity_op)]
        {
            assert_eq!(BcFormat::Bc3.level_byte_size(1, 1000), 1 * 250 * 16);
            assert_eq!(BcFormat::Bc4.level_byte_size(1000, 1), 250 * 1 * 8);
        }
    }

    // ── 格式选择策略 ──────────────────────────────────────────────────

    fn traits(channels: u8, alpha: bool) -> SourceTraits {
        SourceTraits {
            channels,
            alpha_significant: alpha,
        }
    }

    #[test]
    fn auto_linear_splits_by_channel_count() {
        let auto = FormatOption::Auto;
        let lin = ColorSpace::Linear;
        assert_eq!(
            resolve_format(auto, traits(1, false), lin).unwrap(),
            BcFormat::Bc4
        );
        assert_eq!(
            resolve_format(auto, traits(2, false), lin).unwrap(),
            BcFormat::Bc5
        );
        assert_eq!(
            resolve_format(auto, traits(3, false), lin).unwrap(),
            BcFormat::Bc1
        );
        assert_eq!(
            resolve_format(auto, traits(4, true), lin).unwrap(),
            BcFormat::Bc3
        );
    }

    #[test]
    fn auto_falls_back_to_bc1_when_alpha_is_opaque() {
        // 4 通道但 alpha 全 255: 走 BC3 会白花一倍显存。
        assert_eq!(
            resolve_format(FormatOption::Auto, traits(4, false), ColorSpace::Srgb).unwrap(),
            BcFormat::Bc1
        );
        assert_eq!(
            resolve_format(FormatOption::Auto, traits(4, true), ColorSpace::Srgb).unwrap(),
            BcFormat::Bc3
        );
    }

    #[test]
    fn auto_srgb_grayscale_still_uses_bc1() {
        // 灰度 albedo 是颜色, 不是数据。落到 BC4 会丢掉 sRGB 标记。
        assert_eq!(
            resolve_format(FormatOption::Auto, traits(1, false), ColorSpace::Srgb).unwrap(),
            BcFormat::Bc1
        );
    }

    #[test]
    fn srgb_with_bc4_or_bc5_must_fail() {
        // 这是"检查能真的失败"的一个点: 若把 dxgi_format 的 None 分支
        // 悄悄降级成 UNORM, 下面两条会变成 Ok 而无人察觉。
        let err = resolve_format(FormatOption::Bc5, traits(3, false), ColorSpace::Srgb)
            .expect_err("BC5 + sRGB 必须报错");
        assert!(
            err.to_string().contains("sRGB"),
            "错误信息要指向原因, 实际: {}",
            err
        );
        assert!(resolve_format(FormatOption::Bc4, traits(1, false), ColorSpace::Srgb).is_err());
        // 对照: 同样的格式在 linear 下必须成功
        assert!(resolve_format(FormatOption::Bc5, traits(3, false), ColorSpace::Linear).is_ok());
        assert!(resolve_format(FormatOption::Bc4, traits(1, false), ColorSpace::Linear).is_ok());
    }

    #[test]
    fn dxgi_mapping_agrees_with_block_size() {
        for f in [BcFormat::Bc1, BcFormat::Bc3, BcFormat::Bc4, BcFormat::Bc5] {
            for cs in [ColorSpace::Linear, ColorSpace::Srgb] {
                if let Some(dxgi) = f.dxgi_format(cs) {
                    assert_eq!(
                        dxgi_block_bytes(dxgi),
                        Some(f.block_bytes()),
                        "{} 的 DXGI 号 {} 块大小与 BcFormat 不一致",
                        f.name(),
                        dxgi
                    );
                    assert!(dxgi_format_name(dxgi).is_some());
                }
            }
        }
    }
}
