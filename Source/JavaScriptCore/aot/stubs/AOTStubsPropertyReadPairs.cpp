/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTPropertyCachesFront.h"
#include "AOTStubDescriptor.h"
#include "AOTStubFrames.h"
#include "AOTStubHelpers.h"
#include "AOTStubsStringLiterals.h"
#include "AOTStubsValues.h"
#include "AOTVector.h"
#include "JSCJSValue.h"
#include <wtf/Box.h>

namespace JSC { namespace AOT {

constexpr StubRegister secondOperandRegister = StubRegister::A1;
constexpr StubRegister firstResultRegister = StubRegister::A2;
constexpr SlotHandlerTable firstReadHandlerTable = SlotHandlerTable::ReadByIdPacked;

static_assert(noOverlap(gprOf(secondOperandRegister), gprOf(firstResultRegister), gprOf(firstStubOperandRegister), gprOf(slotNameRegister), gprOf(slotHeaderRegister), gprOf(StubRegister::T10), gprOf(StubRegister::T11), gprOf(StubRegister::T13), gprOf(StubRegister::T14)));
static_assert(callerFrameOf(traitsOf(firstReadHandlerTable).origin) == CallerFrame::HasFrame);
static_assert([] {
    for (StubFamily callee : { StubFamily::GetByIdAfterFrontProbe, StubFamily::GetByIdHashed, StubFamily::ReadSlot, StubFamily::ReadSlotExit, StubFamily::SlotCannot }) {
        if (descriptorOf(callee).clobbers != StubClobbers::ClobbersOnlyScratch)
            return false;
    }
    return true;
}());

enum class SecondStep : uint8_t { Read, TestTruthiness, CompareWithLiteral };

struct SecondReadExits {
    JumpList isNotOwnInline;
    JumpList hasNoStructure;
};

static constexpr StubKey literalComparison(unsigned length)
{
    return equalsLiteralOfLength(ComparedOperand::Value, length, CallerFrame::HasFrame);
}

static void generateTwoReads(StubAssembler& jit, SecondReceiver secondReceiver, bool returnsBoth, SecondStep step, unsigned literalLength = 0)
{
    static_assert(secondNameShift == 16 && RowDisplacementTable::maxColumn < 1u << secondNameShift);
    RELEASE_ASSERT((step == SecondStep::CompareWithLiteral) == !!literalLength);
    RELEASE_ASSERT(!returnsBoth || step == SecondStep::Read);
    RELEASE_ASSERT(returnsBoth || secondReceiver == SecondReceiver::FirstResult);

    const bool readsSameReceiver = secondReceiver == SecondReceiver::FirstReceiver;
    const GPRReg R0 = jit.operand(0);
    const GPRReg first = returnsBoth ? jit.result() : R0;
    const GPRReg second = secondReceiver == SecondReceiver::Other ? jit.operand(1) : secondReceiver == SecondReceiver::FirstResult ? first : R0;
    if (step != SecondStep::Read)
        jit.clobberOperand(0);
    const GPRReg names = jit.immediate();
    const GPRReg name = jit.allocateScratch(StubRegister::T10);
    const GPRReg cell = jit.allocateScratch(slotHeaderRegister);
    const GPRReg row = jit.allocateScratch(StubRegister::T13);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    RELEASE_ASSERT(signatureOf(getByIdAfterFrontProbe()).operands[1].gpr() == cell);
    // loadStructureIDOfReceiver adds to its list in an out-of-line part, which runs after this function has returned.
    Box<SecondReadExits> secondReadExits = Box<SecondReadExits>::create();
    Box<JumpList> firstHasNoStructure = Box<JumpList>::create();

    auto emitSecondStep = [&](bool rowIsLoaded) {
        switch (step) {
        case SecondStep::CompareWithLiteral: {
            const StubKey comparison = literalComparison(literalLength);
            const StubSignature signature = signatureOf(comparison);
            const GPRReg subject = signature.operands[0].gpr();
            const GPRReg text = signature.operands[1].gpr();
            RELEASE_ASSERT(subject == T11 && text == names);
            jit.move(R0, subject);
            jit.move(jit.operand(1), text);
            jit.jumpToStub(comparison);
            return;
        }
        case SecondStep::TestTruthiness:
            RELEASE_ASSERT(signatureOf(valueIsTruthy()).operands[0].gpr() == R0);
            jit.jumpToStub(valueIsTruthy());
            return;
        case SecondStep::Read:
            jit.urshift32(TrustedImm32(secondNameShift), names);
            if (rowIsLoaded)
                jit.load64(BaseIndex(row, names, CCallHelpers::TimesEight), cell);
            else {
                loadStructureIDOfReceiver(jit, second, T11, secondReadExits->hasNoStructure, [&] {
                    JumpList isNotInFieldTable;
                    readFieldByTable(jit, { second, names, T11, cell, std::nullopt, true, true }, R0, isNotInFieldTable, [&] {
                        jit.returnToCaller();
                    });
                    isNotInFieldTable.link(&jit);
                });
                loadFrontCell(jit, SlotAccess::Read, { T11, names, row, cell, T14 });
            }
            secondReadExits->isNotOwnInline.append(branchUnlessOwnInlineValue(jit, names, cell, T14));
            loadOwnInlineValue(jit, second, cell, R0);
            jit.returnToCaller();
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    jit.zeroExtend16To32(names, name);
    loadStructureIDOfReceiver(jit, R0, T11, *firstHasNoStructure, [&] {
        JumpList isNotInFieldTable;
        readFieldByTable(jit, { R0, name, T11, cell, std::nullopt, true, true }, first, isNotInFieldTable, [&] {
            emitSecondStep(false);
        });
        isNotInFieldTable.link(&jit);
    });
    if (readsSameReceiver) {
        loadFrontRow(jit, SlotAccess::Read, T11, row, T14);
        jit.load64(BaseIndex(row, name, CCallHelpers::TimesEight), cell);
    } else
        loadFrontCell(jit, SlotAccess::Read, { T11, name, row, cell, T14 });
    Jump isNotOwnInline = branchUnlessOwnInlineValue(jit, name, cell, T14);
    loadOwnInlineValue(jit, R0, cell, first);
    emitSecondStep(readsSameReceiver);

    isNotOwnInline.link(&jit);
    Jump isGeneric = branchIfCellNameDiffers(jit, name, cell, T14);
    Jump isNotValue = branchIfSlotKindDiffers(jit, { SlotForm::PackedSlot, cell }, Slot::Kind::Value, T14);
    loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, cell, InvalidGPRReg), R0, name, T11, first);
    emitSecondStep(readsSameReceiver);

    isNotValue.link(&jit);
    loadSlotKind(jit, { SlotForm::PackedSlot, cell }, T11);
    Jump isAbsent = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(static_cast<int32_t>(Slot::Kind::Absent)));
    Jump needsFrame = jit.jump();

    isAbsent.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), first);
    emitSecondStep(readsSameReceiver);

    Vector<GPRReg, 2> saved;
    if (step == SecondStep::Read)
        saved.append(names);
    if (readsSameReceiver)
        saved.append(R0);
    // The callees are HasFrame variants: a stack walker sees the label only as the return address in a callee's own frame record.
    auto readFirstInFrame = [&](const auto& emitCall) {
        jit.emitFunctionPrologue();
        jit.preserving(saved.span(), [&] {
            jit.move(name, names);
            emitCall();
            if (step == SecondStep::Read)
                jit.bindReturnLabel(StubFrameKind::FirstOfTwoReads);
            jit.move(R0, first);
        });
        jit.emitFunctionEpilogueWithEmptyFrame();
    };

    needsFrame.link(&jit);
    readFirstInFrame([&] {
        callSlotHandler(jit, firstReadHandlerTable, T11);
    });
    Jump didRead = jit.jump();

    isGeneric.link(&jit);
    Label genericRead = jit.label();
    readFirstInFrame([&] {
        jit.callStub(getByIdHashed());
    });
    didRead.link(&jit);
    emitSecondStep(false);

    jit.addOutOfLinePart([R0, second, secondReadExits, firstHasNoStructure, genericRead](StubAssembler& jit) {
        firstHasNoStructure->linkTo(genericRead, &jit);
        auto leaveForSecondRead = [&](JumpList& jumps, StubKey stub) {
            if (second == R0) {
                jit.linkToStub(jumps, stub);
                return;
            }
            if (jumps.empty())
                return;
            jumps.link(&jit);
            jit.move(second, R0);
            jit.jumpToStub(stub);
        };
        leaveForSecondRead(secondReadExits->isNotOwnInline, getByIdAfterFrontProbe());
        leaveForSecondRead(secondReadExits->hasNoStructure, slotExit(SlotHandlerTable::ReadById, SlotExit::Miss));
    });
}

void generateGetByIdChain(StubAssembler& jit) { generateTwoReads(jit, SecondReceiver::FirstResult, false, SecondStep::Read); }
void generateGetByIdPair(StubAssembler& jit) { generateTwoReads(jit, static_cast<SecondReceiver>(jit.key().form()), true, SecondStep::Read); }
void generateGetByIdIsTruthy(StubAssembler& jit) { generateTwoReads(jit, SecondReceiver::FirstResult, false, SecondStep::TestTruthiness); }
void generateGetByIdEqualsLiteral(StubAssembler& jit) { generateTwoReads(jit, SecondReceiver::FirstResult, false, SecondStep::CompareWithLiteral, jit.key().count()); }

static StubSignature signatureOfTwoReads(StubResult result, bool hasSecondOperand)
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    if (hasSecondOperand)
        signature.addOperand(gprOf(secondOperandRegister));
    signature.immediate = gprOf(slotNameRegister);
    signature.result = result;
    return signature;
}

StubSignature signatureOfGetByIdChain(StubKey)
{
    StubSignature signature = signatureOfTwoReads(StubResult::GPR, false);
    signature.firstResult = gprOf(firstStubOperandRegister);
    return signature;
}

StubSignature signatureOfGetByIdPair(StubKey key)
{
    StubSignature signature = signatureOfTwoReads(StubResult::TwoGPRs, static_cast<SecondReceiver>(key.form()) == SecondReceiver::Other);
    signature.firstResult = gprOf(firstResultRegister);
    signature.secondResult = gprOf(firstStubOperandRegister);
    return signature;
}

StubSignature signatureOfGetByIdIsTruthy(StubKey)
{
    StubSignature signature = signatureOfTwoReads(StubResult::Flags, false);
    signature.trueCondition = signatureOf(valueIsTruthy()).trueCondition;
    return signature;
}

StubSignature signatureOfGetByIdEqualsLiteral(StubKey key)
{
    StubSignature signature = signatureOfTwoReads(StubResult::Flags, true);
    signature.trueCondition = signatureOf(literalComparison(key.count())).trueCondition;
    return signature;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
