/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "AllowMacroScratchRegisterUsage.h"
#include <array>
#include <bit>

namespace JSC { namespace AOT { namespace Machine {

enum class MemoryOfEncodedMove : bool { IsRead, IsWritten };

static bool encodedMoveIsWide(Width width)
{
    RELEASE_ASSERT(width == Width32 || width == Width64);
    return width == Width64;
}

// The assembler reaches far addresses through the second scratch register, so no step names it.
static void checkRegisterOfEncodedMove(Reg reg)
{
    RELEASE_ASSERT(reg);
    RELEASE_ASSERT(!reg.isGPR() || reg.gpr() != secondScratchGPR);
}

// On ARM64 the assembler remembers the constant that it has put into the scratch register, until it is asked for the register again.
static void willWriteRegisterOfEncodedMove(CCallHelpers& jit, Reg destination)
{
    checkRegisterOfEncodedMove(destination);
    if (destination.isGPR() && destination.gpr() == scratchGPR)
        jit.scratchRegister();
}

static void checkMemoryOfEncodedMove(const Encoder& encoder, Location memory, Width width, MemoryOfEncodedMove access)
{
    switch (memory.kind()) {
    case Location::Spill:
        RELEASE_ASSERT(encoder.m_frameIsSetUp);
        RELEASE_ASSERT(bytesForWidth(width) <= bytesForWidth(encoder.m_allocation.spills()[memory.spillIndex()].width));
        return;
    case Location::Outgoing:
        RELEASE_ASSERT(encoder.m_frameIsSetUp);
        return;
    case Location::InstanceWord:
        RELEASE_ASSERT(access == MemoryOfEncodedMove::IsRead);
        return;
    case Location::None:
    case Location::Register:
    case Location::SlotBytes:
    case Location::Immediate:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The register's bank picks the instruction, not the value's type: on x86-64 an integer goes from memory to memory through the scratch FPR.
static void loadForEncodedMove(CCallHelpers& jit, MacroAssembler::Address source, Width width, Reg destination)
{
    bool isWide = encodedMoveIsWide(width);
    willWriteRegisterOfEncodedMove(jit, destination);
    if (destination.isGPR()) {
        if (isWide)
            jit.load64(source, destination.gpr());
        else
            jit.load32(source, destination.gpr());
        return;
    }
    if (isWide)
        jit.loadDouble(source, destination.fpr());
    else
        jit.loadFloat(source, destination.fpr());
}

static void storeForEncodedMove(CCallHelpers& jit, Reg source, Width width, MacroAssembler::Address destination)
{
    bool isWide = encodedMoveIsWide(width);
    checkRegisterOfEncodedMove(source);
    if (source.isGPR()) {
        if (isWide)
            jit.store64(source.gpr(), destination);
        else
            jit.store32(source.gpr(), destination);
        return;
    }
    if (isWide)
        jit.storeDouble(source.fpr(), destination);
    else
        jit.storeFloat(source.fpr(), destination);
}

// The upper half of a register that holds a reference is unspecified.
static void decodeKeptReference(CCallHelpers& jit, GPRReg reference, GPRReg pointer)
{
    static_assert(cell32Shift <= static_cast<unsigned>(CCallHelpers::TimesEight));
#if CPU(ARM64)
    jit.m_assembler.add<64>(pointer, cellCageBaseGPR, reference, ARM64Assembler::UXTW, cell32Shift);
#else
    jit.zeroExtend32ToWord(reference, pointer);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(cellCageBaseGPR, pointer, static_cast<CCallHelpers::Scale>(cell32Shift)), pointer);
#endif
}

struct BaseOfMaterializedAddress {
    GPRReg base;
    int64_t offset;
};

static BaseOfMaterializedAddress baseOfMaterializedAddress(const Encoder& encoder, const Operation* operation)
{
    uint64_t offset = 0;
    while (operation->opcode() == Add) {
        RELEASE_ASSERT(operation->hasNoPosition() && operation->child(1)->hasInt64());
        offset += static_cast<uint64_t>(operation->child(1)->asInt64());
        operation = operation->child(0);
    }

    if (operation->opcode() == FramePointer) {
        RELEASE_ASSERT(encoder.m_frameIsSetUp);
        return { MacroAssembler::framePointerRegister, static_cast<int64_t>(offset) };
    }
    if (operation->opcode() == SlotBase) {
        RELEASE_ASSERT(encoder.m_frameIsSetUp);
        MacroAssembler::Address slot = encoder.m_frame.addressInFrame(operation->slot()->offsetFromFP());
        return { slot.base, static_cast<int64_t>(offset + static_cast<uint64_t>(static_cast<int64_t>(slot.offset))) };
    }

    RELEASE_ASSERT(operation->opcode() == ArgumentReg && operation->hasNoPosition());
    Reg reg = operation->argumentReg();
    RELEASE_ASSERT(reg.isGPR() && isPinned(reg));
    return { reg.gpr(), static_cast<int64_t>(offset) };
}

static void materializeSumOfBaseAndOffset(CCallHelpers& jit, BaseOfMaterializedAddress address, GPRReg destination)
{
    RELEASE_ASSERT(address.base != destination);
    if (isRepresentableAs<int32_t>(address.offset)) {
        jit.add64(CCallHelpers::TrustedImm32(static_cast<int32_t>(address.offset)), address.base, destination);
        return;
    }
    jit.move(CCallHelpers::TrustedImm64(address.offset), destination);
    jit.add64(address.base, destination);
}

// ARM64 makes a boxed int32 of two or three halfwords, an instruction each. The tag lies in a register, and one addition to it makes every small one.
static bool tryMaterializeBitsBesidePinnedRegister(CCallHelpers& jit, uint64_t bits, GPRReg destination)
{
    if (!isARM64())
        return false;
    for (uint64_t pinnedBits : bitsOfPinnedConstants()) {
        int64_t distance = static_cast<int64_t>(bits - pinnedBits);
        if (!isRepresentableAs<int32_t>(distance) || !isEncodableArithmeticConstant(distance))
            continue;
        std::optional<PinnedConstant> pinned = findPinnedConstant(pinnedBits);
        if (!pinned || pinned->offset)
            continue;
        jit.add64(CCallHelpers::TrustedImm32(static_cast<int32_t>(distance)), pinned->reg, destination);
        return true;
    }
    return false;
}

static void materializeBitsInGPR(CCallHelpers& jit, uint64_t bits, Width width, GPRReg destination)
{
    if (!encodedMoveIsWide(width)) {
        jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(bits))), destination);
        return;
    }

    if (bits) {
        if (std::optional<PinnedConstant> pinned = findPinnedConstant(bits)) {
            if (pinned->offset)
                jit.load64(MacroAssembler::Address(pinned->reg, *pinned->offset), destination);
            else
                jit.move(pinned->reg, destination);
            return;
        }
        if (tryMaterializeBitsBesidePinnedRegister(jit, bits, destination))
            return;
    }
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(bits)), destination);
}

// The alternative is to take the address of a pooled constant and load from it: three instructions on ARM64, twelve bytes on x86-64, plus the eight bytes of the constant.
static bool bitsAreCheapestFromImmediates(uint64_t bits)
{
#if CPU(ARM64)
    unsigned numberOfHalfwords = 0;
    for (unsigned shift = 0; shift < 64; shift += 16)
        numberOfHalfwords += !!(bits >> shift & 0xffff);
    return numberOfHalfwords <= 2;
#else
    return !(bits >> 32);
#endif
}

static void materializeBitsInFPR(Encoder& encoder, uint64_t bits, Width width, FPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    if (!encodedMoveIsWide(width)) {
        jit.move32ToFloat(CCallHelpers::TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(bits))), destination);
        return;
    }

    if (!bits) {
        jit.moveZeroToDouble(destination);
        return;
    }
    if (std::optional<PinnedConstant> pinned = findPinnedConstant(bits)) {
        if (pinned->offset)
            jit.loadDouble(MacroAssembler::Address(pinned->reg, *pinned->offset), destination);
        else
            jit.move64ToDouble(pinned->reg, destination);
        return;
    }
    if (bitsAreCheapestFromImmediates(bits)) {
        jit.move64ToDouble(CCallHelpers::TrustedImm64(static_cast<int64_t>(bits)), destination);
        return;
    }

    // The constants of all functions lie in one pool, which a load relative to the pc does not reach on ARM64.
    std::array<uint8_t, sizeof(uint64_t)> bytes = std::bit_cast<std::array<uint8_t, sizeof(uint64_t)>>(bits);
    uint32_t index = encoder.m_proc.addConstantData(std::span<const uint8_t>(bytes), sizeof(uint64_t), ConstantKind::Number);
    GPRReg address = jit.scratchRegister();
    encoder.m_relocations.moveAddress(CodeSymbol { SymbolSpace::Constant, index }, address, PCRelativeReach::Far);
    jit.loadDouble(MacroAssembler::Address(address), destination);
}

static void materializeInGPR(Encoder& encoder, const Operation* operation, GPRReg destination)
{
    switch (operation->opcode()) {
    case FramePointer:
    case SlotBase:
    case ArgumentReg:
    case Add:
        materializeSumOfBaseAndOffset(encoder.m_jit, baseOfMaterializedAddress(encoder, operation), destination);
        return;

    case AddressOf:
        encoder.m_relocations.moveAddress(operation->symbol(), destination, PCRelativeReach::Far);
        return;

    case NumberOf:
        encoder.m_relocations.moveNumber(operation->symbol(), destination);
        return;

    case InstanceAddressOf:
        encoder.m_relocations.moveInstanceAddress(operation->symbol(), instanceGPR, destination);
        return;

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Encoder::encodeMove(const Step& step)
{
    ASSERT(step.kind == StepKind::Move);
    RELEASE_ASSERT(step.destination.isRegister() && step.sources[0].isRegister());
    Reg source = step.sources[0].reg();
    Reg destination = step.destination.reg();
    checkRegisterOfEncodedMove(source);
    RELEASE_ASSERT(bankOf(source) == bankOf(destination));
    bool isWide = encodedMoveIsWide(step.width);
    if (source == destination)
        return;

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    willWriteRegisterOfEncodedMove(m_jit, destination);
    if (destination.isFPR()) {
        m_jit.moveDouble(source.fpr(), destination.fpr());
        return;
    }
    if (isWide)
        m_jit.move(source.gpr(), destination.gpr());
    else
        m_jit.zeroExtend32ToWord(source.gpr(), destination.gpr());
}

void Encoder::encodeReload(const Step& step)
{
    ASSERT(step.kind == StepKind::Reload);
    RELEASE_ASSERT(step.destination.isRegister());
    checkMemoryOfEncodedMove(*this, step.sources[0], step.width, MemoryOfEncodedMove::IsRead);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    loadForEncodedMove(m_jit, addressOf(step.sources[0]), step.width, step.destination.reg());
}

void Encoder::encodeSave(const Step& step)
{
    ASSERT(step.kind == StepKind::Save);
    RELEASE_ASSERT(step.sources[0].isRegister());
    checkMemoryOfEncodedMove(*this, step.destination, step.width, MemoryOfEncodedMove::IsWritten);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    storeForEncodedMove(m_jit, step.sources[0].reg(), step.width, addressOf(step.destination));
}

// The whole registers change places, so the two values need not have one width.
void Encoder::encodeSwap(const Step& step)
{
    ASSERT(step.kind == StepKind::Swap);
    RELEASE_ASSERT(step.destination.isRegister() && step.sources[0].isRegister());
    Reg first = step.destination.reg();
    Reg second = step.sources[0].reg();
    checkRegisterOfEncodedMove(first);
    checkRegisterOfEncodedMove(second);
    RELEASE_ASSERT(bankOf(first) == bankOf(second));
    if (first == second)
        return;

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    if (first.isGPR()) {
        RELEASE_ASSERT(first.gpr() != scratchGPR && second.gpr() != scratchGPR);
        m_jit.swap(first.gpr(), second.gpr());
        return;
    }
    static_assert(scratchFPR == MacroAssembler::fpTempRegister);
    RELEASE_ASSERT(first.fpr() != scratchFPR && second.fpr() != scratchFPR);
    m_jit.swapDouble(first.fpr(), second.fpr());
}

void Encoder::encodeClearSpill(const Step& step)
{
    ASSERT(step.kind == StepKind::ClearSpill);
    RELEASE_ASSERT(step.destination.isSpill() && m_frameIsSetUp);
    Width width = m_allocation.spills()[step.destination.spillIndex()].width;
    MacroAssembler::Address address = addressOf(step.destination);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    if (encodedMoveIsWide(width))
        m_jit.store64(CCallHelpers::TrustedImm32(0), address);
    else
        m_jit.store32(CCallHelpers::TrustedImm32(0), address);
}

void Encoder::encodeKeepPointer(const Step& step)
{
    ASSERT(step.kind == StepKind::KeepPointer);
    RELEASE_ASSERT(step.destination.isSpill());
    checkMemoryOfEncodedMove(*this, step.destination, Width64, MemoryOfEncodedMove::IsWritten);
    ASSERT(m_allocation.spills()[step.destination.spillIndex()].isPointerBesideNarrowReference);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    GPRReg pointer = m_jit.scratchRegister();
    ASSERT(pointer == scratchGPR);
    Location reference = step.sources[0];
    if (reference.isRegister()) {
        checkRegisterOfEncodedMove(reference.reg());
        decodeKeptReference(m_jit, reference.gpr(), pointer);
    } else {
        checkMemoryOfEncodedMove(*this, reference, Width32, MemoryOfEncodedMove::IsRead);
        m_jit.load32(addressOf(reference), pointer);
        decodeKeptReference(m_jit, pointer, pointer);
    }
    m_jit.store64(pointer, addressOf(step.destination));
}

// On x86-64 it may write the flags: a zero is made by xor, and a sum that no lea reaches by add.
// It may write the scratch GPR wherever the destination is an FPR, and on ARM64 for a sum as well.
void Encoder::encodeMaterialize(const Operation* operation, Reg destination)
{
    RELEASE_ASSERT(operation->hasNoPosition());
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    willWriteRegisterOfEncodedMove(m_jit, destination);

    Width width = widthOf(operation->type());
    if (operation->isConstant()) {
        uint64_t bits = static_cast<uint64_t>(operation->constantBits());
        if (width == Width32)
            bits = static_cast<uint32_t>(bits);
        if (destination.isGPR())
            materializeBitsInGPR(m_jit, bits, width, destination.gpr());
        else
            materializeBitsInFPR(*this, bits, width, destination.fpr());
        return;
    }

    if (destination.isGPR()) {
        materializeInGPR(*this, operation, destination.gpr());
        return;
    }

    GPRReg scratch = m_jit.scratchRegister();
    materializeInGPR(*this, operation, scratch);
    if (encodedMoveIsWide(width))
        m_jit.move64ToDouble(scratch, destination.fpr());
    else
        m_jit.move32ToFloat(scratch, destination.fpr());
}

bool Encoder::tryEncodePairOfMoves(const Step& first, const Step& second)
{
#if CPU(ARM64)
    if (first.kind != second.kind || (first.kind != StepKind::Save && first.kind != StepKind::Reload))
        return false;
    if (first.width != Width64 || second.width != Width64)
        return false;

    bool isSave = first.kind == StepKind::Save;
    Location firstMemory = isSave ? first.destination : first.sources[0];
    Location secondMemory = isSave ? second.destination : second.sources[0];
    Location firstPlace = isSave ? first.sources[0] : first.destination;
    Location secondPlace = isSave ? second.sources[0] : second.destination;
    if (!firstMemory.isMemory() || !secondMemory.isMemory() || !firstPlace.isRegister() || !secondPlace.isRegister())
        return false;

    Reg firstRegister = firstPlace.reg();
    Reg secondRegister = secondPlace.reg();
    if (bankOf(firstRegister) != bankOf(secondRegister))
        return false;
    // There is no load of a pair into one register.
    if (!isSave && firstRegister == secondRegister)
        return false;

    MemoryOfEncodedMove access = isSave ? MemoryOfEncodedMove::IsWritten : MemoryOfEncodedMove::IsRead;
    checkMemoryOfEncodedMove(*this, firstMemory, Width64, access);
    checkMemoryOfEncodedMove(*this, secondMemory, Width64, access);
    MacroAssembler::Address firstAddress = addressOf(firstMemory);
    MacroAssembler::Address secondAddress = addressOf(secondMemory);
    if (firstAddress.base != secondAddress.base)
        return false;
    constexpr int64_t bytesOfWord = bytesForWidth(Width64);
    int64_t distance = static_cast<int64_t>(secondAddress.offset) - static_cast<int64_t>(firstAddress.offset);
    if (distance != bytesOfWord && distance != -bytesOfWord)
        return false;

    bool firstIsLower = distance > 0;
    MacroAssembler::Address address = firstIsLower ? firstAddress : secondAddress;
    Reg lower = firstIsLower ? firstRegister : secondRegister;
    Reg upper = firstIsLower ? secondRegister : firstRegister;
    bool isEncodable;
    if (isSave)
        isEncodable = lower.isGPR() ? ARM64Assembler::isValidSTPImm<64>(address.offset) : ARM64Assembler::isValidSTPFPImm<64>(address.offset);
    else
        isEncodable = lower.isGPR() ? ARM64Assembler::isValidLDPImm<64>(address.offset) : ARM64Assembler::isValidLDPFPImm<64>(address.offset);
    if (!isEncodable)
        return false;

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    if (isSave) {
        checkRegisterOfEncodedMove(lower);
        checkRegisterOfEncodedMove(upper);
        if (lower.isGPR())
            m_jit.storePair64(lower.gpr(), upper.gpr(), address);
        else
            m_jit.storePairDouble(lower.fpr(), upper.fpr(), address);
        return true;
    }

    willWriteRegisterOfEncodedMove(m_jit, lower);
    willWriteRegisterOfEncodedMove(m_jit, upper);
    if (lower.isGPR()) {
        ASSERT(lower.gpr() != address.base && upper.gpr() != address.base);
        m_jit.loadPair64(address, lower.gpr(), upper.gpr());
    } else
        m_jit.loadPairDouble(address, lower.fpr(), upper.fpr());
    return true;
#else
    UNUSED_PARAM(first);
    UNUSED_PARAM(second);
    return false;
#endif
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
