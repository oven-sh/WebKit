/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTInterningPhases.h"
#include "AOTPassRunner.h"
#include "AOTPipelineInternal.h"
#include "AOTTestCommand.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

class SelectionTestSession {
    WTF_MAKE_NONCOPYABLE(SelectionTestSession);
public:
    SelectionTestSession(TestCommandContext&, FunctionID);

    FunctionPassContext& pass() { return m_pass; }
    Function& function() { return m_pass.function; }

private:
    PassRunner m_runner;
    Pipeline m_pipeline;
    OptimizationInputs m_inputs;
    TaskArenas m_arenas;
    FunctionAnalyses m_analyses;
    FunctionPassContext m_pass;
};

std::optional<FunctionID> parseFunctionWithBody(const Program&, StringView);
std::optional<ValueID> parseListedValue(const Function&, StringView);
std::optional<BlockID> parseBlock(const Function&, StringView);
std::optional<StringView> valueForKey(StringView word, ASCIILiteral key);

void runClaimTestCommand(TestCommandContext&);
void runCodeShapesTestCommand(TestCommandContext&);
void runLoopEntryTestCommand(TestCommandContext&);
void runComparisonChainsTestCommand(TestCommandContext&);
void runOperatorFormsTestCommand(TestCommandContext&);
void runInlineFormsTestCommand(TestCommandContext&);
void runRepresentationRulesTestCommand(TestCommandContext&);
void runConversionsTestCommand(TestCommandContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
