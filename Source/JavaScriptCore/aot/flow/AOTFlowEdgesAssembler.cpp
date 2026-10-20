/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdgesAssembler.h"

#if ENABLE(AOT)

#include "AOTGrouping.h"
#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <initializer_list>

namespace JSC { namespace AOT { namespace Typed {

template<typename T>
static FlowAdjacency<T> makeAdjacency(uint32_t numberOfLocals, std::span<const std::pair<FlowLocalID, T>> pairsSortedBySource)
{
    FlowAdjacency<T> adjacency;
    adjacency.offsets.reserveInitialCapacity(numberOfLocals + 1);
    adjacency.entries.reserveInitialCapacity(pairsSortedBySource.size());
    auto addOffsetsThrough = [&](uint32_t index) {
        while (adjacency.offsets.size() <= index)
            adjacency.offsets.append(static_cast<uint32_t>(adjacency.entries.size()));
    };
    for (auto& [source, entry] : pairsSortedBySource) {
        RELEASE_ASSERT(source.index() < numberOfLocals);
        addOffsetsThrough(source.index());
        adjacency.entries.append(entry);
    }
    addOffsetsThrough(numberOfLocals);
    return adjacency;
}

static void forEachLocalOf(FlowFilter& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.from);
    functor(statement.to);
}

static void forEachLocalOf(FlowConditionalSeed& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.from);
    FlowLocalID node = statement.seed.node();
    functor(node);
    statement.seed = statement.seed.withNode(node);
}

static void forEachLocalOf(FlowNaturalOperation& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.result);
}

static void forEachLocalOf(FlowAllocation& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.prototype);
}

static void forEachLocalOf(FlowLocationAccess& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.location);
    functor(statement.value);
}

static void forEachLocalOf(FlowFieldAccess& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.receiver);
    functor(statement.value);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowPropertyAccess& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.receiver);
    functor(statement.value);
    functor(statement.thisValue);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowElementAccess& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.receiver);
    functor(statement.key);
    functor(statement.value);
    functor(statement.thisValue);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowSlotAccess& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.receiver);
    functor(statement.value);
}

static void forEachLocalOf(FlowPrototypeAccess& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.object);
    functor(statement.value);
}

static void forEachLocalOf(FlowPropertyCopy& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.source);
    functor(statement.target);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowCall& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.callee);
    functor(statement.thisValue);
    functor(statement.newTarget);
    functor(statement.argumentList);
    functor(statement.result);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowDirectCall& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.thrown);
}

static void forEachLocalOf(FlowClassDefinition& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.constructor);
    functor(statement.heritage);
    functor(statement.prototypeParent);
}

static void forEachLocalOf(FlowMethodDefinition& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.classObject);
    functor(statement.function);
    functor(statement.computedKey);
}

static void forEachLocalOf(FlowAwait& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.value);
    functor(statement.result);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowIteration& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.iterable);
    functor(statement.state);
    functor(statement.sent);
    functor(statement.value);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowOperandUse& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.operand);
    functor(statement.retainer);
    functor(statement.result);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowModuleOperation& statement, NOESCAPE const Invocable<void(FlowLocalID&)> auto& functor)
{
    functor(statement.specifier);
    functor(statement.result);
    functor(statement.thrown);
}

static void forEachLocalOf(FlowUnseenCodeEvent&, NOESCAPE const Invocable<void(FlowLocalID&)> auto&)
{
}

static FlowLocalID storedValueOf(const auto& access)
{
    return isWrite(access.kind) && access.kind != FlowAccessKind::Delete ? access.value : FlowLocalID();
}

static FlowLocalID loadedValueOf(const auto& access)
{
    return isWrite(access.kind) ? FlowLocalID() : access.value;
}

static ValueID originOf(const auto& statement)
{
    if constexpr (requires { statement.origin; })
        return statement.origin;
    else
        return { };
}

namespace {

struct FlowLocalVisitor {
    const FlowStatements& statements;
    const ScopedLambda<void(FlowLocalID)>& functor;

    void visit(std::initializer_list<FlowLocalID> locals) const
    {
        for (FlowLocalID local : locals) {
            if (local)
                functor(local);
        }
    }

    void visitList(FlowLocalList list) const
    {
        for (FlowLocalID local : statements.list(list)) {
            if (local)
                functor(local);
        }
    }
};

} // anonymous namespace

void forEachLocalReadBy(const FlowStatements& statements, FlowStatementReference reference, NOESCAPE const ScopedLambda<void(FlowLocalID)>& functor)
{
    FlowLocalVisitor visitor { statements, functor };
    uint32_t index = reference.index;

    switch (reference.kind) {
    case FlowStatementKind::Filter:
        visitor.visit({ statements.filters[index].from });
        return;
    case FlowStatementKind::ConditionalSeed:
        visitor.visit({ statements.conditionalSeeds[index].from });
        return;
    case FlowStatementKind::NaturalOperation:
        visitor.visitList(statements.naturalOperations[index].operands);
        return;
    case FlowStatementKind::Allocation:
        visitor.visit({ statements.allocations[index].prototype });
        return;
    case FlowStatementKind::LocationAccess:
        visitor.visit({ storedValueOf(statements.locationAccesses[index]) });
        return;
    case FlowStatementKind::FieldAccess: {
        const FlowFieldAccess& access = statements.fieldAccesses[index];
        visitor.visit({ access.receiver, storedValueOf(access) });
        return;
    }
    case FlowStatementKind::PropertyAccess: {
        const FlowPropertyAccess& access = statements.propertyAccesses[index];
        visitor.visit({ access.receiver, access.thisValue, storedValueOf(access) });
        return;
    }
    case FlowStatementKind::ElementAccess: {
        const FlowElementAccess& access = statements.elementAccesses[index];
        visitor.visit({ access.receiver, access.thisValue, access.key, storedValueOf(access) });
        return;
    }
    case FlowStatementKind::SlotAccess: {
        const FlowSlotAccess& access = statements.slotAccesses[index];
        visitor.visit({ access.receiver, storedValueOf(access) });
        return;
    }
    case FlowStatementKind::PrototypeAccess: {
        const FlowPrototypeAccess& access = statements.prototypeAccesses[index];
        visitor.visit({ access.object, storedValueOf(access) });
        return;
    }
    case FlowStatementKind::PropertyCopy: {
        const FlowPropertyCopy& propertyCopy = statements.propertyCopies[index];
        visitor.visit({ propertyCopy.source, propertyCopy.target });
        visitor.visitList(propertyCopy.excludedKeys);
        return;
    }
    case FlowStatementKind::Call: {
        const FlowCall& call = statements.calls[index];
        visitor.visit({ call.callee, call.thisValue, call.newTarget });
        visitor.visitList(call.arguments);
        visitor.visit({ call.argumentList });
        return;
    }
    case FlowStatementKind::DirectCall:
        visitor.visitList(statements.directCalls[index].arguments);
        return;
    case FlowStatementKind::ClassDefinition: {
        const FlowClassDefinition& definition = statements.classDefinitions[index];
        visitor.visit({ definition.constructor, definition.heritage, definition.prototypeParent });
        return;
    }
    case FlowStatementKind::MethodDefinition: {
        const FlowMethodDefinition& definition = statements.methodDefinitions[index];
        visitor.visit({ definition.classObject, definition.function, definition.computedKey });
        return;
    }
    case FlowStatementKind::Await:
        visitor.visit({ statements.awaits[index].value });
        return;
    case FlowStatementKind::Iteration: {
        const FlowIteration& iteration = statements.iterations[index];
        visitor.visit({ iteration.iterable, iteration.state, iteration.sent });
        return;
    }
    case FlowStatementKind::OperandUse: {
        const FlowOperandUse& use = statements.operandUses[index];
        if (use.thrown)
            visitor.visit({ use.operand, use.retainer });
        return;
    }
    case FlowStatementKind::ModuleOperation:
        visitor.visit({ statements.moduleOperations[index].specifier });
        return;
    case FlowStatementKind::UnseenCodeEvent:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void forEachLocalPassedOnBy(const FlowStatements& statements, FlowStatementReference reference, NOESCAPE const ScopedLambda<void(FlowLocalID)>& functor)
{
    FlowLocalVisitor visitor { statements, functor };
    uint32_t index = reference.index;

    switch (reference.kind) {
    case FlowStatementKind::LocationAccess:
        visitor.visit({ storedValueOf(statements.locationAccesses[index]) });
        return;
    case FlowStatementKind::FieldAccess:
        visitor.visit({ storedValueOf(statements.fieldAccesses[index]) });
        return;
    case FlowStatementKind::PropertyAccess:
        visitor.visit({ storedValueOf(statements.propertyAccesses[index]) });
        return;
    case FlowStatementKind::ElementAccess:
        visitor.visit({ storedValueOf(statements.elementAccesses[index]) });
        return;
    case FlowStatementKind::SlotAccess:
        visitor.visit({ storedValueOf(statements.slotAccesses[index]) });
        return;
    case FlowStatementKind::PrototypeAccess:
        visitor.visit({ storedValueOf(statements.prototypeAccesses[index]) });
        return;
    case FlowStatementKind::PropertyCopy:
        visitor.visit({ statements.propertyCopies[index].source });
        return;
    case FlowStatementKind::Call: {
        const FlowCall& call = statements.calls[index];
        visitor.visit({ call.callee, call.thisValue, call.newTarget });
        visitor.visitList(call.arguments);
        visitor.visit({ call.argumentList });
        return;
    }
    case FlowStatementKind::DirectCall:
        visitor.visitList(statements.directCalls[index].arguments);
        return;
    case FlowStatementKind::ClassDefinition:
        visitor.visit({ statements.classDefinitions[index].heritage, statements.classDefinitions[index].prototypeParent });
        return;
    case FlowStatementKind::MethodDefinition:
        visitor.visit({ statements.methodDefinitions[index].function });
        return;
    case FlowStatementKind::Await:
        visitor.visit({ statements.awaits[index].value });
        return;
    case FlowStatementKind::Iteration:
        visitor.visit({ statements.iterations[index].iterable, statements.iterations[index].sent });
        return;
    case FlowStatementKind::OperandUse:
        if (statements.operandUses[index].thrown)
            visitor.visit({ statements.operandUses[index].operand });
        return;
    case FlowStatementKind::Filter:
    case FlowStatementKind::ConditionalSeed:
    case FlowStatementKind::NaturalOperation:
    case FlowStatementKind::Allocation:
    case FlowStatementKind::ModuleOperation:
    case FlowStatementKind::UnseenCodeEvent:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A run may also put something into the local of the statement's origin.
static void forEachLocalWrittenBy(const FlowStatements& statements, FlowStatementReference reference, NOESCAPE const ScopedLambda<void(FlowLocalID)>& functor)
{
    FlowLocalVisitor visitor { statements, functor };
    uint32_t index = reference.index;

    switch (reference.kind) {
    case FlowStatementKind::Filter:
        visitor.visit({ statements.filters[index].to });
        return;
    case FlowStatementKind::ConditionalSeed:
        visitor.visit({ statements.conditionalSeeds[index].seed.node() });
        return;
    case FlowStatementKind::NaturalOperation:
        visitor.visit({ statements.naturalOperations[index].result });
        return;
    case FlowStatementKind::FieldAccess: {
        const FlowFieldAccess& access = statements.fieldAccesses[index];
        visitor.visit({ loadedValueOf(access), access.thrown });
        return;
    }
    case FlowStatementKind::PropertyAccess: {
        const FlowPropertyAccess& access = statements.propertyAccesses[index];
        visitor.visit({ loadedValueOf(access), access.thrown });
        return;
    }
    case FlowStatementKind::ElementAccess: {
        const FlowElementAccess& access = statements.elementAccesses[index];
        visitor.visit({ loadedValueOf(access), access.thrown });
        return;
    }
    case FlowStatementKind::SlotAccess:
        visitor.visit({ loadedValueOf(statements.slotAccesses[index]) });
        return;
    case FlowStatementKind::PrototypeAccess:
        visitor.visit({ loadedValueOf(statements.prototypeAccesses[index]) });
        return;
    case FlowStatementKind::PropertyCopy:
        visitor.visit({ statements.propertyCopies[index].thrown });
        return;
    case FlowStatementKind::Call:
        visitor.visit({ statements.calls[index].result, statements.calls[index].thrown });
        return;
    case FlowStatementKind::DirectCall:
        visitor.visitList(statements.directCalls[index].results);
        visitor.visit({ statements.directCalls[index].thrown });
        return;
    case FlowStatementKind::Await:
        visitor.visit({ statements.awaits[index].result, statements.awaits[index].thrown });
        return;
    case FlowStatementKind::Iteration: {
        const FlowIteration& iteration = statements.iterations[index];
        visitor.visit({ iteration.state, iteration.value, iteration.thrown });
        return;
    }
    case FlowStatementKind::OperandUse:
        visitor.visit({ statements.operandUses[index].result, statements.operandUses[index].thrown });
        return;
    case FlowStatementKind::ModuleOperation:
        visitor.visit({ statements.moduleOperations[index].result, statements.moduleOperations[index].thrown });
        return;
    // A copy from the location brings what a load reads.
    case FlowStatementKind::LocationAccess:
    case FlowStatementKind::Allocation:
    case FlowStatementKind::ClassDefinition:
    case FlowStatementKind::MethodDefinition:
    case FlowStatementKind::UnseenCodeEvent:
        return;
    case FlowStatementKind::BuiltinCall:
    case FlowStatementKind::Lookup:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool passesParameterOn(const FlowSummary& summary, const BitVector& reachedLocals)
{
    for (uint32_t index = 0; index < summary.numberOfResults; ++index) {
        if (reachedLocals.get(summary.result(index).index()))
            return true;
    }

    for (size_t index : reachedLocals) {
        for (FlowLocalID successor : summary.successors.of(FlowLocalID(index))) {
            if (!summary.isExternal(successor))
                continue;
            const FlowExternalNode& external = summary.externalAt(successor);
            if (external.isPartOfSite() || external.kind() == FlowExternalKind::Variable)
                return true;
        }
    }

    bool passesReachedLocalOn = false;
    summary.statements.forEach([&](FlowStatementReference reference) {
        forEachLocalPassedOnBy(summary.statements, reference, [&](FlowLocalID local) {
            passesReachedLocalOn |= reachedLocals.get(local.index());
        });
    });
    return passesReachedLocalOn;
}

FlowSummaryAssembler::FlowSummaryAssembler(const FlowSummaryShape& shape)
    : m_entryParametersWithoutObjects(shape.entryParametersWithoutObjects)
{
    m_summary.function = shape.function;
    m_summary.numberOfEntryParameters = shape.numberOfEntryParameters;
    m_summary.numberOfResults = shape.numberOfResults;
    m_summary.numberOfValues = shape.numberOfValues;
    m_summary.numberOfLocals = shape.numberOfEntryParameters + shape.numberOfResults + 1;
    m_summary.localOfValue.fill(FlowLocalID(), shape.numberOfValues);
    m_summary.valueOfLocal.fill(ValueID(), m_summary.numberOfLocals);
}

FlowLocalID FlowSummaryAssembler::addLocalFor(ValueID value)
{
    RELEASE_ASSERT(!localOf(value));
    FlowLocalID local = addTemporary();
    bind(value, local);
    return local;
}

FlowLocalID FlowSummaryAssembler::addTemporary()
{
    FlowLocalID local(m_summary.numberOfLocals++);
    RELEASE_ASSERT(local.index() < firstProvisionalExternal);
    m_summary.valueOfLocal.append(ValueID());
    return local;
}

void FlowSummaryAssembler::bind(ValueID value, FlowLocalID local)
{
    m_summary.localOfValue[value.index()] = local;
    ValueID& valueOfLocal = m_summary.valueOfLocal[local.index()];
    if (!valueOfLocal)
        valueOfLocal = value;
}

FlowLocalID FlowSummaryAssembler::external(const FlowExternalNode& node)
{
    return m_externals.ensure(node, [&] {
        FlowLocalID local(firstProvisionalExternal + m_summary.externals.size());
        m_summary.externals.append(node);
        return local;
    }).iterator->value;
}

uint32_t FlowSummaryAssembler::addSite(const FlowSiteDescription& description)
{
    uint32_t site = m_summary.sites.size();
    m_summary.sites.append(description);
    return site;
}

uint32_t FlowSummaryAssembler::addRange(Range range)
{
    constexpr uint32_t numberOfRecentRanges = 4;
    uint32_t numberOfRanges = m_summary.ranges.size();
    for (uint32_t index = numberOfRanges - std::min(numberOfRanges, numberOfRecentRanges); index < numberOfRanges; ++index) {
        if (m_summary.ranges[index] == range)
            return index;
    }
    m_summary.ranges.append(range);
    return numberOfRanges;
}

void FlowSummaryAssembler::seed(const FlowSeed& seed)
{
    RELEASE_ASSERT(seed.node() && seed.node().index() < m_summary.numberOfLocals);
    m_summary.seeds.append(seed);
    if (seed.node().index() >= m_seededLocals.size())
        m_seededLocals.ensureSize(std::max<size_t>(m_summary.numberOfLocals, 2 * m_seededLocals.size()));
    m_seededLocals.quickSet(seed.node().index());
}

void FlowSummaryAssembler::copy(FlowLocalID from, FlowLocalID to)
{
    if (!from || !to || from == to)
        return;
    m_copies.append({ from, to });
}

FlowSummary FlowSummaryAssembler::finish()
{
    renumberLocals(findEqualLocals());
    sortClosureCreations();
    buildCopyEdges();
    m_summary.numbering = FlowStatementNumbering::of(m_summary.statements);
    buildWaiters();
    computeFlags();
    return WTF::move(m_summary);
}

// Callers, callees and resumers find these by position or through the summary, and read and write them.
BitVector FlowSummaryAssembler::localsKnownOutside() const
{
    BitVector locals(m_summary.numberOfLocals);
    for (uint32_t index = 0; index <= m_summary.thrown().index(); ++index)
        locals.quickSet(index);
    for (FlowLocalID local : { m_summary.coroutine.yielded, m_summary.coroutine.sent }) {
        if (local)
            locals.quickSet(local.index());
    }
    return locals;
}

BitVector FlowSummaryAssembler::localsFilledByMoreThanCopies() const
{
    BitVector locals = m_seededLocals;
    locals.ensureSize(m_summary.numberOfLocals);
    auto add = [&](FlowLocalID local) {
        if (local && !isProvisionalExternal(local))
            locals.quickSet(local.index());
    };

    for (auto [from, to] : m_copies) {
        if (isProvisionalExternal(from))
            add(to);
    }

    const FlowStatements& statements = m_summary.statements;
    statements.forEach([&](FlowStatementReference reference) {
        forEachLocalWrittenBy(statements, reference, add);
    });
#define ADD_LOCALS_OF_ORIGINS(kind, type, member, cursors) \
    for (const type& statement : statements.member) { \
        if (ValueID origin = originOf(statement)) \
            add(localOf(origin)); \
    }
    FOR_EACH_FLOW_STATEMENT_KIND(ADD_LOCALS_OF_ORIGINS)
#undef ADD_LOCALS_OF_ORIGINS
    return locals;
}

// The locals of a cycle of copies hold the same, and a local that is filled by copies from one other local alone holds what that local holds.
// Each local is mapped to the one that stands for it.
Vector<FlowLocalID> FlowSummaryAssembler::findEqualLocals() const
{
    uint32_t numberOfLocals = m_summary.numberOfLocals;
    Vector<FlowLocalID> representativeOf(numberOfLocals, [](size_t index) {
        return FlowLocalID(static_cast<uint32_t>(index));
    });

    Vector<std::pair<FlowLocalID, FlowLocalID>> sourcesByTarget;
    for (auto [from, to] : m_copies) {
        if (!isProvisionalExternal(from) && !isProvisionalExternal(to))
            sourcesByTarget.append({ to, from });
    }
    if (sourcesByTarget.isEmpty())
        return representativeOf;
    std::ranges::sort(sourcesByTarget);
    FlowAdjacency<FlowLocalID> sources = makeAdjacency<FlowLocalID>(numberOfLocals, sourcesByTarget.span());

    // A component gets its number after the components that it is filled from.
    StronglyConnectedComponents components = computeStronglyConnectedComponents(numberOfLocals, [&](unsigned local) {
        return sources.of(FlowLocalID(local));
    });
    Vector<uint32_t> firstMember;
    Vector<uint32_t> members;
    groupByIndex(components.numberOfComponents, firstMember, members, [&](const auto& visit) {
        for (uint32_t local = 0; local < numberOfLocals; ++local)
            visit(components.componentOf[local], local);
    });

    BitVector knownOutside = localsKnownOutside();
    BitVector filledByMoreThanCopies = localsFilledByMoreThanCopies();
    auto isIn = [](const BitVector& locals) {
        return [&locals](uint32_t local) {
            return locals.quickGet(local);
        };
    };

    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        std::span<const uint32_t> locals = entriesOfGroup(firstMember, members, component);
        if (std::ranges::any_of(locals, isIn(knownOutside)))
            continue;

        FlowLocalID representative(locals.front());
        if (std::ranges::none_of(locals, isIn(filledByMoreThanCopies))) {
            FlowLocalID onlySource;
            bool hasSeveralSources = false;
            for (uint32_t local : locals) {
                for (FlowLocalID source : sources.of(FlowLocalID(local))) {
                    if (components.componentOf[source.index()] == component)
                        continue;
                    hasSeveralSources |= onlySource && onlySource != representativeOf[source.index()];
                    onlySource = representativeOf[source.index()];
                }
            }
            if (onlySource && !hasSeveralSources)
                representative = onlySource;
        }
        for (uint32_t local : locals)
            representativeOf[local] = representative;
    }
    return representativeOf;
}

// The locals that stand for themselves keep their order, and the externals follow them.
void FlowSummaryAssembler::renumberLocals(const Vector<FlowLocalID>& representativeOf)
{
    uint32_t numberOfLocalsBefore = m_summary.numberOfLocals;
    auto standsForItself = [&](uint32_t index) {
        return representativeOf[index].index() == index;
    };

    Vector<FlowLocalID> renumbered(numberOfLocalsBefore);
    Vector<ValueID> valueOfLocal;
    for (uint32_t index = 0; index < numberOfLocalsBefore; ++index) {
        if (!standsForItself(index))
            continue;
        renumbered[index] = FlowLocalID(static_cast<uint32_t>(valueOfLocal.size()));
        valueOfLocal.append(m_summary.valueOfLocal[index]);
    }
    for (uint32_t index = 0; index < numberOfLocalsBefore; ++index) {
        if (standsForItself(index))
            continue;
        renumbered[index] = renumbered[representativeOf[index].index()];
        ValueID& value = valueOfLocal[renumbered[index].index()];
        if (!value)
            value = m_summary.valueOfLocal[index];
    }
    m_summary.numberOfLocals = valueOfLocal.size();
    m_summary.valueOfLocal = WTF::move(valueOfLocal);

    auto renumber = [&](FlowLocalID& local) {
        if (!local)
            return;
        if (isProvisionalExternal(local))
            local = FlowLocalID(m_summary.numberOfLocals + (local.index() - firstProvisionalExternal));
        else
            local = renumbered[local.index()];
    };

    for (FlowLocalID& local : m_summary.localOfValue)
        renumber(local);
    for (FlowSeed& seed : m_summary.seeds) {
        FlowLocalID node = seed.node();
        renumber(node);
        seed = seed.withNode(node);
    }
    for (FlowClosureCreation& creation : m_summary.closureCreations)
        renumber(creation.node);
    renumber(m_summary.coroutine.yielded);
    renumber(m_summary.coroutine.sent);

    for (auto& [from, to] : m_copies) {
        renumber(from);
        renumber(to);
    }
    m_copies.removeAllMatching([](const auto& copy) {
        return copy.first == copy.second;
    });

#define RENUMBER_FLOW_STATEMENTS(kind, type, member, cursors) \
    for (type& statement : m_summary.statements.member) \
        forEachLocalOf(statement, renumber);
    FOR_EACH_FLOW_STATEMENT_KIND(RENUMBER_FLOW_STATEMENTS)
#undef RENUMBER_FLOW_STATEMENTS
    for (FlowLocalID& local : m_summary.statements.localLists)
        renumber(local);
}

void FlowSummaryAssembler::sortClosureCreations()
{
    std::ranges::sort(m_summary.closureCreations, { }, &FlowClosureCreation::node);
}

void FlowSummaryAssembler::buildCopyEdges()
{
    std::ranges::sort(m_copies);
    removeRepeatedElements(m_copies);
    auto firstCopyFromExternal = std::ranges::partition_point(m_copies, [&](const auto& copy) {
        return !m_summary.isExternal(copy.first);
    });
    size_t numberOfCopiesFromLocals = firstCopyFromExternal - m_copies.begin();
    m_summary.successors = makeAdjacency<FlowLocalID>(m_summary.numberOfLocals, m_copies.span().first(numberOfCopiesFromLocals));
    m_summary.copiesFromExternals.append(m_copies.span().subspan(numberOfCopiesFromLocals));
}

// What a statement has connected flows along the edge, and a wait that a run makes itself wakes the statement without this list.
void forEachLocalThatWakes(const FlowStatements& statements, FlowStatementReference reference, NOESCAPE const ScopedLambda<void(FlowLocalID)>& functor)
{
    FlowLocalVisitor visitor { statements, functor };
    switch (reference.kind) {
    case FlowStatementKind::Filter:
        if (!statements.filters[reference.index].isEdge())
            functor(statements.filters[reference.index].from);
        return;
    case FlowStatementKind::LocationAccess:
    case FlowStatementKind::DirectCall:
        return;
    // An operand use connects its operand once. The objects it looks at come out of cursors with keys, which wake it.
    case FlowStatementKind::OperandUse:
        return;
    // An access hands the stored value and the value of this on by node. Where it looks into one of them (a store into a location with a type, a store with another receiver), it waits on the node.
    case FlowStatementKind::FieldAccess:
        visitor.visit({ statements.fieldAccesses[reference.index].receiver });
        return;
    case FlowStatementKind::PropertyAccess:
        visitor.visit({ statements.propertyAccesses[reference.index].receiver });
        return;
    case FlowStatementKind::ElementAccess:
        visitor.visit({ statements.elementAccesses[reference.index].receiver, statements.elementAccesses[reference.index].key });
        return;
    case FlowStatementKind::SlotAccess:
        visitor.visit({ statements.slotAccesses[reference.index].receiver });
        return;
    // The other operands of a call reach a callee over edges. Whether one of them holds the unknown object matters once it goes through a gate, which waits for that.
    case FlowStatementKind::Call: {
        const FlowCall& call = statements.calls[reference.index];
        visitor.visit({ call.calleeKey ? call.thisValue : call.callee, call.argumentList });
        return;
    }
    case FlowStatementKind::ConditionalSeed:
    case FlowStatementKind::NaturalOperation:
    case FlowStatementKind::Allocation:
    case FlowStatementKind::PrototypeAccess:
    case FlowStatementKind::PropertyCopy:
    case FlowStatementKind::ClassDefinition:
    case FlowStatementKind::MethodDefinition:
    case FlowStatementKind::Await:
    case FlowStatementKind::Iteration:
    case FlowStatementKind::ModuleOperation:
    case FlowStatementKind::UnseenCodeEvent:
        forEachLocalReadBy(statements, reference, functor);
        return;
    case FlowStatementKind::BuiltinCall:
    case FlowStatementKind::Lookup:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryAssembler::buildWaiters()
{
    using Waiter = std::pair<FlowLocalID, FlowStatementReference>;

    const FlowStatements& statements = m_summary.statements;
    auto makeWaiters = [&](Vector<Waiter>&& waiters) {
        std::ranges::stable_sort(waiters, { }, &Waiter::first);
        removeRepeatedElements(waiters);
        return makeAdjacency<FlowStatementReference>(m_summary.numberOfLocals, waiters.span());
    };

    Vector<Waiter> waiters;
    Vector<Waiter> statementsToWake;
    statements.forEach([&](FlowStatementReference reference) {
        forEachLocalReadBy(statements, reference, [&](FlowLocalID local) {
            if (!m_summary.isExternal(local))
                waiters.append({ local, reference });
        });
        forEachLocalThatWakes(statements, reference, [&](FlowLocalID local) {
            if (!m_summary.isExternal(local))
                statementsToWake.append({ local, reference });
        });
    });
    m_summary.waiters = makeWaiters(WTF::move(waiters));
    m_summary.statementsToWake = makeWaiters(WTF::move(statementsToWake));
}

BitVector FlowSummaryAssembler::localsReachedFromEntryParameters() const
{
    Vector<FlowLocalID, 16> parameters;
    for (uint32_t index = 0; index < m_summary.numberOfEntryParameters; ++index) {
        if (!m_entryParametersWithoutObjects.get(index))
            parameters.append(m_summary.entryParameter(index));
    }
    return localsReachedFrom(WTF::move(parameters));
}

bool FlowSummaryAssembler::handsUpNewObject() const
{
    Vector<FlowLocalID, 16> newObjects;
    for (const FlowSeed& seed : m_summary.seeds) {
        if (seed.kind() == FlowSeedKind::Site && m_summary.sites[seed.site()].kind == FlowSiteKind::Layout)
            newObjects.append(seed.node());
    }
    for (const FlowAllocation& allocation : m_summary.statements.allocations) {
        if (m_summary.sites[allocation.site].kind == FlowSiteKind::Layout)
            newObjects.append(m_summary.localOfValue[allocation.origin.index()]);
    }
    for (const FlowCall& call : m_summary.statements.calls)
        newObjects.append(call.result);
    for (const FlowDirectCall& call : m_summary.statements.directCalls)
        newObjects.append(m_summary.statements.list(call.results));

    BitVector reachedLocals = localsReachedFrom(WTF::move(newObjects));
    for (uint32_t index = 0; index < m_summary.numberOfResults; ++index) {
        if (reachedLocals.get(m_summary.result(index).index()))
            return true;
    }
    return false;
}

void FlowSummaryAssembler::flagCallsByUseOfResult()
{
    FlowStatements& statements = m_summary.statements;
    if (statements.calls.isEmpty() && statements.directCalls.isEmpty())
        return;

    using Copy = std::pair<FlowLocalID, FlowLocalID>;
    Vector<Copy> copiesByTarget;
    for (uint32_t index = 0; index < m_summary.numberOfLocals; ++index) {
        for (FlowLocalID target : m_summary.successors.of(FlowLocalID(index))) {
            if (!m_summary.isExternal(target))
                copiesByTarget.append({ target, FlowLocalID(index) });
        }
    }
    auto addCopy = [&](FlowLocalID from, FlowLocalID to) {
        if (from && to && !m_summary.isExternal(from) && !m_summary.isExternal(to))
            copiesByTarget.append({ to, from });
    };
    for (const FlowFilter& filter : statements.filters)
        addCopy(filter.from, filter.to);
    // A function may give back what it was handed: `count(new Node())`.
    for (const FlowCall& call : statements.calls) {
        for (FlowLocalID argument : statements.list(call.arguments))
            addCopy(argument, call.result);
    }
    for (const FlowDirectCall& call : statements.directCalls) {
        for (FlowLocalID argument : statements.list(call.arguments)) {
            for (FlowLocalID result : statements.list(call.results))
                addCopy(argument, result);
        }
    }
    std::ranges::sort(copiesByTarget);
    FlowAdjacency<FlowLocalID> sources = makeAdjacency<FlowLocalID>(m_summary.numberOfLocals, copiesByTarget.span());

    auto localsThatReach = [&](Vector<FlowLocalID, 16>&& worklist) {
        BitVector reaching(m_summary.numberOfLocals);
        worklist.removeAllMatching([&](FlowLocalID local) {
            return !local || m_summary.isExternal(local) || !reaching.add(local.index());
        });
        while (!worklist.isEmpty()) {
            for (FlowLocalID source : sources.of(worklist.takeLast())) {
                if (reaching.add(source.index()))
                    worklist.append(source);
            }
        }
        return reaching;
    };

    Vector<FlowLocalID, 16> receiversOfStores;
    for (const FlowPropertyAccess& access : statements.propertyAccesses) {
        if (access.kind == FlowAccessKind::Store || access.kind == FlowAccessKind::Define)
            receiversOfStores.append(access.receiver);
    }
    Vector<FlowLocalID, 16> results;
    for (uint32_t index = 0; index < m_summary.numberOfResults; ++index)
        results.append(m_summary.result(index));
    BitVector getsProperties = localsThatReach(WTF::move(receiversOfStores));
    BitVector isHandedUp = localsThatReach(WTF::move(results));

    auto flagsOf = [&](FlowLocalID result) {
        OptionSet<FlowCallFlag> flags;
        if (!result || m_summary.isExternal(result))
            return flags;
        if (getsProperties.get(result.index()))
            flags.add(FlowCallFlag::ResultGetsProperties);
        if (isHandedUp.get(result.index()))
            flags.add(FlowCallFlag::ResultIsHandedUp);
        return flags;
    };
    for (FlowCall& call : statements.calls)
        call.flags.add(flagsOf(call.result));
    for (FlowDirectCall& call : statements.directCalls) {
        for (FlowLocalID result : statements.list(call.results))
            call.flags.add(flagsOf(result));
    }
}

BitVector FlowSummaryAssembler::localsReachedFrom(Vector<FlowLocalID, 16>&& sources) const
{
    BitVector reachedLocals(m_summary.numberOfLocals);
    Vector<FlowLocalID, 16> worklist;
    auto reach = [&](FlowLocalID local) {
        if (local && !m_summary.isExternal(local) && reachedLocals.add(local.index()))
            worklist.append(local);
    };

    for (FlowLocalID source : sources)
        reach(source);
    while (!worklist.isEmpty()) {
        FlowLocalID local = worklist.takeLast();
        for (FlowLocalID successor : m_summary.successors.of(local))
            reach(successor);
        for (FlowStatementReference waiter : m_summary.waiters.of(local)) {
            if (waiter.kind == FlowStatementKind::Filter)
                reach(m_summary.statements.filters[waiter.index].to);
        }
    }
    return reachedLocals;
}

void FlowSummaryAssembler::computeFlags()
{
    if (!m_summary.sites.isEmpty())
        m_summary.flags.add(FlowSummaryFlag::CreatesObjects);
    if (!m_summary.statements.calls.isEmpty() || !m_summary.statements.directCalls.isEmpty())
        m_summary.flags.add(FlowSummaryFlag::HasCalls);
    if (passesParameterOn(m_summary, localsReachedFromEntryParameters()))
        m_summary.flags.add(FlowSummaryFlag::PassesParameterOn);
    if (handsUpNewObject())
        m_summary.flags.add(FlowSummaryFlag::HandsUpNewObject);
    flagCallsByUseOfResult();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
