/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTTableParts.h"
#include "AOTThreads.h"
#include <algorithm>
#include <initializer_list>
#include <utility>

namespace JSC { namespace AOT {

struct ModuleLists {
    void setListStarts(ModuleData& entry) const
    {
        entry.firstRequest = Number24(requests.size());
        entry.firstExport = Number24(exports.size());
        entry.firstSyntheticExport = Number24(syntheticExports.size());
        entry.firstRequiredName = Number24(requiredNames.size());
    }

    Vector<ModuleData> modules;
    Vector<ModuleDirectoryData> directories;
    Vector<ModuleDisplayPathData> displayPaths;
    Vector<ModuleRequestData> requests;
    Vector<ModuleExportData> exports;
    Vector<IndirectExportData> indirectExports;
    Vector<SyntheticExportData> syntheticExports;
    Vector<RequiredNameData> requiredNames;
    Vector<ExportStatementData> exportStatements;
    Vector<AmbiguousExportData> ambiguousExports;
    Vector<SourceStampData> sourceStamps;
};

Vector<WriterOfExportData> listWritersOfExports(const TableInputs&);

struct LoopBesideOthers {
    unsigned count;
    ScopedLambda<void(unsigned index)> body;
};
void forEachIndexOfLoopsInParallel(const ParallelLoops&, std::initializer_list<LoopBesideOthers>);

inline std::pair<size_t, size_t> boundsOfRangeOfIndices(unsigned range, size_t count)
{
    size_t first = static_cast<size_t>(range) * indicesInRangeOfLoop;
    return { first, std::min(count, first + indicesInRangeOfLoop) };
}

using AnswerAboutModule = AnswersAboutModules::Answer;
AnswersAboutModules askAboutModules(const TableInputs&, const ParallelLoops&);

ModuleLists collectModuleLists(TableEncoder&, const AnswersAboutModules&);
void nameWhatModuleListsName(const TableInputs&, const AnswersAboutModules&, const ParallelLoops&, NamesOfTables&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
