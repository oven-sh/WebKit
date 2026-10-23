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
#include "AOTFrameAllocationMaterialization.h"
#include "AOTFunctionKinds.h"
#include "AOTHeat.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>

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

struct HolderReference {
    enum class Kind : uint8_t { EnvironmentField, Capture, Key };

    Kind kind { Kind::Key };
    uint32_t owner { 0 };
    uint32_t index { 0 };

    friend bool operator==(const HolderReference&, const HolderReference&) = default;
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
    std::optional<uint32_t> position = cell.variadicOperandIndex(operandIndex);
    if (!position)
        return false;
    if (cell.opcode == Opcode::NewClosure)
        return !mayLeakCapture(program, flow, cell, *position);
    if (cell.opcode != Opcode::NewObject || program.at(cell.layout()).kind != LayoutKind::Environment || *position >= cell.fields().size())
        return false;
    return !mayLeakStoredValue(flow, FieldReference { cell.layout(), cell.fields()[*position] });
}

static bool hasFieldStoredBy(const Program& program, const Value& cell, const Value& store)
{
    if (cell.opcode != Opcode::NewObject)
        return false;
    if (store.opcode == Opcode::PutEnvironmentField)
        return cell.layout() == store.layout();
    return program.at(cell.layout()).findField(store.key()).has_value();
}

static HolderReference referenceStoredBy(const Value& user, uint32_t operandIndex)
{
    switch (user.opcode) {
    case Opcode::NewClosure:
        return { HolderReference::Kind::Capture, user.function().index(), *user.variadicOperandIndex(operandIndex) };
    case Opcode::NewObject:
        return { HolderReference::Kind::EnvironmentField, user.layout().index(), user.fields()[*user.variadicOperandIndex(operandIndex)] };
    case Opcode::PutEnvironmentField:
        return { HolderReference::Kind::EnvironmentField, user.layout().index(), user.field() };
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return { HolderReference::Kind::Key, user.key().index(), 0 };
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static HolderReference referenceReadBy(const Value& read)
{
    switch (read.opcode) {
    case Opcode::GetEnvironmentField:
        return { HolderReference::Kind::EnvironmentField, read.layout().index(), read.field() };
    case Opcode::GetCapture:
        return { HolderReference::Kind::Capture, read.function().index(), read.index() };
    case Opcode::GetSlot:
    case Opcode::GetSlotOrUndefined:
        return { HolderReference::Kind::Key, read.key().index(), 0 };
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool mayBeHeldIn(const Program& program, const Value& cell, const HolderReference& reference)
{
    switch (reference.kind) {
    case HolderReference::Kind::EnvironmentField:
        return cell.opcode == Opcode::NewObject && cell.layout().index() == reference.owner;
    case HolderReference::Kind::Capture:
        return cell.opcode == Opcode::NewClosure && cell.function().index() == reference.owner;
    case HolderReference::Kind::Key:
        return cell.opcode == Opcode::NewObject && program.at(cell.layout()).findField(PropertyKeyID(reference.owner)).has_value();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The closure that such a field makes on demand is a cell of the heap. It holds the object, or the environment that the object keeps for its closures.
static bool defersClosureWithCapture(ClosureDeferral deferral)
{
    switch (deferral) {
    case ClosureDeferral::Unknown:
    case ClosureDeferral::CapturesNothing:
        return false;
    // Whoever borrows the object that is not made yet holds the outer object under the type of the inner one.
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
    std::optional<uint32_t> position = user.variadicOperandIndex(operandIndex);
    return field && position && *position < user.fields().size() && user.fields()[*position] == *field;
}

// The slot of a field that leaves a closure unmade is a narrow reference, which reaches no frame.
static bool isValueOfDeferringField(const Program& program, const Value& user, uint32_t operandIndex)
{
    auto defersClosure = [&](LayoutID layout, uint32_t field) {
        return flowOf(program, FieldReference { layout, field }).location.deferral != ClosureDeferral::Unknown;
    };
    if (user.opcode == Opcode::NewObject) {
        std::optional<uint32_t> position = user.variadicOperandIndex(operandIndex);
        return position && *position < user.fields().size() && defersClosure(user.layout(), user.fields()[*position]);
    }
    if ((user.opcode != Opcode::PutSlot && user.opcode != Opcode::PutSlotOrAdd) || operandIndex != OperandIndex::PutSlot::value)
        return false;
    return std::ranges::any_of(program.at(user.receivers()).members, [&](LayoutID member) {
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
        const TrackedObject& tracked = escape.at(*object);
        decision.forcedBy = tracked.firstEscape;
        decision.reasons = tracked.reasons;
    }
    return decision;
}

static void addAllocation(FramePlan& plan, const Program& program, const Value& allocation, AllocationStorage storage, bool isLent = false, bool isMadeAgain = false)
{
    if (arrayAllocationsWithStorage.contains(allocation.opcode)) {
        if (storage == AllocationStorage::Frame)
            ++(allocation.opcode == Opcode::CreateRest ? plan.restArraysInFrames : plan.arraysInFrames);
        return;
    }
    if (allocation.opcode == Opcode::NewClosure) {
        plan.allocation.addClosure(allocation.function(), storage);
        if (storage == AllocationStorage::Frame)
            ++plan.closuresInFrames;
        // The copy that a callee gets which does not borrow.
        if (isLent) {
            plan.allocation.addClosure(allocation.function(), AllocationStorage::Heap);
            ++plan.closuresLent;
        }
        if (isMadeAgain)
            plan.allocation.addClosure(allocation.function(), AllocationStorage::Heap);
        return;
    }

    LayoutID layout = allocation.layout();
    plan.allocation.addCell(layout, storage);
    if (program.at(layout).kind != LayoutKind::Environment) {
        if (storage == AllocationStorage::Frame)
            ++plan.objectsInFrames;
        return;
    }

    switch (storage) {
    case AllocationStorage::Heap:
        ++plan.environmentsOnHeap;
        return;
    case AllocationStorage::Frame:
        ++plan.environmentsInFrames;
        return;
    case AllocationStorage::FrameUntilMaterialized:
        ++plan.environmentsMaterializedLate;
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

FramePlanner::FramePlanner(FunctionAnalyses& analyses, const Heat& heat)
    : m_analyses(analyses)
    , m_heat(heat)
    , m_program(analyses.program())
    , m_function(analyses.function())
    , m_flow(analyses.context().flow)
    , m_escape(analyses.escapeWithLiveInstances())
    , m_isCandidate(m_escape.numberOfObjects())
    , m_decisions(FillWith { }, m_escape.numberOfObjects(), FrameDecision { .cause = AllocationHeapCause::NotTracked })
{
}

FramePlan FramePlanner::run()
{
    shareEmptyArgumentLists();
    collectCandidates();
    if (m_isCandidate.isEmpty()) {
        finishPlan();
        return WTF::move(m_plan);
    }

    applyEscapeAnalysis();
    checkCellForms();
    checkUses();
    checkUsesOfArrays();
    checkSuspensions();
    checkReturnCalls();
    checkHolders();
    checkMaterialization();
    takeClosuresOfTable();
    m_plan.demotedReturnCalls.removeAllMatching([&](BlockID block) {
        return !passesCellOfFrame(m_function.at(m_function.at(block).terminator.callOperand()));
    });
    finishPlan();
    return WTF::move(m_plan);
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

void FramePlanner::shareEmptyArgumentLists()
{
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (!isEmptyArrayLiteral(m_function.at(id)))
                continue;
            std::optional<ObjectID> list = m_escape.objectDefinedBy(id);
            if (list && isOnlyReadAsHiddenRest(*list))
                m_plan.emptyArgumentLists.append(id);
        }
    }
}

// The language has no name for the list of surplus arguments, so nothing tells one empty list from another.
bool FramePlanner::isOnlyReadAsHiddenRest(ObjectID list) const
{
    if (!m_escape.at(list).uses.containsOnly(usesOfSharedLists))
        return false;
    return std::ranges::all_of(m_escape.usesOf(list), [&](const ObjectUse& entry) {
        return isHiddenRestOfEveryCallee(entry.use);
    });
}

bool FramePlanner::isHiddenRestOfEveryCallee(const Use& use) const
{
    if (use.kind != Use::Kind::Operand)
        return false;
    const Value& call = m_function.at(use.user);
    if (effectRuleOf(call.opcode) != EffectRule::Call)
        return false;
    CalleeList callees = calleesOfCall(m_program, call);
    if (!callees.isComplete || !callees.builtins.isEmpty())
        return false;
    return std::ranges::all_of(callees.functions, [&](FunctionID id) {
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
            if (!hasStorageChoice(value) || value.storage() != AllocationStorage::Heap)
                continue;
            if (staysOnHeapForValidation(m_program, m_analyses.context().build, value))
                continue;
            if (arrayAllocationsWithStorage.contains(value.opcode) && (!canLieInFrame(m_program, m_analyses.context().build, value) || m_plan.emptyArgumentLists.contains(id)))
                continue;
            std::optional<ObjectID> object = m_escape.objectDefinedBy(id);
            if (!object)
                continue;
            m_isCandidate.quickSet(object->index());
            m_decisions[object->index()] = FrameDecision { .allocation = id, .storage = AllocationStorage::Frame };
        }
    }
}

void FramePlanner::applyEscapeAnalysis()
{
    for (size_t index : m_isCandidate)
        decideByEscapeState(ObjectID(index));
}

// Whether an array escapes is decided use by use: the summaries of the callees know no path that an array in a frame never takes.
void FramePlanner::decideArrayByLiveInstances(ObjectID array)
{
    const TrackedObject& tracked = m_escape.at(array);
    if (tracked.state == EscapeState::Unknown)
        moveToHeap(array, AllocationHeapCause::NotTracked, std::nullopt, reachedLimitOf(m_escape));
    else if (m_escape.liveInstancesOf(array) != LiveInstances::AtMostOne)
        moveToHeap(array, AllocationHeapCause::SeveralLiveInstances);
}

void FramePlanner::decideByEscapeState(ObjectID object)
{
    if (isArray(object)) {
        decideArrayByLiveInstances(object);
        return;
    }

    const TrackedObject& tracked = m_escape.at(object);
    bool hasOneLiveInstance = m_escape.liveInstancesOf(object) == LiveInstances::AtMostOne;
    switch (tracked.state) {
    case EscapeState::Unknown:
        moveToHeap(object, AllocationHeapCause::NotTracked, std::nullopt, reachedLimitOf(m_escape));
        return;
    case EscapeState::Escapes:
        if (hasOneLiveInstance && escapesOnlyIntoClosures(object))
            m_decisions[object.index()].storage = AllocationStorage::FrameUntilMaterialized;
        else if (!keepClosureUntilItLeaves(object))
            moveToHeap(object, AllocationHeapCause::Escapes, tracked.firstEscape);
        return;
    case EscapeState::DoesNotEscape:
        if (!hasOneLiveInstance)
            moveToHeap(object, AllocationHeapCause::SeveralLiveInstances);
        else if (hasUntrackedAlias(object))
            moveToHeap(object, AllocationHeapCause::HasUntrackedAlias);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FramePlanner::hasUntrackedAlias(ObjectID object) const
{
    return std::ranges::any_of(m_escape.aliasesOf(object), [&](ValueID alias) {
        if (m_program.typeOf(m_function, alias).isOpaque(OpaqueKind::Guard))
            return false;
        return !propagatesFramePointers(m_function.at(alias).opcode);
    });
}

bool FramePlanner::escapesOnlyIntoClosures(ObjectID object) const
{
    const TrackedObject& tracked = m_escape.at(object);
    const Value& definition = definitionOf(object);
    if (definition.opcode != Opcode::NewObject || m_program.at(definition.layout()).kind != LayoutKind::Environment)
        return false;
    bool escapesOnlyAsCapture = std::ranges::all_of(m_escape.usesOf(object), [&](const ObjectUse& entry) {
        return !entry.isEscape || (entry.use.kind == Use::Kind::Operand && m_function.at(entry.use.user).opcode == Opcode::NewClosure);
    });
    if (!escapesOnlyAsCapture)
        return false;
    bool hasOnlyClosureContainers = std::ranges::all_of(m_escape.containersOf(object), [&](ObjectID container) {
        return definitionOf(container).opcode == Opcode::NewClosure;
    });
    if (!hasOnlyClosureContainers)
        return false;
    return std::ranges::all_of(m_escape.aliasesOf(object), [&](ValueID alias) {
        return m_analyses.sameObjectThroughCasts(alias) == tracked.definition;
    });
}

// A read of a method that is called at once ends as a closure that goes through a join into a call by value.
void FramePlanner::takeClosuresOfTable()
{
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (!mayBeInFrame(object) && (isOnlyCalled(object) || isLikeEveryClosureOfItsFunction(definitionOf(object))))
            m_decisions[index].isHeldByClosureTable = true;
    }
}

// Flow has followed the closures of the function to every place that they reach.
bool FramePlanner::isLikeEveryClosureOfItsFunction(const Program& program, const FlowResults* flow, const Function& maker, const Value& closure)
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

// Without captures one closure of a function does what any other does, and the function does not get hold of the closure it is entered through.
bool FramePlanner::isOnlyCalled(ObjectID closure) const
{
    const Value& definition = definitionOf(closure);
    if (definition.opcode != Opcode::NewClosure || !definition.capturesOperands().empty() || checkClosureForm(m_program, definition) != AllocationHeapCause::None)
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

static constexpr unsigned maxUsesOfClosureKeptUntilItLeaves = 8;

// Where a closure of the heap is one call of a stub, a closure of the frame is more code than it, and so is every closure that is made again.
bool FramePlanner::willHaveLongFormsInLine(BlockID block)
{
    BlockFrequency frequency = m_function.at(block).frequency;
    if (frequency != BlockFrequency::Normal)
        return false;
    double activations = m_heat.activationsOf(m_program, m_analyses.context().id);
    if (codeShapeFor(activations * roundingOfFrequencySums, false, frequency) != CodeShape::Fast && runsNoBlockMoreOftenThanEntry(m_analyses.cfg()))
        return false;
    if (m_analyses.executionCountOf(block) == ExecutionCount::AtMostOnce)
        return false;
    double frequencyInFunction = m_analyses.blockFrequencies().frequencyOf(block);
    double heat = activations * frequencyInFunction;
    return codeShapeFor(heat, false, frequency, m_heat.leastHeatOfFastCode(m_program, m_analyses.context().id, frequencyInFunction)) == CodeShape::Fast && m_heat.isAmongHottestCode(heat);
}

// A use on the way that every run takes from where the closure is made would only add the closure of the frame to the one of the heap.
static bool liesOffTheWay(const Function& function, BlockID blockOfDefinition, const Use& use)
{
    return use.block != blockOfDefinition || (use.kind == Use::Kind::EdgeArgument && function.at(use.block).terminator.targets.size() > 1);
}

// Whoever borrows the closure cannot tell it from another one afterwards.
bool FramePlanner::keepClosureUntilItLeaves(ObjectID closure)
{
    ValueID definition = m_escape.at(closure).definition;
    if (!m_flow || m_function.at(definition).opcode != Opcode::NewClosure)
        return false;
    const UseLists& useLists = m_analyses.useLists();
    std::span<const Use> uses = useLists.usesOf(definition);
    if (uses.empty() || uses.size() > maxUsesOfClosureKeptUntilItLeaves)
        return false;

    BlockID blockOfDefinition = useLists.blockOf(definition);
    if (!willHaveLongFormsInLine(blockOfDefinition))
        return false;
    std::span<const ObjectUse> knownUses = m_escape.usesOf(closure);
    bool isLent = false;
    bool isBorrowed = false;
    Vector<Use, 2> lastUses;
    Vector<Use, 1> madeAgainAt;
    for (const Use& use : uses) {
        if (use.kind == Use::Kind::HandlerArgument || use.kind == Use::Kind::BlockSubject)
            return false;
        auto known = std::ranges::find(knownUses, use, &ObjectUse::use);
        // A use that keeps the closure in a cell or gives it back makes another way to it, which is not among the uses of the value.
        bool staysInFrame = use.kind == Use::Kind::Operand && known != knownUses.end() && (!known->isEscape || effectRuleOf(m_function.at(use.user).opcode) == EffectRule::Call);
        if (staysInFrame && known->uses.containsOnly(usesOfBorrowedParameter) && checkUse(closure, *known) == AllocationHeapCause::None) {
            isBorrowed = true;
            continue;
        }
        lastUses.append(use);
        // Code that seldom runs is small with a closure of the heap.
        bool runsOften = m_function.at(use.block).frequency == BlockFrequency::Normal;
        if (runsOften && use.kind == Use::Kind::Operand && mayLendArgument(m_function.at(use.user), use.operandIndex) && mayFindBorrowingCallee(m_function.at(use.user), use.operandIndex)) {
            isLent = true;
            continue;
        }
        if (!liesOffTheWay(m_function, blockOfDefinition, use))
            return false;
        madeAgainAt.append(use);
    }
    if (!isLent && !isBorrowed)
        return false;
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

// A copy costs more than a closure that is made on the heap at once.
bool FramePlanner::mayFindBorrowingCallee(const Value& call, uint32_t operandIndex) const
{
    auto borrows = [&](FunctionID id) {
        std::optional<uint32_t> entryParameter = entryParameterOf(m_program.at(id), call, operandIndex);
        return entryParameter && m_flow->framePointerSummary(id, *entryParameter).uses.containsOnly(usesOfBorrowedParameter);
    };
    CalleeList callees = calleesOfCall(m_program, call);
    if (std::ranges::any_of(callees.functions, borrows))
        return true;
    if (!callees.isComplete)
        return callees.functions.isEmpty();

    const BuiltinTable& table = m_program.builtins();
    return std::ranges::any_of(callees.builtins, [&](BuiltinID builtin) {
        return std::ranges::any_of(table.overloadsOf(builtin), [&](const BuiltinOverload& overload) {
            const RuntimeFunctionEntry* entry = overload.code.implementation.function ? findRuntimeFunction(m_program, overload.code.implementation.function) : nullptr;
            return entry && borrows(entry->function);
        });
    });
}

void FramePlanner::checkCellForms()
{
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (!mayBeInFrame(object))
            continue;

        if (isArray(object))
            continue;

        const Value& allocation = definitionOf(object);
        AllocationHeapCause cause = allocation.opcode == Opcode::NewClosure ? checkClosureForm(m_program, allocation) : checkObjectForm(m_program, allocation);
        if (cause != AllocationHeapCause::None) {
            moveToHeap(object, cause);
            continue;
        }
        if (fitsInFrame(m_program, m_analyses.context().build, allocation))
            continue;
        GaveUp gaveUp = m_analyses.context().build.gaveUp(LimitID::MaxFrameEnvironmentFields, m_analyses.subject(m_escape.at(object).definition));
        moveToHeap(object, AllocationHeapCause::ExceedsLimit, std::nullopt, gaveUp.limit);
    }
}

AllocationHeapCause FramePlanner::checkClosureForm(const Program& program, const Value& closure)
{
    const Function& callee = program.at(closure.function());
    if (closure.isMadeOnDemand())
        return AllocationHeapCause::LayoutDefersClosures;
    if (callee.has(FunctionFlags::TakesCallee) || !callee.hasOrdinaryFrame())
        return AllocationHeapCause::ClosureIsNotOnlyCalled;
    return AllocationHeapCause::None;
}

AllocationHeapCause FramePlanner::checkObjectForm(const Program& program, const Value& object)
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

    if (object.prototypeOperand())
        return AllocationHeapCause::ChangedByUse;
    for (uint32_t field = 0; field < layout.fields.size(); ++field) {
        if (defersClosureWithCapture(flowOf(program, FieldReference { id, field }).location.deferral))
            return AllocationHeapCause::LayoutDefersClosures;
    }
    if (layout.flow.fieldCreation != FieldCreation::OnlyAtConstruction)
        return AllocationHeapCause::FieldsCreatedLater;
    return AllocationHeapCause::None;
}

bool FramePlanner::fitsInFrame(const Program& program, const BuildContext& build, const Value& allocation)
{
    if (allocation.opcode != Opcode::NewObject)
        return true;
    return program.at(allocation.layout()).fields.size() <= build.limit(LimitID::MaxFrameEnvironmentFields);
}

// checkCellForms() moves the first two kinds to the heap. It gives up on an object that does not fit, which is recorded, and checkUses() moves every other object that may change its form.
bool FramePlanner::staysOnHeapByForm(const Program& program, const FlowResults& flow, const BuildContext& build, const Value& allocation)
{
    if (allocation.opcode == Opcode::NewClosure)
        return checkClosureForm(program, allocation) != AllocationHeapCause::None;
    RELEASE_ASSERT(allocation.opcode == Opcode::NewObject);
    if (checkObjectForm(program, allocation) != AllocationHeapCause::None)
        return true;
    return fitsInFrame(program, build, allocation) && mayChangeForm(program, &flow, allocation);
}

void FramePlanner::checkUses()
{
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
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
    if (user.operandKind(operandIndex) == OperandKind::Guard)
        return AllocationHeapCause::None;
    if (isEnvironmentOfDeferredClosures(m_program, user, operandIndex) || isValueOfDeferringField(m_program, user, operandIndex))
        return AllocationHeapCause::HeldByHeapCell;
    if (effectRuleOf(user.opcode) == EffectRule::Access && provenAccessOf(m_program, m_function, user).isLookupByKey)
        return AllocationHeapCause::SeenByNativeCode;

    // The pointer goes nowhere. What is wrong is where the cell lies.
    if (!mayLookAtFrameCells(effectRuleOf(user.opcode)) && effectRuleOf(user.opcode) == EffectRule::Builtin && entry.uses.containsOnly(inlineUsesOfFrameCells))
        return AllocationHeapCause::SeenByNativeCode;
    if (mayLeakFramePointer(m_program, m_flow, m_function, user, operandIndex)) {
        bool isDecidedByHolder = isInFrame(object)
            && entry.uses.contains(OperandUse::Retains)
            && summarizeFramePointerUse(m_program, m_flow, m_function, user, operandIndex).retainedIn != RetainedIn::UnknownMemory;
        if (!isDecidedByHolder)
            return AllocationHeapCause::Escapes;
    }
    if (entry.uses.containsAny(usesChangingFrameCells))
        return AllocationHeapCause::ChangedByUse;

    const Value& allocation = definitionOf(object);
    bool isClosure = allocation.opcode == Opcode::NewClosure;
    // What is read out of an environment that never holds a pointer into a frame may go anywhere.
    if (mayLeakContentsThrough(entry) && !holdsOnlyHeapValues(m_program, m_flow, allocation.type)) {
        // What the cell holds goes to the heap in checkHolders(). The verifier knows what an operand holds only where it sees the allocation, and of an environment by its layout alone.
        bool isSeenAsAllocation = m_analyses.sameObjectThroughCasts(user.operand(operandIndex)) == m_escape.at(object).definition;
        if (!isSeenAsAllocation || (!isClosure && m_program.at(allocation.layout()).isEnvironment()))
            return AllocationHeapCause::Escapes;
    }
    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
        if (!isClosure || !mayLookAtFrameCells(effectRuleOf(user.opcode)) || !entry.uses.containsOnly(inlineUsesOfFrameCells))
            return AllocationHeapCause::SeenByNativeCode;
        break;
    case EffectRule::Access:
        // A read of a property of a function makes the function an object with properties.
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
    if (isClosure || m_program.at(allocation.layout()).isEnvironment())
        return AllocationHeapCause::None;

    OperandMemory memory = memoryAccessedThrough(m_analyses.effectContext(), m_function, user, operandIndex, m_analyses.effectsOf(entry.use.user));
    if (!memory.writes.containsOnly(heapsInsideFrameCells))
        return AllocationHeapCause::ChangedByUse;
    return AllocationHeapCause::None;
}

bool FramePlanner::mayLeakContentsThrough(const ObjectUse& entry) const
{
    if (entry.use.kind != Use::Kind::Operand)
        return false;
    const Value& user = m_function.at(entry.use.user);
    if (user.operandKind(entry.use.operandIndex) == OperandKind::Guard)
        return false;
    return mayLeakContents(m_program, m_flow, m_function, user, entry.use.operandIndex);
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

    auto add = [&](ValueID id, uint32_t given) {
        if (!(given & ~mayBe[id.index()]))
            return false;
        mayBe[id.index()] |= given;
        return true;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            BlockID block(blockIndex);
            for (ValueID id : function.at(block).values) {
                forEachReturnedOperand(function, function.at(id), [&](ValueID operand) {
                    changed |= add(id, mayBe[operand.index()]);
                });
            }
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                const CompactList<ValueID>& parameters = function.at(target.block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index) {
                    if (ValueID passed = valuePassedOn(function, edge, index))
                        changed |= add(parameters[index], mayBe[passed.index()]);
                }
            });
        }
    }

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
    for (size_t index : m_isCandidate) {
        ObjectID array(index);
        if (!isArray(array) || !mayBeInFrame(array))
            continue;
        if (arrays.size() == maxFollowedArrays) {
            moveToHeap(array, AllocationHeapCause::NotTracked);
            continue;
        }
        arrays.append(array);
        definitions.append(m_escape.at(array).definition);
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
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
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

bool FramePlanner::calleesEndTheirTailCalls(const Value& call) const
{
    if (!m_flow)
        return false;
    CalleeList callees = calleesOfCall(m_program, call);
    return callees.isComplete && callees.builtins.isEmpty() && std::ranges::all_of(callees.functions, [&](FunctionID callee) {
        return m_flow->frameArrayParameters().endsItsTailCalls(callee);
    });
}

bool FramePlanner::passesCellOfFrame(const Value& call) const
{
    bool passesCell = false;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        for (ObjectID object : m_escape.pointsTo(operand).objects)
            passesCell |= isCandidate(object) && mayBeInFrame(object);
    });
    return passesCell;
}

void FramePlanner::checkReturnCalls()
{
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        ValueID call = terminator.callOperand();
        BlockID callBlock = m_analyses.useLists().blockOf(call);
        std::optional<bool> answer;
        auto mayKeepFrame = [&] {
            if (!answer)
                answer = calleesEndTheirTailCalls(m_function.at(call));
            return *answer;
        };
        bool keepsFrame = false;
        m_function.at(call).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            Use use { .kind = Use::Kind::Operand, .block = callBlock, .user = call, .operandIndex = operandIndex };
            for (ObjectID object : m_escape.pointsTo(operand).objects) {
                // The code of such a tail call keeps the frame only for a callee that ends its tail calls, and moves the closure to the heap for every other.
                if (isCandidate(object) && isLent(object) && m_escape.at(object).definition == operand && mayLendArgument(m_function.at(call), operandIndex))
                    continue;
                if (isCandidate(object) && m_decisions[object.index()].madeAgainAt.contains(use))
                    continue;
                if (isCandidate(object) && mayKeepFrame())
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
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (!isInFrame(object) || isArray(object) || isKeptUntilItLeaves(object))
            continue;
        for (const ObjectUse& entry : m_escape.usesOf(object)) {
            if (!entry.uses.contains(OperandUse::Retains))
                continue;
            std::optional<ObjectID> holder = findHolder(entry.use);
            if (!holder || !isInFrame(*holder)) {
                worklist.append(PendingMove { object, AllocationHeapCause::HeldByHeapCell, entry.use });
                continue;
            }
            bool isInitialValue = m_escape.at(*holder).definition == entry.use.user;
            if (!isInitialValue && !dieTogether(*holder, object)) {
                worklist.append(PendingMove { object, AllocationHeapCause::HeldAcrossIterations, entry.use });
                continue;
            }
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

    // What a cell holds leaves the frame through such a use. FramePointers knows what a cell holds by the reference of the store alone.
    for (size_t index : m_isCandidate) {
        ObjectID cell(index);
        bool mayHoldFrameCells = isInFrame(cell) && !isArray(cell) && !holdsOnlyHeapValues(m_program, m_flow, definitionOf(cell).type);
        if (!mayHoldFrameCells || !leaksContents(cell))
            continue;
        for (const Vector<RetainedObject, 1>& retainedInHolder : retainedObjects) {
            for (const RetainedObject& retained : retainedInHolder) {
                if (mayBeHeldIn(m_program, definitionOf(cell), referenceStoredBy(m_function.at(retained.use.user), retained.use.operandIndex)))
                    worklist.append(PendingMove { retained.object, AllocationHeapCause::Escapes, retained.use });
            }
        }
    }
    moveToHeapWithRetainedObjects();

    // FramePointers follows a read of a frame cell by its reference alone, whichever cell the pointer was stored in.
    auto mayGiveFrameCell = [&](ValueID value) {
        for (ObjectID object : m_escape.pointsTo(value).objects) {
            if (isCandidate(object) && mayBeInFrame(object))
                return true;
        }
        return false;
    };
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
            HolderReference reference = referenceReadBy(m_function.at(read));
            for (size_t index : m_isCandidate) {
                if (!isInFrame(ObjectID(index)))
                    continue;
                for (const RetainedObject& retained : retainedObjects[index]) {
                    if (isInFrame(retained.object) && referenceStoredBy(m_function.at(retained.use.user), retained.use.operandIndex) == reference)
                        worklist.append(PendingMove { retained.object, AllocationHeapCause::HasUntrackedAlias, retained.use });
                }
            }
        }
        changed = !worklist.isEmpty();
        moveToHeapWithRetainedObjects();
    }

    for (size_t index : m_isCandidate) {
        ObjectID holder(index);
        if (!isInFrame(holder))
            continue;
        for (const RetainedObject& retained : retainedObjects[index]) {
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
    OperandSummary summary = summarizeFramePointerUse(m_program, m_flow, m_function, user, use.operandIndex);
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
        std::optional<ObjectID> holder = m_escape.objectDefinedBy(m_analyses.sameObjectThroughCasts(user.operand(summary.retainingOperand)));
        if (!holder || !hasFieldStoredBy(m_program, definitionOf(*holder), user))
            return std::nullopt;
        return holder;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FramePlanner::dieTogether(ObjectID holder, ObjectID retained) const
{
    const BlockCycles& cycles = m_analyses.blockCycles();
    const UseLists& useLists = m_analyses.useLists();
    BlockID holderBlock = useLists.blockOf(m_escape.at(holder).definition);
    BlockID retainedBlock = useLists.blockOf(m_escape.at(retained).definition);
    if (!cycles.isInCycle(holderBlock) && !cycles.isInCycle(retainedBlock))
        return true;
    return cycles.innermostCycleOf(holderBlock) == cycles.innermostCycleOf(retainedBlock)
        && m_escape.liveInstancesOf(holder) == LiveInstances::AtMostOne
        && m_escape.liveInstancesOf(retained) == LiveInstances::AtMostOne;
}

void FramePlanner::addHolder(ObjectID holder, const Use& use)
{
    const Value& user = m_function.at(use.user);
    switch (user.opcode) {
    case Opcode::NewClosure:
        m_plan.allocation.addHolder(CaptureReference { user.function(), *user.variadicOperandIndex(use.operandIndex) });
        return;
    case Opcode::NewObject:
        m_plan.allocation.addHolder(FieldReference { user.layout(), user.fields()[*user.variadicOperandIndex(use.operandIndex)] });
        return;
    case Opcode::PutEnvironmentField:
        m_plan.allocation.addHolder(FieldReference { user.layout(), user.field() });
        return;
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd: {
        LayoutID layout = definitionOf(holder).layout();
        m_plan.allocation.addHolder(FieldReference { layout, *m_program.at(layout).findField(user.key()) });
        return;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void FramePlanner::checkMaterialization()
{
    bool hasLateMaterialization = std::ranges::any_of(m_decisions, [](const FrameDecision& decision) {
        return decision.storage == AllocationStorage::FrameUntilMaterialized;
    });
    if (!hasLateMaterialization)
        return;

    BitVector frameClosures = findFrameClosures();
    for (size_t index : m_isCandidate) {
        ObjectID closure(index);
        if (isInFrame(closure) && definitionOf(closure).opcode == Opcode::NewClosure && !frameClosures.get(m_escape.at(closure).definition.index()))
            m_plan.frameClosuresOverHeapCells.append(m_escape.at(closure).definition);
    }
    for (size_t index : m_isCandidate) {
        ObjectID environment(index);
        if (m_decisions[index].storage != AllocationStorage::FrameUntilMaterialized)
            continue;
        if (!canMaterializeLate(environment, frameClosures)) {
            moveToHeap(environment, AllocationHeapCause::MaterializationDoesNotPay);
            continue;
        }
        for (const ObjectUse& entry : m_escape.usesOf(environment)) {
            if (entry.use.kind == Use::Kind::Operand && frameClosures.get(entry.use.user.index()))
                addHolder(*m_escape.objectDefinedBy(entry.use.user), entry.use);
        }
    }
}

// A closure that lets out what it holds takes the environment of the heap, as a closure of the heap does. FramePointers knows what a closure holds by its function alone.
BitVector FramePlanner::findFrameClosures() const
{
    Vector<FunctionID, 4> functionsLettingCapturesOut;
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (isInFrame(object) && definitionOf(object).opcode == Opcode::NewClosure && leaksContents(object))
            functionsLettingCapturesOut.append(definitionOf(object).function());
    }

    BitVector frameClosures(m_function.values.size());
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (isInFrame(object) && definitionOf(object).opcode == Opcode::NewClosure && !functionsLettingCapturesOut.contains(definitionOf(object).function()))
            frameClosures.quickSet(m_escape.at(object).definition.index());
    }
    return frameClosures;
}

bool FramePlanner::canMaterializeLate(ObjectID environment, const BitVector& frameClosures)
{
    bool isLeakedByFrameClosure = std::ranges::any_of(m_escape.usesOf(environment), [&](const ObjectUse& entry) {
        if (entry.use.kind != Use::Kind::Operand || !frameClosures.get(entry.use.user.index()))
            return false;
        const Value& closure = m_function.at(entry.use.user);
        return mayLeakCapture(m_program, m_flow, closure, *closure.variadicOperandIndex(entry.use.operandIndex));
    });
    if (isLeakedByFrameClosure)
        return false;
    Vector<EnvironmentMaterialization::ClosureMadeAgain, 2> closuresMadeAgain;
    for (size_t index : m_isCandidate) {
        if (!isInFrame(ObjectID(index)))
            continue;
        for (const Use& use : m_decisions[index].madeAgainAt)
            closuresMadeAgain.append({ m_escape.at(ObjectID(index)).definition, use });
    }
    return EnvironmentMaterialization(m_analyses, m_escape.at(environment).definition, frameClosures, closuresMadeAgain.span()).canRun();
}

void FramePlanner::finishPlan()
{
    bool collectsRemarks = m_analyses.context().build.options().collectsAllocationRemarks();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (m_plan.emptyArgumentLists.contains(id))
                continue;
            bool isArrayWithoutFrameForm = arrayAllocationsWithStorage.contains(value.opcode) && !canLieInFrame(m_program, m_analyses.context().build, value);
            if (!hasStorageChoice(value) || isArrayWithoutFrameForm) {
                if (collectsRemarks && Value::opcodesWithAllocationSite.contains(value.opcode) && value.allocationSite())
                    m_plan.decisions.append(createHeapDecision(m_escape, id));
                continue;
            }

            std::optional<ObjectID> object = m_escape.objectDefinedBy(id);
            if (object && isCandidate(*object)) {
                const FrameDecision& decision = m_decisions[object->index()];
                m_plan.decisions.append(decision);
                addAllocation(m_plan, m_program, value, decision.storage, decision.isLent, !decision.madeAgainAt.isEmpty());
                continue;
            }
            if (value.storage() == AllocationStorage::Heap)
                m_plan.decisions.append(FrameDecision { .allocation = id, .cause = AllocationHeapCause::NotTracked, .isHeldByClosureTable = isLikeEveryClosureOfItsFunction(value), .limit = reachedLimitOf(m_escape) });
            addAllocation(m_plan, m_program, value, value.storage(), value.opcode == Opcode::NewClosure && value.isLent());
        }
    }
    AOT::sort(m_plan.decisions, { }, &FrameDecision::allocation);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
