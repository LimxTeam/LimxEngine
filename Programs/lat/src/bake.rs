/*******************************************************************************
 * 文件: bake.rs
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   烘焙主流程: 源图 → mip 链 → 逐层块压缩 → DDS 字节流。
 *   以及往返质量度量 (PSNR), 供单元测试当 oracle 用。
 *
 * 为什么要有往返测试:
 *   块压缩器写错了不会崩。端点拟合退化、索引顺序反了、通道拿错了 ——
 *   这些错误的输出全都是"看着像那么回事"的一张图。唯一能自动发现它们
 *   的办法是把压缩结果解回来和原图比。block_compression 自带的 decode
 *   模块正好能当这个 oracle: 它是独立于编码路径的一份实现, 按 BC 规范
 *   写的, 编码器出错时它不会跟着一起错。
 *   BC7 走 `crate::bc7`, 那里的解码器同样是按规范单独写的一份 (不复用
 *   编码器的任何一步), 扮演的是同一个角色。
 *
 ******************************************************************************/

use anyhow::{Context, Result};
use block_compression::decode::decompress_blocks_as_rgba8;
use block_compression::encode::compress_rgba8;
use rayon::prelude::*;

use crate::dds::DdsTexture;
use crate::format::{BcFormat, ColorSpace, FormatOption, SourceTraits};
use crate::mip::{generate_mip_chain, pad_to_block_multiple, Rgba8Image};

/// 单张图的烘焙参数。
#[derive(Copy, Clone, Debug)]
pub struct BakeOptions {
    pub format: FormatOption,
    pub color_space: ColorSpace,
    /// 是否生成 mip 链。关掉只写第 0 层 —— 给 UI 贴图之类确实不需要 mip 的场景。
    pub mipmaps: bool,
    /// 解回第 0 层与源图比对, 算出 PSNR
    pub verify: bool,
    /// PSNR 下限。设了就隐含开启 verify, 低于它整张图判为失败。
    pub min_psnr: Option<f64>,
}

/// 一次烘焙的结果概要, 供命令行汇总。
#[derive(Clone, Debug)]
pub struct BakeReport {
    pub format: BcFormat,
    pub width: u32,
    pub height: u32,
    pub mip_count: u32,
    pub output_bytes: usize,
    /// 第 0 层的往返 PSNR (dB), 仅在开了校验时有值
    pub psnr: Option<f64>,
}

/// 把一张已解码的图烘成 DDS 字节流。
pub fn bake(
    image: &Rgba8Image,
    traits: SourceTraits,
    options: BakeOptions,
) -> Result<(Vec<u8>, BakeReport)> {
    let format = crate::format::resolve_format(options.format, traits, options.color_space)?;

    let chain = if options.mipmaps {
        generate_mip_chain(image, options.color_space)
    } else {
        vec![image.clone()]
    };

    // 逐层并行: 层与层之间没有依赖 (mip 链已经全部生成好了)。
    // 层内还会再按块行切一次, 因为第 0 层通常占掉四分之三的工作量,
    // 只按层并行的话最长的那条链路仍然是单线程的。
    let levels: Vec<Vec<u8>> = chain
        .par_iter()
        .map(|level| compress_level(level, format))
        .collect();

    // 要求 mip 时链必须一路收敛到 1×1。DDS 容器本身允许短链, 所以这条
    // 只能在这里检查 —— 少了一层的纹理在引擎里表现为远处采样闪烁,
    // 而不是任何形式的报错。
    if options.mipmaps {
        ensure_chain_converges(&chain)?;
    }

    // 往返校验: 用 block_compression 自带的解码器把第 0 层解回来和源图比。
    // 解码器是独立于编码路径的一份实现, 编码器出错时它不会跟着一起错 ——
    // 这是我们唯一一个能自动发现"压出来的图看着像但其实错了"的手段。
    let psnr_value = if options.verify || options.min_psnr.is_some() {
        Some(verify_level0(&chain[0], &levels[0], format))
    } else {
        None
    };

    if let (Some(min), Some(actual)) = (options.min_psnr, psnr_value) {
        anyhow::ensure!(
            actual >= min,
            "第 0 层往返 PSNR = {:.2} dB, 低于下限 {:.2} dB ({} / {:?})",
            actual,
            min,
            format.name(),
            options.color_space
        );
    }

    let texture = DdsTexture {
        width: image.width,
        height: image.height,
        format,
        color_space: options.color_space,
        levels,
    };

    let mip_count = texture.levels.len() as u32;
    let bytes = texture
        .encode()
        .with_context(|| format!("生成 {} DDS 失败", format.name()))?;

    let report = BakeReport {
        format,
        width: image.width,
        height: image.height,
        mip_count,
        output_bytes: bytes.len(),
        psnr: psnr_value,
    };

    Ok((bytes, report))
}

/// 校验 mip 链确实一路收敛到了 1×1, 且逐层都在缩小。
///
/// 检查的是链的 **结果尺寸**, 不是层数。
/// 一开始这里写的是 `levels.len() == mip_level_count(w, h)` —— 但
/// `generate_mip_chain` 的循环次数本身就是用 `mip_level_count` 算的,
/// 那条断言等于拿一个值和它自己比, 无论降采样怎么写错都恒为真。
/// 换成看末层尺寸之后, "提前停在 4×4"、"某一层忘了减半" 这类错误才真的
/// 会被拦住。
fn ensure_chain_converges(chain: &[Rgba8Image]) -> Result<()> {
    let last = chain
        .last()
        .ok_or_else(|| anyhow::anyhow!("mip 链为空, 至少要有第 0 层"))?;

    anyhow::ensure!(
        last.width == 1 && last.height == 1,
        "mip 链没有收敛到 1×1: 共 {} 层, 末层是 {}×{}",
        chain.len(),
        last.width,
        last.height
    );

    for pair in chain.windows(2) {
        let (a, b) = (&pair[0], &pair[1]);
        anyhow::ensure!(
            b.width <= a.width && b.height <= a.height && (b.width < a.width || b.height < a.height),
            "mip 链的相邻两层没有缩小: {}×{} → {}×{}",
            a.width,
            a.height,
            b.width,
            b.height
        );
    }

    Ok(())
}

/// 解开第 0 层, 与 (补块后的) 源图算 PSNR。
///
/// 比对的是补块后的图而不是原图: 补出来的那几列/行确实被编码进了块里,
/// 拿原图比会因为长度对不上而无法计算。补块用的是边缘复制, 所以这部分
/// 的误差本身也是真实的质量信息。
fn verify_level0(level0: &Rgba8Image, blocks: &[u8], format: BcFormat) -> f64 {
    let padded = pad_to_block_multiple(level0);
    let decoded = decode_blocks(format, padded.width, padded.height, blocks);
    psnr(&padded.pixels, &decoded, format.meaningful_channels())
}

/// 解开一层块数据成 RGBA8。分派到对应格式的解码器。
///
/// 两条路的解码器都 **不是** 各自编码器的逆过程: BC1..BC5 用的是
/// block_compression 里独立的 decode 模块, BC7 用的是 `crate::bc7` 里
/// 按规范单独写的一份。这是这些往返数字有意义的前提 —— 用编码器自己
/// 的逆过程去验编码器, 端点顺序反了之类的错误会自洽地通过。
pub fn decode_blocks(format: BcFormat, width: u32, height: u32, blocks: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; width as usize * height as usize * 4];
    match format.variant() {
        Some(variant) => {
            decompress_blocks_as_rgba8(variant, width, height, blocks, &mut out);
        }
        None => {
            debug_assert_eq!(format, BcFormat::Bc7);
            crate::bc7::decompress_blocks(width, height, blocks, &mut out);
        }
    }
    out
}

/// 压缩一段像素成块数据。分派到对应格式的编码器。
fn encode_blocks(
    format: BcFormat,
    pixels: &[u8],
    out: &mut [u8],
    width: u32,
    height: u32,
    stride: u32,
) {
    match format.variant() {
        Some(variant) => compress_rgba8(variant, pixels, out, width, height, stride),
        None => {
            debug_assert_eq!(format, BcFormat::Bc7);
            crate::bc7::compress_blocks(pixels, out, width, height, stride);
        }
    }
}

/// 压缩一个 mip 层。
///
/// 两件事在这里发生:
///   1. 补齐到 4 的倍数 —— `compress_rgba8` 对非 4 倍数尺寸直接 assert,
///      而 mip 链末端的 2×2 / 1×1 层必然不是 4 的倍数。
///   2. 按块行切分并行 —— 块之间完全独立, 这是 BC 编码天然的并行粒度。
pub fn compress_level(level: &Rgba8Image, format: BcFormat) -> Vec<u8> {
    let padded = pad_to_block_multiple(level);
    let stride = padded.width * 4;
    let block_rows = (padded.height / 4) as usize;
    let row_bytes = (padded.width / 4) as usize * format.block_bytes();

    let mut out = vec![0u8; block_rows * row_bytes];

    // 补齐后的尺寸算出来的块数, 必须和按原始尺寸算出来的一致 ——
    // 两条路径算出不同结果就说明块公式有分歧。
    debug_assert_eq!(out.len(), format.level_byte_size(level.width, level.height));

    // 块行太少时并行的调度开销比压缩本身还大。
    if block_rows < 8 {
        encode_blocks(
            format,
            &padded.pixels,
            &mut out,
            padded.width,
            padded.height,
            stride,
        );
        return out;
    }

    out.par_chunks_mut(row_bytes)
        .enumerate()
        .for_each(|(row, chunk)| {
            // 每个任务处理一行 4 像素高的块。BC 的块存储是块行优先的,
            // 所以第 row 行块的输出正好是 out 的第 row 个 row_bytes 分片。
            let src_offset = row * 4 * stride as usize;
            encode_blocks(
                format,
                &padded.pixels[src_offset..],
                chunk,
                padded.width,
                4,
                stride,
            );
        });

    out
}

// ── 质量度量 ─────────────────────────────────────────────────────────────

/// 峰值信噪比 (dB), 只统计 `channels` 里列出的通道。
///
/// 为什么要挑通道: BC4 的解码器把 G/B/A 填 0, BC5 把 B/A 填 0 —— 那些
/// 位置的"误差"是格式定义的一部分, 不是压缩损失。全通道一起算会得到
/// 一个既低又没有意义的数字, 阈值也就失去了判别力。
pub fn psnr(original: &[u8], decoded: &[u8], channels: &[usize]) -> f64 {
    assert_eq!(original.len(), decoded.len(), "PSNR 两侧长度必须一致");
    assert_eq!(original.len() % 4, 0, "PSNR 输入必须是 RGBA8");

    let mut sum_sq = 0f64;
    let mut count = 0usize;
    for (a, b) in original.chunks_exact(4).zip(decoded.chunks_exact(4)) {
        for &c in channels {
            let d = a[c] as f64 - b[c] as f64;
            sum_sq += d * d;
            count += 1;
        }
    }

    if count == 0 {
        return f64::NAN;
    }
    let mse = sum_sq / count as f64;
    if mse == 0.0 {
        return f64::INFINITY;
    }
    10.0 * (255.0f64 * 255.0 / mse).log10()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// BC1 在平滑渐变上应当达到的质量下限。
    ///
    /// 这个数字不是拍脑袋来的: 渐变图的块内方差很小, BC1 的两端点插值
    /// 几乎能完美拟合, 误差主要来自 RGB565 的端点量化 (蓝色只有 5 bit)。
    /// 实测在 40 dB 以上, 取 35 留一档余量。下面的对照用例负责证明
    /// 这个阈值真的能被压破 —— 一个永远为真的断言等于没有断言。
    const BC1_GRADIENT_PSNR_FLOOR: f64 = 35.0;

    /// 一张彩色渐变图: R 沿 x, G 沿 y, B 为常量。
    /// B 取常量是故意的 —— 端点被写坏时它最容易暴露 (会变成灰阶斜坡)。
    fn gradient(width: u32, height: u32) -> Rgba8Image {
        let mut px = Vec::with_capacity(width as usize * height as usize * 4);
        for y in 0..height {
            for x in 0..width {
                px.push((x * 255 / (width - 1).max(1)) as u8);
                px.push((y * 255 / (height - 1).max(1)) as u8);
                px.push(128);
                px.push(255);
            }
        }
        Rgba8Image::new(width, height, px)
    }

    fn roundtrip(img: &Rgba8Image, format: BcFormat) -> Vec<u8> {
        let blocks = compress_level(img, format);
        let padded = pad_to_block_multiple(img);
        decode_blocks(format, padded.width, padded.height, &blocks)
    }

    // ── 往返 PSNR: 正例 ────────────────────────────────────────────────

    #[test]
    fn bc1_roundtrip_on_gradient_is_high_quality() {
        let img = gradient(128, 128);
        let decoded = roundtrip(&img, BcFormat::Bc1);
        let q = psnr(&img.pixels, &decoded, BcFormat::Bc1.meaningful_channels());
        assert!(
            q > BC1_GRADIENT_PSNR_FLOOR,
            "BC1 渐变往返 PSNR = {:.2} dB, 低于下限 {:.1} dB",
            q,
            BC1_GRADIENT_PSNR_FLOOR
        );
        assert!(q.is_finite(), "渐变经 BC1 必然有损, PSNR 不该是无穷大");
    }

    // ── 往返 PSNR: 对照 —— 证明阈值真的能失败 ─────────────────────────

    #[test]
    fn corrupted_endpoints_break_the_psnr_floor() {
        // 从一份 *正确* 的 BC1 结果出发, 只改每个块的两个端点 (前 4 字节),
        // 索引位原样保留 —— 这正是"端点拟合写错了"这类 bug 的形态。
        // 端点被设成白(0xFFFF)→黑(0x0000): c0 > c1, 仍是合法的 4 色不透明
        // 模式, 解码器不会拒绝它, 只是每个块都变成灰阶斜坡。
        let img = gradient(128, 128);

        let good_blocks = compress_level(&img, BcFormat::Bc1);
        let mut bad_blocks = good_blocks.clone();
        for block in bad_blocks.chunks_exact_mut(8) {
            block[0..2].copy_from_slice(&0xFFFFu16.to_le_bytes());
            block[2..4].copy_from_slice(&0x0000u16.to_le_bytes());
        }
        assert_ne!(good_blocks, bad_blocks, "对照组必须真的改动了数据");

        let padded = pad_to_block_multiple(&img);
        let decoded = decode_blocks(BcFormat::Bc1, padded.width, padded.height, &bad_blocks);

        let bad = psnr(&img.pixels, &decoded, BcFormat::Bc1.meaningful_channels());
        assert!(
            bad < BC1_GRADIENT_PSNR_FLOOR,
            "端点写坏之后 PSNR 仍有 {:.2} dB, 没有跌破 {:.1} dB —— \
             说明这个阈值对真正的编码错误没有判别力, 正例那条断言是摆设",
            bad,
            BC1_GRADIENT_PSNR_FLOOR
        );

        // 再要求两者拉开足够距离, 否则阈值只是"恰好卡在中间"。
        let good = psnr(
            &img.pixels,
            &roundtrip(&img, BcFormat::Bc1),
            BcFormat::Bc1.meaningful_channels(),
        );
        assert!(
            good - bad > 15.0,
            "正确编码 {:.2} dB 与损坏编码 {:.2} dB 只差 {:.2} dB, 判别力不足",
            good,
            bad,
            good - bad
        );
    }

    #[test]
    fn psnr_itself_behaves() {
        // 度量函数本身也要能失败。三条性质: 相同 → 无穷; 有差异 → 有限;
        // 差异越大 → 数值越小。
        let a = vec![10u8, 20, 30, 255, 40, 50, 60, 255];
        assert!(psnr(&a, &a, &[0, 1, 2]).is_infinite());

        let mut b = a.clone();
        b[0] = 11;
        let near = psnr(&a, &b, &[0, 1, 2]);
        assert!(near.is_finite());

        let mut c = a.clone();
        c[0] = 200;
        let far = psnr(&a, &c, &[0, 1, 2]);
        assert!(
            far < near,
            "误差更大时 PSNR 必须更低 ({:.2} 应小于 {:.2})",
            far,
            near
        );

        // 只看未被改动的通道时, 差异应当消失 —— 证明通道选择真的生效。
        assert!(psnr(&a, &c, &[1, 2]).is_infinite());
    }

    // ── BC3 / BC4 / BC5 各自的往返 ─────────────────────────────────────

    #[test]
    fn bc3_preserves_alpha_gradient() {
        // alpha 沿 x 渐变, 颜色恒定。BC3 的 alpha 块是 8 位端点 + 3 位索引,
        // 精度比颜色高得多, 这里应当非常接近无损。
        let (w, h) = (64u32, 64u32);
        let mut px = Vec::new();
        for _y in 0..h {
            for x in 0..w {
                px.extend_from_slice(&[200, 100, 50, (x * 255 / (w - 1)) as u8]);
            }
        }
        let img = Rgba8Image::new(w, h, px);
        let decoded = roundtrip(&img, BcFormat::Bc3);
        let q = psnr(&img.pixels, &decoded, &[3]);
        assert!(q > 40.0, "BC3 alpha 往返 PSNR = {:.2} dB, 偏低", q);
    }

    #[test]
    fn bc4_and_bc5_roundtrip_their_own_channels() {
        let img = gradient(64, 64);

        let bc4 = roundtrip(&img, BcFormat::Bc4);
        let q4 = psnr(&img.pixels, &bc4, BcFormat::Bc4.meaningful_channels());
        assert!(q4 > 40.0, "BC4 (R 通道) PSNR = {:.2} dB", q4);

        let bc5 = roundtrip(&img, BcFormat::Bc5);
        let q5 = psnr(&img.pixels, &bc5, BcFormat::Bc5.meaningful_channels());
        assert!(q5 > 40.0, "BC5 (RG 通道) PSNR = {:.2} dB", q5);

        // 反过来: BC4 没有保存 G 通道, 拿 G 去比必须得到很差的分数。
        // 这条防止 meaningful_channels() 被写成 &[0,1,2,3] 之后
        // 上面两条还"碰巧"通过。
        let q4_wrong = psnr(&img.pixels, &bc4, &[1]);
        assert!(
            q4_wrong < 20.0,
            "BC4 不保存 G 通道, 用 G 比对应当得到很低的 PSNR, 实际 {:.2} dB",
            q4_wrong
        );
    }

    // ── BC7 相对 BC1 / BC3 的质量 ──────────────────────────────────────

    /// 一张"像真实贴图"的合成图: 大片平缓渐变 + 几条硬色彩边界 + 细节。
    ///
    /// 纯渐变会高估 BC1 (它拟合直线很在行), 纯噪声会低估两者。真实贴图的
    /// 难点在边界: 一个 4×4 块里跨过材质接缝时, BC1 只有一条端点线,
    /// 中间两级插值落在谁也不是的颜色上 —— 这正是 BC7 双子集要解决的。
    fn synthetic_material(width: u32, height: u32, alpha: bool) -> Rgba8Image {
        let mut px = Vec::with_capacity(width as usize * height as usize * 4);
        for y in 0..height {
            for x in 0..width {
                let fx = x as f32 / width as f32;
                let fy = y as f32 / height as f32;
                // 底色: 双向渐变 (BC1 擅长)
                let mut r = (40.0 + 180.0 * fx) as i32;
                let mut g = (60.0 + 120.0 * fy) as i32;
                let mut b = (200.0 - 150.0 * fx * fy) as i32;
                // 硬边界: 竖直的砖缝 + 斜向的接缝 (BC1 不擅长)
                if (x / 7) % 3 == 0 {
                    r = 230 - r / 4;
                    g = 40;
                    b = 30 + b / 5;
                }
                if (x + y) % 23 < 2 {
                    r = 250;
                    g = 250;
                    b = 90;
                }
                let a = if alpha {
                    // 抠图形态的 alpha: 与颜色无关的阶跃
                    if (y / 5) % 2 == 0 {
                        255
                    } else {
                        (x * 255 / width.max(1)) as i32
                    }
                } else {
                    255
                };
                px.push(r.clamp(0, 255) as u8);
                px.push(g.clamp(0, 255) as u8);
                px.push(b.clamp(0, 255) as u8);
                px.push(a.clamp(0, 255) as u8);
            }
        }
        Rgba8Image::new(width, height, px)
    }

    /// BC7 相对 BC1 至少要拿到的 dB。
    ///
    /// 实测 Sponza 的 65 张不透明贴图上平均 +7.85 dB, 最小 +1.25 dB
    /// (那一张是近乎纯色的贴图, BC1 本来就有 52.9 dB, 没有多少可拿的),
    /// 这张合成图上是 +9 dB 左右。取 4.0 作为下限 —— 它足够宽, 不会因为
    /// 编码器的细微改动误报; 又足够窄, "BC7 退化成只用 mode 6 的单子集
    /// 编码器"这类回退会立刻把它压破。
    const BC7_OVER_BC1_FLOOR: f64 = 4.0;

    #[test]
    fn bc7_beats_bc1_by_a_wide_margin_on_the_same_channels() {
        // 关键: 两边都只统计 RGB 三通道。BC1 压根不存 alpha, 用各自的
        // meaningful_channels 去比等于让 BC1 免考一门 —— 那样的对比
        // 说明不了任何事。
        let img = synthetic_material(128, 128, false);
        let rgb = &[0usize, 1, 2][..];

        let bc1 = psnr(&img.pixels, &roundtrip(&img, BcFormat::Bc1), rgb);
        let bc7 = psnr(&img.pixels, &roundtrip(&img, BcFormat::Bc7), rgb);

        assert!(
            bc7 - bc1 > BC7_OVER_BC1_FLOOR,
            "BC7 只比 BC1 好 {:.2} dB (BC1 {:.2} / BC7 {:.2}), 低于下限 {:.1} dB —— \
             要么编码器退化了, 要么这张测试图对两者没有区分度",
            bc7 - bc1,
            bc1,
            bc7,
            BC7_OVER_BC1_FLOOR
        );
        // 绝对值也要看一眼: 光有差距但两边都很差没有意义。
        assert!(bc7 > 40.0, "BC7 在合成材质图上只有 {:.2} dB", bc7);
    }

    #[test]
    fn bc7_beats_bc3_at_the_same_block_size() {
        // BC7 与 BC3 每块都是 16 字节。auto 策略里已经不再选 BC3, 依据
        // 就是这条 —— 同样的显存下 BC7 严格更好。这里连 alpha 一起比,
        // 两者的 meaningful_channels 相同, 是公平对照。
        let img = synthetic_material(128, 128, true);
        let rgba = BcFormat::Bc3.meaningful_channels();
        assert_eq!(rgba, BcFormat::Bc7.meaningful_channels());

        let bc3 = psnr(&img.pixels, &roundtrip(&img, BcFormat::Bc3), rgba);
        let bc7 = psnr(&img.pixels, &roundtrip(&img, BcFormat::Bc7), rgba);

        assert_eq!(
            BcFormat::Bc3.block_bytes(),
            BcFormat::Bc7.block_bytes(),
            "这条对比的前提是两者同尺寸"
        );
        assert!(
            bc7 > bc3 + 2.0,
            "同为 16 字节/块, BC7 {:.2} dB 没有明显好过 BC3 {:.2} dB",
            bc7,
            bc3
        );
    }

    #[test]
    fn bc7_is_nearly_lossless_on_flat_and_two_tone_content() {
        // UI / 图集 / 色卡这类内容 BC1 会明显崩 (5-6-5 端点 + 4 级插值),
        // BC7 应当几乎无损。这条同时是"BC7 的收益不只在照片上"的证据。
        let (w, h) = (64u32, 64u32);
        let mut px = Vec::new();
        for y in 0..h {
            for x in 0..w {
                let c: [u8; 4] = match ((x / 8) + (y / 8)) % 3 {
                    0 => [17, 200, 133, 255],
                    1 => [240, 31, 90, 255],
                    _ => [12, 44, 233, 255],
                };
                px.extend_from_slice(&c);
            }
        }
        let img = Rgba8Image::new(w, h, px);
        let rgb = &[0usize, 1, 2][..];

        let bc1 = psnr(&img.pixels, &roundtrip(&img, BcFormat::Bc1), rgb);
        let bc7 = psnr(&img.pixels, &roundtrip(&img, BcFormat::Bc7), rgb);
        assert!(
            bc7 > 45.0,
            "三色块图上 BC7 只有 {:.2} dB, 这类内容应当几乎无损",
            bc7
        );
        assert!(
            bc7 - bc1 > 8.0,
            "三色块图: BC7 {:.2} dB 与 BC1 {:.2} dB 只差 {:.2} dB",
            bc7,
            bc1,
            bc7 - bc1
        );
    }

    #[test]
    fn bc7_end_to_end_writes_a_bc7_dds() {
        // 端到端: auto + 有效 alpha 必须落到 BC7, 头里写的是 BC7 的
        // DXGI 号, 层大小按 16 字节/块算, 且能被自己的解析器读回。
        let img = synthetic_material(64, 64, true);
        let (bytes, report) = bake(
            &img,
            SourceTraits {
                channels: 4,
                alpha_significant: true,
            },
            BakeOptions {
                format: FormatOption::Auto,
                color_space: ColorSpace::Srgb,
                mipmaps: true,
                verify: true,
                min_psnr: Some(35.0),
            },
        )
        .unwrap();

        assert_eq!(report.format, BcFormat::Bc7);
        let info = crate::dds::parse(&bytes).unwrap();
        assert_eq!(info.dxgi_format, crate::format::DXGI_FORMAT_BC7_UNORM_SRGB);
        assert_eq!(info.mip_count, 7);
        assert_eq!(info.level_sizes[0], 16 * 16 * 16);
        assert_eq!(
            info.level_sizes.last(),
            Some(&16),
            "1×1 层仍要占满一个 16 字节的 BC7 块"
        );
        let q = report.psnr.expect("开了 verify 就必须有 PSNR");
        assert!(q > 35.0, "BC7 在合成材质图上只有 {:.2} dB", q);

        // 反向: 同一张图要求 BC1 达到同样的 PSNR 下限必须失败 ——
        // 否则上面那个 min_psnr 的门槛对格式没有区分力。
        let err = bake(
            &img,
            SourceTraits {
                channels: 4,
                alpha_significant: true,
            },
            BakeOptions {
                format: FormatOption::Bc1,
                color_space: ColorSpace::Srgb,
                mipmaps: true,
                verify: true,
                min_psnr: Some(35.0),
            },
        )
        .unwrap_err();
        assert!(format!("{:#}", err).contains("PSNR"), "实际: {:#}", err);
    }

    // ── 并行切分的正确性 ───────────────────────────────────────────────

    #[test]
    fn parallel_strips_match_single_shot() {
        // 按块行切分并行压缩, 结果必须和一次性压缩逐字节相同。
        // 切分写错 (源偏移算错 / 输出分片错位) 时图像仍然"看着像",
        // 只有逐字节比对才抓得住。
        for format in [
            BcFormat::Bc1,
            BcFormat::Bc3,
            BcFormat::Bc4,
            BcFormat::Bc5,
            BcFormat::Bc7,
        ] {
            // BC7 的编码比其它格式贵一个量级 (debug 构建下尤甚), 用小一号的
            // 图。高度仍然 ≥ 32 (8 个块行), 否则走不到并行分支, 这条用例就
            // 什么也没验到。
            let img = if format == BcFormat::Bc7 {
                gradient(64, 64)
            } else {
                gradient(256, 128)
            };
            let parallel = compress_level(&img, format);

            let padded = pad_to_block_multiple(&img);
            let mut single = vec![0u8; format.level_byte_size(img.width, img.height)];
            encode_blocks(
                format,
                &padded.pixels,
                &mut single,
                padded.width,
                padded.height,
                padded.width * 4,
            );

            assert_eq!(
                parallel,
                single,
                "{} 的并行切分结果与单次压缩不一致",
                format.name()
            );
        }
    }

    // ── 端到端: 尺寸 / 层数 / DDS 可读回 ───────────────────────────────

    #[test]
    fn end_to_end_1024_produces_11_levels() {
        let img = gradient(1024, 1024);
        let (bytes, report) = bake(
            &img,
            SourceTraits {
                channels: 3,
                alpha_significant: false,
            },
            BakeOptions {
                format: FormatOption::Auto,
                color_space: ColorSpace::Srgb,
                mipmaps: true,
                verify: false,
                min_psnr: None,
            },
        )
        .unwrap();

        assert_eq!(report.format, BcFormat::Bc1);
        assert_eq!(report.mip_count, 11);

        let info = crate::dds::parse(&bytes).unwrap();
        assert_eq!(info.mip_count, 11);
        assert_eq!(info.level_dims.first(), Some(&(1024, 1024)));
        assert_eq!(info.level_dims.last(), Some(&(1, 1)));
        assert_eq!(info.level_sizes.last(), Some(&8), "1×1 层占满一个 BC1 块");
        assert_eq!(bytes.len(), report.output_bytes);
    }

    #[test]
    fn end_to_end_non_power_of_two_and_tiny_levels() {
        // 300×173 的每一层都要各自向上取整; 末端 4×2 / 2×1 / 1×1 三层
        // 全部小于一个块。这两件事同时出错时, 输出长度仍可能"看着合理",
        // 所以逐层核对。
        let img = gradient(300, 173);
        let (bytes, report) = bake(
            &img,
            SourceTraits {
                channels: 2,
                alpha_significant: false,
            },
            BakeOptions {
                format: FormatOption::Auto,
                color_space: ColorSpace::Linear,
                mipmaps: true,
                verify: false,
                min_psnr: None,
            },
        )
        .unwrap();

        assert_eq!(report.format, BcFormat::Bc5, "2 通道线性图应当选 BC5");
        assert_eq!(report.mip_count, 9);

        let info = crate::dds::parse(&bytes).unwrap();
        assert_eq!(
            info.level_dims,
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
        // 写成 "块宽 × 块高 × 每块字节" 的形式而不是算好的数字 ——
        // 这样每一项在读的时候就能直接对着上面的层尺寸验算。
        #[allow(clippy::identity_op)]
        let expected_sizes = vec![
            75 * 44 * 16,
            38 * 22 * 16,
            19 * 11 * 16,
            10 * 6 * 16,
            5 * 3 * 16,
            3 * 2 * 16,
            1 * 1 * 16, // 4×2  → 单块
            1 * 1 * 16, // 2×1  → 单块
            1 * 1 * 16, // 1×1  → 单块
        ];
        assert_eq!(info.level_sizes, expected_sizes);
        assert_eq!(
            bytes.len(),
            crate::dds::DDS_TOTAL_HEADER_SIZE + info.level_sizes.iter().sum::<usize>()
        );
    }

    #[test]
    fn every_mip_level_in_the_payload_decodes_back_to_its_source_level() {
        // 只校验第 0 层是不够的: "mip 数据写反了顺序"、"每层写到了错误的
        // 偏移"、"某几层压根没写(全 0)" 这三类 bug 全都能通过尺寸检查 ——
        // 每层的字节数都是对的, 只是内容不对。
        // 这里把载荷按 level_sizes 切开, 逐层解码, 和 CPU 生成的对应层比 PSNR。
        // 走 sRGB/不透明这一支, 落到 BC1 —— 这条用例验的是"每层写到了正确
        // 的偏移", 与用哪个编码器无关, 挑最便宜的那个。
        let img = gradient(256, 256);
        let (bytes, _) = bake(
            &img,
            SourceTraits {
                channels: 3,
                alpha_significant: false,
            },
            BakeOptions {
                format: FormatOption::Auto,
                color_space: ColorSpace::Srgb,
                mipmaps: true,
                verify: false,
                min_psnr: None,
            },
        )
        .unwrap();

        let info = crate::dds::parse(&bytes).unwrap();
        let expected_chain = crate::mip::generate_mip_chain(&img, ColorSpace::Srgb);
        assert_eq!(info.mip_count as usize, expected_chain.len());

        // 逐层做 **逐字节** 比对而不是比 PSNR。
        // PSNR 需要一个阈值, 而小 mip 层本来就更难压 (同样的 0..255 跨度
        // 挤进 16 个像素, 块内梯度陡得多, 实测 16×16 层只有 27 dB) ——
        // 阈值要么松到抓不住错位, 要么紧到误报。压缩是确定性的, 所以
        // "这一层的字节 == 重新压一遍这一层得到的字节" 是个不需要阈值的
        // 精确判据, 且顺序、偏移、漏写三类错误全都能抓。
        let mut offset = info.payload_offset;
        for (i, (size, expected_level)) in info
            .level_sizes
            .iter()
            .zip(expected_chain.iter())
            .enumerate()
        {
            let stored = &bytes[offset..offset + size];
            offset += size;

            let recompressed = compress_level(expected_level, BcFormat::Bc1);
            assert_eq!(
                stored,
                recompressed.as_slice(),
                "第 {} 层 ({}×{}) 的载荷不是这一层自己的数据 —— \
                 检查 mip 顺序与写入偏移",
                i,
                expected_level.width,
                expected_level.height
            );
        }

        assert_eq!(offset, bytes.len(), "逐层切完之后应当正好用尽整个文件");

        // 再确认各层内容确实互不相同, 否则"每层都写成第 0 层"这种错误
        // 会在上面的比对里被 recompressed 一起带偏而看不出来。
        let level0 = &bytes[info.payload_offset..info.payload_offset + info.level_sizes[0]];
        let level1_start = info.payload_offset + info.level_sizes[0];
        let level1 = &bytes[level1_start..level1_start + info.level_sizes[1]];
        assert_ne!(&level0[..info.level_sizes[1]], level1, "第 0 层与第 1 层不该相同");
    }

    #[test]
    fn tiny_image_still_produces_one_full_block() {
        // 1×1 源图: 一层, 一个块。这是最容易在"height/4 = 0"上翻车的输入。
        let img = Rgba8Image::new(1, 1, vec![255, 0, 0, 255]);
        let (bytes, report) = bake(
            &img,
            SourceTraits {
                channels: 3,
                alpha_significant: false,
            },
            BakeOptions {
                format: FormatOption::Auto,
                color_space: ColorSpace::Srgb,
                mipmaps: true,
                verify: false,
                min_psnr: None,
            },
        )
        .unwrap();
        assert_eq!(report.mip_count, 1);
        assert_eq!(bytes.len(), crate::dds::DDS_TOTAL_HEADER_SIZE + 8);
    }

    #[test]
    fn no_mipmaps_option_writes_single_level() {
        let img = gradient(64, 64);
        let (bytes, report) = bake(
            &img,
            SourceTraits {
                channels: 3,
                alpha_significant: false,
            },
            BakeOptions {
                format: FormatOption::Auto,
                color_space: ColorSpace::Srgb,
                mipmaps: false,
                verify: false,
                min_psnr: None,
            },
        )
        .unwrap();
        assert_eq!(report.mip_count, 1);
        let info = crate::dds::parse(&bytes).unwrap();
        assert_eq!(info.mip_count, 1);
        assert_eq!(bytes.len(), crate::dds::DDS_TOTAL_HEADER_SIZE + 64 * 64 / 2);
    }

    // ── mip 链收敛性检查本身 ───────────────────────────────────────────
    // 这条检查在正确代码下永远不会触发, 所以必须能直接喂给它坏数据,
    // 否则它就是一条不会失败的检查。

    fn dummy(w: u32, h: u32) -> Rgba8Image {
        Rgba8Image::new(w, h, vec![0u8; w as usize * h as usize * 4])
    }

    #[test]
    fn chain_convergence_accepts_a_real_chain() {
        let chain = crate::mip::generate_mip_chain(&dummy(300, 173), ColorSpace::Linear);
        assert!(ensure_chain_converges(&chain).is_ok());
    }

    #[test]
    fn chain_convergence_rejects_early_stop() {
        // 停在 4×4 —— "next_mip_size 的下限写成 4" 这类 bug 的形态。
        let chain = vec![dummy(16, 16), dummy(8, 8), dummy(4, 4)];
        let e = ensure_chain_converges(&chain).unwrap_err().to_string();
        assert!(e.contains("没有收敛到 1×1"), "实际: {}", e);
        assert!(e.contains("4×4"), "错误信息要说清停在哪, 实际: {}", e);
    }

    #[test]
    fn chain_convergence_rejects_non_shrinking_step() {
        // 中间某层忘了减半。末层仍是 1×1, 只看末层抓不住它。
        let chain = vec![dummy(8, 8), dummy(8, 8), dummy(4, 4), dummy(2, 2), dummy(1, 1)];
        let e = ensure_chain_converges(&chain).unwrap_err().to_string();
        assert!(e.contains("没有缩小"), "实际: {}", e);
    }

    #[test]
    fn chain_convergence_rejects_empty() {
        assert!(ensure_chain_converges(&[]).is_err());
    }

    #[test]
    fn bake_propagates_format_conflict_as_error() {
        // sRGB + BC5 必须在这里就失败, 而不是写出一个丢了 sRGB 标记的文件。
        let img = gradient(64, 64);
        let err = bake(
            &img,
            SourceTraits {
                channels: 3,
                alpha_significant: false,
            },
            BakeOptions {
                format: FormatOption::Bc5,
                color_space: ColorSpace::Srgb,
                mipmaps: true,
                verify: false,
                min_psnr: None,
            },
        )
        .unwrap_err();
        assert!(format!("{:#}", err).contains("sRGB"));
    }
}
