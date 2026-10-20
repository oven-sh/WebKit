/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardAvailability.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinPristine.h"
#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperationColumns.h"
#include "AOTSchemaValueEffects.h"
#include "AOTValueIdentity.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ProofSet proofsOfNewLayoutInstance {
    Proof::Shape,
    Proof::OwnInstance,
    Proof::SlotFilled,
    Proof::NoExtraProperties,
    Proof::NotFrozen,
    Proof::KeyOrder,
};
static constexpr ProofSet proofsOfNewArray { Proof::Pristine, Proof::ArrayStorage };
static constexpr ProofSet proofsOfNewBuiltinInstance { Proof::Pristine };

// The run time answers with an ordinary object when the source does not fit the layout.
static constexpr OpcodeSet allocationsWithUnprovenLayout { Opcode::CloneAs };

// A source stays open across the calls of its parsers, which write what opening it read. The typed-data rules check where it is closed.
static constexpr ProofSet proofsThatOutlastWrites { Proof::SourceOpen };

// A flow record of the field, the layout or the class can stand for these, so nobody can tell whether an access takes them from its guard.
static constexpr ProofSet proofsDecidedByFlow {
    Proof::SlotFilled,
    Proof::NoExtraProperties,
    Proof::NotFrozen,
    Proof::OwnInstance,
    Proof::KeyOrder,
    Proof::MemberUnchanged,
    Proof::PrototypeLinkUnchanged,
    Proof::EvaluatedOnce,
};

namespace {

struct ReadCollector {
    void reads(const MemoryLocation& location)
    {
        Vector<MemoryLocation, 2>& list = bookkeepingHeaps.contains(location.heap()) ? bookkeepingLocations : locations;
        if (!list.contains(location))
            list.append(location);
    }

    void writes(const MemoryLocation&) { }
    void readsAllHeaps() { isUnknown = true; }
    void throws() { }
    void collects() { }
    void suspends() { }
    void terminates() { }
    void unknownCode() { isUnknown = true; }

    Vector<MemoryLocation, 2> takeSortedLocations()
    {
        if (isUnknown) {
            locations.clear();
            for (AbstractHeap heap : allMembersOf<AbstractHeap>)
                locations.append(MemoryLocation(heap));
        } else if (locations.isEmpty()) {
            // A test that reads nothing else depends on its bookkeeping reads, so its proof ends where those are written.
            locations = WTF::move(bookkeepingLocations);
        }
        std::ranges::sort(locations);
        return WTF::move(locations);
    }

    Vector<MemoryLocation, 2> locations;
    Vector<MemoryLocation, 2> bookkeepingLocations;
    bool isUnknown { false };
};

} // anonymous namespace

bool isGuardValue(const Function& function, ValueID id)
{
    return id && function.at(id).type == TypeTable::guard;
}

BitVector guardValuesOf(const Function& function, const CFG& cfg)
{
    BitVector guards(function.values.size());
    auto addGuardsNamedBy = [&](const auto& user) {
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (user.operandKind(operandIndex) == OperandKind::Guard)
                guards.quickSet(operand.index());
        });
    };
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters) {
            if (isGuardValue(function, parameter))
                guards.quickSet(parameter.index());
        }
        for (ValueID value : block.values) {
            if (isGuardValue(function, value))
                guards.quickSet(value.index());
            addGuardsNamedBy(function.at(value));
        }
        addGuardsNamedBy(block.terminator);
    }
    return guards;
}

const GuardFact* GuardFacts::find(Proof proof, ValueID subject) const
{
    for (const GuardFact& fact : facts) {
        if (fact.proves(proof) && fact.subject == subject)
            return &fact;
    }
    return nullptr;
}

const GuardFact* GuardFacts::find(Proof proof) const
{
    for (const GuardFact& fact : facts) {
        if (fact.proves(proof))
            return &fact;
    }
    return nullptr;
}

Vector<MemoryLocation, 2> locationsBehindAllocationProof(Proof proof)
{
    switch (proof) {
    case Proof::Shape:
    case Proof::OwnInstance:
    case Proof::SlotFilled:
        return { MemoryLocation(AbstractHeap::ShapeWords) };
    case Proof::NotFrozen:
    case Proof::KeyOrder:
        return { MemoryLocation(AbstractHeap::ShapeWords), MemoryLocation(AbstractHeap::OrderWords) };
    case Proof::NoExtraProperties:
        return { MemoryLocation(AbstractHeap::NamedProperties) };
    case Proof::Pristine:
        return { MemoryLocation(AbstractHeap::PrototypeLinks), MemoryLocation(AbstractHeap::NamedProperties) };
    case Proof::ArrayStorage:
        return { MemoryLocation(AbstractHeap::ArrayStorage) };
    case Proof::None:
    case Proof::TestedType:
    case Proof::Truthiness:
    case Proof::Equality:
    case Proof::IntegerBound:
    case Proof::ExactFunction:
    case Proof::CaseValue:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::PrototypeLinkUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::EpochCurrent:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::PlainData:
    case Proof::SourceOpen:
    case Proof::TextMatches:
    case Proof::RequireEdgeReady:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasFreshResult(const Program& program, const Function& function, const Value& value)
{
    EffectSummaries unknownSummaries;
    return freshnessOf(EffectContext { program, unknownSummaries }, function, value) == ResultFreshness::Fresh;
}

// Such a result inherits from what the call was handed, not from the prototype object of its class.
static bool takesPrototypeFromOperand(const BuiltinTable& builtins, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    const BuiltinOverload& overload = builtins.at(value.overload());
    return overload.result.prototypeSource || std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
        return action.kind == BuiltinPropertyActionKind::BecomesPrototype;
    });
}

// The answer comes from the allocation and the program alone. What is known about new.target is a narrowing that a later pass may drop, and a value that was named as a guard has to stay one.
bool allocatesOwnLayout(const Program& program, const Value& allocation)
{
    return !mayAllocateOtherLayout(program, allocation);
}

ProofSet proofsOfAllocation(const Program& program, const Function& function, const Value& allocation)
{
    if (allocationsWithUnprovenLayout.contains(allocation.opcode) || !allocatesOwnLayout(program, allocation))
        return { };
    bool hasAllocationRule = resultRuleOf(allocation.opcode) == ResultRule::Allocation;
    if (hasAllocationRule && Value::opcodesWithLayout.contains(allocation.opcode) && allocation.layout())
        return proofsOfNewLayoutInstance;
    const ClassPart* classPart = allocation.type ? program.at(allocation.type).singleBuiltinClass() : nullptr;
    if (!classPart || newTargetOf(allocation.opcode))
        return { };
    if (!hasAllocationRule && !hasFreshResult(program, function, allocation))
        return { };
    if (takesPrototypeFromOperand(program.builtins(), allocation))
        return { };
    if (pristineTestOf(program.builtins(), classPart->builtinClass) == PristineTest::NeverPasses)
        return { };
    return classPart->builtinClass == BuiltinClasses::Array ? proofsOfNewArray : proofsOfNewBuiltinInstance;
}

static ArrayStorage arrayStorageOfNewArray(const Program& program, const Value& allocation)
{
    ArrayStorage named = program.at(allocation.type).singleBuiltinClass()->arrayStorage;
    if (named != ArrayStorage::Other || allocation.opcode != Opcode::NewArray)
        return named;
    return arrayStorageOfLiteral(program, allocation);
}

// The storage of constant data is shared until the first store, and a built-in may hand out such storage again.
static bool allocatesWritableStorage(const Value& allocation)
{
    return allocation.opcode == Opcode::NewArray;
}

static bool listsFieldWithKey(const Program& program, const Value& value, PropertyKeyID key)
{
    if (!Value::opcodesWithFields.contains(value.opcode))
        return false;
    return std::ranges::any_of(value.fields(), [&](uint32_t field) {
        return fieldAt(program, { value.layout(), field }).key == key;
    });
}

bool allocationProves(const Program& program, const Function& function, const Value& allocation, const Value& test)
{
    Proof proof = provesOf(test.opcode);
    if (!proofsOfAllocation(program, function, allocation).contains(proof))
        return false;

    const ClassPart* allocatedClass = allocation.type ? program.at(allocation.type).singleBuiltinClass() : nullptr;
    switch (proof) {
    case Proof::Shape:
        return std::ranges::binary_search(program.at(test.layouts()).members, allocation.layout());
    case Proof::SlotFilled:
        return listsFieldWithKey(program, allocation, test.key());
    case Proof::NoExtraProperties:
    case Proof::NotFrozen:
    case Proof::OwnInstance:
        return true;
    case Proof::Pristine:
        return allocatedClass && Value::opcodesWithBuiltinClass.contains(test.opcode) && allocatedClass->builtinClass == test.builtinClass();
    case Proof::ArrayStorage:
        if (test.isWritable() && !allocatesWritableStorage(allocation))
            return false;
        return allocatedClass && test.arrayStorage() != ArrayStorage::Other && arrayStorageOfNewArray(program, allocation) == test.arrayStorage();
    case Proof::None:
    case Proof::TestedType:
    case Proof::Truthiness:
    case Proof::Equality:
    case Proof::IntegerBound:
    case Proof::ExactFunction:
    case Proof::CaseValue:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::PrototypeLinkUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::KeyOrder:
    case Proof::EpochCurrent:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::PlainData:
    case Proof::SourceOpen:
    case Proof::TextMatches:
    case Proof::RequireEdgeReady:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID guardedOperandOf(const Value& user)
{
    std::optional<uint8_t> operandIndex = guardedOf(user.opcode);
    return operandIndex ? user.operand(*operandIndex) : ValueID();
}

static OptionSet<PristineAspects> requiredPristineAspects(Opcode opcode)
{
    if (opcode == Opcode::RegExpMatchLiteral)
        return { PristineAspects::Storage, PristineAspects::Names, PristineAspects::AsCreated };

    switch (interpretedOf(opcode)) {
    case Interpretation::Read:
    case Interpretation::Store:
    case Interpretation::Test:
        return PristineAspects::Storage;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return { PristineAspects::Storage, PristineAspects::Names };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool namesArrayStorage(const Value& user)
{
    return Value::opcodesWithArrayStorage.contains(user.opcode) && user.arrayStorage() != ArrayStorage::Other;
}

static bool isInsideLength(const Value& user)
{
    return Value::opcodesWithIsInBounds.contains(user.opcode) && user.isInBounds();
}

static bool admitsLayouts(const Program& program, const Value& user, const LayoutSet& layouts)
{
    if (!Value::opcodesWithReceivers.contains(user.opcode))
        return true;
    LayoutSetID receivers = user.receivers();
    if (!receivers || isSubsetOf(layouts, program.at(receivers)))
        return true;
    if (!layouts.isComplete)
        return false;
    // The access plan leaves out a layout that has no instance.
    const Vector<LayoutID>& admitted = program.at(receivers).members;
    return std::ranges::all_of(layouts.members, [&](LayoutID member) {
        return program.at(member).flow.allocation == AllocationState::NeverAllocated || std::ranges::binary_search(admitted, member);
    });
}

static bool isProvenByType(const Program& program, const Value& user, Proof proof, const Type& type)
{
    if (type.isNever())
        return true;
    if (proof == Proof::Shape)
        return type.hasOnlyLayouts() && !type.mayBeUndefinedOrNull() && admitsLayouts(program, user, program.at(type.layouts));
    if (proof == Proof::NotThenable)
        return !protocolMayFindMember(program, type, Protocol::Thenable);
    if (proof != Proof::Pristine && proof != Proof::ArrayStorage)
        return false;

    const ClassPart* part = type.singleBuiltinClass();
    if (!part || type.mayBeUndefinedOrNull())
        return false;
    if (proof == Proof::Pristine)
        return part->provenPristine.containsAll(requiredPristineAspects(user.opcode));
    return part->contentsAreProven && part->arrayStorage == user.arrayStorage();
}

ProofSet proofsNeededFromGuard(const Program& program, const Function& function, const Value& user)
{
    ProofSet needed;
    ValueID subject = guardedOperandOf(user);
    if (!subject && guardedOf(user.opcode))
        return needed;

    ProofSet relied = reliesOf(user.opcode);
    for (Proof proof : allMembersOf<Proof>) {
        if (!relied.contains(proof) || proofsDecidedByFlow.contains(proof))
            continue;
        if (proof == Proof::ArrayStorage && !namesArrayStorage(user))
            continue;
        if (proof == Proof::InBounds && !isInsideLength(user))
            continue;
        if (subject && isProvenByType(program, user, proof, program.typeOf(function, subject)))
            continue;
        needed.add(proof);
    }
    return needed;
}

static bool provesPristineForUser(const Program& program, const Function& function, const GuardOrigin& origin, const Value& user)
{
    BuiltinClassID provenClass = BuiltinClasses::Array;
    OptionSet<PristineAspects> provenAspects { PristineAspects::Storage, PristineAspects::Names };
    switch (origin.source) {
    case GuardSource::Test:
        if (const Value& test = function.at(origin.test); Value::opcodesWithAspects.contains(test.opcode)) {
            provenClass = test.builtinClass();
            provenAspects = test.aspects();
        }
        break;
    case GuardSource::SwitchCase:
        return false;
    case GuardSource::Allocation:
        provenClass = program.at(function.at(origin.test).type).singleBuiltinClass()->builtinClass;
        break;
    }

    // The guard adds to what the type of the object proves for good.
    OptionSet<PristineAspects> required = requiredPristineAspects(user.opcode);
    if (const ClassPart* part = program.typeOf(function, guardedOperandOf(user)).singleBuiltinClass()) {
        if (part->builtinClass != provenClass)
            return false;
        required.remove(part->provenPristine);
    }
    return provenAspects.containsAll(required);
}

static ArrayStorage arrayStorageProvenBy(const Program& program, const Function& function, const GuardOrigin& origin)
{
    switch (origin.source) {
    case GuardSource::Test:
        return function.at(origin.test).arrayStorage();
    case GuardSource::SwitchCase:
        return ArrayStorage::Other;
    case GuardSource::Allocation:
        return arrayStorageOfNewArray(program, function.at(origin.test));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool provesWritableStorage(const Function& function, const GuardOrigin& origin)
{
    switch (origin.source) {
    case GuardSource::Test:
        return function.at(origin.test).isWritable();
    case GuardSource::SwitchCase:
        return false;
    case GuardSource::Allocation:
        return allocatesWritableStorage(function.at(origin.test));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A store inside the length has no path that replaces shared storage.
static bool provesArrayStorageForUser(const Program& program, const Function& function, const GuardOrigin& origin, const Value& user)
{
    if (arrayStorageProvenBy(program, function, origin) != user.arrayStorage())
        return false;
    return interpretedOf(user.opcode) != Interpretation::Store || !isInsideLength(user) || provesWritableStorage(function, origin);
}

static bool provesInBoundsForUser(FunctionAnalyses& analyses, const GuardOrigin& origin, const Value& user)
{
    if (origin.source != GuardSource::Test)
        return false;
    return analyses.identityOf(analyses.function().at(origin.test).positionOperand()) == analyses.identityOf(user.positionOperand());
}

bool provesForUser(FunctionAnalyses& analyses, const GuardFact& fact, Proof proof, const Value& user)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    if (!fact.proves(proof))
        return false;
    if (proof == Proof::Shape)
        return admitsLayouts(program, user, LayoutSet { Vector<LayoutID>(layoutsProvenBy(program, function, fact)), true });

    return std::ranges::all_of(fact.origins, [&](const GuardOrigin& origin) {
        if (proof == Proof::Pristine)
            return provesPristineForUser(program, function, origin, user);
        if (proof == Proof::ArrayStorage)
            return provesArrayStorageForUser(program, function, origin, user);
        if (proof == Proof::InBounds)
            return provesInBoundsForUser(analyses, origin, user);
        if (proof == Proof::RequireEdgeReady)
            return function.at(origin.test).module() == user.module();
        return true;
    });
}

Vector<MemoryLocation, 2> locationsReadBy(const Value& test)
{
    if (proofsThatOutlastWrites.contains(provesOf(test.opcode)))
        return { };
    ReadCollector collector;
    forEachDeclaredEffect(test, collector);
    return collector.takeSortedLocations();
}

Vector<MemoryLocation, 2> locationsReadBy(const Terminator& provingTerminator, uint32_t caseIndex)
{
    ReadCollector collector;
    forEachDeclaredEffect(provingTerminator, collector);
    if (Terminator::kindsWithLayoutSets.contains(provingTerminator.kind)) {
        std::span<const LayoutSetID> layoutSets = provingTerminator.layoutSets();
        RELEASE_ASSERT(caseIndex < layoutSets.size());
        std::ranges::replace(collector.locations, MemoryLocation(AbstractHeap::ShapeWords), MemoryLocation::shapeWords(layoutSets[caseIndex]));
    }
    return collector.takeSortedLocations();
}

void GuardAvailability::addGuard(ValueID guard)
{
    std::optional<uint32_t>& index = m_guardIndexOfValue[guard.index()];
    if (index)
        return;
    index = static_cast<uint32_t>(m_guards.size());
    m_guards.append(guard);
}

void GuardAvailability::removeGuardsFrom(size_t firstRemovedGuard)
{
    for (ValueID guard : m_guards.span().subspan(firstRemovedGuard))
        m_guardIndexOfValue[guard.index()] = std::nullopt;
    m_guards.shrink(firstRemovedGuard);
}

void GuardAvailability::collectGuards()
{
    m_guardIndexOfValue.fill(std::nullopt, m_function.values.size());
    for (size_t index : guardValuesOf(m_function, m_analyses.cfg()))
        addGuard(ValueID(index));
}

// A call of a built-in has no guard operand, so whoever resolves it asks about the allocation of its receiver before any operation names that allocation.
void GuardAvailability::collectUnnamedAllocations()
{
    const Program& program = m_analyses.program();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID value : m_function.at(block).values) {
            if (proofsOfAllocation(program, m_function, m_function.at(value)).contains(Proof::Pristine))
                addGuard(value);
        }
    }
}

static std::optional<MemoryLocation> findShapeWordsOf(const Program& program, const Value& allocation)
{
    if (!Value::opcodesWithLayout.contains(allocation.opcode) || !allocation.layout())
        return std::nullopt;
    for (TypeID type : { allocation.type, allocation.prediction() }) {
        LayoutSetID layouts = type ? program.at(type).layouts : LayoutSetID();
        if (!layouts)
            continue;
        const LayoutSet& set = program.at(layouts);
        if (set.isComplete && set.members.size() == 1 && set.members.first() == allocation.layout())
            return MemoryLocation::shapeWords(layouts);
    }
    return std::nullopt;
}

GuardFacts GuardAvailability::factsOfAllocation(ValueID id) const
{
    GuardFacts result { GuardKnowledge::Known, { } };
    const Program& program = m_analyses.program();
    const Value& allocation = m_function.at(id);
    ProofSet proofs = proofsOfAllocation(program, m_function, allocation);
    ValueID subject = m_analyses.identityOf(id);
    std::optional<MemoryLocation> shapeWords = findShapeWordsOf(program, allocation);
    for (Proof proof : allMembersOf<Proof>) {
        if (!proofs.contains(proof))
            continue;
        Vector<MemoryLocation, 2> locations = locationsBehindAllocationProof(proof);
        if (shapeWords)
            std::ranges::replace(locations, MemoryLocation(AbstractHeap::ShapeWords), *shapeWords);

        // The shape epoch says nothing about a slot that was emptied, so what it renews stands apart from what it does not.
        bool isRenewed = proofsRenewedByShapeEpoch.contains(proof);
        auto existing = std::ranges::find_if(result.facts, [&](const GuardFact& fact) {
            return fact.dependsOn == locations && fact.isRenewedByShapeEpoch() == isRenewed;
        });
        if (existing != result.facts.end()) {
            existing->proofs.add(proof);
            continue;
        }
        result.facts.append(GuardFact { { proof }, subject, WTF::move(locations), { GuardOrigin { GuardSource::Allocation, id, { }, 0 } } });
    }
    return result;
}

GuardFacts GuardAvailability::factsOfEdgeResult(BlockID from, uint32_t, const EdgeResult& result) const
{
    RELEASE_ASSERT(result.kind == EdgeResultKind::Guard);
    ValueID subject = result.subject ? m_analyses.identityOf(result.subject) : ValueID();
    if (result.caseIndex) {
        Vector<MemoryLocation, 2> locations = locationsReadBy(m_function.at(from).terminator, *result.caseIndex);
        return { GuardKnowledge::Known, { GuardFact { { result.proof }, subject, WTF::move(locations), { GuardOrigin { GuardSource::SwitchCase, { }, from, *result.caseIndex } } } } };
    }
    const Value& test = m_function.at(result.test);
    if (result.proof == Proof::EpochCurrent) {
        GuardFacts renewed = factsOf(test.guardOperand());
        renewed.facts.removeAllMatching([](const GuardFact& fact) {
            return !fact.isRenewedByShapeEpoch();
        });
        return renewed;
    }
    return { GuardKnowledge::Known, { GuardFact { { result.proof }, subject, locationsReadBy(test), { GuardOrigin { GuardSource::Test, result.test, { }, 0 } } } } };
}

void GuardAvailability::computeFacts()
{
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();
    m_facts.grow(m_guards.size());
    BitVector isComputed(m_guards.size());

    auto isPending = [&](ValueID guard) {
        if (guard.index() >= m_guardIndexOfValue.size())
            return false;
        std::optional<uint32_t> index = m_guardIndexOfValue[guard.index()];
        return index && !isComputed.quickGet(*index);
    };

    auto factsOnEdge = [&](Edge edge, uint32_t parameterIndex) -> std::optional<GuardFacts> {
        IncomingValue incoming = incomingValueOf(m_function, edge, parameterIndex);
        switch (incoming.kind) {
        case IncomingValue::Kind::Argument:
            if (isPending(incoming.argument))
                return std::nullopt;
            return factsAfterEdge(factsOf(incoming.argument), edge);
        case IncomingValue::Kind::EdgeResult: {
            std::optional<EdgeResult> result = tryGetEdgeResult(m_function, incoming.result);
            if (!result || result->kind != EdgeResultKind::Guard)
                return GuardFacts { };
            if (result->proof == Proof::EpochCurrent && isPending(m_function.at(result->test).guardOperand()))
                return std::nullopt;
            return factsAfterEdge(factsOfEdgeResult(edge.from, parameterIndex, *result), edge);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    auto factsOfParameter = [&](BlockID block, uint32_t parameterIndex) -> std::optional<GuardFacts> {
        if (block == entryBlock)
            return GuardFacts { };
        Vector<Edge, 4> edges;
        Vector<GuardFacts, 4> factsAfterEdges;
        for (Edge edge : cfg.incomingEdges(block)) {
            std::optional<GuardFacts> incoming = factsOnEdge(edge, parameterIndex);
            if (!incoming)
                continue;
            edges.append(edge);
            factsAfterEdges.append(WTF::move(*incoming));
        }
        if (edges.isEmpty())
            return std::nullopt;
        if (edges.size() == 1)
            return WTF::move(factsAfterEdges.first());
        return meetFactsAfterEdges(edges.span(), factsAfterEdges.span());
    };

    auto factsOfGuardValue = [&](ValueID guard) -> std::optional<GuardFacts> {
        ValueLocation location = useLists.locationOf(guard);
        switch (location.placement) {
        case ValuePlacement::Detached:
            return GuardFacts { };
        case ValuePlacement::Parameter:
            return factsOfParameter(location.block, location.index);
        case ValuePlacement::InBlock: {
            const Value& value = m_function.at(guard);
            if (!VariadicOperandPosition::guards(value.opcode))
                return GuardFacts { };
            if (std::ranges::any_of(value.guardsOperands(), isPending))
                return std::nullopt;
            return uniteFacts(value.guardsOperands());
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    for (uint32_t index = 0; index < m_guards.size(); ++index) {
        if (isGuardValue(m_function, m_guards[index]))
            continue;
        m_facts[index] = factsOfAllocation(m_guards[index]);
        isComputed.quickSet(index);
    }

    for (bool didChange = true; didChange;) {
        didChange = false;
        for (uint32_t index = 0; index < m_guards.size(); ++index) {
            if (!isGuardValue(m_function, m_guards[index]))
                continue;
            std::optional<GuardFacts> next = factsOfGuardValue(m_guards[index]);
            if (!next)
                continue;
            if (isComputed.quickGet(index)) {
                next = meetFacts(m_facts[index], *next);
                if (*next == m_facts[index])
                    continue;
            }
            m_facts[index] = WTF::move(*next);
            isComputed.quickSet(index);
            didChange = true;
        }
    }
}

const GuardFacts& GuardAvailability::factsOf(ValueID guard) const
{
    if (guard.index() >= m_guardIndexOfValue.size())
        return m_unknownFacts;
    std::optional<uint32_t> index = m_guardIndexOfValue[guard.index()];
    return index ? m_facts[*index] : m_unknownFacts;
}

static std::span<const LayoutID> layoutsTestedBy(const Program& program, const Function& function, const GuardOrigin& origin)
{
    switch (origin.source) {
    case GuardSource::Test:
        return program.at(function.at(origin.test).layouts()).members.span();
    case GuardSource::SwitchCase:
        return program.at(function.at(origin.switchBlock).terminator.layoutSets()[origin.caseIndex]).members.span();
    case GuardSource::Allocation:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<LayoutID, 4> layoutsProvenBy(const Program& program, const Function& function, const GuardFact& fact)
{
    if (!fact.proves(Proof::Shape))
        return { };

    Vector<LayoutID, 4> layouts;
    for (const GuardOrigin& origin : fact.origins) {
        if (origin.source == GuardSource::Allocation)
            layouts.append(function.at(origin.test).layout());
        else
            layouts.append(layoutsTestedBy(program, function, origin));
    }
    if (fact.origins.size() > 1) {
        std::ranges::sort(layouts);
        removeRepeatedElements(layouts);
    }
    return layouts;
}

static bool provesSlotFilled(const Program& program, const Function& function, const GuardFact& fact, const GuardOrigin& origin, PropertyKeyID key)
{
    switch (origin.source) {
    case GuardSource::Test:
        if (fact.proves(Proof::SlotFilled))
            return function.at(origin.test).key() == key;
        // Only a field that exists has a position in the key order.
        return fact.proves(Proof::KeyOrder) && listsFieldWithKey(program, function.at(origin.test), key);
    case GuardSource::SwitchCase:
        return false;
    case GuardSource::Allocation:
        return fact.proves(Proof::SlotFilled) && listsFieldWithKey(program, function.at(origin.test), key);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool provesSlotFilled(const Program& program, const Function& function, const GuardFact& fact, PropertyKeyID key)
{
    return std::ranges::all_of(fact.origins, [&](const GuardOrigin& origin) {
        return provesSlotFilled(program, function, fact, origin, key);
    });
}

bool guardProvesSlotFilled(FunctionAnalyses& analyses, const Value& slotOperation)
{
    ValueID guard = slotOperation.guardOperand();
    if (!guard)
        return false;

    ValueID object = analyses.identityOf(slotOperation.objectOperand());
    return std::ranges::any_of(analyses.guardFacts(guard).facts, [&](const GuardFact& fact) {
        return fact.subject == object && provesSlotFilled(analyses.program(), analyses.function(), fact, slotOperation.key());
    });
}

bool testReadsOnlySubject(const Program& program, const Function& function, const Value& test)
{
    if (!readsOnlySubjectOf(provesOf(test.opcode)))
        return false;

    bool mayReadOtherObject = false;
    test.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (test.operandKind(operandIndex) == OperandKind::Guard)
            return;
        const Type& type = program.typeOf(function, operand);
        mayReadOtherObject |= operandIndex == subjectOperandIndex ? !type.isJavaScriptValue() : mayBeReference(type);
    });
    return !mayReadOtherObject;
}

ValueID exclusiveSubjectOf(FunctionAnalyses& analyses, const GuardFact& fact)
{
    bool readsOnlySubject = std::ranges::all_of(fact.origins, [&](const GuardOrigin& origin) {
        return origin.source != GuardSource::Test || testReadsOnlySubject(analyses.program(), analyses.function(), analyses.function().at(origin.test));
    });
    return readsOnlySubject ? fact.subject : ValueID();
}

static ValueID subjectNamedBy(const Function& function, const GuardOrigin& origin)
{
    switch (origin.source) {
    case GuardSource::Test: {
        const Value& test = function.at(origin.test);
        return test.numberOfOperands() ? test.operand(subjectOperandIndex) : ValueID();
    }
    case GuardSource::SwitchCase:
        return function.at(origin.switchBlock).terminator.subjectOperand();
    case GuardSource::Allocation:
        return origin.test;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasRenamedSubject(FunctionAnalyses& analyses, const GuardFact& fact)
{
    return std::ranges::any_of(fact.origins, [&](const GuardOrigin& origin) {
        ValueID named = subjectNamedBy(analyses.function(), origin);
        return fact.subject != (named ? analyses.identityOf(named) : ValueID());
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
