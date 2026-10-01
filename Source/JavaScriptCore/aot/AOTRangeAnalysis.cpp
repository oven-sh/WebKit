/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(FTL_JIT)

#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

namespace {

// Determines which numbers are integers, and their ranges (see IntegerRange). JavaScript has one number type, but most numbers in a
// program are counters or indices. Those are kept in integer registers when that is proven to make no observable difference.
// Nothing here is speculative.
//
// A value that changes in a loop is bounded by the loop's exit test, which is looked for on the edges that lead to a phi. Without
// such a test, or if the bound is unknown, incrementing still cannot get past 2^53. That is where a double stops counting too (2^53
// + 1 is 2^53), and the lowering of op_inc saturates in the same way.
class RangeAnalysis {
public:
    RangeAnalysis(Graph& graph)
        : m_graph(graph)
    {
    }

    void run()
    {
        // Ascend from the bottom of the lattice to a fixpoint, widening where a phi keeps growing.
        bool changed = true;
        while (changed) {
            changed = false;
            for (BasicBlock* block : m_graph.m_rpo) {
                for (Node* phi : block->phis)
                    changed |= grow(phi, true);
                for (Node* node : block->nodes)
                    changed |= grow(node, false);
            }
            changed |= std::exchange(m_elementsChanged, false);
        }
        // Recomputing from a sound solution gives a sound solution, which is often narrower.
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (BasicBlock* block : m_graph.m_rpo) {
                for (Node* phi : block->phis)
                    phi->range = compute(phi);
                for (Node* node : block->nodes)
                    node->range = compute(node);
            }
        }
        // So far, the values that re-enter the fast copy of a loop from the generic copy have been ignored. They are only admitted
        // if they are within the ranges of the loop's own values. Had they been included, a range that had once been widened too
        // far would justify itself.
        m_reentering = true;
        for (BasicBlock* block : m_graph.m_rpo) {
            if (!block->isReentry)
                continue;
            for (Node* node : block->nodes)
                node->range = compute(node);
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            if (!block->isPreHeader)
                continue;
            for (Node* phi : block->phis)
                phi->range = compute(phi);
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis)
                settle(phi);
            for (Node* node : block->nodes)
                settle(node);
        }
    }

private:
    using Range = IntegerRange;
    static constexpr int64_t limit = Range::limit;

    static void settle(Node* node)
    {
        // The node is never reached. It is compiled anyway, according to its type.
        if (node->range.isNone())
            node->range = Range::unknown();
    }

    static int64_t widenUp(int64_t value)
    {
        if (value <= INT32_MAX)
            return INT32_MAX;
        if (value <= UINT32_MAX)
            return UINT32_MAX;
        return limit;
    }

    static int64_t widenDown(int64_t value)
    {
        if (value >= 0)
            return 0;
        if (value >= INT32_MIN)
            return INT32_MIN;
        return -limit;
    }

    bool grow(Node* node, bool isPhi)
    {
        Range old = node->range;
        Range result = old.unionWith(compute(node));
        if (result == old)
            return false;
        if (isPhi && result.isKnown() && old.isKnown() && ++node->rangeUpdates > 2) {
            if (result.max > old.max)
                result.max = widenUp(result.max);
            if (result.min < old.min)
                result.min = widenDown(result.min);
        }
        node->range = result;
        return true;
    }

    // The range implied by the type of a value that is known to be a number.
    static Range byType(Node* node)
    {
        if (node->type && !mayBe(node->type, TDouble))
            return Range::of(INT32_MIN, INT32_MAX);
        return Range::unknown();
    }

    static Range bounded(int64_t min, int64_t max)
    {
        if (min < -limit || max > limit)
            return Range::unknown();
        return Range::of(min, max);
    }

    enum class Relation : uint8_t { Less, LessEq, Greater, GreaterEq };
    static Relation negated(Relation relation)
    {
        switch (relation) {
        case Relation::Less: return Relation::GreaterEq;
        case Relation::LessEq: return Relation::Greater;
        case Relation::Greater: return Relation::LessEq;
        case Relation::GreaterEq: return Relation::Less;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    static Relation mirrored(Relation relation)
    {
        switch (relation) {
        case Relation::Less: return Relation::Greater;
        case Relation::LessEq: return Relation::GreaterEq;
        case Relation::Greater: return Relation::Less;
        case Relation::GreaterEq: return Relation::LessEq;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The range of the value on the edge from one block to the other, refined by the branch condition.
    Range onEdge(Node* value, BasicBlock* from, BasicBlock* to)
    {
        Range range = value->range;
        if (!range.isKnown() || !isSubtype(value->type, TNumber))
            return range;
        Node* branch = from->terminal();
        if (!branch || branch->kind != NodeKind::Bytecode || from->successors.size() != 2 || from->successors[0] == from->successors[1])
            return range;

        VirtualRegister lhs;
        VirtualRegister rhs;
        Relation whenTaken;
        switch (branch->opcode) {
#define AOT_CASE(Struct, opcodeName, relation) \
        case opcodeName: { \
            auto bytecode = branch->as<Struct>(); \
            lhs = bytecode.m_lhs; \
            rhs = bytecode.m_rhs; \
            whenTaken = relation; \
            break; \
        }
        AOT_CASE(OpJless, op_jless, Relation::Less)
        AOT_CASE(OpJlesseq, op_jlesseq, Relation::LessEq)
        AOT_CASE(OpJgreater, op_jgreater, Relation::Greater)
        AOT_CASE(OpJgreatereq, op_jgreatereq, Relation::GreaterEq)
        AOT_CASE(OpJnless, op_jnless, Relation::GreaterEq)
        AOT_CASE(OpJnlesseq, op_jnlesseq, Relation::Greater)
        AOT_CASE(OpJngreater, op_jngreater, Relation::LessEq)
        AOT_CASE(OpJngreatereq, op_jngreatereq, Relation::Less)
#undef AOT_CASE
        default:
            return range;
        }
        Node* left = branch->use(lhs);
        Node* right = branch->use(rhs);
        if (left == right || (value != left && value != right))
            return range;
        // Both operands are integers, so if a relation does not hold, its negation does.
        Node* other = value == left ? right : left;
        if (!other->range.isKnown() || !isSubtype(other->type, TNumber))
            return range;
        Relation relation = to == from->successors[0] ? whenTaken : negated(whenTaken);
        if (value == right)
            relation = mirrored(relation);
        switch (relation) {
        case Relation::Less:
            range.max = std::min(range.max, other->range.max - 1);
            break;
        case Relation::LessEq:
            range.max = std::min(range.max, other->range.max);
            break;
        case Relation::Greater:
            range.min = std::max(range.min, other->range.min + 1);
            break;
        case Relation::GreaterEq:
            range.min = std::max(range.min, other->range.min);
            break;
        }
        // The edge is never taken.
        if (range.min > range.max)
            return Range::none();
        return range;
    }

    // The range of a value that may not be a number applies in case it is one. A later check establishes that.
    Range compute(Node* node)
    {
        if (node->type && !mayBe(node->type, TNumber))
            return Range::none();
        switch (node->kind) {
        case NodeKind::Constant:
            return node->range;
        case NodeKind::Phi: {
            Range result = Range::none();
            for (unsigned i = 0; i < node->uses.size(); ++i)
                result = result.unionWith(onEdge(node->uses[i].node, node->block->predecessors[i], node->block));
            return result;
        }
        case NodeKind::Proj: {
            Node* parent = node->uses[0].node;
            if (parent->opcode == op_enumerator_next && node->reg == parent->as<OpEnumeratorNext>().m_index)
                return Range::of(0, UINT32_MAX);
            return byType(node);
        }
        case NodeKind::Narrow: {
            // The reentry test ensures that the value is within the range of the loop's own values.
            if (!m_reentering && node->target)
                return Range::none();
            Range range = node->uses[0].node->range;
            if (!node->target || !node->target->range.isKnown())
                return node->target && node->target->range.isNone() ? Range::none() : range;
            Range wanted = node->target->range;
            if (!range.isKnown())
                return range.isNone() ? range : wanted;
            if (range.max < wanted.min || range.min > wanted.max)
                return Range::none();
            return Range::of(std::max(range.min, wanted.min), std::min(range.max, wanted.max));
        }
        case NodeKind::Bytecode:
            return computeBytecode(node);
        default:
            return byType(node);
        }
    }

    Range computeBytecode(Node* node)
    {
        // Whatever the generic copy of a loop stores, the fast copy stores too, with more precise ranges.
        if (auto [array, element] = m_graph.arrayAndElementStored(node); array && Graph::isLocallyAllocatedArray(array) && !node->block->isGeneric)
            noteElement(array, element);

        // For arithmetic operands. Arithmetic converts a value that is not a number, so its range says nothing.
        auto rangeOf = [&](VirtualRegister reg) {
            Node* operand = node->use(reg);
            if (operand->type && !isSubtype(operand->type, TNumber))
                return Range::unknown();
            return operand->range;
        };
        // An unreached operand makes the result unreached, and an unknown operand makes it unknown.
#define AOT_OPERANDS(a, b) \
        if (a.isNone() || b.isNone()) \
            return Range::none(); \
        if (!a.isKnown() || !b.isKnown()) \
            return byType(node);

        switch (node->opcode) {
        case op_add: {
            auto bytecode = node->as<OpAdd>();
            Range a = rangeOf(bytecode.m_lhs);
            Range b = rangeOf(bytecode.m_rhs);
            AOT_OPERANDS(a, b)
            return bounded(a.min + b.min, a.max + b.max);
        }
        case op_sub: {
            auto bytecode = node->as<OpSub>();
            Range a = rangeOf(bytecode.m_lhs);
            Range b = rangeOf(bytecode.m_rhs);
            AOT_OPERANDS(a, b)
            return bounded(a.min - b.max, a.max - b.min);
        }
        case op_mul: {
            auto bytecode = node->as<OpMul>();
            Range a = rangeOf(bytecode.m_lhs);
            Range b = rangeOf(bytecode.m_rhs);
            AOT_OPERANDS(a, b)
            // Zero times a negative number is negative zero, which is not an integer here.
            if ((a.min < 0 || b.min < 0) && (a.contains(0) || b.contains(0)))
                return Range::unknown();
            int64_t min = INT64_MAX;
            int64_t max = INT64_MIN;
            for (int64_t x : { a.min, a.max }) {
                for (int64_t y : { b.min, b.max }) {
                    int64_t product;
                    if (__builtin_mul_overflow(x, y, &product))
                        return Range::unknown();
                    min = std::min(min, product);
                    max = std::max(max, product);
                }
            }
            return bounded(min, max);
        }
        case op_mod: {
            auto bytecode = node->as<OpMod>();
            Range a = rangeOf(bytecode.m_lhs);
            Range b = rangeOf(bytecode.m_rhs);
            AOT_OPERANDS(a, b)
            // The result has the sign of the dividend, which may give negative zero, and the remainder of a division by zero is
            // NaN.
            if (a.min < 0 || b.min <= 0)
                return Range::unknown();
            return Range::of(0, std::min(a.max, b.max - 1));
        }
        case op_inc: {
            Range a = rangeOf(node->as<OpInc>().m_srcDst);
            AOT_OPERANDS(a, a)
            return Range::of(std::min(a.min + 1, limit), std::min(a.max + 1, limit));
        }
        case op_dec: {
            Range a = rangeOf(node->as<OpDec>().m_srcDst);
            AOT_OPERANDS(a, a)
            return Range::of(std::max(a.min - 1, -limit), std::max(a.max - 1, -limit));
        }
        case op_negate: {
            Range a = rangeOf(node->as<OpNegate>().m_operand);
            AOT_OPERANDS(a, a)
            if (a.contains(0))
                return Range::unknown();
            return Range::of(-a.max, -a.min);
        }
        case op_to_number:
            return rangeOf(node->as<OpToNumber>().m_operand);
        case op_to_numeric:
            return rangeOf(node->as<OpToNumeric>().m_operand);
        case op_check_type:
            return node->use(node->as<OpCheckType>().m_value)->range;
        case op_bitand: {
            auto bytecode = node->as<OpBitand>();
            Range a = rangeOf(bytecode.m_lhs);
            Range b = rangeOf(bytecode.m_rhs);
            if (a.isNone() || b.isNone())
                return Range::none();
            // A bitwise AND with a non-negative mask is at most the mask.
            int64_t max = INT32_MAX;
            bool nonNegative = false;
            for (Range operand : { a, b }) {
                if (operand.isKnown() && operand.min >= 0 && operand.max <= INT32_MAX) {
                    max = std::min(max, operand.max);
                    nonNegative = true;
                }
            }
            return nonNegative ? Range::of(0, max) : byType(node);
        }
        case op_rshift: {
            auto bytecode = node->as<OpRshift>();
            Range a = rangeOf(bytecode.m_lhs);
            Range b = rangeOf(bytecode.m_rhs);
            AOT_OPERANDS(a, b)
            if (!a.fitsInt32() || b.min != b.max || b.min < 0 || b.min > 31)
                return byType(node);
            return Range::of(a.min >> b.min, a.max >> b.min);
        }
        case op_unsigned:
            return Range::of(0, UINT32_MAX);
        case op_get_by_val:
            {
                if (auto type = Graph::typedArrayAccessed(node)) {
                    switch (*type) {
                    case Int8ArrayType:
                        return Range::of(INT8_MIN, INT8_MAX);
                    case Uint8ArrayType:
                        return Range::of(0, UINT8_MAX);
                    case Int16ArrayType:
                        return Range::of(INT16_MIN, INT16_MAX);
                    case Uint16ArrayType:
                        return Range::of(0, UINT16_MAX);
                    case Uint32ArrayType:
                        return Range::of(0, UINT32_MAX);
                    case Int32ArrayType:
                        return Range::of(INT32_MIN, INT32_MAX);
                    default:
                        break;
                    }
                }
                // An array that the function only stores integers in probably has Int32 storage. In the fast copy of a loop, the
                // guard only passes elements from that kind of storage.
                Node* array = node->use(node->as<OpGetByVal>().m_base);
                if (node->guard && isSubtype(node->type, TNumber) && Graph::isLocallyAllocatedArray(array)) {
                    if (array->opcode == op_new_array) {
                        for (auto& use : array->uses)
                            noteElement(array, use.node);
                    }
                    return m_elements.get(array);
                }
            }
            return byType(node);
        case op_argument_count:
            return Range::of(0, INT32_MAX);
        case op_call: {
            if (node->guard && m_graph.intrinsicOfCall(node) == CallIntrinsic::StringCharCodeAt)
                return Range::of(0, UINT16_MAX);
            // @toLength() of an integer is the integer, or zero if it is negative.
            auto bytecode = node->as<OpCall>();
            if (bytecode.m_argc != 2 || Graph::linkTimeConstantOf(node->use(bytecode.m_callee)) != LinkTimeConstant::toLength)
                return byType(node);
            Range argument = rangeOf(VirtualRegister(-static_cast<int>(bytecode.m_argv) + CallFrame::thisArgumentOffset() + 1));
            if (argument.isNone())
                return Range::none();
            if (!argument.isKnown())
                return byType(node);
            return Range::of(std::max<int64_t>(argument.min, 0), std::max<int64_t>(argument.max, 0));
        }
        case op_get_length: {
            // In the fast copy of a loop, the guard only passes an int32 length.
            Type base = node->use(node->as<OpGetLength>().m_base)->type;
            if (node->guard || (base && isSubtype(base, TString)))
                return Range::of(0, INT32_MAX);
            if (base && isSubtype(base, TString | TArray))
                return Range::of(0, UINT32_MAX);
            if (base && isSubtype(base, TString | TArray | TTypedArray))
                return Range::of(0, limit);
            return Range::unknown();
        }
        default:
            return byType(node);
        }
#undef AOT_OPERANDS
    }

    void noteElement(Node* array, Node* element)
    {
        Range& elements = m_elements.add(array, Range::none()).iterator->value;
        Range range = element->range;
        if (element->type && !isSubtype(element->type, TNumber))
            range = Range::unknown();
        Range result = elements.unionWith(range.isKnown() ? Range::of(INT32_MIN, INT32_MAX) : range);
        if (result != elements) {
            elements = result;
            m_elementsChanged = true;
        }
    }

    Graph& m_graph;
    // For each array that the function creates: none if no store to it has been seen, and the whole int32 range if only integers
    // are stored.
    UncheckedKeyHashMap<Node*, Range> m_elements;
    bool m_elementsChanged { false };
    bool m_reentering { false };
};

} // anonymous namespace

void inferRanges(Graph& graph)
{
    RangeAnalysis analysis(graph);
    analysis.run();
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
