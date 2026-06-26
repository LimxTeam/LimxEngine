#version 450

// 深度预 Pass 片段着色器
// 此着色器无任何颜色输出 — 仅通过光栅化阶段将顶点插值深度写入深度缓冲区。
// 配合深度预 Pass 的 depth-only RenderPass 使用 (无颜色附件)。
// 后续 FForwardPass 使用 DepthCompareOp::Equal 跳过被遮挡的片段。
void main()
{
    // 深度值由 GPU 光栅化阶段从 gl_Position.z/w 自动写入深度缓冲区
    // 此处无需任何操作
}
