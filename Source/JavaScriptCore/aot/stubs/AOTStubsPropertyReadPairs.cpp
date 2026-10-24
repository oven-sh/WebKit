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

struct TwoReads {
    SecondReceiver secondReceiver;
    bool returnsBoth;
    SecondStep step;
    unsigned literalLength;
};

struct SecondReadExits {
    JumpList isNotOwnInline;
    JumpList hasNoStructure;
};

struct TwoReadsBody {
    bool readsSameReceiver() const { return reads.secondReceiver == SecondReceiver::FirstReceiver; }

    StubAssembler& jit;
    TwoReads reads;
    GPRReg R0;
    GPRReg first;
    GPRReg second;
    GPRReg names;
    GPRReg name;
    GPRReg cell;
    GPRReg row;
    GPRReg T11;
    GPRReg T14;
    Box<SecondReadExits> secondReadExits;
};

struct FirstProbeExits {
    Box<JumpList> hasNoStructure;
    JumpList isGeneric;
    Jump isAbsent;
    Jump needsFrame;
    Label genericRead;
};

static constexpr StubKey literalComparison(unsigned length)
{
    return equalsLiteralOfLength(ComparedOperand::Value, length, CallerFrame::HasFrame);
}

static StubSignature calleeSignature(StubKey callee)
{
    return signatureOf(callee);
}

static void emitSecondRead(const TwoReadsBody& body, bool rowIsLoaded)
{
    StubAssembler& jit = body.jit;
    const GPRReg T11 = body.T11;
    const GPRReg T14 = body.T14;
    const GPRReg second = body.second;
    const GPRReg names = body.names;
    const GPRReg cell = body.cell;

    jit.urshift32(TrustedImm32(secondNameShift), names);
    if (rowIsLoaded)
        jit.load64(BaseIndex(body.row, names, CCallHelpers::TimesEight), cell);
    else {
        loadStructureIDOfReceiver(jit, second, T11, body.secondReadExits->hasNoStructure, [&] {
            JumpList isNotInFieldTable;
            readFieldByTable(jit, { second, names, T11, cell, std::nullopt, true, true }, body.R0, isNotInFieldTable, [&] {
                jit.returnToCaller();
            });
            isNotInFieldTable.link(&jit);
        });
        loadFrontCell(jit, SlotAccess::Read, { T11, names, body.row, cell, T14 });
    }
    body.secondReadExits->isNotOwnInline.append(branchUnlessOwnInlineValue(jit, names, cell, T14));
    loadOwnInlineValue(jit, second, cell, body.R0);
    jit.returnToCaller();
}

static void emitSecondStep(const TwoReadsBody& body, bool rowIsLoaded)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.R0;

    switch (body.reads.step) {
    case SecondStep::CompareWithLiteral: {
        const StubKey comparison = literalComparison(body.reads.literalLength);
        const StubSignature signature = calleeSignature(comparison);
        const GPRReg subject = signature.operands[0].gpr();
        const GPRReg text = signature.operands[1].gpr();
        RELEASE_ASSERT(subject == body.T11 && text == body.names);
        jit.move(R0, subject);
        jit.move(jit.operand(1), text);
        jit.jumpToStub(comparison);
        return;
    }
    case SecondStep::TestTruthiness:
        RELEASE_ASSERT(calleeSignature(valueIsTruthy()).operands[0].gpr() == R0);
        jit.jumpToStub(valueIsTruthy());
        return;
    case SecondStep::Read:
        emitSecondRead(body, rowIsLoaded);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitFirstProbe(const TwoReadsBody& body, FirstProbeExits& exits)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.R0;
    const GPRReg T11 = body.T11;
    const GPRReg T14 = body.T14;
    const GPRReg first = body.first;
    const GPRReg name = body.name;
    const GPRReg cell = body.cell;
    const bool readsSameReceiver = body.readsSameReceiver();

    if (readsSameReceiver) {
        loadFrontRow(jit, SlotAccess::Read, T11, body.row, T14);
        jit.load64(BaseIndex(body.row, name, CCallHelpers::TimesEight), cell);
    } else
        loadFrontCell(jit, SlotAccess::Read, { T11, name, body.row, cell, T14 });
    Jump isNotOwnInline = branchUnlessOwnInlineValue(jit, name, cell, T14);
    loadOwnInlineValue(jit, R0, cell, first);
    emitSecondStep(body, readsSameReceiver);

    isNotOwnInline.link(&jit);
    exits.isGeneric.append(branchIfCellNameDiffers(jit, name, cell, T14));
    Jump isNotValue = branchIfSlotKindDiffers(jit, { SlotForm::PackedSlot, cell }, Slot::Kind::Value, T14);
    loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, cell, InvalidGPRReg), R0, name, T11, first);
    emitSecondStep(body, readsSameReceiver);

    isNotValue.link(&jit);
    loadSlotKind(jit, { SlotForm::PackedSlot, cell }, T11);
    exits.isAbsent = jit.branch32(CCallHelpers::Equal, T11, TrustedImm32(static_cast<int32_t>(Slot::Kind::Absent)));
    exits.needsFrame = jit.jump();
}

static void emitFirstReadInFrame(const TwoReadsBody& body, FirstProbeExits& exits)
{
    StubAssembler& jit = body.jit;
    const TwoReads& reads = body.reads;
    const GPRReg R0 = body.R0;

    Vector<GPRReg, 2> saved;
    if (reads.step == SecondStep::Read)
        saved.append(body.names);
    if (body.readsSameReceiver())
        saved.append(R0);

    // The callees are HasFrame variants: a stack walker sees the label only as the return address in a callee's own frame record.
    auto readFirstInFrame = [&](const auto& emitCall) {
        jit.emitFunctionPrologue();
        jit.preserving(saved.span(), [&] {
            jit.move(body.name, body.names);
            emitCall();
            if (reads.step == SecondStep::Read)
                jit.bindReturnLabel(StubFrameKind::FirstOfTwoReads);
            jit.move(R0, body.first);
        });
        jit.emitFunctionEpilogueWithEmptyFrame();
    };

    exits.needsFrame.link(&jit);
    readFirstInFrame([&] {
        callSlotHandler(jit, firstReadHandlerTable, body.T11);
    });
    Jump didRead = jit.jump();

    exits.isGeneric.link(&jit);
    exits.genericRead = jit.label();
    readFirstInFrame([&] {
        jit.callStub(getByIdHashed());
    });
    didRead.link(&jit);
    emitSecondStep(body, false);
}

static void emitSecondReadExit(const TwoReadsBody& body, JumpList& jumps, StubKey stub)
{
    StubAssembler& jit = body.jit;

    if (body.second == body.R0) {
        jit.linkToStub(jumps, stub);
        return;
    }
    if (jumps.empty())
        return;

    jumps.link(&jit);
    jit.move(body.second, body.R0);
    jit.jumpToStub(stub);
}

static void generateTwoReads(StubAssembler& jit, const TwoReads& reads)
{
    static_assert(secondNameShift == 16 && RowDisplacementTable::maxColumn < 1u << secondNameShift);
    RELEASE_ASSERT((reads.step == SecondStep::CompareWithLiteral) == !!reads.literalLength);
    RELEASE_ASSERT(!reads.returnsBoth || reads.step == SecondStep::Read);
    RELEASE_ASSERT(reads.returnsBoth || reads.secondReceiver == SecondReceiver::FirstResult);

    const GPRReg R0 = jit.operand(0);
    const GPRReg first = reads.returnsBoth ? jit.result() : R0;
    const GPRReg second = reads.secondReceiver == SecondReceiver::Other ? jit.operand(1) : reads.secondReceiver == SecondReceiver::FirstResult ? first : R0;
    if (reads.step != SecondStep::Read)
        jit.clobberOperand(0);

    const TwoReadsBody body {
        jit,
        reads,
        R0,
        first,
        second,
        jit.immediate(),
        jit.allocateScratch(StubRegister::T10),
        jit.allocateScratch(slotHeaderRegister),
        jit.allocateScratch(StubRegister::T13),
        jit.allocateScratch(StubRegister::T11),
        jit.allocateScratch(StubRegister::T14),
        Box<SecondReadExits>::create(),
    };
    RELEASE_ASSERT(calleeSignature(getByIdAfterFrontProbe()).operands[1].gpr() == body.cell);

    FirstProbeExits exits;
    exits.hasNoStructure = Box<JumpList>::create();
    jit.zeroExtend16To32(body.names, body.name);
    loadStructureIDOfReceiver(jit, R0, body.T11, *exits.hasNoStructure, [&] {
        JumpList isNotInFieldTable;
        readFieldByTable(jit, { R0, body.name, body.T11, body.cell, std::nullopt, true, true }, first, isNotInFieldTable, [&] {
            emitSecondStep(body, false);
        });
        isNotInFieldTable.link(&jit);
    });
    emitFirstProbe(body, exits);

    exits.isAbsent.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), first);
    emitSecondStep(body, body.readsSameReceiver());

    emitFirstReadInFrame(body, exits);

    // loadStructureIDOfReceiver adds to its list in an out-of-line part, which runs after this function has returned.
    jit.addOutOfLinePart([body, firstHasNoStructure = exits.hasNoStructure, genericRead = exits.genericRead](StubAssembler& jit) {
        firstHasNoStructure->linkTo(genericRead, &jit);
        emitSecondReadExit(body, body.secondReadExits->isNotOwnInline, getByIdAfterFrontProbe());
        emitSecondReadExit(body, body.secondReadExits->hasNoStructure, slotExit(SlotHandlerTable::ReadById, SlotExit::Miss));
    });
}

static StubSignature signatureOfFirstRead()
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

void generateGetByIdChain(StubAssembler& jit)
{
    generateTwoReads(jit, { SecondReceiver::FirstResult, false, SecondStep::Read, 0 });
}

StubSignature signatureOfGetByIdChain(StubKey)
{
    StubSignature signature = signatureOfFirstRead();
    signature.result = StubResult::GPR;
    signature.firstResult = gprOf(firstStubOperandRegister);
    return signature;
}

void generateGetByIdPair(StubAssembler& jit)
{
    generateTwoReads(jit, { static_cast<SecondReceiver>(jit.key().form()), true, SecondStep::Read, 0 });
}

StubSignature signatureOfGetByIdPair(StubKey key)
{
    StubSignature signature = signatureOfFirstRead();
    if (static_cast<SecondReceiver>(key.form()) == SecondReceiver::Other)
        signature.addOperand(gprOf(secondOperandRegister));
    signature.result = StubResult::TwoGPRs;
    signature.firstResult = gprOf(firstResultRegister);
    signature.secondResult = gprOf(firstStubOperandRegister);
    return signature;
}

void generateGetByIdIsTruthy(StubAssembler& jit)
{
    generateTwoReads(jit, { SecondReceiver::FirstResult, false, SecondStep::TestTruthiness, 0 });
}

StubSignature signatureOfGetByIdIsTruthy(StubKey)
{
    StubSignature signature = signatureOfFirstRead();
    signature.result = StubResult::Flags;
    signature.trueCondition = calleeSignature(valueIsTruthy()).trueCondition;
    return signature;
}

void generateGetByIdEqualsLiteral(StubAssembler& jit)
{
    generateTwoReads(jit, { SecondReceiver::FirstResult, false, SecondStep::CompareWithLiteral, jit.key().count() });
}

StubSignature signatureOfGetByIdEqualsLiteral(StubKey key)
{
    StubSignature signature = signatureOfFirstRead();
    signature.addOperand(gprOf(secondOperandRegister));
    signature.result = StubResult::Flags;
    signature.trueCondition = calleeSignature(literalComparison(key.count())).trueCondition;
    return signature;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
