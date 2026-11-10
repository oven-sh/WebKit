/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT)

#include "AllowMacroScratchRegisterUsage.h"
#include <algorithm>
#include <type_traits>
#include <wtf/HashFunctions.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Machine {

static_assert(std::is_trivially_destructible_v<PatchParameters>);

static constexpr uint32_t emptySharedLatePathSlot = UINT32_MAX;
static constexpr size_t smallestNumberOfSharedLatePathSlots = 8;

static size_t firstSharedLatePathSlotOf(std::span<const uint32_t> slots, const PatchGenerator* generator)
{
    ASSERT(hasOneBitSet(slots.size()));
    return PtrHash<const PatchGenerator*>::hash(generator) & (slots.size() - 1);
}

static size_t nextSharedLatePathSlot(std::span<const uint32_t> slots, size_t slot)
{
    return (slot + 1) & (slots.size() - 1);
}

static void noteSharedLatePathInSlots(Encoder& encoder, uint32_t indexOfLatePath)
{
    std::span<uint32_t> slots = encoder.m_sharedLatePathSlots;
    size_t slot = firstSharedLatePathSlotOf(slots, encoder.m_sharedLatePaths[indexOfLatePath].generator);
    while (slots[slot] != emptySharedLatePathSlot)
        slot = nextSharedLatePathSlot(slots, slot);
    slots[slot] = indexOfLatePath;
}

static void addSharedLatePath(Encoder& encoder, const Encoder::SharedLatePath& latePath)
{
    uint32_t index = encoder.m_sharedLatePaths.size();
    encoder.m_sharedLatePaths.append(encoder.m_arena, latePath);

    size_t numberOfSlotsNeeded = 2 * (static_cast<size_t>(index) + 1);
    if (numberOfSlotsNeeded <= encoder.m_sharedLatePathSlots.size()) {
        noteSharedLatePathInSlots(encoder, index);
        return;
    }

    size_t numberOfSlots = std::max(smallestNumberOfSharedLatePathSlots, 2 * encoder.m_sharedLatePathSlots.size());
    ASSERT(numberOfSlotsNeeded <= numberOfSlots);
    encoder.m_sharedLatePathSlots = encoder.m_arena.createSpan<uint32_t>(numberOfSlots);
    std::ranges::fill(encoder.m_sharedLatePathSlots, emptySharedLatePathSlot);
    for (uint32_t known = 0; known <= index; ++known)
        noteSharedLatePathInSlots(encoder, known);
}

static void restoreValueRepFromRegister(CCallHelpers& jit, Reg source, Reg destination)
{
    if (source == destination)
        return;
    if (source.isGPR()) {
        if (destination.isGPR())
            jit.move(source.gpr(), destination.gpr());
        else
            jit.move64ToDouble(source.gpr(), destination.fpr());
        return;
    }
    if (destination.isGPR())
        jit.moveDoubleTo64(source.fpr(), destination.gpr());
    else
        jit.moveDouble(source.fpr(), destination.fpr());
}

static void restoreValueRepFromMemory(CCallHelpers& jit, MacroAssembler::Address source, Reg destination)
{
    if (destination.isGPR())
        jit.load64(source, destination.gpr());
    else
        jit.loadDouble(source, destination.fpr());
}

static void restoreValueRepFromConstant(CCallHelpers& jit, int64_t bits, Reg destination)
{
    if (destination.isGPR()) {
        jit.move(MacroAssembler::TrustedImm64(bits), destination.gpr());
        return;
    }
    GPRReg scratch = jit.scratchRegister();
    jit.move(MacroAssembler::TrustedImm64(bits), scratch);
    jit.move64ToDouble(scratch, destination.fpr());
}

static std::span<MacroAssembler::Jump> oneJumpOfFailingCheck(Encoder& encoder, MacroAssembler::Jump jump)
{
    ASSERT(jump.isSet());
    std::span<MacroAssembler::Jump> jumps = encoder.m_arena.createSpan<MacroAssembler::Jump>(1);
    jumps[0] = jump;
    return jumps;
}

static std::span<MacroAssembler::Jump> jumpsOfFailingCheck(Encoder& encoder, const Step& step, const PatchOperation* check)
{
    bool failsWhenItHolds = !check->failsWhenPredicateIsZero();
#if CPU(X86_64)
    if (std::optional<MacroAssembler::RelationalCondition> condition = encoder.tryEmitFlagsOfPredicate(step, check->child(0)))
        return oneJumpOfFailingCheck(encoder, encoder.m_jit.branchOnFlags(failsWhenItHolds ? *condition : MacroAssembler::invert(*condition)));
#endif
    LeafCursor leaves(step);
    Test test = encoder.resolveTest(check, leaves);
#if CPU(X86_64)
    if (test.kind != Test::CompareNumbers)
        return oneJumpOfFailingCheck(encoder, encoder.jumpOnIntegers(test, failsWhenItHolds));
#endif
    MacroAssembler::JumpList failures = encoder.branchOn(test, failsWhenItHolds);
    std::span<MacroAssembler::Jump> jumps = encoder.m_arena.createSpan<MacroAssembler::Jump>(failures.jumps().size());
    std::ranges::copy(failures.jumps().span(), jumps.begin());
    return jumps;
}

void Encoder::encodeCheck(const Step& step, PatchOperation* check)
{
    ASSERT(check->opcode() == Check);
    RELEASE_ASSERT(check->generator());
    RELEASE_ASSERT(m_frameIsSetUp || check->runsWithoutFrame());

    std::span<MacroAssembler::Jump> jumps;
    if (const Operation* predicate = check->child(0); predicate->hasInt()) {
        RELEASE_ASSERT(!check->failsWhenPredicateIsZero());
        if (!predicate->asInt())
            return;
        jumps = oneJumpOfFailingCheck(*this, m_jit.jump());
    } else
        jumps = jumpsOfFailingCheck(*this, step, check);

    PatchParameters* parameters = new (NotNull, m_arena.allocate(sizeof(PatchParameters), alignof(PatchParameters))) PatchParameters(*this, check);
    fillParameters<true>(*parameters, step, check);

    if (const SharedLatePath* shared = findSharedLatePath(check, parameters->m_reps)) {
        auto linkToSharedLatePath = [jumps, entry = shared->entry](CCallHelpers& jit) {
            RELEASE_ASSERT(entry->isSet());
            for (const MacroAssembler::Jump& jump : jumps)
                jump.linkTo(*entry, &jit);
        };
        addLatePath(m_arena.create<LatePathOf<decltype(linkToSharedLatePath)>>(linkToSharedLatePath));
        return;
    }

    MacroAssembler::Label* entry = m_arena.create<MacroAssembler::Label>();
    if (check->sharesLatePath())
        addSharedLatePath(*this, SharedLatePath { check->generator(), parameters->m_reps, m_frameIsSetUp, entry });
    auto runGenerator = [jumps, entry, parameters](CCallHelpers& jit) {
        for (const MacroAssembler::Jump& jump : jumps)
            jump.link(&jit);
        *entry = jit.label();
        parameters->value()->generator()->run(jit, *parameters);
    };
    addLatePath(m_arena.create<LatePathOf<decltype(runGenerator)>>(runGenerator));
}

Encoder::SharedLatePath* Encoder::findSharedLatePath(PatchOperation* check, std::span<const ValueRep> reps)
{
    if (!check->sharesLatePath() || m_sharedLatePathSlots.empty())
        return nullptr;

    std::span<const uint32_t> slots = m_sharedLatePathSlots;
    for (size_t slot = firstSharedLatePathSlotOf(slots, check->generator()); slots[slot] != emptySharedLatePathSlot; slot = nextSharedLatePathSlot(slots, slot)) {
        SharedLatePath& candidate = m_sharedLatePaths[slots[slot]];
        if (candidate.generator == check->generator() && candidate.frameIsSetUp == m_frameIsSetUp && std::ranges::equal(candidate.reps, reps))
            return &candidate;
    }
    return nullptr;
}

RegisterSet PatchParameters::unavailableRegisters() const
{
    RegisterMask unsavedCalleeSaves = maskOf(RegisterSet::calleeSaveRegisters()) | registerFileFor(proc().traits()).calleeSaved;
    if (m_frameIsSetUp)
        unsavedCalleeSaves &= ~frame().calleeSavesMask();

    RegisterMask scratch = 0;
    for (GPRReg reg : m_gpScratch) {
        if (reg != InvalidGPRReg)
            scratch |= maskOf(Reg(reg));
    }
    for (FPRReg reg : m_fpScratch) {
        if (reg != InvalidFPRReg)
            scratch |= maskOf(Reg(reg));
    }
    return registerSetOf((m_usedRegisters | pinnedRegisters() | unsavedCalleeSaves) & ~scratch);
}

bool PatchParameters::fallsThroughToSuccessor(unsigned successorIndex) const
{
    RELEASE_ASSERT(m_value->endsBlock());
    const Block* block = m_value->owner();
    return m_encoder.fallsThroughTo(block, block->successor(successorIndex).block());
}

const Frame& PatchParameters::frame() const
{
    return m_encoder.frame();
}

Procedure& PatchParameters::proc() const
{
    return m_encoder.procedure();
}

void ValueRep::emitRestore(CCallHelpers& jit, Reg destination) const
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    switch (m_kind) {
    case Register:
        restoreValueRepFromRegister(jit, m_reg, destination);
        return;
    case Stack:
        restoreValueRepFromMemory(jit, MacroAssembler::Address(MacroAssembler::framePointerRegister, offsetFromFP()), destination);
        return;
    case StackArgument:
        restoreValueRepFromMemory(jit, MacroAssembler::Address(MacroAssembler::stackPointerRegister, offsetFromSP()), destination);
        return;
    case Constant:
        restoreValueRepFromConstant(jit, value(), destination);
        return;
    case WarmAny:
    case ColdAny:
    case LateColdAny:
    case SomeRegister:
    case SomeEarlyRegister:
    case SomeLateRegister:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
