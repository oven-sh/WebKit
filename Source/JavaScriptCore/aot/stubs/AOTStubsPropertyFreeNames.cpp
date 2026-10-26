/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "JSGlobalObject.h"

namespace JSC { namespace AOT {

static_assert(exists(getFreeName(FreeNameRead::Read)) && exists(getFreeName(FreeNameRead::TypeOf)));
static_assert(exists(setFreeName(PutMode::Strict)) && exists(setFreeName(PutMode::Sloppy)));
static_assert(descriptorOf(StubFamily::GetFreeName).numberOfForms == static_cast<unsigned>(FreeNameRead::TypeOf) + 1);
static_assert(descriptorOf(StubFamily::SetFreeName).numberOfForms == static_cast<unsigned>(PutMode::Sloppy) + 1);
static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfPayload() == sizeof(uint64_t) && sizeof(Slot) == 2 * sizeof(uint64_t));

static constexpr SlotHandlerTable handlerTableFor(FreeNameRead read)
{
    switch (read) {
    case FreeNameRead::Read:
        return SlotHandlerTable::ReadFreeName;
    case FreeNameRead::TypeOf:
        return SlotHandlerTable::ReadFreeNameForTypeOf;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr SlotHandlerTable handlerTableFor(PutMode mode)
{
    return slotHandlerTable(operationFor(mode), SlotOrigin::FreeName, SlotForm::Slot);
}

static_assert(traitsOf(handlerTableFor(FreeNameRead::Read)) == SlotHandlerTableTraits { SlotOperation::Read, SlotOrigin::FreeName, SlotForm::Slot });
static_assert(traitsOf(handlerTableFor(FreeNameRead::TypeOf)) == SlotHandlerTableTraits { SlotOperation::Read, SlotOrigin::FreeNameForTypeOf, SlotForm::Slot });

// The site is the key, so no StructureID is compared.
static void jumpToSlotHandlerAtSite(StubAssembler& jit, SlotHandlerTable table, GPRReg site, GPRReg receiver)
{
    const GPRReg header = jit.allocateScratch(slotHeaderRegister);
    const GPRReg payload = jit.allocateScratch(slotPayloadRegister);
    const GPRReg kind = jit.allocateScratch(StubRegister::T10);
    RELEASE_ASSERT(receiver == gprOf(firstStubOperandRegister));
    RELEASE_ASSERT(noOverlap(site, receiver, header, payload, kind, jit.immediate()));
    jit.loadPair64(site, header, payload);
    // Every slot of a free name has a holder, so the receiver is only what an accessor is entered with.
    jit.move(TrustedImm64(JSValue::ValueUndefined), receiver);
    jumpToSlotHandler(jit, table, header, kind);
}

StubSignature signatureOfGetFreeName(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(readSiteCacheRegister) });
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

void generateGetFreeName(StubAssembler& jit)
{
    jumpToSlotHandlerAtSite(jit, handlerTableFor(static_cast<FreeNameRead>(jit.key().form())), jit.operand(0), jit.result());
}

StubSignature signatureOfSetFreeName(StubKey)
{
    StubSignature signature = StubSignature::withoutResult({ gprOf(storedValueByIdRegister), gprOf(storeSiteCacheRegister) });
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

void generateSetFreeName(StubAssembler& jit)
{
    jumpToSlotHandlerAtSite(jit, handlerTableFor(static_cast<PutMode>(jit.key().form())), jit.operand(1), jit.allocateScratch(firstStubOperandRegister));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
