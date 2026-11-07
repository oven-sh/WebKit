/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT) && CPU(X86_64)

namespace JSC { namespace AOT { namespace Machine {

static bool isPlainStoreOfX86Constant(const Encoder& encoder, const Operation* store)
{
    return store->child(0)->hasInt() && !store->traps() && !encoder.m_liveness.worksInMemory(store) && !encoder.m_liveness.of(store).has(ValueFlag::TakesOverTrap);
}

bool Encoder::tryEncodePairOfStores(const Step& first, const Step& second)
{
    ASSERT(first.kind == StepKind::Execute && second.kind == StepKind::Execute);
    const Operation* firstStore = first.operation;
    const Operation* secondStore = second.operation;
    ASSERT(firstStore->opcode() == Store && secondStore->opcode() == Store);
    if (!first.sources[0].isImmediate() || !second.sources[0].isImmediate())
        return false;

    Width width = firstStore->accessWidth();
    if (secondStore->accessWidth() != width || (width != Width32 && width != Width64))
        return false;
    if (!isPlainStoreOfX86Constant(*this, firstStore) || !isPlainStoreOfX86Constant(*this, secondStore))
        return false;

    LeafCursor firstLeaves(first);
    LeafCursor secondLeaves(second);
    if (!firstLeaves.take().isImmediate() || !secondLeaves.take().isImmediate())
        return false;
    AddressForm firstAddress = resolveAddress(firstStore, firstLeaves);
    AddressForm secondAddress = resolveAddress(secondStore, secondLeaves);
    if (firstAddress.kind != AddressForm::BaseOffset || secondAddress.kind != AddressForm::BaseOffset || firstAddress.base != secondAddress.base)
        return false;

    int64_t distance = static_cast<int64_t>(secondAddress.offset) - static_cast<int64_t>(firstAddress.offset);
    int64_t size = bytesForWidth(width);
    if (distance != size && distance != -size)
        return false;
    bool firstIsLower = distance > 0;
    MacroAssembler::Address address = firstIsLower ? firstAddress.address() : secondAddress.address();
    int64_t lower = (firstIsLower ? firstStore : secondStore)->child(0)->asInt();
    int64_t upper = (firstIsLower ? secondStore : firstStore)->child(0)->asInt();

    if (width == Width32) {
        int64_t whole = static_cast<int64_t>(static_cast<uint32_t>(lower) | static_cast<uint64_t>(static_cast<uint32_t>(upper)) << 32);
        if (address.offset % 8 || !isRepresentableAs<int32_t>(whole))
            return false;
        m_jit.store64(MacroAssembler::TrustedImm32(static_cast<int32_t>(whole)), address);
        return true;
    }

    if (lower || upper)
        return false;
    if (m_blockWithClearedScratchFPR != m_block || m_offsetBehindClearedScratchFPR != m_jit.debugOffset())
        m_jit.moveZeroToDouble(scratchFPR);
    m_jit.storeVector(scratchFPR, address);
    m_blockWithClearedScratchFPR = m_block;
    m_offsetBehindClearedScratchFPR = m_jit.debugOffset();
    return true;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
