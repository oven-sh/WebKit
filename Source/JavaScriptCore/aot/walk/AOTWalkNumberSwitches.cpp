/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations with UnboxedNumbers: a switch on what charCodeAt() gave, or on one character of a text, goes by the code unit, and a switch whose cases cover its subject leaves one of them untested.
// It plans and builds nothing: Walker::walkTerminator() changes the draft. The targets stay as written, each with its own case.
// At a suspension and at a throw: nothing, a Switch on an integer does neither. Withheld: the switch is built as written.

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTTypeArena.h"
#include "AOTTypeNarrowing.h"
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!OperandIndex::Switch::subject && !OperandIndex::SwitchString::subject);

// `switch (s.charCodeAt(i)) { case 32: .. }`. NaN takes no case, and -1, which stands in its place, takes none that is not negative.
static Reference integerToSwitchOnInPlaceOf(Walker& walker, const BodyTerminator& terminator, Reference subject)
{
    Reference codeUnit = codeUnitBesideCharCode(walker, subject);
    if (!codeUnit)
        return { };
    for (int64_t written : terminator.cases()) {
        if (written < 0)
            return { };
    }
    return codeUnit;
}

// `switch (s[i]) { case "a": .. }`, `switch (s.charAt(i))`. A text of one code unit is taken where the code unit is that one.
// The empty text is what charAt() gives outside the text, where -1 stands. What may be undefined there is never the empty text.
// No other text is one character, and of two equal texts the first takes the subject: such a case gets an integer that no code unit is, and keeps its place among the targets.
static bool planSwitchOnCodeUnitOfCharacter(Walker& walker, const BodyTerminator& terminator, Reference subject, SwitchOnNumbers& planned)
{
    Reference codeUnit = codeUnitBesideCharacter(walker, subject);
    if (!codeUnit)
        return false;
    bool mayBeUndefined = walker.types().at(walker.typeOf(subject)).primitives & Primitive::Undefined;

    Vector<char16_t> buffer;
    uint32_t caseIndex = 0;
    for (StringID written : terminator.strings()) {
        int64_t neverTaken = -2 - static_cast<int64_t>(caseIndex++);
        StringView text = walker.types().viewOf(written, buffer);
        int64_t taken = neverTaken;
        if (text.length() == 1)
            taken = text[0];
        else if (text.isEmpty() && !mayBeUndefined)
            taken = -1;
        planned.cases.append(planned.cases.contains(taken) ? neverTaken : taken);
    }
    if (planned.cases.isEmpty())
        return false;
    planned.subject = codeUnit;
    return true;
}

// `switch (n & 3) { case 0: .. case 1: .. case 2: .. case 3: .. }`.
static std::optional<uint32_t> caseThatNeedsNoTest(Walker& walker, const BodyTerminator& terminator, Reference subject)
{
    TypeID id = walker.typeOf(subject);
    if (!id || isNeverType(id))
        return std::nullopt;
    Type type = walker.types().at(id);
    uint32_t numberOfCases = terminator.numberOfCases();
    if (!type.isInteger() || type.integers.maximum - type.integers.minimum >= static_cast<int64_t>(numberOfCases))
        return std::nullopt;

    TypingContext typing = walker.typing();
    if (!isNeverType(typeOfCaseResult(typing, terminator, id, numberOfCases)))
        return std::nullopt;
    for (uint32_t caseIndex = numberOfCases; caseIndex--;) {
        if (!isNeverType(typeOfCaseResult(typing, terminator, id, caseIndex)))
            return caseIndex;
    }
    return std::nullopt;
}

std::optional<SwitchOnNumbers> planSwitchOnNumbers(Walker& walker, const BodyTerminator& terminator)
{
    if (!hasMechanismsOfNumberForms(walker) || !terminator.numberOfCases())
        return std::nullopt;
    Reference subject = walker.referenceOf(terminator.operand(0));
    if (!subject.isValue())
        return std::nullopt;

    SwitchOnNumbers planned;
    if (terminator.kind() == TerminatorKind::SwitchString)
        return planSwitchOnCodeUnitOfCharacter(walker, terminator, subject, planned) ? std::optional(WTF::move(planned)) : std::nullopt;
    if (terminator.kind() != TerminatorKind::Switch)
        return std::nullopt;

    planned.subject = integerToSwitchOnInPlaceOf(walker, terminator, subject);
    if (!planned.subject)
        planned.caseLeftUntested = caseThatNeedsNoTest(walker, terminator, subject);
    if (!planned.subject && !planned.caseLeftUntested)
        return std::nullopt;
    return planned;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
