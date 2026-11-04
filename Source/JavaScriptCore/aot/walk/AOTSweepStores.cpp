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

// Each clears a mark in the cell that its operand names. The schema files that under RuntimeState, so no use of the operand says that it is written.
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

// The one named operand that the operation writes into, where the roles of its operands let it reach no other object.
// Which heaps it writes lies with the operands it has: the traits of the record say that. They do not say whose objects a function writes that a protocol has been found to call.
static std::optional<uint32_t> operandWrittenByHeldBackStore(Opcode opcode)
{
    if (heldBackOpcodesThatWriteWordOfTheirCell.contains(opcode))
        return OperandIndex::FinishBirth::object;

    // One array links an allocation to its stores and the stores to each other, so no record may be both.
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

// The row of the overload says that each call makes what it answers with. Where a species may make it instead, the traits do not say whose function that is.
static bool isCallOfBuiltinWhoseStoresAreHeldBack(const BuiltinTable& builtins, const Operation& call)
{
    const BuiltinOverload& overload = builtins.at(call.overload());
    bool answersAsRowSays = call.opcode == Opcode::CallBuiltin ? call.builtinCallForm() == BuiltinCallForm::Whole : overload.code.implementation.inlineBuiltin == call.inlineBuiltin();
    // Done in place, it answers with the cell of its receiver.
    if (!answersAsRowSays || call.isDoneInPlace() || !builtins.contains(overload.result.type))
        return false;
    return builtins.at(overload.result.type).freshness == BuiltinFreshness::Yes;
}

// When it has run, no memory and no other value names its result.
// None of these is a store for operandWrittenByHeldBackStore(): an operand that is kept in the result or called ends the question there, and Data has no operand.
static bool isAllocationWhoseStoresAreHeldBack(const BuiltinTable& builtins, const Operation& made)
{
    EffectTraits traits = made.traits();
    if (traits.mayWriteWhatIsSeen() || traits.mayRunOtherCode())
        return false;

    Opcode opcode = made.opcode;
    if (opcode == Opcode::CallBuiltin || opcode == Opcode::CallInline)
        return isCallOfBuiltinWhoseStoresAreHeldBack(builtins, made);
    // A closure is asked about by itself: isClosureWhoseStoresAreHeldBack().
    if (opcode == Opcode::NewClosure || effectRuleOf(opcode) != EffectRule::Declared || boundToOf(opcode) == BoundTo::Site)
        return false;
    // Every evaluation hands out the one cell of the place.
    if ((opcode == Opcode::Data || opcode == Opcode::NewRegExp) && made.isNeverToldApart())
        return false;

    bool keepsOperandInResult = false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (info.uses.contains(OperandUse::Returns))
            return false;
        keepsOperandInResult |= info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Result;
    }
    // Whatever keeps an operand in its result has made the result.
    return resultRuleOf(opcode) == ResultRule::Allocation || keepsOperandInResult || opcode == Opcode::Data;
}

// The same of a closure, whose rule is that of a constant. The closure table hands its one cell to whoever reads the function elsewhere, and a closure on demand is the mark in a slot whose first read makes the cell.
static bool isClosureWhoseStoresAreHeldBack(const Operation& made)
{
    if (made.opcode != Opcode::NewClosure || made.isHeldByClosureTable() || made.isMadeOnDemand())
        return false;
    switch (made.closureForm()) {
    case ClosureForm::Full:
    case ClosureForm::Slim:
        break;
    case ClosureForm::Deferred:
    case ClosureForm::Singleton:
        return false;
    }

    EffectTraits traits = made.traits();
    return !traits.mayWriteWhatIsSeen() && !traits.mayRunOtherCode();
}

static constexpr uint8_t operandWrittenByHeldBackStoreIsNotLookedUp = 0;
static constexpr uint8_t noOperandIsWrittenByHeldBackStore = 1;
static constexpr uint8_t firstOperandWrittenByHeldBackStore = 2;

bool Sweep::isStoreIntoOwnObject(const Operation& operation, ValueID& object)
{
    // The effects of such a store are those of a definition, which may be refused. The walk has found that this one is not.
    bool isPlainStoreToClosure = operation.opcode == Opcode::PropSet && operation.isPlainStoreToClosure();

    // A store by name or by key may end in a setter, which writes into objects of its own choice. The traits do not tell those from the receiver.
    // A definition looks for no setter. A store by key goes out in operandWrittenByHeldBackStore(), for the protocol on its key.
    if (effectRuleOf(operation.opcode) == EffectRule::Access && accessOf(operation.opcode) == Access::Store && !isPlainStoreToClosure)
        return false;

    // An access that tests its own guess has the traits of the generic operation on its failing side, and goes out here.
    EffectTraits traits = operation.traits();
    if ((traits.mayThrowByRule() && !isPlainStoreToClosure) || traits.mayBeTerminated() || traits.mayRunOtherCode() || traits.mayWriteOutsideObjects())
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

    // Whatever needs another name of the object needs the allocation through it. A store through a parameter of a block stays a root.
    // A constant bears no traits to go by.
    ValueID allocation = rootOfCellInSweptCode(*m_code, name);
    if (m_code->isConstant(allocation))
        return false;
    // Of all that writes into a closure, only what the walk has marked waits for it.
    const Operation& made = m_code->at(allocation);
    if (isPlainStoreToClosure ? !isClosureWhoseStoresAreHeldBack(made) : !isAllocationWhoseStoresAreHeldBack(m_inputs->program.builtins(), made))
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

// `object` has left the worklist: whatever is held back for it from here on finds it needed.
void Sweep::needStoresHeldBackFor(ValueID object)
{
    if (!m_holdsBackStores || m_heldBack.quickGet(object.index()))
        return;
    for (ValueID store = m_waitingStores[object.index()]; store; store = m_waitingStores[store.index()])
        need(store);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
