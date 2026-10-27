/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocationPointers.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinPristine.h"
#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTResultRules.h"
#include "AOTSchemaRuntimeColumns.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

bool hasFrameStorage(const Value& value)
{
    return Value::opcodesWithStorage.contains(value.opcode) && value.storage() != AllocationStorage::Heap;
}

bool isFrameCell(const Function& function, ValueID id)
{
    const Value& definition = function.at(sameObjectThroughCasts(function, id));
    return Value::opcodesWithStorage.contains(definition.opcode) && definition.storage() == AllocationStorage::Frame;
}

static constexpr OperandUses usesOfArrayReaders { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::RetainsContents };

static constexpr OpcodeSet readersOfArraysWhereTheyLie {
    Opcode::TypeTest, Opcode::IsPristine, Opcode::StrictEq, Opcode::ValueEqualsLiteral, Opcode::ToBoolean,
    Opcode::HasArrayStorage, Opcode::IsInBounds, Opcode::IsElementPresent, Opcode::GetArrayLength, Opcode::GetArrayElement, Opcode::HasSpareArrayCapacity,
    Opcode::ArrayStorageKind, Opcode::HasContiguousIndex, Opcode::GetContiguousIndex,
    Opcode::CallList, Opcode::AppendElements, Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments,
    Opcode::ValidateType, Opcode::ValidateSameValue,
};

bool canLieInFrame(const Program& program, const BuildContext& build, const Value& allocation)
{
    switch (allocation.opcode) {
    case Opcode::NewArray:
        return allocation.holes().empty() && !allocation.capacityOperand()
            && allocation.elementsOperands().size() <= build.limit(LimitID::MaxFrameArrayLength)
            && arrayStorageAtAllocation(program, program.at(allocation.type)) == ArrayStorage::Contiguous;
    case Opcode::CreateRest:
        return true;
    default:
        return false;
    }
}

bool givesSameArray(Opcode opcode, uint32_t operandIndex)
{
    return isCast(opcode) && opcode != Opcode::Check && operandUsesOf(opcode, operandIndex).contains(OperandUse::Returns);
}

Vector<ValueID, 8> findArraysThatMayLieInFrame(const Program& program, const BuildContext& build, const Function& function)
{
    BitVector arrays;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (canLieInFrame(program, build, function.at(id)))
                arrays.ensureSizeAndSet(id.index(), function.values.size());
        }
    }
    if (arrays.isEmpty())
        return { };

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& user = function.at(id);
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!arrays.quickGet(operand.index()) || user.operandKind(operandIndex) == OperandKind::Guard)
                    return;
                if (effectRuleOf(user.opcode) != EffectRule::Call && !givesSameArray(user.opcode, operandIndex) && !readsArrayWhereItLies(program, function, user, operandIndex))
                    arrays.quickClear(operand.index());
            });
        }
        block.terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (block.terminator.operandKind(operandIndex) != OperandKind::Guard)
                arrays.quickClear(operand.index());
        });
    }

    Vector<ValueID, 8> result;
    for (size_t index : arrays)
        result.append(ValueID(index));
    return result;
}

static bool readsArrayWhereItLies(std::optional<InlineBuiltin> builtin, uint32_t operandIndex, uint32_t receiver, uint32_t firstArgument)
{
    if (!builtin)
        return false;
    switch (*builtin) {
    case InlineBuiltin::ArrayLength:
        return operandIndex == receiver;
    // f.apply(thisValue, list) is operationAOTApply wherever it stands.
    case InlineBuiltin::FunctionApply:
        return operandIndex == firstArgument + 1;
    default:
        return false;
    }
}

bool readsArrayWhereItLies(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
    switch (user.opcode) {
    case Opcode::CallBuiltin:
        return user.builtinCallForm() == BuiltinCallForm::Whole && readsArrayWhereItLies(inlineImplementationOf(program, user), operandIndex, OperandIndex::CallBuiltin::receiver, OperandIndex::CallBuiltin::arguments);
    case Opcode::CallInline:
        return readsArrayWhereItLies(user.inlineBuiltin(), operandIndex, OperandIndex::CallInline::receiver, OperandIndex::CallInline::arguments);
    // No prototype of an array has a property whose name is a number.
    case Opcode::ElemGet:
        return operandIndex == OperandIndex::ElemGet::object && program.typeOf(function, user.propertyOperand()).isNumber();
    default:
        return readersOfArraysWhereTheyLie.contains(user.opcode) && operandUsesOf(user.opcode, operandIndex).containsOnly(usesOfArrayReaders);
    }
}

static bool mayHandFrameArrayTo(const Program& program, const FlowResults* flow, const Value& call, uint32_t operandIndex)
{
    if (!flow || effectRuleOf(call.opcode) != EffectRule::Call || maySuspend(call.opcode) || call.operandUses(operandIndex).containsAny(callingUses))
        return false;
    CalleeList callees = calleesOfCall(program, call);
    if (!callees.isComplete || !callees.builtins.isEmpty())
        return false;
    return std::ranges::all_of(callees.functions, [&](FunctionID callee) {
        std::optional<uint32_t> entryParameter = entryParameterOf(program.at(callee), call, operandIndex);
        return entryParameter && flow->mayTakeFrameArray(callee, *entryParameter);
    });
}

static bool holdsPartOfItsCaller(FunctionRole role)
{
    return role == FunctionRole::OutlinedBody;
}

bool mayBeGivenFrameArray(const Program& program, const FlowResults* flow, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (user.operandKind(operandIndex) == OperandKind::Guard || givesSameArray(user.opcode, operandIndex))
        return true;
    // A narrow reference counts from the base of the cage of cells.
    if (familyOf(user.opcode) == OperationFamily::Representation)
        return user.opcode != Opcode::Compress;
    // What was moved out of a function since the table was made is asked where it is now.
    if (user.opcode == Opcode::CallDirect && flow && flow->frameArrayParameters().isOlderThan(user.function()) && holdsPartOfItsCaller(program.at(user.function()).role))
        return true;
    return readsArrayWhereItLies(program, function, user, operandIndex) || mayHandFrameArrayTo(program, flow, user, operandIndex);
}

std::optional<FrameArrayTest> testOfFrameArray(const Program& program, const Value& test)
{
    switch (test.opcode) {
    case Opcode::IsPristine:
        if (test.builtinClass() != BuiltinClasses::Array)
            return std::nullopt;
        return FrameArrayTest { test.valueOperand(), !test.aspects().containsAny(aspectsWithoutTest) && pristineTestOf(program.builtins(), BuiltinClasses::Array) == PristineTest::MayPass };
    case Opcode::HasArrayStorage:
        return FrameArrayTest { test.arrayOperand(), test.arrayStorage() == ArrayStorage::Contiguous };
    case Opcode::ToBoolean:
        return FrameArrayTest { test.valueOperand(), true };
    case Opcode::TypeTest: {
        // A test looks at the class alone, and leaves out a class of which the type says more than that.
        const Type& tested = program.at(test.testedType());
        if (tested.anyObject)
            return FrameArrayTest { test.valueOperand(), true };
        const ClassPart* arrays = tested.findClassPart(BuiltinClasses::Array);
        if (!arrays)
            return FrameArrayTest { test.valueOperand(), false };
        if (arrays->contentsAreProven || !arrays->provenPristine.isEmpty())
            return std::nullopt;
        return FrameArrayTest { test.valueOperand(), true };
    }
    default:
        return std::nullopt;
    }
}

static bool canFollowResultOf(Opcode opcode)
{
    if (opcode == Opcode::MaterializeEnvironment)
        return false;
    switch (effectRuleOf(opcode)) {
    case EffectRule::Call:
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return false;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool readsStoredValues(Opcode opcode)
{
    return declaredBoundOf(opcode).reads.containsAny(heapsHoldingValues);
}

bool propagatesFramePointers(Opcode opcode)
{
    return canFollowResultOf(opcode) && (readsFollowedByFramePointers.contains(opcode) || !readsStoredValues(opcode));
}

ValueID cellAccessedBy(const Value& followedAccess)
{
    ASSERT(storesFollowedByFramePointers.contains(followedAccess.opcode) || readsFollowedByFramePointers.contains(followedAccess.opcode));
    if (OperandPosition::closure(followedAccess.opcode))
        return followedAccess.closureOperand();
    if (OperandPosition::environment(followedAccess.opcode))
        return followedAccess.environmentOperand();
    return followedAccess.objectOperand();
}

std::optional<CellPlace> placeStoredTo(const Value& store, uint32_t operandIndex)
{
    static_assert(OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);
    std::optional<uint32_t> position = store.variadicOperandIndex(operandIndex);
    switch (store.opcode) {
    case Opcode::NewObject:
        if (!position || *position >= store.fields().size())
            return std::nullopt;
        return CellPlace { CellPlace::Kind::Field, store.layout().index(), store.fields()[*position] };
    case Opcode::NewClosure:
        if (!position)
            return std::nullopt;
        return CellPlace { CellPlace::Kind::Capture, store.function().index(), *position };
    case Opcode::PutSlots:
        if (!position)
            return std::nullopt;
        return CellPlace { CellPlace::Kind::Key, store.placedKeys()[*position].key.index(), 0 };
    case Opcode::PutEnvironmentField:
        if (operandIndex != OperandIndex::PutEnvironmentField::value)
            return std::nullopt;
        return CellPlace { CellPlace::Kind::Field, store.layout().index(), store.field() };
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        if (operandIndex != OperandIndex::PutSlot::value)
            return std::nullopt;
        return CellPlace { CellPlace::Kind::Key, store.key().index(), 0 };
    default:
        ASSERT(!storesFollowedByFramePointers.contains(store.opcode));
        return std::nullopt;
    }
}

CellPlace placeReadBy(const Value& followedRead)
{
    switch (followedRead.opcode) {
    case Opcode::GetEnvironmentField:
        return { CellPlace::Kind::Field, followedRead.layout().index(), followedRead.field() };
    case Opcode::GetCapture:
        return { CellPlace::Kind::Capture, followedRead.function().index(), followedRead.index() };
    case Opcode::GetSlot:
    case Opcode::GetSlotOrUndefined:
        return { CellPlace::Kind::Key, followedRead.key().index(), 0 };
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool cellHasPlace(const Program& program, const Value& allocation, const CellPlace& place)
{
    switch (place.kind) {
    case CellPlace::Kind::Field:
        return allocation.opcode == Opcode::NewObject && allocation.layout().index() == place.owner;
    case CellPlace::Kind::Capture:
        return allocation.opcode == Opcode::NewClosure && allocation.function().index() == place.owner;
    case CellPlace::Kind::Key:
        return allocation.opcode == Opcode::NewObject && program.at(allocation.layout()).findField(PropertyKeyID(place.owner)).has_value();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void forEachReturnedOperand(const Function& function, const Value& value, NOESCAPE const ScopedLambda<void(ValueID)>& functor)
{
    if (value.opcode == Opcode::ResultAt) {
        ValueID results = value.resultsOperand();
        const Value& producer = function.at(results);
        functor(producer.opcode == Opcode::Results ? producer.operand(value.index()) : results);
        return;
    }
    if (!canFollowResultOf(value.opcode))
        return;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandUsesOf(value.opcode, operandIndex).contains(OperandUse::Returns))
            functor(operand);
    });
}

bool mayReturnOperand(Opcode opcode)
{
    if (opcode == Opcode::ResultAt)
        return true;
    if (!canFollowResultOf(opcode))
        return false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (operandUsesOf(opcode, operandIndex).contains(OperandUse::Returns))
            return true;
    }
    return false;
}

static bool hasSourceOfUnspecifiedValues(FunctionAnalyses& analyses)
{
    const OpcodeSet& opcodes = analyses.opcodesOfValues();
    return opcodes.contains(Opcode::Unspecified) || opcodes.contains(Opcode::ResultAt);
}

static bool isSourceOfUnspecifiedValues(const Function& function, const Value& value)
{
    if (value.opcode == Opcode::Unspecified)
        return true;
    return value.opcode == Opcode::ResultAt && effectRuleOf(function.at(value.resultsOperand()).opcode) == EffectRule::Call;
}

static void forEachValueHandedOn(FunctionAnalyses& analyses, ValueID id, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    const Function& function = analyses.function();
    const Value& value = function.at(id);
    if (value.opcode != Opcode::Parameter) {
        forEachReturnedOperand(function, value, functor);
        return;
    }

    BlockID block = analyses.useLists().blockOf(id);
    uint32_t parameterIndex = static_cast<uint32_t>(function.at(block).parameters.find(id));
    for (Edge edge : analyses.cfg().incomingEdges(block)) {
        if (ValueID passed = valuePassedOn(function, edge, parameterIndex))
            functor(passed);
    }
}

UnspecifiedValues::UnspecifiedValues(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_hasSource(hasSourceOfUnspecifiedValues(analyses))
{
    if (m_hasSource)
        m_isSpecified.ensureSize(analyses.function().values.size());
}

bool UnspecifiedValues::mayContain(ValueID start)
{
    if (!m_hasSource)
        return false;

    const Function& function = m_analyses.function();
    m_visited.shrink(0);
    Vector<ValueID, 8> worklist { start };
    while (!worklist.isEmpty()) {
        ValueID id = worklist.takeLast();
        if (m_isSpecified.quickSet(id.index()))
            continue;
        m_visited.append(id);
        if (isSourceOfUnspecifiedValues(function, function.at(id))) {
            for (ValueID visited : m_visited)
                m_isSpecified.quickClear(visited.index());
            return true;
        }
        forEachValueHandedOn(m_analyses, id, [&](ValueID source) {
            worklist.append(source);
        });
    }
    return false;
}

FramePointers::FramePointers(const Function& function)
    : FramePointers(nullptr, function, FrameCells::All)
{
}

FramePointers::FramePointers(const Program& program, const Function& function, FrameCells cells)
    : FramePointers(&program, function, cells)
{
}

static bool isAmong(FrameCells cells, const Value& allocation)
{
    switch (cells) {
    case FrameCells::All:
        return true;
    case FrameCells::Arrays:
        return arrayAllocationsWithStorage.contains(allocation.opcode);
    case FrameCells::AllButArrays:
        return !arrayAllocationsWithStorage.contains(allocation.opcode);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FramePointers::FramePointers(const Program* program, const Function& function, FrameCells cells)
    : m_program(program)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (!hasFrameStorage(function.at(id)) || !isAmong(cells, function.at(id)))
                continue;
            m_allocations.append(id);
            m_values.ensureSizeAndSet(id.index(), function.values.size());
        }
    }
    if (m_allocations.isEmpty())
        return;
    AOT::sort(m_allocations);

    propagateUntilNothingChanges(function, std::views::iota(0u, function.blocks.size()), [&](BlockID, ValueID id) {
        return propagate(function, id);
    }, [&](Edge, ValueID passed, ValueID parameter) {
        return contains(passed) && add(function, parameter);
    });
}

bool FramePointers::propagate(const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    bool changed = false;
    if (Value::opcodesWithStorage.contains(value.opcode) || storesFollowedByFramePointers.contains(value.opcode)) {
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (std::optional<CellPlace> place = contains(operand) ? placeStoredTo(value, operandIndex) : std::nullopt)
                changed |= m_placesWithPointers.appendIfNotContains(*place);
        });
    }
    if (contains(id))
        return changed;

    bool mayBeFramePointer = readsFollowedByFramePointers.contains(value.opcode) && contains(cellAccessedBy(value)) && m_placesWithPointers.contains(placeReadBy(value));
    forEachReturnedOperand(function, value, [&](ValueID operand) {
        mayBeFramePointer |= contains(operand);
    });
    return (mayBeFramePointer && add(function, id)) || changed;
}

bool FramePointers::add(const Function& function, ValueID id)
{
    if (m_program && !mayBeReference(m_program->typeOf(function, id)) && !numberOfResultsOf(*m_program, function, function.at(id)))
        return false;
    return m_values.add(id.index());
}

bool FramePointers::mayHoldPointers(const Program& program, const Function& function, ValueID cell) const
{
    const Value& definition = function.at(sameObjectThroughCasts(function, cell));
    // An array holds values of the heap alone, wherever it lies.
    if (arrayAllocationsWithStorage.contains(definition.opcode))
        return false;
    // An environment holds what its fields hold, wherever the cell was read from.
    LayoutID environment = definition.opcode == Opcode::NewClosure ? LayoutID() : program.typeOf(function, cell).environment;
    return std::ranges::any_of(m_placesWithPointers, [&](const CellPlace& place) {
        if (environment)
            return place.kind == CellPlace::Kind::Field && place.owner == environment.index();
        return !Value::opcodesWithStorage.contains(definition.opcode) || cellHasPlace(program, definition, place);
    });
}

static OperandSummary summarizePublishedParameter(const Program& program, const FlowResults& flow, const Value& call, FunctionID callee, std::optional<uint32_t> entryParameter)
{
    if (!entryParameter)
        return { };

    OperandSummary summary = flow.framePointerSummary(callee, *entryParameter);
    if (summary.retainedIn != RetainedIn::Operand)
        return summary;

    if (std::optional<uint32_t> retainingOperand = findOperandPassedFor(program.at(callee), call, summary.retainingOperand)) {
        summary.retainingOperand = *retainingOperand;
        return summary;
    }
    summary.retainedIn = RetainedIn::UnknownMemory;
    summary.retainingOperand = 0;
    return summary;
}

static OperandSummary summarizeCallOperand(const Program& program, const FlowResults& flow, const Value& call, uint32_t operandIndex)
{
    CalleeList callees = calleesOfCall(program, call);
    if (!callees.isComplete || !callees.builtins.isEmpty() || maySuspend(call.opcode))
        return { };

    OperandUses declaredUses = operandUsesOf(call.opcode, operandIndex);
    bool isCalledValue = declaredUses.containsAny(callingUses);
    OperandSummary summary { isCalledValue ? declaredUses : OperandUses { OperandUse::Inspects }, RetainedIn::UnknownMemory, 0 };
    for (FunctionID id : callees.functions) {
        const Function& callee = program.at(id);
        if (!isCalledValue) {
            summary = joinOf(summary, summarizePublishedParameter(program, flow, call, id, entryParameterOf(callee, call, operandIndex)));
            continue;
        }
        for (uint32_t parameter : entryParametersOfCalledValue(callee, call))
            summary = joinOf(summary, summarizePublishedParameter(program, flow, call, id, parameter));
    }
    return summary;
}

OperandSummary summarizeFramePointerUse(const Program& program, const FlowResults* flow, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (effectRuleOf(user.opcode) == EffectRule::Call)
        return flow ? summarizeCallOperand(program, *flow, user, operandIndex) : OperandSummary();

    EffectSummaries unknownSummaries;
    return summarizeOperand(EffectContext { program, unknownSummaries }, function, user, operandIndex);
}

// Where the stub of a call with a list cannot copy the list to the stack (a hole, a long list, another storage), the run-time function of the operation makes the call.
static bool isCalledByRuntimeFunction(const Value& user, uint32_t operandIndex)
{
    return runtimeOf(user.opcode) && operandUsesOf(user.opcode, operandIndex).containsAny(callingUses);
}

bool mayLendArgument(const Value& call, uint32_t operandIndex)
{
    if (call.opcode != Opcode::Call || call.argumentsOperands().size() > numberOfArgumentGPRs)
        return false;
    std::optional<uint32_t> argument = call.variadicOperandIndex(operandIndex);
    return argument && *argument < ClosureWord::numberOfBorrowedArguments;
}

bool isLentTo(const Function& function, const Value& user, uint32_t operandIndex)
{
    if (!mayLendArgument(user, operandIndex))
        return false;
    const Value& operand = function.at(sameObjectThroughCasts(function, user.operand(operandIndex)));
    return operand.opcode == Opcode::NewClosure && operand.isLent();
}

uint8_t argumentsLentBy(const Function& function, const Value& call)
{
    uint8_t lent = 0;
    if (call.opcode != Opcode::Call)
        return lent;
    call.forEachOperand([&](ValueID, uint32_t operandIndex) {
        if (isLentTo(function, call, operandIndex))
            lent |= 1u << *call.variadicOperandIndex(operandIndex);
    });
    return lent;
}

bool isLastUse(const Function& function, const CFG& cfg, const UseLists& useLists, ValueID definition, std::span<const Use> uses, const Use& last)
{
    for (const Use& use : uses) {
        if (use != last && use.block == last.block && !useLists.isBefore(use, last))
            return false;
    }

    BlockID blockOfDefinition = useLists.blockOf(definition);
    BitVector barrier(function.blocks.size());
    barrier.quickSet(blockOfDefinition.index());
    BitVector reached(function.blocks.size());
    auto follow = [&](BlockID successor) {
        if (successor != blockOfDefinition && !reached.get(successor.index()))
            reached.merge(cfg.blocksReachableFrom(successor, barrier));
    };
    if (last.isEdgeUse())
        follow(targetOf(function, last.edge()).block);
    else {
        for (BlockID successor : cfg.successors(last.block))
            follow(successor);
    }
    return std::ranges::none_of(uses, [&](const Use& use) {
        return reached.get(use.block.index());
    });
}

bool mayLeakFramePointer(const Program& program, const FlowResults* flow, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (isLentTo(function, user, operandIndex))
        return false;
    OperandSummary summary = summarizeFramePointerUse(program, flow, function, user, operandIndex);
    if (summary.uses.containsAny(unfollowedLeakingUses))
        return true;
    bool isRetained = summary.uses.contains(OperandUse::Retains);
    if (isRetained && summary.retainedIn == RetainedIn::UnknownMemory)
        return true;
    if (summary.uses.contains(OperandUse::Returns) && !propagatesFramePointers(user.opcode))
        return true;

    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Call:
        return isRetained || isCalledByRuntimeFunction(user, operandIndex);
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return !mayLookAtFrameCells(effectRuleOf(user.opcode)) || !summary.uses.containsOnly(inlineUsesOfFrameCells);
    case EffectRule::Access:
        return provenAccessOf(program, function, user).isLookupByKey;
    case EffectRule::Declared:
    case EffectRule::Operator:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool mayLeakContents(const Program& program, const FlowResults* flow, const Function& function, const Value& user, uint32_t operandIndex)
{
    return summarizeFramePointerUse(program, flow, function, user, operandIndex).uses.containsAny(usesLeakingContents);
}

bool mayLeakFramePointer(const Program& program, const FlowResults*, const Function& function, const Terminator& user, uint32_t operandIndex)
{
    EffectSummaries unknownSummaries;
    return summarizeOperand(EffectContext { program, unknownSummaries }, function, user, operandIndex).uses.containsAny(usesLeakingFramePointers);
}

// A captured closure keeps the strict rule: its captures may be pointers into a frame, and the callee can read them.
bool mayLeakCapture(const Program& program, const FlowResults* flow, const Value& closure, uint32_t captureIndex)
{
    if (!flow)
        return true;
    OperandUses leakingUses = usesLeakingFramePointers;
    if (holdsOnlyHeapValues(program, flow, program.provenTypeOf(program.at(closure.function()).captures[captureIndex])))
        leakingUses = leakingUses - usesLeakingContents;
    return flow->framePointerSummary(closure.function(), captureIndex).uses.containsAny(leakingUses);
}

// Without flow no other function gets the environment: every capture and every operand of a call counts as leaked.
bool mayLeakStoredValue(const FlowResults* flow, FieldReference environmentField)
{
    return flow && flow->storedFramePointerUses(environmentField).containsAny(usesLeakingFramePointers);
}

bool holdsOnlyHeapValues(const Program& program, const FlowResults* flow, TypeID type)
{
    LayoutID environment = program.at(type).environment;
    if (!environment)
        return false;
    uint32_t numberOfFields = program.at(environment).fields.size();
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        if (!mayLeakStoredValue(flow, FieldReference { environment, field }))
            return false;
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)