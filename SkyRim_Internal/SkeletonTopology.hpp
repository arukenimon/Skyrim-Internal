#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Skyrim::Skeleton
{
    using NodeId = std::uintptr_t;
    struct Edge { std::size_t child, parent; };

    // Skin palettes omit some transform nodes and use mesh-local indices.
    // Connect a bone to its nearest captured ancestor in the actual scene
    // hierarchy. Validate the chain reaches this actor's root; reject cycles
    // and unreadable chains. This is done once when its 3D is captured.
    template <class ParentReader>
    [[nodiscard]] std::vector<Edge> BuildEdges(
        const std::span<const NodeId> nodes, const NodeId root, ParentReader&& readParent)
    {
        std::unordered_map<NodeId, std::size_t> indices;
        indices.reserve(nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index)
            if (nodes[index] != 0)
                indices.try_emplace(nodes[index], index);

        struct Parent { NodeId id; bool readable; };
        std::unordered_map<NodeId, Parent> parents;
        parents.reserve(nodes.size() * 2);
        std::vector<Edge> edges;
        edges.reserve(nodes.size());
        for (std::size_t child = 0; child < nodes.size(); ++child)
        {
            if (nodes[child] == 0 || nodes[child] == root || indices.at(nodes[child]) != child)
                continue;
            NodeId current = nodes[child];
            std::array<NodeId, 64> visited{};
            std::size_t depth = 0;
            std::size_t nearest = nodes.size();
            bool reachedRoot = false;
            while (current != 0 && depth < visited.size())
            {
                if (current == root)
                {
                    reachedRoot = true;
                    break;
                }
                bool cycle = false;
                for (std::size_t index = 0; index < depth; ++index)
                    if (visited[index] == current)
                        cycle = true;
                if (cycle)
                    break;
                visited[depth++] = current;
                auto parent = parents.find(current);
                if (parent == parents.end())
                {
                    NodeId parentId = 0;
                    const bool readable = readParent(current, parentId);
                    parent = parents.emplace(current, Parent{ parentId, readable }).first;
                }
                if (!parent->second.readable)
                    break;
                current = parent->second.id;
                const auto captured = indices.find(current);
                if (nearest == nodes.size() && captured != indices.end() && captured->second != child)
                    nearest = captured->second;
            }
            if (reachedRoot && nearest != nodes.size())
                edges.push_back({ child, nearest });
        }
        return edges;
    }
}
