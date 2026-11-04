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

// `# -- <name>` down to `# -- end of <name>`: text for a stage other than the reader, each line after `# `. The one name: code.
struct TestSection {
    String name;
    // Of the first line of the text, so that an error in it names a line of the file.
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
    // `# TYPES: f<n>`: `t<k>` in a section or an argument is the declared type of parameter k of that function.
    std::optional<FunctionID> functionOfTypesLine;
    std::optional<TestTodo> todo;
    // `# HANGS-ON-ORDER: <owner>: <what differs>`: a debt of Link alone. It says nothing of what the file's own lines expect.
    std::optional<TestTodo> hangsOnOrder;
    Vector<TestFileError> errors;

    std::span<const CheckLine> checkLinesFor(StringView prefix) const;
};

TestFile parseTestFile(String&& text, Architecture = hostArchitecture(), const String& folder = { });
bool holds(StatisticComparison, uint64_t actual, uint64_t expected);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
