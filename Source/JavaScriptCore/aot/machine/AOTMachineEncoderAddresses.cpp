/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Machine {

static_assert(registerWrittenAtFaults == scratchGPR);

static AddressForm resolvedAddressAt(GPRReg base, int64_t offset)
{
    RELEASE_ASSERT(base != InvalidGPRReg);
    AddressForm form;
    form.kind = AddressForm::BaseOffset;
    form.base = base;
    form.offset = safeCast<int32_t>(offset);
    return form;
}

static AddressForm resolvedAddressInFrame(const Encoder& encoder, int64_t offsetFromFP)
{
    RELEASE_ASSERT(encoder.m_frameIsSetUp);
    MacroAssembler::Address address = encoder.m_frame.addressInFrame(static_cast<intptr_t>(offsetFromFP));
    return resolvedAddressAt(address.base, address.offset);
}

static GPRReg resolvedAddressRegisterOf(Location leaf)
{
    RELEASE_ASSERT(leaf.isRegister() && leaf.reg().isGPR());
    return leaf.gpr();
}

// The allocator gave this base no register, which it does only where acceptsImmediate() allows it.
static AddressForm resolvedAddressOfBaseWithoutPosition(const Encoder& encoder, const Operation* base, int32_t offsetOfAccess)
{
    RELEASE_ASSERT(base->hasNoPosition());

    if (base->opcode() == InstanceAddressOf) {
        RELEASE_ASSERT(!offsetOfAccess);
        AddressForm form;
        form.kind = AddressForm::InstanceSymbol;
        form.symbol = base->symbol();
        ASSERT(form.symbol.space == SymbolSpace::InstanceWord);
        return form;
    }

    Checked<int64_t> offset = offsetOfAccess;
    while (base->opcode() == Add) {
        RELEASE_ASSERT(base->hasNoPosition() && base->child(1)->hasInt64());
        offset += base->child(1)->asInt64();
        base = base->child(0);
    }

    if (base->opcode() == SlotBase) {
        offset += base->slot()->offsetFromFP();
        return resolvedAddressInFrame(encoder, offset.value());
    }

    if (base->opcode() == FramePointer) {
        RELEASE_ASSERT(encoder.m_frameIsSetUp);
        return resolvedAddressAt(MacroAssembler::framePointerRegister, offset.value());
    }

    // A constant is never a base, because code holds no absolute address, and the allocator puts the address of code in a register.
    RELEASE_ASSERT(base->opcode() == ArgumentReg && base->hasNoPosition());
    Reg reg = base->argumentReg();
    RELEASE_ASSERT(reg.isGPR() && isPinned(reg));
    return resolvedAddressAt(reg.gpr(), offset.value());
}

static AddressForm resolvedAddressOfLeaf(const Encoder& encoder, const Operation* base, Location location, int32_t offset)
{
    switch (location.kind()) {
    case Location::Register:
        return resolvedAddressAt(resolvedAddressRegisterOf(location), offset);

    case Location::SlotBytes:
        RELEASE_ASSERT(base->opcode() == SlotBase && base->slot()->index() == location.indexOfStackSlot());
        return resolvedAddressOfBaseWithoutPosition(encoder, base, offset);

    case Location::Immediate:
        return resolvedAddressOfBaseWithoutPosition(encoder, base, offset);

    case Location::None:
    case Location::Spill:
    case Location::Outgoing:
    case Location::InstanceWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void resolveIndexOfAddress(const Liveness& liveness, AddressForm& form, const Operation* index, LeafCursor& leaves)
{
    const Operation* shift = nullptr;
    if (index->opcode() == Shl && liveness.isFused(index)) {
        shift = index;
        index = shift->child(0);
    }
    if (liveness.isFused(index)) {
        RELEASE_ASSERT(index->opcode() == ZExt32 || index->opcode() == SExt32);
        form.extension = index->opcode() == ZExt32 ? IndexExtension::ZeroExtend32 : IndexExtension::SignExtend32;
        index = index->child(0);
        // The register holds the extended word already, and the scan has asked for the form without an extension.
        if (form.extension == IndexExtension::ZeroExtend32 && clearsUpperHalfOfRegister(index))
            form.extension = IndexExtension::None;
    }
    RELEASE_ASSERT(!liveness.isFused(index));
    form.index = resolvedAddressRegisterOf(leaves.take());

    if (shift) {
        // The amount is the Shl's second child, so its leaf comes after the leaf of the shifted value.
        const Operation* amount = shift->child(1);
        RELEASE_ASSERT(amount->hasInt32() && !liveness.isFused(amount));
        leaves.take();
        form.shift = static_cast<uint8_t>(static_cast<uint32_t>(amount->asInt32()) & 63);
    }
}

// It emits nothing: tryEncodePairOfStores() resolves addresses and may then leave both stores to encodeStore().
AddressForm Encoder::resolveAddress(const Operation* access, LeafCursor& leaves)
{
    RELEASE_ASSERT(isMemoryAccess(access->opcode()));
    const Operation* address = access->children().back();
    if (!m_liveness.isFused(address))
        return resolvedAddressOfLeaf(*this, address, leaves.take(), access->offset());

    const Operation* sum = address;
    RELEASE_ASSERT(sum->opcode() == Add && !m_liveness.isFused(sum->child(0)));

    AddressForm form;
    form.kind = AddressForm::BaseIndex;
    form.offset = access->offset();
    form.base = resolvedAddressRegisterOf(leaves.take());
    resolveIndexOfAddress(m_liveness, form, sum->child(1), leaves);

    ASSERT(isEncodableIndexedAddress(access->accessWidth(), form.shift, form.extension, form.offset));
    return form;
}

AddressForm Encoder::resolveScaledSum(const Operation* sum, LeafCursor& leaves)
{
    RELEASE_ASSERT(addsScaledIndex(m_liveness, sum));
    unsigned indexChildIndex = m_liveness.isFused(sum->child(1));
    const Operation* base = sum->child(1 - indexChildIndex);
    RELEASE_ASSERT(!m_liveness.isFused(base));

    AddressForm form;
    form.kind = AddressForm::BaseIndex;
    auto takeBase = [&] {
        AddressForm resolved = resolvedAddressOfLeaf(*this, base, leaves.take(), 0);
        RELEASE_ASSERT(resolved.kind == AddressForm::BaseOffset);
        form.base = resolved.base;
        form.offset = resolved.offset;
    };
    if (indexChildIndex)
        takeBase();
    resolveIndexOfAddress(m_liveness, form, sum->child(indexChildIndex), leaves);
    if (!indexChildIndex)
        takeBase();

    RELEASE_ASSERT(isEncodableScaledSum(form.shift, form.extension));
    return form;
}

MacroAssembler::Address Encoder::addressOf(Location memory) const
{
    switch (memory.kind()) {
    case Location::Spill:
        RELEASE_ASSERT(m_frameIsSetUp);
        return m_frame.addressInFrame(m_frame.offsetFromFPOf(memory.spillIndex()));

    case Location::Outgoing:
        RELEASE_ASSERT(m_frameIsSetUp);
        ASSERT(memory.offsetFromSP() < m_frame.bytesBelowFrameRecord());
        return MacroAssembler::Address(MacroAssembler::stackPointerRegister, safeCast<int32_t>(memory.offsetFromSP()));

    case Location::InstanceWord:
        return MacroAssembler::Address(instanceGPR, memory.offsetInInstance());

    // SlotBytes is an address, not a word that holds a value.
    case Location::None:
    case Location::Register:
    case Location::SlotBytes:
    case Location::Immediate:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::pair<GPRReg, bool> Encoder::referenceOf(const Operation* access, const AddressForm& address) const
{
    ASSERT_UNUSED(access, isMemoryAccess(access->opcode()));

    GPRReg reference = InvalidGPRReg;
    bool isNarrow = false;
    switch (address.kind) {
    case AddressForm::BaseOffset:
        RELEASE_ASSERT(address.base != cellCageBaseGPR);
        reference = address.base;
        break;

    case AddressForm::BaseIndex:
        // [cage base + (word << cell32Shift) + offset]: the reference was never made a pointer.
        // With any other base, the index could move the address of a null reference out of the pages that fault.
        RELEASE_ASSERT(address.base == cellCageBaseGPR && address.shift == cell32Shift && address.extension != IndexExtension::SignExtend32);
        reference = address.index;
        isNarrow = true;
        break;

    case AddressForm::InstanceSymbol:
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The reference is read after the fault handler has overwritten a scratch register.
    RELEASE_ASSERT(reference != InvalidGPRReg && reference != scratchGPR && reference != secondScratchGPR);
    return { reference, isNarrow };
}

void Encoder::recordTrappingAccess(const Operation* access, const AddressForm& address, MacroAssembler::Label instruction)
{
    RELEASE_ASSERT(instruction.isSet());
    ASSERT(access->offset() >= 0 && static_cast<uint64_t>(access->offset()) + bytesForWidth(access->accessWidth()) <= trapReach());

    // An access has one record, and so one origin.
    const Operation* trappingLoad = access;
    if (m_liveness.of(access).has(ValueFlag::TakesOverTrap)) {
        RELEASE_ASSERT(!access->traps());
        trappingLoad = m_liveness.trapTakenOverBy(access);
        ASSERT(trappingLoad && m_liveness.isDropped(trappingLoad) && trappingLoad->lastChild() == access->lastChild());
    }
    RELEASE_ASSERT(trappingLoad && trappingLoad->traps());

    auto [reference, isNarrow] = referenceOf(access, address);
    m_proc.m_trappingAccesses.append(m_arena, TrappingAccess { instruction, trappingLoad->origin(), reference, isNarrow });
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
