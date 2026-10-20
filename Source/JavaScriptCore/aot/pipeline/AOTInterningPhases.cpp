/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInterningPhases.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

InterningPhases::InterningPhases(Program& program, unsigned numberOfTasks)
    : m_types(program.types, numberOfTasks)
    , m_strings(program.strings, numberOfTasks)
{
}

InterningPhases::InterningPhases(ProgramEditor& editor, unsigned numberOfTasks)
    : m_types(editor.types(), numberOfTasks)
    , m_strings(editor.strings(), numberOfTasks)
{
}

unsigned InterningPhases::addTasks(unsigned count)
{
    unsigned firstTypeTask = m_types.addTasks(count);
    unsigned firstStringTask = m_strings.addTasks(count);
    RELEASE_ASSERT(firstTypeTask == firstStringTask);
    return firstTypeTask;
}

BodyRemaps InterningPhases::finish()
{
    StringRemap strings = m_strings.finish();
    TypeRemap types = m_types.finish(strings);
    return { WTF::move(types), WTF::move(strings), FunctionRemap() };
}

BodyRemaps InterningPhases::finish(std::span<const unsigned> taskOrder, FunctionRemap&& functions)
{
    StringRemap strings = m_strings.finish(taskOrder);
    TypeRemap types = m_types.finish(taskOrder, functions, strings);
    return { WTF::move(types), WTF::move(strings), WTF::move(functions) };
}

void InterningPhases::discard()
{
    m_types.discard();
    m_strings.discard();
}

TaskArenas::TaskArenas(Program& program)
    : types(program.types)
    , strings(program.strings)
{
}

TaskArenas::TaskArenas(InterningPhases& phases, unsigned task)
    : types(phases.m_types, task)
    , strings(phases.m_strings, task)
{
}

void applyRemaps(const BodyRemaps& remaps, Function& function)
{
    if (remaps.isIdentity())
        return;
    remapIDs(function, remaps.types, remaps.strings, remaps.functions);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
