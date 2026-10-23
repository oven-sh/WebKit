/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardAvailability.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinPristine.h"
#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPointsToSolver.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperationColumns.h"
#include "AOTSchemaValueEffects.h"
#include "AOTSort.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ProofSet proofsOfNewLayoutInstance {
    Proof::Shape,
    Proof::OwnInstance,
    Proof::SlotFilled,
    Proof::NoExtraProperties,
    Proof::NoFieldOutsideSlot,
    Proof::NotFrozen,
    Proof::KeyOrder,
};
static constexpr ProofSet proofsOfNewArray { Proof::Pristine, Proof::ArrayStorage };
static constexpr ProofSet proofsOfNewBuiltinInstance { Proof::Pristine };

// The run time answers with an ordinary object when the source does not fit the layout.
static constexpr OpcodeSet allocationsWithUnprovenLayout { Opcode::CloneAs };

// A compiled object keeps its shape, and with it its instance, for life.
static constexpr ProofSet proofsThatOutlastWrites { Proof::Shape, Proof::OwnInstance, Proof::SourceOpen };

// A flow record of the field, the layout or the class can stand for these, so nobody can tell whether an access takes them from its guard.
static constexpr ProofSet proofsDecidedByFlow {
    Proof::SlotFilled,
    Proof::NoExtraProperties,
    Proof::NoFieldOutsideSlot,
    Proof::BirthFieldsFilled,
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
        AOT::sort(locations);
        return WTF::move(locations);
    }

    Vector<MemoryLocation, 2> locations;
    Vector<MemoryLocation, 2> bookkeepingLocations;
    bool isUnknown { false };
};

} // anonymous namespace

static OpcodeSet opcodesMatching(NOESCAPE const Invocable<bool(Opcode)> auto& predicate)
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (predicate(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}

static bool hasDeclaredOperandMatching(Opcode opcode, NOESCAPE const Invocable<bool(const OperandInfo&)> auto& predicate)
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (predicate(operandInfoOf(opcode, operandIndex)))
            return true;
    }
    return false;
}

static const OpcodeSet& opcodesThatNameGuards()
{
    static const OpcodeSet opcodes = opcodesMatching([](Opcode opcode) {
        return hasDeclaredOperandMatching(opcode, [](const OperandInfo& operand) {
            return operand.kind == OperandKind::Guard;
        });
    });
    return opcodes;
}

// No other opcode has the result rule of an allocation, and freshnessOf() knows nothing of the result of any other.
static const OpcodeSet& opcodesThatMayMakeObject()
{
    static const OpcodeSet opcodes = opcodesMatching([](Opcode opcode) {
        return resultRuleOf(opcode) == ResultRule::Allocation || resultMayBeFresh(opcode);
    });
    return opcodes;
}

// The solver of EscapeAnalysis lets a value stand for objects of the function alone only where the value defines such an object, is a cast or a conversion, or is a parameter of a block behind the entry.
static bool isDefinedOutside(FunctionAnalyses& analyses, ValueID definition)
{
    static const OpcodeSet opcodesThatMayGiveOwnObject = opcodesThatMayDefineObjects() | castsAndConversions | OpcodeSet { Opcode::Unspecified };

    ValueLocation location = analyses.useLists().locationOf(definition);
    switch (location.placement) {
    case ValuePlacement::Detached:
        return false;
    case ValuePlacement::Parameter:
        return location.block == entryBlock && definition != findFreshEntryParameter(analyses.function(), analyses.cfg());
    case ValuePlacement::InBlock:
        return !opcodesThatMayGiveOwnObject.contains(analyses.function().at(definition).opcode);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool comesFromOutside(FunctionAnalyses& analyses, ValueID value)
{
    constexpr size_t maxDefinitions = 24;

    // What one edge brings from outside, the parameter may be, and so may the parameters that it is passed on to.
    Vector<ValueID, maxDefinitions> definitions { analyses.identityOf(value) };
    for (size_t next = 0; next < definitions.size(); ++next) {
        ValueID definition = definitions[next];
        if (isDefinedOutside(analyses, definition))
            return true;
        ValueLocation location = analyses.useLists().locationOf(definition);
        if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
            continue;
        for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
            ValueID passed = valuePassedOn(analyses.function(), edge, location.index);
            if (passed && definitions.size() < maxDefinitions)
                definitions.appendIfNotContains(analyses.identityOf(passed));
        }
    }
    return false;
}

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
    const OpcodeSet& opcodesNamingGuards = opcodesThatNameGuards();
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters) {
            if (isGuardValue(function, parameter))
                guards.quickSet(parameter.index());
        }
        for (ValueID value : block.values) {
            if (isGuardValue(function, value))
                guards.quickSet(value.index());
            if (opcodesNamingGuards.contains(function.at(value).opcode))
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
    case Proof::SlotFilled:
    // A store that creates a field writes the order and the count of the order word, never its integrity bits. Whatever changes those writes the shape words too.
    case Proof::NotFrozen:
        return { MemoryLocation(AbstractHeap::ShapeWords) };
    case Proof::KeyOrder:
        return { MemoryLocation(AbstractHeap::ShapeWords), MemoryLocation(AbstractHeap::OrderWords) };
    case Proof::NoExtraProperties:
    case Proof::NoFieldOutsideSlot:
    case Proof::BirthFieldsFilled:
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
    case Proof::NotNullish:
    case Proof::Shape:
    case Proof::OwnInstance:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::PrototypeLinkUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::PlainData:
    case Proof::SourceOpen:
    case Proof::TextMatches:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasFreshResult(const Program& program, const Function& function, const Value& value)
{
    if (!opcodesThatMayMakeObject().contains(value.opcode))
        return false;
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

// The object of a literal is born with what the literal lists. The fields of a class are stored by its constructor, and some of them hold raw values that a listed undefined would not be.
static bool fillsEveryFieldOfBirth(const Program& program, const Value& allocation)
{
    if (allocation.opcode != Opcode::NewObject || program.at(allocation.layout()).kind != LayoutKind::Record)
        return false;
    std::span<const uint32_t> listed = allocation.fields();
    bool hasFieldOfBirth = false;
    for (uint32_t field = 0; field < program.at(allocation.layout()).fields.size(); ++field) {
        if (!isFilledAtBirth(program, { allocation.layout(), field }))
            continue;
        if (!std::ranges::contains(listed, field))
            return false;
        hasFieldOfBirth = true;
    }
    return hasFieldOfBirth;
}

ProofSet proofsOfAllocation(const Program& program, const Function& function, const Value& allocation)
{
    if (allocationsWithUnprovenLayout.contains(allocation.opcode) || !allocatesOwnLayout(program, allocation))
        return { };
    // A template object is frozen and has `raw`: it is an array, and in nothing like a new one.
    if (allocation.opcode == Opcode::NewTemplateObject)
        return { };
    bool hasAllocationRule = resultRuleOf(allocation.opcode) == ResultRule::Allocation;
    if (hasAllocationRule && Value::opcodesWithLayout.contains(allocation.opcode) && allocation.layout())
        return fillsEveryFieldOfBirth(program, allocation) ? proofsOfNewLayoutInstance | ProofSet { Proof::BirthFieldsFilled } : proofsOfNewLayoutInstance;
    const ClassPart* classPart = allocation.type ? program.at(allocation.type).singleBuiltinClass() : nullptr;
    if (!classPart || newTargetOf(allocation.opcode))
        return { };
    if (!hasAllocationRule && !hasFreshResult(program, function, allocation))
        return { };
    if (takesPrototypeFromOperand(program.builtins(), allocation))
        return { };
    if (pristineTestOf(program.builtins(), classPart->builtinClass) == PristineTest::NeverPasses && !canOnlyBeProvenPristine(program.builtins(), classPart->builtinClass))
        return { };
    return classPart->builtinClass == BuiltinClasses::Array ? proofsOfNewArray : proofsOfNewBuiltinInstance;
}

// ExpandLongArrayLiterals puts an array of holes that is filled and then cut in the place of a long literal, and whoever named the literal as its guard names the cut array from then on.
static bool isFilledArrayOfHoles(const Function& function, const Value& allocation)
{
    return allocation.opcode == Opcode::TruncateNewArray && function.at(allocation.arrayOperand()).opcode == Opcode::NewContiguousArray;
}

static ArrayStorage arrayStorageOfNewArray(const Program& program, const Function& function, const Value& allocation)
{
    ArrayStorage named = program.at(allocation.type).singleBuiltinClass()->arrayStorage;
    if (named != ArrayStorage::Other)
        return named;
    if (isFilledArrayOfHoles(function, allocation))
        return ArrayStorage::Contiguous;
    return allocation.opcode == Opcode::NewArray ? arrayStorageOfLiteral(program, allocation) : named;
}

// The storage of constant data is shared until the first store, and a built-in may hand out such storage again.
static bool allocatesWritableStorage(const Function& function, const Value& allocation)
{
    return allocation.opcode == Opcode::NewArray || isFilledArrayOfHoles(function, allocation);
}

// Such a test passes only where the slot holds a value of the field's type without undefined and null, and such a read loads the slot as one.
static bool holdsDeclaredType(const Value& slotOperation)
{
    return Value::opcodesWithHoldsDeclaredType.contains(slotOperation.opcode) && slotOperation.holdsDeclaredType();
}

TestedSlotState slotStateTestedBy(const Value& test)
{
    if (test.opcode != Opcode::IsSlotFilled)
        return TestedSlotState::None;
    return holdsDeclaredType(test) ? TestedSlotState::HoldsDeclaredType : TestedSlotState::Filled;
}

TestedSlotState slotStateProvenBy(const Function& function, const GuardFact& fact)
{
    if (fact.proofs != ProofSet { Proof::SlotFilled } || fact.origins.isEmpty())
        return TestedSlotState::None;
    TestedSlotState proven = TestedSlotState::Filled;
    for (const GuardOrigin& origin : fact.origins) {
        TestedSlotState tested = origin.source == GuardSource::Test ? slotStateTestedBy(function.at(origin.test)) : TestedSlotState::None;
        if (tested == TestedSlotState::None)
            return TestedSlotState::None;
        proven = std::max(proven, tested);
    }
    return proven;
}

bool leavesSlotsAsTested(const Program& program, const Function& function, const Value& writer, const Effects& effectsOfWriter, TestedSlotState tested)
{
    if (tested == TestedSlotState::None || !effectsOfWriter.writes.isSubsetOf(declaredBoundOf(Opcode::PutSlotOrAdd).writes))
        return false;
    auto fillsSlot = [&](ValueID stored) {
        const Type& type = program.typeOf(function, stored);
        if (type.primitives & Primitive::Absent)
            return false;
        return tested == TestedSlotState::Filled || !type.mayBeUndefinedOrNull();
    };
    if (writer.opcode == Opcode::PutSlots)
        return std::ranges::all_of(writer.variadicOperands(), fillsSlot);
    std::optional<uint8_t> stored = storedOf(writer.opcode);
    return stored && fillsSlot(writer.operand(*stored));
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
        // An allocation may fill the slot with undefined.
        return !holdsDeclaredType(test) && listsFieldWithKey(program, allocation, test.key());
    case Proof::NoExtraProperties:
    case Proof::NoFieldOutsideSlot:
    case Proof::BirthFieldsFilled:
    case Proof::NotFrozen:
    case Proof::OwnInstance:
        return true;
    case Proof::Pristine:
        return allocatedClass && Value::opcodesWithBuiltinClass.contains(test.opcode) && allocatedClass->builtinClass == test.builtinClass();
    case Proof::ArrayStorage:
        if (test.isWritable() && !allocatesWritableStorage(function, allocation))
            return false;
        return allocatedClass && test.arrayStorage() != ArrayStorage::Other && arrayStorageOfNewArray(program, function, allocation) == test.arrayStorage();
    case Proof::None:
    case Proof::TestedType:
    case Proof::Truthiness:
    case Proof::Equality:
    case Proof::IntegerBound:
    case Proof::ExactFunction:
    case Proof::CaseValue:
    case Proof::NotNullish:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::PrototypeLinkUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::KeyOrder:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::PlainData:
    case Proof::SourceOpen:
    case Proof::TextMatches:
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

static bool isInBounds(const Value& user)
{
    return Value::opcodesWithIsInBounds.contains(user.opcode) && user.isInBounds();
}

static bool readsPresentElement(const Value& user)
{
    return Value::opcodesWithIsPresent.contains(user.opcode) && user.isPresent();
}

static bool isInsideLength(const Value& user)
{
    if (isInBounds(user) || readsPresentElement(user))
        return true;
    return Value::opcodesWithIsInsideView.contains(user.opcode) && user.isInsideView();
}

static bool admitsLayouts(const Program& program, const Value& user, std::span<const LayoutID> sortedLayouts, bool isComplete)
{
    if (!Value::opcodesWithReceivers.contains(user.opcode))
        return true;
    LayoutSetID receivers = user.receivers();
    if (!receivers || !program.at(receivers).isComplete)
        return true;
    if (!isComplete)
        return false;
    const Vector<LayoutID>& admitted = program.at(receivers).members;
    if (sortedLayouts.size() <= admitted.size() && std::ranges::includes(admitted, sortedLayouts))
        return true;
    // The access plan leaves out a layout that has no instance.
    return std::ranges::all_of(sortedLayouts, [&](LayoutID member) {
        return program.at(member).flow.allocation == AllocationState::NeverAllocated || std::ranges::binary_search(admitted, member);
    });
}

static bool admitsLayoutSet(const Program& program, const Value& user, LayoutSetID layouts, bool isComplete)
{
    if (Value::opcodesWithReceivers.contains(user.opcode) && user.receivers() == layouts)
        return true;
    return admitsLayouts(program, user, program.at(layouts).members.span(), isComplete);
}

static LayoutSetID layoutSetTestedBy(const Function& function, const GuardOrigin& origin)
{
    switch (origin.source) {
    case GuardSource::Test:
        return function.at(origin.test).layouts();
    case GuardSource::SwitchCase:
        return function.at(origin.switchBlock).terminator.layoutSets()[origin.caseIndex];
    case GuardSource::Allocation:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasOneTestedLayoutSet(const GuardFact& fact)
{
    return fact.proves(Proof::Shape) && fact.origins.size() == 1 && fact.origins[0].source != GuardSource::Allocation;
}

static bool isProvenByType(const Program& program, const Value& user, Proof proof, const Type& type)
{
    if (type.isNever())
        return true;
    if (proof == Proof::Shape)
        return type.hasOnlyLayouts(program) && !type.mayBeUndefinedOrNull() && admitsLayoutSet(program, user, type.layouts, program.at(type.layouts).isComplete);
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
        if (!relied.contains(proof))
            continue;
        // No flow record says which of the values that fill a slot it holds.
        bool needsTestOfSlot = proof == Proof::SlotFilled && holdsDeclaredType(user);
        if (proofsDecidedByFlow.contains(proof) && !needsTestOfSlot)
            continue;
        if (proof == Proof::ArrayStorage && !namesArrayStorage(user))
            continue;
        if (proof == Proof::InBounds && !isInsideLength(user))
            continue;
        // An own element that is present answers without the prototypes.
        if (proof == Proof::Pristine && readsPresentElement(user))
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
        return arrayStorageOfNewArray(program, function, function.at(origin.test));
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
        return allocatesWritableStorage(function, function.at(origin.test));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A store inside the length has no path that replaces shared storage.
static bool provesArrayStorageForUser(const Program& program, const Function& function, const GuardOrigin& origin, const Value& user)
{
    if (arrayStorageProvenBy(program, function, origin) != user.arrayStorage())
        return false;
    return interpretedOf(user.opcode) != Interpretation::Store || !isInBounds(user) || provesWritableStorage(function, origin);
}

// A constant is made again wherever a copy of the code needs it, so the test and the access may name two values.
static bool isSamePosition(FunctionAnalyses& analyses, ValueID a, ValueID b)
{
    if (analyses.identityOf(a) == analyses.identityOf(b))
        return true;
    std::optional<int64_t> constant = integerConstantThroughCasts(analyses.program(), analyses.function(), a);
    return constant && constant == integerConstantThroughCasts(analyses.program(), analyses.function(), b);
}

static bool provesInBoundsForUser(FunctionAnalyses& analyses, const GuardOrigin& origin, const Value& user)
{
    if (origin.source != GuardSource::Test)
        return false;
    const Value& test = analyses.function().at(origin.test);
    // A position inside the length may hold a hole.
    if (readsPresentElement(user) && test.opcode != Opcode::IsElementPresent)
        return false;
    if (Value::opcodesWithIsInsideView.contains(user.opcode) != (test.opcode == Opcode::IsInsideView))
        return false;
    return isSamePosition(analyses, test.positionOperand(), user.positionOperand());
}

// The mark of the cell answers for the fields that every constructor fills, and for the raw value of a number or a boolean among them.
static bool isAnsweredByTestOfCell(const Program& program, const Value& slotOperation)
{
    bool needsRawValue = holdsDeclaredType(slotOperation);
    return std::ranges::all_of(program.at(slotOperation.receivers()).members, [&](LayoutID layout) {
        std::optional<FieldReference> field = findField(program, layout, slotOperation.key(), BaseChain::Proven);
        if (!field)
            return false;
        if (needsRawValue)
            return holdsRawValueFromBirth(program, *field) || (staysFilled(program, *field) && !slotHoldsUndefinedAndNull(program, *field));
        return staysFilled(program, *field) || isFilledAtBirth(program, *field);
    });
}

// An allocation may fill the slot with undefined or null.
static bool fillsSlotWithDeclaredType(const Program& program, const Function& function, const Value& allocation, PropertyKeyID key)
{
    if (!Value::opcodesWithFields.contains(allocation.opcode))
        return false;
    auto mayBeMarker = [&](ValueID stored) {
        return program.typeOf(function, stored).mayBeUndefinedOrNull();
    };
    std::span<const uint32_t> fields = allocation.fields();
    std::span<const ValueID> values = allocation.variadicOperands();
    bool listsValue = false;
    for (size_t index = 0; index < fields.size() && index < values.size(); ++index) {
        if (fieldAt(program, { allocation.layout(), fields[index] }).key == key)
            listsValue = !mayBeMarker(values[index]);
    }
    if (!listsValue)
        return false;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& store = function.at(id);
            if (store.opcode == Opcode::PutSlots) {
                PayloadListView<PlacedKey> keys = store.placedKeys();
                std::span<const ValueID> stored = store.variadicOperands();
                for (uint32_t index = 0; index < keys.size() && index < stored.size(); ++index) {
                    if (keys[index].key == key && mayBeMarker(stored[index]))
                        return false;
                }
                continue;
            }
            if ((store.opcode == Opcode::PutSlot || store.opcode == Opcode::PutSlotOrAdd) && store.key() == key && mayBeMarker(store.operand(OperandIndex::PutSlot::value)))
                return false;
        }
    }
    return true;
}

static bool provesSlotFilledForUser(const Program& program, const Function& function, const GuardOrigin& origin, const Value& user)
{
    if (!holdsDeclaredType(user))
        return true;
    if (origin.source == GuardSource::Allocation)
        return fillsSlotWithDeclaredType(program, function, function.at(origin.test), user.key());
    if (origin.source != GuardSource::Test)
        return false;
    const Value& test = function.at(origin.test);
    return holdsDeclaredType(test) && test.key() == user.key();
}

bool provesForUser(FunctionAnalyses& analyses, const GuardFact& fact, Proof proof, const Value& user)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    if (proof == Proof::SlotFilled && fact.proves(Proof::BirthFieldsFilled) && isAnsweredByTestOfCell(program, user))
        return true;
    if (!fact.proves(proof))
        return false;
    if (proof == Proof::Shape) {
        if (hasOneTestedLayoutSet(fact))
            return admitsLayoutSet(program, user, layoutSetTestedBy(function, fact.origins[0]), true);
        return admitsLayouts(program, user, layoutsProvenBy(program, function, fact).span(), true);
    }

    return std::ranges::all_of(fact.origins, [&](const GuardOrigin& origin) {
        if (proof == Proof::SlotFilled)
            return provesSlotFilledForUser(program, function, origin, user);
        if (proof == Proof::Pristine)
            return provesPristineForUser(program, function, origin, user);
        if (proof == Proof::ArrayStorage)
            return provesArrayStorageForUser(program, function, origin, user);
        if (proof == Proof::InBounds)
            return provesInBoundsForUser(analyses, origin, user);
        return true;
    });
}

Vector<MemoryLocation, 2> locationsReadBy(const Value& test)
{
    if (proofsThatOutlastWrites.contains(provesOf(test.opcode)))
        return { };
    ReadCollector collector;
    forEachDeclaredEffect(test, collector);
    Vector<MemoryLocation, 2> locations = collector.takeSortedLocations();
    if (provesOf(test.opcode) == Proof::NotFrozen)
        locations.removeAll(MemoryLocation(AbstractHeap::OrderWords));
    return locations;
}

Vector<MemoryLocation, 2> locationsReadBy(const Terminator& provingTerminator, uint32_t)
{
    if (proofsThatOutlastWrites.contains(provesOf(provingTerminator.kind)))
        return { };
    ReadCollector collector;
    forEachDeclaredEffect(provingTerminator, collector);
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
    const OpcodeSet& opcodesOfAllocations = opcodesThatMayMakeObject();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID value : m_function.at(block).values) {
            const Value& allocation = m_function.at(value);
            if (opcodesOfAllocations.contains(allocation.opcode) && proofsOfAllocation(program, m_function, allocation).contains(Proof::Pristine))
                addGuard(value);
        }
    }
}

static std::optional<MemoryLocation> findShapeWordsOf(const Program& program, const Value& allocation)
{
    if (!Value::opcodesWithLayout.contains(allocation.opcode) || !allocation.layout())
        return std::nullopt;
    for (TypeID type : { allocation.type, allocation.expected() }) {
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

        auto existing = std::ranges::find_if(result.facts, [&](const GuardFact& fact) {
            return fact.dependsOn == locations;
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
    return { GuardKnowledge::Known, { GuardFact { { result.proof }, subject, locationsReadBy(m_function.at(result.test)), { GuardOrigin { GuardSource::Test, result.test, { }, 0 } } } } };
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
    return program.at(layoutSetTestedBy(function, origin)).members.span();
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
        AOT::sort(layouts);
        removeRepeatedElements(layouts);
    }
    return layouts;
}

bool provenLayoutsAreSubsetOf(const Program& program, const Function& function, const GuardFact& fact, const GuardFact& other)
{
    Vector<LayoutID, 4> unitedLayouts;
    Vector<LayoutID, 4> otherUnitedLayouts;
    auto sortedLayoutsOf = [&](const GuardFact& proving, Vector<LayoutID, 4>& united) -> std::span<const LayoutID> {
        if (hasOneTestedLayoutSet(proving))
            return layoutsTestedBy(program, function, proving.origins[0]);
        united = layoutsProvenBy(program, function, proving);
        return united.span();
    };
    std::span<const LayoutID> layouts = sortedLayoutsOf(fact, unitedLayouts);
    std::span<const LayoutID> otherLayouts = sortedLayoutsOf(other, otherUnitedLayouts);
    if (layouts.data() == otherLayouts.data() && layouts.size() == otherLayouts.size())
        return true;
    return layouts.size() <= otherLayouts.size() && std::ranges::includes(otherLayouts, layouts);
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

// A proof that a slot is filled begins at a test that proves it, or at an allocation that names its layout: proofOfTerminator(), proofsOfAllocation().
static constexpr OpcodeSet opcodesBehindProofsOfFilledSlots = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        Proof proof = provesOf(opcode);
        bool isTest = proof == Proof::SlotFilled || proof == Proof::KeyOrder || proof == Proof::BirthFieldsFilled;
        if (isTest || (resultRuleOf(opcode) == ResultRule::Allocation && Value::opcodesWithLayout.contains(opcode)))
            opcodes.add(opcode);
    }
    return opcodes;
}();

bool guardProvesSlotFilled(FunctionAnalyses& analyses, const Value& slotOperation)
{
    ValueID guard = slotOperation.guardOperand();
    if (!guard)
        return false;

    // A build that verifies the program asks the facts all the same, and compares.
    bool mayBeProven = analyses.opcodesOfValues().intersects(opcodesBehindProofsOfFilledSlots);
    if (!mayBeProven && !analyses.context().build.options().verifiesProgram)
        return false;

    const Program& program = analyses.program();
    bool testOfCellAnswers = isAnsweredByTestOfCell(program, slotOperation);
    ValueID object = analyses.identityOf(slotOperation.objectOperand());
    bool isProven = std::ranges::any_of(analyses.guardFacts(guard).facts, [&](const GuardFact& fact) {
        if (fact.subject != object)
            return false;
        return provesSlotFilled(program, analyses.function(), fact, slotOperation.key()) || (testOfCellAnswers && fact.proves(Proof::BirthFieldsFilled));
    });
    RELEASE_ASSERT(mayBeProven || !isProven);
    return isProven;
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
