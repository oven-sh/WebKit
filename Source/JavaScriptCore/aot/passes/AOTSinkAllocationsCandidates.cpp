/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool namesField(const Program& program, const Value& value)
{
    switch (interpretationOf(program, value)) {
    case Interpretation::Read:
    case Interpretation::Store:
    case Interpretation::Test:
        return true;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void ensureFieldsInUse(const VirtualObjectContext& context, const EscapeAnalysis& escape, const VirtualObjectDescription& description, VirtualFamily& family, uint64_t maxFields)
{
    auto hasTooManyFields = [&] {
        return family.numberOfFields() > maxFields;
    };

    forEachInitialField(context, description, [&](const VirtualField& field, ValueID) {
        if (!hasTooManyFields())
            family.ensureField(field);
    });

    std::optional<ObjectID> object = escape.objectDefinedBy(description.allocation);
    if (!object)
        return;
    for (const ObjectUse& objectUse : escape.usesOf(*object)) {
        if (hasTooManyFields())
            return;
        if (objectUse.use.kind != Use::Kind::Operand)
            continue;
        const Value& user = context.function.at(objectUse.use.user);
        if (user.opcode == Opcode::DestructureIterable && description.kind == VirtualObjectKind::BuiltinPrefix) {
            for (uint32_t position = 0; position < user.count() && !hasTooManyFields(); ++position)
                family.ensureField(VirtualField::element(position));
            continue;
        }
        if (!namesField(context.program, user))
            continue;
        VirtualFieldAccess access = lookUpField(context, description, user);
        if (access.lookup == FieldLookup::Field)
            family.ensureField(access.field);
    }
}

// One value carries a field for every variant of a join. After a test has left one variant, that value is read, stored and
// passed on at the type the variant's own description gives the field, and no cast states what the tag proves.
static bool givesOneFieldSeveralTypes(const VirtualObjectContext& context, const VirtualFamily& family)
{
    std::span<const VirtualObjectDescription> descriptions = family.descriptions();
    if (descriptions.size() < 2)
        return false;

    for (uint32_t field = 0; field < family.numberOfFields(); ++field) {
        TypeID commonType;
        for (const VirtualObjectDescription& description : descriptions) {
            TypeID type = typeOfField(context, description, family.fieldAt(field));
            if (type == TypeTable::never)
                continue;
            if (commonType && commonType != type)
                return true;
            commonType = type;
        }
    }
    return false;
}

bool AllocationSinking::collectCandidates()
{
    Vector<DescribedObject> described = collectAllocations();
    if (described.isEmpty())
        return false;

    collectJoins();
    Vector<uint32_t> rootOfCandidate;
    uniteFamilies(rootOfCandidate);
    createFamilies(described.span(), rootOfCandidate.span());
    computeInputs();
    return true;
}

Vector<DescribedObject> AllocationSinking::collectAllocations()
{
    Vector<DescribedObject> result;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (interpretedOf(m_function.at(id).opcode) != Interpretation::Allocation)
                continue;

            DescribedObject described = describeVirtualObject(m_analyses, id);
            switch (described.support) {
            case VirtualObjectSupport::ExceedsLimit:
                if (m_policy.recordsLosses)
                    context().build.gaveUp(described.limit, m_analyses.subject(id));
                continue;
            case VirtualObjectSupport::Unknown:
            case VirtualObjectSupport::NotAnAllocation:
            case VirtualObjectSupport::NotFresh:
            case VirtualObjectSupport::HasEffectsAtAllocation:
            case VirtualObjectSupport::HasChosenStorage:
            case VirtualObjectSupport::HasNoPartsForm:
                continue;
            case VirtualObjectSupport::Supported:
                break;
            }

            m_candidateOfValue[id.index()] = VirtualObjectID(m_candidates.size());
            m_candidates.append(Candidate { .definition = id, .block = block });
            result.append(WTF::move(described));
        }
    }
    return result;
}

bool AllocationSinking::isJoinParameter(BlockID block, uint32_t parameterIndex) const
{
    const EscapeAnalysis& escape = m_analyses.escape();
    ValueID parameter = m_function.at(block).parameters[parameterIndex];
    // The guards of an object point to it, and none of them is the object.
    if (m_function.at(parameter).type == TypeTable::guard)
        return false;
    if (parameter == m_function.at(block).subject)
        return false;

    bool mayHoldCandidate = false;
    PointsToSet set = escape.pointsTo(parameter);
    for (ObjectID object : set.objects)
        mayHoldCandidate |= !!candidateDefinedBy(escape.at(object).definition);
    if (!mayHoldCandidate)
        return false;

    std::optional<ObjectID> aliasedObject;
    bool isAliasOfOneObject = true;
    for (Edge edge : m_analyses.cfg().incomingEdges(block)) {
        ValueID incoming = valuePassedOn(m_function, edge, parameterIndex);
        if (!incoming)
            return false;
        if (incoming == parameter)
            continue;

        std::optional<ObjectID> object = escape.singleObject(incoming);
        if (!object || (aliasedObject && aliasedObject != object))
            isAliasOfOneObject = false;
        aliasedObject = object;
    }
    return !isAliasOfOneObject;
}

void AllocationSinking::collectJoins()
{
    if (m_analyses.escape().gaveUp())
        return;

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        if (block == entryBlock)
            continue;
        uint32_t numberOfParameters = m_function.at(block).parameters.size();
        for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
            if (!isJoinParameter(block, parameterIndex))
                continue;
            ValueID parameter = m_function.at(block).parameters[parameterIndex];
            m_candidateOfValue[parameter.index()] = VirtualObjectID(m_candidates.size());
            m_candidates.append(Candidate { .definition = parameter, .block = block, .isJoin = true });
        }
    }
}

void AllocationSinking::uniteFamilies(Vector<uint32_t>& parentOfCandidate)
{
    uint32_t numberOfCandidates = m_candidates.size();
    parentOfCandidate = Vector<uint32_t>(numberOfCandidates, [](size_t index) {
        return static_cast<uint32_t>(index);
    });

    auto find = [&](uint32_t index) {
        while (parentOfCandidate[index] != index) {
            parentOfCandidate[index] = parentOfCandidate[parentOfCandidate[index]];
            index = parentOfCandidate[index];
        }
        return index;
    };
    auto unite = [&](uint32_t a, uint32_t b) {
        uint32_t rootOfA = find(a);
        uint32_t rootOfB = find(b);
        if (rootOfA == rootOfB)
            return;
        if (rootOfA > rootOfB)
            std::swap(rootOfA, rootOfB);
        parentOfCandidate[rootOfB] = rootOfA;
    };

    const EscapeAnalysis& escape = m_analyses.escape();
    for (uint32_t join = 0; join < numberOfCandidates; ++join) {
        if (!m_candidates[join].isJoin)
            continue;
        PointsToSet set = escape.pointsTo(m_candidates[join].definition);
        for (ObjectID object : set.objects) {
            if (VirtualObjectID source = candidateDefinedBy(escape.at(object).definition))
                unite(join, source.index());
        }
    }

    for (uint32_t index = 0; index < numberOfCandidates; ++index)
        parentOfCandidate[index] = find(index);
}

void AllocationSinking::createFamilies(std::span<const DescribedObject> described, std::span<const uint32_t> rootOfCandidate)
{
    uint32_t numberOfCandidates = m_candidates.size();
    for (uint32_t index = 0; index < numberOfCandidates; ++index) {
        uint32_t root = rootOfCandidate[index];
        RELEASE_ASSERT(root <= index);
        if (root == index) {
            m_candidates[index].family = VirtualFamilyID(m_families.size());
            m_families.append(VirtualFamily());
            continue;
        }
        m_candidates[index].family = m_candidates[root].family;
    }

    const EscapeAnalysis& escape = m_analyses.escape();
    uint64_t maxFields = limit(LimitID::MaxVirtualObjectFields);
    for (uint32_t index = 0; index < described.size(); ++index) {
        Candidate& candidate = m_candidates[index];
        RELEASE_ASSERT(!candidate.isJoin);
        const VirtualObjectDescription& description = described[index].description;
        VirtualFamily& family = m_families[candidate.family.index()];
        candidate.descriptionIndex = family.addDescription(description);
        ensureFieldsInUse(m_objectContext, escape, description, family, maxFields);
    }

    BitVector familiesWithTooManyFields(m_families.size());
    BitVector familiesWithoutJoins(m_families.size());
    for (uint32_t index = 0; index < numberOfCandidates; ++index) {
        if (rootOfCandidate[index] != index)
            continue;
        VirtualFamilyID family = m_candidates[index].family;
        if (m_families[family.index()].numberOfFields() > maxFields)
            familiesWithTooManyFields.set(family.index());
        else if (givesOneFieldSeveralTypes(m_objectContext, m_families[family.index()]))
            familiesWithoutJoins.set(family.index());
    }
    for (uint32_t index = 0; index < numberOfCandidates; ++index) {
        const Candidate& candidate = m_candidates[index];
        if (familiesWithTooManyFields.get(candidate.family.index()))
            abandon(VirtualObjectID(index), LimitID::MaxVirtualObjectFields);
        else if (candidate.isJoin && familiesWithoutJoins.get(candidate.family.index()))
            abandon(VirtualObjectID(index));
    }

    for (uint32_t index = 0; index < described.size(); ++index) {
        Candidate& candidate = m_candidates[index];
        if (candidate.isAbandoned || !described[index].description.absentResult)
            continue;
        candidate.tagBitWhenAbsent = m_families[candidate.family.index()].reserveConsecutiveTagBits(2, limit(LimitID::MaxVirtualObjectVariants));
        if (!candidate.tagBitWhenAbsent)
            abandon(VirtualObjectID(index), LimitID::MaxVirtualObjectVariants);
    }
}

void AllocationSinking::computeInputs()
{
    size_t numberOfValues = m_function.values.size();
    size_t numberOfBlocks = m_function.blocks.size();
    m_inputs.throwingValues.ensureSize(numberOfValues);
    m_inputs.valuesWritingObjects.ensureSize(numberOfValues);
    m_inputs.throwingTerminators.ensureSize(numberOfBlocks);
    m_inputs.conditionsOnlyBranchedOn.ensureSize(numberOfValues);
    m_inputs.blocksInCycles.ensureSize(numberOfBlocks);
    m_inputs.mergeBlocks.ensureSize(numberOfBlocks);
    m_inputs.blocksWithCandidates.ensureSize(numberOfBlocks);

    const CFG& cfg = m_analyses.cfg();
    const BlockCycles& cycles = m_analyses.blockCycles();
    for (BlockID block : cfg.reversePostOrder()) {
        if (cycles.isInCycle(block))
            m_inputs.blocksInCycles.set(block.index());
        if (cfg.incomingEdges(block).size() > 1)
            m_inputs.mergeBlocks.set(block.index());
        if (m_analyses.effectsOfTerminator(block).mayThrow())
            m_inputs.throwingTerminators.set(block.index());

        for (ValueID id : m_function.at(block).values) {
            const EffectsOnEdges& effects = m_analyses.effectsOf(id);
            if (effects.mayThrow())
                m_inputs.throwingValues.set(id.index());
            if (effects.onAnyEdge().writes.containsAny(perObjectHeaps) || effects.normal.mayRunOtherCode())
                m_inputs.valuesWritingObjects.set(id.index());
        }

        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        ValueID condition = terminator.conditionOperand();
        bool onlyReads = !m_inputs.throwingValues.get(condition.index()) && !m_inputs.valuesWritingObjects.get(condition.index());
        if (onlyReads && branchesOf(m_function.at(condition).opcode) != Branches::No && m_analyses.useLists().useCount(condition) == 1)
            m_inputs.conditionsOnlyBranchedOn.set(condition.index());
    }

    for (const Candidate& candidate : m_candidates) {
        if (!candidate.isJoin)
            m_inputs.blocksWithCandidates.set(candidate.block.index());
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
