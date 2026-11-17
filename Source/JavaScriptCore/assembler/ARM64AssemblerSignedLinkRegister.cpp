/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "ARM64Assembler.h"

#if ENABLE(ASSEMBLER) && ENABLE(RETURN_ADDRESS_SIGNING)

#include "ARM64LinkRegisterUse.h"
#include <wtf/SetForScope.h>

namespace JSC {

using namespace ARM64LinkRegister;
using ARM64Registers::fp;
using ARM64Registers::lr;
using ARM64Registers::sp;
using ARM64Registers::x16;
using ARM64Registers::x17;
using ARM64Registers::zr;

enum Hint : int { XPACLRI = 7, PACIB1716 = 10, AUTIB1716 = 14, PACIBSP = 27, AUTIBSP = 31 };

static constexpr uint32_t loadThroughLinkRegister = 0x394003df; // LDRB wzr, [x30]
static constexpr int sizeOfWord = 8;
static constexpr int sizeOfPair = 2 * sizeOfWord;

static constexpr uint32_t withZeroRegisterAt(uint32_t instruction, unsigned low)
{
    return instruction | zeroOrStackPointer << low;
}

static constexpr bool modifierIsStackPointerOutsideOf(uint32_t instruction, const AccessOfWords& access)
{
    if (registerAt(instruction, 5) != zeroOrStackPointer || (access.isPair && registerAt(instruction, 0) == linkRegister))
        return false;
    return !((access.isLoad ? access.offsetAfter() : access.offsetBefore()) + (access.isPair ? sizeOfPair : sizeOfWord));
}

void ARM64Assembler::putSigningLinkRegister(int word)
{
    const uint32_t instruction = word;
    if (signsAuthenticatesOrStrips(instruction) || instruction == loadThroughLinkRegister)
        return;
    if (!mayName(instruction)) [[likely]]
        return m_buffer.putInt(word);
    if (jumpsToIt(instruction))
        m_buffer.removeIntUnlessPinned(std::exchange(m_offsetOfLoadThroughLinkRegister, UINT_MAX), onlyComputesWithoutIt);
    const Use use = useBy(instruction);
    if (use == Use { })
        return m_buffer.putInt(word);

    SetForScope asWritten(m_signsLinkRegister, false);
    if (std::optional access = accessOfWordsBy(instruction); access && !modifierIsStackPointerOutsideOf(instruction, *access)) {
        bool isInAddress = registerAt(instruction, 5) == linkRegister || (access->hasIndex && registerAt(instruction, 16) == linkRegister);
        if (!access->isLoad)
            return storeLinkRegister(instruction, *access);
        if (!isInAddress)
            return loadLinkRegister(instruction, *access);
    }
    if (use.isCopiedOut)
        hint(PACIBSP);
    insn(word);
    if (!use.isOverwritten)
        return hint(XPACLRI);
    hint(AUTIBSP);
    loadThroughAuthenticatedLinkRegister();
}

// Not every processor faults where it fails to authenticate: MacroAssemblerARM64::validateUntaggedPtr(). A jump to the address faults
// as well, and takes this out again if nothing can happen in between.
void ARM64Assembler::loadThroughAuthenticatedLinkRegister()
{
    m_offsetOfLoadThroughLinkRegister = m_buffer.putRemovableInt(loadThroughLinkRegister);
}

void ARM64Assembler::saveX16AndX17AndMoveAddressBehindWordToX16(uint32_t instruction, const AccessOfWords& access, int offsetOfWord)
{
    RegisterID base = static_cast<RegisterID>(registerAt(instruction, 5));
    const int distance = offsetOfWord + sizeOfWord + (base == sp ? sizeOfPair : 0);
    stp<64>(x16, x17, sp, PairPreIndex(-sizeOfPair));
    if (access.hasIndex) {
        unsigned index = registerAt(instruction, 16);
        add<64>(x16, base, index == zeroOrStackPointer ? zr : static_cast<RegisterID>(index), static_cast<ExtendType>(bitsOf(instruction, 15, 13)), bitOf(instruction, 12) ? 3 : 0);
        base = x16;
    }
    if (distance < 0)
        return sub<64>(x16, base, UInt12(-distance));
    if (distance >> 12) {
        add<64>(x16, base, UInt12(distance >> 12), 12);
        base = x16;
    }
    add<64>(x16, base, UInt12(distance & 0xfff));
}

void ARM64Assembler::storeLinkRegister(uint32_t instruction, const AccessOfWords& access)
{
    const bool isFirst = registerAt(instruction, 0) == linkRegister;
    const bool isSecond = access.isPair && registerAt(instruction, 10) == linkRegister;
    if (access.isPair || access.writesBaseBack) {
        uint32_t rest = isFirst ? withZeroRegisterAt(instruction, 0) : instruction;
        insn(isSecond ? withZeroRegisterAt(rest, 10) : rest);
    }
    auto storeAt = [&](int offset) {
        saveX16AndX17AndMoveAddressBehindWordToX16(instruction, access, offset);
        mov<64>(x17, lr);
        hint(PACIB1716);
        stur<64>(x17, x16, -sizeOfWord);
        ldp<64>(x16, x17, sp, PairPostIndex(sizeOfPair));
    };
    if (isFirst)
        storeAt(access.offsetAfter());
    if (isSecond)
        storeAt(access.offsetAfter() + sizeOfWord);
}

void ARM64Assembler::loadLinkRegister(uint32_t instruction, const AccessOfWords& access)
{
    const bool isFirst = registerAt(instruction, 0) == linkRegister;
    saveX16AndX17AndMoveAddressBehindWordToX16(instruction, access, access.offsetBefore() + (isFirst ? 0 : sizeOfWord));
    ldur<64>(x17, x16, -sizeOfWord);
    hint(AUTIB1716);
    mov<64>(lr, x17);
    ldp<64>(x16, x17, sp, PairPostIndex(sizeOfPair));

    const unsigned other = access.isPair ? registerAt(instruction, isFirst ? 10 : 0) : zeroOrStackPointer;
    if (other != zeroOrStackPointer && other != linkRegister)
        insn(withZeroRegisterAt(instruction, isFirst ? 0 : 10));
    else if (access.writesBaseBack) {
        RegisterID base = static_cast<RegisterID>(registerAt(instruction, 5));
        if (access.immediate < 0)
            sub<64>(base, base, UInt12(-access.immediate));
        else
            add<64>(base, base, UInt12(access.immediate));
    }
    loadThroughAuthenticatedLinkRegister();
}

void ARM64Assembler::popLinkRegisterAndReturnToOldOne(FirstOfPoppedPair first)
{
    SetForScope asWritten(m_signsLinkRegister, false);
    mov<64>(x17, lr);
    ldp<64>(first == FirstOfPoppedPair::FramePointer ? fp : zr, lr, sp, PairPostIndex(sizeOfPair));
    hint(AUTIBSP);
    m_buffer.putInt(loadThroughLinkRegister);
    ret(x17);
}

void ARM64Assembler::signLinkRegisterInX17()
{
    SetForScope asWritten(m_signsLinkRegister, false);
    mov<64>(x17, lr);
    hint(PACIB1716);
}

void ARM64Assembler::signNearPCRelativeAddressInX17()
{
    SetForScope asWritten(m_signsLinkRegister, false);
    adr(x17, 0);
    hint(PACIB1716);
}

void ARM64Assembler::signFarPCRelativeAddressInX17()
{
    SetForScope asWritten(m_signsLinkRegister, false);
    adrp(x17, 0);
    add<64>(x17, x17, UInt12(0));
    hint(PACIB1716);
}

void ARM64Assembler::authenticateX17IntoLinkRegister()
{
    SetForScope asWritten(m_signsLinkRegister, false);
    hint(AUTIB1716);
    mov<64>(lr, x17);
    mov<64>(x17, zr);
    loadThroughAuthenticatedLinkRegister();
}

void ARM64Assembler::authenticateX17AndJump()
{
    SetForScope asWritten(m_signsLinkRegister, false);
    hint(AUTIB1716);
    br(x17);
}

void ARM64Assembler::authenticateX17AndCompare(RegisterID other)
{
    SetForScope asWritten(m_signsLinkRegister, false);
    hint(AUTIB1716);
    ldrb(zr, x17, 0);
    cmp<64>(x17, other);
    mov<64>(x17, zr);
}

void ARM64Assembler::authenticateX17ToLookAt(RegisterID dest)
{
    {
        SetForScope asWritten(m_signsLinkRegister, false);
        hint(AUTIB1716);
    }
    ldrb(zr, x17, 0);
    sub<64>(x17, x17, UInt12(distanceInFrontOfAddressToLookAt));
    mov<64>(dest, x17);
}

} // namespace JSC

#endif // ENABLE(ASSEMBLER) && ENABLE(RETURN_ADDRESS_SIGNING)
