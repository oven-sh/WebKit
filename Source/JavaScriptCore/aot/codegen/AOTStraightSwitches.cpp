/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStringEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsStringLiterals.h"
#include <algorithm>
#include <wtf/text/ASCIIFastPath.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr GPRReg subjectOfStraightSwitchGPR = gprOf(comparedSubjectRegister);

namespace {

struct EdgeOfStraightSwitch {
    uint32_t index;
    BodyTarget target;
};

struct TextPackedForStraightSwitch {
    unsigned length;
    uint64_t packed;
};

struct TestOfStraightSwitch {
    CCallHelpers::Jump jump;
    bool keepsSubject;
};

} // anonymous namespace

bool StraightEmitter::coversSwitchesOnStrings()
{
    return true;
}

static std::optional<TextPackedForStraightSwitch> tryPackTextForStraightSwitch(std::span<const uint8_t> programString)
{
    constexpr size_t maxLength = descriptorOf(StubFamily::ValueEqualsLiteralOfLength).maxCount;
    constexpr size_t maxEncodedSizeOfLatin1Character = 2;
    if (programString.size() > maxLength * maxEncodedSizeOfLatin1Character)
        return std::nullopt;
    Vector<char16_t> bufferForNonASCII;
    StringView text = viewOfProgramString(programString, bufferForNonASCII);
    if (text.length() > maxLength || !(text.is8Bit() || WTF::charactersAreAllLatin1(text.span16())))
        return std::nullopt;
    return TextPackedForStraightSwitch { text.length(), packStringIntoRegister(text) };
}

static bool keepsSubjectOfStraightSwitch(const StubSummary& summary)
{
    return !summary.clobbered.contains(subjectOfStraightSwitchGPR, IgnoreVectors);
}

static TestOfStraightSwitch testCaseOfStraightSwitch(StraightEmitter& emitter, Origin origin, StringID literal, bool jumpsIfEqual)
{
    CCallHelpers& jit = emitter.jit();
    StraightArgument subject = StraightArgument::inRegister(subjectOfStraightSwitchGPR);
    std::span<const uint8_t> programString = emitter.program().strings[literal];
    if (programString.empty())
        return { jit.branch64(jumpsIfEqual ? CCallHelpers::Equal : CCallHelpers::NotEqual, subjectOfStraightSwitchGPR, CCallHelpers::Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString))), true };

    if (std::optional<TextPackedForStraightSwitch> text = tryPackTextForStraightSwitch(programString)) {
        StubKey stub = equalsLiteralOfLength(ComparedOperand::Value, text->length);
        const StubSummary& summary = emitter.catalog().summaryOf(stub);
        StraightArgument operands[] { subject, StraightArgument::word(text->packed) };
        emitter.callStub(origin, stub, operands, { });
        bool zeroSaysEqual = summary.signature.trueCondition == MacroAssembler::Zero;
        return { jit.branchOnFlags(jumpsIfEqual == zeroSaysEqual ? CCallHelpers::Equal : CCallHelpers::NotEqual), keepsSubjectOfStraightSwitch(summary) };
    }

    StubKey stub = AOT::equalsLiteral(ComparedOperand::Value);
    emitter.callStub(origin, stub, { &subject, 1 }, { .numberedImmediate = NumberedImmediate { emitter.mentions().add(NumberSpace::String, literal) } });
    return { jit.branchTest32(jumpsIfEqual ? CCallHelpers::NonZero : CCallHelpers::Zero, emitter.resultOfCall()), keepsSubjectOfStraightSwitch(emitter.catalog().summaryOf(stub)) };
}

void StraightEmitter::generateSwitchOnString(const BodyTerminator& terminator)
{
    auto literals = terminator.strings();
    RELEASE_ASSERT(terminator.numberOfTargets() == literals.size() + 1);

    std::span<const BlockID> passedOverByScan = facts().summaries().at(function()).blocksNeverEntered.span();
    Vector<EdgeOfStraightSwitch, 16> edges;
    uint32_t targetIndex = 0;
    for (BodyTarget target : terminator.targets()) {
        if (!std::ranges::binary_search(passedOverByScan, target.block()))
            edges.append({ targetIndex, target });
        ++targetIndex;
    }
    if (edges.isEmpty()) {
        if (waitsInRegister(terminator.subjectOperand()))
            m_valueInRegister = { };
        m_jit->breakpoint();
        return;
    }
    EdgeOfStraightSwitch rest = edges.takeLast();

    bool hasSubject = false;
    for (const EdgeOfStraightSwitch& edge : edges) {
        if (!hasSubject)
            load(terminator.subjectOperand(), subjectOfStraightSwitchGPR);
        bool jumpsIfEqual = edgeMovesNothing(terminator, edge.index, edge.target);
        TestOfStraightSwitch test = testCaseOfStraightSwitch(*this, terminator.origin(), literals[edge.index], jumpsIfEqual);
        hasSubject = test.keepsSubject;
        passArguments(terminator, edge.index, edge.target);
        if (jumpsIfEqual) {
            jumpTo(test.jump, edge.target.block());
            continue;
        }
        jumpTo(m_jit->jump(), edge.target.block());
        test.jump.link(m_jit);
    }
    passArguments(terminator, rest.index, rest.target);
    jumpTo(rest.target.block());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
