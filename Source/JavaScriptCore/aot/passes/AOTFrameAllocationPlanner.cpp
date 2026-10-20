/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocationPlanner.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationMaterialization.h"
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
        return !mayLeakCapture(flow, cell, *position);
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

static void addAllocation(FramePlan& plan, const Program& program, const Value& allocation, AllocationStorage storage)
{
    if (allocation.opcode == Opcode::NewClosure) {
        plan.allocation.addClosure(allocation.function(), storage);
        if (storage == AllocationStorage::Frame)
            ++plan.closuresInFrames;
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

FramePlanner::FramePlanner(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_program(analyses.program())
    , m_function(analyses.function())
    , m_flow(analyses.context().flow)
    , m_escape(analyses.escape())
    , m_isCandidate(m_escape.numberOfObjects())
    , m_decisions(FillWith { }, m_escape.numberOfObjects(), FrameDecision { .cause = AllocationHeapCause::NotTracked })
{
}

FramePlan FramePlanner::run()
{
    collectCandidates();
    if (m_isCandidate.isEmpty()) {
        finishPlan();
        return WTF::move(m_plan);
    }

    applyEscapeAnalysis();
    checkCellForms();
    checkUses();
    checkSuspensions();
    checkReturnCalls();
    checkHolders();
    checkMaterialization();
    finishPlan();
    return WTF::move(m_plan);
}

void FramePlanner::moveToHeap(ObjectID object, AllocationHeapCause cause, std::optional<Use> use, LimitID limit)
{
    FrameDecision& decision = m_decisions[object.index()];
    if (decision.storage == AllocationStorage::Heap)
        return;
    decision.storage = AllocationStorage::Heap;
    decision.cause = cause;
    decision.forcedBy = use;
    decision.reasons = m_escape.at(object).reasons;
    decision.limit = limit;
}

void FramePlanner::collectCandidates()
{
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (!hasStorageChoice(value) || value.storage() != AllocationStorage::Heap)
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

void FramePlanner::decideByEscapeState(ObjectID object)
{
    const TrackedObject& tracked = m_escape.at(object);
    bool hasOneLiveInstance = tracked.liveInstances == LiveInstances::AtMostOne;
    switch (tracked.state) {
    case EscapeState::Unknown:
        moveToHeap(object, AllocationHeapCause::NotTracked, std::nullopt, reachedLimitOf(m_escape));
        return;
    case EscapeState::Escapes:
        if (hasOneLiveInstance && escapesOnlyIntoClosures(object))
            m_decisions[object.index()].storage = AllocationStorage::FrameUntilMaterialized;
        else
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

void FramePlanner::checkCellForms()
{
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (!mayBeInFrame(object))
            continue;

        const Value& allocation = definitionOf(object);
        AllocationHeapCause cause = allocation.opcode == Opcode::NewClosure ? checkClosureForm(allocation) : checkObjectForm(allocation);
        if (cause != AllocationHeapCause::None) {
            moveToHeap(object, cause);
            continue;
        }
        if (fitsInFrame(allocation))
            continue;
        GaveUp gaveUp = m_analyses.context().build.gaveUp(LimitID::MaxFrameEnvironmentFields, m_analyses.subject(m_escape.at(object).definition));
        moveToHeap(object, AllocationHeapCause::ExceedsLimit, std::nullopt, gaveUp.limit);
    }
}

AllocationHeapCause FramePlanner::checkClosureForm(const Value& closure) const
{
    const Function& callee = m_program.at(closure.function());
    if (closure.isMadeOnDemand())
        return AllocationHeapCause::LayoutDefersClosures;
    if (callee.has(FunctionFlags::TakesCallee) || !callee.hasOrdinaryFrame())
        return AllocationHeapCause::ClosureIsNotOnlyCalled;
    return AllocationHeapCause::None;
}

AllocationHeapCause FramePlanner::checkObjectForm(const Value& object) const
{
    LayoutID id = object.layout();
    const Layout& layout = m_program.at(id);
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
        if (flowOf(m_program, FieldReference { id, field }).location.deferral == ClosureDeferral::CapturesObject)
            return AllocationHeapCause::LayoutDefersClosures;
    }
    if (layout.flow.fieldCreation != FieldCreation::OnlyAtConstruction)
        return AllocationHeapCause::FieldsCreatedLater;
    return AllocationHeapCause::None;
}

bool FramePlanner::fitsInFrame(const Value& allocation)
{
    if (allocation.opcode != Opcode::NewObject)
        return true;
    return m_program.at(allocation.layout()).fields.size() <= m_analyses.context().build.limit(LimitID::MaxFrameEnvironmentFields);
}

void FramePlanner::checkUses()
{
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (!mayBeInFrame(object))
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
    if (effectRuleOf(user.opcode) == EffectRule::Access && provenAccessOf(m_program, m_function, user).isLookupByKey)
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
    if (mayLeakContentsThrough(entry)) {
        // A closure and an environment get what they hold after this check, and the verifier knows what an operand holds only where it sees the allocation.
        bool isSeenAsAllocation = m_analyses.sameObjectThroughCasts(user.operand(operandIndex)) == m_escape.at(object).definition;
        if (isClosure || m_program.at(allocation.layout()).isEnvironment() || !isSeenAsAllocation)
            return AllocationHeapCause::Escapes;
    }
    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
        if (!isClosure || !entry.uses.containsOnly(usesOfFrameCellsAllowedIn(m_program, user)))
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

void FramePlanner::checkSuspensions()
{
    CFG& cfg = m_analyses.cfg();
    auto isSuspensionPoint = [&](ValueID id) {
        return maySuspend(m_function.at(id).opcode) && m_analyses.effectsOf(id).normal.maySuspend;
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

void FramePlanner::checkReturnCalls()
{
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        ValueID call = terminator.callOperand();
        BlockID callBlock = m_analyses.useLists().blockOf(call);
        m_function.at(call).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            Use use { .kind = Use::Kind::Operand, .block = callBlock, .user = call, .operandIndex = operandIndex };
            for (ObjectID object : m_escape.pointsTo(operand).objects)
                moveToHeap(object, AllocationHeapCause::LiveAtReturnCall, use);
        });
    }
}

void FramePlanner::checkHolders()
{
    Vector<Vector<RetainedObject, 1>> retainedObjects(m_decisions.size());
    Vector<PendingMove, 8> worklist;
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (!isInFrame(object))
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
        bool leaksContents = isInFrame(cell) && std::ranges::any_of(m_escape.usesOf(cell), [&](const ObjectUse& entry) {
            return mayLeakContentsThrough(entry);
        });
        if (!leaksContents)
            continue;
        for (const Vector<RetainedObject, 1>& retainedInHolder : retainedObjects) {
            for (const RetainedObject& retained : retainedInHolder) {
                if (mayBeHeldIn(m_program, definitionOf(cell), referenceStoredBy(m_function.at(retained.use.user), retained.use.operandIndex)))
                    worklist.append(PendingMove { retained.object, AllocationHeapCause::Escapes, retained.use });
            }
        }
    }
    moveToHeapWithRetainedObjects();

    // FramePointers follows a read of a frame cell by its reference alone, whichever cell the pointer was stored in. The uses of
    // such a read were checked above only if the read may give a cell that is in the frame.
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
        && m_escape.at(holder).liveInstances == LiveInstances::AtMostOne
        && m_escape.at(retained).liveInstances == LiveInstances::AtMostOne;
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

BitVector FramePlanner::findFrameClosures() const
{
    BitVector frameClosures(m_function.values.size());
    for (size_t index : m_isCandidate) {
        ObjectID object(index);
        if (isInFrame(object) && definitionOf(object).opcode == Opcode::NewClosure)
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
        return mayLeakCapture(m_flow, closure, *closure.variadicOperandIndex(entry.use.operandIndex));
    });
    if (isLeakedByFrameClosure)
        return false;
    return EnvironmentMaterialization(m_analyses, m_escape.at(environment).definition, frameClosures).canRun();
}

void FramePlanner::finishPlan()
{
    bool collectsRemarks = m_analyses.context().build.options().collectsAllocationRemarks();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (!hasStorageChoice(value)) {
                if (collectsRemarks && Value::opcodesWithAllocationSite.contains(value.opcode) && value.allocationSite())
                    m_plan.decisions.append(createHeapDecision(m_escape, id));
                continue;
            }

            std::optional<ObjectID> object = m_escape.objectDefinedBy(id);
            if (object && isCandidate(*object)) {
                const FrameDecision& decision = m_decisions[object->index()];
                m_plan.decisions.append(decision);
                addAllocation(m_plan, m_program, value, decision.storage);
                continue;
            }
            if (value.storage() == AllocationStorage::Heap)
                m_plan.decisions.append(FrameDecision { .allocation = id, .cause = AllocationHeapCause::NotTracked, .limit = reachedLimitOf(m_escape) });
            addAllocation(m_plan, m_program, value, value.storage());
        }
    }
    std::ranges::sort(m_plan.decisions, { }, &FrameDecision::allocation);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
