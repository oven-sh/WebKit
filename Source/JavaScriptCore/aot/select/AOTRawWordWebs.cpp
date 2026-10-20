/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRawWordWebs.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include <algorithm>
#include <utility>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

RawWordWebs::RawWordWebs(const Program& program, const Function& function, FunctionAnalyses& analyses)
    : m_program(program)
    , m_function(function)
    , m_analyses(analyses)
{
    const UseLists& useLists = analyses.useLists();
    for (size_t index = 0; index < function.values.size(); ++index) {
        ValueID id(index);
        ValueLocation location = useLists.locationOf(id);
        if (location.placement == ValuePlacement::Detached)
            continue;

        std::optional<OpaqueKind> kind = program.typeOf(function, id).opaque;
        if (!kind)
            continue;
        if (isRawWord(*kind))
            m_rawWords.set(index);
        if (!endsAtSuspension(*kind))
            continue;
        m_trackedValues.set(index);
        m_blocksDefiningTrackedValues.set(location.block.index());
    }
}

std::optional<OpaqueKind> RawWordWebs::rawKindOf(ValueID id) const
{
    if (!m_rawWords.get(id.index()))
        return std::nullopt;
    return m_program.typeOf(m_function, id).opaque;
}

std::optional<RawWordProduction> RawWordWebs::productionOf(ValueID id) const
{
    const Value& value = m_function.at(id);
    if (value.opcode == Opcode::ResultAt) {
        ValueID producer = value.resultsOperand();
        if (!Typed::baseOf(m_function.at(producer).opcode))
            return std::nullopt;
        return RawWordProduction { producer, value.index() };
    }
    if (!Typed::baseOf(value.opcode) || resultRuleOf(value.opcode) == ResultRule::Results)
        return std::nullopt;
    return RawWordProduction { id, 0 };
}

ProductionReach RawWordWebs::collectProductions(ValueID word, Vector<RawWordProduction, 2>& productions) const
{
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();
    size_t previousSize = productions.size();
    BitVector visited;
    Vector<ValueID, 8> worklist;
    auto visit = [&](ValueID id) {
        if (!visited.set(id.index()))
            worklist.append(id);
    };

    visit(word);
    while (!worklist.isEmpty()) {
        ValueID id = worklist.takeLast();
        if (std::optional<RawWordProduction> production = productionOf(id)) {
            productions.append(*production);
            continue;
        }

        const Value& value = m_function.at(id);
        if (isCast(value.opcode)) {
            visit(value.operand(0));
            continue;
        }

        ValueLocation location = useLists.locationOf(id);
        if (location.placement != ValuePlacement::Parameter || location.block == cfg.root()) {
            productions.shrink(previousSize);
            return ProductionReach::Unknown;
        }
        for (Edge edge : cfg.incomingEdges(location.block)) {
            IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
            if (incoming.kind == IncomingValue::Kind::EdgeResult) {
                productions.shrink(previousSize);
                return ProductionReach::Unknown;
            }
            visit(incoming.argument);
        }
    }

    std::ranges::sort(productions);
    removeRepeatedElements(productions);
    return ProductionReach::OnlyProductions;
}

ProductionIdentity RawWordWebs::compareProductions(ValueID word, ValueID otherWord) const
{
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> visited;
    Vector<std::pair<ValueID, ValueID>, 8> worklist;
    auto addPair = [&](ValueID first, ValueID second) {
        if (visited.add(static_cast<uint64_t>(first.index()) << 32 | second.index()).isNewEntry)
            worklist.append({ first, second });
    };

    addPair(word, otherWord);
    while (!worklist.isEmpty()) {
        auto [first, second] = worklist.takeLast();
        std::optional<RawWordProduction> firstProduction = productionOf(first);
        std::optional<RawWordProduction> secondProduction = productionOf(second);
        if (firstProduction && secondProduction) {
            if (firstProduction->producer != secondProduction->producer)
                return ProductionIdentity::Unknown;
            continue;
        }

        ValueLocation firstLocation = useLists.locationOf(first);
        ValueLocation secondLocation = useLists.locationOf(second);
        bool areParametersOfSameBlock = firstLocation.placement == ValuePlacement::Parameter
            && secondLocation.placement == ValuePlacement::Parameter
            && firstLocation.block == secondLocation.block;
        if (!areParametersOfSameBlock || firstLocation.block == cfg.root())
            return ProductionIdentity::Unknown;
        for (Edge edge : cfg.incomingEdges(firstLocation.block)) {
            IncomingValue firstIncoming = incomingValueOf(m_function, edge, firstLocation.index);
            IncomingValue secondIncoming = incomingValueOf(m_function, edge, secondLocation.index);
            if (firstIncoming.kind == IncomingValue::Kind::EdgeResult || secondIncoming.kind == IncomingValue::Kind::EdgeResult)
                return ProductionIdentity::Unknown;
            addPair(firstIncoming.argument, secondIncoming.argument);
        }
    }
    return ProductionIdentity::SameProduction;
}

ValueID RawWordWebs::canonicalBase(ValueID id) const
{
    return sameObjectThroughCasts(m_function, id);
}

ValueID RawWordWebs::baseOf(const RawWordProduction& production) const
{
    const Value& producer = m_function.at(production.producer);
    return canonicalBase(producer.operand(*Typed::baseOf(producer.opcode)));
}

bool RawWordWebs::isBaseOnEveryPath(ValueID base, ValueID pointer) const
{
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> visited;
    Vector<std::pair<ValueID, ValueID>, 8> worklist;
    auto addPair = [&](ValueID word, ValueID object) {
        object = canonicalBase(object);
        if (visited.add(static_cast<uint64_t>(word.index()) << 32 | object.index()).isNewEntry)
            worklist.append({ word, object });
    };

    addPair(pointer, base);
    while (!worklist.isEmpty()) {
        auto [word, object] = worklist.takeLast();
        if (std::optional<RawWordProduction> production = productionOf(word)) {
            if (baseOf(*production) != object)
                return false;
            continue;
        }
        if (isCast(m_function.at(word).opcode)) {
            addPair(m_function.at(word).operand(0), object);
            continue;
        }

        ValueLocation wordLocation = useLists.locationOf(word);
        if (wordLocation.placement != ValuePlacement::Parameter || wordLocation.block == cfg.root())
            return false;
        ValueLocation objectLocation = useLists.locationOf(object);
        bool mergesBesideWord = objectLocation.placement == ValuePlacement::Parameter && objectLocation.block == wordLocation.block;
        for (Edge edge : cfg.incomingEdges(wordLocation.block)) {
            IncomingValue incomingWord = incomingValueOf(m_function, edge, wordLocation.index);
            if (incomingWord.kind == IncomingValue::Kind::EdgeResult)
                return false;
            if (!mergesBesideWord) {
                addPair(incomingWord.argument, object);
                continue;
            }
            IncomingValue incomingObject = incomingValueOf(m_function, edge, objectLocation.index);
            if (incomingObject.kind == IncomingValue::Kind::EdgeResult)
                return false;
            addPair(incomingWord.argument, incomingObject.argument);
        }
    }
    return true;
}

Vector<EndedRawPointer> RawWordWebs::findEndedPointers() const
{
    Vector<EndedRawPointer> endedPointers;
    if (m_trackedValues.isEmpty())
        return endedPointers;

    const CFG& cfg = m_analyses.cfg();
    Liveness liveness(m_function, cfg, m_analyses.useLists(), { .trackedValues = &m_trackedValues });
    for (BlockID blockID : cfg.reversePostOrder()) {
        if (liveness.liveAtHead(blockID).empty() && !m_blocksDefiningTrackedValues.get(blockID.index()))
            continue;

        const Block& block = m_function.at(blockID);
        Liveness::LocalCalc liveAfterPoint(liveness, blockID);
        auto appendPointersEndedAt = [&](uint32_t index, ValueID definition, const auto& computeEffects) {
            std::optional<Effects> effects;
            liveAfterPoint.forEachLive([&](ValueID pointer) {
                if (pointer == definition)
                    return;
                if (!effects)
                    effects = computeEffects();
                if (effects->maySuspend)
                    endedPointers.append(EndedRawPointer { { blockID, index }, pointer });
            });
        };

        uint32_t numberOfValues = static_cast<uint32_t>(block.values.size());
        appendPointersEndedAt(numberOfValues, { }, [&] {
            return m_analyses.effectsOfTerminator(blockID).onAnyEdge();
        });
        for (uint32_t index = numberOfValues; index--;) {
            ValueID id = block.values[index];
            appendPointersEndedAt(index, id, [&] {
                return m_analyses.effectsOf(id).onAnyEdge();
            });
            liveAfterPoint.execute(index);
        }
    }
    return endedPointers;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
