/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoderPairsOfLoads.h"

#if ENABLE(AOT)

#include "AOTMachineEncoder.h"

namespace JSC { namespace AOT { namespace Machine {

#if CPU(ARM64)

static bool isLoadPairOffsetEncodableARM64(Width width, bool loadsNumbers, int32_t offset)
{
    if (width == Width32)
        return loadsNumbers ? ARM64Assembler::isValidLDPFPImm<32>(offset) : ARM64Assembler::isValidLDPImm<32>(offset);
    return loadsNumbers ? ARM64Assembler::isValidLDPFPImm<64>(offset) : ARM64Assembler::isValidLDPImm<64>(offset);
}

static bool loadOfPairMayFaultARM64(const Encoder& encoder, const Operation* load)
{
    return load->traps() || encoder.m_liveness.of(load).has(ValueFlag::TakesOverTrap);
}

bool tryEncodePairOfLoads(Encoder& encoder, const Step& first, const Step& second)
{
    ASSERT(first.kind == StepKind::Execute);
    if (second.kind != StepKind::Execute || first.operation->opcode() != Load || second.operation->opcode() != Load)
        return false;
    const Operation* firstLoad = first.operation;
    const Operation* secondLoad = second.operation;

    Width width = firstLoad->accessWidth();
    bool loadsNumbers = firstLoad->type().isFloat();
    if ((width != Width32 && width != Width64) || secondLoad->accessWidth() != width || secondLoad->type().isFloat() != loadsNumbers)
        return false;
    if (loadOfPairMayFaultARM64(encoder, firstLoad) || loadOfPairMayFaultARM64(encoder, secondLoad))
        return false;
    if (!first.destination.isRegister() || !second.destination.isRegister() || first.destination == second.destination)
        return false;

    LeafCursor firstLeaves(first);
    LeafCursor secondLeaves(second);
    AddressForm firstAddress = encoder.resolveAddress(firstLoad, firstLeaves);
    AddressForm secondAddress = encoder.resolveAddress(secondLoad, secondLeaves);
    if (firstAddress.kind != AddressForm::BaseOffset || secondAddress.kind != AddressForm::BaseOffset || firstAddress.base != secondAddress.base)
        return false;
    if (!loadsNumbers && first.destination.gpr() == firstAddress.base)
        return false;

    int64_t distance = static_cast<int64_t>(secondAddress.offset) - static_cast<int64_t>(firstAddress.offset);
    int64_t size = bytesForWidth(width);
    if (distance != size && distance != -size)
        return false;
    bool firstIsLower = distance > 0;
    MacroAssembler::Address address = firstIsLower ? firstAddress.address() : secondAddress.address();
    if (!isLoadPairOffsetEncodableARM64(width, loadsNumbers, address.offset))
        return false;

    Reg lower = firstIsLower ? first.destination.reg() : second.destination.reg();
    Reg upper = firstIsLower ? second.destination.reg() : first.destination.reg();
    if (loadsNumbers) {
        if (width == Width32)
            encoder.m_jit.loadPairFloat(address, lower.fpr(), upper.fpr());
        else
            encoder.m_jit.loadPairDouble(address, lower.fpr(), upper.fpr());
        return true;
    }
    if (width == Width32)
        encoder.m_jit.loadPair32(address, lower.gpr(), upper.gpr());
    else
        encoder.m_jit.loadPair64(address, lower.gpr(), upper.gpr());
    return true;
}

#else

bool tryEncodePairOfLoads(Encoder&, const Step&, const Step&)
{
    return false;
}

#endif

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
