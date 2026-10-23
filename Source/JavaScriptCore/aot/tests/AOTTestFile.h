/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCheckLines.h"
#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTLimits.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/Variant.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

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

struct ExpectedLoss {
    LimitID limit;
    std::optional<uint32_t> count;
    std::optional<FunctionID> function;
    std::optional<ValueID> value;
    std::optional<LayoutID> layout;
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

struct TestFile {
    String text;
    Vector<RunDirective> runs;
    std::optional<IRForm> form;
    bool isOptimizing { false };
    unsigned formLine { 0 };
    Vector<OptionWord> optionWords;
    Vector<CheckGroup> checkGroups;
    Vector<ExpectedDiagnostic> diagnostics;
    Vector<ExpectedStatistic> statistics;
    Vector<ExpectedLoss> losses;
    std::optional<TestTodo> todo;
    Vector<TestFileError> errors;

    std::span<const CheckLine> checkLinesFor(StringView prefix) const;
};

TestFile parseTestFile(String&& text, Architecture = hostArchitecture());
bool holds(StatisticComparison, uint64_t actual, uint64_t expected);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
