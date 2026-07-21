#pragma once

#include "Volt/Core/Support/StringInterner.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace Volt
{

namespace Driver
{

    // The module dependency graph of a circuit: one node per module, one
    // directed edge per `@[Link("Other")]`. Kept as flat index vectors (no
    // pointers) so it is cheap to build in parallel and to serialise.
    //
    // A circuit's build order is the reverse-topological order of this graph;
    // a `@[Link]` cycle (see samples/Circuits/CycleDeps) is a hard error, so
    // the graph reports the offending cycle rather than silently looping.
    class CircuitGraph
    {

    public:

        using NodeIndex = std::size_t;

        static constexpr NodeIndex InvalidNode = ~NodeIndex{ 0 };

        // Register a module by name, returning its stable index. Idempotent:
        // the same name always maps to the same node.
        NodeIndex AddModule ( std::string Name );

        // Record `From` links `To`. Both are created on demand.
        void AddLink ( const std::string &From, const std::string &To );

        [[nodiscard]] NodeIndex Find ( const std::string &Name ) const;

        [[nodiscard]] std::size_t ModuleCount () const
        {
            return Names.size();
        }

        [[nodiscard]] const std::string &NameOf ( NodeIndex Node ) const
        {
            return Names[Node];
        }

        [[nodiscard]] const std::vector<NodeIndex> &LinksOf ( NodeIndex Node ) const
        {
            return Edges[Node];
        }

        // Reverse-topological build order (a module appears after everything
        // it links). Returns false and leaves Order empty when a cycle is
        // present; the cycle is then available via FindCycle().
        [[nodiscard]] bool TopoOrder ( std::vector<NodeIndex> &Order ) const;

        // The node sequence of one dependency cycle (first == last), or an
        // empty vector when the graph is acyclic.
        [[nodiscard]] std::vector<NodeIndex> FindCycle () const;

    private:

        std::vector<std::string> Names;
        std::vector<std::vector<NodeIndex>> Edges;
        std::unordered_map<std::string, NodeIndex> Lookup;
    };

} // namespace Driver

} // namespace Volt
