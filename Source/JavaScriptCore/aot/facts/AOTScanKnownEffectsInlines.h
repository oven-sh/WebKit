/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Members of ScanOfBody, which AOTScan.cpp declares. That file includes this one at its foot.
// The scan leaves effects that Link can cut: what has a record under a written key is left out of ThinSummary::ownEffects, operators on operands that Link types lie apart,
// and each record says whether it lies on a way to a Return. The body is read once: behind it comes one go backward over its edges.

#if ENABLE(AOT)

#include "AOTLinkKnownEffects.h"
#include "AOTTypeRelations.h"

namespace JSC { namespace AOT { namespace Typed {

// A number, a boolean, undefined and null become numbers without memory, a string becomes one without a rule that could turn it away, and no operator makes anything else of them.
static constexpr uint16_t plainPrimitivesInScanKnownEffects = Primitive::NumberLike | Primitive::AnyString;

// As readAccess() finds it.
static PropertyKeyID keyWrittenAtInScanKnownEffects(const Program& program, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (PayloadWord::field(opcode) && value.layout() && value.field() < program.at(value.layout()).fields.size())
        return program.at(value.layout()).fields[value.field()].key;
    return PayloadWord::key(opcode) ? value.key() : PropertyKeyID();
}

void ScanOfBody::noteWritesIn(uint32_t block, const LocationMasks& writes)
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

// ThinSummary::layoutsOfOwnWrites. An allocation fills what it makes, and no caller knows anything of that.
void ScanOfBody::noteObjectsWrittenBy(const BodyValue& value, const Effects& effects)
{
    Opcode opcode = value.opcode();
    if (effects.writes.contains(AbstractHeap::EnvironmentFields)) {
        LayoutID layout = PayloadWord::layout(opcode) ? value.layout() : LayoutID();
        m_summary.layoutsOfOwnWrites |= layout ? bitOfLayoutInKnownEffects(m_program, layout) : objectsOfAnyLayout;
    }
    // Whether it leaves a record whose subject says whose object is written is known behind the body: finishKnownEffects().
    if (effects.writes.containsAny(heapsOfPartsOfObjects))
        m_knownEffects.sitesThatWriteIntoObjects.append(value.id());
}

// `(a, b) => a.rank - b.rank`. The bound that the row declares holds for a symbol and a BigInt among the operands too. Which primitives arrive only Link can tell.
const Effects& ScanOfBody::noteOwnEffectsOfOperator(const BodyValue& value, uint32_t block, const Effects& effects, Effects& storage)
{
    Opcode opcode = value.opcode();
    // `a === b`, `a == b`: neither turns a symbol or a BigInt away. What the conversion of an object runs and throws is counted with its calls, which stand in the summary by themselves.
    if (provesOf(opcode) == Proof::Equality) {
        noteWritesIn(block, effects.writes);
        if (!effects.mayThrow)
            return effects;
        if (&effects != &storage)
            storage = effects;
        storage.mayThrow = ThrowEffect::onlyForLackOfMemory();
        return storage;
    }

    TypeID widest = widestPrimitiveResultOf(opcode);
    uint32_t numberOfOperands = numberOfNamedOperandsOf(opcode);
    uint32_t numberOfOperandsAtHand = 0;
    value.forEachOperand([&](ValueID, uint32_t) {
        ++numberOfOperandsAtHand;
    });
    if (!widest || hasVariadicOperand(opcode) || effects.mayRunOtherCode() || numberOfOperandsAtHand != numberOfOperands) {
        // What it gives may be an operand of another operator, and nobody has looked at where it comes from.
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
    // `String(n)`: it makes a text whatever it is handed.
    if (!(given & Primitive::NumberLike))
        leaveApart(listOfNode(constant(TypeTable::string)));
    // `a + b`: the text may come out longer than a string can be.
    if ((given & Primitive::AnyString) && numberOfOperands > 1)
        m_summary.operatorsLeftApartMayJoinTexts = true;
    m_knownEffects.ofOperators.merge(effects);
    m_knownEffects.whatIsLeftApartMayThrow |= !!effects.mayThrow;
    return noEffectsInScan;
}

// It answers with what ThinSummary::ownEffects takes.
const Effects& ScanOfBody::noteOwnEffects(const BodyValue& value, uint32_t block, std::optional<SummaryAccess::Kind> access, const Effects& effects, Effects& storage)
{
    Opcode opcode = value.opcode();
    // `const w = box.width; bump(counter); return w + box.width`: the access leaves a record, or the call by name that stands for it does, and Link says what a lookup under that key does where something arrives at its subject.
    // A store that becomes an edge leaves none: noteStoreThatBecameEdge(). Under a computed key ownEffects stays the only word.
    if (access && declaredBoundInTable(opcode).mayCallUnknownCode && keyWrittenAtInScanKnownEffects(m_program, value)) {
        m_knownEffects.whatIsLeftApartMayThrow = true;
        return noEffectsInScan;
    }
    if (effectRuleOf(opcode) == EffectRule::Operator)
        return noteOwnEffectsOfOperator(value, block, effects, storage);
    // The record of an access names its subject.
    if (!access)
        noteObjectsWrittenBy(value, effects);
    noteWritesIn(block, effects.writes);
    return effects;
}

void ScanOfBody::noteEndOfBlock(const BodyTerminator& terminator, uint32_t block, const Effects& effects)
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

// `function make(a) { const o = {}; o.a = a; return o }`: the object was made by this very call and nothing else has it yet, so no caller knows anything of what is written.
// What is left is what the definition of a property does beside that.
void ScanOfBody::noteStoreThatBecameEdge(const StoreInScan& store)
{
    constexpr Effects ofDefinition = declaredBoundOf(Opcode::DefineGetter);
    Effects effects = declaredBoundOf(NamedEffectSet::Allocates);
    effects.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
    effects.reads.merge(ofDefinition.reads);
    effects.reads.add(AbstractHeapSet { AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards });
    effects.writes.merge(ofDefinition.writes.restrictedTo(heapsWhoseWritesNobodySees));
    effects.mayThrow = ThrowEffect(true);
    m_summary.ownEffects.merge(effects);
    noteWritesIn(store.block, effects.writes);
}

// The nodes that the operands of the operators come from, through the parameters of blocks. No node is made for it but the constant that stands for a text.
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
        // A value that comes from nowhere is one that this scan knows nothing of.
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
                // It is left apart itself. So it gives a plain primitive where its operands are plain primitives, and those are looked at on their own.
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
    // A list that grows by itself begins with room for sixteen.
    ThinSummary::KnownEffectsApart::Lists& lists = m_summary.knownEffectsApart.ensureLists();
    lists.operandsOfOperatorsLeftApart.reserveInitialCapacity(sources.size());
    lists.operandsOfOperatorsLeftApart.appendVector(sources);
    lists.ownEffectsOfOperators.reserveInitialCapacity(1);
    lists.ownEffectsOfOperators.append(known.ofOperators);
}

// `function set(o, v) { if (v < 0) { o.bad = v; throw new RangeError() } o.v = v }`: whoever gets an answer from set() finds `bad` as it was.
void ScanOfBody::tellWaysToReturnApart()
{
    KnownEffectsOfScan& known = m_knownEffects;
    // A coroutine comes back to its caller at each suspension.
    if (!known.someBlockEndsWithoutReturning || m_function->isCoroutine())
        return;

    known.leadsToReturn.fill(false, m_function->body.numberOfBlocks());
    for (uint32_t block : known.blocksThatReturn)
        known.leadsToReturn[block] = true;
    // Blocks stand in reverse postorder and the edges in the order of their sources, so whoever goes backward has judged the target of every edge that leads forward.
    // An edge that leads back counts as one that leads to a Return: nobody has judged the head of its loop yet.
    for (size_t place = known.edges.size(); place--;) {
        KnownEffectsOfScan::Edge edge = known.edges[place];
        if (edge.to <= edge.from || known.leadsToReturn[edge.to])
            known.leadsToReturn[edge.from] = true;
    }

    auto liesOnNoWayToReturn = [&](ValueID site) {
        return site && !known.leadsToReturn[m_blocks[site.index()]];
    };
    // The calls of a conversion are left once, at the first operation that converts the value: they stand for every later one too.
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
    Vector<LocationMasks>& onWaysToReturn = m_summary.knownEffectsApart.ensureLists().ownWritesOnWaysToReturn;
    onWaysToReturn.reserveInitialCapacity(1);
    onWaysToReturn.append(writes);
}

void ScanOfBody::finishKnownEffects()
{
    listOperandsOfOperatorsLeftApart();
    tellWaysToReturnApart();
    // What is left out of ownEffects may throw all the same. finish() says this of ownEffects.
    if (m_knownEffects.whatIsLeftApartMayThrow && !m_summary.ownEffects.mayThrow) {
        m_summary.traits.add(SummaryTrait::MayThrow);
        addEdge(0, nodeFor(SummaryNode::Kind::Thrown));
    }

    // An operation that has left neither a change nor a store writes in an object that nobody names: a class object, a prototype object.
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

// `const s = typeof a === "string" ? a : b.name; if (typeof s === "string") ..`: what has passed both tests is of both types. The scan runs beside others and makes no type,
// so it keeps the narrower where one lies within the other, and the newer where neither does. Where the two have nothing in common nothing passes.
OriginInScan ScanOfBody::narrowerOfTwoTests(OriginInScan origin, TypeID passes)
{
    if (!origin.passes || origin.passes == passes)
        return { origin.node, passes };
    TypeReader types(m_program);
    Type older = types.at(origin.passes);
    Type newer = types.at(passes);
    if (isSubset(types, older, newer))
        return origin;
    if (isSubset(types, newer, older) || overlap(types, older, newer))
        return { origin.node, passes };
    return { constant(TypeTable::never), { } };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
