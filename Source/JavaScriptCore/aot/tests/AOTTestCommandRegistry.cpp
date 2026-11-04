/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr auto testCommandProviders()
{
#define AOT_TEST_COMMAND_PROVIDER_ADDRESS(name) &name,
    return std::array { FOR_EACH_AOT_TEST_COMMAND_PROVIDER(AOT_TEST_COMMAND_PROVIDER_ADDRESS) };
#undef AOT_TEST_COMMAND_PROVIDER_ADDRESS
}

const TestCommand* findTestCommand(StringView name)
{
    for (auto provider : testCommandProviders()) {
        for (const TestCommand& command : provider()) {
            if (name == command.name)
                return &command;
        }
    }
    return nullptr;
}

void checkTestCommandNames()
{
    for (auto provider : testCommandProviders()) {
        for (const TestCommand& command : provider()) {
            RELEASE_ASSERT_WITH_MESSAGE(!command.name.isEmpty(), "A test command has no name");
            RELEASE_ASSERT_WITH_MESSAGE(command.run, "Test command %s has no function to run", command.name.characters());
            RELEASE_ASSERT_WITH_MESSAGE(findTestCommand(command.name) == &command, "Two test commands are named %s", command.name.characters());
        }
    }
}

void listTestCommands(PrintStream& out)
{
    for (auto provider : testCommandProviders()) {
        for (const TestCommand& command : provider())
            out.println(command.name, " ", command.defaultForm ? nameOf(*command.defaultForm) : "-"_s);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
