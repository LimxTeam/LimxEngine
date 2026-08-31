/*******************************************************************************
 * 文件: cli.rs
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LAT 命令行接口定义。
 *
 * 支持的命令:
 *   - bake:      烘焙单张贴图
 *   - bake-all:  递归批量烘焙一个目录
 *   - inspect:   打印 DDS 的格式 / 尺寸 / mip 层数 / 各层字节数
 *
 ******************************************************************************/

use std::path::PathBuf;

use clap::{Args, Parser, Subcommand};

use crate::format::{ColorSpace, FormatOption};

#[derive(Parser)]
#[command(name = "lat")]
#[command(author = "LimxTeam")]
#[command(version = "0.1.0")]
#[command(about = "Limx Asset Tool - 纹理离线烘焙 (BC 压缩 + mip 链 + DDS 输出)")]
#[command(long_about = r#"
LAT (Limx Asset Tool) 把源图离线烘焙成 GPU 能直接采样的块压缩纹理。

为什么要离线烘焙:
  运行时导入一张 1024×1024 的 JPEG 要解码成 4 MiB 的 RGBA8 再上传,
  Sponza 场景 69 张贴图光解码就占了导入耗时的 79%。烘成 BC 之后,
  运行时只剩一次 memcpy, 显存也降到原来的 1/4 (BC1) 或 1/2 (BC3)。

  mip 链也必须在这里生成: 引擎运行时的 mip 是用 vkCmdBlitImage 放缩的,
  而 BC 格式不支持 blit —— 压缩纹理的 mip 只能离线做。

这一轮支持的格式:
  BC1 (RGB, 1/8)   BC3 (RGBA, 1/4)   BC4 (R, 1/8)   BC5 (RG, 1/4)
  BC7 是下一步。

色彩空间必须显式指定, 工具不从文件名猜:
  --color-space srgb     albedo / baseColor / emissive
  --color-space linear   normal / metallicRoughness / occlusion / height

示例:
  lat bake wall_albedo.png -o wall_albedo.dds --color-space srgb
  lat bake wall_normal.png -o wall_normal.dds --color-space linear --format bc5
  lat bake-all -s Content/Sponza -o Baked/Sponza --color-space srgb
  lat inspect Baked/Sponza/wall_albedo.dds
"#)]
pub struct Cli {
    /// 全局详细输出
    #[arg(short, long, global = true)]
    pub verbose: bool,

    #[command(subcommand)]
    pub command: Commands,
}

/// bake 与 bake-all 共用的烘焙选项。
#[derive(Args, Clone)]
pub struct BakeArgs {
    /// 源图的色彩空间 —— 必填, 没有默认值。
    ///
    /// 这个开关同时决定 DDS 里写不写 sRGB 格式号, 以及 mip 降采样是否
    /// 先转到线性光。猜错的后果是整张贴图偏亮或偏暗, 而且不会有任何报错,
    /// 所以宁可让调用方每次都写一遍。
    #[arg(long, value_enum)]
    pub color_space: ColorSpace,

    /// 输出格式。auto 按通道数与 alpha 有效性决定。
    ///
    /// 3 通道的切线空间法线图必须显式写 --format bc5: 从像素上分不出
    /// "RGB 法线" 和 "RGB 颜色", auto 只能保守地给 BC1。
    #[arg(long, value_enum, default_value = "auto")]
    pub format: FormatOption,

    /// 不生成 mip 链, 只写第 0 层 (UI / 图集之类确实不需要 mip 的场景)
    #[arg(long)]
    pub no_mipmaps: bool,

    /// 把第 0 层解回来与源图比对, 报告 PSNR。
    ///
    /// 解码走的是 block_compression 自带的、独立于编码路径的实现,
    /// 所以它能发现"压出来的图看着像但其实错了"这一类问题。
    #[arg(long)]
    pub verify: bool,

    /// PSNR 下限 (dB), 低于它这张图判为失败。隐含开启 --verify。
    ///
    /// 参考值: BC1 在照片类贴图上通常 30~40 dB, 低于 25 说明这张图
    /// 不适合 BC1 (常见于强色阶、纯色块、或者被当成颜色烘的法线图)。
    #[arg(long, value_name = "DB")]
    pub min_psnr: Option<f64>,

    /// 覆盖已存在的输出文件
    #[arg(long)]
    pub overwrite: bool,
}

#[derive(Subcommand)]
pub enum Commands {
    /// 烘焙单张贴图
    #[command(visible_alias = "b")]
    Bake {
        /// 源图路径 (png / jpg / jpeg)
        input: PathBuf,

        /// 输出的 .dds 路径
        #[arg(short, long)]
        output: PathBuf,

        #[command(flatten)]
        bake_args: BakeArgs,
    },

    /// 递归批量烘焙一个目录
    #[command(visible_alias = "ba")]
    BakeAll {
        /// 源目录
        #[arg(short, long)]
        source_dir: PathBuf,

        /// 输出目录 (保留相对于源目录的子目录结构)
        #[arg(short, long)]
        output_dir: PathBuf,

        /// 并行任务数 (0 = 硬件线程数)
        #[arg(short = 'j', long, default_value = "0")]
        jobs: usize,

        /// 只扫描一层, 不递归
        #[arg(long)]
        no_recursive: bool,

        /// 源图扩展名过滤 (逗号分隔)
        #[arg(long, default_value = "png,jpg,jpeg")]
        extensions: String,

        #[command(flatten)]
        bake_args: BakeArgs,
    },

    /// 打印 DDS 的格式、尺寸、mip 层数与各层字节数
    #[command(visible_alias = "i")]
    Inspect {
        /// 要检查的 .dds 文件
        input: PathBuf,
    },
}
