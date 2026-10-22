/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTestCommand.h"
#include "AOTTestFile.h"
#include <optional>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class ChildProcessLauncher;

namespace Typed {

// Another way to run the same files: under more option words than they name themselves, and for one diagnostic alone.
struct TestVariant {
    Vector<String> optionWords;
    std::optional<DiagnosticName> onlyDiagnostic;
};

struct TestRunRequest {
    Vector<String> paths;
    String filter;
    unsigned jobs { 0 };
    bool showsOutput { false };
    const ChildProcessLauncher* launcher { nullptr };
    TestVariant variant;
};

struct TestRunSummary {
    unsigned passed { 0 };
    unsigned failed { 0 };
    unsigned todo { 0 };
};

TestRunSummary runTestFiles(const TestRunRequest&, PrintStream&);

inline constexpr ASCIILiteral runSubcommandName = "run"_s;
inline constexpr ASCIILiteral showOutputFlag = "--show-output"_s;
inline constexpr ASCIILiteral optionFlag = "--option"_s;
inline constexpr ASCIILiteral onlyDiagnosticFlag = "--only-diagnostic"_s;

struct TestFailure {
    unsigned line;
    String message;
};

struct RunOutput {
    unsigned line;
    String text;
};

Vector<TestFailure> runTestText(String&& text, Vector<RunOutput>* outputs = nullptr, const TestVariant& = { }, std::optional<TestTodo>* = nullptr);

void reopenDiagnostics(BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
