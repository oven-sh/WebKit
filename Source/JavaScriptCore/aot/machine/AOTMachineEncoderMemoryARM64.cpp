/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"
#include "AOTInliningAcrossFiles.h"

#if ENABLE(AOT) && CPU(ARM64)

#include "AllowMacroScratchRegisterUsage.h"
#include <limits>

namespace JSC { namespace AOT { namespace Machine {

struct StoreSourceARM64 {
    bool isNumber() const { return fpr != InvalidFPRReg; }

    GPRReg gpr { InvalidGPRReg };
    FPRReg fpr { InvalidFPRReg };
};

static bool memoryAccessMayFaultARM64(const Encoder& encoder, const Operation* access)
{
    return access->traps() || encoder.m_liveness.of(access).has(ValueFlag::TakesOverTrap);
}

static bool isMemoryOffsetEncodableARM64(Width width, int32_t offset)
{
    if (ARM64Assembler::canEncodeSImmOffset(offset))
        return true;

    switch (width) {
    case Width8:
        return ARM64Assembler::canEncodePImmOffset<8>(offset);
    case Width16:
        return ARM64Assembler::canEncodePImmOffset<16>(offset);
    case Width32:
        return ARM64Assembler::canEncodePImmOffset<32>(offset);
    case Width64:
        return ARM64Assembler::canEncodePImmOffset<64>(offset);
    case Width128:
        return ARM64Assembler::canEncodePImmOffset<128>(offset);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static AddressForm makeMemoryAddressEncodableARM64(Encoder& encoder, const AddressForm& address, Width width)
{
    switch (address.kind) {
    case AddressForm::BaseOffset: {
        if (isMemoryOffsetEncodableARM64(width, address.offset))
            return address;

        AddressForm indexed;
        indexed.kind = AddressForm::BaseIndex;
        indexed.base = address.base;
        indexed.index = encoder.m_jit.getCachedMemoryTempRegisterIDAndInvalidate();
        RELEASE_ASSERT(indexed.base != indexed.index);
        encoder.m_jit.signExtend32ToPtr(MacroAssembler::TrustedImm32(address.offset), indexed.index);
        return indexed;
    }

    case AddressForm::BaseIndex:
        RELEASE_ASSERT(!address.offset);
        RELEASE_ASSERT(!address.shift || (1u << address.shift) == bytesForWidth(width));
        return address;

    case AddressForm::InstanceSymbol:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MacroAssembler::BaseIndex indexedMemoryOperandARM64(const AddressForm& address)
{
    ASSERT(address.kind == AddressForm::BaseIndex && !address.offset);

    MacroAssembler::Extend extend = MacroAssembler::Extend::None;
    switch (address.extension) {
    case IndexExtension::None:
        break;
    case IndexExtension::ZeroExtend32:
        extend = MacroAssembler::Extend::ZExt32;
        break;
    case IndexExtension::SignExtend32:
        extend = MacroAssembler::Extend::SExt32;
        break;
    }
    return MacroAssembler::BaseIndex(address.base, address.index, static_cast<MacroAssembler::Scale>(address.shift), 0, extend);
}

template<typename AddressType>
static void emitMemoryLoadARM64(CCallHelpers& jit, const Operation* load, AddressType address, Reg destination)
{
    bool isNumber = load->type().isInNumberRegister();

    switch (load->accessWidth()) {
    case Width8:
        if (load->opcode() == Load8S)
            jit.load8SignedExtendTo32(address, destination.gpr());
        else
            jit.load8(address, destination.gpr());
        return;

    case Width16:
        if (load->opcode() == Load16S)
            jit.load16SignedExtendTo32(address, destination.gpr());
        else
            jit.load16(address, destination.gpr());
        return;

    case Width32:
        if (isNumber)
            jit.loadFloat(address, destination.fpr());
        else
            jit.load32(address, destination.gpr());
        return;

    case Width64:
        if (isNumber)
            jit.loadDouble(address, destination.fpr());
        else
            jit.load64(address, destination.gpr());
        return;

    case Width128:
        jit.loadVector(address, destination.fpr());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename AddressType>
static void emitMemoryStoreARM64(CCallHelpers& jit, Width width, StoreSourceARM64 source, AddressType address)
{
    switch (width) {
    case Width8:
        jit.store8(source.gpr, address);
        return;

    case Width16:
        jit.store16(source.gpr, address);
        return;

    case Width32:
        if (source.isNumber())
            jit.storeFloat(source.fpr, address);
        else
            jit.store32(source.gpr, address);
        return;

    case Width64:
        if (source.isNumber())
            jit.storeDouble(source.fpr, address);
        else
            jit.store64(source.gpr, address);
        return;

    case Width128:
        jit.storeVector(source.fpr, address);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MacroAssembler::Address materializeInstanceWordAddressARM64(Encoder& encoder, CodeSymbol symbol)
{
    GPRReg scratch = encoder.m_jit.getCachedMemoryTempRegisterIDAndInvalidate();
    encoder.m_relocations.moveInstanceAddress(symbol, instanceGPR, scratch);
    return MacroAssembler::Address(scratch);
}

static void emitInstanceWordLoadARM64(Encoder& encoder, const Operation* load, CodeSymbol symbol, Reg destination)
{
    RelocationRecorder& relocations = encoder.m_relocations;
    bool isNumber = load->type().isFloat();

    switch (load->accessWidth()) {
    case Width8:
        relocations.loadInstanceWord(symbol, InstanceWordWidth::Byte, instanceGPR, destination.gpr());
        if (load->opcode() == Load8S)
            encoder.m_jit.signExtend8To32(destination.gpr(), destination.gpr());
        return;

    case Width16:
        emitMemoryLoadARM64(encoder.m_jit, load, materializeInstanceWordAddressARM64(encoder, symbol), destination);
        return;

    case Width32:
        if (isNumber)
            emitMemoryLoadARM64(encoder.m_jit, load, materializeInstanceWordAddressARM64(encoder, symbol), destination);
        else
            relocations.loadInstanceWord(symbol, InstanceWordWidth::Word32, instanceGPR, destination.gpr());
        return;

    case Width64:
        if (isNumber)
            relocations.loadInstanceDouble(symbol, instanceGPR, destination.fpr());
        else
            relocations.loadInstanceWord(symbol, InstanceWordWidth::Word64, instanceGPR, destination.gpr());
        return;

    case Width128:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitInstanceWordStoreARM64(Encoder& encoder, Width width, StoreSourceARM64 source, CodeSymbol symbol)
{
    RelocationRecorder& relocations = encoder.m_relocations;

    switch (width) {
    case Width8:
        relocations.storeInstanceWord(symbol, InstanceWordWidth::Byte, source.gpr, instanceGPR);
        return;

    case Width16:
        emitMemoryStoreARM64(encoder.m_jit, width, source, materializeInstanceWordAddressARM64(encoder, symbol));
        return;

    case Width32:
        if (source.isNumber())
            emitMemoryStoreARM64(encoder.m_jit, width, source, materializeInstanceWordAddressARM64(encoder, symbol));
        else
            relocations.storeInstanceWord(symbol, InstanceWordWidth::Word32, source.gpr, instanceGPR);
        return;

    case Width64:
        if (source.isNumber())
            relocations.storeInstanceDouble(symbol, source.fpr, instanceGPR);
        else
            relocations.storeInstanceWord(symbol, InstanceWordWidth::Word64, source.gpr, instanceGPR);
        return;

    case Width128:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<StoreSourceARM64> tryGetStoreSourceARM64(const Operation* store, Location location)
{
    const Operation* value = store->child(0);
    StoreSourceARM64 source;

    switch (location.kind()) {
    case Location::Register:
        if (value->type().isInNumberRegister())
            source.fpr = location.fpr();
        else
            source.gpr = location.gpr();
        return source;

    case Location::Immediate:
        if (!value->isConstant() || (static_cast<uint64_t>(value->constantBits()) & JSC::mask(store->accessWidth())))
            return std::nullopt;
        source.gpr = ARM64Registers::zr;
        return source;

    case Location::SlotBytes:
        return std::nullopt;

    case Location::None:
    case Location::Spill:
    case Location::Outgoing:
    case Location::InstanceWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StoreSourceARM64 materializeStoreSourceARM64(Encoder& encoder, const Operation* store, Location location)
{
    if (std::optional<StoreSourceARM64> source = tryGetStoreSourceARM64(store, location))
        return *source;

    StoreSourceARM64 source;
    source.gpr = encoder.m_jit.scratchRegister();
    encoder.encodeMaterialize(store->child(0), Reg(source.gpr));
    return source;
}

static bool isStorePairOffsetEncodableARM64(Width width, bool storesNumbers, int32_t offset)
{
    if (width == Width32)
        return storesNumbers ? ARM64Assembler::isValidSTPFPImm<32>(offset) : ARM64Assembler::isValidSTPImm<32>(offset);

    RELEASE_ASSERT(width == Width64);
    return storesNumbers ? ARM64Assembler::isValidSTPFPImm<64>(offset) : ARM64Assembler::isValidSTPImm<64>(offset);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Encoder::encodeLoad(const Step& step)
{
    ASSERT(step.kind == StepKind::Execute);
    const Operation* load = step.operation;
    ASSERT(isLoad(load->opcode()));
    ASSERT(load->opcode() == Load || load->type() == Int32);

    RELEASE_ASSERT(step.destination.isRegister());
    Reg destination = step.destination.reg();
    ASSERT(destination.isFPR() == load->type().isInNumberRegister());

    LeafCursor leaves(step);
    AddressForm address = resolveAddress(load, leaves);
    bool mayFault = memoryAccessMayFaultARM64(*this, load);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);

    if (address.kind == AddressForm::InstanceSymbol) {
        RELEASE_ASSERT(!address.offset && !mayFault);
        emitInstanceWordLoadARM64(*this, load, address.symbol, destination);
        return;
    }

    AddressForm encodable = makeMemoryAddressEncodableARM64(*this, address, load->accessWidth());
    MacroAssembler::Label instruction = m_jit.labelIgnoringWatchpoints();
    if (encodable.kind == AddressForm::BaseOffset)
        emitMemoryLoadARM64(m_jit, load, encodable.address(), destination);
    else
        emitMemoryLoadARM64(m_jit, load, indexedMemoryOperandARM64(encodable), destination);

    if (mayFault)
        recordTrappingAccess(load, address, instruction);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Encoder::encodeStore(const Step& step)
{
    ASSERT(step.kind == StepKind::Execute);
    const Operation* store = step.operation;
    ASSERT(isStore(store->opcode()));
    ASSERT(store->opcode() == Store || store->child(0)->type() == Int32);
    ASSERT(!m_liveness.isFused(store->child(0)) || store->child(0)->opcode() == Trunc || namesWord(store->child(0)));
    Width width = store->accessWidth();

    LeafCursor leaves(step);
    Location value = leaves.take();
    AddressForm address = resolveAddress(store, leaves);
    bool mayFault = memoryAccessMayFaultARM64(*this, store);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);

    StoreSourceARM64 source = materializeStoreSourceARM64(*this, store, value);

    if (address.kind == AddressForm::InstanceSymbol) {
        RELEASE_ASSERT(!address.offset && !mayFault);
        emitInstanceWordStoreARM64(*this, width, source, address.symbol);
        return;
    }

    AddressForm encodable = makeMemoryAddressEncodableARM64(*this, address, width);
    MacroAssembler::Label instruction = m_jit.labelIgnoringWatchpoints();
    if (encodable.kind == AddressForm::BaseOffset)
        emitMemoryStoreARM64(m_jit, width, source, encodable.address());
    else
        emitMemoryStoreARM64(m_jit, width, source, indexedMemoryOperandARM64(encodable));

    if (mayFault)
        recordTrappingAccess(store, address, instruction);
}

// Builder::fence() promises order among stores and nothing about loads.
void Encoder::encodeFence()
{
    m_jit.storeFence();
}

bool Encoder::tryEncodePairOfStores(const Step& first, const Step& second)
{
    ASSERT(first.kind == StepKind::Execute && second.kind == StepKind::Execute);
    const Operation* firstStore = first.operation;
    const Operation* secondStore = second.operation;
    ASSERT(firstStore->opcode() == Store && secondStore->opcode() == Store);

    Width width = firstStore->accessWidth();
    if (secondStore->accessWidth() != width || width == Width128)
        return false;
    if (memoryAccessMayFaultARM64(*this, firstStore) || memoryAccessMayFaultARM64(*this, secondStore))
        return false;

    LeafCursor firstLeaves(first);
    LeafCursor secondLeaves(second);
    std::optional<StoreSourceARM64> firstSource = tryGetStoreSourceARM64(firstStore, firstLeaves.take());
    std::optional<StoreSourceARM64> secondSource = tryGetStoreSourceARM64(secondStore, secondLeaves.take());
    if (!firstSource || !secondSource || firstSource->isNumber() != secondSource->isNumber())
        return false;

    AddressForm firstAddress = resolveAddress(firstStore, firstLeaves);
    AddressForm secondAddress = resolveAddress(secondStore, secondLeaves);
    if (firstAddress.kind != AddressForm::BaseOffset || secondAddress.kind != AddressForm::BaseOffset)
        return false;
    if (firstAddress.base != secondAddress.base)
        return false;

    int64_t distance = static_cast<int64_t>(secondAddress.offset) - static_cast<int64_t>(firstAddress.offset);
    int64_t size = bytesForWidth(width);
    if (distance != size && distance != -size)
        return false;

    bool firstIsLower = distance > 0;
    const StoreSourceARM64& lower = firstIsLower ? *firstSource : *secondSource;
    const StoreSourceARM64& upper = firstIsLower ? *secondSource : *firstSource;
    MacroAssembler::Address address = firstIsLower ? firstAddress.address() : secondAddress.address();
    if (!isStorePairOffsetEncodableARM64(width, lower.isNumber(), address.offset))
        return false;

    if (lower.isNumber()) {
        if (width == Width32)
            m_jit.storePairFloat(lower.fpr, upper.fpr, address);
        else
            m_jit.storePairDouble(lower.fpr, upper.fpr, address);
        return true;
    }

    if (width == Width32)
        m_jit.storePair32(lower.gpr, upper.gpr, address);
    else
        m_jit.storePair64(lower.gpr, upper.gpr, address);
    return true;
}

void Encoder::encodeBranchInMemory(Block* block, const Step& step)
{
    MemoryUpdate update = memoryUpdateOf(m_liveness, step.operation);
    RELEASE_ASSERT(update.kind == MemoryUpdate::TestedBehind && update.testsOverflow);
    const Block* stores = block->successor(update.successorThatStores).block();
    const Block* fails = block->successor(!update.successorThatStores).block();

    LeafCursor leaves(step);
    AddressForm address = resolveAddress(update.load, leaves);
    Location operand = leaves.take();
    RELEASE_ASSERT(address.kind != AddressForm::BaseIndex);
    bool isNamedBySymbol = address.kind == AddressForm::InstanceSymbol;

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    GPRReg word = m_jit.scratchRegister();
    if (isNamedBySymbol)
        m_relocations.loadInstanceWord(address.symbol, InstanceWordWidth::Word32, instanceGPR, word);
    else
        m_jit.load32(address.address(), word);

    bool adds = update.opcode == Add;
    MacroAssembler::Jump overflows;
    if (operand.isImmediate()) {
        RELEASE_ASSERT(update.constant <= maxConstantAddedInScratchRegister && update.constant >= -maxConstantAddedInScratchRegister);
        MacroAssembler::TrustedImm32 immediate(static_cast<int32_t>(update.constant));
        overflows = adds ? m_jit.branchAdd32(MacroAssembler::Overflow, word, immediate, word) : m_jit.branchSub32(MacroAssembler::Overflow, word, immediate, word);
    } else {
        GPRReg source = operand.gpr();
        overflows = adds ? m_jit.branchAdd32(MacroAssembler::Overflow, word, source, word) : m_jit.branchSub32(MacroAssembler::Overflow, word, source, word);
    }
    addPendingJump(overflows, fails);

    if (update.comparesWord) {
        int64_t distance = update.bound - std::numeric_limits<int32_t>::min();
        RELEASE_ASSERT(update.relation == GreaterEqual && distance > 0 && distance <= maxConstantAddedInScratchRegister);
        GPRReg difference = m_jit.getCachedMemoryTempRegisterIDAndInvalidate();
        addPendingJump(m_jit.branchSub32(MacroAssembler::Overflow, word, MacroAssembler::TrustedImm32(static_cast<int32_t>(distance)), difference), fails);
    }

    if (isNamedBySymbol)
        m_relocations.storeInstanceWord(address.symbol, InstanceWordWidth::Word32, word, instanceGPR);
    else
        m_jit.store32(word, address.address());
    if (!fallsThroughTo(block, stores))
        jumpTo(stores);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(ARM64)
