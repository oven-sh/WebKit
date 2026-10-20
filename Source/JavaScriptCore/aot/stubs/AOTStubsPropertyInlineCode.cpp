/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyInlineCode.h"

#if ENABLE(AOT)

#include "AOTPropertyCachesFront.h"
#include "AOTStubsPropertyProbes.h"
#include "JSObject.h"

namespace JSC { namespace AOT { namespace InlinePropertyCode {

static_assert(!PackedSlot::offsetOfPayload() && sizeof(PackedSlot) == sizeof(uint64_t));
static_assert(PackedSlot::offsetOfLocation() == sizeof(uint32_t) && PackedSlot::offsetOfName() + sizeof(uint16_t) == sizeof(PackedSlot));

struct FrontProbe {
    static constexpr GPRReg scratch = gprOf(StubRegister::T10);
    static constexpr GPRReg structureID = gprOf(StubRegister::T11);
    static constexpr GPRReg cell = gprOf(slotHeaderRegister);
    static constexpr GPRReg row = gprOf(StubRegister::T13);
#if CPU(ARM64)
    static constexpr GPRReg column = gprOf(slotNameRegister);
#endif
};

Jump loadConstantSlotAtSite(CCallHelpers& jit, GPRReg site, GPRReg header, GPRReg result)
{
    jit.load8(Address(site, Slot::offsetOfKind()), header);
    Jump isNotConstant = jit.branch32(CCallHelpers::NotEqual, header, TrustedImm32(static_cast<int32_t>(Slot::Kind::Constant)));
    jit.load64(Address(site, Slot::offsetOfPayload()), result);
    return isNotConstant;
}

// The stubs that `otherwise` leads to take the entire cell in FrontProbe::cell.
static BaseIndex locateOwnInlineValue(CCallHelpers& jit, SlotAccess access, GPRReg receiver, uint32_t column, JumpList& otherwise)
{
    RELEASE_ASSERT(RowDisplacementTable::hasColumn(column));
    RELEASE_ASSERT(!registersOfFrontProbe().contains(receiver, IgnoreVectors));
    const TrustedImm32 upperHalf(static_cast<int32_t>(PackedSlot::upperHalfForOwnInlineValue(static_cast<uint16_t>(column))));
#if CPU(X86_64)
    const int32_t offsetOfCell = static_cast<int32_t>(column * sizeof(PackedSlot));
    const int32_t offsetOfUpperHalf = offsetOfCell + static_cast<int32_t>(PackedSlot::offsetOfLocation());
    jit.load32(Address(receiver, JSCell::structureIDOffset()), FrontProbe::structureID);
    loadFrontRow(jit, access, FrontProbe::structureID, FrontProbe::row, FrontProbe::scratch);
    jit.load64(Address(FrontProbe::row, offsetOfCell), FrontProbe::cell);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, Address(FrontProbe::row, offsetOfUpperHalf), upperHalf));
    jit.zeroExtend32ToWord(FrontProbe::cell, FrontProbe::cell);
    return BaseIndex(receiver, FrontProbe::cell, CCallHelpers::TimesEight);
#elif CPU(ARM64)
    constexpr int upperHalfShift = 8 * PackedSlot::offsetOfLocation();
    jit.move(TrustedImm32(static_cast<int32_t>(column)), FrontProbe::column);
    jit.load32(Address(receiver, JSCell::structureIDOffset()), FrontProbe::structureID);
    loadFrontRow(jit, access, FrontProbe::structureID, FrontProbe::row, FrontProbe::scratch);
    jit.load64(BaseIndex(FrontProbe::row, FrontProbe::column, CCallHelpers::TimesEight), FrontProbe::cell);
    jit.move(upperHalf, FrontProbe::scratch);
    jit.m_assembler.sub<64>(FrontProbe::scratch, FrontProbe::scratch, FrontProbe::cell, ARM64Assembler::LSR, upperHalfShift);
    otherwise.append(jit.branchTest64(CCallHelpers::NonZero, FrontProbe::scratch));
    return BaseIndex(receiver, FrontProbe::cell, CCallHelpers::TimesEight, 0, CCallHelpers::Extend::ZExt32);
#endif
}

JumpList loadOwnInlineValueByFront(CCallHelpers& jit, GPRReg receiver, uint32_t column, GPRReg result)
{
    JumpList otherwise;
    jit.load64(locateOwnInlineValue(jit, SlotAccess::Read, receiver, column, otherwise), result);
    return otherwise;
}

JumpList storeOwnInlineValueByFront(CCallHelpers& jit, GPRReg receiver, uint32_t column, GPRReg value, SlotAccess access)
{
    RELEASE_ASSERT(access != SlotAccess::Read);
    RELEASE_ASSERT(!registersOfFrontProbe().contains(value, IgnoreVectors));
    JumpList otherwise;
    jit.store64(value, locateOwnInlineValue(jit, access, receiver, column, otherwise));
    return otherwise;
}

RegisterSet registersOfFrontProbe()
{
#if CPU(X86_64)
    static_assert(noOverlap(FrontProbe::scratch, FrontProbe::structureID, FrontProbe::cell, FrontProbe::row));
    return RegisterSet { FrontProbe::scratch, FrontProbe::structureID, FrontProbe::cell, FrontProbe::row };
#elif CPU(ARM64)
    static_assert(noOverlap(FrontProbe::column, FrontProbe::scratch, FrontProbe::structureID, FrontProbe::cell, FrontProbe::row));
    return RegisterSet { FrontProbe::column, FrontProbe::scratch, FrontProbe::structureID, FrontProbe::cell, FrontProbe::row };
#endif
}

} } } // namespace JSC::AOT::InlinePropertyCode

#endif // ENABLE(AOT)
