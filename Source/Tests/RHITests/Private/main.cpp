/*******************************************************************************
 * 文件: main.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   RHITests 可执行文件入口 — 转发到测试运行器
 *
 * 设计哲学:
 *   入口零逻辑 — 用例通过静态注册自动汇入, 新增测试文件无需改动本文件
 *
 * 依赖关系:
 *   内部: RHITests/RHITestsMinimal.h
 *
 ******************************************************************************/

#include "RHITests/RHITestsMinimal.h"

/// 控制台入口 — 退出码 0 表示全部通过
int main(int argc, char** argv)
{
    return static_cast<int>(::Limx::FTestRunner::Main(argc, argv));
}
