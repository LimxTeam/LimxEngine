/*******************************************************************************
 * 文件: mip.rs
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   CPU 侧 mip 链生成 (面积盒式滤波), 以及 sRGB ⇄ 线性光的转换。
 *
 * 为什么 mip 必须离线生成:
 *   引擎当前的 mip 是运行时用 vkCmdBlitImage 从第 N 层线性放缩出第 N+1 层的。
 *   这条路对 BC 纹理走不通, 有两个各自独立的原因:
 *     1. Vulkan 的 blit 要求源/目标格式带 BLIT_SRC_BIT / BLIT_DST_BIT。
 *        所有 BC 压缩格式都没有这两个 feature —— 硬件的采样器能读它们,
 *        但固定功能的放缩单元不能写它们。
 *     2. 就算能, 也不该: 逐层"解压 → 放缩 → 重压"会让 BC 的量化误差
 *        沿 mip 链累积, 越远的 mip 越花。
 *   所以顺序必须反过来 —— 先在未压缩的浮点域把整条 mip 链生成完,
 *   再逐层独立压缩。这也是本工具存在的直接理由之一。
 *
 ******************************************************************************/

use crate::format::ColorSpace;

/// 一层未压缩的 RGBA8 图像。mip 链就是它的序列。
#[derive(Clone, Debug)]
pub struct Rgba8Image {
    pub width: u32,
    pub height: u32,
    /// 长度恒为 width * height * 4, 行优先、紧凑排列 (无行间 padding)
    pub pixels: Vec<u8>,
}

impl Rgba8Image {
    pub fn new(width: u32, height: u32, pixels: Vec<u8>) -> Self {
        assert_eq!(
            pixels.len(),
            width as usize * height as usize * 4,
            "RGBA8 缓冲区长度与尺寸不符"
        );
        Self {
            width,
            height,
            pixels,
        }
    }

    #[inline]
    pub fn texel(&self, x: u32, y: u32) -> [u8; 4] {
        let i = (y as usize * self.width as usize + x as usize) * 4;
        [
            self.pixels[i],
            self.pixels[i + 1],
            self.pixels[i + 2],
            self.pixels[i + 3],
        ]
    }
}

// ── sRGB 传输函数 ────────────────────────────────────────────────────────
// 用的是 IEC 61966-2-1 的分段定义, 而不是常见的 pow(x, 2.2) 近似:
// 低端那一段线性区在暗部差别可观, 而暗部恰好是 mip 链最容易发灰的地方。

/// 256 项查表 —— 每张 1024×1024 的图要做上百万次这个转换, 查表比 powf 快一个量级。
static SRGB_TO_LINEAR_LUT: std::sync::LazyLock<[f32; 256]> = std::sync::LazyLock::new(|| {
    let mut lut = [0.0f32; 256];
    for (i, slot) in lut.iter_mut().enumerate() {
        let c = i as f32 / 255.0;
        *slot = if c <= 0.040_448_237 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        };
    }
    lut
});

#[inline]
pub fn srgb_to_linear(v: u8) -> f32 {
    SRGB_TO_LINEAR_LUT[v as usize]
}

#[inline]
pub fn linear_to_srgb(v: f32) -> u8 {
    let c = v.clamp(0.0, 1.0);
    let s = if c <= 0.003_130_8 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    };
    (s * 255.0 + 0.5) as u8
}

/// mip 链的层数: 一直缩到 1×1 为止。
///
/// 公式是 `1 + floor(log2(max(w, h)))` —— 与 Vulkan 规范里
/// `VkImageCreateInfo::mipLevels` 的上限完全一致, 引擎侧创建 image 时
/// 用同一个式子算出来的层数必须能对上, 否则 vkCmdCopyBufferToImage
/// 会越界。
pub fn mip_level_count(width: u32, height: u32) -> u32 {
    debug_assert!(width > 0 && height > 0);
    32 - width.max(height).leading_zeros()
}

/// 下一层的尺寸: 减半, 但不小于 1。
///
/// 非 2 的幂尺寸会在这里被反复地板除, 例如 300 → 150 → 75 → 37 → 18 → 9 → 4 → 2 → 1。
/// 注意 75 → 37 丢掉了半个像素 —— 这是 D3D/Vulkan 都采用的约定,
/// 引擎侧算 mip 尺寸时必须用同一个 floor 规则, 否则层尺寸会对不上。
#[inline]
pub fn next_mip_size(size: u32) -> u32 {
    (size / 2).max(1)
}

/// 同尺寸、同层数的未压缩 RGBA8 会占多少字节。
///
/// 用来和 BC 输出做显存对照。**必须把整条 mip 链都算进去** ——
/// 只拿第 0 层的 RGBA8 去比一份含 mip 的 BC 输出, 压缩比会被低估
/// 大约 4/3 倍, 而这个数字正是烘焙这件事的收益本身。
pub fn rgba8_chain_bytes(width: u32, height: u32, mip_count: u32) -> usize {
    let mut total = 0usize;
    let (mut w, mut h) = (width, height);
    for _ in 0..mip_count {
        total += w as usize * h as usize * 4;
        w = next_mip_size(w);
        h = next_mip_size(h);
    }
    total
}

/// 生成完整 mip 链 (含第 0 层)。
///
/// `color_space` 决定平均是在哪个域里做的:
///   * `Srgb`  —— 先解码到线性光, 平均, 再编码回 sRGB。
///   * `Linear` —— 直接在存储值上平均。
///
/// alpha 通道 **永远** 按线性处理, 不参与 sRGB 转换 —— sRGB 传输函数只
/// 定义在颜色通道上, 把它套到覆盖率上会让半透明边缘发生可见的偏移。
pub fn generate_mip_chain(base: &Rgba8Image, color_space: ColorSpace) -> Vec<Rgba8Image> {
    let levels = mip_level_count(base.width, base.height) as usize;
    let mut chain = Vec::with_capacity(levels);

    // 工作缓冲保持在 f32: 若每层都写回 u8 再读出来, 量化误差会沿链累积,
    // 1024 的链有 10 次降采样, 末端可见地偏移。
    let mut current = to_working_space(base, color_space);
    let mut cur_w = base.width;
    let mut cur_h = base.height;

    chain.push(base.clone());

    for _ in 1..levels {
        let next_w = next_mip_size(cur_w);
        let next_h = next_mip_size(cur_h);
        current = box_downsample(&current, cur_w, cur_h, next_w, next_h);
        cur_w = next_w;
        cur_h = next_h;
        chain.push(from_working_space(&current, cur_w, cur_h, color_space));
    }

    chain
}

/// RGBA8 → 工作域 f32。RGB 视 color_space 决定是否解 sRGB, A 恒为线性。
fn to_working_space(img: &Rgba8Image, color_space: ColorSpace) -> Vec<f32> {
    let mut out = Vec::with_capacity(img.pixels.len());
    match color_space {
        ColorSpace::Srgb => {
            for px in img.pixels.chunks_exact(4) {
                out.push(srgb_to_linear(px[0]));
                out.push(srgb_to_linear(px[1]));
                out.push(srgb_to_linear(px[2]));
                out.push(px[3] as f32 / 255.0);
            }
        }
        ColorSpace::Linear => {
            for &v in &img.pixels {
                out.push(v as f32 / 255.0);
            }
        }
    }
    out
}

/// 工作域 f32 → RGBA8。
fn from_working_space(
    data: &[f32],
    width: u32,
    height: u32,
    color_space: ColorSpace,
) -> Rgba8Image {
    let mut pixels = Vec::with_capacity(data.len());
    match color_space {
        ColorSpace::Srgb => {
            for px in data.chunks_exact(4) {
                pixels.push(linear_to_srgb(px[0]));
                pixels.push(linear_to_srgb(px[1]));
                pixels.push(linear_to_srgb(px[2]));
                pixels.push((px[3].clamp(0.0, 1.0) * 255.0 + 0.5) as u8);
            }
        }
        ColorSpace::Linear => {
            for &v in data {
                pixels.push((v.clamp(0.0, 1.0) * 255.0 + 0.5) as u8);
            }
        }
    }
    Rgba8Image::new(width, height, pixels)
}

/// 面积盒式滤波降采样。
///
/// 目标像素 x 覆盖源区间 `[x * sx, (x+1) * sx)`, 取其中所有源像素的等权平均。
/// 对偶数尺寸这退化成标准的 2×2 盒式; 对奇数尺寸 (例如 5 → 2, sx = 2.5)
/// 相邻目标像素的覆盖区间会有一列重叠, 但 **没有任何一列被丢掉** ——
/// 简单写成 `src[2*x]` 与 `src[2*x+1]` 平均的实现会把最后一列直接扔掉,
/// 那正是非 2 的幂纹理 mip 链出现"边缘往一侧漂"的原因。
fn box_downsample(src: &[f32], src_w: u32, src_h: u32, dst_w: u32, dst_h: u32) -> Vec<f32> {
    let mut dst = vec![0.0f32; dst_w as usize * dst_h as usize * 4];
    let sx = src_w as f64 / dst_w as f64;
    let sy = src_h as f64 / dst_h as f64;

    for dy in 0..dst_h {
        let y0 = (dy as f64 * sy).floor() as u32;
        let y1 = (((dy + 1) as f64 * sy).ceil() as u32).min(src_h).max(y0 + 1);
        for dx in 0..dst_w {
            let x0 = (dx as f64 * sx).floor() as u32;
            let x1 = (((dx + 1) as f64 * sx).ceil() as u32).min(src_w).max(x0 + 1);

            let mut acc = [0.0f64; 4];
            let mut n = 0.0f64;
            for y in y0..y1 {
                for x in x0..x1 {
                    let i = (y as usize * src_w as usize + x as usize) * 4;
                    acc[0] += src[i] as f64;
                    acc[1] += src[i + 1] as f64;
                    acc[2] += src[i + 2] as f64;
                    acc[3] += src[i + 3] as f64;
                    n += 1.0;
                }
            }

            let o = (dy as usize * dst_w as usize + dx as usize) * 4;
            for c in 0..4 {
                dst[o + c] = (acc[c] / n) as f32;
            }
        }
    }

    dst
}

/// 把一层图像补齐到 4 的倍数, 供块编码器使用。
///
/// block_compression 的 `compress_rgba8` 对非 4 倍数尺寸直接 assert 崩溃,
/// 所以补齐这一步必须由我们做。补的方式是 **边缘复制** 而不是填 0:
/// 一个 4×4 块的端点是对块内 16 个像素拟合出来的, 如果右下角补的是黑色,
/// 端点会被拽向黑, 整块可见地变暗 —— 这在非 2 的幂纹理的右/下边缘上
/// 表现为一条深色描边。
pub fn pad_to_block_multiple(img: &Rgba8Image) -> Rgba8Image {
    let pw = img.width.div_ceil(4) * 4;
    let ph = img.height.div_ceil(4) * 4;
    if pw == img.width && ph == img.height {
        return img.clone();
    }

    let mut pixels = vec![0u8; pw as usize * ph as usize * 4];
    for y in 0..ph {
        let sy = y.min(img.height - 1);
        for x in 0..pw {
            let sx = x.min(img.width - 1);
            let t = img.texel(sx, sy);
            let o = (y as usize * pw as usize + x as usize) * 4;
            pixels[o..o + 4].copy_from_slice(&t);
        }
    }

    Rgba8Image::new(pw, ph, pixels)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn solid(width: u32, height: u32, rgba: [u8; 4]) -> Rgba8Image {
        let mut px = Vec::with_capacity(width as usize * height as usize * 4);
        for _ in 0..(width as usize * height as usize) {
            px.extend_from_slice(&rgba);
        }
        Rgba8Image::new(width, height, px)
    }

    // ── mip 层数与逐层尺寸 ────────────────────────────────────────────

    #[test]
    fn mip_chain_1024_has_11_levels() {
        let img = solid(1024, 1024, [128, 64, 32, 255]);
        let chain = generate_mip_chain(&img, ColorSpace::Linear);
        assert_eq!(chain.len(), 11, "1024×1024 应当有 11 层");

        let expected: Vec<u32> = vec![1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1];
        let actual: Vec<u32> = chain.iter().map(|l| l.width).collect();
        assert_eq!(actual, expected);
        let actual_h: Vec<u32> = chain.iter().map(|l| l.height).collect();
        assert_eq!(actual_h, expected);

        // 每层缓冲区长度必须与自己声明的尺寸一致 —— Rgba8Image::new 里
        // 已经断言过, 这里再显式核一遍最后一层, 因为 1×1 是最容易被
        // "提前退出循环" 漏掉的一层。
        assert_eq!(chain[10].pixels.len(), 4);
    }

    #[test]
    fn mip_chain_non_square_uses_longest_edge() {
        // 长边决定层数; 短边先到 1 之后保持 1。
        let img = solid(1024, 256, [10, 20, 30, 255]);
        let chain = generate_mip_chain(&img, ColorSpace::Linear);
        assert_eq!(chain.len(), 11);
        let dims: Vec<(u32, u32)> = chain.iter().map(|l| (l.width, l.height)).collect();
        assert_eq!(
            dims,
            vec![
                (1024, 256),
                (512, 128),
                (256, 64),
                (128, 32),
                (64, 16),
                (32, 8),
                (16, 4),
                (8, 2),
                (4, 1),
                (2, 1),
                (1, 1),
            ]
        );
    }

    #[test]
    fn mip_chain_non_power_of_two() {
        // 300×173: 层数 = 1 + floor(log2(300)) = 9, 逐层地板除。
        let img = solid(300, 173, [200, 100, 50, 255]);
        let chain = generate_mip_chain(&img, ColorSpace::Linear);
        let dims: Vec<(u32, u32)> = chain.iter().map(|l| (l.width, l.height)).collect();
        assert_eq!(
            dims,
            vec![
                (300, 173),
                (150, 86),
                (75, 43),
                (37, 21),
                (18, 10),
                (9, 5),
                (4, 2),
                (2, 1),
                (1, 1),
            ]
        );
        assert_eq!(chain.len(), 9);
    }

    #[test]
    fn mip_level_count_matches_vulkan_formula() {
        // 与 floor(log2(max)) + 1 逐个对照, 覆盖 2 的幂边界前后各一格。
        for (w, h, expect) in [
            (1, 1, 1),
            (2, 1, 2),
            (3, 3, 2),
            (4, 4, 3),
            (5, 5, 3),
            (255, 1, 8),
            (256, 256, 9),
            (257, 1, 9),
            (1024, 1024, 11),
            (2048, 1, 12),
        ] {
            assert_eq!(mip_level_count(w, h), expect, "{}×{} 层数不符", w, h);
        }
    }

    // ── 滤波正确性 ────────────────────────────────────────────────────

    #[test]
    fn constant_image_survives_every_level() {
        // 常量图在任何滤波下都必须逐层保持原值。这条能抓住"漏乘权重"、
        // "越界读到 0"、"sRGB 往返不闭合" 三类问题。
        for cs in [ColorSpace::Linear, ColorSpace::Srgb] {
            let img = solid(300, 173, [37, 200, 9, 128]);
            for (i, level) in generate_mip_chain(&img, cs).iter().enumerate() {
                for px in level.pixels.chunks_exact(4) {
                    assert_eq!(px, [37, 200, 9, 128], "{:?} 第 {} 层颜色漂移", cs, i);
                }
            }
        }
    }

    #[test]
    fn srgb_downsample_differs_from_naive_average() {
        // 两个像素 64 与 192 缩成一个。三条路径给出三个不同的数:
        //   * 正确: 解 sRGB → 线性平均 → 编码回     = 146
        //   * 漏了解码那一半 (只在输出端编码)        = 188
        //   * 完全不做转换 (当成线性数据)            = 128
        //
        // 取 64/192 而不是 0/255 是有意的 —— 0 和 255 是 sRGB 传输函数的
        // 不动点, 用它们做样本时"漏解码"和"正确"都会得到 188, 这条用例
        // 就只能验到编码那一半, 解码那一半怎么写都通过。
        let img = Rgba8Image::new(2, 1, vec![64, 64, 64, 255, 192, 192, 192, 255]);

        let srgb_mip1 = generate_mip_chain(&img, ColorSpace::Srgb)[1].pixels[0];
        assert_eq!(
            srgb_mip1, 146,
            "sRGB 降采样必须在线性光里平均。得到 188 说明漏了 sRGB→线性 这一步, \
             得到 128 说明整个转换都没做"
        );

        let lin_mip1 = generate_mip_chain(&img, ColorSpace::Linear)[1].pixels[0];
        assert_eq!(lin_mip1, 128, "线性数据应当是朴素平均 (64+192)/2 = 128");

        assert_ne!(
            srgb_mip1, lin_mip1,
            "两种色彩空间必须给出不同结果, 否则色彩空间开关是摆设"
        );
    }

    #[test]
    fn odd_width_downsample_keeps_last_column() {
        // 5×1: 前 4 列黑, 第 5 列白。若实现按 src[2x], src[2x+1] 取平均,
        // 第 5 列 (索引 4) 会被完全丢掉, 结果两个目标像素都是纯黑。
        let mut px = vec![0u8; 5 * 4];
        for i in 0..5 {
            px[i * 4 + 3] = 255;
        }
        px[4 * 4] = 255;
        px[4 * 4 + 1] = 255;
        px[4 * 4 + 2] = 255;
        let img = Rgba8Image::new(5, 1, px);

        let chain = generate_mip_chain(&img, ColorSpace::Linear);
        assert_eq!((chain[1].width, chain[1].height), (2, 1));
        let right = chain[1].texel(1, 0);
        assert!(
            right[0] > 0,
            "最后一列的白色必须进入降采样结果, 实际右侧像素 = {:?}",
            right
        );
    }

    // ── 补块 ──────────────────────────────────────────────────────────

    #[test]
    fn padding_rounds_up_and_replicates_edge() {
        // 5×3 → 8×4, 补出来的部分必须是边缘复制而不是黑色。
        let mut px = vec![0u8; 5 * 3 * 4];
        for i in 0..(5 * 3) {
            px[i * 4] = 200;
            px[i * 4 + 1] = 150;
            px[i * 4 + 2] = 100;
            px[i * 4 + 3] = 255;
        }
        let img = Rgba8Image::new(5, 3, px);
        let padded = pad_to_block_multiple(&img);

        assert_eq!((padded.width, padded.height), (8, 4));
        for y in 0..4 {
            for x in 0..8 {
                assert_eq!(
                    padded.texel(x, y),
                    [200, 150, 100, 255],
                    "({}, {}) 补块用了黑色而不是边缘复制",
                    x,
                    y
                );
            }
        }
    }

    #[test]
    fn padding_is_noop_when_already_aligned() {
        let img = solid(8, 4, [1, 2, 3, 4]);
        let padded = pad_to_block_multiple(&img);
        assert_eq!((padded.width, padded.height), (8, 4));
        assert_eq!(padded.pixels, img.pixels);
    }

    #[test]
    fn rgba8_chain_bytes_counts_every_level() {
        // 单层就是 w*h*4
        assert_eq!(rgba8_chain_bytes(1024, 1024, 1), 1024 * 1024 * 4);

        // 完整 11 层。逐层写死, 不用 4/3 的近似 —— 近似值和真值差
        // 一万多字节, 用它对账会掩盖真正的偏差。
        let expected: usize = (0..11)
            .map(|i| {
                let s = (1024usize >> i).max(1);
                s * s * 4
            })
            .sum();
        assert_eq!(rgba8_chain_bytes(1024, 1024, 11), expected);
        // 1398101 个纹素 × 4 字节。写死它是为了让"逐层求和"和"几何级数"
        // 两种算法互相对账 —— 用 4/3 近似会得到 5_592_405, 差 1 字节。
        assert_eq!(expected, 5_592_404);

        // 与只算第 0 层必须显著不同, 否则这个函数写了等于没写。
        assert!(rgba8_chain_bytes(1024, 1024, 11) > rgba8_chain_bytes(1024, 1024, 1));

        // 非 2 的幂: 层尺寸要跟着 next_mip_size 走
        assert_eq!(
            rgba8_chain_bytes(300, 173, 3),
            (300 * 173 + 150 * 86 + 75 * 43) * 4
        );
    }

    #[test]
    fn srgb_transfer_roundtrip_is_stable() {
        // 每一个 u8 都必须能原样往返, 否则常量图那条用例的"逐层保持"
        // 会因为转换本身而失败, 而不是因为滤波。
        for v in 0..=255u8 {
            assert_eq!(linear_to_srgb(srgb_to_linear(v)), v, "sRGB 往返在 {} 处不闭合", v);
        }
    }
}
