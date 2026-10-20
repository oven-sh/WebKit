/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleTable.h"

#if ENABLE(AOT)

#include "AOTProgramStrings.h"
#include <algorithm>
#include <compare>
#include <utility>

namespace JSC { namespace AOT {

// WTF::codePointCompare() orders by UTF-16 code unit. modulesByPath has the order of the UTF-8 bytes of the paths, which is code point order.
static std::strong_ordering compareCodePoints(StringView first, StringView second)
{
    if (first.is8Bit() || second.is8Bit())
        return codePointCompare(first, second);
    auto firstCodePoints = first.codePoints();
    auto secondCodePoints = second.codePoints();
    return std::lexicographical_compare_three_way(firstCodePoints.begin(), firstCodePoints.end(), secondCodePoints.begin(), secondCodePoints.end());
}

ModuleTable::ModuleTable(const ProgramTables& tables)
    : m_tables(&tables)
    , m_modules(tables.modules())
    , m_requests(tables.moduleRequests())
    , m_exports(tables.moduleExports())
    , m_syntheticExports(tables.syntheticExports())
    , m_requiredNames(tables.requiredNames())
    , m_requireEdges(tables.requireEdges())
    , m_failures(tables.moduleFailures())
    , m_modulesByPath(tables.modulesByPath())
    , m_runtimeModules(tables.compiledRuntimeModules())
    , m_runtimeFunctions(tables.compiledRuntimeFunctions())
{
    RELEASE_ASSERT(!m_modules.empty());
}

std::span<const ModuleExportData> ModuleTable::exports(ModuleID module) const
{
    return listOf(m_exports, module, &ModuleData::firstExport);
}

const RequireEdgeData& ModuleTable::requireEdge(uint32_t edge) const
{
    RELEASE_ASSERT(edge < m_requireEdges.size());
    return m_requireEdges[edge];
}

std::span<const ModuleResetSlotData> ModuleTable::resetSlots(ModuleID module) const
{
    auto range = std::ranges::equal_range(m_tables->moduleResetSlots(), module.index(), { }, &ModuleResetSlotData::module);
    return { range.begin(), range.end() };
}

const ModuleFailureData& ModuleTable::failure(ModuleFailureID id) const
{
    RELEASE_ASSERT(id.index() < m_failures.size());
    return m_failures[id.index()];
}

std::optional<ModuleID> ModuleTable::findByPath(StringView publicPath) const
{
    auto pathOf = [&](uint32_t module) {
        return path(ModuleID(module));
    };
    auto found = std::ranges::lower_bound(m_modulesByPath, publicPath, [](StringView first, StringView second) {
        return compareCodePoints(first, second) < 0;
    }, pathOf);
    if (found == m_modulesByPath.end() || pathOf(*found) != publicPath)
        return std::nullopt;
    return ModuleID(*found);
}

bool ModuleTable::namesUncompiledCode(StringView specifier) const
{
    for (unsigned index = 0; index < size(); ++index) {
        ModuleID module(index);
        if (!data(module).isUncompiledCode())
            continue;
        StringView name = path(module);
        if (specifier == name || (specifier.startsWith(name) && specifier[name.length()] == '/'))
            return true;
    }
    return false;
}

std::optional<ModuleID> ModuleTable::findRuntimeSource(uint32_t registryID) const
{
    auto found = std::ranges::lower_bound(m_runtimeModules, registryID, { }, &CompiledRuntimeModuleData::registryID);
    if (found == m_runtimeModules.end() || found->registryID != registryID)
        return std::nullopt;
    return ModuleID(found->module);
}

std::optional<unsigned> ModuleTable::findExport(ModuleID module, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return std::nullopt;
    std::span<const ModuleExportData> entries = exports(module);
    StringView nameView { *name };
    auto compare = [&](const ModuleExportData& entry) {
        StringID string = ProgramStrings::identifierString(*m_tables, entry.name);
        ASSERT(string);
        return codePointCompare(ProgramStrings::viewInImage(*m_tables, string), nameView);
    };
    unsigned from = 0;
    unsigned to = static_cast<unsigned>(entries.size());
    while (from < to) {
        unsigned middle = from + (to - from) / 2;
        auto order = compare(entries[middle]);
        if (order == std::strong_ordering::equal)
            return middle;
        if (order < 0)
            from = middle + 1;
        else
            to = middle;
    }
    return std::nullopt;
}

std::optional<uint32_t> ModuleTable::findRequireEdge(ModuleID referrer, ModuleID target) const
{
    std::pair<uint32_t, uint32_t> edge { referrer.index(), target.index() };
    auto endpointsOf = [](const RequireEdgeData& candidate) {
        return std::pair<uint32_t, uint32_t> { candidate.referrer, candidate.target };
    };
    auto found = std::ranges::lower_bound(m_requireEdges, edge, { }, endpointsOf);
    if (found == m_requireEdges.end() || endpointsOf(*found) != edge)
        return std::nullopt;
    return static_cast<uint32_t>(found - m_requireEdges.begin());
}

const CompiledRuntimeFunctionData* ModuleTable::findRuntimeFunction(uint32_t id) const
{
    auto found = std::ranges::lower_bound(m_runtimeFunctions, id, { }, &CompiledRuntimeFunctionData::id);
    return found != m_runtimeFunctions.end() && found->id == id ? &*found : nullptr;
}

StringView ModuleTable::path(ModuleID module) const
{
    return ProgramStrings::viewInImage(*m_tables, StringID(data(module).publicPath));
}

String ModuleTable::pathText(ModuleID module) const
{
    return ProgramStrings::text(*m_tables, StringID(data(module).publicPath));
}

bool ModuleTable::hasFilePath(ModuleID module) const
{
    switch (data(module).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        return true;
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ModuleRequestWalk::ModuleRequestWalk(const ModuleTable& table, ModuleID root)
    : m_table(table)
    , m_visited(table.size())
    , m_onPath(table.size())
{
    enter(root);
}

void ModuleRequestWalk::enter(ModuleID module)
{
    ASSERT(!hasVisited(module));
    m_visited.set(module.index());
    m_onPath.set(module.index());
    m_path.append({ module, 0 });
}

ModuleRequestWalk::Step ModuleRequestWalk::next()
{
    if (m_path.isEmpty())
        return { StepKind::Done, { }, nullptr };

    Frame& frame = m_path.last();
    std::span<const ModuleRequestData> requests = m_table.requests(frame.module);
    if (frame.nextRequest == requests.size()) {
        ModuleID module = frame.module;
        m_onPath.clear(module.index());
        m_path.removeLast();
        return { StepKind::Leave, module, nullptr };
    }
    return { StepKind::Request, frame.module, &requests[frame.nextRequest++] };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
