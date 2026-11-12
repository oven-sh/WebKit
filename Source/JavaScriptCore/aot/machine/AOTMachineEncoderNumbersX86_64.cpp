/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT) && CPU(X86_64)

#include "AllowMacroScratchRegisterUsage.h"
#include <algorithm>
#include <array>
#include <bit>
#include <utility>

namespace JSC { namespace AOT { namespace Machine {

namespace {

struct X86NumberSource {
    bool isRegister() const { return reg.isSet(); }
    bool isBaseOffset() const { return !isRegister() && memory.kind == AddressForm::BaseOffset; }

    Reg reg;
    AddressForm memory;
    const Operation* constant { nullptr };
};

} // anonymous namespace

static X86NumberSource takeX86NumberSource(Encoder& encoder, const Operation* user, unsigned childIndex, LeafCursor& cursor)
{
    X86NumberSource source;
    const Operation* child = user->child(childIndex);
    if (encoder.m_liveness.isFused(child) && child->opcode() != Trunc && child->opcode() != ZExt32) {
        RELEASE_ASSERT(child->opcode() == Load);
        source.memory = encoder.resolveAddress(child, cursor);
        return source;
    }

    Location location = cursor.take();
    switch (location.kind()) {
    case Location::Register:
        source.reg = location.reg();
        return source;
    case Location::Spill:
    case Location::Outgoing:
    case Location::InstanceWord: {
        MacroAssembler::Address address = encoder.addressOf(location);
        source.memory.base = address.base;
        source.memory.offset = address.offset;
        return source;
    }
    case Location::Immediate:
        RELEASE_ASSERT(readsConstantInMemory(*user, childIndex, *child) && child->hasDouble());
        source.constant = child;
        return source;
    case Location::None:
    case Location::SlotBytes:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MacroAssembler::BaseIndex indexedAddressOfX86NumberSource(const X86NumberSource& source)
{
    const AddressForm& form = source.memory;
    ASSERT(!source.isRegister() && form.kind == AddressForm::BaseIndex && form.extension == IndexExtension::None);
    return MacroAssembler::BaseIndex(form.base, form.index, static_cast<MacroAssembler::Scale>(form.shift), form.offset);
}

static void loadX86Number(Encoder& encoder, const X86NumberSource& source, Width width, FPRReg destination)
{
    ASSERT(!source.isRegister());
    CCallHelpers& jit = encoder.m_jit;
    bool isDouble = width == Width64;

    switch (source.memory.kind) {
    case AddressForm::BaseOffset:
        if (isDouble)
            jit.loadDouble(source.memory.address(), destination);
        else
            jit.loadFloat(source.memory.address(), destination);
        return;
    case AddressForm::BaseIndex:
        if (isDouble)
            jit.loadDouble(indexedAddressOfX86NumberSource(source), destination);
        else
            jit.loadFloat(indexedAddressOfX86NumberSource(source), destination);
        return;
    case AddressForm::InstanceSymbol: {
        if (isDouble) {
            encoder.m_relocations.loadInstanceDouble(source.memory.symbol, instanceGPR, destination);
            return;
        }
        AllowMacroScratchRegisterUsage allowScratch(jit);
        encoder.m_relocations.loadInstanceWord(source.memory.symbol, InstanceWordWidth::Word32, instanceGPR, scratchGPR);
        jit.move32ToFloat(scratchGPR, destination);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void loadX86IntegerForNumber(Encoder& encoder, const X86NumberSource& source, Width width, GPRReg destination)
{
    ASSERT(!source.isRegister());
    CCallHelpers& jit = encoder.m_jit;
    bool is64Bits = width == Width64;

    switch (source.memory.kind) {
    case AddressForm::BaseOffset:
        if (is64Bits)
            jit.load64(source.memory.address(), destination);
        else
            jit.load32(source.memory.address(), destination);
        return;
    case AddressForm::BaseIndex:
        if (is64Bits)
            jit.load64(indexedAddressOfX86NumberSource(source), destination);
        else
            jit.load32(indexedAddressOfX86NumberSource(source), destination);
        return;
    case AddressForm::InstanceSymbol:
        encoder.m_relocations.loadInstanceWord(source.memory.symbol, is64Bits ? InstanceWordWidth::Word64 : InstanceWordWidth::Word32, instanceGPR, destination);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FPRReg x86NumberInRegister(Encoder& encoder, const X86NumberSource& source, Width width, FPRReg registerIfInMemory)
{
    if (source.isRegister())
        return source.reg.fpr();
    loadX86Number(encoder, source, width, registerIfInMemory);
    return registerIfInMemory;
}

bool Encoder::blockHasWrittenNumberRegisterInFrontOf(const Step& step, FPRReg reg)
{
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const BlockSteps& steps = stepsOf(m_block);
    const Step* first = m_steps + steps.begin;
    if (&step < first || &step >= m_steps + steps.end)
        return false;
    if (m_blockWhoseWrittenNumberRegistersAreNoted != m_block) {
        m_blockWhoseWrittenNumberRegistersAreNoted = m_block;
        m_firstStepNotLookedAtForNumberRegisters = first;
        m_numberRegistersWrittenInBlock = 0;
    }
    for (; m_firstStepNotLookedAtForNumberRegisters < &step; ++m_firstStepNotLookedAtForNumberRegisters) {
        Location written = m_firstStepNotLookedAtForNumberRegisters->destination;
        if (written.isRegister() && written.reg().isFPR())
            m_numberRegistersWrittenInBlock |= maskOf(written.reg());
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    return m_numberRegistersWrittenInBlock & maskOf(Reg(reg));
}

void Encoder::zeroDestinationOf(const Step& step, FPRReg destination)
{
    m_jit.moveZeroToDouble(destination);
    m_stepsWithZeroedDestination[m_numberOfZeroedDestinations++ % m_stepsWithZeroedDestination.size()] = &step;
}

bool Encoder::writesNumberRegister(const Step& step, FPRReg reg) const
{
    if (step.destination.isRegister() && step.destination.reg() == Reg(reg))
        return true;
    if (step.kind == StepKind::Swap && step.sources[0].isRegister() && step.sources[0].reg() == Reg(reg))
        return true;
    if (step.patchAssignment == UINT32_MAX)
        return false;
    const PatchAssignment& assignment = m_allocation.patchAssignment(step.patchAssignment);
    return (assignment.fpScratch & maskOf(Reg(reg))) || std::ranges::any_of(assignment.locations, [&](const Location& location) {
        return location.isRegister() && location.reg() == Reg(reg);
    });
}

bool Encoder::wasNumberRegisterWrittenRecently(const Step& step, FPRReg reg) const
{
    static constexpr unsigned maxRecentWriterSteps = 16;
    static constexpr unsigned mostBlocks = 8;
    struct EndOfBlock {
        const Block* block;
        unsigned stepsLeft;
    };
    Vector<EndOfBlock, mostBlocks> ends;
    auto isWrittenBetween = [&](const Step* begin, const Step* end, unsigned& stepsLeft) {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        for (; end > begin && stepsLeft; --stepsLeft) {
            if (writesNumberRegister(*--end, reg))
                return true;
        }
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        return false;
    };
    auto appendPredecessors = [&](const Block* block, unsigned stepsLeft) {
        for (const Block* predecessor : placeOf(block).predecessors) {
            if (ends.size() == mostBlocks)
                return false;
            ends.append(EndOfBlock { predecessor, stepsLeft });
        }
        return true;
    };

    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const BlockSteps& steps = stepsOf(m_block);
    if (&step < m_steps + steps.begin || &step >= m_steps + steps.end)
        return true;
    unsigned stepsLeft = maxRecentWriterSteps;
    if (isWrittenBetween(m_steps + steps.begin, &step, stepsLeft) || (stepsLeft && !appendPredecessors(m_block, stepsLeft)))
        return true;
    for (unsigned index = 0; index < ends.size(); ++index) {
        auto [block, remainingSteps] = ends[index];
        const BlockSteps& stepsThere = stepsOf(block);
        if (stepsThere.terminator != UINT32_MAX && writesNumberRegister(m_steps[stepsThere.terminator], reg))
            return true;
        if (isWrittenBetween(m_steps + stepsThere.beginOfEdgeMoves, m_steps + stepsThere.endOfEdgeMoves, remainingSteps))
            return true;
        if (isWrittenBetween(m_steps + stepsThere.begin, m_steps + stepsThere.end, remainingSteps))
            return true;
        if (remainingSteps && !appendPredecessors(block, remainingSteps))
            return true;
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    return false;
}

static bool keepsUpperX86Destination(Opcode opcode)
{
    return opcode == FloatToDouble || opcode == DoubleToFloat || opcode == Sqrt || opcode == Floor || opcode == Ceil || opcode == FTrunc;
}

bool Encoder::startsNumberChain(const Step& step) const
{
    if (std::ranges::contains(m_stepsWithZeroedDestination, &step))
        return true;
    auto holdsNumber = [](const Location& source) { return source.isRegister() && source.reg().isFPR(); };
    auto holdsWord = [](const Location& source) { return source.isRegister() && source.reg().isGPR(); };
    if (std::ranges::any_of(step.sources, holdsNumber))
        return false;
    if (step.kind != StepKind::Execute || !keepsUpperX86Destination(step.operation->opcode()))
        return true;
    return std::ranges::count_if(step.sources, holdsWord) > 1;
}

bool Encoder::isNumberRegisterChainedAcrossIterations(const Step& step, FPRReg reg) const
{
    static constexpr unsigned maxSteps = 256;
    const uint32_t loop = placeOf(m_block).loop;
    if (loop == noLoop)
        return false;
    unsigned numberOfSteps = 0;
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    auto isChainBetween = [&](const Step* begin, const Step* end) {
        while (end > begin) {
            --end;
            if (++numberOfSteps > maxSteps)
                return false;
            if (writesNumberRegister(*end, reg) && startsNumberChain(*end))
                return false;
        }
        return true;
    };
    Vector<const Block*, 16> blocks;
    auto appendPredecessors = [&](const Block* block) {
        for (const Block* predecessor : placeOf(block).predecessors) {
            const BlockPlace& place = placeOf(predecessor);
            if (place.loop == loop && !place.isRare() && place.layoutPart != LayoutPart::Rare && !blocks.contains(predecessor))
                blocks.append(predecessor);
        }
    };
    const BlockSteps& ownSteps = stepsOf(m_block);
    if (&step < m_steps + ownSteps.begin || &step >= m_steps + ownSteps.end || !isChainBetween(m_steps + ownSteps.begin, &step))
        return false;
    appendPredecessors(m_block);
    bool isClosed = false;
    for (unsigned index = 0; index < blocks.size(); ++index) {
        const Block* block = blocks[index];
        const BlockSteps& steps = stepsOf(block);
        if (steps.terminator != UINT32_MAX && writesNumberRegister(m_steps[steps.terminator], reg))
            return false;
        if (!isChainBetween(m_steps + steps.beginOfEdgeMoves, m_steps + steps.endOfEdgeMoves))
            return false;
        if (block == m_block) {
            isClosed = true;
            if (!isChainBetween(&step + 1, m_steps + steps.end))
                return false;
            continue;
        }
        if (!isChainBetween(m_steps + steps.begin, m_steps + steps.end))
            return false;
        appendPredecessors(block);
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    return isClosed;
}

enum class WaitForDestination : bool { IsShort, IsLong };

template<typename Functor>
static void encodeX86NumberUnary(Encoder& encoder, const Step& step, WaitForDestination wait, const X86NumberSource& source, Width widthOfSource, FPRReg destination, const Functor& emit)
{
    bool keepsRestOfDestination = source.isRegister() ? source.reg.fpr() != destination : source.isBaseOffset();
    bool waitsForEarlierBlock = wait == WaitForDestination::IsLong && source.isRegister() && !encoder.blockHasWrittenNumberRegisterInFrontOf(step, destination);
    if (keepsRestOfDestination && (waitsForEarlierBlock || encoder.isNumberRegisterChainedAcrossIterations(step, destination)))
        encoder.zeroDestinationOf(step, destination);
    if (source.isRegister()) {
        emit(source.reg.fpr());
        return;
    }
    if (source.isBaseOffset()) {
        emit(source.memory.address());
        return;
    }
    loadX86Number(encoder, source, widthOfSource, destination);
    emit(destination);
}

static void encodeX86ArithmeticWithConstant(Encoder& encoder, Opcode opcode, FPRReg left, const Operation* constant, FPRReg destination)
{
    encoder.m_jit.moveDouble(left, destination);
    auto bytes = std::bit_cast<std::array<uint8_t, sizeof(uint64_t)>>(constant->constantBits());
    CodeSymbol symbol { SymbolSpace::Constant, encoder.m_proc.addConstantData(std::span<const uint8_t>(bytes), sizeof(uint64_t), ConstantKind::Number) };
    if (opcode == Add)
        encoder.m_relocations.addDouble(symbol, destination);
    else if (opcode == Sub)
        encoder.m_relocations.subDouble(symbol, destination);
    else if (opcode == Mul)
        encoder.m_relocations.mulDouble(symbol, destination);
    else {
        RELEASE_ASSERT(opcode == Div);
        encoder.m_relocations.divDouble(symbol, destination);
    }
}

static void encodeX86NumberArithmetic(Encoder& encoder, Opcode opcode, Width width, X86NumberSource left, X86NumberSource right, FPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    bool isDouble = width == Width64;

    if (right.constant) {
        RELEASE_ASSERT(isDouble && left.isRegister());
        encodeX86ArithmeticWithConstant(encoder, opcode, left.reg.fpr(), right.constant, destination);
        return;
    }

    if (!left.isRegister() && isCommutative(opcode))
        std::swap(left, right);

    if (!left.isRegister()) {
        if (right.isRegister() && right.reg.fpr() == destination) {
            loadX86Number(encoder, left, width, scratchFPR);
            if (opcode == Sub) {
                if (isDouble)
                    jit.subDouble(destination, scratchFPR);
                else
                    jit.subFloat(destination, scratchFPR);
            } else {
                ASSERT(opcode == Div);
                if (isDouble)
                    jit.divDouble(destination, scratchFPR);
                else
                    jit.divFloat(destination, scratchFPR);
            }
            jit.moveDouble(scratchFPR, destination);
            return;
        }
        loadX86Number(encoder, left, width, destination);
        left.reg = destination;
    }
    FPRReg leftFPR = left.reg.fpr();

    if (opcode == Div) {
        if (right.isBaseOffset()) {
            jit.moveDouble(leftFPR, destination);
            if (isDouble)
                jit.divDouble(right.memory.address(), destination);
            else
                jit.divFloat(right.memory.address(), destination);
            return;
        }
        FPRReg rightFPR = x86NumberInRegister(encoder, right, width, scratchFPR);
        if (isDouble)
            jit.divDouble(leftFPR, rightFPR, destination);
        else
            jit.divFloat(leftFPR, rightFPR, destination);
        return;
    }

    auto emit = [&](auto rightOperand) {
        if (opcode == Add) {
            if (isDouble)
                jit.addDouble(rightOperand, leftFPR, destination);
            else
                jit.addFloat(rightOperand, leftFPR, destination);
        } else if (opcode == Sub) {
            if (isDouble)
                jit.subDouble(leftFPR, rightOperand, destination);
            else
                jit.subFloat(leftFPR, rightOperand, destination);
        } else {
            ASSERT(opcode == Mul);
            if (isDouble)
                jit.mulDouble(rightOperand, leftFPR, destination);
            else
                jit.mulFloat(rightOperand, leftFPR, destination);
        }
    };

    if (right.isRegister()) {
        emit(right.reg.fpr());
        return;
    }
    switch (right.memory.kind) {
    case AddressForm::BaseOffset:
        emit(right.memory.address());
        return;
    case AddressForm::BaseIndex:
        emit(indexedAddressOfX86NumberSource(right));
        return;
    case AddressForm::InstanceSymbol:
        loadX86Number(encoder, right, width, scratchFPR);
        emit(scratchFPR);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void encodeX86NumberNegOrAbs(Encoder& encoder, Opcode opcode, Width width, const X86NumberSource& source, FPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    AllowMacroScratchRegisterUsage allowScratch(jit);
    bool isDouble = width == Width64;
    bool negates = opcode == Neg;
    ASSERT(negates || opcode == Abs);

    auto moveMaskTo = [&](FPRReg mask) {
        if (isDouble) {
            uint64_t signBit = std::bit_cast<uint64_t>(-0.0);
            encoder.materializeBitsInFPR(negates ? signBit : ~signBit, Width64, mask);
        } else {
            int32_t signBit = std::bit_cast<int32_t>(-0.0f);
            jit.move32ToFloat(CCallHelpers::TrustedImm32(negates ? signBit : ~signBit), mask);
        }
    };
    auto combineIntoDestination = [&](FPRReg other) {
        if (negates) {
            if (isDouble)
                jit.xorDouble(other, destination);
            else
                jit.xorFloat(other, destination);
        } else {
            if (isDouble)
                jit.andDouble(other, destination);
            else
                jit.andFloat(other, destination);
        }
    };

    if (source.isRegister() && source.reg.fpr() != destination) {
        moveMaskTo(destination);
        combineIntoDestination(source.reg.fpr());
        return;
    }
    if (!source.isRegister())
        loadX86Number(encoder, source, width, destination);
    moveMaskTo(scratchFPR);
    combineIntoDestination(scratchFPR);
}

static void roundX86NumberThroughInteger(Encoder& encoder, Opcode opcode, Width width, FPRReg value)
{
    CCallHelpers& jit = encoder.m_jit;
    AllowMacroScratchRegisterUsage allowScratch(jit);
    bool isDouble = width == Width64;
    int32_t numberOfFractionBits = isDouble ? 52 : 23;
    int32_t exponentMask = isDouble ? 0x7ff : 0xff;
    int32_t exponentBias = isDouble ? 1023 : 127;

    auto moveBitsToScratch = [&] {
        if (isDouble)
            jit.moveDoubleTo64(value, scratchGPR);
        else
            jit.moveFloatTo32(value, scratchGPR);
    };
    auto convertScratchToNumber = [&] {
        if (isDouble)
            jit.convertInt64ToDouble(scratchGPR, scratchFPR);
        else
            jit.convertInt32ToFloat(scratchGPR, scratchFPR);
    };

    moveBitsToScratch();
    if (isDouble)
        jit.urshift64(CCallHelpers::TrustedImm32(numberOfFractionBits), scratchGPR);
    else
        jit.urshift32(CCallHelpers::TrustedImm32(numberOfFractionBits), scratchGPR);
    jit.and32(CCallHelpers::TrustedImm32(exponentMask), scratchGPR);
    MacroAssembler::Jump needsNoRounding = jit.branch32(MacroAssembler::AboveOrEqual, scratchGPR, CCallHelpers::TrustedImm32(exponentBias + numberOfFractionBits));

    if (isDouble)
        jit.truncateDoubleToInt64(value, scratchGPR);
    else
        jit.truncateFloatToInt32(value, scratchGPR);
    jit.moveZeroToDouble(scratchFPR);
    convertScratchToNumber();

    if (opcode != FTrunc) {
        ASSERT(opcode == Floor || opcode == Ceil);
        bool roundsDown = opcode == Floor;
        MacroAssembler::DoubleCondition isOnCorrectSide = roundsDown ? MacroAssembler::DoubleLessThanOrEqualAndOrdered : MacroAssembler::DoubleGreaterThanOrEqualAndOrdered;
        MacroAssembler::Jump needsNoAdjustment = isDouble ? jit.branchDouble(isOnCorrectSide, scratchFPR, value) : jit.branchFloat(isOnCorrectSide, scratchFPR, value);
        CCallHelpers::TrustedImm32 adjustment(roundsDown ? -1 : 1);
        if (isDouble)
            jit.add64(adjustment, scratchGPR);
        else
            jit.add32(adjustment, scratchGPR);
        convertScratchToNumber();
        needsNoAdjustment.link(&jit);
    }

    moveBitsToScratch();
    if (isDouble) {
        jit.urshift64(CCallHelpers::TrustedImm32(63), scratchGPR);
        jit.lshift64(CCallHelpers::TrustedImm32(63), scratchGPR);
        jit.move64ToDouble(scratchGPR, value);
        jit.orDouble(scratchFPR, value);
    } else {
        jit.and32(CCallHelpers::TrustedImm32(std::bit_cast<int32_t>(-0.0f)), scratchGPR);
        jit.move32ToFloat(scratchGPR, value);
        jit.orFloat(scratchFPR, value);
    }
    needsNoRounding.link(&jit);
}

static void encodeX86NumberRounding(Encoder& encoder, const Step& step, Opcode opcode, Width width, const X86NumberSource& source, FPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    bool isDouble = width == Width64;

    if (!jit.supportsFloatingPointRounding()) {
        jit.moveDouble(x86NumberInRegister(encoder, source, width, destination), destination);
        roundX86NumberThroughInteger(encoder, opcode, width, destination);
        return;
    }

    encodeX86NumberUnary(encoder, step, WaitForDestination::IsShort, source, width, destination, [&](auto operand) {
        if (opcode == Ceil) {
            if (isDouble)
                jit.ceilDouble(operand, destination);
            else
                jit.ceilFloat(operand, destination);
        } else if (opcode == Floor) {
            if (isDouble)
                jit.floorDouble(operand, destination);
            else
                jit.floorFloat(operand, destination);
        } else {
            ASSERT(opcode == FTrunc);
            if (isDouble)
                jit.roundTowardZeroDouble(operand, destination);
            else
                jit.roundTowardZeroFloat(operand, destination);
        }
    });
}

static void encodeX86NumberMinOrMax(Encoder& encoder, Opcode opcode, Width width, X86NumberSource left, X86NumberSource right, FPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    bool isDouble = width == Width64;
    bool isMin = opcode == FMin;
    ASSERT(isMin || opcode == FMax);

    if (!left.isRegister())
        std::swap(left, right);
    FPRReg leftFPR = x86NumberInRegister(encoder, left, width, destination);
    FPRReg rightFPR = x86NumberInRegister(encoder, right, width, scratchFPR);

    auto branchIf = [&](MacroAssembler::DoubleCondition condition) {
        return isDouble ? jit.branchDouble(condition, leftFPR, rightFPR) : jit.branchFloat(condition, leftFPR, rightFPR);
    };

    MacroAssembler::Jump orderedAndDifferent = branchIf(MacroAssembler::DoubleNotEqualAndOrdered);
    MacroAssembler::Jump unordered = branchIf(MacroAssembler::DoubleLessThanOrUnordered);

    if (isMin) {
        if (isDouble)
            jit.orDouble(leftFPR, rightFPR, destination);
        else
            jit.orFloat(leftFPR, rightFPR, destination);
    } else {
        if (isDouble)
            jit.andDouble(leftFPR, rightFPR, destination);
        else
            jit.andFloat(leftFPR, rightFPR, destination);
    }
    MacroAssembler::Jump doneWithEqual = jit.jump();

    unordered.link(&jit);
    if (isDouble)
        jit.addDouble(leftFPR, rightFPR, destination);
    else
        jit.addFloat(leftFPR, rightFPR, destination);
    MacroAssembler::Jump doneWithUnordered = jit.jump();

    orderedAndDifferent.link(&jit);
    if (isMin) {
        if (isDouble)
            jit.minOrSecondDouble(leftFPR, rightFPR, destination);
        else
            jit.minOrSecondFloat(leftFPR, rightFPR, destination);
    } else {
        if (isDouble)
            jit.maxOrSecondDouble(leftFPR, rightFPR, destination);
        else
            jit.maxOrSecondFloat(leftFPR, rightFPR, destination);
    }
    doneWithEqual.link(&jit);
    doneWithUnordered.link(&jit);
}

static bool canBeFirstX86MinMaxOperand(Opcode opcode, const Operation* number)
{
    if (number->opcode() == IToD)
        return opcode == FMin;
    if (!number->hasDouble() || std::isnan(number->asDouble()))
        return false;
    return number->asDouble() || std::signbit(number->asDouble()) == (opcode == FMax);
}

static void encodeX86OrderedMinMax(Encoder& encoder, Opcode opcode, const X86NumberSource& first, const X86NumberSource& second, FPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    FPRReg registerOfResult = first.isRegister() || !second.isRegister() ? destination : scratchFPR;
    FPRReg firstFPR = x86NumberInRegister(encoder, first, Width64, registerOfResult);
    FPRReg secondFPR = x86NumberInRegister(encoder, second, Width64, scratchFPR);
    if (opcode == FMin)
        jit.minOrSecondDouble(firstFPR, secondFPR, registerOfResult);
    else
        jit.maxOrSecondDouble(firstFPR, secondFPR, registerOfResult);
    jit.moveDouble(registerOfResult, destination);
}

static void encodeX86NumberFromInteger(Encoder& encoder, const Step& step, Width widthOfInteger, const X86NumberSource& source, FPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    auto convert = [&](auto operand) {
        if (widthOfInteger == Width64)
            jit.convertInt64ToDouble(operand, destination);
        else
            jit.convertInt32ToDouble(operand, destination);
    };

    if (encoder.wasNumberRegisterWrittenRecently(step, destination) || encoder.isNumberRegisterChainedAcrossIterations(step, destination))
        jit.moveZeroToDouble(destination);
    if (source.isRegister()) {
        convert(source.reg.gpr());
        return;
    }
    if (source.isBaseOffset()) {
        convert(source.memory.address());
        return;
    }
    AllowMacroScratchRegisterUsage allowScratch(jit);
    loadX86IntegerForNumber(encoder, source, widthOfInteger, scratchGPR);
    convert(scratchGPR);
}

static void encodeX86NumberBitwiseCast(Encoder& encoder, Width width, const X86NumberSource& source, Reg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    bool is64Bits = width == Width64;

    if (destination.isFPR()) {
        if (!source.isRegister())
            loadX86Number(encoder, source, width, destination.fpr());
        else if (source.reg.isFPR())
            jit.moveDouble(source.reg.fpr(), destination.fpr());
        else if (is64Bits)
            jit.move64ToDouble(source.reg.gpr(), destination.fpr());
        else
            jit.move32ToFloat(source.reg.gpr(), destination.fpr());
        return;
    }

    if (!source.isRegister())
        loadX86IntegerForNumber(encoder, source, width, destination.gpr());
    else if (source.reg.isFPR()) {
        if (is64Bits)
            jit.moveDoubleTo64(source.reg.fpr(), destination.gpr());
        else
            jit.moveFloatTo32(source.reg.fpr(), destination.gpr());
    } else if (is64Bits)
        jit.move(source.reg.gpr(), destination.gpr());
    else if (source.reg != destination)
        jit.zeroExtend32ToWord(source.reg.gpr(), destination.gpr());
}

void Encoder::encodeNumber(const Step& step)
{
    const Operation* operation = step.operation;
    Opcode opcode = operation->opcode();
    Width width = widthOf(operation->type());
    LeafCursor cursor(step);
    auto takeSource = [&](unsigned childIndex) {
        return takeX86NumberSource(*this, operation, childIndex, cursor);
    };

    switch (opcode) {
    case Add:
    case Sub:
    case Mul:
    case Div: {
        ASSERT(operation->type().isFloat());
        X86NumberSource left = takeSource(0);
        X86NumberSource right = takeSource(1);
        encodeX86NumberArithmetic(*this, opcode, width, left, right, step.destination.fpr());
        return;
    }
    case FMin:
    case FMax: {
        X86NumberSource left = takeSource(0);
        X86NumberSource right = takeSource(1);
        for (unsigned known = 0; width == Width64 && known < 2; ++known) {
            if (!canBeFirstX86MinMaxOperand(opcode, operation->child(known)))
                continue;
            encodeX86OrderedMinMax(*this, opcode, known ? right : left, known ? left : right, step.destination.fpr());
            return;
        }
        encodeX86NumberMinOrMax(*this, opcode, width, left, right, step.destination.fpr());
        return;
    }
    case Neg:
    case Abs:
        ASSERT(operation->type().isFloat());
        encodeX86NumberNegOrAbs(*this, opcode, width, takeSource(0), step.destination.fpr());
        return;
    case Ceil:
    case Floor:
    case FTrunc:
        encodeX86NumberRounding(*this, step, opcode, width, takeSource(0), step.destination.fpr());
        return;
    case Sqrt: {
        FPRReg destination = step.destination.fpr();
        encodeX86NumberUnary(*this, step, WaitForDestination::IsLong, takeSource(0), width, destination, [&](auto operand) {
            if (width == Width64)
                m_jit.sqrtDouble(operand, destination);
            else
                m_jit.sqrtFloat(operand, destination);
        });
        return;
    }
    case FloatToDouble: {
        FPRReg destination = step.destination.fpr();
        encodeX86NumberUnary(*this, step, WaitForDestination::IsLong, takeSource(0), Width32, destination, [&](auto operand) {
            m_jit.convertFloatToDouble(operand, destination);
        });
        return;
    }
    case DoubleToFloat: {
        FPRReg destination = step.destination.fpr();
        encodeX86NumberUnary(*this, step, WaitForDestination::IsLong, takeSource(0), Width64, destination, [&](auto operand) {
            m_jit.convertDoubleToFloat(operand, destination);
        });
        return;
    }
    case IToD:
        ASSERT(operation->type() == Double);
        encodeX86NumberFromInteger(*this, step, widthOf(operation->child(0)->type()), takeSource(0), step.destination.fpr());
        return;
    case BitwiseCast:
        ASSERT(width == widthOf(operation->child(0)->type()));
        encodeX86NumberBitwiseCast(*this, width, takeSource(0), step.destination.reg());
        return;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
