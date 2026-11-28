/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkKnownEffects.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTTypeRelations.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t plainPrimitivesInScanKnownEffects = Primitive::NumberLike | Primitive::AnyString;

static PropertyKeyID keyWrittenAtInScanKnownEffects(const Program& program, const BodyValue& value, PropertyKeyID keyOfOperation)
{
    Opcode opcode = value.opcode();
    if (PayloadWord::field(opcode) && value.layout() && value.field() < program.at(value.layout()).fields.size())
        return program.at(value.layout()).fields[value.field()].key;
    return keyOfOperation;
}

ALWAYS_INLINE_INTO_OTHER_FILES void ScanOfBody::noteWritesIn(uint32_t block, const LocationMasks& writes)
{
    if (writes.isEmpty())
        return;
    m_knownEffects.writesNoted.merge(writes);
    Vector<KnownEffectsOfScan::WritesOfBlock>& blocks = m_knownEffects.writesOfBlocks;
    if (!blocks.isEmpty() && blocks.last().block == block) {
        blocks.last().writes.merge(writes);
        return;
    }
    blocks.append(KnownEffectsOfScan::WritesOfBlock { block, writes });
}

void ScanOfBody::noteObjectsWrittenBy(const BodyValue& value, const Effects& effects)
{
    Opcode opcode = value.opcode();
    if (effects.writes.contains(AbstractHeap::EnvironmentFields)) {
        LayoutID layout = PayloadWord::layout(opcode) ? value.layout() : LayoutID();
        m_summary.layoutsOfOwnWrites |= layout ? bitOfLayoutInKnownEffects(m_program, layout) : objectsOfAnyLayout;
    }
    if (effects.writes.containsAny(heapsOfPartsOfObjects))
        m_knownEffects.sitesThatWriteIntoObjects.append(value.id());
}

const Effects& ScanOfBody::noteOwnEffectsOfOperator(const BodyValue& value, uint32_t block, const Effects& effects, Effects& storage)
{
    Opcode opcode = value.opcode();
    if (provesOf(opcode) == Proof::Equality) {
        noteWritesIn(block, effects.writes);
        if (!effects.mayThrow)
            return effects;
        if (&effects != &storage)
            storage = effects;
        storage.mayThrow = ThrowEffect::onlyForLackOfMemory();
        return storage;
    }

    TypeID widest = m_widestPrimitiveResults[static_cast<unsigned>(opcode)];
    uint32_t numberOfOperands = numberOfNamedOperandsOf(opcode);
    uint32_t numberOfOperandsAtHand = 0;
    value.forEachOperand([&](ValueID, uint32_t) {
        ++numberOfOperandsAtHand;
    });
    if (!widest || hasVariadicOperand(opcode) || effects.mayRunOtherCode() || numberOfOperandsAtHand != numberOfOperands) {
        m_knownEffects.operatorsStayInOwnEffects = true;
        noteWritesIn(block, effects.writes);
        return effects;
    }

    auto leaveApart = [&](ListInScan list) {
        m_knownEffects.operandsOfOperators.append(KnownEffectsOfScan::Operand { list.first, list.count });
    };
    value.forEachOperand([&](ValueID operand, uint32_t) {
        leaveApart(listOf(operand));
    });
    uint16_t given = primitivesInID(widest);
    if (!(given & Primitive::NumberLike))
        leaveApart(listOfNode(constant(TypeTable::string)));
    if ((given & Primitive::AnyString) && numberOfOperands > 1)
        m_summary.operatorsLeftApartMayJoinTexts = true;
    m_knownEffects.ofOperators.merge(effects);
    m_knownEffects.whatIsLeftApartMayThrow |= !!effects.mayThrow;
    return noEffectsInScan;
}

ALWAYS_INLINE const Effects& ScanOfBody::noteOwnEffects(const BodyValue& value, uint32_t block, std::optional<SummaryAccess::Kind> access, const Effects& effects, Effects& storage)
{
    uint32_t traits = traitsOfOpcodeInScan(value.opcode());
    auto isJudgedByLink = [&] {
        return value.opcode() == Opcode::ElemGet || value.opcode() == Opcode::ElemSet || !!keyWrittenAtInScanKnownEffects(m_program, value, m_keyOfOperation);
    };
    if (access && (traits & RowOfOpcodeInScan::mayCallUnknownCode) && isJudgedByLink()) {
        m_knownEffects.whatIsLeftApartMayThrow = true;
        return noEffectsInScan;
    }
    if (traits & RowOfOpcodeInScan::isOperator)
        return noteOwnEffectsOfOperator(value, block, effects, storage);
    if (effects.writes.isEmpty())
        return effects;
    if (!access)
        noteObjectsWrittenBy(value, effects);
    noteWritesIn(block, effects.writes);
    return effects;
}

ALWAYS_INLINE_INTO_OTHER_FILES void ScanOfBody::noteEndOfBlock(const BodyTerminator& terminator, uint32_t block, const Effects& effects)
{
    noteWritesIn(block, effects.writes);
    TerminatorKind kind = terminator.kind();
    if (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall)
        m_knownEffects.blocksThatReturn.append(block);
    else if (!terminator.numberOfTargets())
        m_knownEffects.someBlockEndsWithoutReturning = true;
}

void ScanOfBody::noteEdgeBetweenBlocks(uint32_t from, uint32_t to, KindOfEdgeInScan kind)
{
    m_knownEffects.edges.append(KnownEffectsOfScan::Edge { from, to });
    if (kind == KindOfEdgeInScan::ToHandler)
        m_summary.someBlockHasHandler = true;
}

void ScanOfBody::noteStoreThatBecameEdge(const StoreInScan& store)
{
    constexpr Effects ofDefinition = declaredBoundOf(NamedEffectSet::DefinesProperty);
    Effects effects = declaredBoundOf(NamedEffectSet::Allocates);
    effects.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
    effects.reads.merge(ofDefinition.reads);
    effects.reads.add(AbstractHeapSet { AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards });
    effects.writes.merge(ofDefinition.writes.restrictedTo(unobservableWriteHeaps));
    effects.mayThrow = ThrowEffect(true);
    m_summary.ownEffects.merge(effects);
    noteWritesIn(store.block, effects.writes);
}

void ScanOfBody::listOperandsOfOperatorsLeftApart()
{
    KnownEffectsOfScan& known = m_knownEffects;
    if (known.operandsOfOperators.isEmpty())
        return;

    Vector<uint32_t>& sources = known.sourcesOfOperands;
    sources.shrink(0);
    known.parametersLookedAt.fill(false, m_numberOfParameters);
    known.parametersToLookAt.shrink(0);
    auto lookAt = [&](uint32_t first, uint32_t count) {
        known.operatorsStayInOwnEffects |= !count;
        for (uint32_t position = 0; position < count; ++position) {
            uint32_t atom = m_atoms[first + position];
            if (atom & parameterBit) {
                uint32_t parameter = atom & ~parameterBit;
                if (!std::exchange(known.parametersLookedAt[parameter], true))
                    known.parametersToLookAt.append(parameter);
                continue;
            }
            if (atom & operatorBit) {
                Opcode opcode = m_operators[atom & ~operatorBit].opcode;
                if (effectRuleOf(opcode) == EffectRule::Operator)
                    continue;
                TypeID widest = widestPrimitiveResultOf(opcode);
                if (!widest || (primitivesInID(widest) & ~plainPrimitivesInScanKnownEffects))
                    known.operatorsStayInOwnEffects = true;
                else if (primitivesInID(widest) & Primitive::AnyString)
                    sources.append(constant(TypeTable::string));
                continue;
            }
            if (TypeID written = primitiveConstantAt(atom)) {
                if (primitivesInID(written) & ~plainPrimitivesInScanKnownEffects)
                    known.operatorsStayInOwnEffects = true;
                else if (primitivesInID(written) & Primitive::AnyString)
                    sources.append(atom);
                continue;
            }
            known.operatorsStayInOwnEffects |= !atom;
            sources.append(atom);
        }
    };

    for (KnownEffectsOfScan::Operand operand : known.operandsOfOperators)
        lookAt(operand.first, operand.count);
    while (!known.parametersToLookAt.isEmpty() && !known.operatorsStayInOwnEffects) {
        uint32_t parameter = known.parametersToLookAt.takeLast();
        known.operatorsStayInOwnEffects |= m_firstIncoming[parameter] == m_firstIncoming[parameter + 1];
        for (uint32_t place = m_firstIncoming[parameter]; place < m_firstIncoming[parameter + 1]; ++place) {
            ListInScan list = m_incoming[m_incomingByParameter[place]].list;
            lookAt(list.first, list.count);
        }
    }

    if (known.operatorsStayInOwnEffects) {
        m_summary.operatorsLeftApartMayJoinTexts = false;
        m_summary.ownEffects.merge(known.ofOperators);
        known.writesOnEveryWay.merge(known.ofOperators.writes);
        known.writesNoted.merge(known.ofOperators.writes);
        return;
    }
    std::sort(sources.begin(), sources.end());
    sources.shrink(std::unique(sources.begin(), sources.end()) - sources.begin());
    m_summary.operandsOfOperatorsLeftApart.appendVector(sources);
    m_summary.ownEffectsOfOperators = known.ofOperators;
}

void ScanOfBody::tellWaysToReturnApart()
{
    KnownEffectsOfScan& known = m_knownEffects;
    if (!known.someBlockEndsWithoutReturning || m_function->isCoroutine())
        return;

    known.leadsToReturn.fill(false, m_function->body.numberOfBlocks());
    for (uint32_t block : known.blocksThatReturn)
        known.leadsToReturn[block] = true;
    for (size_t place = known.edges.size(); place--;) {
        KnownEffectsOfScan::Edge edge = known.edges[place];
        if (edge.to <= edge.from || known.leadsToReturn[edge.to])
            known.leadsToReturn[edge.from] = true;
    }

    auto liesOnNoWayToReturn = [&](ValueID site) {
        return site && !known.leadsToReturn[m_blocks[site.index()]];
    };
    for (SummaryCall& call : m_summary.calls)
        call.liesOnNoWayToReturn = !call.isMadeByProtocol && liesOnNoWayToReturn(call.site);
    for (SummaryAccess& access : m_summary.accesses)
        access.liesOnNoWayToReturn = liesOnNoWayToReturn(access.site);
    for (SummaryChange& change : m_summary.changes)
        change.liesOnNoWayToReturn = liesOnNoWayToReturn(change.site);

    if (m_summary.ownEffects.writes != known.writesNoted)
        return;
    LocationMasks writes = known.writesOnEveryWay;
    for (const KnownEffectsOfScan::WritesOfBlock& written : known.writesOfBlocks) {
        if (known.leadsToReturn[written.block])
            writes.merge(written.writes);
    }
    if (writes == m_summary.ownEffects.writes)
        return;
    m_summary.ownWritesOnWaysToReturn.append(writes);
}

void ScanOfBody::finishKnownEffects()
{
    listOperandsOfOperatorsLeftApart();
    tellWaysToReturnApart();
    if (m_knownEffects.whatIsLeftApartMayThrow && !m_summary.ownEffects.mayThrow) {
        m_summary.traits.add(SummaryTrait::MayThrow);
        addEdge(0, nodeFor(SummaryNode::Kind::Thrown));
    }

    KnownEffectsOfScan& known = m_knownEffects;
    if (known.sitesThatWriteIntoObjects.isEmpty())
        return;
    known.sitesOfChanges.shrink(0);
    for (const SummaryChange& change : m_summary.changes)
        known.sitesOfChanges.append(change.site);
    for (const SummaryAccess& access : m_summary.accesses) {
        if (access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define)
            known.sitesOfChanges.append(access.site);
    }
    std::sort(known.sitesOfChanges.begin(), known.sitesOfChanges.end());
    for (ValueID site : known.sitesThatWriteIntoObjects) {
        if (!std::binary_search(known.sitesOfChanges.begin(), known.sitesOfChanges.end(), site))
            m_summary.layoutsOfOwnWrites = objectsOfAnyLayout;
    }
}

OriginInScan ScanOfBody::narrowerOfTwoTests(OriginInScan origin, TypeID passes)
{
    if (!origin.passes || origin.passes == passes)
        return { origin.node, passes };
    TypeReader types(m_program);
    Type older = types.at(origin.passes);
    Type newer = types.at(passes);
    if (isSubset(types, older, newer))
        return origin;
    if (isSubset(types, newer, older))
        return { origin.node, passes };
    if (!overlap(types, older, newer))
        return { constant(TypeTable::never), { } };
    Type object = types.at(TypeTable::object);
    auto passesNoOtherObjects = [&](Type type) {
        type.primitives = 0;
        type.noteAnswersOfParts();
        return isSubset(types, type, object);
    };
    bool passesObjectsAlone = !(older.primitives & newer.primitives) && isSubset(types, object, older) && isSubset(types, object, newer) && (passesNoOtherObjects(older) || passesNoOtherObjects(newer));
    return { origin.node, passesObjectsAlone ? TypeTable::object : passes };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
