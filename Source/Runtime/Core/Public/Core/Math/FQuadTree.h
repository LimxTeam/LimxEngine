/*******************************************************************************
 * 文件: FQuadTree.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   四叉树 — 2D 递归空间索引
 *   将 2D 空间递归四等分，快速查询指定矩形区域内的元素
 *   用于 2D 碰撞检测宽相、可见性剔除、空间查询优化等场景
 *
 * 设计哲学:
 *   固定节点池 — 预分配节点，无碎片化堆分配
 *   矩形包围 — 每节点以 AABB 定义区域
 *   整数容量限制 — 每叶节点超出阈值后分裂
 *
 * 技术特性:
 *   - FQuadTree<T, MaxDepth, BucketSize>: 2D 四叉树
 *   - Insert: 插入元素
 *   - Query: 矩形范围查询
 *   - Clear: 清空重置
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FRect.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FRect.h"
#include "Core/Containers/TArray.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 2D 四叉树
/// @tparam T 元素类型 (须提供 Bounds: FRect 字段)
/// @tparam MaxDepth 最大分裂深度 (默认 8)
/// @tparam BucketSize 叶节点最大元素数 (默认 16)
template<typename T,
         Int32 MaxDepth = 8,
         Int32 BucketSize = 16>
class FQuadTree
{
    static_assert(MaxDepth > 0,
        "MaxDepth must be > 0");
    static_assert(BucketSize > 0,
        "BucketSize must be > 0");

    struct FNode
    {
        FRect    Bounds;              ///< 节点覆盖区域
        Int32    Children[4];         ///< 子节点索引 (-1 = 无)
        TArray<T> Elements;           ///< 叶节点存储的元素
        bool     IsLeaf;             ///< 是否为叶节点

        FNode()
            : IsLeaf(true)
        {
            Children[0] = -1;
            Children[1] = -1;
            Children[2] = -1;
            Children[3] = -1;
        }
    };

public:
    /// 构造时需指定覆盖范围
    explicit FQuadTree(const FRect& worldBounds)
    {
        FNode root;
        root.Bounds = worldBounds;
        m_Nodes.Add(root);
    }

    // ========================================================================
    // 插入
    // ========================================================================

    /// 插入元素 (元素须提供 Bounds 字段)
    /// @param element 要插入的元素
    /// @param bounds 元素的 2D 包围矩形
    void Insert(const T& element, const FRect& bounds)
    {
        InsertIntoNode(0, element, bounds, 0);
    }

    void Insert(T&& element, const FRect& bounds)
    {
        InsertIntoNode(
            0, static_cast<T&&>(element), bounds, 0);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 矩形范围查询
    /// @param queryBounds 查询矩形
    /// @param outResults 结果数组
    void Query(const FRect& queryBounds,
               TArray<T>& outResults) const
    {
        QueryNode(0, queryBounds, outResults);
    }

    // ========================================================================
    // 重置
    // ========================================================================

    /// 清空四叉树 (保留根节点区域)
    void Clear()
    {
        FRect rootBounds = m_Nodes[0].Bounds;
        m_Nodes.Clear();

        FNode root;
        root.Bounds = rootBounds;
        m_Nodes.Add(root);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 总元素数
    LIMX_NODISCARD Int32 GetTotalElementCount() const
    {
        Int32 count = 0;
        CountElements(0, count);
        return count;
    }

    /// 节点数
    LIMX_NODISCARD Int32 GetNodeCount() const
    {
        return static_cast<Int32>(m_Nodes.GetSize());
    }

    /// 世界包围矩形
    LIMX_NODISCARD const FRect& GetWorldBounds() const
    {
        return m_Nodes[0].Bounds;
    }

private:
    void InsertIntoNode(Int32 nodeIdx,
                        const T& element,
                        const FRect& bounds,
                        Int32 depth)
    {
        FNode& node = m_Nodes[nodeIdx];

        // 叶节点且未满，直接插入
        if (node.IsLeaf &&
            static_cast<Int32>(
                node.Elements.GetSize()) < BucketSize)
        {
            node.Elements.Add(element);
            return;
        }

        // 叶节点但已满，且未达最大深度 — 分裂
        if (node.IsLeaf && depth < MaxDepth)
        {
            Subdivide(nodeIdx);
        }

        // 非叶节点 — 放入与 bounds 相交的子节点
        if (!m_Nodes[nodeIdx].IsLeaf)
        {
            bool insertedIntoChild = false;
            for (Int32 childSlot = 0;
                 childSlot < 4; ++childSlot)
            {
                Int32 childIdx =
                    m_Nodes[nodeIdx].Children[childSlot];
                if (childIdx != -1 &&
                    m_Nodes[childIdx].Bounds.Intersects(bounds))
                {
                    InsertIntoNode(
                        childIdx, element, bounds,
                        depth + 1);
                    insertedIntoChild = true;
                }
            }
            if (!insertedIntoChild)
            {
                // 跨越多个子节点，放入父节点
                m_Nodes[nodeIdx].Elements.Add(element);
            }
            return;
        }

        // 已达最大深度，强制插入
        m_Nodes[nodeIdx].Elements.Add(element);
    }

    void InsertIntoNode(Int32 nodeIdx, T&& element,
                        const FRect& bounds, Int32 depth)
    {
        FNode& node = m_Nodes[nodeIdx];

        if (node.IsLeaf &&
            static_cast<Int32>(
                node.Elements.GetSize()) < BucketSize)
        {
            node.Elements.Add(static_cast<T&&>(element));
            return;
        }

        if (node.IsLeaf && depth < MaxDepth)
        {
            Subdivide(nodeIdx);
        }

        if (!m_Nodes[nodeIdx].IsLeaf)
        {
            bool insertedIntoChild = false;
            for (Int32 childSlot = 0;
                 childSlot < 4; ++childSlot)
            {
                Int32 childIdx =
                    m_Nodes[nodeIdx].Children[childSlot];
                if (childIdx != -1 &&
                    m_Nodes[childIdx].Bounds.Intersects(bounds))
                {
                    InsertIntoNode(
                        childIdx,
                        static_cast<T&&>(element),
                        bounds, depth + 1);
                    insertedIntoChild = true;
                    break;
                }
            }
            if (!insertedIntoChild)
            {
                m_Nodes[nodeIdx].Elements.Add(
                    static_cast<T&&>(element));
            }
            return;
        }

        m_Nodes[nodeIdx].Elements.Add(
            static_cast<T&&>(element));
    }

    void Subdivide(Int32 nodeIdx)
    {
        FNode& node = m_Nodes[nodeIdx];
        node.IsLeaf = false;

        Float32 cx = node.Bounds.X +
                     node.Bounds.Width * 0.5f;
        Float32 cy = node.Bounds.Y +
                     node.Bounds.Height * 0.5f;
        Float32 hw = node.Bounds.Width * 0.5f;
        Float32 hh = node.Bounds.Height * 0.5f;

        // 四个象限: 左下/右下/左上/右上
        FRect quadBounds[4];
        quadBounds[0] = FRect(
            node.Bounds.X, node.Bounds.Y, hw, hh);
        quadBounds[1] = FRect(cx, node.Bounds.Y, hw, hh);
        quadBounds[2] = FRect(
            node.Bounds.X, cy, hw, hh);
        quadBounds[3] = FRect(cx, cy, hw, hh);

        for (Int32 quadIdx = 0; quadIdx < 4; ++quadIdx)
        {
            FNode child;
            child.Bounds = quadBounds[quadIdx];
            node.Children[quadIdx] =
                static_cast<Int32>(m_Nodes.GetSize());
            m_Nodes.Add(child);
        }
    }

    void QueryNode(Int32 nodeIdx,
                   const FRect& queryBounds,
                   TArray<T>& outResults) const
    {
        const FNode& node = m_Nodes[nodeIdx];
        if (!node.Bounds.Intersects(queryBounds)) return;

        // 收集本节点元素
        for (SizeType elemIdx = 0;
             elemIdx < node.Elements.GetSize(); ++elemIdx)
        {
            outResults.Add(node.Elements[elemIdx]);
        }

        // 递归子节点
        if (!node.IsLeaf)
        {
            for (Int32 childSlot = 0;
                 childSlot < 4; ++childSlot)
            {
                Int32 childIdx =
                    node.Children[childSlot];
                if (childIdx != -1)
                {
                    QueryNode(childIdx,
                              queryBounds, outResults);
                }
            }
        }
    }

    void CountElements(Int32 nodeIdx, Int32& count) const
    {
        const FNode& node = m_Nodes[nodeIdx];
        count += static_cast<Int32>(
            node.Elements.GetSize());

        if (!node.IsLeaf)
        {
            for (Int32 childSlot = 0;
                 childSlot < 4; ++childSlot)
            {
                Int32 childIdx =
                    node.Children[childSlot];
                if (childIdx != -1)
                {
                    CountElements(childIdx, count);
                }
            }
        }
    }

    TArray<FNode> m_Nodes;  ///< 节点池
};

} // namespace Limx
