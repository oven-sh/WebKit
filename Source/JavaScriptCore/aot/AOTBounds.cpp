/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

namespace {

class BoundsProver {
public:
    BoundsProver(Graph& graph)
        : m_graph(graph)
    {
    }

    bool run()
    {
        bool hasDominators = false;
        bool changed = false;
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (node->kind != NodeKind::Bytecode || node->indexIsInBounds || node->guard || node->guarded)
                    continue;
                Node* base;
                Node* index;
                if (node->opcode == op_get_by_val) {
                    base = node->use(node->as<OpGetByVal>().m_base);
                    index = node->use(node->as<OpGetByVal>().m_property);
                } else if (node->opcode == op_put_by_val) {
                    base = node->use(node->as<OpPutByVal>().m_base);
                    index = node->use(node->as<OpPutByVal>().m_property);
                } else
                    continue;
                if (!Graph::hasFixedLength(base) || !index->type || !isSubtype(index->type, TNumber) || !index->range.isKnown() || index->range.min < 0)
                    continue;
                if (!std::exchange(hasDominators, true))
                    m_graph.computeDominators();
                m_phisAssumed.clear();
                if (isBelowLength(origin(index), origin(base), block, 0)) {
                    node->indexIsInBounds = true;
                    m_graph.remark("index-is-in-bounds"_s);
                    changed = true;
                }
            }
        }
        return changed;
    }

private:
    static Node* origin(Node* node)
    {
        while (node->kind == NodeKind::Narrow)
            node = node->uses[0].node;
        return node;
    }

    static bool isLengthOf(Node* value, Node* base)
    {
        value = origin(value);
        if (value->isBytecode(op_get_length))
            return origin(value->use(value->as<OpGetLength>().m_base)) == base;
        if (base->isBytecode(op_construct) && base->as<OpConstruct>().m_argc == 2 && value->type && isSubtype(value->type, TNumber) && value->range.isKnown()) {
            auto bytecode = base->as<OpConstruct>();
            return origin(base->use(VirtualRegister(-static_cast<int>(bytecode.m_argv) + CallFrame::thisArgumentOffset() + 1))) == value;
        }
        return false;
    }

    static bool holdsOnEdge(BasicBlock* from, BasicBlock* to, Node* value, Node* base)
    {
        Node* branch = from->terminal();
        if (!branch || branch->kind != NodeKind::Bytecode || from->successors.size() != 2 || from->successors[0] == from->successors[1])
            return false;
        bool isTaken = to == from->successors[0];
        VirtualRegister smaller;
        VirtualRegister larger;
        switch (branch->opcode) {
#define AOT_CASE(Struct, opcodeName, whenTaken, small, large) \
        case opcodeName: { \
            if (isTaken != whenTaken) \
                return false; \
            auto bytecode = branch->as<Struct>(); \
            smaller = bytecode.small; \
            larger = bytecode.large; \
            break; \
        }
        AOT_CASE(OpJless, op_jless, true, m_lhs, m_rhs)
        AOT_CASE(OpJnless, op_jnless, false, m_lhs, m_rhs)
        AOT_CASE(OpJgreater, op_jgreater, true, m_rhs, m_lhs)
        AOT_CASE(OpJngreater, op_jngreater, false, m_rhs, m_lhs)
#undef AOT_CASE
        default:
            return false;
        }
        Node* bound = branch->use(larger);
        if (!bound->type || !isSubtype(bound->type, TNumber))
            return false;
        return origin(branch->use(smaller)) == value && isLengthOf(bound, base);
    }

    bool isBelowLength(Node* value, Node* base, BasicBlock* block, unsigned depth)
    {
        for (BasicBlock* at = block; at; at = at->immediateDominator) {
            if (at->predecessors.size() == 1 && holdsOnEdge(at->predecessors[0], at, value, base))
                return true;
            if (at == value->block)
                break;
        }
        if (value->kind != NodeKind::Phi || depth > 3)
            return false;
        if (!m_phisAssumed.add(value).isNewEntry)
            return true;
        for (unsigned i = 0; i < value->uses.size(); ++i) {
            Node* incoming = origin(value->uses[i].node);
            BasicBlock* predecessor = value->block->predecessors[i];
            if (incoming == value || holdsOnEdge(predecessor, value->block, incoming, base))
                continue;
            if (!isBelowLength(incoming, base, predecessor, depth + 1))
                return false;
        }
        return true;
    }

    Graph& m_graph;
    UncheckedKeyHashSet<Node*> m_phisAssumed;
};

} // anonymous namespace

bool Graph::hasFixedLength(const Node* base)
{
    return Facts::shared() && Facts::shared()->buffersAreFixed() && base->type && isSubtype(base->type, TTypedArray);
}

bool proveIndexesInBounds(Graph& graph)
{
    return BoundsProver(graph).run();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
