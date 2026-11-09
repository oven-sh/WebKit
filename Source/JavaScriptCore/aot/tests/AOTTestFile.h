/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCheckLines.h"
#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "CPUFeatureSet.h"
#include <optional>
#include <span>
#include <wtf/Variant.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

#if CPU(ARM64)
static constexpr bool targetHasJSCVT = CPUFeatureSet::buildTarget().contains(CPUFeature::JSCVT);
#else
static constexpr bool targetHasJSCVT = false;
#endif

struct RunDirective {
    String prefix;
    String command;
    Vector<String> arguments;
    unsigned line;
    bool isLeftOutUnderFurtherOptionWords { false };
};

struct OptionWord {
    String word;
    unsigned line;
};

using DiagnosticName = Variant<UserDiagnosticKind, InternalErrorKind>;

std::optional<DiagnosticName> parseDiagnosticName(StringView);
ASCIILiteral nameOf(const DiagnosticName&);

struct ExpectedDiagnostic {
    DiagnosticName kind;
    std::optional<FunctionID> function;
    std::optional<unsigned> count;
    unsigned line;
};

enum class StatisticComparison : uint8_t { Equal, AtLeast, AtMost };

struct ExpectedStatistic {
    StatisticID statistic;
    StatisticComparison comparison;
    uint64_t value;
    unsigned line;
};

struct CheckGroup {
    String prefix;
    Vector<CheckLine> lines;
};

struct TestFileError {
    unsigned line;
    String message;
};

struct TestTodo {
    String reason;
    unsigned line;
};

struct TestSection {
    String name;
    unsigned firstLine { 0 };
    String text;
};

struct TestFile {
    String text;
    Vector<RunDirective> runs;
    Vector<OptionWord> optionWords;
    Vector<OptionWord> jscOptions;
    Vector<CheckGroup> checkGroups;
    Vector<ExpectedDiagnostic> diagnostics;
    Vector<ExpectedStatistic> statistics;
    Vector<TestSection> sections;
    std::optional<FunctionID> functionOfTypesLine;
    std::optional<TestTodo> todo;
    std::optional<TestTodo> hangsOnOrder;
    std::optional<TestTodo> asUsersBuild;
    Vector<TestFileError> errors;

    std::span<const CheckLine> checkLinesFor(StringView prefix) const;
};

TestFile parseTestFile(String&& text, Architecture = hostArchitecture(), const String& folder = { });
bool holds(StatisticComparison, uint64_t actual, uint64_t expected);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
