/*
 * Copyright (C) 2011, 2016 Apple Inc. All rights reserved.
 * Copyright (C) 2025 Tetsuharu Ohzeki <tetsuharu.ohzeki@gmail.com>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "YarrSyntaxChecker.h"

#include "YarrFlags.h"
#include "YarrParser.h"

namespace JSC { namespace Yarr {

class SyntaxChecker {
public:
    NODELETE void assertionBOL() { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void assertionEOL() { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void assertionWordBoundary(bool) { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void atomPatternCharacter(char32_t, bool) { }
    NODELETE void atomBuiltInCharacterClass(BuiltInCharacterClassID, bool) { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void atomCharacterClassBegin(bool = false) { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void atomCharacterClassAtom(char16_t) { }
    NODELETE void atomCharacterClassRange(char16_t, char16_t) { }
    NODELETE void atomCharacterClassBuiltIn(BuiltInCharacterClassID, bool) { }
    NODELETE void atomClassStringDisjunction(Vector<Vector<char32_t>>&) { }
    NODELETE void atomCharacterClassSetOp(CharacterClassSetOp) { }
    NODELETE void atomCharacterClassPushNested(bool) { }
    NODELETE void atomCharacterClassPopNested(bool) { }
    NODELETE void atomCharacterClassEnd() { }
    NODELETE void atomCharacterClassRemembered(unsigned) { }
    NODELETE bool atomCharacterClassRepeated(unsigned) { return true; }
    NODELETE void atomParenthesesSubpatternBegin(bool capture, std::optional<String> groupName = std::nullopt)
    {
        parenthesesBegin();
        if (!capture)
            return;
        ++m_summary.numSubpatterns;
        if (groupName)
            m_summary.hasNamedCaptureGroups = true;
    }
    NODELETE void atomParentheticalAssertionBegin(bool, MatchDirection) { parenthesesBegin(); }
    NODELETE void atomParentheticalModifierBegin(OptionSet<Flags>, OptionSet<Flags>) { parenthesesBegin(); }
    NODELETE void atomParenthesesEnd() { --m_parenthesesDepth; }
    NODELETE void atomBackReference(unsigned) { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void atomNamedBackReference(const String&) { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void atomNamedForwardReference(const String&) { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void quantifyAtom(unsigned min, unsigned max, bool)
    {
        m_summary.isOnlyPatternCharacters = false;
        m_summary.hasCountedQuantifier |= min > 1 || (max > 1 && max != quantifyInfinite);
    }
    NODELETE void disjunction(CreateDisjunctionPurpose) { m_summary.isOnlyPatternCharacters = false; }
    NODELETE void willLookUpUnicodeProperty() { m_summary.hasUnicodePropertyLookup = true; }
    NODELETE void resetForReparsing()
    {
        bool hasUnicodePropertyLookup = m_summary.hasUnicodePropertyLookup;
        m_summary = { };
        m_summary.hasUnicodePropertyLookup = hasUnicodePropertyLookup;
        m_parenthesesDepth = 0;
    }

    NODELETE constexpr static bool abortedDueToError() { return false; }
    NODELETE constexpr static ErrorCode abortErrorCode() { return ErrorCode::NoError; }

    SyntaxSummary m_summary;

private:
    void parenthesesBegin()
    {
        m_summary.isOnlyPatternCharacters = false;
        m_summary.maxParenthesesDepth = std::max(m_summary.maxParenthesesDepth, ++m_parenthesesDepth);
    }

    unsigned m_parenthesesDepth { 0 };
};
static_assert(YarrSyntaxCheckable<SyntaxChecker>);

SyntaxSummary checkSyntax(StringView pattern, OptionSet<Flags> flags)
{
    SyntaxChecker syntaxChecker;
    syntaxChecker.m_summary.error = parse(syntaxChecker, pattern, compileMode(flags));
    return syntaxChecker.m_summary;
}

ErrorCode checkSyntax(StringView pattern, StringView flags)
{
    auto parsedFlags = parseFlags(flags);
    if (!parsedFlags)
        return ErrorCode::InvalidRegularExpressionFlags;

    return checkSyntax(pattern, parsedFlags.value()).error;
}

}} // JSC::Yarr
