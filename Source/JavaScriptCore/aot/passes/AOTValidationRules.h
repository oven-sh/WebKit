/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONEmission.h"
#include "AOTSelectTypedResultsNodes.h"
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

// GivenValue: the value as the caller or the text gave it: no default is filled in, no member is left out.
enum class RuleSubject : uint8_t { Number, Length, Text, GivenValue, Nothing };
enum class RuleCode : uint8_t { ByHelper, Inline, CountedCharacters, RegularExpression, Nothing };

RuleSubject subjectOf(const ValidationRule&);
bool appliesTo(const ValidationRule&, ResultShape);
bool hasIntegerRule(const ResultNode&);
bool hasRuleOnGivenValue(const ResultNode&);

ASCIILiteral formatName(TextFormat);

class ValidationRuleEmitter {
    WTF_MAKE_NONCOPYABLE(ValidationRuleEmitter);
public:
    explicit ValidationRuleEmitter(Emission& emission)
        : m_emission(emission)
    {
    }

    RuleCode codeFor(const ValidationRule&, ResultShape);

    void emitRules(const ResultNode&, ValueID built, ValueID given, BlockID rejected);
    void emitLengthRules(const ResultNode&, ValueID length, BlockID rejected);
    void emitRequireFinite(ValueID number, BlockID rejected);
    ValueID appendLiteral(const ResultLiteral&);

private:
    Emission& m_emission;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
