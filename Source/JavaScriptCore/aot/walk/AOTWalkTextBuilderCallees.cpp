/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Of a text that lies in a builder, where code of the program runs in the middle of a growth.
// TypedOperations: `s += obj`: every part that may run code is converted where the join stands, in the order of the parts, and only then the first part is appended: whatever the conversions run or throw finds the text as it was.
// Inlining: `s = join(s, x)`: the callee hands its parameter back with more behind it, so its copy appends. Where the walk makes the call after all, the call takes a string, and the builder is filled anew with what the call gives.
// At a suspension: no text lies in a builder across one. At a throw in a callee: no text that grows in callees lies in a builder under a handler. Withheld: the call is a reader.

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

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

static constexpr uint32_t frontOperandInTextBuilderCallees = 0;
static_assert(OperandIndex::Add::left == frontOperandInTextBuilderCallees && OperandIndex::Strcat::parts == frontOperandInTextBuilderCallees);

// ---- A part that may run code

bool convertsPartsThatMayRunCode()
{
    constexpr bool convertsWhereJoinStands = true;
    return convertsWhereJoinStands;
}

// Add converts an object with another hint than Strcat does. A symbol throws in either.
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
    conversion.setForm(formAllowedForTypedOperator(walker, conversion));
    return walker.build(WTF::move(conversion));
}

static void appendPartsInTextBuilderCallees(Walker& walker, const BodyValue& join, Reference name, std::span<const Reference> parts, Origin origin)
{
    TextInStringBuilder& text = *findTextNamedBy(walker, join.id());
    if (text.web.standsUnderHandlers)
        appendPartsRolledBackWhereTheyFail(walker, text, name, parts, origin);
    else
        appendPartsToStringBuilder(walker, text, name, parts, origin);
}

// `line += c` with c out of a list that is written number[], `text += `${row.id},${row.title}\n``: what is written of each part that is not proven to have text by itself is tested, every test in front of every append.
// Where all pass no part runs code, and each is appended in its narrowest form. Where one fails the parts are converted as the join converts them. A test runs no code, so nothing changes its place.
static bool appendPartsBehindTestsOfWhatIsWrittenInTextBuilderCallees(Walker& walker, const BodyValue& join, std::span<const Reference> parts, Origin origin)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    // The parts are the operands of the join one by one: takePartsInWalkTexts() has refused the join by itself.
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
        // In a copy the part may be known to be something else than is written of it.
        if (!walker.types().overlap(walker.typeOf(parts[place]), expectation.type))
            return false;
        if (!shouldOpenGuess(walker, parts[place], expectation.type, expectation.isDeclared, weightOfPartAppendedInLine))
            return false;
        guesses.append({ place, expectation.type });
    }
    if (guesses.isEmpty())
        return false;

    // Every test is built before a block is made: a test that is decided opens no guess, and nothing is left half built.
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
            walker.fail("a part of a text in a builder passes the test of what is written of it, and nothing can be built for it behind the test"_s);
            return true;
        }
        appended[guess.place - 1] = narrowed;
    }
    walker.noteGuessOpened();
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

bool convertPartsAndAppendToTextInStringBuilder(Walker& walker, const BodyValue& join, std::span<const Reference> parts)
{
    if (!walker.hasTextsInStringBuilders() || parts.size() < 2 || !isTextInStringBuilder(walker, parts[0]) || !isGrowthOfTextInStringBuilder(walker, join))
        return false;
    for (Reference part : parts.subspan(1)) {
        if (!part || !part.isRecord() || isTextInStringBuilder(walker, part))
            return false;
    }
    if (!findTextNamedBy(walker, join.id()))
        return false;

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
