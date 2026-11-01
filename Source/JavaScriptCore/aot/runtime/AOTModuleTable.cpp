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
#include <functional>
#include <ranges>
#include <utility>
#include <wtf/text/MakeString.h>
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

ModuleTable::ModuleTable(const ProgramTables& tables, const TextOfString& text)
    : ModuleTable(tables)
{
    m_textWhereNoImageRuns = &text;
}

StringView ModuleTable::viewOfPathPiece(StringID piece) const
{
    return m_textWhereNoImageRuns ? (*m_textWhereNoImageRuns)(piece) : ProgramStrings::viewInImage(*m_tables, piece);
}

const ModuleFailureData& ModuleTable::failure(ModuleFailureID id) const
{
    std::span<const ModuleFailureData> failures = m_tables->moduleFailures();
    RELEASE_ASSERT(id.index() < failures.size());
    return failures[id.index()];
}

std::optional<ModuleFailureID> ModuleTable::loadFailure(ModuleID module) const
{
    std::span<const ModuleFailureData> failures = m_tables->moduleFailures();
    if (failures.empty() || !failures.back().isLoadOfModule()) [[likely]]
        return std::nullopt;
    auto loads = std::ranges::subrange(std::ranges::partition_point(failures, std::not_fn(&ModuleFailureData::isLoadOfModule)), failures.end());
    auto found = std::ranges::lower_bound(loads, module.index(), { }, [](const ModuleFailureData& load) { return load.importerModule().index(); });
    if (found == loads.end() || found->importerModule() != module)
        return std::nullopt;
    return found->failureOfLoad();
}

std::span<const AmbiguousExportData> ModuleTable::ambiguousExports(ModuleID module) const
{
    auto ofModule = std::ranges::equal_range(m_tables->ambiguousExports(), module.index(), { }, &AmbiguousExportData::module);
    return { ofModule.begin(), ofModule.end() };
}

bool ModuleTable::isAmbiguousExport(ModuleID module, StringView name) const
{
    return std::ranges::any_of(ambiguousExports(module), [&](const AmbiguousExportData& entry) {
        return ProgramStrings::text(*m_tables, entry.nameString()) == name;
    });
}

std::optional<uint64_t> ModuleTable::sourceStamp(ModuleID module) const
{
    std::span<const SourceStampData> stamps = m_tables->sourceStamps();
    auto found = std::ranges::lower_bound(stamps, module.index(), { }, &SourceStampData::module);
    return found != stamps.end() && found->module == module.index() ? std::optional(found->stamp()) : std::nullopt;
}

static std::atomic<uint32_t>& moduleFoundLast(unsigned hashOfPath)
{
    static std::array<std::atomic<uint32_t>, 256> modulesFoundLast;
    return modulesFoundLast[hashOfPath % modulesFoundLast.size()];
}

void ModuleTable::willBeAskedForPath(ModuleID module, unsigned hashOfPath) const
{
    moduleFoundLast(hashOfPath).store(module.index(), std::memory_order_relaxed);
}

std::optional<ModuleID> ModuleTable::findByPath(StringView publicPath) const
{
    std::atomic<uint32_t>& foundLast = moduleFoundLast(publicPath.hash());
    uint32_t found = foundLast.load(std::memory_order_relaxed);
    if (found >= size() || comparePath(ModuleID(found), publicPath) != 0) {
        ModuleNumbers byPath = modulesByPath();
        auto places = std::views::iota(size_t(0), byPath.size());
        size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
            return comparePath(ModuleID(byPath[candidate]), publicPath) < 0;
        }) - places.begin());
        if (place == byPath.size() || comparePath(ModuleID(byPath[place]), publicPath) != 0)
            return std::nullopt;
        found = byPath[place];
        foundLast.store(found, std::memory_order_relaxed);
    }
    std::optional<uint32_t> registryID = data(ModuleID(found)).registryID();
    if (registryID && !BuiltinTable::singleton().at(RuntimeModuleID(*registryID)).flags.contains(RuntimeModuleFlag::IsPublic))
        return std::nullopt;
    return ModuleID(found);
}

bool ModuleTable::holdsFormThatAttributeAsksFor(StringView publicPath) const
{
    String start = makeString(publicPath, '?');
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), start) < 0;
    }) - places.begin());
    return place < byPath.size() && pathText(ModuleID(byPath[place])).startsWith(start);
}

Vector<ModuleID, 2> ModuleTable::formsThatAttributesAskFor(StringView publicPath) const
{
    String start = makeString(publicPath, '?');
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), start) < 0;
    }) - places.begin());
    Vector<ModuleID, 2> forms;
    for (; place < byPath.size() && pathText(ModuleID(byPath[place])).startsWith(start); ++place)
        forms.append(ModuleID(byPath[place]));
    return forms;
}

bool ModuleTable::holdsPathStartingWith(StringView start) const
{
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), start) < 0;
    }) - places.begin());
    return place < byPath.size() && pathText(ModuleID(byPath[place])).startsWith(start);
}

bool ModuleTable::mayHoldPathStartingWith(StringView start) const
{
    std::span<const uint32_t> bits = m_tables->directoriesOfModulesByPath();
    size_t slash = start.reverseFind('/');
    if (bits.empty() || slash == notFound)
        return true;
    uint32_t hash = HashOfDirectory::ofNoText;
    for (char16_t character : start.left(slash + 1).codeUnits())
        hash = HashOfDirectory::with(hash, character);
    size_t bit = HashOfDirectory::bit(hash, bits.size() * 32);
    return bits[bit / 32] >> (bit % 32) & 1;
}

bool Image::mayHoldModulePathStartingWith(StringView start) const
{
    ModuleTable table(programTables());
    bool mayHold = table.mayHoldPathStartingWith(start);
    ASSERT(mayHold || !table.holdsPathStartingWith(start));
    return mayHold;
}

std::span<const ImportOfHeldFileData> ModuleTable::importsOfHeldFile(ModuleID file) const
{
    std::span<const ImportOfHeldFileData> imports = m_tables->importsOfHeldFiles();
    auto found = std::ranges::equal_range(imports, file.index(), { }, &ImportOfHeldFileData::file);
    return { found.begin(), found.end() };
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

static bool spellOneText(std::span<const StringView> first, std::span<const StringView> second)
{
    StringView restOfFirst;
    StringView restOfSecond;
    while (true) {
        while (restOfFirst.isEmpty() && !first.empty()) {
            restOfFirst = first.front();
            first = first.subspan(1);
        }
        while (restOfSecond.isEmpty() && !second.empty()) {
            restOfSecond = second.front();
            second = second.subspan(1);
        }
        if (restOfFirst.isEmpty() || restOfSecond.isEmpty())
            return restOfFirst.isEmpty() && restOfSecond.isEmpty();
        unsigned length = std::min(restOfFirst.length(), restOfSecond.length());
        if (restOfFirst.left(length) != restOfSecond.left(length))
            return false;
        restOfFirst = restOfFirst.substring(length);
        restOfSecond = restOfSecond.substring(length);
    }
}

// The further modules of a file follow the file.
std::optional<ModuleID> ModuleTable::faceOfFile(ModuleID file) const
{
    if (!data(file).hasFace())
        return std::nullopt;
    for (unsigned index = file.index() + 1; index < size(); ++index) {
        ModuleID module(index);
        if (fileBehindFace(module) == file)
            return module;
        if (!spellOneText(piecesOfDisplayPath(file), piecesOfDisplayPath(module)))
            break;
    }
    return std::nullopt;
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
    pieces.append(viewOfPathPiece(StringID(data(module).publicPathOrName)));
    std::span<const ModuleDirectoryData> directories = m_tables->moduleDirectories();
    for (std::optional<uint32_t> directory = optionalIDInImage<uint32_t>(data(module).directory); directory; directory = directories[*directory].parentDirectory())
        pieces.append(viewOfPathPiece(StringID(directories[*directory].name)));
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
    if (data(module).directory == Number24::max && !m_textWhereNoImageRuns)
        return ProgramStrings::text(*m_tables, StringID(data(module).publicPathOrName));
    PiecesOfPath pieces = piecesOfPath(module);
    unsigned length = 0;
    for (StringView piece : pieces)
        length += piece.length();
    StringBuilder path;
    path.reserveCapacity(length);
    for (StringView piece : pieces)
        path.append(piece);
    return path.toString();
}

// The text that displayPathText() makes.
ModuleTable::PiecesOfPath ModuleTable::piecesOfDisplayPath(ModuleID module) const
{
    unsigned charactersInFront = data(module).charactersInFrontOfDisplayPath;
    if (charactersInFront == ModuleData::hasNoDisplayPath)
        return { };
    if (charactersInFront == ModuleData::displayPathIsListed) {
        auto listed = std::ranges::lower_bound(m_tables->moduleDisplayPaths(), module.index(), { }, [](const ModuleDisplayPathData& entry) -> uint32_t { return entry.module; });
        return { ProgramStrings::viewInImage(*m_tables, StringID(listed->path)) };
    }
    PiecesOfPath pieces = piecesOfPath(module);
    for (StringView& piece : pieces) {
        unsigned dropped = std::min(charactersInFront, piece.length());
        piece = piece.substring(dropped);
        charactersInFront -= dropped;
        if (!charactersInFront)
            break;
    }
    return pieces;
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

String ModuleTable::typeAttribute(ModuleID module) const
{
    ASSERT(data(module).isUncompiledCode());
    auto listed = std::ranges::lower_bound(modulesOfUncompiledCode(), module.index(), { }, &UncompiledModuleData::module);
    return listed->typeAttribute == UncompiledModuleData::noTypeAttribute ? String() : ProgramStrings::text(*m_tables, StringID(listed->typeAttribute));
}

String ModuleTable::pathTextForLineColumns(ModuleID module) const
{
    return data(module).hasInstalledPositions ? pathText(module) : displayPathText(module);
}

// The name of a file ends its public path, whether or not the directory stands apart.
IterationStatus ModuleTable::forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>& functor) const
{
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), start) < 0;
    }) - places.begin());
    for (; place < byPath.size(); ++place) {
        ModuleID module(byPath[place]);
        String path = pathText(module);
        if (!path.startsWith(start))
            break;
        bool isCode = data(module).kind == ModuleKind::SourceText || data(module).kind == ModuleKind::CommonJs;
        if (isCode && !data(module).registryID() && !data(module).evaluatorModule() && functor(path) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

bool ModuleTable::holdsFileAt(StringView path) const
{
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), path) < 0;
    }) - places.begin());
    if (place == byPath.size() || comparePath(ModuleID(byPath[place]), path) != 0)
        return false;
    const ModuleData& found = data(ModuleID(byPath[place]));
    bool isFile = found.kind == ModuleKind::SourceText || found.kind == ModuleKind::CommonJs || found.kind == ModuleKind::Data;
    return isFile && !found.registryID() && !found.evaluatorModule();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
