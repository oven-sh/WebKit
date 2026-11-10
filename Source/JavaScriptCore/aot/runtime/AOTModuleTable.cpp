/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTProgramStrings.h"
#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <array>
#include <compare>
#include <functional>
#include <ranges>
#include <utility>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

CyclesOfImports::CyclesOfImports(const ModuleTable& table)
{
    struct ImportTargets {
        std::span<const ModuleRequestData> requests;
        uint32_t importer;

        size_t size() const { return requests.size(); }
        uint32_t operator[](size_t index) const { return requests[index].isRequire || requests[index].isDeferred ? importer : requests[index].target(); }
    };
    Typed::StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(table.size(), [&](unsigned index) {
        return ImportTargets { table.requests(ModuleID(index)), index };
    });
    firstMemberOf.append(0);
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        std::span<const uint32_t> found = components.membersOf(component);
        if (found.size() < 2)
            continue;
        uint32_t cycle = firstMemberOf.size() - 1;
        for (uint32_t module : found) {
            membersByModule.append({ module, cycle, static_cast<uint32_t>(members.size() - firstMemberOf.last()) });
            members.append(module);
        }
        firstMemberOf.append(members.size());
    }
    std::ranges::sort(membersByModule, { }, &Member::module);
}

const CyclesOfImports::Member* CyclesOfImports::find(ModuleID module) const
{
    auto found = std::ranges::lower_bound(membersByModule, module.index(), { }, &Member::module);
    return found != membersByModule.end() && found->module == module.index() ? &*found : nullptr;
}

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

std::optional<uint64_t> ModuleTable::sourceStamp(ModuleID module) const
{
    std::span<const SourceStampData> stamps = m_tables->sourceStamps();
    auto found = std::ranges::lower_bound(stamps, module.index(), { }, &SourceStampData::module);
    if (found == stamps.end() || found->module != module.index())
        return std::nullopt;
    return found->stamp();
}

std::span<const NameOfStatementData> ModuleTable::namesOfStatements(ModuleID module) const
{
    auto ofModule = std::ranges::equal_range(m_tables->namesOfStatements(), module.index(), { }, &NameOfStatementData::module);
    return { ofModule.begin(), ofModule.end() };
}

std::span<const StarAsWrittenData> ModuleTable::starsAsWritten(ModuleID module) const
{
    auto ofModule = std::ranges::equal_range(m_tables->starsAsWritten(), module.index(), { }, &StarAsWrittenData::module);
    return { ofModule.begin(), ofModule.end() };
}

std::optional<ModuleID> ModuleTable::writerOfExport(ModuleID module, unsigned place) const
{
    auto key = [](const WriterOfExportData& entry) {
        return static_cast<uint64_t>(entry.module) << 32 | entry.place;
    };
    std::span<const WriterOfExportData> writers = m_tables->writersOfExports();
    auto found = std::ranges::lower_bound(writers, static_cast<uint64_t>(module.index()) << 32 | place, { }, key);
    if (found == writers.end() || found->module != module.index() || found->place != place)
        return std::nullopt;
    return ModuleID(found->writer);
}

bool ModuleTable::isAmbiguousExport(ModuleID module, StringView name) const
{
    return std::ranges::any_of(ambiguousExports(module), [&](const AmbiguousExportData& entry) {
        return ProgramStrings::text(*m_tables, entry.nameString()) == name;
    });
}

std::optional<ModuleID> ModuleTable::findByPath(StringView publicPath, unsigned hashOfPath) const
{
    std::span<const uint32_t> entries = m_tables->modulesByHashOfPath();
    if (entries.empty())
        return std::nullopt;
    ModuleByHashOfPath sought(m_modules.size(), hashOfPath);
    size_t bucket = sought.bucket(ModuleByHashOfPath::numberOfBuckets(entries.size()));
    ModuleNumbers ends = ModuleNumbers(m_tables->firstModulesByHashOfPath(), m_modules.size()).subspan(bucket, 2);
    for (uint32_t entry : entries.subspan(ends[0], ends[1] - ends[0])) {
        uint32_t module = sought.moduleOrMore(entry);
        if (!sought.isModule(module))
            continue;
        ModuleID found(module);
        if (comparePath(found, publicPath) != 0)
            continue;
        std::optional<uint32_t> registryID = data(found).registryID();
        if (registryID && !BuiltinTable::singleton().at(RuntimeModuleID(*registryID)).flags.contains(RuntimeModuleFlag::IsPublic))
            return std::nullopt;
        return found;
    }
    return std::nullopt;
}

bool ModuleTable::hasAttributeVariant(StringView publicPath) const
{
    String start = makeString(publicPath, '?');
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), start) < 0;
    }) - places.begin());
    return place < byPath.size() && fullPath(ModuleID(byPath[place])).startsWith(start);
}

Vector<ModuleID, 2> ModuleTable::importAttributeVariants(StringView publicPath) const
{
    String start = makeString(publicPath, '?');
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), start) < 0;
    }) - places.begin());
    Vector<ModuleID, 2> forms;
    for (; place < byPath.size() && fullPath(ModuleID(byPath[place])).startsWith(start); ++place)
        forms.append(ModuleID(byPath[place]));
    return forms;
}

bool ModuleTable::containsPathWithPrefix(StringView start) const
{
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), start) < 0;
    }) - places.begin());
    return place < byPath.size() && fullPath(ModuleID(byPath[place])).startsWith(start);
}

bool ModuleTable::mayContainPathWithPrefix(StringView start) const
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

bool Image::mayContainModulePathWithPrefix(StringView start) const
{
    ModuleTable table(programTables());
    bool mayHold = table.mayContainPathWithPrefix(start);
    ASSERT(mayHold || !table.containsPathWithPrefix(start));
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

bool ModuleTable::valueOfRuntimeSourceStaysWithCompiledCode(uint32_t registryID) const
{
    std::span<const CompiledRuntimeModuleData> runtimeModules = m_tables->compiledRuntimeModules();
    auto found = std::ranges::lower_bound(runtimeModules, registryID, { }, &CompiledRuntimeModuleData::registryID);
    return found != runtimeModules.end() && found->registryID == registryID && found->valueStaysWithCompiledCode;
}

bool ModuleTable::recordOfRuntimeSourceIsHandedOutFrozen(uint32_t registryID) const
{
    std::span<const CompiledRuntimeModuleData> runtimeModules = m_tables->compiledRuntimeModules();
    auto found = std::ranges::lower_bound(runtimeModules, registryID, { }, &CompiledRuntimeModuleData::registryID);
    return found != runtimeModules.end() && found->registryID == registryID && found->recordIsHandedOutFrozen;
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

std::optional<ModuleID> ModuleTable::facadeOf(ModuleID file) const
{
    if (!data(file).hasFacade())
        return std::nullopt;
    for (unsigned distance = 1; distance < size(); ++distance) {
        ModuleID module((file.index() + distance) % size());
        if (moduleBehindFacade(module) == file)
            return module;
    }
    RELEASE_ASSERT_NOT_REACHED();
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

std::strong_ordering ModuleTable::comparePath(ModuleID module, StringView other) const
{
    if (const ModuleData& moduleData = data(module); !optionalIDInImage<uint32_t>(moduleData.directory)) {
        StringView name = ProgramStrings::viewInImage(*m_tables, StringID(moduleData.publicPathOrName));
        if (std::strong_ordering order = compareCodePoints(name, other.substring(0, name.length())); order != 0)
            return order;
        return name.length() == other.length() ? std::strong_ordering::equal : std::strong_ordering::less;
    }
    unsigned position = 0;
    for (StringView piece : piecesOfPath(module)) {
        if (std::strong_ordering order = compareCodePoints(piece, other.substring(position, piece.length())); order != 0)
            return order;
        position += piece.length();
    }
    return position == other.length() ? std::strong_ordering::equal : std::strong_ordering::less;
}

ModuleTable::PathInUTF8 ModuleTable::pathInUTF8(ModuleID module) const
{
    PathInUTF8 path;
    auto append = [&](StringView piece) {
        if (piece.is8Bit() && charactersAreAllASCII(piece.span8()))
            path.append(byteCast<char>(piece.span8()));
        else
            path.append(piece.utf8().span());
    };
    if (data(module).directory == Number24::max)
        append(ProgramStrings::viewInImage(*m_tables, StringID(data(module).publicPathOrName)));
    else {
        for (StringView piece : piecesOfPath(module))
            append(piece);
    }
    return path;
}

String ModuleTable::fullPath(ModuleID module) const
{
    if (data(module).directory == Number24::max)
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

String ModuleTable::displayPathText(ModuleID module) const
{
    uint8_t charactersInFront = data(module).charactersInFrontOfDisplayPath;
    if (charactersInFront == ModuleData::hasNoDisplayPath)
        return emptyString();
    if (charactersInFront == ModuleData::displayPathIsListed) {
        auto listed = std::ranges::lower_bound(m_tables->moduleDisplayPaths(), module.index(), { }, [](const ModuleDisplayPathData& entry) -> uint32_t { return entry.module; });
        return ProgramStrings::text(*m_tables, StringID(listed->path));
    }
    String path = fullPath(module);
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
    return data(module).hasInstalledPositions ? fullPath(module) : displayPathText(module);
}

IterationStatus ModuleTable::forEachFilePathWithPrefix(StringView start, const ScopedLambda<IterationStatus(const String&)>& functor) const
{
    ModuleNumbers byPath = modulesByPath();
    auto places = std::views::iota(size_t(0), byPath.size());
    size_t place = static_cast<size_t>(std::ranges::partition_point(places, [&](size_t candidate) {
        return comparePath(ModuleID(byPath[candidate]), start) < 0;
    }) - places.begin());
    for (; place < byPath.size(); ++place) {
        ModuleID module(byPath[place]);
        String path = fullPath(module);
        if (!path.startsWith(start))
            break;
        bool isCode = data(module).kind == ModuleKind::SourceText || data(module).kind == ModuleKind::CommonJs;
        if (isCode && !data(module).registryID() && !data(module).evaluatorModule() && functor(path) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
