/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRepresentationRules.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

using DecorationMark = B3::AbstractHeapRepository::DecorationMark;

static bool receivesUnspecifiedValue(const Function& function, const BitVector& unspecifiedValues, Edge edge, uint32_t parameterIndex)
{
    IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        return unspecifiedValues.get(incoming.argument.index());
    case IncomingValue::Kind::EdgeResult:
        break;
    }

    EdgeResult result = edgeResultAt(function, incoming.result);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return unspecifiedValues.get(result.subject.index());
    case EdgeResultKind::Guard:
    case EdgeResultKind::DeclaredResult:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A function that returns the parts of an object returns Unspecified values for the parts that the returned variant does not have.
static bool isUnspecifiedAtDefinition(const Function& function, const Value& value)
{
    if (value.opcode == Opcode::Unspecified)
        return true;
    return value.opcode == Opcode::ResultAt && effectRuleOf(function.at(value.resultsOperand()).opcode) == EffectRule::Call;
}

static bool returnsUnspecifiedOperand(const Function& function, const Value& value, const BitVector& unspecifiedValues)
{
    if (!hasRegister(value.representation))
        return false;
    bool returnsUnspecified = false;
    forEachReturnedOperand(function, value, [&](ValueID operand) {
        returnsUnspecified |= unspecifiedValues.get(operand.index());
    });
    return returnsUnspecified;
}

static BitVector collectUnspecifiedValues(const Function& function, const CFG& cfg)
{
    BitVector unspecifiedValues;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (isUnspecifiedAtDefinition(function, function.at(id)))
                unspecifiedValues.set(id.index());
        }
    }
    if (unspecifiedValues.isEmpty())
        return unspecifiedValues;

    bool changed = true;
    while (changed) {
        changed = false;
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (uint32_t parameterIndex = 0; parameterIndex < block.parameters.size(); ++parameterIndex) {
                for (Edge edge : cfg.incomingEdges(id)) {
                    if (receivesUnspecifiedValue(function, unspecifiedValues, edge, parameterIndex))
                        changed |= unspecifiedValues.add(block.parameters[parameterIndex].index());
                }
            }
            for (ValueID value : block.values) {
                if (returnsUnspecifiedOperand(function, function.at(value), unspecifiedValues))
                    changed |= unspecifiedValues.add(value.index());
            }
        }
        changed &= cfg.hasCycle();
    }
    return unspecifiedValues;
}

bool CodeGenerator::mayBeUnspecified(ValueID id)
{
    if (!m_unspecifiedValues)
        m_unspecifiedValues = collectUnspecifiedValues(function(), analyses().cfg());
    return m_unspecifiedValues->get(id.index());
}

// Decode declares the effects of creating a string constant, yet the decoded reference of a cell is plain arithmetic.
void CodeGenerator::checkInlineStores(const OperationContext& context, const DecorationMark& mark)
{
    struct StorePositions {
        unsigned begin;
        unsigned end;
    };

    DecorationMark end = m_heaps.markDecorations();
    auto forEachStoreSince = [&](const DecorationMark& begin, const auto& functor) {
        m_heaps.forEachWriteDecoratedSince(DecorationMark { begin.memory, end.cCallWrites, end.patchpointWrites }, functor);
    };
    auto numberOfStoresSince = [&](const DecorationMark& begin) {
        unsigned count = 0;
        forEachStoreSince(begin, [&](const B3::AbstractHeap&, B3::Value*) {
            ++count;
        });
        return count;
    };

    unsigned numberOfStores = numberOfStoresSince(mark);
    if (!numberOfStores)
        return;

    // A mark counts loads too, so a boundary is located by the number of stores that follow it.
    auto positionOf = [&](const DecorationMark& boundary) {
        return numberOfStores - std::min(numberOfStores, numberOfStoresSince(boundary));
    };
    Vector<StorePositions, 2> exemptPositions;
    for (const ExemptStores& exempt : m_exemptStores)
        exemptPositions.append({ positionOf(exempt.begin), positionOf(exempt.end) });

    LocationMasks declared = context.effects().onAnyEdge().writes;
    unsigned position = 0;
    forEachStoreSince(mark, [&](const B3::AbstractHeap& heap, B3::Value*) {
        unsigned current = position++;
        bool isExempt = exemptPositions.containsIf([&](const StorePositions& exempt) {
            return exempt.begin <= current && current < exempt.end;
        });
        if (!isExempt && !m_heaps.isCoveredBy(heap, declared))
            fail(context, InternalErrorKind::StoreOutsideDeclaredEffects);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
