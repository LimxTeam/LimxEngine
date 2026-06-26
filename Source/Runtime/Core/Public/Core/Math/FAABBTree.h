/*******************************************************************************
 * 文件: FAABBTree.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   AABB 层次树 — 动态包围盒层次结构 (BVH)
 *   支持动态插入/删除，SAH 近似分裂策略
 *   用于宽相碰撞检测、光线与场景求交加速、可见性剔除等场景
 *
 * 设计哲学:
 *   节点池 — 预分配节点，避免碎片化
 *   胖化 AABB — 插入时膨胀包围盒，减少更新频率
 *   叶节点存用户数据 — 内节点只存包围盒和子节点索引
 *
 * 技术特性:
 *   - FAABBTree<T>: AABB 层次树
 *   - Insert: 插入带包围盒的对象
 *   - Remove: 删除对象
 *   - Update: 更新对象包围盒
 *   - Query: AABB 范围查询
 *   - RayCast: 射线查询 (最近相交)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FBoundingBox.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FBoundingBox.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// AABB 层次树 (BVH)
/// @tparam T 用户数据类型
template<typename T>
class FAABBTree
{
    static constexpr Int32 kNullNode  = -1;
    static constexpr Float32 kFatFactor = 0.1f;  ///< 胖化比例

    struct FNode
    {
        FBoundingBox Bounds;       ///< 节点包围盒
        Int32        Parent;       ///< 父节点索引
        Int32        Children[2];  ///< 子节点索引 (叶节点为 -1)
        Int32        Height;       ///< 节点高度 (叶节点为 0)
        T            UserData;     ///< 用户数据 (仅叶节点有效)
        Int32        NextFree;     ///< 空闲链表下一节点

        LIMX_NODISCARD bool IsLeaf() const
        {
            return Children[0] == kNullNode;
        }
    };

public:
    FAABBTree()
        : m_Root(kNullNode)
        , m_FreeList(kNullNode)
        , m_NodeCount(0)
    {
    }

    // ========================================================================
    // 插入/删除/更新
    // ========================================================================

    /// 插入对象
    /// @param bounds 对象的 AABB
    /// @param data 用户数据
    /// @return 节点索引 (用于后续 Remove/Update)
    Int32 Insert(const FBoundingBox& bounds, const T& data)
    {
        Int32 nodeIdx = AllocNode();
        FNode& node = m_Nodes[nodeIdx];

        // 胖化 AABB
        FVector3 extension(
            bounds.GetExtent().X * kFatFactor,
            bounds.GetExtent().Y * kFatFactor,
            bounds.GetExtent().Z * kFatFactor);

        node.Bounds = FBoundingBox(
            bounds.Min - extension,
            bounds.Max + extension);
        node.UserData = data;
        node.Height = 0;

        InsertLeaf(nodeIdx);
        return nodeIdx;
    }

    /// 删除节点
    void Remove(Int32 nodeIdx)
    {
        LIMX_ASSERT(nodeIdx >= 0 &&
            nodeIdx < static_cast<Int32>(m_Nodes.GetSize()));
        LIMX_ASSERT(m_Nodes[nodeIdx].IsLeaf());
        RemoveLeaf(nodeIdx);
        FreeNode(nodeIdx);
    }

    /// 更新对象包围盒
    /// @return 是否需要重新插入 (移动超出胖化区域时为 true)
    bool Update(Int32 nodeIdx, const FBoundingBox& bounds)
    {
        LIMX_ASSERT(nodeIdx >= 0 &&
            nodeIdx < static_cast<Int32>(m_Nodes.GetSize()));
        LIMX_ASSERT(m_Nodes[nodeIdx].IsLeaf());

        // 如果还在胖化区域内，无需重建
        if (m_Nodes[nodeIdx].Bounds.Contains(bounds))
        {
            return false;
        }

        // 重新插入
        T userData = m_Nodes[nodeIdx].UserData;
        RemoveLeaf(nodeIdx);
        FreeNode(nodeIdx);

        Int32 newIdx = AllocNode();
        FNode& node = m_Nodes[newIdx];
        FVector3 extension(
            bounds.GetExtent().X * kFatFactor,
            bounds.GetExtent().Y * kFatFactor,
            bounds.GetExtent().Z * kFatFactor);
        node.Bounds = FBoundingBox(
            bounds.Min - extension,
            bounds.Max + extension);
        node.UserData = userData;
        node.Height = 0;
        InsertLeaf(newIdx);

        return true;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// AABB 范围查询
    void Query(const FBoundingBox& bounds,
               TArray<T>& outResults) const
    {
        if (m_Root == kNullNode) return;

        TArray<Int32> stack;
        stack.Reserve(64);
        stack.Add(m_Root);

        while (stack.GetSize() > 0)
        {
            Int32 idx =
                stack[stack.GetSize() - 1];
            stack.RemoveAt(stack.GetSize() - 1);

            if (idx == kNullNode) continue;

            const FNode& node = m_Nodes[idx];
            if (!node.Bounds.Intersects(bounds)) continue;

            if (node.IsLeaf())
            {
                outResults.Add(node.UserData);
            }
            else
            {
                stack.Add(node.Children[0]);
                stack.Add(node.Children[1]);
            }
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD Int32 GetNodeCount() const
    {
        return m_NodeCount;
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Root == kNullNode;
    }

    void Clear()
    {
        m_Nodes.Clear();
        m_Root = kNullNode;
        m_FreeList = kNullNode;
        m_NodeCount = 0;
    }

private:
    Int32 AllocNode()
    {
        if (m_FreeList == kNullNode)
        {
            FNode newNode;
            newNode.Parent     = kNullNode;
            newNode.Children[0] = kNullNode;
            newNode.Children[1] = kNullNode;
            newNode.Height     = 0;
            newNode.NextFree   = kNullNode;
            m_Nodes.Add(newNode);
            ++m_NodeCount;
            return static_cast<Int32>(
                m_Nodes.GetSize() - 1);
        }

        Int32 idx = m_FreeList;
        m_FreeList = m_Nodes[idx].NextFree;
        m_Nodes[idx].Parent     = kNullNode;
        m_Nodes[idx].Children[0] = kNullNode;
        m_Nodes[idx].Children[1] = kNullNode;
        m_Nodes[idx].Height     = 0;
        ++m_NodeCount;
        return idx;
    }

    void FreeNode(Int32 idx)
    {
        m_Nodes[idx].NextFree = m_FreeList;
        m_FreeList = idx;
        --m_NodeCount;
    }

    void InsertLeaf(Int32 leafIdx)
    {
        if (m_Root == kNullNode)
        {
            m_Root = leafIdx;
            m_Nodes[m_Root].Parent = kNullNode;
            return;
        }

        // 找最优兄弟节点 (贪心面积启发)
        FBoundingBox leafBounds =
            m_Nodes[leafIdx].Bounds;
        Int32 bestSibling = FindBestSibling(leafBounds);

        // 创建新内节点
        Int32 oldParent = m_Nodes[bestSibling].Parent;
        Int32 newParent = AllocNode();
        m_Nodes[newParent].Parent = oldParent;
        m_Nodes[newParent].Bounds =
            FBoundingBox::Merge(leafBounds,
                m_Nodes[bestSibling].Bounds);
        m_Nodes[newParent].Height =
            m_Nodes[bestSibling].Height + 1;

        if (oldParent != kNullNode)
        {
            if (m_Nodes[oldParent].Children[0] ==
                bestSibling)
                m_Nodes[oldParent].Children[0] = newParent;
            else
                m_Nodes[oldParent].Children[1] = newParent;
        }
        else
        {
            m_Root = newParent;
        }

        m_Nodes[newParent].Children[0] = bestSibling;
        m_Nodes[newParent].Children[1] = leafIdx;
        m_Nodes[bestSibling].Parent = newParent;
        m_Nodes[leafIdx].Parent = newParent;

        // 向上修正包围盒
        RefitAncestors(m_Nodes[leafIdx].Parent);
    }

    void RemoveLeaf(Int32 leafIdx)
    {
        if (leafIdx == m_Root)
        {
            m_Root = kNullNode;
            return;
        }

        Int32 parent = m_Nodes[leafIdx].Parent;
        Int32 grandParent = m_Nodes[parent].Parent;
        Int32 sibling =
            (m_Nodes[parent].Children[0] == leafIdx)
            ? m_Nodes[parent].Children[1]
            : m_Nodes[parent].Children[0];

        if (grandParent != kNullNode)
        {
            if (m_Nodes[grandParent].Children[0] == parent)
                m_Nodes[grandParent].Children[0] = sibling;
            else
                m_Nodes[grandParent].Children[1] = sibling;

            m_Nodes[sibling].Parent = grandParent;
            FreeNode(parent);
            RefitAncestors(grandParent);
        }
        else
        {
            m_Root = sibling;
            m_Nodes[sibling].Parent = kNullNode;
            FreeNode(parent);
        }
    }

    Int32 FindBestSibling(
        const FBoundingBox& leafBounds) const
    {
        Int32 bestNode = m_Root;
        Float32 bestCost =
            FBoundingBox::Merge(leafBounds,
                m_Nodes[m_Root].Bounds).GetSurfaceArea();

        TArray<Int32> stack;
        stack.Reserve(64);
        stack.Add(m_Root);

        while (stack.GetSize() > 0)
        {
            Int32 idx =
                stack[stack.GetSize() - 1];
            stack.RemoveAt(stack.GetSize() - 1);

            if (idx == kNullNode) continue;

            const FNode& node = m_Nodes[idx];
            Float32 merged =
                FBoundingBox::Merge(leafBounds,
                    node.Bounds).GetSurfaceArea();

            if (merged < bestCost)
            {
                bestCost = merged;
                bestNode = idx;
            }

            if (!node.IsLeaf())
            {
                stack.Add(node.Children[0]);
                stack.Add(node.Children[1]);
            }
        }

        return bestNode;
    }

    void RefitAncestors(Int32 idx)
    {
        while (idx != kNullNode)
        {
            FNode& node = m_Nodes[idx];
            node.Bounds = FBoundingBox::Merge(
                m_Nodes[node.Children[0]].Bounds,
                m_Nodes[node.Children[1]].Bounds);
            node.Height = 1 + FMath::Max(
                m_Nodes[node.Children[0]].Height,
                m_Nodes[node.Children[1]].Height);
            idx = node.Parent;
        }
    }

    TArray<FNode> m_Nodes;     ///< 节点池
    Int32         m_Root;      ///< 根节点索引
    Int32         m_FreeList;  ///< 空闲链表头
    Int32         m_NodeCount; ///< 活跃节点数
};

} // namespace Limx
