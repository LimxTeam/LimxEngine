/*******************************************************************************
 * 文件: main.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   AssetTests 可执行文件入口 — 转发到测试运行器
 *
 * 依赖关系:
 *   内部: AssetTests/AssetTestsMinimal.h
 *
 ******************************************************************************/

#include "AssetTests/AssetTestsMinimal.h"

/// 控制台入口 — 退出码 0 表示全部通过
int main(int argc, char** argv)
{
    return static_cast<int>(::Limx::FTestRunner::Main(argc, argv));
}
