/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"
#include "AOTInliningAcrossFiles.h"

#if ENABLE(AOT) && CPU(X86_64)

#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Machine {

static bool loadOrStoreMayFault(const Encoder& encoder, const Operation* access)
{
    return access->traps() || encoder.factsOf(access).has(ValueFlag::TakesOverTrap);
}

static bool loadOrStoreMovesNumber(const Operation* access)
{
    Type type = isStore(access->opcode()) ? access->child(0)->type() : access->type();
    return type.isInNumberRegister();
}

template<typename Functor>
static ALWAYS_INLINE void withOperandOfLoadOrStore(const AddressForm& form, const Functor& functor)
{
    static_assert(!MacroAssembler::TimesOne && MacroAssembler::TimesTwo == 1 && MacroAssembler::TimesFour == 2 && MacroAssembler::TimesEight == 3);
    switch (form.kind) {
    case AddressForm::BaseOffset:
        functor(form.address());
        return;
    case AddressForm::BaseIndex:
        RELEASE_ASSERT(form.extension == IndexExtension::None && form.shift <= MacroAssembler::TimesEight);
        functor(MacroAssembler::BaseIndex(form.base, form.index, static_cast<MacroAssembler::Scale>(form.shift), form.offset));
        return;
    case AddressForm::InstanceSymbol:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool onlyFaultsOnEmptyPointer(const Encoder& encoder, const Operation* load, const AddressForm& form)
{
    return load->opcode() == Load8Z && load->traps() && encoder.factsOf(load).lastUse == noPosition && form.kind == AddressForm::BaseOffset && form.offset >= 0 && form.offset < static_cast<int32_t>(sizeof(uint64_t));
}

static ALWAYS_INLINE void emitX86Load(Encoder& encoder, const Operation* load, const AddressForm& form, Reg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    ASSERT(isLoad(load->opcode()));
    Type type = load->type();
    bool isNumber = type.isInNumberRegister();
    Width width = accessWidthOf(load->opcode(), type);
    ASSERT(destination.isFPR() == isNumber);
    if (onlyFaultsOnEmptyPointer(encoder, load, form)) {
        jit.load32(MacroAssembler::Address(form.base), destination.gpr());
        return;
    }
    withOperandOfLoadOrStore(form, [&](auto address) ALWAYS_INLINE_LAMBDA {
        switch (width) {
        case Width8:
            ASSERT(load->type() == Int32);
            if (load->opcode() == Load8S)
                jit.load8SignedExtendTo32(address, destination.gpr());
            else
                jit.load8(address, destination.gpr());
            return;
        case Width16:
            ASSERT(load->type() == Int32);
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
    });
}

static ALWAYS_INLINE void emitX86StoreOfRegister(Encoder& encoder, const Operation* store, Reg source, const AddressForm& form)
{
    CCallHelpers& jit = encoder.m_jit;
    ASSERT(isStore(store->opcode()));
    Type type = inlineChildWithoutCheck(store, 0)->type();
    bool isNumber = type.isInNumberRegister();
    Width width = accessWidthOf(store->opcode(), type);
    ASSERT(source.isFPR() == isNumber);
    withOperandOfLoadOrStore(form, [&](auto address) ALWAYS_INLINE_LAMBDA {
        switch (width) {
        case Width8:
            jit.store8(source.gpr(), address);
            return;
        case Width16:
            jit.store16(source.gpr(), address);
            return;
        case Width32:
            if (isNumber)
                jit.storeFloat(source.fpr(), address);
            else
                jit.store32(source.gpr(), address);
            return;
        case Width64:
            if (isNumber)
                jit.storeDouble(source.fpr(), address);
            else
                jit.store64(source.gpr(), address);
            return;
        case Width128:
            jit.storeVector(source.fpr(), address);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static void emitX86StoreOfImmediate(Encoder& encoder, const Operation* store, const AddressForm& form)
{
    CCallHelpers& jit = encoder.m_jit;
    const Operation* constant = store->child(0);
    RELEASE_ASSERT(constant->isConstant());
    int64_t bits = constant->constantBits();
    Width width = store->accessWidth();
    RELEASE_ASSERT(width != Width64 || isRepresentableAs<int32_t>(bits));
    MacroAssembler::TrustedImm32 immediate(static_cast<int32_t>(bits));
    withOperandOfLoadOrStore(form, [&](auto address) {
        switch (width) {
        case Width8:
            jit.store8(immediate, address);
            return;
        case Width16:
            jit.store16(immediate, address);
            return;
        case Width32:
            jit.store32(immediate, address);
            return;
        case Width64:
            jit.store64(immediate, address);
            return;
        case Width128:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static ALWAYS_INLINE void emitX86Store(Encoder& encoder, const Operation* store, Location value, const AddressForm& form)
{
    switch (value.kind()) {
    case Location::Register:
        emitX86StoreOfRegister(encoder, store, value.reg(), form);
        return;
    case Location::Immediate:
        emitX86StoreOfImmediate(encoder, store, form);
        return;
    case Location::None:
    case Location::Spill:
    case Location::SlotBytes:
    case Location::Outgoing:
    case Location::InstanceWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<InstanceWordWidth> instanceWordWidthOfLoadOrStore(Width width)
{
    switch (width) {
    case Width8:
        return InstanceWordWidth::Byte;
    case Width32:
        return InstanceWordWidth::Word32;
    case Width64:
        return InstanceWordWidth::Word64;
    case Width16:
    case Width128:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool tryLoadInstanceWordInOneInstruction(Encoder& encoder, const Operation* load, CodeSymbol symbol, Reg destination)
{
    Width width = load->accessWidth();
    if (loadOrStoreMovesNumber(load)) {
        if (width != Width64)
            return false;
        encoder.m_relocations.loadInstanceDouble(symbol, instanceGPR, destination.fpr());
        return true;
    }
    if (load->opcode() == Load8S)
        return false;
    if (width == Width16) {
        return encoder.m_relocations.withInstanceWordAsOperand(symbol, instanceGPR, [&](MacroAssembler::Address word) {
            AddressForm form;
            form.base = word.base;
            form.offset = word.offset;
            emitX86Load(encoder, load, form, destination);
            return true;
        });
    }
    std::optional<InstanceWordWidth> widthOfWord = instanceWordWidthOfLoadOrStore(width);
    if (!widthOfWord)
        return false;
    encoder.m_relocations.loadInstanceWord(symbol, *widthOfWord, instanceGPR, destination.gpr());
    return true;
}

static bool tryStoreInstanceWordInOneInstruction(Encoder& encoder, const Operation* store, Location value, CodeSymbol symbol)
{
    if (value.isImmediate()) {
        return encoder.m_relocations.withInstanceWordAsOperand(symbol, instanceGPR, [&](MacroAssembler::Address word) {
            AddressForm form;
            form.base = word.base;
            form.offset = word.offset;
            emitX86StoreOfImmediate(encoder, store, form);
            return true;
        });
    }
    if (!value.isRegister())
        return false;
    Width width = store->accessWidth();
    if (loadOrStoreMovesNumber(store)) {
        if (width != Width64)
            return false;
        encoder.m_relocations.storeInstanceDouble(symbol, value.fpr(), instanceGPR);
        return true;
    }
    std::optional<InstanceWordWidth> widthOfWord = instanceWordWidthOfLoadOrStore(width);
    if (!widthOfWord)
        return false;
    encoder.m_relocations.storeInstanceWord(symbol, *widthOfWord, value.gpr(), instanceGPR);
    return true;
}

static AddressForm moveAddressOfInstanceWordToScratch(Encoder& encoder, CodeSymbol symbol)
{
    encoder.m_relocations.moveInstanceAddress(symbol, instanceGPR, scratchGPR);
    AddressForm form;
    form.base = scratchGPR;
    return form;
}

static MacroAssembler::Address addressOfX86WordInMemory(Encoder& encoder, const AddressForm& form)
{
    if (form.kind == AddressForm::InstanceSymbol) {
        RELEASE_ASSERT(!form.offset);
        return moveAddressOfInstanceWordToScratch(encoder, form.symbol).address();
    }
    RELEASE_ASSERT(form.kind == AddressForm::BaseOffset);
    return form.address();
}

static MacroAssembler::RelationalCondition conditionOfX86WordInMemory(Opcode relation)
{
    if (relation == Equal)
        return MacroAssembler::Equal;
    if (relation == NotEqual)
        return MacroAssembler::NotEqual;
    if (relation == LessThan)
        return MacroAssembler::LessThan;
    if (relation == GreaterThan)
        return MacroAssembler::GreaterThan;
    if (relation == LessEqual)
        return MacroAssembler::LessThanOrEqual;
    if (relation == GreaterEqual)
        return MacroAssembler::GreaterThanOrEqual;
    if (relation == Above)
        return MacroAssembler::Above;
    if (relation == Below)
        return MacroAssembler::Below;
    if (relation == AboveEqual)
        return MacroAssembler::AboveOrEqual;
    RELEASE_ASSERT(relation == BelowEqual);
    return MacroAssembler::BelowOrEqual;
}

template<typename Source>
static void applyX86InstructionToWordInMemory(CCallHelpers& jit, Opcode opcode, Width width, Source source, MacroAssembler::Address word)
{
    RELEASE_ASSERT(width == Width32 || width == Width64);
    bool is32 = width == Width32;
    if (opcode == Add) {
        if (is32)
            jit.add32(source, word);
        else
            jit.add64(source, word);
        return;
    }
    if (opcode == Sub) {
        if (is32)
            jit.sub32(source, word);
        else
            jit.sub64(source, word);
        return;
    }
    if (opcode == BitAnd) {
        if (is32)
            jit.and32(source, word);
        else
            jit.and64(source, word);
        return;
    }
    if (opcode == BitOr) {
        if (is32)
            jit.or32(source, word);
        else
            jit.or64(source, word);
        return;
    }
    RELEASE_ASSERT(opcode == BitXor);
    if (is32)
        jit.xor32(source, word);
    else
        jit.xor64(source, word);
}

static void addToX86WordInMemory(CCallHelpers& jit, bool adds, Location operand, int64_t constant, MacroAssembler::Address word)
{
    if (operand.isImmediate()) {
        RELEASE_ASSERT(isRepresentableAs<int32_t>(constant));
        int32_t immediate = static_cast<int32_t>(constant);
        if (adds)
            jit.m_assembler.addl_im(immediate, word.offset, word.base);
        else
            jit.m_assembler.subl_im(immediate, word.offset, word.base);
        return;
    }
    if (adds)
        jit.m_assembler.addl_rm(operand.gpr(), word.offset, word.base);
    else
        jit.m_assembler.subl_rm(operand.gpr(), word.offset, word.base);
}

static MacroAssembler::Address addressOfX86ConstantInInstance(int64_t constant)
{
    std::optional<PinnedConstant> pinned = findPinnedConstant(static_cast<uint64_t>(constant));
    RELEASE_ASSERT(pinned && pinned->offset);
    return MacroAssembler::Address(pinned->reg, *pinned->offset);
}

static void addX86ConstantToScratch(CCallHelpers& jit, bool adds, int64_t constant)
{
    if (isRepresentableAs<int32_t>(constant)) {
        MacroAssembler::TrustedImm32 immediate(static_cast<int32_t>(constant));
        if (adds)
            jit.add64(immediate, scratchGPR);
        else
            jit.sub64(immediate, scratchGPR);
        return;
    }
    if (adds)
        jit.add64(addressOfX86ConstantInInstance(constant), scratchGPR);
    else
        jit.sub64(addressOfX86ConstantInInstance(constant), scratchGPR);
}

static MacroAssembler::Jump storeX86SumOfWholeWords(CCallHelpers& jit, const MemoryUpdate& update, Location operand, MacroAssembler::Address word)
{
    RELEASE_ASSERT(word.base != scratchGPR);
    bool adds = update.opcode == Add;
    if (operand.isImmediate()) {
        jit.load64(word, scratchGPR);
        addX86ConstantToScratch(jit, adds, update.constant);
    } else {
        jit.zeroExtend32ToWord(operand.gpr(), scratchGPR);
        if (!adds)
            jit.neg64(scratchGPR);
        jit.add64(word, scratchGPR);
    }

    if (update.bias)
        addX86ConstantToScratch(jit, true, update.bias);
    MacroAssembler::RelationalCondition failing = MacroAssembler::invert(conditionOfX86WordInMemory(update.relation));
    MacroAssembler::Jump fails = isRepresentableAs<int32_t>(update.bound)
        ? jit.branch64(failing, scratchGPR, MacroAssembler::TrustedImm32(static_cast<int32_t>(update.bound)))
        : jit.branch64(failing, scratchGPR, addressOfX86ConstantInInstance(update.bound));
    if (update.bias)
        addX86ConstantToScratch(jit, false, update.bias);
    jit.store64(scratchGPR, word);
    return fails;
}

template<typename Functor>
static void addX86LatePathOfBranchInMemory(Encoder& encoder, const Functor& functor)
{
    encoder.addLatePath(encoder.m_arena.create<LatePathOf<Functor>>(functor));
}

static void encodeX86StoreInMemory(Encoder& encoder, const Step& step)
{
    CCallHelpers& jit = encoder.m_jit;
    const Operation* store = step.operation;
    MemoryUpdate update = memoryUpdateOf(encoder.m_liveness, store);
    RELEASE_ASSERT(update.kind == MemoryUpdate::InPlace);

    AllowMacroScratchRegisterUsage allowScratch(jit);
    LeafCursor leaves(step);
    AddressForm form = encoder.resolveAddress(update.load, leaves);
    Location operand = leaves.take();
    MacroAssembler::Address word = addressOfX86WordInMemory(encoder, form);
    Width width = store->accessWidth();

    MacroAssembler::Label instruction = jit.label();
    if (operand.isImmediate()) {
        RELEASE_ASSERT(isRepresentableAs<int32_t>(update.constant));
        applyX86InstructionToWordInMemory(jit, update.opcode, width, MacroAssembler::TrustedImm32(static_cast<int32_t>(update.constant)), word);
    } else
        applyX86InstructionToWordInMemory(jit, update.opcode, width, operand.gpr(), word);
    if (loadOrStoreMayFault(encoder, store))
        encoder.recordTrappingAccess(store, form, instruction);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Encoder::encodeLoad(const Step& step)
{
    const Operation* load = step.operation;
    ASSERT(isLoad(load->opcode()) && !m_liveness.isFused(load));
    LeafCursor leaves(step);
    AddressForm form = resolveAddress(load, leaves);
    RELEASE_ASSERT(step.destination.isRegister());
    Reg destination = step.destination.reg();

    if (form.kind == AddressForm::InstanceSymbol) {
        RELEASE_ASSERT(!form.offset);
        ASSERT(!loadOrStoreMayFault(*this, load));
        if (tryLoadInstanceWordInOneInstruction(*this, load, form.symbol, destination))
            return;
        AllowMacroScratchRegisterUsage allowScratch(m_jit);
        AddressForm formInScratch = moveAddressOfInstanceWordToScratch(*this, form.symbol);
        emitX86Load(*this, load, formInScratch, destination);
        return;
    }

    MacroAssembler::Label instruction = m_jit.label();
    emitX86Load(*this, load, form, destination);
    if (loadOrStoreMayFault(*this, load))
        recordTrappingAccess(load, form, instruction);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Encoder::encodeStore(const Step& step)
{
    const Operation* store = step.operation;
    ASSERT(isStore(store->opcode()) && (!m_liveness.isFused(store->child(0)) || store->child(0)->opcode() == Trunc || namesWord(store->child(0))));
    if (factsOf(store).has(ValueFlag::WorksInMemory)) {
        encodeX86StoreInMemory(*this, step);
        return;
    }

    LeafCursor leaves(step);
    Location value = leaves.take();
    AddressForm form = resolveAddress(store, leaves);

    if (form.kind == AddressForm::InstanceSymbol) {
        RELEASE_ASSERT(!form.offset);
        ASSERT(!loadOrStoreMayFault(*this, store));
        if (tryStoreInstanceWordInOneInstruction(*this, store, value, form.symbol))
            return;
        ASSERT(!value.isRegister() || value.reg() != Reg(scratchGPR));
        AllowMacroScratchRegisterUsage allowScratch(m_jit);
        AddressForm formInScratch = moveAddressOfInstanceWordToScratch(*this, form.symbol);
        emitX86Store(*this, store, value, formInScratch);
        return;
    }

    MacroAssembler::Label instruction = m_jit.label();
    emitX86Store(*this, store, value, form);
    if (loadOrStoreMayFault(*this, store))
        recordTrappingAccess(store, form, instruction);
}

void Encoder::encodeFence()
{
    m_jit.storeFence();
}


void Encoder::encodeBranchInMemory(Block* block, const Step& step)
{
    const Operation* branch = step.operation;
    MemoryUpdate update = memoryUpdateOf(m_liveness, branch);
    const Block* stores = block->successor(update.successorThatStores).block();
    const Block* fails = block->successor(!update.successorThatStores).block();
    MacroAssembler::Label* labelOfStores = labelOf(stores);
    MacroAssembler::Label* labelOfFails = labelOf(fails);
    m_mayEndLoop = true;

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    LeafCursor leaves(step);
    AddressForm form = resolveAddress(update.load, leaves);
    Location operand = leaves.take();
    RELEASE_ASSERT(operand.isRegister() || operand.isImmediate());
    MacroAssembler::Address word = addressOfX86WordInMemory(*this, form);
    bool adds = update.opcode == Add;
    int64_t constant = update.constant;
    Encoder* encoder = this;

    switch (update.kind) {
    case MemoryUpdate::InPlace:
        break;

    case MemoryUpdate::ComparedAhead: {
        MacroAssembler::RelationalCondition condition = conditionOfX86WordInMemory(update.relation);
        MacroAssembler::TrustedImm32 bound(static_cast<int32_t>(update.bound));
        bool jumpsWhereItStores = fallsThroughTo(block, fails);
        if (!jumpsWhereItStores)
            condition = MacroAssembler::invert(condition);
        addPendingJump(update.load->type() == Int32 ? m_jit.branch32(condition, word, bound) : m_jit.branch64(condition, word, bound), jumpsWhereItStores ? stores : fails);
        if (!jumpsWhereItStores && !fallsThroughTo(block, stores))
            jumpTo(stores);
        return;
    }

    case MemoryUpdate::TestedBehind: {
        addToX86WordInMemory(m_jit, adds, operand, constant, word);
        MacroAssembler::Jump overflows;
        MacroAssembler::Jump failsComparison;
        if (update.testsOverflow)
            overflows = MacroAssembler::Jump(m_jit.m_assembler.jCC(X86Assembler::ConditionO));
        if (update.comparesWord)
            failsComparison = m_jit.branch32(MacroAssembler::invert(conditionOfX86WordInMemory(update.relation)), word, MacroAssembler::TrustedImm32(static_cast<int32_t>(update.bound)));
        if (!fallsThroughTo(block, stores))
            jumpTo(stores);

        addX86LatePathOfBranchInMemory(*this, [encoder, form, operand, constant, adds, overflows, failsComparison, labelOfFails](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratchOnLatePath(jit);
            if (overflows.isSet())
                overflows.link(&jit);
            if (failsComparison.isSet())
                failsComparison.link(&jit);
            addToX86WordInMemory(jit, !adds, operand, constant, addressOfX86WordInMemory(*encoder, form));
            jit.jump().linkTo(*labelOfFails, &jit);
        });
        return;
    }

    case MemoryUpdate::LowHalf: {
        if (m_liveness.of(branch).has(ValueFlag::AddsToWholeWord)) {
            addPendingJump(storeX86SumOfWholeWords(m_jit, update, operand, word), fails);
            if (!fallsThroughTo(block, stores))
                jumpTo(stores);
            return;
        }

        addToX86WordInMemory(m_jit, adds, operand, constant, word);
        MacroAssembler::Jump leavesLowHalf(m_jit.m_assembler.jCC(X86Assembler::ConditionC));
        if (!fallsThroughTo(block, stores))
            jumpTo(stores);

        addX86LatePathOfBranchInMemory(*this, [update, word, operand, constant, adds, leavesLowHalf, labelOfStores, labelOfFails](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratchOnLatePath(jit);
            leavesLowHalf.link(&jit);
            addToX86WordInMemory(jit, !adds, operand, constant, word);
            storeX86SumOfWholeWords(jit, update, operand, word).linkTo(*labelOfFails, &jit);
            jit.jump().linkTo(*labelOfStores, &jit);
        });
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
