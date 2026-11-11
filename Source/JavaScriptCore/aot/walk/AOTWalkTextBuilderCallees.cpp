/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTDeclaredType.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);

static constexpr uint32_t frontOperandInTextBuilderCallees = 0;
static_assert(OperandIndex::Add::left == frontOperandInTextBuilderCallees && OperandIndex::Strcat::parts == frontOperandInTextBuilderCallees);

Reference primitivePartOfStringBuilder(Walker& walker, Opcode join, Reference part, Origin origin)
{
    if (hasTextByItself(walker.types().at(walker.typeOf(part))))
        return part;

    origin.isTailCall = false;
    Draft conversion(join == Opcode::Add ? Opcode::Add : Opcode::ToString, origin);
    walker.chooseConservativeFields(conversion);
    if (join == Opcode::Add) {
        Draft emptyText(Opcode::String, origin);
        emptyText.setString(walker.program().strings.emptyString());
        conversion.setLeftOperand(walker.constant(WTF::move(emptyText)));
        conversion.setRightOperand(part);
    } else
        conversion.setValueOperand(part);
    conversion.setForm(operatorFormFor(walker, conversion));
    return walker.build(WTF::move(conversion));
}

static void appendPartsInTextBuilderCallees(Walker& walker, const BodyValue& join, Reference name, std::span<const Reference> parts, Origin origin)
{
    StringInBuilder& text = *findStringInBuilder(walker, join.id());
    if (text.web.standsUnderHandlers)
        appendPartsRolledBackWhereTheyFail(walker, text, name, parts, origin);
    else
        appendPartsToStringBuilder(walker, text, name, parts, origin);
}

static bool appendPartsBehindTestsOfWhatIsWrittenInTextBuilderCallees(Walker& walker, const BodyValue& join, std::span<const Reference> parts, Origin origin)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    Vector<ValueID, 8> written;
    join.forEachOperand([&](ValueID operand, uint32_t) {
        written.append(operand);
    });
    if (written.size() != parts.size())
        return false;

    struct GuessAtPart {
        size_t place;
        TypeID expected;
    };
    Vector<GuessAtPart, 4> guesses;
    constexpr uint32_t weightOfPartAppendedInLine = 1;
    for (size_t place = 1; place < parts.size(); ++place) {
        if (hasTextByItself(walker.types().at(walker.typeOf(parts[place]))))
            continue;
        if (!parts[place].isValue())
            return false;
        ExpectationOfValue expectation = findExpectation(walker, written[place]);
        if (!expectation.type || isNeverType(expectation.type) || !hasTextByItself(walker.types().at(expectation.type)))
            return false;
        if (!walker.types().overlap(walker.typeOf(parts[place]), expectation.type))
            return false;
        if (!shouldOpenGuess(walker, parts[place], expectation.type, expectation.isDeclared, weightOfPartAppendedInLine))
            return false;
        guesses.append({ place, expectation.type });
    }
    if (guesses.isEmpty())
        return false;

    Vector<Reference, 4> tests;
    for (const GuessAtPart& guess : guesses) {
        Draft test(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(parts[guess.place]);
        test.setTestedType(guess.expected);
        Reference passes = walker.build(WTF::move(test));
        if (!passes)
            return true;
        if (!passes.isValue())
            return false;
        tests.append(passes);
    }

    BlockID converted = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    Vector<Reference, 8> appended;
    appended.append(parts.subspan(1));
    for (size_t position = 0; position < guesses.size(); ++position) {
        const GuessAtPart& guess = guesses[position];
        Reference part = parts[guess.place];
        Reference passes = tests[position];
        BlockID passed = walker.addBlock();
        walker.branch(passes, passed, converted);
        walker.continueIn(passed);

        Draft proof(Opcode::Narrowed, origin);
        walker.chooseConservativeFields(proof);
        proof.setValueOperand(part);
        proof.setTestOperand(passes);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        Reference narrowed = walker.build(WTF::move(proof));
        if (!narrowed) {
            walker.fail("a part of a string in a builder passes the check of its declared type, and nothing can be built for it after the check"_s);
            return true;
        }
        appended[guess.place - 1] = narrowed;
    }
    walker.recordSpeculation();
    appendPartsInTextBuilderCallees(walker, join, parts[0], appended.span(), origin);
    if (!walker.isUnreachable())
        walker.jumpTo(meeting);

    walker.continueIn(converted);
    appended.shrink(0);
    for (Reference part : parts.subspan(1)) {
        Reference primitive = primitivePartOfStringBuilder(walker, join.opcode(), part, origin);
        if (!primitive)
            break;
        appended.append(primitive);
    }
    if (appended.size() + 1 == parts.size())
        appendPartsInTextBuilderCallees(walker, join, parts[0], appended.span(), origin);
    if (!walker.isUnreachable())
        walker.jumpTo(meeting);

    walker.continueIn(meeting);
    return true;
}

bool tryConvertAndAppendParts(Walker& walker, const BodyValue& join, std::span<const Reference> parts)
{
    if (!walker.hasStringsInBuilders() || parts.size() < 2 || !isStringInBuilder(walker, parts[0]) || !isAppendToStringInBuilder(walker, join))
        return false;
    for (Reference part : parts.subspan(1)) {
        if (!part || !part.isRecord() || isStringInBuilder(walker, part))
            return false;
    }
    StringInBuilder* grown = findStringInBuilder(walker, join.id());
    if (!grown)
        return false;
    if (!ensureAppendIsConcatenation(walker, *grown, parts.subspan(1)))
        return true;

    Origin origin = walker.originOf(join);
    origin.isTailCall = false;
    if (appendPartsBehindTestsOfWhatIsWrittenInTextBuilderCallees(walker, join, parts, origin)) {
        if (!walker.isUnreachable())
            walker.bind(join.id(), parts[0]);
        return true;
    }
    Vector<Reference, 8> converted;
    for (Reference part : parts.subspan(1)) {
        Reference primitive = primitivePartOfStringBuilder(walker, join.opcode(), part, origin);
        if (!primitive)
            return true;
        converted.append(primitive);
    }

    appendPartsInTextBuilderCallees(walker, join, parts[0], converted.span(), origin);
    if (!walker.isUnreachable())
        walker.bind(join.id(), parts[0]);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
