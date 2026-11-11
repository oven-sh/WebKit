/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTTypeArena.h"
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!OperandIndex::Switch::subject && !OperandIndex::SwitchString::subject);

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

static bool planSwitchOnCodeUnitOfCharacter(Walker& walker, const BodyTerminator& terminator, Reference subject, SwitchOnNumbers& planned)
{
    Reference codeUnit = codePointBesideCharacter(walker, subject);
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
        else if (text.length() == 2 && U16_IS_LEAD(text[0]) && U16_IS_TRAIL(text[1]))
            taken = U16_GET_SUPPLEMENTARY(text[0], text[1]);
        else if (text.isEmpty() && !mayBeUndefined)
            taken = -1;
        planned.cases.append(planned.cases.contains(taken) ? neverTaken : taken);
    }
    if (planned.cases.isEmpty())
        return false;
    planned.subject = codeUnit;
    return true;
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
        return std::nullopt;
    return planned;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
