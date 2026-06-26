/*******************************************************************************
 * 文件: TGraph.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   有向图 — 基于邻接表的有向图数据结构
 *   支持节点/边增删、拓扑排序、可达性查询
 *   用于渲染图、任务依赖、模块加载顺序、资源依赖分析等场景
 *
 * 设计哲学:
 *   邻接表 — TArray<TArray<SizeType>> 存储出边
 *   节点索引 — 节点以连续整数索引，数据通过外部数组按索引关联
 *   算法内建 — 拓扑排序、环检测等常用图算法直接内建
 *
 * 技术特性:
 *   - TGraph<NodeData>: 参数化有向图
 *   - AddNode: 添加节点，返回索引
 *   - AddEdge: 添加有向边
 *   - TopologicalSort: 拓扑排序 (Kahn 算法)
 *   - HasCycle: 环检测
 *   - GetSuccessors/GetPredecessors: 后继/前驱查询
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 有向图
/// @tparam NodeData 节点关联数据类型
template<typename NodeData>
class TGraph
{
public:
    /// 无效节点索引
    static constexpr SizeType kInvalidNode =
        static_cast<SizeType>(-1);

    TGraph() = default;

    // ========================================================================
    // 节点操作
    // ========================================================================

    /// 添加节点
    /// @return 节点索引
    SizeType AddNode(const NodeData& data)
    {
        SizeType index = m_NodeData.GetSize();
        m_NodeData.Add(data);
        m_Adjacency.Add(TArray<SizeType>());
        return index;
    }

    /// 添加节点 (移动)
    SizeType AddNode(NodeData&& data)
    {
        SizeType index = m_NodeData.GetSize();
        m_NodeData.Add(MoveTemp(data));
        m_Adjacency.Add(TArray<SizeType>());
        return index;
    }

    /// 获取节点数据
    LIMX_NODISCARD NodeData& GetNodeData(SizeType nodeIndex)
    {
        LIMX_ASSERT(nodeIndex < m_NodeData.GetSize());
        return m_NodeData[nodeIndex];
    }

    LIMX_NODISCARD const NodeData& GetNodeData(
        SizeType nodeIndex) const
    {
        LIMX_ASSERT(nodeIndex < m_NodeData.GetSize());
        return m_NodeData[nodeIndex];
    }

    /// 节点数量
    LIMX_NODISCARD SizeType GetNodeCount() const
    {
        return m_NodeData.GetSize();
    }

    // ========================================================================
    // 边操作
    // ========================================================================

    /// 添加有向边 (from -> to)
    void AddEdge(SizeType fromNode, SizeType toNode)
    {
        LIMX_ASSERT(fromNode < m_NodeData.GetSize());
        LIMX_ASSERT(toNode < m_NodeData.GetSize());

        // 避免重复边
        TArray<SizeType>& neighbors = m_Adjacency[fromNode];
        for (SizeType edgeIndex = 0;
             edgeIndex < neighbors.GetSize(); ++edgeIndex)
        {
            if (neighbors[edgeIndex] == toNode) return;
        }
        neighbors.Add(toNode);
    }

    /// 移除有向边
    bool RemoveEdge(SizeType fromNode, SizeType toNode)
    {
        LIMX_ASSERT(fromNode < m_NodeData.GetSize());
        TArray<SizeType>& neighbors = m_Adjacency[fromNode];
        for (SizeType edgeIndex = 0;
             edgeIndex < neighbors.GetSize(); ++edgeIndex)
        {
            if (neighbors[edgeIndex] == toNode)
            {
                neighbors.RemoveAt(edgeIndex);
                return true;
            }
        }
        return false;
    }

    /// 是否存在边
    LIMX_NODISCARD bool HasEdge(
        SizeType fromNode, SizeType toNode) const
    {
        LIMX_ASSERT(fromNode < m_NodeData.GetSize());
        const TArray<SizeType>& neighbors = m_Adjacency[fromNode];
        for (SizeType edgeIndex = 0;
             edgeIndex < neighbors.GetSize(); ++edgeIndex)
        {
            if (neighbors[edgeIndex] == toNode) return true;
        }
        return false;
    }

    /// 获取节点的后继列表
    LIMX_NODISCARD const TArray<SizeType>& GetSuccessors(
        SizeType nodeIndex) const
    {
        LIMX_ASSERT(nodeIndex < m_NodeData.GetSize());
        return m_Adjacency[nodeIndex];
    }

    /// 获取节点的前驱列表
    LIMX_NODISCARD TArray<SizeType> GetPredecessors(
        SizeType nodeIndex) const
    {
        TArray<SizeType> predecessors;
        for (SizeType candidateIndex = 0;
             candidateIndex < m_Adjacency.GetSize();
             ++candidateIndex)
        {
            const TArray<SizeType>& neighbors =
                m_Adjacency[candidateIndex];
            for (SizeType edgeIndex = 0;
                 edgeIndex < neighbors.GetSize(); ++edgeIndex)
            {
                if (neighbors[edgeIndex] == nodeIndex)
                {
                    predecessors.Add(candidateIndex);
                    break;
                }
            }
        }
        return predecessors;
    }

    /// 获取节点的出度
    LIMX_NODISCARD SizeType GetOutDegree(
        SizeType nodeIndex) const
    {
        LIMX_ASSERT(nodeIndex < m_NodeData.GetSize());
        return m_Adjacency[nodeIndex].GetSize();
    }

    /// 获取总边数
    LIMX_NODISCARD SizeType GetEdgeCount() const
    {
        SizeType edgeTotal = 0;
        for (SizeType nodeIndex = 0;
             nodeIndex < m_Adjacency.GetSize(); ++nodeIndex)
        {
            edgeTotal += m_Adjacency[nodeIndex].GetSize();
        }
        return edgeTotal;
    }

    // ========================================================================
    // 算法
    // ========================================================================

    /// 拓扑排序 (Kahn 算法)
    /// @param outOrder 输出排序结果 (节点索引)
    /// @return 是否成功 (false = 存在环)
    bool TopologicalSort(TArray<SizeType>& outOrder) const
    {
        SizeType nodeCount = m_NodeData.GetSize();
        outOrder.Clear();

        if (nodeCount == 0) return true;

        // 计算每个节点的入度
        TArray<SizeType> inDegree;
        inDegree.Reserve(nodeCount);
        for (SizeType initIndex = 0;
             initIndex < nodeCount; ++initIndex)
        {
            inDegree.Add(0);
        }

        for (SizeType sourceIndex = 0;
             sourceIndex < nodeCount; ++sourceIndex)
        {
            const TArray<SizeType>& neighbors =
                m_Adjacency[sourceIndex];
            for (SizeType edgeIndex = 0;
                 edgeIndex < neighbors.GetSize(); ++edgeIndex)
            {
                ++inDegree[neighbors[edgeIndex]];
            }
        }

        // 收集入度为 0 的节点到队列
        TArray<SizeType> queue;
        for (SizeType scanIndex = 0;
             scanIndex < nodeCount; ++scanIndex)
        {
            if (inDegree[scanIndex] == 0)
            {
                queue.Add(scanIndex);
            }
        }

        // BFS 处理
        SizeType queueFront = 0;
        while (queueFront < queue.GetSize())
        {
            SizeType current = queue[queueFront];
            ++queueFront;
            outOrder.Add(current);

            const TArray<SizeType>& neighbors =
                m_Adjacency[current];
            for (SizeType edgeIndex = 0;
                 edgeIndex < neighbors.GetSize(); ++edgeIndex)
            {
                SizeType neighbor = neighbors[edgeIndex];
                --inDegree[neighbor];
                if (inDegree[neighbor] == 0)
                {
                    queue.Add(neighbor);
                }
            }
        }

        // 如果排序结果不包含所有节点，则存在环
        return outOrder.GetSize() == nodeCount;
    }

    /// 是否存在环
    LIMX_NODISCARD bool HasCycle() const
    {
        TArray<SizeType> order;
        return !TopologicalSort(order);
    }

    /// 清空图
    void Clear()
    {
        m_NodeData.Clear();
        m_Adjacency.Clear();
    }

private:
    TArray<NodeData>         m_NodeData;   ///< 节点数据
    TArray<TArray<SizeType>> m_Adjacency;  ///< 邻接表
};

} // namespace Limx
