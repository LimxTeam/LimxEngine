/*******************************************************************************
 * 文件: FGrid2D.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   2D 网格 — 均匀空间划分的二维网格
 *   将二维空间划分为等大小的单元格，提供坐标到网格索引的映射
 *   用于碰撞检测宽阶段、粒子空间索引、2D 地图分块等场景
 *
 * 设计哲学:
 *   扁平存储 — 2D 索引映射到 1D 连续数组
 *   模板化单元格 — 单元格可存储任意类型
 *   坐标映射 — 世界坐标到网格坐标的双向映射
 *
 * 技术特性:
 *   - FGrid2D<CellType>: 2D 网格容器
 *   - GetCell: 按行列或世界坐标获取单元格
 *   - SetCell: 设置单元格数据
 *   - WorldToGrid/GridToWorld: 坐标转换
 *   - GetNeighbors: 获取邻居单元格
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h,
 *          Core/Math/FMath.h, Core/Math/FVector.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"

namespace Limx
{

/// 2D 网格
/// @tparam CellType 单元格数据类型
template<typename CellType>
class FGrid2D
{
public:
    /// 构造空网格
    FGrid2D()
        : m_Rows(0)
        , m_Columns(0)
        , m_CellSize(1.0f)
        , m_OriginX(0.0f)
        , m_OriginY(0.0f)
    {
    }

    /// 构造指定大小的网格
    /// @param rows 行数
    /// @param columns 列数
    /// @param cellSize 单元格大小 (正方形)
    /// @param originX 网格原点 X (左上角世界坐标)
    /// @param originY 网格原点 Y
    FGrid2D(SizeType rows, SizeType columns,
            Float32 cellSize,
            Float32 originX = 0.0f,
            Float32 originY = 0.0f)
        : m_Rows(rows)
        , m_Columns(columns)
        , m_CellSize(cellSize)
        , m_OriginX(originX)
        , m_OriginY(originY)
    {
        m_Cells.Reserve(rows * columns);
        for (SizeType cellIdx = 0;
             cellIdx < rows * columns; ++cellIdx)
        {
            m_Cells.Add(CellType());
        }
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 按行列获取单元格 (只读)
    LIMX_NODISCARD const CellType& GetCell(
        SizeType row, SizeType column) const
    {
        return m_Cells[row * m_Columns + column];
    }

    /// 按行列获取单元格 (可写)
    LIMX_NODISCARD CellType& GetCell(
        SizeType row, SizeType column)
    {
        return m_Cells[row * m_Columns + column];
    }

    /// 设置单元格数据
    void SetCell(SizeType row, SizeType column,
                 const CellType& value)
    {
        m_Cells[row * m_Columns + column] = value;
    }

    /// 按扁平索引访问
    LIMX_NODISCARD const CellType& GetCellFlat(
        SizeType index) const
    {
        return m_Cells[index];
    }

    LIMX_NODISCARD CellType& GetCellFlat(SizeType index)
    {
        return m_Cells[index];
    }

    // ========================================================================
    // 坐标转换
    // ========================================================================

    /// 世界坐标转网格行列
    void WorldToGrid(Float32 worldX, Float32 worldY,
                     Int32& outRow, Int32& outColumn) const
    {
        outColumn = static_cast<Int32>(
            (worldX - m_OriginX) / m_CellSize);
        outRow = static_cast<Int32>(
            (worldY - m_OriginY) / m_CellSize);
    }

    /// 网格行列转世界坐标 (单元格中心)
    void GridToWorld(SizeType row, SizeType column,
                     Float32& outX, Float32& outY) const
    {
        outX = m_OriginX +
               (static_cast<Float32>(column) + 0.5f) *
               m_CellSize;
        outY = m_OriginY +
               (static_cast<Float32>(row) + 0.5f) *
               m_CellSize;
    }

    /// 网格行列转世界坐标 (FVector2)
    LIMX_NODISCARD FVector2 GridToWorld(
        SizeType row, SizeType column) const
    {
        Float32 x, y;
        GridToWorld(row, column, x, y);
        return FVector2(x, y);
    }

    /// 行列是否在网格范围内
    LIMX_NODISCARD bool IsValidCell(
        Int32 row, Int32 column) const
    {
        return row >= 0 &&
               column >= 0 &&
               static_cast<SizeType>(row) < m_Rows &&
               static_cast<SizeType>(column) < m_Columns;
    }

    // ========================================================================
    // 邻居查询
    // ========================================================================

    /// 获取 4 邻居 (上下左右) 的有效索引
    void GetNeighbors4(SizeType row, SizeType column,
                       TArray<SizeType>& outIndices) const
    {
        outIndices.Clear();
        static constexpr Int32 kDirs[4][2] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1}
        };

        for (Int32 dirIdx = 0; dirIdx < 4; ++dirIdx)
        {
            Int32 nr = static_cast<Int32>(row) +
                       kDirs[dirIdx][0];
            Int32 nc = static_cast<Int32>(column) +
                       kDirs[dirIdx][1];
            if (IsValidCell(nr, nc))
            {
                outIndices.Add(
                    static_cast<SizeType>(nr) * m_Columns +
                    static_cast<SizeType>(nc));
            }
        }
    }

    /// 获取 8 邻居 (含对角) 的有效索引
    void GetNeighbors8(SizeType row, SizeType column,
                       TArray<SizeType>& outIndices) const
    {
        outIndices.Clear();
        static constexpr Int32 kDirs[8][2] = {
            {-1, -1}, {-1, 0}, {-1, 1},
            { 0, -1},          { 0, 1},
            { 1, -1}, { 1, 0}, { 1, 1}
        };

        for (Int32 dirIdx = 0; dirIdx < 8; ++dirIdx)
        {
            Int32 nr = static_cast<Int32>(row) +
                       kDirs[dirIdx][0];
            Int32 nc = static_cast<Int32>(column) +
                       kDirs[dirIdx][1];
            if (IsValidCell(nr, nc))
            {
                outIndices.Add(
                    static_cast<SizeType>(nr) * m_Columns +
                    static_cast<SizeType>(nc));
            }
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetRows() const
    {
        return m_Rows;
    }
    LIMX_NODISCARD SizeType GetColumns() const
    {
        return m_Columns;
    }
    LIMX_NODISCARD SizeType GetCellCount() const
    {
        return m_Rows * m_Columns;
    }
    LIMX_NODISCARD Float32 GetCellSize() const
    {
        return m_CellSize;
    }
    LIMX_NODISCARD Float32 GetOriginX() const
    {
        return m_OriginX;
    }
    LIMX_NODISCARD Float32 GetOriginY() const
    {
        return m_OriginY;
    }

    /// 网格覆盖的世界宽度
    LIMX_NODISCARD Float32 GetWorldWidth() const
    {
        return static_cast<Float32>(m_Columns) * m_CellSize;
    }

    /// 网格覆盖的世界高度
    LIMX_NODISCARD Float32 GetWorldHeight() const
    {
        return static_cast<Float32>(m_Rows) * m_CellSize;
    }

    /// 用指定值填充所有单元格
    void Fill(const CellType& value)
    {
        for (SizeType cellIdx = 0;
             cellIdx < m_Cells.GetSize(); ++cellIdx)
        {
            m_Cells[cellIdx] = value;
        }
    }

private:
    TArray<CellType> m_Cells;     ///< 扁平单元格数组
    SizeType         m_Rows;      ///< 行数
    SizeType         m_Columns;   ///< 列数
    Float32          m_CellSize;  ///< 单元格大小
    Float32          m_OriginX;   ///< 原点 X
    Float32          m_OriginY;   ///< 原点 Y
};

} // namespace Limx
