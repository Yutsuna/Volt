#include "Volt/Driver/CircuitGraph.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

Volt::Driver::CircuitGraph::NodeIndex Volt::Driver::CircuitGraph::AddModule ( std::string Name )
{
    if ( const auto It = Lookup.find( Name ); It != Lookup.end() )
    {
        return It->second;
    }

    const NodeIndex Node = Names.size();
    Lookup.emplace( Name, Node );
    Names.push_back( std::move( Name ) );
    Edges.emplace_back();
    return Node;
}

void Volt::Driver::CircuitGraph::AddLink ( const std::string &From, const std::string &To )
{
    const NodeIndex FromNode = AddModule( From );
    const NodeIndex ToNode   = AddModule( To );

    std::vector<NodeIndex> &Out = Edges[FromNode];
    if ( std::ranges::find( Out, ToNode ) == Out.end() )
    {
        Out.push_back( ToNode );
    }
}

Volt::Driver::CircuitGraph::NodeIndex Volt::Driver::CircuitGraph::Find ( const std::string &Name ) const
{
    const auto It = Lookup.find( Name );
    return It != Lookup.end() ? It->second : InvalidNode;
}

namespace
{

// Colours for the standard cycle-detecting DFS.
enum class EMark : std::uint8_t
{

    White = 0, // unvisited
    Grey  = 1, // on the active stack
    Black = 2, // fully explored
};

} // namespace

bool Volt::Driver::CircuitGraph::TopoOrder ( std::vector<NodeIndex> &Order ) const
{
    Order.clear();
    const std::size_t Count = Names.size();

    std::vector<EMark> Marks( Count, EMark::White );

    // Iterative post-order DFS; a node pushed a second time (its children
    // done) is appended, giving reverse-topological order directly.
    std::vector<std::pair<NodeIndex, bool>> Stack;
    for ( NodeIndex Root = 0; Root < Count; ++Root )
    {
        if ( Marks[Root] != EMark::White )
        {
            continue;
        }

        Stack.emplace_back( Root, false );
        while ( !Stack.empty() )
        {
            const auto [Node, bChildrenDone] = Stack.back();
            Stack.pop_back();

            if ( bChildrenDone )
            {
                Marks[Node] = EMark::Black;
                Order.push_back( Node );
                continue;
            }

            if ( Marks[Node] == EMark::Black )
            {
                continue;
            }

            Marks[Node] = EMark::Grey;
            Stack.emplace_back( Node, true );

            for ( const NodeIndex Next : Edges[Node] )
            {
                if ( Marks[Next] == EMark::Grey )
                {
                    Order.clear();
                    return false; // back-edge -> cycle
                }
                if ( Marks[Next] == EMark::White )
                {
                    Stack.emplace_back( Next, false );
                }
            }
        }
    }

    return true;
}

std::vector<Volt::Driver::CircuitGraph::NodeIndex> Volt::Driver::CircuitGraph::FindCycle () const
{
    const std::size_t Count = Names.size();

    std::vector<EMark> Marks( Count, EMark::White );
    std::vector<NodeIndex> Path;

    // Recursive-style DFS emulated with an explicit stack of iterators so
    // the grey path on the stack *is* the reconstructable cycle.
    std::vector<std::pair<NodeIndex, std::size_t>> Stack;

    for ( NodeIndex Root = 0; Root < Count; ++Root )
    {
        if ( Marks[Root] != EMark::White )
        {
            continue;
        }

        Stack.emplace_back( Root, 0 );
        Marks[Root] = EMark::Grey;
        Path.push_back( Root );

        while ( !Stack.empty() )
        {
            auto &[Node, EdgeIdx] = Stack.back();

            if ( EdgeIdx < Edges[Node].size() )
            {
                const NodeIndex Next = Edges[Node][EdgeIdx];
                ++EdgeIdx;

                if ( Marks[Next] == EMark::Grey )
                {
                    // Cycle: slice Path from the first occurrence of Next
                    // and close it back onto Next.
                    const auto It = std::ranges::find( Path, Next );
                    std::vector<NodeIndex> Cycle( It, Path.end() );
                    Cycle.push_back( Next );
                    return Cycle;
                }
                if ( Marks[Next] == EMark::White )
                {
                    Marks[Next] = EMark::Grey;
                    Path.push_back( Next );
                    Stack.emplace_back( Next, 0 );
                }
            }
            else
            {
                Marks[Node] = EMark::Black;
                Path.pop_back();
                Stack.pop_back();
            }
        }
    }

    return {};
}
