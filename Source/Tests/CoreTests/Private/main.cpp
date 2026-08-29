/*******************************************************************************
 * 文件: main.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   CoreTests 可执行文件入口 — 转发到测试运行器
 *
 * 设计哲学:
 *   入口零逻辑 — 用例通过静态注册自动汇入, 入口无需感知任何具体测试,
 *   新增测试文件不需要修改本文件。
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"

/// 控制台入口 — 退出码 0 表示全部通过
int main(int argc, char** argv)
{
    return static_cast<int>(::Limx::FTestRunner::Main(argc, argv));
}
