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
    bool hasOnlyLowHalf = !encodedMoveIsWide(width) || isRepresentableAs<uint32_t>(static_cast<size_t>(bits));
    if (bits && hasOnlyLowHalf && tryMoveSmallConstant(jit, static_cast<int32_t>(static_cast<uint32_t>(bits)), destination))
        return;
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

#if CPU(X86_64)
static bool isLoadedIntoGPR(uint64_t bits)
{
    if (isRepresentableAs<uint32_t>(static_cast<size_t>(bits)) || isRepresentableAs<int32_t>(static_cast<int64_t>(bits)))
        return false;
    constexpr unsigned shiftOfTag = 48;
    constexpr uint64_t lowBytesOfLongDouble = (1ull << 40) - 1;
    bool isBoxedInteger = bits >> shiftOfTag == static_cast<uint64_t>(JSValue::NumberTag) >> shiftOfTag;
    return (isBoxedInteger || !(bits >> shiftOfTag) || !(bits & lowBytesOfLongDouble)) && !findPinnedConstant(bits);
}
#endif

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

void Encoder::materializeBitsInFPR(uint64_t bits, Width width, FPRReg destination)
{
    CCallHelpers& jit = m_jit;
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

    std::array<uint8_t, sizeof(uint64_t)> bytes = std::bit_cast<std::array<uint8_t, sizeof(uint64_t)>>(bits);
    uint32_t index = m_proc.addConstantData(std::span<const uint8_t>(bytes), sizeof(uint64_t), ConstantKind::Number);
#if CPU(X86_64)
    m_relocations.loadDouble(CodeSymbol { SymbolSpace::Constant, index }, destination);
#else
    GPRReg address = jit.scratchRegister();
    m_relocations.moveAddress(CodeSymbol { SymbolSpace::Constant, index }, address, PCRelativeReach::Far);
    jit.loadDouble(MacroAssembler::Address(address), destination);
#endif
}

static ALWAYS_INLINE void materializeInGPR(Encoder& encoder, const Operation* operation, GPRReg destination)
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
    m_jit.store64(pointer, addressOf(step.destination, MemoryOfEncodedMove::IsWritten));
}

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
#if CPU(X86_64)
        if (destination.isGPR() && encodedMoveIsWide(width) && isLoadedIntoGPR(bits)) {
            auto bytes = std::bit_cast<std::array<uint8_t, sizeof(uint64_t)>>(bits);
            uint32_t index = m_proc.addConstantData(std::span<const uint8_t>(bytes), sizeof(uint64_t), ConstantKind::Number);
            m_relocations.load64(CodeSymbol { SymbolSpace::Constant, index }, destination.gpr());
            return;
        }
#endif
        if (destination.isGPR())
            materializeBitsInGPR(m_jit, bits, width, destination.gpr());
        else
            materializeBitsInFPR(bits, width, destination.fpr());
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
    if (!isSave && firstRegister == secondRegister)
        return false;

    MemoryOfEncodedMove access = isSave ? MemoryOfEncodedMove::IsWritten : MemoryOfEncodedMove::IsRead;
    checkMemoryOfEncodedMove(*this, firstMemory, Width64, access);
    checkMemoryOfEncodedMove(*this, secondMemory, Width64, access);
    MacroAssembler::Address firstAddress = addressOf(firstMemory, access);
    MacroAssembler::Address secondAddress = addressOf(secondMemory, access);
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
