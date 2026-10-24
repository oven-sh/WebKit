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
#include <ranges>
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
{
    RELEASE_ASSERT(!m_modules.empty());
}

const ModuleFailureData& ModuleTable::failure(ModuleFailureID id) const
{
    std::span<const ModuleFailureData> failures = m_tables->moduleFailures();
    RELEASE_ASSERT(id.index() < failures.size());
    return failures[id.index()];
}

std::optional<ModuleID> ModuleTable::findByPath(StringView publicPath) const
{
    static std::array<std::atomic<uint32_t>, 256> placesFoundLast;
    std::atomic<uint32_t>& placeFoundLast = placesFoundLast[publicPath.hash() % placesFoundLast.size()];
    ModuleNumbers byPath = modulesByPath();
    size_t place = placeFoundLast.load(std::memory_order_relaxed);
    if (place >= byPath.size() || comparePath(ModuleID(byPath[place]), publicPath) != 0) {
        auto places = std::views::iota(size_t(0), byPath.size());
        place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
            return comparePath(ModuleID(byPath[candidate]), publicPath) < 0;
        }) - places.begin());
        if (place == byPath.size() || comparePath(ModuleID(byPath[place]), publicPath) != 0)
            return std::nullopt;
        placeFoundLast.store(static_cast<uint32_t>(place), std::memory_order_relaxed);
    }
    ModuleID found(byPath[place]);
    std::optional<uint32_t> registryID = data(found).registryID();
    if (registryID && !BuiltinTable::singleton().at(RuntimeModuleID(*registryID)).flags.contains(RuntimeModuleFlag::IsPublic))
        return std::nullopt;
    return found;
}

std::optional<ModuleID> ModuleTable::findRuntimeSource(uint32_t registryID) const
{
    std::span<const CompiledRuntimeModuleData> runtimeModules = m_tables->compiledRuntimeModules();
    auto found = std::ranges::lower_bound(runtimeModules, registryID, { }, &CompiledRuntimeModuleData::registryID);
    if (found == runtimeModules.end() || found->registryID != registryID)
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
    std::span<const CompiledRuntimeFunctionData> runtimeFunctions = m_tables->compiledRuntimeFunctions();
    auto found = std::ranges::lower_bound(runtimeFunctions, id, { }, &CompiledRuntimeFunctionData::id);
    return found != runtimeFunctions.end() && found->id == id ? &*found : nullptr;
}

bool ModuleTable::isRuntimeFunction(FunctionID function) const
{
    return std::ranges::contains(m_tables->compiledRuntimeFunctions(), function.index(), &CompiledRuntimeFunctionData::function);
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
    return std::ranges::any_of(modulesByPath(), [&](uint32_t module) {
        return ProgramStrings::viewInImage(*m_tables, StringID(m_modules[module].publicPathOrName)).endsWith(extension);
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
