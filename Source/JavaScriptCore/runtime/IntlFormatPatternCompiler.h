/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "IntlDateTimeFormat.h"
#include "IntlFormatPattern.h"
#include "IntlNumberFormat.h"
#include "JSString.h"
#include <optional>
#include <span>
#include <unicode/udat.h>
#include <utility>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

enum class PatternDecline : uint8_t {
    Unknown,
    CalendarOrNumberingSystemNotSupported,
    OptionNotSupported,
    PatternNotReadable,
    PatternLetterNotSupported,
    SymbolsNotReadable,
    GroupingNotSupported,
    PatternTooLong,
    DiffersFromICU,
};

class PatternCode {
public:
    void append(unsigned unit)
    {
        m_isValid &= unit <= 0xff;
        m_units.append(static_cast<char16_t>(unit));
    }
    void append(DateTimePatternOp op) { append(static_cast<unsigned>(op)); }
    void appendText(StringView text)
    {
        append(text.length());
        for (unsigned i = 0; i < text.length(); ++i) {
            m_is16Bit |= text[i] > 0xff;
            m_units.append(text[i]);
        }
    }
    void appendLiteral(StringView text)
    {
        if (text.isEmpty())
            return;
        append(DateTimePatternOp::Literal);
        appendText(text);
        m_maxLength += text.length();
    }
    void appendNames(DateTimePatternOp op, std::span<const String> names)
    {
        unsigned longest = 0;
        for (auto& name : names)
            longest = std::max(longest, name.length());
        append(op);
        append(names.size());
        append(longest + 1);
        for (auto& name : names) {
            appendText(name);
            for (unsigned i = name.length(); i < longest; ++i)
                m_units.append(0);
        }
        m_maxLength += longest;
    }
    void didAppendField(unsigned maxLength) { m_maxLength += maxLength; }

    std::optional<Vector<uint8_t>> finish(FormatPatternKind kind, FormatPatternTimeZone timeZone, int32_t utcOffsetInMinutes) const
    {
        if (!m_isValid || m_maxLength > maxFormatPatternLength)
            return std::nullopt;
        FormatPatternHeader header { kind, m_is16Bit, timeZone, static_cast<uint16_t>(m_maxLength), utcOffsetInMinutes };
        Vector<uint8_t> bytes;
        bytes.append(asByteSpan(header));
        if (m_is16Bit)
            bytes.append(asBytes(m_units.span()));
        else {
            for (char16_t unit : m_units)
                bytes.append(static_cast<uint8_t>(unit));
        }
        return bytes;
    }

private:
    Vector<char16_t, 128> m_units;
    unsigned m_maxLength { 0 };
    bool m_is16Bit { false };
    bool m_isValid { true };
};

class PseudoRandomNumbers {
public:
    double next()
    {
        m_state = m_state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(m_state >> 11) / static_cast<double>(1ull << 53);
    }

private:
    uint64_t m_state { 0x9e3779b97f4a7c15ull };
};

class IntlFormatPatternCompiler {
public:
    std::unique_ptr<IntlFormatPattern> compile(const IntlDateTimeFormat&);
    std::unique_ptr<IntlFormatPattern> compile(const IntlNumberFormat&);
    PatternDecline lastDecline() const { return m_decline; }

private:
    std::nullptr_t decline(PatternDecline);

    static String withNormalSpaces(std::span<const char16_t>);
    static bool appendSymbols(PatternCode&, DateTimePatternOp, const UDateFormat*, UDateFormatSymbolType, int32_t first, unsigned count);
    static bool appendField(PatternCode&, const UDateFormat*, char16_t letter, unsigned count);
    std::optional<PatternCode> codeFromPatternOf(const UDateFormat*);
    static Vector<double> sampleTimeValues(int32_t utcOffsetInMinutes);
    bool agreesOnSamples(const IntlDateTimeFormat&, IntlFormatPattern&);
    bool agreesAtTimeZoneTransitions(const IntlDateTimeFormat&, IntlFormatPattern&);

    struct NumberParts;
    struct NumberOptions;
    struct NumberRules;
    std::optional<NumberParts> partsOf(const IntlNumberFormat&, double number, bool hasFraction);
    std::optional<NumberParts> partsByFieldPositions(const IntlNumberFormat&, double number);
    std::optional<NumberOptions> readNumberOptions(const IntlNumberFormat&);
    std::optional<NumberRules> readNumberRules(const NumberOptions&);
    bool agreesOnParts(const IntlNumberFormat&, const NumberOptions&, const NumberRules&);
    std::optional<unsigned> findMinDigitsToGroup(const IntlNumberFormat&, const NumberOptions&, const NumberRules&);
    bool agreesOnSamples(const IntlNumberFormat&, const IntlFormatPattern&);

    PatternDecline m_decline { PatternDecline::Unknown };
};

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
