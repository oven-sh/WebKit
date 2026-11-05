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

class ModuleTable {
public:
    explicit ModuleTable(const ProgramTables&);

    const ProgramTables& programTables() const { return *m_tables; }
    unsigned size() const { return static_cast<unsigned>(m_modules.size() - 1); }
    static bool isImport(const ModuleRequestData& request) { return !request.isRequire; }

    const ModuleData& data(ModuleID module) const { return m_modules[checkedIndex(module)]; }
    std::span<const ModuleRequestData> requests(ModuleID module) const { return listOf(m_tables->moduleRequests(), module, &ModuleData::firstRequest); }
    auto imports(ModuleID module) const { return requests(module) | std::views::filter(isImport); }
    std::span<const ModuleRequestData> allRequests() const { return m_tables->moduleRequests(); }
    uint32_t endOfRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstRequest; }
    ModuleNumbers linkRequests(ModuleID module) const { return listOf(allLinkRequests(), module, &ModuleData::firstLinkRequest); }
    ModuleNumbers allLinkRequests() const { return { m_tables->linkRequests(), m_modules.size() }; }
    uint32_t endOfLinkRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstLinkRequest; }
    ModuleNumbers evaluationRequests(ModuleID module) const { return listOf(allEvaluationRequests(), module, &ModuleData::firstEvaluationRequest); }
    ModuleNumbers allEvaluationRequests() const { return { m_tables->evaluationRequests(), m_modules.size() }; }
    uint32_t endOfEvaluationRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstEvaluationRequest; }
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
    std::optional<SlotData> wordOfMock(ModuleID module) const
    {
        std::span<const ModuleExportData> entries = listOf(m_tables->moduleExports(), module, &ModuleData::firstExport);
        return !entries.empty() && entries.back().kindBits == ModuleExportData::kindBitsOfWordOfMock ? std::optional(entries.back().variable) : std::nullopt;
    }
    const IndirectExportData& indirectExport(const ModuleExportData&) const;
    std::span<const SyntheticExportData> syntheticExports(ModuleID module) const { return listOf(m_tables->syntheticExports(), module, &ModuleData::firstSyntheticExport); }
    std::span<const RequiredNameData> requiredNames(ModuleID module) const { return listOf(m_tables->requiredNames(), module, &ModuleData::firstRequiredName); }
    const ExportStatementData* findExportStatement(const RequiredNameData&) const;
    std::span<const AmbiguousExportData> ambiguousExports(ModuleID) const;
    bool isAmbiguousExport(ModuleID, StringView name) const;
    bool hasSourceStamps() const { return !m_tables->sourceStamps().empty(); }
    std::optional<uint64_t> sourceStamp(ModuleID) const;
    std::span<const NameOfStatementData> namesOfStatements(ModuleID) const;
    const ModuleFailureData& failure(ModuleFailureID) const;
    std::optional<ModuleFailureID> loadFailure(ModuleID) const;

    std::optional<ModuleID> findByPath(StringView publicPath) const { return findByPath(publicPath, publicPath.hash()); }
    std::optional<ModuleID> findByPath(StringView publicPath, unsigned hashOfPath) const;
    bool holdsFormThatAttributeAsksFor(StringView publicPath) const;
    Vector<ModuleID, 2> formsThatAttributesAskFor(StringView publicPath) const;
    bool holdsPathStartingWith(StringView start) const;
    std::span<const UncompiledModuleData> modulesOfUncompiledCode() const { return m_tables->modulesOfUncompiledCode(); }
    String typeAttribute(ModuleID) const;
    std::optional<ModuleID> findRuntimeSource(uint32_t registryID) const;
    std::optional<unsigned> findExport(ModuleID, UniquedStringImpl* name) const;
    std::optional<ModuleID> fileBehindFace(ModuleID module) const { return data(module).isFaceOfFile() ? std::optional(requests(module).front().targetModule()) : std::nullopt; }
    std::optional<ModuleID> faceOfFile(ModuleID) const;
    const CompiledRuntimeFunctionData* findRuntimeFunction(uint32_t id) const;
    bool isRuntimeFunction(FunctionID) const;

    String pathText(ModuleID) const;
    String displayPathText(ModuleID) const;
    String pathTextForLineColumns(ModuleID) const;
    bool hasFilePath(ModuleID module) const { return data(module).hasExportTable() || data(module).kind == ModuleKind::CommonJs; }
    IterationStatus forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>&) const;
    bool mayHoldPathStartingWith(StringView start) const;

private:
    using PiecesOfPath = Vector<StringView, 16>;

    size_t checkedIndex(ModuleID) const;
    PiecesOfPath piecesOfPath(ModuleID) const;
    std::strong_ordering comparePath(ModuleID, StringView) const;
    ModuleNumbers modulesByPath() const { return { m_tables->modulesByPath(), m_modules.size() }; }
    template<typename List> List listOf(List, ModuleID, Number24 ModuleData::* first) const;

    const ProgramTables* m_tables;
    std::span<const ModuleData> m_modules;
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
