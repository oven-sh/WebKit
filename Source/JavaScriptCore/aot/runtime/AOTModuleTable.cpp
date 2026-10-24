/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTProgramStrings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <utility>
#include <wtf/text/StringBuilder.h>

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
    , m_linkRequests(tables.linkRequests(), tables.modules().size())
    , m_evaluationRequests(tables.evaluationRequests(), tables.modules().size())
    , m_exports(tables.moduleExports())
    , m_syntheticExports(tables.syntheticExports())
    , m_requiredNames(tables.requiredNames())
    , m_failures(tables.moduleFailures())
    , m_modulesByPath(tables.modulesByPath(), tables.modules().size())
    , m_runtimeModules(tables.compiledRuntimeModules())
    , m_runtimeFunctions(tables.compiledRuntimeFunctions())
{
    RELEASE_ASSERT(!m_modules.empty());
}

std::span<const ModuleExportData> ModuleTable::exports(ModuleID module) const
{
    return listOf(m_exports, module, &ModuleData::firstExport);
}

const ModuleFailureData& ModuleTable::failure(ModuleFailureID id) const
{
    RELEASE_ASSERT(id.index() < m_failures.size());
    return m_failures[id.index()];
}

std::optional<ModuleID> ModuleTable::findPlaceOfPath(StringView publicPath) const
{
    static std::array<std::atomic<uint32_t>, 256> placesFoundLast;
    std::atomic<uint32_t>& placeFoundLast = placesFoundLast[publicPath.hash() % placesFoundLast.size()];
    size_t from = placeFoundLast.load(std::memory_order_relaxed);
    if (from < m_modulesByPath.size() && comparePath(ModuleID(m_modulesByPath[from]), publicPath) == 0)
        return ModuleID(m_modulesByPath[from]);

    from = 0;
    size_t to = m_modulesByPath.size();
    while (from < to) {
        size_t middle = from + (to - from) / 2;
        if (comparePath(ModuleID(m_modulesByPath[middle]), publicPath) < 0)
            from = middle + 1;
        else
            to = middle;
    }
    if (from == m_modulesByPath.size())
        return std::nullopt;
    ModuleID found(m_modulesByPath[from]);
    if (comparePath(found, publicPath) != 0)
        return std::nullopt;
    placeFoundLast.store(static_cast<uint32_t>(from), std::memory_order_relaxed);
    return found;
}

std::optional<ModuleID> ModuleTable::findByPath(StringView publicPath) const
{
    std::optional<ModuleID> place = findPlaceOfPath(publicPath);
    if (!place)
        return std::nullopt;
    ModuleID found = *place;
    std::optional<uint32_t> registryID = data(found).registryID();
    if (registryID && !BuiltinTable::singleton().at(RuntimeModuleID(*registryID)).flags.contains(RuntimeModuleFlag::IsPublic))
        return std::nullopt;
    return found;
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

std::optional<ModuleID> ModuleTable::fileBehindFace(ModuleID module) const
{
    const ModuleData& face = data(module);
    std::span<const ModuleRequestData> requestsOfFace = requests(module);
    if (face.kind != ModuleKind::SourceText || requestsOfFace.empty() || !requestsOfFace.front().isRequire)
        return std::nullopt;
    ModuleID file = requestsOfFace.front().targetModule();
    if (data(file).kind != ModuleKind::CommonJs || displayPathText(file) != displayPathText(module))
        return std::nullopt;
    return file;
}

auto ModuleTable::linkPlanOfRoot(ModuleID module) const -> std::optional<LinkPlanOfRoot>
{
    std::span<const LinkPlanRootData> roots = m_tables->linkPlanRoots();
    auto found = std::ranges::lower_bound(roots, module.index(), { }, &LinkPlanRootData::module);
    if (found == roots.end() || found->module != module.index() || found + 1 == roots.end())
        return std::nullopt;
    return LinkPlanOfRoot { m_tables->linkPlanStepsOfRoots().subspan(found->firstStep, (found + 1)->firstStep - found->firstStep), ModuleID(found->memberOfCycle) };
}

const CompiledRuntimeFunctionData* ModuleTable::findRuntimeFunction(uint32_t id) const
{
    auto found = std::ranges::lower_bound(m_runtimeFunctions, id, { }, &CompiledRuntimeFunctionData::id);
    return found != m_runtimeFunctions.end() && found->id == id ? &*found : nullptr;
}

bool ModuleTable::isRuntimeFunction(FunctionID function) const
{
    return std::ranges::contains(m_runtimeFunctions, function.index(), &CompiledRuntimeFunctionData::function);
}

// From the start of the path.
ModuleTable::PiecesOfPath ModuleTable::piecesOfPath(ModuleID module) const
{
    PiecesOfPath pieces;
    pieces.append(ProgramStrings::viewInImage(*m_tables, StringID(data(module).publicPathOrName)));
    std::span<const ModuleDirectoryData> directories = m_tables->moduleDirectories();
    for (std::optional<uint32_t> directory = optionalIDInImage<uint32_t>(data(module).directory); directory; directory = directories[*directory].parentDirectory())
        pieces.append(ProgramStrings::viewInImage(*m_tables, StringID(directories[*directory].name)));
    pieces.reverse();
    return pieces;
}

// A piece ends with a slash or with the path, so none ends inside a surrogate pair.
std::strong_ordering ModuleTable::comparePath(ModuleID module, StringView other) const
{
    unsigned position = 0;
    for (StringView piece : piecesOfPath(module)) {
        if (std::strong_ordering order = compareCodePoints(piece, other.substring(position, piece.length())); order != 0)
            return order;
        position += piece.length();
    }
    return position == other.length() ? std::strong_ordering::equal : std::strong_ordering::less;
}

String ModuleTable::pathText(ModuleID module) const
{
    if (data(module).directory == Number24::max)
        return ProgramStrings::text(*m_tables, StringID(data(module).publicPathOrName));
    StringBuilder path;
    for (StringView piece : piecesOfPath(module))
        path.append(piece);
    return path.toString();
}

String ModuleTable::displayPathText(ModuleID module) const
{
    uint8_t charactersInFront = data(module).charactersInFrontOfDisplayPath;
    if (charactersInFront == ModuleData::hasNoDisplayPath)
        return emptyString();
    if (charactersInFront == ModuleData::displayPathIsListed) {
        auto listed = std::ranges::lower_bound(m_tables->moduleDisplayPaths(), module.index(), { }, [](const ModuleDisplayPathData& entry) -> uint32_t { return entry.module; });
        return ProgramStrings::text(*m_tables, StringID(listed->path));
    }
    String path = pathText(module);
    return charactersInFront ? path.substring(charactersInFront) : path;
}

String ModuleTable::pathTextForLineColumns(ModuleID module) const
{
    return data(module).hasInstalledPositions ? pathText(module) : displayPathText(module);
}

// The name of a file ends its public path, whether or not the directory stands apart.
bool ModuleTable::hasFileWithExtension(StringView extension) const
{
    for (size_t index = 0; index < m_modulesByPath.size(); ++index) {
        if (ProgramStrings::viewInImage(*m_tables, StringID(m_modules[m_modulesByPath[index]].publicPathOrName)).endsWith(extension))
            return true;
    }
    return false;
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
