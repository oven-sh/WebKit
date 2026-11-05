/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSweep.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses usesThatWriteIntoOperandOfHeldBackStore {
    OperandUse::WritesContents,
    OperandUse::MovesElements,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::ChangesIntegrity,
};

static constexpr OperandUses usesThatReachPastOperandsOfHeldBackStore {
    OperandUse::Calls,
    OperandUse::CallsPerElement,
    OperandUse::CallsRepeatedly,
    OperandUse::DefinesName,
    OperandUse::TakesDescriptor,
    OperandUse::ChangesPrototype,
    OperandUse::BecomesPrototype,
    OperandUse::PassesToUserCode,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

static constexpr OpcodeSet heldBackOpcodesThatWriteWordOfTheirCell { Opcode::FinishBirth, Opcode::TryFinishBirth };
static_assert(OperandIndex::FinishBirth::object == OperandIndex::TryFinishBirth::object);

static constexpr bool writesReceiverOfHeldBackStore(Access access)
{
    switch (access) {
    case Access::Store:
    case Access::Define:
        return true;
    case Access::None:
    case Access::Read:
    case Access::Has:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static std::optional<uint32_t> operandWrittenByHeldBackStore(Opcode opcode)
{
    if (heldBackOpcodesThatWriteWordOfTheirCell.contains(opcode))
        return OperandIndex::FinishBirth::object;

    if (resultRuleOf(opcode) == ResultRule::Allocation || opcode == Opcode::NewClosure)
        return std::nullopt;

    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(opcode);
    uint32_t numberOfDeclaredOperands = numberOfNamedOperands + hasVariadicOperand(opcode);
    std::optional<uint32_t> written;
    bool writesSeveralOperands = false;
    auto noteWrittenOperand = [&](uint32_t operandIndex) {
        writesSeveralOperands |= written && *written != operandIndex;
        written = operandIndex;
    };

    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (info.uses.containsAny(usesThatReachPastOperandsOfHeldBackStore) || !info.protocols.isEmpty())
            return std::nullopt;
        if (info.uses.containsAny(usesThatWriteIntoOperandOfHeldBackStore))
            noteWrittenOperand(operandIndex);
        if (!info.uses.contains(OperandUse::Retains))
            continue;
        switch (info.retainedIn) {
        case RetainedIn::UnknownMemory:
        case RetainedIn::Result:
            return std::nullopt;
        case RetainedIn::Operand:
            if (!info.retainingOperand)
                return std::nullopt;
            noteWrittenOperand(*info.retainingOperand);
            break;
        }
    }
    if (std::optional<uint8_t> receiver = receiverOf(opcode); receiver && writesReceiverOfHeldBackStore(accessOf(opcode)))
        noteWrittenOperand(*receiver);

    if (!written || writesSeveralOperands || *written >= numberOfNamedOperands)
        return std::nullopt;
    return written;
}

static bool isCallOfBuiltinWhoseStoresAreHeldBack(const BuiltinTable& builtins, const Operation& call)
{
    const BuiltinOverload& overload = builtins.at(call.overload());
    bool answersAsRowSays = call.opcode == Opcode::CallBuiltin ? call.builtinCallForm() == BuiltinCallForm::Whole : overload.code.implementation.inlineBuiltin == call.inlineBuiltin();
    if (!answersAsRowSays || call.isDoneInPlace() || !builtins.contains(overload.result.type))
        return false;
    return builtins.at(overload.result.type).freshness == BuiltinFreshness::Yes;
}

static bool isOpcodeOfAllocationWhoseStoresAreHeldBack(Opcode opcode)
{
    if (opcode == Opcode::NewClosure || effectRuleOf(opcode) != EffectRule::Declared || boundToOf(opcode) == BoundTo::Site)
        return false;

    bool keepsOperandInResult = false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (info.uses.contains(OperandUse::Returns))
            return false;
        keepsOperandInResult |= info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Result;
    }
    return resultRuleOf(opcode) == ResultRule::Allocation || keepsOperandInResult || opcode == Opcode::Data;
}

static constexpr uint8_t opcodeOfHeldBackAllocationIsNotLookedUp = 0;
static constexpr uint8_t isNoOpcodeOfHeldBackAllocation = 1;
static constexpr uint8_t isOpcodeOfHeldBackAllocation = 2;

static bool isAllocationWhoseStoresAreHeldBack(const BuiltinTable& builtins, const Operation& made, std::array<uint8_t, numberOfEnumerators<Opcode>>& answersOfSchema)
{
    EffectTraits traits = made.traits();
    if (traits.mayWriteWhatIsSeen() || traits.mayRunOtherCode())
        return false;

    Opcode opcode = made.opcode;
    if (opcode == Opcode::CallBuiltin || opcode == Opcode::CallInline)
        return isCallOfBuiltinWhoseStoresAreHeldBack(builtins, made);
    uint8_t& answerOfSchema = answersOfSchema[static_cast<unsigned>(opcode)];
    if (answerOfSchema == opcodeOfHeldBackAllocationIsNotLookedUp)
        answerOfSchema = isOpcodeOfAllocationWhoseStoresAreHeldBack(opcode) ? isOpcodeOfHeldBackAllocation : isNoOpcodeOfHeldBackAllocation;
    return answerOfSchema != isNoOpcodeOfHeldBackAllocation;
}

static constexpr uint8_t operandWrittenByHeldBackStoreIsNotLookedUp = 0;
static constexpr uint8_t noOperandIsWrittenByHeldBackStore = 1;
static constexpr uint8_t firstOperandWrittenByHeldBackStore = 2;

bool Sweep::isStoreIntoOwnObject(const Operation& operation, ValueID& object)
{
    if (effectRuleOf(operation.opcode) == EffectRule::Access && accessOf(operation.opcode) == Access::Store)
        return false;

    EffectTraits traits = operation.traits();
    if (traits.mayThrowByRule() || traits.mayBeTerminated() || traits.mayRunOtherCode() || traits.mayWriteOutsideObjects())
        return false;

    uint8_t& written = m_operandsWrittenByHeldBackStores[static_cast<unsigned>(operation.opcode)];
    if (written == operandWrittenByHeldBackStoreIsNotLookedUp) {
        std::optional<uint32_t> found = operandWrittenByHeldBackStore(operation.opcode);
        written = found ? static_cast<uint8_t>(*found + firstOperandWrittenByHeldBackStore) : noOperandIsWrittenByHeldBackStore;
    }
    if (written == noOperandIsWrittenByHeldBackStore)
        return false;
    ValueID name = operation.child(written - firstOperandWrittenByHeldBackStore);
    if (!name)
        return false;

    ValueID allocation = rootOfCellInSweptCode(*m_code, name);
    const Operation& made = m_code->at(allocation);
    if (made.blockIndex == Operation::noBlock)
        return false;
    if (!isAllocationWhoseStoresAreHeldBack(m_inputs->program.builtins(), made, m_opcodesOfAllocationsWhoseStoresAreHeldBack))
        return false;

    object = allocation;
    return true;
}

void Sweep::holdBackUntilNeeded(ValueID store, ValueID object)
{
    Code& code = *m_code;
    if (code.isNeeded(object)) {
        need(store);
        return;
    }

    if (!m_holdsBackStores) {
        m_holdsBackStores = true;
        m_heldBack.clearAll();
        m_heldBack.ensureSize(code.numberOfValues());
        m_waitingStores.fill(ValueID(), code.numberOfValues());
    }
    ASSERT(!m_heldBack.quickGet(object.index()));
    m_heldBack.quickSet(store.index());
    m_waitingStores[store.index()] = m_waitingStores[object.index()];
    m_waitingStores[object.index()] = store;
}

void Sweep::needStoresHeldBackFor(ValueID object)
{
    if (!m_holdsBackStores || m_heldBack.quickGet(object.index()))
        return;
    for (ValueID store = m_waitingStores[object.index()]; store; store = m_waitingStores[store.index()])
        need(store);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
