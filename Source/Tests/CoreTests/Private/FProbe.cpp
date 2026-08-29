/*******************************************************************************
 * 文件: FProbe.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FProbe 静态计数成员的定义
 *
 * 设计哲学:
 *   计数集中定义 — 静态成员需要唯一定义点, 放在独立 .cpp 保证所有
 *   测试翻译单元共享同一份计数
 *
 * 依赖关系:
 *   内部: CoreTests/FProbe.h
 *
 ******************************************************************************/

#include "CoreTests/FProbe.h"

namespace Limx
{

Int32 FProbe::s_DefaultConstructCount = 0;
Int32 FProbe::s_ValueConstructCount   = 0;
Int32 FProbe::s_CopyConstructCount    = 0;
Int32 FProbe::s_MoveConstructCount    = 0;
Int32 FProbe::s_CopyAssignCount       = 0;
Int32 FProbe::s_MoveAssignCount       = 0;
Int32 FProbe::s_DestructCount         = 0;

} // namespace Limx
