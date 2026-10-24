/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocationPointers.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLiveness.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static bool canHoldFramePointers(const Value& cell)
{
    return hasFrameStorage(cell) && cell.storage() == AllocationStorage::Frame && !arrayAllocationsWithStorage.contains(cell.opcode);
}

namespace {

class FramePointerVerifier {
    WTF_MAKE_NONCOPYABLE(FramePointerVerifier);
public:
    FramePointerVerifier(FunctionAnalyses&, const FramePointers&, Vector<InternalError>& found);

    void run();

private:
    void verifyOperand(BlockID, ValueID user, uint32_t operandIndex);
    void verifyRetainedOperand(BlockID, ValueID user, uint32_t operandIndex, RetainedIn);
    void verifyTerminator(BlockID);
    void verifySuspensions(BlockID);
    void verifyClosure(BlockID, ValueID allocation);
    void verifyIterations(ValueID allocation, ValueLocation);

    void report(InternalErrorKind, BlockID, ValueID);
    bool mayLeakHeldPointer(const Value& user, uint32_t operandIndex) const;
    bool canHoldInitialValue(const Value& cell, uint32_t operandIndex) const;
    bool retainsInFrameCell(const Value& user, uint32_t operandIndex) const;
    bool isHolderOfFrameCell(ValueID);
    bool takesEdgeAsFrameArray(Edge, ValueID identity) const;
    bool reachesAsFrameArray(ValueID, BlockID);
    const BitVector& arraysByPaths();
    bool mayBeFrameArrayIn(BlockID block, ValueID id) { return arraysByPaths().quickGet(id.index()) && reachesAsFrameArray(id, block); }
    bool mayPointIntoFrameIn(BlockID, ValueID);
    BitVector computeHolders(std::span<const ValueID> allocations) const;

    const Program& m_program;
    const FlowResults* m_flow;
    FunctionID m_id;
    const Function& m_function;
    Vector<InternalError>& m_found;
    const FramePointers& m_pointers;
    FramePointers m_arrays;
    std::optional<FramePointers> m_pointersBesideArrays;
    EffectSummaries m_unknownSummaries;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    const BlockCycles& m_cycles;
    Liveness m_liveness;
    const ValueIdentities& m_identities;
    std::optional<BitVector> m_frameCellHolders;
    Vector<std::pair<ValueID, BitVector>, 2> m_blocksReachedAsFrameArray;
    std::optional<BitVector> m_arraysByPaths;
};

FramePointerVerifier::FramePointerVerifier(FunctionAnalyses& analyses, const FramePointers& pointers, Vector<InternalError>& found)
    : m_program(analyses.program())
    , m_flow(analyses.context().flow)
    , m_id(analyses.context().id)
    , m_function(analyses.function())
    , m_found(found)
    , m_pointers(pointers)
    , m_arrays(m_program, m_function, FrameCells::Arrays)
    , m_cfg(analyses.cfg())
    , m_useLists(analyses.useLists())
    , m_cycles(analyses.blockCycles())
    , m_liveness(m_function, m_cfg, m_useLists, { .trackedValues = &pointers.values() })
    , m_identities(analyses.identities())
{
    if (!m_arrays.isEmpty())
        m_pointersBesideArrays.emplace(m_program, m_function, FrameCells::AllButArrays);
}

void FramePointerVerifier::run()
{
    const FramePointers& otherCells = m_pointersBesideArrays ? *m_pointersBesideArrays : m_pointers;
    for (BlockID block : m_cfg.reversePostOrder()) {
        for (ValueID userID : m_function.at(block).values) {
            const Value& user = m_function.at(userID);
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (m_arrays.contains(operand) && !mayBeGivenFrameArray(m_program, m_flow, m_function, user, operandIndex) && mayBeFrameArrayIn(block, operand))
                    report(effectRuleOf(user.opcode) == EffectRule::Call ? InternalErrorKind::CalleeRetainsFramePointer : InternalErrorKind::FramePointerRetained, block, userID);
                if (otherCells.contains(operand))
                    verifyOperand(block, userID, operandIndex);
            });
        }
        verifyTerminator(block);
        verifySuspensions(block);
    }
    for (ValueID allocation : m_pointers.allocations()) {
        ValueLocation location = m_useLists.locationOf(allocation);
        if (location.placement != ValuePlacement::InBlock)
            continue;
        if (m_function.at(allocation).opcode == Opcode::NewClosure)
            verifyClosure(location.block, allocation);
        if (m_cycles.isInCycle(location.block))
            verifyIterations(allocation, location);
    }
}

void FramePointerVerifier::report(InternalErrorKind kind, BlockID block, ValueID value)
{
    m_found.append(InternalError { .kind = kind, .function = m_id, .block = block, .value = value, .location = m_function.locationOf(value) });
}

void FramePointerVerifier::verifyOperand(BlockID block, ValueID userID, uint32_t operandIndex)
{
    const Value& user = m_function.at(userID);
    if (user.operandKind(operandIndex) == OperandKind::Guard)
        return;

    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Call:
        if (mayLeakFramePointer(m_program, m_flow, m_function, user, operandIndex) || mayLeakHeldPointer(user, operandIndex))
            report(InternalErrorKind::CalleeRetainsFramePointer, block, userID);
        return;
    case EffectRule::Builtin:
    case EffectRule::Inline:
        if (mayLeakFramePointer(m_program, m_flow, m_function, user, operandIndex) || mayLeakHeldPointer(user, operandIndex))
            report(InternalErrorKind::FramePointerRetained, block, userID);
        return;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
        break;
    }

    if (user.opcode == Opcode::MaterializeEnvironment) {
        if (isHolderOfFrameCell(user.operand(operandIndex)))
            report(InternalErrorKind::FramePointerRetained, block, userID);
        return;
    }
    OperandSummary summary = summarizeFramePointerUse(m_program, m_flow, m_function, user, operandIndex);
    if (summary.uses.contains(OperandUse::Retains) && summary.retainedIn != RetainedIn::UnknownMemory) {
        if (summary.uses.containsAny(unfollowedLeakingUses) || mayLeakHeldPointer(user, operandIndex))
            report(InternalErrorKind::FramePointerRetained, block, userID);
        else
            verifyRetainedOperand(block, userID, operandIndex, summary.retainedIn);
        return;
    }
    if (mayLeakFramePointer(m_program, m_flow, m_function, user, operandIndex) || mayLeakHeldPointer(user, operandIndex))
        report(InternalErrorKind::FramePointerRetained, block, userID);
}

bool FramePointerVerifier::mayLeakHeldPointer(const Value& user, uint32_t operandIndex) const
{
    return mayLeakContents(m_program, m_flow, m_function, user, operandIndex) && m_pointers.mayHoldPointers(m_program, m_function, user.operand(operandIndex));
}

void FramePointerVerifier::verifyRetainedOperand(BlockID block, ValueID userID, uint32_t operandIndex, RetainedIn retainedIn)
{
    const Value& user = m_function.at(userID);
    switch (retainedIn) {
    case RetainedIn::UnknownMemory:
        report(InternalErrorKind::FramePointerRetained, block, userID);
        return;
    case RetainedIn::Result:
        if (!canHoldInitialValue(user, operandIndex))
            report(InternalErrorKind::FramePointerRetained, block, userID);
        if (user.opcode == Opcode::NewClosure && mayLeakCapture(m_program, m_flow, user, *user.variadicOperandIndex(operandIndex)))
            report(InternalErrorKind::CalleeRetainsFramePointer, block, userID);
        return;
    case RetainedIn::Operand:
        if (!retainsInFrameCell(user, operandIndex))
            report(InternalErrorKind::FramePointerRetained, block, userID);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FramePointerVerifier::canHoldInitialValue(const Value& cell, uint32_t operandIndex) const
{
    std::optional<uint32_t> position = cell.variadicOperandIndex(operandIndex);
    if (!canHoldFramePointers(cell) || !position)
        return false;
    if (cell.opcode == Opcode::NewClosure)
        return true;
    return m_program.at(cell.layout()).kind == LayoutKind::Environment && !mayLeakStoredValue(m_flow, FieldReference { cell.layout(), cell.fields()[*position] });
}

bool FramePointerVerifier::retainsInFrameCell(const Value& user, uint32_t operandIndex) const
{
    if (!storesFollowedByFramePointers.contains(user.opcode))
        return false;
    ValueID container = m_identities.identityOf(cellAccessedBy(user));
    if (!canHoldFramePointers(m_function.at(container)))
        return false;
    if (user.opcode == Opcode::PutEnvironmentField && mayLeakStoredValue(m_flow, FieldReference { user.layout(), user.field() }))
        return false;
    ValueID stored = m_identities.identityOf(user.operand(operandIndex));
    return !hasFrameStorage(m_function.at(stored)) || m_cycles.innermostCycleOf(m_useLists.blockOf(container)) == m_cycles.innermostCycleOf(m_useLists.blockOf(stored));
}

bool FramePointerVerifier::isHolderOfFrameCell(ValueID id)
{
    if (!m_frameCellHolders) {
        Vector<ValueID, 8> frameCells;
        for (ValueID allocation : m_pointers.allocations()) {
            if (canHoldFramePointers(m_function.at(allocation)))
                frameCells.append(allocation);
        }
        m_frameCellHolders = computeHolders(frameCells.span());
    }
    return m_frameCellHolders->get(id.index());
}

// What AOTFrameArrayParameters.cpp allows in a function that is handed an array is allowed where a part of that function was taken into its caller:
// an array in a frame leaves a test of it on the side that the test has for every such array.
bool FramePointerVerifier::takesEdgeAsFrameArray(Edge edge, ValueID identity) const
{
    const Terminator& terminator = m_function.at(edge.from).terminator;
    if (edge.isHandlerEdge() || !terminator.isConditionalBranch())
        return true;
    std::optional<FrameArrayTest> test = testOfFrameArray(m_program, m_function.at(terminator.conditionOperand()));
    bool isTakenEdge = !edge.targetIndex;
    return !test || test->passes == isTakenEdge || m_identities.identityOf(test->tested) != identity;
}

bool FramePointerVerifier::reachesAsFrameArray(ValueID id, BlockID block)
{
    ValueID identity = m_identities.identityOf(id);
    for (const auto& [known, reached] : m_blocksReachedAsFrameArray) {
        if (known == identity)
            return reached.quickGet(block.index());
    }

    ValueLocation location = m_useLists.locationOf(identity);
    if (location.placement == ValuePlacement::Detached)
        return true;
    BitVector reached(m_function.blocks.size());
    reached.quickSet(location.block.index());
    Vector<BlockID, 16> worklist { location.block };
    while (!worklist.isEmpty()) {
        forEachOutgoingEdge(m_function, worklist.takeLast(), [&](Edge edge, const Target& target) {
            if (takesEdgeAsFrameArray(edge, identity) && !reached.quickSet(target.block.index()))
                worklist.append(target.block);
        });
    }
    m_blocksReachedAsFrameArray.append({ identity, WTF::move(reached) });
    return m_blocksReachedAsFrameArray.last().second.quickGet(block.index());
}

const BitVector& FramePointerVerifier::arraysByPaths()
{
    if (m_arraysByPaths)
        return *m_arraysByPaths;

    BitVector arrays(m_function.values.size());
    for (ValueID allocation : m_arrays.allocations())
        arrays.quickSet(allocation.index());
    auto isArrayIn = [&](BlockID block, ValueID id) {
        return arrays.quickGet(id.index()) && reachesAsFrameArray(id, block);
    };
    propagateUntilNothingChanges(m_function, m_cfg.reversePostOrder(), [&](BlockID block, ValueID id) {
        if (!m_arrays.contains(id) || arrays.quickGet(id.index()))
            return false;
        bool isArray = false;
        forEachReturnedOperand(m_function, m_function.at(id), [&](ValueID operand) {
            isArray |= isArrayIn(block, operand);
        });
        return isArray && !arrays.quickSet(id.index());
    }, [&](Edge edge, ValueID passed, ValueID parameter) {
        return m_arrays.contains(parameter) && isArrayIn(edge.from, passed) && takesEdgeAsFrameArray(edge, m_identities.identityOf(passed)) && !arrays.quickSet(parameter.index());
    });
    m_arraysByPaths = WTF::move(arrays);
    return *m_arraysByPaths;
}

bool FramePointerVerifier::mayPointIntoFrameIn(BlockID block, ValueID id)
{
    if (!m_pointers.contains(id))
        return false;
    bool isArrayAtMost = m_pointersBesideArrays && !m_pointersBesideArrays->contains(id);
    return !isArrayAtMost || mayBeFrameArrayIn(block, id);
}

void FramePointerVerifier::verifyTerminator(BlockID block)
{
    const Terminator& terminator = m_function.at(block).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall) {
        const Value& call = m_function.at(terminator.callOperand());
        bool passesFramePointer = false;
        call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            passesFramePointer |= mayPointIntoFrameIn(block, operand) && !isLentTo(m_function, call, operandIndex);
        });
        if (passesFramePointer)
            report(InternalErrorKind::FramePointerInTailCall, block, terminator.callOperand());
    }
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!mayPointIntoFrameIn(block, operand))
            return;
        switch (terminator.kind) {
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
            report(InternalErrorKind::FramePointerReturned, block, operand);
            return;
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
            report(InternalErrorKind::FramePointerThrown, block, operand);
            return;
        case TerminatorKind::Jump:
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            if (mayLeakFramePointer(m_program, m_flow, m_function, terminator, operandIndex))
                report(InternalErrorKind::FramePointerRetained, block, operand);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

void FramePointerVerifier::verifySuspensions(BlockID block)
{
    const BlockValues& values = m_function.at(block).values;
    EffectContext context { m_program, m_unknownSummaries };
    auto isSuspensionPoint = [&](ValueID id) {
        const Value& value = m_function.at(id);
        return maySuspend(value.opcode) && effectsOf(context, m_function, value).normal.maySuspend;
    };
    if (std::ranges::none_of(values, isSuspensionPoint))
        return;

    Liveness::LocalCalc liveAfterValue(m_liveness, block);
    for (uint32_t index = values.size(); index--;) {
        if (isSuspensionPoint(values[index])) {
            bool isFramePointerLive = false;
            liveAfterValue.forEachLive([&](ValueID) {
                isFramePointerLive = true;
            });
            if (isFramePointerLive)
                report(InternalErrorKind::FramePointerAcrossSuspension, block, values[index]);
        }
        liveAfterValue.execute(index);
    }
}

void FramePointerVerifier::verifyClosure(BlockID block, ValueID allocation)
{
    const Value& closure = m_function.at(allocation);
    std::optional<unsigned> calleeParameterIndex = m_program.at(closure.function()).calleeParameterIndex();
    if (calleeParameterIndex && (!m_flow || m_flow->framePointerSummary(closure.function(), *calleeParameterIndex).uses.containsAny(usesLeakingFramePointers)))
        report(InternalErrorKind::CalleeRetainsFramePointer, block, allocation);
    if (!closure.isLent())
        return;

    Vector<Use, 4> uses;
    Vector<ValueID, 2> worklist { allocation };
    while (!worklist.isEmpty()) {
        for (const Use& use : m_useLists.usesOf(worklist.takeLast())) {
            if (use.kind == Use::Kind::Operand && isCastOrConversion(m_function.at(use.user).opcode))
                worklist.append(use.user);
            else
                uses.append(use);
        }
    }
    bool isLentLast = std::ranges::all_of(uses, [&](const Use& use) {
        if (use.kind != Use::Kind::Operand)
            return false;
        return !isLentTo(m_function, m_function.at(use.user), use.operandIndex) || isLastUse(m_function, m_cfg, m_useLists, allocation, uses.span(), use);
    });
    if (!isLentLast || closure.storage() != AllocationStorage::Frame)
        report(InternalErrorKind::FramePointerRetained, block, allocation);
}

void FramePointerVerifier::verifyIterations(ValueID allocation, ValueLocation location)
{
    Liveness::LocalCalc liveBeforeAllocation(m_liveness, location.block);
    for (uint32_t index = m_function.at(location.block).values.size(); index > location.index;)
        liveBeforeAllocation.execute(--index);

    std::optional<BitVector> holders;
    bool isHolderLive = false;
    liveBeforeAllocation.forEachLive([&](ValueID id) {
        if (!holders)
            holders = computeHolders(singleElementSpan(allocation));
        isHolderLive |= holders->get(id.index());
    });
    if (isHolderLive)
        report(InternalErrorKind::FrameCellAcrossIterations, location.block, allocation);
}

BitVector FramePointerVerifier::computeHolders(std::span<const ValueID> allocations) const
{
    BitVector holders(m_function.values.size());
    for (ValueID allocation : allocations)
        holders.quickSet(allocation.index());
    auto isHolder = [&](ValueID id) {
        return holders.get(id.index());
    };
    auto add = [&](ValueID id) {
        return m_pointers.contains(id) && !holders.quickSet(id.index());
    };

    propagateUntilNothingChanges(m_function, m_cfg.reversePostOrder(), [&](BlockID, ValueID id) {
        const Value& value = m_function.at(id);
        bool isStore = storesFollowedByFramePointers.contains(value.opcode);
        bool takesHolder = false;
        if (isStore || Value::opcodesWithStorage.contains(value.opcode)) {
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                takesHolder |= isHolder(operand) && (!isStore || value.operandUses(operandIndex).contains(OperandUse::Retains));
            });
        } else if (readsFollowedByFramePointers.contains(value.opcode))
            takesHolder = isHolder(cellAccessedBy(value));
        else {
            forEachReturnedOperand(m_function, value, [&](ValueID operand) {
                takesHolder |= isHolder(operand);
            });
        }
        return takesHolder && add(isStore ? m_identities.identityOf(cellAccessedBy(value)) : id);
    }, [&](Edge, ValueID passed, ValueID parameter) {
        return isHolder(passed) && add(parameter);
    });
    return holders;
}

} // anonymous namespace

void verifyFramePointers(FunctionAnalyses& analyses, Vector<InternalError>& found)
{
    FramePointers pointers(analyses.program(), analyses.function());
    if (!pointers.isEmpty())
        FramePointerVerifier(analyses, pointers, found).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
