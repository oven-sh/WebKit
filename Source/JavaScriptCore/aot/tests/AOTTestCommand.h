/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIR.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include "AOTWire.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/PrintStream.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

struct TestCommandContext {
    std::unique_ptr<Program> program;
    IRForm form;
    std::span<const String> arguments;
    PrintStream& out;
    BuildContext& build;
    Vector<Wire::LossRecord> losses { };
    bool isOptimizing { false };
};

enum class ProgramAsRead : uint8_t { AsParsed, WithLibrary, TypedAndVerified };

struct TestCommand {
    ASCIILiteral name;
    std::optional<IRForm> defaultForm;
    void (*run)(TestCommandContext&);
    RawWordCheck rawWordCheck { RawWordCheck::Run };
    ProgramAsRead programAsRead { ProgramAsRead::AsParsed };
};

#define FOR_EACH_AOT_TEST_COMMAND_PROVIDER(v) \
    v(irTestCommands) \
    v(typeTestCommands) \
    v(effectsTestCommands) \
    v(supportTestCommands) \
    v(pipelineTestCommands) \
    v(analysisTestCommands) \
    v(flowTestCommands) \
    v(valueFlowTestCommands) \
    v(propertyAccessTestCommands) \
    v(passTestCommands) \
    v(allocationTestCommands) \
    v(testPlacementTestCommands) \
    v(selectionTestCommands) \
    v(placementTestCommands) \
    v(collectorTestCommands) \
    v(codeGeneratorTestCommands) \
    v(generatedCodeTestCommands) \
    v(validationTestCommands)

#define AOT_DECLARE_TEST_COMMAND_PROVIDER(name) std::span<const TestCommand> name();
FOR_EACH_AOT_TEST_COMMAND_PROVIDER(AOT_DECLARE_TEST_COMMAND_PROVIDER)
#undef AOT_DECLARE_TEST_COMMAND_PROVIDER

bool runReadStage(Program&, BuildContext&);

const TestCommand* findTestCommand(StringView name);
void checkTestCommandNames();
void listTestCommands(PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
