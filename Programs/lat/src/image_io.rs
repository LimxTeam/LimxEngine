/*******************************************************************************
 * 文件: image_io.rs
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   源图读取 —— 解码成统一的 RGBA8, 并量出通道特征供格式策略使用。
 *
 * 关于"量出来"而不是"猜出来":
 *   这里只报告两件能从像素数据本身确定的事实: 声明的通道数, 以及
 *   alpha 通道是否携带信息。语义 (albedo / normal / roughness) 一律
 *   由调用方在命令行上给出。文件名不参与任何决策。
 *
 ******************************************************************************/

use std::path::Path;

use anyhow::{Context, Result};

use crate::format::SourceTraits;
use crate::mip::Rgba8Image;

/// 支持的源图扩展名。
///
/// 与 Cargo.toml 里 image 的 feature 列表严格对应 —— 多列一个扩展名
/// 会让 bake-all 把文件收进来然后在解码时才失败, 那时错误信息指向的是
/// "格式不支持" 而不是 "这个工具没开这个 feature"。
pub const SUPPORTED_EXTENSIONS: &[&str] = &["png", "jpg", "jpeg"];

pub struct LoadedImage {
    pub image: Rgba8Image,
    pub traits: SourceTraits,
    /// 实际探测到的容器格式, 供日志使用
    pub detected_format: String,
}

/// 读取并解码一张源图。
pub fn load(path: &Path) -> Result<LoadedImage> {
    // with_guessed_format 会真的去嗅探文件头, 而不是只信扩展名。
    // Sponza 这类导出资产里 ".jpg" 其实是 PNG 的情况并不罕见,
    // 只信扩展名会得到一条指向解码器内部的错误, 完全看不出真正原因。
    let reader = image::ImageReader::open(path)
        .with_context(|| format!("无法打开源图 {}", path.display()))?
        .with_guessed_format()
        .with_context(|| format!("无法识别 {} 的图像格式", path.display()))?;

    let detected_format = reader
        .format()
        .map(|f| format!("{:?}", f))
        .unwrap_or_else(|| "未知".to_string());

    let decoded = reader
        .decode()
        .with_context(|| format!("解码 {} 失败 (探测到的格式: {})", path.display(), detected_format))?;

    let color = decoded.color();
    let channels = color.channel_count();
    let width = decoded.width();
    let height = decoded.height();

    anyhow::ensure!(
        width > 0 && height > 0,
        "{} 的尺寸是 {}×{}, 无法烘焙",
        path.display(),
        width,
        height
    );

    let rgba = decoded.to_rgba8();
    let pixels = rgba.into_raw();

    // alpha 只有在源图真的有 alpha 通道、且其中存在非 255 的值时才算"有效"。
    // 大量导出管线会无条件把 RGB 图升成 RGBA, 若照单全收就会把整片
    // albedo 都推到 BC3, 显存直接翻倍而画面毫无区别。
    let alpha_significant = color.has_alpha() && pixels.chunks_exact(4).any(|px| px[3] != 255);

    Ok(LoadedImage {
        image: Rgba8Image::new(width, height, pixels),
        traits: SourceTraits {
            channels,
            alpha_significant,
        },
        detected_format,
    })
}

/// 判断扩展名是否是本工具能读的源图。
pub fn is_supported_source(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| {
            let lower = e.to_ascii_lowercase();
            SUPPORTED_EXTENSIONS.contains(&lower.as_str())
        })
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn extension_filter_is_case_insensitive() {
        assert!(is_supported_source(&PathBuf::from("a.PNG")));
        assert!(is_supported_source(&PathBuf::from("a.Jpg")));
        assert!(is_supported_source(&PathBuf::from("a.jpeg")));
        // 下面这些必须被排除: dds 是我们的输出, tga/exr 没有开 feature。
        assert!(!is_supported_source(&PathBuf::from("a.dds")));
        assert!(!is_supported_source(&PathBuf::from("a.tga")));
        assert!(!is_supported_source(&PathBuf::from("a.exr")));
        assert!(!is_supported_source(&PathBuf::from("noext")));
    }

    #[test]
    fn missing_file_reports_the_path() {
        let msg = match load(&PathBuf::from("这个文件不存在.png")) {
            Ok(_) => panic!("不存在的文件必须报错"),
            Err(e) => format!("{:#}", e),
        };
        assert!(
            msg.contains("这个文件不存在.png"),
            "错误信息必须带上路径, 实际: {}",
            msg
        );
    }
}
