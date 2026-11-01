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
    // Of an image that does not run, where no image is the current one: the caller finds the text of a string. Only for the paths, and for as long as the lambda lives.
    using TextOfString = ScopedLambda<StringView(StringID)>;
    ModuleTable(const ProgramTables&, const TextOfString&);

    const ProgramTables& programTables() const { return *m_tables; }
    unsigned size() const { return static_cast<unsigned>(m_modules.size() - 1); }
    static bool isImport(const ModuleRequestData& request) { return !request.isRequire; }

    const ModuleData& data(ModuleID module) const { return m_modules[checkedIndex(module)]; }
    std::span<const ModuleRequestData> requests(ModuleID module) const { return listOf(m_tables->moduleRequests(), module, &ModuleData::firstRequest); }
    // What is evaluated in front of the module.
    auto imports(ModuleID module) const { return requests(module) | std::views::filter(isImport); }
    std::span<const ModuleRequestData> allRequests() const { return m_tables->moduleRequests(); }
    uint32_t endOfRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstRequest; }
    ModuleNumbers linkRequests(ModuleID module) const { return listOf(allLinkRequests(), module, &ModuleData::firstLinkRequest); }
    ModuleNumbers allLinkRequests() const { return { m_tables->linkRequests(), m_modules.size() }; }
    uint32_t endOfLinkRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstLinkRequest; }
    ModuleNumbers evaluationRequests(ModuleID module) const { return listOf(allEvaluationRequests(), module, &ModuleData::firstEvaluationRequest); }
    ModuleNumbers allEvaluationRequests() const { return { m_tables->evaluationRequests(), m_modules.size() }; }
    uint32_t endOfEvaluationRequests(ModuleID module) const { return m_modules[checkedIndex(module) + 1].firstEvaluationRequest; }
    // The build sorts the ES modules of the program into groups: whatever has loaded one module of a group has loaded all of them. Each group is known by one of its modules, the head.
    ModuleID headOfLoadGroup(ModuleID module) const;
    // The heads of the groups that are loaded whenever the group of this head is.
    ModuleNumbers groupsLoadedWith(ModuleID head) const;
    std::span<const LinkPlanStepData> entryPointLinkPlan() const { return m_tables->entryPointLinkPlan(); }
    struct LinkPlanOfRoot {
        std::span<const LinkPlanStepData> steps;
        ModuleID memberOfCycle;
    };
    std::optional<LinkPlanOfRoot> linkPlanOfRoot(ModuleID) const;
    std::span<const ImportOfHeldFileData> importsOfHeldFile(ModuleID) const;
    std::span<const ModuleExportData> exports(ModuleID module) const { return listOf(m_tables->moduleExports(), module, &ModuleData::firstExport); }
    const IndirectExportData& indirectExport(const ModuleExportData&) const;
    std::span<const SyntheticExportData> syntheticExports(ModuleID module) const { return listOf(m_tables->syntheticExports(), module, &ModuleData::firstSyntheticExport); }
    std::span<const RequiredNameData> requiredNames(ModuleID module) const { return listOf(m_tables->requiredNames(), module, &ModuleData::firstRequiredName); }
    // The export statement that asks for the name. An import asks for any other one.
    const ExportStatementData* findExportStatement(const RequiredNameData&) const;
    // The table of exports has no entry for such a name.
    std::span<const AmbiguousExportData> ambiguousExports(ModuleID) const;
    bool isAmbiguousExport(ModuleID, StringView name) const;
    bool hasSourceStamps() const { return !m_tables->sourceStamps().empty(); }
    std::optional<uint64_t> sourceStamp(ModuleID) const;
    const ModuleFailureData& failure(ModuleFailureID) const;
    // Of a module that imports, itself or through what it imports, a name that the build did not find or a file that is no program.
    std::optional<ModuleFailureID> loadFailure(ModuleID) const;

    std::optional<ModuleID> findByPath(StringView publicPath) const;
    // A file that only an attribute asks for stands under its path, a question mark and what the host calls the form.
    bool holdsFormThatAttributeAsksFor(StringView publicPath) const;
    Vector<ModuleID, 2> formsThatAttributesAskFor(StringView publicPath) const;
    bool holdsPathStartingWith(StringView start) const;
    // The next search for the path of the module finds it at once.
    void willBeAskedForPath(ModuleID, unsigned hashOfPath) const;
    std::span<const UncompiledModuleData> modulesOfUncompiledCode() const { return m_tables->modulesOfUncompiledCode(); }
    // Of a module of uncompiled code. Null: the statements that name it say nothing of its type.
    String typeAttribute(ModuleID) const;
    std::optional<ModuleID> findRuntimeSource(uint32_t registryID) const;
    std::optional<unsigned> findExport(ModuleID, UniquedStringImpl* name) const;
    // ES modules import a CommonJS file through a module that evaluates the file first of all and then takes copies of what it exports. Answers the file for such a module.
    std::optional<ModuleID> fileBehindFace(ModuleID module) const { return data(module).isFaceOfFile() ? std::optional(requests(module).front().targetModule()) : std::nullopt; }
    std::optional<ModuleID> faceOfFile(ModuleID) const;
    const CompiledRuntimeFunctionData* findRuntimeFunction(uint32_t id) const;
    // Walks the table: for what happens rarely.
    bool isRuntimeFunction(FunctionID) const;

    // The characters of a path that is whole in the image are not copied.
    String pathText(ModuleID) const;
    String displayPathText(ModuleID) const;
    String pathTextForLineColumns(ModuleID) const;
    bool hasFilePath(ModuleID module) const { return data(module).hasExportTable() || data(module).kind == ModuleKind::CommonJs; }
    // The path of each program file with code that starts with `start`, in sorted order.
    IterationStatus forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>&) const;
    // A file with code or with data, as it is loaded where no attribute asks for another form.
    bool holdsFileAt(StringView path) const;
    // False: no module that is listed by path has a path that starts so.
    bool mayHoldPathStartingWith(StringView start) const;

private:
    using PiecesOfPath = Vector<StringView, 16>;

    size_t checkedIndex(ModuleID) const;
    PiecesOfPath piecesOfPath(ModuleID) const;
    PiecesOfPath piecesOfDisplayPath(ModuleID) const;
    std::strong_ordering comparePath(ModuleID, StringView) const;
    ModuleNumbers modulesByPath() const { return { m_tables->modulesByPath(), m_modules.size() }; }
    template<typename List> List listOf(List, ModuleID, Number24 ModuleData::* first) const;

    StringView viewOfPathPiece(StringID) const;

    const ProgramTables* m_tables;
    const TextOfString* m_textWhereNoImageRuns { nullptr };
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

inline ModuleID ModuleTable::headOfLoadGroup(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    ASSERT(moduleData.isESModuleOfProgram());
    uint32_t group = moduleData.referrerRuntimeModuleOrLoadGroup;
    return group < size() ? ModuleID(group) : module;
}

inline ModuleNumbers ModuleTable::groupsLoadedWith(ModuleID head) const
{
    uint32_t group = data(head).referrerRuntimeModuleOrLoadGroup;
    ASSERT(group >= size());
    if (group == Number24::max)
        return { };
    // In front of the list stands its length.
    ModuleNumbers lists(m_tables->groupsLoadedWithGroups(), m_modules.size());
    return lists.subspan(group - size() + 1, lists[group - size()]);
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
