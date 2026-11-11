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

ImportersOfModules::ImportersOfModules(const ModuleTable& table)
    : namedByListsAlone(table.size())
{
    auto forEachRequest = [&](auto&& functor) {
        for (uint32_t importer = 0; importer < table.size(); ++importer) {
            for (const ModuleRequestData& request : table.requests(ModuleID(importer))) {
                if (!request.isHoisted)
                    functor(importer, request.target());
            }
        }
    };
    firstImporterOf.fill(0, table.size() + 1);
    forEachRequest([&](uint32_t, uint32_t imported) {
        ++firstImporterOf[imported];
    });
    for (uint32_t module = 0; module < table.size(); ++module)
        firstImporterOf[module + 1] += firstImporterOf[module];
    importers.fill(0, firstImporterOf[table.size()]);
    forEachRequest([&](uint32_t importer, uint32_t imported) {
        importers[--firstImporterOf[imported]] = importer;
    });

    BitVector isAskedFor(table.size());
    for (const ModuleRequestData& request : table.allRequests()) {
        if (!request.isHoisted || table.data(request.targetModule()).kind == ModuleKind::SourceText)
            isAskedFor.quickSet(request.target());
    }
    for (uint32_t module = 0; module < table.size(); ++module)
        namedByListsAlone.quickSet(module, !isAskedFor.quickGet(module));
}

void ImportersOfModules::addModulesAbove(Vector<ModuleID, 32>&& modules, BitVector& found) const
{
    while (!modules.isEmpty()) {
        uint32_t module = modules.takeLast().index();
        if (found.quickSet(module))
            continue;
        for (uint32_t position = firstImporterOf[module]; position < firstImporterOf[module + 1]; ++position)
            modules.append(ModuleID(importers[position]));
    }
}

ReexportedNames::ReexportedNames(const ModuleTable& table)
    : m_answers(table.programTables().requiredNames().size())
    , m_seen(table.size())
    , m_modulesWithNamesInTables(table.size())
    , m_modulesWithOtherNames(table.size())
{
    Vector<std::pair<uint32_t, uint32_t>> edges;
    for (uint32_t index = 0; index < table.size(); ++index) {
        ModuleID reexporter(index);
        bool hasExportOfOtherModule = table.data(reexporter).hasDynamicStarExport();
        for (const ModuleExportData& entry : table.exports(reexporter)) {
            hasExportOfOtherModule |= entry.isFromStarExport;
            switch (entry.kind()) {
            case ModuleExportKind::Variable:
                break;
            case ModuleExportKind::Namespace:
                edges.append({ entry.targetModule().index(), index });
                break;
            case ModuleExportKind::Indirect:
                edges.append({ table.indirectExport(entry).targetModule().index(), index });
                break;
            }
        }
        for (const RequiredNameData& requiredName : table.requiredNames(reexporter)) {
            if (table.findExportStatement(requiredName))
                edges.append({ requiredName.exporterModule().index(), index });
        }
        for (const StarAsWrittenData& star : table.starsAsWritten(reexporter))
            edges.append({ star.target, index });
        if (!hasExportOfOtherModule)
            continue;
        for (const ModuleRequestData& request : table.requests(reexporter)) {
            if (!request.isRequire)
                edges.append({ request.target(), index });
        }
    }
    std::ranges::sort(edges);
    edges.shrink(std::ranges::unique(edges).begin() - edges.begin());
    m_reexporters = edges.map([](auto& edge) { return edge.second; });
    size_t next = 0;
    for (uint32_t index = 0; index <= table.size(); ++index) {
        while (next < edges.size() && edges[next].first < index)
            ++next;
        m_firstReexporterOf.append(next);
    }
}

BitVector ReexportedNames::transitiveReexportersOf(Vector<ModuleID, 16>&& modules) const
{
    BitVector found(m_firstReexporterOf.size() - 1);
    while (!modules.isEmpty()) {
        uint32_t index = modules.takeLast().index();
        if (found.quickSet(index))
            continue;
        for (uint32_t place = m_firstReexporterOf[index]; place < m_firstReexporterOf[index + 1]; ++place)
            modules.append(ModuleID(m_reexporters[place]));
    }
    return found;
}

static bool hasNamesInTables(const ModuleData& data)
{
    return data.hasExportTable() && !data.capturesExports() && !data.isUncompiledCode();
}

bool ReexportedNames::areNamesBehindStarsInTables(const ModuleTable& table, ModuleID module)
{
    if (m_modulesWithNamesInTables.quickGet(module.index()))
        return true;
    if (m_modulesWithOtherNames.quickGet(module.index()))
        return false;
    BitVector seen(table.size());
    Vector<ModuleID, 8> toVisit { module };
    while (!toVisit.isEmpty()) {
        ModuleID visited = toVisit.takeLast();
        if (seen.quickSet(visited.index()) || m_modulesWithNamesInTables.quickGet(visited.index()))
            continue;
        if (!hasNamesInTables(table.data(visited)) || m_modulesWithOtherNames.quickGet(visited.index())) {
            m_modulesWithOtherNames.quickSet(module.index());
            return false;
        }
        for (ModuleRequestData request : table.dynamicStars(visited))
            toVisit.append(request.targetModule());
    }
    m_modulesWithNamesInTables.merge(seen);
    return true;
}

static bool haveSameBinding(const ModuleExportData& a, const ModuleExportData& b)
{
    if (a.kind() != b.kind())
        return false;
    switch (a.kind()) {
    case ModuleExportKind::Variable:
        return a.variable == b.variable;
    case ModuleExportKind::Namespace:
        return a.module == b.module && a.isDeferred == b.isDeferred;
    case ModuleExportKind::Indirect:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint64_t keyOf(ModuleID exporter, uint32_t name)
{
    return static_cast<uint64_t>(exporter.index()) << 32 | name;
}

static bool hasDynamicStars(const ModuleTable& table, ModuleID module)
{
    for ([[maybe_unused]] ModuleRequestData request : table.dynamicStars(module))
        return true;
    return false;
}

static bool isAmbiguousName(const ModuleTable& table, ModuleID exporter, uint32_t name)
{
    return table.isAmbiguousExport(exporter, ProgramStrings::viewInImage(table.programTables(), ProgramStrings::identifierString(table.programTables(), name)));
}

auto ReexportedNames::starsOf(const ModuleTable& table, ModuleID exporter) -> const Stars&
{
    std::unique_ptr<Stars>& stars = m_stars.add(exporter.index(), nullptr).iterator->value;
    if (stars)
        return *stars;
    stars = makeUnique<Stars>();
    for (ModuleRequestData request : table.dynamicStars(exporter)) {
        ModuleID target = request.targetModule();
        if (hasDynamicStars(table, target)) {
            stars->reexporters.append(target);
            continue;
        }
        stars->listsEveryName &= table.ambiguousExports(target).empty();
        for (const ModuleExportData& entry : table.exports(target)) {
            bool isBinding = entry.kind() != ModuleExportKind::Indirect && !entry.isHiddenUntilInitialized;
            auto added = stars->bindings.add(entry.name, isBinding ? &entry : nullptr);
            if (added.isNewEntry)
                continue;
            const ModuleExportData*& binding = added.iterator->value;
            if (binding && !(isBinding && haveSameBinding(*binding, entry)))
                binding = nullptr;
        }
    }
    return *stars;
}

auto ReexportedNames::resolveHere(const ModuleTable& table, ModuleID exporter, uint32_t name, const Stars& stars) -> Resolution
{
    const ModuleExportData* found = nullptr;
    if (std::optional<unsigned> index = table.findExport(exporter, name)) {
        const ModuleExportData& entry = table.exports(exporter)[*index];
        if (entry.isHiddenUntilInitialized)
            return { };
        if (entry.kind() == ModuleExportKind::Indirect) {
            const IndirectExportData& binding = table.indirectExport(entry);
            if (binding.importName != name || !areNamesBehindStarsInTables(table, binding.targetModule()))
                return { };
            Resolution resolution = resolve(table, binding.targetModule(), name);
            return resolution.answer == Answer::Found ? resolution : Resolution { };
        }
        found = &entry;
        if (!entry.isFromStarExport)
            return { Answer::Found, found };
    } else if (isAmbiguousName(table, exporter, name))
        return { };

    auto add = [&](Resolution resolution) {
        if (resolution.answer == Answer::Unknown || (found && resolution.binding && !haveSameBinding(*found, *resolution.binding)))
            return false;
        if (resolution.binding)
            found = resolution.binding;
        return true;
    };
    auto binding = stars.bindings.find(name);
    bool isInBindings = binding != stars.bindings.end();
    if (stars.listsEveryName && (!isInBindings || binding->value)) {
        if (isInBindings && !add({ Answer::Found, binding->value }))
            return { };
    } else {
        for (ModuleRequestData request : table.dynamicStars(exporter)) {
            if (ModuleID target = request.targetModule(); !hasDynamicStars(table, target) && !add(resolve(table, target, name)))
                return { };
        }
    }
    for (ModuleID reexporter : stars.reexporters) {
        if (!add(resolve(table, reexporter, name)))
            return { };
    }
    return { found ? Answer::Found : Answer::NotFound, found };
}

auto ReexportedNames::resolve(const ModuleTable& table, ModuleID exporter, uint32_t name) -> Resolution
{
    Vector<ModuleID, 8> modulesWithSameAnswer;
    Resolution resolution;
    while (true) {
        if (auto known = m_resolutions.find(keyOf(exporter, name)); known != m_resolutions.end()) {
            resolution = known->value;
            break;
        }
        if (m_seen.quickSet(exporter.index())) {
            m_isOnCycle = true;
            return { Answer::NotFound, nullptr };
        }
        m_visited.append(exporter);
        modulesWithSameAnswer.append(exporter);
        const Stars& stars = starsOf(table, exporter);
        bool hasAnswerOfReexporter = stars.reexporters.size() == 1 && stars.listsEveryName && !stars.bindings.contains(name)
            && !table.findExport(exporter, name) && !isAmbiguousName(table, exporter, name);
        if (!hasAnswerOfReexporter) {
            resolution = resolveHere(table, exporter, name, stars);
            break;
        }
        exporter = stars.reexporters[0];
    }
    if (!m_isOnCycle) {
        for (ModuleID module : modulesWithSameAnswer)
            m_resolutions.add(keyOf(module, name), resolution);
    }
    return resolution;
}

auto ReexportedNames::resolve(const ModuleTable& table, const RequiredNameData& requiredName) -> Answer
{
    std::optional<Answer>& answer = m_answers[&requiredName - table.programTables().requiredNames().data()];
    if (answer)
        return *answer;
    answer = Answer::Unknown;
    if (ModuleID exporter = requiredName.exporterModule(); areNamesBehindStarsInTables(table, exporter)) {
        m_isOnCycle = false;
        answer = resolve(table, exporter, requiredName.name).answer;
        for (ModuleID visited : std::exchange(m_visited, { }))
            m_seen.quickClear(visited.index());
    }
    return *answer;
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
    return name->isSymbol() ? std::nullopt : findExport(module, StringView { *name });
}

std::optional<unsigned> ModuleTable::findExport(ModuleID module, uint32_t identifier) const
{
    return findExport(module, ProgramStrings::viewInImage(*m_tables, ProgramStrings::identifierString(*m_tables, identifier)));
}

std::optional<unsigned> ModuleTable::findExport(ModuleID module, StringView name) const
{
    std::span<const ModuleExportData> entries = exports(module);
    auto compare = [&](const ModuleExportData& entry) {
        StringID string = ProgramStrings::identifierString(*m_tables, entry.name);
        ASSERT(string);
        return codePointCompare(ProgramStrings::viewInImage(*m_tables, string), name);
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

bool ModuleTable::hasQueryInDirectory() const
{
    return std::ranges::any_of(m_tables->moduleDirectories(), [&](const ModuleDirectoryData& directory) {
        return ProgramStrings::viewInImage(*m_tables, StringID(directory.name)).contains('?');
    });
}

bool ModuleTable::pathWithoutQueryEndsWith(ModuleID module, StringView end, bool hasQueryInDirectory) const
{
    StringView name = ProgramStrings::viewInImage(*m_tables, StringID(data(module).publicPathOrName));
    size_t query = name.reverseFind('?');
    StringView nameWithoutQuery = name.left(query);
    if (nameWithoutQuery.length() >= end.length() && (query != notFound || !hasQueryInDirectory))
        return nameWithoutQuery.endsWith(end);
    String path = fullPath(module);
    return StringView(path).left(path.reverseFind('?')).endsWith(end);
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
