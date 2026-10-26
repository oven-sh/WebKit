/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInterningPhases.h"

#if ENABLE(AOT)

#include "AOTSchemaWalkers.h"

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

BodyRemaps InterningPhases::finish(const ParallelLoops* loops)
{
    StringRemap strings = m_strings.finish();
    TypeRemap types = m_types.finish(strings, loops);
    return { WTF::move(types), WTF::move(strings), FunctionRemap() };
}

BodyRemaps InterningPhases::finish(std::span<const unsigned> taskOrder, FunctionRemap&& functions, const ParallelLoops* loops)
{
    StringRemap strings = m_strings.finish(taskOrder);
    TypeRemap types = m_types.finish(taskOrder, functions, strings, loops);
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

namespace {

struct ProvisionalIDFinder {
    void operator()(const TypeID& id) { found |= TypeTable::isProvisional(id); }
    void operator()(const LayoutSetID& id) { found |= TypeTable::isProvisional(id); }
    void operator()(const CalleeSetID& id) { found |= TypeTable::isProvisional(id); }
    void operator()(const PropertyKeyID& id) { found |= TypeTable::isProvisional(id); }
    void operator()(const StringID& id) { found |= StringTable::isProvisional(id); }
    void operator()(const FunctionID& id) { found |= ProvisionalFunctions::contains(id); }
    template<typename Tag> void operator()(const ID<Tag>&) { }

    bool found { false };
};

struct InterningProvisionalIDRewriter {
    void operator()(TypeID& id) const { remaps.types.apply(id); }
    void operator()(LayoutSetID& id) const { remaps.types.apply(id); }
    void operator()(CalleeSetID& id) const { remaps.types.apply(id); }
    void operator()(PropertyKeyID& id) const { remaps.types.apply(id); }
    void operator()(StringID& id) const { remaps.strings.apply(id); }
    void operator()(FunctionID& id) const { remaps.functions.apply(id); }
    template<typename Tag> void operator()(ID<Tag>&) const { }

    const BodyRemaps& remaps;
};

} // anonymous namespace

RemapSites findRemapSites(const Function& function)
{
    RemapSites sites;
    {
        ProvisionalIDFinder finder;
        forEachReference(static_cast<const FunctionRecord&>(function), finder);
        sites.record = finder.found;
    }
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        const Value& value = function.values[index];
        ProvisionalIDFinder finder;
        finder(value.type);
        finder(value.expected().predicted());
        forEachReference(value, finder);
        if (finder.found || (value.expected().predicted() && value.expected().predicted() == value.type))
            sites.values.append(index);
    }
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        ProvisionalIDFinder finder;
        forEachReference(function.blocks[index].terminator, finder);
        if (finder.found)
            sites.blocks.append(index);
    }
    return sites;
}

void applyRemaps(const BodyRemaps& remaps, Function& function, const RemapSites& sites)
{
    if (remaps.isIdentity() || sites.isEmpty())
        return;
    // Other names for the same types.
    bool valuesAreTypedAsDefined = function.valuesAreTypedAsDefined();
    InterningProvisionalIDRewriter rewriter { remaps };
    if (sites.record)
        forEachReference(static_cast<FunctionRecord&>(function), rewriter);
    if (!sites.values.isEmpty()) {
        auto& values = function.editAllValues();
        for (uint32_t index : sites.values) {
            Value& value = values[index];
            remaps.types.apply(value.type);
            TypeID expected = remaps.types(value.expectedForTyping().predicted());
            value.setExpected(ExpectedType(expected == value.type ? TypeID() : expected));
            forEachReference(value, rewriter);
        }
    }
    if (!sites.blocks.isEmpty()) {
        auto& blocks = function.editAllBlocks();
        for (uint32_t index : sites.blocks)
            forEachReference(blocks[index].terminator, rewriter);
    }
    if (valuesAreTypedAsDefined)
        function.setValuesAreTypedAsDefined();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
