/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTProgramTables.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/UniquedStringImpl.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class ModuleTable {
public:
    explicit ModuleTable(const ProgramTables&);

    const ProgramTables& programTables() const { return *m_tables; }
    unsigned size() const { return static_cast<unsigned>(m_modules.size() - 1); }
    unsigned numberOfRequireEdges() const { return static_cast<unsigned>(m_requireEdges.size()); }

    const ModuleData& data(ModuleID) const;
    std::span<const ModuleRequestData> requests(ModuleID) const;
    std::span<const ModuleRequestData> allRequests() const { return m_requests; }
    uint32_t endOfRequests(ModuleID) const;
    std::span<const ModuleExportData> exports(ModuleID) const;
    const IndirectExportData& indirectExport(const ModuleExportData&) const;
    std::span<const SyntheticExportData> syntheticExports(ModuleID) const;
    std::span<const RequiredNameData> requiredNames(ModuleID) const;
    const RequireEdgeData& requireEdge(uint32_t edge) const;
    std::span<const ModuleResetSlotData> resetSlots(ModuleID) const;
    const ModuleFailureData& failure(ModuleFailureID) const;

    std::optional<ModuleID> findByPath(StringView publicPath) const;
    bool namesUncompiledCode(StringView specifier) const;
    std::optional<ModuleID> findRuntimeSource(uint32_t registryID) const;
    std::optional<unsigned> findExport(ModuleID, UniquedStringImpl* name) const;
    std::optional<uint32_t> findRequireEdge(ModuleID referrer, ModuleID target) const;
    const CompiledRuntimeFunctionData* findRuntimeFunction(uint32_t id) const;

    StringView path(ModuleID) const;
    String pathText(ModuleID) const;
    bool hasFilePath(ModuleID) const;

private:
    size_t checkedIndex(ModuleID) const;
    template<typename T> std::span<const T> listOf(std::span<const T> list, ModuleID, uint32_t ModuleData::* first) const;

    const ProgramTables* m_tables;
    std::span<const ModuleData> m_modules;
    std::span<const ModuleRequestData> m_requests;
    std::span<const ModuleExportData> m_exports;
    std::span<const SyntheticExportData> m_syntheticExports;
    std::span<const RequiredNameData> m_requiredNames;
    std::span<const RequireEdgeData> m_requireEdges;
    std::span<const ModuleFailureData> m_failures;
    std::span<const uint32_t> m_modulesByPath;
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

template<typename T>
inline std::span<const T> ModuleTable::listOf(std::span<const T> list, ModuleID module, uint32_t ModuleData::* first) const
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
