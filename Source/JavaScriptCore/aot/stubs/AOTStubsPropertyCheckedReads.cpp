/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTOperationData.h"
#include "AOTPropertyCachesFront.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsOperations.h"
#include "JSObject.h"
#include <wtf/Box.h>

namespace JSC { namespace AOT {

static_assert(exists(getByIdHashed().withCallerFrame(CallerFrame::StubFrame)));
static_assert(!PackedSlot::offsetOfPayload() && sizeof(PackedSlot) == sizeof(uint64_t));

static void returnNumberMarker(StubAssembler& jit, uint64_t marker, GPRReg scratch)
{
    jit.move(TrustedImm64(static_cast<int64_t>(marker)), scratch);
    jit.move64ToDouble(scratch, jit.resultFPR());
    jit.returnToCaller();
}

static void emitCheckedReadEnd(StubAssembler& jit, CheckedReadKind kind, GPRReg value, GPRReg scratch, JumpList& typeMismatch)
{
    switch (kind) {
    case CheckedReadKind::Number: {
        Jump notInt32 = jit.branchIfNotInt32(value);
        jit.convertInt32ToDouble(value, jit.resultFPR());
        jit.returnToCaller();
        notInt32.link(&jit);
        Jump notNumber = jit.branchIfNotNumber(value);
        unboxDouble(jit, value, scratch, jit.resultFPR());
        jit.returnToCaller();
        notNumber.link(&jit);
        break;
    }
    case CheckedReadKind::Boolean: {
        Jump notBoolean = jit.branchIfNotBoolean(value, scratch);
        jit.returnToCaller();
        notBoolean.link(&jit);
        break;
    }
    case CheckedReadKind::String: {
        Jump notCell = jit.branchIfNotCell(value);
        typeMismatch.append(jit.branchIfNotString(value));
        jit.returnToCaller();
        notCell.link(&jit);
        break;
    }
    case CheckedReadKind::Array: {
        Jump notCell = jit.branchIfNotCell(value);
        Jump notArrayType = jit.branch8(CCallHelpers::NotEqual, Address(value, JSCell::typeInfoTypeOffset()), TrustedImm32(ArrayType));
        jit.returnToCaller();
        notArrayType.link(&jit);
        typeMismatch.append(jit.branch8(CCallHelpers::NotEqual, Address(value, JSCell::typeInfoTypeOffset()), TrustedImm32(DerivedArrayType)));
        jit.returnToCaller();
        notCell.link(&jit);
        break;
    }
    }
    auto passNullish = [&](JSValue nullish, uint64_t marker) {
        Jump isNot = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm64(JSValue::encode(nullish)));
        if (kind == CheckedReadKind::Number)
            returnNumberMarker(jit, marker, scratch);
        else
            jit.returnToCaller();
        isNot.link(&jit);
    };
    passNullish(jsUndefined(), undefinedDouble);
    passNullish(jsNull(), nullDouble);
    typeMismatch.append(jit.jump());
}

template<typename Functor>
static void callWithinFrame(StubAssembler& jit, const Functor& emitCall)
{
    jit.emitFunctionPrologue();
    emitCall();
    jit.emitFunctionEpilogueWithEmptyFrame();
}

void generateGetByIdChecked(StubAssembler& jit)
{
    const CheckedReadKind kind = static_cast<CheckedReadKind>(jit.key().form());
    const GPRReg R0 = jit.clobberOperand(0);
    const GPRReg name = jit.immediate();
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg cell = jit.allocateScratch(slotHeaderRegister);
    const GPRReg T13 = jit.allocateScratch(StubRegister::T13);
    Box<JumpList> hasNoStructure = Box<JumpList>::create();
    JumpList typeMismatch;

    loadStructureIDOfReceiver(jit, R0, T11, *hasNoStructure, [&] {
        JumpList isNotInFieldTable;
        readFieldByTable(jit, { R0, name, T11, cell, std::nullopt, true, true }, R0, isNotInFieldTable, [&] {
            emitCheckedReadEnd(jit, kind, R0, T11, typeMismatch);
        });
        isNotInFieldTable.link(&jit);
    });
    loadFrontCell(jit, SlotAccess::Read, { T11, name, T13, cell, T10 });
    Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, name, cell, T10);
    loadOwnInlineValue(jit, R0, cell, R0);
    emitCheckedReadEnd(jit, kind, R0, T11, typeMismatch);

    isNotOwnInlineValue.link(&jit);
    Jump hasOtherName = branchIfCellNameDiffers(jit, name, cell, T10);
    loadSlotKind(jit, { SlotForm::PackedSlot, cell }, T10);
    Jump isAbsent = jit.branch32(CCallHelpers::Equal, T10, TrustedImm32(static_cast<int32_t>(Slot::Kind::Absent)));
    callWithinFrame(jit, [&] {
        callSlotHandler(jit, SlotHandlerTable::ReadByIdPackedInStubFrame, T10);
    });
    emitCheckedReadEnd(jit, kind, R0, T11, typeMismatch);

    isAbsent.link(&jit);
    if (kind == CheckedReadKind::Number)
        returnNumberMarker(jit, undefinedDouble, T11);
    else {
        jit.move(TrustedImm64(JSValue::ValueUndefined), R0);
        jit.returnToCaller();
    }

    hasOtherName.link(&jit);
    callWithinFrame(jit, [&] {
        jit.callStub(getByIdHashed().withCallerFrame(CallerFrame::StubFrame));
    });
    Label isRead = jit.label();
    emitCheckedReadEnd(jit, kind, R0, T11, typeMismatch);

    typeMismatch.link(&jit);
#if CPU(X86_64)
    // ThrowAtFault takes the return address in this register, where the fault handler puts it.
    jit.pop(CCallHelpers::s_scratchRegister);
#endif
    jit.jumpToStub(throwAtSite(ThrowSiteSource::FaultSite, CallerFrame::HasFrame));

    // loadStructureIDOfReceiver adds to its list in an out-of-line part, which runs after this function has returned.
    jit.addOutOfLinePart([hasNoStructure, isRead](StubAssembler& jit) {
        hasNoStructure->link(&jit);
        callWithinFrame(jit, [&] {
            jit.callStub(getById().withCallerFrame(CallerFrame::StubFrame));
        });
        jit.jump().linkTo(isRead, &jit);
    });
}

static StubSignature signatureReadingById()
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::R0) });
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

StubSignature signatureOfGetByIdChecked(StubKey key)
{
    StubSignature signature = signatureReadingById();
    switch (static_cast<CheckedReadKind>(key.form())) {
    case CheckedReadKind::Number:
        signature.result = StubResult::FPR;
        signature.firstResult = FPRInfo::fpRegT0;
        return signature;
    case CheckedReadKind::Boolean:
    case CheckedReadKind::String:
    case CheckedReadKind::Array:
        return signature;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
