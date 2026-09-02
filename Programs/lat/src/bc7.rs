/*******************************************************************************
 * 文件: bc7.rs
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   BC7 (DXGI_FORMAT_BC7_UNORM / _UNORM_SRGB) 的编码器与解码器, 纯 CPU、
 *   零第三方依赖。BC1..BC5 走 block_compression, BC7 走这里。
 *
 * 为什么要自己写:
 *   lat 现有的 block_compression 只开了 `bc15` feature。打开它的 `bc7`
 *   会把 wgpu 拉进依赖树 (它的 BC7 是 GPU compute 路径), 而这个工具的
 *   全部价值就在于"离线、无 GPU、可在 CI 里跑"。所以 BC7 自己实现。
 *
 * ── BC7 的结构 ──────────────────────────────────────────────────────────
 *   每个 4×4 块固定 128 bit。开头是一元编码的 mode: 第一个 1 之前有几个 0
 *   就是几号 mode (mode 6 = 六个 0 加一个 1, 共 7 bit)。8 种 mode 各自
 *   规定了: 子集数、分区表位数、端点位宽、p 位形态、索引位宽。一个块只能
 *   用一种 mode —— 编码器的工作就是替每个块挑 mode、挑分区、拟合端点。
 *
 * ── 实现了哪几种 mode, 为什么 ──────────────────────────────────────────
 *   八种全产出。解码器同样八种全支持 (见 `decode_block`) —— 解码器是拿来
 *   验编码器的 oracle, 它必须按规范完整实现, 否则"编码器写错、解码器跟着
 *   错"就会互相掩盖。
 *
 *   * **Mode 6** — 单子集 / RGBA / 7 位端点 + 每端点 1 位 p / 4 位索引。
 *     唯一给到 4 位 (16 级) 索引的 mode, 也是唯一能同时表达 alpha 与
 *     全精度插值的 mode。端点 7+1 = 8 位意味着 *纯色块无损*。它是兜底:
 *     任何块都能用它编, 其它 mode 只是在特定形态上比它更省。
 *
 *   * **Mode 1** — 双子集 / RGB / 6 位端点 + 每子集 1 位共享 p / 3 位索引。
 *     块内有色彩边界时 (材质接缝、贴图上的文字、砖缝) 单条端点线必然
 *     跨过边界两侧, 中间的插值点落在谁也不是的地方。双子集把 16 个像素
 *     按 64 张分区表之一切成两半, 各拟合各的线。3 位索引 (8 级) 是双子集
 *     里最高的, 所以它是有边界时的首选。
 *
 *   * **Mode 3** — 双子集 / RGB / 7 位端点 + 每端点 1 位 p / 2 位索引。
 *     和 mode 1 相反的取舍: 端点精度换索引精度。边界两侧各自近似平坦、
 *     但两侧颜色差得很细 (例如两块只差几个色阶的墙面) 时, 7+1 位端点
 *     能精确落位, 而 2 位索引够用。mode 1 在这种块上会因为 6 位端点
 *     量化误差反而更差。
 *
 *   * **Mode 7** — 双子集 / RGBA / 5 位端点 + 每端点 1 位 p / 2 位索引。
 *     带 alpha 的边界块专用。抠图 (树叶、铁链) 的边缘上 alpha 是阶跃的,
 *     而颜色仍在变 —— mode 6 只有一套索引, 必须让颜色和 alpha 共用同一个
 *     插值位置, 边缘会同时出现色渗和 alpha 毛边。mode 7 把阶跃两侧分进
 *     不同子集, 各自的端点各自表达, 问题消失。
 *
 *   * **Mode 0 / 2 (三子集)** —— 块内有三种明显不同的材质时, 双子集必须
 *     把其中两种并进一个子集, 而那两种的点张成一个平面, 一条直线盖不住。
 *     mode 0 有 16 张分区、4 位端点、3 位索引; mode 2 有 64 张分区、5 位
 *     端点、2 位索引。
 *
 *   * **Mode 4 / 5 (旋转 + 两套独立索引)** —— 颜色与某一个通道走势完全
 *     无关时 (法线贴图的 z、粗糙度塞进 alpha 的打包贴图、抠图边缘),
 *     一套索引必须让两者共用同一个插值位置, 必然牺牲一个。这两个 mode
 *     给它们各一套索引; "旋转"决定哪个通道享受独立索引 (0 = alpha,
 *     1/2/3 = R/G/B)。mode 5 是 7 位颜色 + 8 位 alpha + 两套 2 位索引;
 *     mode 4 是 5 位颜色 + 6 位 alpha, 两套索引一个 2 位一个 3 位, 外加
 *     一位选择谁用哪套。
 *
 * ── 补齐这四种 mode 值不值 (实测) ───────────────────────────────────────
 *   Sponza 的三张 1024x1024 贴图, 每张 65536 个块, 拿"八种全开"与"只用
 *   原来那四种"逐块对比:
 *
 *                    八种        四种       增益     mode 4/5 占比  mode 0/2 占比
 *     albedo A     48.88 dB   48.67 dB   +0.21 dB      10.8%        0.015%
 *     albedo B     43.99 dB   42.96 dB   +1.03 dB      39.9%        0.03%
 *     albedo C     45.12 dB   44.12 dB   +1.00 dB      68.9%        1.4%
 *
 *   结论分得很开: **mode 4/5 是这次补齐的全部价值**, 而 mode 0/2 在真实
 *   贴图上几乎不被选中 (最多 1.4%, 最少 15 个块)。
 *
 *   代价也量了 (单张 1024x1024 的烘焙时间):
 *
 *     原来四种            0.48 秒
 *     + mode 4/5          0.77 秒   (+60%)
 *     + mode 0/2          0.89 秒   (+87%)
 *     八种全开            1.02 秒   (+113%)
 *
 *   也就是说三子集那一路要了近一半的额外时间, 换来不到 1.5% 的块。留着
 *   是因为它在该赢的形态上赢得干净 (见判据 `three_regions_pick_a_three_subset_mode`),
 *   但要为烘焙速度动刀, 第一刀应该落在这里。
 *
 * ── 误差度量 ────────────────────────────────────────────────────────────
 *   编码器挑 mode/分区/端点时用的是 **RGBA 四通道等权平方误差**, 没有加
 *   任何感知权重 (常见的做法是给 G 通道加权)。理由是验收标准是 PSNR,
 *   而 PSNR 就是等权平方误差 —— 优化目标和验收指标必须是同一个, 否则
 *   报告里的数字和编码器真正在最小化的东西不是一回事。
 *
 ******************************************************************************/

// ── 规范常量 ─────────────────────────────────────────────────────────────

/// 索引 → 插值权重, 分母恒为 64。2/3/4 位索引各一张表。
///
/// 三张表都关于 32 对称 (`w[n-1-i] == 64 - w[i]`)。编码器依赖这条性质:
/// "交换两个端点 + 索引取反" 必须得到逐位相同的解码结果, 这是把索引压进
/// anchor 约束 (见下) 的唯一手段。`weight_tables_are_symmetric` 守着它。
const WEIGHTS_2: [i32; 4] = [0, 21, 43, 64];
const WEIGHTS_3: [i32; 8] = [0, 9, 18, 27, 37, 46, 55, 64];
const WEIGHTS_4: [i32; 16] = [
    0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64,
];

#[inline]
fn weights(index_bits: u32) -> &'static [i32] {
    match index_bits {
        2 => &WEIGHTS_2,
        3 => &WEIGHTS_3,
        4 => &WEIGHTS_4,
        _ => unreachable!("BC7 的索引只可能是 2/3/4 位, 收到 {}", index_bits),
    }
}

/// 双子集分区表, 64 张, 每张说明 16 个 texel 各属于哪个子集。
///
/// 存成位图而不是 `[[u8; 16]; 64]`: 低 16 位是子集 0 的 texel 位图,
/// 高 16 位是子集 1 的。位 i 对应第 i 个 texel (行优先, 0 是左上角)。
/// 双子集时两张位图互补, 所以取子集号只要看高 16 位的一个 bit ——
/// 这正是编码器内层循环要做的事, 展开成数组反而多一次访存。
///
/// **低 16 位在运行时用不到, 保留它是故意的。** 两半是各自独立抄进来的,
/// 于是"低半 == 高半取反"就成了一道覆盖全部 64×16 位的校验和 ——
/// 任何一处一位的手抄错误都会破坏它 (`partition_masks_are_complementary`)。
/// 只存高半的话, 中间某张分区抄错一位就没有任何东西能发现。
///
/// 数值本身是 D3D11 功能规范里的规范表, 不是本工具的设计;
/// `partition_table_matches_the_spec` 另外用手抄的几张分区图对账。
const PARTITION_MASKS_2: [u32; 64] = [
    0xCCCC_3333, 0x8888_7777, 0xEEEE_1111, 0xECC8_1337, 0xC880_377F, 0xFEEC_0113, 0xFEC8_0137,
    0xEC80_137F, 0xC800_37FF, 0xFFEC_0013, 0xFE80_017F, 0xE800_17FF, 0xFFE8_0017, 0xFF00_00FF,
    0xFFF0_000F, 0xF000_0FFF, 0xF710_08EF, 0x008E_FF71, 0x7100_8EFF, 0x08CE_F731, 0x008C_FF73,
    0x7310_8CEF, 0x3100_CEFF, 0x8CCE_7331, 0x088C_F773, 0x3110_CEEF, 0x6666_9999, 0x366C_C993,
    0x17E8_E817, 0x0FF0_F00F, 0x718E_8E71, 0x399C_C663, 0xAAAA_5555, 0xF0F0_0F0F, 0x5A5A_A5A5,
    0x33CC_CC33, 0x3C3C_C3C3, 0x55AA_AA55, 0x9696_6969, 0xA55A_5AA5, 0x73CE_8C31, 0x13C8_EC37,
    0x324C_CDB3, 0x3BDC_C423, 0x6996_9669, 0xC33C_3CC3, 0x9966_6699, 0x0660_F99F, 0x0272_FD8D,
    0x04E4_FB1B, 0x4E40_B1BF, 0x2720_D8DF, 0xC936_36C9, 0x936C_6C93, 0x39C6_C639, 0x639C_9C63,
    0x9336_6CC9, 0x9CC6_6339, 0x817E_7E81, 0xE718_18E7, 0xCCF0_330F, 0x0FCC_F033, 0x7744_88BB,
    0xEE22_11DD,
];

/// 三子集分区表, 64 张。低 16 位 = 子集 0, 高 16 位 = 子集 1,
/// 两者都没置位的 texel 属于子集 2。
///
/// 三子集分区表 —— mode 0 (只用前 16 张) 与 mode 2 (64 张全用) 共用。
/// 解码器还要能读任何合法的 BC7 块, 包括别的工具 (texconv /
/// Compressonator) 产出的。
const PARTITION_MASKS_3: [u32; 64] = [
    0x08CC_0133, 0x8CC8_0037, 0xCC80_006F, 0xEC00_1331, 0x3300_00FF, 0x00CC_3333, 0xFF00_0033,
    0xCCCC_0033, 0x0F00_00FF, 0x0FF0_000F, 0x00F0_000F, 0x4444_3333, 0x6666_1111, 0x2222_1111,
    0x136C_0013, 0x008C_8C63, 0x36C8_0137, 0x08CE_C631, 0x3330_000F, 0xF000_0333, 0x00EE_1111,
    0x8888_0077, 0x22C0_113F, 0x4430_88CF, 0x0C22_F311, 0x0344_0033, 0x6996_9009, 0x9960_009F,
    0x0330_3443, 0x0066_0699, 0xC22C_3113, 0x8C00_00EF, 0x1300_007F, 0xC400_3331, 0x004C_1333,
    0x2222_9999, 0x00F0_F00F, 0x2492_9249, 0x2942_9429, 0xC30C_30C3, 0xC03C_3C03, 0x00AA_0055,
    0xAA00_00FF, 0x3030_0303, 0xC0C0_3333, 0x9090_0909, 0xA00A_5005, 0xAAA0_000F, 0x0AAA_0555,
    0xE0E0_1111, 0x7070_0707, 0x6660_000F, 0x0EE0_1111, 0x0770_7007, 0x0666_0999, 0x6600_00FF,
    0x0066_0099, 0x0CC0_3333, 0x0330_3003, 0x6000_0FFF, 0x8080_7777, 0x1010_0101, 0x000A_0005,
    0x08CE_8421,
];

/// 每个子集里有一个 texel 的索引 **少一位** —— 它的最高位被规范定义为 0,
/// 因此不写进码流。这是 BC7 用来吃掉"端点可以整体交换"这一自由度的手段:
/// 没有它, 同一个块会有两种等价编码, 白白浪费 1 bit/子集。
///
/// 子集 0 的 anchor 恒为 texel 0 (所有分区表的第 0 个 texel 都属于子集 0)。
/// 子集 1/2 的 anchor 位置随分区而变, 就是下面这三张表。
const ANCHOR_2ND_OF_2: [u8; 64] = [
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8,
    2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2,
    2, 15, 15, 15, 15, 15, 2, 2, 15,
];
const ANCHOR_2ND_OF_3: [u8; 64] = [
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5,
    15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5,
    10, 8, 13, 15, 12, 3, 3,
];
const ANCHOR_3RD_OF_3: [u8; 64] = [
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6,
    10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
];

#[inline]
fn subset_of_2(partition: usize, texel: usize) -> usize {
    ((PARTITION_MASKS_2[partition] >> (16 + texel)) & 1) as usize
}

#[inline]
/// 按子集数选表 —— 编码与解码共用一个入口, 免得两边各写一份 match
fn subset_of(subsets: usize, partition: usize, texel: usize) -> usize {
    match subsets {
        1 => 0,
        2 => subset_of_2(partition, texel),
        _ => subset_of_3(partition, texel),
    }
}

fn subset_of_3(partition: usize, texel: usize) -> usize {
    let m = PARTITION_MASKS_3[partition];
    if (m >> texel) & 1 != 0 {
        0
    } else if (m >> (16 + texel)) & 1 != 0 {
        1
    } else {
        2
    }
}

/// 一种 mode 的全部结构参数。字段名沿用规范里的缩写, 便于对照。
#[derive(Copy, Clone, Debug)]
struct ModeInfo {
    /// NS: 子集数 (1 / 2 / 3)
    subsets: usize,
    /// PB: 分区号位数 (0 = 无分区)
    partition_bits: u32,
    /// RB: 旋转位数 —— 解码末尾把 alpha 与某个颜色通道对调
    rotation_bits: u32,
    /// ISB: 索引集选择位 —— 决定两套索引里哪套给颜色
    index_sel_bits: u32,
    /// CB: 每个颜色端点分量的位宽 (不含 p 位)
    color_bits: u32,
    /// AB: 每个 alpha 端点的位宽; 0 表示该 mode 不带 alpha, 解码恒为 255
    alpha_bits: u32,
    /// EPB: 每个端点 1 位 p
    endpoint_pbits: u32,
    /// SPB: 每个子集 1 位 p, 两个端点共用
    shared_pbits: u32,
    /// IB: 主索引位宽
    index_bits: u32,
    /// IB2: 次索引位宽; 0 表示只有一套索引
    index_bits2: u32,
}

/// 8 种 mode 的参数表 (D3D11 规范 表 "BC7 mode 定义")。
///
/// `mode_table_bit_budget_is_exactly_128` 会用这张表反算每种 mode 的位数
/// 总和 —— 任何一个字段抄错都会让某一行不等于 128。
const MODES: [ModeInfo; 8] = [
    // 0: 三子集, 4 位端点, 3 位索引
    ModeInfo { subsets: 3, partition_bits: 4, rotation_bits: 0, index_sel_bits: 0, color_bits: 4, alpha_bits: 0, endpoint_pbits: 1, shared_pbits: 0, index_bits: 3, index_bits2: 0 },
    // 1: 双子集, 6 位端点 + 共享 p, 3 位索引
    ModeInfo { subsets: 2, partition_bits: 6, rotation_bits: 0, index_sel_bits: 0, color_bits: 6, alpha_bits: 0, endpoint_pbits: 0, shared_pbits: 1, index_bits: 3, index_bits2: 0 },
    // 2: 三子集, 5 位端点, 2 位索引, 无 p 位
    ModeInfo { subsets: 3, partition_bits: 6, rotation_bits: 0, index_sel_bits: 0, color_bits: 5, alpha_bits: 0, endpoint_pbits: 0, shared_pbits: 0, index_bits: 2, index_bits2: 0 },
    // 3: 双子集, 7 位端点 + 每端点 p, 2 位索引
    ModeInfo { subsets: 2, partition_bits: 6, rotation_bits: 0, index_sel_bits: 0, color_bits: 7, alpha_bits: 0, endpoint_pbits: 1, shared_pbits: 0, index_bits: 2, index_bits2: 0 },
    // 4: 单子集, 颜色/alpha 两套索引 (2 位 + 3 位) + 旋转
    ModeInfo { subsets: 1, partition_bits: 0, rotation_bits: 2, index_sel_bits: 1, color_bits: 5, alpha_bits: 6, endpoint_pbits: 0, shared_pbits: 0, index_bits: 2, index_bits2: 3 },
    // 5: 单子集, 颜色/alpha 两套 2 位索引 + 旋转, alpha 端点 8 位
    ModeInfo { subsets: 1, partition_bits: 0, rotation_bits: 2, index_sel_bits: 0, color_bits: 7, alpha_bits: 8, endpoint_pbits: 0, shared_pbits: 0, index_bits: 2, index_bits2: 2 },
    // 6: 单子集, RGBA 7 位端点 + 每端点 p, 4 位索引
    ModeInfo { subsets: 1, partition_bits: 0, rotation_bits: 0, index_sel_bits: 0, color_bits: 7, alpha_bits: 7, endpoint_pbits: 1, shared_pbits: 0, index_bits: 4, index_bits2: 0 },
    // 7: 双子集, RGBA 5 位端点 + 每端点 p, 2 位索引
    ModeInfo { subsets: 2, partition_bits: 6, rotation_bits: 0, index_sel_bits: 0, color_bits: 5, alpha_bits: 5, endpoint_pbits: 1, shared_pbits: 0, index_bits: 2, index_bits2: 0 },
];

/// 把 `bits` 位的原始端点值 (已含 p 位) 拉伸到 8 位。
///
/// 规范的做法是左移到高位后 **用自己的高位补低位**, 而不是补 0 ——
/// 补 0 的话最大值 0b1111 会变成 240 而不是 255, 纯白就压不出纯白了。
#[inline]
fn expand_to_8(value: u32, bits: u32) -> i32 {
    debug_assert!((4..=8).contains(&bits), "端点位宽 {} 超出规范范围", bits);
    if bits == 8 {
        value as i32
    } else {
        ((value << (8 - bits)) | (value >> (2 * bits - 8))) as i32
    }
}

/// 端点插值。分母 64, 加 32 是四舍五入。
#[inline]
fn interpolate(e0: i32, e1: i32, weight: i32) -> u8 {
    (((64 - weight) * e0 + weight * e1 + 32) >> 6) as u8
}

// ── 位流 ─────────────────────────────────────────────────────────────────

struct BitReader {
    bits: u128,
    pos: u32,
}

impl BitReader {
    #[inline]
    fn read(&mut self, n: u32) -> u32 {
        debug_assert!(self.pos + n <= 128, "读越过了块的 128 bit");
        if n == 0 {
            return 0;
        }
        let v = ((self.bits >> self.pos) & ((1u128 << n) - 1)) as u32;
        self.pos += n;
        v
    }
}

#[derive(Default)]
struct BitWriter {
    bits: u128,
    pos: u32,
}

impl BitWriter {
    #[inline]
    fn write(&mut self, value: u32, n: u32) {
        if n == 0 {
            return;
        }
        debug_assert!(self.pos + n <= 128, "写越过了块的 128 bit");
        debug_assert!(
            (value as u128) < (1u128 << n),
            "值 {} 放不进 {} bit",
            value,
            n
        );
        self.bits |= (value as u128) << self.pos;
        self.pos += n;
    }
}

// ── 解码 ─────────────────────────────────────────────────────────────────

/// 解开一个 BC7 块。8 种 mode 全部支持。
///
/// 这份实现是 **按规范独立写的**, 不复用编码器的任何一步 —— 它存在的
/// 全部意义就是当编码器的 oracle。用编码器的逆过程去验编码器是循环论证:
/// 端点顺序反了、索引位序错了、p 位挂在错误的端点上, 这些错误在"编码器
/// 自己解自己"的往返里全都自洽, 只有一份独立按规范写的解码器能抓住。
///
/// 返回 16 个 RGBA8 texel, 行优先。
pub fn decode_block(block: &[u8; 16]) -> [[u8; 4]; 16] {
    let bits = u128::from_le_bytes(*block);

    // mode 是一元码: 第一个 1 之前的 0 的个数。
    let mut mode = 0u32;
    while mode < 8 && (bits >> mode) & 1 == 0 {
        mode += 1;
    }
    if mode == 8 {
        // 128 位全部以 8 个 0 开头 = 规范里的保留模式, 没有对应的 mode。
        // 规范说结果未定义; 所有已知硬件解码成全 0 (含 alpha), 这里照做。
        return [[0u8; 4]; 16];
    }

    let m = MODES[mode as usize];
    let mut r = BitReader {
        bits,
        pos: mode + 1,
    };

    let partition = r.read(m.partition_bits) as usize;
    let rotation = r.read(m.rotation_bits);
    let index_mode = r.read(m.index_sel_bits);

    // 端点是 **通道优先** 排列: 先全部 R, 再全部 G, 再全部 B, 最后全部 A。
    // 每个通道内部按端点顺序 (子集 0 的 e0, e1, 子集 1 的 e0, e1, ...)。
    // 这个顺序是最容易写反的地方 —— 写成"端点优先"仍然是 128 bit,
    // 文件长度、mode、分区全都对, 只有颜色是乱的。
    let endpoint_count = m.subsets * 2;
    let mut raw = [[0u32; 4]; 6];
    for c in 0..3 {
        for e in raw.iter_mut().take(endpoint_count) {
            e[c] = r.read(m.color_bits);
        }
    }
    if m.alpha_bits > 0 {
        for e in raw.iter_mut().take(endpoint_count) {
            e[3] = r.read(m.alpha_bits);
        }
    }

    let mut pbit = [0u32; 6];
    let has_pbit = m.shared_pbits > 0 || m.endpoint_pbits > 0;
    if m.shared_pbits > 0 {
        for s in 0..m.subsets {
            let p = r.read(1);
            pbit[s * 2] = p;
            pbit[s * 2 + 1] = p;
        }
    } else if m.endpoint_pbits > 0 {
        for e in pbit.iter_mut().take(endpoint_count) {
            *e = r.read(1);
        }
    }

    let mut endpoints = [[0i32; 4]; 6];
    for e in 0..endpoint_count {
        for c in 0..4 {
            endpoints[e][c] = if c == 3 && m.alpha_bits == 0 {
                255
            } else {
                let base = if c == 3 { m.alpha_bits } else { m.color_bits };
                if has_pbit {
                    expand_to_8((raw[e][c] << 1) | pbit[e], base + 1)
                } else {
                    expand_to_8(raw[e][c], base)
                }
            };
        }
    }

    let anchors = anchor_texels(&m, partition);
    let is_anchor = |t: usize| anchors[..m.subsets].contains(&t);

    let mut index1 = [0u32; 16];
    for (t, slot) in index1.iter_mut().enumerate() {
        *slot = r.read(m.index_bits - u32::from(is_anchor(t)));
    }
    let mut index2 = [0u32; 16];
    if m.index_bits2 > 0 {
        for (t, slot) in index2.iter_mut().enumerate() {
            *slot = r.read(m.index_bits2 - u32::from(is_anchor(t)));
        }
    }
    debug_assert_eq!(r.pos, 128, "mode {} 的位数没有正好用满 128", mode);

    let mut out = [[0u8; 4]; 16];
    for (t, texel) in out.iter_mut().enumerate() {
        let s = match m.subsets {
            1 => 0,
            2 => subset_of_2(partition, t),
            _ => subset_of_3(partition, t),
        };

        // 只有一套索引时颜色和 alpha 共用它; 有两套时 index_sel_bits 决定
        // 哪套归颜色 (mode 4 才有这个自由度, mode 5 固定第一套给颜色)。
        let (color_idx, color_bits, alpha_idx, alpha_bits) = if m.index_bits2 == 0 {
            (index1[t], m.index_bits, index1[t], m.index_bits)
        } else if index_mode == 0 {
            (index1[t], m.index_bits, index2[t], m.index_bits2)
        } else {
            (index2[t], m.index_bits2, index1[t], m.index_bits)
        };

        let e0 = endpoints[s * 2];
        let e1 = endpoints[s * 2 + 1];
        let wc = weights(color_bits)[color_idx as usize];
        let wa = weights(alpha_bits)[alpha_idx as usize];

        let mut px = [0u8; 4];
        for c in 0..3 {
            px[c] = interpolate(e0[c], e1[c], wc);
        }
        px[3] = interpolate(e0[3], e1[3], wa);

        // 旋转: 解码的最后一步把 alpha 和某个颜色通道换回来。
        match rotation {
            1 => px.swap(0, 3),
            2 => px.swap(1, 3),
            3 => px.swap(2, 3),
            _ => {}
        }
        *texel = px;
    }

    out
}

/// 各子集的 anchor texel 下标。子集 0 恒为 texel 0。
fn anchor_texels(m: &ModeInfo, partition: usize) -> [usize; 3] {
    match m.subsets {
        1 => [0, usize::MAX, usize::MAX],
        2 => [0, ANCHOR_2ND_OF_2[partition] as usize, usize::MAX],
        _ => [
            0,
            ANCHOR_2ND_OF_3[partition] as usize,
            ANCHOR_3RD_OF_3[partition] as usize,
        ],
    }
}

/// 解开一整张 BC7 图。`out` 是紧凑排列的 RGBA8, 长度 width*height*4。
pub fn decompress_blocks(width: u32, height: u32, blocks: &[u8], out: &mut [u8]) {
    assert!(
        width % 4 == 0 && height % 4 == 0,
        "BC7 解码要求尺寸是 4 的倍数, 收到 {}×{}",
        width,
        height
    );
    let blocks_x = (width / 4) as usize;
    let blocks_y = (height / 4) as usize;
    assert_eq!(
        blocks.len(),
        blocks_x * blocks_y * 16,
        "BC7 块数据长度与尺寸不符"
    );
    assert_eq!(
        out.len(),
        width as usize * height as usize * 4,
        "输出缓冲区长度与尺寸不符"
    );

    let stride = width as usize * 4;
    for by in 0..blocks_y {
        for bx in 0..blocks_x {
            let mut raw = [0u8; 16];
            raw.copy_from_slice(&blocks[(by * blocks_x + bx) * 16..][..16]);
            let texels = decode_block(&raw);
            for y in 0..4 {
                for x in 0..4 {
                    let o = (by * 4 + y) * stride + (bx * 4 + x) * 4;
                    out[o..o + 4].copy_from_slice(&texels[y * 4 + x]);
                }
            }
        }
    }
}

// ── 编码 ─────────────────────────────────────────────────────────────────

/// 每个块要完整评估的分区数。
///
/// 64 张分区表全部做完整拟合太贵 (端点拟合 + 索引分配 + 最小二乘细化),
/// 所以先用一个便宜的判据 (每个子集到其最佳拟合直线的残差平方和, 见
/// `partition_residual`) 给 64 张排个序, 只对最好的几张做完整评估。
/// 4 是实测的拐点: 从 1 → 4 涨约 0.4 dB, 4 → 8 再涨不到 0.05 dB,
/// 而耗时接近线性。
const PARTITION_CANDIDATES: usize = 4;

/// 端点/索引交替细化的轮数。第 1 轮几乎总能改进, 第 3 轮起基本收敛。
const REFINE_ROUNDS: usize = 3;

/// p 位的形态。
#[derive(Copy, Clone, PartialEq, Eq)]
enum PBits {
    /// 没有 p 位 (mode 2)
    None,
    /// 每个子集 1 位, 两端点共用 (mode 1)
    Shared,
    /// 每个端点各 1 位 (mode 0/3/6/7)
    PerEndpoint,
}

impl PBits {
    /// 需要枚举的 (p0, p1) 组合。
    fn candidates(self) -> &'static [(u32, u32)] {
        match self {
            PBits::None => &[(0, 0)],
            PBits::Shared => &[(0, 0), (1, 1)],
            PBits::PerEndpoint => &[(0, 0), (0, 1), (1, 0), (1, 1)],
        }
    }

    fn active(self) -> bool {
        self != PBits::None
    }
}

/// 一个子集拟合完的结果。
#[derive(Clone)]
struct SubsetFit {
    error: f64,
    /// 量化后的端点分量 (不含 p 位), 索引 0 = e0, 1 = e1
    quantized: [[u32; 4]; 2],
    pbits: (u32, u32),
    /// 只有属于本子集的 texel 位置有意义
    indices: [u32; 16],
}

/// 把 [0,255] 的实数分量量化成 `bits` 位 (可选再带 1 位 p)。
///
/// 不是简单的四舍五入: 带 p 位时可取的值集合是 {(q<<1)|p}, p 固定,
/// 所以要在展开后的 8 位域里找最接近的 q, 而不是在量化域里取整。
fn quantize_component(value: f64, bits: u32, pbit: Option<u32>) -> u32 {
    let total = bits + u32::from(pbit.is_some());
    let target = value.round().clamp(0.0, 255.0) as i32;
    let max_q = (1u32 << bits) - 1;

    let raw_est = ((target as f64) * ((1u32 << total) - 1) as f64 / 255.0).round() as u32;
    let est = (if pbit.is_some() { raw_est >> 1 } else { raw_est }).min(max_q);

    let mut best = est;
    let mut best_err = i32::MAX;
    for q in est.saturating_sub(1)..=(est + 1).min(max_q) {
        let raw = match pbit {
            Some(p) => (q << 1) | p,
            None => q,
        };
        let err = (expand_to_8(raw, total) - target).abs();
        if err < best_err {
            best_err = err;
            best = q;
        }
    }
    best
}

/// 量化端点 → 解码器看到的 8 位端点。和 `decode_block` 用的是同一套展开。
fn expand_endpoint(
    quantized: [u32; 4],
    pbit: Option<u32>,
    color_bits: u32,
    alpha_bits: u32,
) -> [i32; 4] {
    let mut out = [0i32; 4];
    for (c, slot) in out.iter_mut().enumerate() {
        if c == 3 && alpha_bits == 0 {
            *slot = 255;
            continue;
        }
        let base = if c == 3 { alpha_bits } else { color_bits };
        *slot = match pbit {
            Some(p) => expand_to_8((quantized[c] << 1) | p, base + 1),
            None => expand_to_8(quantized[c], base),
        };
    }
    out
}

/// 从两个端点造出调色板 (2^index_bits 项)。
fn build_palette(e0: [i32; 4], e1: [i32; 4], index_bits: u32) -> [[i32; 4]; 16] {
    let wt = weights(index_bits);
    let mut pal = [[0i32; 4]; 16];
    for (k, w) in wt.iter().enumerate() {
        for c in 0..4 {
            pal[k][c] = interpolate(e0[c], e1[c], *w) as i32;
        }
    }
    pal
}

/// 给子集里每个 texel 挑最近的调色板项, 返回总平方误差。
///
/// `alpha_active == false` 时 alpha 不参与选择 (该 mode 的解码 alpha 恒为
/// 255), 但 (255 - a)² 仍然记进误差 —— 否则挑 mode 时会以为不带 alpha 的
/// mode 在半透明块上"没有误差", 从而选中它。
/// `alpha_penalty` = 这个 mode 解码出来的 alpha 恒为 255, 因此源图的 alpha
/// 与 255 的差要记进误差里。
///
/// mode 4/5 是例外: 它们不带 alpha 端点是假象 —— alpha 由**另一套索引**单独
/// 编码, 只是不走这个函数。那时把 (255-a)² 记进去会让颜色拟合被一个与它
/// 无关的量牵着走, 选出的端点偏向"让 alpha 看起来对", 而 alpha 根本不由
/// 这里决定。
fn assign_indices(
    texels: &[[i32; 4]; 16],
    members: &[usize],
    palette: &[[i32; 4]; 16],
    index_bits: u32,
    alpha_active: bool,
    alpha_penalty: bool,
    indices: &mut [u32; 16],
) -> f64 {
    let count = 1usize << index_bits;
    let channels = if alpha_active { 4 } else { 3 };
    let mut total = 0f64;

    for &t in members {
        let p = texels[t];
        let mut best = 0u32;
        let mut best_dist = i64::MAX;
        for (k, q) in palette.iter().take(count).enumerate() {
            let mut d = 0i64;
            for c in 0..channels {
                let diff = (p[c] - q[c]) as i64;
                d += diff * diff;
            }
            if d < best_dist {
                best_dist = d;
                best = k as u32;
            }
        }
        indices[t] = best;
        total += best_dist as f64;
        if !alpha_active && alpha_penalty {
            let da = (255 - p[3]) as f64;
            total += da * da;
        }
    }
    total
}

/// 对称矩阵的主特征向量 (幂迭代)。`n` 是有效维数 (3 或 4)。
fn principal_axis(cov: &[[f64; 4]; 4], n: usize) -> [f64; 4] {
    // 起始向量取对角元最大的那一列 —— 用固定的 (1,1,1,1) 在轴恰好与它
    // 正交时会一直迭代不出东西。
    let mut best_c = 0;
    for c in 1..n {
        if cov[c][c] > cov[best_c][best_c] {
            best_c = c;
        }
    }
    let mut v = [0.0f64; 4];
    v[..n].copy_from_slice(&cov[best_c][..n]);

    for _ in 0..8 {
        let mut next = [0.0f64; 4];
        for r in 0..n {
            let mut acc = 0.0;
            for c in 0..n {
                acc += cov[r][c] * v[c];
            }
            next[r] = acc;
        }
        let norm = next[..n].iter().map(|x| x * x).sum::<f64>().sqrt();
        if norm < 1e-12 {
            return [0.0; 4];
        }
        for x in next.iter_mut().take(n) {
            *x /= norm;
        }
        v = next;
    }
    v
}

/// 子集的统计量: 数量、均值、协方差 (未除以 n, 即离差平方和矩阵)。
struct SubsetStats {
    count: f64,
    mean: [f64; 4],
    cov: [[f64; 4]; 4],
}

fn subset_stats(texels: &[[i32; 4]; 16], members: &[usize], n: usize) -> SubsetStats {
    let count = members.len() as f64;
    let mut mean = [0.0f64; 4];
    for &t in members {
        for c in 0..n {
            mean[c] += texels[t][c] as f64;
        }
    }
    for c in 0..n {
        mean[c] /= count;
    }

    let mut cov = [[0.0f64; 4]; 4];
    for &t in members {
        let mut d = [0.0f64; 4];
        for c in 0..n {
            d[c] = texels[t][c] as f64 - mean[c];
        }
        for r in 0..n {
            for c in 0..n {
                cov[r][c] += d[r] * d[c];
            }
        }
    }

    SubsetStats { count, mean, cov }
}

/// 给定索引, 反解使平方误差最小的实数端点 (最小二乘)。
///
/// 这一步是质量的主要来源: 初始端点来自主成分轴的极值投影, 它只保证
/// "线的方向对", 端点位置往往偏。固定索引后端点有闭式最优解, 解完再重新
/// 分配索引, 交替几轮。
fn least_squares_endpoints(
    texels: &[[i32; 4]; 16],
    members: &[usize],
    indices: &[u32; 16],
    index_bits: u32,
) -> Option<([f64; 4], [f64; 4])> {
    let wt = weights(index_bits);
    let (mut a11, mut a12, mut a22) = (0.0f64, 0.0f64, 0.0f64);
    let mut b0 = [0.0f64; 4];
    let mut b1 = [0.0f64; 4];

    for &t in members {
        let w = wt[indices[t] as usize] as f64 / 64.0;
        let u = 1.0 - w;
        a11 += u * u;
        a12 += u * w;
        a22 += w * w;
        for c in 0..4 {
            let v = texels[t][c] as f64;
            b0[c] += u * v;
            b1[c] += w * v;
        }
    }

    let det = a11 * a22 - a12 * a12;
    // 所有 texel 落在同一个索引上时方程退化 (det = 0), 此时端点不唯一,
    // 保留原来的即可。
    if det.abs() < 1e-9 {
        return None;
    }

    let mut e0 = [0.0f64; 4];
    let mut e1 = [0.0f64; 4];
    for c in 0..4 {
        e0[c] = (a22 * b0[c] - a12 * b1[c]) / det;
        e1[c] = (a11 * b1[c] - a12 * b0[c]) / det;
    }
    Some((e0, e1))
}

/// 拟合一个子集: 挑端点、量化、分配索引、交替细化, 最后压进 anchor 约束。
#[allow(clippy::too_many_arguments)]
fn fit_subset(
    texels: &[[i32; 4]; 16],
    members: &[usize],
    color_bits: u32,
    alpha_bits: u32,
    pbits: PBits,
    index_bits: u32,
    anchor: usize,
    alpha_penalty: bool,
) -> SubsetFit {
    let alpha_active = alpha_bits > 0;
    let n = if alpha_active { 4 } else { 3 };

    // ── 起始端点: 主成分轴上的极值投影 ──
    let stats = subset_stats(texels, members, n);
    let axis = principal_axis(&stats.cov, n);
    let (mut t_min, mut t_max) = (f64::INFINITY, f64::NEG_INFINITY);
    for &t in members {
        let mut proj = 0.0;
        for c in 0..n {
            proj += (texels[t][c] as f64 - stats.mean[c]) * axis[c];
        }
        t_min = t_min.min(proj);
        t_max = t_max.max(proj);
    }
    if !t_min.is_finite() || !t_max.is_finite() {
        t_min = 0.0;
        t_max = 0.0;
    }

    let mut start0 = [255.0f64; 4];
    let mut start1 = [255.0f64; 4];
    for c in 0..n {
        start0[c] = (stats.mean[c] + axis[c] * t_min).clamp(0.0, 255.0);
        start1[c] = (stats.mean[c] + axis[c] * t_max).clamp(0.0, 255.0);
    }
    debug_assert!(stats.count > 0.0);

    let mut best: Option<SubsetFit> = None;

    for &(p0, p1) in pbits.candidates() {
        let (pb0, pb1) = if pbits.active() {
            (Some(p0), Some(p1))
        } else {
            (None, None)
        };

        let mut cur0 = start0;
        let mut cur1 = start1;
        let mut fit: Option<SubsetFit> = None;

        for round in 0..REFINE_ROUNDS {
            let mut q0 = [0u32; 4];
            let mut q1 = [0u32; 4];
            for c in 0..4 {
                let bits = if c == 3 { alpha_bits } else { color_bits };
                if bits == 0 {
                    continue;
                }
                q0[c] = quantize_component(cur0[c], bits, pb0);
                q1[c] = quantize_component(cur1[c], bits, pb1);
            }

            let e0 = expand_endpoint(q0, pb0, color_bits, alpha_bits);
            let e1 = expand_endpoint(q1, pb1, color_bits, alpha_bits);
            let palette = build_palette(e0, e1, index_bits);

            let mut indices = [0u32; 16];
            let error = assign_indices(
                texels,
                members,
                &palette,
                index_bits,
                alpha_active,
                alpha_penalty,
                &mut indices,
            );

            let improved = fit.as_ref().is_none_or(|f| error < f.error);
            if improved {
                fit = Some(SubsetFit {
                    error,
                    quantized: [q0, q1],
                    pbits: (p0, p1),
                    indices,
                });
            } else {
                // 细化不再改进就停 —— 继续迭代只会来回震荡。
                break;
            }

            if round + 1 == REFINE_ROUNDS {
                break;
            }
            match least_squares_endpoints(texels, members, &indices, index_bits) {
                Some((n0, n1)) => {
                    for c in 0..n {
                        cur0[c] = n0[c].clamp(0.0, 255.0);
                        cur1[c] = n1[c].clamp(0.0, 255.0);
                    }
                }
                None => break,
            }
        }

        let fit = fit.expect("每个 p 位组合至少要产出一轮结果");
        if best.as_ref().is_none_or(|b| fit.error < b.error) {
            best = Some(fit);
        }
    }

    let mut fit = best.expect("p 位候选集合不可能为空");

    // ── anchor 约束 ──
    // anchor texel 的索引最高位必须是 0。做不到时交换两个端点并把索引取反,
    // 由于权重表关于 32 对称, 这个变换的解码结果逐位相同, 误差也不变。
    let half = 1u32 << (index_bits - 1);
    if fit.indices[anchor] >= half {
        fit.quantized.swap(0, 1);
        fit.pbits = (fit.pbits.1, fit.pbits.0);
        let top = (1u32 << index_bits) - 1;
        for &t in members {
            fit.indices[t] = top - fit.indices[t];
        }
    }
    debug_assert!(fit.indices[anchor] < half, "anchor 索引没有被压进约束");

    fit
}

/// 一个分区的"便宜的坏度": 每个子集到自己最佳拟合直线的残差平方和。
///
/// 这是完整评估之前的排序判据。它不考虑量化、不考虑索引位数, 但抓住了
/// 分区的核心作用 —— 把块切成两半之后, 各半能不能被一条直线拟合。
fn partition_residual(
    texels: &[[i32; 4]; 16],
    partition: usize,
    n: usize,
    total_sum: &[f64; 4],
    total_prod: &[[f64; 4]; 4],
) -> f64 {
    let mut sum0 = [0.0f64; 4];
    let mut prod0 = [[0.0f64; 4]; 4];
    let mut count0 = 0.0f64;

    for t in 0..16 {
        if subset_of_2(partition, t) != 0 {
            continue;
        }
        count0 += 1.0;
        for r in 0..n {
            let vr = texels[t][r] as f64;
            sum0[r] += vr;
            for c in 0..n {
                prod0[r][c] += vr * texels[t][c] as f64;
            }
        }
    }
    let count1 = 16.0 - count0;

    let mut residual = 0.0;
    for s in 0..2 {
        let count = if s == 0 { count0 } else { count1 };
        if count <= 0.0 {
            continue;
        }
        let mut cov = [[0.0f64; 4]; 4];
        let mut trace = 0.0;
        for r in 0..n {
            let sr = if s == 0 {
                sum0[r]
            } else {
                total_sum[r] - sum0[r]
            };
            for c in 0..n {
                let sc = if s == 0 {
                    sum0[c]
                } else {
                    total_sum[c] - sum0[c]
                };
                let p = if s == 0 {
                    prod0[r][c]
                } else {
                    total_prod[r][c] - prod0[r][c]
                };
                cov[r][c] = p - sr * sc / count;
            }
            trace += cov[r][r];
        }

        let axis = principal_axis(&cov, n);
        let mut lambda = 0.0;
        for r in 0..n {
            for c in 0..n {
                lambda += axis[r] * cov[r][c] * axis[c];
            }
        }
        residual += (trace - lambda).max(0.0);
    }

    residual
}

/// 三子集版本的"便宜的坏度"。
///
/// 双子集那一版用增量和省掉了一半的统计 (子集 1 = 全体减子集 0), 三子集
/// 没有这个便利, 于是老老实实逐子集统计。每块 64 张分区 × 16 个 texel,
/// 代价可以接受。
fn partition_residual_3(texels: &[[i32; 4]; 16], partition: usize, n: usize) -> f64 {
    let mut sum = [[0.0f64; 4]; 3];
    let mut prod = [[[0.0f64; 4]; 4]; 3];
    let mut count = [0.0f64; 3];

    for (t, texel) in texels.iter().enumerate() {
        let s = subset_of_3(partition, t);
        count[s] += 1.0;
        for r in 0..n {
            let vr = texel[r] as f64;
            sum[s][r] += vr;
            for c in 0..n {
                prod[s][r][c] += vr * texel[c] as f64;
            }
        }
    }

    let mut residual = 0.0;
    for s in 0..3 {
        if count[s] <= 0.0 {
            continue;
        }
        let mut cov = [[0.0f64; 4]; 4];
        let mut trace = 0.0;
        for r in 0..n {
            for c in 0..n {
                cov[r][c] = prod[s][r][c] - sum[s][r] * sum[s][c] / count[s];
            }
            trace += cov[r][r];
        }

        let axis = principal_axis(&cov, n);
        let mut lambda = 0.0;
        for r in 0..n {
            for c in 0..n {
                lambda += axis[r] * cov[r][c] * axis[c];
            }
        }
        residual += (trace - lambda).max(0.0);
    }

    residual
}

/// 三子集分区排序 —— 一次排完 64 张, 同时给出 mode 0 能用的那一档。
///
/// mode 0 与 mode 2 共用同一张三子集分区表, 只是 mode 0 的分区号只有 4 位
/// (前 16 张)。一次排完是为了不把那 16 张的残差算两遍。
///
/// **它不省时间。** 分开排与合并排在 1024x1024 的贴图上都是 1.02～1.04 秒,
/// 差在噪声里 —— 贵的是那 64 张的残差本身 (每张 16 个 texel 的三份协方差
/// 加幂迭代), 而合并前后都要算这 64 张。合并只是少写一遍同样的东西。
///
/// 三子集这一路的代价是实测出来的: 单独加它让整张贴图的烘焙从 0.48 秒涨到
/// 0.89 秒 (+87%), 而它在这张贴图上一共只赢下 10 个块 (65536 个里)。留着是
/// 因为在三种材质挤在一个块里时它赢得干净利落 (见判据), 但要为烘焙速度
/// 动刀的话, 这里是第一刀。
fn rank_partitions_3(
    texels: &[[i32; 4]; 16],
    n: usize,
) -> ([usize; PARTITION_CANDIDATES], [usize; PARTITION_CANDIDATES]) {
    let mut scored = [(0.0f64, 0usize); 64];
    for (p, slot) in scored.iter_mut().enumerate() {
        *slot = (partition_residual_3(texels, p, n), p);
    }
    scored.sort_by(|a, b| a.0.total_cmp(&b.0));

    let all = std::array::from_fn(|i| scored[i].1);

    // mode 0 只能用编号 < 16 的那些
    let mut low = [0usize; PARTITION_CANDIDATES];
    let mut filled = 0;
    for &(_, p) in scored.iter() {
        if p < 16 {
            low[filled] = p;
            filled += 1;
            if filled == PARTITION_CANDIDATES {
                break;
            }
        }
    }

    (all, low)
}

/// 按 `partition_residual` 排序, 取最好的 `PARTITION_CANDIDATES` 张分区。
fn rank_partitions(texels: &[[i32; 4]; 16], n: usize) -> [usize; PARTITION_CANDIDATES] {
    let mut total_sum = [0.0f64; 4];
    let mut total_prod = [[0.0f64; 4]; 4];
    for t in 0..16 {
        for r in 0..n {
            let vr = texels[t][r] as f64;
            total_sum[r] += vr;
            for c in 0..n {
                total_prod[r][c] += vr * texels[t][c] as f64;
            }
        }
    }

    let mut scored = [(0.0f64, 0usize); 64];
    for (p, slot) in scored.iter_mut().enumerate() {
        *slot = (
            partition_residual(texels, p, n, &total_sum, &total_prod),
            p,
        );
    }
    scored.sort_by(|a, b| a.0.total_cmp(&b.0));

    std::array::from_fn(|i| scored[i].1)
}

/// 用指定的 mode + 分区编一个块, 返回 (平方误差, 128 bit 码流)。
///
/// 只处理编码器实际会产出的形态: 单/双子集、一套索引、无旋转。
fn encode_with_mode(mode: usize, partition: usize, texels: &[[i32; 4]; 16]) -> (f64, [u8; 16]) {
    let m = MODES[mode];
    debug_assert!(m.rotation_bits == 0 && m.index_sel_bits == 0 && m.index_bits2 == 0);

    let pbits = if m.shared_pbits > 0 {
        PBits::Shared
    } else if m.endpoint_pbits > 0 {
        PBits::PerEndpoint
    } else {
        PBits::None
    };
    let anchors = anchor_texels(&m, partition);

    let mut members: [Vec<usize>; 3] = [
        Vec::with_capacity(16),
        Vec::with_capacity(16),
        Vec::with_capacity(16),
    ];
    for t in 0..16 {
        members[subset_of(m.subsets, partition, t)].push(t);
    }

    let mut fits = Vec::with_capacity(m.subsets);
    let mut error = 0.0;
    for s in 0..m.subsets {
        let fit = fit_subset(
            texels,
            &members[s],
            m.color_bits,
            m.alpha_bits,
            pbits,
            m.index_bits,
            anchors[s],
            true,
        );
        error += fit.error;
        fits.push(fit);
    }

    // ── 打包 ──
    let mut w = BitWriter::default();
    // 一元 mode 码: 低位起 mode 个 0, 然后一个 1。写成 `write(1, mode+1)`
    // 会把那个 1 放在第 0 位, 也就是永远写成 mode 0 —— 而 mode 0 的位数
    // 合计同样是 128, 解码器照样读得下去, 只是颜色全乱。
    w.write(1u32 << mode, mode as u32 + 1);
    w.write(partition as u32, m.partition_bits);

    // 通道优先, 通道内按端点顺序 —— 与 decode_block 的读取顺序严格对应。
    for c in 0..3 {
        for fit in &fits {
            w.write(fit.quantized[0][c], m.color_bits);
            w.write(fit.quantized[1][c], m.color_bits);
        }
    }
    if m.alpha_bits > 0 {
        for fit in &fits {
            w.write(fit.quantized[0][3], m.alpha_bits);
            w.write(fit.quantized[1][3], m.alpha_bits);
        }
    }

    if m.shared_pbits > 0 {
        for fit in &fits {
            debug_assert_eq!(fit.pbits.0, fit.pbits.1, "共享 p 位的两端必须相同");
            w.write(fit.pbits.0, 1);
        }
    } else if m.endpoint_pbits > 0 {
        for fit in &fits {
            w.write(fit.pbits.0, 1);
            w.write(fit.pbits.1, 1);
        }
    }

    for t in 0..16 {
        let s = subset_of(m.subsets, partition, t);
        let bits = m.index_bits - u32::from(anchors[..m.subsets].contains(&t));
        w.write(fits[s].indices[t], bits);
    }

    debug_assert_eq!(w.pos, 128, "mode {} 打包后不是 128 bit", mode);
    (error, w.bits.to_le_bytes())
}


/// 单通道 (alpha) 的拟合结果
struct ScalarFit {
    error: f64,
    /// 量化后的两个端点 (不含 p 位 —— mode 4/5 没有 p 位)
    quantized: [u32; 2],
    indices: [u32; 16],
}

/// 把一个通道拟合成"两端点 + 一套索引"。
///
/// 与 `fit_subset` 是同一套路数, 但只有一维: 起点取极值, 量化, 逐 texel 取
/// 最近的调色板项, 再按索引做一次最小二乘重拟合。一维下最小二乘有闭式解,
/// 不必走主成分那一套。
fn fit_scalar(values: &[i32; 16], bits: u32, index_bits: u32, anchor: usize) -> ScalarFit {
    let mut lo = values[0] as f64;
    let mut hi = values[0] as f64;
    for &v in values.iter() {
        lo = lo.min(v as f64);
        hi = hi.max(v as f64);
    }

    let table = weights(index_bits);
    let count = 1usize << index_bits;

    let mut best: Option<ScalarFit> = None;

    for _ in 0..REFINE_ROUNDS {
        let q0 = quantize_component(lo, bits, None);
        let q1 = quantize_component(hi, bits, None);

        let e0 = expand_to_8(q0, bits);
        let e1 = expand_to_8(q1, bits);

        let mut palette = [0i32; 16];
        for (k, slot) in palette.iter_mut().enumerate().take(count) {
            *slot = i32::from(interpolate(e0, e1, table[k]));
        }

        let mut indices = [0u32; 16];
        let mut error = 0.0f64;

        for (t, &v) in values.iter().enumerate() {
            let mut pick = 0u32;
            let mut best_dist = i64::MAX;
            for (k, &p) in palette.iter().enumerate().take(count) {
                let d = ((v - p) as i64) * ((v - p) as i64);
                if d < best_dist {
                    best_dist = d;
                    pick = k as u32;
                }
            }
            indices[t] = pick;
            error += best_dist as f64;
        }

        let improved = best.as_ref().is_none_or(|b| error < b.error);
        if !improved {
            break;
        }

        best = Some(ScalarFit {
            error,
            quantized: [q0, q1],
            indices,
        });

        // 一维最小二乘: 把 v ≈ a + (b-a)·w 里的 a、b 解出来
        let (mut s_ww, mut s_w, mut s_1, mut s_vw, mut s_v) = (0.0, 0.0, 0.0, 0.0, 0.0);
        for (t, &v) in values.iter().enumerate() {
            let w = table[indices[t] as usize] as f64 / 64.0;
            s_ww += w * w;
            s_w += w;
            s_1 += 1.0;
            s_vw += v as f64 * w;
            s_v += v as f64;
        }
        let det = s_1 * s_ww - s_w * s_w;
        if det.abs() < 1.0e-9 {
            break;
        }
        let a = (s_v * s_ww - s_w * s_vw) / det;
        let b = (s_1 * s_vw - s_w * s_v) / det;
        lo = a.clamp(0.0, 255.0);
        hi = (a + b).clamp(0.0, 255.0);
    }

    let mut fit = best.expect("至少要产出一轮结果");

    // anchor 约束 —— 与 fit_subset 同理: 权重表关于 32 对称, 交换端点并把
    // 索引取反之后解码逐位相同。
    let half = 1u32 << (index_bits - 1);
    if fit.indices[anchor] >= half {
        fit.quantized.swap(0, 1);
        let top = (1u32 << index_bits) - 1;
        for slot in fit.indices.iter_mut() {
            *slot = top - *slot;
        }
    }
    debug_assert!(fit.indices[anchor] < half);

    fit
}

/// mode 4 / 5: 单子集, 旋转, 颜色与 alpha 各用一套索引。
///
/// ── 旋转是干什么的 ──
///
/// 解码的最后一步把 alpha 与某个颜色通道对调。于是"哪个通道享受独立索引"
/// 就成了可选的: 旋转 0 给 alpha, 旋转 1/2/3 分别给 R/G/B。对"三个通道里
/// 有一个走势与另外两个完全不同"的块 (法线贴图的 z、粗糙度塞进 alpha 的
/// 打包贴图), 这比让四个通道共用一套索引省得多。
///
/// 编码侧只要把源 texel 先做同样的对调, 后面就当成普通的 "RGB + 单通道"
/// 来拟合 —— 误差在对调前后是同一个数, 因为对调只是通道换位置。
fn encode_with_dual_index(
    mode: usize,
    rotation: u32,
    index_mode: u32,
    texels: &[[i32; 4]; 16],
) -> (f64, [u8; 16]) {
    let m = MODES[mode];
    debug_assert_eq!(m.subsets, 1);
    debug_assert!(m.index_bits2 > 0 && m.rotation_bits == 2);
    debug_assert!(m.endpoint_pbits == 0 && m.shared_pbits == 0);

    // 与解码末尾那次 swap 一致
    let mut rotated = *texels;
    if rotation > 0 {
        let c = (rotation - 1) as usize;
        for texel in rotated.iter_mut() {
            texel.swap(c, 3);
        }
    }

    let (color_index_bits, alpha_index_bits) = if index_mode == 0 {
        (m.index_bits, m.index_bits2)
    } else {
        (m.index_bits2, m.index_bits)
    };

    let all: Vec<usize> = (0..16).collect();

    // 颜色: 三通道拟合, **不记** alpha 缺失惩罚 —— alpha 由下面那套索引管
    let color = fit_subset(
        &rotated,
        &all,
        m.color_bits,
        0,
        PBits::None,
        color_index_bits,
        0,
        false,
    );

    let alpha_values: [i32; 16] = std::array::from_fn(|t| rotated[t][3]);
    let alpha = fit_scalar(&alpha_values, m.alpha_bits, alpha_index_bits, 0);

    let error = color.error + alpha.error;

    // ── 打包 ──
    let mut w = BitWriter::default();
    w.write(1u32 << mode, mode as u32 + 1);
    w.write(rotation, m.rotation_bits);
    if m.index_sel_bits > 0 {
        w.write(index_mode, m.index_sel_bits);
    }

    for c in 0..3 {
        w.write(color.quantized[0][c], m.color_bits);
        w.write(color.quantized[1][c], m.color_bits);
    }
    w.write(alpha.quantized[0], m.alpha_bits);
    w.write(alpha.quantized[1], m.alpha_bits);

    // 存储上的第一套索引宽度恒为 m.index_bits, 第二套恒为 m.index_bits2;
    // index_mode 只决定**谁用哪一套**, 不改变存储宽度。
    let (set1, set2) = if index_mode == 0 {
        (&color.indices, &alpha.indices)
    } else {
        (&alpha.indices, &color.indices)
    };

    for (t, &v) in set1.iter().enumerate() {
        w.write(v, m.index_bits - u32::from(t == 0));
    }
    for (t, &v) in set2.iter().enumerate() {
        w.write(v, m.index_bits2 - u32::from(t == 0));
    }

    debug_assert_eq!(w.pos, 128, "mode {} 打包后不是 128 bit", mode);
    (error, w.bits.to_le_bytes())
}

/// 编一个 4×4 RGBA8 块。
///
/// 策略: mode 6 (单子集) 永远评估, 作为兜底; 再按分区残差挑出最好的
/// 几张分区, 对每张评估双子集的 mode。不透明块用 mode 1/3 (它们不带
/// alpha, 解码 alpha 恒为 255); 带 alpha 的块用 mode 7。全部候选里取
/// 平方误差最小的。
pub fn encode_block(src: &[[u8; 4]; 16]) -> [u8; 16] {
    let texels: [[i32; 4]; 16] =
        std::array::from_fn(|t| std::array::from_fn(|c| src[t][c] as i32));
    let opaque = texels.iter().all(|p| p[3] == 255);

    let (mut best_error, mut best) = encode_with_mode(6, 0, &texels);

    // 不透明块才考虑 mode 1/3 —— 它们解码出的 alpha 恒为 255, 用在
    // 半透明块上误差会被 assign_indices 里的 (255-a)² 记账记回来, 不会
    // 被误选, 但白算一遍不划算。
    let two_subset_modes: &[usize] = if opaque { &[1, 3] } else { &[7] };
    let channels = if opaque { 3 } else { 4 };

    for &partition in rank_partitions(&texels, channels).iter() {
        for &mode in two_subset_modes {
            let (error, block) = encode_with_mode(mode, partition, &texels);
            if error < best_error {
                best_error = error;
                best = block;
            }
        }
    }

    // ── 三子集 (mode 0 / 2) ──
    //
    // 两者都不带 alpha, 所以只对不透明块评估 —— 半透明块上它们解码出的
    // alpha 恒为 255, 误差会被记回来而不至于误选, 但白算一遍不划算。
    //
    // mode 0 只有 16 张分区 (4 位分区号), mode 2 有 64 张。分开排序: 拿
    // 64 张里最好的四张去喂 mode 0, 其中三张它根本编不了。
    if opaque {
        let (all, low) = rank_partitions_3(&texels, channels);

        for &partition in low.iter() {
            let (error, block) = encode_with_mode(0, partition, &texels);
            if error < best_error {
                best_error = error;
                best = block;
            }
        }
        for &partition in all.iter() {
            let (error, block) = encode_with_mode(2, partition, &texels);
            if error < best_error {
                best_error = error;
                best = block;
            }
        }
    }

    // ── 旋转 + 两套索引 (mode 4 / 5) ──
    //
    // 四种旋转都试。不透明块也试 —— 旋转 0 之外的三种把一个**颜色**通道
    // 挪进独立索引, 而那正是法线贴图 (z 与 xy 走势无关) 这类块受益的地方。
    // 旋转 0 时 alpha 必须编回 255, mode 5 的 8 位 alpha 端点做得到, 编不
    // 好的组合会被误差挡在外面。
    for rotation in 0..4u32 {
        let (error, block) = encode_with_dual_index(5, rotation, 0, &texels);
        if error < best_error {
            best_error = error;
            best = block;
        }

        for index_mode in 0..2u32 {
            let (error, block) = encode_with_dual_index(4, rotation, index_mode, &texels);
            if error < best_error {
                best_error = error;
                best = block;
            }
        }
    }

    best
}

/// 压缩一整张图。`stride` 是源图每行的字节数 (允许大于 width*4)。
///
/// 尺寸必须已经补齐到 4 的倍数 —— 补齐由 `mip::pad_to_block_multiple`
/// 负责, 这里只断言, 不悄悄裁掉边缘。
pub fn compress_blocks(pixels: &[u8], out: &mut [u8], width: u32, height: u32, stride: u32) {
    assert!(
        width % 4 == 0 && height % 4 == 0,
        "BC7 编码要求尺寸是 4 的倍数, 收到 {}×{}",
        width,
        height
    );
    let blocks_x = (width / 4) as usize;
    let blocks_y = (height / 4) as usize;
    assert_eq!(
        out.len(),
        blocks_x * blocks_y * 16,
        "BC7 输出缓冲区长度与尺寸不符"
    );

    for by in 0..blocks_y {
        for bx in 0..blocks_x {
            let mut texels = [[0u8; 4]; 16];
            for y in 0..4 {
                let row = (by * 4 + y) * stride as usize;
                for x in 0..4 {
                    let o = row + (bx * 4 + x) * 4;
                    texels[y * 4 + x].copy_from_slice(&pixels[o..o + 4]);
                }
            }
            let block = encode_block(&texels);
            let dst = (by * blocks_x + bx) * 16;
            out[dst..dst + 16].copy_from_slice(&block);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── 规范表的自检 ──────────────────────────────────────────────────
    // 这几条守的是"抄表抄错了"这一类错误。抄错的后果非常隐蔽: 文件长度、
    // mode、分区号全都合法, 引擎能加载, 只是画面上多了一层规则的噪点。

    #[test]
    fn mode_table_bit_budget_is_exactly_128() {
        // 每种 mode 的所有字段加起来必须正好 128 bit。这条用 MODES 表
        // 反算总位数 —— 任何一个字段抄错 (端点位宽、索引位宽、p 位形态)
        // 都会让对应的行不等于 128。
        for (mode, m) in MODES.iter().enumerate() {
            let mode_bits = mode as u32 + 1;
            let endpoints = (m.subsets as u32) * 2 * 3 * m.color_bits;
            let alpha = if m.alpha_bits > 0 {
                (m.subsets as u32) * 2 * m.alpha_bits
            } else {
                0
            };
            let pbits = m.shared_pbits * m.subsets as u32
                + m.endpoint_pbits * m.subsets as u32 * 2;
            // 每个子集有一个 anchor, 它的索引少一位
            let idx1 = 16 * m.index_bits - m.subsets as u32;
            let idx2 = if m.index_bits2 > 0 {
                16 * m.index_bits2 - m.subsets as u32
            } else {
                0
            };
            let total = mode_bits
                + m.partition_bits
                + m.rotation_bits
                + m.index_sel_bits
                + endpoints
                + alpha
                + pbits
                + idx1
                + idx2;
            assert_eq!(total, 128, "mode {} 的位数合计是 {}, 应为 128", mode, total);
        }
    }

    #[test]
    fn weight_tables_are_symmetric() {
        // 编码器把索引压进 anchor 约束的手段是"交换端点 + 索引取反",
        // 它成立的前提就是权重表关于 32 对称。表抄错时这条先红。
        for table in [&WEIGHTS_2[..], &WEIGHTS_3[..], &WEIGHTS_4[..]] {
            let n = table.len();
            for i in 0..n {
                assert_eq!(
                    table[n - 1 - i],
                    64 - table[i],
                    "权重表 {:?} 不对称",
                    table
                );
            }
            assert_eq!(table[0], 0);
            assert_eq!(table[n - 1], 64);
        }
    }

    #[test]
    fn partition_table_matches_the_spec() {
        // 手抄规范里几张有代表性的分区图, 与位图形式对账。
        let expected: [(usize, [u8; 16]); 6] = [
            (0, [0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1]),
            (1, [0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]),
            (2, [0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1]),
            (13, [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1]),
            (14, [0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]),
            (32, [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1]),
        ];
        for (p, want) in expected {
            let got: [u8; 16] = std::array::from_fn(|t| subset_of_2(p, t) as u8);
            assert_eq!(got, want, "双子集分区 {} 与规范不符", p);
        }

        // 三子集的第 0 张也对一下, 确认 mask 的三值解释没搞反。
        let want3: [u8; 16] = [0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2];
        let got3: [u8; 16] = std::array::from_fn(|t| subset_of_3(0, t) as u8);
        assert_eq!(got3, want3, "三子集分区 0 与规范不符");
    }

    #[test]
    fn partition_masks_are_complementary() {
        // 双子集分区的两半必须互补。`subset_of_2` 只读高 16 位, 所以低 16 位
        // 在运行时是冗余的 —— 正因为冗余, 它才能当校验和用: 两半各抄各的,
        // 任意一位抄错都会让这条断言红。没有它, 低半的错误无人察觉
        // (变异验证里 M1 就是这么漏掉的)。
        for (p, &mask) in PARTITION_MASKS_2.iter().enumerate() {
            let subset0 = mask & 0xFFFF;
            let subset1 = mask >> 16;
            assert_eq!(
                subset0,
                !subset1 & 0xFFFF,
                "双子集分区 {} 的两半不互补: 子集0 = {:#06X}, 子集1 = {:#06X}",
                p,
                subset0,
                subset1
            );
        }

        // 三子集分区: 两张位图必须互不相交, 且并起来不能盖住全部 16 个
        // texel —— 剩下的那些才是子集 2。
        for (p, &mask) in PARTITION_MASKS_3.iter().enumerate() {
            let subset0 = mask & 0xFFFF;
            let subset1 = mask >> 16;
            assert_eq!(subset0 & subset1, 0, "三子集分区 {} 的前两个子集重叠", p);
            assert_ne!(
                subset0 | subset1,
                0xFFFF,
                "三子集分区 {} 没有给子集 2 留下任何 texel",
                p
            );
        }
    }

    #[test]
    fn first_sixteen_partitions_anchor_at_the_last_texel() {
        // 规范里前 16 张双子集分区都是"沿某条线把块切成两片, 子集 1 在
        // 右/下侧", 它们的 fix-up 索引一律是 15。
        //
        // 这条是照着规范手抄的, 与 ANCHOR_2ND_OF_2 的来源无关 —— 因此它
        // 真的能抓住抄错。**其余 48 项没有这样的独立判据**: anchor 只是
        // "哪个 texel 少写一位", 编码器和解码器共用同一张表, 抄错也能自洽
        // 地往返。那 48 项只能靠端到端 (让 GPU 的硬件解码器当裁判) 来验。
        for p in 0..16 {
            assert_eq!(
                ANCHOR_2ND_OF_2[p], 15,
                "分区 {} 的第二子集 anchor 应当是 15",
                p
            );
        }
        // 后面的分区形状复杂, anchor 不再是 15 —— 顺带确认这条断言不是
        // 对全表都成立的废话。
        assert_ne!(ANCHOR_2ND_OF_2[17], 15);
    }

    #[test]
    fn partition_tables_are_structurally_sound() {
        for p in 0..64 {
            // texel 0 恒属于子集 0 —— 子集 0 的 anchor 固定是它。
            assert_eq!(subset_of_2(p, 0), 0, "双子集分区 {} 的 texel 0 不在子集 0", p);
            assert_eq!(subset_of_3(p, 0), 0, "三子集分区 {} 的 texel 0 不在子集 0", p);

            // 每个子集都必须非空, 否则拟合时会除以 0。
            let mut seen2 = [false; 2];
            let mut seen3 = [false; 3];
            for t in 0..16 {
                seen2[subset_of_2(p, t)] = true;
                seen3[subset_of_3(p, t)] = true;
            }
            assert!(seen2.iter().all(|x| *x), "双子集分区 {} 有空子集", p);
            assert!(seen3.iter().all(|x| *x), "三子集分区 {} 有空子集", p);

            // anchor 必须落在它所属的子集里, 否则那一位就少写在了错误的
            // texel 上, 整块索引会错位。
            assert_eq!(
                subset_of_2(p, ANCHOR_2ND_OF_2[p] as usize),
                1,
                "双子集分区 {} 的 anchor 不在子集 1 里",
                p
            );
            assert_eq!(subset_of_3(p, ANCHOR_2ND_OF_3[p] as usize), 1);
            assert_eq!(subset_of_3(p, ANCHOR_3RD_OF_3[p] as usize), 2);
        }
    }

    #[test]
    fn endpoint_expansion_hits_both_ends() {
        // 展开必须把量化域的最小/最大值映射到 0 和 255。补 0 而不是补高位
        // 的实现会让最大值停在 248/240, 纯白压出来发灰。
        for bits in 4..=8u32 {
            let max = (1u32 << bits) - 1;
            assert_eq!(expand_to_8(0, bits), 0, "{} 位的 0 应展开成 0", bits);
            assert_eq!(expand_to_8(max, bits), 255, "{} 位的最大值应展开成 255", bits);
        }
        // 8 位是恒等映射
        for v in [0u32, 1, 127, 254, 255] {
            assert_eq!(expand_to_8(v, 8), v as i32);
        }
    }

    // ── 往返 ──────────────────────────────────────────────────────────

    fn solid_block(rgba: [u8; 4]) -> [[u8; 4]; 16] {
        [rgba; 16]
    }

    /// 确定性 PRNG (xorshift64*)。测试要可复现, 不引第三方 rand。
    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u32 {
            let mut x = self.0;
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            self.0 = x;
            (x.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 32) as u32
        }
        fn byte(&mut self) -> u8 {
            (self.next() & 0xFF) as u8
        }
    }

    fn max_abs_error(a: &[[u8; 4]; 16], b: &[[u8; 4]; 16]) -> i32 {
        let mut worst = 0;
        for t in 0..16 {
            for c in 0..4 {
                worst = worst.max((a[t][c] as i32 - b[t][c] as i32).abs());
            }
        }
        worst
    }

    fn mse(a: &[[u8; 4]; 16], b: &[[u8; 4]; 16]) -> f64 {
        let mut sum = 0.0;
        for t in 0..16 {
            for c in 0..4 {
                let d = a[t][c] as f64 - b[t][c] as f64;
                sum += d * d;
            }
        }
        sum / 64.0
    }

    /// 纯色块 / 双色块的每通道误差上限。
    ///
    /// **为什么不是 0。** mode 6 的端点是 7 位 + 1 位 p = 8 位, 看上去
    /// 任意 8 位颜色都能精确表示 —— 但那 1 位 p 是 **一个端点的四个通道
    /// 共用的**。取定 p 之后, 该端点能表达的只有"最低位等于 p"的那 128 个
    /// 8 位值。一个像 (32, 1, 107, 154) 这样四个通道最低位不一致的颜色,
    /// 无论 p 取 0 还是 1 都至少有两个通道差 1。
    ///
    /// 所以 1 是这条路径上可证明的上限, 而不是一个拍脑袋的容差:
    /// 「与 t 同奇偶的最近 8 位值」离 t 至多 1。纯色块的索引会落在端点上,
    /// 误差就是端点的量化误差。
    ///
    /// (理论上存在能精确表示的构造: 令两个端点取不同的 p、每通道各偏一格,
    ///  再挑一个中间索引, 可以让插值结果正好落回原值。找到它需要在
    ///  端点 × 索引的联合空间里搜索, 当前编码器走的是"每通道各自取最近"
    ///  的路, 找不到。代价是纯色区域上 1/255 的偏差, 已在报告里记账。)
    const FLAT_BLOCK_ABS_BUDGET: i32 = 1;

    #[test]
    fn solid_blocks_roundtrip_to_within_one_step() {
        // 纯色块是整条编码链路最锐的判据: 端点展开、p 位、索引位序、
        // 打包顺序里任何一处错位, 误差都会从 1 跳到几十。
        let mut rng = Rng(0x1234_5678_9ABC_DEF0);
        for _ in 0..256 {
            let color = [rng.byte(), rng.byte(), rng.byte(), rng.byte()];
            let src = solid_block(color);
            let decoded = decode_block(&encode_block(&src));
            let worst = max_abs_error(&src, &decoded);
            assert!(
                worst <= FLAT_BLOCK_ABS_BUDGET,
                "纯色块 {:?} 的往返误差是 {}, 超过 {} —— 解出来是 {:?}",
                color,
                worst,
                FLAT_BLOCK_ABS_BUDGET,
                decoded[0]
            );
        }
        // 边界值单独再来一遍: 0 和 255 是端点展开最容易出错的地方。
        for color in [
            [0u8, 0, 0, 0],
            [255, 255, 255, 255],
            [0, 255, 0, 255],
            [255, 0, 255, 0],
            [254, 253, 252, 251],
        ] {
            let src = solid_block(color);
            let decoded = decode_block(&encode_block(&src));
            assert!(
                max_abs_error(&src, &decoded) <= FLAT_BLOCK_ABS_BUDGET,
                "纯色块 {:?} 解出来是 {:?}",
                color,
                decoded[0]
            );
        }
        // 四通道最低位一致的颜色则必须 **精确** 无损 —— 此时 p 位没有冲突,
        // 端点能落在原值上。这条把上面那个 "≤1" 钉死在"只因为 p 位冲突",
        // 而不是"编码器就是差这么一点"。
        for color in [[10u8, 20, 30, 40], [11, 21, 31, 41], [0, 128, 64, 200]] {
            let src = solid_block(color);
            let decoded = decode_block(&encode_block(&src));
            assert_eq!(
                max_abs_error(&src, &decoded),
                0,
                "四通道同奇偶的纯色 {:?} 必须无损, 解出来是 {:?}",
                color,
                decoded[0]
            );
        }
    }

    #[test]
    fn two_color_blocks_roundtrip_to_within_one_step() {
        // 块里只有两种颜色时, 两个端点各占一种, 索引二选一。误差仍然只来自
        // p 位冲突 (见 FLAT_BLOCK_ABS_BUDGET)。这条专门守"索引分配"那一段:
        // 索引分错的话必然有像素拿到另一种颜色, 误差会是几十上百。
        let mut rng = Rng(0xDEAD_BEEF_CAFE_1234);
        for _ in 0..128 {
            let a = [rng.byte(), rng.byte(), rng.byte(), 255];
            let b = [rng.byte(), rng.byte(), rng.byte(), 255];
            let src: [[u8; 4]; 16] =
                std::array::from_fn(|_| if (rng.next() & 1) == 0 { a } else { b });
            let decoded = decode_block(&encode_block(&src));
            let worst = max_abs_error(&src, &decoded);
            assert!(
                worst <= FLAT_BLOCK_ABS_BUDGET,
                "双色块 {:?} / {:?} 的往返误差是 {}",
                a,
                b,
                worst
            );
        }
    }

    /// 有结构的随机块的 MSE 上限。
    ///
    /// "有结构" = 16 个像素落在 RGBA 空间的一条随机直线上, 这正是真实贴图
    /// 里绝大多数块的形态 (一个 4×4 邻域通常只是明暗或色相的单向渐变)。
    /// 实测最坏 1.16 / 平均 0.55, 取 4.0 留出三倍余量 (≈ 42 dB)。
    /// `corrupting_the_indices_breaks_the_budget` 负责证明它真的能被压破。
    const STRUCTURED_BLOCK_MSE_BUDGET: f64 = 4.0;

    /// 沿一条随机直线取 16 个点, 这是"有结构的随机块"。
    fn structured_block(rng: &mut Rng) -> [[u8; 4]; 16] {
        let a = [rng.byte(), rng.byte(), rng.byte(), rng.byte()];
        let b = [rng.byte(), rng.byte(), rng.byte(), rng.byte()];
        std::array::from_fn(|t| {
            let f = t as f64 / 15.0;
            std::array::from_fn(|c| (a[c] as f64 * (1.0 - f) + b[c] as f64 * f).round() as u8)
        })
    }

    #[test]
    fn structured_random_blocks_roundtrip_within_budget() {
        let mut rng = Rng(0xABCD_1234_5678_9999);
        let mut worst = 0.0f64;
        for _ in 0..512 {
            let src = structured_block(&mut rng);
            worst = worst.max(mse(&src, &decode_block(&encode_block(&src))));
        }
        assert!(
            worst < STRUCTURED_BLOCK_MSE_BUDGET,
            "有结构的随机块最坏 MSE = {:.3}, 超过上限 {:.1}",
            worst,
            STRUCTURED_BLOCK_MSE_BUDGET
        );
    }

    #[test]
    fn uniform_noise_blocks_have_a_bounded_worst_case() {
        // 均匀随机的 RGBA 块是 BC7 的理论最坏输入: 16 个互不相关的 4 维点
        // 没有任何直线结构, 双子集也救不了。这条不是质量指标, 是"最坏情况
        // 有界"的记录 —— 实测最坏 MSE ≈ 2843 (≈ 13.6 dB)。
        // 真实贴图不长这样; 有结构的块看上面那条。
        let mut rng = Rng(0x0BAD_F00D_1234_5678);
        let mut worst = 0.0f64;
        for _ in 0..512 {
            let src: [[u8; 4]; 16] =
                std::array::from_fn(|_| [rng.byte(), rng.byte(), rng.byte(), rng.byte()]);
            worst = worst.max(mse(&src, &decode_block(&encode_block(&src))));
        }
        assert!(worst < 3600.0, "均匀噪声块最坏 MSE = {:.1}", worst);
    }

    #[test]
    fn smooth_blocks_are_nearly_exact() {
        // 有结构的块 (线性渐变) 才是真实贴图的形态。渐变整个落在一条直线上,
        // mode 6 的 4 位索引有 16 级 —— 16 个像素的渐变应当几乎无损。
        let mut src = [[0u8; 4]; 16];
        for (t, texel) in src.iter_mut().enumerate() {
            let v = (t * 17) as u8; // 0, 17, 34, ... 255
            *texel = [v, 255 - v, 128, 255];
        }
        let decoded = decode_block(&encode_block(&src));
        let worst = max_abs_error(&src, &decoded);
        assert!(worst <= 2, "线性渐变块的最大绝对误差 {} 偏大", worst);
    }

    /// 从码流里读出 mode 号 —— 一元码, 第一个 1 之前有几个 0
    fn mode_of(block: &[u8; 16]) -> u32 {
        let bits = u128::from_le_bytes(*block);
        let mut mode = 0u32;
        while mode < 8 && (bits >> mode) & 1 == 0 {
            mode += 1;
        }
        mode
    }

    #[test]
    fn three_regions_pick_a_three_subset_mode() {
        // 三块区域, 每块沿**互相正交**的一个颜色轴变化。
        //
        // 内容是挑过的。第一版给三块各一种纯色 —— 那个块双子集就能编到
        // 零误差: 把其中两种颜色放进同一个子集当两个端点, 第三种放另一个
        // 子集。三种纯色只要三个调色板项, 而双子集有四个。**判据得让两个
        // 子集装不下才有判别力。**
        //
        // 三条正交的线段就装不下: 任意两块并进一个子集, 那两条线段张成的
        // 是一个平面, 而一个子集只有一条直线。
        let mut src = [[0u8; 4]; 16];
        let mut seen = [0usize; 3];
        for t in 0..16 {
            let s = subset_of_3(0, t);
            let step = (seen[s] % 4) as u8;
            seen[s] += 1;

            let mut px = [20u8, 20, 20, 255];
            px[s] = 70 + step * 60;
            src[t] = px;
        }

        let texels: [[i32; 4]; 16] =
            std::array::from_fn(|t| std::array::from_fn(|c| src[t][c] as i32));

        // 先把"双子集里最好的"和"三子集里最好的"都算出来 —— 判据要的是
        // 后者明显更好, 否则这个块选不出三子集的必要性。
        let mut two_best = encode_with_mode(6, 0, &texels).0;
        for partition in 0..64 {
            for mode in [1usize, 3] {
                two_best = two_best.min(encode_with_mode(mode, partition, &texels).0);
            }
        }

        let mut three_best = f64::INFINITY;
        for partition in 0..64 {
            three_best = three_best.min(encode_with_mode(2, partition, &texels).0);
            if partition < 16 {
                three_best = three_best.min(encode_with_mode(0, partition, &texels).0);
            }
        }

        assert!(
            three_best * 4.0 < two_best,
            "三子集 {three_best:.1} 没有明显好过双子集 {two_best:.1} —— \
             这个块选不出三子集的必要性, 换个内容"
        );

        let mode = mode_of(&encode_block(&src));
        assert!(
            mode == 0 || mode == 2,
            "三条正交线段应当选三子集 mode, 实际选了 mode {mode}"
        );
    }

    #[test]
    fn independent_alpha_picks_a_rotation_mode() {
        // 颜色是 8 级灰阶, alpha 是与它**无关**的奇偶阶跃。
        //
        // 内容同样是挑过的。第一版用了 16 级灰阶 —— 那正好是 mode 6 的
        // 强项 (4 位索引 = 16 级), 而 mode 5 只有 2 位颜色索引, 于是"两套
        // 索引"反而更差。**判据的内容要让被验的东西占优, 否则它验的是别的
        // 东西。**
        //
        // 8 级正好是 mode 4 第二套索引的宽度 (3 位), 而奇偶阶跃正好是第一
        // 套索引的宽度 (2 位)。一套索引的 mode 做不到: 它得让 alpha 跟着
        // 灰阶单调走, 而 alpha 是跳的。
        // 灰阶取的是 BC7 两位索引的**四个插值点**: 端点 0 与 255 之间,
        // 权重表 [0, 21, 43, 64] 插出来正好是 0 / 84 / 171 / 255。用这四个
        // 值意味着两位索引能把颜色编到**零误差**, 于是判据不必挑阈值 ——
        // 两套索引的那一路是精确的, 一套索引的那一路不可能精确。
        //
        // alpha 走棋盘格, 不是列奇偶: BC7 的双子集分区表里有能把列分开的
        // 图案, 那时按列切一刀就能把这个块编到零误差, 于是判据验的是分区
        // 而不是"两套索引"。棋盘格切不开。
        const LEVELS: [u8; 4] = [0, 84, 171, 255];

        let mut src = [[0u8; 4]; 16];
        for t in 0..16 {
            let gray = LEVELS[t % 4];
            let checker = (t / 4 + t % 4) % 2;
            src[t] = [gray, gray, gray, if checker == 0 { 0 } else { 255 }];
        }

        let texels: [[i32; 4]; 16] =
            std::array::from_fn(|t| std::array::from_fn(|c| src[t][c] as i32));

        let mut single_best = encode_with_mode(6, 0, &texels).0;
        for partition in 0..64 {
            single_best = single_best.min(encode_with_mode(7, partition, &texels).0);
        }

        let mut dual_best = f64::INFINITY;
        for rotation in 0..4u32 {
            dual_best = dual_best.min(encode_with_dual_index(5, rotation, 0, &texels).0);
            for index_mode in 0..2u32 {
                dual_best =
                    dual_best.min(encode_with_dual_index(4, rotation, index_mode, &texels).0);
            }
        }

        assert!(
            dual_best * 4.0 < single_best,
            "两套索引 {dual_best:.1} 没有明显好过一套 {single_best:.1}"
        );

        let mode = mode_of(&encode_block(&src));
        assert!(
            mode == 4 || mode == 5,
            "颜色与 alpha 各走各的应当选 mode 4/5, 实际选了 mode {mode}"
        );
    }

    #[test]
    fn rotation_moves_the_independent_channel_into_its_own_index_set() {
        // 与上一条同一个形态, 只是"走势不同的那个通道"是**蓝色**而不是
        // alpha (alpha 恒为不透明)。旋转 3 把蓝色换到 alpha 的位置。
        //
        // 这一条单独立着, 是因为旋转位整个写死成 0 时上一条照样通过 ——
        // 那时 alpha 本来就在它该在的位置, 旋转没被用到。
        const LEVELS: [u8; 4] = [0, 84, 171, 255];

        let mut src = [[0u8; 4]; 16];
        for t in 0..16 {
            let ramp = LEVELS[t % 4];
            let checker = (t / 4 + t % 4) % 2;
            src[t] = [ramp, ramp, if checker == 0 { 0 } else { 255 }, 255];
        }

        let texels: [[i32; 4]; 16] =
            std::array::from_fn(|t| std::array::from_fn(|c| src[t][c] as i32));

        let mut no_rotation = f64::INFINITY;
        let mut with_rotation = f64::INFINITY;

        for index_mode in 0..2u32 {
            no_rotation = no_rotation.min(encode_with_dual_index(4, 0, index_mode, &texels).0);
            with_rotation =
                with_rotation.min(encode_with_dual_index(4, 3, index_mode, &texels).0);
        }
        no_rotation = no_rotation.min(encode_with_dual_index(5, 0, 0, &texels).0);
        with_rotation = with_rotation.min(encode_with_dual_index(5, 3, 0, &texels).0);

        assert!(
            with_rotation * 4.0 < no_rotation,
            "把蓝色转进独立索引 ({with_rotation:.1}) 没有明显好过不转 \
             ({no_rotation:.1}) —— 旋转位没起作用"
        );

        // 而且整条编码路径要真的挑中它, 并且真的转了。
        //
        // 上面那两句是直接调 encode_with_dual_index 算的, 它们证明"转了更
        // 好", 但证明不了 encode_block **会去试**。把候选循环从四种旋转砍成
        // 一种, 上面照样通过 —— 变异验证就是这么发现的。
        let encoded = encode_block(&src);
        let mode = mode_of(&encoded);

        assert!(
            mode == 4 || mode == 5,
            "蓝色独立走势应当选 mode 4/5, 实际选了 mode {mode}"
        );

        // 旋转位紧跟在一元 mode 码之后 (mode 4/5 没有分区号)
        let bits = u128::from_le_bytes(encoded);
        let rotation = ((bits >> (mode + 1)) & 0b11) as u32;

        assert_eq!(
            rotation, 3,
            "走势独立的是蓝色, 应当转到 alpha 的位置 (旋转 3), 实际是 旋转 {rotation}"
        );
    }

    #[test]
    fn every_encoder_mode_round_trips_through_the_decoder() {
        // 编码器产出的每一种 mode 都要能被解码器读回来, 而且读回来的
        // 结果必须与编码时算的误差一致。
        //
        // 这一条防的是"打包顺序与解码顺序对不上": 那种错仍然是 128 bit,
        // mode 号也对, 只有内容是乱的 —— 而误差是在打包**之前**算的, 所以
        // 光看编码器自己的误差发现不了。
        let texels: [[i32; 4]; 16] = std::array::from_fn(|t| {
            [
                (t * 13 % 256) as i32,
                (t * 29 % 256) as i32,
                (t * 47 % 256) as i32,
                (t * 17 % 256) as i32,
            ]
        });

        let mut checked = 0;

        for (mode, partition) in [(0usize, 3usize), (1, 5), (2, 7), (3, 11), (6, 0), (7, 13)] {
            let (error, block) = encode_with_mode(mode, partition, &texels);
            assert_eq!(mode_of(&block), mode as u32, "mode {mode} 的一元码写错了");

            let decoded = decode_block(&block);
            let mut actual = 0.0f64;
            for t in 0..16 {
                let channels = if MODES[mode].alpha_bits > 0 { 4 } else { 3 };
                for c in 0..channels {
                    let d = texels[t][c] as f64 - decoded[t][c] as f64;
                    actual += d * d;
                }
                if MODES[mode].alpha_bits == 0 {
                    let d = texels[t][3] as f64 - 255.0;
                    actual += d * d;
                }
            }
            assert!(
                (actual - error).abs() < 1.0e-6,
                "mode {mode}: 解码后的误差 {actual:.3} 与编码时算的 {error:.3} 不符"
            );
            checked += 1;
        }

        for rotation in 0..4u32 {
            for (mode, index_mode) in [(5usize, 0u32), (4, 0), (4, 1)] {
                let (error, block) = encode_with_dual_index(mode, rotation, index_mode, &texels);
                assert_eq!(mode_of(&block), mode as u32);

                let decoded = decode_block(&block);
                let mut actual = 0.0f64;
                for t in 0..16 {
                    for c in 0..4 {
                        let d = texels[t][c] as f64 - decoded[t][c] as f64;
                        actual += d * d;
                    }
                }
                assert!(
                    (actual - error).abs() < 1.0e-6,
                    "mode {mode} 旋转 {rotation} 索引集 {index_mode}: \
                     解码后的误差 {actual:.3} 与编码时算的 {error:.3} 不符"
                );
                checked += 1;
            }
        }

        assert_eq!(checked, 6 + 12, "有 mode 没被这条判据覆盖到");
    }

    #[test]
    fn hard_color_boundary_uses_two_subsets() {
        // 左半边一种颜色、右半边另一种, 各自还带梯度 —— 单子集必须让一条
        // 直线同时穿过两团点, 中间的插值点谁也不是。双子集把两团分开。
        // 这条断言"块的误差足够小", 间接证明 mode 1/3 真的被选中了:
        // 只用 mode 6 的话这个块的误差会大一个量级。
        let mut src = [[0u8; 4]; 16];
        for y in 0..4 {
            for x in 0..4 {
                src[y * 4 + x] = if x < 2 {
                    [200 + (y * 8) as u8, 30, 40, 255]
                } else {
                    [20, 60, 220 - (y * 8) as u8, 255]
                };
            }
        }
        let decoded = decode_block(&encode_block(&src));
        let e = mse(&src, &decoded);
        assert!(e < 4.0, "色彩边界块的 MSE = {:.2}, 双子集没有起作用?", e);

        // 对照: 强行只用 mode 6 编同一个块, 误差必须明显更大 ——
        // 否则上面那条断言对"双子集有没有被选"没有判别力。
        let texels: [[i32; 4]; 16] =
            std::array::from_fn(|t| std::array::from_fn(|c| src[t][c] as i32));
        let (_, mode6_only) = encode_with_mode(6, 0, &texels);
        let e6 = mse(&src, &decode_block(&mode6_only));
        assert!(
            e6 > e * 4.0,
            "单子集 MSE {:.2} 与双子集 {:.2} 没有拉开差距, 这个用例不成立",
            e6,
            e
        );
    }

    #[test]
    fn alpha_step_with_varying_color() {
        // 抠图边缘: alpha 阶跃, 颜色同时在变。mode 6 只有一套索引, 颜色和
        // alpha 必须共用插值位置; mode 7 用分区把阶跃两侧分开。
        let mut src = [[0u8; 4]; 16];
        for y in 0..4 {
            for x in 0..4 {
                src[y * 4 + x] = if x < 2 {
                    [180, 140, 90, 255]
                } else {
                    [60, 200, 210, 0]
                };
            }
        }
        let decoded = decode_block(&encode_block(&src));
        assert!(
            max_abs_error(&src, &decoded) <= FLAT_BLOCK_ABS_BUDGET,
            "alpha 阶跃块只有两种 RGBA 取值, 误差应当只剩 p 位冲突那 1 格"
        );

        // 对照: 把 alpha 从阶跃改成与颜色同向变化 (相关), 单子集就够了。
        // 这条不是必需的, 但它证明上面那条测的确实是 "alpha 与颜色不相关"
        // 这个形态, 而不是随便一个两值块。
        let mut correlated = [[0u8; 4]; 16];
        for (t, texel) in correlated.iter_mut().enumerate() {
            let v = (t * 17) as u8;
            *texel = [v, v, v, v];
        }
        let d2 = decode_block(&encode_block(&correlated));
        assert!(max_abs_error(&correlated, &d2) <= 2);
    }

    // ── 证明上面的往返判据真的能失败 ──────────────────────────────────

    #[test]
    fn corrupting_the_indices_breaks_the_budget() {
        // 从正确编码出发, 只把每个块的索引位全清零 —— 这正是"索引分配写错"
        // 这类 bug 的形态: mode、分区、端点全部合法, 解码器照单全收,
        // 只是每个像素都取了端点 0。
        // 如果这样都还能通过 STRUCTURED_BLOCK_MSE_BUDGET, 那条上限就是摆设。
        let mut rng = Rng(0xABCD_1234_5678_9999);
        let mut worst_good = 0.0f64;
        let mut worst_bad = f64::INFINITY;
        for _ in 0..512 {
            let src = structured_block(&mut rng);
            let good = encode_block(&src);
            worst_good = worst_good.max(mse(&src, &decode_block(&good)));

            // mode 6 的索引从第 65 位开始 (7 位 mode + 56 位端点 + 2 位 p)。
            // 清掉第 65 位往上, 对双子集 mode 同样落在索引区里。
            let bits = u128::from_le_bytes(good) & ((1u128 << 65) - 1);
            worst_bad = worst_bad.min(mse(&src, &decode_block(&bits.to_le_bytes())));
        }
        assert!(
            worst_bad > STRUCTURED_BLOCK_MSE_BUDGET,
            "把索引清零之后 **最好** 的一块 MSE 也才 {:.1}, 没有越过上限 {:.1} —— \
             说明这个上限对真正的编码错误没有判别力",
            worst_bad,
            STRUCTURED_BLOCK_MSE_BUDGET
        );
        assert!(
            worst_bad > worst_good * 100.0,
            "正确编码最坏 {:.3} 与索引清零后最好 {:.1} 没有拉开量级差距",
            worst_good,
            worst_bad
        );
    }

    #[test]
    fn swapped_endpoint_order_is_detected() {
        // 端点写成"端点优先"而不是"通道优先"是最容易犯的打包错误。
        // 这里手工造一个这样的块 (把 mode 6 的 R/G/B/A 段整体重排),
        // 解码结果必须与正确的明显不同 —— 否则解码器根本没在读那些位。
        let mut src = [[0u8; 4]; 16];
        for (t, texel) in src.iter_mut().enumerate() {
            *texel = [(t * 16) as u8, 200, 40, 255];
        }
        let good = encode_block(&src);
        let good_px = decode_block(&good);

        let mut bits = u128::from_le_bytes(good);
        // mode 6: 7 位 mode 之后是 R0 R1 G0 G1 B0 B1 A0 A1, 各 7 位。
        // 把 R1 和 G0 对调 —— 这正是排列写错时会发生的事。
        let base = 7u32;
        let mask = (1u128 << 7) - 1;
        let r1 = (bits >> (base + 7)) & mask;
        let g0 = (bits >> (base + 14)) & mask;
        bits &= !(mask << (base + 7));
        bits &= !(mask << (base + 14));
        bits |= g0 << (base + 7);
        bits |= r1 << (base + 14);
        let bad_px = decode_block(&bits.to_le_bytes());

        assert_ne!(
            good_px, bad_px,
            "打乱端点排列之后解码结果没变, 说明解码器没在读这些位"
        );
        assert!(
            mse(&src, &bad_px) > mse(&src, &good_px) * 4.0,
            "端点排列错乱之后误差没有明显变大"
        );
    }

    #[test]
    fn reserved_mode_decodes_to_zero() {
        // 前 8 位全 0 = 保留模式。解码器必须给出确定的结果而不是 panic
        // 或者读越界 —— 它要能吃下别的工具产出的任意字节。
        let block = [0u8; 16];
        assert_eq!(decode_block(&block), [[0u8; 4]; 16]);
    }

    #[test]
    fn decoder_consumes_exactly_128_bits_for_every_mode() {
        // 拿随机字节喂解码器: 只要 mode 位合法, 解码就必须正好用满 128 位
        // (debug_assert 守着), 且不 panic。位宽表抄错时这条会炸。
        let mut rng = Rng(0xFEED_FACE_0000_1111);
        for mode in 0..8u32 {
            for _ in 0..64 {
                let mut block = [0u8; 16];
                for b in block.iter_mut() {
                    *b = rng.byte();
                }
                // 把开头改成指定 mode 的一元码
                let mut bits = u128::from_le_bytes(block);
                bits &= !((1u128 << (mode + 1)) - 1);
                bits |= 1u128 << mode;
                let _ = decode_block(&bits.to_le_bytes());
            }
        }
    }

    // ── 整图接口 ──────────────────────────────────────────────────────

    /// 造一张带任意 stride 的测试图, 内容是一个二维斜坡 (相邻块内容互不相同)。
    fn strided_test_image(w: u32, h: u32, stride: u32) -> Vec<u8> {
        assert!(stride >= w * 4);
        let mut pixels = vec![0u8; (stride * h) as usize];
        for y in 0..h {
            for x in 0..w {
                let o = (y * stride + x * 4) as usize;
                pixels[o] = (x.wrapping_mul(23) % 256) as u8;
                pixels[o + 1] = (y.wrapping_mul(37) % 256) as u8;
                pixels[o + 2] = ((x + y) * 11 % 256) as u8;
                pixels[o + 3] = 255;
            }
        }
        pixels
    }

    /// 按 (bx, by) 手工把 4×4 个 texel 抓出来 —— 与 compress_blocks 里的
    /// 索引算法各写一遍, 用来交叉验证。
    fn gather(pixels: &[u8], stride: u32, bx: usize, by: usize) -> [[u8; 4]; 16] {
        let mut texels = [[0u8; 4]; 16];
        for y in 0..4 {
            for x in 0..4 {
                let o = (by * 4 + y) * stride as usize + (bx * 4 + x) * 4;
                texels[y * 4 + x].copy_from_slice(&pixels[o..o + 4]);
            }
        }
        texels
    }

    #[test]
    fn whole_image_path_matches_per_block_calls() {
        // 整图接口只是块接口的循环 —— 但"块写到了哪个偏移"、"从哪里取源
        // 像素"这两件事只有整图接口知道。逐块比对是精确判据, 不需要阈值:
        // 块顺序反了、行列写反了、偏移算错了, 全都会立刻不相等。
        for (w, h, stride) in [(16u32, 8u32, 64u32), (8, 4, 32), (4, 4, 16), (12, 12, 48)] {
            let pixels = strided_test_image(w, h, stride);
            let (bx_count, by_count) = ((w / 4) as usize, (h / 4) as usize);

            let mut blocks = vec![0u8; bx_count * by_count * 16];
            compress_blocks(&pixels, &mut blocks, w, h, stride);

            for by in 0..by_count {
                for bx in 0..bx_count {
                    let expect = encode_block(&gather(&pixels, stride, bx, by));
                    let got = &blocks[(by * bx_count + bx) * 16..][..16];
                    assert_eq!(
                        got,
                        expect.as_slice(),
                        "{}×{} (stride {}) 的块 ({},{}) 与逐块编码结果不同",
                        w,
                        h,
                        stride,
                        bx,
                        by
                    );
                }
            }

            // 解码侧同理
            let mut decoded = vec![0u8; (w * h * 4) as usize];
            decompress_blocks(w, h, &blocks, &mut decoded);
            for by in 0..by_count {
                for bx in 0..bx_count {
                    let mut raw = [0u8; 16];
                    raw.copy_from_slice(&blocks[(by * bx_count + bx) * 16..][..16]);
                    let expect = decode_block(&raw);
                    for y in 0..4 {
                        for x in 0..4 {
                            let o = ((by * 4 + y) * w as usize + bx * 4 + x) * 4;
                            assert_eq!(
                                &decoded[o..o + 4],
                                &expect[y * 4 + x][..],
                                "解码到 ({},{}) 的像素放错了位置",
                                bx * 4 + x,
                                by * 4 + y
                            );
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn stride_actually_participates_in_addressing() {
        // 上一条用同一个 stride 走两遍, 如果 compress_blocks 干脆忽略 stride
        // 而用 width*4, 两边会一起错、一起通过。这条把 stride 改掉:
        // 同样的像素内容, 换一个 stride 打包, 编出来的块必须不同。
        let (w, h) = (8u32, 4u32);
        let tight = strided_test_image(w, h, w * 4);
        let loose = strided_test_image(w, h, 64);

        let mut a = vec![0u8; 2 * 16];
        let mut b = vec![0u8; 2 * 16];
        compress_blocks(&tight, &mut a, w, h, w * 4);
        compress_blocks(&loose, &mut b, w, h, 64);
        assert_eq!(a, b, "同样的像素内容换 stride 之后结果应当一致");

        // 反过来: 拿宽 stride 的缓冲区按紧凑 stride 去读, 取到的是错位的
        // 像素, 结果必须不同 —— 否则说明 stride 根本没参与寻址。
        let mut c = vec![0u8; 2 * 16];
        compress_blocks(&loose, &mut c, w, h, w * 4);
        assert_ne!(b, c, "stride 没有参与寻址");
    }

    #[test]
    fn whole_image_roundtrip_is_high_quality_on_smooth_content() {
        // 一维渐变: 每个 4×4 块内部都落在一条直线上, BC7 应当几乎无损。
        let (w, h) = (64u32, 64u32);
        let mut pixels = Vec::with_capacity((w * h * 4) as usize);
        for y in 0..h {
            for x in 0..w {
                let v = ((x + y) * 2) as u8;
                pixels.extend_from_slice(&[v, 255 - v, 128, 255]);
            }
        }
        let mut blocks = vec![0u8; (w / 4 * (h / 4)) as usize * 16];
        compress_blocks(&pixels, &mut blocks, w, h, w * 4);
        let mut decoded = vec![0u8; pixels.len()];
        decompress_blocks(w, h, &blocks, &mut decoded);

        let worst = pixels
            .iter()
            .zip(decoded.iter())
            .map(|(a, b)| (*a as i32 - *b as i32).abs())
            .max()
            .unwrap();
        assert!(worst <= 2, "平滑渐变整图往返最大误差 {} 偏大", worst);
    }
}


/// 真实贴图上的测量工具 —— 不是判据, 是产出数字的那把尺子。
///
/// 判据能证明"每种 mode 在为它设计的内容上会被选中", 但证明不了"在真实
/// 贴图上值不值"。后者只能量。这个测量跑一张图上的每个块两遍 —— 一遍用
/// 全部八种 mode, 一遍只用原来的四种 —— 报出两者的 MSE / PSNR 与 mode
/// 直方图。
///
/// 它带 #[ignore], 不进常规测试 (要读磁盘上的贴图, 而且一张图要跑十几秒)。
/// 跑法:
///
/// ```text
/// cd Programs
/// LAT_MEASURE_IMAGE=<绝对路径.png> ///     cargo test --release -p lat measure_real_texture -- --ignored --nocapture
/// ```
#[cfg(test)]
mod measure {
    use super::*;

    #[test]
    #[ignore]
    fn measure_real_texture() {
        let path = std::env::var("LAT_MEASURE_IMAGE").unwrap();
        let img = crate::image_io::load(std::path::Path::new(&path)).unwrap().image;
        let (w, h) = (img.width, img.height);

        let mut full = 0.0f64;
        let mut restricted = 0.0f64;
        let mut blocks = 0u64;
        let mut picked = [0u64; 8];

        for by in (0..h / 4 * 4).step_by(4) {
            for bx in (0..w / 4 * 4).step_by(4) {
                let mut src = [[0u8; 4]; 16];
                for y in 0..4 {
                    for x in 0..4 {
                        let o = (((by + y) * w + bx + x) * 4) as usize;
                        src[(y * 4 + x) as usize] =
                            [img.pixels[o], img.pixels[o + 1], img.pixels[o + 2], img.pixels[o + 3]];
                    }
                }

                let texels: [[i32; 4]; 16] =
                    std::array::from_fn(|t| std::array::from_fn(|c| src[t][c] as i32));

                let encoded = encode_block(&src);
                let mut mode = 0u32;
                let bits = u128::from_le_bytes(encoded);
                while mode < 8 && (bits >> mode) & 1 == 0 {
                    mode += 1;
                }
                if mode < 8 {
                    picked[mode as usize] += 1;
                }

                let dec = decode_block(&encoded);
                for t in 0..16 {
                    for c in 0..4 {
                        let d = src[t][c] as f64 - dec[t][c] as f64;
                        full += d * d;
                    }
                }

                let opaque = texels.iter().all(|p| p[3] == 255);
                let mut best = encode_with_mode(6, 0, &texels).1;
                let mut best_err = encode_with_mode(6, 0, &texels).0;
                let modes: &[usize] = if opaque { &[1, 3] } else { &[7] };
                for partition in 0..64 {
                    for &m in modes {
                        let (e, b) = encode_with_mode(m, partition, &texels);
                        if e < best_err {
                            best_err = e;
                            best = b;
                        }
                    }
                }
                let dec = decode_block(&best);
                for t in 0..16 {
                    for c in 0..4 {
                        let d = src[t][c] as f64 - dec[t][c] as f64;
                        restricted += d * d;
                    }
                }

                blocks += 1;
            }
        }

        let n = (blocks * 16 * 4) as f64;
        let mse_full = full / n;
        let mse_restricted = restricted / n;
        let psnr = |m: f64| 10.0 * (255.0f64 * 255.0 / m).log10();

        println!("块数 {blocks}");
        println!("八种 mode : MSE {mse_full:.4}  PSNR {:.2} dB", psnr(mse_full));
        println!("原四种    : MSE {mse_restricted:.4}  PSNR {:.2} dB", psnr(mse_restricted));
        println!("mode 直方图 {picked:?}");
    }
}
