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
