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

// The parameters of a Check wait in the arena for its late path, and nothing destroys them.
static_assert(std::is_trivially_destructible_v<PatchParameters>);

static constexpr uint32_t emptySharedLatePathSlot = UINT32_MAX;
static constexpr size_t smallestNumberOfSharedLatePathSlots = 8;

static ALWAYS_INLINE bool patchRepAnswersConstraint(const ValueRep& answer, const ValueRep& constraint)
{
    switch (constraint.kind()) {
    case ValueRep::WarmAny:
    case ValueRep::ColdAny:
    case ValueRep::LateColdAny:
        return true;
    case ValueRep::SomeRegister:
    case ValueRep::SomeEarlyRegister:
    case ValueRep::SomeLateRegister:
        return answer.isReg();
    case ValueRep::Register:
    case ValueRep::StackArgument:
        return answer == constraint;
    case ValueRep::Stack:
    case ValueRep::Constant:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE bool patchRegisterAnswersConstraint(const ValueRep& answer, const ValueRep& constraint)
{
    static_assert(ValueRep::WarmAny < ValueRep::Register && ValueRep::ColdAny < ValueRep::Register && ValueRep::LateColdAny < ValueRep::Register);
    static_assert(ValueRep::SomeRegister < ValueRep::Register && ValueRep::SomeEarlyRegister < ValueRep::Register && ValueRep::SomeLateRegister < ValueRep::Register);
    static_assert(ValueRep::StackArgument > ValueRep::Register && ValueRep::Stack > ValueRep::Register && ValueRep::Constant > ValueRep::Register);
    ASSERT(answer.isReg());
    return constraint.kind() < ValueRep::Register || answer == constraint;
}

// The address of a generator differs from run to run, and so does the slot. Only the length of a search follows from it: no two late paths in the list are alike, so at most one matches.
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

    // Half of the slots stay empty, so every search ends.
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

// Nothing holds the words here, so no operand lies in one. Like a ClearSpill, a store writes no register but the one through which ARM64 reaches a far slot.
static void clearFrameWordsInFrontOfSuspension(Encoder& encoder, const PatchOperation* suspension)
{
    const BitSpan& words = suspension->frameWordsClearedInFrontOfSuspension();
    if (!words.size())
        return;

    RELEASE_ASSERT(encoder.m_frameIsSetUp);
    AllowMacroScratchRegisterUsage allowScratch(encoder.m_jit);
    auto addressOfWord = [&](size_t word) {
        return encoder.m_frame.addressInFrame(-static_cast<intptr_t>(word * sizeof(uint64_t)));
    };
    for (size_t word = 0; word < words.size(); ++word) {
        if (!words.get(word))
            continue;
#if CPU(ARM64)
        // The next word lies below this one.
        if (words.get(word + 1)) {
            encoder.m_jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, addressOfWord(++word));
            continue;
        }
#endif
        encoder.m_jit.store64(CCallHelpers::TrustedImm32(0), addressOfWord(word));
    }
}

void Encoder::encodePatch(const Step& step, PatchOperation* patch)
{
    ASSERT(patch->opcode() == Patch);
    ASSERT(!patch->onlyKeepsChildrenAlive);
    RELEASE_ASSERT(patch->generator());
    RELEASE_ASSERT(m_frameIsSetUp || patch->runsWithoutFrame());

    if (patch->isSuspension())
        clearFrameWordsInFrontOfSuspension(*this, patch);

    PatchParameters parameters(*this, patch);
    fillParameters(parameters, step, patch);
    patch->generator()->run(m_jit, parameters);
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
#if CPU(X86_64)
    if (std::optional<MacroAssembler::RelationalCondition> condition = encoder.tryEmitFlagsOfPredicate(step))
        return oneJumpOfFailingCheck(encoder, encoder.m_jit.branchOnFlags(*condition));
#endif
    LeafCursor leaves(step);
    Test test = encoder.resolveTest(check, leaves);
#if CPU(X86_64)
    if (test.kind != Test::CompareNumbers)
        return oneJumpOfFailingCheck(encoder, encoder.jumpOnIntegers(test, true));
#endif
    MacroAssembler::JumpList failures = encoder.branchOn(test, true);
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
        if (!predicate->asInt())
            return;
        jumps = oneJumpOfFailingCheck(*this, m_jit.jump());
    } else
        jumps = jumpsOfFailingCheck(*this, step, check);

    PatchParameters* parameters = new (NotNull, m_arena.allocate(sizeof(PatchParameters), alignof(PatchParameters))) PatchParameters(*this, check);
    fillParameters(*parameters, step, check);

    // Late paths run in the order of adding, so the entry has its place by the time this one runs. It emits nothing.
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

void Encoder::fillParameters(PatchParameters& parameters, const Step& step, PatchOperation* patch)
{
    RELEASE_ASSERT(step.patchAssignment != UINT32_MAX);
    const PatchAssignment& assignment = m_allocation.patchAssignment(step.patchAssignment);
    std::span<const Location> locations = assignment.locations;
    std::span<Operation* const> operands = patch->operands();

    unsigned numberOfResults = m_proc.resultCount(patch->type());
    unsigned firstOperand = patch->opcode() == Check ? 1 : 0;
    unsigned numberOfOperands = static_cast<unsigned>(operands.size()) - firstOperand;
    RELEASE_ASSERT(locations.size() == numberOfResults + numberOfOperands);

    if (size_t numberOfReps = locations.size()) {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        ValueRep* reps = static_cast<ValueRep*>(m_arena.allocate(numberOfReps * sizeof(ValueRep), alignof(ValueRep)));
        const Location* location = locations.data();
        ValueRep* rep = reps;
        static constexpr ValueRep someRegister { ValueRep::SomeRegister };
        for (unsigned index = 0; index < numberOfResults; ++index, ++location, ++rep) {
            const ValueRep& constraint = index < patch->resultConstraints.size() ? patch->resultConstraints[index] : someRegister;
            if (location->isRegister()) {
                new (NotNull, rep) ValueRep(location->reg());
                RELEASE_ASSERT(patchRegisterAnswersConstraint(*rep, constraint));
                continue;
            }
            new (NotNull, rep) ValueRep(repOf(*location, patch));
            RELEASE_ASSERT(patchRepAnswersConstraint(*rep, constraint));
        }
        Operation* const* operand = operands.data() + firstOperand;
        for (unsigned child = firstOperand; child < firstOperand + numberOfOperands; ++child, ++location, ++rep, ++operand) {
            const ValueRep& constraint = patch->constraintOf(child);
            if (location->isRegister()) {
                new (NotNull, rep) ValueRep(location->reg());
                RELEASE_ASSERT(patchRegisterAnswersConstraint(*rep, constraint));
                continue;
            }
            new (NotNull, rep) ValueRep(repOf(*location, *operand));
            RELEASE_ASSERT(patchRepAnswersConstraint(*rep, constraint));
        }
        parameters.m_reps = std::span<const ValueRep>(reps, numberOfReps);
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    }

    std::span<GPRReg> gpScratch(parameters.m_gpScratch);
    unsigned numberOfGPScratch = 0;
    forEachRegister(assignment.gpScratch, [&](Reg reg) {
        RELEASE_ASSERT(numberOfGPScratch < gpScratch.size());
        gpScratch[numberOfGPScratch++] = reg.gpr();
    });
    RELEASE_ASSERT(numberOfGPScratch == patch->numGPScratchRegisters);

    std::span<FPRReg> fpScratch(parameters.m_fpScratch);
    unsigned numberOfFPScratch = 0;
    forEachRegister(assignment.fpScratch, [&](Reg reg) {
        RELEASE_ASSERT(numberOfFPScratch < fpScratch.size());
        fpScratch[numberOfFPScratch++] = reg.fpr();
    });
    RELEASE_ASSERT(numberOfFPScratch == patch->numFPScratchRegisters);

    parameters.m_usedRegisters = assignment.usedRegisters;
    parameters.m_frameIsSetUp = m_frameIsSetUp;

    if (!patch->endsBlock())
        return;
    const Block* block = patch->owner();
    ASSERT(block == m_block);
    std::span<MacroAssembler::Label*> labels = m_arena.createSpan<MacroAssembler::Label*>(block->numSuccessors());
    for (unsigned index = 0; index < block->numSuccessors(); ++index)
        labels[index] = labelOf(block->successor(index).block());
    parameters.m_successorLabels = labels;
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
    // A register that the platform's calling convention has the callee save holds a caller's value, even where the allocator never hands it out.
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
    // On ARM64 a distant word of the frame is reached through a scratch register. On both targets a constant goes to an FPR through one.
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
