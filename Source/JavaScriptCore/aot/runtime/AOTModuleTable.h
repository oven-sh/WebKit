/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include <array>
#include <compare>
#include <optional>
#include <ranges>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/IterationStatus.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/StringView.h>
#include <wtf/text/UniquedStringImpl.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class DynamicStars {
public:
    DynamicStars(std::span<const ModuleRequestData> requests, std::span<const StarAsWrittenData> asWritten)
        : m_requests(requests)
        , m_asWritten(asWritten)
    {
    }

    bool placesAreWritten() const { return !m_asWritten.empty(); }
    size_t numberOfPlaces() const { return m_asWritten.empty() ? m_requests.size() : m_asWritten.size(); }
    std::optional<ModuleRequestData> at(size_t place) const
    {
        if (!m_asWritten.empty()) [[unlikely]]
            return m_asWritten[place].isFollowed ? std::optional(ModuleRequestData::create(m_asWritten[place].target, false, true)) : std::nullopt;
        return m_requests[place].isDynamicStarExport ? std::optional(m_requests[place]) : std::nullopt;
    }

    class Iterator {
    public:
        Iterator(const DynamicStars& stars, size_t place)
            : m_stars(stars)
            , m_place(place)
        {
            skipWhatIsNoStar();
        }

        ModuleRequestData operator*() const { return *m_stars.at(m_place); }
        Iterator& operator++()
        {
            ++m_place;
            skipWhatIsNoStar();
            return *this;
        }
        bool operator==(const Iterator& other) const { return m_place == other.m_place; }

    private:
        void skipWhatIsNoStar()
        {
            while (m_place < m_stars.numberOfPlaces() && !m_stars.at(m_place))
                ++m_place;
        }

        const DynamicStars& m_stars;
        size_t m_place;
    };

    Iterator begin() const { return { *this, 0 }; }
    Iterator end() const { return { *this, numberOfPlaces() }; }

private:
    std::span<const ModuleRequestData> m_requests;
    std::span<const StarAsWrittenData> m_asWritten;
};

class ModuleTable {
public:
    explicit ModuleTable(const ProgramTables&);

    const ProgramTables& programTables() const { return *m_tables; }
    unsigned size() const { return static_cast<unsigned>(m_modules.size() - 1); }
    static bool isImport(const ModuleRequestData& request) { return !request.isRequire; }

    const ModuleData& data(ModuleID module) const { return m_modules[checkedIndex(module)]; }
    std::span<const ModuleRequestData> requests(ModuleID module) const { return listOf(m_tables->moduleRequests(), module, &ModuleData::firstRequest); }
    auto imports(ModuleID module) const { return requests(module) | std::views::filter(isImport); }
    DynamicStars dynamicStars(ModuleID module) const { return { requests(module), m_tables->starsAsWritten().empty() ? std::span<const StarAsWrittenData> { } : starsAsWritten(module) }; }
    std::span<const ModuleRequestData> allRequests() const { return m_tables->moduleRequests(); }
    uint32_t endOfRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstRequest; }
    ModuleNumbers linkRequests(ModuleID module) const { return listOf(allLinkRequests(), module, &ModuleData::firstLinkRequest); }
    ModuleNumbers allLinkRequests() const { return { m_tables->linkRequests(), m_modules.size() }; }
    uint32_t endOfLinkRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstLinkRequest; }
    ModuleID leaderInLink(ModuleID module) const { return ModuleID(allLinkRequests()[data(module).firstLinkRequest]); }
    ModuleNumbers evaluationRequests(ModuleID module) const { return listOf(allEvaluationRequests(), module, &ModuleData::firstEvaluationRequest); }
    ModuleNumbers allEvaluationRequests() const { return { m_tables->evaluationRequests(), m_modules.size() }; }
    uint32_t endOfEvaluationRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstEvaluationRequest; }
    ModuleID headOfLoadGroup(ModuleID) const;
    ModuleNumbers groupsLoadedWith(ModuleID head) const;
    std::span<const LinkPlanStepData> entryPointLinkPlan() const { return m_tables->entryPointLinkPlan(); }
    std::span<const ImportOfHeldFileData> importsOfHeldFile(ModuleID) const;
    std::span<const ModuleExportData> exports(ModuleID module) const
    {
        std::span<const ModuleExportData> entries = listOf(m_tables->moduleExports(), module, &ModuleData::firstExport);
        return !entries.empty() && entries.back().kindBits == ModuleExportData::kindBitsOfWordOfMock ? entries.first(entries.size() - 1) : entries;
    }
    bool someExportStaysIn(ModuleID module) const
    {
        return std::ranges::any_of(exports(module), [](const ModuleExportData& entry) {
            return entry.staysIn;
        });
    }
    std::optional<SlotData> mockSlot(ModuleID module) const
    {
        std::span<const ModuleExportData> entries = listOf(m_tables->moduleExports(), module, &ModuleData::firstExport);
        return !entries.empty() && entries.back().kindBits == ModuleExportData::kindBitsOfWordOfMock ? std::optional(entries.back().variable) : std::nullopt;
    }
    const IndirectExportData& indirectExport(const ModuleExportData&) const;
    std::span<const SyntheticExportData> syntheticExports(ModuleID module) const { return listOf(m_tables->syntheticExports(), module, &ModuleData::firstSyntheticExport); }
    bool dependsOnMoreThanSource(ModuleID module) const { return std::ranges::binary_search(m_tables->modulesThatDependOnMoreThanSource(), module.index()); }
    std::span<const RequiredNameData> requiredNames(ModuleID module) const { return listOf(m_tables->requiredNames(), module, &ModuleData::firstRequiredName); }
    const ExportStatementData* findExportStatement(const RequiredNameData&) const;
    std::span<const AmbiguousExportData> ambiguousExports(ModuleID) const;
    bool isAmbiguousExport(ModuleID, StringView name) const;
    bool hasSourceStamps() const { return !m_tables->sourceStamps().empty(); }
    std::optional<uint64_t> sourceStamp(ModuleID) const;
    using PathInUTF8 = Vector<char, 256>;
    PathInUTF8 pathInUTF8(ModuleID) const;
    std::span<const NameOfStatementData> namesOfStatements(ModuleID) const;
    bool listsNamesOfStatements() const { return !m_tables->namesOfStatements().empty(); }
    std::span<const StarAsWrittenData> starsAsWritten(ModuleID) const;
    std::optional<ModuleID> writerOfExport(ModuleID, unsigned place) const;
    const ModuleFailureData& failure(ModuleFailureID) const;
    std::optional<ModuleFailureID> loadFailure(ModuleID) const;
    bool hasLoadFailures() const { return !m_tables->moduleFailures().empty() && m_tables->moduleFailures().back().isLoadOfModule(); }

    std::optional<ModuleID> findByPath(StringView publicPath) const { return findByPath(publicPath, publicPath.hash()); }
    std::optional<ModuleID> findByPath(StringView publicPath, unsigned hashOfPath) const;
    bool hasAttributeVariant(StringView publicPath) const;
    Vector<ModuleID, 2> importAttributeVariants(StringView publicPath) const;
    bool containsPathWithPrefix(StringView start) const;
    std::span<const UncompiledModuleData> modulesOfUncompiledCode() const { return m_tables->modulesOfUncompiledCode(); }
    String typeAttribute(ModuleID) const;
    std::optional<ModuleID> findRuntimeSource(uint32_t registryID) const;
    bool valueOfRuntimeSourceStaysWithCompiledCode(uint32_t registryID) const;
    bool recordOfRuntimeSourceIsHandedOutFrozen(uint32_t registryID) const;
    std::optional<unsigned> findExport(ModuleID, UniquedStringImpl* name) const;
    std::optional<unsigned> findExport(ModuleID, uint32_t identifier) const;
    std::optional<ModuleID> moduleBehindFacade(ModuleID module) const { return data(module).isFacade() ? std::optional(requests(module).front().targetModule()) : std::nullopt; }
    std::optional<ModuleID> facadeOf(ModuleID) const;
    const CompiledRuntimeFunctionData* findRuntimeFunction(uint32_t id) const;
    bool isRuntimeFunction(FunctionID) const;

    String fullPath(ModuleID) const;
    bool hasQueryInDirectory() const;
    bool pathWithoutQueryEndsWith(ModuleID, StringView end, bool hasQueryInDirectory) const;
    String displayPathText(ModuleID) const;
    String pathTextForLineColumns(ModuleID) const;
    bool hasFilePath(ModuleID module) const { return data(module).hasExportTable() || data(module).kind == ModuleKind::CommonJs; }
    IterationStatus forEachFilePathWithPrefix(StringView start, const ScopedLambda<IterationStatus(const String&)>&) const;
    bool mayContainPathWithPrefix(StringView start) const;

private:
    using PiecesOfPath = Vector<StringView, 16>;

    size_t checkedIndex(ModuleID) const;
    PiecesOfPath piecesOfPath(ModuleID) const;
    std::strong_ordering comparePath(ModuleID, StringView) const;
    ModuleNumbers modulesByPath() const { return { m_tables->modulesByPath(), m_modules.size() }; }
    std::optional<unsigned> findExport(ModuleID, StringView name) const;
    template<typename List> List listOf(List, ModuleID, Number24 ModuleData::* first) const;

    const ProgramTables* m_tables;
    std::span<const ModuleData> m_modules;
};

struct CyclesOfImports {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CyclesOfImports);

    struct Member {
        uint32_t module;
        uint32_t cycle;
        uint32_t place;
    };

    explicit CyclesOfImports(const ModuleTable&);

    const Member* find(ModuleID) const;
    std::span<const uint32_t> membersOf(uint32_t cycle) const LIFETIME_BOUND { return members.span().subspan(firstMemberOf[cycle], firstMemberOf[cycle + 1] - firstMemberOf[cycle]); }

    Vector<Member> membersByModule;
    Vector<uint32_t> members;
    Vector<uint32_t> firstMemberOf;
};

struct ImportersOfModules {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ImportersOfModules);

    explicit ImportersOfModules(const ModuleTable&);

    void addModulesAbove(Vector<ModuleID, 32>&&, BitVector& found) const;

    Vector<uint32_t> importers;
    Vector<uint32_t> firstImporterOf;
    BitVector namedByListsAlone;
};

struct ReexportedNames {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ReexportedNames);

    enum class Answer : uint8_t { Unknown, Found, NotFound };

    explicit ReexportedNames(const ModuleTable&);

    Answer resolve(const ModuleTable&, const RequiredNameData&);
    BitVector transitiveReexportersOf(Vector<ModuleID, 16>&&) const;

private:
    struct Resolution {
        Answer answer { Answer::Unknown };
        const ModuleExportData* binding { nullptr };
    };

    struct Stars {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Stars);

        Vector<ModuleID, 4> reexporters;
        UncheckedKeyHashMap<uint32_t, const ModuleExportData*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bindings;
        bool listsEveryName { true };
    };

    const Stars& starsOf(const ModuleTable&, ModuleID exporter);
    Resolution resolve(const ModuleTable&, ModuleID exporter, uint32_t name);
    Resolution resolveHere(const ModuleTable&, ModuleID exporter, uint32_t name, const Stars&);
    bool areNamesBehindStarsInTables(const ModuleTable&, ModuleID);

    Vector<uint32_t> m_reexporters;
    Vector<uint32_t> m_firstReexporterOf;
    Vector<std::optional<Answer>> m_answers;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<Stars>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_stars;
    UncheckedKeyHashMap<uint64_t, Resolution, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_resolutions;
    BitVector m_seen;
    Vector<ModuleID, 16> m_visited;
    bool m_isOnCycle { false };
    BitVector m_modulesWithNamesInTables;
    BitVector m_modulesWithOtherNames;
};

inline size_t ModuleTable::checkedIndex(ModuleID module) const
{
    RELEASE_ASSERT(module.index() < size());
    return module.index();
}

template<typename List>
inline List ModuleTable::listOf(List list, ModuleID module, Number24 ModuleData::* first) const
{
    size_t index = checkedIndex(module);
    uint32_t start = m_modules[index].*first;
    return list.subspan(start, m_modules[index + 1].*first - start);
}

inline ModuleID ModuleTable::headOfLoadGroup(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    ASSERT(moduleData.isESModuleOfProgram());
    return moduleData.isHeadOfLoadGroup() ? module : ModuleID(moduleData.numberInLoadGroup());
}

inline ModuleNumbers ModuleTable::groupsLoadedWith(ModuleID head) const
{
    ASSERT(data(head).isHeadOfLoadGroup());
    uint32_t first = data(head).numberInLoadGroup();
    if (!first)
        return { };
    ModuleNumbers lists(m_tables->groupsLoadedWithGroups(), m_modules.size());
    return lists.subspan(first, lists[first - 1]);
}

inline const IndirectExportData& ModuleTable::indirectExport(const ModuleExportData& moduleExport) const
{
    RELEASE_ASSERT(moduleExport.kind() == ModuleExportKind::Indirect);
    std::span<const IndirectExportData> indirectExports = m_tables->indirectExports();
    RELEASE_ASSERT(moduleExport.indirectExport < indirectExports.size());
    return indirectExports[moduleExport.indirectExport];
}

inline const ExportStatementData* ModuleTable::findExportStatement(const RequiredNameData& requiredName) const
{
    uint32_t number = static_cast<uint32_t>(&requiredName - m_tables->requiredNames().data());
    std::span<const ExportStatementData> statements = m_tables->exportStatements();
    auto found = std::ranges::lower_bound(statements, number, { }, &ExportStatementData::requiredName);
    return found != statements.end() && found->requiredName == number ? &*found : nullptr;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
