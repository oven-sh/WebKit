/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocationPointers.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTCFG.h"
#include "AOTEdgeResults.h"
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

constexpr OperandUses unfollowedLeakingUses = usesLeakingFramePointers - usesLeakingContents - OperandUses { OperandUse::Retains, OperandUse::Returns };

class FramePointerVerifier {
    WTF_MAKE_NONCOPYABLE(FramePointerVerifier);
public:
    FramePointerVerifier(FunctionAnalyses&, const FramePointers&, const LivenessOptions&);

    Vector<InternalError> run();

private:
    void verifyOperands(BlockID);
    void verifyOperand(BlockID, ValueID user, uint32_t operandIndex);
    void verifyArrayOperand(BlockID, ValueID user, uint32_t operandIndex);
    void verifyRetainedOperand(BlockID, ValueID user, uint32_t operandIndex, RetainedIn);
    void verifyTerminator(BlockID);
    void verifyTerminatorOperand(BlockID, const Terminator&, uint32_t operandIndex);
    void verifyReturnCall(BlockID, ValueID call);
    void verifySuspensions(BlockID);
    void verifyCalleeParameter(BlockID, ValueID allocation);
    void verifyLentClosure(BlockID, ValueID allocation);
    void verifyIterations(ValueID allocation, ValueLocation);

    void report(InternalErrorKind, BlockID, ValueID);
    OperandUses argumentUses(FunctionID callee, uint32_t entryParameter) const;
    bool mayLeakHeldPointer(const Value& user, uint32_t operandIndex) const;
    bool canHoldInitialValue(const Value& cell, uint32_t operandIndex) const;
    bool retainsInFrameCell(const Value& user, uint32_t operandIndex) const;
    bool dieTogether(ValueID container, ValueID stored) const;
    bool isHolderOfFrameCell(ValueID);
    std::optional<ValueID> findHolder(ValueID, const BitVector& holders) const;
    bool addHolder(ValueID, BitVector& holders) const;
    bool addHoldersAcrossEdge(Edge, const Target&, BitVector& holders) const;
    BitVector computeHolders(std::span<const ValueID> allocations) const;

    const Program& m_program;
    const FlowResults* m_flow;
    FunctionID m_id;
    const Function& m_function;
    Vector<InternalError> m_found;
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
};

FramePointerVerifier::FramePointerVerifier(FunctionAnalyses& analyses, const FramePointers& pointers, const LivenessOptions& livenessOptions)
    : m_program(analyses.program())
    , m_flow(analyses.context().flow)
    , m_id(analyses.context().id)
    , m_function(analyses.function())
    , m_pointers(pointers)
    , m_arrays(m_program, m_function, FrameCells::Arrays)
    , m_cfg(analyses.cfg())
    , m_useLists(analyses.useLists())
    , m_cycles(analyses.blockCycles())
    , m_liveness(m_function, m_cfg, m_useLists, livenessOptions)
    , m_identities(analyses.identities())
{
    if (!m_arrays.isEmpty())
        m_pointersBesideArrays.emplace(m_program, m_function, FrameCells::AllButArrays);
}

Vector<InternalError> FramePointerVerifier::run()
{
    for (BlockID block : m_cfg.reversePostOrder()) {
        verifyOperands(block);
        verifyTerminator(block);
        verifySuspensions(block);
    }
    for (ValueID allocation : m_pointers.allocations()) {
        ValueLocation location = m_useLists.locationOf(allocation);
        bool isReachable = location.placement == ValuePlacement::InBlock;
        if (!isReachable)
            continue;
        verifyCalleeParameter(location.block, allocation);
        verifyLentClosure(location.block, allocation);
        if (m_cycles.isInCycle(location.block))
            verifyIterations(allocation, location);
    }
    return WTF::move(m_found);
}

void FramePointerVerifier::report(InternalErrorKind kind, BlockID block, ValueID value)
{
    m_found.append(InternalError { .kind = kind, .function = m_id, .block = block, .value = value, .location = m_function.locationOf(value) });
}

OperandUses FramePointerVerifier::argumentUses(FunctionID callee, uint32_t entryParameter) const
{
    return m_flow ? m_flow->framePointerSummary(callee, entryParameter).uses : OperandUses::all();
}

void FramePointerVerifier::verifyOperands(BlockID block)
{
    const FramePointers& otherCells = m_pointersBesideArrays ? *m_pointersBesideArrays : m_pointers;
    for (ValueID user : m_function.at(block).values) {
        m_function.at(user).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!m_arrays.isEmpty() && m_arrays.contains(operand))
                verifyArrayOperand(block, user, operandIndex);
            if (otherCells.contains(operand))
                verifyOperand(block, user, operandIndex);
        });
    }
}

void FramePointerVerifier::verifyArrayOperand(BlockID block, ValueID userID, uint32_t operandIndex)
{
    const Value& user = m_function.at(userID);
    if (mayBeGivenFrameArray(m_program, m_flow, m_function, user, operandIndex))
        return;
    report(effectRuleOf(user.opcode) == EffectRule::Call ? InternalErrorKind::CalleeRetainsFramePointer : InternalErrorKind::FramePointerRetained, block, userID);
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
    return !hasFrameStorage(m_function.at(stored)) || dieTogether(container, stored);
}

bool FramePointerVerifier::dieTogether(ValueID container, ValueID stored) const
{
    return m_cycles.innermostCycleOf(m_useLists.blockOf(container)) == m_cycles.innermostCycleOf(m_useLists.blockOf(stored));
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

void FramePointerVerifier::verifyTerminator(BlockID block)
{
    const Terminator& terminator = m_function.at(block).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall)
        verifyReturnCall(block, terminator.callOperand());
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (m_pointers.contains(operand))
            verifyTerminatorOperand(block, terminator, operandIndex);
    });
}

void FramePointerVerifier::verifyTerminatorOperand(BlockID block, const Terminator& terminator, uint32_t operandIndex)
{
    ValueID operand = terminator.operand(operandIndex);
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
}

void FramePointerVerifier::verifyReturnCall(BlockID block, ValueID call)
{
    bool passesFramePointer = false;
    m_function.at(call).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        passesFramePointer |= m_pointers.contains(operand) && !isLentTo(m_function, m_function.at(call), operandIndex);
    });
    if (passesFramePointer)
        report(InternalErrorKind::FramePointerInTailCall, block, call);
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

void FramePointerVerifier::verifyCalleeParameter(BlockID block, ValueID allocation)
{
    const Value& value = m_function.at(allocation);
    if (value.opcode != Opcode::NewClosure)
        return;
    std::optional<unsigned> calleeParameterIndex = m_program.at(value.function()).calleeParameterIndex();
    if (calleeParameterIndex && argumentUses(value.function(), *calleeParameterIndex).containsAny(usesLeakingFramePointers))
        report(InternalErrorKind::CalleeRetainsFramePointer, block, allocation);
}

void FramePointerVerifier::verifyLentClosure(BlockID block, ValueID allocation)
{
    const Value& closure = m_function.at(allocation);
    if (closure.opcode != Opcode::NewClosure || !closure.isLent())
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

    Vector<ValueID, 8> liveFramePointers;
    liveBeforeAllocation.forEachLive([&](ValueID id) {
        liveFramePointers.append(id);
    });
    if (liveFramePointers.isEmpty())
        return;

    BitVector holders = computeHolders(singleElementSpan(allocation));
    bool isHolderLive = std::ranges::any_of(liveFramePointers, [&](ValueID id) {
        return holders.get(id.index());
    });
    if (isHolderLive)
        report(InternalErrorKind::FrameCellAcrossIterations, location.block, allocation);
}

std::optional<ValueID> FramePointerVerifier::findHolder(ValueID id, const BitVector& holders) const
{
    const Value& value = m_function.at(id);
    auto isHolder = [&](ValueID operand) {
        return holders.get(operand.index());
    };

    if (storesFollowedByFramePointers.contains(value.opcode)) {
        bool storesHolder = false;
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (value.operandUses(operandIndex).contains(OperandUse::Retains))
                storesHolder |= isHolder(operand);
        });
        if (!storesHolder)
            return std::nullopt;
        return m_identities.identityOf(cellAccessedBy(value));
    }

    bool isResultHolder = false;
    if (readsFollowedByFramePointers.contains(value.opcode))
        isResultHolder = isHolder(cellAccessedBy(value));
    else if (Value::opcodesWithStorage.contains(value.opcode)) {
        value.forEachOperand([&](ValueID operand, uint32_t) {
            isResultHolder |= isHolder(operand);
        });
    } else {
        forEachReturnedOperand(m_function, value, [&](ValueID operand) {
            isResultHolder |= isHolder(operand);
        });
    }
    if (!isResultHolder)
        return std::nullopt;
    return id;
}

bool FramePointerVerifier::addHolder(ValueID id, BitVector& holders) const
{
    return m_pointers.contains(id) && !holders.quickSet(id.index());
}

bool FramePointerVerifier::addHoldersAcrossEdge(Edge edge, const Target& target, BitVector& holders) const
{
    const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
    bool changed = false;
    for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
        if (holders.get(target.arguments[argumentIndex].index()))
            changed |= addHolder(parameters[argumentIndex], holders);
    }
    if (edge.isHandlerEdge())
        return changed;

    uint32_t firstParameter = firstEdgeResultParameter(m_function, edge.from, edge.targetIndex);
    uint32_t numberOfResults = numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        EdgeResult result = edgeResultAt(m_function, { edge.from, edge.targetIndex, resultIndex });
        if (result.kind == EdgeResultKind::NarrowedOperand && holders.get(result.subject.index()))
            changed |= addHolder(parameters[firstParameter + resultIndex], holders);
    }
    return changed;
}

BitVector FramePointerVerifier::computeHolders(std::span<const ValueID> allocations) const
{
    BitVector holders(m_function.values.size());
    for (ValueID allocation : allocations)
        holders.quickSet(allocation.index());

    for (bool changed = true; changed;) {
        changed = false;
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (std::optional<ValueID> holder = findHolder(id, holders))
                    changed |= addHolder(*holder, holders);
            }
            forEachOutgoingEdge(m_function, block, [&](Edge edge, const Target& target) {
                changed |= addHoldersAcrossEdge(edge, target, holders);
            });
        }
    }
    return holders;
}

} // anonymous namespace

void verifyFramePointers(FunctionAnalyses& analyses, Vector<InternalError>& found)
{
    FramePointers pointers(analyses.program(), analyses.function());
    if (pointers.isEmpty())
        return;
    LivenessOptions livenessOptions;
    livenessOptions.trackedValues = &pointers.values();
    FramePointerVerifier verifier(analyses, pointers, livenessOptions);
    found.appendVector(verifier.run());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
