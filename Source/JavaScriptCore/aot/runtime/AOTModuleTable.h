/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include <compare>
#include <optional>
#include <ranges>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
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

    const ModuleData& data(ModuleID) const;
    std::span<const ModuleRequestData> requests(ModuleID) const;
    // What is evaluated in front of the module.
    auto imports(ModuleID module) const { return requests(module) | std::views::filter(isImport); }
    std::span<const ModuleRequestData> allRequests() const { return m_requests; }
    uint32_t endOfRequests(ModuleID) const;
    ModuleNumbers linkRequests(ModuleID) const;
    ModuleNumbers allLinkRequests() const { return m_linkRequests; }
    uint32_t endOfLinkRequests(ModuleID) const;
    ModuleNumbers evaluationRequests(ModuleID) const;
    ModuleNumbers allEvaluationRequests() const { return m_evaluationRequests; }
    uint32_t endOfEvaluationRequests(ModuleID) const;
    std::span<const LinkPlanStepData> entryPointLinkPlan() const { return m_tables->entryPointLinkPlan(); }
    struct LinkPlanOfRoot {
        std::span<const LinkPlanStepData> steps;
        ModuleID memberOfCycle;
    };
    std::optional<LinkPlanOfRoot> linkPlanOfRoot(ModuleID) const;
    std::span<const ModuleExportData> exports(ModuleID) const;
    const IndirectExportData& indirectExport(const ModuleExportData&) const;
    std::span<const SyntheticExportData> syntheticExports(ModuleID) const;
    std::span<const RequiredNameData> requiredNames(ModuleID) const;
    const ModuleFailureData& failure(ModuleFailureID) const;

    std::optional<ModuleID> findByPath(StringView publicPath) const;
    std::span<const uint32_t> modulesOfUncompiledCode() const { return m_tables->modulesOfUncompiledCode(); }
    std::optional<ModuleID> findRuntimeSource(uint32_t registryID) const;
    std::optional<unsigned> findExport(ModuleID, UniquedStringImpl* name) const;
    // ES modules import a CommonJS file through a module that evaluates the file first of all and then takes copies of what it exports. Answers the file for such a module.
    std::optional<ModuleID> fileBehindFace(ModuleID) const;
    const CompiledRuntimeFunctionData* findRuntimeFunction(uint32_t id) const;
    // Walks the table: for what happens rarely.
    bool isRuntimeFunction(FunctionID) const;

    // The characters of a path that is whole in the image are not copied.
    String pathText(ModuleID) const;
    String displayPathText(ModuleID) const;
    String pathTextForLineColumns(ModuleID) const;
    bool hasFilePath(ModuleID) const;
    bool hasFileWithExtension(StringView) const;

private:
    using PiecesOfPath = Vector<StringView, 16>;

    size_t checkedIndex(ModuleID) const;
    PiecesOfPath piecesOfPath(ModuleID) const;
    std::strong_ordering comparePath(ModuleID, StringView) const;
    std::optional<ModuleID> findPlaceOfPath(StringView publicPath) const;
    template<typename List> List listOf(List, ModuleID, Number24 ModuleData::* first) const;

    const ProgramTables* m_tables;
    std::span<const ModuleData> m_modules;
    std::span<const ModuleRequestData> m_requests;
    ModuleNumbers m_linkRequests;
    ModuleNumbers m_evaluationRequests;
    std::span<const ModuleExportData> m_exports;
    std::span<const SyntheticExportData> m_syntheticExports;
    std::span<const RequiredNameData> m_requiredNames;
    std::span<const ModuleFailureData> m_failures;
    ModuleNumbers m_modulesByPath;
    std::span<const CompiledRuntimeModuleData> m_runtimeModules;
    std::span<const CompiledRuntimeFunctionData> m_runtimeFunctions;
};

class ModuleRequestWalk {
    WTF_MAKE_NONCOPYABLE(ModuleRequestWalk);
public:
    enum class StepKind : uint8_t { Request, Leave, Done };

    struct Step {
        StepKind kind;
        ModuleID module;
        const ModuleRequestData* request;
    };

    ModuleRequestWalk(const ModuleTable&, ModuleID root);

    Step next();
    void enter(ModuleID);
    bool hasVisited(ModuleID module) const { return m_visited.get(module.index()); }
    bool isOnPath(ModuleID module) const { return m_onPath.get(module.index()); }

private:
    struct Frame {
        ModuleID module;
        unsigned nextRequest;
    };

    const ModuleTable& m_table;
    BitVector m_visited;
    BitVector m_onPath;
    Vector<Frame, 16> m_path;
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

inline const ModuleData& ModuleTable::data(ModuleID module) const
{
    return m_modules[checkedIndex(module)];
}

inline uint32_t ModuleTable::endOfRequests(ModuleID module) const
{
    return m_modules[checkedIndex(module) + 1].firstRequest;
}

inline std::span<const ModuleRequestData> ModuleTable::requests(ModuleID module) const
{
    return listOf(m_requests, module, &ModuleData::firstRequest);
}

inline uint32_t ModuleTable::endOfLinkRequests(ModuleID module) const
{
    return m_modules[checkedIndex(module) + 1].firstLinkRequest;
}

inline ModuleNumbers ModuleTable::linkRequests(ModuleID module) const
{
    return listOf(m_linkRequests, module, &ModuleData::firstLinkRequest);
}

inline uint32_t ModuleTable::endOfEvaluationRequests(ModuleID module) const
{
    return m_modules[checkedIndex(module) + 1].firstEvaluationRequest;
}

inline ModuleNumbers ModuleTable::evaluationRequests(ModuleID module) const
{
    return listOf(m_evaluationRequests, module, &ModuleData::firstEvaluationRequest);
}

inline std::span<const SyntheticExportData> ModuleTable::syntheticExports(ModuleID module) const
{
    return listOf(m_syntheticExports, module, &ModuleData::firstSyntheticExport);
}

inline std::span<const RequiredNameData> ModuleTable::requiredNames(ModuleID module) const
{
    return listOf(m_requiredNames, module, &ModuleData::firstRequiredName);
}

inline const IndirectExportData& ModuleTable::indirectExport(const ModuleExportData& moduleExport) const
{
    RELEASE_ASSERT(moduleExport.kind() == ModuleExportKind::Indirect);
    std::span<const IndirectExportData> indirectExports = m_tables->indirectExports();
    RELEASE_ASSERT(moduleExport.indirectExport < indirectExports.size());
    return indirectExports[moduleExport.indirectExport];
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
