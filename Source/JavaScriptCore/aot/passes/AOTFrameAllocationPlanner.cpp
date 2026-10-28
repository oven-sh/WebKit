/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocationPlanner.h"

#if ENABLE(AOT)

#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTCodeShapes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTFlowResults.h"
#include "AOTFunctionKinds.h"
#include "AOTHeat.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "AOTWrittenFunction.h"
#include <algorithm>
#include <ranges>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RetainedObject {
    ObjectID object;
    Use use;
};

struct PendingMove {
    ObjectID object;
    AllocationHeapCause cause { AllocationHeapCause::None };
    Use use;
};

class FramePlanner {
    WTF_MAKE_NONCOPYABLE(FramePlanner);
public:
    FramePlanner(FunctionAnalyses&, const Heat&);

    FramePlan run();

private:
    ObjectSet candidates() const { return ObjectSet(m_isCandidate); }
    ValueID allocationOf(ObjectID object) const { return m_escape.at(object).definition; }
    const Value& definitionOf(ObjectID object) const LIFETIME_BOUND { return m_function.at(allocationOf(object)); }
    bool isCandidate(ObjectID object) const { return m_isCandidate.get(object.index()); }
    bool isArray(ObjectID object) const { return arrayAllocationsWithStorage.contains(definitionOf(object).opcode); }
    bool isClosure(ObjectID object) const { return definitionOf(object).opcode == Opcode::NewClosure; }
    bool isInFrame(ObjectID object) const { return m_decisions[object.index()].storage == AllocationStorage::Frame; }
    bool mayBeInFrame(ObjectID object) const { return m_decisions[object.index()].storage != AllocationStorage::Heap; }
    bool hasOneLiveInstance(ObjectID object) const { return m_escape.liveInstancesOf(object) == LiveInstances::AtMostOne; }
    bool isLent(ObjectID object) const { return m_decisions[object.index()].isLent; }
    bool isKeptUntilItLeaves(ObjectID object) const { return isLent(object) || !m_decisions[object.index()].madeAgainAt.isEmpty(); }
    bool mayGiveFrameCell(ValueID) const;
    void moveToHeap(ObjectID, AllocationHeapCause, std::optional<Use> = std::nullopt, LimitID = LimitID::None);

    bool isHiddenRestOfEveryCallee(const Use&) const;
    void collectCandidates();
    void decideByEscapeState(ObjectID);
    bool hasUntrackedAlias(ObjectID) const;
    bool escapesOnlyIntoClosures(ObjectID) const;
    bool keepClosureUntilItLeaves(ObjectID closure);
    bool isPassedOnByBranch(const Use&) const;
    bool willHaveLongFormsInLine(BlockID);
    Expected<bool> mayFindBorrowingCallee(const Value& call, uint32_t operandIndex) const;
    bool isOnlyCalled(ObjectID closure) const;
    void checkCellForms();
    void checkUses();
    AllocationHeapCause checkUse(ObjectID, const ObjectUse&) const;
    OperandSummary summarizeUse(ValueID user, uint32_t operandIndex) const;
    bool writesOnlyInsideFrameCells(ValueID user, uint32_t operandIndex) const;
    bool mayLeakContentsThrough(const ObjectUse&) const;
    bool leaksContents(ObjectID cell) const;
    void checkUsesOfArrays();
    void checkSuspensions();
    void checkReturnCalls();
    void checkHolders();
    std::optional<ObjectID> findHolder(const Use&) const;
    bool dieTogether(ObjectID holder, ObjectID held) const;
    CellPlace placeStoredBy(const Use&) const;
    void addHolder(ObjectID holder, const Use&);
    void checkMaterialization();
    void finishPlan();

    FunctionAnalyses& m_analyses;
    const Heat& m_heat;
    BuildContext& m_build;
    const Program& m_program;
    const Function& m_function;
    const FlowResults* m_flow;
    const EscapeAnalysis& m_escape;
    BitVector m_isCandidate;
    Vector<FrameDecision> m_decisions;
    FramePlan m_plan;

    // What the callees of a call do with an operand is the same for every object that may arrive there.
    struct OperandOfCall {
        std::optional<OperandSummary> summary;
        std::optional<bool> writesOnlyInsideFrameCells;
    };
    OperandOfCall& operandOfCall(ValueID call, uint32_t operandIndex) const;
    mutable Vector<uint32_t> m_endOfOperandsOfCall;
    mutable Vector<OperandOfCall> m_operandsOfCalls;
};

} // anonymous namespace

static constexpr AbstractHeapSet heapsInsideFrameCells { AbstractHeap::ObjectFields, AbstractHeap::OrderWords, AbstractHeap::EnvironmentFields };
static constexpr OptionSet<ObjectMutation> mutationsChangingFrameCells {
    ObjectMutation::DeleteField,
    ObjectMutation::RedefineProperty,
    ObjectMutation::AddProperty,
    ObjectMutation::ChangeIntegrity,
    ObjectMutation::ChangePrototype,
    ObjectMutation::StoreOfOtherType,
};
static constexpr OperandUses usesChangingFrameCells {
    OperandUse::MovesElements,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::DefinesName,
    OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity,
    OperandUse::ChangesPrototype,
};
static constexpr OperandUses usesOfSharedLists { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::RetainsContents };

static LimitID reachedLimitOf(const EscapeAnalysis& escape)
{
    return escape.gaveUp() ? LimitID::MaxEscapeAnalysisWork : LimitID::None;
}

static bool mayChangeForm(const Program& program, const FlowResults* flow, const Value& allocation)
{
    if (allocation.opcode != Opcode::NewObject)
        return false;
    const Layout& layout = program.at(allocation.layout());
    if (layout.isEnvironment())
        return false;
    AllocationClassID allocationClass = allocation.allocationClass();
    OptionSet<ObjectMutation> impossibleMutations = flow && allocationClass ? flow->at(allocationClass).impossibleMutations : layout.flow.impossibleMutations;
    return !impossibleMutations.containsAll(mutationsChangingFrameCells);
}

static bool canRetainInitialValue(const Program& program, const FlowResults* flow, const Value& cell, uint32_t operandIndex)
{
    std::optional<CellPlace> place = placeStoredTo(cell, operandIndex);
    if (!place || !Value::opcodesWithStorage.contains(cell.opcode))
        return false;
    if (place->kind == CellPlace::Kind::Capture)
        return !mayLeakCapture(program, flow, cell, place->index);
    return program.at(cell.layout()).kind == LayoutKind::Environment && !mayLeakStoredValue(flow, FieldReference { cell.layout(), place->index });
}

// The closure that such a field makes on demand is a cell of the heap. It holds the object, or the environment that the object keeps for its closures.
static bool defersClosureWithCapture(ClosureDeferral deferral)
{
    switch (deferral) {
    case ClosureDeferral::Unknown:
    case ClosureDeferral::CapturesNothing:
        return false;
    case ClosureDeferral::CapturesObject:
    case ClosureDeferral::CapturesEnvironment:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isEnvironmentOfDeferredClosures(const Program& program, const Value& user, uint32_t operandIndex)
{
    if (user.opcode != Opcode::NewObject)
        return false;
    std::optional<uint32_t> field = program.at(user.layout()).flow.closureEnvironmentField;
    std::optional<CellPlace> place = placeStoredTo(user, operandIndex);
    return field && place && place->index == *field;
}

// The slot of a field that leaves a closure unmade is a narrow reference, which reaches no frame.
static bool isValueOfDeferringField(const Program& program, const Value& user, uint32_t operandIndex)
{
    auto defersClosure = [&](LayoutID layout, uint32_t field) {
        return flowOf(program, FieldReference { layout, field }).location.deferral != ClosureDeferral::Unknown;
    };
    if (user.opcode == Opcode::NewObject) {
        std::optional<CellPlace> place = placeStoredTo(user, operandIndex);
        return place && defersClosure(user.layout(), place->index);
    }
    if ((user.opcode != Opcode::PutSlot && user.opcode != Opcode::PutSlotOrAdd) || operandIndex != OperandIndex::PutSlot::value)
        return false;
    return std::ranges::any_of(program.at(user.receivers()).completeMembers(), [&](LayoutID member) {
        std::optional<uint32_t> field = program.at(member).findField(user.key());
        return field && defersClosure(member, *field);
    });
}

// A validating build reads the header of every object and closure that a proof names, and a cell of a frame may have none. An array has its header wherever it lies.
static bool staysOnHeapForValidation(const Program& program, const BuildContext& build, const Value& allocation)
{
    bool readsHeaders = build.validates() || build.recordedOptions().has(TestingSwitch::CheckProofsAtRunTime);
    if (!readsHeaders || arrayAllocationsWithStorage.contains(allocation.opcode))
        return false;
    return allocation.opcode != Opcode::NewObject || !program.at(allocation.layout()).isEnvironment();
}

// A conversion, a test against `prototype` and the like read members of the function, which makes it an object with properties.
static bool treatsOperandAsObject(Opcode opcode, uint32_t operandIndex)
{
    OperandBehavior behavior = declaredBehaviorOf(opcode, operandIndex);
    return !behavior.protocols.isEmpty() || !behavior.namedMembers.empty() || !behavior.calledMembers.empty();
}

static FrameDecision createHeapDecision(const EscapeAnalysis& escape, ValueID allocation)
{
    FrameDecision decision { .allocation = allocation, .cause = AllocationHeapCause::HasNoFrameForm, .limit = reachedLimitOf(escape) };
    if (std::optional<ObjectID> object = escape.objectDefinedBy(allocation)) {
        decision.forcedBy = escape.at(*object).firstEscape;
        decision.reasons = escape.at(*object).reasons;
    }
    return decision;
}

void FramePlan::add(const Program& program, const Value& allocation, AllocationStorage storage, bool isLent, bool isMadeAgain)
{
    bool isInFrame = storage == AllocationStorage::Frame;
    if (arrayAllocationsWithStorage.contains(allocation.opcode)) {
        if (isInFrame)
            count(allocation.opcode == Opcode::CreateRest ? StatisticID::RestArraysInFrames : StatisticID::ArraysInFrames);
        return;
    }
    if (allocation.opcode == Opcode::NewClosure) {
        closures.append({ allocation.function(), storage });
        if (isLent || isMadeAgain)
            closures.append({ allocation.function(), AllocationStorage::Heap });
        if (isInFrame)
            count(StatisticID::ClosuresInFrames);
        if (isLent)
            count(StatisticID::ClosuresLent);
        return;
    }

    cells.append({ allocation.layout(), storage });
    if (program.at(allocation.layout()).kind != LayoutKind::Environment) {
        if (isInFrame)
            count(StatisticID::ObjectsInFrames);
        return;
    }

    switch (storage) {
    case AllocationStorage::Heap:
        count(StatisticID::EnvironmentsOnHeap);
        return;
    case AllocationStorage::Frame:
        count(StatisticID::EnvironmentsInFrames);
        return;
    case AllocationStorage::FrameUntilMaterialized:
        count(StatisticID::EnvironmentsMaterializedLate);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasStorageChoice(const Value& value)
{
    return Value::opcodesWithStorage.contains(value.opcode);
}

bool isEmptyArrayLiteral(const Value& value)
{
    return value.opcode == Opcode::NewArray && value.elementsOperands().empty() && value.holes().empty() && !value.capacityOperand();
}

FramePlan planFrame(FunctionAnalyses& analyses, const Heat& heat)
{
    return FramePlanner(analyses, heat).run();
}

FramePlanner::FramePlanner(FunctionAnalyses& analyses, const Heat& heat)
    : m_analyses(analyses)
    , m_heat(heat)
    , m_build(analyses.context().build)
    , m_program(analyses.program())
    , m_function(analyses.function())
    , m_flow(analyses.context().flow)
    , m_escape(analyses.escapeWithLiveInstances())
    , m_isCandidate(m_escape.numberOfObjects())
    , m_decisions(FillWith { }, m_escape.numberOfObjects(), FrameDecision { .cause = AllocationHeapCause::NotTracked })
{
    m_plan.function = analyses.context().id;
}

FramePlan FramePlanner::run()
{
    collectCandidates();
    if (!m_isCandidate.isEmpty()) {
        for (ObjectID object : candidates())
            decideByEscapeState(object);
        checkCellForms();
        checkUses();
        checkUsesOfArrays();
        checkSuspensions();
        checkReturnCalls();
        checkHolders();
        checkMaterialization();
        for (ObjectID object : candidates()) {
            if (!mayBeInFrame(object) && (isOnlyCalled(object) || isLikeEveryClosureOfItsFunction(m_program, m_flow, m_function, definitionOf(object))))
                m_decisions[object.index()].isHeldByClosureTable = true;
        }
        m_plan.demotedReturnCalls.removeAllMatching([&](BlockID block) {
            bool passesCellOfFrame = false;
            m_function.at(m_function.at(block).terminator.callOperand()).forEachOperand([&](ValueID operand, uint32_t) {
                passesCellOfFrame |= mayGiveFrameCell(operand);
            });
            return !passesCellOfFrame;
        });
    }
    finishPlan();
    return WTF::move(m_plan);
}

bool FramePlanner::mayGiveFrameCell(ValueID value) const
{
    for (ObjectID object : m_escape.pointsTo(value).objects) {
        if (isCandidate(object) && mayBeInFrame(object))
            return true;
    }
    return false;
}

void FramePlanner::moveToHeap(ObjectID object, AllocationHeapCause cause, std::optional<Use> use, LimitID limit)
{
    FrameDecision& decision = m_decisions[object.index()];
    if (decision.storage == AllocationStorage::Heap)
        return;
    decision.storage = AllocationStorage::Heap;
    decision.isLent = false;
    decision.madeAgainAt.clear();
    decision.cause = cause;
    decision.forcedBy = use;
    decision.reasons = m_escape.at(object).reasons;
    decision.limit = limit;
}

bool FramePlanner::isHiddenRestOfEveryCallee(const Use& use) const
{
    if (use.kind != Use::Kind::Operand)
        return false;
    const Value& call = m_function.at(use.user);
    if (effectRuleOf(call.opcode) != EffectRule::Call)
        return false;
    CalleeList callees = calleesOfCall(m_program, call);
    std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone();
    if (!functions)
        return false;
    return std::ranges::all_of(functions->value(), [&](FunctionID id) {
        const Function& callee = m_program.at(id);
        std::optional<uint32_t> entryParameter = entryParameterOf(callee, call, use.operandIndex);
        return entryParameter && *entryParameter == callee.hiddenRestParameterIndex();
    });
}

void FramePlanner::collectCandidates()
{
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            std::optional<ObjectID> object = hasStorageChoice(value) ? m_escape.objectDefinedBy(id) : std::nullopt;
            if (!object)
                continue;
            // The language has no name for the list of surplus arguments, so nothing tells one empty list from another.
            bool isOnlyReadAsHiddenRest = isEmptyArrayLiteral(value) && m_escape.at(*object).uses.containsOnly(usesOfSharedLists) && std::ranges::all_of(m_escape.usesOf(*object), [&](const ObjectUse& entry) {
                return isHiddenRestOfEveryCallee(entry.use);
            });
            if (isOnlyReadAsHiddenRest) {
                m_plan.emptyArgumentLists.append(id);
                m_plan.count(StatisticID::EmptyArgumentListsShared);
                continue;
            }
            if (value.storage() != AllocationStorage::Heap || staysOnHeapForValidation(m_program, m_build, value))
                continue;
            if (arrayAllocationsWithStorage.contains(value.opcode) && !canLieInFrame(m_program, m_build, value))
                continue;
            m_isCandidate.quickSet(object->index());
            m_decisions[object->index()] = FrameDecision { .allocation = id, .storage = AllocationStorage::Frame };
        }
    }
}

// Whether an array escapes is decided use by use: the summaries of the callees know no path that an array in a frame never takes.
void FramePlanner::decideByEscapeState(ObjectID object)
{
    const TrackedObject& tracked = m_escape.at(object);
    switch (tracked.state) {
    case EscapeState::Unknown:
        moveToHeap(object, AllocationHeapCause::NotTracked, std::nullopt, reachedLimitOf(m_escape));
        return;
    case EscapeState::Escapes:
        if (isArray(object))
            break;
        if (hasOneLiveInstance(object) && escapesOnlyIntoClosures(object))
            m_decisions[object.index()].storage = AllocationStorage::FrameUntilMaterialized;
        else if (!keepClosureUntilItLeaves(object))
            moveToHeap(object, AllocationHeapCause::Escapes, tracked.firstEscape);
        return;
    case EscapeState::DoesNotEscape:
        break;
    }
    RELEASE_ASSERT(tracked.state == EscapeState::DoesNotEscape || isArray(object));
    if (!hasOneLiveInstance(object))
        moveToHeap(object, AllocationHeapCause::SeveralLiveInstances);
    else if (!isArray(object) && hasUntrackedAlias(object))
        moveToHeap(object, AllocationHeapCause::HasUntrackedAlias);
}

bool FramePlanner::hasUntrackedAlias(ObjectID object) const
{
    return std::ranges::any_of(m_escape.aliasesOf(object), [&](ValueID alias) {
        return !m_program.typeOf(m_function, alias).isOpaque(OpaqueKind::Guard) && !propagatesFramePointers(m_function.at(alias).opcode);
    });
}

bool FramePlanner::escapesOnlyIntoClosures(ObjectID object) const
{
    const Value& definition = definitionOf(object);
    if (definition.opcode != Opcode::NewObject || m_program.at(definition.layout()).kind != LayoutKind::Environment)
        return false;
    return std::ranges::all_of(m_escape.usesOf(object), [&](const ObjectUse& entry) {
        return !entry.isEscape || (entry.use.kind == Use::Kind::Operand && m_function.at(entry.use.user).opcode == Opcode::NewClosure);
    }) && std::ranges::all_of(m_escape.containersOf(object), [&](ObjectID container) {
        return isClosure(container);
    }) && std::ranges::all_of(m_escape.aliasesOf(object), [&](ValueID alias) {
        return m_analyses.sameObjectThroughCasts(alias) == allocationOf(object);
    });
}

// Flow has followed the closures of the function to every place that they reach.
bool isLikeEveryClosureOfItsFunction(const Program& program, const FlowResults* flow, const Function& maker, const Value& closure)
{
    if (!flow || closure.opcode != Opcode::NewClosure || !closure.capturesOperands().empty() || closure.isMadeOnDemand())
        return false;
    // The code of a module runs once. There the closure of the table is no closure less, and it is made by the run time.
    if (maker.kind == FunctionKind::Module || maker.kind == FunctionKind::CommonJsModule)
        return false;
    FunctionID function = closure.function();
    // What `new` makes has the prototype of the closure that made it.
    if (program.at(function).has(FunctionFlags::TakesCallee) || program.at(function).constructorForm() != ConstructorForm::NotConstructor)
        return false;
    return flow->function(function).closureUse == ClosureUse::CalledOnly && flow->closuresAreNeverCompared(function);
}

static AllocationHeapCause checkClosureForm(const Program& program, const Value& closure)
{
    const Function& callee = program.at(closure.function());
    if (closure.isMadeOnDemand())
        return AllocationHeapCause::LayoutDefersClosures;
    if (callee.has(FunctionFlags::TakesCallee) || !callee.hasOrdinaryFrame())
        return AllocationHeapCause::ClosureIsNotOnlyCalled;
    return AllocationHeapCause::None;
}

// Without captures one closure of a function does what any other does, and the function does not get hold of the closure it is entered through.
static bool isHeldByClosureTableWhereOnlyCalled(const Program& program, const Value& allocation)
{
    return allocation.opcode == Opcode::NewClosure && allocation.capturesOperands().empty() && checkClosureForm(program, allocation) == AllocationHeapCause::None;
}

bool FramePlanner::isOnlyCalled(ObjectID closure) const
{
    if (!isHeldByClosureTableWhereOnlyCalled(m_program, definitionOf(closure)))
        return false;
    if (m_escape.at(closure).state != EscapeState::DoesNotEscape || hasUntrackedAlias(closure))
        return false;
    return std::ranges::all_of(m_escape.usesOf(closure), [&](const ObjectUse& entry) {
        if (entry.use.kind != Use::Kind::Operand)
            return false;
        const Value& user = m_function.at(entry.use.user);
        if (user.operandKind(entry.use.operandIndex) == OperandKind::Guard)
            return true;
        if (user.opcode == Opcode::Call)
            return entry.use.operandIndex == OperandIndex::Call::callee;
        return user.opcode == Opcode::TypeTest || user.opcode == Opcode::ToBoolean;
    });
}

AOT_THRESHOLD(unsigned, maxUsesOfClosureKeptUntilItLeaves, 8);

bool FramePlanner::willHaveLongFormsInLine(BlockID block)
{
    Expected<BlockFrequency> frequency = m_function.at(block).frequency;
    if (frequency.predicted() != BlockFrequency::Normal)
        return false;
    Expected<double> activations = m_heat.activationsOf(m_program, m_plan.function);
    if (codeShapeFor(activations * roundingOfFrequencySums, false, frequency) != CodeShape::Fast && runsNoBlockMoreOftenThanEntry(m_analyses.cfg()))
        return false;
    if (m_analyses.executionCountOf(block) == ExecutionCount::AtMostOnce)
        return false;
    Expected<double> frequencyInFunction = m_analyses.blockFrequencies().frequencyOf(block);
    Expected<double> heat = activations * frequencyInFunction;
    return codeShapeFor(heat, false, frequency, m_heat.leastHeatOfFastCode(m_program, m_plan.function, frequencyInFunction)) == CodeShape::Fast && m_heat.isAmongHottestCode(heat).predicted();
}

bool FramePlanner::isPassedOnByBranch(const Use& use) const
{
    return !findNarrowedParameters(m_function, m_analyses.useLists(), use).isEmpty();
}

static bool mayFollow(const Function& function, const CFG& cfg, const UseLists& useLists, ValueID definition, const Use& earlier, const Use& later)
{
    if (earlier.block == later.block && useLists.isBefore(earlier, later))
        return true;
    BlockID blockOfDefinition = useLists.blockOf(definition);
    BitVector barrier(function.blocks.size());
    barrier.quickSet(blockOfDefinition.index());
    return std::ranges::any_of(cfg.successors(earlier.block), [&](BlockID successor) {
        return successor != blockOfDefinition && cfg.blocksReachableFrom(successor, barrier).get(later.block.index());
    });
}

// Whoever borrows the closure cannot tell it from another one afterwards.
bool FramePlanner::keepClosureUntilItLeaves(ObjectID closure)
{
    ValueID definition = allocationOf(closure);
    if (!m_flow || !isClosure(closure))
        return false;
    const UseLists& useLists = m_analyses.useLists();
    std::span<const Use> uses = useLists.usesOf(definition);
    if (uses.empty() || uses.size() > maxUsesOfClosureKeptUntilItLeaves)
        return false;

    BlockID blockOfDefinition = useLists.blockOf(definition);
    if (!willHaveLongFormsInLine(blockOfDefinition))
        return false;
    std::span<const ObjectUse> knownUses = m_escape.usesOf(closure);
    // A stack trace lists a call under the name of the function object that ran, and what leaves is another function object.
    bool isListedUnderItsOwnName = m_program.at(writtenFunctionOf(m_program, m_function.at(definition).function())).has(FunctionCapabilities::RecordsCalleeOnceRenamed);
    bool isLent = false;
    bool isBorrowed = false;
    Vector<Use, 2> usesThatRunIt;
    Vector<Use, 2> lastUses;
    Vector<Use, 1> madeAgainAt;
    for (const Use& use : uses) {
        if (use.kind == Use::Kind::HandlerArgument || use.kind == Use::Kind::BlockSubject)
            return false;
        auto known = std::ranges::find(knownUses, use, &ObjectUse::use);
        // A use that keeps the closure in a cell, gives it back or passes it on makes another way to it, which is not among the uses of the value.
        bool staysInFrame = use.kind == Use::Kind::Operand && known != knownUses.end() && (!known->isEscape || effectRuleOf(m_function.at(use.user).opcode) == EffectRule::Call) && !isPassedOnByBranch(use);
        if (staysInFrame && known->uses.containsOnly(usesOfBorrowedParameter) && checkUse(closure, *known) == AllocationHeapCause::None) {
            if (isListedUnderItsOwnName && known->uses.containsAny({ OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly }))
                usesThatRunIt.append(use);
            isBorrowed = true;
            continue;
        }
        lastUses.append(use);
        bool runsOften = m_function.at(use.block).frequency.predicted() == BlockFrequency::Normal;
        if (runsOften && use.kind == Use::Kind::Operand && mayLendArgument(m_function.at(use.user), use.operandIndex) && mayFindBorrowingCallee(m_function.at(use.user), use.operandIndex).predicted()) {
            isLent = true;
            continue;
        }
        // A use on the way that every run takes from where the closure is made would only add the closure of the frame to the one of the heap.
        bool liesOffTheWay = use.block != blockOfDefinition || (use.kind == Use::Kind::EdgeArgument && m_function.at(use.block).terminator.targets.size() > 1);
        if (!liesOffTheWay)
            return false;
        madeAgainAt.append(use);
    }
    if (!isLent && !isBorrowed)
        return false;
    for (const Use& earlier : usesThatRunIt) {
        for (const Use& later : lastUses) {
            if (mayFollow(m_function, m_analyses.cfg(), useLists, definition, earlier, later))
                return false;
        }
    }
    if (isLent && closureFormOf(m_program, *m_flow, m_function.at(definition).function()) != ClosureForm::Slim)
        return false;
    for (const Use& use : lastUses) {
        if (!isLastUse(m_function, m_analyses.cfg(), useLists, definition, uses, use))
            return false;
    }

    FrameDecision& decision = m_decisions[closure.index()];
    decision.isLent = isLent;
    decision.madeAgainAt = WTF::move(madeAgainAt);
    return true;
}

// Whoever is lent a closure and does not borrow it moves it to the heap.
Expected<bool> FramePlanner::mayFindBorrowingCallee(const Value& call, uint32_t operandIndex) const
{
    auto borrows = [&](FunctionID id) {
        const Function& callee = m_program.at(id);
        if (std::optional<uint32_t> entryParameter = entryParameterOf(callee, call, operandIndex))
            return m_flow->framePointerSummary(id, *entryParameter).uses.containsOnly(usesOfBorrowedParameter);
        // Nothing takes an argument that has no parameter.
        std::optional<uint32_t> argument = call.variadicOperandIndex(operandIndex);
        return argument && *argument >= callee.numberOfPositionalParameters() && !callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter });
    };
    CalleeList callees = calleesOfCall(m_program, call);
    Callees seen = callees.seen().predicted();
    if (std::ranges::any_of(seen.functions, borrows))
        return true;
    if (!callees.listsEveryCallee())
        return seen.functions.empty();

    const BuiltinTable& table = m_program.builtins();
    return std::ranges::any_of(seen.builtins, [&](BuiltinID builtin) {
        return std::ranges::any_of(table.overloadsOf(builtin), [&](const BuiltinOverload& overload) {
            const RuntimeFunctionEntry* entry = overload.code.implementation.function ? findRuntimeFunction(m_program, overload.code.implementation.function) : nullptr;
            return entry && borrows(entry->function);
        });
    });
}

static AllocationHeapCause checkObjectForm(const Program& program, const Value& object)
{
    LayoutID id = object.layout();
    const Layout& layout = program.at(id);
    switch (layout.kind) {
    case LayoutKind::Environment:
        return AllocationHeapCause::None;
    case LayoutKind::CoroutineFrame:
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return AllocationHeapCause::Escapes;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        break;
    }

    if (object.prototypeOperand() || layout.has(LayoutFlags::IsOfCreatedObject))
        return AllocationHeapCause::ChangedByUse;
    for (uint32_t field = 0; field < layout.fields.size(); ++field) {
        if (defersClosureWithCapture(flowOf(program, FieldReference { id, field }).location.deferral))
            return AllocationHeapCause::LayoutDefersClosures;
    }
    if (layout.flow.fieldCreation != FieldCreation::OnlyAtConstruction)
        return AllocationHeapCause::FieldsCreatedLater;
    return AllocationHeapCause::None;
}

static AllocationHeapCause checkCellForm(const Program& program, const Value& allocation)
{
    if (allocation.opcode != Opcode::NewClosure)
        return checkObjectForm(program, allocation);
    // The list of recorded callees holds the closure that the function is entered through, and the collector visits the list. Where it is the run time that records, once a closure of the function was renamed, no closure that lies in a frame is written into the list.
    const Function& callee = program.at(allocation.function());
    if (callee.has(FunctionCapabilities::RecordsCallee) && !callee.has(FunctionCapabilities::RecordsCalleeOnceRenamed))
        return AllocationHeapCause::ClosureIsNotOnlyCalled;
    return checkClosureForm(program, allocation);
}

static bool fitsInFrame(const Program& program, const BuildContext& build, const Value& allocation)
{
    return allocation.opcode != Opcode::NewObject || program.at(allocation.layout()).fields.size() <= build.limit(LimitID::MaxFrameEnvironmentFields);
}

void FramePlanner::checkCellForms()
{
    for (ObjectID object : candidates()) {
        if (!mayBeInFrame(object) || isArray(object))
            continue;
        const Value& allocation = definitionOf(object);
        if (AllocationHeapCause cause = checkCellForm(m_program, allocation); cause != AllocationHeapCause::None)
            moveToHeap(object, cause);
        else if (!fitsInFrame(m_program, m_build, allocation))
            moveToHeap(object, AllocationHeapCause::ExceedsLimit, std::nullopt, m_build.gaveUp(LimitID::MaxFrameEnvironmentFields, m_analyses.subject(allocationOf(object))).limit);
    }
}

// checkCellForms() gives up on an object that does not fit, which is recorded, and checkUses() moves every other object that may change its form.
bool staysOnHeapByForm(const Program& program, const FlowResults& flow, const BuildContext& build, const Function& maker, const Value& allocation)
{
    RELEASE_ASSERT(allocation.opcode == Opcode::NewClosure || allocation.opcode == Opcode::NewObject);
    // isOnlyCalled() goes by the uses.
    if (isHeldByClosureTableWhereOnlyCalled(program, allocation) && !isLikeEveryClosureOfItsFunction(program, &flow, maker, allocation))
        return false;
    return checkCellForm(program, allocation) != AllocationHeapCause::None || (fitsInFrame(program, build, allocation) && mayChangeForm(program, &flow, allocation));
}

void FramePlanner::checkUses()
{
    for (ObjectID object : candidates()) {
        if (!mayBeInFrame(object) || isArray(object) || isKeptUntilItLeaves(object))
            continue;
        for (const ObjectUse& entry : m_escape.usesOf(object)) {
            AllocationHeapCause cause = checkUse(object, entry);
            if (cause == AllocationHeapCause::None)
                continue;
            moveToHeap(object, cause, entry.use);
            break;
        }
        if (mayChangeForm(m_program, m_flow, definitionOf(object)))
            moveToHeap(object, AllocationHeapCause::ChangedByUse);
    }
}

AllocationHeapCause FramePlanner::checkUse(ObjectID object, const ObjectUse& entry) const
{
    if (entry.use.kind != Use::Kind::Operand)
        return AllocationHeapCause::None;

    const Value& user = m_function.at(entry.use.user);
    uint32_t operandIndex = entry.use.operandIndex;
    EffectRule rule = effectRuleOf(user.opcode);
    if (user.operandKind(operandIndex) == OperandKind::Guard)
        return AllocationHeapCause::None;
    if (isEnvironmentOfDeferredClosures(m_program, user, operandIndex) || isValueOfDeferringField(m_program, user, operandIndex))
        return AllocationHeapCause::HeldByHeapCell;
    if (rule == EffectRule::Access && provenAccessOf(m_program, m_function, user).isLookupByKey)
        return AllocationHeapCause::SeenByNativeCode;

    if (!mayLookAtFrameCells(rule) && rule == EffectRule::Builtin && entry.uses.containsOnly(inlineUsesOfFrameCells))
        return AllocationHeapCause::SeenByNativeCode;
    OperandSummary summary = summarizeUse(entry.use.user, operandIndex);
    if (mayLeakFramePointer(m_program, m_function, user, operandIndex, summary)) {
        bool isDecidedByHolder = isInFrame(object)
            && entry.uses.contains(OperandUse::Retains)
            && summary.retainedIn != RetainedIn::UnknownMemory;
        if (!isDecidedByHolder)
            return AllocationHeapCause::Escapes;
    }
    if (entry.uses.containsAny(usesChangingFrameCells))
        return AllocationHeapCause::ChangedByUse;

    const Value& allocation = definitionOf(object);
    bool isClosure = allocation.opcode == Opcode::NewClosure;
    bool isEnvironment = !isClosure && m_program.at(allocation.layout()).isEnvironment();
    // What is read out of an environment that never holds a pointer into a frame may go anywhere.
    if (mayLeakContentsThrough(entry) && !holdsOnlyHeapValues(m_program, m_flow, allocation.type)) {
        // What the cell holds goes to the heap in checkHolders(). The verifier knows what an operand holds only where it sees the allocation, and of an environment by its layout alone.
        if (isEnvironment || m_analyses.sameObjectThroughCasts(user.operand(operandIndex)) != allocationOf(object))
            return AllocationHeapCause::Escapes;
    }
    switch (rule) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
        if (!isClosure || !mayLookAtFrameCells(rule) || !entry.uses.containsOnly(inlineUsesOfFrameCells))
            return AllocationHeapCause::SeenByNativeCode;
        break;
    case EffectRule::Access:
        if (isClosure && receiverOf(user.opcode) == operandIndex)
            return AllocationHeapCause::SeenByNativeCode;
        break;
    case EffectRule::Declared:
    case EffectRule::Operator:
        if (isClosure && treatsOperandAsObject(user.opcode, operandIndex))
            return AllocationHeapCause::SeenByNativeCode;
        break;
    case EffectRule::Call:
        break;
    }
    if (isClosure || isEnvironment)
        return AllocationHeapCause::None;

    return writesOnlyInsideFrameCells(entry.use.user, operandIndex) ? AllocationHeapCause::None : AllocationHeapCause::ChangedByUse;
}

FramePlanner::OperandOfCall& FramePlanner::operandOfCall(ValueID call, uint32_t operandIndex) const
{
    if (m_endOfOperandsOfCall.isEmpty())
        m_endOfOperandsOfCall.fill(0, m_function.values.size());
    uint32_t& end = m_endOfOperandsOfCall[call.index()];
    uint32_t numberOfOperands = m_function.at(call).numberOfOperands();
    if (!end) {
        m_operandsOfCalls.grow(m_operandsOfCalls.size() + numberOfOperands);
        end = m_operandsOfCalls.size();
    }
    return m_operandsOfCalls[end - numberOfOperands + operandIndex];
}

OperandSummary FramePlanner::summarizeUse(ValueID id, uint32_t operandIndex) const
{
    const Value& user = m_function.at(id);
    if (effectRuleOf(user.opcode) != EffectRule::Call)
        return summarizeFramePointerUse(m_program, m_flow, m_function, user, operandIndex);
    std::optional<OperandSummary>& summary = operandOfCall(id, operandIndex).summary;
    if (!summary) {
        summary = summarizeFramePointerUse(m_program, m_flow, m_function, user, operandIndex);
        m_build.statistics().count(StatisticID::CallOperandsSummarizedForFrames);
    }
    return *summary;
}

bool FramePlanner::writesOnlyInsideFrameCells(ValueID id, uint32_t operandIndex) const
{
    const Value& user = m_function.at(id);
    auto ask = [&] {
        return memoryAccessedThrough(m_analyses.effectContext(), m_function, user, operandIndex, m_analyses.effectsOf(id)).writes.containsOnly(heapsInsideFrameCells);
    };
    if (effectRuleOf(user.opcode) != EffectRule::Call)
        return ask();
    std::optional<bool>& answer = operandOfCall(id, operandIndex).writesOnlyInsideFrameCells;
    if (!answer) {
        answer = ask();
        m_build.statistics().count(StatisticID::CallOperandsSummarizedForFrames);
    }
    return *answer;
}

bool FramePlanner::mayLeakContentsThrough(const ObjectUse& entry) const
{
    if (entry.use.kind != Use::Kind::Operand)
        return false;
    const Value& user = m_function.at(entry.use.user);
    return user.operandKind(entry.use.operandIndex) != OperandKind::Guard && summarizeUse(entry.use.user, entry.use.operandIndex).uses.containsAny(usesLeakingContents);
}

bool FramePlanner::leaksContents(ObjectID cell) const
{
    return std::ranges::any_of(m_escape.usesOf(cell), [&](const ObjectUse& entry) {
        return mayLeakContentsThrough(entry);
    });
}

static AllocationHeapCause checkUseOfArray(const Program& program, const FlowResults* flow, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (mayBeGivenFrameArray(program, flow, function, user, operandIndex))
        return AllocationHeapCause::None;
    if (user.operandUses(operandIndex).containsAny(operandWritingUses))
        return AllocationHeapCause::ChangedByUse;
    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
    case EffectRule::Access:
        return AllocationHeapCause::SeenByNativeCode;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Call:
        return AllocationHeapCause::Escapes;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Follows the arrays through casts and block parameters, and names every use that an array in a frame may not have: functor(the arrays it may be, one bit each, the cause, the use).
static void forEachUseThatKeepsArraysOnHeap(const Program& program, const FlowResults* flow, const Function& function, std::span<const ValueID> arrays, NOESCAPE const Invocable<void(uint32_t, AllocationHeapCause, const Use&)> auto& functor)
{
    RELEASE_ASSERT(arrays.size() <= maxFollowedArrays);
    Vector<uint32_t> mayBe(FillWith { }, function.values.size(), 0);
    for (unsigned index = 0; index < arrays.size(); ++index)
        mayBe[arrays[index].index()] = 1u << index;

    auto add = [&](ValueID id, ValueID given) {
        uint32_t before = mayBe[id.index()];
        mayBe[id.index()] |= mayBe[given.index()];
        return mayBe[id.index()] != before;
    };
    propagateUntilNothingChanges(function, std::views::iota(0u, function.blocks.size()), [&](BlockID, ValueID id) {
        bool changed = false;
        forEachReturnedOperand(function, function.at(id), [&](ValueID operand) {
            changed |= add(id, operand);
        });
        return changed;
    }, [&](Edge, ValueID passed, ValueID parameter) {
        return add(parameter, passed);
    });

    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID id : function.at(block).values) {
            const Value& user = function.at(id);
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                uint32_t given = mayBe[operand.index()];
                if (!given)
                    return;
                AllocationHeapCause cause = checkUseOfArray(program, flow, function, user, operandIndex);
                if (cause != AllocationHeapCause::None)
                    functor(given, cause, Use { .kind = Use::Kind::Operand, .block = block, .user = id, .operandIndex = operandIndex });
            });
        }
        const Terminator& terminator = function.at(block).terminator;
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (uint32_t given = mayBe[operand.index()]; given && terminator.operandKind(operandIndex) != OperandKind::Guard)
                functor(given, AllocationHeapCause::Escapes, Use { .kind = Use::Kind::TerminatorOperand, .block = block, .operandIndex = operandIndex });
        });
    }
}

bool mayPutSomeArrayInFrame(const Program& program, const FlowResults& flow, const BuildContext& build, const Function& function)
{
    Vector<ValueID, 8> arrays = findArraysThatMayLieInFrame(program, build, function);
    if (arrays.isEmpty())
        return false;
    arrays.shrink(std::min<size_t>(arrays.size(), maxFollowedArrays));

    uint32_t onHeap = 0;
    forEachUseThatKeepsArraysOnHeap(program, &flow, function, arrays.span(), [&](uint32_t given, AllocationHeapCause, const Use&) {
        onHeap |= given;
    });
    return onHeap != (arrays.size() == 32 ? std::numeric_limits<uint32_t>::max() : (1u << arrays.size()) - 1);
}

void FramePlanner::checkUsesOfArrays()
{
    Vector<ObjectID, 8> arrays;
    Vector<ValueID, 8> definitions;
    for (ObjectID array : candidates()) {
        if (!isArray(array) || !mayBeInFrame(array))
            continue;
        if (arrays.size() == maxFollowedArrays) {
            moveToHeap(array, AllocationHeapCause::NotTracked);
            continue;
        }
        arrays.append(array);
        definitions.append(allocationOf(array));
    }
    if (arrays.isEmpty())
        return;

    forEachUseThatKeepsArraysOnHeap(m_program, m_flow, m_function, definitions.span(), [&](uint32_t given, AllocationHeapCause cause, const Use& use) {
        for (unsigned index = 0; index < arrays.size(); ++index) {
            if (given >> index & 1)
                moveToHeap(arrays[index], cause, use);
        }
    });
}

void FramePlanner::checkSuspensions()
{
    CFG& cfg = m_analyses.cfg();
    auto isSuspensionPoint = [&](ValueID id) {
        return maySuspend(m_function.at(id).opcode) && m_analyses.maySuspend(id);
    };

    Vector<BlockID, 4> suspendingBlocks;
    for (BlockID block : cfg.reversePostOrder()) {
        if (std::ranges::any_of(m_function.at(block).values, isSuspensionPoint))
            suspendingBlocks.append(block);
    }
    if (suspendingBlocks.isEmpty())
        return;

    BitVector aliases(m_function.values.size());
    for (ObjectID object : candidates()) {
        if (!mayBeInFrame(object))
            continue;
        for (ValueID alias : m_escape.aliasesOf(object))
            aliases.quickSet(alias.index());
    }

    Liveness liveness(m_function, cfg, m_analyses.useLists(), { .trackedValues = &aliases });
    for (BlockID block : suspendingBlocks) {
        Liveness::LocalCalc liveBeforeValue(liveness, block);
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = values.size(); index--;) {
            liveBeforeValue.execute(index);
            if (!isSuspensionPoint(values[index]))
                continue;
            Use suspension { .kind = Use::Kind::Operand, .block = block, .user = values[index] };
            liveBeforeValue.forEachLive([&](ValueID live) {
                for (ObjectID object : m_escape.pointsTo(live).objects)
                    moveToHeap(object, AllocationHeapCause::LiveAcrossSuspension, suspension);
            });
        }
    }
}

void FramePlanner::checkReturnCalls()
{
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        ValueID callID = terminator.callOperand();
        const Value& call = m_function.at(callID);
        BlockID callBlock = m_analyses.useLists().blockOf(callID);
        std::optional<bool> answer;
        auto calleesEndTheirTailCalls = [&] {
            if (!answer) {
                CalleeList callees = calleesOfCall(m_program, call);
                std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone();
                answer = m_flow && functions && std::ranges::all_of(functions->value(), [&](FunctionID callee) {
                    const FrameArrayParameters& table = m_flow->frameArrayParameters();
                    return call.opcode == Opcode::CallDirect ? table.endsItsTailCalls(callee) : table.endsItsTailCallsWhenCalledByValue(callee);
                });
            }
            return *answer;
        };
        bool keepsFrame = false;
        call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            Use use { .kind = Use::Kind::Operand, .block = callBlock, .user = callID, .operandIndex = operandIndex };
            for (ObjectID object : m_escape.pointsTo(operand).objects) {
                // The code of such a tail call keeps the frame only for a callee that ends its tail calls, and moves the closure to the heap for every other.
                if (isCandidate(object) && isLent(object) && allocationOf(object) == operand && mayLendArgument(call, operandIndex))
                    continue;
                if (isCandidate(object) && m_decisions[object.index()].madeAgainAt.contains(use))
                    continue;
                if (isCandidate(object) && calleesEndTheirTailCalls())
                    keepsFrame |= mayBeInFrame(object);
                else
                    moveToHeap(object, AllocationHeapCause::LiveAtReturnCall, use);
            }
        });
        if (keepsFrame)
            m_plan.demotedReturnCalls.append(block);
    }
}

void FramePlanner::checkHolders()
{
    Vector<Vector<RetainedObject, 1>> retainedObjects(m_decisions.size());
    Vector<PendingMove, 8> worklist;
    for (ObjectID object : candidates()) {
        if (!isInFrame(object) || isArray(object) || isKeptUntilItLeaves(object))
            continue;
        for (const ObjectUse& entry : m_escape.usesOf(object)) {
            if (!entry.uses.contains(OperandUse::Retains))
                continue;
            std::optional<ObjectID> holder = findHolder(entry.use);
            if (!holder || !isInFrame(*holder))
                worklist.append(PendingMove { object, AllocationHeapCause::HeldByHeapCell, entry.use });
            else if (allocationOf(*holder) != entry.use.user && !dieTogether(*holder, object))
                worklist.append(PendingMove { object, AllocationHeapCause::HeldAcrossIterations, entry.use });
            else
                retainedObjects[holder->index()].append(RetainedObject { object, entry.use });
        }
    }

    auto moveToHeapWithRetainedObjects = [&] {
        while (!worklist.isEmpty()) {
            PendingMove move = worklist.takeLast();
            if (!isInFrame(move.object))
                continue;
            moveToHeap(move.object, move.cause, move.use);
            for (const RetainedObject& retained : retainedObjects[move.object.index()])
                worklist.append(PendingMove { retained.object, AllocationHeapCause::HeldByHeapCell, retained.use });
        }
    };
    moveToHeapWithRetainedObjects();

    // What a cell holds leaves the frame through a use that lets its contents out. FramePointers knows what a cell holds by the place of the store alone.
    for (ObjectID cell : candidates()) {
        bool mayHoldFrameCells = isInFrame(cell) && !isArray(cell) && !holdsOnlyHeapValues(m_program, m_flow, definitionOf(cell).type);
        if (!mayHoldFrameCells || !leaksContents(cell))
            continue;
        for (const Vector<RetainedObject, 1>& retainedInHolder : retainedObjects) {
            for (const RetainedObject& retained : retainedInHolder) {
                if (cellHasPlace(m_program, definitionOf(cell), placeStoredBy(retained.use)))
                    worklist.append(PendingMove { retained.object, AllocationHeapCause::Escapes, retained.use });
            }
        }
    }
    moveToHeapWithRetainedObjects();

    // FramePointers follows a read of a frame cell by its place alone, whichever cell the pointer was stored in.
    Vector<ValueID, 8> readsOfFrameCells;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (readsFollowedByFramePointers.contains(value.opcode) && mayGiveFrameCell(cellAccessedBy(value)))
                readsOfFrameCells.append(id);
        }
    }
    for (bool changed = !readsOfFrameCells.isEmpty(); changed;) {
        for (ValueID read : readsOfFrameCells) {
            if (mayGiveFrameCell(read))
                continue;
            CellPlace place = placeReadBy(m_function.at(read));
            for (ObjectID holder : candidates()) {
                if (!isInFrame(holder))
                    continue;
                for (const RetainedObject& retained : retainedObjects[holder.index()]) {
                    if (isInFrame(retained.object) && placeStoredBy(retained.use) == place)
                        worklist.append(PendingMove { retained.object, AllocationHeapCause::HasUntrackedAlias, retained.use });
                }
            }
        }
        changed = !worklist.isEmpty();
        moveToHeapWithRetainedObjects();
    }

    for (ObjectID holder : candidates()) {
        if (!isInFrame(holder))
            continue;
        for (const RetainedObject& retained : retainedObjects[holder.index()]) {
            if (isInFrame(retained.object))
                addHolder(holder, retained.use);
        }
    }
}

std::optional<ObjectID> FramePlanner::findHolder(const Use& use) const
{
    if (use.kind != Use::Kind::Operand)
        return std::nullopt;

    const Value& user = m_function.at(use.user);
    OperandSummary summary = summarizeUse(use.user, use.operandIndex);
    switch (summary.retainedIn) {
    case RetainedIn::UnknownMemory:
        return std::nullopt;
    case RetainedIn::Result:
        if (!canRetainInitialValue(m_program, m_flow, user, use.operandIndex))
            return std::nullopt;
        return m_escape.objectDefinedBy(use.user);
    case RetainedIn::Operand: {
        if (user.opcode != Opcode::PutEnvironmentField && user.opcode != Opcode::PutSlot && user.opcode != Opcode::PutSlotOrAdd)
            return std::nullopt;
        if (user.opcode == Opcode::PutEnvironmentField && mayLeakStoredValue(m_flow, FieldReference { user.layout(), user.field() }))
            return std::nullopt;
        std::optional<ObjectID> holder = m_escape.objectDefinedBy(m_analyses.sameObjectThroughCasts(user.operand(summary.retainingOperand)).valueInFrontOfFacts());
        std::optional<CellPlace> place = placeStoredTo(user, use.operandIndex);
        if (!holder || !place || !cellHasPlace(m_program, definitionOf(*holder), *place))
            return std::nullopt;
        return holder;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FramePlanner::dieTogether(ObjectID holder, ObjectID retained) const
{
    const BlockCycles& cycles = m_analyses.blockCycles();
    BlockID holderBlock = m_analyses.useLists().blockOf(allocationOf(holder));
    BlockID retainedBlock = m_analyses.useLists().blockOf(allocationOf(retained));
    if (!cycles.isInCycle(holderBlock) && !cycles.isInCycle(retainedBlock))
        return true;
    return cycles.innermostCycleOf(holderBlock) == cycles.innermostCycleOf(retainedBlock) && hasOneLiveInstance(holder) && hasOneLiveInstance(retained);
}

CellPlace FramePlanner::placeStoredBy(const Use& use) const
{
    const Value& user = m_function.at(use.user);
    std::optional<CellPlace> place = placeStoredTo(user, use.operandIndex);
    RELEASE_ASSERT(place && user.opcode != Opcode::PutSlots);
    return *place;
}

void FramePlanner::addHolder(ObjectID holder, const Use& use)
{
    CellPlace place = placeStoredBy(use);
    switch (place.kind) {
    case CellPlace::Kind::Capture:
        m_plan.capturesHoldingFramePointers.append(CaptureReference { FunctionID(place.owner), place.index });
        return;
    case CellPlace::Kind::Field:
        m_plan.fieldsHoldingFramePointers.append(FieldReference { LayoutID(place.owner), place.index });
        return;
    case CellPlace::Kind::Key: {
        LayoutID layout = definitionOf(holder).layout();
        m_plan.fieldsHoldingFramePointers.append(FieldReference { layout, *m_program.at(layout).findField(PropertyKeyID(place.owner)) });
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FramePlanner::checkMaterialization()
{
    bool hasLateMaterialization = std::ranges::any_of(m_decisions, [](const FrameDecision& decision) {
        return decision.storage == AllocationStorage::FrameUntilMaterialized;
    });
    if (!hasLateMaterialization)
        return;

    // A closure that lets out what it holds takes the environment of the heap, as a closure of the heap does. FramePointers knows what a closure holds by its function alone.
    Vector<FunctionID, 4> functionsLettingCapturesOut;
    for (ObjectID closure : candidates()) {
        if (isInFrame(closure) && isClosure(closure) && leaksContents(closure))
            functionsLettingCapturesOut.append(definitionOf(closure).function());
    }
    BitVector frameClosures(m_function.values.size());
    Vector<ClosureMadeAgain, 2> closuresMadeAgain;
    for (ObjectID object : candidates()) {
        if (!isInFrame(object))
            continue;
        for (const Use& use : m_decisions[object.index()].madeAgainAt)
            closuresMadeAgain.append({ allocationOf(object), use });
        if (!isClosure(object))
            continue;
        if (functionsLettingCapturesOut.contains(definitionOf(object).function()))
            m_plan.frameClosuresOverHeapCells.append(allocationOf(object));
        else
            frameClosures.quickSet(allocationOf(object).index());
    }

    for (ObjectID environment : candidates()) {
        if (m_decisions[environment.index()].storage != AllocationStorage::FrameUntilMaterialized)
            continue;
        std::span<const ObjectUse> uses = m_escape.usesOf(environment);
        auto isCaptureOfFrameClosure = [&](const ObjectUse& entry) {
            return entry.use.kind == Use::Kind::Operand && frameClosures.get(entry.use.user.index());
        };
        bool isLeakedByFrameClosure = std::ranges::any_of(uses, [&](const ObjectUse& entry) {
            return isCaptureOfFrameClosure(entry) && mayLeakCapture(m_program, m_flow, m_function.at(entry.use.user), *m_function.at(entry.use.user).variadicOperandIndex(entry.use.operandIndex));
        });
        if (isLeakedByFrameClosure || !canMaterializeEnvironmentLate(m_analyses, allocationOf(environment), frameClosures, closuresMadeAgain.span())) {
            moveToHeap(environment, AllocationHeapCause::MaterializationDoesNotPay);
            continue;
        }
        for (const ObjectUse& entry : uses) {
            if (isCaptureOfFrameClosure(entry))
                addHolder(*m_escape.objectDefinedBy(entry.use.user), entry.use);
        }
    }
}

void FramePlanner::finishPlan()
{
    bool collectsRemarks = m_build.options().collectsAllocationRemarks();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (m_plan.emptyArgumentLists.contains(id))
                continue;
            bool isArrayWithoutFrameForm = arrayAllocationsWithStorage.contains(value.opcode) && !canLieInFrame(m_program, m_build, value);
            if (!hasStorageChoice(value) || isArrayWithoutFrameForm) {
                if (collectsRemarks && Value::opcodesWithAllocationSite.contains(value.opcode) && value.allocationSite())
                    m_plan.decisions.append(createHeapDecision(m_escape, id));
                continue;
            }

            std::optional<ObjectID> object = m_escape.objectDefinedBy(id);
            if (object && isCandidate(*object)) {
                const FrameDecision& decision = m_decisions[object->index()];
                m_plan.decisions.append(decision);
                m_plan.add(m_program, value, decision.storage, decision.isLent, !decision.madeAgainAt.isEmpty());
                continue;
            }
            if (value.storage() == AllocationStorage::Heap)
                m_plan.decisions.append(FrameDecision { .allocation = id, .cause = AllocationHeapCause::NotTracked, .isHeldByClosureTable = isLikeEveryClosureOfItsFunction(m_program, m_flow, m_function, value), .limit = reachedLimitOf(m_escape) });
            m_plan.add(m_program, value, value.storage(), value.opcode == Opcode::NewClosure && value.isLent());
        }
    }
    AOT::sort(m_plan.decisions, { }, &FrameDecision::allocation);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
